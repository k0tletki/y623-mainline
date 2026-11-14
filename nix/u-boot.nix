# OpenIPC U-Boot (V831) + u-boot/0001-v831-sd-boot.patch: SPL + U-Boot for the
# SD card (written at 8 KiB). Boots zImage/initramfs.uImage/DTB from FAT.
{ pkgs, sources, src }:
let
  cross = pkgs.pkgsCross.armv7l-hf-multiplatform.buildPackages.gcc;
  prefix = cross.targetPrefix;
in
pkgs.stdenv.mkDerivation {
  pname = "u-boot-v831-sd";
  version = "openipc-bd3334c";
  src = sources.uboot;
  patches = [ "${src}/u-boot/0001-v831-sd-boot.patch" ];
  nativeBuildInputs = with pkgs; [ cross bc bison flex openssl xz python3 dtc gnumake ];
  hardeningDisable = [ "all" ];
  dontConfigure = true;
  postPatch = ''
    patchShebangs .
    grep -rl /bin/pwd --include=Makefile* --include=*.mk . | xargs -r sed -i "s|/bin/pwd|pwd|g"
  '';
  buildPhase = ''
    export MAKEFLAGS="HOSTCC=cc HOSTCFLAGS=-DOPENSSL_ENGINE_STUBS"
    CROSS_COMPILE=${prefix} ./build.sh v831 || true
    # build.sh builds nor + mmc; only the SD (mmc) image is needed
    test -f output/u-boot-v831-mmc.bin
  '';
  installPhase = ''
    install -D -m644 output/u-boot-v831-mmc.bin $out/u-boot-v831-mmc.bin
  '';
  dontFixup = true;
}
