#!/bin/sh
# Install mpc-boot-thingy onto a mounted SD card.
set -eu

ROOT=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
SALT='f7bed1c9-7e90-4731-9194-c996666b4331'

# exFAT has nowhere to put macOS extended attributes. cp still exits 1
# after writing the file, which would stop setup on the first binary.
card_cp() {
  if [ "$(uname -s)" = Darwin ]; then
    cp -X "$@"
  else
    cp "$@"
  fi
}
SSH_CHECK='e5a0adf36e44a7872bf991dc5e110daf702445a15735aefa067411cfb979d128'

usage() {
  cat <<'EOF'
Usage:
  ./setup.sh -h
  ./setup.sh --help
  ./setup.sh --SD "/path/to/mounted/SD"
  ./setup.sh --SD "/path/to/mounted/SD" --tools
  ./setup.sh --ssh
  ./setup.sh --ssh --tools
  ./setup.sh --reboot
  ./setup.sh --ssh -r

-h, --help
  Show this help and exit.

--SD PATH
  Mount point of the card on this computer, not a device node.
  The path must be an existing writable directory, and it cannot be /.

If the card already has an autorun folder, setup names what it found
(mpc-boot-thingy, the USB SSH kit, or something else) and asks before
replacing it. Answering y copies that folder to
autorun-backup-YYYYMMDD-HHMMSS on the card, then installs. The backup
is only a copy. Boot does not read it or run it. Any other answer
leaves the card unchanged. Without a terminal, an existing autorun is
not replaced.

A card with no autorun folder is installed with no question.
A card that already has mpc-boot-thingy is updated with no question
and no new backup. Menu files in autorun/boot-menu/ are left in place.

The backup question appears only when the card has a different autorun,
such as the USB SSH kit. Answering y copies that folder aside, then
installs. Any other answer leaves the card unchanged.

Add a program by dragging a folder into apps/ on the card. The folder
name sets the menu order (10-game comes before 20-tool). Stock stays
first. No rebuild and no new check. The folder looks like this:

  apps/lab-firmware/boot.entry
  apps/lab-firmware/mpcfw

  title=Lab firmware
  run=mpcfw

title is the label on screen. run is the program in that same folder,
unless it starts with /. Optional args= is split on spaces. The program
starts in its folder. Lines starting with # and blank lines are ignored.
A boot.entry missing title or run is skipped.

A program that has no boot.entry can be placed with one command. This
copies the executable into apps/ and writes boot.entry. It does not
change autorun/:

  ./setup.sh --SD "/path/to/mounted/SD" --add /path/to/program --title "Lab firmware"
  ./setup.sh --ssh --add /path/to/program

--title is optional. Without it, the file name is the menu label.
With --ssh, the card is the one directory under /media on the MPC.
If --add is a project folder that already contains boot.entry, only
that file and the program it names are copied. Other files in the
project stay where they are.

Older autorun/boot-menu/*.entry files still load. run in those files
is relative to the card root.

After the splash, the screen says "Hold SHIFT for boot thingy" and the
SHIFT button lights. Press SHIFT while that text is up to open the menu.
The encoder moves the line. The encoder press confirms it.
Waiting without pressing SHIFT starts the stock app.

SSH starts with the card, on the USB link only. There is no client key.
From the computer:

  ssh -p 2222 -o PreferredAuthentications=password -o PubkeyAuthentication=no root@192.168.155.1

The password is empty: press Enter. The first connection asks you to
trust this MPC. Plug in the USB cable before you turn the MPC on.

SSH and the web terminal both start bash from os_tools/bin. Aliases and
exports belong in os_tools/.bashrc on the card. Setup keeps that file
when it updates the shell.

--tools
  Also install vim, nano, busybox-full, rsync, curl, htop, and the
  other shell utilities into os_tools/bin. Pass this when setting up
  a dev card. The GitHub release runs setup without this flag, so the
  published card has bash and omits these utilities. They are not
  boot-menu apps.

  ./setup.sh --SD "/path/to/mounted/SD" --tools
  ./setup.sh --ssh --tools

The web page is on that same USB link:

  http://192.168.155.1:8080

From the page you can install a boot program, choose the program that
starts when SHIFT is not held, reboot the MPC, and open a shell.

--ssh
  Update the card already mounted on the MPC. This is for a machine
  that already runs this boot setup. There is no backup question.
  The card is the one directory under /media, whatever its name is.
  Menu entries and an existing server identity are left in place.
  With --add, only that program is copied. The boot menu is not replaced.

-r, --reboot
  Reboot the MPC over SSH. Alone, it only reboots. With --ssh, it
  installs and then reboots. It is not used with --SD or --add.

  ./setup.sh --reboot
  ./setup.sh --ssh -r
EOF
}

kind_of() {
  A=$1/autorun
  if [ ! -d "$A" ]; then
    printf '%s' empty
    return
  fi
  if [ -f "$A/mpc-boot-thingy.mark" ]; then
    printf '%s' ours
    return
  fi
  if [ -f "$A/autorun.sh" ] && grep -q 'mpc-sample-usb-ssh' "$A/autorun.sh"; then
    printf '%s' ssh
    return
  fi
  if [ -f "$A/autorun.check" ] && grep -qx "$SSH_CHECK" "$A/autorun.check"; then
    printf '%s' ssh
    return
  fi
  printf '%s' other
}

warn_kind() {
  sd=$1
  kind=$2
  case "$kind" in
    ours)
      echo "This card already has mpc-boot-thingy."
      echo "Found marker: $sd/autorun/mpc-boot-thingy.mark"
      ;;
    ssh)
      echo "This card has the USB SSH kit (mpc-sample-root-ssh)."
      echo "Found: $sd/autorun"
      echo "Replacing it will stop that kit from running at boot."
      ;;
    other)
      echo "This card has an autorun setup that is not mpc-boot-thingy."
      echo "Found: $sd/autorun"
      ;;
  esac
  echo "Continuing replaces what this card runs at boot."
}

confirm_backup() {
  if [ ! -t 0 ]; then
    echo "Refusing to replace an existing autorun setup without a terminal to confirm the backup." >&2
    exit 1
  fi
  printf 'Back up the existing autorun folder before replacing it? [y/N] '
  read -r ans || ans=
  case "$ans" in
    y|Y) ;;
    *)
      echo "Not changed."
      exit 0
      ;;
  esac
}

backup_autorun() {
  sd=$1
  ts=$(date +%Y%m%d-%H%M%S)
  dest=$sd/autorun-backup-$ts
  n=2
  while [ -e "$dest" ]; do
    dest=$sd/autorun-backup-$ts-$n
    n=$((n + 1))
  done
  card_cp -a "$sd/autorun" "$dest"
  echo "Backed up to $dest"
}

one_line() {
  case "$1" in
    *'
'*) return 1 ;;
  esac
  return 0
}

slug_of() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '-' | sed 's/^-//;s/-$//'
}

entry_field() {
  key=$1
  file=$2
  awk -v key="$key" '
    {
      line = $0
      sub(/\r$/, "", line)
      sub(/^[[:space:]]*/, "", line)
      if (line == "" || substr(line, 1, 1) == "#") next
      eq = index(line, "=")
      if (eq == 0) next
      k = substr(line, 1, eq - 1)
      v = substr(line, eq + 1)
      sub(/[[:space:]]*$/, "", k)
      sub(/^[[:space:]]*/, "", v)
      sub(/[[:space:]]*$/, "", v)
      if (k == key) { print v; exit }
    }
  ' "$file"
}

install_app() {
  sd=$1
  src=$2
  title=$3
  if [ -d "$src" ]; then
    if [ ! -f "$src/boot.entry" ]; then
      echo "ERROR: $src has no boot.entry. Pass the executable: --add /path/to/program --title \"Name\"" >&2
      exit 1
    fi
    run=$(entry_field run "$src/boot.entry")
    label=$(entry_field title "$src/boot.entry")
    if [ -z "$run" ] || [ -z "$label" ]; then
      echo "ERROR: $src/boot.entry needs title and run" >&2
      exit 1
    fi
    case "$run" in
      ./*) run=${run#./} ;;
    esac
    case "$run" in
      /*) ;;
      *..*)
        echo "ERROR: run must stay inside the app folder: $run" >&2
        exit 1
        ;;
      *)
        if [ ! -f "$src/$run" ]; then
          echo "ERROR: $src/boot.entry names $run, and that file is not in the folder" >&2
          exit 1
        fi
        ;;
    esac
    if [ -n "$title" ]; then label=$title; fi
    name=$(slug_of "$label")
    if [ -z "$name" ]; then name=$(slug_of "$(basename "$src")"); fi
    if [ -z "$name" ]; then
      echo "ERROR: could not name the app folder" >&2
      exit 1
    fi
    dest=$sd/apps/$name
    mkdir -p "$dest"
    if [ -n "$title" ]; then
      printf 'title=%s\nrun=%s\n' "$label" "$run" > "$dest/boot.entry"
      extra=$(entry_field args "$src/boot.entry" || true)
      if [ -n "$extra" ]; then printf 'args=%s\n' "$extra" >> "$dest/boot.entry"; fi
    else
      card_cp "$src/boot.entry" "$dest/boot.entry"
    fi
    case "$run" in
      /*) ;;
      *)
        mkdir -p "$dest/$(dirname -- "$run")"
        card_cp "$src/$run" "$dest/$run"
        chmod 755 "$dest/$run"
        ;;
    esac
    echo "Installed $label in $dest"
    return
  fi
  if [ ! -f "$src" ]; then
    echo "ERROR: not a file or folder: $src" >&2
    exit 1
  fi
  if [ -z "$title" ]; then title=$(basename "$src"); fi
  if ! one_line "$title"; then
    echo "ERROR: title must be one line" >&2
    exit 1
  fi
  name=$(slug_of "$title")
  if [ -z "$name" ]; then
    echo "ERROR: could not name the app folder" >&2
    exit 1
  fi
  bin=$(basename "$src")
  case "$bin" in
    */*|*..*|boot.entry)
      echo "ERROR: refusing executable name: $bin" >&2
      exit 1
      ;;
  esac
  dest=$sd/apps/$name
  mkdir -p "$dest"
  card_cp "$src" "$dest/$bin"
  chmod 755 "$dest/$bin"
  printf 'title=%s\nrun=%s\n' "$title" "$bin" > "$dest/boot.entry"
  echo "Installed $title in $dest"
}

ensure_toolkit() {
  if [ -n "$TOOLS" ]; then
    if [ -n "${MPC_TOOLKIT_TOOLS_SRC:-}" ]; then
      return
    fi
    dest=$ROOT/.mpc-cache/os_tools-dev
    if [ -x "$dest/bin/vim" ] && [ -x "$dest/bin/nano" ] && [ -x "$dest/bin/busybox-full" ] &&
       [ -x "$dest/bin/bash" ] && [ -f "$dest/toolkit/lib/ld-musl-aarch64.so.1" ] &&
       grep -q 'set -- -l -i' "$dest/bin/bash"; then
      return
    fi
    fetch_args=--tools
    fetch_msg="Fetching dev tools into $dest"
  else
    if [ -n "${MPC_TOOLKIT_SRC:-}" ]; then
      return
    fi
    dest=$ROOT/.mpc-cache/os_tools
    if [ -x "$dest/bin/bash" ] && [ -f "$dest/toolkit/lib/ld-musl-aarch64.so.1" ] &&
       grep -q 'set -- -l -i' "$dest/bin/bash" &&
       [ ! -e "$dest/bin/vim" ] && [ ! -e "$dest/bin/nano" ] &&
       [ ! -e "$dest/bin/busybox-full" ]; then
      return
    fi
    fetch_args=
    fetch_msg="Fetching bash into $dest"
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required to fetch the shell tools" >&2
    exit 1
  fi
  echo "$fetch_msg"
  # fetch_args is empty unless --tools was passed.
  python3 "$ROOT/fetch-toolkit.py" $fetch_args "$dest"
}

toolkit_src() {
  if [ -n "$TOOLS" ]; then
    if [ -n "${MPC_TOOLKIT_TOOLS_SRC:-}" ]; then
      printf '%s\n' "$MPC_TOOLKIT_TOOLS_SRC"
      return
    fi
    printf '%s\n' "$ROOT/.mpc-cache/os_tools-dev"
    return
  fi
  if [ -n "${MPC_TOOLKIT_SRC:-}" ]; then
    printf '%s\n' "$MPC_TOOLKIT_SRC"
    return
  fi
  printf '%s\n' "$ROOT/.mpc-cache/os_tools"
}

copy_toolkit() {
  sd=$1
  if [ -z "$sd" ] || [ "$sd" = / ]; then
    echo "ERROR: refusing to write tools onto $sd" >&2
    exit 1
  fi
  src=$(toolkit_src)
  if [ ! -d "$src/bin" ] || [ ! -d "$src/toolkit" ]; then
    echo "ERROR: toolkit is missing: $src" >&2
    exit 1
  fi
  hold=$(mktemp -d)
  for name in .bashrc .bash_profile; do
    if [ -f "$sd/os_tools/$name" ]; then
      card_cp "$sd/os_tools/$name" "$hold/$name"
    fi
  done
  rm -rf "$sd/os_tools"
  mkdir -p "$sd/os_tools"
  card_cp -R "$src/bin" "$src/toolkit" "$sd/os_tools/"
  for name in .bashrc .bash_profile; do
    if [ -f "$hold/$name" ]; then
      card_cp "$hold/$name" "$sd/os_tools/$name"
    fi
  done
  rm -rf "$hold"
  write_shell_rc "$sd/os_tools"
}

write_shell_rc() {
  dest=$1
  if [ ! -f "$dest/.bashrc" ]; then
    cat > "$dest/.bashrc" <<'RC'
# Shell settings for this card. Add aliases and exports below.
# Setup keeps this file when it updates os_tools.

_src=${BASH_SOURCE[0]}
if command -v readlink >/dev/null 2>&1; then
  _real=$(readlink -f "$_src" 2>/dev/null || true)
  if [ -n "$_real" ]; then
    _src=$_real
  fi
fi
_os=$(CDPATH= cd -- "$(dirname -- "$_src")" && pwd)
case ":${PATH}:" in
  *":${_os}/bin:"*) ;;
  *) PATH="${_os}/bin:${PATH}" ;;
esac
export PATH
unset _src _real _os
RC
  fi
  if [ ! -f "$dest/.bash_profile" ]; then
    cat > "$dest/.bash_profile" <<'RC'
# Login shells read this file. Aliases and exports belong in .bashrc.
if [ -f "$HOME/.bashrc" ]; then
  . "$HOME/.bashrc"
fi
RC
  fi
}

write_autorun() {
  sd=$1
  A=$sd/autorun
  mkdir -p "$A/boot-menu"
  cat > "$A/autorun.json" <<'EOF'
{
  "value0": {
    "scripts": [
      {
        "path": "autorun.sh",
        "askQuestion": false,
        "message": "Start mpc-boot-thingy?",
        "yesButtonText": "Start",
        "noButtonText": "Cancel"
      }
    ],
    "files": []
  }
}
EOF
  cat > "$A/autorun.sh" <<'EOF'
#!/bin/sh
# mpc-boot-thingy. Starts the boot menu, the web page, and a keyless USB SSH shell.
# edisksd mounts the card noexec on boot and again when the card is inserted.
# This script runs for both, so unlock execution before starting anything.
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
card=$(CDPATH= cd -- "$(dirname -- "$D")" && pwd)
if mount -o remount,exec "$card"; then
  echo "remount exec $card"
else
  echo "WARN: could not remount exec $card"
fi
cp "$D/mpc-boot-thingy" /run/mpc-boot-thingy
chmod 755 /run/mpc-boot-thingy
systemd-run --unit=mpc-boot-thingy --collect --no-block /run/mpc-boot-thingy "$D"
if ! systemctl is-active --quiet mpc-boot-web.service; then
  systemd-run --unit=mpc-boot-web --collect --no-block /bin/sh "$D/web.sh"
fi
if systemctl is-active --quiet mpc-boot-ssh.service; then
  exit 0
fi
systemd-run --unit=mpc-boot-ssh --collect --no-block /bin/sh "$D/ssh.sh"
EOF
  chmod 755 "$A/autorun.sh"
  cat > "$A/ssh.sh" <<'EOF'
#!/bin/sh
# Root shell on the USB link. No client key. The password is empty.
set -eu
umask 077
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAM=/run/mpc-boot-ssh
mkdir -p "$RAM" /run/sshd
chmod 700 "$RAM"
chmod 755 /run/sshd
: > "$RAM/ssh.log"
exec >> "$RAM/ssh.log" 2>&1
log_to_card() { cp "$RAM/ssh.log" "$D/ssh.log" 2>/dev/null || true; }
trap log_to_card EXIT
echo 'MPC_BOOT_SSH_START'
date -u || true
if [ "$(id -u)" != 0 ]; then
  echo 'ERROR: not root'
  exit 1
fi
if [ ! -x /usr/sbin/sshd ]; then
  echo 'ERROR: /usr/sbin/sshd missing'
  exit 1
fi
if [ ! -s "$D/ssh_host_ed25519_key" ]; then
  echo 'ERROR: host key missing'
  exit 1
fi
cp "$D/ssh_host_ed25519_key" "$RAM/ssh_host_ed25519_key"
chmod 600 "$RAM/ssh_host_ed25519_key"
if [ -r /etc/shadow ]; then
  sed 's/^root:[^:]*:/root::/' /etc/shadow > "$RAM/shadow"
  chmod 600 "$RAM/shadow"
  if ! mount --bind "$RAM/shadow" /etc/shadow; then
    echo 'ERROR: could not set an empty root password for this boot'
    exit 1
  fi
fi
cat > "$RAM/sshd_config" <<'SSHCONF'
Port 2222
ListenAddress 192.168.155.1
HostKey /run/mpc-boot-ssh/ssh_host_ed25519_key
PidFile /run/mpc-boot-ssh/sshd.pid
PermitRootLogin yes
PubkeyAuthentication no
PasswordAuthentication yes
KbdInteractiveAuthentication no
PermitEmptyPasswords yes
UsePAM no
# Do not set AuthenticationMethods. A blank password is recorded as
# method "none", and requiring "password" then closes the connection.
AllowUsers root
StrictModes yes
UseDNS no
PermitTTY yes
PrintMotd no
PrintLastLog no
AllowTcpForwarding no
X11Forwarding no
SSHCONF
usb_wait=0
while ! grep -q '192.168.155.1' /proc/net/fib_trie 2>/dev/null; do
  if [ "$usb_wait" -ge 45 ]; then
    echo 'ERROR: USB IP absent after 45 seconds'
    exit 1
  fi
  sleep 1
  usb_wait=$((usb_wait + 1))
done
echo "USB_IP_READY waited_seconds=$usb_wait"
card=$(CDPATH= cd -- "$(dirname -- "$D")" && pwd)
mkdir -p /root
if [ -f "$card/os_tools/.bashrc" ]; then
  ln -sfn "$card/os_tools/.bashrc" /root/.bashrc || echo 'WARN: could not link .bashrc'
else
  echo 'WARN: os_tools/.bashrc is missing'
fi
if [ -f "$card/os_tools/.bash_profile" ]; then
  ln -sfn "$card/os_tools/.bash_profile" /root/.bash_profile || echo 'WARN: could not link .bash_profile'
fi
if [ -x "$card/os_tools/bin/bash" ] && [ -r /etc/passwd ]; then
  awk -F: -v shell="$card/os_tools/bin/bash" 'BEGIN{OFS=":"} $1=="root" {$7=shell} {print}' /etc/passwd > "$RAM/passwd"
  chmod 644 "$RAM/passwd"
  if mount --bind "$RAM/passwd" /etc/passwd; then
    echo "SHELL_BASH $card/os_tools/bin/bash"
  else
    echo 'WARN: could not make bash the login shell'
  fi
else
  echo 'WARN: bash is missing; login shell was not changed'
fi
if [ ! -d "$card/os_tools/bin" ]; then
  echo 'WARN: os_tools/bin is missing; tools were not added to PATH'
elif [ ! -r /etc/profile ]; then
  echo 'WARN: /etc/profile is not readable; tools were not added to PATH'
elif ! cp /etc/profile "$RAM/profile"; then
  echo 'WARN: could not add os_tools to PATH'
else
  printf '\nexport PATH="%s:$PATH"\n' "$card/os_tools/bin" >> "$RAM/profile"
  if mount --bind "$RAM/profile" /etc/profile; then
    echo "PATH_TOOLS $card/os_tools/bin"
  else
    echo 'WARN: could not add os_tools to PATH'
  fi
fi
/usr/sbin/sshd -t -f "$RAM/sshd_config"
log_to_card
exec /usr/sbin/sshd -D -e -f "$RAM/sshd_config"
EOF
  chmod 755 "$A/ssh.sh"
  cat > "$A/web.sh" <<'EOF'
#!/bin/sh
# Web page on the USB link.
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAM=/run/mpc-boot-web
mkdir -p "$RAM"
: > "$RAM/web.log"
exec >> "$RAM/web.log" 2>&1
echo 'MPC_BOOT_WEB_START'
remount_card() {
  card=$(CDPATH= cd -- "$(dirname -- "$D")" && pwd)
  if mount -o remount,exec "$card"; then
    echo "remount exec $card"
  else
    echo "WARN: could not remount exec $card"
  fi
}
remount_card
bin=$D/mpc-boot-web
if ! "$bin" --check >/dev/null 2>&1; then
  echo 'card binary did not run, copying into /run'
  cp "$D/mpc-boot-web" "$RAM/mpc-boot-web"
  chmod 755 "$RAM/mpc-boot-web"
  bin=$RAM/mpc-boot-web
fi
usb_wait=0
while ! grep -q '192.168.155.1' /proc/net/fib_trie 2>/dev/null; do
  if [ "$usb_wait" -ge 45 ]; then
    echo 'ERROR: USB IP absent after 45 seconds'
    exit 1
  fi
  sleep 1
  usb_wait=$((usb_wait + 1))
done
echo "USB_IP_READY waited_seconds=$usb_wait"
remount_card
card=$(CDPATH= cd -- "$(dirname -- "$D")" && pwd)
mkdir -p /root
if [ -f "$card/os_tools/.bashrc" ]; then
  ln -sfn "$card/os_tools/.bashrc" /root/.bashrc || echo 'WARN: could not link .bashrc'
fi
if [ -f "$card/os_tools/.bash_profile" ]; then
  ln -sfn "$card/os_tools/.bash_profile" /root/.bash_profile || echo 'WARN: could not link .bash_profile'
fi
exec "$bin" "$D" 8080 192.168.155.1
EOF
  chmod 755 "$A/web.sh"
  if [ ! -s "$A/ssh_host_ed25519_key" ]; then
    if ! command -v ssh-keygen >/dev/null 2>&1; then
      echo "ERROR: ssh-keygen is required to create the server identity" >&2
      exit 1
    fi
    ssh-keygen -q -t ed25519 -N '' -f "$A/ssh_host_ed25519_key" -C 'mpc-boot-thingy'
    chmod 600 "$A/ssh_host_ed25519_key" 2>/dev/null || true
  fi
  if command -v shasum >/dev/null 2>&1; then
    digest=$({ cat "$A/autorun.json" "$A/autorun.sh"; printf '%s' "$SALT"; } | shasum -a 256 | awk '{print $1}')
  elif command -v sha256sum >/dev/null 2>&1; then
    digest=$({ cat "$A/autorun.json" "$A/autorun.sh"; printf '%s' "$SALT"; } | sha256sum | awk '{print $1}')
  else
    echo "ERROR: shasum or sha256sum is required" >&2
    exit 1
  fi
  printf '%s' "$digest" > "$A/autorun.check"
  if [ ! -x "$ROOT/build/mpc-boot-web" ]; then
    echo "ERROR: build did not produce $ROOT/build/mpc-boot-web" >&2
    exit 1
  fi
  card_cp "$ROOT/build/mpc-boot-thingy" "$A/mpc-boot-thingy"
  chmod 755 "$A/mpc-boot-thingy"
  card_cp "$ROOT/build/mpc-boot-web" "$A/mpc-boot-web"
  chmod 755 "$A/mpc-boot-web"
  mkdir -p "$A/web"
  card_cp "$ROOT/webserver/index.html" "$ROOT/webserver/xterm.js" \
    "$ROOT/webserver/xterm.css" "$ROOT/webserver/xterm-addon-fit.js" \
    "$ROOT/webserver/xterm.LICENSE" "$A/web/"
  printf '%s\n' mpc-boot-thingy > "$A/mpc-boot-thingy.mark"
  ensure_toolkit
  copy_toolkit "$sd"
  if [ "${2:-}" = quiet ]; then
    return
  fi
  echo "Installed mpc-boot-thingy in $A"
  echo "SSH, no key file. Password is empty (press Enter):"
  echo "  ssh -p 2222 -o PreferredAuthentications=password -o PubkeyAuthentication=no root@192.168.155.1"
  echo "Bash is in $sd/os_tools/bin"
  if [ -n "$TOOLS" ]; then
    echo "Dev tools are in $sd/os_tools/bin"
  fi
  echo "Web page:"
  echo "  http://192.168.155.1:8080"
  if command -v ssh-keygen >/dev/null 2>&1 && [ -s "$A/ssh_host_ed25519_key.pub" ]; then
    echo "This MPC's fingerprint:"
    ssh-keygen -lf "$A/ssh_host_ed25519_key.pub"
  fi
}

MPC_HOST=${MPC_HOST:-root@192.168.155.1}
MPC_PORT=${MPC_PORT:-2222}

ssh_mpc() {
  ask=$1
  shift
  SSH_ASKPASS=$ask SSH_ASKPASS_REQUIRE=force \
    ssh -p "$MPC_PORT" \
    -o PreferredAuthentications=password \
    -o PubkeyAuthentication=no \
    -o StrictHostKeyChecking=accept-new \
    -o NumberOfPasswordPrompts=1 \
    -o ConnectTimeout=8 \
    "$MPC_HOST" "$@"
}

ssh_ask_begin() {
  ask=$(mktemp)
  printf '#!/bin/sh\nexit 0\n' > "$ask"
  chmod 755 "$ask"
}

mpc_media_path() {
  media=$(ssh_mpc "$ask" 'find /media -mindepth 1 -maxdepth 1 -type d') || {
    echo "ERROR: could not reach the MPC over SSH" >&2
    exit 1
  }
  if [ -z "$media" ]; then
    echo "ERROR: no SD card is mounted under /media" >&2
    exit 1
  fi
  n=$(printf '%s\n' "$media" | wc -l | tr -d ' ')
  if [ "$n" != 1 ]; then
    echo "ERROR: expected one directory in /media, found $n" >&2
    printf '%s\n' "$media" >&2
    exit 1
  fi
  printf '%s\n' "$media"
}

add_over_ssh() {
  ssh_ask_begin
  stage=$(mktemp -d)
  trap 'rm -f "$ask"; rm -rf "$stage"' EXIT
  media=$(mpc_media_path) || exit 1
  install_app "$stage" "$ADD" "$TITLE"
  {
    printf '%s\n' "$media"
    tar -C "$stage" --format ustar -cf - apps
  } | ssh_mpc "$ask" 'IFS= read -r dest; tar -C "$dest" -xf -'
  echo "Updated the app on $media over SSH."
}

install_over_ssh() {
  if [ ! -x "$ROOT/build/mpc-boot-thingy" ] || [ ! -x "$ROOT/build/mpc-boot-web" ]; then
    make -C "$ROOT"
  fi
  if [ ! -x "$ROOT/build/mpc-boot-thingy" ] || [ ! -x "$ROOT/build/mpc-boot-web" ]; then
    echo "ERROR: build did not produce the boot and web binaries" >&2
    exit 1
  fi
  ssh_ask_begin
  trap 'rm -f "$ask"' EXIT
  media=$(mpc_media_path) || exit 1
  stage=$(mktemp -d)
  trap 'rm -f "$ask"; rm -rf "$stage"' EXIT
  keep_key=0
  if printf '%s\n' "$media" | ssh_mpc "$ask" 'IFS= read -r dest; test -s "$dest/autorun/ssh_host_ed25519_key"'; then
    keep_key=1
  fi
  write_autorun "$stage" quiet
  if [ "$keep_key" = 1 ]; then
    rm -f "$stage/autorun/ssh_host_ed25519_key" "$stage/autorun/ssh_host_ed25519_key.pub"
  fi
  {
    printf '%s\n' "$media"
    tar -C "$stage" --format ustar -cf - autorun os_tools
  } | ssh_mpc "$ask" 'IFS= read -r dest; mkdir -p "$dest"; hold=$(mktemp -d); for n in .bashrc .bash_profile; do if [ -f "$dest/os_tools/$n" ]; then cp "$dest/os_tools/$n" "$hold/$n"; fi; done; rm -rf "$dest/os_tools"; tar -C "$dest" -xf -; mkdir -p "$dest/os_tools"; for n in .bashrc .bash_profile; do if [ -f "$hold/$n" ]; then cp "$hold/$n" "$dest/os_tools/$n"; fi; done; rm -rf "$hold"'
  echo "Updated $media over SSH. Menu entries are kept."
  echo "Bash is in $media/os_tools/bin"
  if [ -n "$TOOLS" ]; then
    echo "Dev tools are in $media/os_tools/bin"
  fi
  if [ "$keep_key" = 0 ] && command -v ssh-keygen >/dev/null 2>&1 && [ -s "$stage/autorun/ssh_host_ed25519_key.pub" ]; then
    echo "This MPC's fingerprint:"
    ssh-keygen -lf "$stage/autorun/ssh_host_ed25519_key.pub"
  fi
}

reboot_mpc() {
  ask=$(mktemp)
  printf '#!/bin/sh\nexit 0\n' > "$ask"
  chmod 755 "$ask"
  # The bootloader counts boots and opens the updater when the count stays
  # above zero. Userspace clears it only after 10 minutes, so zero it here.
  if ! ssh_mpc "$ask" 'echo 0 > /sys/devices/platform/fdc20000.syscon/fdc20000.syscon:bootcount/bootcount; nohup sh -c "sleep 1; reboot" >/dev/null 2>&1 &'; then
    rm -f "$ask"
    echo "ERROR: could not reach the MPC over SSH" >&2
    exit 1
  fi
  rm -f "$ask"
  echo "Rebooting the MPC."
}

SD=
ADD=
TITLE=
SSH_MODE=
REBOOT=
TOOLS=
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --SD)
      shift
      if [ $# -lt 1 ] || [ -z "$1" ]; then
        usage >&2
        exit 1
      fi
      SD=$1
      ;;
    --add)
      shift
      if [ $# -lt 1 ] || [ -z "$1" ]; then
        usage >&2
        exit 1
      fi
      ADD=$1
      ;;
    --title)
      shift
      if [ $# -lt 1 ] || [ -z "$1" ]; then
        usage >&2
        exit 1
      fi
      TITLE=$1
      ;;
    --ssh)
      SSH_MODE=1
      ;;
    -r|--reboot)
      REBOOT=1
      ;;
    --tools)
      TOOLS=1
      ;;
    *)
      usage >&2
      exit 1
      ;;
  esac
  shift
done

if [ -n "$TOOLS" ] && [ -n "$ADD" ]; then
  echo "ERROR: --tools is not used with --add" >&2
  exit 1
fi
if [ -n "$TOOLS" ] && [ -z "$SD" ] && [ -z "$SSH_MODE" ]; then
  echo "ERROR: --tools needs --SD or --ssh" >&2
  exit 1
fi

if [ -n "$REBOOT" ] && { [ -n "$SD" ] || [ -n "$ADD" ] || [ -n "$TITLE" ]; }; then
  echo "ERROR: --reboot only works alone or with --ssh" >&2
  exit 1
fi

if [ -n "$REBOOT" ] && [ -z "$SSH_MODE" ]; then
  reboot_mpc
  exit 0
fi

if [ -n "$SSH_MODE" ]; then
  if [ -n "$SD" ]; then
    echo "ERROR: --ssh does not take --SD" >&2
    exit 1
  fi
  if [ -n "$TITLE" ] && [ -z "$ADD" ]; then
    echo "ERROR: --title needs --add" >&2
    exit 1
  fi
  if [ -n "$ADD" ]; then
    add_over_ssh
    exit 0
  fi
  install_over_ssh
  if [ -n "$REBOOT" ]; then
    reboot_mpc
  fi
  exit 0
fi

if [ -n "$ADD" ] && [ -z "$SD" ]; then
  echo "ERROR: --add needs --SD" >&2
  exit 1
fi

if [ -z "$SD" ]; then
  usage
  exit 0
fi

if [ ! -d "$SD" ]; then
  echo "ERROR: not a directory: $SD" >&2
  exit 1
fi
SD=$(CDPATH= cd -P -- "$SD" && pwd)
if [ "$SD" = / ]; then
  echo "ERROR: refusing to use /" >&2
  exit 1
fi
if [ ! -w "$SD" ]; then
  echo "ERROR: not writable: $SD" >&2
  exit 1
fi

if [ -n "$ADD" ]; then
  if ! one_line "$TITLE"; then
    echo "ERROR: title must be one line" >&2
    exit 1
  fi
  install_app "$SD" "$ADD" "$TITLE"
  if [ ! -f "$SD/autorun/mpc-boot-thingy.mark" ]; then
    echo "The boot menu is not on this card yet. Run ./setup.sh --SD \"$SD\" to install it."
  fi
  exit 0
fi

if [ ! -x "$ROOT/build/mpc-boot-thingy" ] || [ ! -x "$ROOT/build/mpc-boot-web" ]; then
  make -C "$ROOT"
fi
if [ ! -x "$ROOT/build/mpc-boot-thingy" ] || [ ! -x "$ROOT/build/mpc-boot-web" ]; then
  echo "ERROR: build did not produce the boot and web binaries" >&2
  exit 1
fi

kind=$(kind_of "$SD")
if [ "$kind" = ours ]; then
  echo "This card already has mpc-boot-thingy. Updating it. Menu entries are kept."
elif [ "$kind" != empty ]; then
  warn_kind "$SD" "$kind"
  confirm_backup
  backup_autorun "$SD"
fi

probe=$SD/.mpc-boot-thingy-write-test
if ! touch "$probe" 2>/dev/null; then
  echo "ERROR: not writable: $SD" >&2
  exit 1
fi
rm -f "$probe"
write_autorun "$SD"
