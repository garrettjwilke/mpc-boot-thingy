#include "boot_edit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    if (lstat(path, &st) == 0) return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) ? 0 : -1;
    return mkdir(path, 0755);
}

static int copy_file(const char *src, const char *dst) {
    char buf[8192];
    int in, out;
    struct stat st;
    in = open(src, O_RDONLY | O_NOFOLLOW);
    if (in < 0) return -1;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(in);
        return -1;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(in);
            close(out);
            unlink(dst);
            return -1;
        }
        if (r == 0) break;
        size_t off = 0;
        while (off < (size_t)r) {
            ssize_t w = write(out, buf + off, (size_t)r - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                close(in);
                close(out);
                unlink(dst);
                return -1;
            }
            off += (size_t)w;
        }
    }
    close(in);
    if (close(out) != 0) {
        unlink(dst);
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
    char tmp[512];
    int fd, rc;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
    if (fd < 0) return -1;
    rc = write_all(fd, body, strlen(body));
    if (close(fd) != 0) rc = -1;
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }
    if (chmod(tmp, mode) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
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

static int fill_patches(const char *card, const char *slug, const char *const *patch_ids, int npatches) {
    char root[512], work[512], final[512], old[512], src[512], dst[512], menu[512];
    int i;
    if (join2(root, sizeof root, card, "autorun/boot-edits") != 0) return -1;
    if (ensure_dir(root) != 0) return -1;
    if (join2(root, sizeof root, root, slug) != 0) return -1;
    if (ensure_dir(root) != 0) return -1;
    if (join2(work, sizeof work, root, "patches.work") != 0) return -1;
    if (join2(final, sizeof final, root, "patches") != 0) return -1;
    if (join2(old, sizeof old, root, "patches.old") != 0) return -1;
    if (lstat(work, &(struct stat){0}) == 0 && rm_tree(work, 0) != 0) return -1;
    if (mkdir(work, 0755) != 0) return -1;
    for (i = 0; i < npatches; i++) {
        if (catalog_file(card, patch_ids[i], src, sizeof src) != 0) {
            rm_tree(work, 0);
            return BOOT_EDIT_BAD_PATCH;
        }
        if (join2(dst, sizeof dst, work, patch_ids[i]) != 0 || copy_file(src, dst) != 0) {
            rm_tree(work, 0);
            return -1;
        }
    }
    if (join2(menu, sizeof menu, card, "autorun/patches/boot-menu.list") == 0) {
        struct stat st;
        if (lstat(menu, &st) == 0 && S_ISREG(st.st_mode)) {
            if (join2(dst, sizeof dst, work, "boot-menu.list") != 0 || copy_file(menu, dst) != 0) {
                rm_tree(work, 0);
                return -1;
            }
        }
    }
    if (lstat(old, &(struct stat){0}) == 0 && rm_tree(old, 0) != 0) {
        rm_tree(work, 0);
        return -1;
    }
    if (lstat(final, &(struct stat){0}) == 0 && rename(final, old) != 0) {
        rm_tree(work, 0);
        return -1;
    }
    if (rename(work, final) != 0) {
        if (lstat(old, &(struct stat){0}) == 0) rename(old, final);
        rm_tree(work, 0);
        return -1;
    }
    if (lstat(old, &(struct stat){0}) == 0) rm_tree(old, 0);
    return 0;
}

static int write_launch(const char *card, const char *slug, const char *const *patch_ids, int npatches) {
    char path[512], body[8192];
    int n, i, left;
    char *p;
    if (snprintf(path, sizeof path, "%s/autorun/boot-edits/%s/launch.sh", card, slug) >= (int)sizeof path) return -1;
    n = snprintf(body, sizeof body, "#!/bin/sh\n# Boot Edit entry. The boot thingy runs this with sh.\n# patches:");
    if (n < 0 || (size_t)n >= sizeof body) return -1;
    p = body + n;
    left = (int)sizeof body - n;
    for (i = 0; i < npatches; i++) {
        int w = snprintf(p, (size_t)left, " %s", patch_ids[i]);
        if (w < 0 || w >= left) return -1;
        p += w;
        left -= w;
    }
    if (snprintf(p, (size_t)left, "\n%s", LAUNCH_BODY) >= left) return -1;
    return write_text(path, body, 0755);
}

static int write_entry(const char *path, const char *title, const char *slug, const char *patches) {
    char body[MENU_TITLE + MENU_PATCHES + 128];
    int n = snprintf(body, sizeof body,
                     "title=%s\nrun=autorun/boot-edits/%s/launch.sh\nboot=ac50\npatches=%s\n",
                     title, slug, patches ? patches : "");
    if (n < 0 || (size_t)n >= sizeof body) return -1;
    return write_text(path, body, 0644);
}

int boot_edit_save(const char *card, const char *id, const char *title, const char *const *patch_ids, int npatches,
                   char *out_id, size_t out_n) {
    char stem[64], joined[MENU_PATCHES], entry[512], auto_title[MENU_TITLE];
    int create, i, used, ntitle;
    const char *use_title;
    if (!card || !card[0] || !out_id || out_n < 8) return BOOT_EDIT_ERR;
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
    }
    if (ensure_dir(card) != 0) return BOOT_EDIT_ERR;
    {
        char menu_dir[512];
        if (join2(menu_dir, sizeof menu_dir, card, "autorun") != 0) return BOOT_EDIT_ERR;
        if (ensure_dir(menu_dir) != 0) return BOOT_EDIT_ERR;
        if (join2(menu_dir, sizeof menu_dir, menu_dir, "boot-menu") != 0) return BOOT_EDIT_ERR;
        if (ensure_dir(menu_dir) != 0) return BOOT_EDIT_ERR;
    }
    {
        int filled = fill_patches(card, stem, patch_ids, npatches);
        if (filled != 0) return filled == BOOT_EDIT_BAD_PATCH ? filled : BOOT_EDIT_ERR;
    }
    if (write_launch(card, stem, patch_ids, npatches) != 0 || write_entry(entry, use_title, stem, joined) != 0) {
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
