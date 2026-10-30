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
  BOOT_SCREEN_STORAGE,
  BOOT_SCREEN_POWER,
  BOOT_SCREEN_ITEM_EDIT,
  BOOT_SCREEN_DELETE_CONFIRM,
  BOOT_SCREEN_ENTRY,
  BOOT_SCREEN_UNSAVED,
  BOOT_SCREEN_TEXT_EDIT,
  BOOT_SCREEN_FORMAT_CONFIRM,
  BOOT_SCREEN_FORMAT_PROGRESS
};

enum {
  BOOT_ACT_NONE = 0,
  BOOT_ACT_LAUNCH,
  BOOT_ACT_SET_DEFAULT,
  BOOT_ACT_SAVE_THEME,
  BOOT_ACT_SAVE_BOOT,
  BOOT_ACT_RELOAD,
  BOOT_ACT_USB_SD_DONE,
  BOOT_ACT_UPDATE_ENTRY,
  BOOT_ACT_DELETE_ENTRY,
  BOOT_ACT_REBOOT,
  BOOT_ACT_SHUTDOWN,
  BOOT_ACT_FORMAT_SD
};

enum { BOOT_TEXT_RENAME = 0, BOOT_TEXT_ARGS = 1, BOOT_TEXT_FORMAT_LABEL = 2 };

struct boot_ui {
  const struct menu *menu;
  int screen;
  int theme;
  int saved_theme;
  int default_index;
  int action;
  int action_index;
  char action_id[MENU_ID];
  struct ui_list_state apps_ls;
  struct ui_list_state settings_ls;
  struct ui_list_state default_ls;
  struct ui_list_state theme_ls;
  struct ui_list_state power_ls;
  struct ui_list_state item_ls;
  struct ui_list_state delete_confirm_ls;
  struct ui_list_state format_confirm_ls;
  struct ui_list_state entry_ls;
  struct ui_list_state unsaved_ls;
  int soft_active;
  struct ui_soft_state soft_ks;
  struct boot_patch catalog[BOOT_PATCH_MAX];
  int ncatalog;
  int enabled[BOOT_PATCH_MAX];
  int saved_on[BOOT_PATCH_MAX];
  int edit_new;
  int edit_is_app;
  int edit_boot_ac50;
  char edit_id[MENU_ID];
  char edit_title[MENU_TITLE];
  char edit_args[MENU_ARGS];
  char save_id[MENU_ID];
  char save_title[MENU_TITLE];
  char save_args[MENU_ARGS];
  char save_patches[BOOT_PATCH_MAX][BOOT_PATCH_ID];
  int save_npatches;
  struct ui_text_editor text_ed;
  char text_buf[MENU_ARGS];
  int text_edit_mode;
  int text_prev_screen;
  char notice[96];
  char format_label[32];
  char format_status[128];
  struct ui_usb_sd usb;
};

void boot_ui_init(struct boot_ui *ui, const struct menu *menu, int theme,
                  int default_index);
void boot_ui_set_catalog(struct boot_ui *ui, const struct boot_patch *patches,
                         int n);
int boot_ui_dirty(const struct boot_ui *ui);
void boot_ui_saved(struct boot_ui *ui);
void boot_ui_notice(struct boot_ui *ui, const char *text);
int boot_frame(struct ui *u, const struct input *in, void *state);
void boot_draw_prompt(struct ui *u, int theme);
void boot_draw_loading(struct ui *u, int theme);
