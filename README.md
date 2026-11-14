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

See `docs/building.md`. A Nix flake is planned. Secrets never go into the repo
or the Nix store: the build reads them from the git-ignored `secrets/`.

- `secrets/wpa_supplicant.conf` (see `rootfs/etc/wpa_supplicant.conf.example`)
- `secrets/authorized_keys`
- `secrets/dropbear_ed25519_host_key`

## Status

Personal project, not upstream-quality code and not supported. Use it at your own
risk (see LICENSE).
