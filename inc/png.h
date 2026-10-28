#pragma once
#include <stdint.h>

/* Decode a non-interlaced 320x240 8-bit PNG into px as 0x00RRGGBB.
   Alpha is composited over black. Returns 0 on success, -1 otherwise.
   px must hold 320 * 240 pixels. A wrong size or an unsupported file fails. */
int png_load_screen(const char *path, uint32_t *px);
