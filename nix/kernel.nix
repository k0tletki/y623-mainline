# Linux 7.3.4 + Bootlin Cedrus overlay + kernel/patches, y623_min_defconfig,
# built with LLVM. Also builds the out-of-tree rtl8189fs Wi-Fi module.
{ pkgs, sources, src }:
let
  llvm = pkgs.llvmPackages_latest;
  make = ''make ARCH=arm LLVM=1 CC=${llvm.clang-unwrapped}/bin/clang HOSTCC=cc \
    LD=${llvm.lld}/bin/ld.lld CROSS_COMPILE=arm-linux-gnueabihf- O=$NIX_BUILD_TOP/build'';
  # modules loaded by rootfs/bin/camera-up
  modules = [
    "drivers/media/v4l2-core/v4l2-cci.ko"
    "drivers/media/i2c/gc3003.ko"
    "drivers/media/platform/sunxi/sun8i-v831-vin/sun8i_v831_vin.ko"
    "drivers/media/platform/sunxi/sun8i-cedrus-enc/cedrus-enc.ko"
    "drivers/iio/adc/industrialio-adc.ko"
    "drivers/iio/adc/sun20i-gpadc-iio.ko"
    "sound/soc/sunxi/sun8i-v831-codec.ko"
    "drivers/media/platform/sunxi/sun8i-eise/sun8i-eise.ko"
    "drivers/misc/sun8i-npu/sun8i-npu.ko"
  ];
in
pkgs.stdenv.mkDerivation {
  pname = "linux-y623";
  version = "7.3.4";
  src = sources.linux;

  nativeBuildInputs = with pkgs; [
    llvm.clang-unwrapped llvm.lld llvm.llvm bc bison flex perl python3 openssl elfutils
    kmod cpio gnumake
  ];
  hardeningDisable = [ "all" ];
  dontConfigure = true;
  dontFixup = true;   # ARM kernel objects: no host strip/patchelf

  # reproducible: no timestamps/hostnames in the image
  KBUILD_BUILD_TIMESTAMP = "1970-01-01";
  KBUILD_BUILD_USER = "nix";
  KBUILD_BUILD_HOST = "nix";
  KBUILD_BUILD_VERSION = "1";

  postPatch = ''
    bash ${src}/kernel/overlay.sh ${sources.bootlinCedrus} .
    chmod -R u+w drivers/media/platform/sunxi/sun8i-cedrus-enc
    while read p; do patch -p1 --no-backup-if-mismatch < ${src}/kernel/patches/$p; done < ${src}/kernel/patches/series
    patchShebangs scripts
  '';

  buildPhase = ''
    runHook preBuild
    mkdir -p $NIX_BUILD_TOP/build
    export LD_LIBRARY_PATH=${pkgs.openssl.out}/lib:${pkgs.elfutils.out}/lib  # host tools (extract-cert)
    ${make} y623_min_defconfig
    ${make} -j$NIX_BUILD_CORES zImage dtbs modules

    # Wi-Fi: unmodified upstream driver, against this kernel
    cp -r ${sources.rtl8189fs} $NIX_BUILD_TOP/rtl8189fs; chmod -R u+w $NIX_BUILD_TOP/rtl8189fs
    make -C $NIX_BUILD_TOP/rtl8189fs ARCH=arm LLVM=1 CC=${llvm.clang-unwrapped}/bin/clang HOSTCC=cc \
      LD=${llvm.lld}/bin/ld.lld CROSS_COMPILE=arm-linux-gnueabihf- \
      KSRC=$NIX_BUILD_TOP/build KVER=7.3.4 -j$NIX_BUILD_CORES modules
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    b=$NIX_BUILD_TOP/build
    install -D -m644 $b/arch/arm/boot/zImage $out/zImage
    install -D -m644 $b/arch/arm/boot/dts/allwinner/sun8i-v831-yi-y623.dtb $out/sun8i-v831-yi-y623.dtb
    install -D -m644 $b/.config $out/config
    mkdir -p $out/modules
    ${pkgs.lib.concatMapStrings (m: "install -m644 $b/${m} $out/modules/\n") modules}
    llvm-strip --strip-debug -o $out/modules/8189fs.ko $NIX_BUILD_TOP/rtl8189fs/8189fs.ko
    # uAPI headers of our drivers, for userspace
    install -D -m644 include/uapi/linux/sun8i-eise.h $out/include/linux/sun8i-eise.h
    install -D -m644 include/uapi/linux/sun8i-npu.h $out/include/linux/sun8i-npu.h
    runHook postInstall
  '';
}
