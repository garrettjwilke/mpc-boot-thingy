#!/usr/bin/env python3
"""Build the exFAT-safe ARM64 shell tools used by an SSH login.

Writes os_tools/bin and os_tools/toolkit. The default tree is bash and
the libraries it needs. --tools also adds vim, nano, busybox-full, and
the other utilities. Wrappers locate os_tools from their own path, so
the card does not have to be mounted at /media/MPC.
"""

import io
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

MIRROR = "http://dl-cdn.alpinelinux.org/alpine/v3.20/main/aarch64"

# bash is the login shell. The rest are dev utilities and stay off the
# release card unless setup is run with --tools.
BASE = ["musl", "bash", "ncurses-terminfo-base"]
EXTRA = [
    "busybox-static", "file", "rsync", "vim", "nano",
    "strace", "htop", "tree", "lsof", "jq", "xxd",
    "curl", "less", "grep", "sed", "gawk", "findutils",
]
SKIP = {"yash", "yash-binsh", "busybox", "busybox-binsh"}

WRAPPER = """#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
"""


def die(msg):
    print("ERROR: " + msg, file=sys.stderr)
    raise SystemExit(1)


def load_index(mirror):
    print("Fetching package index from %sAPKINDEX.tar.gz" % mirror)
    data = urllib.request.urlopen(mirror + "APKINDEX.tar.gz", timeout=20).read()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        text = tf.extractfile("APKINDEX").read().decode("utf-8", errors="ignore")
    pkgs = {}
    for block in text.split("\n\n"):
        info = {}
        for line in block.splitlines():
            if ":" in line:
                key, val = line.split(":", 1)
                info[key] = val
        if "P" not in info:
            continue
        pkgs[info["P"]] = info
        for provide in info.get("p", "").split():
            name = provide.split("=")[0]
            if name not in pkgs:
                pkgs[name] = info
    return pkgs


def resolve(pkgs, wanted):
    resolved = {}
    queue = list(wanted)
    while queue:
        name = queue.pop(0)
        if name in SKIP or name in resolved or name not in pkgs:
            continue
        info = pkgs[name]
        real = info["P"]
        if real in SKIP or real in resolved:
            continue
        resolved[real] = "%s-%s.apk" % (real, info["V"])
        for dep in info.get("D", "").split():
            if dep.startswith("!"):
                continue
            dep_name = dep.split("=")[0].split("<")[0].split(">")[0].split("~")[0]
            if dep_name not in resolved and dep_name not in SKIP:
                queue.append(dep_name)
    missing = [name for name in wanted if name not in pkgs and name not in resolved]
    if missing:
        die("package index has no %s" % ", ".join(missing))
    return resolved


def copy_libs(stage, real_lib):
    for lib_src in (stage / "lib", stage / "usr" / "lib"):
        if not lib_src.is_dir():
            continue
        for item in lib_src.iterdir():
            if item.is_dir():
                continue
            if not (item.is_symlink() or item.name.count(".") == 2 or item.name.startswith("ld-musl")):
                continue
            target = item.resolve()
            if target.is_file():
                shutil.copy2(target, real_lib / item.name)


def copy_share(stage, real_share, dev):
    magic = stage / "usr" / "share" / "misc" / "magic.mgc"
    if magic.is_file():
        (real_share / "misc").mkdir(parents=True, exist_ok=True)
        shutil.copy2(magic, real_share / "misc" / "magic.mgc")
    for term in (stage / "etc" / "terminfo", stage / "usr" / "share" / "terminfo"):
        if term.is_dir():
            shutil.copytree(term, real_share / "terminfo", symlinks=False)
            break
    if not dev:
        return None
    vim_src = stage / "usr" / "share" / "vim"
    if not vim_src.is_dir():
        die("vim runtime was not in the packages")
    shutil.copytree(vim_src, real_share / "vim", symlinks=False)
    versions = sorted(
        p for p in (real_share / "vim").iterdir()
        if p.is_dir() and p.name.startswith("vim")
    )
    if not versions:
        die("vim runtime directory was not found")
    for vim_ver in versions:
        for sub in ("doc", "tutor", "spell", "lang"):
            nested = vim_ver / sub
            if nested.exists():
                shutil.rmtree(nested)
    return versions[-1].name


def write_wrapper(path, name, extra, login=False):
    text = WRAPPER + extra
    if login:
        # No arguments means sshd started this as the login shell. A script
        # loses the leading-dash argv0, so ask bash for a login shell here.
        text += (
            'case ":${PATH}:" in\n'
            '  *":$root/bin:"*) ;;\n'
            '  *) PATH="$root/bin:$PATH" ;;\n'
            'esac\n'
            'export PATH\n'
            'if [ $# -eq 0 ]; then set -- -l -i; fi\n'
        )
    text += (
        'exec "$root/toolkit/lib/ld-musl-aarch64.so.1" '
        '--library-path "$root/toolkit/lib" '
        '"$root/toolkit/bin/%s" "$@"\n' % name
    )
    path.write_text(text)
    path.chmod(0o755)


def build(out, mirror, dev):
    mirror = mirror.rstrip("/") + "/"
    cache = out.parent
    apk_cache = cache / "apks"
    stage = cache / "stage"
    apk_cache.mkdir(parents=True, exist_ok=True)
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for sub in ("bin", "toolkit"):
        dest = out / sub
        if dest.exists():
            shutil.rmtree(dest)

    wanted = list(BASE)
    if dev:
        wanted.extend(EXTRA)
    resolved = resolve(load_index(mirror), wanted)
    for name, apk in sorted(resolved.items()):
        apk_path = apk_cache / apk
        if not apk_path.exists():
            print("  Downloading %s" % apk)
            apk_path.write_bytes(urllib.request.urlopen(mirror + apk, timeout=60).read())
        subprocess.run(
            ["tar", "-xzf", str(apk_path), "-C", str(stage)],
            stderr=subprocess.DEVNULL,
            check=False,
        )

    bin_dir = out / "bin"
    real_bin = out / "toolkit" / "bin"
    real_lib = out / "toolkit" / "lib"
    real_share = out / "toolkit" / "share"
    for folder in (bin_dir, real_bin, real_lib, real_share):
        folder.mkdir(parents=True)

    copy_libs(stage, real_lib)
    runtime = copy_share(stage, real_share, dev)
    loader = real_lib / "ld-musl-aarch64.so.1"
    if not loader.is_file():
        die("ld-musl-aarch64.so.1 was not in the packages")

    terminfo = 'export TERMINFO="$root/toolkit/share/terminfo"\n'
    tools = [("bin/bash", "bash", terminfo)]
    if dev:
        busybox = stage / "bin" / "busybox.static"
        if not busybox.is_file():
            die("busybox.static was not in the packages")
        shutil.copy2(busybox, bin_dir / "busybox-full")
        (bin_dir / "busybox-full").chmod(0o755)
        tools.extend([
            ("usr/bin/file", "file", 'export MAGIC="$root/toolkit/share/misc/magic.mgc"\n'),
            ("usr/bin/rsync", "rsync", ""),
            ("usr/bin/vim", "vim", 'export VIMRUNTIME="$root/toolkit/share/vim/%s"\n%s' % (runtime, terminfo)),
            ("usr/bin/nano", "nano", terminfo),
            ("usr/bin/htop", "htop", terminfo),
            ("usr/bin/strace", "strace", ""),
            ("usr/bin/lsof", "lsof", ""),
            ("usr/bin/tree", "tree", ""),
            ("usr/bin/jq", "jq", ""),
            ("usr/bin/xxd", "xxd", ""),
            ("usr/bin/curl", "curl", ""),
            ("usr/bin/less", "less", terminfo),
            ("usr/bin/find", "gfind", ""),
            ("usr/bin/gawk", "gawk", ""),
        ])
    for rel, name, extra in tools:
        src = (stage / rel).resolve()
        if not src.is_file():
            die("%s was not in the packages" % rel)
        shutil.copy2(src, real_bin / name)
        write_wrapper(bin_dir / name, name, extra, login=(name == "bash"))

    shutil.rmtree(stage)
    print("Toolkit ready at %s" % out)


def main():
    args = sys.argv[1:]
    dev = False
    if args and args[0] == "--tools":
        dev = True
        args = args[1:]
    if len(args) < 1 or len(args) > 2:
        die("usage: fetch-toolkit.py [--tools] OUTPUT_DIR [MIRROR]")
    mirror = args[1] if len(args) == 2 else MIRROR
    build(Path(args[0]), mirror, dev)


if __name__ == "__main__":
    main()
