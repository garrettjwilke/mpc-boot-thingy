#pragma once
#include "widgets.h"
#include <stddef.h>

#define UI_USB_APPS_LABEL "Install App (USB)"
#define UI_USB_APPS_HINT "press to drop .zip apps over USB"

/* Return codes from ui_usb_apps(). */
enum {
  UI_USB_APPS_ACTIVE = 0, /* screen is up: preparing, connected, exiting, or error */
  UI_USB_APPS_DONE = 1,   /* user exited; gadget restored, image ready to scan */
  UI_USB_APPS_FAILED = -1 /* start failed and user dismissed the error screen */
};

/* Screen phases (read-only for callers). */
enum {
  UI_USB_APPS_PHASE_PREPARING = 0,
  UI_USB_APPS_PHASE_ACTIVE,
  UI_USB_APPS_PHASE_EXITING,
  UI_USB_APPS_PHASE_ERROR,
  UI_USB_APPS_PHASE_DONE,
  UI_USB_APPS_PHASE_FAILED
};

struct ui_usb_apps_style {
  uint32_t bg, text, dim, hl, box, border, bar, active_text;
};

struct ui_usb_apps {
  int phase;
  int presented;
  int bound;
  char card[256];
  char err[160];
  int soft_active;
  struct ui_soft_state soft;
};

void ui_usb_apps_init(struct ui_usb_apps *s, const char *card);

/* Render one frame of the Install App (USB) screen and handle B1/encoder. */
int ui_usb_apps(struct ui *u, struct ui_usb_apps *s,
                struct ui_usb_apps_style st, const struct input *in);

int ui_usb_apps_is_active(const struct ui_usb_apps *s);

/* Unbind the image and restore the default USB gadget if bound. */
int ui_usb_apps_stop(struct ui_usb_apps *s);

/* Copy non-dotfile *.zip packages from the mounted virtual USB image into
   card/autorun/apps/ and remove the temporary image. Sets *copied_out to
   the number of .zip files staged. Returns 0 on success, -1 on error. */
int usb_apps_collect_zips(const char *card, int *copied_out, char *err,
                          size_t err_n);

/* Helper exposed for host tests: copy valid *.zip files from src_dir to
   card/autorun/apps/. */
int usb_apps_copy_zips_from_dir(const char *src_dir, const char *card,
                                int *copied_out, char *err, size_t err_n);

/* Host test mocks. Non-Linux always uses mock mode. */
void ui_usb_apps_set_mock(int enable, int fail_start);
void ui_usb_apps_set_mock_dir(const char *dir);

