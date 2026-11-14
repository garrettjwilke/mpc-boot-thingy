#pragma once
#include <stdint.h>

struct ui;

/* Decode a picture and fit it into a size-by-size square, keeping its aspect
   ratio and centering it. PNG and binary PPM/PGM are read by their headers.
   A headerless file whose size is exactly a 320x240 screen at 4 (XRGB), 3
   (RGB) or 2 (RGB565) bytes per pixel is read as a framebuffer dump. The result is 0xAARRGGBB with straight alpha.
   Pixels outside the picture are transparent. Shrinking averages source
   pixels; growing repeats them. A picture larger than 1024 on a side is
   rejected. Returns 0 on success, -1 otherwise. px must hold size * size. */
int icon_load(const char *path, uint32_t *px, int size);

/* Composite a size-by-size 0xAARRGGBB icon onto the framebuffer at x,y
   over the color bg. Pixels off the screen are skipped. */
void icon_blit(struct ui *u, int x, int y, const uint32_t *px, int size, uint32_t bg);
