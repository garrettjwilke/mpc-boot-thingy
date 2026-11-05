#include "menu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void trim(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t' || *a == '\r') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static int ends_with(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && memcmp(s + n - m, suf, m) == 0;
}

int menu_parse_entry(const char *path, struct menu_entry *e) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(e, 0, sizeof *e);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key);
        trim(val);
        if (strcmp(key, "title") == 0) snprintf(e->title, sizeof e->title, "%s", val);
        else if (strcmp(key, "run") == 0) snprintf(e->run, sizeof e->run, "%s", val);
        else if (strcmp(key, "args") == 0) snprintf(e->args, sizeof e->args, "%s", val);
        else if (strcmp(key, "boot") == 0) e->boot_ac50 = strcmp(val, "ac50") == 0;
        else if (strcmp(key, "card") == 0) e->card_pkg = strcmp(val, "0") != 0;
        else if (strcmp(key, "takeover") == 0) e->takeover = strcmp(val, "0") != 0;
        else if (strcmp(key, "dirs") == 0 && strlen(val) < sizeof e->dirs)
            snprintf(e->dirs, sizeof e->dirs, "%s", val);
        else if (strcmp(key, "patches") == 0 && strlen(val) < sizeof e->patches)
            snprintf(e->patches, sizeof e->patches, "%s", val);
    }
    fclose(f);
    if (!e->title[0] || !e->run[0]) return -1;
    return 0;
}

struct found {
    char key[256];
    char path[512];
    char base[512];
    char id[MENU_ID];
    int synthetic; /* path is a run string; title is the folder name in key */
};

static int found_cmp(const void *a, const void *b) {
    return strcmp(((const struct found *)a)->key, ((const struct found *)b)->key);
}

static int push_found(struct found *f, int n, int max, const char *key, const char *path, const char *base, const char *id) {
    if (n >= max) return n;
    if (strlen(key) >= sizeof f->key || strlen(path) >= sizeof f->path || strlen(base) >= sizeof f->base) return n;
    if (strlen(id) >= sizeof f->id) return n;
    snprintf(f[n].key, sizeof f[n].key, "%s", key);
    snprintf(f[n].path, sizeof f[n].path, "%s", path);
    snprintf(f[n].base, sizeof f[n].base, "%s", base);
    snprintf(f[n].id, sizeof f[n].id, "%s", id);
    f[n].synthetic = 0;
    return n + 1;
}

static int join2(char *out, size_t n, const char *a, const char *b) {
    int wr = snprintf(out, n, "%s/%s", a, b);
    return wr < 0 || (size_t)wr >= n ? -1 : 0;
}

/* 0 when rel stays inside the package. -1 when it does not. */
static int script_rel_ok(const char *rel) {
    const char *p;
    if (!rel || !rel[0] || rel[0] == '/' || rel[0] == '\\') return 0;
    p = rel;
    while (*p) {
        const char *start;
        size_t len;
        if (*p == '/' || *p == '\\') return 0;
        if ((unsigned char)*p < 0x20 || *p == 0x7f) return 0;
        start = p;
        while (*p && *p != '/' && *p != '\\') {
            if ((unsigned char)*p < 0x20 || *p == 0x7f) return 0;
            p++;
        }
        len = (size_t)(p - start);
        if (len == 0 || (len == 1 && start[0] == '.') || (len == 2 && start[0] == '.' && start[1] == '.')) return 0;
        if (*p == '\\') return 0;
        if (*p == '/') {
            p++;
            if (!*p) return 0;
        }
    }
    return 1;
}

/* 0 and the path is in out. -1 when the file has no path. -2 when a path is broken. */
static int json_first_path(const char *file, char *out, size_t n) {
    FILE *f = fopen(file, "r");
    char buf[65536];
    size_t got, len;
    char *p;
    int full;
    if (!f) return -1;
    got = fread(buf, 1, sizeof buf - 1, f);
    full = got < sizeof buf - 1;
    fclose(f);
    buf[got] = 0;
    p = buf;
    while ((p = strstr(p, "\"path\"")) != NULL) {
        char *q;
        p += 6;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '"') continue;
        p++;
        q = p;
        while (*q && *q != '"' && *q != '\n' && *q != '\r' && *q != '\\') q++;
        if (*q != '"') return -2;
        len = (size_t)(q - p);
        if (len == 0 || len + 1 > n) return -2;
        memcpy(out, p, len);
        out[len] = 0;
        return 0;
    }
    if (!full) return -2;
    return -1;
}

int menu_nested_run(const char *app_dir, char *run, size_t n) {
    char pkg[512], json[512], script[512], rel[MENU_RUN];
    struct stat st;
    int from_json = 0;
    if (!app_dir || !app_dir[0] || !run || n == 0) return -1;
    if (join2(pkg, sizeof pkg, app_dir, "autorun") != 0) return -1;
    if (lstat(pkg, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
    rel[0] = 0;
    if (join2(json, sizeof json, pkg, "autorun.json") == 0 &&
        lstat(json, &st) == 0 && S_ISREG(st.st_mode)) {
        int jr = json_first_path(json, rel, sizeof rel);
        if (jr == -2) return -1;
        if (jr == 0) from_json = 1;
    }
    if (!from_json) {
        if (snprintf(rel, sizeof rel, "autorun.sh") >= (int)sizeof rel) return -1;
    }
    if (rel[0] == '.' && rel[1] == '/') memmove(rel, rel + 2, strlen(rel + 2) + 1);
    if (!script_rel_ok(rel)) return -1;
    if (join2(script, sizeof script, pkg, rel) != 0) return -1;
    if (lstat(script, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    if (snprintf(run, n, "autorun/%s", rel) >= (int)n) return -1;
    return 0;
}

/* Last "/apps/<name>/autorun/<script>" in script. 0 and pkg/rel are set. */
static int nested_autorun_parts(const char *script, char *pkg, size_t pkg_n, char *rel, size_t rel_n) {
    const char *scan;
    int found = 0;
    if (!script || script[0] != '/') return -1;
    scan = script;
    while ((scan = strstr(scan, "/apps/")) != NULL) {
        const char *name = scan + 6;
        const char *slash = strchr(name, '/');
        const char *inside;
        size_t name_len, pkg_len, rel_len;
        scan = name;
        if (!slash) continue;
        name_len = (size_t)(slash - name);
        if (name_len == 0 || (name_len == 1 && name[0] == '.') ||
            (name_len == 2 && name[0] == '.' && name[1] == '.'))
            continue;
        if (strncmp(slash, "/autorun/", 9) != 0) continue;
        inside = slash + 9;
        if (!script_rel_ok(inside)) continue;
        pkg_len = (size_t)((slash + 8) - script);
        rel_len = strlen(inside);
        if (pkg_len + 1 > pkg_n || rel_len + 1 > rel_n) continue;
        memcpy(pkg, script, pkg_len);
        pkg[pkg_len] = 0;
        memcpy(rel, inside, rel_len + 1);
        found = 1;
    }
    return found ? 0 : -1;
}

/* 1 for autorun.sh or the script named by autorun.json. */
static int is_package_entry(const char *pkg, const char *rel) {
    char json[512], named[MENU_RUN];
    int jr;
    if (strcmp(rel, "autorun.sh") == 0) return 1;
    if (join2(json, sizeof json, pkg, "autorun.json") != 0) return 0;
    jr = json_first_path(json, named, sizeof named);
    if (jr != 0) return 0;
    if (named[0] == '.' && named[1] == '/') memmove(named, named + 2, strlen(named + 2) + 1);
    if (!script_rel_ok(named)) return 0;
    return strcmp(named, rel) == 0;
}

/* 1 when the first 64KB of path contains a stock-replacement marker. */
static int script_has_marker(const char *path) {
    FILE *f = fopen(path, "rb");
    char buf[65536 + 1];
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, 65536, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "ac50") != NULL || strstr(buf, "LD_PRELOAD") != NULL;
}

/* 1 when any regular .sh under dir, skipping symlinks, replaces stock. */
static int sh_replaces_stock(const char *dir, int depth) {
    DIR *d;
    struct dirent *de;
    int found = 0;
    if (depth > 16) return 0;
    d = opendir(dir);
    if (!d) return 0;
    while (!found && (de = readdir(d)) != NULL) {
        char path[512];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        if (join2(path, sizeof path, dir, de->d_name) != 0) continue;
        if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode))
            found = sh_replaces_stock(path, depth + 1);
        else if (S_ISREG(st.st_mode) && ends_with(de->d_name, ".sh") && script_has_marker(path))
            found = 1;
    }
    closedir(d);
    return found;
}

static int is_takeover_package(const char *pkg) {
    char p[512];
    struct stat st;
    if (join2(p, sizeof p, pkg, "mod-launcher.sh") == 0 &&
        lstat(p, &st) == 0 && S_ISREG(st.st_mode)) {
        return 1;
    }
    return 0;
}

int menu_autorun_kind(const char *script_path) {
    char pkg[512], rel[MENU_RUN], full[512];
    struct stat st;
    if (nested_autorun_parts(script_path, pkg, sizeof pkg, rel, sizeof rel) != 0) return -1;
    if (lstat(pkg, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
    if (!is_package_entry(pkg, rel)) return -1;
    if (join2(full, sizeof full, pkg, rel) != 0) return -1;
    if (lstat(full, &st) == 0 && (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))) return -1;
    if (is_takeover_package(pkg)) return 2;
    return sh_replaces_stock(pkg, 0) ? 1 : 0;
}

/* *.entry files in dir. run is relative to base. key is the filename. */
static int collect_entries(struct found *f, int n, int max, const char *dir, const char *base) {
    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max) {
        if (de->d_name[0] == '.') continue;
        if (!ends_with(de->d_name, ".entry")) continue;
        char path[512], id[MENU_ID];
        if (join2(path, sizeof path, dir, de->d_name) != 0) continue;
        if (snprintf(id, sizeof id, "entry:%s", de->d_name) >= (int)sizeof id) continue;
        n = push_found(f, n, max, de->d_name, path, base, id);
    }
    closedir(d);
    return n;
}

/* autorun/apps/<name>/boot.entry. run is relative to that folder. key is the folder name.
   A nested autorun package with no boot.entry is listed from its script. */
static int collect_apps(struct found *f, int n, int max, const char *apps) {
    DIR *d = opendir(apps);
    if (!d) return n;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max) {
        if (de->d_name[0] == '.') continue;
        char folder[512], path[512];
        if (join2(folder, sizeof folder, apps, de->d_name) != 0) continue;
        struct stat st;
        if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (join2(path, sizeof path, folder, "boot.entry") != 0) continue;
        char id[MENU_ID];
        if (snprintf(id, sizeof id, "app:%s", de->d_name) >= (int)sizeof id) continue;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            n = push_found(f, n, max, de->d_name, path, folder, id);
            continue;
        }
        {
            char run[MENU_RUN];
            int next;
            if (menu_nested_run(folder, run, sizeof run) != 0) continue;
            next = push_found(f, n, max, de->d_name, run, folder, id);
            if (next > n) f[n].synthetic = 1;
            n = next;
        }
    }
    closedir(d);
    return n;
}

int menu_load(struct menu *m, const char *card) {
    memset(m, 0, sizeof *m);
    snprintf(m->items[0].title, sizeof m->items[0].title, "Stock");
    snprintf(m->items[0].id, sizeof m->items[0].id, "stock");
    m->items[0].stock = 1;
    m->n = 1;
    if (!card || !card[0]) return m->n;

    enum { MAX_FILES = 64 };
    struct found found[MAX_FILES];
    int nc = 0;
    char bootmenu[512], apps[512];
    if (join2(bootmenu, sizeof bootmenu, card, "autorun/boot-menu") == 0)
        nc = collect_entries(found, nc, MAX_FILES, bootmenu, card);
    if (join2(apps, sizeof apps, card, "autorun/apps") == 0)
        nc = collect_apps(found, nc, MAX_FILES, apps);
    qsort(found, (size_t)nc, sizeof found[0], found_cmp);

    for (int i = 0; i < nc && m->n < MENU_MAX; i++) {
        struct menu_entry e;
        if (found[i].synthetic) {
            memset(&e, 0, sizeof e);
            if (!found[i].key[0] || !found[i].path[0]) continue;
            if (snprintf(e.title, sizeof e.title, "%s", found[i].key) >= (int)sizeof e.title) continue;
            if (snprintf(e.run, sizeof e.run, "%s", found[i].path) >= (int)sizeof e.run) continue;
        } else if (menu_parse_entry(found[i].path, &e) != 0) {
            continue;
        }
        snprintf(e.base, sizeof e.base, "%s", found[i].base);
        snprintf(e.id, sizeof e.id, "%s", found[i].id);
        char script_path[512];
        if (e.run[0] == '/') snprintf(script_path, sizeof script_path, "%s", e.run);
        else join2(script_path, sizeof script_path, e.base[0] ? e.base : card, e.run);
        if (menu_autorun_kind(script_path) == 2) e.takeover = 1;
        m->items[m->n++] = e;
    }
    return m->n;
}

int menu_default_index(const struct menu *m, const char *card) {
    if (!m || m->n < 1 || !card || !card[0]) return 0;
    char path[512];
    int wr = snprintf(path, sizeof path, "%s/autorun/system/boot-default", card);
    if (wr < 0 || (size_t)wr >= sizeof path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[MENU_ID];
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    int truncated = strchr(line, '\n') == NULL && !feof(f);
    fclose(f);
    if (truncated) return 0;
    trim(line);
    if (!line[0] || strcmp(line, "stock") == 0) return 0;
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->items[i].id, line) == 0) return i;
    }
    return 0;
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

int menu_set_default(const char *card, const char *id) {
    char path[512], tmp[512], line[MENU_ID + 2];
    if (!card || !card[0] || !id || !id[0] || strlen(id) >= MENU_ID) return -1;
    if (strchr(id, '\n') || strchr(id, '\r')) return -1;
    int pw = snprintf(path, sizeof path, "%s/autorun/system/boot-default", card);
    int tw = snprintf(tmp, sizeof tmp, "%s/autorun/system/boot-default.tmp", card);
    if (pw < 0 || (size_t)pw >= sizeof path || tw < 0 || (size_t)tw >= sizeof tmp) return -1;
    int n = snprintf(line, sizeof line, "%s\n", id);
    if (n < 0 || (size_t)n >= sizeof line) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    int rc = write_all(fd, line, (size_t)n);
    if (close(fd) != 0) rc = -1;
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int menu_split_args(const char *args, char *buf, size_t buf_n, char **out, int max) {
    if (!args || !buf || buf_n == 0 || max <= 0) return 0;
    snprintf(buf, buf_n, "%s", args);
    int n = 0;
    char *p = buf;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        out[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static int dirs_name_ok(const char *s) {
    size_t n;
    if (!s || !s[0] || s[0] == '.') return 0;
    n = strlen(s);
    if (n >= 200) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return 0;
    }
    if (strstr(s, "..")) return 0;
    return 1;
}

static int dirs_rm(const char *path, int depth) {
    struct stat st;
    DIR *d;
    struct dirent *de;
    if (depth > 16 || lstat(path, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return unlink(path);
    d = opendir(path);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char child[512];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child) {
            closedir(d);
            return -1;
        }
        if (dirs_rm(child, depth + 1) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path);
}

int menu_card_dirs_remove(const char *card, const char *entry_path) {
    struct menu_entry e;
    char list[MENU_PATCHES];
    char *p;
    if (!card || !card[0] || !entry_path || !entry_path[0]) return 0;
    if (menu_parse_entry(entry_path, &e) != 0) return 0;
    if (!e.dirs[0]) return 0;
    if (strlen(e.dirs) >= sizeof list) return -1;
    snprintf(list, sizeof list, "%s", e.dirs);
    p = list;
    while (*p) {
        char name[256];
        char full[512];
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t i = 0;
        while (*p && *p != ' ' && *p != '\t' && i + 1 < sizeof name) name[i++] = *p++;
        name[i] = 0;
        if (!dirs_name_ok(name)) return -1;
        if (snprintf(full, sizeof full, "%s/%s", card, name) >= (int)sizeof full) return -1;
        if (lstat(full, &(struct stat){0}) == 0) {
            if (dirs_rm(full, 0) != 0) return -1;
        }
    }
    return 0;
}
