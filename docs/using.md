# Using the camera

Everything is configured in `/etc/camera.conf` (see `rootfs/etc/camera.conf` for
all options). `camera-up` loads the drivers and keeps camrtsp, onvifd and
persond running.

## Streams

- `rtsp://<camera>/main`: H.264, 1920x1080 at 30 fps (`-s 2304x1296` for the
  full sensor), plus an L16 16 kHz audio track (`-E pcmu|pcma` for G.711).
- `rtsp://<camera>/sub`: 640x360, uncorrected full view.

Lowest latency with mpv. The decisive settings are a non-zero audio buffer and
not pacing video to audio:

    mpv --profile=low-latency --audio-buffer=0.1 --video-sync=desync \
        --demuxer-readahead-secs=0 --no-cache --untimed \
        --demuxer-lavf-o=rtsp_transport=tcp rtsp://<camera>/main

`--profile=low-latency` alone sets `audio-buffer=0`. With PipeWire, that makes
playback run slower than real time, so the delay grows without bound.

## ONVIF

onvifd answers WS-Discovery and the Device, Media, Imaging and Events services
on port 80. It reports day/night as the IR cut filter, and person/cat
detections as PullPoint events:

- `tns1:RuleEngine/CellMotionDetector/Motion` with Rule `PersonDetector` or
  `CatDetector`
- `tns1:VideoSource/MotionAlarm` for either.

## Person / cat detection (persond)

persond runs YOLOv5n on the NPU every 250 ms on the sub-stream. Results are in
`/tmp/person.state`. On each change persond fetches plain-HTTP URLs:
`-u`/`-U` when a person appears or leaves, `-a`/`-A` for a cat. Example for
homebridge-camera-ffmpeg with its HTTP motion server (`porthttp`) enabled:

    PERSOND_ARGS="-i 250 -t 0.45 -T 0.3 -n 2 -H 10 \
      -u http://<homebridge>:<porthttp>/motion?<Camera%20Name> \
      -U http://<homebridge>:<porthttp>/motion/reset?<Camera%20Name>"

Detection works for people and cats of normal size in the room. Small or
backlit subjects (under ~25 px in the 320x192 input) are missed.

## SSH

dropbear is key-only, with the keys in `/root/.ssh/authorized_keys` from
`secrets/`. There is no shell on the serial console unless the kernel command
line contains `consoleshell` (the board's UART RX floats).
