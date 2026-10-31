#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "file_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static char g_data_root[PATH_MAX] = {0};

static int ensure_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int file_ops_init(const char *data_root) {
    char candidate[PATH_MAX];
    struct stat st;

    if (data_root && data_root[0]) {
        snprintf(candidate, sizeof candidate, "%s", data_root);
    } else if (stat("/data/UserContent", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(candidate, sizeof candidate, "/data/UserContent");
    } else if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(candidate, sizeof candidate, "/data/UserContent");
    } else {
        const char *env = getenv("MPC_DATA_DIR");
        if (env && env[0]) {
            snprintf(candidate, sizeof candidate, "%s", env);
        } else {
            snprintf(candidate, sizeof candidate, "build/test_data/UserContent");
        }
    }

    if (ensure_dir_exists(candidate) != 0) {
        return -1;
    }

    if (!realpath(candidate, g_data_root)) {
        return -1;
    }

    return 0;
}

const char *file_ops_get_root(void) {
    if (!g_data_root[0]) {
        if (file_ops_init(NULL) != 0) return "/data/UserContent";
    }
    return g_data_root;
}

static int is_safe_component(const char *s) {
    if (!s || !s[0]) return 0;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return 0;
    }
    return 1;
}

/* Normalizes a relative path string in-place: removes duplicate slashes, leading/trailing slashes.
   Returns -1 if path contains forbidden characters or '..' */
static int normalize_rel(const char *input, char *out, size_t out_sz) {
    if (!input || !out || out_sz == 0) return -1;
    out[0] = '\0';

    const char *p = input;
    while (*p == '/' || *p == ' ' || *p == '\t') p++;
    if (!*p) {
        return 0; // Empty means root of /data
    }

    char buf[PATH_MAX];
    size_t blen = 0;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        if (clen == 0) continue;

        char comp[256];
        if (clen >= sizeof comp) return -1;
        memcpy(comp, start, clen);
        comp[clen] = '\0';

        if (!is_safe_component(comp)) return -1;

        if (blen > 0) {
            if (blen + 1 >= sizeof buf) return -1;
            buf[blen++] = '/';
        }
        if (blen + clen >= sizeof buf) return -1;
        memcpy(buf + blen, comp, clen);
        blen += clen;
        buf[blen] = '\0';
    }

    if (blen >= out_sz) return -1;
    memcpy(out, buf, blen + 1);
    return 0;
}

int file_ops_resolve(const char *req_path, char *out_resolved, size_t res_sz, char *out_rel, size_t rel_sz) {
    const char *root = file_ops_get_root();
    size_t root_len = strlen(root);

    char clean_rel[PATH_MAX];
    if (normalize_rel(req_path, clean_rel, sizeof clean_rel) != 0) {
        return -1;
    }

    char candidate[PATH_MAX];
    if (clean_rel[0] == '\0') {
        snprintf(candidate, sizeof candidate, "%s", root);
    } else {
        if (snprintf(candidate, sizeof candidate, "%s/%s", root, clean_rel) >= (int)sizeof candidate) {
            return -1;
        }
    }

    struct stat st;
    if (lstat(candidate, &st) == 0) {
        char real[PATH_MAX];
        if (!realpath(candidate, real)) {
            return -1;
        }
        /* Verify realpath is inside root */
        if (strncmp(real, root, root_len) != 0) {
            return -1;
        }
        if (real[root_len] != '\0' && real[root_len] != '/') {
            return -1;
        }
        if (out_resolved) {
            if (snprintf(out_resolved, res_sz, "%s", real) >= (int)res_sz) return -1;
        }
    } else {
        /* Path does not exist yet (e.g. for mkdir, upload, rename target) */
        char ancestor[PATH_MAX];
        snprintf(ancestor, sizeof ancestor, "%s", candidate);
        while (1) {
            char *slash = strrchr(ancestor, '/');
            if (!slash || slash == ancestor) {
                snprintf(ancestor, sizeof ancestor, "%s", root);
                break;
            }
            *slash = '\0';
            struct stat ast;
            if (stat(ancestor, &ast) == 0) break;
        }

        char real_ancestor[PATH_MAX];
        if (!realpath(ancestor, real_ancestor)) {
            return -1;
        }
        if (strncmp(real_ancestor, root, root_len) != 0) {
            return -1;
        }
        if (real_ancestor[root_len] != '\0' && real_ancestor[root_len] != '/') {
            return -1;
        }
        if (out_resolved) {
            if (snprintf(out_resolved, res_sz, "%s", candidate) >= (int)res_sz) return -1;
        }
    }

    if (out_rel) {
        if (snprintf(out_rel, rel_sz, "%s", clean_rel) >= (int)rel_sz) return -1;
    }
    return 0;
}

static void json_escape(const char *in, char *out, size_t out_sz) {
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 6 < out_sz; p++) {
        if (*p == '"') {
            out[j++] = '\\'; out[j++] = '"';
        } else if (*p == '\\') {
            out[j++] = '\\'; out[j++] = '\\';
        } else if (*p == '\b') {
            out[j++] = '\\'; out[j++] = 'b';
        } else if (*p == '\f') {
            out[j++] = '\\'; out[j++] = 'f';
        } else if (*p == '\n') {
            out[j++] = '\\'; out[j++] = 'n';
        } else if (*p == '\r') {
            out[j++] = '\\'; out[j++] = 'r';
        } else if (*p == '\t') {
            out[j++] = '\\'; out[j++] = 't';
        } else if (*p < 0x20) {
            j += (size_t)snprintf(out + j, out_sz - j, "\\u%04x", *p);
        } else {
            out[j++] = (char)*p;
        }
    }
    out[j] = '\0';
}

char *file_ops_list_json(const char *req_path) {
    char real_path[PATH_MAX];
    char clean_rel[PATH_MAX];
    if (file_ops_resolve(req_path, real_path, sizeof real_path, clean_rel, sizeof clean_rel) != 0) {
        return NULL;
    }

    struct stat st;
    if (stat(real_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return NULL;
    }

    DIR *d = opendir(real_path);
    if (!d) return NULL;

    unsigned long long free_bytes = 0;
    unsigned long long total_bytes = 0;
    struct statvfs vfs;
    if (statvfs(file_ops_get_root(), &vfs) == 0) {
        free_bytes = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        total_bytes = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
    }

    char parent_rel[PATH_MAX] = {0};
    if (clean_rel[0] != '\0') {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", clean_rel);
        char *last = strrchr(tmp, '/');
        if (last) {
            *last = '\0';
            snprintf(parent_rel, sizeof parent_rel, "%s", tmp);
        }
    }

    size_t cap = 16384;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        closedir(d);
        return NULL;
    }

    char esc_path[PATH_MAX * 2], esc_parent[PATH_MAX * 2];
    json_escape(clean_rel, esc_path, sizeof esc_path);
    json_escape(parent_rel, esc_parent, sizeof esc_parent);

    len = (size_t)snprintf(buf, cap,
                           "{\"path\":\"%s\",\"parent\":\"%s\",\"free_bytes\":%llu,\"total_bytes\":%llu,\"entries\":[",
                           esc_path, esc_parent, free_bytes, total_bytes);

    struct dirent *de;
    int first = 1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", real_path, de->d_name) >= (int)sizeof child) continue;

        struct stat cst;
        if (lstat(child, &cst) != 0) continue;

        int is_dir = S_ISDIR(cst.st_mode);
        unsigned long long sz = is_dir ? 0 : (unsigned long long)cst.st_size;
        unsigned long long mtime = (unsigned long long)cst.st_mtime;

        char esc_name[512];
        json_escape(de->d_name, esc_name, sizeof esc_name);

        char entry_buf[1024];
        int elen = snprintf(entry_buf, sizeof entry_buf,
                            "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%llu,\"mtime\":%llu}",
                            first ? "" : ",", esc_name, is_dir ? "true" : "false", sz, mtime);
        first = 0;

        while (len + (size_t)elen + 16 >= cap) {
            cap *= 2;
            char *next = realloc(buf, cap);
            if (!next) {
                free(buf);
                closedir(d);
                return NULL;
            }
            buf = next;
        }
        memcpy(buf + len, entry_buf, (size_t)elen);
        len += (size_t)elen;
    }
    closedir(d);

    if (len + 4 >= cap) {
        cap += 16;
        char *next = realloc(buf, cap);
        if (!next) {
            free(buf);
            return NULL;
        }
        buf = next;
    }
    buf[len++] = ']';
    buf[len++] = '}';
    buf[len++] = '\n';
    buf[len] = '\0';
    return buf;
}

int file_ops_mkdir(const char *req_path) {
    char real_path[PATH_MAX];
    if (file_ops_resolve(req_path, real_path, sizeof real_path, NULL, 0) != 0) {
        return -1;
    }
    return mkdir(real_path, 0755);
}

int file_ops_rename(const char *old_path, const char *new_name) {
    if (!new_name || !is_safe_component(new_name)) return -1;

    char src_real[PATH_MAX], src_rel[PATH_MAX];
    if (file_ops_resolve(old_path, src_real, sizeof src_real, src_rel, sizeof src_rel) != 0) {
        return -1;
    }
    if (src_rel[0] == '\0') return -1; // Cannot rename root

    char dst_rel[PATH_MAX];
    char *last = strrchr(src_rel, '/');
    if (last) {
        *last = '\0';
        snprintf(dst_rel, sizeof dst_rel, "%s/%s", src_rel, new_name);
    } else {
        snprintf(dst_rel, sizeof dst_rel, "%s", new_name);
    }

    char dst_real[PATH_MAX];
    if (file_ops_resolve(dst_rel, dst_real, sizeof dst_real, NULL, 0) != 0) {
        return -1;
    }

    return rename(src_real, dst_real);
}

static int copy_single_file(const char *src, const char *dst) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;

    struct stat st;
    mode_t mode = 0644;
    if (fstat(sfd, &st) == 0) mode = st.st_mode & 0777;

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld", dst, (long)getpid()) >= (int)sizeof tmp) {
        close(sfd);
        return -1;
    }

    int dfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dfd < 0) {
        close(sfd);
        return -1;
    }

    char buf[16384];
    ssize_t r;
    while ((r = read(sfd, buf, sizeof buf)) > 0) {
        char *p = buf;
        ssize_t left = r;
        while (left > 0) {
            ssize_t w = write(dfd, p, (size_t)left);
            if (w < 0) {
                if (errno == EINTR) continue;
                close(sfd);
                close(dfd);
                unlink(tmp);
                return -1;
            }
            left -= w;
            p += w;
        }
    }
    close(sfd);
    if (r < 0) {
        close(dfd);
        unlink(tmp);
        return -1;
    }

#if defined(__APPLE__) && defined(F_FULLFSYNC)
    fcntl(dfd, F_FULLFSYNC);
#else
    fsync(dfd);
#endif
    close(dfd);

    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int copy_recursive(const char *src, const char *dst, int depth) {
    if (depth > 32) return -1;
    struct stat st;
    if (lstat(src, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0755) != 0 && errno != EEXIST) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char c_src[PATH_MAX], c_dst[PATH_MAX];
            if (snprintf(c_src, sizeof c_src, "%s/%s", src, de->d_name) >= (int)sizeof c_src ||
                snprintf(c_dst, sizeof c_dst, "%s/%s", dst, de->d_name) >= (int)sizeof c_dst) {
                rc = -1;
                break;
            }
            if (copy_recursive(c_src, c_dst, depth + 1) != 0) {
                rc = -1;
                break;
            }
        }
        closedir(d);
        return rc;
    } else if (S_ISREG(st.st_mode)) {
        return copy_single_file(src, dst);
    }
    return 0;
}

int file_ops_copy(const char **items, size_t count, const char *dest_dir) {
    char dst_dir_real[PATH_MAX], dst_dir_rel[PATH_MAX];
    if (file_ops_resolve(dest_dir, dst_dir_real, sizeof dst_dir_real, dst_dir_rel, sizeof dst_dir_rel) != 0) {
        return -1;
    }
    struct stat dst_st;
    if (stat(dst_dir_real, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        if (!items[i] || !items[i][0]) continue;
        char src_real[PATH_MAX], src_rel[PATH_MAX];
        if (file_ops_resolve(items[i], src_real, sizeof src_real, src_rel, sizeof src_rel) != 0) {
            return -1;
        }
        if (src_rel[0] == '\0') continue; // Skip root

        const char *base = strrchr(src_rel, '/');
        base = base ? base + 1 : src_rel;

        char target_real[PATH_MAX];
        if (snprintf(target_real, sizeof target_real, "%s/%s", dst_dir_real, base) >= (int)sizeof target_real) {
            return -1;
        }

        /* Prevent copying directory into itself */
        size_t src_len = strlen(src_real);
        if (strncmp(target_real, src_real, src_len) == 0 &&
            (target_real[src_len] == '/' || target_real[src_len] == '\0')) {
            return -1;
        }

        if (copy_recursive(src_real, target_real, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

int file_ops_move(const char **items, size_t count, const char *dest_dir) {
    char dst_dir_real[PATH_MAX], dst_dir_rel[PATH_MAX];
    if (file_ops_resolve(dest_dir, dst_dir_real, sizeof dst_dir_real, dst_dir_rel, sizeof dst_dir_rel) != 0) {
        return -1;
    }
    struct stat dst_st;
    if (stat(dst_dir_real, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        if (!items[i] || !items[i][0]) continue;
        char src_real[PATH_MAX], src_rel[PATH_MAX];
        if (file_ops_resolve(items[i], src_real, sizeof src_real, src_rel, sizeof src_rel) != 0) {
            return -1;
        }
        if (src_rel[0] == '\0') continue;

        const char *base = strrchr(src_rel, '/');
        base = base ? base + 1 : src_rel;

        char target_real[PATH_MAX];
        if (snprintf(target_real, sizeof target_real, "%s/%s", dst_dir_real, base) >= (int)sizeof target_real) {
            return -1;
        }

        if (strcmp(src_real, target_real) == 0) continue;

        /* Prevent moving directory into itself */
        size_t src_len = strlen(src_real);
        if (strncmp(target_real, src_real, src_len) == 0 &&
            (target_real[src_len] == '/' || target_real[src_len] == '\0')) {
            return -1;
        }

        if (rename(src_real, target_real) == 0) continue;

        if (errno == EXDEV) {
            if (copy_recursive(src_real, target_real, 0) == 0) {
                file_ops_delete(&items[i], 1);
                continue;
            }
        }
        return -1;
    }
    return 0;
}

static int rm_tree_recursive(const char *path, int depth) {
    if (depth > 32) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return unlink(path);

    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child) {
            rc = -1;
            break;
        }
        if (rm_tree_recursive(child, depth + 1) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    if (rc != 0) return -1;
    return rmdir(path);
}

int file_ops_delete(const char **items, size_t count) {
    if (count == 0 || !items) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!items[i] || !items[i][0]) return -1;
        char real_path[PATH_MAX], rel_path[PATH_MAX];
        if (file_ops_resolve(items[i], real_path, sizeof real_path, rel_path, sizeof rel_path) != 0) {
            return -1;
        }
        if (rel_path[0] == '\0') {
            /* Forbid deleting root directory */
            return -1;
        }
        if (rm_tree_recursive(real_path, 0) != 0) {
            return -1;
        }
    }
    return 0;
}
