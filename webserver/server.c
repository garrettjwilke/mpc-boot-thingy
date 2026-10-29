/* Web page for mpc-boot-thingy. Serves the card's boot menu and a shell. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "boot_edit.h"
#include "menu.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_UPLOAD (64u * 1024u * 1024u)
#define WS_MAX (256u * 1024u)
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static const char BOOTCOUNT[] =
    "/sys/devices/platform/fdc20000.syscon/fdc20000.syscon:bootcount/bootcount";

static char g_autorun[PATH_MAX];
static char g_card[PATH_MAX];
static char g_bind[64];
static int g_port;

struct io {
    int fd;
    unsigned char buf[16384];
    size_t len;
    size_t pos;
};

struct sha1 {
    uint32_t h[5];
    uint64_t nbits;
    unsigned char buf[64];
    size_t n;
};

static void reap(int sig) {
    int saved = errno;
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved;
}

static uint32_t rol(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_init(struct sha1 *s) {
    s->h[0] = 0x67452301;
    s->h[1] = 0xEFCDAB89;
    s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
    s->nbits = 0;
    s->n = 0;
}

static void sha1_block(struct sha1 *s, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_update(struct sha1 *s, const void *data, size_t len) {
    const unsigned char *p = data;
    s->nbits += (uint64_t)len * 8;
    while (len) {
        size_t room = 64 - s->n;
        size_t take = len < room ? len : room;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        len -= take;
        if (s->n == 64) {
            sha1_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha1_final(struct sha1 *s, unsigned char out[20]) {
    size_t n = s->n;
    s->buf[n++] = 0x80;
    if (n > 56) {
        while (n < 64) s->buf[n++] = 0;
        sha1_block(s, s->buf);
        n = 0;
    }
    while (n < 56) s->buf[n++] = 0;
    uint64_t bits = s->nbits;
    for (int i = 0; i < 8; i++) s->buf[56 + i] = (unsigned char)((bits >> (56 - 8 * i)) & 0xff);
    sha1_block(s, s->buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s->h[i];
    }
}

static void b64_encode(const unsigned char *in, size_t n, char *out) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i + 3 <= n) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[j++] = tab[(v >> 18) & 63];
        out[j++] = tab[(v >> 12) & 63];
        out[j++] = tab[(v >> 6) & 63];
        out[j++] = tab[v & 63];
        i += 3;
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        out[j++] = tab[(v >> 18) & 63];
        out[j++] = tab[(v >> 12) & 63];
        if (i + 1 < n) {
            out[j++] = tab[(v >> 6) & 63];
            out[j++] = '=';
        } else {
            out[j++] = '=';
            out[j++] = '=';
        }
    }
    out[j] = 0;
}

static void ws_accept(const char *key, char *out, size_t out_n) {
    struct sha1 s;
    unsigned char dig[20];
    char b64[64];
    sha1_init(&s);
    sha1_update(&s, key, strlen(key));
    sha1_update(&s, WS_GUID, sizeof WS_GUID - 1);
    sha1_final(&s, dig);
    b64_encode(dig, 20, b64);
    snprintf(out, out_n, "%s", b64);
}

static int dig_is(const unsigned char *d, const char *hex) {
    for (int i = 0; i < 20; i++) {
        unsigned int v = 0;
        if (sscanf(hex + (size_t)i * 2, "%2x", &v) != 1 || d[i] != (unsigned char)v) return 0;
    }
    return 1;
}

static int runnable_program(const unsigned char *buf, size_t n);

static int selftest(void) {
    unsigned char d[20];
    struct sha1 s;
    char acc[64];
    sha1_init(&s);
    sha1_final(&s, d);
    if (!dig_is(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709")) {
        fprintf(stderr, "sha1 empty mismatch\n");
        return 1;
    }
    sha1_init(&s);
    sha1_update(&s, "abc", 3);
    sha1_final(&s, d);
    if (!dig_is(d, "a9993e364706816aba3e25717850c26c9cd0d89d")) {
        fprintf(stderr, "sha1 abc mismatch\n");
        return 1;
    }
    ws_accept("dGhlIHNhbXBsZSBub25jZQ==", acc, sizeof acc);
    if (strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != 0) {
        fprintf(stderr, "ws accept mismatch: %s\n", acc);
        return 1;
    }
    {
        unsigned char elf[64];
        static const char sh[] = "#!/bin/sh\necho hi\n";
        static const char envbash[] = "#!/usr/bin/env bash\n";
        static const char bashflag[] = "#!/bin/bash -e\n";
        static const char python[] = "#!/usr/bin/python3\n";
        static const char noshebang[] = "echo hi\n";
        static const char crlf[] = "#!/bin/sh\r\n";
        memset(elf, 0, sizeof elf);
        elf[0] = 0x7f;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 2;
        elf[5] = 1;
        elf[6] = 1;
        elf[16] = 2;
        elf[18] = 0xb7;
        elf[24] = 0x40;
        if (!runnable_program(elf, sizeof elf)) {
            fprintf(stderr, "aarch64 exec rejected\n");
            return 1;
        }
        elf[16] = 3;
        if (!runnable_program(elf, sizeof elf)) {
            fprintf(stderr, "aarch64 pie rejected\n");
            return 1;
        }
        elf[24] = 0;
        if (runnable_program(elf, sizeof elf)) {
            fprintf(stderr, "aarch64 library accepted\n");
            return 1;
        }
        elf[16] = 2;
        elf[18] = 62;
        elf[24] = 0x40;
        if (runnable_program(elf, sizeof elf)) {
            fprintf(stderr, "x86_64 accepted\n");
            return 1;
        }
        elf[4] = 1;
        elf[18] = 40;
        elf[19] = 0;
        if (runnable_program(elf, sizeof elf)) {
            fprintf(stderr, "arm32 accepted\n");
            return 1;
        }
        if (!runnable_program((const unsigned char *)sh, sizeof sh - 1) ||
            !runnable_program((const unsigned char *)envbash, sizeof envbash - 1) ||
            !runnable_program((const unsigned char *)bashflag, sizeof bashflag - 1)) {
            fprintf(stderr, "shell script rejected\n");
            return 1;
        }
        if (runnable_program((const unsigned char *)python, sizeof python - 1) ||
            runnable_program((const unsigned char *)noshebang, sizeof noshebang - 1) ||
            runnable_program((const unsigned char *)crlf, sizeof crlf - 1) ||
            runnable_program((const unsigned char *)"", 0)) {
            fprintf(stderr, "bad script accepted\n");
            return 1;
        }
    }
    puts("web-check ok");
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int io_read(struct io *io, void *dst, size_t n) {
    unsigned char *out = dst;
    while (n) {
        if (io->pos >= io->len) {
            ssize_t r;
            io->pos = 0;
            io->len = 0;
            do {
                r = read(io->fd, io->buf, sizeof io->buf);
            } while (r < 0 && errno == EINTR);
            if (r <= 0) return -1;
            io->len = (size_t)r;
        }
        size_t have = io->len - io->pos;
        size_t take = have < n ? have : n;
        memcpy(out, io->buf + io->pos, take);
        io->pos += take;
        out += take;
        n -= take;
    }
    return 0;
}

static void send_body(int fd, int status, const char *text, const char *ctype, const void *body, size_t len) {
    char hdr[384];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n"
                     "\r\n",
                     status, text, ctype, len);
    if (n < 0 || (size_t)n >= sizeof hdr) return;
    if (write_full(fd, hdr, (size_t)n) != 0) return;
    if (len) write_full(fd, body, len);
}

static void send_json(int fd, int status, const char *text, const char *json) {
    send_body(fd, status, text, "application/json; charset=utf-8", json, strlen(json));
}

static void send_err(int fd, int status, const char *text, const char *msg) {
    char buf[512];
    char esc[256];
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)msg; *p && j + 2 < sizeof esc; p++) {
        if (*p == '"' || *p == '\\') esc[j++] = '\\';
        if (*p >= 0x20) esc[j++] = (char)*p;
    }
    esc[j] = 0;
    snprintf(buf, sizeof buf, "{\"error\":\"%s\"}\n", esc);
    send_json(fd, status, text, buf);
}

static int serve_file(int fd, const char *path, const char *ctype) {
    int f = open(path, O_RDONLY);
    struct stat st;
    char hdr[384];
    char buf[8192];
    unsigned long long left;
    int n;
    if (f < 0) return -1;
    if (fstat(f, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(f);
        return -1;
    }
    n = snprintf(hdr, sizeof hdr,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %llu\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-store\r\n"
                 "\r\n",
                 ctype, (unsigned long long)st.st_size);
    if (n < 0 || (size_t)n >= sizeof hdr || write_full(fd, hdr, (size_t)n) != 0) {
        close(f);
        return 0;
    }
    left = (unsigned long long)st.st_size;
    while (left) {
        size_t chunk = left < sizeof buf ? (size_t)left : sizeof buf;
        ssize_t r = read(f, buf, chunk);
        if (r <= 0) break;
        if (write_full(fd, buf, (size_t)r) != 0) break;
        left -= (unsigned long long)r;
    }
    close(f);
    return 0;
}

static void url_decode(const char *src, size_t src_n, char *dst, size_t dst_sz) {
    size_t i = 0, j = 0;
    while (i < src_n && j + 1 < dst_sz) {
        if (src[i] == '%' && i + 2 < src_n && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = 0;
}

static void query_param(const char *query, const char *key, char *out, size_t out_sz) {
    size_t klen;
    const char *p;
    out[0] = 0;
    if (!query || !key) return;
    klen = strlen(key);
    p = query;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *end;
            size_t vlen;
            p += klen + 1;
            end = strchr(p, '&');
            vlen = end ? (size_t)(end - p) : strlen(p);
            url_decode(p, vlen, out, out_sz);
            return;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
}

static int header_value(const char *headers, const char *key, char *out, size_t out_sz) {
    size_t klen = strlen(key);
    const char *p = headers;
    out[0] = 0;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        int match = linelen > klen && p[klen] == ':';
        if (match) {
            for (size_t i = 0; i < klen; i++) {
                if (tolower((unsigned char)p[i]) != tolower((unsigned char)key[i])) {
                    match = 0;
                    break;
                }
            }
        }
        if (match) {
            const char *v = p + klen + 1;
            const char *end = p + linelen;
            size_t n;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            n = (size_t)(end - v);
            if (n >= out_sz) n = out_sz - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return 0;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return -1;
}

static int contains_ci(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (!nlen || !hay) return 0;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return 1;
    }
    return 0;
}

static void trim_inplace(char *s) {
    char *a = s;
    size_t n;
    while (*a == ' ' || *a == '\t') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static int origin_ok(const char *origin) {
    char expect[96];
    int n = snprintf(expect, sizeof expect, "http://%s:%d", g_bind, g_port);
    if (n < 0 || (size_t)n >= sizeof expect || !origin) return 0;
    return strcmp(origin, expect) == 0;
}

static void json_escape_print(FILE *fp, const char *s) {
    fputc('"', fp);
    if (!s) s = "";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') fputs("\\\"", fp);
        else if (c == '\\') fputs("\\\\", fp);
        else if (c == '\b') fputs("\\b", fp);
        else if (c == '\f') fputs("\\f", fp);
        else if (c == '\n') fputs("\\n", fp);
        else if (c == '\r') fputs("\\r", fp);
        else if (c == '\t') fputs("\\t", fp);
        else if (c < 0x20) fprintf(fp, "\\u%04x", c);
        else fputc(c, fp);
    }
    fputc('"', fp);
}

static void json_csv(FILE *fp, const char *list) {
    char buf[MENU_PATCHES];
    char *p;
    int first = 1;
    fputc('[', fp);
    if (list && list[0] && strlen(list) < sizeof buf) {
        snprintf(buf, sizeof buf, "%s", list);
        p = buf;
        while (*p) {
            char *comma = strchr(p, ',');
            if (comma) *comma = 0;
            if (*p) {
                if (!first) fputc(',', fp);
                first = 0;
                json_escape_print(fp, p);
            }
            if (!comma) break;
            p = comma + 1;
        }
    }
    fputc(']', fp);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static char *run_capture(char *const argv[]) {
    int fds[2];
    pid_t pid;
    size_t cap = 256, len = 0;
    char *buf;
    if (pipe(fds) != 0) return strdup("");
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return strdup("");
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        write(STDOUT_FILENO, "unavailable\n", 12);
        _exit(127);
    }
    close(fds[1]);
    buf = malloc(cap);
    if (!buf) {
        close(fds[0]);
        return strdup("");
    }
    for (;;) {
        ssize_t r;
        if (len + 1 >= cap) {
            char *nbuf;
            if (cap >= 16384) {
                kill(pid, SIGKILL);
                break;
            }
            cap *= 2;
            nbuf = realloc(buf, cap);
            if (!nbuf) {
                kill(pid, SIGKILL);
                break;
            }
            buf = nbuf;
        }
        r = read(fds[0], buf + len, cap - len - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = 0;
    close(fds[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    return buf;
}

static int find_sep(const char *buf, size_t n, size_t *at) {
    if (n < 4) return 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *at = i;
            return 1;
        }
    }
    return 0;
}

static int reload_boot_thingy(void);

static void card_of(const char *autorun, char *out, size_t n) {
    snprintf(out, n, "%s", autorun ? autorun : "");
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;

    char *ar = NULL;
    for (char *p = out; (p = strstr(p, "/autorun")) != NULL; p += 8) {
        if (p[8] == 0 || p[8] == '/') ar = p;
    }
    if (ar) {
        if (ar == out) out[1] = 0;
        else *ar = 0;
        return;
    }

    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, n, ".");
        return;
    }
    if (slash == out) out[1] = 0;
    else *slash = 0;
}

static int one_line(const char *s) {
    if (!s || !s[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return 0;
    }
    return 1;
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

static int slug_of(const char *title, char *out, size_t n) {
    size_t j = 0;
    int dash = 1;
    for (const unsigned char *p = (const unsigned char *)title; *p; p++) {
        unsigned char c = *p;
        int alnum;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (j + 1 >= n) return -1;
            out[j++] = (char)c;
            dash = 0;
        } else if (!dash && j + 1 < n) {
            out[j++] = '-';
            dash = 1;
        }
    }
    if (j && out[j - 1] == '-') j--;
    out[j] = 0;
    if (!j || j > 64) return -1;
    return 0;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
        return 0;
    }
    return mkdir(path, 0755);
}

static int path_inside(const char *root_real, const char *resolved) {
    size_t n = strlen(root_real);
    if (strncmp(resolved, root_real, n) != 0) return 0;
    return resolved[n] == '/';
}

static const char *web_type(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js") || !strcmp(dot, ".mjs")) return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif")) return "image/gif";
    if (!strcmp(dot, ".webp")) return "image/webp";
    if (!strcmp(dot, ".ico")) return "image/x-icon";
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".map") || !strcmp(dot, ".LICENSE"))
        return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".woff")) return "font/woff";
    if (!strcmp(dot, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

/* URL path under the card's web directory. "/" is index.html. */
static int web_rel(const char *url_path, char *rel, size_t n) {
    char decoded[1024];
    const char *p;
    size_t i;
    if (!url_path || url_path[0] != '/') return -1;
    url_decode(url_path, strlen(url_path), decoded, sizeof decoded);
    if (strcmp(decoded, "/") == 0) {
        if (n < 11) return -1;
        memcpy(rel, "index.html", 11);
        return 0;
    }
    p = decoded + 1;
    if (!p[0]) return -1;
    for (i = 0; p[i]; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7f || c == '\\') return -1;
    }
    if (p[0] == '/' || strstr(p, "..") || strstr(p, "//")) return -1;
    {
        const char *s = p;
        while (*s) {
            const char *slash = strchr(s, '/');
            size_t len = slash ? (size_t)(slash - s) : strlen(s);
            if (len == 0 || (len == 1 && s[0] == '.')) return -1;
            if (!slash) break;
            s = slash + 1;
            if (!*s) return -1;
        }
    }
    if (strlen(p) + 1 > n) return -1;
    memcpy(rel, p, strlen(p) + 1);
    return 0;
}

static int serve_web(int client, const char *url_path) {
    char rel[1024];
    char root[PATH_MAX], full[PATH_MAX], root_real[PATH_MAX], resolved[PATH_MAX];
    struct stat st;
    if (web_rel(url_path, rel, sizeof rel) != 0) return -1;
    if (snprintf(root, sizeof root, "%s/system/web", g_autorun) >= (int)sizeof root ||
        snprintf(full, sizeof full, "%s/system/web/%s", g_autorun, rel) >= (int)sizeof full) {
        return -1;
    }
    if (!realpath(root, root_real)) return -1;
    if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return -1;
    if (!realpath(full, resolved) || !path_inside(root_real, resolved)) return -1;
    return serve_file(client, resolved, web_type(rel));
}

static int is_one_child(const char *root_real, const char *resolved) {
    size_t n = strlen(root_real);
    const char *rest;
    if (strncmp(resolved, root_real, n) != 0 || resolved[n] != '/') return 0;
    rest = resolved + n + 1;
    return rest[0] && strchr(rest, '/') == NULL;
}

static int rm_tree(const char *path, int depth) {
    struct stat st;
    DIR *d;
    struct dirent *de;
    if (depth > 16 || lstat(path, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return unlink(path);
    d = opendir(path);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char child[PATH_MAX];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child) {
            closedir(d);
            return -1;
        }
        if (rm_tree(child, depth + 1) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path);
}

static void entry_run(const char *path, char *out, size_t n) {
    FILE *f;
    char line[512];
    out[0] = 0;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *v;
        size_t L;
        if (strncmp(line, "run=", 4) != 0) continue;
        v = line + 4;
        L = strlen(v);
        while (L && (v[L - 1] == '\n' || v[L - 1] == '\r' || v[L - 1] == ' ')) v[--L] = 0;
        snprintf(out, n, "%s", v);
        break;
    }
    fclose(f);
}

static void handle_status(int fd) {
    struct menu menu;
    int def;
    char *uname_s, *uptime_s, *mem_s;
    char *uarg[] = {"uname", "-srn", NULL};
    char *aarg[] = {"uptime", NULL};
    char *marg[] = {"free", "-m", NULL};
    struct statvfs vfs;
    uint64_t total = 0, freeb = 0;
    char *buf = NULL;
    size_t sz = 0;
    FILE *fp;
    menu_load(&menu, g_card);
    def = menu_default_index(&menu, g_card);
    uname_s = run_capture(uarg);
    uptime_s = run_capture(aarg);
    mem_s = run_capture(marg);
    rstrip(uname_s);
    rstrip(uptime_s);
    if (statvfs(g_card, &vfs) == 0) {
        total = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
        freeb = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    }
    fp = open_memstream(&buf, &sz);
    if (!fp) {
        send_err(fd, 500, "Internal Server Error", "could not build status");
        free(uname_s);
        free(uptime_s);
        free(mem_s);
        return;
    }
    fputs("{\"uname\":", fp);
    json_escape_print(fp, uname_s);
    fputs(",\"uptime\":", fp);
    json_escape_print(fp, uptime_s);
    fputs(",\"memory\":", fp);
    json_escape_print(fp, mem_s);
    fputs(",\"card\":", fp);
    json_escape_print(fp, g_card);
    fprintf(fp, ",\"sd_total_bytes\":%" PRIu64 ",\"sd_free_bytes\":%" PRIu64, total, freeb);
    fputs(",\"default_id\":", fp);
    json_escape_print(fp, menu.items[def].id);
    fputs(",\"apps\":[", fp);
    for (int i = 0; i < menu.n; i++) {
        if (i) fputc(',', fp);
        fputs("{\"id\":", fp);
        json_escape_print(fp, menu.items[i].id);
        fputs(",\"title\":", fp);
        json_escape_print(fp, menu.items[i].title);
        fputs(",\"run\":", fp);
        json_escape_print(fp, menu.items[i].run);
        fputs(",\"args\":", fp);
        json_escape_print(fp, menu.items[i].args);
        fprintf(fp, ",\"stock\":%s,\"editable\":%s,\"patches\":", menu.items[i].stock ? "true" : "false",
                menu.items[i].boot_ac50 ? "true" : "false");
        json_csv(fp, menu.items[i].patches);
        fputc('}', fp);
    }
    fputs("]}\n", fp);
    fclose(fp);
    send_json(fd, 200, "OK", buf);
    free(buf);
    free(uname_s);
    free(uptime_s);
    free(mem_s);
}

static int sh_name(const unsigned char *s, size_t n) {
    return (n == 2 && s[0] == 's' && s[1] == 'h') || (n == 4 && memcmp(s, "bash", 4) == 0);
}

/* The boot menu runs the file with exec. A script runs only when its first line names sh or bash. */
static int shell_script(const unsigned char *buf, size_t n) {
    size_t i = 2, start, end, path_end, base, arg;
    if (n < 3 || buf[0] != '#' || buf[1] != '!') return 0;
    if (buf[2] == ' ') i = 3;
    start = i;
    while (i < n && buf[i] != '\n') {
        if (buf[i] == '\r' || buf[i] < 0x20 || buf[i] > 0x7e) return 0;
        if (i - start > 128) return 0;
        i++;
    }
    end = i;
    while (start < end && buf[start] == ' ') start++;
    path_end = start;
    while (path_end < end && buf[path_end] != ' ') path_end++;
    if (path_end == start) return 0;
    base = path_end;
    while (base > start && buf[base - 1] != '/') base--;
    if (sh_name(buf + base, path_end - base)) return 1;
    if (path_end - base == 3 && memcmp(buf + base, "env", 3) == 0) {
        arg = path_end;
        while (arg < end && buf[arg] == ' ') arg++;
        base = arg;
        while (arg < end && buf[arg] != ' ') arg++;
        return sh_name(buf + base, arg - base);
    }
    return 0;
}

/* ELF64 little-endian AArch64 executable or PIE. A shared library has no entry point. */
static int aarch64_exec(const unsigned char *b, size_t n) {
    unsigned type, machine;
    uint64_t entry = 0;
    int i;
    if (n < 32) return 0;
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return 0;
    if (b[4] != 2 || b[5] != 1 || b[6] != 1) return 0;
    type = (unsigned)b[16] | ((unsigned)b[17] << 8);
    machine = (unsigned)b[18] | ((unsigned)b[19] << 8);
    if (machine != 183 || (type != 2 && type != 3)) return 0;
    for (i = 0; i < 8; i++) entry |= (uint64_t)b[24 + i] << (8 * i);
    return entry != 0;
}

static int runnable_program(const unsigned char *buf, size_t n) {
    return aarch64_exec(buf, n) || shell_script(buf, n);
}

static void handle_upload(int fd, struct io *io, int has_len, size_t content_len, const char *query) {
    char title[512], args[512], name[256], slug[80];
    char dir[PATH_MAX], apps[PATH_MAX], part[PATH_MAX], dest[PATH_MAX], entry[PATH_MAX];
    char old[MENU_RUN], body[640], resp[160];
    int partfd = -1, entryfd = -1;
    int bn;
    if (!has_len || content_len == 0) {
        send_err(fd, 400, "Bad Request", "program body is empty");
        return;
    }
    query_param(query, "title", title, sizeof title);
    query_param(query, "args", args, sizeof args);
    query_param(query, "name", name, sizeof name);
    trim_inplace(title);
    trim_inplace(args);
    trim_inplace(name);
    if (!one_line(title) || strlen(title) >= MENU_TITLE) {
        send_err(fd, 400, "Bad Request", "title must be one short line");
        return;
    }
    if (args[0] && (!one_line(args) || strlen(args) >= MENU_ARGS)) {
        send_err(fd, 400, "Bad Request", "args must be one short line");
        return;
    }
    if (slug_of(title, slug, sizeof slug) != 0) {
        send_err(fd, 400, "Bad Request", "could not name the app folder");
        return;
    }
    if (!name[0]) snprintf(name, sizeof name, "%s", slug);
    if (!safe_component(name) || strcmp(name, "boot.entry") == 0 || strlen(name) >= MENU_RUN) {
        send_err(fd, 400, "Bad Request", "executable name is not allowed");
        return;
    }
    if (snprintf(apps, sizeof apps, "%s/autorun/apps", g_card) >= (int)sizeof apps ||
        snprintf(dir, sizeof dir, "%s/autorun/apps/%s", g_card, slug) >= (int)sizeof dir ||
        snprintf(part, sizeof part, "%s/autorun/apps/%s/.upload", g_card, slug) >= (int)sizeof part ||
        snprintf(dest, sizeof dest, "%s/autorun/apps/%s/%s", g_card, slug, name) >= (int)sizeof dest ||
        snprintf(entry, sizeof entry, "%s/autorun/apps/%s/boot.entry", g_card, slug) >= (int)sizeof entry) {
        send_err(fd, 400, "Bad Request", "path is too long");
        return;
    }
    if (ensure_dir(apps) != 0 || ensure_dir(dir) != 0) {
        send_err(fd, 500, "Internal Server Error", "could not create the app folder");
        return;
    }
    entry_run(entry, old, sizeof old);
    partfd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (partfd < 0) {
        send_err(fd, 500, "Internal Server Error", "could not store the program");
        return;
    }
    {
        size_t left = content_len;
        char buf[8192];
        while (left) {
            size_t chunk = left < sizeof buf ? left : sizeof buf;
            if (io_read(io, buf, chunk) != 0 || write_full(partfd, buf, chunk) != 0) {
                close(partfd);
                unlink(part);
                send_err(fd, 400, "Bad Request", "upload ended early");
                return;
            }
            left -= chunk;
        }
    }
    {
        unsigned char magic[512];
        ssize_t got;
        if (lseek(partfd, 0, SEEK_SET) < 0) {
            close(partfd);
            unlink(part);
            send_err(fd, 500, "Internal Server Error", "could not store the program");
            return;
        }
        got = read(partfd, magic, sizeof magic);
        if (got < 0 || !runnable_program(magic, (size_t)got)) {
            close(partfd);
            unlink(part);
            send_err(fd, 400, "Bad Request",
                     "program must be an aarch64 Linux executable or a sh/bash script");
            return;
        }
    }
    if (fchmod(partfd, 0755) != 0) {
        close(partfd);
        unlink(part);
        send_err(fd, 500, "Internal Server Error", "could not store the program");
        return;
    }
    if (close(partfd) != 0) {
        unlink(part);
        send_err(fd, 500, "Internal Server Error", "could not store the program");
        return;
    }
    if (rename(part, dest) != 0) {
        unlink(part);
        send_err(fd, 500, "Internal Server Error", "could not store the program");
        return;
    }
    if (args[0]) bn = snprintf(body, sizeof body, "title=%s\nrun=%s\nargs=%s\n", title, name, args);
    else bn = snprintf(body, sizeof body, "title=%s\nrun=%s\n", title, name);
    if (bn < 0 || (size_t)bn >= sizeof body) {
        send_err(fd, 500, "Internal Server Error", "could not write boot.entry");
        return;
    }
    entryfd = open(entry, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (entryfd < 0 || write_full(entryfd, body, (size_t)bn) != 0 || close(entryfd) != 0) {
        if (entryfd >= 0) close(entryfd);
        send_err(fd, 500, "Internal Server Error", "could not write boot.entry");
        return;
    }
    if (old[0] && strcmp(old, name) != 0 && safe_component(old)) {
        char oldp[PATH_MAX];
        struct stat st;
        if (snprintf(oldp, sizeof oldp, "%s/%s", dir, old) < (int)sizeof oldp &&
            lstat(oldp, &st) == 0 && S_ISREG(st.st_mode)) {
            unlink(oldp);
        }
    }
    reload_boot_thingy();
    snprintf(resp, sizeof resp, "{\"ok\":true,\"id\":\"app:%s\"}\n", slug);
    send_json(fd, 200, "OK", resp);
}

static void handle_delete(int fd, const char *query) {
    char id[MENU_ID];
    char root[PATH_MAX], full[PATH_MAX], root_real[PATH_MAX], resolved[PATH_MAX];
    struct stat st;
    const char *name;
    int app;
    query_param(query, "id", id, sizeof id);
    if (strncmp(id, "app:", 4) == 0) {
        app = 1;
        name = id + 4;
        if (snprintf(root, sizeof root, "%s/autorun/apps", g_card) >= (int)sizeof root ||
            snprintf(full, sizeof full, "%s/autorun/apps/%s", g_card, name) >= (int)sizeof full) {
            send_err(fd, 400, "Bad Request", "path is too long");
            return;
        }
    } else if (strncmp(id, "entry:", 6) == 0) {
        app = 0;
        name = id + 6;
        if (snprintf(root, sizeof root, "%s/autorun/boot-menu", g_card) >= (int)sizeof root ||
            snprintf(full, sizeof full, "%s/autorun/boot-menu/%s", g_card, name) >= (int)sizeof full) {
            send_err(fd, 400, "Bad Request", "path is too long");
            return;
        }
    } else {
        send_err(fd, 400, "Bad Request", "that program cannot be deleted");
        return;
    }
    if (!safe_component(name)) {
        send_err(fd, 400, "Bad Request", "that program cannot be deleted");
        return;
    }
    if (lstat(full, &st) != 0) {
        send_err(fd, 404, "Not Found", "program not found");
        return;
    }
    if (S_ISLNK(st.st_mode) || !realpath(root, root_real) || !realpath(full, resolved) ||
        !is_one_child(root_real, resolved)) {
        send_err(fd, 400, "Bad Request", "that program cannot be deleted");
        return;
    }
    if (app) {
        if (!S_ISDIR(st.st_mode) || rm_tree(resolved, 0) != 0) {
            send_err(fd, 500, "Internal Server Error", "could not remove the program");
            return;
        }
    } else {
        if (boot_edit_remove(g_card, id) != 0) {
            send_err(fd, 500, "Internal Server Error", "could not remove the boot entry");
            return;
        }
        if (!S_ISREG(st.st_mode) || unlink(full) != 0) {
            send_err(fd, 500, "Internal Server Error", "could not remove the program");
            return;
        }
    }
    {
        char def_path[PATH_MAX], line[MENU_ID];
        if (snprintf(def_path, sizeof def_path, "%s/autorun/system/boot-default", g_card) < (int)sizeof def_path) {
            FILE *df = fopen(def_path, "r");
            if (df) {
                int is_def = 0;
                if (fgets(line, sizeof line, df)) {
                    trim_inplace(line);
                    if (strcmp(line, id) == 0) is_def = 1;
                }
                fclose(df);
                if (is_def) menu_set_default(g_card, "stock");
            }
        }
    }
    reload_boot_thingy();
    send_json(fd, 200, "OK", "{\"ok\":true}\n");
}

static int body_id(const char *body, char *out, size_t n) {
    size_t i = 0;
    while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
    if (*body == '{') {
        const char *p = strstr(body, "\"id\"");
        if (!p) return -1;
        p = strchr(p, ':');
        if (!p) return -1;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') return -1;
        p++;
        while (*p && *p != '"' && *p != '\n' && *p != '\r') {
            if (i + 1 >= n) return -1;
            out[i++] = *p++;
        }
        if (*p != '"') return -1;
        out[i] = 0;
        return out[0] ? 0 : -1;
    }
    while (*body && *body != '\r' && *body != '\n') {
        if (i + 1 >= n) return -1;
        out[i++] = *body++;
    }
    while (i && (out[i - 1] == ' ' || out[i - 1] == '\t')) i--;
    out[i] = 0;
    return out[0] ? 0 : -1;
}

static void handle_default(int fd, struct io *io, int has_len, size_t content_len) {
    char raw[1024], id[MENU_ID];
    struct menu menu;
    char resp[MENU_ID + 32];
    int found = 0;
    if (!has_len || content_len == 0 || content_len >= sizeof raw) {
        send_err(fd, 400, "Bad Request", "missing default id");
        return;
    }
    if (io_read(io, raw, content_len) != 0) {
        send_err(fd, 400, "Bad Request", "missing default id");
        return;
    }
    raw[content_len] = 0;
    if (body_id(raw, id, sizeof id) != 0) {
        send_err(fd, 400, "Bad Request", "missing default id");
        return;
    }
    menu_load(&menu, g_card);
    for (int i = 0; i < menu.n; i++) {
        if (strcmp(menu.items[i].id, id) == 0) found = 1;
    }
    if (!found) {
        send_err(fd, 400, "Bad Request", "that id is not in the menu");
        return;
    }
    if (menu_set_default(g_card, id) != 0) {
        send_err(fd, 500, "Internal Server Error", "could not save the default");
        return;
    }
    reload_boot_thingy();
    snprintf(resp, sizeof resp, "{\"ok\":true,\"id\":\"%s\"}\n", id);
    /* ids are single-line menu keys without quotes */
    if (strchr(id, '"') || strchr(id, '\\') || strchr(id, '\n')) {
        send_json(fd, 200, "OK", "{\"ok\":true}\n");
        return;
    }
    send_json(fd, 200, "OK", resp);
}

static const char *boot_now_file_path(void) {
    const char *env = getenv("MPC_BOOT_NOW_FILE");
    return (env && env[0]) ? env : "/run/mpc-boot-now";
}

static const char *boot_pid_file_path(void) {
    const char *env = getenv("MPC_BOOT_PID_FILE");
    return (env && env[0]) ? env : "/run/mpc-boot-thingy.pid";
}

static int trigger_boot_now(const char *id) {
    const char *path = boot_now_file_path();
    const char *pid_path = boot_pid_file_path();
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", id);
    fflush(f);
    fclose(f);
    if (strcmp(tmp, path) != 0) {
        rename(tmp, path);
    }

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        sync();
        FILE *pf = fopen(pid_path, "r");
        if (pf) {
            int target_pid = 0;
            if (fscanf(pf, "%d", &target_pid) == 1 && target_pid > 1) {
                if (kill(target_pid, SIGUSR2) == 0) {
                    fclose(pf);
                    _exit(0);
                }
            }
            fclose(pf);
        }
        pid_t kpid = fork();
        if (kpid == 0) {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                if (null_fd > STDERR_FILENO) close(null_fd);
            }
            execlp("killall", "killall", "-USR2", "mpc-boot-thingy", (char *)NULL);
            _exit(127);
        }
        if (kpid > 0) {
            int kst = 0;
            if (waitpid(kpid, &kst, 0) > 0 && WIFEXITED(kst) && WEXITSTATUS(kst) == 0) {
                _exit(0);
            }
        }
        pid_t rpid = fork();
        if (rpid == 0) {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                if (null_fd > STDERR_FILENO) close(null_fd);
            }
            execlp("systemctl", "systemctl", "reset-failed", "mpc-boot-thingy.service", (char *)NULL);
            _exit(127);
        }
        if (rpid > 0) waitpid(rpid, NULL, 0);

        char run_bin[512];
        snprintf(run_bin, sizeof run_bin, "%s/system/mpc-boot-thingy", g_autorun);
        if (access("/run/mpc-boot-thingy", X_OK) == 0) {
            execlp("systemd-run", "systemd-run", "--unit=mpc-boot-thingy", "--collect", "--no-block", "/run/mpc-boot-thingy", g_autorun, (char *)NULL);
            execlp("/run/mpc-boot-thingy", "/run/mpc-boot-thingy", g_autorun, (char *)NULL);
        }
        execlp("systemd-run", "systemd-run", "--unit=mpc-boot-thingy", "--collect", "--no-block", run_bin, g_autorun, (char *)NULL);
        execlp(run_bin, run_bin, g_autorun, (char *)NULL);
        _exit(127);
    }
    return 0;
}

static void handle_boot_now(int fd, struct io *io, int has_len, size_t content_len) {
    char raw[1024], id[MENU_ID];
    struct menu menu;
    char resp[MENU_ID + 32];
    int found = 0;
    if (!has_len || content_len == 0 || content_len >= sizeof raw) {
        send_err(fd, 400, "Bad Request", "missing app id");
        return;
    }
    if (io_read(io, raw, content_len) != 0) {
        send_err(fd, 400, "Bad Request", "missing app id");
        return;
    }
    raw[content_len] = 0;
    if (body_id(raw, id, sizeof id) != 0) {
        send_err(fd, 400, "Bad Request", "missing app id");
        return;
    }
    if (strcmp(id, MENU_BOOT_THINGY_ID) != 0) {
        menu_load(&menu, g_card);
        for (int i = 0; i < menu.n; i++) {
            if (strcmp(menu.items[i].id, id) == 0) found = 1;
        }
        if (!found) {
            send_err(fd, 400, "Bad Request", "that id is not in the menu");
            return;
        }
    }
    if (trigger_boot_now(id) != 0) {
        send_err(fd, 500, "Internal Server Error", "could not trigger boot");
        return;
    }
    snprintf(resp, sizeof resp, "{\"ok\":true,\"id\":\"%s\"}\n", id);
    if (strchr(id, '"') || strchr(id, '\\') || strchr(id, '\n')) {
        send_json(fd, 200, "OK", "{\"ok\":true}\n");
        return;
    }
    send_json(fd, 200, "OK", resp);
}

static int reboot_device(void) {
    int fd = open(BOOTCOUNT, O_WRONLY);
    pid_t pid;
    if (fd < 0) return -1;
    if (write_full(fd, "0\n", 2) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        sync();
        sleep(1);
        execlp("reboot", "reboot", (char *)NULL);
        _exit(127);
    }
    return 0;
}

static int reload_boot_thingy(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        sync();
        FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
        if (f) {
            fputs("3\n", f);
            fclose(f);
        }
        FILE *pf = fopen(boot_pid_file_path(), "r");
        if (pf) {
            int target_pid = 0;
            if (fscanf(pf, "%d", &target_pid) == 1 && target_pid > 1) {
                if (kill(target_pid, SIGHUP) == 0) {
                    fclose(pf);
                    _exit(0);
                }
            }
            fclose(pf);
        }
        execlp("killall", "killall", "-HUP", "mpc-boot-thingy", (char *)NULL);
        _exit(0);
    }
    return 0;
}

static int ws_send(int fd, int opcode, const void *data, size_t len) {
    unsigned char hdr[10];
    size_t hlen;
    hdr[0] = (unsigned char)(0x80 | (opcode & 0x0f));
    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)((len >> 8) & 0xff);
        hdr[3] = (unsigned char)(len & 0xff);
        hlen = 4;
    } else {
        uint64_t l = len;
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (unsigned char)((l >> (56 - 8 * i)) & 0xff);
        hlen = 10;
    }
    if (write_full(fd, hdr, hlen) != 0) return -1;
    if (len && write_full(fd, data, len) != 0) return -1;
    return 0;
}

static int ws_read_frame(struct io *io, int *op, int *fin, unsigned char **data, size_t *len) {
    unsigned char hdr[2];
    uint64_t plen;
    int masked;
    unsigned char mask[4];
    unsigned char *buf;
    if (io_read(io, hdr, 2) != 0) return -1;
    *fin = (hdr[0] & 0x80) != 0;
    *op = hdr[0] & 0x0f;
    masked = (hdr[1] & 0x80) != 0;
    plen = hdr[1] & 0x7f;
    if (!masked) return -1;
    if (plen == 126) {
        unsigned char ext[2];
        if (io_read(io, ext, 2) != 0) return -1;
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        if (io_read(io, ext, 8) != 0) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }
    if (plen > WS_MAX) return -1;
    if (io_read(io, mask, 4) != 0) return -1;
    buf = malloc((size_t)plen + 1);
    if (!buf) return -1;
    if (plen && io_read(io, buf, (size_t)plen) != 0) {
        free(buf);
        return -1;
    }
    for (uint64_t i = 0; i < plen; i++) buf[i] ^= mask[i & 3];
    buf[plen] = 0;
    *data = buf;
    *len = (size_t)plen;
    return 0;
}

static int append_msg(unsigned char **msg, size_t *len, const unsigned char *data, size_t n) {
    unsigned char *nbuf;
    if (*len + n > WS_MAX) return -1;
    nbuf = realloc(*msg, *len + n + 1);
    if (!nbuf) return -1;
    memcpy(nbuf + *len, data, n);
    *len += n;
    nbuf[*len] = 0;
    *msg = nbuf;
    return 0;
}

static int parse_resize(const char *s, int *cols, int *rows) {
    const char *c = strstr(s, "\"cols\"");
    const char *r = strstr(s, "\"rows\"");
    char *endc = NULL, *endr = NULL;
    long cv, rv;
    if (!c || !r) return -1;
    c = strchr(c + 6, ':');
    r = strchr(r + 6, ':');
    if (!c || !r) return -1;
    cv = strtol(c + 1, &endc, 10);
    rv = strtol(r + 1, &endr, 10);
    if (endc == c + 1 || endr == r + 1) return -1;
    if (cv < 1 || cv > 500 || rv < 1 || rv > 200) return -1;
    *cols = (int)cv;
    *rows = (int)rv;
    return 0;
}

static int start_shell(int *ptm_out, pid_t *pid_out) {
    int ptm = posix_openpt(O_RDWR | O_NOCTTY);
    char *name;
    char slave[128];
    pid_t pid;
    struct winsize wz;
    if (ptm < 0) {
        fprintf(stderr, "posix_openpt: %s\n", strerror(errno));
        return -1;
    }
    if (grantpt(ptm) != 0 || unlockpt(ptm) != 0) {
        fprintf(stderr, "grantpt: %s\n", strerror(errno));
        close(ptm);
        return -1;
    }
    name = ptsname(ptm);
    if (!name || strlen(name) >= sizeof slave) {
        fprintf(stderr, "ptsname: %s\n", strerror(errno));
        close(ptm);
        return -1;
    }
    memcpy(slave, name, strlen(name) + 1);
    pid = fork();
    if (pid < 0) {
        close(ptm);
        return -1;
    }
    if (pid == 0) {
        int s;
        char shell[PATH_MAX];
        int sn;
        close(ptm);
        if (setsid() < 0) _exit(127);
        s = open(slave, O_RDWR);
        if (s < 0) _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, STDIN_FILENO);
        dup2(s, STDOUT_FILENO);
        dup2(s, STDERR_FILENO);
        if (s > STDERR_FILENO) close(s);
        chdir(g_card);
        setenv("TERM", "xterm-256color", 1);
        setenv("HOME", "/root", 1);
        setenv("USER", "root", 1);
        setenv("LOGNAME", "root", 1);
        sn = snprintf(shell, sizeof shell, "%s/autorun/os_tools/bin/bash", g_card);
        if (sn > 0 && (size_t)sn < sizeof shell && access(shell, X_OK) == 0) {
            setenv("SHELL", shell, 1);
            execl(shell, shell, "-l", "-i", (char *)NULL);
        }
        setenv("SHELL", "/bin/sh", 1);
        execl("/bin/sh", "/bin/sh", "-i", (char *)NULL);
        _exit(127);
    }
    memset(&wz, 0, sizeof wz);
    wz.ws_col = 80;
    wz.ws_row = 24;
    ioctl(ptm, TIOCSWINSZ, &wz);
    *ptm_out = ptm;
    *pid_out = pid;
    return 0;
}

static void stop_shell(int ptm, pid_t pid) {
    if (ptm >= 0) close(ptm);
    if (pid > 0) {
        kill(pid, SIGHUP);
        kill(-pid, SIGHUP);
    }
}

static void handle_term(int fd, struct io *io, const char *headers) {
    char upgrade[64], connection[128], version[32], key[128], accept[64], resp[256];
    int ptm = -1, n;
    pid_t pid = 0;
    unsigned char *acc = NULL;
    size_t acc_len = 0;
    int start_op = 0;
    if (header_value(headers, "Upgrade", upgrade, sizeof upgrade) != 0 || !contains_ci(upgrade, "websocket") ||
        header_value(headers, "Connection", connection, sizeof connection) != 0 || !contains_ci(connection, "upgrade") ||
        header_value(headers, "Sec-WebSocket-Version", version, sizeof version) != 0 || strcmp(version, "13") != 0 ||
        header_value(headers, "Sec-WebSocket-Key", key, sizeof key) != 0) {
        send_err(fd, 400, "Bad Request", "websocket upgrade required");
        return;
    }
    trim_inplace(key);
    if (!key[0] || strlen(key) > 128) {
        send_err(fd, 400, "Bad Request", "websocket upgrade required");
        return;
    }
    if (start_shell(&ptm, &pid) != 0) {
        send_err(fd, 500, "Internal Server Error", "could not open a shell");
        return;
    }
    ws_accept(key, accept, sizeof accept);
    n = snprintf(resp, sizeof resp,
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n"
                 "\r\n",
                 accept);
    if (n < 0 || (size_t)n >= sizeof resp || write_full(fd, resp, (size_t)n) != 0) {
        stop_shell(ptm, pid);
        return;
    }
    for (;;) {
        struct pollfd pf[2];
        int pr;
        pf[0].fd = fd;
        pf[0].events = POLLIN;
        pf[1].fd = ptm;
        pf[1].events = POLLIN;
        pr = poll(pf, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pf[1].revents & POLLIN) {
            char buf[4096];
            ssize_t r = read(ptm, buf, sizeof buf);
            if (r <= 0) break;
            if (ws_send(fd, 2, buf, (size_t)r) != 0) break;
        } else if (pf[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            break;
        }
        if (pf[0].revents & POLLIN) {
            int op = 0, fin = 0;
            unsigned char *data = NULL;
            size_t len = 0;
            if (ws_read_frame(io, &op, &fin, &data, &len) != 0) break;
            if (op == 0x8) {
                free(data);
                break;
            }
            if (op == 0x9) {
                ws_send(fd, 0xA, data, len);
                free(data);
                continue;
            }
            if (op == 0xA) {
                free(data);
                continue;
            }
            if (op == 0) {
                if (!acc) {
                    free(data);
                    break;
                }
            } else {
                if (acc) {
                    free(data);
                    break;
                }
                start_op = op;
            }
            if (append_msg(&acc, &acc_len, data, len) != 0) {
                free(data);
                break;
            }
            free(data);
            if (!fin) continue;
            if (start_op == 1) {
                int cols = 0, rows = 0;
                if (parse_resize((char *)acc, &cols, &rows) == 0) {
                    struct winsize wz;
                    memset(&wz, 0, sizeof wz);
                    wz.ws_col = (unsigned short)cols;
                    wz.ws_row = (unsigned short)rows;
                    ioctl(ptm, TIOCSWINSZ, &wz);
                }
            } else if (start_op == 2) {
                if (acc_len && write_full(ptm, acc, acc_len) != 0) {
                    free(acc);
                    break;
                }
            }
            free(acc);
            acc = NULL;
            acc_len = 0;
            start_op = 0;
        } else if (pf[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            break;
        }
    }
    free(acc);
    stop_shell(ptm, pid);
}

static int read_request(struct io *io, char *method, char *uri, char **headers_out, int *has_len, size_t *content_len) {
    char hbuf[16384];
    size_t used = 0, sep = 0;
    char *line_end;
    size_t linelen, hlen, header_end;
    char line[2048];
    char cl[64], te[64];
    *headers_out = NULL;
    *has_len = 0;
    *content_len = 0;
    method[0] = 0;
    uri[0] = 0;
    while (used < sizeof hbuf && !find_sep(hbuf, used, &sep)) {
        ssize_t r = read(io->fd, hbuf + used, sizeof hbuf - used);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        used += (size_t)r;
    }
    if (!find_sep(hbuf, used, &sep)) {
        send_err(io->fd, 400, "Bad Request", "headers too large");
        return -1;
    }
    line_end = memchr(hbuf, '\n', sep);
    if (!line_end) {
        send_err(io->fd, 400, "Bad Request", "bad request");
        return -1;
    }
    linelen = (size_t)(line_end - hbuf);
    if (linelen && hbuf[linelen - 1] == '\r') linelen--;
    if (linelen >= sizeof line) {
        send_err(io->fd, 400, "Bad Request", "bad request");
        return -1;
    }
    memcpy(line, hbuf, linelen);
    line[linelen] = 0;
    if (sscanf(line, "%15s %1023s", method, uri) != 2) {
        send_err(io->fd, 400, "Bad Request", "bad request");
        return -1;
    }
    hlen = sep - (size_t)(line_end + 1 - hbuf);
    *headers_out = malloc(hlen + 1);
    if (!*headers_out) return -1;
    memcpy(*headers_out, line_end + 1, hlen);
    (*headers_out)[hlen] = 0;
    header_end = sep + 4;
    if (used > header_end) {
        size_t left = used - header_end;
        if (left > sizeof io->buf) left = sizeof io->buf;
        memcpy(io->buf, hbuf + header_end, left);
        io->len = left;
        io->pos = 0;
    }
    if (header_value(*headers_out, "Transfer-Encoding", te, sizeof te) == 0 && te[0]) {
        send_err(io->fd, 400, "Bad Request", "chunked bodies are not supported");
        return -1;
    }
    if (header_value(*headers_out, "Content-Length", cl, sizeof cl) == 0) {
        char *end = NULL;
        unsigned long long v = strtoull(cl, &end, 10);
        if (end == cl || (end && *end)) {
            send_err(io->fd, 400, "Bad Request", "bad content length");
            return -1;
        }
        if (v > MAX_UPLOAD) {
            send_err(io->fd, 413, "Payload Too Large", "program is larger than 64 MiB");
            return -1;
        }
        *has_len = 1;
        *content_len = (size_t)v;
    }
    return 0;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int json_string(const char **pp, char *out, size_t n) {
    const char *p = *pp;
    size_t i = 0;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (*p == '"' || *p == '\\' || *p == '/') c = (unsigned char)*p++;
            else if (*p == 'n') {
                c = '\n';
                p++;
            } else if (*p == 't') {
                c = '\t';
                p++;
            } else return -1;
        }
        if (c < 0x20 || i + 1 >= n) return -1;
        out[i++] = (char)c;
    }
    if (*p != '"') return -1;
    out[i] = 0;
    *pp = p + 1;
    return 0;
}

static const char *json_key(const char *s, const char *key) {
    size_t klen = strlen(key);
    const char *p;
    for (p = s; *p; p++) {
        if (*p != '"') continue;
        p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            p = skip_ws(p + klen + 1);
            if (*p == ':') return skip_ws(p + 1);
        }
    }
    return NULL;
}

static void handle_patches(int fd) {
    struct boot_patch patches[BOOT_PATCH_MAX];
    char *buf = NULL;
    size_t sz = 0;
    FILE *fp;
    int n = boot_edit_list(g_card, patches, BOOT_PATCH_MAX);
    int i;
    fp = open_memstream(&buf, &sz);
    if (!fp) {
        send_err(fd, 500, "Internal Server Error", "could not list patches");
        return;
    }
    fputs("{\"patches\":[", fp);
    for (i = 0; i < n; i++) {
        if (i) fputc(',', fp);
        fputs("{\"id\":", fp);
        json_escape_print(fp, patches[i].id);
        fputs(",\"title\":", fp);
        json_escape_print(fp, patches[i].title);
        fputc('}', fp);
    }
    fputs("]}\n", fp);
    fclose(fp);
    send_json(fd, 200, "OK", buf);
    free(buf);
}

static void handle_boot_edit(int fd, struct io *io, int has_len, size_t content_len) {
    char raw[8192], title[MENU_TITLE], id[MENU_ID], out_id[MENU_ID];
    char ids[BOOT_PATCH_MAX][BOOT_PATCH_ID];
    const char *ptrs[BOOT_PATCH_MAX];
    const char *title_at, *id_at, *patches_at, *p;
    int npatches = 0, i, rc;
    char resp[MENU_ID + 32];
    if (!has_len || content_len == 0 || content_len >= sizeof raw) {
        send_err(fd, 400, "Bad Request", "missing boot entry");
        return;
    }
    if (io_read(io, raw, content_len) != 0) {
        send_err(fd, 400, "Bad Request", "missing boot entry");
        return;
    }
    raw[content_len] = 0;
    title_at = json_key(raw, "title");
    patches_at = json_key(raw, "patches");
    id_at = json_key(raw, "id");
    if (!title_at || json_string(&title_at, title, sizeof title) != 0 || !one_line(title) || strlen(title) >= MENU_TITLE) {
        send_err(fd, 400, "Bad Request", "title must be one short line");
        return;
    }
    if (!patches_at) {
        send_err(fd, 400, "Bad Request", "missing patches");
        return;
    }
    p = skip_ws(patches_at);
    if (*p != '[') {
        send_err(fd, 400, "Bad Request", "missing patches");
        return;
    }
    p = skip_ws(p + 1);
    if (*p != ']') {
        while (*p) {
            if (npatches >= BOOT_PATCH_MAX || json_string(&p, ids[npatches], sizeof ids[npatches]) != 0) {
                send_err(fd, 400, "Bad Request", "unknown patch");
                return;
            }
            npatches++;
            p = skip_ws(p);
            if (*p == ']') break;
            if (*p != ',') {
                send_err(fd, 400, "Bad Request", "missing patches");
                return;
            }
            p = skip_ws(p + 1);
        }
        if (*p != ']') {
            send_err(fd, 400, "Bad Request", "missing patches");
            return;
        }
    }
    id[0] = 0;
    if (id_at && *id_at != 'n') {
        if (json_string(&id_at, id, sizeof id) != 0) {
            send_err(fd, 400, "Bad Request", "that entry cannot be edited");
            return;
        }
    }
    for (i = 0; i < npatches; i++) ptrs[i] = ids[i];
    rc = boot_edit_save(g_card, id[0] ? id : NULL, title, ptrs, npatches, out_id, sizeof out_id);
    if (rc == BOOT_EDIT_BAD_TITLE) {
        send_err(fd, 400, "Bad Request", "title must be one short line");
        return;
    }
    if (rc == BOOT_EDIT_BAD_PATCH) {
        send_err(fd, 400, "Bad Request", "unknown patch");
        return;
    }
    if (rc == BOOT_EDIT_BAD_ID) {
        send_err(fd, 400, "Bad Request", "that entry cannot be edited");
        return;
    }
    if (rc != BOOT_EDIT_OK) {
        send_err(fd, 500, "Internal Server Error", boot_edit_last_error());
        return;
    }
    reload_boot_thingy();
    if (strchr(out_id, '"') || strchr(out_id, '\\')) {
        send_json(fd, 200, "OK", "{\"ok\":true}\n");
        return;
    }
    snprintf(resp, sizeof resp, "{\"ok\":true,\"id\":\"%s\"}\n", out_id);
    send_json(fd, 200, "OK", resp);
}

static void handle_client(int client) {
    struct io io;
    char method[16], uri[1024], path[1024];
    char *headers = NULL;
    char *query = NULL;
    int has_len = 0;
    size_t content_len = 0;
    int needs_origin;
    memset(&io, 0, sizeof io);
    io.fd = client;
    if (read_request(&io, method, uri, &headers, &has_len, &content_len) != 0) goto out;
    query = strchr(uri, '?');
    if (query) *query++ = 0;
    snprintf(path, sizeof path, "%s", uri);
    if (strcmp(method, "OPTIONS") == 0) {
        send_body(client, 204, "No Content", "text/plain", "", 0);
        goto out;
    }
    needs_origin = strcmp(method, "POST") == 0 || strcmp(method, "DELETE") == 0 || strcmp(path, "/ws/term") == 0;
    if (needs_origin) {
        char origin[160];
        if (header_value(headers, "Origin", origin, sizeof origin) != 0) {
            send_err(client, 403, "Forbidden", "origin is not this page");
            goto out;
        }
        trim_inplace(origin);
        if (!origin_ok(origin)) {
            send_err(client, 403, "Forbidden", "origin is not this page");
            goto out;
        }
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/ws/term") == 0) {
        handle_term(client, &io, headers);
        goto out;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        handle_status(client);
        goto out;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/patches") == 0) {
        handle_patches(client);
        goto out;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/boot-edits") == 0) {
        handle_boot_edit(client, &io, has_len, content_len);
        goto out;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps") == 0) {
        handle_upload(client, &io, has_len, content_len, query);
        goto out;
    }
    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/apps") == 0) {
        handle_delete(client, query);
        goto out;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/default") == 0) {
        handle_default(client, &io, has_len, content_len);
        goto out;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/boot-now") == 0) {
        handle_boot_now(client, &io, has_len, content_len);
        goto out;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reboot") == 0) {
        if (reboot_device() != 0) send_err(client, 409, "Conflict", "bootcount node is absent");
        else send_json(client, 200, "OK", "{\"ok\":true}\n");
        goto out;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reload") == 0) {
        if (reload_boot_thingy() != 0) send_err(client, 500, "Internal Server Error", "reload failed");
        else send_json(client, 200, "OK", "{\"ok\":true}\n");
        goto out;
    }
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/", 5) != 0) {
        if (serve_web(client, path) != 0) {
            send_body(client, 404, "Not Found", "text/plain; charset=utf-8", "Not found\n", 10);
        }
        goto out;
    }
    send_err(client, 404, "Not Found", "not found");
out:
    free(headers);
    close(client);
}

int main(int argc, char **argv) {
    int srv, opt = 1;
    struct sockaddr_in addr;
    struct sigaction sa;
    if (argc >= 2 && strcmp(argv[1], "--check") == 0) return selftest();
    if (argc < 2) {
        fprintf(stderr, "usage: mpc-boot-web AUTORUN_DIR [port] [bind]\n");
        return 1;
    }
    if (!realpath(argv[1], g_autorun)) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    card_of(g_autorun, g_card, sizeof g_card);
    if (strcmp(g_card, ".") != 0) {
        char norm_autorun[PATH_MAX];
        struct stat st;
        if (snprintf(norm_autorun, sizeof norm_autorun, "%s/autorun", g_card) < (int)sizeof norm_autorun &&
            stat(norm_autorun, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(g_autorun, sizeof g_autorun, "%s", norm_autorun);
        }
    }
    (void)chdir("/");
    g_port = 8080;
    snprintf(g_bind, sizeof g_bind, "%s", "127.0.0.1");
    if (argc > 2) {
        char *end = NULL;
        long p = strtol(argv[2], &end, 10);
        if (!argv[2][0] || (end && *end) || p < 1 || p > 65535) {
            fprintf(stderr, "bad port\n");
            return 1;
        }
        g_port = (int)p;
    }
    if (argc > 3) {
        if (strlen(argv[3]) >= sizeof g_bind) {
            fprintf(stderr, "bad address\n");
            return 1;
        }
        snprintf(g_bind, sizeof g_bind, "%s", argv[3]);
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = reap;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_bind, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad address: %s\n", g_bind);
        return 1;
    }
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 16) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "mpc-boot-web listening on http://%s:%d\n", g_bind, g_port);
    fprintf(stderr, "card %s\n", g_card);
    for (;;) {
        int client = accept(srv, NULL, NULL);
        pid_t pid;
        if (client < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        pid = fork();
        if (pid < 0) {
            close(client);
            continue;
        }
        if (pid == 0) {
            close(srv);
            handle_client(client);
            _exit(0);
        }
        close(client);
    }
}
