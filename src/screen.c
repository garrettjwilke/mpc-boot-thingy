/* Screen open, flush, and close copied from mpc-sample-lab/firmware/hw.c.
   /dev/fb0 stays dark while az0x-splash holds DRM, so draw into our own buffer. */
#include "screen.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

uint32_t *hw_fb;
int hw_stride;
static int drm_fd = -1;
static uint32_t fb_id;
static size_t fb_bytes;

static int drm_call(unsigned long req, void *arg, const char *what) {
    int r;
    while ((r = ioctl(drm_fd, req, arg)) < 0 && errno == EINTR) {}
    if (r < 0) fprintf(stderr, "drm %s: %s\n", what, strerror(errno));
    return r;
}

void hw_screen_open(void) {
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("/dev/dri/card0"); return; }
    /* The previous master may still be dropping its mapping. */
    for (int i = 0; i < 50; i++) {
        if (ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0) == 0) break;
        if (errno != EBUSY || i == 49) fprintf(stderr, "drm set master: %s\n", strerror(errno));
        if (errno != EBUSY) break;
        usleep(20 * 1000);
    }
    uint32_t crtcs[4], conns[4];
    struct drm_mode_card_res res = {.crtc_id_ptr = (uintptr_t)crtcs, .connector_id_ptr = (uintptr_t)conns,
                                    .count_crtcs = 4, .count_connectors = 4};
    if (drm_call(DRM_IOCTL_MODE_GETRESOURCES, &res, "getresources") < 0) return;
    if (!res.count_crtcs || !res.count_connectors) { fputs("drm: no crtc or connector\n", stderr); return; }
    struct drm_mode_modeinfo modes[8];
    struct drm_mode_get_connector con = {.connector_id = conns[0]};
    if (drm_call(DRM_IOCTL_MODE_GETCONNECTOR, &con, "getconnector") < 0) return;
    if (!con.count_modes || con.count_modes > 8) { fprintf(stderr, "drm: %u modes\n", con.count_modes); return; }
    con = (struct drm_mode_get_connector){.connector_id = conns[0], .modes_ptr = (uintptr_t)modes, .count_modes = con.count_modes};
    if (drm_call(DRM_IOCTL_MODE_GETCONNECTOR, &con, "getconnector modes") < 0) return;
    struct drm_mode_modeinfo mode = modes[0];
    if (mode.hdisplay != 320 || mode.vdisplay != 240) { fprintf(stderr, "drm: mode %ux%u\n", mode.hdisplay, mode.vdisplay); return; }
    struct drm_mode_create_dumb dumb = {.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
    if (drm_call(DRM_IOCTL_MODE_CREATE_DUMB, &dumb, "create dumb") < 0) return;
    struct drm_mode_fb_cmd2 add = {.width = dumb.width, .height = dumb.height, .pixel_format = DRM_FORMAT_XRGB8888,
                                   .handles = {dumb.handle}, .pitches = {dumb.pitch}};
    if (drm_call(DRM_IOCTL_MODE_ADDFB2, &add, "addfb2") < 0) return;
    struct drm_mode_map_dumb map = {.handle = dumb.handle};
    if (drm_call(DRM_IOCTL_MODE_MAP_DUMB, &map, "map dumb") < 0) return;
    void *p = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
    if (p == MAP_FAILED) { perror("mmap dumb"); return; }
    memset(p, 0, dumb.size);
    struct drm_mode_crtc set = {.crtc_id = crtcs[0], .fb_id = add.fb_id, .set_connectors_ptr = (uintptr_t)&conns[0],
                                .count_connectors = 1, .mode = mode, .mode_valid = 1};
    if (drm_call(DRM_IOCTL_MODE_SETCRTC, &set, "setcrtc") < 0) return;
    hw_fb = p, hw_stride = (int)(dumb.pitch / 4), fb_bytes = dumb.size, fb_id = add.fb_id;
    printf("drm %ux%u crtc %u connector %u fb %u\n", mode.hdisplay, mode.vdisplay, crtcs[0], conns[0], fb_id);
}

void hw_screen_flush(int x0, int y0, int x1, int y1) {
    struct drm_clip_rect clip = {(unsigned short)x0, (unsigned short)y0, (unsigned short)x1, (unsigned short)y1};
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&clip};
    if (hw_fb && x1 > x0 && y1 > y0) drm_call(DRM_IOCTL_MODE_DIRTYFB, &dirty, "dirtyfb");
}

void hw_screen_close(void) {
    if (hw_fb) memset(hw_fb, 0, fb_bytes), hw_screen_flush(0, 0, 320, 240);
    /* munmap drops the DRM file reference. close() alone leaves this process
       master, so the next program cannot put its own buffer on the panel. */
    if (hw_fb) munmap(hw_fb, fb_bytes);
    hw_fb = NULL;
    if (drm_fd >= 0) {
        ioctl(drm_fd, DRM_IOCTL_DROP_MASTER, 0);
        close(drm_fd);
        drm_fd = -1;
    }
}
