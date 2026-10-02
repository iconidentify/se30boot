#!/bin/sh
# install-se30boot.sh TARGET: install se30boot on the SE/30's card (a block
# device such as /dev/sdX) or on a disk image. It copies stage 2 to the Mac
# volume as :Linux:se30boot (locked), writes :Linux:se30boot.cfg if there
# isn't one, unpacks :Linux:vmlinux.gz to :Linux:vmlinux (a gzipped kernel
# takes about a minute to unpack on a 16 MHz 68030; this loads in seconds),
# and points the boot blocks at stage 1. Remove it again with
#   tools/se30boot.py remove TARGET
#   SE30BOOT_CFG   a configuration file to install instead of se30boot.cfg
#                  (replaces the one on the volume)
set -e
TARGET=${1:?usage: install-se30boot.sh /dev/sdX or disk image}
P=$(dirname "$(readlink -f "$0")")/..
B=$P/build
[ -f "$B/stage1.bin" ] && [ -f "$B/se30boot" ] || { echo "build first: ./make.sh"; exit 1; }
if [ -b "$TARGET" ]; then
    for part in "$TARGET"?*; do
        if findmnt -n "$part" >/dev/null 2>&1; then udisksctl unmount -b "$part"; fi
    done
    DOCKER_TARGET="--device $TARGET"
    T=$TARGET
else
    TARGET=$(readlink -f "$TARGET")
    DOCKER_TARGET="-v $(dirname "$TARGET"):/target"
    T=/target/$(basename "$TARGET")
fi
CFG=$(mktemp)
trap 'rm -f "$CFG"' EXIT
cp "${SE30BOOT_CFG:-$P/se30boot.cfg}" "$CFG"
chmod a+r "$CFG"
sudo docker run --rm $DOCKER_TARGET -v "$B":/sb:ro -v "$P/tools":/tools:ro -v "$CFG":/cfg:ro \
    -e REPLACE_CFG="${SE30BOOT_CFG:+1}" se30-build sh -c "
    set -e
    hmount $T 1 >/dev/null
    hls :Linux >/dev/null
    has() { hls -1a :Linux | grep -qx \"\$1\"; }
    if has vmlinux.gz; then
        hcopy -r :Linux:vmlinux.gz /tmp/vmlinux.gz
        gunzip -f /tmp/vmlinux.gz
        if has vmlinux; then hdel :Linux:vmlinux; fi
        hcopy -r /tmp/vmlinux :Linux:vmlinux
        hattrib -t BINA -c '????' :Linux:vmlinux
    fi
    if has se30boot; then hattrib -l :Linux:se30boot; hdel :Linux:se30boot; fi
    hcopy -r /sb/se30boot :Linux:se30boot
    hattrib -t boot -c SE3B :Linux:se30boot
    hattrib +l :Linux:se30boot
    if [ -n \"\$REPLACE_CFG\" ] || ! has se30boot.cfg; then
        if has se30boot.cfg; then hdel :Linux:se30boot.cfg; fi
        hcopy -t /cfg :Linux:se30boot.cfg
        hattrib -t TEXT -c ttxt :Linux:se30boot.cfg
    fi
    humount
    python3 /tools/se30boot.py install $T /sb/stage1.bin Linux:se30boot
    python3 /tools/se30boot.py info $T"
sync
