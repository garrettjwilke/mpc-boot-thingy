#pragma once
#include "boot_edit.h"
#include "menu.h"
#include "widgets.h"

struct input;

enum {
    BOOT_SCREEN_APPS = 0,
    BOOT_SCREEN_SETTINGS,
    BOOT_SCREEN_DEFAULT,
    BOOT_SCREEN_THEME,
    BOOT_SCREEN_EDIT,
    BOOT_SCREEN_ENTRY,
    BOOT_SCREEN_UNSAVED
};

enum {
    BOOT_ACT_NONE = 0,
    BOOT_ACT_LAUNCH,
    BOOT_ACT_SET_DEFAULT,
    BOOT_ACT_SAVE_THEME,
    BOOT_ACT_SAVE_BOOT
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
    struct ui_list_state edit_ls;
    struct ui_list_state entry_ls;
    struct ui_list_state unsaved_ls;
    int soft_active;
    struct ui_soft_state soft_ks;
    struct boot_patch catalog[BOOT_PATCH_MAX];
    int ncatalog;
    int enabled[BOOT_PATCH_MAX];
    int saved_on[BOOT_PATCH_MAX];
    int edit_new;
    char edit_id[MENU_ID];
    char edit_title[MENU_TITLE];
    char save_id[MENU_ID];
    char save_title[MENU_TITLE];
    char save_patches[BOOT_PATCH_MAX][BOOT_PATCH_ID];
    int save_npatches;
};

void boot_ui_init(struct boot_ui *ui, const struct menu *menu, int theme, int default_index);
void boot_ui_set_catalog(struct boot_ui *ui, const struct boot_patch *patches, int n);
int boot_ui_dirty(const struct boot_ui *ui);
void boot_ui_saved(struct boot_ui *ui);
int boot_frame(struct ui *u, const struct input *in, void *state);
void boot_draw_prompt(struct ui *u, int theme);
void boot_draw_loading(struct ui *u, int theme);
