// onvifd - minimal ONVIF Profile S front end for camrtsp
//
//   onvifd [-H http_port] [camrtsp options...]
//
// Takes the same options as camrtsp (it only looks at -s -f -q -g -p -A -r -E)
// so camera-up can pass one argument list to both.
//
// Implements what NVRs and discovery tools need to find the camera and its
// stream: WS-Discovery (UDP 3702, Probe -> ProbeMatches, Hello at start-up),
// the Device service (time, device info, capabilities, services, scopes,
// network interfaces) and the Media service (one profile "main", sources,
// encoder configurations, GetStreamUri) and the Imaging service (IR-cut
// filter = night vision: ON = day, OFF = night, AUTO = light sensor; passed
// to camrtsp through /tmp/camrtsp.ctl). Other Set* calls answer with a fault. No authentication: like the RTSP stream it relies on
// the home network being trusted. Each HTTP connection serves one request.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXCONN		4
#define RXMAX		16384
#define WSD_PORT	3702
#define WSD_GROUP	"239.255.255.250"

static unsigned W = 1920, H = 1080, FPS = 30, QP = 30, GOP = 60, RTSP_PORT = 554;
static unsigned SW = 640, SH = 360;	/* sub-stream (camrtsp -S); 0 = none */
static unsigned HTTP_PORT = 80, ARATE = 16000;
static int audio = 1;
static const char *aenc = "l16";
static char mac[18] = "00:00:00:00:00:00", uuid[64];
static unsigned boot_id;

static void LOG(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "onvifd: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---------------------------------------------------------- output buffer */

static char out[24576];
static size_t outlen;

static void ap(const char *fmt, ...)
{
	va_list a;
	if (outlen >= sizeof(out))
		return;
	va_start(a, fmt);
	int n = vsnprintf(out + outlen, sizeof(out) - outlen, fmt, a);
	va_end(a);
	outlen += n > 0 ? (size_t)n : 0;
	if (outlen > sizeof(out))
		outlen = sizeof(out);
}

/* ---------------------------------------------------------------- helpers */

static void read_mac(void)
{
	FILE *f = fopen("/sys/class/net/wlan0/address", "r");
	if (f) {
		if (fscanf(f, "%17s", mac) != 1)
			strcpy(mac, "00:00:00:00:00:00");
		fclose(f);
	}
	unsigned m[6] = { 0 };
	sscanf(mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
	snprintf(uuid, sizeof(uuid), "urn:uuid:6c8a3f10-7623-4831-a7c3-%02x%02x%02x%02x%02x%02x",
		 m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* text of the first element whose local name is `name` (any namespace prefix) */
static int xml_text(const char *x, const char *name, char *dst, size_t dl)
{
	size_t nl = strlen(name);
	for (const char *p = x; (p = strchr(p, '<')); p++) {
		const char *q = p + 1, *colon;
		if (*q == '/' || *q == '?' || *q == '!')
			continue;
		const char *e = q + strcspn(q, " \t\r\n>/");
		colon = memchr(q, ':', e - q);
		if (colon)
			q = colon + 1;
		if ((size_t)(e - q) != nl || strncmp(q, name, nl))
			continue;
		const char *gt = strchr(e, '>');
		if (!gt || gt[-1] == '/')
			return -1;
		const char *end = strchr(gt + 1, '<');
		if (!end)
			return -1;
		while (gt + 1 < end && strchr(" \t\r\n", gt[1]))
			gt++;
		size_t n = end - gt - 1;
		while (n && strchr(" \t\r\n", gt[n]))
			n--;
		if (n >= dl)
			n = dl - 1;
		memcpy(dst, gt + 1, n);
		dst[n] = 0;
		return 0;
	}
	return -1;
}

/* local name of the first element inside soap Body */
static int body_action(const char *x, char *dst, size_t dl)
{
	const char *p = x;
	for (;;) {
		p = strstr(p, "Body");
		if (!p)
			return -1;
		if (p > x && (p[-1] == ':' || p[-1] == '<') && (p[4] == '>' || p[4] == ' '))
			break;
		p += 4;
	}
	p = strchr(p, '>');
	if (!p)
		return -1;
	p = strchr(p, '<');
	if (!p || p[1] == '/')
		return -1;
	p++;
	const char *e = p + strcspn(p, " \t\r\n>/");
	const char *colon = memchr(p, ':', e - p);
	if (colon)
		p = colon + 1;
	size_t n = e - p < (long)dl - 1 ? (size_t)(e - p) : dl - 1;
	memcpy(dst, p, n);
	dst[n] = 0;
	return 0;
}

/* ------------------------------------------------------------- SOAP bodies */

static const char *NS =
	"xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
	"xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
	"xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
	"xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
	"xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" "
	"xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
	"xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
	"xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" "
	"xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\" "
	"xmlns:tns1=\"http://www.onvif.org/ver10/topics\" "
	"xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
	"xmlns:ter=\"http://www.onvif.org/ver10/error\"";

static void env_begin(void)
{
	ap("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<s:Envelope %s><s:Body>", NS);
}

/* envelope with a WS-Addressing Action header (event service responses) */
static void env_begin_action(const char *action)
{
	ap("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<s:Envelope %s><s:Header>"
	   "<wsa:Action>http://www.onvif.org/ver10/events/wsdl/%s</wsa:Action></s:Header><s:Body>",
	   NS, action);
}

/* ----------------------------------------------------------------- events */

/*
 * Person/cat detection (persond -> /tmp/person.state) as ONVIF events, PullPoint
 * only: tns1:RuleEngine/CellMotionDetector/Motion (IsMotion), one message per
 * rule (Rule = PersonDetector, CatDetector), and tns1:VideoSource/MotionAlarm
 * (State), true while either is seen.
 * A new subscription (and SetSynchronizationPoint) first gets the current state
 * as "Initialized"; changes are "Changed". PullMessages waits (long poll) until
 * the state changes or its Timeout (capped at 20 s) runs out.
 */
#ifndef STATE_PATH
#define STATE_PATH	"/tmp/person.state"
#endif
#define MAXSUBS		4
#define PULL_MAX	20.0
struct sub {
	unsigned id;			/* 0 = free */
	double expires;			/* monotonic */
	unsigned seq;			/* last event sequence delivered */
	int sync;			/* send the current state as Initialized */
};
static struct sub subs[MAXSUBS];
static unsigned sub_next = 1;
static int ev_state;			/* bit 0 person, bit 1 cat */
static unsigned ev_seq = 1;		/* bumped on every change */
static time_t ev_time;			/* UTC of the last change */

double mono(void);
static void env_end(void);

static void ev_poll(void)
{
	char buf[256];
	FILE *f = fopen(STATE_PATH, "r");
	int st = 0;
	if (f) {
		while (fgets(buf, sizeof(buf), f)) {
			if (!strncmp(buf, "person=", 7) && atoi(buf + 7))
				st |= 1;
			if (!strncmp(buf, "cat=", 4) && atoi(buf + 4))
				st |= 2;
		}
		fclose(f);
	}
	if (st != ev_state || !ev_time) {
		if (ev_time)
			LOG("event: person %s, cat %s", st & 1 ? "yes" : "no", st & 2 ? "yes" : "no");
		ev_state = st;
		ev_seq++;
		ev_time = time(NULL);
	}
}

static void utc_str(time_t t, char *dst, size_t n)
{
	struct tm u;
	gmtime_r(&t, &u);
	strftime(dst, n, "%Y-%m-%dT%H:%M:%SZ", &u);
}

/* xs:duration "PT10S", "PT1M", "PT1M30S", "PT0.5S" -> seconds */
static double duration_s(const char *d, double dflt)
{
	if (strncmp(d, "PT", 2))
		return dflt;
	double t = 0;
	for (const char *p = d + 2; *p; ) {
		char *e;
		double v = strtod(p, &e);
		if (e == p)
			return dflt;
		t += *e == 'H' ? v * 3600 : *e == 'M' ? v * 60 : v;
		p = *e ? e + 1 : e;
	}
	return t;
}

static struct sub *sub_find(unsigned id)
{
	for (int i = 0; i < MAXSUBS; i++)
		if (subs[i].id && subs[i].id == id)
			return &subs[i];
	return NULL;
}

static void ev_message(const char *op)
{
	char t[32];
	utc_str(ev_time, t, sizeof(t));
	static const char *rules[2] = { "PersonDetector", "CatDetector" };
	for (int r = 0; r < 2; r++)
		ap("<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">"
		   "tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic><wsnt:Message>"
		   "<tt:Message UtcTime=\"%s\" PropertyOperation=\"%s\"><tt:Source>"
		   "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"vsc0\"/>"
		   "<tt:SimpleItem Name=\"VideoAnalyticsConfigurationToken\" Value=\"va0\"/>"
		   "<tt:SimpleItem Name=\"Rule\" Value=\"%s\"/></tt:Source>"
		   "<tt:Data><tt:SimpleItem Name=\"IsMotion\" Value=\"%s\"/></tt:Data></tt:Message>"
		   "</wsnt:Message></wsnt:NotificationMessage>", t, op, rules[r],
		   ev_state & (1 << r) ? "true" : "false");
	const char *v = ev_state ? "true" : "false";
	ap("<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">"
	   "tns1:VideoSource/MotionAlarm</wsnt:Topic><wsnt:Message>"
	   "<tt:Message UtcTime=\"%s\" PropertyOperation=\"%s\"><tt:Source>"
	   "<tt:SimpleItem Name=\"Source\" Value=\"vs0\"/></tt:Source>"
	   "<tt:Data><tt:SimpleItem Name=\"State\" Value=\"%s\"/></tt:Data></tt:Message>"
	   "</wsnt:Message></wsnt:NotificationMessage>", t, op, v);
}

/* does subscription s have something to deliver? */
static int sub_pending(const struct sub *s)
{
	return s->sync || s->seq != ev_seq;
}

/* the whole PullMessagesResponse envelope into out[] */
static void pull_response(struct sub *s, double now)
{
	char ct[32], tt[32];
	utc_str(time(NULL), ct, sizeof(ct));
	utc_str(time(NULL) + (time_t)(s->expires - now), tt, sizeof(tt));
	outlen = 0;
	env_begin_action("PullPointSubscription/PullMessagesResponse");
	ap("<tev:PullMessagesResponse><tev:CurrentTime>%s</tev:CurrentTime>"
	   "<tev:TerminationTime>%s</tev:TerminationTime>", ct, tt);
	if (sub_pending(s))
		ev_message(s->sync ? "Initialized" : "Changed");
	s->sync = 0;
	s->seq = ev_seq;
	ap("</tev:PullMessagesResponse>");
	env_end();
}

static void topic_set(void)
{
	ap("<wstop:TopicSet><tns1:RuleEngine><tns1:CellMotionDetector><tns1:Motion wstop:topic=\"true\">"
	   "<tt:MessageDescription IsProperty=\"true\"><tt:Source>"
	   "<tt:SimpleItemDescription Name=\"VideoSourceConfigurationToken\" Type=\"tt:ReferenceToken\"/>"
	   "<tt:SimpleItemDescription Name=\"VideoAnalyticsConfigurationToken\" Type=\"tt:ReferenceToken\"/>"
	   "<tt:SimpleItemDescription Name=\"Rule\" Type=\"xs:string\"/></tt:Source><tt:Data>"
	   "<tt:SimpleItemDescription Name=\"IsMotion\" Type=\"xs:boolean\"/></tt:Data>"
	   "</tt:MessageDescription></tns1:Motion></tns1:CellMotionDetector></tns1:RuleEngine>"
	   "<tns1:VideoSource><tns1:MotionAlarm wstop:topic=\"true\">"
	   "<tt:MessageDescription IsProperty=\"true\"><tt:Source>"
	   "<tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source><tt:Data>"
	   "<tt:SimpleItemDescription Name=\"State\" Type=\"xs:boolean\"/></tt:Data>"
	   "</tt:MessageDescription></tns1:MotionAlarm></tns1:VideoSource></wstop:TopicSet>");
}

/* parked PullMessages: set by handle_soap, picked up by conn_read */
static struct sub *pull_park;
static double pull_deadline;

static void env_end(void)
{
	ap("</s:Body></s:Envelope>\n");
}

static int fault(const char *sub, const char *reason)
{
	env_begin();
	ap("<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:%s</s:Value>"
	   "</s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason></s:Fault>",
	   sub, reason);
	env_end();
	return 500;
}

static unsigned bitrate_kbps(void)
{
	/* rough guide for clients; the encoder runs constant QP */
	return W * H * FPS / 1000 * 2 / 10 / (QP > 20 ? QP - 20 : 1);
}

static unsigned nprofiles(void)
{
	return SW && SH ? 2 : 1;
}

/* profile index named by a token ("main"/"sub", "vec0"/"vec1") in the request */
static unsigned req_profile(const char *req, const char *elem)
{
	char tok[32];
	if (xml_text(req, elem, tok, sizeof(tok)))
		return 0;
	return nprofiles() > 1 && (!strcmp(tok, "sub") || !strcmp(tok, "vec1")) ? 1 : 0;
}

static void video_source_cfg(const char *tag)
{
	ap("<%s token=\"vsc0\"><tt:Name>vsc0</tt:Name><tt:UseCount>1</tt:UseCount>"
	   "<tt:SourceToken>vs0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"%u\" height=\"%u\"/>"
	   "</%s>", tag, W, H, tag);
}

static void video_encoder_cfg(const char *tag, unsigned n)
{
	unsigned w = n ? SW : W, h = n ? SH : H;
	unsigned kbps = n ? bitrate_kbps() * SW * SH / (W * H) + 1 : bitrate_kbps();

	ap("<%s token=\"vec%u\"><tt:Name>vec%u</tt:Name><tt:UseCount>1</tt:UseCount>"
	   "<tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>%u</tt:Width>"
	   "<tt:Height>%u</tt:Height></tt:Resolution><tt:Quality>%u</tt:Quality>"
	   "<tt:RateControl><tt:FrameRateLimit>%u</tt:FrameRateLimit>"
	   "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%u</tt:BitrateLimit>"
	   "</tt:RateControl><tt:H264><tt:GovLength>%u</tt:GovLength>"
	   "<tt:H264Profile>Main</tt:H264Profile></tt:H264>"
	   "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address>"
	   "</tt:Address><tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart>"
	   "</tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></%s>",
	   tag, n, n, w, h, QP < 51 ? (51 - QP) * 100 / 51 : 0, FPS, kbps, GOP, tag);
}

static void audio_source_cfg(const char *tag)
{
	ap("<%s token=\"asc0\"><tt:Name>asc0</tt:Name><tt:UseCount>1</tt:UseCount>"
	   "<tt:SourceToken>as0</tt:SourceToken></%s>", tag, tag);
}

static int audio_g711(void)
{
	return audio && (!strcmp(aenc, "pcmu") || !strcmp(aenc, "pcma"));
}

static void audio_encoder_cfg(const char *tag)
{
	/* ONVIF Media 1 only knows G711/G726/AAC; L16 is announced in the SDP */
	ap("<%s token=\"aec0\"><tt:Name>aec0</tt:Name><tt:UseCount>1</tt:UseCount>"
	   "<tt:Encoding>G711</tt:Encoding><tt:Bitrate>64</tt:Bitrate>"
	   "<tt:SampleRate>8</tt:SampleRate>"
	   "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address>"
	   "</tt:Address><tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart>"
	   "</tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></%s>", tag, tag);
}

static void profile(const char *tag, unsigned n)
{
	const char *name = n ? "sub" : "main";

	ap("<%s token=\"%s\" fixed=\"true\"><tt:Name>%s</tt:Name>", tag, name, name);
	video_source_cfg("tt:VideoSourceConfiguration");
	if (audio)
		audio_source_cfg("tt:AudioSourceConfiguration");
	video_encoder_cfg("tt:VideoEncoderConfiguration", n);
	if (audio_g711())
		audio_encoder_cfg("tt:AudioEncoderConfiguration");
	ap("</%s>", tag);
}

/* current day/night mode as camrtsp reports it: auto | day | night */
static void dn_mode(char *dst, size_t n)
{
	char line[64];
	FILE *f = fopen("/tmp/camrtsp.status", "r");
	snprintf(dst, n, "auto");
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "mode=", 5)) {
			line[strcspn(line, "\r\n")] = 0;
			snprintf(dst, n, "%s", line + 5);
		}
	if (f)
		fclose(f);
}

static const char *ircut_of_mode(const char *m)
{
	return !strcmp(m, "day") ? "ON" : !strcmp(m, "night") ? "OFF" : "AUTO";
}

static int set_dn_mode(const char *ircut)
{
	const char *m = !strcmp(ircut, "ON") ? "day" : !strcmp(ircut, "OFF") ? "night" :
			!strcmp(ircut, "AUTO") ? "auto" : NULL;
	if (!m)
		return -1;
	FILE *f = fopen("/tmp/camrtsp.ctl.new", "w");
	if (!f)
		return -1;
	fprintf(f, "mode=%s\n", m);
	fclose(f);
	rename("/tmp/camrtsp.ctl.new", "/tmp/camrtsp.ctl");
	LOG("IrCutFilter %s -> mode %s", ircut, m);
	return 0;
}

static int handle_soap(const char *req, const char *ip, const char *path)
{
	char act[64], base[64];
	int media = strstr(path, "media") != NULL, imaging = strstr(path, "imaging") != NULL;
	int events = strstr(path, "events") != NULL;
	unsigned sid = 0;
	const char *sp = strstr(path, "/subscription/");
	if (sp)
		sid = strtoul(sp + 14, NULL, 10);
	double now = mono();

	snprintf(base, sizeof(base), "http://%s:%u/onvif", ip, HTTP_PORT);
	if (body_action(req, act, sizeof(act)))
		return fault("InvalidArgVal", "no SOAP body");

	/* ---- event service and subscription managers (own envelopes) */
	if (sid || (events && strcmp(act, "GetServiceCapabilities") && strcmp(act, "GetEventProperties") &&
		    strcmp(act, "CreatePullPointSubscription"))) {
		struct sub *s = sub_find(sid);
		if (!s || now > s->expires)
			return fault("InvalidArgVal", "unknown or expired subscription");
		if (!strcmp(act, "PullMessages")) {
			char to[32] = "PT10S";
			xml_text(req, "Timeout", to, sizeof(to));
			double t = duration_s(to, 10);
			if (t > PULL_MAX) t = PULL_MAX;
			if (t < 0) t = 0;
			if (s->expires < now + t + 5)
				s->expires = now + t + 5;	/* a pull in progress keeps it alive */
			if (sub_pending(s) || t == 0) {
				pull_response(s, now);
				return 200;
			}
			pull_park = s;
			pull_deadline = now + t;
			return 0;		/* reply later */
		}
		if (!strcmp(act, "Renew")) {
			char tt[32] = "PT60S", ct[32], et[32];
			xml_text(req, "TerminationTime", tt, sizeof(tt));
			double d = duration_s(tt, 60);
			if (d < 10) d = 10;
			if (d > 3600) d = 3600;
			s->expires = now + d;
			utc_str(time(NULL), ct, sizeof(ct));
			utc_str(time(NULL) + (time_t)d, et, sizeof(et));
			env_begin();
			ap("<wsnt:RenewResponse><wsnt:TerminationTime>%s</wsnt:TerminationTime>"
			   "<wsnt:CurrentTime>%s</wsnt:CurrentTime></wsnt:RenewResponse>", et, ct);
			env_end();
			return 200;
		}
		if (!strcmp(act, "Unsubscribe")) {
			LOG("subscription %u: unsubscribed", s->id);
			s->id = 0;
			env_begin();
			ap("<wsnt:UnsubscribeResponse/>");
			env_end();
			return 200;
		}
		if (!strcmp(act, "SetSynchronizationPoint")) {
			s->sync = 1;
			env_begin();
			ap("<tev:SetSynchronizationPointResponse/>");
			env_end();
			return 200;
		}
		LOG("unsupported subscription action %s", act);
		return fault("ActionNotSupported", "action not supported");
	}
	if (events && !strcmp(act, "CreatePullPointSubscription")) {
		char tt[32] = "PT60S", ct[32], et[32];
		xml_text(req, "InitialTerminationTime", tt, sizeof(tt));
		double d = duration_s(tt, 60);
		if (d < 10) d = 10;
		if (d > 3600) d = 3600;
		struct sub *s = NULL;
		for (int i = 0; i < MAXSUBS; i++)
			if (!subs[i].id || now > subs[i].expires) { s = &subs[i]; break; }
		if (!s) {
			for (int i = 0; i < MAXSUBS; i++)	/* replace the one expiring first */
				if (!s || subs[i].expires < s->expires)
					s = &subs[i];
		}
		*s = (struct sub){ .id = sub_next++, .expires = now + d, .seq = ev_seq, .sync = 1 };
		LOG("subscription %u: created (%.0f s)", s->id, d);
		utc_str(time(NULL), ct, sizeof(ct));
		utc_str(time(NULL) + (time_t)d, et, sizeof(et));
		env_begin_action("EventPortType/CreatePullPointSubscriptionResponse");
		ap("<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>"
		   "<wsa:Address>%s/subscription/%u</wsa:Address></tev:SubscriptionReference>"
		   "<wsnt:CurrentTime>%s</wsnt:CurrentTime><wsnt:TerminationTime>%s</wsnt:TerminationTime>"
		   "</tev:CreatePullPointSubscriptionResponse>", base, s->id, ct, et);
		env_end();
		return 200;
	}

	env_begin();
	if (events && !strcmp(act, "GetServiceCapabilities")) {
		ap("<tev:GetServiceCapabilitiesResponse><tev:Capabilities WSSubscriptionPolicySupport=\"false\" "
		   "WSPausableSubscriptionManagerInterfaceSupport=\"false\" MaxNotificationProducers=\"0\" "
		   "MaxPullPoints=\"%d\" PersistentNotificationStorage=\"false\"/></tev:GetServiceCapabilitiesResponse>",
		   MAXSUBS);
	} else if (events && !strcmp(act, "GetEventProperties")) {
		ap("<tev:GetEventPropertiesResponse>"
		   "<tev:TopicNamespaceLocation>http://www.onvif.org/onvif/ver10/topics/topicns.xml</tev:TopicNamespaceLocation>"
		   "<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet>");
		topic_set();
		ap("<wsnt:TopicExpressionDialect>http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet</wsnt:TopicExpressionDialect>"
		   "<wsnt:TopicExpressionDialect>http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete</wsnt:TopicExpressionDialect>"
		   "<tev:MessageContentFilterDialect>http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter</tev:MessageContentFilterDialect>"
		   "<tev:MessageContentSchemaLocation>http://www.onvif.org/onvif/ver10/schema/onvif.xsd</tev:MessageContentSchemaLocation>"
		   "</tev:GetEventPropertiesResponse>");
	} else if (!strcmp(act, "GetSystemDateAndTime")) {
		time_t t = time(NULL);
		struct tm u;
		gmtime_r(&t, &u);
		ap("<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
		   "<tt:DateTimeType>NTP</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
		   "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone><tt:UTCDateTime>"
		   "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
		   "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
		   "</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
		   u.tm_hour, u.tm_min, u.tm_sec, u.tm_year + 1900, u.tm_mon + 1, u.tm_mday);
	} else if (!strcmp(act, "GetDeviceInformation")) {
		ap("<tds:GetDeviceInformationResponse><tds:Manufacturer>Yi</tds:Manufacturer>"
		   "<tds:Model>2K Pro (y623)</tds:Model><tds:FirmwareVersion>mainline-camrtsp</tds:FirmwareVersion>"
		   "<tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>V831</tds:HardwareId>"
		   "</tds:GetDeviceInformationResponse>", mac);
	} else if (!strcmp(act, "GetCapabilities")) {
		ap("<tds:GetCapabilitiesResponse><tds:Capabilities>"
		   "<tt:Device><tt:XAddr>%s/device_service</tt:XAddr>"
		   "<tt:Network><tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
		   "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network>"
		   "<tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve><tt:DiscoveryBye>false</tt:DiscoveryBye>"
		   "<tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup>"
		   "<tt:SystemLogging>false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
		   "<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tt:SupportedVersions>"
		   "</tt:System><tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
		   "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
		   "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
		   "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken></tt:Security>"
		   "</tt:Device><tt:Events><tt:XAddr>%s/events_service</tt:XAddr>"
		   "<tt:WSSubscriptionPolicySupport>false</tt:WSSubscriptionPolicySupport>"
		   "<tt:WSPullPointSupport>true</tt:WSPullPointSupport>"
		   "<tt:WSPausableSubscriptionManagerInterfaceSupport>false</tt:WSPausableSubscriptionManagerInterfaceSupport>"
		   "</tt:Events><tt:Imaging><tt:XAddr>%s/imaging_service</tt:XAddr></tt:Imaging>"
		   "<tt:Media><tt:XAddr>%s/media_service</tt:XAddr><tt:StreamingCapabilities>"
		   "<tt:RTPMulticast>false</tt:RTPMulticast><tt:RTP_TCP>true</tt:RTP_TCP>"
		   "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media>"
		   "</tds:Capabilities></tds:GetCapabilitiesResponse>", base, base, base, base);
	} else if (!strcmp(act, "GetServices")) {
		int caps = strstr(req, "IncludeCapability>true") != NULL;
		ap("<tds:GetServicesResponse>"
		   "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
		   "<tds:XAddr>%s/device_service</tds:XAddr>", base);
		if (caps)
			ap("<tds:Capabilities><tds:Capabilities><tds:Network IPFilter=\"false\" "
			   "ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/>"
			   "<tds:Security UsernameToken=\"false\"/><tds:System DiscoveryResolve=\"false\" "
			   "DiscoveryBye=\"false\" RemoteDiscovery=\"false\"/></tds:Capabilities></tds:Capabilities>");
		ap("<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
		   "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
		   "<tds:XAddr>%s/media_service</tds:XAddr>", base);
		if (caps)
			ap("<tds:Capabilities><trt:Capabilities SnapshotUri=\"false\" Rotation=\"false\">"
			   "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"2\"/>"
			   "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" "
			   "RTP_RTSP_TCP=\"true\"/></trt:Capabilities></tds:Capabilities>");
		ap("<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
		   "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace>"
		   "<tds:XAddr>%s/imaging_service</tds:XAddr>", base);
		if (caps)
			ap("<tds:Capabilities><timg:Capabilities ImageStabilization=\"false\"/></tds:Capabilities>");
		ap("<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
		   "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/events/wsdl</tds:Namespace>"
		   "<tds:XAddr>%s/events_service</tds:XAddr>", base);
		if (caps)
			ap("<tds:Capabilities><tev:Capabilities WSSubscriptionPolicySupport=\"false\" "
			   "WSPausableSubscriptionManagerInterfaceSupport=\"false\" MaxNotificationProducers=\"0\" "
			   "MaxPullPoints=\"%d\" PersistentNotificationStorage=\"false\"/></tds:Capabilities>", MAXSUBS);
		ap("<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
		   "</tds:GetServicesResponse>");
	} else if (!strcmp(act, "GetServiceCapabilities") && imaging) {
		ap("<timg:GetServiceCapabilitiesResponse><timg:Capabilities ImageStabilization=\"false\" "
		   "Presets=\"false\" AdaptablePreset=\"false\"/></timg:GetServiceCapabilitiesResponse>");
	} else if (!strcmp(act, "GetImagingSettings")) {
		char m[16];
		dn_mode(m, sizeof(m));
		ap("<timg:GetImagingSettingsResponse><timg:ImagingSettings>"
		   "<tt:IrCutFilter>%s</tt:IrCutFilter></timg:ImagingSettings>"
		   "</timg:GetImagingSettingsResponse>", ircut_of_mode(m));
	} else if (!strcmp(act, "GetOptions") && imaging) {
		ap("<timg:GetOptionsResponse><timg:ImagingOptions><tt:IrCutFilterModes>ON</tt:IrCutFilterModes>"
		   "<tt:IrCutFilterModes>OFF</tt:IrCutFilterModes><tt:IrCutFilterModes>AUTO</tt:IrCutFilterModes>"
		   "</timg:ImagingOptions></timg:GetOptionsResponse>");
	} else if (!strcmp(act, "SetImagingSettings")) {
		char v[16];
		if (xml_text(req, "IrCutFilter", v, sizeof(v)) == 0 && set_dn_mode(v)) {
			outlen = 0;
			return fault("InvalidArgVal", "IrCutFilter must be ON, OFF or AUTO");
		}
		ap("<timg:SetImagingSettingsResponse/>");
	} else if (!strcmp(act, "GetServiceCapabilities")) {
		if (media)
			ap("<trt:GetServiceCapabilitiesResponse><trt:Capabilities SnapshotUri=\"false\" "
			   "Rotation=\"false\"><trt:ProfileCapabilities MaximumNumberOfProfiles=\"2\"/>"
			   "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" "
			   "RTP_RTSP_TCP=\"true\"/></trt:Capabilities></trt:GetServiceCapabilitiesResponse>");
		else
			ap("<tds:GetServiceCapabilitiesResponse><tds:Capabilities><tds:Network IPFilter=\"false\" "
			   "ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/>"
			   "<tds:Security UsernameToken=\"false\"/><tds:System DiscoveryResolve=\"false\" "
			   "DiscoveryBye=\"false\" RemoteDiscovery=\"false\"/></tds:Capabilities>"
			   "</tds:GetServiceCapabilitiesResponse>");
	} else if (!strcmp(act, "GetScopes")) {
		static const char *sc[] = {
			"onvif://www.onvif.org/type/video_encoder",
			"onvif://www.onvif.org/type/audio_encoder",
			"onvif://www.onvif.org/Profile/Streaming",
			"onvif://www.onvif.org/name/Yi_2K_Pro",
			"onvif://www.onvif.org/hardware/y623",
			"onvif://www.onvif.org/location/home",
		};
		ap("<tds:GetScopesResponse>");
		for (unsigned i = 0; i < sizeof(sc) / sizeof(sc[0]); i++)
			ap("<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>%s</tt:ScopeItem></tds:Scopes>",
			   sc[i]);
		ap("</tds:GetScopesResponse>");
	} else if (!strcmp(act, "GetHostname")) {
		char hn[64] = "y623";
		gethostname(hn, sizeof(hn));
		ap("<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP>"
		   "<tt:Name>%s</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>", hn);
	} else if (!strcmp(act, "GetNetworkInterfaces")) {
		ap("<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"wlan0\">"
		   "<tt:Enabled>true</tt:Enabled><tt:Info><tt:Name>wlan0</tt:Name>"
		   "<tt:HwAddress>%s</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info>"
		   "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:FromDHCP><tt:Address>%s</tt:Address>"
		   "<tt:PrefixLength>24</tt:PrefixLength></tt:FromDHCP><tt:DHCP>true</tt:DHCP></tt:Config>"
		   "</tt:IPv4></tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>", mac, ip);
	} else if (!strcmp(act, "GetUsers")) {
		ap("<tds:GetUsersResponse/>");
	} else if (!strcmp(act, "GetProfiles")) {
		ap("<trt:GetProfilesResponse>");
		for (unsigned n = 0; n < nprofiles(); n++)
			profile("trt:Profiles", n);
		ap("</trt:GetProfilesResponse>");
	} else if (!strcmp(act, "GetProfile")) {
		ap("<trt:GetProfileResponse>");
		profile("trt:Profile", req_profile(req, "ProfileToken"));
		ap("</trt:GetProfileResponse>");
	} else if (!strcmp(act, "GetVideoSources")) {
		ap("<trt:GetVideoSourcesResponse><trt:VideoSources token=\"vs0\"><tt:Framerate>%u</tt:Framerate>"
		   "<tt:Resolution><tt:Width>%u</tt:Width><tt:Height>%u</tt:Height></tt:Resolution>"
		   "</trt:VideoSources></trt:GetVideoSourcesResponse>", FPS, W, H);
	} else if (!strcmp(act, "GetVideoSourceConfigurations")) {
		ap("<trt:GetVideoSourceConfigurationsResponse>");
		video_source_cfg("trt:Configurations");
		ap("</trt:GetVideoSourceConfigurationsResponse>");
	} else if (!strcmp(act, "GetVideoSourceConfiguration")) {
		ap("<trt:GetVideoSourceConfigurationResponse>");
		video_source_cfg("trt:Configuration");
		ap("</trt:GetVideoSourceConfigurationResponse>");
	} else if (!strcmp(act, "GetVideoEncoderConfigurations")) {
		ap("<trt:GetVideoEncoderConfigurationsResponse>");
		for (unsigned n = 0; n < nprofiles(); n++)
			video_encoder_cfg("trt:Configurations", n);
		ap("</trt:GetVideoEncoderConfigurationsResponse>");
	} else if (!strcmp(act, "GetVideoEncoderConfiguration")) {
		ap("<trt:GetVideoEncoderConfigurationResponse>");
		video_encoder_cfg("trt:Configuration", req_profile(req, "ConfigurationToken"));
		ap("</trt:GetVideoEncoderConfigurationResponse>");
	} else if (!strcmp(act, "GetVideoEncoderConfigurationOptions")) {
		ap("<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options>"
		   "<tt:QualityRange><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:QualityRange><tt:H264>"
		   "<tt:ResolutionsAvailable><tt:Width>%u</tt:Width><tt:Height>%u</tt:Height></tt:ResolutionsAvailable>"
		   "<tt:GovLengthRange><tt:Min>%u</tt:Min><tt:Max>%u</tt:Max></tt:GovLengthRange>"
		   "<tt:FrameRateRange><tt:Min>%u</tt:Min><tt:Max>%u</tt:Max></tt:FrameRateRange>"
		   "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>"
		   "<tt:H264ProfilesSupported>Main</tt:H264ProfilesSupported></tt:H264>"
		   "</trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>",
		   W, H, GOP, GOP, FPS, FPS);
	} else if (!strcmp(act, "GetAudioSources")) {
		ap("<trt:GetAudioSourcesResponse>");
		if (audio)
			ap("<trt:AudioSources token=\"as0\"><tt:Channels>1</tt:Channels></trt:AudioSources>");
		ap("</trt:GetAudioSourcesResponse>");
	} else if (!strcmp(act, "GetAudioSourceConfigurations")) {
		ap("<trt:GetAudioSourceConfigurationsResponse>");
		if (audio)
			audio_source_cfg("trt:Configurations");
		ap("</trt:GetAudioSourceConfigurationsResponse>");
	} else if (!strcmp(act, "GetAudioEncoderConfigurations")) {
		ap("<trt:GetAudioEncoderConfigurationsResponse>");
		if (audio_g711())
			audio_encoder_cfg("trt:Configurations");
		ap("</trt:GetAudioEncoderConfigurationsResponse>");
	} else if (!strcmp(act, "GetStreamUri")) {
		ap("<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:%u/%s</tt:Uri>"
		   "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
		   "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout>"
		   "</trt:MediaUri></trt:GetStreamUriResponse>", ip, RTSP_PORT,
		   req_profile(req, "ProfileToken") ? "sub" : "main");
	} else {
		outlen = 0;
		LOG("unsupported action %s", act);
		return fault("ActionNotSupported", "action not supported");
	}
	env_end();
	return 200;
}

/* ------------------------------------------------------------------- HTTP */

struct conn {
	int fd;
	char rx[RXMAX + 1];
	size_t len;
	double t0;
	int continued;
	struct sub *pull;		/* parked PullMessages */
	unsigned pull_id;
	double pull_deadline;
};
static struct conn conns[MAXCONN];

double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void conn_close(struct conn *c)
{
	close(c->fd);
	c->fd = -1;
	c->len = 0;
	c->pull = NULL;
}

static void send_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			if (r < 0 && errno == EAGAIN) {
				struct pollfd pf = { .fd = fd, .events = POLLOUT };
				if (poll(&pf, 1, 2000) > 0)
					continue;
			}
			return;
		}
		p += r;
		n -= r;
	}
}

static void http_reply(struct conn *c, int code, const char *ctype)
{
	char h[256];
	int n = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nServer: onvifd\r\nContent-Type: %s\r\n"
			 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
			 code, code == 200 ? "OK" : code == 404 ? "Not Found" : code == 400 ?
			 "Bad Request" : "Internal Server Error", ctype, outlen);
	send_all(c->fd, h, n);
	send_all(c->fd, out, outlen);
}

static void conn_read(struct conn *c)
{
	ssize_t r = recv(c->fd, c->rx + c->len, RXMAX - c->len, MSG_DONTWAIT);
	if (r <= 0) {
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		conn_close(c);
		return;
	}
	c->len += r;
	c->rx[c->len] = 0;
	char *eoh = strstr(c->rx, "\r\n\r\n");
	if (!eoh) {
		if (c->len >= RXMAX)
			conn_close(c);
		return;
	}
	size_t body = eoh + 4 - c->rx, clen = 0;
	char *cl = strcasestr(c->rx, "\r\nContent-Length:");
	if (cl && cl < eoh)
		clen = strtoul(cl + 17, NULL, 10);
	if (body + clen > RXMAX) {
		outlen = 0;
		http_reply(c, 400, "text/plain");
		conn_close(c);
		return;
	}
	if (c->len < body + clen) {
		char *ex = strcasestr(c->rx, "\r\nExpect: 100-continue");
		if (ex && ex < eoh && !c->continued) {
			send_all(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
			c->continued = 1;
		}
		return;
	}

	struct sockaddr_in me;
	socklen_t ml = sizeof(me);
	char ip[INET_ADDRSTRLEN] = "0.0.0.0";
	if (!getsockname(c->fd, (struct sockaddr *)&me, &ml))
		inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip));

	outlen = 0;
	if (!strncmp(c->rx, "POST ", 5)) {
		char path[128] = "";
		sscanf(c->rx + 5, "%127s", path);
		pull_park = NULL;
		int code = handle_soap(eoh + 4, ip, path);
		if (!code && pull_park) {	/* long poll: answered from the main loop */
			c->pull = pull_park;
			c->pull_id = pull_park->id;
			c->pull_deadline = pull_deadline;
			c->len = 0;
			return;
		}
		http_reply(c, code, "application/soap+xml; charset=utf-8");
	} else if (!strncmp(c->rx, "GET ", 4)) {
		ap("y623 camera\nstream: rtsp://%s:%u/main\nonvif: http://%s:%u/onvif/device_service\n"
		   "person: %d\ncat: %d\n", ip, RTSP_PORT, ip, HTTP_PORT, ev_state & 1, ev_state >> 1 & 1);
		http_reply(c, 200, "text/plain");
	} else {
		http_reply(c, 400, "text/plain");
	}
	conn_close(c);
}

/* ----------------------------------------------------------- WS-Discovery */

static int wsd_fd = -1, wsd_joined;
static unsigned wsd_msgno;

static void new_msgid(char *dst, size_t n)
{
	snprintf(dst, n, "urn:uuid:%08x-%04x-4%03x-8%03x-%04x%08x", (unsigned)rand(),
		 rand() & 0xffff, rand() & 0xfff, rand() & 0xfff, rand() & 0xffff, (unsigned)rand());
}

static void wsd_envelope(const char *action, const char *relates, const char *to,
			 const char *body_tag, const char *ip)
{
	char mid[64];
	new_msgid(mid, sizeof(mid));
	outlen = 0;
	ap("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	   "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
	   "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
	   "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
	   "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" "
	   "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"><s:Header>"
	   "<a:MessageID>%s</a:MessageID>", mid);
	if (relates)
		ap("<a:RelatesTo>%s</a:RelatesTo>", relates);
	ap("<a:To>%s</a:To><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/%s</a:Action>"
	   "<d:AppSequence InstanceId=\"%u\" MessageNumber=\"%u\"/></s:Header><s:Body>",
	   to, action, boot_id, ++wsd_msgno);
	ap("<d:%s>%s<d:EndpointReference><a:Address>%s</a:Address></d:EndpointReference>"
	   "<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>"
	   "<d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/audio_encoder "
	   "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/name/Yi_2K_Pro "
	   "onvif://www.onvif.org/hardware/y623 onvif://www.onvif.org/location/home</d:Scopes>"
	   "<d:XAddrs>http://%s:%u/onvif/device_service</d:XAddrs>"
	   "<d:MetadataVersion>1</d:MetadataVersion>%s</d:%s></s:Body></s:Envelope>\n",
	   body_tag, !strcmp(body_tag, "ProbeMatches") ? "<d:ProbeMatch>" : "",
	   uuid, ip, HTTP_PORT, !strcmp(body_tag, "ProbeMatches") ? "</d:ProbeMatch>" : "", body_tag);
}

/* our address as seen on the route to `dst` */
static int local_ip_for(const struct sockaddr_in *dst, char *ip)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in me;
	socklen_t ml = sizeof(me);
	int r = -1;
	if (s < 0)
		return -1;
	if (!connect(s, (const struct sockaddr *)dst, sizeof(*dst)) &&
	    !getsockname(s, (struct sockaddr *)&me, &ml) && me.sin_addr.s_addr) {
		inet_ntop(AF_INET, &me.sin_addr, ip, INET_ADDRSTRLEN);
		r = 0;
	}
	close(s);
	return r;
}

static void wsd_hello(void)
{
	struct sockaddr_in g = { .sin_family = AF_INET, .sin_port = htons(WSD_PORT) };
	char ip[INET_ADDRSTRLEN];
	inet_pton(AF_INET, WSD_GROUP, &g.sin_addr);
	if (local_ip_for(&g, ip))
		return;
	wsd_envelope("Hello", NULL, "urn:schemas-xmlsoap-org:ws:2005:04:discovery", "Hello", ip);
	sendto(wsd_fd, out, outlen, 0, (struct sockaddr *)&g, sizeof(g));
	LOG("hello sent, device_service at http://%s:%u/onvif/device_service", ip, HTTP_PORT);
}

static void wsd_join(void)
{
	struct ip_mreq m = { .imr_interface.s_addr = htonl(INADDR_ANY) };
	inet_pton(AF_INET, WSD_GROUP, &m.imr_multiaddr);
	if (!setsockopt(wsd_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) || errno == EADDRINUSE) {
		if (!wsd_joined)
			wsd_hello();
		wsd_joined = 1;
	} else {
		wsd_joined = 0;
	}
}

static void wsd_open(void)
{
	int one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(WSD_PORT) };
	wsd_fd = socket(AF_INET, SOCK_DGRAM, 0);
	setsockopt(wsd_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(wsd_fd, (struct sockaddr *)&a, sizeof(a))) {
		LOG("bind %u: %s", WSD_PORT, strerror(errno));
		close(wsd_fd);
		wsd_fd = -1;
		return;
	}
	unsigned char ttl = 1, loop = 0;
	setsockopt(wsd_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	setsockopt(wsd_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
	wsd_join();
}

static void wsd_read(void)
{
	static char buf[8192];
	struct sockaddr_in from;
	socklen_t fl = sizeof(from);
	ssize_t n = recvfrom(wsd_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
	char act[64], mid[128], ip[INET_ADDRSTRLEN];

	if (n <= 0)
		return;
	buf[n] = 0;
	if (body_action(buf, act, sizeof(act)) || strcmp(act, "Probe"))
		return;
	/* answer probes for any type list that does not exclude us */
	char types[256] = "";
	xml_text(buf, "Types", types, sizeof(types));
	if (types[0] && !strstr(types, "NetworkVideoTransmitter") && !strstr(types, "Device"))
		return;
	if (xml_text(buf, "MessageID", mid, sizeof(mid)))
		mid[0] = 0;
	if (local_ip_for(&from, ip))
		return;
	wsd_envelope("ProbeMatches", mid[0] ? mid : NULL,
		     "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous",
		     "ProbeMatches", ip);
	sendto(wsd_fd, out, outlen, 0, (struct sockaddr *)&from, fl);
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	int opt, one = 1;

	signal(SIGPIPE, SIG_IGN);
	srand((unsigned)time(NULL) ^ getpid());
	boot_id = (unsigned)time(NULL) ^ (unsigned)rand();
	opterr = 0;
	while ((opt = getopt(argc, argv, "H:c:e:s:S:f:q:g:p:b:a:F:G:M:N:D:n:d:L:WAr:m:E:P:v")) != -1) {
		switch (opt) {
		case 'H': HTTP_PORT = atoi(optarg); break;
		case 's': sscanf(optarg, "%ux%u", &W, &H); break;
		case 'S':
			if (sscanf(optarg, "%ux%u", &SW, &SH) != 2)
				SW = SH = 0;
			break;
		case 'f': FPS = atoi(optarg); break;
		case 'q': QP = atoi(optarg); break;
		case 'g': GOP = atoi(optarg); break;
		case 'p': RTSP_PORT = atoi(optarg); break;
		case 'A': audio = 0; break;
		case 'r': ARATE = atoi(optarg); break;
		case 'E': aenc = optarg; break;
		default: break;		/* other camrtsp options */
		}
	}
	read_mac();
	for (int i = 0; i < MAXCONN; i++)
		conns[i].fd = -1;

	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons(HTTP_PORT) };
	if (bind(lfd, (struct sockaddr *)&la, sizeof(la)) || listen(lfd, 4)) {
		LOG("http listen %u: %s", HTTP_PORT, strerror(errno));
		return 1;
	}
	fcntl(lfd, F_SETFL, O_NONBLOCK);
	wsd_open();
	LOG("http :%u, %s", HTTP_PORT, uuid);

	double last_join = mono();
	for (;;) {
		struct pollfd pfd[2 + MAXCONN];
		int np = 0, cidx[MAXCONN];
		pfd[np++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		pfd[np++] = (struct pollfd){ .fd = wsd_fd, .events = POLLIN };
		for (int i = 0; i < MAXCONN; i++) {
			cidx[i] = -1;
			if (conns[i].fd >= 0) {
				cidx[i] = np;
				pfd[np++] = (struct pollfd){ .fd = conns[i].fd, .events = POLLIN };
			}
		}
		int parked = 0;
		for (int i = 0; i < MAXCONN; i++)
			parked |= conns[i].fd >= 0 && conns[i].pull;
		poll(pfd, np, parked ? 250 : 1000);
		double now = mono();
		ev_poll();
		for (int i = 0; i < MAXCONN; i++) {
			struct conn *c = &conns[i];
			if (c->fd < 0 || !c->pull)
				continue;
			if (c->pull->id != c->pull_id) {	/* unsubscribed meanwhile */
				conn_close(c);
				continue;
			}
			if (sub_pending(c->pull) || now >= c->pull_deadline) {
				pull_response(c->pull, now);
				http_reply(c, 200, "application/soap+xml; charset=utf-8");
				conn_close(c);
			}
		}

		if (pfd[0].revents & POLLIN) {
			int fd = accept(lfd, NULL, NULL);
			if (fd >= 0) {
				struct conn *c = NULL;
				for (int i = 0; i < MAXCONN && !c; i++)
					if (conns[i].fd < 0)
						c = &conns[i];
				if (!c) {
					close(fd);
				} else {
					c->fd = fd;
					c->len = 0;
					c->continued = 0;
					c->t0 = now;
					fcntl(fd, F_SETFL, O_NONBLOCK);
				}
			}
		}
		if (wsd_fd >= 0 && (pfd[1].revents & POLLIN))
			wsd_read();
		for (int i = 0; i < MAXCONN; i++) {
			struct conn *c = &conns[i];
			if (c->fd < 0 || cidx[i] < 0)
				continue;
			if (pfd[cidx[i]].revents & (POLLIN | POLLERR | POLLHUP)) {
				if (c->pull) {		/* client gave up on the long poll */
					char b[64];
					if (recv(c->fd, b, sizeof(b), MSG_DONTWAIT) <= 0)
						conn_close(c);
				} else {
					conn_read(c);
				}
			}
			if (c->fd >= 0 && !c->pull && now - c->t0 > 10)
				conn_close(c);
		}
		/* (re)join the discovery group: Wi-Fi may come up after us or reconnect */
		if (wsd_fd < 0 && now - last_join > 10) {
			last_join = now;
			wsd_open();
		} else if (wsd_fd >= 0 && now - last_join > (wsd_joined ? 60 : 10)) {
			last_join = now;
			if (!wsd_joined)
				read_mac();
			wsd_join();
		}
	}
}
