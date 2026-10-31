#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "zip_writer.h"
#include "file_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint32_t s_crc_table[256];
static int s_crc_ready = 0;

static void crc32_init(void) {
    if (s_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *buf, size_t len) {
    crc32_init();
    crc ^= 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc = s_crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

static void wu16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void wu32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void to_dos_time(time_t t, uint16_t *dos_time, uint16_t *dos_date) {
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        *dos_time = 0;
        *dos_date = 0x0021; // 1980-01-01
        return;
    }
    int yr = tm.tm_year + 1900;
    if (yr < 1980) yr = 1980;
    if (yr > 2107) yr = 2107;

    *dos_time = (uint16_t)(((tm.tm_hour & 0x1f) << 11) |
                           ((tm.tm_min & 0x3f) << 5) |
                           ((tm.tm_sec / 2) & 0x1f));
    *dos_date = (uint16_t)((((yr - 1980) & 0x7f) << 9) |
                           (((tm.tm_mon + 1) & 0x0f) << 5) |
                           (tm.tm_mday & 0x1f));
}

static int sock_write_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

struct zip_entry {
    char *name;
    uint32_t offset;
    uint32_t crc32;
    uint32_t csize;
    uint32_t usize;
    uint16_t dos_time;
    uint16_t dos_date;
    uint32_t ext_attr;
};

struct zip_state {
    int fd;
    uint32_t bytes_written;
    struct zip_entry *entries;
    size_t count;
    size_t cap;
};

static int state_add_entry(struct zip_state *zs, const char *name, uint32_t offset,
                           uint32_t crc, uint32_t sz, uint16_t dtime, uint16_t ddate, uint32_t ext_attr) {
    if (zs->count >= zs->cap) {
        size_t next_cap = zs->cap == 0 ? 64 : zs->cap * 2;
        struct zip_entry *next = realloc(zs->entries, next_cap * sizeof(struct zip_entry));
        if (!next) return -1;
        zs->entries = next;
        zs->cap = next_cap;
    }
    struct zip_entry *e = &zs->entries[zs->count++];
    e->name = strdup(name);
    if (!e->name) return -1;
    e->offset = offset;
    e->crc32 = crc;
    e->csize = sz;
    e->usize = sz;
    e->dos_time = dtime;
    e->dos_date = ddate;
    e->ext_attr = ext_attr;
    return 0;
}

static int zip_write_file(struct zip_state *zs, const char *fs_path, const char *arcname, const struct stat *st) {
    uint16_t dos_time, dos_date;
    to_dos_time(st->st_mtime, &dos_time, &dos_date);

    size_t name_len = strlen(arcname);
    if (name_len > 65535) return -1;

    uint32_t local_offset = zs->bytes_written;

    /* Write Local Header (30 bytes) */
    unsigned char lhdr[30];
    wu32(lhdr, 0x04034b50);
    wu16(lhdr + 4, 20);      // version needed
    wu16(lhdr + 6, 0x0008);  // flag: data descriptor follows
    wu16(lhdr + 8, 0);       // method: store
    wu16(lhdr + 10, dos_time);
    wu16(lhdr + 12, dos_date);
    wu32(lhdr + 14, 0);      // crc (in descriptor)
    wu32(lhdr + 18, 0);      // csize (in descriptor)
    wu32(lhdr + 22, 0);      // usize (in descriptor)
    wu16(lhdr + 26, (uint16_t)name_len);
    wu16(lhdr + 28, 0);      // extra len

    if (sock_write_full(zs->fd, lhdr, sizeof lhdr) != 0) return -1;
    if (sock_write_full(zs->fd, arcname, name_len) != 0) return -1;
    zs->bytes_written += (uint32_t)(sizeof(lhdr) + name_len);

    uint32_t crc = 0;
    uint32_t total_size = 0;

    int in_fd = open(fs_path, O_RDONLY);
    if (in_fd < 0) return -1;

    unsigned char buf[16384];
    ssize_t r;
    while ((r = read(in_fd, buf, sizeof buf)) > 0) {
        crc = crc32_update(crc, buf, (size_t)r);
        total_size += (uint32_t)r;
        if (sock_write_full(zs->fd, buf, (size_t)r) != 0) {
            close(in_fd);
            return -1;
        }
        zs->bytes_written += (uint32_t)r;
    }
    close(in_fd);
    if (r < 0) return -1;

    /* Write Data Descriptor (16 bytes) */
    unsigned char desc[16];
    wu32(desc, 0x08074b50);
    wu32(desc + 4, crc);
    wu32(desc + 8, total_size);
    wu32(desc + 12, total_size);

    if (sock_write_full(zs->fd, desc, sizeof desc) != 0) return -1;
    zs->bytes_written += sizeof(desc);

    uint32_t ext_attr = ((uint32_t)(st->st_mode & 0777) | 0100000u) << 16;
    return state_add_entry(zs, arcname, local_offset, crc, total_size, dos_time, dos_date, ext_attr);
}

static int zip_write_dir(struct zip_state *zs, const char *arcname, const struct stat *st) {
    uint16_t dos_time, dos_date;
    to_dos_time(st->st_mtime, &dos_time, &dos_date);

    char dir_name[PATH_MAX];
    size_t name_len = strlen(arcname);
    if (name_len == 0 || name_len >= sizeof(dir_name) - 2) return 0;

    memcpy(dir_name, arcname, name_len);
    if (dir_name[name_len - 1] != '/') {
        dir_name[name_len++] = '/';
    }
    dir_name[name_len] = '\0';

    uint32_t local_offset = zs->bytes_written;

    unsigned char lhdr[30];
    wu32(lhdr, 0x04034b50);
    wu16(lhdr + 4, 20);
    wu16(lhdr + 6, 0);       // no descriptor needed for empty dir
    wu16(lhdr + 8, 0);
    wu16(lhdr + 10, dos_time);
    wu16(lhdr + 12, dos_date);
    wu32(lhdr + 14, 0);
    wu32(lhdr + 18, 0);
    wu32(lhdr + 22, 0);
    wu16(lhdr + 26, (uint16_t)name_len);
    wu16(lhdr + 28, 0);

    if (sock_write_full(zs->fd, lhdr, sizeof lhdr) != 0) return -1;
    if (sock_write_full(zs->fd, dir_name, name_len) != 0) return -1;
    zs->bytes_written += (uint32_t)(sizeof(lhdr) + name_len);

    uint32_t ext_attr = ((uint32_t)(st->st_mode & 0777) | 0040000u) << 16;
    return state_add_entry(zs, dir_name, local_offset, 0, 0, dos_time, dos_date, ext_attr);
}

static int zip_add_tree(struct zip_state *zs, const char *fs_path, const char *arcname, int depth) {
    if (depth > 32) return -1;

    struct stat st;
    if (lstat(fs_path, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) return 0; // Skip symlinks

    if (S_ISREG(st.st_mode)) {
        return zip_write_file(zs, fs_path, arcname, &st);
    }

    if (S_ISDIR(st.st_mode)) {
        if (arcname && arcname[0]) {
            if (zip_write_dir(zs, arcname, &st) != 0) return -1;
        }

        DIR *d = opendir(fs_path);
        if (!d) return -1;

        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

            char child_fs[PATH_MAX];
            if (snprintf(child_fs, sizeof child_fs, "%s/%s", fs_path, de->d_name) >= (int)sizeof child_fs) {
                rc = -1;
                break;
            }

            char child_arc[PATH_MAX];
            if (arcname && arcname[0]) {
                if (snprintf(child_arc, sizeof child_arc, "%s/%s", arcname, de->d_name) >= (int)sizeof child_arc) {
                    rc = -1;
                    break;
                }
            } else {
                if (snprintf(child_arc, sizeof child_arc, "%s", de->d_name) >= (int)sizeof child_arc) {
                    rc = -1;
                    break;
                }
            }

            if (zip_add_tree(zs, child_fs, child_arc, depth + 1) != 0) {
                rc = -1;
                break;
            }
        }
        closedir(d);
        return rc;
    }
    return 0;
}

int zip_stream_export(int client_fd, const char **items, size_t count) {
    /* Send HTTP 200 Header */
    char hdr[256];
    int hlen = snprintf(hdr, sizeof hdr,
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/zip\r\n"
                        "Content-Disposition: attachment; filename=\"mpc-data-backup.zip\"\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n"
                        "\r\n");
    if (sock_write_full(client_fd, hdr, (size_t)hlen) != 0) return -1;

    struct zip_state zs;
    memset(&zs, 0, sizeof zs);
    zs.fd = client_fd;

    const char *root = file_ops_get_root();

    int rc = 0;
    if (count == 0 || items == NULL) {
        /* Export the entire data directory */
        rc = zip_add_tree(&zs, root, "", 0);
    } else {
        /* Export selected items */
        for (size_t i = 0; i < count; i++) {
            if (!items[i] || !items[i][0]) continue;
            char real_path[PATH_MAX], rel_path[PATH_MAX];
            if (file_ops_resolve(items[i], real_path, sizeof real_path, rel_path, sizeof rel_path) != 0) {
                continue;
            }
            if (rel_path[0] == '\0') {
                /* Whole directory requested */
                rc = zip_add_tree(&zs, root, "", 0);
                break;
            }
            const char *base = strrchr(rel_path, '/');
            base = base ? base + 1 : rel_path;
            if (zip_add_tree(&zs, real_path, base, 0) != 0) {
                rc = -1;
                break;
            }
        }
    }

    if (rc != 0) goto cleanup;

    /* Write Central Directory */
    uint32_t cd_offset = zs.bytes_written;
    for (size_t i = 0; i < zs.count; i++) {
        struct zip_entry *e = &zs.entries[i];
        size_t nlen = strlen(e->name);

        unsigned char chdr[46];
        wu32(chdr, 0x02014b50);
        wu16(chdr + 4, 0x0314); // Unix, zip 2.0
        wu16(chdr + 6, 20);     // version needed
        wu16(chdr + 8, (uint16_t)((e->ext_attr & (0040000u << 16)) ? 0 : 0x0008));
        wu16(chdr + 10, 0);     // method: store
        wu16(chdr + 12, e->dos_time);
        wu16(chdr + 14, e->dos_date);
        wu32(chdr + 16, e->crc32);
        wu32(chdr + 20, e->csize);
        wu32(chdr + 24, e->usize);
        wu16(chdr + 28, (uint16_t)nlen);
        wu16(chdr + 30, 0);     // extra len
        wu16(chdr + 32, 0);     // comm len
        wu16(chdr + 34, 0);     // disk num
        wu16(chdr + 36, 0);     // int attr
        wu32(chdr + 38, e->ext_attr);
        wu32(chdr + 42, e->offset);

        if (sock_write_full(client_fd, chdr, sizeof chdr) != 0) {
            rc = -1;
            goto cleanup;
        }
        if (sock_write_full(client_fd, e->name, nlen) != 0) {
            rc = -1;
            goto cleanup;
        }
        zs.bytes_written += (uint32_t)(sizeof(chdr) + nlen);
    }
    uint32_t cd_size = zs.bytes_written - cd_offset;

    /* Write End of Central Directory (22 bytes) */
    unsigned char eocd[22];
    wu32(eocd, 0x06054b50);
    wu16(eocd + 4, 0);
    wu16(eocd + 6, 0);
    wu16(eocd + 8, (uint16_t)(zs.count > 0xffff ? 0xffff : zs.count));
    wu16(eocd + 10, (uint16_t)(zs.count > 0xffff ? 0xffff : zs.count));
    wu32(eocd + 12, cd_size);
    wu32(eocd + 16, cd_offset);
    wu16(eocd + 20, 0);

    if (sock_write_full(client_fd, eocd, sizeof eocd) != 0) {
        rc = -1;
    }

cleanup:
    for (size_t i = 0; i < zs.count; i++) {
        free(zs.entries[i].name);
    }
    free(zs.entries);
    return rc;
}
