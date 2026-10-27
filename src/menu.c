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
    return n + 1;
}

static int join2(char *out, size_t n, const char *a, const char *b) {
    int wr = snprintf(out, n, "%s/%s", a, b);
    return wr < 0 || (size_t)wr >= n ? -1 : 0;
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

/* apps/<name>/boot.entry. run is relative to that folder. key is the folder name. */
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
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        char id[MENU_ID];
        if (snprintf(id, sizeof id, "app:%s", de->d_name) >= (int)sizeof id) continue;
        n = push_found(f, n, max, de->d_name, path, folder, id);
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
    if (join2(apps, sizeof apps, card, "apps") == 0)
        nc = collect_apps(found, nc, MAX_FILES, apps);
    qsort(found, (size_t)nc, sizeof found[0], found_cmp);

    for (int i = 0; i < nc && m->n < MENU_MAX; i++) {
        struct menu_entry e;
        if (menu_parse_entry(found[i].path, &e) != 0) continue;
        snprintf(e.base, sizeof e.base, "%s", found[i].base);
        snprintf(e.id, sizeof e.id, "%s", found[i].id);
        m->items[m->n++] = e;
    }
    return m->n;
}

int menu_default_index(const struct menu *m, const char *card) {
    if (!m || m->n < 1 || !card || !card[0]) return 0;
    char path[512];
    int wr = snprintf(path, sizeof path, "%s/autorun/boot-default", card);
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
    int pw = snprintf(path, sizeof path, "%s/autorun/boot-default", card);
    int tw = snprintf(tmp, sizeof tmp, "%s/autorun/boot-default.tmp", card);
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
