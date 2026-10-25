#!/bin/sh
# Fixture checks for setup.sh. Does not touch a real card.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT=$(pwd)
SSH_DIR=$(CDPATH= cd -- "$ROOT/../mpc-sample-root-ssh/autorun" && pwd)
SALT='f7bed1c9-7e90-4731-9194-c996666b4331'

fail() { echo "FAIL: $*" >&2; exit 1; }

web_page() {
  dir=$1
  test -f "$dir/index.html"
  test -f "$dir/xterm.js"
  test -f "$dir/xterm.css"
  test -f "$dir/xterm-addon-fit.js"
  test -f "$dir/xterm.LICENSE"
  python3 - "$dir" <<'PY'
import re, sys
from pathlib import Path
web = Path(sys.argv[1])
html = (web / "index.html").read_text()
refs = re.findall(r'(?:src|href)="(/[^"]+)"', html)
built = [ref for ref in refs if ref.startswith("/assets/")]
if not any(ref.endswith(".js") for ref in built) or not any(ref.endswith(".css") for ref in built):
    sys.exit("page is missing its built script or stylesheet")
for ref in refs:
    path = web.joinpath(*ref.strip("/").split("/"))
    if not path.is_file():
        sys.exit("missing " + ref)
PY
}

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
printf '%s\n' "$out" | grep -q '192.168.155.1:8080' || fail "help missing web page"
printf '%s\n' "$out" | grep -q 'press Enter' || fail "help missing empty password"
printf '%s\n' "$out" | grep -q -- '--ssh' || fail "help missing --ssh"
printf '%s\n' "$out" | grep -q -- '--reboot' || fail "help missing --reboot"
printf '%s\n' "$out" | grep -q 'os_tools/bin' || fail "help missing os_tools"
printf '%s\n' "$out" | grep -q 'os_tools/.bashrc' || fail "help missing bashrc"
printf '%s\n' "$out" | grep -q -- '--tools' || fail "help missing --tools"
if python3 ./fetch-toolkit.py >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then
  fail "fetch-toolkit without a directory should fail"
fi
grep -q -- '--tools' /tmp/mpc-boot-err || fail "fetch-toolkit usage should mention --tools"
if ./setup.sh --tools >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "--tools alone should fail"; fi
grep -q 'needs --SD or --ssh' /tmp/mpc-boot-err || fail "--tools alone should explain itself"
if ./setup.sh --reboot --tools >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "--tools --reboot should fail"; fi
grep -q 'needs --SD or --ssh' /tmp/mpc-boot-err || fail "--tools --reboot should explain itself"
if ./setup.sh --SD /tmp --add /tmp/x --tools >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "--tools --add should fail"; fi
grep -q 'not used with --add' /tmp/mpc-boot-err || fail "--tools --add should explain itself"
if ./setup.sh --ssh --add /tmp/x --tools >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "--ssh --tools --add should fail"; fi
grep -q 'not used with --add' /tmp/mpc-boot-err || fail "--ssh --tools --add should explain itself"
./setup.sh -h >/dev/null
./setup.sh >/dev/null

if ./setup.sh --nope >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "unknown flag should exit 1"; fi
grep -q -- '--SD' /tmp/mpc-boot-err || fail "unknown flag should print help on stderr"
if ./setup.sh --SD >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "--SD needs a path"; fi
if ./setup.sh --SD / >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "refused /"; fi
if ./setup.sh --SD "/no/such/mpc-boot-thingy-$$" >/tmp/mpc-boot-out 2>/tmp/mpc-boot-err; then fail "missing dir should fail"; fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mpc-boot-thingy-setup.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Fixtures stand in for the downloaded toolkit so these checks stay offline.
# The default tree is bash. --tools copies the dev tree.
fix=$work/toolkit-src
mkdir -p "$fix/bin" "$fix/toolkit/bin" "$fix/toolkit/lib"
printf '#!/bin/sh\necho bash\n' > "$fix/bin/bash"
chmod 755 "$fix/bin/bash"
printf 'real-bash\n' > "$fix/toolkit/bin/bash"
printf 'ld\n' > "$fix/toolkit/lib/ld-musl-aarch64.so.1"
printf 'bash-marker\n' > "$fix/toolkit/marker"
export MPC_TOOLKIT_SRC=$fix
devfix=$work/toolkit-dev
mkdir -p "$devfix/bin" "$devfix/toolkit/bin" "$devfix/toolkit/lib"
printf '#!/bin/sh\necho bash\n' > "$devfix/bin/bash"
chmod 755 "$devfix/bin/bash"
printf '#!/bin/sh\necho vim\n' > "$devfix/bin/vim"
chmod 755 "$devfix/bin/vim"
printf '#!/bin/sh\necho busybox\n' > "$devfix/bin/busybox-full"
chmod 755 "$devfix/bin/busybox-full"
printf 'real-vim\n' > "$devfix/toolkit/bin/vim"
printf 'ld\n' > "$devfix/toolkit/lib/ld-musl-aarch64.so.1"
printf 'dev-marker\n' > "$devfix/toolkit/marker"
export MPC_TOOLKIT_TOOLS_SRC=$devfix

# Empty card: no prompt, installs, check matches, script does not mention a backup.
empty=$work/empty
mkdir "$empty"
./setup.sh --SD "$empty" >/tmp/mpc-boot-out
grep -q 'os_tools/bin' /tmp/mpc-boot-out || fail "install did not name os_tools"
if grep -q 'Dev tools are in' /tmp/mpc-boot-out; then fail "default install announced dev tools"; fi
test -x "$empty/os_tools/bin/bash"
cmp "$fix/bin/bash" "$empty/os_tools/bin/bash"
cmp "$fix/toolkit/marker" "$empty/os_tools/toolkit/marker"
test ! -e "$empty/os_tools/bin/vim"
test ! -e "$empty/os_tools/bin/busybox-full"
test -f "$empty/os_tools/.bashrc"
grep -q 'BASH_SOURCE' "$empty/os_tools/.bashrc"
grep -q 'PATH' "$empty/os_tools/.bashrc"
grep -q '.bashrc' "$empty/os_tools/.bash_profile"
test ! -e "$empty/bin"
test ! -e "$empty/toolkit"
test ! -d "$empty/apps"
test -f "$empty/autorun/mpc-boot-thingy.mark"
test -d "$empty/autorun/boot-menu"
test -x "$empty/autorun/mpc-boot-thingy"
grep -q 'systemd-run --unit=mpc-boot-thingy' "$empty/autorun/autorun.sh"
grep -q 'systemd-run --unit=mpc-boot-web' "$empty/autorun/autorun.sh"
grep -q 'systemd-run --unit=mpc-boot-ssh' "$empty/autorun/autorun.sh"
grep -q 'remount,exec' "$empty/autorun/autorun.sh"
if grep -q 'boot-menu-apply' "$empty/autorun/autorun.sh"; then fail "autorun applies the stock patch"; fi
if grep -q 'stock-os-patched' "$empty/autorun/autorun.sh"; then fail "autorun mounts the stock patch"; fi
test -x "$empty/autorun/boot-menu-apply.sh"
test -x "$empty/autorun/poke"
test -s "$empty/autorun/patches.list"
if grep -q 'conv=notrunc' "$empty/autorun/boot-menu-apply.sh"; then fail "device dd cannot use conv=notrunc"; fi
grep -q 'menu-os' "$empty/autorun/boot-menu-apply.sh"
if grep -q '/tmp/mpc-stock-patched' "$empty/autorun/boot-menu-apply.sh"; then fail "patched app is rebuilt in /tmp"; fi
grep -q 'boot-menu-apply.sh' "$ROOT/src/main.c"
test -x "$empty/autorun/mpc-boot-web"
strings "$empty/autorun/mpc-boot-web" | grep -q 'application/octet-stream' || fail "web server cannot serve built page files"
test -x "$empty/autorun/web.sh"
web_page "$empty/autorun/web"
grep -q 'mpc-boot-web' "$empty/autorun/web.sh"
grep -q '8080' "$empty/autorun/web.sh"
grep -q 'remount,exec' "$empty/autorun/web.sh"
grep -q 'exec "\$bin"' "$empty/autorun/web.sh"
if grep -q 'exec /run/mpc-boot-web ' "$empty/autorun/web.sh"; then
  fail "web.sh executes the log directory"
fi
if grep -q 'autorun-backup' "$empty/autorun/autorun.sh"; then fail "new autorun mentions a backup"; fi
if grep -q 'mpc-sample-usb-ssh' "$empty/autorun/autorun.sh" "$empty/autorun/ssh.sh"; then fail "new autorun starts the ssh kit"; fi
if grep -q 'authorized_keys' "$empty/autorun/ssh.sh"; then fail "ssh still expects a client key"; fi
grep -q 'os_tools/bin' "$empty/autorun/ssh.sh"
grep -q 'export PATH="%s:$PATH"' "$empty/autorun/ssh.sh"
grep -q 'mount --bind "$RAM/profile" /etc/profile' "$empty/autorun/ssh.sh"
grep -q 'tools were not added to PATH' "$empty/autorun/ssh.sh"
grep -q 'dirname -- "$D"' "$empty/autorun/ssh.sh"
grep -q 'ln -sfn "$card/os_tools/.bashrc" /root/.bashrc' "$empty/autorun/ssh.sh"
grep -q 'os_tools/bin/bash' "$empty/autorun/ssh.sh"
grep -q '/etc/passwd' "$empty/autorun/ssh.sh"
grep -q 'ln -sfn "$card/os_tools/.bashrc" /root/.bashrc' "$empty/autorun/web.sh"
if grep -q '/media/MPC' "$empty/autorun/ssh.sh"; then fail "ssh.sh hardcodes the card path"; fi
grep -q 'os_tools/bin/bash' "$ROOT/webserver/server.c"
grep -q '"-l", "-i"' "$ROOT/webserver/server.c"
grep -q 'PermitEmptyPasswords yes' "$empty/autorun/ssh.sh"
grep -q 'PubkeyAuthentication no' "$empty/autorun/ssh.sh"
grep -q 'UsePAM no' "$empty/autorun/ssh.sh"
if grep -Eq '^[[:space:]]*AuthenticationMethods' "$empty/autorun/ssh.sh"; then
  fail "blank password must not be tied to AuthenticationMethods"
fi
test -s "$empty/autorun/ssh_host_ed25519_key"
test -s "$empty/autorun/ssh_host_ed25519_key.pub"
cmp -s "$ROOT/ssh/ssh_host_ed25519_key" "$empty/autorun/ssh_host_ed25519_key" || fail "fresh install did not use the saved server identity"
hostkey=$(shasum -a 256 "$empty/autorun/ssh_host_ed25519_key" | awk '{print $1}')
got=$(digest_of "$empty/autorun")
have=$(cat "$empty/autorun/autorun.check")
test "$got" = "$have" || fail "autorun.check mismatch"

# Re-run on our own install updates without a backup prompt and keeps menu entries.
printf 'title=Snake\nrun=apps/snake\nargs=\n' > "$empty/autorun/boot-menu/30-snake.entry"
printf 'stray\n' > "$empty/os_tools/stray"
printf '\nalias ll="ls -l"\n' >> "$empty/os_tools/.bashrc"
./setup.sh --SD "$empty" >/tmp/mpc-boot-out
grep -q 'Updating it' /tmp/mpc-boot-out || fail "reinstall should update without asking"
test ! -e "$empty/os_tools/stray" || fail "reinstall left a file in os_tools"
grep -q 'alias ll=' "$empty/os_tools/.bashrc" || fail "reinstall replaced .bashrc"
test -x "$empty/os_tools/bin/bash"
test ! -e "$empty/os_tools/bin/vim"
cmp "$fix/toolkit/marker" "$empty/os_tools/toolkit/marker"
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
test ! -d "$ssh/os_tools" 

# Declining the backup on a terminal leaves the kit in place.
pty_setup n "$ssh" >/tmp/mpc-boot-out
grep -q 'Not changed' /tmp/mpc-boot-out || fail "decline should say Not changed"
grep -q 'USB SSH kit' /tmp/mpc-boot-out || fail "decline should name the USB SSH kit"
test "$(digest_of "$ssh/autorun")" = "$before" || fail "decline changed autorun"
test ! -d "$ssh"/autorun-backup-* 2>/dev/null || fail "decline created a backup"
test ! -d "$ssh/os_tools"

# Accepting backs up and replaces. The new script only starts mpc-boot-thingy.
pty_setup y "$ssh" >/tmp/mpc-boot-out
test -d "$ssh"/autorun-backup-*
test -f "$ssh/autorun/mpc-boot-thingy.mark"
test -x "$ssh/os_tools/bin/bash"
test ! -e "$ssh/os_tools/bin/vim"
test ! -e "$ssh/bin"
test ! -e "$ssh/toolkit"
test ! -d "$ssh/apps"
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
test ! -d "$bare/os_tools"

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
test ! -d "$bare/os_tools"

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
test -x "$empty/os_tools/bin/bash"
test ! -e "$empty/os_tools/bin/vim"
test ! -e "$empty/apps/os_tools"

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
    hold=$(mktemp -d)
    if printf '%s\n' "$last" | grep -q '.bashrc'; then
      for n in .bashrc .bash_profile; do
        if [ -f "$FAKE_ROOT$dest/os_tools/$n" ]; then
          cp "$FAKE_ROOT$dest/os_tools/$n" "$hold/$n"
        fi
      done
    fi
    case "$last" in
      *'rm -rf "$dest/os_tools"'*) rm -rf "$FAKE_ROOT$dest/os_tools" ;;
    esac
    tar -C "$FAKE_ROOT$dest" -xf -
    mkdir -p "$FAKE_ROOT$dest/os_tools"
    for n in .bashrc .bash_profile; do
      if [ -f "$hold/$n" ]; then
        cp "$hold/$n" "$FAKE_ROOT$dest/os_tools/$n"
      fi
    done
    rm -rf "$hold"
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
mkdir -p "$card/os_tools"
printf 'stray\n' > "$card/os_tools/stray"
printf 'alias kept=yes\n' > "$card/os_tools/.bashrc"
printf 'leave-me\n' > "$card/notes.txt"

PATH="$bin:$PATH" FAKE_ROOT=$remote ./setup.sh --ssh >/tmp/mpc-boot-out
grep -q 'Updated /media/Lab Card over SSH' /tmp/mpc-boot-out || fail "ssh install did not name the card"
grep -q '/media/Lab Card/os_tools/bin' /tmp/mpc-boot-out || fail "ssh install did not name os_tools"
if grep -q 'Back up the existing' /tmp/mpc-boot-out; then fail "ssh install asked for a backup"; fi
test ! -d "$remote"/autorun-backup-* 
grep -q 'title=Snake' "$card/autorun/boot-menu/30-snake.entry" || fail "ssh install removed a menu entry"
test "$(cat "$card/autorun/ssh_host_ed25519_key")" = keep-key || fail "ssh install replaced the server identity"
test -x "$card/autorun/mpc-boot-thingy" || test -f "$card/autorun/mpc-boot-thingy"
grep -q 'systemd-run --unit=mpc-boot-ssh' "$card/autorun/autorun.sh"
grep -q 'systemd-run --unit=mpc-boot-web' "$card/autorun/autorun.sh"
web_page "$card/autorun/web"
test ! -e "$card/os_tools/stray" || fail "ssh install left a file in os_tools"
grep -q 'alias kept=yes' "$card/os_tools/.bashrc" || fail "ssh install replaced .bashrc"
test -x "$card/os_tools/bin/bash"
test ! -e "$card/os_tools/bin/vim"
cmp "$fix/toolkit/marker" "$card/os_tools/toolkit/marker"
PATH="$bin:$PATH" FAKE_ROOT=$remote ./setup.sh --ssh --tools >/tmp/mpc-boot-out
grep -q 'Dev tools are in /media/Lab Card/os_tools/bin' /tmp/mpc-boot-out || fail "ssh --tools did not name the dev tools"
test -x "$card/os_tools/bin/vim"
test -x "$card/os_tools/bin/busybox-full"
test -x "$card/os_tools/bin/bash"
cmp "$devfix/toolkit/marker" "$card/os_tools/toolkit/marker"
grep -q 'alias kept=yes' "$card/os_tools/.bashrc" || fail "ssh --tools replaced .bashrc"
test ! -e "$card/bin"
test ! -e "$card/toolkit"
test ! -d "$card/apps"
test "$(cat "$card/notes.txt")" = leave-me || fail "ssh install removed a file outside os_tools"
test "$(digest_of "$card/autorun")" = "$(cat "$card/autorun/autorun.check")" || fail "ssh install check mismatch"

PATH="$bin:$PATH" FAKE_ROOT=$remote ./setup.sh --ssh --add "$proj" >/tmp/mpc-boot-out
grep -q 'Installed Lab firmware' /tmp/mpc-boot-out || fail "ssh add did not install"
grep -q 'Updated the app on /media/Lab Card over SSH' /tmp/mpc-boot-out || fail "ssh add did not name the card"
cmp "$proj/mpcfw" "$card/apps/lab-firmware/mpcfw" || fail "ssh add did not copy the program"
cmp "$proj/boot.entry" "$card/apps/lab-firmware/boot.entry" || fail "ssh add did not copy boot.entry"
test ! -e "$card/apps/lab-firmware/extra.c" || fail "ssh add copied a source file"
test "$(cat "$card/notes.txt")" = leave-me || fail "ssh add removed a file outside the app"
grep -q 'title=Snake' "$card/autorun/boot-menu/30-snake.entry" || fail "ssh add removed a menu entry"

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

# macOS cp exits 1 when exFAT refuses extended attributes. Setup must
# copy without them or the card update stops on the first binary.
if [ "$(uname -s)" = Darwin ]; then
  cpx=$work/cpx
  mkdir "$cpx"
  cat > "$cpx/cp" <<'EOF'
#!/usr/bin/env python3
import os, sys
args = [a for a in sys.argv[1:] if a != "-X"]
if len(args) == len(sys.argv[1:]):
    sys.stderr.write("cp: could not copy extended attributes: Attribute not found\n")
    sys.exit(1)
os.execv("/bin/cp", ["/bin/cp", *args])
EOF
  chmod 755 "$cpx/cp"
  xcard=$work/xattr
  mkdir "$xcard"
  PATH="$cpx:$PATH" ./setup.sh --SD "$xcard" >/tmp/mpc-boot-out
  test -f "$xcard/autorun/mpc-boot-web" || fail "attribute failure stopped the copy"
  test -x "$xcard/os_tools/bin/bash" || fail "attribute failure stopped bash"
  test ! -e "$xcard/os_tools/bin/vim" || fail "attribute copy included vim"
fi

# --tools installs vim and busybox-full. Updating without the flag
# returns the card to bash and keeps .bashrc.
devcard=$work/dev
mkdir "$devcard"
./setup.sh --SD "$devcard" --tools >/tmp/mpc-boot-out
grep -q 'Dev tools are in' /tmp/mpc-boot-out || fail "tools install did not name the dev tools"
test -x "$devcard/os_tools/bin/bash"
test -x "$devcard/os_tools/bin/vim"
test -x "$devcard/os_tools/bin/busybox-full"
cmp "$devfix/bin/vim" "$devcard/os_tools/bin/vim"
cmp "$devfix/toolkit/marker" "$devcard/os_tools/toolkit/marker"
printf '\nalias ll="ls -l"\n' >> "$devcard/os_tools/.bashrc"
./setup.sh --SD "$devcard" >/tmp/mpc-boot-out
test ! -e "$devcard/os_tools/bin/vim" || fail "update without --tools left vim"
test ! -e "$devcard/os_tools/bin/busybox-full" || fail "update without --tools left busybox-full"
test -x "$devcard/os_tools/bin/bash"
cmp "$fix/bin/bash" "$devcard/os_tools/bin/bash"
cmp "$fix/toolkit/marker" "$devcard/os_tools/toolkit/marker"
grep -q 'alias ll=' "$devcard/os_tools/.bashrc" || fail "update without --tools replaced .bashrc"

echo "setup fixtures ok"
