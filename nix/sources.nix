# Third-party sources (mirrors upstream.lock). Nothing here is vendored.
{ pkgs }:
let
  inherit (pkgs) lib fetchurl fetchFromGitHub;
  files = import ./files.nix;
  hexToSri = h: h;  # fetchurl accepts a base16 sha256

  # a directory assembled from individually pinned files of a GitHub repo
  githubFiles = { name, owner, repo, rev, prefix ? "", files }:
    let
      fetched = lib.mapAttrsToList (dst: v:
        let
          path = if builtins.isAttrs v then v.path else "${prefix}/${dst}";
          sha = if builtins.isAttrs v then v.sha256 else v;
        in {
          inherit dst;
          src = fetchurl {
            url = "https://raw.githubusercontent.com/${owner}/${repo}/${rev}/${path}";
            sha256 = hexToSri sha;
            name = "${name}-" + builtins.replaceStrings [ "/" ] [ "-" ] dst;
          };
        }) files;
    in
    pkgs.runCommand name { } (''
      mkdir -p $out
    '' + lib.concatMapStrings (f: ''
      install -D -m644 ${f.src} $out/${f.dst}
    '') fetched);
in
{
  linux = fetchurl {
    url = "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.3.4.tar.xz";
    sha256 = hexToSri "b11b6df8295e8e991ff040b71aaa4994a8b55caa23b6b092907f195b5840fd1f";
  };

  bootlinCedrus = githubFiles {
    name = "bootlin-cedrus";
    owner = "bootlin"; repo = "linux";
    inherit (files.bootlinCedrus) rev files;
    prefix = files.bootlinCedrus.dir;
  };

  nvdlaFirmware = githubFiles {
    name = "nvdla-firmware";
    owner = "nvdla"; repo = "sw";
    inherit (files.nvdlaFirmware) rev files;
  };

  rtl8189fs = fetchFromGitHub {
    owner = "jwrdegoede"; repo = "rtl8189ES_linux";
    rev = "13bbdbc750bfaa37ff47dff063edcc857fad0584";
    hash = "sha256-la8g/5mCkAMiwmfX63ZhrGa+TAmWeAnbICARaDLurLY=";
  };

  uboot = fetchFromGitHub {
    owner = "OpenIPC"; repo = "u-boot";
    rev = "bd3334c112a87c5b4b41fb5c05999682414f003a";
    hash = "sha256-B/h5RH8tepngPgsuhLwLpXvdenEoeGXgY6FT5fK97Jc=";
  };

  busybox = fetchurl {
    url = "https://busybox.net/downloads/busybox-1.37.0.tar.bz2";
    sha256 = hexToSri "3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4";
  };

  wpa_supplicant = fetchurl {
    url = "https://w1.fi/releases/wpa_supplicant-2.11.tar.gz";
    sha256 = hexToSri "912ea06f74e30a8e36fbb68064d6cdff218d8d591db0fc5d75dee6c81ac7fc0a";
  };

  dropbear = fetchFromGitHub {
    owner = "mkj"; repo = "dropbear";
    rev = "28216cd9af822732a1549b78621c2ea9a76a0fe5";
    hash = "sha256-AJ6JvTPhd+Y4xnIIVqbJiU1rkmRmojoZX3pwKV0qrxA=";
  };

  tinyalsa = fetchFromGitHub {
    owner = "tinyalsa"; repo = "tinyalsa";
    rev = "a7cd26a17fe2add2ff38d2f281ed0b919187a068";
    hash = "sha256-B2u9xjH3oHb5zrQyyKVUK86hx1Voq+AxdVP4n9ZYerg=";
  };

  yolov5n = fetchurl {
    url = "https://github.com/ultralytics/yolov5/releases/download/v7.0/yolov5n.onnx";
    sha256 = hexToSri "04f0e55c26f58d17145b36045780fe1250d5bd2187543e11568e5141d05b3262";
  };
}
