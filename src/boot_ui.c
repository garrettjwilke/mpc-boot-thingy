#include "boot_ui.h"
#include "font.h"
#include "input.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static const struct ui_menu_item set_default_items[] = {
    {.label = "Yes", .id = 1, .pad = 16, .key = -1},
    {.label = "No", .id = 2, .pad = 16, .key = -1},
};

static int wrap(int v, int n) {
    if (n <= 0) return 0;
    v %= n;
    if (v < 0) v += n;
    return v;
}

static void move_cursor(struct ui_list_state *ls, int n, struct input *ev) {
    if (!ev || !ev->encoder || n <= 0) return;
    ls->cursor = wrap(ls->cursor + ev->encoder, n);
    ev->encoder = 0;
}

static void row_label(char *dst, size_t n, const char *title, int mark) {
    const char *suffix = mark ? " (default)" : "";
    size_t suf = strlen(suffix);
    size_t max = n > 0 ? n - 1 : 0;
    if (suf > max) suf = 0;
    size_t keep = max - suf;
    size_t len = title ? strlen(title) : 0;
    if (len > keep) len = keep;
    if (len && title) memcpy(dst, title, len);
    if (suf) memcpy(dst + len, suffix, suf);
    dst[len + suf] = 0;
}

static void fit_text(char *dst, size_t n, const char *src) {
    size_t len = src ? strlen(src) : 0;
    if (n == 0) return;
    if (len >= n) len = n - 1;
    if (len && src) memcpy(dst, src, len);
    dst[len] = 0;
}

static struct ui_soft_style soft_style(const struct boot_theme *th) {
    struct ui_soft_style st = {
        .fill = th->box, .text = th->text, .active_text = th->active_text, .highlight = th->hl, .gap = 4,
        .feedback = UI_SOFT_FLASH, .flash_ms = 120,
    };
    return st;
}

static struct ui_list_style list_style(const struct boot_theme *th) {
    struct ui_list_style st = {
        .text = th->text, .highlight = th->hl, .row_h = 20, .highlight_px = 2,
        .current_fill_set = 1, .current_fill = th->bar,
    };
    return st;
}

static struct ui_menu_style menu_style(const struct boot_theme *th) {
    struct ui_menu_style st = ui_menu_style_default();
    st.fill = th->menu_fill;
    st.text = th->text;
    st.title = th->hl;
    st.highlight = th->hl;
    st.border = th->border;
    st.current_fill_set = 1;
    st.current_fill = th->bar;
    st.light_pads = 0;
    return st;
}

static void note_default(struct boot_ui *s, int index) {
    if (!s->menu || index < 0 || index >= s->menu->n) return;
    s->action = BOOT_ACT_SET_DEFAULT;
    s->action_index = index;
    s->default_index = index;
    snprintf(s->action_id, sizeof s->action_id, "%s", s->menu->items[index].id);
}

static int edit_count(const struct boot_ui *s) {
    int n = 0;
    if (!s->menu) return 0;
    for (int i = 0; i < s->menu->n; i++) {
        if (s->menu->items[i].boot_ac50) n++;
    }
    return n;
}

static void mark_enabled(struct boot_ui *s, const char *list) {
    char buf[MENU_PATCHES];
    char *p;
    memset(s->enabled, 0, sizeof s->enabled);
    if (!list || !list[0]) return;
    snprintf(buf, sizeof buf, "%s", list);
    p = buf;
    while (*p) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        for (int i = 0; i < s->ncatalog; i++) {
            if (strcmp(s->catalog[i].id, p) == 0) s->enabled[i] = 1;
        }
        if (!comma) break;
        p = comma + 1;
    }
}

static void open_entry(struct boot_ui *s, int cursor) {
    int seen = 0;
    memset(s->enabled, 0, sizeof s->enabled);
    memset(s->saved_on, 0, sizeof s->saved_on);
    s->entry_ls.cursor = 0;
    s->edit_id[0] = 0;
    s->edit_title[0] = 0;
    s->edit_new = 1;
    if (cursor > 0 && s->menu) {
        for (int i = 0; i < s->menu->n; i++) {
            if (!s->menu->items[i].boot_ac50) continue;
            seen++;
            if (seen != cursor) continue;
            s->edit_new = 0;
            snprintf(s->edit_id, sizeof s->edit_id, "%s", s->menu->items[i].id);
            snprintf(s->edit_title, sizeof s->edit_title, "%s", s->menu->items[i].title);
            mark_enabled(s, s->menu->items[i].patches);
            memcpy(s->saved_on, s->enabled, sizeof s->enabled);
            break;
        }
    }
    s->screen = BOOT_SCREEN_ENTRY;
}

static void queue_save(struct boot_ui *s) {
    s->action = BOOT_ACT_SAVE_BOOT;
    s->save_id[0] = 0;
    s->save_title[0] = 0;
    if (!s->edit_new) {
        snprintf(s->save_id, sizeof s->save_id, "%s", s->edit_id);
        snprintf(s->save_title, sizeof s->save_title, "%s", s->edit_title);
    }
    s->save_npatches = 0;
    for (int i = 0; i < s->ncatalog && s->save_npatches < BOOT_PATCH_MAX; i++) {
        if (!s->enabled[i]) continue;
        snprintf(s->save_patches[s->save_npatches], BOOT_PATCH_ID, "%s", s->catalog[i].id);
        s->save_npatches++;
    }
}

static void discard_entry(struct boot_ui *s) {
    memcpy(s->enabled, s->saved_on, sizeof s->enabled);
    s->screen = BOOT_SCREEN_EDIT;
}

void boot_ui_init(struct boot_ui *ui, const struct menu *menu, int theme, int default_index) {
    if (!ui) return;
    memset(ui, 0, sizeof *ui);
    ui->menu = menu;
    if (theme < 0 || theme >= theme_count()) theme = 0;
    ui->theme = theme;
    ui->saved_theme = theme;
    ui->theme_ls.cursor = theme;
    ui->default_index = default_index;
    ui->default_ls.cursor = default_index;
    ui_menu_prepare(&ui->shift_menu, 1u << IN_SHIFT, "Set as default?", set_default_items, 2);
}

void boot_ui_set_catalog(struct boot_ui *ui, const struct boot_patch *patches, int n) {
    if (!ui) return;
    if (n < 0) n = 0;
    if (n > BOOT_PATCH_MAX) n = BOOT_PATCH_MAX;
    if (n > 0 && patches) memcpy(ui->catalog, patches, (size_t)n * sizeof *patches);
    ui->ncatalog = n > 0 && patches ? n : 0;
}

int boot_ui_dirty(const struct boot_ui *ui) {
    if (!ui) return 0;
    return memcmp(ui->enabled, ui->saved_on, sizeof ui->enabled) != 0;
}

void boot_ui_saved(struct boot_ui *ui) {
    if (!ui) return;
    ui->screen = BOOT_SCREEN_EDIT;
    memset(ui->enabled, 0, sizeof ui->enabled);
    memset(ui->saved_on, 0, sizeof ui->saved_on);
    ui->edit_new = 0;
    ui->edit_id[0] = 0;
    ui->edit_title[0] = 0;
    ui->save_npatches = 0;
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
    ui_text(u, (UI_W - (int)strlen(a) * gw) / 2, (UI_H - gh) / 2, a, th->hl, scale);
}

int boot_frame(struct ui *u, const struct input *in, void *state) {
    struct boot_ui *s = state;
    if (!u || !s) return 0;
    s->action = BOOT_ACT_NONE;
    s->action_index = 0;
    s->action_id[0] = 0;

    struct input ev;
    if (in) ev = *in;
    else input_begin(&ev, 0);

    int shift_open = s->screen == BOOT_SCREEN_APPS && ui_menu_open(&s->shift_menu, &ev);
    if (!shift_open) {
        int screen = s->screen;
        int n = s->menu ? s->menu->n : 0;
        int edits = 1 + edit_count(s);
        if (edits > MENU_MAX) edits = MENU_MAX;
        if (screen == BOOT_SCREEN_SETTINGS) move_cursor(&s->settings_ls, 2, &ev);
        else if (screen == BOOT_SCREEN_DEFAULT) move_cursor(&s->default_ls, n, &ev);
        else if (screen == BOOT_SCREEN_THEME) {
            move_cursor(&s->theme_ls, theme_count(), &ev);
            s->theme = s->theme_ls.cursor;
        } else if (screen == BOOT_SCREEN_EDIT) move_cursor(&s->edit_ls, edits, &ev);
        else if (screen == BOOT_SCREEN_ENTRY) move_cursor(&s->entry_ls, s->ncatalog, &ev);
        else if (screen == BOOT_SCREEN_UNSAVED) move_cursor(&s->unsaved_ls, 3, &ev);

        int left = 0;
        if (screen == BOOT_SCREEN_APPS && input_down(&ev, IN_B1)) {
            s->screen = BOOT_SCREEN_EDIT;
            left = 1;
        } else if (screen == BOOT_SCREEN_APPS && input_down(&ev, IN_B3)) {
            s->screen = BOOT_SCREEN_SETTINGS;
            left = 1;
        } else if (screen == BOOT_SCREEN_ENTRY && input_down(&ev, IN_B3)) {
            queue_save(s);
            left = 1;
        } else if (screen != BOOT_SCREEN_APPS && input_down(&ev, IN_B1)) {
            if (screen == BOOT_SCREEN_THEME) {
                s->theme = s->saved_theme;
                s->theme_ls.cursor = s->saved_theme;
                s->screen = BOOT_SCREEN_SETTINGS;
            } else if (screen == BOOT_SCREEN_ENTRY) {
                s->screen = boot_ui_dirty(s) ? BOOT_SCREEN_UNSAVED : BOOT_SCREEN_EDIT;
                if (s->screen == BOOT_SCREEN_UNSAVED) s->unsaved_ls.cursor = 0;
            } else if (screen == BOOT_SCREEN_UNSAVED) {
                s->screen = BOOT_SCREEN_ENTRY;
            } else if (screen == BOOT_SCREEN_EDIT) {
                s->screen = BOOT_SCREEN_APPS;
            } else if (screen == BOOT_SCREEN_SETTINGS) {
                s->screen = BOOT_SCREEN_APPS;
            } else {
                s->screen = BOOT_SCREEN_SETTINGS;
            }
            left = 1;
        }

        if (left) ev.encoder_press = 0;

        if (!left && ev.encoder_press && !ev.shift) {
            if (screen == BOOT_SCREEN_SETTINGS) {
                if (s->settings_ls.cursor == 0) {
                    s->screen = BOOT_SCREEN_DEFAULT;
                    s->default_ls.cursor = s->default_index;
                } else {
                    s->screen = BOOT_SCREEN_THEME;
                    s->theme = s->saved_theme;
                    s->theme_ls.cursor = s->saved_theme;
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
    }

    const struct boot_theme *th = theme_at(s->theme);
    ui_clear(u, th->bg);

    const char *labels[3];
    if (s->screen == BOOT_SCREEN_APPS) {
        labels[0] = "Boot Edit";
        labels[1] = "";
        labels[2] = "Settings";
    } else if (s->screen == BOOT_SCREEN_ENTRY) {
        labels[0] = "Back";
        labels[1] = "";
        labels[2] = "Save";
    } else {
        labels[0] = "Back";
        labels[1] = "";
        labels[2] = "";
    }
    const struct input *widget_in = shift_open ? 0 : &ev;
    struct ui_soft_style soft = soft_style(th);
    ui_soft_keys(u, 8, 4, 304, 28, labels, &s->soft_active, soft, widget_in, &s->soft_ks);

    const char *title = "mpc-boot-thingy";
    const char *hint = "";
    char title_buf[48];
    if (s->screen == BOOT_SCREEN_SETTINGS) title = "Settings";
    else if (s->screen == BOOT_SCREEN_DEFAULT) title = "Default app";
    else if (s->screen == BOOT_SCREEN_THEME) title = "Theme";
    else if (s->screen == BOOT_SCREEN_EDIT) title = "Boot Edit";
    else if (s->screen == BOOT_SCREEN_ENTRY) {
        fit_text(title_buf, sizeof title_buf, s->edit_new ? "New entry" : s->edit_title);
        title = title_buf;
        hint = "press to toggle";
    } else if (s->screen == BOOT_SCREEN_UNSAVED) {
        title = "Not saved yet";
        hint = "this entry has changes";
    } else hint = "hold shift to set default";
    ui_text(u, 12, 40, title, th->text, 1);
    if (hint[0]) ui_text(u, 12, 56, hint, th->dim, 1);

    int list_y = hint[0] ? 76 : 60;
    int list_h = UI_H - list_y - 8;
    struct ui_list_style rows_st = list_style(th);
    int n = s->menu ? s->menu->n : 0;
    if (n > MENU_MAX) n = MENU_MAX;

    if (s->screen == BOOT_SCREEN_APPS) {
        char text[MENU_MAX][48];
        const char *rows[MENU_MAX];
        for (int i = 0; i < n; i++) {
            row_label(text[i], sizeof text[i], s->menu->items[i].title, i == s->default_index);
            rows[i] = text[i];
        }
        int hit = ui_list(u, 8, list_y, 304, list_h, rows, n, rows_st, widget_in, &s->apps_ls);
        if (hit >= 0) {
            s->action = BOOT_ACT_LAUNCH;
            s->action_index = hit;
            snprintf(s->action_id, sizeof s->action_id, "%s", s->menu->items[hit].id);
        }
    } else if (s->screen == BOOT_SCREEN_SETTINGS) {
        const char *rows[] = {"Set default app", "Set theme"};
        ui_list(u, 8, list_y, 304, list_h, rows, 2, rows_st, widget_in, &s->settings_ls);
    } else if (s->screen == BOOT_SCREEN_DEFAULT) {
        char text[MENU_MAX][48];
        const char *rows[MENU_MAX];
        for (int i = 0; i < n; i++) {
            row_label(text[i], sizeof text[i], s->menu->items[i].title, i == s->default_index);
            rows[i] = text[i];
        }
        ui_list(u, 8, list_y, 304, list_h, rows, n, rows_st, widget_in, &s->default_ls);
    } else if (s->screen == BOOT_SCREEN_THEME) {
        const char *rows[8];
        int tn = theme_count();
        if (tn > 8) tn = 8;
        for (int i = 0; i < tn; i++) rows[i] = theme_at(i)->name;
        ui_list(u, 8, list_y, 304, list_h, rows, tn, rows_st, widget_in, &s->theme_ls);
    } else if (s->screen == BOOT_SCREEN_EDIT) {
        char text[MENU_MAX][48];
        const char *rows[MENU_MAX];
        int shown = 0;
        rows[shown++] = "New entry";
        if (s->menu) {
            for (int i = 0; i < s->menu->n && shown < MENU_MAX; i++) {
                if (!s->menu->items[i].boot_ac50) continue;
                fit_text(text[shown], sizeof text[shown], s->menu->items[i].title);
                rows[shown] = text[shown];
                shown++;
            }
        }
        int hit = ui_list(u, 8, list_y, 304, list_h, rows, shown, rows_st, widget_in, &s->edit_ls);
        if (hit >= 0) open_entry(s, hit);
    } else if (s->screen == BOOT_SCREEN_ENTRY) {
        char text[BOOT_PATCH_MAX][48];
        const char *rows[BOOT_PATCH_MAX];
        int pn = s->ncatalog;
        if (pn > BOOT_PATCH_MAX) pn = BOOT_PATCH_MAX;
        for (int i = 0; i < pn; i++) {
            const char *name = s->catalog[i].title[0] ? s->catalog[i].title : s->catalog[i].id;
            snprintf(text[i], sizeof text[i], "%s%s", s->enabled[i] ? "On  " : "    ", name);
            rows[i] = text[i];
        }
        int hit = ui_list(u, 8, list_y, 304, list_h, rows, pn, rows_st, widget_in, &s->entry_ls);
        if (hit >= 0 && hit < pn) s->enabled[hit] = !s->enabled[hit];
    } else if (s->screen == BOOT_SCREEN_UNSAVED) {
        const char *rows[] = {"Save", "Don't save", "Cancel"};
        int hit = ui_list(u, 8, list_y, 304, list_h, rows, 3, rows_st, widget_in, &s->unsaved_ls);
        if (hit == 0) queue_save(s);
        else if (hit == 1) discard_entry(s);
        else if (hit == 2) s->screen = BOOT_SCREEN_ENTRY;
    }

    if (s->screen == BOOT_SCREEN_APPS) {
        struct ui_menu_style mst = menu_style(th);
        int id = ui_menu(u, 48, 44, 224, 152, &s->shift_menu, mst, &ev);
        if (id == 1) note_default(s, n > 0 ? wrap(s->apps_ls.cursor, n) : 0);
    }
    return 0;
}
