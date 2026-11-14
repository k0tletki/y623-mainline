#!/usr/bin/env bash
# Regenerate the kernel patch series from the working tree.
#   ./mkpatches.sh [tree]     (default ../linux-7.3.4-v831-min)
# Diffs the tree against the pristine linux-7.3.4 tarball (../dl, sha256
# checked) using git tree objects only (no commits). Build artefacts are left
# out by the kernel's own .gitignore. Every changed path must be claimed by
# exactly one patch below or be listed in EXCLUDE, otherwise this fails.
#
# Upstream overlay: Bootlin's Cedrus driver with the H.264 encoder
# (github.com/bootlin/linux, branch cedrus/h264-encoding, commit 4e94979,
# drivers/staging/media/sunxi/cedrus) is placed at
# drivers/media/platform/sunxi/sun8i-cedrus-enc in the BASE tree, so patch 0009
# only carries our V831 changes on top of it. Apply the same overlay before the
# series (overlay.sh). BOOTLIN=<git checkout containing that commit>.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "${1:-$here/../linux-7.3.4-v831-min}" && pwd)
tarball=${TARBALL:-$here/../dl/linux-7.3.4.tar.xz}
bootlin=${BOOTLIN:-$here/../bootlin-linux-enc}
sha=b11b6df8295e8e991ff040b71aaa4994a8b55caa23b6b092907f195b5840fd1f
echo "$sha  $tarball" | sha256sum -c --quiet

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
tar -C "$work" -xf "$tarball"
"$here/overlay.sh" "$bootlin" "$work/linux-7.3.4"
export GIT_DIR=$work/git GIT_INDEX_FILE=$work/index
git init -q
git --work-tree="$work/linux-7.3.4" add -A
base=$(git write-tree)
git --work-tree="$tree" add -A
mod=$(git write-tree)

# Changed paths that are not part of the port
EXCLUDE=(
  'drivers/media/platform/sunxi/sun8i-v831-vin/sun8i_v831_vin.c.single' # old backup
  'arch/arm/configs/sun8iw19p1_defconfig'                               # superseded by y623_min_defconfig
  'tools/perf/util/bpf_skel/vmlinux/.gitignore'                         # deleted by a build, not by us
)

# name | subject | description | paths...
PATCHES=(
"0001-dt-bindings-sunxi-add-V831-V833-compatibles|dt-bindings: sunxi: add V831/V833 compatibles and CCU indices|CCU, MMC, pinctrl, SPI and watchdog compatibles plus the clock/reset index
headers for the sun8i-v833 CCU (V831 is the same die without the extra pins).|Documentation/devicetree/bindings include/dt-bindings/clock/sun8i-v833-ccu.h include/dt-bindings/reset/sun8i-v833-ccu.h"
"0002-clk-sunxi-ng-add-V833-CCU|clk: sunxi-ng: add the V831/V833 CCU|Based on Icenowy Zheng's out-of-tree V833 CCU driver (itself derived from H616).|drivers/clk/sunxi-ng"
"0003-pinctrl-sunxi-add-V83x|pinctrl: sunxi: add the V831/V833 pin controller|Based on Icenowy Zheng's out-of-tree V83x pinctrl driver.|drivers/pinctrl/sunxi"
"0004-rtc-sun6i-add-V831|rtc: sun6i: add V831 RTC clock data|16 MHz RC oscillator, fixed /32 prescaler, external LOSC enable and auto-switch.|drivers/rtc/rtc-sun6i.c"
"0005-iommu-sun50i-V831-and-per-master-bypass|iommu: sun50i: add V831 and bypass masters not claimed in DT|Only masters referenced by an \"iommus\" property are translated; all others
stay in bypass, so devices that do not use the IOMMU (VIN, NPU) keep working
with physical addresses.|drivers/iommu/sun50i-iommu.c"
"0006-ARM-sunxi-add-V831-machine-compatible|ARM: sunxi: add allwinner,sun8i-v831 to the sun8i board list||arch/arm/mach-sunxi/sunxi.c"
"0007-media-i2c-add-gc3003|media: i2c: add GalaxyCore GC3003 sensor driver|Mainline V4L2 subdev driver. Register sequences and the analogue gain table
come from the GPL-2.0 Rockchip gc3003 driver.|drivers/media/i2c"
"0008-media-sunxi-add-V831-VIN-ISP|media: sunxi: add V831 VIN (CSI + ISP521 + scaler) driver|Capture driver for the V831 video input: parallel/MIPI CSI, the ISP521 pipeline
(defaults and gamma tables from the GPL-2.0 Allwinner sunxi-vin sources) and
the main/sub-stream scalers.|drivers/media/platform/sunxi/sun8i-v831-vin"
"0009-media-sunxi-add-cedrus-enc|media: sunxi: add cedrus-enc (Cedrus with H.264 encoder) for V831|Bootlin's rework of the staging Cedrus driver into decoder/encoder procs with
the H.264 encoder (github.com/bootlin/linux cedrus/h264-encoding), on the V831 video
engine. Separate from staging VIDEO_SUNXI_CEDRUS; module cedrus-enc.|drivers/media/platform/sunxi/sun8i-cedrus-enc"
"0010-media-sunxi-add-V831-EISE|media: sunxi: add V831 EISE lens-distortion correction driver|/dev/eise: NV12 dmabuf -> dmabuf resampling through a per-tile mesh from
userspace. Register programming worked out on hardware.|drivers/media/platform/sunxi/sun8i-eise include/uapi/linux/sun8i-eise.h"
"0011-media-sunxi-build-V831-drivers|media: sunxi: hook up the V831 VIN, cedrus-enc and EISE drivers||drivers/media/platform/sunxi/Kconfig drivers/media/platform/sunxi/Makefile"
"0012-misc-add-sun8i-npu|misc: add Allwinner V831 NPU driver|/dev/npu: power, clocks, resets, register-window mmap and contiguous buffers;
the NVDLA units are programmed from userspace.|drivers/misc/sun8i-npu drivers/misc/Kconfig drivers/misc/Makefile include/uapi/linux/sun8i-npu.h"
"0013-ASoC-sunxi-add-V831-codec|ASoC: sunxi: add V831 internal audio codec|Mic ADC and line/speaker DAC with the analog block in the same MMIO window.|sound/soc/sunxi"
"0014-ARM-dts-sunxi-add-V831-and-Yi-Y623|ARM: dts: sunxi: add V831 SoC and Yi 2K Pro (Y623) camera|V831 SoC dtsi (based on Icenowy Zheng's) and the board DT: SD card, RTL8189FS
SDIO Wi-Fi, GC3003 sensor, IR LEDs/cut filter, codec, VE/EISE/NPU nodes and the
reserved-memory pools (CMA pools must be 2 MiB aligned on arm32).|arch/arm/boot/dts/allwinner"
"0015-ARM-configs-add-y623_min_defconfig|ARM: configs: add y623_min_defconfig|Minimal config for the camera (generated with make savedefconfig).|arch/arm/configs/y623_min_defconfig"
)

out=$here/patches; rm -rf "$out"; mkdir -p "$out"
: > "$out/series"
claimed=$work/claimed; : > "$claimed"
excl_spec=(); for e in "${EXCLUDE[@]}"; do excl_spec+=(":(exclude)$e"); done
for p in "${PATCHES[@]}"; do
  IFS='|' read -r -d '' name subj desc paths <<<"$p" || true
  read -ra pa <<<"$paths"
  f=$out/$name.patch
  { echo "Subject: [PATCH] $subj"; echo; [ -n "$desc" ] && { echo "$desc"; echo; }
    echo "---"; git diff --stat=78 "$base" "$mod" -- "${pa[@]}" "${excl_spec[@]}"; echo
    git diff --no-color --full-index "$base" "$mod" -- "${pa[@]}" "${excl_spec[@]}"; } > "$f"
  git diff --name-only "$base" "$mod" -- "${pa[@]}" "${excl_spec[@]}" >> "$claimed"
  grep -q '^diff --git' "$f" || { echo "empty patch: $name" >&2; exit 1; }
  echo "$name.patch" >> "$out/series"
done

all=$(git diff --name-only "$base" "$mod" | sort)
ex=$(printf '%s\n' "${EXCLUDE[@]}" | sort)
left=$(comm -23 <(echo "$all") <(sort "$claimed" | cat - <(echo "$ex") | sort -u))
dup=$(sort "$claimed" | uniq -d)
[ -z "$left" ] || { echo "unclaimed changed paths:" >&2; echo "$left" >&2; exit 1; }
[ -z "$dup" ] || { echo "paths in more than one patch:" >&2; echo "$dup" >&2; exit 1; }

# Check: the series applies to a clean tree and reproduces the working tree
chk=$work/check; mkdir "$chk"; tar -C "$chk" -xf "$tarball"
env -u GIT_DIR -u GIT_INDEX_FILE "$here/overlay.sh" "$bootlin" "$chk/linux-7.3.4"
while read -r s; do patch -s -p1 -d "$chk/linux-7.3.4" < "$out/$s"; done < "$out/series"
git --work-tree="$chk/linux-7.3.4" add -A
got=$(git write-tree)
want=$(git diff --name-only "$base" "$mod" | grep -vxF -f <(printf '%s\n' "${EXCLUDE[@]}") || true)
bad=$(git diff --name-only "$got" "$mod" | grep -vxF -f <(printf '%s\n' "${EXCLUDE[@]}") || true)
[ -z "$bad" ] || { echo "series does not reproduce the tree:" >&2; echo "$bad" >&2; exit 1; }
echo "$(wc -l < "$out/series") patches, $(echo "$want" | wc -l) files; applies cleanly and matches $tree"
