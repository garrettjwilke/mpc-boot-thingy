/* Host self-test: Stock first, drop-in apps and older boot-menu files, bad files skipped. */
#include "boot_ui.h"
#include "input.h"
#include "menu.h"
#include "theme.h"

#include <assert.h>
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
    write_file(path, "\n# a game\ntitle=Snake\nrun=apps/snake\nargs=\n");

    snprintf(path, sizeof path, "%s/apps/20-lab", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/apps/20-lab/boot.entry", dir);
    write_file(path, "title=Lab firmware\nrun=mpcfw\nargs=--demo one\n");
    snprintf(path, sizeof path, "%s/apps/20-lab/notes.txt", dir);
    write_file(path, "title=Not an entry\nrun=nope\n");
    snprintf(path, sizeof path, "%s/apps/loose-binary", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/apps/loose-binary/mpcfw", dir);
    write_file(path, "no boot.entry here\n");
    snprintf(path, sizeof path, "%s/apps/90-skip", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/apps/90-skip/boot.entry", dir);
    write_file(path, "title=Missing run\n");

    char lab[512];
    snprintf(lab, sizeof lab, "%s/apps/20-lab", dir);

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
    assert(strcmp(m.items[2].run, "apps/snake") == 0);
    assert(strcmp(m.items[2].base, dir) == 0);
    assert(strcmp(m.items[0].id, "stock") == 0);
    assert(strcmp(m.items[1].id, "app:20-lab") == 0);
    assert(strcmp(m.items[2].id, "entry:30-snake.entry") == 0);
    assert(menu_default_index(&m, dir) == 0);
    assert(menu_default_index(NULL, dir) == 0);

    snprintf(path, sizeof path, "%s/autorun/boot-default", dir);
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
    snprintf(path, sizeof path, "%s/autorun/boot-theme", dir);
    write_file(path, "nope\n");
    assert(theme_load(dir) == 0);
    write_file(path, "  blue\n");
    assert(theme_load(dir) == 1);

    static uint32_t px[UI_W * UI_H];
    struct ui view = {.px = px};
    struct boot_ui boot;
    boot_ui_init(&boot, &m, 0, 0);
    struct input in;
    input_begin(&in, 0);
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_APPS);
    in.down = 1u << IN_B3;
    boot_frame(&view, &in, &boot);
    assert(boot.screen == BOOT_SCREEN_SETTINGS);

    boot_ui_init(&boot, &m, 0, 1);
    input_begin(&in, 0);
    in.encoder = 1;
    boot_frame(&view, &in, &boot);
    assert(boot.apps_ls.cursor == 1);
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

    puts("selftest ok");
    return 0;
}
