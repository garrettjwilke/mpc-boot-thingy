#include "boot_ui.h"
#include "font.h"
#include "icon.h"
#include "input.h"
#include "theme.h"
#include "usb_sd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

static int64_t mono_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int env_is_truthy(const char *v) {
  if (!v || !v[0])
    return -1;
  if (v[0] == '1')
    return 1;
  if (v[0] == '0')
    return 0;
  if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 ||
      strcmp(v, "on") == 0 || strcmp(v, "connected") == 0)
    return 1;
  if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0 ||
      strcmp(v, "off") == 0 || strcmp(v, "disconnected") == 0)
    return 0;
  return -1;
}

static int g_web_connected_override = -1;

void boot_ui_set_web_connected(int connected) {
  g_web_connected_override = connected;
}

static int web_connected(void) {
  if (g_web_connected_override >= 0)
    return g_web_connected_override;
  static const char *direct_vars[] = {
      "MPC_BOOT_WEB_CONNECTED", "MPC_WEB_CONNECTED",
      "MPC_BOOT_WEB_STATUS", "MPC_WEB_STATUS", NULL};
  static const char *file_vars[] = {
      "MPC_BOOT_WEB_CONNECTED_FILE", "MPC_WEB_CONNECTED_FILE",
      "MPC_BOOT_WEB_STATUS_FILE", "MPC_WEB_STATUS_FILE",
      "MPC_BOOT_WEB_LINK_FILE", "MPC_WEB_LINK_FILE", NULL};
  for (int i = 0; direct_vars[i]; i++) {
    const char *v = getenv(direct_vars[i]);
    int r = env_is_truthy(v);
    if (r >= 0)
      return r;
  }
  for (int i = 0; file_vars[i]; i++) {
    const char *path = getenv(file_vars[i]);
    if (path && path[0]) {
      FILE *f = fopen(path, "r");
      if (f) {
        char buf[64];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        int r = env_is_truthy(buf);
        if (r >= 0)
          return r;
        return 1; /* file exists => connected */
      }
      return 0; /* specified file missing => disconnected */
    }
  }
  /* USB device controller state: configured means the host enumerated us. */
  int udc_any = 0;
  int udc_configured = 0;
  {
    DIR *d = opendir("/sys/class/udc");
    if (d) {
      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
          continue;
        char path[256];
        snprintf(path, sizeof path, "/sys/class/udc/%s/state", de->d_name);
        FILE *sf = fopen(path, "r");
        if (sf) {
          udc_any = 1;
          char buf[64];
          if (fgets(buf, sizeof buf, sf)) {
            if (strstr(buf, "configured"))
              udc_configured = 1;
          }
          fclose(sf);
        }
      }
      closedir(d);
    }
  }

  /* Network interface carrying 192.168.155.1 must be running (carrier up). */
  {
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
      int found = 0;
      int running = 0;
      for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
          continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
        if (sin->sin_addr.s_addr != inet_addr("192.168.155.1"))
          continue;
        found = 1;
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
          struct ifreq ifr;
          memset(&ifr, 0, sizeof ifr);
          snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", p->ifa_name);
          if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (ifr.ifr_flags & IFF_RUNNING)
              running = 1;
          }
          close(sock);
        }
      }
      freeifaddrs(ifa);
      if (found) {
        if (udc_any)
          return udc_configured && running;
        return running;
      }
      if (udc_any)
        return udc_configured;
    }
  }

  /* Real device detection: USB link IP present in routing table. */
  FILE *f = fopen("/proc/net/fib_trie", "r");
  if (!f)
    return 0;
  char line[256];
  int connected = 0;
  while (fgets(line, sizeof line, f)) {
    if (strstr(line, "192.168.155.1")) {
      connected = 1;
      break;
    }
  }
  fclose(f);
  return connected;
}

static int wrap(int v, int n) {
  if (n <= 0)
    return 0;
  v %= n;
  if (v < 0)
    v += n;
  return v;
}

static int clamp_index(int v, int n) {
  if (n <= 0)
    return 0;
  if (v < 0)
    return 0;
  if (v >= n)
    return n - 1;
  return v;
}

static void move_cursor(struct ui_list_state *ls, int n, struct input *ev) {
  if (!ev || !ev->encoder || n <= 0)
    return;
  ls->cursor = clamp_index(ls->cursor + ev->encoder, n);
  ev->encoder = 0;
}

static void row_label(char *dst, size_t n, const char *title, int mark) {
  const char *suffix = mark ? " (default)" : "";
  size_t suf = strlen(suffix);
  size_t max = n > 0 ? n - 1 : 0;
  if (suf > max)
    suf = 0;
  size_t keep = max - suf;
  size_t len = title ? strlen(title) : 0;
  if (len > keep)
    len = keep;
  if (len && title)
    memcpy(dst, title, len);
  if (suf)
    memcpy(dst + len, suffix, suf);
  dst[len + suf] = 0;
}

static void fit_text(char *dst, size_t n, const char *src) {
  size_t len = src ? strlen(src) : 0;
  if (n == 0)
    return;
  if (len >= n)
    len = n - 1;
  if (len && src)
    memcpy(dst, src, len);
  dst[len] = 0;
}

static struct ui_soft_style soft_style(const struct boot_theme *th) {
  struct ui_soft_style st = {
      .fill = th->box,
      .text = th->text,
      .active_text = th->active_text,
      .highlight = th->hl,
      .gap = 4,
      .feedback = UI_SOFT_FLASH,
      .flash_ms = 120,
  };
  return st;
}

static struct ui_list_style list_style(const struct boot_theme *th) {
  struct ui_list_style st = {
      .text = th->text,
      .highlight = th->hl,
      .row_h = 20,
      .highlight_px = 2,
      .current_fill_set = 1,
      .current_fill = th->bar,
  };
  return st;
}

/* ---- apps screen: a grid of tiles ------------------------------------- */

enum {
  APPS_COLS = 4,
  APPS_CELL_H = 80,
  APPS_SCROLL_W = 6,
  APPS_LABEL_LINES = 2,
};

/* Picture for row i, decoded on first use. NULL when the row has no PNG or
   it could not be read, so the caller draws a generated tile. */
static const struct boot_icon *row_icon(struct boot_ui *s, int i) {
  if (!s->menu || i < 0 || i >= MENU_MAX)
    return NULL;
  const struct menu_entry *e = &s->menu->items[i];
  struct boot_icon *ic = &s->icons[i];
  if (!e->icon[0]) {
    ic->path[0] = 0;
    ic->ok = 0;
    return NULL;
  }
  if (strcmp(ic->path, e->icon) != 0) {
    snprintf(ic->path, sizeof ic->path, "%s", e->icon);
    ic->ok = icon_load(e->icon, ic->px, BOOT_ICON_PX) == 0;
  }
  return ic->ok ? ic : NULL;
}

/* Tile color for a title without a picture. The same title always gets the
   same color. */
static uint32_t tile_color(const char *title) {
  static const uint32_t palette[] = {0xd9534f, 0xe08a2e, 0xb8a32c, 0x4caf50,
                                     0x2bb3a3, 0x3b82c4, 0x7a5cc9, 0xd05aa0};
  unsigned h = 5381;
  for (const unsigned char *p = (const unsigned char *)title; p && *p; p++)
    h = h * 33 + *p;
  return palette[h % (sizeof palette / sizeof palette[0])];
}

static uint32_t text_on(uint32_t fill) {
  unsigned r = (fill >> 16) & 255, g = (fill >> 8) & 255, b = fill & 255;
  unsigned luma = (r * 299 + g * 587 + b * 114) / 1000;
  return luma > 150 ? 0x101014 : 0xf4f4f4;
}

static int is_word_sep(char c) {
  return c == ' ' || c == '-' || c == '_' || c == '.' || c == '/';
}

/* Up to two capital letters: the first letter of the first two words, or
   the first two letters of a single word. */
static void initials(const char *title, char *out) {
  int n = 0;
  int at_word = 1;
  int words = 0;
  for (const char *p = title; p && *p && n < 2; p++) {
    if (is_word_sep(*p)) {
      at_word = 1;
      continue;
    }
    if (at_word) {
      words++;
      at_word = 0;
      if (words >= 2 || n == 0) {
        out[n++] = (char)toupper((unsigned char)*p);
        continue;
      }
    }
  }
  if (n < 2) {
    /* One word: take its first two letters. */
    n = 0;
    for (const char *p = title; p && *p && n < 2; p++) {
      if (is_word_sep(*p))
        continue;
      out[n++] = (char)toupper((unsigned char)*p);
    }
  }
  if (n == 0)
    out[n++] = '?';
  out[n] = 0;
}

/* A colored square with the title's initials. */
static void draw_placeholder_tile(struct ui *u, int x, int y, const char *title) {
  uint32_t fill = tile_color(title);
  struct ui_paint paint = {.fill_set = 1, .fill = fill};
  ui_box(u, x, y, BOOT_ICON_PX, BOOT_ICON_PX, paint);
  char ini[3];
  initials(title, ini);
  int tw = (int)strlen(ini) * FONT_L_W * 2;
  int th = FONT_L_H * 2;
  ui_text(u, x + (BOOT_ICON_PX - tw) / 2, y + (BOOT_ICON_PX - th) / 2, ini,
          text_on(fill), 2);
}

/* Stock: the 4x4 pad bank. */
static void draw_stock_tile(struct ui *u, int x, int y,
                            const struct boot_theme *th) {
  struct ui_paint paint = {.fill_set = 1,
                           .fill = th->box,
                           .border_set = 1,
                           .border = th->border,
                           .border_px = 1};
  ui_box(u, x, y, BOOT_ICON_PX, BOOT_ICON_PX, paint);
  const int pad = 9, gap = 2, inset = 3;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      struct ui_paint p = {.fill_set = 1, .fill = th->dim};
      if (r == 3 && c == 0)
        p.fill = th->hl;
      ui_box(u, x + inset + c * (pad + gap), y + inset + r * (pad + gap), pad,
             pad, p);
    }
  }
}

/* Break title into up to APPS_LABEL_LINES lines of at most width chars,
   on word boundaries when possible. Lines are NUL terminated. */
static int wrap_label(const char *title, int width,
                      char lines[APPS_LABEL_LINES][MENU_TITLE]) {
  int nlines = 0;
  const char *p = title ? title : "";
  if (width < 1)
    width = 1;
  if (width >= MENU_TITLE)
    width = MENU_TITLE - 1;
  while (*p && nlines < APPS_LABEL_LINES) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;
    int len = (int)strlen(p);
    int take = len;
    if (take > width) {
      take = width;
      if (nlines + 1 < APPS_LABEL_LINES) {
        /* Break after a separator. Prefer the latest break whose remainder
           still fits on the next line; otherwise the latest break at all. */
        int latest = -1, fitting = -1;
        for (int i = take; i > 0; i--) {
          int ok = p[i] == ' ' || is_word_sep(p[i - 1]);
          if (!ok)
            continue;
          if (latest < 0)
            latest = i;
          /* The next line skips a space at the cut. */
          int rest = len - i - (p[i] == ' ' ? 1 : 0);
          if (rest <= width) {
            fitting = i;
            break;
          }
        }
        if (fitting > 0)
          take = fitting;
        else if (latest > 0)
          take = latest;
      }
    }
    memcpy(lines[nlines], p, (size_t)take);
    lines[nlines][take] = 0;
    /* Trim trailing spaces. */
    int t = take;
    while (t > 0 && lines[nlines][t - 1] == ' ')
      lines[nlines][--t] = 0;
    nlines++;
    p += take;
  }
  return nlines;
}

/* Draws every row as a tile in a grid. The cursor is ls->cursor. Returns
   the row chosen with the encoder press, or -1. */
static int apps_grid(struct ui *u, struct boot_ui *s, int x, int y, int w,
                     int h, int n, const struct boot_theme *th,
                     const struct input *in, struct ui_list_state *ls) {
  if (n < 0)
    n = 0;
  if (n == 0)
    ls->cursor = 0;
  else
    ls->cursor = clamp_index(ls->cursor, n);
  int hit = -1;
  if (in && n > 0 && in->encoder_press && !in->shift)
    hit = ls->cursor;

  int rows = h / APPS_CELL_H;
  if (rows < 1)
    rows = 1;
  int total_rows = (n + APPS_COLS - 1) / APPS_COLS;
  int reserve = total_rows > rows ? APPS_SCROLL_W : 0;
  int cell_w = (w - reserve) / APPS_COLS;
  int cur_row = ls->cursor / APPS_COLS;
  int first_row = cur_row >= rows ? cur_row - rows + 1 : 0;
  int label_chars = (cell_w - 4) / FONT_S_W;

  for (int i = first_row * APPS_COLS; i < n && i < (first_row + rows) * APPS_COLS;
       i++) {
    int col = i % APPS_COLS;
    int row = i / APPS_COLS - first_row;
    int cx = x + col * cell_w;
    int cy = y + row * APPS_CELL_H;
    uint32_t bg = th->bg;
    if (i == ls->cursor) {
      bg = th->bar;
      struct ui_paint sel = {.fill_set = 1, .fill = th->bar};
      ui_box(u, cx, cy, cell_w, APPS_CELL_H, sel);
      ui_highlight(u, cx, cy, cell_w, APPS_CELL_H, th->hl, 2);
    }
    const struct menu_entry *e = &s->menu->items[i];
    int ix = cx + (cell_w - BOOT_ICON_PX) / 2;
    int iy = cy + 4;
    const struct boot_icon *ic = row_icon(s, i);
    if (ic)
      icon_blit(u, ix, iy, ic->px, BOOT_ICON_PX, bg);
    else if (e->stock)
      draw_stock_tile(u, ix, iy, th);
    else
      draw_placeholder_tile(u, ix, iy, e->title);
    if (i == s->default_index) {
      /* Default badge in the top-right corner of the picture. */
      struct ui_paint badge = {.fill_set = 1,
                               .fill = th->hl,
                               .border_set = 1,
                               .border = bg,
                               .border_px = 1};
      ui_box(u, ix + BOOT_ICON_PX - 7, iy - 3, 10, 10, badge);
    }
    char lines[APPS_LABEL_LINES][MENU_TITLE];
    int nl = wrap_label(e->title, label_chars, lines);
    for (int l = 0; l < nl; l++) {
      int tw = (int)strlen(lines[l]) * FONT_S_W;
      int tx = cx + (cell_w - tw) / 2;
      ui_text(u, tx, iy + BOOT_ICON_PX + 2 + l * FONT_S_H, lines[l], th->text,
              1);
    }
  }

  if (reserve) {
    int bx = x + w - 4;
    struct ui_paint track = {.fill_set = 1, .fill = th->box};
    ui_box(u, bx, y, 4, h, track);
    int thumb_h = h * rows / total_rows;
    if (thumb_h < 8)
      thumb_h = h < 8 ? h : 8;
    int max_first = total_rows - rows;
    int travel = h - thumb_h;
    int ty = y;
    if (max_first > 0 && travel > 0)
      ty = y + travel * first_row / max_first;
    struct ui_paint thumb = {.fill_set = 1, .fill = th->hl};
    ui_box(u, bx, ty, 4, thumb_h, thumb);
  }
  return hit;
}

static struct ui_usb_sd_style usb_style(const struct boot_theme *th) {
  struct ui_usb_sd_style st = {
      .bg = th->bg,
      .text = th->text,
      .dim = th->dim,
      .hl = th->hl,
      .box = th->box,
      .border = th->border,
      .bar = th->bar,
      .active_text = th->active_text,
  };
  return st;
}

static struct ui_usb_apps_style usb_apps_style(const struct boot_theme *th) {
  struct ui_usb_apps_style st = {
      .bg = th->bg,
      .text = th->text,
      .dim = th->dim,
      .hl = th->hl,
      .box = th->box,
      .border = th->border,
      .bar = th->bar,
      .active_text = th->active_text,
  };
  return st;
}

/* 1 when this frame belongs to the USB screen. 0 when the caller should draw
   the screen underneath, which is Settings after a failed start. */
static int run_usb_sd(struct ui *u, struct boot_ui *s, const struct input *in) {
  int rc = ui_usb_sd(u, &s->usb, usb_style(theme_at(s->theme)), in);
  if (rc == UI_USB_SD_DONE) {
    s->action = BOOT_ACT_USB_SD_DONE;
    return 1;
  }
  if (rc == UI_USB_SD_FAILED) {
    s->screen = BOOT_SCREEN_USB;
    return 0;
  }
  return 1;
}

static int run_usb_apps(struct ui *u, struct boot_ui *s,
                        const struct input *in) {
  int rc = ui_usb_apps(u, &s->usb_apps, usb_apps_style(theme_at(s->theme)), in);
  if (rc == UI_USB_APPS_DONE) {
    s->action = BOOT_ACT_USB_APPS_DONE;
    return 1;
  }
  if (rc == UI_USB_APPS_FAILED) {
    s->screen = s->usb_apps_return_screen;
    return 0;
  }
  return 1;
}

/* index MENU_DEFAULT_BOOT_THINGY makes the menu itself the default. */
static void note_default(struct boot_ui *s, int index) {
  if (!s->menu || index >= s->menu->n)
    return;
  if (index < 0 && index != MENU_DEFAULT_BOOT_THINGY)
    return;
  s->action = BOOT_ACT_SET_DEFAULT;
  s->action_index = index;
  s->default_index = index;
  snprintf(s->action_id, sizeof s->action_id, "%s",
           index < 0 ? MENU_BOOT_THINGY_ID : s->menu->items[index].id);
}

/* Row of the default-app list that holds the current default. The menu
   itself is the row after the last app. */
static int default_row(const struct boot_ui *s) {
  int n = s->menu ? s->menu->n : 0;
  return s->default_index >= 0 && s->default_index < n ? s->default_index : n;
}

static void trim(char *s) {
  char *a = s;
  while (*a == ' ' || *a == '\t' || *a == '\r')
    a++;
  if (a != s)
    memmove(s, a, strlen(a) + 1);
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
               s[n - 1] == '\n'))
    s[--n] = 0;
}

static void mark_enabled(struct boot_ui *s, const char *list) {
  char buf[MENU_PATCHES];
  char *p;
  memset(s->enabled, 0, sizeof s->enabled);
  if (!list || !list[0])
    return;
  snprintf(buf, sizeof buf, "%s", list);
  p = buf;
  while (*p) {
    char *comma = strchr(p, ',');
    if (comma)
      *comma = 0;
    for (int i = 0; i < s->ncatalog; i++) {
      if (strcmp(s->catalog[i].id, p) == 0)
        s->enabled[i] = 1;
    }
    if (!comma)
      break;
    p = comma + 1;
  }
}

static int open_menu_item_edit(struct boot_ui *s, int menu_idx) {
  if (!s->menu || menu_idx < 0 || menu_idx >= s->menu->n ||
      s->menu->items[menu_idx].stock)
    return -1;
  memset(s->enabled, 0, sizeof s->enabled);
  memset(s->saved_on, 0, sizeof s->saved_on);
  s->entry_ls.cursor = 0;
  s->item_ls.cursor = 0;
  s->edit_new = 0;
  snprintf(s->edit_id, sizeof s->edit_id, "%s", s->menu->items[menu_idx].id);
  snprintf(s->edit_title, sizeof s->edit_title, "%s",
           s->menu->items[menu_idx].title);
  snprintf(s->edit_args, sizeof s->edit_args, "%s",
           s->menu->items[menu_idx].args);
  s->edit_boot_ac50 = s->menu->items[menu_idx].boot_ac50;
  s->edit_is_app = strncmp(s->menu->items[menu_idx].id, "app:", 4) == 0;
  mark_enabled(s, s->menu->items[menu_idx].patches);
  memcpy(s->saved_on, s->enabled, sizeof s->enabled);
  s->screen = BOOT_SCREEN_ITEM_EDIT;
  return 0;
}

static void open_new_entry(struct boot_ui *s) {
  memset(s->enabled, 0, sizeof s->enabled);
  memset(s->saved_on, 0, sizeof s->saved_on);
  s->entry_ls.cursor = 0;
  s->item_ls.cursor = 0;
  s->edit_id[0] = 0;
  s->edit_title[0] = 0;
  s->edit_args[0] = 0;
  s->edit_is_app = 0;
  s->edit_boot_ac50 = 0;
  s->edit_new = 1;
  s->screen = BOOT_SCREEN_ENTRY;
}

static void queue_save(struct boot_ui *s) {
  s->action = BOOT_ACT_SAVE_BOOT;
  s->save_id[0] = 0;
  s->save_title[0] = 0;
  s->save_args[0] = 0;
  if (!s->edit_new) {
    snprintf(s->save_id, sizeof s->save_id, "%s", s->edit_id);
  }
  if (s->edit_title[0]) {
    snprintf(s->save_title, sizeof s->save_title, "%s", s->edit_title);
  }
  if (s->edit_args[0]) {
    snprintf(s->save_args, sizeof s->save_args, "%s", s->edit_args);
  }
  s->save_npatches = 0;
  for (int i = 0; i < s->ncatalog && s->save_npatches < BOOT_PATCH_MAX; i++) {
    if (!s->enabled[i])
      continue;
    snprintf(s->save_patches[s->save_npatches], BOOT_PATCH_ID, "%s",
             s->catalog[i].id);
    s->save_npatches++;
  }
}

static void discard_entry(struct boot_ui *s) {
  memcpy(s->enabled, s->saved_on, sizeof s->enabled);
  s->screen = s->edit_new ? BOOT_SCREEN_APPS : BOOT_SCREEN_ITEM_EDIT;
}

void boot_ui_init(struct boot_ui *ui, const struct menu *menu, int theme,
                  int default_index) {
  if (!ui)
    return;
  memset(ui, 0, sizeof *ui);
  ui->menu = menu;
  if (theme < 0 || theme >= theme_count())
    theme = 0;
  ui->theme = theme;
  ui->saved_theme = theme;
  ui->theme_ls.cursor = theme;
  ui->default_index = default_index;
  ui->default_ls.cursor = default_row(ui);
  snprintf(ui->format_label, sizeof ui->format_label, "MPC-Sample");
  int n = menu ? menu->n : 0;
  int sel = (default_index >= 0 && default_index < n) ? default_index : 0;
  ui->apps_ls.cursor = sel;
}

void boot_ui_set_view(struct boot_ui *ui, int view) {
  if (!ui)
    return;
  ui->view = view == MENU_VIEW_GRID ? MENU_VIEW_GRID : MENU_VIEW_LIST;
}

static const char *view_row_label(const struct boot_ui *s) {
  return s->view == MENU_VIEW_GRID ? "Apps view: Grid" : "Apps view: List";
}

void boot_ui_set_card(struct boot_ui *ui, const char *card) {
  if (!ui)
    return;
  if (card && card[0])
    snprintf(ui->card, sizeof ui->card, "%s", card);
  else
    ui->card[0] = 0;
}

void boot_ui_set_catalog(struct boot_ui *ui, const struct boot_patch *patches,
                         int n) {
  if (!ui)
    return;
  if (n < 0)
    n = 0;
  if (n > BOOT_PATCH_MAX)
    n = BOOT_PATCH_MAX;
  if (n > 0 && patches)
    memcpy(ui->catalog, patches, (size_t)n * sizeof *patches);
  ui->ncatalog = n > 0 && patches ? n : 0;
}

int boot_ui_dirty(const struct boot_ui *ui) {
  if (!ui)
    return 0;
  return memcmp(ui->enabled, ui->saved_on, sizeof ui->enabled) != 0;
}

void boot_ui_saved(struct boot_ui *ui) {
  if (!ui)
    return;
  ui->screen = BOOT_SCREEN_APPS;
  memset(ui->enabled, 0, sizeof ui->enabled);
  memset(ui->saved_on, 0, sizeof ui->saved_on);
  ui->edit_new = 0;
  ui->edit_is_app = 0;
  ui->edit_boot_ac50 = 0;
  ui->edit_id[0] = 0;
  ui->edit_title[0] = 0;
  ui->edit_args[0] = 0;
  ui->save_args[0] = 0;
  ui->save_npatches = 0;
  ui->notice[0] = 0;
  ui->notice_until_ms = 0;
}

static int64_t ui_now_ms(const struct ui *u) {
  if (u && u->now_ms)
    return u->now_ms;
  return mono_now_ms();
}

void boot_ui_notice(struct boot_ui *ui, const char *text) {
  if (!ui)
    return;
  snprintf(ui->notice, sizeof ui->notice, "%s", text ? text : "");
  /* Timing is resolved against the ui clock on the next frame. */
  ui->notice_until_ms = 0;
}

void boot_draw_prompt(struct ui *u, int theme) {
  const struct boot_theme *th = theme_at(theme);
  ui_clear(u, th->bg);
  const char *a = "Press SHIFT";
  const char *b = "for boot thingy";
  int scale = 2;
  int gw = FONT_L_W * scale;
  int gh = FONT_L_H * scale;
  int y = (UI_H - (gh * 2 + 12)) / 2;
  ui_text(u, (UI_W - (int)strlen(a) * gw) / 2, y, a, th->hl, scale);
  ui_text(u, (UI_W - (int)strlen(b) * gw) / 2, y + gh + 12, b, th->text, scale);
}

void boot_draw_status(struct ui *u, int theme, const char *title,
                      const char *subtitle) {
  const struct boot_theme *th = theme_at(theme);
  ui_clear(u, th->bg);
  const char *a = (title && title[0]) ? title : "Loading...";
  int scale = 2;
  int gw = FONT_L_W * scale;
  int gh = FONT_L_H * scale;
  int tw = (int)strlen(a) * gw;
  int tx = (UI_W - tw) / 2;
  if (tx < 8)
    tx = 8;
  int has_sub = subtitle && subtitle[0];
  int ty = has_sub ? (UI_H - gh) / 2 - 12 : (UI_H - gh) / 2;
  ui_text(u, tx, ty, a, th->hl, scale);
  if (has_sub) {
    char sub[48];
    fit_text(sub, sizeof sub, subtitle);
    int sw = (int)strlen(sub) * FONT_S_W;
    int sx = (UI_W - sw) / 2;
    if (sx < 12)
      sx = 12;
    ui_text(u, sx, ty + gh + 10, sub, th->dim, 1);
  }
}

void boot_draw_loading(struct ui *u, int theme) {
  boot_draw_status(u, theme, "Loading Patches...", NULL);
}

int boot_frame(struct ui *u, const struct input *in, void *state) {
  struct boot_ui *s = state;
  if (!u || !s)
    return 0;
  s->action = BOOT_ACT_NONE;
  s->action_index = 0;
  s->action_id[0] = 0;

  struct input ev;
  if (in)
    ev = *in;
  else
    input_begin(&ev, 0);
  int shift_held = ev.shift || ((ev.held & (1u << IN_SHIFT)) != 0);
  ev.shift = shift_held;
  if (ev.down || ev.encoder || ev.encoder_press) {
    s->notice[0] = 0;
    s->notice_until_ms = 0;
  }
  if (s->notice[0]) {
    if (s->notice_until_ms == 0)
      s->notice_until_ms = ui_now_ms(u) + 2000;
    if (ui_now_ms(u) >= s->notice_until_ms) {
      s->notice[0] = 0;
      s->notice_until_ms = 0;
    }
  }

  if (s->screen == BOOT_SCREEN_TEXT_EDIT) {
    const struct boot_theme *th = theme_at(s->theme);
    struct ui_text_editor_style ed_st = ui_text_editor_style_default();
    ed_st.fill = th->bg;
    ed_st.text_box = th->box;
    ed_st.text = th->text;
    ed_st.title = th->hl;
    ed_st.border = th->border;
    ed_st.highlight = th->hl;

    int res = ui_text_editor(u, 0, 0, UI_W, UI_H, &s->text_ed, ed_st, &ev);
    if (res == UI_TEXT_OK) {
      trim(s->text_buf);
      if (s->text_edit_mode == BOOT_TEXT_FORMAT_LABEL) {
        if (s->text_buf[0])
          snprintf(s->format_label, sizeof s->format_label, "%s", s->text_buf);
        else
          snprintf(s->format_label, sizeof s->format_label, "MPC-Sample");
        s->screen = BOOT_SCREEN_FORMAT_PROGRESS;
        snprintf(s->format_status, sizeof s->format_status,
                 "Preparing to format...");
        s->action = BOOT_ACT_FORMAT_SD;
      } else if (s->text_edit_mode == BOOT_TEXT_RENAME) {
        if (s->text_buf[0]) {
          snprintf(s->edit_title, sizeof s->edit_title, "%s", s->text_buf);
          if (s->text_prev_screen == BOOT_SCREEN_ITEM_EDIT) {
            s->action = BOOT_ACT_UPDATE_ENTRY;
            snprintf(s->action_id, sizeof s->action_id, "%s", s->edit_id);
            snprintf(s->save_title, sizeof s->save_title, "%s", s->edit_title);
            snprintf(s->save_args, sizeof s->save_args, "%s", s->edit_args);
          }
          s->screen = s->text_prev_screen;
        } else {
          boot_ui_notice(s, "Title cannot be empty");
        }
      } else if (s->text_edit_mode == BOOT_TEXT_ARGS) {
        snprintf(s->edit_args, sizeof s->edit_args, "%s", s->text_buf);
        if (s->text_prev_screen == BOOT_SCREEN_ITEM_EDIT) {
          s->action = BOOT_ACT_UPDATE_ENTRY;
          snprintf(s->action_id, sizeof s->action_id, "%s", s->edit_id);
          snprintf(s->save_title, sizeof s->save_title, "%s", s->edit_title);
          snprintf(s->save_args, sizeof s->save_args, "%s", s->edit_args);
        }
        s->screen = s->text_prev_screen;
      }
    } else if (res == UI_TEXT_CANCEL) {
      s->screen = s->text_prev_screen;
    }
    if (s->notice[0]) {
      struct ui_paint pop = {.fill_set = 1,
                             .fill = th->box,
                             .border_set = 1,
                             .border = th->border,
                             .border_px = 1};
      ui_box(u, 8, UI_H - 44, 304, 36, pop);
      int tw = (int)strlen(s->notice) * FONT_S_W;
      int tx = (UI_W - tw) / 2;
      if (tx < 12)
        tx = 12;
      ui_text(u, tx, UI_H - 32, s->notice, th->text, 1);
    }
    return 0;
  }

  int screen = s->screen;
  int n = s->menu ? s->menu->n : 0;
  int shown_n = n > MENU_MAX ? MENU_MAX : n;
  if (screen == BOOT_SCREEN_SETTINGS)
    move_cursor(&s->settings_ls, 5, &ev);
  else if (screen == BOOT_SCREEN_APPS)
    move_cursor(&s->apps_ls, shown_n, &ev);
  else if (screen == BOOT_SCREEN_USB)
    move_cursor(&s->usb_ls, 2, &ev);
  else if (screen == BOOT_SCREEN_DEFAULT)
    move_cursor(&s->default_ls, n + 1, &ev); /* apps, then the menu itself */
  else if (screen == BOOT_SCREEN_THEME) {
    move_cursor(&s->theme_ls, theme_count(), &ev);
    s->theme = s->theme_ls.cursor;
  } else if (screen == BOOT_SCREEN_POWER)
    move_cursor(&s->power_ls, 3, &ev);
  else if (screen == BOOT_SCREEN_ITEM_EDIT)
    move_cursor(&s->item_ls, s->edit_boot_ac50 ? 4 : 3, &ev);
  else if (screen == BOOT_SCREEN_DELETE_CONFIRM)
    move_cursor(&s->delete_confirm_ls, 2, &ev);
  else if (screen == BOOT_SCREEN_FORMAT_CONFIRM)
    move_cursor(&s->format_confirm_ls, 2, &ev);
  else if (screen == BOOT_SCREEN_ENTRY)
    move_cursor(&s->entry_ls, s->ncatalog, &ev);
  else if (screen == BOOT_SCREEN_UNSAVED)
    move_cursor(&s->unsaved_ls, 3, &ev);

  int left = 0;
  int shift_apps = (screen == BOOT_SCREEN_APPS && shift_held);
  int shift_boot = (shift_apps && shown_n > 0 &&
                    s->apps_ls.cursor < shown_n);
  if (shift_apps && input_down(&ev, IN_B1)) {
    if (shift_boot) {
      int sel = s->apps_ls.cursor;
      if (sel >= 0 && sel < n)
        note_default(s, sel);
    }
    left = 1;
  } else if (shift_apps && input_down(&ev, IN_B2)) {
    if (shift_boot) {
      int sel = s->apps_ls.cursor;
      if (sel >= 0 && sel < n) {
        if (s->menu->items[sel].stock)
          boot_ui_notice(s, "Stock cannot be edited");
        else
          open_menu_item_edit(s, sel);
      }
    }
    left = 1;
  } else if (shift_apps && input_down(&ev, IN_B3)) {
    open_new_entry(s);
    left = 1;
  } else if (screen == BOOT_SCREEN_APPS && input_down(&ev, IN_B1)) {
    s->usb_ls.cursor = 0;
    s->screen = BOOT_SCREEN_USB;
    left = 1;
  } else if (screen == BOOT_SCREEN_APPS && input_down(&ev, IN_B2)) {
    s->power_ls.cursor = 0;
    s->screen = BOOT_SCREEN_POWER;
    left = 1;
  } else if (screen == BOOT_SCREEN_APPS && input_down(&ev, IN_B3)) {
    s->screen = BOOT_SCREEN_SETTINGS;
    left = 1;
  } else if (screen == BOOT_SCREEN_ENTRY && input_down(&ev, IN_B2)) {
    snprintf(s->text_buf, sizeof s->text_buf, "%s", s->edit_title);
    ui_text_editor_init(&s->text_ed, s->text_buf, MENU_TITLE, "RENAME ENTRY");
    s->text_edit_mode = BOOT_TEXT_RENAME;
    s->text_prev_screen = BOOT_SCREEN_ENTRY;
    s->screen = BOOT_SCREEN_TEXT_EDIT;
    left = 1;
  } else if ((screen == BOOT_SCREEN_ENTRY || screen == BOOT_SCREEN_UNSAVED) &&
             input_down(&ev, IN_B3)) {
    queue_save(s);
    left = 1;
  } else if (screen != BOOT_SCREEN_APPS && screen != BOOT_SCREEN_STORAGE &&
             screen != BOOT_SCREEN_USB_APPS && input_down(&ev, IN_B1)) {
    if (screen == BOOT_SCREEN_THEME) {
      s->theme = s->saved_theme;
      s->theme_ls.cursor = s->saved_theme;
      s->screen = BOOT_SCREEN_SETTINGS;
    } else if (screen == BOOT_SCREEN_ENTRY) {
      s->screen = boot_ui_dirty(s) ? BOOT_SCREEN_UNSAVED
                                   : (s->edit_new ? BOOT_SCREEN_APPS
                                                  : BOOT_SCREEN_ITEM_EDIT);
      if (s->screen == BOOT_SCREEN_UNSAVED)
        s->unsaved_ls.cursor = 0;
    } else if (screen == BOOT_SCREEN_UNSAVED) {
      s->screen = BOOT_SCREEN_ENTRY;
    } else if (screen == BOOT_SCREEN_ITEM_EDIT) {
      s->screen = BOOT_SCREEN_APPS;
    } else if (screen == BOOT_SCREEN_DELETE_CONFIRM) {
      s->screen = BOOT_SCREEN_ITEM_EDIT;
    } else if (screen == BOOT_SCREEN_FORMAT_CONFIRM) {
      s->screen = BOOT_SCREEN_SETTINGS;
    } else if (screen == BOOT_SCREEN_FORMAT_PROGRESS) {
      /* cannot cancel format progress */
    } else if (screen == BOOT_SCREEN_SETTINGS || screen == BOOT_SCREEN_POWER) {
      s->screen = BOOT_SCREEN_APPS;
    } else if (screen == BOOT_SCREEN_USB) {
      s->screen = BOOT_SCREEN_APPS;
    } else {
      s->screen = BOOT_SCREEN_SETTINGS;
    }
    left = 1;
  }

  if (left)
    ev.encoder_press = 0;

  if (!left && ev.encoder_press && !ev.shift) {
    if (screen == BOOT_SCREEN_SETTINGS) {
      if (s->settings_ls.cursor == 0) {
        s->screen = BOOT_SCREEN_DEFAULT;
        s->default_ls.cursor = default_row(s);
      } else if (s->settings_ls.cursor == 1) {
        s->screen = BOOT_SCREEN_THEME;
        s->theme = s->saved_theme;
        s->theme_ls.cursor = s->saved_theme;
      } else if (s->settings_ls.cursor == 2) {
        /* Toggle the apps view and save it. */
        boot_ui_set_view(s, s->view == MENU_VIEW_GRID ? MENU_VIEW_LIST
                                                       : MENU_VIEW_GRID);
        s->action = BOOT_ACT_SAVE_VIEW;
        s->action_index = s->view;
      } else if (s->settings_ls.cursor == 3) {
        s->action = BOOT_ACT_RELOAD;
      } else if (s->settings_ls.cursor == 4) {
        s->screen = BOOT_SCREEN_FORMAT_CONFIRM;
        s->format_confirm_ls.cursor = 0;
      }
      ev.encoder_press = 0;
    } else if (screen == BOOT_SCREEN_USB) {
      if (s->usb_ls.cursor == 0) {
        s->usb_apps_return_screen = BOOT_SCREEN_USB;
        s->screen = BOOT_SCREEN_USB_APPS;
        ui_usb_apps_init(&s->usb_apps, s->card);
      } else {
        s->screen = BOOT_SCREEN_STORAGE;
        ui_usb_sd_init(&s->usb);
      }
      ev.encoder_press = 0;
    } else if (screen == BOOT_SCREEN_FORMAT_CONFIRM) {
      if (s->format_confirm_ls.cursor == 0) {
        s->screen = BOOT_SCREEN_SETTINGS;
      } else {
        const char *init_lbl =
            s->format_label[0] ? s->format_label : "MPC-Sample";
        snprintf(s->text_buf, sizeof s->text_buf, "%s", init_lbl);
        ui_text_editor_init(&s->text_ed, s->text_buf, sizeof s->format_label,
                            "SD CARD LABEL");
        s->text_edit_mode = BOOT_TEXT_FORMAT_LABEL;
        s->text_prev_screen = BOOT_SCREEN_FORMAT_CONFIRM;
        s->screen = BOOT_SCREEN_TEXT_EDIT;
      }
      ev.encoder_press = 0;
    } else if (screen == BOOT_SCREEN_DEFAULT) {
      int row = wrap(s->default_ls.cursor, n + 1);
      note_default(s, row < n ? row : MENU_DEFAULT_BOOT_THINGY);
      s->screen = BOOT_SCREEN_SETTINGS;
      ev.encoder_press = 0;
    } else if (screen == BOOT_SCREEN_THEME) {
      s->theme = wrap(s->theme_ls.cursor, theme_count());
      s->saved_theme = s->theme;
      s->action = BOOT_ACT_SAVE_THEME;
      s->action_index = s->theme;
      s->screen = BOOT_SCREEN_SETTINGS;
      ev.encoder_press = 0;
    }
  }

  if (s->screen == BOOT_SCREEN_STORAGE) {
    if (run_usb_sd(u, s, &ev))
      return 0;
    input_begin(&ev, 0);
  } else if (s->screen == BOOT_SCREEN_USB_APPS) {
    if (run_usb_apps(u, s, &ev))
      return 0;
    input_begin(&ev, 0);
  }

  const struct boot_theme *th = theme_at(s->theme);
  ui_clear(u, th->bg);

  const char *labels[3];
  if (s->screen == BOOT_SCREEN_APPS) {
    if (shift_held) {
      int on_entry = (shown_n > 0 && s->apps_ls.cursor < shown_n);
      labels[0] = on_entry ? "Set Default" : "";
      labels[1] = on_entry ? "Edit Entry" : "";
      labels[2] = "New Entry";
    } else {
      labels[0] = "USB";
      labels[1] = "Power";
      labels[2] = "Settings";
    }
  } else if (s->screen == BOOT_SCREEN_ENTRY) {
    labels[0] = "Back";
    labels[1] = "Rename";
    labels[2] = "Save";
  } else if (s->screen == BOOT_SCREEN_UNSAVED) {
    labels[0] = "Back";
    labels[1] = "";
    labels[2] = "Save";
  } else if (s->screen == BOOT_SCREEN_FORMAT_PROGRESS) {
    labels[0] = "";
    labels[1] = "";
    labels[2] = "";
  } else {
    labels[0] = "Back";
    labels[1] = "";
    labels[2] = "";
  }
  const struct input *widget_in = &ev;
  struct ui_soft_style soft = soft_style(th);
  ui_soft_keys(u, 8, 4, 304, 28, labels, &s->soft_active, soft, widget_in,
               &s->soft_ks);

  const char *title = "mpc-boot-thingy  |  v" APP_VERSION;
  const char *hint = "";
  char title_buf[48];
  if (s->screen == BOOT_SCREEN_SETTINGS)
    title = "Settings";
  else if (s->screen == BOOT_SCREEN_DEFAULT) {
    title = "Default app";
    hint = "starts when SHIFT is not held";
  }
  else if (s->screen == BOOT_SCREEN_THEME)
    title = "Theme";
  else if (s->screen == BOOT_SCREEN_POWER) {
    title = "Power";
    hint = "choose a power option";
  } else if (s->screen == BOOT_SCREEN_ITEM_EDIT) {
    fit_text(title_buf, sizeof title_buf, s->edit_title);
    title = title_buf;
    if (s->item_ls.cursor == 0 && s->edit_boot_ac50) {
      hint = "press to choose patches";
    } else if ((s->item_ls.cursor == 0 && !s->edit_boot_ac50) ||
               (s->item_ls.cursor == 1 && s->edit_boot_ac50)) {
      hint = "press to change title";
    } else if ((s->item_ls.cursor == 1 && !s->edit_boot_ac50) ||
               (s->item_ls.cursor == 2 && s->edit_boot_ac50)) {
      if (s->edit_args[0]) {
        static char args_hint[64];
        snprintf(args_hint, sizeof args_hint, "args: %s", s->edit_args);
        hint = args_hint;
      } else {
        hint = "press to add boot arguments";
      }
    } else {
      hint = s->edit_is_app ? "press to delete app from card"
                            : "press to delete entry";
    }
  } else if (s->screen == BOOT_SCREEN_DELETE_CONFIRM) {
    title = s->edit_is_app ? "Delete App?" : "Delete Boot Entry?";
    hint = "this cannot be undone";
  } else if (s->screen == BOOT_SCREEN_FORMAT_CONFIRM) {
    title = "Format SD Card?";
    hint = "ALL data on card will be erased";
  } else if (s->screen == BOOT_SCREEN_FORMAT_PROGRESS) {
    title = "Formatting SD Card";
    hint = "system will reboot when done";
  } else if (s->screen == BOOT_SCREEN_ENTRY) {
    fit_text(title_buf, sizeof title_buf,
             s->edit_new ? (s->edit_title[0] ? s->edit_title : "New entry")
                         : s->edit_title);
    title = title_buf;
    hint = "press to toggle";
  } else if (s->screen == BOOT_SCREEN_UNSAVED) {
    title = "Not saved yet";
    hint = "this entry has changes";
  } else if (s->screen == BOOT_SCREEN_USB) {
    title = "USB";
    hint = "install apps or mount storage";
  } else
    hint = "hold shift for options";
  int is_apps = (s->screen == BOOT_SCREEN_APPS);
  if (is_apps && shift_held) {
    uint32_t link_color = web_connected() ? 0x00ff00 : 0xff0000;
    ui_text(u, 12, 40, "http://mpc-sample.local:8080", link_color, 1);
    ui_text(u, 12, 56, "http://192.168.155.1:8080", link_color, 1);
  } else {
    ui_text(u, 12, 40, title, th->text, 1);
    if (hint[0])
      ui_text(u, 12, 56, hint, th->dim, 1);
  }

  int list_y;
  if (is_apps && s->view == MENU_VIEW_GRID)
    list_y = 72; /* two rows of tiles */
  else
    list_y = hint[0] ? 76 : 60;
  int list_h = UI_H - list_y - 8;
  struct ui_list_style rows_st = list_style(th);
  if (n > MENU_MAX)
    n = MENU_MAX;

  if (s->screen == BOOT_SCREEN_APPS) {
    int hit;
    if (s->view == MENU_VIEW_GRID) {
      hit = apps_grid(u, s, 8, list_y, 304, list_h, shown_n, th, widget_in,
                      &s->apps_ls);
    } else {
      char text[MENU_MAX][48];
      const char *rows[MENU_MAX];
      for (int i = 0; i < shown_n; i++) {
        row_label(text[i], sizeof text[i], s->menu->items[i].title,
                  i == s->default_index);
        rows[i] = text[i];
      }
      hit = ui_list(u, 8, list_y, 304, list_h, rows, shown_n, rows_st,
                    widget_in, &s->apps_ls);
    }
    if (hit >= 0 && hit < shown_n) {
      s->action = BOOT_ACT_LAUNCH;
      s->action_index = hit;
      snprintf(s->action_id, sizeof s->action_id, "%s",
               s->menu->items[hit].id);
    }
  } else if (s->screen == BOOT_SCREEN_USB) {
    const char *rows[] = {UI_USB_APPS_LABEL, UI_USB_SD_LABEL};
    ui_list(u, 8, list_y, 304, list_h, rows, 2, rows_st, widget_in,
            &s->usb_ls);
  } else if (s->screen == BOOT_SCREEN_POWER) {
    const char *rows[] = {"Reboot", "Shut Down", "Cancel"};
    int hit = ui_list(u, 8, list_y, 304, list_h, rows, 3, rows_st, widget_in,
                      &s->power_ls);
    if (hit == 0)
      s->action = BOOT_ACT_REBOOT;
    else if (hit == 1)
      s->action = BOOT_ACT_SHUTDOWN;
    else if (hit == 2)
      s->screen = BOOT_SCREEN_APPS;
  } else if (s->screen == BOOT_SCREEN_SETTINGS) {
    const char *rows[] = {"Set default app", "Set theme", view_row_label(s),
                          "Reload menu & SD", "Format SD Card"};
    ui_list(u, 8, list_y, 304, list_h, rows, 5, rows_st, widget_in,
            &s->settings_ls);
  } else if (s->screen == BOOT_SCREEN_FORMAT_CONFIRM) {
    const char *rows[] = {"No, cancel", "Yes, erase and format"};
    int hit = ui_list(u, 8, list_y, 304, list_h, rows, 2, rows_st, widget_in,
                      &s->format_confirm_ls);
    if (hit == 0) {
      s->screen = BOOT_SCREEN_SETTINGS;
    } else if (hit == 1) {
      const char *init_lbl =
          s->format_label[0] ? s->format_label : "MPC-Sample";
      snprintf(s->text_buf, sizeof s->text_buf, "%s", init_lbl);
      ui_text_editor_init(&s->text_ed, s->text_buf, sizeof s->format_label,
                          "SD CARD LABEL");
      s->text_edit_mode = BOOT_TEXT_FORMAT_LABEL;
      s->text_prev_screen = BOOT_SCREEN_FORMAT_CONFIRM;
      s->screen = BOOT_SCREEN_TEXT_EDIT;
    }
  } else if (s->screen == BOOT_SCREEN_FORMAT_PROGRESS) {
    struct ui_paint card_paint = {
        .fill_set = 1,
        .fill = th->box,
        .border_set = 1,
        .border = th->border,
        .border_px = 1,
    };
    ui_box(u, 16, 76, 288, 144, card_paint);
    ui_text(u, 28, 96, "FORMATTING SD CARD", th->hl, 1);
    ui_text(u, 28, 120,
            s->format_status[0] ? s->format_status : "Please wait...",
            th->text, 1);
    ui_text(u, 28, 144, "Do not turn off power.", th->dim, 1);
    ui_text(u, 28, 164, "System will reboot automatically.", th->dim, 1);
  } else if (s->screen == BOOT_SCREEN_DEFAULT) {
    char text[MENU_MAX + 1][48];
    const char *rows[MENU_MAX + 1];
    for (int i = 0; i < n; i++) {
      row_label(text[i], sizeof text[i], s->menu->items[i].title,
                i == s->default_index);
      rows[i] = text[i];
    }
    row_label(text[n], sizeof text[n], "Boot thingy menu",
              s->default_index == MENU_DEFAULT_BOOT_THINGY);
    rows[n] = text[n];
    ui_list(u, 8, list_y, 304, list_h, rows, n + 1, rows_st, widget_in,
            &s->default_ls);
  } else if (s->screen == BOOT_SCREEN_THEME) {
    const char *rows[8];
    int tn = theme_count();
    if (tn > 8)
      tn = 8;
    for (int i = 0; i < tn; i++)
      rows[i] = theme_at(i)->name;
    ui_list(u, 8, list_y, 304, list_h, rows, tn, rows_st, widget_in,
            &s->theme_ls);
  } else if (s->screen == BOOT_SCREEN_ITEM_EDIT) {
    const char *rows[4];
    int count = 0;
    if (s->edit_boot_ac50)
      rows[count++] = "Edit patches";
    rows[count++] = "Rename entry";
    rows[count++] = "Edit arguments";
    rows[count++] =
        s->edit_is_app ? "Delete app completely" : "Delete boot entry";
    int hit = ui_list(u, 8, list_y, 304, list_h, rows, count, rows_st,
                      widget_in, &s->item_ls);
    if (hit >= 0) {
      const char *chosen = rows[hit];
      if (strcmp(chosen, "Edit patches") == 0) {
        s->screen = BOOT_SCREEN_ENTRY;
      } else if (strcmp(chosen, "Rename entry") == 0) {
        snprintf(s->text_buf, sizeof s->text_buf, "%s", s->edit_title);
        ui_text_editor_init(&s->text_ed, s->text_buf, MENU_TITLE,
                            "RENAME ENTRY");
        s->text_edit_mode = BOOT_TEXT_RENAME;
        s->text_prev_screen = BOOT_SCREEN_ITEM_EDIT;
        s->screen = BOOT_SCREEN_TEXT_EDIT;
      } else if (strcmp(chosen, "Edit arguments") == 0) {
        snprintf(s->text_buf, sizeof s->text_buf, "%s", s->edit_args);
        ui_text_editor_init(&s->text_ed, s->text_buf, MENU_ARGS,
                            "BOOT ARGUMENTS");
        s->text_edit_mode = BOOT_TEXT_ARGS;
        s->text_prev_screen = BOOT_SCREEN_ITEM_EDIT;
        s->screen = BOOT_SCREEN_TEXT_EDIT;
      } else {
        s->delete_confirm_ls.cursor = 0;
        s->screen = BOOT_SCREEN_DELETE_CONFIRM;
      }
    }
  } else if (s->screen == BOOT_SCREEN_DELETE_CONFIRM) {
    const char *rows[2];
    rows[0] = "No, keep it";
    rows[1] = s->edit_is_app ? "Yes, delete app completely"
                             : "Yes, delete boot entry";
    int hit = ui_list(u, 8, list_y, 304, list_h, rows, 2, rows_st, widget_in,
                      &s->delete_confirm_ls);
    if (hit == 0) {
      s->screen = BOOT_SCREEN_ITEM_EDIT;
    } else if (hit == 1) {
      s->action = BOOT_ACT_DELETE_ENTRY;
      snprintf(s->action_id, sizeof s->action_id, "%s", s->edit_id);
      s->screen = BOOT_SCREEN_APPS;
    }
  } else if (s->screen == BOOT_SCREEN_ENTRY) {
    char text[BOOT_PATCH_MAX][48];
    const char *rows[BOOT_PATCH_MAX];
    int pn = s->ncatalog;
    if (pn > BOOT_PATCH_MAX)
      pn = BOOT_PATCH_MAX;
    for (int i = 0; i < pn; i++) {
      const char *name =
          s->catalog[i].title[0] ? s->catalog[i].title : s->catalog[i].id;
      snprintf(text[i], sizeof text[i], "%s%s", s->enabled[i] ? "On  " : "    ",
               name);
      rows[i] = text[i];
    }
    int hit = ui_list(u, 8, list_y, 304, list_h, rows, pn, rows_st, widget_in,
                      &s->entry_ls);
    if (hit >= 0 && hit < pn)
      s->enabled[hit] = !s->enabled[hit];
  } else if (s->screen == BOOT_SCREEN_UNSAVED) {
    const char *rows[] = {"Save", "Don't save", "Cancel"};
    int hit = ui_list(u, 8, list_y, 304, list_h, rows, 3, rows_st, widget_in,
                      &s->unsaved_ls);
    if (hit == 0)
      queue_save(s);
    else if (hit == 1)
      discard_entry(s);
    else if (hit == 2)
      s->screen = BOOT_SCREEN_ENTRY;
  }

  if (s->notice[0]) {
    if (s->notice_until_ms == 0)
      s->notice_until_ms = ui_now_ms(u) + 2000;
    struct ui_paint pop = {.fill_set = 1,
                           .fill = th->box,
                           .border_set = 1,
                           .border = th->border,
                           .border_px = 1};
    ui_box(u, 8, UI_H - 44, 304, 36, pop);
    int tw = (int)strlen(s->notice) * FONT_S_W;
    int tx = (UI_W - tw) / 2;
    if (tx < 12)
      tx = 12;
    ui_text(u, tx, UI_H - 32, s->notice, th->text, 1);
    if (s->notice_until_ms > 0) {
      int64_t rem = s->notice_until_ms - ui_now_ms(u);
      if (rem < 1)
        rem = 1;
      if (rem > 2000)
        rem = 2000;
      ui_wake(u, (int)rem);
    } else {
      ui_wake(u, 2000);
    }
  }

  return 0;
}
