#!/bin/sh
# Fixture checks for setup.sh. Does not touch a real card.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT=$(pwd)
SSH_DIR=$(CDPATH= cd -- "$ROOT/../mpc-sample-root-ssh/autorun" && pwd)
SALT='f7bed1c9-7e90-4731-9194-c996666b4331'

fail() { echo "FAIL: $*" >&2; exit 1; }

digest_of() {
  dir=$1
  if command -v shasum >/dev/null 2>&1; then
    { cat "$dir/autorun.json" "$dir/autorun.sh"; printf '%s' "$SALT"; } | shasum -a 256 | awk '{print $1}'
  else
    { cat "$dir/autorun.json" "$dir/autorun.sh"; printf '%s' "$SALT"; } | sha256sum | awk '{print $1}'
  fi
}

# Answer on a pseudo-terminal. "-" means write nothing.
pty_setup() {
  answer=$1
  sd=$2
  python3 -c '
import os, sys
answer, setup, sd = sys.argv[1], sys.argv[2], sys.argv[3]
pid, fd = os.forkpty()
if pid == 0:
    os.execv(setup, [setup, "--SD", sd])
os.write(fd, b"") if False else None
if answer != "-":
    os.write(fd, (answer + "\n").encode())
data = b""
while True:
    try:
        chunk = os.read(fd, 4096)
    except OSError:
        break
    if not chunk:
        break
    data += chunk
_, status = os.waitpid(pid, 0)
code = status >> 8 if os.WIFEXITED(status) else 1
sys.stdout.buffer.write(data)
sys.exit(code)
' "$answer" "$ROOT/setup.sh" "$sd"
}

out=$(./setup.sh --help)
printf '%s\n' "$out" | grep -q -- '--SD' || fail "help missing --SD"
printf '%s\n' "$out" | grep -q 'boot.entry' || fail "help missing boot.entry"
printf '%s\n' "$out" | grep -q 'title=Lab firmware' || fail "help missing entry format"
printf '%s\n' "$out" | grep -q -- '--add' || fail "help missing --add"
printf '%s\n' "$out" | grep -q '2222' || fail "help missing ssh port"
printf '%s\n' "$out" | grep -q 'press Enter' || fail "help missing empty password"
printf '%s\n' "$out" | grep -q -- '--ssh' || fail "help missing --ssh"
printf '%s\n' "$out" | grep -q -- '--reboot' || fail "help missing --reboot"
./setup.sh -h >/dev/null
./setup.sh >/dev/null

if ./setup.sh --nope >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "unknown flag should exit 1"; fi
grep -q -- '--SD' /tmp/mpc-boot-err || fail "unknown flag should print help on stderr"
if ./setup.sh --SD >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "--SD needs a path"; fi
if ./setup.sh --SD / >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "refused /"; fi
if ./setup.sh --SD "/no/such/mpc-boot-thingy-$$" >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "missing dir should fail"; fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mpc-boot-thingy-setup.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Empty card: no prompt, installs, check matches, script does not mention a backup.
empty=$work/empty
mkdir "$empty"
./setup.sh --SD "$empty" >/tmp/mpc-boot-out
test -f "$empty/autorun/mpc-boot-thingy.mark"
test -d "$empty/autorun/boot-menu"
test -x "$empty/autorun/mpc-boot-thingy"
grep -q 'systemd-run --unit=mpc-boot-thingy' "$empty/autorun/autorun.sh"
grep -q 'systemd-run --unit=mpc-boot-ssh' "$empty/autorun/autorun.sh"
if grep -q 'autorun-backup' "$empty/autorun/autorun.sh"; then fail "new autorun mentions a backup"; fi
if grep -q 'mpc-sample-usb-ssh' "$empty/autorun/autorun.sh" "$empty/autorun/ssh.sh"; then fail "new autorun starts the ssh kit"; fi
if grep -q 'authorized_keys' "$empty/autorun/ssh.sh"; then fail "ssh still expects a client key"; fi
grep -q 'PermitEmptyPasswords yes' "$empty/autorun/ssh.sh"
grep -q 'PubkeyAuthentication no' "$empty/autorun/ssh.sh"
grep -q 'UsePAM no' "$empty/autorun/ssh.sh"
if grep -Eq '^[[:space:]]*AuthenticationMethods' "$empty/autorun/ssh.sh"; then
  fail "blank password must not be tied to AuthenticationMethods"
fi
test -s "$empty/autorun/ssh_host_ed25519_key"
test -s "$empty/autorun/ssh_host_ed25519_key.pub"
hostkey=$(shasum -a 256 "$empty/autorun/ssh_host_ed25519_key" | awk '{print $1}')
got=$(digest_of "$empty/autorun")
have=$(cat "$empty/autorun/autorun.check")
test "$got" = "$have" || fail "autorun.check mismatch"

# Re-run on our own install updates without a backup prompt and keeps menu entries.
printf 'title=Snake\nrun=apps/snake\nargs=\n' > "$empty/autorun/boot-menu/30-snake.entry"
./setup.sh --SD "$empty" >/tmp/mpc-boot-out
grep -q 'Updating it' /tmp/mpc-boot-out || fail "reinstall should update without asking"
grep -q 'title=Snake' "$empty/autorun/boot-menu/30-snake.entry" || fail "reinstall removed a menu entry"
test "$(shasum -a 256 "$empty/autorun/ssh_host_ed25519_key" | awk '{print $1}')" = "$hostkey" || fail "reinstall replaced the server identity"
if find "$empty" -maxdepth 1 -type d -name 'autorun-backup-*' | grep -q .; then
  fail "reinstall of mpc-boot-thingy created a backup"
fi

# USB SSH fixture: a pipe is not a terminal, so nothing is written.
ssh=$work/ssh
mkdir "$ssh"
cp "$SSH_DIR/autorun.json" "$SSH_DIR/autorun.sh" "$SSH_DIR/autorun.check" "$ssh/autorun/" 2>/dev/null || {
  mkdir -p "$ssh/autorun"
  cp "$SSH_DIR/autorun.json" "$SSH_DIR/autorun.sh" "$SSH_DIR/autorun.check" "$ssh/autorun/"
}
before=$(digest_of "$ssh/autorun")
if ./setup.sh --SD "$ssh" </dev/null >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "non-tty replace should exit 1"
fi
grep -q 'USB SSH kit' /tmp/mpc-boot-err /tmp/mpc-boot-out || fail "did not name the USB SSH kit"
grep -q 'without a terminal' /tmp/mpc-boot-err || fail "non-tty warning missing"
test "$(digest_of "$ssh/autorun")" = "$before" || fail "non-tty changed autorun"
test ! -e "$ssh"/autorun-backup-* 

# Declining the backup on a terminal leaves the kit in place.
pty_setup n "$ssh" >/tmp/mpc-boot-out
grep -q 'Not changed' /tmp/mpc-boot-out || fail "decline should say Not changed"
grep -q 'USB SSH kit' /tmp/mpc-boot-out || fail "decline should name the USB SSH kit"
test "$(digest_of "$ssh/autorun")" = "$before" || fail "decline changed autorun"
test ! -d "$ssh"/autorun-backup-* 2>/dev/null || fail "decline created a backup"

# Accepting backs up and replaces. The new script only starts mpc-boot-thingy.
pty_setup y "$ssh" >/tmp/mpc-boot-out
test -d "$ssh"/autorun-backup-*
test -f "$ssh/autorun/mpc-boot-thingy.mark"
grep -q 'systemd-run --unit=mpc-boot-thingy' "$ssh/autorun/autorun.sh"
if grep -q 'autorun-backup' "$ssh/autorun/autorun.sh"; then fail "replaced script references the backup"; fi
if grep -q 'mpc-sample-usb-ssh' "$ssh/autorun/autorun.sh"; then fail "replaced script still launches ssh"; fi
test "$(digest_of "$ssh/autorun")" = "$(cat "$ssh/autorun/autorun.check")" || fail "replaced check mismatch"
# The backup still holds the old kit and is not wired into the new script.
bak=$(find "$ssh" -maxdepth 1 -type d -name 'autorun-backup-*' | head -n 1)
grep -q 'mpc-sample-usb-ssh' "$bak/autorun.sh" || fail "backup lost the ssh script"

# --add needs a card, and it does not install or replace autorun.
if ./setup.sh --add "$work/missing" >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "--add without --SD should fail"
fi
bare=$work/bare
mkdir "$bare"
if ./setup.sh --SD "$bare" --add "$work/missing-bin" --title "Nope" >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "missing program should fail"
fi
test ! -d "$bare/autorun"
test ! -d "$bare/apps"

# A lone executable gets a folder and a boot.entry. The menu label is the title.
printf '#!/bin/sh\nprintf hi\n' > "$work/snake"
chmod 755 "$work/snake"
./setup.sh --SD "$bare" --add "$work/snake" --title "Snake Game" >/tmp/mpc-boot-out
grep -q 'Installed Snake Game' /tmp/mpc-boot-out
grep -q 'boot menu is not on this card' /tmp/mpc-boot-out
printf 'title=Snake Game\nrun=snake\n' > "$work/expect.entry"
cmp "$work/expect.entry" "$bare/apps/snake-game/boot.entry"
cmp "$work/snake" "$bare/apps/snake-game/snake"
test ! -d "$bare/autorun"

# A project folder contributes its boot.entry and the program it names, not its sources.
proj=$work/firmware
mkdir "$proj"
printf 'title=Lab firmware\nrun=mpcfw\n# keep\n' > "$proj/boot.entry"
printf 'device-binary\n' > "$proj/mpcfw"
printf 'int main(){return 0;}\n' > "$proj/extra.c"
./setup.sh --SD "$empty" --add "$proj" >/tmp/mpc-boot-out
grep -q 'Installed Lab firmware' /tmp/mpc-boot-out
if grep -q 'boot menu is not on this card' /tmp/mpc-boot-out; then
  fail "card that already has the menu should not ask to install it"
fi
cmp "$proj/boot.entry" "$empty/apps/lab-firmware/boot.entry"
cmp "$proj/mpcfw" "$empty/apps/lab-firmware/mpcfw"
test ! -e "$empty/apps/lab-firmware/extra.c"
# Updating the boot menu leaves the dropped-in app alone.
./setup.sh --SD "$empty" >/tmp/mpc-boot-out
cmp "$proj/mpcfw" "$empty/apps/lab-firmware/mpcfw"
grep -q 'title=Snake' "$empty/autorun/boot-menu/30-snake.entry"

# --ssh finds the one /media directory and overwrites without a backup prompt.
if ./setup.sh --ssh --SD "$empty" >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "--ssh must not take --SD"
fi
grep -q 'does not take' /tmp/mpc-boot-err || fail "--ssh --SD should explain itself"

bin=$work/bin
mkdir -p "$bin"
cat > "$bin/ssh" <<'EOF'
#!/bin/sh
last=
for a in "$@"; do last=$a; done
if [ -n "${FAKE_SSH_LOG:-}" ]; then printf '%s\n' "$last" >> "$FAKE_SSH_LOG"; fi
case "$last" in
  *reboot*)
    exit 0
    ;;
  *'find /media'*)
    if [ "${FAKE_MEDIA_COUNT:-1}" = 0 ]; then exit 0; fi
    if [ "${FAKE_MEDIA_COUNT:-1}" = 2 ]; then
      printf '%s\n' '/media/One' '/media/Two'
      exit 0
    fi
    if [ "${FAKE_SSH_FAIL:-0}" = 1 ]; then exit 1; fi
    printf '%s\n' '/media/Lab Card'
    ;;
  *'test -s'*)
    IFS= read -r dest
    test -s "$FAKE_ROOT$dest/autorun/ssh_host_ed25519_key"
    ;;
  *'tar -C'*)
    IFS= read -r dest
    mkdir -p "$FAKE_ROOT$dest"
    tar -C "$FAKE_ROOT$dest" -xf -
    ;;
  *)
    echo "unexpected ssh: $last" >&2
    exit 1
    ;;
esac
EOF
chmod 755 "$bin/ssh"
remote=$work/remote
card="$remote/media/Lab Card"
mkdir -p "$card/autorun/boot-menu"
printf 'title=Snake\nrun=snake\n' > "$card/autorun/boot-menu/30-snake.entry"
printf 'keep-key\n' > "$card/autorun/ssh_host_ed25519_key"

PATH="$bin:$PATH" FAKE_ROOT=$remote ./setup.sh --ssh >/tmp/mpc-boot-out
grep -q 'Updated /media/Lab Card over SSH' /tmp/mpc-boot-out || fail "ssh install did not name the card"
if grep -q 'Back up the existing' /tmp/mpc-boot-out; then fail "ssh install asked for a backup"; fi
test ! -d "$remote"/autorun-backup-* 
grep -q 'title=Snake' "$card/autorun/boot-menu/30-snake.entry" || fail "ssh install removed a menu entry"
test "$(cat "$card/autorun/ssh_host_ed25519_key")" = keep-key || fail "ssh install replaced the server identity"
test -x "$card/autorun/mpc-boot-thingy" || test -f "$card/autorun/mpc-boot-thingy"
grep -q 'systemd-run --unit=mpc-boot-ssh' "$card/autorun/autorun.sh"
test "$(digest_of "$card/autorun")" = "$(cat "$card/autorun/autorun.check")" || fail "ssh install check mismatch"

if PATH="$bin:$PATH" FAKE_ROOT=$remote FAKE_MEDIA_COUNT=0 ./setup.sh --ssh >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "no /media directory should fail"
fi
grep -q 'no SD card' /tmp/mpc-boot-err || fail "missing card message"
if PATH="$bin:$PATH" FAKE_ROOT=$remote FAKE_MEDIA_COUNT=2 ./setup.sh --ssh >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "two /media directories should fail"
fi
grep -q 'expected one directory' /tmp/mpc-boot-err || fail "two-card message"

if ./setup.sh --reboot --SD "$empty" >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "--reboot must not take --SD"
fi
grep -q 'only works alone or with --ssh' /tmp/mpc-boot-err || fail "--reboot --SD should explain itself"

sshlog=$work/ssh.log
: > "$sshlog"
PATH="$bin:$PATH" FAKE_SSH_LOG=$sshlog ./setup.sh -r >/tmp/mpc-boot-out
grep -q 'Rebooting the MPC' /tmp/mpc-boot-out || fail "reboot did not say it is rebooting"
grep -q reboot "$sshlog" || fail "reboot did not ask the MPC to reboot"
grep -q bootcount "$sshlog" || fail "reboot did not clear the boot counter"
test "$(wc -l < "$sshlog" | tr -d ' ')" = 1 || fail "reboot alone should not install"

: > "$sshlog"
PATH="$bin:$PATH" FAKE_ROOT=$remote FAKE_SSH_LOG=$sshlog ./setup.sh --ssh --reboot >/tmp/mpc-boot-out
grep -q 'Updated /media/Lab Card over SSH' /tmp/mpc-boot-out || fail "ssh -r did not install"
grep -q 'Rebooting the MPC' /tmp/mpc-boot-out || fail "ssh -r did not reboot"
grep -q 'find /media' "$sshlog" || fail "ssh -r did not look for the card"
tail -n 1 "$sshlog" | grep -q reboot || fail "ssh -r should reboot after the install"

echo "setup fixtures ok"
