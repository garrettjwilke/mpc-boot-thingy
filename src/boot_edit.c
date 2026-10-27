#include "boot_edit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char save_err[96];

const char *boot_edit_last_error(void) {
    return save_err[0] ? save_err : "could not save";
}

static int save_fail(int rc, const char *what) {
    int e = errno;
    if (what && what[0]) {
        if (e) snprintf(save_err, sizeof save_err, "%s: %s", what, strerror(e));
        else snprintf(save_err, sizeof save_err, "%s", what);
    }
    return rc;
}

static int save_text(int rc, const char *what) {
    snprintf(save_err, sizeof save_err, "%s", what ? what : "could not save");
    return rc;
}

static const char LAUNCH_BODY[] =
    "set -eu\n"
    "HERE=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
    "STOCK=\"${MPC_STOCK:-/usr/bin/MPC Sample}\"\n"
    "LAUNCH=\"${MPC_LAUNCH:-/usr/bin/launch-ac50}\"\n"
    "PATCH_DIR=\"$HERE/patches\"\n"
    "LOADER_SRC=\"$HERE/../../loader.so\"\n"
    "LOADER=/run/mpc-patch-loader.so\n"
    "REAL=/run/launch-ac50.real\n"
    "WRAP=/run/launch-ac50.wrap\n"
    "STAMP=/run/mpc-patch-stamp\n"
    "\n"
    "log() { echo \"[boot-edit] $*\"; }\n"
    "die() { echo \"[boot-edit] ERROR: $*\" >&2; exit 1; }\n"
    "\n"
    "mounted() {\n"
    "    target=$1\n"
    "    [ -r /proc/self/mountinfo ] || return 1\n"
    "    while read -r _ _ _ _ point _; do\n"
    "        [ \"$point\" = \"$target\" ] && return 0\n"
    "    done < /proc/self/mountinfo\n"
    "    return 1\n"
    "}\n"
    "\n"
    "unmount_stock() {\n"
    "    if ! grep -q -F 'MPC\\040Sample' /proc/self/mountinfo 2>/dev/null; then\n"
    "        return 0\n"
    "    fi\n"
    "    umount \"$STOCK\" || die \"could not unmount the previous patched screen\"\n"
    "}\n"
    "\n"
    "sq() {\n"
    "    s=$1\n"
    "    printf \"'\"\n"
    "    while :; do\n"
    "        case $s in\n"
    "            *\\'*)\n"
    "                printf '%s' \"${s%%\\'*}\"\n"
    "                printf \"'\\\\''\"\n"
    "                s=${s#*\\'}\n"
    "                ;;\n"
    "            *)\n"
    "                printf '%s' \"$s\"\n"
    "                break\n"
    "                ;;\n"
    "        esac\n"
    "    done\n"
    "    printf \"'\"\n"
    "}\n"
    "\n"
    "stamp_matches() {\n"
    "    [ -f \"$STAMP\" ] || return 1\n"
    "    got=$(cat \"$STAMP\") || return 1\n"
    "    [ \"$got\" = \"$PATCH_DIR\" ]\n"
    "}\n"
    "\n"
    "[ -d \"$PATCH_DIR\" ] || die \"missing $PATCH_DIR\"\n"
    "[ -r \"$LOADER_SRC\" ] || die \"missing $LOADER_SRC\"\n"
    "[ -r \"$LAUNCH\" ] || die \"cannot read $LAUNCH\"\n"
    "\n"
    "unmount_stock\n"
    "if mounted \"$LAUNCH\"; then\n"
    "    if stamp_matches; then\n"
    "        log \"patch loader already mounted\"\n"
    "        exit 0\n"
    "    fi\n"
    "    umount \"$LAUNCH\" || die \"could not replace the patch loader\"\n"
    "fi\n"
    "\n"
    "cp \"$LOADER_SRC\" \"$LOADER\"\n"
    "chmod 755 \"$LOADER\"\n"
    "\n"
    "if ! mounted \"$REAL\"; then\n"
    "    cp \"$LAUNCH\" \"$REAL\" || die \"could not keep the stock launcher\"\n"
    "    chmod 755 \"$REAL\"\n"
    "fi\n"
    "\n"
    "{\n"
    "    printf '%s\\n' '#!/bin/sh'\n"
    "    printf '%s\\n' 'export LD_PRELOAD=/run/mpc-patch-loader.so'\n"
    "    printf 'export MPC_PATCH_DIR=%s\\n' \"$(sq \"$PATCH_DIR\")\"\n"
    "    printf '%s\\n' 'exec /run/launch-ac50.real \"$@\"'\n"
    "} > \"$WRAP\"\n"
    "chmod 755 \"$WRAP\"\n"
    "mount --bind \"$WRAP\" \"$LAUNCH\" || die \"could not mount the patch loader\"\n"
    "printf '%s\\n' \"$PATCH_DIR\" > \"$STAMP\" || die \"could not record the patch directory\"\n"
    "log \"patch loader mounted\"\n";

static void trim(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t' || *a == '\r') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static int join2(char *out, size_t n, const char *a, const char *b) {
    int wr = snprintf(out, n, "%s/%s", a, b);
    return wr < 0 || (size_t)wr >= n ? -1 : 0;
}

static int ends_with(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && memcmp(s + n - m, suf, m) == 0;
}

static int plain_name(const char *s, size_t max) {
    size_t n;
    if (!s || !s[0] || s[0] == '.') return 0;
    n = strlen(s);
    if (n >= max || strstr(s, "..")) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                 c == '-';
        if (!ok) return 0;
    }
    return 1;
}

static int patch_id_ok(const char *s) {
    return plain_name(s, BOOT_PATCH_ID) && (ends_with(s, ".list") || ends_with(s, ".so"));
}

static int one_line(const char *s) {
    if (!s || !s[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return 0;
    }
    return 1;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

/* edisksd mounts the card noexec, and a failed remount can leave it read-only. */
static void remount_card(const char *card) {
    pid_t pid;
    if (!card || !card[0] || access("/proc/mounts", R_OK) != 0) return;
    pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        execlp("mount", "mount", "-o", "remount,rw,exec", card, (char *)0);
        _exit(127);
    }
    while (waitpid(pid, 0, 0) < 0 && errno == EINTR) {}
}

static int copy_fail(int in, int out, const char *dst) {
    int e = errno;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    if (dst) unlink(dst);
    errno = e;
    return -1;
}

static int copy_file(const char *src, const char *dst) {
    char buf[8192];
    int in, out;
    struct stat st;
    in = open(src, O_RDONLY | O_NOFOLLOW);
    if (in < 0) return -1;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (errno == 0) errno = EINVAL;
        return copy_fail(in, -1, 0);
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (out < 0) return copy_fail(in, -1, 0);
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            return copy_fail(in, out, dst);
        }
        if (r == 0) break;
        size_t off = 0;
        while (off < (size_t)r) {
            ssize_t w = write(out, buf + off, (size_t)r - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                return copy_fail(in, out, dst);
            }
            off += (size_t)w;
        }
    }
    close(in);
    if (close(out) != 0) {
        int e = errno;
        unlink(dst);
        errno = e;
        return -1;
    }
    return 0;
}

static int rm_tree(const char *path, int depth) {
    struct stat st;
    DIR *d;
    struct dirent *de;
    if (depth > 8 || lstat(path, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return unlink(path);
    d = opendir(path);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char child[512];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (join2(child, sizeof child, path, de->d_name) != 0 || rm_tree(child, depth + 1) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path);
}

static int write_all(int fd, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int write_text(const char *path, const char *body, mode_t mode) {
    int fd, rc;
    /* Write the final name directly. exFAT often refuses chmod and rename. */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
    if (fd < 0) return -1;
    rc = write_all(fd, body, strlen(body));
    if (close(fd) != 0) rc = -1;
    return rc;
}

static void title_of(const char *path, int is_list, const char *filename, char *out, size_t n) {
    FILE *f;
    char line[256];
    snprintf(out, n, "%s", filename);
    if (!is_list) return;
    f = fopen(path, "r");
    if (!f) return;
    if (fgets(line, sizeof line, f) && strchr(line, '\n')) {
        trim(line);
        if (strncmp(line, "# title ", 8) == 0 && line[8] && strlen(line + 8) < n) snprintf(out, n, "%s", line + 8);
    }
    fclose(f);
}

static int patch_cmp(const void *a, const void *b) {
    return strcmp(((const struct boot_patch *)a)->id, ((const struct boot_patch *)b)->id);
}

int boot_edit_list(const char *card, struct boot_patch *out, int max) {
    char dir[512], path[512];
    DIR *d;
    struct dirent *de;
    int n = 0;
    if (!card || !card[0] || !out || max <= 0) return 0;
    if (max > BOOT_PATCH_MAX) max = BOOT_PATCH_MAX;
    if (join2(dir, sizeof dir, card, "autorun/patch-catalog") != 0) return 0;
    d = opendir(dir);
    if (!d) return 0;
    while ((de = readdir(d)) != NULL && n < max) {
        struct stat st;
        if (!patch_id_ok(de->d_name)) continue;
        if (strcmp(de->d_name, "boot-menu.list") == 0) continue;
        if (join2(path, sizeof path, dir, de->d_name) != 0) continue;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(out[n].id, sizeof out[n].id, "%s", de->d_name);
        title_of(path, ends_with(de->d_name, ".list"), de->d_name, out[n].title, sizeof out[n].title);
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], patch_cmp);
    return n;
}

static int catalog_file(const char *card, const char *id, char *out, size_t n) {
    char dir[512];
    struct stat st;
    if (!patch_id_ok(id)) return -1;
    if (join2(dir, sizeof dir, card, "autorun/patch-catalog") != 0) return -1;
    if (join2(out, n, dir, id) != 0) return -1;
    if (lstat(out, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return 0;
}

static int stem_of(const char *id, char *stem, size_t n) {
    const char *name;
    size_t len;
    if (!id || strncmp(id, "entry:", 6) != 0) return -1;
    name = id + 6;
    len = strlen(name);
    if (len <= 6 || !ends_with(name, ".entry")) return -1;
    if (len - 6 >= n) return -1;
    memcpy(stem, name, len - 6);
    stem[len - 6] = 0;
    return plain_name(stem, n) ? 0 : -1;
}

static int entry_is_edit(const char *path) {
    FILE *f = fopen(path, "r");
    char line[1024];
    int yes = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (strcmp(line, "boot=ac50") == 0) yes = 1;
    }
    fclose(f);
    return yes;
}

static int name_kept(const char *name, const char *const *patch_ids, int npatches) {
    int i;
    if (strcmp(name, "boot-menu.list") == 0) return 1;
    for (i = 0; i < npatches; i++) {
        if (strcmp(name, patch_ids[i]) == 0) return 1;
    }
    return 0;
}

static int fill_patches(const char *card, const char *slug, const char *const *patch_ids, int npatches) {
    char edits[512], entry[512], patch_dir[512], src[512], dst[512], menu[512];
    int i;
    DIR *d;
    struct dirent *de;
    /* Copy straight into the final directory. Renaming a directory on the
       card's exFAT volume was failing the save. */
    if (join2(edits, sizeof edits, card, "autorun/boot-edits") != 0) return save_text(-1, "path too long");
    if (ensure_dir(edits) != 0) return save_fail(-1, "boot-edits");
    if (join2(entry, sizeof entry, edits, slug) != 0) return save_text(-1, "path too long");
    if (ensure_dir(entry) != 0) return save_fail(-1, "entry folder");
    if (join2(patch_dir, sizeof patch_dir, entry, "patches") != 0) return save_text(-1, "path too long");
    if (ensure_dir(patch_dir) != 0) return save_fail(-1, "patches");
    if (join2(dst, sizeof dst, entry, "patches.work") == 0 && lstat(dst, &(struct stat){0}) == 0) rm_tree(dst, 0);
    if (join2(dst, sizeof dst, entry, "patches.old") == 0 && lstat(dst, &(struct stat){0}) == 0) rm_tree(dst, 0);
    for (i = 0; i < npatches; i++) {
        if (catalog_file(card, patch_ids[i], src, sizeof src) != 0) return BOOT_EDIT_BAD_PATCH;
        if (join2(dst, sizeof dst, patch_dir, patch_ids[i]) != 0) return save_text(-1, "path too long");
        if (copy_file(src, dst) != 0) return save_fail(-1, "copy patch");
    }
    if (join2(menu, sizeof menu, card, "autorun/patches/boot-menu.list") == 0) {
        struct stat st;
        if (lstat(menu, &st) == 0 && S_ISREG(st.st_mode)) {
            if (join2(dst, sizeof dst, patch_dir, "boot-menu.list") != 0) return save_text(-1, "path too long");
            if (copy_file(menu, dst) != 0) return save_fail(-1, "copy boot menu");
        }
    }
    d = opendir(patch_dir);
    if (!d) return save_fail(-1, "patches");
    while ((de = readdir(d)) != NULL) {
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (name_kept(de->d_name, patch_ids, npatches)) continue;
        if (join2(dst, sizeof dst, patch_dir, de->d_name) != 0) {
            closedir(d);
            return save_text(-1, "path too long");
        }
        if (lstat(dst, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (unlink(dst) != 0) {
            closedir(d);
            return save_fail(-1, "remove old patch");
        }
    }
    closedir(d);
    return 0;
}

static int write_launch(const char *card, const char *slug, const char *const *patch_ids, int npatches) {
    char path[512], body[8192];
    int n, i, left;
    char *p;
    if (snprintf(path, sizeof path, "%s/autorun/boot-edits/%s/launch.sh", card, slug) >= (int)sizeof path)
        return save_text(-1, "path too long");
    n = snprintf(body, sizeof body, "#!/bin/sh\n# Boot Edit entry. The boot thingy runs this with sh.\n# patches:");
    if (n < 0 || (size_t)n >= sizeof body) return save_text(-1, "launch script too long");
    p = body + n;
    left = (int)sizeof body - n;
    for (i = 0; i < npatches; i++) {
        int w = snprintf(p, (size_t)left, " %s", patch_ids[i]);
        if (w < 0 || w >= left) return save_text(-1, "launch script too long");
        p += w;
        left -= w;
    }
    if (snprintf(p, (size_t)left, "\n%s", LAUNCH_BODY) >= left) return save_text(-1, "launch script too long");
    if (write_text(path, body, 0755) != 0) return save_fail(-1, "launch.sh");
    return 0;
}

static int write_entry(const char *path, const char *title, const char *slug, const char *patches, const char *args) {
    char body[MENU_TITLE + MENU_PATCHES + MENU_ARGS + 128];
    int n = snprintf(body, sizeof body,
                     "title=%s\nrun=autorun/boot-edits/%s/launch.sh\nboot=ac50\npatches=%s\n",
                     title, slug, patches ? patches : "");
    if (n < 0 || (size_t)n >= sizeof body) return save_text(-1, "menu entry too long");
    if (args && args[0]) {
        int w = snprintf(body + n, sizeof body - (size_t)n, "args=%s\n", args);
        if (w < 0 || (size_t)(n + w) >= sizeof body) return save_text(-1, "menu entry too long");
    }
    if (write_text(path, body, 0644) != 0) return save_fail(-1, "menu entry");
    return 0;
}

int boot_edit_save(const char *card, const char *id, const char *title, const char *const *patch_ids, int npatches,
                   char *out_id, size_t out_n) {
    char stem[64], joined[MENU_PATCHES], entry[512], auto_title[MENU_TITLE];
    char old_args[MENU_ARGS] = {0};
    int create, i, used, ntitle;
    const char *use_title;
    save_err[0] = 0;
    if (!card || !card[0] || !out_id || out_n < 8) return save_text(BOOT_EDIT_ERR, "missing card");
    remount_card(card);
    if (npatches < 0 || npatches > BOOT_PATCH_MAX || (npatches > 0 && !patch_ids)) return BOOT_EDIT_BAD_PATCH;
    used = 0;
    joined[0] = 0;
    for (i = 0; i < npatches; i++) {
        int j;
        size_t L;
        if (!patch_ids[i] || catalog_file(card, patch_ids[i], entry, sizeof entry) != 0) return BOOT_EDIT_BAD_PATCH;
        for (j = 0; j < i; j++) {
            if (strcmp(patch_ids[j], patch_ids[i]) == 0) return BOOT_EDIT_BAD_PATCH;
        }
        L = strlen(patch_ids[i]);
        if (used && used + 1 >= (int)sizeof joined) return BOOT_EDIT_ERR;
        if (used) joined[used++] = ',';
        if (used + (int)L >= (int)sizeof joined) return BOOT_EDIT_ERR;
        memcpy(joined + used, patch_ids[i], L);
        used += (int)L;
        joined[used] = 0;
    }
    create = !id || !id[0];
    if (create) {
        int num;
        if (title && title[0]) {
            if (!one_line(title) || strlen(title) >= MENU_TITLE) return BOOT_EDIT_BAD_TITLE;
            use_title = title;
        } else {
            use_title = NULL;
        }
        stem[0] = 0;
        for (num = 1; num < 1000; num++) {
            char dir[512];
            struct stat st;
            if (snprintf(stem, sizeof stem, "boot-%d", num) >= (int)sizeof stem) return BOOT_EDIT_ERR;
            if (snprintf(entry, sizeof entry, "%s/autorun/boot-menu/%s.entry", card, stem) >= (int)sizeof entry)
                return BOOT_EDIT_ERR;
            if (snprintf(dir, sizeof dir, "%s/autorun/boot-edits/%s", card, stem) >= (int)sizeof dir) return BOOT_EDIT_ERR;
            if (lstat(entry, &st) == 0 || lstat(dir, &st) == 0) continue;
            if (!use_title) {
                ntitle = snprintf(auto_title, sizeof auto_title, "Boot %d", num);
                if (ntitle < 0 || (size_t)ntitle >= sizeof auto_title) return BOOT_EDIT_ERR;
                use_title = auto_title;
            }
            break;
        }
        if (!stem[0] || !use_title || num >= 1000) return BOOT_EDIT_ERR;
    } else {
        if (stem_of(id, stem, sizeof stem) != 0) return BOOT_EDIT_BAD_ID;
        if (snprintf(entry, sizeof entry, "%s/autorun/boot-menu/%s.entry", card, stem) >= (int)sizeof entry)
            return BOOT_EDIT_ERR;
        if (!entry_is_edit(entry)) return BOOT_EDIT_BAD_ID;
        if (!title || !one_line(title) || strlen(title) >= MENU_TITLE) return BOOT_EDIT_BAD_TITLE;
        use_title = title;
        struct menu_entry old_e;
        if (menu_parse_entry(entry, &old_e) == 0) {
            snprintf(old_args, sizeof old_args, "%s", old_e.args);
        }
    }
    if (ensure_dir(card) != 0) return save_fail(BOOT_EDIT_ERR, "card");
    {
        char autorun_dir[512], menu_dir[512];
        if (join2(autorun_dir, sizeof autorun_dir, card, "autorun") != 0) return save_text(BOOT_EDIT_ERR, "path too long");
        if (ensure_dir(autorun_dir) != 0) return save_fail(BOOT_EDIT_ERR, "autorun");
        if (join2(menu_dir, sizeof menu_dir, autorun_dir, "boot-menu") != 0) return save_text(BOOT_EDIT_ERR, "path too long");
        if (ensure_dir(menu_dir) != 0) return save_fail(BOOT_EDIT_ERR, "boot menu");
    }
    {
        int filled = fill_patches(card, stem, patch_ids, npatches);
        if (filled != 0) return filled == BOOT_EDIT_BAD_PATCH ? filled : (save_err[0] ? filled : save_fail(BOOT_EDIT_ERR, "patches"));
    }
    if (write_launch(card, stem, patch_ids, npatches) != 0 || write_entry(entry, use_title, stem, joined, old_args) != 0) {
        if (create) {
            char dir[512];
            struct stat st;
            if (snprintf(dir, sizeof dir, "%s/autorun/boot-edits/%s", card, stem) < (int)sizeof dir && lstat(entry, &st) != 0)
                rm_tree(dir, 0);
        }
        return BOOT_EDIT_ERR;
    }
    if (snprintf(out_id, out_n, "entry:%s.entry", stem) >= (int)out_n) return BOOT_EDIT_ERR;
    return BOOT_EDIT_OK;
}

int boot_edit_remove(const char *card, const char *id) {
    char stem[64], entry[512], dir[512];
    struct stat st;
    if (!card || !card[0] || stem_of(id, stem, sizeof stem) != 0) return 0;
    if (snprintf(entry, sizeof entry, "%s/autorun/boot-menu/%s.entry", card, stem) >= (int)sizeof entry) return -1;
    if (!entry_is_edit(entry)) return 0;
    if (snprintf(dir, sizeof dir, "%s/autorun/boot-edits/%s", card, stem) >= (int)sizeof dir) return -1;
    if (lstat(dir, &st) != 0) return 0;
    return rm_tree(dir, 0);
}

static int safe_component(const char *s) {
    if (!s || !s[0] || strlen(s) >= 200) return 0;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
    if (strstr(s, "..")) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return 0;
    }
    return 1;
}

int boot_entry_update(const char *card, const char *id, const char *new_title, const char *new_args) {
    char path[512];
    if (!card || !card[0] || !id || !id[0]) return -1;
    remount_card(card);

    if (strncmp(id, "app:", 4) == 0) {
        const char *folder = id + 4;
        if (!safe_component(folder)) return -1;
        if (snprintf(path, sizeof path, "%s/apps/%s/boot.entry", card, folder) >= (int)sizeof path) return -1;
    } else if (strncmp(id, "entry:", 6) == 0) {
        const char *file = id + 6;
        if (!safe_component(file) || !ends_with(file, ".entry")) return -1;
        if (snprintf(path, sizeof path, "%s/autorun/boot-menu/%s", card, file) >= (int)sizeof path) return -1;
    } else {
        return -1;
    }

    struct menu_entry existing;
    if (menu_parse_entry(path, &existing) != 0) return -1;

    const char *title = (new_title && new_title[0]) ? new_title : existing.title;
    if (!one_line(title) || strlen(title) >= MENU_TITLE) return -1;

    const char *args = new_args ? new_args : existing.args;
    if (args[0] && (!one_line(args) || strlen(args) >= MENU_ARGS)) return -1;

    char body[MENU_TITLE + MENU_RUN + MENU_ARGS + MENU_PATCHES + 128];
    int n = snprintf(body, sizeof body, "title=%s\nrun=%s\n", title, existing.run);
    if (n < 0 || (size_t)n >= sizeof body) return -1;

    if (args[0]) {
        int w = snprintf(body + n, sizeof body - (size_t)n, "args=%s\n", args);
        if (w < 0 || (size_t)(n + w) >= sizeof body) return -1;
        n += w;
    }

    if (existing.boot_ac50) {
        int w = snprintf(body + n, sizeof body - (size_t)n, "boot=ac50\n");
        if (w < 0 || (size_t)(n + w) >= sizeof body) return -1;
        n += w;
        if (existing.patches[0]) {
            w = snprintf(body + n, sizeof body - (size_t)n, "patches=%s\n", existing.patches);
            if (w < 0 || (size_t)(n + w) >= sizeof body) return -1;
            n += w;
        }
    }

    return write_text(path, body, 0644);
}

int boot_entry_delete(const char *card, const char *id) {
    if (!card || !card[0] || !id || !id[0]) return -1;
    remount_card(card);

    if (strncmp(id, "app:", 4) == 0) {
        const char *name = id + 4;
        if (!safe_component(name)) return -1;
        char app_dir[512];
        if (snprintf(app_dir, sizeof app_dir, "%s/apps/%s", card, name) >= (int)sizeof app_dir) return -1;
        struct stat st;
        if (lstat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
        if (rm_tree(app_dir, 0) != 0) return -1;
    } else if (strncmp(id, "entry:", 6) == 0) {
        const char *name = id + 6;
        if (!safe_component(name) || !ends_with(name, ".entry")) return -1;
        char entry_path[512];
        if (snprintf(entry_path, sizeof entry_path, "%s/autorun/boot-menu/%s", card, name) >= (int)sizeof entry_path) return -1;
        struct stat st;
        if (lstat(entry_path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

        boot_edit_remove(card, id);

        if (unlink(entry_path) != 0) return -1;
    } else {
        return -1;
    }

    char def_path[512], line[MENU_ID];
    if (snprintf(def_path, sizeof def_path, "%s/autorun/boot-default", card) < (int)sizeof def_path) {
        FILE *df = fopen(def_path, "r");
        if (df) {
            int is_def = 0;
            if (fgets(line, sizeof line, df)) {
                trim(line);
                if (strcmp(line, id) == 0) is_def = 1;
            }
            fclose(df);
            if (is_def) menu_set_default(card, "stock");
        }
    }
    return 0;
}
