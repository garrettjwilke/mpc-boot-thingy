#pragma once
#include "menu.h"
#include "widgets.h"

struct input;

enum {
    BOOT_SCREEN_APPS = 0,
    BOOT_SCREEN_SETTINGS,
    BOOT_SCREEN_DEFAULT,
    BOOT_SCREEN_THEME
};

enum {
    BOOT_ACT_NONE = 0,
    BOOT_ACT_LAUNCH,
    BOOT_ACT_SET_DEFAULT,
    BOOT_ACT_SAVE_THEME
};

struct boot_ui {
    const struct menu *menu;
    int screen;
    int theme;
    int saved_theme;
    int default_index;
    int action;
    int action_index;
    char action_id[MENU_ID];
    struct ui_menu shift_menu;
    struct ui_list_state apps_ls;
    struct ui_list_state settings_ls;
    struct ui_list_state default_ls;
    struct ui_list_state theme_ls;
    int soft_active;
    struct ui_soft_state soft_ks;
};

void boot_ui_init(struct boot_ui *ui, const struct menu *menu, int theme, int default_index);
int boot_frame(struct ui *u, const struct input *in, void *state);
void boot_draw_prompt(struct ui *u, int theme);
