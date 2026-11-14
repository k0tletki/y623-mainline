# Static armv7 musl userspace, built with zig cc: our programs + busybox,
# wpa_supplicant and dropbear.
{ pkgs, sources, src, zigcc, kernel }:
let
  T = "arm-linux-musleabihf";
  common = {
    nativeBuildInputs = [ zigcc pkgs.gnumake pkgs.perl ];
    hardeningDisable = [ "all" ];
    dontConfigure = true;
    dontStrip = true;   # stripped by -s / llvm-strip (host strip can't handle ARM)
    dontPatchELF = true;
    dontFixup = true;
  };
  CC = "${T}-cc";
  ours = name: script: pkgs.stdenv.mkDerivation (common // {
    pname = name; version = "0";
    inherit src;
    buildPhase = ''
      runHook preBuild
      export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig
      ${script}
      runHook postBuild
    '';
    installPhase = "true";
  });
in
rec {
  camrtsp = ours "camrtsp" ''
    T=${sources.tinyalsa}
    ${CC} -Os -static -s -Wall -Wno-unused-function -I${kernel}/include -I$T/include \
      -o camrtsp userspace/camrtsp/camrtsp.c userspace/camrtsp/tinyalsa_noplugin.c \
      $T/src/{pcm,pcm_hw,mixer,mixer_hw,limits}.c -lm
    install -D camrtsp $out/bin/camrtsp
  '';

  onvifd = ours "onvifd" ''
    ${CC} -Os -static -s -Wall -Wno-format-truncation -o onvifd userspace/onvifd/onvifd.c
    install -D onvifd $out/bin/onvifd
  '';

  persond = ours "persond" ''
    cd userspace/persond
    cp -r ${sources.nvdlaFirmware} fw; chmod -R u+w fw   # relative paths: no store paths in the binary
    ${CC} -O2 -static -s -DDLA_2_CONFIG -include compat.h -I${kernel}/include -Ifw/include -Ifw \
      -o persond persond.c netcore.c nn.c port.c $(ls fw/*.c) -lm
    install -D persond $out/bin/persond
    install -D -m644 fw/LICENSE $out/share/licenses/nvdla-LICENSE
  '';

  tools = ours "y623-tools" ''
    for t in gpioset sdctl wdog ledblink; do
      ${CC} -Os -static -s -o $t userspace/tools/$t.c
      install -D $t $out/bin/$t
    done
  '';

  busybox = pkgs.stdenv.mkDerivation (common // {
    pname = "busybox"; version = "1.37.0";
    src = sources.busybox;
    postPatch = "cp ${src}/configs/busybox.config .config";
    buildPhase = ''
      export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig
      make -j$NIX_BUILD_CORES CROSS_COMPILE=${T}- CC=${CC} HOSTCC=cc busybox
      make CROSS_COMPILE=${T}- CC=${CC} HOSTCC=cc busybox.links
    '';
    installPhase = ''
      ${T}-strip -o busybox.s busybox
      install -D busybox.s $out/bin/busybox
      install -D -m644 busybox.links $out/share/busybox.links
    '';
  });

  wpa_supplicant = pkgs.stdenv.mkDerivation (common // {
    pname = "wpa_supplicant"; version = "2.11";
    src = sources.wpa_supplicant;
    postPatch = "cp ${src}/configs/wpa_supplicant.config wpa_supplicant/.config";
    buildPhase = ''
      export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig
      make -C wpa_supplicant -j$NIX_BUILD_CORES CC=${CC} wpa_supplicant
    '';
    installPhase = ''
      ${T}-strip -o wpa.s wpa_supplicant/wpa_supplicant
      install -D wpa.s $out/bin/wpa_supplicant
    '';
  });

  dropbear = pkgs.stdenv.mkDerivation (common // {
    pname = "dropbear"; version = "28216cd";
    src = sources.dropbear;
    nativeBuildInputs = common.nativeBuildInputs ++ [ pkgs.autoconf ];
    buildPhase = ''
      export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig
      # source at ../../dropbear from the build dir (these relative paths end up in
      # the binary through __FILE__)
      cd .. && mv source dropbear && mkdir -p dropbear-build/arm-min && cd dropbear-build/arm-min
      cp ${src}/configs/dropbear-localoptions.h localoptions.h
      CC=${CC} AR=${T}-ar RANLIB=${T}-ranlib \
        CFLAGS='-Os -ffunction-sections -fdata-sections' LDFLAGS='-static -Wl,--gc-sections' \
        ../../dropbear/configure --host=${T} \
        --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp \
        --disable-wtmpx --disable-pututline --disable-pututxline --disable-pam \
        --enable-static --disable-harden
      make -j$NIX_BUILD_CORES PROGRAMS=dropbear
    '';
    installPhase = ''
      ${T}-strip -o dropbear.s dropbear
      install -D dropbear.s $out/bin/dropbear
    '';
  });

  userspace = pkgs.symlinkJoin {
    name = "y623-userspace";
    paths = [ camrtsp onvifd persond tools busybox wpa_supplicant dropbear ];
  };
}
