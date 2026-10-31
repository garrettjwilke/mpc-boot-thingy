#ifndef FILE_OPS_H
#define FILE_OPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize file operations. If data_root is NULL, looks for /data/UserContent,
   MPC_DATA_DIR env, or fallback. Returns 0 on success. */
int file_ops_init(const char *data_root);

/* Get the current resolved root directory path. */
const char *file_ops_get_root(void);

/* Check and resolve a relative path inside the sandboxed data root.
   Returns 0 on success, -1 on invalid path or sandbox escape.
   out_resolved: resolved realpath on disk
   out_rel: normalized clean path relative to data root (e.g. "samples/kick.wav") */
int file_ops_resolve(const char *req_path, char *out_resolved, size_t res_sz, char *out_rel, size_t rel_sz);

/* Produce JSON string listing entries in rel_path.
   Caller must free() the returned pointer. Returns NULL on error. */
char *file_ops_list_json(const char *req_path);

/* Create directory at req_path. Returns 0 on success, -1 on error. */
int file_ops_mkdir(const char *req_path);

/* Rename an entry from old_path to new_name (within same parent or dst_path).
   Returns 0 on success, -1 on error. */
int file_ops_rename(const char *old_path, const char *new_name);

/* Move one or more entries (relative paths) into dest_dir (relative path).
   Returns 0 on success, -1 on error. */
int file_ops_move(const char **items, size_t count, const char *dest_dir);

/* Copy one or more entries (relative paths) into dest_dir (relative path) recursively.
   Returns 0 on success, -1 on error. */
int file_ops_copy(const char **items, size_t count, const char *dest_dir);

/* Delete one or more entries (relative paths) recursively.
   Returns 0 on success, -1 on error. */
int file_ops_delete(const char **items, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* FILE_OPS_H */
