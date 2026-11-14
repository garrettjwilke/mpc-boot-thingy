#ifndef MPC_WEB_SCREEN_CAPTURE_H
#define MPC_WEB_SCREEN_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#define MPC_SCREEN_W 320
#define MPC_SCREEN_H 240

struct screen_capturer;

/* Open a persistent screen capture session (keeps DRM/fbdev fds & mappings
 * cached across frames for low-overhead 30 FPS streaming). */
struct screen_capturer *screen_capturer_open(void);

/* Grab the current 320x240 screen into px_out (each pixel 0x00RRGGBB).
 * Writes a short source identifier ("drm", "fb0", or "mock") into source_out
 * when non-NULL. Returns 0 on success, -1 on failure. */
int screen_capturer_grab(struct screen_capturer *cap, uint32_t *px_out,
                         char *source_out, size_t source_sz);

/* Close a persistent capture session and release any mapped buffers/handles. */
void screen_capturer_close(struct screen_capturer *cap);

/* Stateless single-frame pixel capture (320x240 0x00RRGGBB). */
int screen_capture_pixels(uint32_t *px_out, char *source_out, size_t source_sz);

/* Encode a width x height 0x00RRGGBB pixel buffer into a compressed 24-bit
 * RGB PNG in memory (via lodepng). Caller must free(*out_buf).
 * Returns 0 on success, -1 on failure. */
int screen_encode_png(const uint32_t *px, int width, int height,
                      unsigned char **out_buf, size_t *out_len);

/* Capture the current 320x240 screen and encode it as a PNG in memory.
 * Caller must free(*out_buf). Returns 0 on success, -1 on failure. */
int screen_capture_png(unsigned char **out_buf, size_t *out_len,
                       char *source_out, size_t source_sz);

#endif

