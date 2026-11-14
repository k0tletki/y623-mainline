# y623-sd-image: add secrets to the initramfs and write an SD card image.
# Runs outside the Nix store, so secrets never enter it.
#
#   nix run .#sd-image -- SECRETS_DIR OUT.img          full card image
#   nix run .#sd-image -- --files SECRETS_DIR OUT_DIR  only the FAT files
#
# SECRETS_DIR: wpa_supplicant.conf, authorized_keys and
# dropbear_ed25519_host_key (generated there on first use if missing).
{ pkgs, kernel, initramfs, model, uboot }:
pkgs.writeShellApplication {
  name = "y623-sd-image";
  runtimeInputs = with pkgs; [ coreutils cpio gzip ubootTools mtools dosfstools util-linux dropbear findutils ];
  text = ''
    mode=image
    if [ "''${1:-}" = --files ]; then mode=files; shift; fi
    if [ $# -ne 2 ]; then
      echo "usage: y623-sd-image [--files] SECRETS_DIR OUT" >&2; exit 2
    fi
    sec=$1 out=$2
    for f in wpa_supplicant.conf authorized_keys; do
      [ -f "$sec/$f" ] || { echo "missing $sec/$f" >&2; exit 1; }
    done
    if [ ! -f "$sec/dropbear_ed25519_host_key" ]; then
      echo "generating $sec/dropbear_ed25519_host_key"
      dropbearkey -t ed25519 -f "$sec/dropbear_ed25519_host_key" >/dev/null
    fi

    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    umask 077
    mkdir "$tmp/root"
    (cd "$tmp/root" && zcat ${initramfs}/initramfs.cpio.gz | cpio -idm --quiet --no-absolute-filenames)
    chmod -R u+w "$tmp/root"
    install -m600 "$sec/wpa_supplicant.conf" "$tmp/root/etc/wpa_supplicant.conf"
    install -m600 "$sec/dropbear_ed25519_host_key" "$tmp/root/etc/dropbear/dropbear_ed25519_host_key"
    install -m600 "$sec/authorized_keys" "$tmp/root/root/.ssh/authorized_keys"
    (cd "$tmp/root" && find . | LC_ALL=C sort | cpio -o -H newc --owner 0:0 --quiet) | gzip -9n > "$tmp/initramfs.cpio.gz"

    boot=$tmp/boot; mkdir "$boot"
    mkimage -A arm -O linux -T ramdisk -C gzip -a 0x43000000 -e 0x43000000 -n initramfs \
      -d "$tmp/initramfs.cpio.gz" "$boot/initramfs.uImage" >/dev/null
    cp ${kernel}/zImage ${kernel}/sun8i-v831-yi-y623.dtb ${kernel}/modules/8189fs.ko \
       ${model}/yolov5n.nnp "$boot/"
    chmod 644 "$boot"/*

    if [ $mode = files ]; then
      mkdir -p "$out"; cp "$boot"/* "$out/"
      echo "wrote $out/:"; (cd "$boot" && printf "  %s\n" *)
      echo "contains secrets; U-Boot (not included) goes at 8 KiB: ${uboot}/u-boot-v831-mmc.bin"
      exit 0
    fi

    # 64 MiB image: U-Boot at 8 KiB, one 32 MiB FAT16 partition at 1 MiB
    truncate -s 64M "$tmp/sd.img"
    echo 'start=2048, size=65536, type=6' | sfdisk -q "$tmp/sd.img"
    mkfs.fat -C -F 16 -n Y623 "$tmp/part.img" 32768 >/dev/null
    for f in "$boot"/*; do mcopy -i "$tmp/part.img" "$f" ::; done
    dd if="$tmp/part.img" of="$tmp/sd.img" bs=1M seek=1 conv=notrunc status=none
    dd if=${uboot}/u-boot-v831-mmc.bin of="$tmp/sd.img" bs=1024 seek=8 conv=notrunc status=none
    install -m600 "$tmp/sd.img" "$out"
    echo "wrote $out (64 MiB; contains secrets). Write it with: dd if=$out of=/dev/sdX bs=1M conv=fsync"
  '';
}
