#include "icon.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_PNM
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

enum { ICON_MAX_SIDE = 1024, RAW_W = 320, RAW_H = 240 };

/* A headerless 320x240 screen dump, as apps write to /dev/fb0: 4 bytes per
   pixel XRGB (little-endian 0x00RRGGBB), 3 bytes RGB, or 2 bytes RGB565.
   The caller frees the result. NULL when the file is not one of those. */
static unsigned char *load_raw_screen(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  int bpp = 0;
  if (len == (long)RAW_W * RAW_H * 4)
    bpp = 4;
  else if (len == (long)RAW_W * RAW_H * 3)
    bpp = 3;
  else if (len == (long)RAW_W * RAW_H * 2)
    bpp = 2;
  if (!bpp || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  unsigned char *src = malloc((size_t)len);
  unsigned char *rgba = malloc((size_t)RAW_W * RAW_H * 4);
  if (!src || !rgba || fread(src, 1, (size_t)len, f) != (size_t)len) {
    free(src);
    free(rgba);
    fclose(f);
    return NULL;
  }
  fclose(f);
  for (int i = 0; i < RAW_W * RAW_H; i++) {
    const unsigned char *p = src + (size_t)i * (size_t)bpp;
    unsigned char *o = rgba + (size_t)i * 4;
    if (bpp == 4) {
      o[0] = p[2];
      o[1] = p[1];
      o[2] = p[0];
    } else if (bpp == 3) {
      o[0] = p[0];
      o[1] = p[1];
      o[2] = p[2];
    } else {
      unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
      o[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
      o[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
      o[2] = (unsigned char)((v & 31) * 255 / 31);
    }
    o[3] = 255;
  }
  free(src);
  return rgba;
}

int icon_load(const char *path, uint32_t *px, int size) {
  int w = 0, h = 0, n = 0;
  unsigned char *rgba = NULL;
  int from_stb = 0;
  if (!path || !px || size < 1)
    return -1;
  if (stbi_info(path, &w, &h, &n)) {
    if (w < 1 || h < 1 || w > ICON_MAX_SIDE || h > ICON_MAX_SIDE)
      return -1;
    rgba = stbi_load(path, &w, &h, &n, 4);
    from_stb = 1;
  } else {
    rgba = load_raw_screen(path);
    w = RAW_W;
    h = RAW_H;
  }
  if (!rgba)
    return -1;

  /* Fit the picture inside the square. */
  int dw, dh;
  if (w >= h) {
    dw = size;
    dh = h * size / w;
  } else {
    dh = size;
    dw = w * size / h;
  }
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;
  int ox = (size - dw) / 2;
  int oy = (size - dh) / 2;
  memset(px, 0, (size_t)size * (size_t)size * sizeof *px);

  for (int y = 0; y < dh; y++) {
    int sy0 = y * h / dh;
    int sy1 = (y + 1) * h / dh;
    if (sy1 <= sy0)
      sy1 = sy0 + 1;
    if (sy1 > h)
      sy1 = h;
    for (int x = 0; x < dw; x++) {
      int sx0 = x * w / dw;
      int sx1 = (x + 1) * w / dw;
      if (sx1 <= sx0)
        sx1 = sx0 + 1;
      if (sx1 > w)
        sx1 = w;
      /* Average with premultiplied alpha so transparent edges stay clean. */
      uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
      int count = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        const unsigned char *row = rgba + ((size_t)sy * (size_t)w + (size_t)sx0) * 4;
        for (int sx = sx0; sx < sx1; sx++, row += 4) {
          unsigned a = row[3];
          sr += (uint64_t)row[0] * a;
          sg += (uint64_t)row[1] * a;
          sb += (uint64_t)row[2] * a;
          sa += a;
          count++;
        }
      }
      uint32_t out = 0;
      if (sa > 0 && count > 0) {
        uint32_t a = (uint32_t)(sa / (uint64_t)count);
        uint32_t r = (uint32_t)(sr / sa);
        uint32_t g = (uint32_t)(sg / sa);
        uint32_t b = (uint32_t)(sb / sa);
        out = (a << 24) | (r << 16) | (g << 8) | b;
      }
      px[(oy + y) * size + ox + x] = out;
    }
  }
  if (from_stb)
    stbi_image_free(rgba);
  else
    free(rgba);
  return 0;
}

void icon_blit(struct ui *u, int x, int y, const uint32_t *px, int size, uint32_t bg) {
  if (!u || !u->px || !px || size < 1)
    return;
  for (int j = 0; j < size; j++) {
    int yy = y + j;
    if (yy < 0 || yy >= UI_H)
      continue;
    for (int i = 0; i < size; i++) {
      int xx = x + i;
      if (xx < 0 || xx >= UI_W)
        continue;
      uint32_t c = px[j * size + i];
      uint32_t a = c >> 24;
      uint32_t out;
      if (a == 0)
        out = bg;
      else if (a == 255)
        out = c & 0xffffff;
      else {
        uint32_t ia = 255 - a;
        uint32_t r = (((c >> 16) & 255) * a + ((bg >> 16) & 255) * ia) / 255;
        uint32_t g = (((c >> 8) & 255) * a + ((bg >> 8) & 255) * ia) / 255;
        uint32_t b = ((c & 255) * a + (bg & 255) * ia) / 255;
        out = (r << 16) | (g << 8) | b;
      }
      u->px[yy * UI_W + xx] = out;
    }
  }
}
