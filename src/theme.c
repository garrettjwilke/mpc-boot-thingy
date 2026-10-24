#include "theme.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const struct boot_theme themes[] = {
    {.id = "amber", .name = "Amber", .bg = 0x101014, .text = 0xe8e8e8, .dim = 0x9aa0a6, .hl = 0xffb000,
     .bar = 0x2a2418, .box = 0x303840, .menu_fill = 0x18181e, .border = 0x505868, .active_text = 0x101014},
    {.id = "blue", .name = "Blue", .bg = 0x101820, .text = 0xe8e8e8, .dim = 0x8aa0b4, .hl = 0x44aaff,
     .bar = 0x1a2838, .box = 0x243044, .menu_fill = 0x141c28, .border = 0x3a5068, .active_text = 0x101820},
    {.id = "green", .name = "Green", .bg = 0x101810, .text = 0xe8e8e8, .dim = 0x8ab49a, .hl = 0x44dd88,
     .bar = 0x1a2820, .box = 0x243428, .menu_fill = 0x141c16, .border = 0x3a5844, .active_text = 0x101810},
    {.id = "light", .name = "Light", .bg = 0xe8e4dc, .text = 0x1a1a1a, .dim = 0x5c564c, .hl = 0xc45c00,
     .bar = 0xd8cfc0, .box = 0xd4cfc4, .menu_fill = 0xf7f4ee, .border = 0x8a8070, .active_text = 0xfff8f0},
};

int theme_count(void) { return (int)(sizeof themes / sizeof themes[0]); }

const struct boot_theme *theme_at(int index) {
    if (index < 0 || index >= theme_count()) index = 0;
    return &themes[index];
}

static void trim(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t' || *a == '\r') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static int theme_index(const char *id) {
    for (int i = 0; i < theme_count(); i++)
        if (strcmp(themes[i].id, id) == 0) return i;
    return -1;
}

int theme_load(const char *card) {
    if (!card || !card[0]) return 0;
    char path[512];
    int wr = snprintf(path, sizeof path, "%s/autorun/boot-theme", card);
    if (wr < 0 || (size_t)wr >= sizeof path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    int truncated = strchr(line, '\n') == NULL && !feof(f);
    fclose(f);
    if (truncated) return 0;
    trim(line);
    int idx = theme_index(line);
    return idx < 0 ? 0 : idx;
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

int theme_save(const char *card, int index) {
    char path[512], tmp[512], line[64];
    if (!card || !card[0] || index < 0 || index >= theme_count()) return -1;
    int pw = snprintf(path, sizeof path, "%s/autorun/boot-theme", card);
    int tw = snprintf(tmp, sizeof tmp, "%s/autorun/boot-theme.tmp", card);
    if (pw < 0 || (size_t)pw >= sizeof path || tw < 0 || (size_t)tw >= sizeof tmp) return -1;
    int n = snprintf(line, sizeof line, "%s\n", themes[index].id);
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
