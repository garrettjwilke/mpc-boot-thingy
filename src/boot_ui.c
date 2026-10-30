#include "boot_ui.h"
#include "font.h"
#include "input.h"
#include "theme.h"
#include "usb_sd.h"

#include <stdio.h>
#include <string.h>

static int wrap(int v, int n) {
  if (n <= 0)
    return 0;
  v %= n;
  if (v < 0)
    v += n;
  return v;
}

static void move_cursor(struct ui_list_state *ls, int n, struct input *ev) {
  if (!ev || !ev->encoder || n <= 0)
    return;
  ls->cursor = wrap(ls->cursor + ev->encoder, n);
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

/* 1 when this frame belongs to the USB screen. 0 when the caller should draw
   the screen underneath, which is Settings after a failed start. */
static int run_usb_sd(struct ui *u, struct boot_ui *s, const struct input *in) {
  int rc = ui_usb_sd(u, &s->usb, usb_style(theme_at(s->theme)), in);
  if (rc == UI_USB_SD_DONE) {
    s->action = BOOT_ACT_USB_SD_DONE;
    return 1;
  }
  if (rc == UI_USB_SD_FAILED) {
    s->screen = BOOT_SCREEN_SETTINGS;
    return 0;
  }
  return 1;
}

static void note_default(struct boot_ui *s, int index) {
  if (!s->menu || index < 0 || index >= s->menu->n)
    return;
  s->action = BOOT_ACT_SET_DEFAULT;
  s->action_index = index;
  s->default_index = index;
  snprintf(s->action_id, sizeof s->action_id, "%s", s->menu->items[index].id);
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
  ui->default_ls.cursor = default_index;
  int n = menu ? menu->n : 0;
  int sel = (default_index >= 0 && default_index < n) ? default_index : 0;
  ui->apps_ls.cursor = 1 + sel;
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
}

void boot_ui_notice(struct boot_ui *ui, const char *text) {
  if (!ui)
    return;
  snprintf(ui->notice, sizeof ui->notice, "%s", text ? text : "");
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

void boot_draw_loading(struct ui *u, int theme) {
  const struct boot_theme *th = theme_at(theme);
  ui_clear(u, th->bg);
  const char *a = "Loading Patches...";
  int scale = 2;
  int gw = FONT_L_W * scale;
  int gh = FONT_L_H * scale;
  ui_text(u, (UI_W - (int)strlen(a) * gw) / 2, (UI_H - gh) / 2, a, th->hl,
          scale);
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
  if (ev.down || ev.encoder || ev.encoder_press)
    s->notice[0] = 0;

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
      if (s->text_edit_mode == BOOT_TEXT_RENAME) {
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
    return 0;
  }

  int screen = s->screen;
  int n = s->menu ? s->menu->n : 0;
  if (screen == BOOT_SCREEN_SETTINGS)
    move_cursor(&s->settings_ls, 4, &ev);
  else if (screen == BOOT_SCREEN_DEFAULT)
    move_cursor(&s->default_ls, n, &ev);
  else if (screen == BOOT_SCREEN_THEME) {
    move_cursor(&s->theme_ls, theme_count(), &ev);
    s->theme = s->theme_ls.cursor;
  } else if (screen == BOOT_SCREEN_POWER)
    move_cursor(&s->power_ls, 3, &ev);
  else if (screen == BOOT_SCREEN_ITEM_EDIT)
    move_cursor(&s->item_ls, s->edit_boot_ac50 ? 4 : 3, &ev);
  else if (screen == BOOT_SCREEN_DELETE_CONFIRM)
    move_cursor(&s->delete_confirm_ls, 2, &ev);
  else if (screen == BOOT_SCREEN_ENTRY)
    move_cursor(&s->entry_ls, s->ncatalog, &ev);
  else if (screen == BOOT_SCREEN_UNSAVED)
    move_cursor(&s->unsaved_ls, 3, &ev);

  int left = 0;
  int shift_apps = (screen == BOOT_SCREEN_APPS && shift_held);
  int shift_boot = (shift_apps && s->apps_ls.cursor > 0);
  if (shift_apps && input_down(&ev, IN_B1)) {
    if (shift_boot) {
      int sel = s->apps_ls.cursor - 1;
      if (sel >= 0 && sel < n)
        note_default(s, sel);
    }
    left = 1;
  } else if (shift_apps && input_down(&ev, IN_B2)) {
    if (shift_boot) {
      int sel = s->apps_ls.cursor - 1;
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
             input_down(&ev, IN_B1)) {
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
    } else if (screen == BOOT_SCREEN_SETTINGS || screen == BOOT_SCREEN_POWER) {
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
        s->default_ls.cursor = s->default_index;
      } else if (s->settings_ls.cursor == 1) {
        s->screen = BOOT_SCREEN_THEME;
        s->theme = s->saved_theme;
        s->theme_ls.cursor = s->saved_theme;
      } else if (s->settings_ls.cursor == 2) {
        s->screen = BOOT_SCREEN_STORAGE;
        ui_usb_sd_init(&s->usb);
      } else {
        s->action = BOOT_ACT_RELOAD;
      }
      ev.encoder_press = 0;
    } else if (screen == BOOT_SCREEN_DEFAULT) {
      note_default(s, n > 0 ? wrap(s->default_ls.cursor, n) : 0);
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
  }

  const struct boot_theme *th = theme_at(s->theme);
  ui_clear(u, th->bg);

  int apps_count = 1 + (n > MENU_MAX - 1 ? MENU_MAX - 1 : n);
  int next_apps_cursor = s->apps_ls.cursor;
  if (s->screen == BOOT_SCREEN_APPS && apps_count > 0) {
    if (ev.encoder)
      next_apps_cursor = wrap(next_apps_cursor + ev.encoder, apps_count);
    else if (next_apps_cursor < 0 || next_apps_cursor >= apps_count)
      next_apps_cursor = wrap(next_apps_cursor, apps_count);
  }

  const char *labels[3];
  if (s->screen == BOOT_SCREEN_APPS) {
    if (shift_held) {
      labels[0] = next_apps_cursor > 0 ? "Set Default" : "";
      labels[1] = next_apps_cursor > 0 ? "Edit Entry" : "";
      labels[2] = "New Entry";
    } else {
      labels[0] = "";
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
  } else {
    labels[0] = "Back";
    labels[1] = "";
    labels[2] = "";
  }
  const struct input *widget_in = &ev;
  struct ui_soft_style soft = soft_style(th);
  ui_soft_keys(u, 8, 4, 304, 28, labels, &s->soft_active, soft, widget_in,
               &s->soft_ks);

  const char *title = "mpc-boot-thingy";
  const char *hint = "";
  char title_buf[48];
  if (s->screen == BOOT_SCREEN_SETTINGS)
    title = "Settings";
  else if (s->screen == BOOT_SCREEN_DEFAULT)
    title = "Default app";
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
  } else if (s->screen == BOOT_SCREEN_ENTRY) {
    fit_text(title_buf, sizeof title_buf,
             s->edit_new ? (s->edit_title[0] ? s->edit_title : "New entry")
                         : s->edit_title);
    title = title_buf;
    hint = "press to toggle";
  } else if (s->screen == BOOT_SCREEN_UNSAVED) {
    title = "Not saved yet";
    hint = "this entry has changes";
  } else if (next_apps_cursor == 0)
    hint = UI_USB_SD_HINT;
  else
    hint = "hold shift for options";
  if (s->notice[0])
    hint = s->notice;
  ui_text(u, 12, 40, title, th->text, 1);
  if (hint[0])
    ui_text(u, 12, 56, hint, th->dim, 1);

  int list_y = hint[0] ? 76 : 60;
  int list_h = UI_H - list_y - 8;
  struct ui_list_style rows_st = list_style(th);
  if (n > MENU_MAX)
    n = MENU_MAX;

  if (s->screen == BOOT_SCREEN_APPS) {
    char text[MENU_MAX][48];
    const char *rows[MENU_MAX];
    int count = 0;
    rows[count++] = UI_USB_SD_LABEL;
    for (int i = 0; i < n && count < MENU_MAX; i++) {
      row_label(text[i], sizeof text[i], s->menu->items[i].title,
                i == s->default_index);
      rows[count++] = text[i];
    }
    int hit = ui_list(u, 8, list_y, 304, list_h, rows, count, rows_st,
                      widget_in, &s->apps_ls);
    if (hit == 0) {
      s->screen = BOOT_SCREEN_STORAGE;
      ui_usb_sd_init(&s->usb);
      run_usb_sd(u, s, &ev);
      return 0;
    } else if (hit > 0) {
      int sel = hit - 1;
      s->action = BOOT_ACT_LAUNCH;
      s->action_index = sel;
      snprintf(s->action_id, sizeof s->action_id, "%s", s->menu->items[sel].id);
    }
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
    const char *rows[] = {"Set default app", "Set theme", UI_USB_SD_LABEL,
                          "Reload menu & SD"};
    ui_list(u, 8, list_y, 304, list_h, rows, 4, rows_st, widget_in,
            &s->settings_ls);
  } else if (s->screen == BOOT_SCREEN_DEFAULT) {
    char text[MENU_MAX][48];
    const char *rows[MENU_MAX];
    for (int i = 0; i < n; i++) {
      row_label(text[i], sizeof text[i], s->menu->items[i].title,
                i == s->default_index);
      rows[i] = text[i];
    }
    ui_list(u, 8, list_y, 304, list_h, rows, n, rows_st, widget_in,
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

  return 0;
}
