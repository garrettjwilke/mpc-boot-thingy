/* Host self-test: Stock first, drop-in apps and older boot-menu files, bad files skipped. */
#include "boot_ui.h"
#include "input.h"
#include "menu.h"
#include "png.h"
#include "theme.h"
#include "ui.h"
#include "usb_storage.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static void mkdir_p(const char *path) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    assert(mkdir(buf, 0755) == 0 || access(buf, F_OK) == 0);
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    n = ftell(f);
    assert(n >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    buf = malloc((size_t)n + 1);
    assert(buf);
    assert(n == 0 || fread(buf, 1, (size_t)n, f) == (size_t)n);
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(void) {
    char dir[] = "/tmp/mpc-boot-thingy-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];

    snprintf(path, sizeof path, "%s/autorun/boot-menu", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/boot-menu/10-bad.entry", dir);
    write_file(path, "title=Bad\n# no run line\n");
    snprintf(path, sizeof path, "%s/autorun/boot-menu/05-skip.entry", dir);
    write_file(path, "run=apps/only\n");
    snprintf(path, sizeof path, "%s/autorun/boot-menu/30-snake.entry", dir);
    write_file(path, "\n# a game\ntitle=Snake\nrun=autorun/apps/snake\nargs=\n");

    snprintf(path, sizeof path, "%s/autorun/apps/20-lab", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/boot.entry", dir);
    write_file(path, "title=Lab firmware\nrun=mpcfw\nargs=--demo one\n");
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/notes.txt", dir);
    write_file(path, "title=Not an entry\nrun=nope\n");
    snprintf(path, sizeof path, "%s/autorun/apps/loose-binary", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/loose-binary/mpcfw", dir);
    write_file(path, "no boot.entry here\n");
    snprintf(path, sizeof path, "%s/autorun/apps/90-skip", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/90-skip/boot.entry", dir);
    write_file(path, "title=Missing run\n");

    char lab[512];
    snprintf(lab, sizeof lab, "%s/autorun/apps/20-lab", dir);

    struct menu m;
    assert(menu_load(&m, dir) == 3);
    assert(m.items[0].stock);
    assert(strcmp(m.items[0].title, "Stock") == 0);
    assert(m.items[0].run[0] == 0);
    assert(strcmp(m.items[1].title, "Lab firmware") == 0);
    assert(strcmp(m.items[1].run, "mpcfw") == 0);
    assert(strcmp(m.items[1].base, lab) == 0);
    assert(strcmp(m.items[1].args, "--demo one") == 0);
    assert(!m.items[1].stock);
    assert(strcmp(m.items[2].title, "Snake") == 0);
    assert(strcmp(m.items[2].run, "autorun/apps/snake") == 0);
    assert(strcmp(m.items[2].base, dir) == 0);
    assert(strcmp(m.items[0].id, "stock") == 0);
    assert(strcmp(m.items[1].id, "app:20-lab") == 0);
    assert(strcmp(m.items[2].id, "entry:30-snake.entry") == 0);
    assert(menu_default_index(&m, dir) == 0);
    assert(menu_default_index(NULL, dir) == 0);

    snprintf(path, sizeof path, "%s/autorun/system", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/system/boot-default", dir);
    write_file(path, "app:20-lab\n");
    assert(menu_default_index(&m, dir) == 1);
    write_file(path, "  entry:30-snake.entry\r\n");
    assert(menu_default_index(&m, dir) == 2);
    write_file(path, "stock\n");
    assert(menu_default_index(&m, dir) == 0);
    write_file(path, "\n");
    assert(menu_default_index(&m, dir) == 0);
    write_file(path, "app:missing\n");
    assert(menu_default_index(&m, dir) == 0);

    char buf[64];
    char *av[8];
    int n = menu_split_args(m.items[1].args, buf, sizeof buf, av, 8);
    assert(n == 2);
    assert(strcmp(av[0], "--demo") == 0 && strcmp(av[1], "one") == 0);
    assert(menu_split_args("", buf, sizeof buf, av, 8) == 0);
    assert(menu_split_args(NULL, buf, sizeof buf, av, 8) == 0);

    struct menu empty;
    assert(menu_load(&empty, NULL) == 1 && empty.items[0].stock);
    assert(strcmp(empty.items[0].id, "stock") == 0);
    assert(menu_default_index(&empty, NULL) == 0);
    assert(menu_load(&empty, "/no/such/boot-menu") == 1 && empty.items[0].stock);
    assert(menu_default_index(&empty, "/no/such/boot-menu") == 0);

    assert(menu_set_default(NULL, "stock") != 0);
    assert(menu_set_default(dir, "") != 0);
    assert(menu_set_default(dir, "app:20-lab") == 0);
    assert(menu_default_index(&m, dir) == 1);
    assert(menu_set_default(dir, "entry:30-snake.entry") == 0);
    assert(menu_default_index(&m, dir) == 2);
    assert(menu_set_default("/no/such/boot-menu", "stock") != 0);

    assert(theme_count() == 4);
    assert(strcmp(theme_at(0)->id, "amber") == 0);
    assert(theme_at(0)->bg == 0x101014 && theme_at(0)->hl == 0xffb000);
    assert(theme_load(dir) == 0);
    assert(theme_load(NULL) == 0);
    assert(theme_save(dir, 99) != 0);
    assert(theme_save(NULL, 1) != 0);
    assert(theme_save(dir, 2) == 0);
    assert(theme_load(dir) == 2);
    assert(strcmp(theme_at(theme_load(dir))->name, "Green") == 0);
    snprintf(path, sizeof path, "%s/autorun/system/boot-theme", dir);
    write_file(path, "nope\n");
    assert(theme_load(dir) == 0);
    write_file(path, "  blue\n");
    assert(theme_load(dir) == 1);

    static uint32_t px[UI_W * UI_H];
    struct ui view = {.px = px};
    struct boot_ui boot;
    boot_ui_init(&boot, &m, 0, 0);
    assert(boot.apps_ls.cursor == 1);
    struct input in;
    input_begin(&in, 0);
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_APPS);

    /* Test scrolling up to row 0 (USB storage) */
    input_begin(&in, 0);
    in.encoder = -1;
    boot_frame(&view, &in, &boot);
    assert(boot.apps_ls.cursor == 0);

    /* Shift menu should NOT open on row 0 (USB storage) */
    input_begin(&in, 0);
    in.shift = 1;
    in.held = 1u << IN_SHIFT;
    boot_frame(&view, &in, &boot);
    assert(boot.shift_menu.shown == 0);

    /* Pressing encoder on row 0 should start USB storage */
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_STORAGE);
    assert(boot.action == BOOT_ACT_START_USB_STORAGE);

    /* Return to apps screen */
    boot.screen = BOOT_SCREEN_APPS;
    boot.storage_status = USB_STORAGE_STATUS_IDLE;
    boot.action = BOOT_ACT_NONE;
    boot.apps_ls.cursor = 1;

    in.down = 1u << IN_B3;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_SETTINGS);

    /* Test shift menu to set default on an app row (cursor > 0) */
    boot_ui_init(&boot, &m, 0, 0);
    assert(boot.apps_ls.cursor == 1);
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.apps_ls.cursor == 2);
    assert(boot.action == BOOT_ACT_NONE);
    input_begin(&in, 0);
    in.shift = 1;
    in.held = 1u << IN_SHIFT;
    boot_frame(&view, &in, &boot);
    assert(boot.shift_menu.shown == 1);
    assert(boot.shift_menu.n == 2);
    assert(strcmp(boot.shift_menu.items[0].label, "Yes") == 0);
    assert(strcmp(boot.shift_menu.items[1].label, "No") == 0);
    assert(px[50 * UI_W + 262] == theme_at(0)->menu_fill);
    input_begin(&in, 0);
    in.shift = 1;
    in.held = 1u << IN_SHIFT;
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_SET_DEFAULT);
    assert(strcmp(boot.action_id, "app:20-lab") == 0);

    /* Test that default_index = 1 initializes cursor to 2 (pointing to app:20-lab) */
    boot_ui_init(&boot, &m, 0, 1);
    assert(boot.apps_ls.cursor == 2);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_LAUNCH);
    assert(strcmp(boot.action_id, "app:20-lab") == 0);

    boot_ui_init(&boot, &m, 0, 0);
    input_begin(&in, 0);
    in.down = 1u << IN_B3;
    boot_frame(&view, &in, &boot);
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_SETTINGS);
    assert(boot.settings_ls.cursor == 1);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_THEME);
    assert(boot.theme == 0);
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.theme == 1);
    assert(px[0] == theme_at(1)->bg);
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_SETTINGS);
    assert(boot.theme == 0);
    assert(px[0] == theme_at(0)->bg);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_THEME);
    input_begin(&in, 0);
    in.encoder = 2;
    boot_frame(&view, &in, &boot);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_SAVE_THEME);
    assert(boot.action_index == 2);
    assert(boot.saved_theme == 2);
    assert(boot.screen == BOOT_SCREEN_SETTINGS);

    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_APPS);
    input_begin(&in, 0);
    in.down = 1u << IN_B3;
    boot_frame(&view, &in, &boot);
    input_begin(&in, 0);
    in.encoder = -1;
    boot_frame(&view, &in, &boot);
    assert(boot.settings_ls.cursor == 0);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_DEFAULT);
    input_begin(&in, 0);
    in.encoder = 2;
    boot_frame(&view, &in, &boot);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_SET_DEFAULT);
    assert(strcmp(boot.action_id, "entry:30-snake.entry") == 0);
    assert(boot.screen == BOOT_SCREEN_SETTINGS);

    /* Test USB storage from settings menu (index 2) */
    boot.action = BOOT_ACT_NONE;
    input_begin(&in, 0);
    in.encoder = 2;
    boot_frame(&view, &in, &boot);
    assert(boot.settings_ls.cursor == 2);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_STORAGE);
    assert(boot.action == BOOT_ACT_START_USB_STORAGE);
    assert(boot.storage_status == USB_STORAGE_STATUS_PREPARING);

    /* Storage screen: simulate start succeeded */
    char dev_path[64];
    assert(usb_storage_find_sd_device(dev_path, sizeof dev_path) == 0);
    assert(strcmp(dev_path, "/dev/mock_mmcblk1") == 0);
    char err_buf[128];
    assert(usb_storage_start(err_buf, sizeof err_buf) == 0);
    boot.storage_status = USB_STORAGE_STATUS_ACTIVE;
    boot.action = BOOT_ACT_NONE;

    /* Verify lockout: encoder rotation and other keys do not leave storage screen */
    input_begin(&in, 0);
    in.encoder = 5;
    in.down = (1u << IN_B2) | (1u << IN_B3);
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_STORAGE);
    assert(boot.action == BOOT_ACT_NONE);
    assert(boot.storage_status == USB_STORAGE_STATUS_ACTIVE);

    /* Test exit via soft key B1 */
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_STOP_USB_STORAGE);
    assert(boot.storage_status == USB_STORAGE_STATUS_EXITING);

    /* Simulate main loop finishing stop */
    assert(usb_storage_stop(err_buf, sizeof err_buf) == 0);
    boot.action = BOOT_ACT_NONE;
    boot.storage_status = USB_STORAGE_STATUS_IDLE;
    boot.screen = BOOT_SCREEN_SETTINGS;

    /* Test exit via Encoder press */
    boot.settings_ls.cursor = 2;
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_STORAGE);
    assert(boot.action == BOOT_ACT_START_USB_STORAGE);
    boot.storage_status = USB_STORAGE_STATUS_ACTIVE;
    boot.action = BOOT_ACT_NONE;
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_STOP_USB_STORAGE);
    assert(boot.storage_status == USB_STORAGE_STATUS_EXITING);
    boot.action = BOOT_ACT_NONE;
    boot.storage_status = USB_STORAGE_STATUS_IDLE;
    boot.screen = BOOT_SCREEN_SETTINGS;

    /* Test storage error state and Back button */
    boot.settings_ls.cursor = 2;
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_STORAGE);
    usb_storage_set_mock(1, 1);
    assert(usb_storage_start(err_buf, sizeof err_buf) != 0);
    boot.storage_status = USB_STORAGE_STATUS_ERROR;
    snprintf(boot.storage_err, sizeof boot.storage_err, "%s", err_buf);
    boot.action = BOOT_ACT_NONE;
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_SETTINGS);
    usb_storage_set_mock(1, 0);

    /* Test reload from settings menu (now index 3) */
    boot.action = BOOT_ACT_NONE;
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.settings_ls.cursor == 3);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_RELOAD);

    /* Test reload from soft key 2 on apps screen */
    boot.action = BOOT_ACT_NONE;
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_APPS);
    input_begin(&in, 0);
    in.down = 1u << IN_B2;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_RELOAD);

    snprintf(path, sizeof path, "%s/autorun/patch-catalog", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/patch-catalog/wide-screen.list", dir);
    write_file(path, "# title Wide screen\npatch\n");
    snprintf(path, sizeof path, "%s/autorun/patch-catalog/extra-pads.list", dir);
    write_file(path, "pads\n");
    snprintf(path, sizeof path, "%s/autorun/patch-catalog/note.txt", dir);
    write_file(path, "nope\n");
    snprintf(path, sizeof path, "%s/autorun/system/patches", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/system/patches/boot-menu.list", dir);
    write_file(path, "menu-bytes\n");

    struct boot_patch catalog[8];
    int nc = boot_edit_list(dir, catalog, 8);
    assert(nc == 2);
    assert(strcmp(catalog[0].id, "extra-pads.list") == 0);
    assert(strcmp(catalog[0].title, "extra-pads.list") == 0);
    assert(strcmp(catalog[1].id, "wide-screen.list") == 0);
    assert(strcmp(catalog[1].title, "Wide screen") == 0);

    const char *both[] = {"wide-screen.list", "extra-pads.list"};
    char out_id[MENU_ID];
    char saved_id[MENU_ID];
    assert(boot_edit_save(dir, NULL, NULL, both, 2, out_id, sizeof out_id) == 0);
    assert(strcmp(out_id, "entry:boot-1.entry") == 0);
    snprintf(saved_id, sizeof saved_id, "%s", out_id);

    struct menu again;
    int found = -1;
    assert(menu_load(&again, dir) == 4);
    for (int i = 0; i < again.n; i++) {
        if (strcmp(again.items[i].id, saved_id) == 0) found = i;
    }
    assert(found > 0);
    assert(again.items[found].boot_ac50);
    assert(!again.items[0].boot_ac50);
    assert(strcmp(again.items[found].title, "Boot 1") == 0);
    assert(strcmp(again.items[found].run, "autorun/boot-edits/boot-1/launch.sh") == 0);
    assert(strcmp(again.items[found].patches, "wide-screen.list,extra-pads.list") == 0);

    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1/patches/wide-screen.list", dir);
    char *body = read_all(path);
    assert(strcmp(body, "# title Wide screen\npatch\n") == 0);
    free(body);
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1/patches/boot-menu.list", dir);
    body = read_all(path);
    assert(strcmp(body, "menu-bytes\n") == 0);
    free(body);
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1/launch.sh", dir);
    body = read_all(path);
    assert(strncmp(body, "#!/bin/sh\n", 10) == 0);
    assert(strstr(body, "MPC_PATCH_DIR"));
    assert(strstr(body, "mpc-patch-stamp"));
    assert(strstr(body, "could not replace the patch loader"));
    assert(strstr(body, "wide-screen.list"));
    assert(strstr(body, "extra-pads.list"));
    assert(strstr(body, "../../system/loader.so"));
    free(body);
    {
        struct stat st;
        assert(stat(path, &st) == 0);
        assert(st.st_mode & 0111);
    }

    const char *only[] = {"wide-screen.list"};
    const char *bad[] = {"missing.list"};
    assert(boot_edit_save(dir, "entry:30-snake.entry", "Snake", only, 1, out_id, sizeof out_id) == BOOT_EDIT_BAD_ID);
    assert(boot_edit_save(dir, saved_id, "", only, 1, out_id, sizeof out_id) == BOOT_EDIT_BAD_TITLE);
    assert(boot_edit_save(dir, NULL, "Nope", bad, 1, out_id, sizeof out_id) == BOOT_EDIT_BAD_PATCH);
    assert(boot_edit_save(dir, saved_id, "Lab mix", only, 1, out_id, sizeof out_id) == 0);
    assert(strcmp(out_id, saved_id) == 0);
    assert(menu_load(&again, dir) == 4);
    found = -1;
    for (int i = 0; i < again.n; i++) {
        if (strcmp(again.items[i].id, saved_id) == 0) found = i;
    }
    assert(found > 0);
    assert(strcmp(again.items[found].title, "Lab mix") == 0);
    assert(strcmp(again.items[found].patches, "wide-screen.list") == 0);
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1/patches/extra-pads.list", dir);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1/patches/wide-screen.list", dir);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1/patches/boot-menu.list", dir);
    assert(access(path, F_OK) == 0);

    assert(boot_edit_save(dir, NULL, "Second", NULL, 0, out_id, sizeof out_id) == 0);
    assert(strcmp(out_id, "entry:boot-2.entry") == 0);
    assert(menu_load(&again, dir) == 5);

    boot_ui_init(&boot, &again, 0, 0);
    boot_ui_set_catalog(&boot, catalog, nc);
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_EDIT);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ENTRY);
    assert(strcmp(boot.edit_title, "") == 0);
    assert(!boot_ui_dirty(&boot));
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_EDIT);
    assert(boot.action == BOOT_ACT_NONE);

    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ENTRY);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot_ui_dirty(&boot));
    assert(boot.enabled[0] == 1);
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_UNSAVED);
    input_begin(&in, 0);
    in.encoder = 2;
    boot_frame(&view, &in, &boot);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ENTRY);
    assert(boot_ui_dirty(&boot));
    assert(boot.enabled[0] == 1);
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_UNSAVED);
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_EDIT);
    assert(boot.action == BOOT_ACT_NONE);
    assert(!boot_ui_dirty(&boot));

    input_begin(&in, 0);
    in.encoder = 3; /* Move past New entry (0), Lab firmware (1), Snake (2) to Lab mix (3) */
    boot_frame(&view, &in, &boot);
    assert(boot.edit_ls.cursor == 3);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ITEM_EDIT);
    assert(strcmp(boot.edit_title, "Lab mix") == 0);
    assert(strcmp(boot.edit_id, saved_id) == 0);
    assert(boot.edit_boot_ac50 == 1);
    assert(!boot.edit_is_app);

    /* On item edit for patched entry, row 0 is Edit patches */
    assert(boot.item_ls.cursor == 0);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ENTRY);
    assert(boot.enabled[0] == 0);
    assert(boot.enabled[1] == 1);
    input_begin(&in, 0);
    in.down = 1u << IN_B3;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_SAVE_BOOT);
    assert(strcmp(boot.save_id, saved_id) == 0);
    assert(strcmp(boot.save_title, "Lab mix") == 0);
    assert(boot.save_npatches == 1);
    assert(strcmp(boot.save_patches[0], "wide-screen.list") == 0);
    boot_ui_saved(&boot);
    assert(boot.screen == BOOT_SCREEN_EDIT);

    /* Test selecting an installed app in Boot Edit (row 1: Lab firmware) */
    boot_ui_init(&boot, &again, 0, 0);
    boot_ui_set_catalog(&boot, catalog, nc);
    boot.screen = BOOT_SCREEN_EDIT;
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.edit_ls.cursor == 1);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ITEM_EDIT);
    assert(strcmp(boot.edit_id, "app:20-lab") == 0);
    assert(strcmp(boot.edit_title, "Lab firmware") == 0);
    assert(strcmp(boot.edit_args, "--demo one") == 0);
    assert(boot.edit_is_app == 1);
    assert(!boot.edit_boot_ac50);

    /* Row 0 on non-patched item: Rename entry -> enters BOOT_SCREEN_TEXT_EDIT */
    assert(boot.item_ls.cursor == 0);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_TEXT_EDIT);
    assert(boot.text_edit_mode == BOOT_TEXT_RENAME);
    assert(strcmp(boot.text_buf, "Lab firmware") == 0);
    /* Press B1 Cancel in text editor -> returns to BOOT_SCREEN_ITEM_EDIT without change */
    input_begin(&in, 0);
    in.down = 1u << IN_B1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ITEM_EDIT);
    assert(strcmp(boot.edit_title, "Lab firmware") == 0);
    assert(boot.action == BOOT_ACT_NONE);

    /* Open Rename entry again and confirm with B3 OK */
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_TEXT_EDIT);
    snprintf(boot.text_buf, sizeof boot.text_buf, "Lab Pro");
    boot.text_ed.len = (int)strlen(boot.text_buf);
    input_begin(&in, 0);
    in.down = 1u << IN_B3;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ITEM_EDIT);
    assert(boot.action == BOOT_ACT_UPDATE_ENTRY);
    assert(strcmp(boot.action_id, "app:20-lab") == 0);
    assert(strcmp(boot.save_title, "Lab Pro") == 0);
    assert(strcmp(boot.edit_title, "Lab Pro") == 0);

    /* Row 1 on non-patched item: Edit arguments */
    boot.action = BOOT_ACT_NONE;
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.item_ls.cursor == 1);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_TEXT_EDIT);
    assert(boot.text_edit_mode == BOOT_TEXT_ARGS);
    assert(strcmp(boot.text_buf, "--demo one") == 0);
    snprintf(boot.text_buf, sizeof boot.text_buf, "--fast --stereo");
    boot.text_ed.len = (int)strlen(boot.text_buf);
    input_begin(&in, 0);
    in.down = 1u << IN_B3;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ITEM_EDIT);
    assert(boot.action == BOOT_ACT_UPDATE_ENTRY);
    assert(strcmp(boot.save_args, "--fast --stereo") == 0);

    /* Row 2: Delete app completely -> opens BOOT_SCREEN_DELETE_CONFIRM */
    boot.action = BOOT_ACT_NONE;
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.item_ls.cursor == 2);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_DELETE_CONFIRM);

    /* On delete confirm, row 0 is "No, keep it" -> returns to ITEM_EDIT */
    assert(boot.delete_confirm_ls.cursor == 0);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ITEM_EDIT);
    assert(boot.action == BOOT_ACT_NONE);

    /* Enter delete confirm again and select row 1 "Yes, delete ..." */
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_DELETE_CONFIRM);
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.delete_confirm_ls.cursor == 1);
    input_begin(&in, 0);
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.action == BOOT_ACT_DELETE_ENTRY);
    assert(strcmp(boot.action_id, "app:20-lab") == 0);
    assert(boot.screen == BOOT_SCREEN_EDIT);

    /* Test B2 Rename button in BOOT_SCREEN_ENTRY */
    input_begin(&in, 0);
    boot.edit_ls.cursor = 0; /* New entry */
    in.encoder_press = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ENTRY);
    assert(boot.edit_new);
    input_begin(&in, 0);
    in.down = 1u << IN_B2; /* Rename soft key */
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_TEXT_EDIT);
    assert(boot.text_prev_screen == BOOT_SCREEN_ENTRY);
    snprintf(boot.text_buf, sizeof boot.text_buf, "My Custom Boot");
    boot.text_ed.len = (int)strlen(boot.text_buf);
    input_begin(&in, 0);
    in.down = 1u << IN_B3; /* OK */
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_ENTRY);
    assert(strcmp(boot.edit_title, "My Custom Boot") == 0);

    /* Backend tests for boot_entry_update and boot_entry_delete */
    assert(boot_entry_delete(dir, "stock") != 0);
    assert(boot_entry_update(dir, "stock", "Foo", "Bar") != 0);

    /* Update regular entry */
    assert(boot_entry_update(dir, "entry:30-snake.entry", "Snake DX", "--lives 5") == 0);
    struct menu updated;
    assert(menu_load(&updated, dir) == 5);
    int snk = -1;
    for (int i = 0; i < updated.n; i++) {
        if (strcmp(updated.items[i].id, "entry:30-snake.entry") == 0) snk = i;
    }
    assert(snk > 0);
    assert(strcmp(updated.items[snk].title, "Snake DX") == 0);
    assert(strcmp(updated.items[snk].args, "--lives 5") == 0);

    /* Delete regular entry */
    snprintf(path, sizeof path, "%s/autorun/boot-menu/30-snake.entry", dir);
    assert(access(path, F_OK) == 0);
    assert(boot_entry_delete(dir, "entry:30-snake.entry") == 0);
    assert(access(path, F_OK) != 0);
    assert(menu_load(&updated, dir) == 4);

    /* Delete app folder completely */
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab", dir);
    assert(access(path, F_OK) == 0);
    /* Set default to app:20-lab first to verify default reset to stock */
    assert(menu_set_default(dir, "app:20-lab") == 0);
    assert(menu_default_index(&updated, dir) == 1);
    assert(boot_entry_delete(dir, "app:20-lab") == 0);
    assert(access(path, F_OK) != 0);
    assert(menu_load(&updated, dir) == 3);
    assert(menu_default_index(&updated, dir) == 0); /* Reset to stock */

    /* Delete patched entry */
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1", dir);
    assert(access(path, F_OK) == 0);
    assert(boot_entry_delete(dir, saved_id) == 0);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/autorun/boot-menu/boot-1.entry", dir);
    assert(access(path, F_OK) != 0);

    assert(png_load_screen("splash-screen.png", px) == 0);
    uint64_t sum = 0;
    for (int i = 0; i < UI_W * UI_H; i++) sum = sum * 131 + px[i];
    assert(sum == 4867112348204652761ull);
    assert(px[0] == 0);
    assert(px[120 * UI_W + 160] == 0x00bcb8aa);
    assert(png_load_screen("no-such-splash-screen.png", px) != 0);
    static const unsigned char tiny_png[] = {
        137, 80,  78,  71,  13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,   0,   1,   0,   0,   0,
        1,   8,   2,   0,   0,   0,   144, 119, 83,  222, 0,   0,   0,   12,  73,  68,  65,  84,  120, 156, 99,  248, 207,
        192, 0,   0,   3,   1,   1,   0,   201, 254, 146, 239, 0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96,  130};
    char tiny_path[64];
    snprintf(tiny_path, sizeof tiny_path, "/tmp/mpc-png-tiny-%d.png", (int)getpid());
    FILE *tf = fopen(tiny_path, "wb");
    assert(tf);
    assert(fwrite(tiny_png, 1, sizeof tiny_png, tf) == sizeof tiny_png);
    fclose(tf);
    assert(png_load_screen(tiny_path, px) != 0);
    unlink(tiny_path);
    snprintf(tiny_path, sizeof tiny_path, "/tmp/mpc-png-text-%d.png", (int)getpid());
    write_file(tiny_path, "not a png");
    assert(png_load_screen(tiny_path, px) != 0);
    unlink(tiny_path);

    puts("selftest ok");
    return 0;
}
