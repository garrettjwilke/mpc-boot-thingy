/* Stored and deflated zip reader for an autorun package.
   Rejects encryption, symlinks, zip64, and names that leave the package. */
#include "unzip.h"
#include "menu.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZIP_MAX_BYTES (64u * 1024u * 1024u)
#define ZIP_MAX_ENTRY (32u * 1024u * 1024u)
#define ZIP_MAX_TOTAL (64u * 1024u * 1024u)
#define ZIP_MAX_ENTRIES 4096
#define HUFF_BITS 15

static const char *err_msg = "could not store the package";

const char *unzip_error(void) {
    return err_msg;
}

static uint16_t ru16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t ru32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct huff {
    int count[HUFF_BITS + 1];
    int symbol[320];
};

struct bits {
    const unsigned char *p;
    const unsigned char *end;
    unsigned acc;
    int n;
};

static int bit_get(struct bits *b, int n) {
    if (n == 0) return 0;
    while (b->n < n) {
        if (b->p >= b->end) return -1;
        b->acc |= (unsigned)*b->p++ << b->n;
        b->n += 8;
    }
    int v = (int)(b->acc & ((1u << n) - 1));
    b->acc >>= n;
    b->n -= n;
    return v;
}

static void bit_align(struct bits *b) {
    int drop = b->n & 7;
    b->acc >>= drop;
    b->n -= drop;
}

static int huff_build(struct huff *h, const int *len, int n) {
    int offs[HUFF_BITS + 1];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) {
        if (len[i] < 0 || len[i] > HUFF_BITS) return -1;
        h->count[len[i]]++;
    }
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int bits = 1; bits <= HUFF_BITS; bits++) {
        left <<= 1;
        left -= h->count[bits];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int bits = 1; bits < HUFF_BITS; bits++) offs[bits + 1] = offs[bits] + h->count[bits];
    for (int i = 0; i < n; i++) {
        if (len[i] != 0) h->symbol[offs[len[i]]++] = i;
    }
    return 0;
}

static int huff_decode(struct huff *h, struct bits *b) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= HUFF_BITS; len++) {
        int bit = bit_get(b, 1);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const int len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const int dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const int cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static int inflate_codes(struct bits *b, struct huff *lit, struct huff *dist, unsigned char *out, size_t cap, size_t *used) {
    for (;;) {
        int sym = huff_decode(lit, b);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*used >= cap) return -1;
            out[(*used)++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256) return 0;
        if (sym > 285) return -1;
        int extra = bit_get(b, len_extra[sym - 257]);
        if (extra < 0) return -1;
        int length = len_base[sym - 257] + extra;
        int dsym = huff_decode(dist, b);
        if (dsym < 0 || dsym > 29) return -1;
        extra = bit_get(b, dist_extra[dsym]);
        if (extra < 0) return -1;
        int distance = dist_base[dsym] + extra;
        if (distance <= 0 || (size_t)distance > *used) return -1;
        if (*used + (size_t)length > cap) return -1;
        for (int i = 0; i < length; i++) {
            out[*used] = out[*used - (size_t)distance];
            (*used)++;
        }
    }
}

/* Raw deflate. out must hold exactly cap bytes. */
static int inflate_raw(const unsigned char *src, size_t n, unsigned char *out, size_t cap) {
    struct bits b = {.p = src, .end = src + n, .acc = 0, .n = 0};
    size_t used = 0;
    int bfinal;
    do {
        bfinal = bit_get(&b, 1);
        int btype = bit_get(&b, 2);
        if (bfinal < 0 || btype < 0) return -1;
        if (btype == 0) {
            bit_align(&b);
            int len = bit_get(&b, 16);
            int nlen = bit_get(&b, 16);
            if (len < 0 || nlen < 0 || (len ^ 0xffff) != nlen) return -1;
            if (used + (size_t)len > cap) return -1;
            for (int i = 0; i < len; i++) {
                int byte = bit_get(&b, 8);
                if (byte < 0) return -1;
                out[used++] = (unsigned char)byte;
            }
        } else if (btype == 1 || btype == 2) {
            struct huff lit, dist;
            if (btype == 1) {
                int lens[288];
                for (int i = 0; i < 144; i++) lens[i] = 8;
                for (int i = 144; i < 256; i++) lens[i] = 9;
                for (int i = 256; i < 280; i++) lens[i] = 7;
                for (int i = 280; i < 288; i++) lens[i] = 8;
                int dlens[32];
                for (int i = 0; i < 32; i++) dlens[i] = 5;
                if (huff_build(&lit, lens, 288) != 0 || huff_build(&dist, dlens, 32) != 0) return -1;
            } else {
                int hlit = bit_get(&b, 5);
                int hdist = bit_get(&b, 5);
                int hclen = bit_get(&b, 4);
                if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
                hlit += 257;
                hdist += 1;
                hclen += 4;
                if (hlit > 286 || hdist > 32) return -1;
                int clen[19];
                memset(clen, 0, sizeof clen);
                for (int i = 0; i < hclen; i++) {
                    int v = bit_get(&b, 3);
                    if (v < 0) return -1;
                    clen[cl_order[i]] = v;
                }
                struct huff cl;
                if (huff_build(&cl, clen, 19) != 0) return -1;
                int lens[320];
                memset(lens, 0, sizeof lens);
                int total = hlit + hdist;
                for (int i = 0; i < total;) {
                    int sym = huff_decode(&cl, &b);
                    if (sym < 0) return -1;
                    int repeat = 1, value = sym;
                    if (sym == 16) {
                        if (i == 0) return -1;
                        int extra = bit_get(&b, 2);
                        if (extra < 0) return -1;
                        repeat = 3 + extra;
                        value = lens[i - 1];
                    } else if (sym == 17) {
                        int extra = bit_get(&b, 3);
                        if (extra < 0) return -1;
                        repeat = 3 + extra;
                        value = 0;
                    } else if (sym == 18) {
                        int extra = bit_get(&b, 7);
                        if (extra < 0) return -1;
                        repeat = 11 + extra;
                        value = 0;
                    } else if (sym > 15) {
                        return -1;
                    }
                    if (i + repeat > total) return -1;
                    for (int k = 0; k < repeat; k++) lens[i++] = value;
                }
                if (huff_build(&lit, lens, hlit) != 0 || huff_build(&dist, lens + hlit, hdist) != 0) return -1;
            }
            if (inflate_codes(&b, &lit, &dist, out, cap, &used) != 0) return -1;
        } else {
            return -1;
        }
    } while (!bfinal);
    return used == cap ? 0 : -1;
}

static uint32_t crc32_of(const unsigned char *p, size_t n) {
    static uint32_t table[256];
    static int ready;
    uint32_t c = 0xffffffffu;
    size_t i;
    if (!ready) {
        for (unsigned k = 0; k < 256; k++) {
            unsigned x = k;
            int b;
            for (b = 0; b < 8; b++) x = (x & 1) ? (0xedb88320u ^ (x >> 1)) : (x >> 1);
            table[k] = x;
        }
        ready = 1;
    }
    for (i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static const unsigned char *find_eocd(const unsigned char *b, size_t n) {
    size_t max_back = 22u + 65535u;
    size_t begin, i;
    if (n < 22) return NULL;
    begin = n > max_back ? n - max_back : 0;
    i = n - 22;
    for (;;) {
        unsigned comment;
        if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 5 && b[i + 3] == 6) {
            comment = ru16(b + i + 20);
            if (i + 22u + comment == n) return b + i;
        }
        if (i == begin) break;
        i--;
    }
    return NULL;
}

struct cent {
    const unsigned char *name;
    uint16_t method, flags, name_len, extra_len, comment_len, disk;
    uint32_t crc, comp, uncomp, local, mode;
};

static int cent_next(const unsigned char *cd_end, const unsigned char **pp, struct cent *c) {
    const unsigned char *p = *pp;
    size_t tail;
    if (p == cd_end) return 1;
    if (p + 46 > cd_end) return -1;
    if (p[0] != 'P' || p[1] != 'K' || p[2] != 1 || p[3] != 2) return -1;
    c->flags = ru16(p + 8);
    c->method = ru16(p + 10);
    c->crc = ru32(p + 16);
    c->comp = ru32(p + 20);
    c->uncomp = ru32(p + 24);
    c->name_len = ru16(p + 28);
    c->extra_len = ru16(p + 30);
    c->comment_len = ru16(p + 32);
    c->disk = ru16(p + 34);
    c->mode = ru32(p + 38);
    c->local = ru32(p + 42);
    tail = 46u + (size_t)c->name_len + c->extra_len + c->comment_len;
    if (p + tail > cd_end) return -1;
    c->name = p + 46;
    *pp = p + tail;
    return 0;
}

static int copy_name(const struct cent *c, char *out, size_t n) {
    if (c->name_len == 0 || (size_t)c->name_len >= n) return -1;
    if (memchr(c->name, 0, c->name_len)) return -1;
    memcpy(out, c->name, c->name_len);
    out[c->name_len] = 0;
    return 0;
}

static int name_ok(const char *name) {
    const char *p;
    if (!name || !name[0] || name[0] == '/' || name[0] == '\\') return 0;
    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return 0;
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    p = name;
    while (*p) {
        const char *start = p;
        size_t len;
        if (*p == '/') return 0;
        while (*p && *p != '/') p++;
        len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') return 0;
        if (len == 2 && start[0] == '.' && start[1] == '.') return 0;
        if (*p == '/') {
            p++;
            if (!*p) break;
        }
    }
    return 1;
}

static int component_is(const char *name, const char *want) {
    const char *p = name;
    size_t n = strlen(want);
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        if ((size_t)(p - start) == n && memcmp(start, want, n) == 0) return 1;
        if (*p == '/') p++;
    }
    return 0;
}

static int under_prefix(const char *name, const char *prefix) {
    size_t n;
    if (!prefix[0]) return 1;
    n = strlen(prefix);
    return strncmp(name, prefix, n) == 0;
}

static int parent_of(const char *name, char *out, size_t n) {
    const char *slash = strrchr(name, '/');
    size_t len;
    if (!slash) {
        if (n == 0) return -1;
        out[0] = 0;
        return 0;
    }
    len = (size_t)(slash - name) + 1;
    if (len >= n) return -1;
    memcpy(out, name, len);
    out[len] = 0;
    return 0;
}

static int base_is(const char *name, const char *base) {
    const char *slash = strrchr(name, '/');
    const char *got = slash ? slash + 1 : name;
    return strcmp(got, base) == 0;
}

static int ensure_real_dir(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
        return 0;
    }
    if (mkdir(path, 0755) != 0) return -1;
    return 0;
}

/* Create directories under root for a relative path. root itself already exists.
   A symlink anywhere under root is refused. */
static int ensure_under(const char *root, const char *rel) {
    char buf[1024];
    char *p;
    size_t root_len;
    if (snprintf(buf, sizeof buf, "%s/%s", root, rel) >= (int)sizeof buf) return -1;
    root_len = strlen(root);
    p = buf + root_len + 1;
    for (; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (ensure_real_dir(buf) != 0) return -1;
        *p = '/';
    }
    return 0;
}

static int write_file(const char *root, const char *rel, const unsigned char *data, size_t n) {
    char path[1024];
    int fd, rc = 0;
    if (snprintf(path, sizeof path, "%s/%s", root, rel) >= (int)sizeof path) return -1;
    if (ensure_under(root, rel) != 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    while (n) {
        ssize_t w = write(fd, data, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        if (w == 0) {
            rc = -1;
            break;
        }
        data += w;
        n -= (size_t)w;
    }
    if (close(fd) != 0) rc = -1;
    if (rc != 0) unlink(path);
    return rc;
}

static int note_marker(char *saved, int *set, const char *prefix) {
    if (strlen(prefix) >= 512) return -1;
    if (!*set) {
        snprintf(saved, 512, "%s", prefix);
        *set = 1;
        return 0;
    }
    if (strcmp(saved, prefix) != 0) return -1;
    return 0;
}

int unzip_autorun(const char *zip_path, const char *app_dir, char *script_rel, size_t nout) {
    FILE *f = NULL;
    unsigned char *buf = NULL;
    long sz = 0;
    size_t n = 0;
    const unsigned char *eocd, *cd, *cd_end, *p;
    unsigned entries, seen;
    char json_prefix[512], sh_prefix[512], prefix[512], pkg[1024];
    int json_set = 0, sh_set = 0, sh_conflict = 0;
    int rc = -1;
    uint64_t total = 0;
    struct stat st;
    err_msg = "could not store the package";
    if (!zip_path || !app_dir || !app_dir[0] || !script_rel || nout == 0) goto out;
    if (lstat(app_dir, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) goto out;
    f = fopen(zip_path, "rb");
    if (!f) goto out;
    if (fseek(f, 0, SEEK_END) != 0) goto out;
    sz = ftell(f);
    if (sz < 22 || (unsigned long)sz > ZIP_MAX_BYTES) {
        err_msg = "zip is too large";
        goto out;
    }
    if (fseek(f, 0, SEEK_SET) != 0) goto out;
    n = (size_t)sz;
    buf = malloc(n ? n : 1);
    if (!buf || fread(buf, 1, n, f) != n) goto out;
    fclose(f);
    f = NULL;
    eocd = find_eocd(buf, n);
    if (!eocd) {
        err_msg = "zip is truncated";
        goto out;
    }
    if (ru16(eocd + 4) != 0 || ru16(eocd + 6) != 0) {
        err_msg = "zip method is not supported";
        goto out;
    }
    entries = ru16(eocd + 10);
    if (entries == 0 || entries > ZIP_MAX_ENTRIES || ru16(eocd + 8) != entries) {
        err_msg = "zip is not an autorun package";
        goto out;
    }
    {
        uint32_t cd_off = ru32(eocd + 16);
        uint32_t cd_size = ru32(eocd + 12);
        if (cd_off == 0xffffffffu || cd_size == 0xffffffffu || (size_t)cd_off + cd_size > n) {
            err_msg = "zip is too large";
            goto out;
        }
        cd = buf + cd_off;
        cd_end = cd + cd_size;
    }
    p = cd;
    seen = 0;
    while (seen < entries) {
        struct cent c;
        char name[512], parent[512];
        int cr = cent_next(cd_end, &p, &c);
        if (cr != 0) {
            err_msg = "zip is truncated";
            goto out;
        }
        seen++;
        if (c.disk != 0) {
            err_msg = "zip method is not supported";
            goto out;
        }
        if (copy_name(&c, name, sizeof name) != 0 || !name_ok(name)) {
            err_msg = "zip path leaves the package";
            goto out;
        }
        if (component_is(name, "__MACOSX")) continue;
        if (parent_of(name, parent, sizeof parent) != 0) {
            err_msg = "zip path leaves the package";
            goto out;
        }
        if (base_is(name, "autorun.json")) {
            if (note_marker(json_prefix, &json_set, parent) != 0) {
                err_msg = "zip has more than one autorun package";
                goto out;
            }
        } else if (base_is(name, "autorun.sh")) {
            if (!sh_set) {
                snprintf(sh_prefix, sizeof sh_prefix, "%s", parent);
                sh_set = 1;
            } else if (strcmp(sh_prefix, parent) != 0) {
                sh_conflict = 1;
            }
        }
    }
    if (p != cd_end) {
        err_msg = "zip is truncated";
        goto out;
    }
    if (json_set) snprintf(prefix, sizeof prefix, "%s", json_prefix);
    else if (sh_conflict) {
        err_msg = "zip has more than one autorun package";
        goto out;
    } else if (sh_set) snprintf(prefix, sizeof prefix, "%s", sh_prefix);
    else {
        err_msg = "zip is not an autorun package";
        goto out;
    }
    if (snprintf(pkg, sizeof pkg, "%s/autorun", app_dir) >= (int)sizeof pkg || ensure_real_dir(pkg) != 0) goto out;

    p = cd;
    seen = 0;
    while (seen < entries) {
        struct cent c;
        char name[512], rel[512], dest[1024];
        unsigned char scratch;
        unsigned char *out = NULL;
        int heap = 0;
        size_t data_off, local_name, local_extra;
        const unsigned char *local;
        int cr = cent_next(cd_end, &p, &c);
        if (cr != 0) goto out;
        seen++;
        if (copy_name(&c, name, sizeof name) != 0) goto out;
        if (component_is(name, "__MACOSX") || !under_prefix(name, prefix)) continue;
        if (strlen(name) < strlen(prefix)) continue;
        snprintf(rel, sizeof rel, "%s", name + strlen(prefix));
        if (!rel[0]) continue;
        if (rel[strlen(rel) - 1] == '/') {
            rel[strlen(rel) - 1] = 0;
            if (!rel[0]) continue;
            if (ensure_under(pkg, rel) != 0 ||
                snprintf(dest, sizeof dest, "%s/%s", pkg, rel) >= (int)sizeof dest ||
                ensure_real_dir(dest) != 0) {
                goto out;
            }
            continue;
        }
        if ((c.mode >> 16 & 0170000) == 0120000) {
            err_msg = "refusing a symlink";
            goto out;
        }
        if (c.flags & 1) {
            err_msg = "zip is encrypted";
            goto out;
        }
        if (c.comp == 0xffffffffu || c.uncomp == 0xffffffffu || c.uncomp > ZIP_MAX_ENTRY ||
            total + c.uncomp > ZIP_MAX_TOTAL) {
            err_msg = "zip is too large";
            goto out;
        }
        if (c.method == 8 && c.comp > 0 && c.uncomp > 256u * 1024u && c.uncomp / c.comp > 64u) {
            err_msg = "zip is too large";
            goto out;
        }
        if (c.method != 0 && c.method != 8) {
            err_msg = "zip method is not supported";
            goto out;
        }
        if (c.method == 0 && c.comp != c.uncomp) {
            err_msg = "zip is truncated";
            goto out;
        }
        if ((size_t)c.local + 30 > n || memcmp(buf + c.local, "PK\003\004", 4) != 0) {
            err_msg = "zip is truncated";
            goto out;
        }
        local = buf + c.local;
        local_name = ru16(local + 26);
        local_extra = ru16(local + 28);
        data_off = (size_t)c.local + 30u + local_name + local_extra;
        if (data_off > n || (size_t)c.comp > n - data_off) {
            err_msg = "zip is truncated";
            goto out;
        }
        total += c.uncomp;
        heap = c.uncomp > 0;
        out = heap ? malloc(c.uncomp) : &scratch;
        if (!out) goto out;
        if (c.method == 0) {
            if (c.uncomp) memcpy(out, buf + data_off, c.uncomp);
        } else if (inflate_raw(buf + data_off, c.comp, out, c.uncomp) != 0) {
            if (heap) free(out);
            err_msg = "zip is truncated";
            goto out;
        }
        if (crc32_of(heap ? out : (c.uncomp ? out : (const unsigned char *)""), c.uncomp) != c.crc) {
            if (heap) free(out);
            err_msg = "zip is truncated";
            goto out;
        }
        if (write_file(pkg, rel, out, c.uncomp) != 0) {
            if (heap) free(out);
            err_msg = "could not store the package";
            goto out;
        }
        if (heap) free(out);
    }
    if (menu_nested_run(app_dir, script_rel, nout) != 0) {
        err_msg = "autorun script is missing";
        goto out;
    }
    {
        char script[1024];
        if (snprintf(script, sizeof script, "%s/%s", app_dir, script_rel) >= (int)sizeof script ||
            chmod(script, 0755) != 0)
            goto out;
    }
    rc = 0;
out:
    if (f) fclose(f);
    free(buf);
    return rc;
}
