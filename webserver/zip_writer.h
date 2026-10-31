#ifndef ZIP_WRITER_H
#define ZIP_WRITER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exports selected items as a ZIP archive streamed to client_fd.
   If count == 0 or items == NULL, exports the entire /data directory.
   Sends the HTTP response header and streams the ZIP payload.
   Returns 0 on success, -1 on failure. */
int zip_stream_export(int client_fd, const char **items, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ZIP_WRITER_H */
