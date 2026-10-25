#!/bin/sh
# Shift+Pad 16 "Boot Menu" row, applied by mpc-boot-thingy.
#
# ac50 always starts /usr/bin/MPC Sample. The patched copy lives on the card.
# The first boot, and any boot whose patch hash differs, builds that copy.
# Every other boot bind-mounts the copy that is already there.
# The card is remounted exec, so the mounted file can run.
# Flash is not written. Reboot drops the mount. This does not restart ac50.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CARD=$(CDPATH= cd -- "$(dirname -- "$HERE")" && pwd)
STOCK="${MPC_STOCK:-/usr/bin/MPC Sample}"
PATCH_LIST="$HERE/patches.list"
OUT="$HERE/menu-os"
STAGE="$HERE/menu-os.building"
STAMP="$HERE/menu-os.sha"
ORIG=/tmp/mpc-patch-orig
NEW=/tmp/mpc-patch-new

log() { echo "[boot-menu] $*"; }
die() { echo "[boot-menu] ERROR: $*" >&2; exit 1; }

decode_b64() {
    if command -v base64 >/dev/null 2>&1; then
        printf '%s' "$1" | base64 -d > "$2"
    else
        printf '%s' "$1" | openssl base64 -d -A > "$2"
    fi
}

# mountinfo escapes the space in "/usr/bin/MPC Sample" as \040. That line is
# present only while a patched copy is bind-mounted over the stock app.
unmount_stock() {
    if ! grep -q -F 'MPC\040Sample' /proc/self/mountinfo 2>/dev/null; then
        return 0
    fi
    umount "$STOCK" || die "could not unmount the previous patched screen"
}

mount_out() {
    mount -o remount,exec "$CARD" || die "could not remount the card exec"
    unmount_stock
    mount --bind "$OUT" "$STOCK" || die "bind-mount failed"
    log "patched stock screen mounted from the card"
    exit 0
}

[ -r "$PATCH_LIST" ] || die "missing $PATCH_LIST"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum not found"
[ -x "$HERE/poke" ] || die "missing $HERE/poke"
if ! command -v base64 >/dev/null 2>&1 && ! command -v openssl >/dev/null 2>&1; then
    die "base64 or openssl is required"
fi

TARGET_SHA=$(grep '^#target-sha256 ' "$PATCH_LIST" | cut -d' ' -f2)
RESULT_SHA=$(grep '^#result-sha256 ' "$PATCH_LIST" | cut -d' ' -f2)
[ -n "$TARGET_SHA" ] || die "patch list has no #target-sha256"
[ -n "$RESULT_SHA" ] || die "patch list has no #result-sha256"

if [ -f "$OUT" ] && [ -f "$STAMP" ]; then
    stamp=$(cat "$STAMP")
    if [ "$stamp" = "$RESULT_SHA" ]; then
        mount_out
    fi
    log "card copy does not match this patch set, building it again"
fi

unmount_stock
[ -r "$STOCK" ] || die "cannot read $STOCK"
stock_sha=$(sha256sum "$STOCK" | cut -d' ' -f1)
if [ "$stock_sha" = "$RESULT_SHA" ]; then
    log "patched stock screen already mounted"
    exit 0
fi
if [ "$stock_sha" != "$TARGET_SHA" ]; then
    die "stock sha256 $stock_sha != target $TARGET_SHA"
fi
log "building patched stock screen on the card"

rm -f "$STAGE"
cp "$STOCK" "$STAGE"
while read -r off b64o b64p; do
    case "$off" in ''|\#*) continue ;; esac
    decode_b64 "$b64o" "$ORIG"
    decode_b64 "$b64p" "$NEW"
    len=$(wc -c < "$NEW")
    len=${len##* }
    cur=$(dd if="$STAGE" bs=1 skip="$off" count="$len" 2>/dev/null | sha256sum | cut -d' ' -f1)
    exp=$(sha256sum "$ORIG" | cut -d' ' -f1)
    [ "$cur" = "$exp" ] || die "patch at offset $off does not match"
    "$HERE/poke" "$STAGE" "$off" < "$NEW" || die "could not write patch at offset $off"
done < "$PATCH_LIST"
rm -f "$ORIG" "$NEW"

got=$(sha256sum "$STAGE" | cut -d' ' -f1)
[ "$got" = "$RESULT_SHA" ] || die "patched result $got != $RESULT_SHA"
chmod 755 "$STAGE"
mv "$STAGE" "$OUT"
printf '%s\n' "$RESULT_SHA" > "$STAMP"
mount -o remount,exec "$CARD" || die "could not remount the card exec"
mount --bind "$OUT" "$STOCK" || die "bind-mount failed"
log "patched stock screen mounted from the card"
