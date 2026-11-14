# y623-mainline

Mainline Linux and a small, blob-free userspace for the **Yi 2K Pro home
camera (model Y623, Allwinner V831, 64 MiB DRAM)**. It boots from the SD card,
leaving the camera's SPI flash untouched.

What works:

- Linux 7.3.4 with drivers for the V831 clock/pin controllers, the GC3003
  sensor, the VIN/ISP capture pipeline, the H.264 encoder (cedrus-enc), the EISE
  lens-correction engine, the NPU and the audio codec. RTL8189FS Wi-Fi uses the
  external driver.
- **camrtsp**: RTSP server streaming 1920x1080 H.264 at 30 fps (`/main`) plus a
  640x360 sub-stream (`/sub`), with L16/G.711 microphone audio and hardware
  fisheye correction. It also handles auto exposure, white balance and
  day/night with the IR LEDs. Camera to decoded frame takes about 0.2 s.
- **onvifd**: ONVIF Profile S (discovery, media, imaging, PullPoint events).
- **persond**: person and cat detection with YOLOv5n on the NPU. It runs at
  ~50 ms per frame on the sub-stream and reports through ONVIF events, HTTP
  hooks (e.g. homebridge-camera-ffmpeg motion URLs) and `/tmp/person.state`.
- **Userspace**: static musl binaries of busybox, wpa_supplicant (WPA2-PSK)
  and dropbear (key-only SSH). The whole boot image is about 1.5 MB.

No vendor binaries are used or shipped. The only data taken from the stock
firmware is the GC3003 colour matrices (see the notice in
`kernel/patches/0008-*`).

## Layout

| Path | Contents | License |
|---|---|---|
| `kernel/` | patch series against linux-7.3.4 (+ Bootlin Cedrus overlay) | GPL-2.0 (DT: GPL-2.0+ OR MIT) |
| `u-boot/` | patch against OpenIPC U-Boot: SD boot, no autoboot delay | GPL-2.0+ |
| `userspace/` | camrtsp, onvifd, persond + NPU runtime, small tools | 0BSD |
| `ml/` | YOLOv5n -> NPU program compiler, int8 calibration data | 0BSD |
| `rootfs/` | init, scripts, `camera.conf` | 0BSD |
| `configs/` | busybox, wpa_supplicant, dropbear build configuration | 0BSD |
| `upstream.lock` | every third-party source, pinned by revision/hash | - |

Third-party code is fetched at build time and is not part of this repository;
see `upstream.lock` for each source and its license. Note when distributing
builds:

- persond includes NVIDIA's NVDLA firmware code (BSD-3-Clause; keep its notice).
- The YOLOv5n weights, and the `.nnp` program compiled from them, are
  AGPL-3.0 (Ultralytics).

## Building

    nix build                     # ./result: SD card files without secrets + U-Boot
    nix build .#kernel            # zImage, DTB, modules (incl. rtl8189fs), uAPI headers
    nix build .#userspace         # camrtsp onvifd persond busybox wpa_supplicant dropbear tools
    nix build .#model             # yolov5n.nnp
    nix run .#sd-image -- secrets/ sd.img            # full 64 MiB card image
    nix run .#sd-image -- --files secrets/ boot/     # only the FAT files, for an existing card
    nix develop                   # toolchains for working on the sources

Secrets never go into the repo or the Nix store: `sd-image` reads them from a
git-ignored directory and adds them to the initramfs outside the store.

- `wpa_supplicant.conf` (see `rootfs/etc/wpa_supplicant.conf.example`)
- `authorized_keys`
- `dropbear_ed25519_host_key` (generated on first use if missing)

The card image puts U-Boot at 8 KiB and one 32 MiB FAT16 partition at 1 MiB
holding zImage, initramfs.uImage, the DTB, 8189fs.ko and yolov5n.nnp. All
third-party sources are fetched by the hashes in `nix/sources.nix` and
`nix/files.nix` (the same pins as `upstream.lock`). `docs/building.md` has the
equivalent manual commands.

## Status

Personal project, not upstream-quality code and not supported. Use it at your own
risk (see LICENSE).
