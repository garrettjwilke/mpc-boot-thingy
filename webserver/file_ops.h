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

/* Get the Internal volume path (/data/UserContent or the configured root). */
const char *file_ops_get_root(void);

/* Pin the SD card volume for tests. NULL or "" clears the pin and uses
   auto-detection. Returns 0 on success. */
int file_ops_set_sd_root(const char *sd_root);

/* Check and resolve a path under Internal ("internal/...") or the SD card ("sd/...").
   The empty path is the virtual storage root and does not resolve to a directory.
   Returns 0 on success, -1 on invalid path, missing SD card, or sandbox escape.
   out_resolved: path on disk
   out_rel: normalized path including the volume id (e.g. "internal/samples/kick.wav") */
int file_ops_resolve(const char *req_path, char *out_resolved, size_t res_sz, char *out_rel, size_t rel_sz);

/* "SD card not mounted" when req_path is on the SD volume and no card is present.
   NULL otherwise. */
const char *file_ops_access_error(const char *req_path);

/* Produce JSON string listing entries in rel_path.
   Caller must free() the returned pointer. Returns NULL on error. */
char *file_ops_list_json(const char *req_path);

/* Create directory at req_path. Returns 0 on success, -1 on error. */
int file_ops_mkdir(const char *req_path);

/* Ensure physical directory exists (recursively creates intermediate directories if needed).
   path is a resolved real path on disk. Returns 0 on success, -1 on error. */
int file_ops_ensure_dir(const char *path);

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
