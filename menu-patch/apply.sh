#!/bin/sh
# Shift+Pad 16 "boot thingy" row, applied by mpc-boot-thingy.
#
# ac50 starts through /usr/bin/launch-ac50. This bind-mounts a small wrapper
# over that script so the stock app gets LD_PRELOAD. The loader stores the
# patch bytes in the process. /usr/bin/MPC Sample on disk is not modified.
# Flash is not written. Reboot drops /run and the mounts. This does not
# restart ac50.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STOCK="${MPC_STOCK:-/usr/bin/MPC Sample}"
LAUNCH="${MPC_LAUNCH:-/usr/bin/launch-ac50}"
PATCH_DIR="$HERE/patches"
LOADER_SRC="$HERE/loader.so"
LOADER=/run/mpc-patch-loader.so
REAL=/run/launch-ac50.real
WRAP=/run/launch-ac50.wrap
STAMP=/run/mpc-patch-stamp

log() { echo "[boot-menu] $*"; }
die() { echo "[boot-menu] ERROR: $*" >&2; exit 1; }

# mountinfo field 5 is the mount point. A hit means a bind is in place.
mounted() {
    target=$1
    [ -r /proc/self/mountinfo ] || return 1
    while read -r _ _ _ _ point _; do
        [ "$point" = "$target" ] && return 0
    done < /proc/self/mountinfo
    return 1
}

# A leftover copy of the app is bind-mounted over the stock path. The space
# in that path is escaped as \040. Drop it so the loader sees the stock file.
unmount_stock() {
    if ! grep -q -F 'MPC\040Sample' /proc/self/mountinfo 2>/dev/null; then
        return 0
    fi
    umount "$STOCK" || die "could not unmount the previous patched screen"
}

# Single-quote a string for the wrapper, including embedded quotes.
sq() {
    s=$1
    printf "'"
    while :; do
        case $s in
            *\'*)
                printf '%s' "${s%%\'*}"
                printf "'\\''"
                s=${s#*\'}
                ;;
            *)
                printf '%s' "$s"
                break
                ;;
        esac
    done
    printf "'"
}

[ -d "$PATCH_DIR" ] || die "missing $PATCH_DIR"
[ -r "$LOADER_SRC" ] || die "missing $LOADER_SRC"
[ -r "$LAUNCH" ] || die "cannot read $LAUNCH"

# The stamp is the patch directory last mounted. A Boot Edit entry uses
# its own directory, so a mount for a different set has to be replaced.
stamp_matches() {
    [ -f "$STAMP" ] || return 1
    got=$(cat "$STAMP") || return 1
    [ "$got" = "$PATCH_DIR" ]
}

unmount_stock
if mounted "$LAUNCH"; then
    if stamp_matches; then
        log "patch loader already mounted"
        exit 0
    fi
    umount "$LAUNCH" || die "could not replace the patch loader"
fi

cp "$LOADER_SRC" "$LOADER"
chmod 755 "$LOADER"

# Copy the stock script. A bind of /usr/bin/launch-ac50 follows the shared
# rootfs, so the later wrapper mount would land on the copy too and the
# wrapper would exec itself.
if ! mounted "$REAL"; then
    cp "$LAUNCH" "$REAL" || die "could not keep the stock launcher"
    chmod 755 "$REAL"
fi

{
    printf '%s\n' '#!/bin/sh'
    printf '%s\n' 'export LD_PRELOAD=/run/mpc-patch-loader.so'
    printf 'export MPC_PATCH_DIR=%s\n' "$(sq "$PATCH_DIR")"
    printf '%s\n' 'exec /run/launch-ac50.real "$@"'
} > "$WRAP"
chmod 755 "$WRAP"
mount --bind "$WRAP" "$LAUNCH" || die "could not mount the patch loader"
printf '%s\n' "$PATCH_DIR" > "$STAMP" || die "could not record the patch directory"
case "$HERE" in
    */system) AUTORUN=$(CDPATH= cd -- "$HERE/.." && pwd) ;;
    *) AUTORUN=$HERE ;;
esac
printf '%s\n' "$AUTORUN" > /run/mpc-boot-autorun 2>/dev/null || true
log "patch loader mounted"

