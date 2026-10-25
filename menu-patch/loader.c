/* In-memory patch loader for /usr/bin/MPC Sample.

   launch-ac50 sets LD_PRELOAD to this library. The constructor stores the
   bytes from each *.list into the process's private mapping. The file on
   disk is not modified. Optional *.so modules in the same directory are
   opened after the lists apply, and mpc_apply() is called when present.

   MPC_PATCH_DIR names the directory. A process whose executable is not
   "MPC Sample" returns immediately. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef MPC_LOADER_TEST
#include <dlfcn.h>
#include <link.h>
#endif

#ifndef PF_X
#define PF_X (1u << 0)
#define PF_W (1u << 1)
#define PF_R (1u << 2)
#endif

struct mpc_load {
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint32_t flags;
};

struct mpc_image {
    uintptr_t bias;
    const struct mpc_load *loads;
    int nloads;
    uint64_t file_size;
    int (*protect)(void *addr, size_t len, int writable, uint32_t flags, void *ctx);
    void *protect_ctx;
};

struct rec {
    uint64_t offset;
    uint8_t *orig;
    uint8_t *repl;
    size_t len;
    char *name;
};

void *mpc_va(const struct mpc_image *img, uint64_t off, size_t len, uint32_t *flags) {
    uint64_t end;
    int i;
    if (!img || len == 0) return NULL;
    end = off + (uint64_t)len;
    if (end < off) return NULL;
    for (i = 0; i < img->nloads; i++) {
        const struct mpc_load *L = &img->loads[i];
        uint64_t load_end = L->offset + L->filesz;
        uintptr_t addr;
        if (load_end < L->offset) continue;
        if (off < L->offset || end > load_end) continue;
        addr = img->bias + (uintptr_t)L->vaddr + (uintptr_t)(off - L->offset);
        if (flags) *flags = L->flags;
        return (void *)addr;
    }
    return NULL;
}

static void log_msg(const char *fmt, ...) {
#ifdef MPC_LOADER_TEST
    (void)fmt;
#else
    va_list ap;
    FILE *f;
    va_start(ap, fmt);
    fputs("[boot-menu] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    f = fopen("/tmp/boot-menu-loader.log", "a");
    if (!f) return;
    va_start(ap, fmt);
    fputs("[boot-menu] ", f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
#endif
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *s, uint8_t **out, size_t *n) {
    size_t len = strlen(s);
    size_t cap;
    uint8_t *buf;
    size_t i = 0, o = 0;
    if (len % 4 != 0) return -1;
    cap = len / 4 * 3;
    buf = malloc(cap ? cap : 1);
    if (!buf) return -1;
    while (i < len) {
        int v[4];
        int k;
        int pad = 0;
        for (k = 0; k < 4; k++) {
            char c = s[i + (size_t)k];
            if (c == '=') {
                v[k] = 0;
                pad++;
            } else {
                v[k] = b64_val((unsigned char)c);
                if (v[k] < 0 || pad) {
                    free(buf);
                    return -1;
                }
            }
        }
        if (pad > 2) {
            free(buf);
            return -1;
        }
        if (o >= cap) {
            free(buf);
            return -1;
        }
        buf[o++] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
        if (pad < 2) {
            if (o >= cap) {
                free(buf);
                return -1;
            }
            buf[o++] = (uint8_t)(((v[1] & 15) << 4) | (v[2] >> 2));
        }
        if (pad < 1) {
            if (o >= cap) {
                free(buf);
                return -1;
            }
            buf[o++] = (uint8_t)(((v[2] & 3) << 6) | v[3]);
        }
        i += 4;
    }
    *out = buf;
    *n = o;
    return 0;
}

static int ends_with(const char *name, const char *suf) {
    size_t n = strlen(name);
    size_t s = strlen(suf);
    if (n < s || name[0] == '.') return 0;
    return strcmp(name + n - s, suf) == 0;
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int collect_names(const char *dir, const char *suf, char ***out, int *n) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    char **names = NULL;
    int count = 0, cap = 0;
    if (!d) return -1;
    while ((ent = readdir(d)) != NULL) {
        char *copy;
        if (!ends_with(ent->d_name, suf)) continue;
        if (count == cap) {
            int ncap = cap ? cap * 2 : 8;
            char **p = realloc(names, (size_t)ncap * sizeof *p);
            if (!p) {
                closedir(d);
                goto fail;
            }
            names = p;
            cap = ncap;
        }
        copy = strdup(ent->d_name);
        if (!copy) {
            closedir(d);
            goto fail;
        }
        names[count++] = copy;
    }
    closedir(d);
    if (count > 1) qsort(names, (size_t)count, sizeof *names, cmpstr);
    *out = names;
    *n = count;
    return 0;
fail:
    while (count > 0) free(names[--count]);
    free(names);
    return -1;
}

static void free_names(char **names, int n) {
    int i;
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
}

static int read_line(FILE *f, char **buf, size_t *cap) {
    size_t n = 0;
    int c;
    if (!*buf) {
        *cap = 256;
        *buf = malloc(*cap);
        if (!*buf) return -1;
    }
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= *cap) {
            size_t ncap = *cap * 2;
            char *p = realloc(*buf, ncap);
            if (!p) return -1;
            *buf = p;
            *cap = ncap;
        }
        (*buf)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (c == EOF && n == 0) return 1;
    (*buf)[n] = 0;
    if (n > 0 && (*buf)[n - 1] == '\n') (*buf)[n - 1] = 0;
    if (n > 1 && (*buf)[n - 2] == '\r') (*buf)[n - 2] = 0;
    return 0;
}

static void free_recs(struct rec *recs, int n) {
    int i;
    for (i = 0; i < n; i++) {
        free(recs[i].orig);
        free(recs[i].repl);
        free(recs[i].name);
    }
    free(recs);
}

static int add_rec(struct rec **recs, int *n, int *cap, struct rec rec) {
    if (*n == *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        struct rec *p = realloc(*recs, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        *recs = p;
        *cap = ncap;
    }
    (*recs)[(*n)++] = rec;
    return 0;
}

/* Parse one list. *size is set when the file has #target-size. */
static int parse_list(const char *path, struct rec **recs, int *n, int *cap,
                      uint64_t *size, int *have_size) {
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap_line = 0;
    const char *base;
    int rc = 0;
    if (!f) return -1;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    while ((rc = read_line(f, &line, &cap_line)) == 0) {
        char *p = line;
        char *end = NULL;
        char *b64o;
        char *b64p;
        unsigned long long off;
        struct rec rec;
        uint8_t *orig = NULL;
        uint8_t *repl = NULL;
        size_t olen = 0, rlen = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#') {
            if (strncmp(p, "#target-size", 12) == 0 && (p[12] == ' ' || p[12] == '\t')) {
                unsigned long long v = strtoull(p + 12, &end, 10);
                if (end == p + 12) {
                    rc = -1;
                    break;
                }
                if (*have_size && *size != v) {
                    log_msg("%s target size disagrees with another list", base);
                    rc = -1;
                    break;
                }
                *size = v;
                *have_size = 1;
            }
            continue;
        }
        off = strtoull(p, &end, 10);
        if (end == p || *end != ' ') {
            log_msg("%s has a bad patch line", base);
            rc = -1;
            break;
        }
        b64o = end + 1;
        b64p = strchr(b64o, ' ');
        if (!b64p || strchr(b64p + 1, ' ')) {
            log_msg("%s has a bad patch line", base);
            rc = -1;
            break;
        }
        *b64p = 0;
        b64p++;
        if (b64_decode(b64o, &orig, &olen) != 0 || b64_decode(b64p, &repl, &rlen) != 0 ||
            olen != rlen || olen == 0) {
            free(orig);
            free(repl);
            log_msg("%s patch at offset %s has bad bytes", base, p);
            rc = -1;
            break;
        }
        memset(&rec, 0, sizeof rec);
        rec.offset = off;
        rec.orig = orig;
        rec.repl = repl;
        rec.len = olen;
        rec.name = strdup(base);
        if (!rec.name || add_rec(recs, n, cap, rec) != 0) {
            free(rec.name);
            free(orig);
            free(repl);
            rc = -1;
            break;
        }
    }
    free(line);
    fclose(f);
    if (rc < 0) return -1;
    return 0;
}

#ifndef MPC_LOADER_TEST
static int prot_of(uint32_t flags) {
    int prot = 0;
    if (flags & PF_R) prot |= PROT_READ;
    if (flags & PF_W) prot |= PROT_WRITE;
    if (flags & PF_X) prot |= PROT_EXEC;
    if (prot == 0) prot = PROT_READ;
    return prot;
}
#endif

#ifndef MPC_LOADER_TEST
static int live_protect(void *addr, size_t len, int writable, uint32_t flags, void *ctx) {
    long page;
    uintptr_t start, end;
    int prot;
    (void)ctx;
    page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    start = (uintptr_t)addr & ~((uintptr_t)page - 1);
    end = ((uintptr_t)addr + len + (uintptr_t)page - 1) & ~((uintptr_t)page - 1);
    prot = writable ? (PROT_READ | PROT_WRITE) : prot_of(flags);
    if (mprotect((void *)start, (size_t)(end - start), prot) != 0) {
        log_msg("mprotect %s failed: %s", writable ? "writable" : "restore", strerror(errno));
        return -1;
    }
    return 0;
}
#endif

static int call_protect(const struct mpc_image *img, void *addr, size_t len, int writable, uint32_t flags) {
#ifdef MPC_LOADER_TEST
    if (!img->protect) return -1;
    return img->protect(addr, len, writable, flags, img->protect_ctx);
#else
    if (img->protect) return img->protect(addr, len, writable, flags, img->protect_ctx);
    return live_protect(addr, len, writable, flags, img->protect_ctx);
#endif
}

static int apply_records(const struct mpc_image *img, struct rec *recs, int n) {
    struct site {
        void *addr;
        uint32_t flags;
    } *sites;
    int i;
    if (n == 0) return 0;
    sites = calloc((size_t)n, sizeof *sites);
    if (!sites) return -1;
    for (i = 0; i < n; i++) {
        sites[i].addr = mpc_va(img, recs[i].offset, recs[i].len, &sites[i].flags);
        if (!sites[i].addr) {
            log_msg("%s offset %llu is outside a loaded segment", recs[i].name,
                    (unsigned long long)recs[i].offset);
            free(sites);
            return -1;
        }
        if (memcmp(sites[i].addr, recs[i].orig, recs[i].len) != 0) {
            log_msg("%s offset %llu does not match the stock bytes", recs[i].name,
                    (unsigned long long)recs[i].offset);
            free(sites);
            return -1;
        }
    }
    for (i = 0; i < n; i++) {
        if (call_protect(img, sites[i].addr, recs[i].len, 1, sites[i].flags) != 0) {
            int j;
            for (j = 0; j < i; j++)
                call_protect(img, sites[j].addr, recs[j].len, 0, sites[j].flags);
            free(sites);
            return -1;
        }
    }
    for (i = 0; i < n; i++) memcpy(sites[i].addr, recs[i].repl, recs[i].len);
    for (i = 0; i < n; i++) {
        if (call_protect(img, sites[i].addr, recs[i].len, 0, sites[i].flags) != 0) {
            int j;
            for (j = 0; j < n; j++) {
                call_protect(img, sites[j].addr, recs[j].len, 1, sites[j].flags);
                memcpy(sites[j].addr, recs[j].orig, recs[j].len);
                call_protect(img, sites[j].addr, recs[j].len, 0, sites[j].flags);
            }
            free(sites);
            return -1;
        }
    }
    for (i = 0; i < n; i++)
        __builtin___clear_cache((char *)sites[i].addr, (char *)sites[i].addr + recs[i].len);
    free(sites);
    return 0;
}

#ifndef MPC_LOADER_TEST
static int load_modules(const char *dir) {
    char **names = NULL;
    int n = 0, i;
    if (collect_names(dir, ".so", &names, &n) != 0) {
        log_msg("could not read %s", dir);
        return -1;
    }
    for (i = 0; i < n; i++) {
        char path[4096];
        void *handle;
        int (*fn)(void);
        int wrote;
        wrote = snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof path) {
            log_msg("path too long: %s", names[i]);
            continue;
        }
        handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            log_msg("could not load %s: %s", names[i], dlerror());
            continue;
        }
        fn = (int (*)(void))dlsym(handle, "mpc_apply");
        if (fn && fn() != 0) log_msg("%s mpc_apply failed", names[i]);
    }
    free_names(names, n);
    return 0;
}
#endif

int mpc_apply_dir(const struct mpc_image *img, const char *dir) {
    char **names = NULL;
    struct rec *recs = NULL;
    int nnames = 0, nrecs = 0, cap = 0, i;
    int have_size = 0;
    uint64_t want = 0;
    int rc;
    if (collect_names(dir, ".list", &names, &nnames) != 0) {
        log_msg("could not read %s", dir);
        return -1;
    }
    for (i = 0; i < nnames; i++) {
        char path[4096];
        int wrote = snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof path) {
            log_msg("path too long: %s", names[i]);
            rc = -1;
            goto done;
        }
        if (parse_list(path, &recs, &nrecs, &cap, &want, &have_size) != 0) {
            rc = -1;
            goto done;
        }
    }
    if (have_size && want != img->file_size) {
        log_msg("file size %llu != patch target %llu, leaving this process stock",
                (unsigned long long)img->file_size, (unsigned long long)want);
        rc = -1;
        goto done;
    }
    if (apply_records(img, recs, nrecs) != 0) {
        rc = -1;
        goto done;
    }
    if (nrecs > 0) log_msg("applied %d patch%s in this process", nrecs, nrecs == 1 ? "" : "es");
    rc = 0;
#ifndef MPC_LOADER_TEST
    if (load_modules(dir) != 0) rc = -1;
#endif
done:
    free_names(names, nnames);
    free_recs(recs, nrecs);
    return rc;
}

#ifndef MPC_LOADER_TEST
struct grab {
    uintptr_t bias;
    struct mpc_load *loads;
    int n;
    int found;
    int failed;
};

static int grab_cb(struct dl_phdr_info *info, size_t size, void *data) {
    struct grab *g = data;
    int i;
    (void)size;
    if (info->dlpi_name && info->dlpi_name[0] != '\0') return 0;
    g->bias = (uintptr_t)info->dlpi_addr;
    g->found = 1;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        struct mpc_load *p;
        if (ph->p_type != PT_LOAD) continue;
        p = realloc(g->loads, (size_t)(g->n + 1) * sizeof *p);
        if (!p) {
            g->failed = 1;
            return 1;
        }
        g->loads = p;
        g->loads[g->n].offset = ph->p_offset;
        g->loads[g->n].vaddr = ph->p_vaddr;
        g->loads[g->n].filesz = ph->p_filesz;
        g->loads[g->n].flags = ph->p_flags;
        g->n++;
    }
    return 1;
}

static int image_of_process(struct mpc_image *img, struct grab *g) {
    struct stat st;
    memset(img, 0, sizeof *img);
    memset(g, 0, sizeof *g);
    if (stat("/proc/self/exe", &st) != 0) return -1;
    dl_iterate_phdr(grab_cb, g);
    if (!g->found || g->failed || g->n == 0) return -1;
    img->bias = g->bias;
    img->loads = g->loads;
    img->nloads = g->n;
    img->file_size = (uint64_t)st.st_size;
    return 0;
}

static int is_mpc_sample(void) {
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    const char *base;
    const char *p;
    if (n <= 0 || (size_t)n >= sizeof path - 1) return 0;
    path[n] = 0;
    base = path;
    for (p = path; *p; p++)
        if (*p == '/') base = p + 1;
    return strcmp(base, "MPC Sample") == 0;
}

__attribute__((constructor)) static void mpc_loader_init(void) {
    const char *dir = getenv("MPC_PATCH_DIR");
    struct mpc_image img;
    struct grab g;
    if (!dir || !dir[0]) return;
    if (!is_mpc_sample()) return;
    if (image_of_process(&img, &g) != 0) {
        log_msg("could not read the process image");
        free(g.loads);
        return;
    }
    if (mpc_apply_dir(&img, dir) != 0) log_msg("left the process stock");
    free(g.loads);
}
#endif

#ifdef MPC_LOADER_TEST
static unsigned char image_mem[3 * 4096];

static int failures;

#define expect(cond)                                                                 \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                              \
        }                                                                            \
    } while (0)

struct protect_log {
    int fail_on;
    int calls;
    int writes;
};

static int test_protect(void *addr, size_t len, int writable, uint32_t flags, void *ctx) {
    struct protect_log *log = ctx;
    (void)addr;
    (void)len;
    (void)flags;
    log->calls++;
    if (writable) log->writes++;
    if (log->fail_on && log->calls == log->fail_on) return -1;
    return 0;
}

static void fill_image(struct mpc_image *img, struct mpc_load *loads, struct protect_log *log) {
    memset(img, 0, sizeof *img);
    img->bias = (uintptr_t)image_mem;
    img->loads = loads;
    img->nloads = 2;
    img->file_size = 1000;
    img->protect = test_protect;
    img->protect_ctx = log;
    loads[0] = (struct mpc_load){.offset = 0, .vaddr = 0, .filesz = 4096, .flags = PF_R | PF_X};
    loads[1] = (struct mpc_load){.offset = 4096, .vaddr = 8192, .filesz = 4096, .flags = PF_R | PF_W};
}

static int write_list(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (fputs(body, f) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

static void test_va(void) {
    struct mpc_load loads[2];
    struct mpc_image img;
    struct protect_log log;
    uint32_t flags = 0;
    memset(&log, 0, sizeof log);
    fill_image(&img, loads, &log);
    expect(mpc_va(&img, 0x10, 4, &flags) == image_mem + 0x10);
    expect(flags == (PF_R | PF_X));
    expect(mpc_va(&img, 0x1000, 4, &flags) == image_mem + 0x2000);
    expect(flags == (PF_R | PF_W));
    expect(mpc_va(&img, 0x1800, 1, NULL) == image_mem + 0x2800);
    expect(mpc_va(&img, 0xffc, 4, NULL) == image_mem + 0xffc);
    expect(mpc_va(&img, 0xffc, 5, NULL) == NULL);
    expect(mpc_va(&img, 0x500, 1, NULL) == image_mem + 0x500);
    loads[0].filesz = 0x100;
    expect(mpc_va(&img, 0x500, 1, NULL) == NULL);
    expect(mpc_va(&img, 0, 0, NULL) == NULL);
}

static void test_apply(const char *dir) {
    struct mpc_load loads[2];
    struct mpc_image img;
    struct protect_log log;
    char path[512];
    memset(image_mem, 0x11, sizeof image_mem);
    memcpy(image_mem + 0x20, "ABCD", 4);
    memcpy(image_mem + 0x2000, "WXYZ", 4);
    memset(&log, 0, sizeof log);
    fill_image(&img, loads, &log);
    snprintf(path, sizeof path, "%s/10-a.list", dir);
    expect(write_list(path,
                      "#target-size 1000\n"
                      "\n"
                      "# comment\n"
                      "32 QUJDRA== V1hZWg==\n") == 0);
    snprintf(path, sizeof path, "%s/20-b.list", dir);
    expect(write_list(path, "4096 V1hZWg== QUJDRA==\n") == 0);
    expect(mpc_apply_dir(&img, dir) == 0);
    expect(memcmp(image_mem + 0x20, "WXYZ", 4) == 0);
    expect(memcmp(image_mem + 0x2000, "ABCD", 4) == 0);
    expect(log.writes == 2);
}

static void test_mismatch_writes_nothing(const char *dir) {
    struct mpc_load loads[2];
    struct mpc_image img;
    struct protect_log log;
    char path[512];
    unsigned char before[sizeof image_mem];
    memset(image_mem, 0x22, sizeof image_mem);
    memcpy(image_mem + 0x20, "ABCD", 4);
    memcpy(image_mem + 0x30, "EEEE", 4);
    memcpy(before, image_mem, sizeof before);
    memset(&log, 0, sizeof log);
    fill_image(&img, loads, &log);
    snprintf(path, sizeof path, "%s/a.list", dir);
    expect(write_list(path,
                      "#target-size 1000\n"
                      "32 QUJDRA== V1hZWg==\n"
                      "48 AAAAAA== V1hZWg==\n") == 0);
    expect(mpc_apply_dir(&img, dir) != 0);
    expect(memcmp(image_mem, before, sizeof before) == 0);
    expect(log.calls == 0);
}

static void test_size_gate(const char *dir) {
    struct mpc_load loads[2];
    struct mpc_image img;
    struct protect_log log;
    char path[512];
    memset(image_mem, 0x33, sizeof image_mem);
    memcpy(image_mem + 0x20, "ABCD", 4);
    memset(&log, 0, sizeof log);
    fill_image(&img, loads, &log);
    snprintf(path, sizeof path, "%s/a.list", dir);
    expect(write_list(path, "#target-size 999\n32 QUJDRA== V1hZWg==\n") == 0);
    expect(mpc_apply_dir(&img, dir) != 0);
    expect(memcmp(image_mem + 0x20, "ABCD", 4) == 0);
    expect(log.calls == 0);
}

static void test_protect_failure_rolls_back(const char *dir) {
    struct mpc_load loads[2];
    struct mpc_image img;
    struct protect_log log;
    char path[512];
    memset(image_mem, 0x44, sizeof image_mem);
    memcpy(image_mem + 0x20, "ABCD", 4);
    memcpy(image_mem + 0x2000, "WXYZ", 4);
    memset(&log, 0, sizeof log);
    log.fail_on = 2;
    fill_image(&img, loads, &log);
    snprintf(path, sizeof path, "%s/a.list", dir);
    expect(write_list(path,
                      "#target-size 1000\n"
                      "32 QUJDRA== MTM1Nw==\n"
                      "4096 V1hZWg== QUJDRA==\n") == 0);
    expect(mpc_apply_dir(&img, dir) != 0);
    expect(memcmp(image_mem + 0x20, "ABCD", 4) == 0);
    expect(memcmp(image_mem + 0x2000, "WXYZ", 4) == 0);
}

static void test_real_list(void) {
    struct mpc_load loads[1];
    struct mpc_image img;
    struct protect_log log;
    const char *path = "menu-patch/patches.list";
    unsigned char mark = 0xa5;
    char dir[] = "build/lt-realXXXXXX";
    char dest[128];
    char *made;
    FILE *src;
    FILE *dst;
    char buf[4096];
    size_t n;
    if (access(path, R_OK) != 0) return;
    made = mkdtemp(dir);
    expect(made != NULL);
    if (!made) return;
    snprintf(dest, sizeof dest, "%s/boot-menu.list", made);
    src = fopen(path, "r");
    dst = fopen(dest, "w");
    expect(src != NULL && dst != NULL);
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return;
    }
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    expect(fclose(dst) == 0);
    memset(image_mem, mark, sizeof image_mem);
    memset(&log, 0, sizeof log);
    memset(&img, 0, sizeof img);
    img.bias = (uintptr_t)image_mem;
    img.loads = loads;
    img.nloads = 1;
    img.file_size = 119243440;
    img.protect = test_protect;
    img.protect_ctx = &log;
    loads[0] = (struct mpc_load){.offset = 0, .vaddr = 0, .filesz = sizeof image_mem, .flags = PF_R | PF_X};
    expect(mpc_apply_dir(&img, made) != 0);
    expect(image_mem[0] == mark);
    expect(log.calls == 0);
}

int main(void) {
    char dir[] = "build/lt-mainXXXXXX";
    char *made = mkdtemp(dir);
    expect(made != NULL);
    if (!made) return 1;
    test_va();
    test_apply(made);
    /* fresh directories for the cases that must not see earlier lists */
    {
        char d2[] = "build/lt-misXXXXXX";
        char d3[] = "build/lt-szXXXXXX";
        char d4[] = "build/lt-protXXXXXX";
        char *m2 = mkdtemp(d2);
        char *m3 = mkdtemp(d3);
        char *m4 = mkdtemp(d4);
        expect(m2 && m3 && m4);
        if (m2) test_mismatch_writes_nothing(m2);
        if (m3) test_size_gate(m3);
        if (m4) test_protect_failure_rolls_back(m4);
    }
    test_real_list();
    if (failures) {
        fprintf(stderr, "%d loader check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("loader checks passed\n");
    return 0;
}
#endif
