# The camera's root filesystem WITHOUT secrets, as a gzip'd newc cpio.
# nix/sd-image.nix adds /etc/wpa_supplicant.conf, the dropbear host key and
# /root/.ssh/authorized_keys outside the store.
{ pkgs, src, kernel, userspace }:
let
  u = userspace;
in
pkgs.runCommand "y623-initramfs" { nativeBuildInputs = [ pkgs.cpio pkgs.gzip ]; } ''
  r=$TMPDIR/root
  mkdir -p $r
  cp -r ${src}/rootfs/. $r/
  chmod -R u+w $r
  rm -f $r/etc/wpa_supplicant.conf.example
  mkdir -p $r/{dev,proc,sys,tmp,mnt,lib/modules,etc/dropbear,var/run,usr/share/udhcpc}
  mkdir -m700 -p $r/root/.ssh

  for b in ${u.busybox}/bin/busybox ${u.camrtsp}/bin/camrtsp ${u.onvifd}/bin/onvifd \
           ${u.persond}/bin/persond ${u.tools}/bin/* ${u.wpa_supplicant}/bin/wpa_supplicant \
           ${u.dropbear}/bin/dropbear; do
    install -m755 $b $r/bin/
  done
  # every busybox applet as /bin/<name> (that is what the scripts call)
  for a in $(sed 's|.*/||' ${u.busybox}/share/busybox.links); do
    [ -e $r/bin/$a ] || ln -s busybox $r/bin/$a
  done
  ln -s /tmp/resolv.conf $r/etc/resolv.conf
  # all modules except Wi-Fi: 8189fs.ko (2 MiB) is loaded from the SD card's
  # FAT partition (see rootfs/bin/wifi-up) to keep it out of unmovable RAM
  for m in ${kernel}/modules/*.ko; do
    [ "$(basename $m)" = 8189fs.ko ] || install -m644 $m $r/lib/modules/
  done
  install -D -m644 ${u.persond}/share/licenses/nvdla-LICENSE $r/usr/share/licenses/nvdla-LICENSE

  chmod 755 $r/init $r/bin/* $r/usr/share/udhcpc/default.script
  find $r -exec touch -h -d @1 {} +

  mkdir -p $out
  (cd $r && find . | LC_ALL=C sort | cpio -o -H newc --owner 0:0 --quiet) | gzip -9n > $out/initramfs.cpio.gz
  (cd $r && find . | LC_ALL=C sort) > $out/contents.txt
''
