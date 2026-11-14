# Building (manual)

The flake (`nix build`, see README) is the reference build. These are the
equivalent manual commands. Fetch the sources listed in `upstream.lock` and
check their hashes.

Toolchains used:

- clang/LLD 23 for the kernel
- zig 0.17 `cc` (bundled musl) for all userspace, as `-target arm-linux-musleabihf -mcpu=cortex_a7`
- arm-linux-musleabihf GCC for U-Boot
- Python 3.12 with numpy, onnx, onnxruntime and Pillow for `ml/`

## Kernel and modules

    tar xf linux-7.3.4.tar.xz && cd linux-7.3.4
    ../kernel/overlay.sh <bootlin-linux checkout or directory> .
    while read p; do patch -p1 < ../kernel/patches/$p; done < ../kernel/patches/series
    M="make ARCH=arm LLVM=1 CC=clang HOSTCC=clang CROSS_COMPILE=arm-linux-gnueabihf-"
    $M y623_min_defconfig
    $M -j"$(nproc)" zImage dtbs modules
    $M INSTALL_HDR_PATH=../out/uapi headers_install   # for userspace (-I../out/uapi/include)

Outputs: `arch/arm/boot/zImage`,
`arch/arm/boot/dts/allwinner/sun8i-v831-yi-y623.dtb` and the `.ko` files that
`rootfs/bin/camera-up` loads: v4l2-cci, gc3003, sun8i_v831_vin, cedrus-enc,
industrialio-adc, sun20i-gpadc-iio, sun8i-v831-codec, sun8i-eise, sun8i-npu.

Wi-Fi (rtl8189fs, unmodified upstream):

    make ARCH=arm LLVM=1 CC=clang HOSTCC=clang CROSS_COMPILE=arm-linux-gnueabihf- \
         KSRC=<kernel build dir> KVER=7.3.4 modules
    llvm-strip --strip-debug -o 8189fs.ko 8189fs.ko

`kernel/mkpatches.sh` regenerates the series from an edited tree. It is a
maintainer tool: set `TARBALL=`, `BOOTLIN=`, and pass the tree as an argument.

## U-Boot

    cd u-boot && git apply ../u-boot/0001-v831-sd-boot.patch
    CROSS_COMPILE=arm-linux-musleabihf- ./build.sh v831     # -> output/u-boot-v831-mmc.bin

Write it to the SD card at 8 KiB: `dd if=u-boot-v831-mmc.bin of=/dev/sdX bs=1024 seek=8`.
It boots immediately, without reading the UART (RX floats on this board).

## Userspace

    Z="zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -static -s"
    U=-I<uapi headers>/include

    # camrtsp (tinyalsa sources from upstream.lock)
    $Z -Os -Wall -Wno-unused-function $U -I$TINYALSA/include -o camrtsp \
       userspace/camrtsp/camrtsp.c userspace/camrtsp/tinyalsa_noplugin.c \
       $TINYALSA/src/{pcm,pcm_hw,mixer,mixer_hw,limits}.c -lm
    # onvifd
    $Z -Os -Wall -Wno-format-truncation -o onvifd userspace/onvifd/onvifd.c
    # persond (NVDLA firmware from nvdla/sw: kmd/firmware -> fw/, kmd/include -> fw/include)
    $Z -O2 -DDLA_2_CONFIG -include userspace/persond/compat.h $U -Ifw/include -Ifw \
       -o persond userspace/persond/{persond,netcore,nn,port}.c fw/*.c -lm
    # small tools
    for t in gpioset sdctl wdog ledblink; do $Z -Os -o $t userspace/tools/$t.c; done

busybox 1.37.0 (`configs/busybox.config`), wpa_supplicant 2.11
(`configs/wpa_supplicant.config`, wext driver, WPA2-PSK, internal crypto) and
dropbear (`configs/dropbear-localoptions.h`; configured with `--host=arm-linux-musleabihf
--disable-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp
--disable-wtmpx --disable-pututline --disable-pututxline --disable-pam
--enable-static --disable-harden`) are all built static with the same zig
compiler. The flake will pin the exact invocations.

## Detector model

    cd ml   # with yolov5n.onnx (upstream.lock) here
    python cut.py yolov5n.onnx yolov5n-192x320.onnx 192 320
    python to_fp32.py yolov5n-192x320.onnx yolov5n-192x320-fp32.onnx
    python compiler.py yolov5n.nnp

`amax.json` and `preact.json` hold the int8 calibration ranges. Regenerating
them (`qsim.py`, `export_layer.py`) needs calibration frames from your own
camera in `ml/calib/*.png`, plus the two YOLOv5 sample images in `../dl/testimg/`.
The resulting `.nnp` is AGPL-3.0, like the weights.

## Boot image and SD card

    # rootfs/ + binaries in bin/ + modules in lib/modules/ + secrets:
    #   etc/wpa_supplicant.conf, etc/dropbear/dropbear_ed25519_host_key, root/.ssh/authorized_keys
    # + busybox applet links
    (cd initramfs && find . | LC_ALL=C sort | cpio -o -H newc --owner 0:0) | gzip -9n > initramfs.cpio.gz
    mkimage -A arm -O linux -T ramdisk -C gzip -a 0x43000000 -e 0x43000000 -n initramfs \
            -d initramfs.cpio.gz initramfs.uImage

On the SD card's first partition (FAT):

- `zImage`
- `initramfs.uImage`
- `sun8i-v831-yi-y623.dtb`
- `yolov5n.nnp`, read once at boot by persond (the SD is mounted read-only only
  for that read).
