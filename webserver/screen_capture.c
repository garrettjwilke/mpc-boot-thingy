#include "screen_capture.h"
#include "png.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/fb.h>
#include <sys/ioctl.h>
#endif

/* Compile lodepng in pure C encoder-only mode with warnings suppressed. */
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#define LODEPNG_NO_COMPILE_ERROR_TEXT
#define LODEPNG_NO_COMPILE_DECODER
#define LODEPNG_NO_COMPILE_CPP

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#include "../deps/lodepng/lodepng.cpp"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef __linux__

#define DRM_IOCTL_BASE 'd'
#define DRM_IO(nr) _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOW(nr, type) _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)

#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2

#define DRM_FORMAT_RGB565 0x36314752U   /* 'R','G','1','6' */
#define DRM_FORMAT_XRGB8888 0x34325258U /* 'X','R','2','4' */
#define DRM_FORMAT_ARGB8888 0x34325241U /* 'A','R','2','4' */
#define DRM_FORMAT_XBGR8888 0x34324258U /* 'X','B','2','4' */
#define DRM_FORMAT_ABGR8888 0x34324241U /* 'A','B','2','4' */

struct drm_gem_close {
  uint32_t handle;
  uint32_t pad;
};

struct drm_set_client_cap {
  uint64_t capability;
  uint64_t value;
};

struct drm_prime_handle {
  uint32_t handle;
  uint32_t flags;
  int32_t fd;
};

struct drm_mode_card_res {
  uint64_t fb_id_ptr;
  uint64_t crtc_id_ptr;
  uint64_t connector_id_ptr;
  uint64_t encoder_id_ptr;
  uint32_t count_fbs;
  uint32_t count_crtcs;
  uint32_t count_connectors;
  uint32_t count_encoders;
  uint32_t min_width;
  uint32_t max_width;
  uint32_t min_height;
  uint32_t max_height;
};

struct drm_mode_modeinfo {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh;
  uint32_t flags;
  uint32_t type;
  char name[32];
};

struct drm_mode_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors;
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t x, y;
  uint32_t gamma_size;
  uint32_t mode_valid;
  struct drm_mode_modeinfo mode;
};

struct drm_mode_get_plane_res {
  uint64_t plane_id_ptr;
  uint32_t count_planes;
};

struct drm_mode_get_plane {
  uint32_t plane_id;
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t possible_crtcs;
  uint32_t gamma_size;
  uint32_t count_format_types;
  uint64_t format_type_ptr;
};

struct drm_mode_fb_cmd {
  uint32_t fb_id;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t bpp;
  uint32_t depth;
  uint32_t handle;
};

struct drm_mode_fb_cmd2 {
  uint32_t fb_id;
  uint32_t width;
  uint32_t height;
  uint32_t pixel_format;
  uint32_t flags;
  uint32_t handles[4];
  uint32_t pitches[4];
  uint32_t offsets[4];
  uint64_t modifier[4];
};

struct drm_mode_map_dumb {
  uint32_t handle;
  uint32_t pad;
  uint64_t offset;
};

#define DRM_IOCTL_GEM_CLOSE DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_SET_CLIENT_CAP DRM_IOW(0x0d, struct drm_set_client_cap)
#define DRM_IOCTL_DROP_MASTER DRM_IO(0x1f)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETFB DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_MAP_DUMB DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_GETPLANERESOURCES                                       \
  DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_GETFB2 DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)

struct dma_buf_sync {
  uint64_t flags;
};
#define DMA_BUF_SYNC_READ (1ULL << 0)
#define DMA_BUF_SYNC_START (0ULL << 2)
#define DMA_BUF_SYNC_END (1ULL << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

#endif /* __linux__ */

struct screen_capturer {
#ifdef __linux__
  int drm_fd;
  int fb_fd;
  void *fb_map;
  size_t fb_map_size;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pitch;
  uint32_t fb_bpp;
#endif
  int mock_loaded;
  uint32_t mock_px[MPC_SCREEN_W * MPC_SCREEN_H];
};

static void set_source(char *out, size_t sz, const char *val) {
  if (!out || sz == 0)
    return;
  snprintf(out, sz, "%s", val ? val : "");
}

#ifdef __linux__

static int drm_ioctl_retry(int fd, unsigned long req, void *arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

static void close_gem_handles(int drm_fd, uint32_t handles[4]) {
  if (drm_fd < 0)
    return;
  for (int i = 0; i < 4; i++) {
    uint32_t h = handles[i];
    if (!h)
      continue;
    int dup_handle = 0;
    for (int j = 0; j < i; j++) {
      if (handles[j] == h) {
        dup_handle = 1;
        break;
      }
    }
    if (!dup_handle) {
      struct drm_gem_close gc;
      memset(&gc, 0, sizeof gc);
      gc.handle = h;
      drm_ioctl_retry(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    handles[i] = 0;
  }
}

/* Collect all candidate active fb_ids: CRTCs first, then universal planes
 * in reverse order (newest/topmost plane first) so overlays or newly bound
 * planes are checked. */
static int find_active_fb_ids(int drm_fd, uint32_t *out_ids, int max_ids) {
  int count = 0;
  struct drm_mode_card_res res;
  memset(&res, 0, sizeof res);
  if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 &&
      res.count_crtcs > 0 && res.count_crtcs <= 32) {
    uint32_t crtcs[32];
    uint32_t n_crtcs = res.count_crtcs;
    memset(&res, 0, sizeof res);
    res.count_crtcs = n_crtcs;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
      for (uint32_t i = 0; i < res.count_crtcs && i < n_crtcs; i++) {
        struct drm_mode_crtc crtc;
        memset(&crtc, 0, sizeof crtc);
        crtc.crtc_id = crtcs[i];
        if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0 &&
            crtc.fb_id != 0 && count < max_ids) {
          out_ids[count++] = crtc.fb_id;
        }
      }
    }
  }

  struct drm_set_client_cap cap;
  memset(&cap, 0, sizeof cap);
  cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
  cap.value = 1;
  drm_ioctl_retry(drm_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);

  struct drm_mode_get_plane_res pres;
  memset(&pres, 0, sizeof pres);
  if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0 &&
      pres.count_planes > 0 && pres.count_planes <= 64) {
    uint32_t planes[64];
    uint32_t n_planes = pres.count_planes;
    memset(&pres, 0, sizeof pres);
    pres.count_planes = n_planes;
    pres.plane_id_ptr = (uintptr_t)planes;
    if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0) {
      for (uint32_t i = 0; i < pres.count_planes && i < n_planes; i++) {
        struct drm_mode_get_plane pl;
        memset(&pl, 0, sizeof pl);
        pl.plane_id = planes[i];
        if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETPLANE, &pl) == 0 &&
            pl.fb_id != 0 && count < max_ids) {
          int seen = 0;
          for (int k = 0; k < count; k++) {
            if (out_ids[k] == pl.fb_id) {
              seen = 1;
              break;
            }
          }
          if (!seen)
            out_ids[count++] = pl.fb_id;
        }
      }
    }
  }

  return count;
}

static uint32_t rgb565_to_xrgb8888(uint16_t v) {
  uint32_t r = (v >> 11) & 0x1f;
  uint32_t g = (v >> 5) & 0x3f;
  uint32_t b = v & 0x1f;
  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);
  return (r << 16) | (g << 8) | b;
}

static void convert_scanlines_to_screen(const unsigned char *base,
                                        uint32_t src_w, uint32_t src_h,
                                        uint32_t pitch, uint32_t format,
                                        uint32_t *px_out) {
  memset(px_out, 0, (size_t)MPC_SCREEN_W * MPC_SCREEN_H * sizeof(uint32_t));
  uint32_t copy_w = src_w < MPC_SCREEN_W ? src_w : MPC_SCREEN_W;
  uint32_t copy_h = src_h < MPC_SCREEN_H ? src_h : MPC_SCREEN_H;

  for (uint32_t y = 0; y < copy_h; y++) {
    const unsigned char *row = base + (size_t)y * pitch;
    uint32_t *dst = px_out + (size_t)y * MPC_SCREEN_W;
    if (format == DRM_FORMAT_RGB565) {
      const uint16_t *src16 = (const uint16_t *)row;
      for (uint32_t x = 0; x < copy_w; x++) {
        dst[x] = rgb565_to_xrgb8888(src16[x]);
      }
    } else if (format == DRM_FORMAT_XBGR8888 || format == DRM_FORMAT_ABGR8888) {
      for (uint32_t x = 0; x < copy_w; x++) {
        const unsigned char *p = row + x * 4;
        uint32_t r = p[0], g = p[1], b = p[2];
        dst[x] = (r << 16) | (g << 8) | b;
      }
    } else {
      /* Default XRGB8888 / ARGB8888 (little-endian [B, G, R, X/A]). */
      for (uint32_t x = 0; x < copy_w; x++) {
        const unsigned char *p = row + x * 4;
        uint32_t b = p[0], g = p[1], r = p[2];
        dst[x] = (r << 16) | (g << 8) | b;
      }
    }
  }
}

static int is_all_black(const uint32_t *px) {
  for (int i = 0; i < MPC_SCREEN_W * MPC_SCREEN_H; i++) {
    if ((px[i] & 0x00ffffffU) != 0)
      return 0;
  }
  return 1;
}

/* Map a specific fb_id, copy its pixels into px_out, and immediately unmap
 * and close the GEM handles so we never hold stale buffers across app switches
 * (when the kernel reuses the same fb_id integer for a new process). */
static int grab_drm_fb_once(int drm_fd, uint32_t fb_id, uint32_t *px_out) {
  uint32_t handles[4] = {0, 0, 0, 0};
  uint32_t width = 0, height = 0, pitch = 0, offset = 0, format = 0;

  struct drm_mode_fb_cmd2 fb2;
  memset(&fb2, 0, sizeof fb2);
  fb2.fb_id = fb_id;
  if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETFB2, &fb2) == 0 &&
      fb2.handles[0] != 0) {
    for (int i = 0; i < 4; i++)
      handles[i] = fb2.handles[i];
    width = fb2.width;
    height = fb2.height;
    pitch = fb2.pitches[0];
    offset = fb2.offsets[0];
    format = fb2.pixel_format;
  } else {
    struct drm_mode_fb_cmd fb1;
    memset(&fb1, 0, sizeof fb1);
    fb1.fb_id = fb_id;
    if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_GETFB, &fb1) != 0 ||
        fb1.handle == 0) {
      return -1;
    }
    handles[0] = fb1.handle;
    width = fb1.width;
    height = fb1.height;
    pitch = fb1.pitch;
    offset = 0;
    format = (fb1.bpp == 16) ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888;
  }

  if (width == 0 || height == 0 || pitch == 0) {
    close_gem_handles(drm_fd, handles);
    return -1;
  }

  size_t need_size = (size_t)offset + (size_t)pitch * (size_t)height;
  void *map_ptr = MAP_FAILED;
  int dmabuf_fd = -1;

  struct drm_prime_handle prime;
  memset(&prime, 0, sizeof prime);
  prime.handle = handles[0];
  prime.flags = O_CLOEXEC;
  prime.fd = -1;
  if (drm_ioctl_retry(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0 &&
      prime.fd >= 0) {
    map_ptr = mmap(NULL, need_size, PROT_READ, MAP_SHARED, prime.fd, 0);
    if (map_ptr != MAP_FAILED) {
      dmabuf_fd = prime.fd;
    } else {
      close(prime.fd);
    }
  }

  if (map_ptr == MAP_FAILED) {
    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = handles[0];
    if (drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0) {
      map_ptr = mmap(NULL, need_size, PROT_READ, MAP_SHARED, drm_fd, mreq.offset);
    }
  }

  if (map_ptr == MAP_FAILED) {
    close_gem_handles(drm_fd, handles);
    return -1;
  }

  if (dmabuf_fd >= 0) {
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    drm_ioctl_retry(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
  }

  const unsigned char *base = (const unsigned char *)map_ptr + offset;
  convert_scanlines_to_screen(base, width, height, pitch, format, px_out);

  if (dmabuf_fd >= 0) {
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
    drm_ioctl_retry(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
  }

  munmap(map_ptr, need_size);
  if (dmabuf_fd >= 0)
    close(dmabuf_fd);
  close_gem_handles(drm_fd, handles);
  return 0;
}

static int grab_drm(struct screen_capturer *cap, uint32_t *px_out) {
  if (cap->drm_fd < 0) {
    cap->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (cap->drm_fd < 0)
      cap->drm_fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
    if (cap->drm_fd < 0)
      return -1;
  }
  /* Never hold DRM master so mpc-boot-thingy and other apps can always call
   * DRM_IOCTL_SET_MASTER without EBUSY. */
  drm_ioctl_retry(cap->drm_fd, DRM_IOCTL_DROP_MASTER, 0);

  uint32_t fb_ids[16];
  int n_fbs = find_active_fb_ids(cap->drm_fd, fb_ids, 16);
  if (n_fbs <= 0)
    return -1;

  int got_any = 0;
  for (int i = 0; i < n_fbs; i++) {
    if (grab_drm_fb_once(cap->drm_fd, fb_ids[i], px_out) == 0) {
      got_any = 1;
      if (!is_all_black(px_out))
        return 0;
    }
  }
  return got_any ? 0 : -1;
}

static int grab_fb0(struct screen_capturer *cap, uint32_t *px_out) {
  if (cap->fb_fd < 0) {
    cap->fb_fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (cap->fb_fd < 0)
      return -1;

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    memset(&finfo, 0, sizeof finfo);
    memset(&vinfo, 0, sizeof vinfo);
    if (ioctl(cap->fb_fd, FBIOGET_FSCREENINFO, &finfo) != 0 ||
        ioctl(cap->fb_fd, FBIOGET_VSCREENINFO, &vinfo) != 0 ||
        finfo.smem_len == 0 || vinfo.xres == 0 || vinfo.yres == 0) {
      close(cap->fb_fd);
      cap->fb_fd = -1;
      return -1;
    }
    void *ptr =
        mmap(NULL, finfo.smem_len, PROT_READ, MAP_SHARED, cap->fb_fd, 0);
    if (ptr == MAP_FAILED) {
      close(cap->fb_fd);
      cap->fb_fd = -1;
      return -1;
    }
    cap->fb_map = ptr;
    cap->fb_map_size = finfo.smem_len;
    cap->fb_width = vinfo.xres;
    cap->fb_height = vinfo.yres;
    cap->fb_pitch = finfo.line_length ? finfo.line_length
                                      : vinfo.xres * (vinfo.bits_per_pixel / 8);
    cap->fb_bpp = vinfo.bits_per_pixel;
  }

  uint32_t fmt =
      (cap->fb_bpp == 16) ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888;
  convert_scanlines_to_screen((const unsigned char *)cap->fb_map, cap->fb_width,
                              cap->fb_height, cap->fb_pitch, fmt, px_out);
  return 0;
}

#endif /* __linux__ */

static void fill_mock_screen(struct screen_capturer *cap, uint32_t *px_out) {
  if (!cap->mock_loaded) {
    const char *env_png = getenv("MPC_SCREEN_PNG");
    if (env_png && env_png[0] && png_load_screen(env_png, cap->mock_px) == 0) {
      cap->mock_loaded = 1;
    } else if (png_load_screen("splash-screen.png", cap->mock_px) == 0) {
      cap->mock_loaded = 1;
    } else {
      const char *autorun = getenv("AUTORUN");
      char path[1024];
      if (autorun && autorun[0]) {
        snprintf(path, sizeof path, "%s/system/splash-screen.png", autorun);
        if (png_load_screen(path, cap->mock_px) == 0) {
          cap->mock_loaded = 1;
        }
      }
    }
    if (!cap->mock_loaded) {
      for (int y = 0; y < MPC_SCREEN_H; y++) {
        for (int x = 0; x < MPC_SCREEN_W; x++) {
          uint32_t col = 0x0d0f12U;
          if (x < 2 || x >= MPC_SCREEN_W - 2 || y < 2 ||
              y >= MPC_SCREEN_H - 2) {
            col = 0xe8453cU;
          } else if (y == 28) {
            col = 0x262c35U;
          } else if ((x % 32 == 0 || y % 32 == 0) && y > 28) {
            col = 0x161a20U;
          }
          cap->mock_px[y * MPC_SCREEN_W + x] = col;
        }
      }
      cap->mock_loaded = 1;
    }
  }
  memcpy(px_out, cap->mock_px,
         (size_t)MPC_SCREEN_W * MPC_SCREEN_H * sizeof(uint32_t));
}

struct screen_capturer *screen_capturer_open(void) {
  struct screen_capturer *cap = calloc(1, sizeof(*cap));
  if (!cap)
    return NULL;
#ifdef __linux__
  cap->drm_fd = -1;
  cap->fb_fd = -1;
#endif
  return cap;
}

int screen_capturer_grab(struct screen_capturer *cap, uint32_t *px_out,
                         char *source_out, size_t source_sz) {
  if (!cap || !px_out)
    return -1;

#ifdef __linux__
  if (grab_drm(cap, px_out) == 0) {
    /* If the active DRM FB is completely black (e.g. an fbdev app is drawing
     * directly to /dev/fb0 instead), check /dev/fb0 as a fallback. */
    if (is_all_black(px_out)) {
      uint32_t fb_px[MPC_SCREEN_W * MPC_SCREEN_H];
      if (grab_fb0(cap, fb_px) == 0 && !is_all_black(fb_px)) {
        memcpy(px_out, fb_px, sizeof fb_px);
        set_source(source_out, source_sz, "fb0");
        return 0;
      }
    }
    set_source(source_out, source_sz, "drm");
    return 0;
  }
  if (grab_fb0(cap, px_out) == 0) {
    set_source(source_out, source_sz, "fb0");
    return 0;
  }
#endif

  fill_mock_screen(cap, px_out);
  set_source(source_out, source_sz, "mock");
  return 0;
}

void screen_capturer_close(struct screen_capturer *cap) {
  if (!cap)
    return;
#ifdef __linux__
  if (cap->drm_fd >= 0) {
    drm_ioctl_retry(cap->drm_fd, DRM_IOCTL_DROP_MASTER, 0);
    close(cap->drm_fd);
    cap->drm_fd = -1;
  }
  if (cap->fb_map && cap->fb_map != MAP_FAILED) {
    munmap(cap->fb_map, cap->fb_map_size);
    cap->fb_map = NULL;
  }
  if (cap->fb_fd >= 0) {
    close(cap->fb_fd);
    cap->fb_fd = -1;
  }
#endif
  free(cap);
}

int screen_capture_pixels(uint32_t *px_out, char *source_out,
                          size_t source_sz) {
  struct screen_capturer *cap = screen_capturer_open();
  if (!cap)
    return -1;
  int rc = screen_capturer_grab(cap, px_out, source_out, source_sz);
  screen_capturer_close(cap);
  return rc;
}

int screen_encode_png(const uint32_t *px, int width, int height,
                      unsigned char **out_buf, size_t *out_len) {
  if (!px || width <= 0 || height <= 0 || !out_buf || !out_len)
    return -1;

  size_t npix = (size_t)width * (size_t)height;
  unsigned char *rgb = malloc(npix * 3);
  if (!rgb)
    return -1;

  for (size_t i = 0; i < npix; i++) {
    uint32_t p = px[i];
    rgb[i * 3 + 0] = (unsigned char)((p >> 16) & 0xff);
    rgb[i * 3 + 1] = (unsigned char)((p >> 8) & 0xff);
    rgb[i * 3 + 2] = (unsigned char)(p & 0xff);
  }

  *out_buf = NULL;
  *out_len = 0;
  unsigned err = lodepng_encode24(out_buf, out_len, rgb, (unsigned)width,
                                  (unsigned)height);
  free(rgb);
  if (err != 0) {
    free(*out_buf);
    *out_buf = NULL;
    *out_len = 0;
    return -1;
  }
  return 0;
}

int screen_capture_png(unsigned char **out_buf, size_t *out_len,
                       char *source_out, size_t source_sz) {
  uint32_t *px = malloc((size_t)MPC_SCREEN_W * MPC_SCREEN_H * sizeof(uint32_t));
  if (!px)
    return -1;
  if (screen_capture_pixels(px, source_out, source_sz) != 0) {
    free(px);
    return -1;
  }
  int rc = screen_encode_png(px, MPC_SCREEN_W, MPC_SCREEN_H, out_buf, out_len);
  free(px);
  return rc;
}
