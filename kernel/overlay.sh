#!/usr/bin/env bash
# overlay.sh BOOTLIN_GIT KERNEL_TREE: put Bootlin's Cedrus (+H.264 encoder)
# driver at drivers/media/platform/sunxi/sun8i-cedrus-enc before the series.
#   bootlin/linux @ 4e9497947c56f10351ed8c000605dbe47d4aee46
#   path drivers/staging/media/sunxi/cedrus (26 files, GPL-2.0)
set -euo pipefail
rev=4e9497947c56f10351ed8c000605dbe47d4aee46
dst=$2/drivers/media/platform/sunxi/sun8i-cedrus-enc
mkdir -p "$dst"
if [ -d "$1/.git" ] || git -C "$1" rev-parse --git-dir >/dev/null 2>&1; then
  env -u GIT_DIR -u GIT_INDEX_FILE -u GIT_WORK_TREE \
    git -C "$1" archive "$rev" drivers/staging/media/sunxi/cedrus | tar -x -C "$dst" --strip-components=5
else
  cp "$1"/* "$dst"/   # a plain directory holding exactly those files (e.g. from the flake)
fi
