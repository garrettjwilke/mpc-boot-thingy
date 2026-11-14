/* Host self-test: Stock first, drop-in apps and older boot-menu files, bad
 * files skipped. */
#include "app_install.h"
#include "boot_ui.h"
#include "file_ops.h"
#include "format_sd.h"
#include "icon.h"
#include "input.h"
#include "menu.h"
#include "png.h"
#include "screen_capture.h"
#include "sd_check.h"
#include "testapp.h"
#include "theme.h"
#include "ui.h"
#include "unzip.h"
#include "usb_apps.h"
#include "usb_sd.h"
#include "zip_writer.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int setenv(const char *name, const char *value, int overwrite);

static void dummy_progress(const char *phase, void *user) {
  if (phase && user)
    snprintf((char *)user, 128, "%s", phase);
}

struct test_prog_ctx {
  int calls;
  int cancel_after;
};

static int test_prog(void *u, const char *name, unsigned long long bd,
                     unsigned long long tb, size_t fd, size_t tf) {
  (void)name;
  (void)bd;
  (void)tb;
  (void)fd;
  (void)tf;
  struct test_prog_ctx *c = (struct test_prog_ctx *)u;
  c->calls++;
  if (c->cancel_after && c->calls >= c->cancel_after)
    return -1;
  return 0;
}

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

  /* Tile pictures. A 4x4 solid #123456 PNG. */
  static const unsigned char tile_png[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
      0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00, 0x00, 0x00,
      0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10, 0x32, 0x09, 0xfb,
      0x8f, 0x8c, 0x19, 0x48, 0x17, 0x00, 0x00, 0x52, 0x89, 0x19, 0xb1, 0x95,
      0x3a, 0x06, 0x50, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
      0x42, 0x60, 0x82};
  {
    /* No PNG anywhere: every row draws a generated tile. */
    assert(m.items[0].icon[0] == 0);
    assert(m.items[1].icon[0] == 0);
    assert(m.items[2].icon[0] == 0);

    /* icon.png inside the app folder. */
    char icon_path[512];
    snprintf(icon_path, sizeof icon_path, "%s/icon.png", lab);
    FILE *f = fopen(icon_path, "wb");
    assert(f);
    assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
    fclose(f);
    /* <name>.png beside <name>.entry. */
    char snake_png[512];
    snprintf(snake_png, sizeof snake_png,
             "%s/autorun/boot-menu/30-snake.png", dir);
    f = fopen(snake_png, "wb");
    assert(f);
    assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
    fclose(f);
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, icon_path) == 0);
    assert(strcmp(m.items[2].icon, snake_png) == 0);

    /* Without icon.png, the package's launch screen is the tile. icon.png
       wins when both exist. */
    char splash[512];
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/autorun", dir);
    mkdir_p(path);
    snprintf(splash, sizeof splash,
             "%s/autorun/apps/20-lab/autorun/splash-screen.png", dir);
    f = fopen(splash, "wb");
    assert(f);
    assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
    fclose(f);
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, icon_path) == 0);
    unlink(icon_path);
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, splash) == 0);
    unlink(splash);
    snprintf(splash, sizeof splash, "%s/autorun/apps/20-lab/splash-screen.png",
             dir);
    f = fopen(splash, "wb");
    assert(f);
    assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
    fclose(f);
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, splash) == 0);
    unlink(splash);
    f = fopen(icon_path, "wb");
    assert(f);
    assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
    fclose(f);
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, icon_path) == 0);

    /* Launch screens in other formats. A binary PPM is read by its header;
       a headerless 320x240 XRGB dump by its size. */
    {
      char ppm_path[512], raw_path[512], deep[512];
      snprintf(path, sizeof path, "%s/autorun/apps/20-lab/autorun", dir);
      mkdir_p(path);
      snprintf(ppm_path, sizeof ppm_path,
               "%s/autorun/apps/20-lab/autorun/splash1.ppm", dir);
      f = fopen(ppm_path, "wb");
      assert(f);
      fputs("P6\n2 2\n255\n", f);
      for (int i = 0; i < 4; i++)
        assert(fwrite("\x12\x34\x56", 1, 3, f) == 3);
      fclose(f);
      static uint32_t tile2[BOOT_ICON_PX * BOOT_ICON_PX];
      assert(icon_load(ppm_path, tile2, BOOT_ICON_PX) == 0);
      assert(tile2[0] == 0xff123456);

      snprintf(raw_path, sizeof raw_path,
               "%s/autorun/apps/20-lab/autorun/qol-splash.raw", dir);
      f = fopen(raw_path, "wb");
      assert(f);
      for (int i = 0; i < 320 * 240; i++)
        assert(fwrite("\x56\x34\x12\x00", 1, 4, f) == 4); /* B G R X */
      fclose(f);
      assert(icon_load(raw_path, tile2, BOOT_ICON_PX) == 0);
      assert((tile2[0] >> 24) == 0);               /* letterbox bar */
      assert(tile2[6 * BOOT_ICON_PX] == 0xff123456); /* picture */

      /* A wrong-size .raw is not a launch picture. */
      snprintf(path, sizeof path, "%s/autorun/apps/20-lab/autorun/a-splash.raw",
               dir);
      write_file(path, "short");

      /* With no icon.png: splash-screen.png is not there, so the sorted
         scan of autorun/ picks qol-splash.raw before splash1.ppm. */
      unlink(icon_path);
      assert(menu_load(&m, dir) == 3);
      assert(strcmp(m.items[1].icon, raw_path) == 0);
      unlink(raw_path);
      assert(menu_load(&m, dir) == 3);
      assert(strcmp(m.items[1].icon, ppm_path) == 0);
      /* A shallower picture wins over a deeper one. */
      snprintf(path, sizeof path, "%s/autorun/apps/20-lab/my-splash.png", dir);
      f = fopen(path, "wb");
      assert(f);
      assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
      fclose(f);
      assert(menu_load(&m, dir) == 3);
      assert(strcmp(m.items[1].icon, path) == 0);
      unlink(path);
      unlink(ppm_path);
      snprintf(path, sizeof path, "%s/autorun/apps/20-lab/autorun/a-splash.raw",
               dir);
      unlink(path);
      assert(menu_load(&m, dir) == 3);
      assert(m.items[1].icon[0] == 0);

      /* A card package: dirs= names card-root folders, searched three deep. */
      snprintf(path, sizeof path, "%s/autorun/boot-menu/45-grid.entry", dir);
      write_file(path, "title=Grid\nrun=Grid/start.sh\ncard=1\ndirs=Grid\n");
      snprintf(path, sizeof path, "%s/Grid/releases/abc", dir);
      mkdir_p(path);
      snprintf(deep, sizeof deep, "%s/Grid/releases/abc/splash.png", dir);
      f = fopen(deep, "wb");
      assert(f);
      assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
      fclose(f);
      assert(menu_load(&m, dir) == 4);
      assert(strcmp(m.items[3].title, "Grid") == 0);
      assert(strcmp(m.items[3].icon, deep) == 0);
      snprintf(path, sizeof path, "%s/autorun/boot-menu/45-grid.entry", dir);
      unlink(path);
      unlink(deep);
      assert(menu_load(&m, dir) == 3);

      f = fopen(icon_path, "wb");
      assert(f);
      assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
      fclose(f);
      assert(menu_load(&m, dir) == 3);
      assert(strcmp(m.items[1].icon, icon_path) == 0);
    }

    /* A wide picture is fitted: full width, centered, transparent bars. */
    {
      static uint32_t fit[BOOT_ICON_PX * BOOT_ICON_PX];
      assert(icon_load("splash-screen.png", fit, BOOT_ICON_PX) == 0);
      assert((fit[0] >> 24) == 0);                                 /* bar */
      assert((fit[(BOOT_ICON_PX - 1) * BOOT_ICON_PX] >> 24) == 0); /* bar */
      assert((fit[6 * BOOT_ICON_PX] >> 24) == 255);  /* 48x36 picture */
      assert((fit[41 * BOOT_ICON_PX + 47] >> 24) == 255);
      assert((fit[5 * BOOT_ICON_PX] >> 24) == 0);
      assert((fit[42 * BOOT_ICON_PX] >> 24) == 0);
    }

    /* icon= names a file relative to the app folder. A missing file falls
       back to icon.png. */
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/boot.entry", dir);
    write_file(path,
               "title=Lab firmware\nrun=mpcfw\nargs=--demo one\nicon=art/x.png\n");
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, icon_path) == 0);
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/art", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/art/x.png", dir);
    f = fopen(path, "wb");
    assert(f);
    assert(fwrite(tile_png, 1, sizeof tile_png, f) == sizeof tile_png);
    fclose(f);
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, path) == 0);
    unlink(path);
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/boot.entry", dir);
    write_file(path, "title=Lab firmware\nrun=mpcfw\nargs=--demo one\n");
    assert(menu_load(&m, dir) == 3);
    assert(strcmp(m.items[1].icon, icon_path) == 0);

    /* Decoding fills the square with the picture, straight alpha. */
    static uint32_t tile[BOOT_ICON_PX * BOOT_ICON_PX];
    assert(icon_load(icon_path, tile, BOOT_ICON_PX) == 0);
    assert(tile[0] == 0xff123456);
    assert(tile[BOOT_ICON_PX * BOOT_ICON_PX - 1] == 0xff123456);
    assert(icon_load(path, tile, BOOT_ICON_PX) != 0); /* boot.entry is not a PNG */
    assert(icon_load("/no/such/icon.png", tile, BOOT_ICON_PX) != 0);
    assert(icon_load(NULL, tile, BOOT_ICON_PX) != 0);

    /* Blit composites alpha over the background. */
    static uint32_t fb[UI_W * UI_H];
    struct ui blit_ui = {.px = fb};
    uint32_t half[4] = {0x80ff0000, 0x00ff0000, 0xff00ff00, 0x80ff0000};
    ui_clear(&blit_ui, 0x000000);
    icon_blit(&blit_ui, 0, 0, half, 2, 0x000000);
    assert(fb[0] == 0x800000);
    assert(fb[1] == 0x000000);
    assert(fb[UI_W] == 0x00ff00);
    /* Off-screen pixels are skipped. */
    icon_blit(&blit_ui, UI_W - 1, UI_H - 1, half, 2, 0x000000);
    assert(fb[UI_W * UI_H - 1] == 0x800000);
  }

  /* card package entry: card=1 and dirs= are parsed and cleaned up */
  snprintf(path, sizeof path, "%s/autorun/boot-menu/40-card.entry", dir);
  write_file(path, "title=Card\nrun=WaveGrid/start.sh\ncard=1\ndirs=WaveGrid\n");
  snprintf(path, sizeof path, "%s/WaveGrid", dir);
  mkdir_p(path);
  snprintf(path, sizeof path, "%s/WaveGrid/start.sh", dir);
  write_file(path, "#!/bin/sh\necho hi\n");
  {
    struct menu cm;
    assert(menu_load(&cm, dir) == 4);
    const struct menu_entry *ce = NULL;
    for (int i = 0; i < cm.n; i++) {
      if (strcmp(cm.items[i].id, "entry:40-card.entry") == 0) {
        ce = &cm.items[i];
        break;
      }
    }
    assert(ce != NULL);
    assert(ce->card_pkg == 1);
    assert(strcmp(ce->run, "WaveGrid/start.sh") == 0);
    assert(strcmp(ce->dirs, "WaveGrid") == 0);
    snprintf(path, sizeof path, "%s/autorun/boot-menu/40-card.entry", dir);
    assert(menu_card_dirs_remove(dir, path) == 0);
    {
      char wg[512];
      snprintf(wg, sizeof wg, "%s/WaveGrid", dir);
      assert(access(wg, F_OK) != 0);
    }
    snprintf(path, sizeof path, "%s/autorun/boot-menu/40-card.entry", dir);
    unlink(path);
  }

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
  /* The menu itself can be the default. */
  assert(menu_set_default(dir, MENU_BOOT_THINGY_ID) == 0);
  assert(menu_default_index(&m, dir) == MENU_DEFAULT_BOOT_THINGY);
  assert(menu_set_default(dir, "stock") == 0);
  assert(menu_default_index(&m, dir) == 0);

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

  /* Apps view preference. The list is the default. */
  assert(menu_view_load(dir) == MENU_VIEW_LIST);
  assert(menu_view_load(NULL) == MENU_VIEW_LIST);
  assert(menu_view_save(dir, MENU_VIEW_GRID) == 0);
  assert(menu_view_load(dir) == MENU_VIEW_GRID);
  assert(menu_view_save(dir, 7) != 0);
  assert(menu_view_save(NULL, MENU_VIEW_LIST) != 0);
  snprintf(path, sizeof path, "%s/autorun/system/boot-view", dir);
  write_file(path, "nope\n");
  assert(menu_view_load(dir) == MENU_VIEW_LIST);
  assert(menu_view_save(dir, MENU_VIEW_LIST) == 0);
  assert(menu_view_load(dir) == MENU_VIEW_LIST);

  static uint32_t px[UI_W * UI_H];
  struct ui view = {.px = px};
  struct boot_ui boot;
  boot_ui_init(&boot, &m, 0, 0);
  assert(boot.apps_ls.cursor == 0);
  assert(boot.view == MENU_VIEW_LIST); /* the list is the default view */
  struct input in;
  input_begin(&in, 0);
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);

  /* List view: row 0 reads "Stock (default)" on the current-row fill at
     list y 76 with 20px rows, and no tile picture is drawn. */
  {
    static uint32_t ref_px[UI_W * UI_H];
    struct ui ref = {.px = ref_px};
    ui_clear(&ref, theme_at(0)->bg);
    struct ui_paint bar = {.fill_set = 1, .fill = theme_at(0)->bar};
    ui_box(&ref, 8, 76, 304, 20, bar);
    ui_text(&ref, 16, 80, "Stock (default)", theme_at(0)->text, 1);
    for (int y = 80; y < 92; y++)
      for (int x = 16; x < 16 + 15 * 6; x++) /* 6px glyphs */
        assert(px[y * UI_W + x] == ref_px[y * UI_W + x]);
    int cell_w = 304 / 4;
    int lab_x = 8 + 1 * cell_w + (cell_w - BOOT_ICON_PX) / 2;
    assert(px[(72 + 4 + 10) * UI_W + lab_x + 10] != 0x123456);
  }

  /* The rest of the apps-screen checks use the grid. */
  boot_ui_set_view(&boot, MENU_VIEW_GRID);
  assert(boot.view == MENU_VIEW_GRID);
  input_begin(&in, 0);
  boot_frame(&view, &in, &boot);

  /* Main screen header shows "mpc-boot-thingy  |  v" APP_VERSION at (12, 40) */
  {
    static uint32_t ref_px[UI_W * UI_H];
    struct ui ref = {.px = ref_px};
    ui_clear(&ref, theme_at(0)->bg);
    ui_text(&ref, 12, 40, "mpc-boot-thingy  |  v" APP_VERSION,
            theme_at(0)->text, 1);
    for (int y = 40; y < 52; y++) {
      for (int x = 0; x < UI_W; x++) {
        assert(px[y * UI_W + x] == ref_px[y * UI_W + x]);
      }
    }
  }

  /* The apps screen is a grid of tiles, four to a row, from y 72. Lab (row 1)
     draws its icon.png; Stock and Snake draw generated tiles. */
  {
    int cell_w = 304 / 4;
    int lab_x = 8 + 1 * cell_w + (cell_w - BOOT_ICON_PX) / 2;
    int tile_y = 72 + 4;
    assert(px[(tile_y + 10) * UI_W + lab_x + 10] == 0x123456);
    assert(px[(tile_y + BOOT_ICON_PX - 1) * UI_W + lab_x + BOOT_ICON_PX - 1] ==
           0x123456);
    assert(boot.icons[1].ok);
    assert(strcmp(boot.icons[1].path, m.items[1].icon) == 0);
    /* Snake has a PNG beside its entry file too. */
    int snake_x = 8 + 2 * cell_w + (cell_w - BOOT_ICON_PX) / 2;
    assert(px[(tile_y + 10) * UI_W + snake_x + 10] == 0x123456);
    /* Stock's tile is drawn from theme colors, not the background. */
    int stock_x = 8 + (cell_w - BOOT_ICON_PX) / 2;
    assert(px[(tile_y + 1) * UI_W + stock_x + 1] == theme_at(0)->box);
    assert(!boot.icons[0].ok);
    /* The first row is selected and framed with the highlight color. */
    assert(px[72 * UI_W + 8] == theme_at(0)->hl);
    assert(px[(72 + 2) * UI_W + 8 + 2] == theme_at(0)->bar);
  }

  /* Scrolling up at the top row clamps instead of wrapping. */
  input_begin(&in, 0);
  in.encoder = -1;
  boot_frame(&view, &in, &boot);
  assert(boot.apps_ls.cursor == 0);

  /* Scrolling down reaches Lab (row 1) then Snake (row 2), and clamps at the
   * bottom. */
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.apps_ls.cursor == 1);
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.apps_ls.cursor == 2);
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.apps_ls.cursor == 2);

  /* B1 opens the dedicated USB screen. */
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_USB);
  assert(boot.usb_ls.cursor == 0);

  /* The USB screen lists Install App (row 0) and USB storage (row 1). */
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.usb_ls.cursor == 1);
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.usb_ls.cursor == 1);

  /* Selecting USB storage opens the storage screen. Start waits for the next
   * frame. */
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.action == BOOT_ACT_NONE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_PREPARING);

  /* Return to apps screen. The card is not bound yet. */
  boot.screen = BOOT_SCREEN_APPS;
  boot.action = BOOT_ACT_NONE;
  boot.apps_ls.cursor = 0;

  in.down = 1u << IN_B3;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_SETTINGS);

  /* Test shift + top buttons (Set Default, Edit Entry) on an app row. */
  boot_ui_init(&boot, &m, 0, 0);
  assert(boot.apps_ls.cursor == 0);
  /* On Stock (cursor == 0), Shift + B2 (Edit Entry) shows notice and stays on
   * apps screen */
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B2;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);
  assert(strcmp(boot.notice, "Stock cannot be edited") == 0);

  /* Scroll to app:20-lab (cursor == 1) */
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.apps_ls.cursor == 1);
  assert(boot.action == BOOT_ACT_NONE);

  /* Holding shift does NOT draw a popup over the center of the screen */
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  boot_frame(&view, &in, &boot);
  assert(px[50 * UI_W + 262] == theme_at(0)->bg);

  /* Shift + B1 sets current item as default */
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_SET_DEFAULT);
  assert(strcmp(boot.action_id, "app:20-lab") == 0);

  /* Shift + B2 opens Edit Entry (BOOT_SCREEN_ITEM_EDIT) for the selected boot
   * item */
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B2;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_ITEM_EDIT);
  assert(strcmp(boot.edit_id, "app:20-lab") == 0);
  assert(strcmp(boot.edit_title, "Lab firmware") == 0);

  /* Pressing B1 (Back) from Shift->Edit Entry returns directly to
   * BOOT_SCREEN_APPS */
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);

  /* Test that default_index = 1 initializes cursor to 1 (pointing to
   * app:20-lab) */
  boot_ui_init(&boot, &m, 0, 1);
  assert(boot.apps_ls.cursor == 1);
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

  /* The row after the last app is the menu itself. Choosing it saves
     MENU_BOOT_THINGY_ID and no app carries the default mark. */
  boot.action = BOOT_ACT_NONE;
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_DEFAULT);
  assert(boot.default_ls.cursor == 2); /* opens on the current default */
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.default_ls.cursor == m.n);
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.default_ls.cursor == m.n); /* clamps at the menu row */
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_SET_DEFAULT);
  assert(boot.action_index == MENU_DEFAULT_BOOT_THINGY);
  assert(boot.default_index == MENU_DEFAULT_BOOT_THINGY);
  assert(strcmp(boot.action_id, MENU_BOOT_THINGY_ID) == 0);
  assert(boot.screen == BOOT_SCREEN_SETTINGS);
  /* Reopening the list lands on the menu row. */
  boot.action = BOOT_ACT_NONE;
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_DEFAULT);
  assert(boot.default_ls.cursor == m.n);
  /* Back to an app default so the rest of the test is unchanged. */
  input_begin(&in, 0);
  in.encoder = -1;
  boot_frame(&view, &in, &boot);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(strcmp(boot.action_id, "entry:30-snake.entry") == 0);
  assert(boot.default_index == 2);
  assert(boot.screen == BOOT_SCREEN_SETTINGS);
  /* A fresh UI with the menu as default puts the app cursor on Stock. */
  {
    struct boot_ui fresh;
    boot_ui_init(&fresh, &m, 0, MENU_DEFAULT_BOOT_THINGY);
    assert(fresh.apps_ls.cursor == 0);
    assert(fresh.default_ls.cursor == m.n);
  }

  /* Test USB storage from the dedicated USB screen. */
  ui_usb_sd_set_mock(1, 0);
  boot.action = BOOT_ACT_NONE;
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_USB);
  assert(boot.usb_ls.cursor == 0);
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.usb_ls.cursor == 1);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.action == BOOT_ACT_NONE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_PREPARING);

  /* Next frame runs the mock start. */
  input_begin(&in, 0);
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.action == BOOT_ACT_NONE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_ACTIVE);
  assert(ui_usb_sd_is_active(&boot.usb));

  /* Encoder rotation and B2/B3 stay on the storage screen. */
  input_begin(&in, 0);
  in.encoder = 5;
  in.down = (1u << IN_B2) | (1u << IN_B3);
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.action == BOOT_ACT_NONE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_ACTIVE);

  /* B1 draws the eject screen. The following frame finishes and asks main to
   * reload. */
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_NONE);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_EXITING);
  input_begin(&in, 0);
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_USB_SD_DONE);
  assert(!ui_usb_sd_is_active(&boot.usb));
  /* Main reloads the card and returns to the apps list. Stay here to re-enter.
   */
  boot.action = BOOT_ACT_NONE;
  boot.screen = BOOT_SCREEN_USB;
  boot.usb_ls.cursor = 1;

  /* Exit via encoder press. */
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.action == BOOT_ACT_NONE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_PREPARING);
  input_begin(&in, 0);
  boot_frame(&view, &in, &boot);
  assert(boot.usb.phase == UI_USB_SD_PHASE_ACTIVE);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_NONE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_EXITING);
  input_begin(&in, 0);
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_USB_SD_DONE);
  boot.action = BOOT_ACT_NONE;
  boot.screen = BOOT_SCREEN_USB;
  boot.usb_ls.cursor = 1;

  /* A failed start returns to the USB screen from the error screen. */
  ui_usb_sd_set_mock(1, 1);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_PREPARING);
  input_begin(&in, 0);
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_STORAGE);
  assert(boot.usb.phase == UI_USB_SD_PHASE_ERROR);
  assert(!ui_usb_sd_is_active(&boot.usb));
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_USB);
  ui_usb_sd_set_mock(1, 0);

  /* Test reload from settings menu (index 2). */
  boot.action = BOOT_ACT_NONE;
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);
  input_begin(&in, 0);
  in.down = 1u << IN_B3;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_SETTINGS);
  input_begin(&in, 0);
  in.encoder = 2;
  boot_frame(&view, &in, &boot);
  assert(boot.settings_ls.cursor == 2);
  /* Row 2 toggles the apps view and asks to save it. The UI was
     re-initialized above, so it is back on the list. */
  assert(boot.view == MENU_VIEW_LIST);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_SAVE_VIEW);
  assert(boot.action_index == MENU_VIEW_GRID);
  assert(boot.view == MENU_VIEW_GRID);
  assert(boot.screen == BOOT_SCREEN_SETTINGS);
  boot.action = BOOT_ACT_NONE;
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_SAVE_VIEW);
  assert(boot.action_index == MENU_VIEW_LIST);
  assert(boot.view == MENU_VIEW_LIST);
  boot.action = BOOT_ACT_NONE;
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.settings_ls.cursor == 3);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_RELOAD);

  /* Test Power button (soft key 2) on apps screen and Power page options */
  boot.action = BOOT_ACT_NONE;
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);
  input_begin(&in, 0);
  in.down = 1u << IN_B2;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_POWER);
  assert(boot.power_ls.cursor == 0);
  assert(boot.action == BOOT_ACT_NONE);

  /* Row 0: Reboot */
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_REBOOT);

  /* Row 1: Shut Down */
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.power_ls.cursor == 1);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.action == BOOT_ACT_SHUTDOWN);

  /* Row 2: Cancel returns to BOOT_SCREEN_APPS */
  input_begin(&in, 0);
  in.encoder = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.power_ls.cursor == 2);
  input_begin(&in, 0);
  in.encoder_press = 1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);
  assert(boot.action == BOOT_ACT_NONE);

  /* Re-open Power page and press B1 (Back) to return to BOOT_SCREEN_APPS */
  input_begin(&in, 0);
  in.down = 1u << IN_B2;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_POWER);
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);

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
    if (strcmp(again.items[i].id, saved_id) == 0)
      found = i;
  }
  assert(found > 0);
  assert(again.items[found].boot_ac50);
  assert(!again.items[0].boot_ac50);
  assert(strcmp(again.items[found].title, "Boot 1") == 0);
  assert(strcmp(again.items[found].run,
                "autorun/boot-edits/boot-1/launch.sh") == 0);
  assert(strcmp(again.items[found].patches,
                "wide-screen.list,extra-pads.list") == 0);

  snprintf(path, sizeof path,
           "%s/autorun/boot-edits/boot-1/patches/wide-screen.list", dir);
  char *body = read_all(path);
  assert(strcmp(body, "# title Wide screen\npatch\n") == 0);
  free(body);
  snprintf(path, sizeof path,
           "%s/autorun/boot-edits/boot-1/patches/boot-menu.list", dir);
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
  assert(strstr(body, "/run/mpc-boot-autorun"));
  free(body);
  {
    struct stat st;
    assert(stat(path, &st) == 0);
    assert(st.st_mode & 0111);
  }

  const char *only[] = {"wide-screen.list"};
  const char *bad[] = {"missing.list"};
  assert(boot_edit_save(dir, "entry:30-snake.entry", "Snake", only, 1, out_id,
                        sizeof out_id) == BOOT_EDIT_BAD_ID);
  assert(boot_edit_save(dir, saved_id, "", only, 1, out_id, sizeof out_id) ==
         BOOT_EDIT_BAD_TITLE);
  assert(boot_edit_save(dir, NULL, "Nope", bad, 1, out_id, sizeof out_id) ==
         BOOT_EDIT_BAD_PATCH);
  assert(boot_edit_save(dir, saved_id, "Lab mix", only, 1, out_id,
                        sizeof out_id) == 0);
  assert(strcmp(out_id, saved_id) == 0);
  assert(menu_load(&again, dir) == 4);
  found = -1;
  for (int i = 0; i < again.n; i++) {
    if (strcmp(again.items[i].id, saved_id) == 0)
      found = i;
  }
  assert(found > 0);
  assert(strcmp(again.items[found].title, "Lab mix") == 0);
  assert(strcmp(again.items[found].patches, "wide-screen.list") == 0);
  snprintf(path, sizeof path,
           "%s/autorun/boot-edits/boot-1/patches/extra-pads.list", dir);
  assert(access(path, F_OK) != 0);
  snprintf(path, sizeof path,
           "%s/autorun/boot-edits/boot-1/patches/wide-screen.list", dir);
  assert(access(path, F_OK) == 0);
  snprintf(path, sizeof path,
           "%s/autorun/boot-edits/boot-1/patches/boot-menu.list", dir);
  assert(access(path, F_OK) == 0);

  assert(boot_edit_save(dir, NULL, "Second", NULL, 0, out_id, sizeof out_id) ==
         0);
  assert(strcmp(out_id, "entry:boot-2.entry") == 0);
  assert(menu_load(&again, dir) == 5);

  boot_ui_init(&boot, &again, 0, 0);
  boot_ui_set_catalog(&boot, catalog, nc);

  /* Pressing B1 without shift opens the USB screen. */
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_USB);
  /* Back returns to the apps screen. */
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);

  /* Shift + B3 opens New Entry (BOOT_SCREEN_ENTRY) */
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B3;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_ENTRY);
  assert(strcmp(boot.edit_title, "") == 0);
  assert(!boot_ui_dirty(&boot));
  input_begin(&in, 0);
  in.down = 1u << IN_B1;
  boot_frame(&view, &in, &boot);
  assert(boot.screen == BOOT_SCREEN_APPS);
  assert(boot.action == BOOT_ACT_NONE);

  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B3;
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
  assert(boot.screen == BOOT_SCREEN_APPS);
  assert(boot.action == BOOT_ACT_NONE);
  assert(!boot_ui_dirty(&boot));

  /* Hover Lab mix (menu index `found` -> apps_ls.cursor = found) and Shift + B2
   * -> Edit Entry */
  boot.apps_ls.cursor = found;
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B2;
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
  assert(boot.screen == BOOT_SCREEN_APPS);

  /* Test editing an installed app via Shift + B2 on main menu (cursor 1: Lab
   * firmware) */
  boot_ui_init(&boot, &again, 0, 0);
  boot_ui_set_catalog(&boot, catalog, nc);
  boot.apps_ls.cursor = 1;
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B2;
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
  /* Press B1 Cancel in text editor -> returns to BOOT_SCREEN_ITEM_EDIT without
   * change */
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
  assert(boot.screen == BOOT_SCREEN_APPS);

  /* Test B2 Rename button in BOOT_SCREEN_ENTRY opened via Shift + B3 */
  input_begin(&in, 0);
  in.shift = 1;
  in.held = 1u << IN_SHIFT;
  in.down = 1u << IN_B3;
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
  assert(boot_entry_update(dir, "entry:30-snake.entry", "Snake DX",
                           "--lives 5") == 0);
  struct menu updated;
  assert(menu_load(&updated, dir) == 5);
  int snk = -1;
  for (int i = 0; i < updated.n; i++) {
    if (strcmp(updated.items[i].id, "entry:30-snake.entry") == 0)
      snk = i;
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

  /* A nested autorun package is a boot entry. boot.entry wins when it exists.
   */
  snprintf(path, sizeof path, "%s/autorun/apps/hand-mod/autorun", dir);
  mkdir_p(path);
  snprintf(path, sizeof path, "%s/autorun/apps/hand-mod/autorun/autorun.json",
           dir);
  write_file(path, "{\"value0\":{\"scripts\":[{\"path\":\"start.sh\"}]}}\n");
  snprintf(path, sizeof path, "%s/autorun/apps/hand-mod/autorun/start.sh", dir);
  write_file(path, "#!/bin/sh\n");
  snprintf(path, sizeof path, "%s/autorun/apps/hand-mod/autorun/payload.bin",
           dir);
  write_file(path, "data\n");
  snprintf(path, sizeof path, "%s/autorun/apps/named-mod/autorun", dir);
  mkdir_p(path);
  snprintf(path, sizeof path, "%s/autorun/apps/named-mod/autorun/autorun.sh",
           dir);
  write_file(path, "#!/bin/sh\n");
  snprintf(path, sizeof path, "%s/autorun/apps/named-mod/boot.entry", dir);
  write_file(path, "title=Named mod\nrun=autorun/autorun.sh\n");
  snprintf(path, sizeof path, "%s/autorun/apps/sh-only/autorun", dir);
  mkdir_p(path);
  snprintf(path, sizeof path, "%s/autorun/apps/sh-only/autorun/autorun.sh",
           dir);
  write_file(path, "#!/bin/sh\n");
  snprintf(path, sizeof path, "%s/autorun/apps/evil/autorun", dir);
  mkdir_p(path);
  snprintf(path, sizeof path, "%s/autorun/apps/evil/autorun/autorun.json", dir);
  write_file(path, "{\"value0\":{\"scripts\":[{\"path\":\"../out.sh\"}]}}\n");
  snprintf(path, sizeof path, "%s/autorun/apps/evil/autorun/autorun.sh", dir);
  write_file(path, "#!/bin/sh\n");
  assert(menu_load(&updated, dir) >= 4);
  {
    int hand = -1, named = -1, shonly = -1, evil = -1, i;
    for (i = 0; i < updated.n; i++) {
      if (strcmp(updated.items[i].id, "app:hand-mod") == 0)
        hand = i;
      if (strcmp(updated.items[i].id, "app:named-mod") == 0)
        named = i;
      if (strcmp(updated.items[i].id, "app:sh-only") == 0)
        shonly = i;
      if (strcmp(updated.items[i].id, "app:evil") == 0)
        evil = i;
    }
    assert(hand > 0);
    assert(strcmp(updated.items[hand].title, "hand-mod") == 0);
    assert(strcmp(updated.items[hand].run, "autorun/start.sh") == 0);
    snprintf(path, sizeof path, "%s/autorun/apps/hand-mod", dir);
    assert(strcmp(updated.items[hand].base, path) == 0);
    assert(named > 0);
    assert(strcmp(updated.items[named].title, "Named mod") == 0);
    assert(strcmp(updated.items[named].run, "autorun/autorun.sh") == 0);
    assert(shonly > 0);
    assert(strcmp(updated.items[shonly].title, "sh-only") == 0);
    assert(strcmp(updated.items[shonly].run, "autorun/autorun.sh") == 0);
    assert(evil < 0);
  }
  assert(boot_entry_update(dir, "app:loose-binary", "Nope", NULL) != 0);
  assert(boot_entry_update(dir, "app:hand-mod", "Hand Mod", NULL) == 0);
  snprintf(path, sizeof path, "%s/autorun/apps/hand-mod/boot.entry", dir);
  {
    char *body = read_all(path);
    assert(strcmp(body, "title=Hand Mod\nrun=autorun/start.sh\n") == 0);
    free(body);
  }
  write_file(path, "title=Hand Mod\nrun=autorun/start.sh\ntakeover=1\n");
  assert(boot_entry_update(dir, "app:hand-mod", "Hand Mod", NULL) == 0);
  {
    char *body = read_all(path);
    assert(strcmp(body, "title=Hand Mod\nrun=autorun/start.sh\ntakeover=1\n") == 0);
    free(body);
  }
  assert(menu_load(&updated, dir) >= 4);
  {
    int hand = -1, i;
    for (i = 0; i < updated.n; i++) {
      if (strcmp(updated.items[i].id, "app:hand-mod") == 0)
        hand = i;
    }
    assert(hand > 0);
    assert(strcmp(updated.items[hand].title, "Hand Mod") == 0);
    assert(updated.items[hand].takeover == 1);
  }

  /* Nested autorun: SampQOL replaces stock. A sidecar does not.
     Markers past 64KB, inside a .so, or behind a symlink do not count.
     The card-root autorun and a helper script are not entry scripts. */
  {
    FILE *big;
    int i;
    snprintf(path, sizeof path, "%s/autorun/apps/sampqol/autorun", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/sampqol/autorun/autorun.sh",
             dir);
    write_file(path, "#!/bin/sh\nsetsid sh /run/qol-run.sh \"$DIR\"\n");
    snprintf(path, sizeof path, "%s/autorun/apps/sampqol/autorun/qol-run.sh",
             dir);
    write_file(path, "#!/bin/sh\nsystemctl stop ac50.service\nexport "
                     "LD_PRELOAD=/tmp/mpcqol.so\n");
    snprintf(path, sizeof path, "%s/autorun/apps/sampqol/autorun/mpcqol.so",
             dir);
    write_file(path, "LD_PRELOAD ac50 not a script\n");
    snprintf(path, sizeof path, "%s/autorun/apps/sampqol/autorun/autorun.sh",
             dir);
    assert(menu_autorun_kind(path) == 1);
    snprintf(path, sizeof path, "%s/autorun/apps/sampqol/autorun/qol-run.sh",
             dir);
    assert(menu_autorun_kind(path) == -1);

    snprintf(path, sizeof path, "%s/autorun/apps/sshkit/autorun", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/sshkit/autorun/autorun.json",
             dir);
    write_file(path,
               "{\"value0\":{\"scripts\":[{\"path\":\"./autorun.sh\"}]}}\n");
    snprintf(path, sizeof path, "%s/autorun/apps/sshkit/autorun/autorun.sh",
             dir);
    write_file(
        path,
        "#!/bin/sh\nsystemd-run --unit=mpc-sample-usb-ssh /bin/sh worker.sh\n");
    assert(menu_autorun_kind(path) == 0);
    snprintf(path, sizeof path, "%s/autorun/apps/sshkit/autorun/linked.sh",
             dir);
    assert(symlink("/tmp/mpc-boot-marker.sh", path) == 0);
    write_file("/tmp/mpc-boot-marker.sh", "#!/bin/sh\nLD_PRELOAD=/tmp/x.so\n");
    snprintf(path, sizeof path, "%s/autorun/apps/sshkit/autorun/autorun.sh",
             dir);
    assert(menu_autorun_kind(path) == 0);
    unlink("/tmp/mpc-boot-marker.sh");

    snprintf(path, sizeof path, "%s/autorun/apps/named/autorun", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/named/autorun/autorun.json",
             dir);
    write_file(path, "{\"value0\":{\"scripts\":[{\"path\":\"start.sh\"}]}}\n");
    snprintf(path, sizeof path, "%s/autorun/apps/named/autorun/start.sh", dir);
    write_file(path, "#!/bin/sh\nexport LD_PRELOAD=/tmp/mod.so\n");
    assert(menu_autorun_kind(path) == 1);

    snprintf(path, sizeof path, "%s/autorun/apps/late/autorun", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/late/autorun/autorun.sh", dir);
    big = fopen(path, "wb");
    assert(big);
    for (i = 0; i < 65536; i++)
      assert(fputc('a', big) == 'a');
    fputs("LD_PRELOAD\n", big);
    fclose(big);
    assert(menu_autorun_kind(path) == 0);

    snprintf(path, sizeof path, "%s/autorun/autorun.sh", dir);
    write_file(path, "#!/bin/sh\nexport LD_PRELOAD=/tmp/nope.so\n");
    assert(menu_autorun_kind(path) == -1);
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/boot-edits/boot-1/launch.sh", dir);
    write_file(path, "#!/bin/sh\nsystemctl stop ac50.service\n");
    assert(menu_autorun_kind(path) == -1);
    snprintf(path, sizeof path, "%s/autorun/apps/20-lab/mpcfw", dir);
    assert(menu_autorun_kind(path) == -1);
    snprintf(path, sizeof path, "%s/autorun/apps/sampqol/autorun/missing.sh",
             dir);
    assert(menu_autorun_kind(path) == -1);
    snprintf(path, sizeof path, "%s/autorun/apps/am/autorun", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/apps/am/autorun/autorun.sh", dir);
    write_file(path, "#!/bin/sh\n/bin/sh mod-launcher.sh\n");
    snprintf(path, sizeof path, "%s/autorun/apps/am/autorun/mod-launcher.sh",
             dir);
    write_file(path, "#!/bin/sh\nsystemctl stop ac50.service\n");
    snprintf(path, sizeof path, "%s/autorun/apps/am/autorun/autorun.sh", dir);
    assert(menu_autorun_kind(path) == 2);
  }

  /* Device unzip: a deflated archive, a root package, and a path that escapes.
   */
  {
    char script[2048], cmd[640], run[256], zipout[512];
    snprintf(path, sizeof path, "%s/mkzip.py", dir);
    snprintf(
        script, sizeof script,
        "import zipfile, sys\n"
        "kind = sys.argv[1]\n"
        "dest = sys.argv[2]\n"
        "if kind == 'hub':\n"
        "    z = zipfile.ZipFile(dest, 'w', compression=zipfile.ZIP_DEFLATED)\n"
        "    z.writestr('proj-1.0/README.md', 'skip')\n"
        "    z.writestr('proj-1.0/DATA/nope.txt', 'nope')\n"
        "    z.writestr('proj-1.0/autorun/autorun.json', "
        "'{\"value0\":{\"scripts\":[{\"path\":\"autorun.sh\"}]}}')\n"
        "    z.writestr('proj-1.0/autorun/autorun.sh', '#!/bin/sh\\necho "
        "hi\\n')\n"
        "    z.writestr('proj-1.0/autorun/note.txt', 'keep')\n"
        "    z.close()\n"
        "elif kind == 'root':\n"
        "    z = zipfile.ZipFile(dest, 'w', compression=zipfile.ZIP_STORED)\n"
        "    z.writestr('autorun.sh', '#!/bin/sh\\n')\n"
        "    z.writestr('extra.txt', 'keep')\n"
        "    z.close()\n"
        "elif kind == 'slip':\n"
        "    z = zipfile.ZipFile(dest, 'w', compression=zipfile.ZIP_STORED)\n"
        "    z.writestr('autorun/autorun.sh', '#!/bin/sh\\n')\n"
        "    z.writestr('autorun/../escaped.txt', 'nope')\n"
        "    z.close()\n"
        "elif kind == 'bt':\n"
        "    z = zipfile.ZipFile(dest, 'w', compression=zipfile.ZIP_DEFLATED)\n"
        "    z.writestr('LICENSE', 'mit\\n')\n"
        "    z.writestr('BeatTrain/Songs/song1.txt', 'song\\n')\n"
        "    z.writestr('autorun/autorun.json', "
        "'{\"value0\":{\"scripts\":[{\"path\":\"autorun.sh\"}]}}')\n"
        "    z.writestr('autorun/autorun.sh', '#!/bin/sh\\necho bt\\n')\n"
        "    z.close()\n");
    write_file(path, script);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkzip.py' hub '%s/hub.zip'", dir,
             dir);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkzip.py' root '%s/root.zip'", dir,
             dir);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkzip.py' slip '%s/slip.zip'", dir,
             dir);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkzip.py' bt '%s/bt.zip'", dir,
             dir);
    assert(system(cmd) == 0);

    snprintf(zipout, sizeof zipout, "%s/zip-hub", dir);
    assert(mkdir(zipout, 0755) == 0);
    snprintf(path, sizeof path, "%s/hub.zip", dir);
    assert(unzip_autorun(path, zipout, run, sizeof run) == 0);
    assert(strcmp(run, "autorun/autorun.sh") == 0);
    snprintf(path, sizeof path, "%s/autorun/note.txt", zipout);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/autorun/autorun.sh", zipout);
    assert(access(path, X_OK) == 0);
    snprintf(path, sizeof path, "%s/README.md", zipout);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/autorun/README.md", zipout);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/DATA/nope.txt", zipout);
    assert(access(path, F_OK) != 0);

    snprintf(zipout, sizeof zipout, "%s/zip-root", dir);
    assert(mkdir(zipout, 0755) == 0);
    snprintf(path, sizeof path, "%s/root.zip", dir);
    assert(unzip_autorun(path, zipout, run, sizeof run) == 0);
    assert(strcmp(run, "autorun/autorun.sh") == 0);
    snprintf(path, sizeof path, "%s/autorun/extra.txt", zipout);
    {
      char *body = read_all(path);
      assert(strcmp(body, "keep") == 0);
      free(body);
    }

    snprintf(zipout, sizeof zipout, "%s/zip-slip", dir);
    assert(mkdir(zipout, 0755) == 0);
    snprintf(path, sizeof path, "%s/slip.zip", dir);
    assert(unzip_autorun(path, zipout, run, sizeof run) != 0);
    snprintf(path, sizeof path, "%s/escaped.txt", dir);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/escaped.txt", zipout);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/autorun/escaped.txt", zipout);
    assert(access(path, F_OK) != 0);

    /* Sibling directory extraction (Beat Train style) */
    {
      char dirs_out[256];
      char card_test[512], app_entry[512], app_dir[512];
      snprintf(card_test, sizeof card_test, "%s/card-bt", dir);
      assert(mkdir(card_test, 0755) == 0);
      /* unzip_sibling_dirs ignores non-BeatTrain sibling directories (like DATA) */
      snprintf(path, sizeof path, "%s/hub.zip", dir);
      assert(unzip_sibling_dirs(path, card_test, dirs_out, sizeof dirs_out) == 0);
      assert(dirs_out[0] == '\0');
      snprintf(path, sizeof path, "%s/DATA", card_test);
      assert(access(path, F_OK) != 0);

      /* unzip_sibling_dirs extracts BeatTrain sibling directory */
      snprintf(path, sizeof path, "%s/bt.zip", dir);
      assert(unzip_sibling_dirs(path, card_test, dirs_out, sizeof dirs_out) == 0);
      assert(strcmp(dirs_out, "BeatTrain") == 0);
      snprintf(path, sizeof path, "%s/BeatTrain/Songs/song1.txt", card_test);
      assert(access(path, F_OK) == 0);
      snprintf(path, sizeof path, "%s/LICENSE", card_test);
      assert(access(path, F_OK) != 0);
      snprintf(path, sizeof path, "%s/autorun", card_test);
      assert(access(path, F_OK) != 0);

      /* Deleting an app with dirs= removes the sibling directories */
      snprintf(app_dir, sizeof app_dir, "%s/autorun/apps/bt", card_test);
      mkdir_p(app_dir);
      snprintf(app_entry, sizeof app_entry, "%s/boot.entry", app_dir);
      write_file(app_entry, "title=BT\nrun=autorun/autorun.sh\ndirs=BeatTrain\n");
      assert(boot_entry_delete(card_test, "app:bt") == 0);
      assert(access(app_dir, F_OK) != 0);
      snprintf(path, sizeof path, "%s/BeatTrain", card_test);
      assert(access(path, F_OK) != 0);

      /* Beat Train zip test if present in ignore/ */
      if (access("ignore/beattrain-mpc-sample.zip", F_OK) == 0) {
        assert(unzip_sibling_dirs("ignore/beattrain-mpc-sample.zip", card_test, dirs_out, sizeof dirs_out) == 0);
        assert(strcmp(dirs_out, "BeatTrain") == 0);
        snprintf(path, sizeof path, "%s/BeatTrain/Songs", card_test);
        struct stat st;
        assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
        /* Cleanup */
        snprintf(app_dir, sizeof app_dir, "%s/autorun/apps/beattrain", card_test);
        mkdir_p(app_dir);
        snprintf(app_entry, sizeof app_entry, "%s/boot.entry", app_dir);
        write_file(app_entry, "title=Beat-Train\nrun=autorun/autorun.sh\ndirs=BeatTrain\n");
        assert(boot_entry_delete(card_test, "app:beattrain") == 0);
        snprintf(path, sizeof path, "%s/BeatTrain", card_test);
        assert(access(path, F_OK) != 0);
      }
    }
  }

  assert(png_load_screen("splash-screen.png", px) == 0);
  uint64_t sum = 0;
  for (int i = 0; i < UI_W * UI_H; i++)
    sum = sum * 131 + px[i];
  assert(sum == 4867112348204652761ull);
  assert(px[0] == 0);
  assert(px[120 * UI_W + 160] == 0x00bcb8aa);
  assert(png_load_screen("no-such-splash-screen.png", px) != 0);
  static const unsigned char tiny_png[] = {
      137, 80,  78,  71,  13,  10, 26, 10, 0,  0,   0,  13,  73,  72,
      68,  82,  0,   0,   0,   1,  0,  0,  0,  1,   8,  2,   0,   0,
      0,   144, 119, 83,  222, 0,  0,  0,  12, 73,  68, 65,  84,  120,
      156, 99,  248, 207, 192, 0,  0,  3,  1,  1,   0,  201, 254, 146,
      239, 0,   0,   0,   0,   73, 69, 78, 68, 174, 66, 96,  130};
  char tiny_path[64];
  snprintf(tiny_path, sizeof tiny_path, "/tmp/mpc-png-tiny-%d.png",
           (int)getpid());
  FILE *tf = fopen(tiny_path, "wb");
  assert(tf);
  assert(fwrite(tiny_png, 1, sizeof tiny_png, tf) == sizeof tiny_png);
  fclose(tf);
  assert(png_load_screen(tiny_path, px) != 0);
  unlink(tiny_path);
  snprintf(tiny_path, sizeof tiny_path, "/tmp/mpc-png-text-%d.png",
           (int)getpid());
  write_file(tiny_path, "not a png");
  assert(png_load_screen(tiny_path, px) != 0);
  unlink(tiny_path);

  /* SD sync and check sanity tests */
  sd_sync_and_drop_caches(NULL);
  sd_sync_and_drop_caches(dir);
  char dev_dummy[64], fs_dummy[64];
  assert(sd_find_device(NULL, dev_dummy, sizeof dev_dummy, fs_dummy,
                        sizeof fs_dummy) != 0);
  assert(sd_check_repair(NULL) != 0);
  assert(sd_check_repair("/nonexistent/path/for/sd") != 0);

  /* SD format UI & backend sanity tests */
  struct boot_ui ui_fmt;
  boot_ui_init(&ui_fmt, &m, 0, 0);
  assert(strcmp(ui_fmt.format_label, "MPC-Sample") == 0);

  /* Navigate Settings -> Format SD Card */
  ui_fmt.screen = BOOT_SCREEN_SETTINGS;
  ui_fmt.settings_ls.cursor = 4;
  struct input in_press = {.encoder_press = 1};
  struct ui ui_test = {.px = px};
  boot_frame(&ui_test, &in_press, &ui_fmt);
  assert(ui_fmt.screen == BOOT_SCREEN_FORMAT_CONFIRM);

  /* Cancel on Format Confirm returns to Settings */
  ui_fmt.format_confirm_ls.cursor = 0;
  boot_frame(&ui_test, &in_press, &ui_fmt);
  assert(ui_fmt.screen == BOOT_SCREEN_SETTINGS);

  /* Confirm on Format Confirm transitions to text editor */
  ui_fmt.screen = BOOT_SCREEN_FORMAT_CONFIRM;
  ui_fmt.format_confirm_ls.cursor = 1;
  boot_frame(&ui_test, &in_press, &ui_fmt);
  assert(ui_fmt.screen == BOOT_SCREEN_TEXT_EDIT);
  assert(ui_fmt.text_edit_mode == BOOT_TEXT_FORMAT_LABEL);

  /* Mock format_sd_card test */
  format_sd_set_mock(1, 0);
  char last_status[128] = {0};
  assert(format_sd_card(dir, "MPC-Sample", dummy_progress, last_status) == 0);
  assert(strcmp(last_status, "Format complete! Rebooting...") == 0);

  format_sd_set_mock(1, 1);
  assert(format_sd_card(dir, "MPC-Sample", dummy_progress, last_status) != 0);
  format_sd_set_mock(0, 0);

  format_sd_set_mock(1, 0);
  assert(format_sd_blank(dir, "MPC-Sample", dummy_progress, last_status) == 0);
  assert(strcmp(last_status, "Format complete.") == 0);
  format_sd_set_mock(1, 1);
  assert(format_sd_blank(dir, NULL, dummy_progress, last_status) != 0);
  format_sd_set_mock(0, 0);

  /* Web File Browser: virtual Storage root over Internal and the SD card. */
  {
    char test_data[512];
    char test_sd[512];
    snprintf(test_data, sizeof test_data, "%s/data_test/UserContent", dir);
    snprintf(test_sd, sizeof test_sd, "%s/data_test/sdcard", dir);
    assert(file_ops_init(test_data) == 0);
    assert(file_ops_set_sd_root(test_sd) == 0);

    /* 1. Sandboxing and path resolution */
    char resolved[1024], rel[1024];
    assert(file_ops_resolve("", resolved, sizeof resolved, rel, sizeof rel) !=
           0);
    assert(file_ops_resolve("/", resolved, sizeof resolved, rel, sizeof rel) !=
           0);
    assert(file_ops_resolve("internal", resolved, sizeof resolved, rel,
                            sizeof rel) == 0);
    assert(strcmp(rel, "internal") == 0);
    assert(file_ops_resolve("internal/drums/kick.wav", resolved,
                            sizeof resolved, rel, sizeof rel) == 0);
    assert(strcmp(rel, "internal/drums/kick.wav") == 0);
    assert(file_ops_resolve("sd", resolved, sizeof resolved, rel, sizeof rel) ==
           0);
    assert(strcmp(rel, "sd") == 0);

    /* Traversal outside either volume must fail */
    assert(file_ops_resolve("../", resolved, sizeof resolved, rel,
                            sizeof rel) != 0);
    assert(file_ops_resolve("../etc/passwd", resolved, sizeof resolved, rel,
                            sizeof rel) != 0);
    assert(file_ops_resolve("internal/../../etc/passwd", resolved,
                            sizeof resolved, rel, sizeof rel) != 0);
    assert(file_ops_resolve("internal/drums/../../etc/shadow", resolved,
                            sizeof resolved, rel, sizeof rel) != 0);
    assert(file_ops_resolve("sd/../etc/passwd", resolved, sizeof resolved, rel,
                            sizeof rel) != 0);
    assert(file_ops_resolve("drums/./../..", resolved, sizeof resolved, rel,
                            sizeof rel) != 0);

    char linkpath[1024];
    snprintf(linkpath, sizeof linkpath, "%s/escape", test_data);
    assert(symlink("/etc", linkpath) == 0);
    assert(file_ops_resolve("internal/escape", resolved, sizeof resolved, rel,
                            sizeof rel) != 0);
    assert(file_ops_resolve("internal/escape/passwd", resolved, sizeof resolved,
                            rel, sizeof rel) != 0);

    char *root_json = file_ops_list_json("");
    assert(root_json != NULL);
    assert(strstr(root_json, "\"id\":\"internal\"") != NULL);
    assert(strstr(root_json, "\"name\":\"Internal\"") != NULL);
    assert(strstr(root_json, "\"id\":\"sd\"") != NULL);
    assert(strstr(root_json, "\"name\":\"SD Card\"") != NULL);
    assert(strstr(root_json, "\"total_bytes\":0") != NULL);
    free(root_json);

    /* 2. Volume roots cannot be created; files live under a volume */
    assert(file_ops_mkdir("") != 0);
    assert(file_ops_mkdir("internal") != 0);
    assert(file_ops_mkdir("sd") != 0);
    assert(file_ops_mkdir("samples") != 0);
    assert(file_ops_mkdir("internal/samples") == 0);
    assert(file_ops_mkdir("internal/samples/drums") == 0);

    /* 3. Create sample files */
    char fpath1[1024], fpath2[1024];
    snprintf(fpath1, sizeof fpath1, "%s/samples/drums/snare.wav", test_data);
    write_file(fpath1, "WAV_SNARE_DATA_12345");
    snprintf(fpath2, sizeof fpath2, "%s/samples/note.txt", test_data);
    write_file(fpath2, "Sample note");

    /* 4. List directory JSON */
    char *json = file_ops_list_json("internal/samples");
    assert(json != NULL);
    assert(strstr(json, "\"name\":\"drums\"") != NULL);
    assert(strstr(json, "\"is_dir\":true") != NULL);
    assert(strstr(json, "\"name\":\"note.txt\"") != NULL);
    assert(strstr(json, "\"is_dir\":false") != NULL);
    assert(strstr(json, "\"total_bytes\":0") == NULL);
    free(json);

    /* 5. Rename */
    assert(file_ops_rename("internal", "nope") != 0);
    assert(file_ops_rename("sd", "nope") != 0);
    assert(file_ops_rename("internal/samples/note.txt", "readme.txt") == 0);
    snprintf(fpath2, sizeof fpath2, "%s/samples/readme.txt", test_data);
    assert(access(fpath2, F_OK) == 0);

    /* 5b. Space calculation and checks */
    unsigned long long calc_sz = 0;
    size_t calc_cnt = 0;
    const char *calc_items[] = {"internal/samples"};
    assert(file_ops_calc_size(calc_items, 1, &calc_sz, &calc_cnt) == 0);
    assert(calc_sz > 0 && calc_cnt == 2);

    unsigned long long req_bytes = 0, free_bytes = 0;
    int fits = 0;
    assert(file_ops_check_space(calc_items, 1, "internal", &req_bytes,
                                &free_bytes, &fits) == 0);
    assert(fits == 1);
    assert(req_bytes == calc_sz);
    assert(free_bytes > 0);

    /* 6. Recursive copy, including onto the SD card root and SD to internal */
    const char *copy_items[] = {"internal/samples/drums"};
    assert(file_ops_copy(copy_items, 1, "") != 0);
    assert(file_ops_copy(copy_items, 1, "internal") == 0);
    char fcopy[1024];
    snprintf(fcopy, sizeof fcopy, "%s/drums/snare.wav", test_data);
    assert(access(fcopy, F_OK) == 0);
    assert(access(fpath1, F_OK) == 0); /* Source still exists: 2 copies */

    assert(file_ops_copy(copy_items, 1, "sd") == 0);
    char sdcopy[1024];
    snprintf(sdcopy, sizeof sdcopy, "%s/drums/snare.wav", test_sd);
    assert(access(sdcopy, F_OK) == 0);

    /* Copy from SD to Internal */
    const char *sd_items[] = {"sd/drums"};
    assert(file_ops_mkdir("internal/imported") == 0);
    assert(file_ops_copy(sd_items, 1, "internal/imported") == 0);
    char imported_copy[1024];
    snprintf(imported_copy, sizeof imported_copy, "%s/imported/drums/snare.wav",
             test_data);
    assert(access(imported_copy, F_OK) == 0);
    assert(access(sdcopy, F_OK) == 0); /* Both copies exist */

    /* Copy with progress and cancel */
    struct test_prog_ctx pctx = {0, 0};
    assert(file_ops_copy_progress(copy_items, 1, "internal/imported", test_prog,
                                  &pctx) == 0);
    assert(pctx.calls > 0);

    struct test_prog_ctx pctx_cancel = {0, 1};
    assert(file_ops_copy_progress(copy_items, 1, "internal/imported", test_prog,
                                  &pctx_cancel) != 0);

    /* 7. Move */
    const char *move_items[] = {"internal/drums"};
    assert(file_ops_mkdir("internal/backup") == 0);
    assert(file_ops_move(move_items, 1, "") != 0);
    assert(file_ops_move(move_items, 1, "internal/backup") == 0);
    char fmove[1024];
    snprintf(fmove, sizeof fmove, "%s/backup/drums/snare.wav", test_data);
    assert(access(fmove, F_OK) == 0);
    assert(access(fcopy, F_OK) != 0);

    /* 8. ZIP export of Internal only. Archive paths stay relative to
     * UserContent. */
    char zip_file[1024];
    snprintf(zip_file, sizeof zip_file, "%s/export.zip", dir);
    int zfd = open(zip_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(zfd >= 0);
    assert(zip_stream_export(zfd, NULL, 0) == 0);
    close(zfd);

    char pycmd[2048];
    snprintf(pycmd, sizeof pycmd,
             "python3 -c '\n"
             "import zipfile, sys\n"
             "zf = zipfile.ZipFile(\"%s/export.zip\", \"r\")\n"
             "names = zf.namelist()\n"
             "assert any(\"samples/readme.txt\" in n for n in names), "
             "f\"missing readme: {names}\"\n"
             "assert any(\"backup/drums/snare.wav\" in n for n in names), "
             "f\"missing snare: {names}\"\n"
             "assert not any(n.startswith(\"sd/\") or \"/sd/\" in n for n in "
             "names), f\"sd leaked: {names}\"\n"
             "data = zf.read(\"samples/readme.txt\").decode(\"utf-8\")\n"
             "assert data == \"Sample note\", f\"bad content: {data}\"\n"
             "assert zf.testzip() is None, \"zip CRC test failed\"\n"
             "zf.close()\n"
             "'",
             dir);
    assert(system(pycmd) == 0);

    /* 9. Delete protects the virtual root and both volumes */
    const char *root_item[] = {""};
    assert(file_ops_delete(root_item, 1) != 0);
    const char *internal_item[] = {"internal"};
    assert(file_ops_delete(internal_item, 1) != 0);
    const char *sd_item[] = {"sd"};
    assert(file_ops_delete(sd_item, 1) != 0);

    const char *del_items[] = {"internal/backup", "internal/samples"};
    assert(file_ops_delete(del_items, 2) == 0);
    assert(access(fmove, F_OK) != 0);

    /* Card removed: the SD folder is still listed, and opening it fails. */
    assert(file_ops_set_sd_root(NULL) == 0);
    assert(file_ops_list_json("sd") == NULL);
    const char *missing = file_ops_access_error("sd");
    assert(missing != NULL);
    assert(strcmp(missing, "SD card not mounted") == 0);
    char *still = file_ops_list_json("");
    assert(still != NULL);
    assert(strstr(still, "\"id\":\"sd\"") != NULL);
    free(still);
  }

  {
    char req[512], ready[512], proc[512];
    snprintf(req, sizeof req, "%s/testapp.request", dir);
    snprintf(ready, sizeof ready, "%s/testapp.ready", dir);
    snprintf(proc, sizeof proc, "%s/proc", dir);
    assert(setenv("MPC_BOOT_TESTAPP_REQUEST", req, 1) == 0);
    assert(setenv("MPC_BOOT_TESTAPP_READY", ready, 1) == 0);
    assert(!testapp_request_pending());
    write_file(req, "1\n");
    assert(testapp_request_pending());
    assert(access(ready, F_OK) != 0);
    assert(testapp_mark_ready() == 0);
    assert(access(ready, F_OK) == 0);
    assert(testapp_request_pending());
    assert(!testapp_comm_matches(NULL));
    assert(!testapp_comm_matches(""));
    assert(!testapp_comm_matches("mpc-boot-thingy"));
    assert(!testapp_comm_matches("sh"));
    assert(testapp_comm_matches("test-app-launcher"));
    assert(testapp_comm_matches("test-app-launche"));
    assert(testapp_comm_matches("usb_installer"));
    assert(testapp_comm_matches("usb_installer_t"));
    assert(testapp_comm_matches("usb_installer_testapp"));
    mkdir_p(proc);
    snprintf(path, sizeof path, "%s/1", proc);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/1/comm", proc);
    write_file(path, "mpc-boot-thingy\n");
    snprintf(path, sizeof path, "%s/2", proc);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/2/comm", proc);
    write_file(path, "test-app-launche\n");
    assert(testapp_proc_has_launcher(proc));
    write_file(path, "usb_installer_t\n");
    assert(testapp_proc_has_launcher(proc));
    assert(unlink(path) == 0);
    assert(!testapp_proc_has_launcher(proc));
    assert(!testapp_proc_has_launcher(NULL));
  }

  /* Dropped .zip auto-install into autorun/apps */
  {
    char card_zip[512], apps_dir[512], zpath[512], script[2048], cmd[640];
    char slug[80];
    assert(app_install_slug("My Cool App", slug, sizeof slug) == 0);
    assert(strcmp(slug, "my-cool-app") == 0);
    assert(app_install_slug("am-mod-r27r", slug, sizeof slug) == 0);
    assert(strcmp(slug, "am-mod-r27r") == 0);
    assert(app_install_slug("---", slug, sizeof slug) != 0);

    snprintf(card_zip, sizeof card_zip, "%s/card-zip-drop", dir);
    snprintf(path, sizeof path, "%s/autorun", card_zip);
    mkdir_p(path);

    struct app_install_summary sum;
    assert(app_install_pending_zips(card_zip, NULL, NULL, &sum) == 0);
    assert(sum.installed == 0 && sum.failed == 0);
    snprintf(apps_dir, sizeof apps_dir, "%s/autorun/apps", card_zip);
    struct stat st;
    assert(stat(apps_dir, &st) == 0 && S_ISDIR(st.st_mode));

    snprintf(path, sizeof path, "%s/mkdrop.py", dir);
    snprintf(
        script, sizeof script,
        "import zipfile, sys\n"
        "kind, dest = sys.argv[1], sys.argv[2]\n"
        "z = zipfile.ZipFile(dest, 'w', compression=zipfile.ZIP_DEFLATED)\n"
        "if kind == 'takeover':\n"
        "    z.writestr('autorun/autorun.json', "
        "'{\"value0\":{\"scripts\":[{\"path\":\"autorun.sh\"}]}}')\n"
        "    z.writestr('autorun/autorun.sh', '#!/bin/sh\\n/bin/sh mod-launcher.sh\\n')\n"
        "    z.writestr('autorun/mod-launcher.sh', '#!/bin/sh\\necho mod\\n')\n"
        "elif kind == 'card':\n"
        "    z.writestr('Gridzilla - V1/README.txt', 'ignore\\n')\n"
        "    z.writestr('Gridzilla - V1/WaveGrid/start.sh', '#!/bin/sh\\necho wg\\n')\n"
        "    z.writestr('Gridzilla - V1/autorun/autorun.json', "
        "'{\"value0\":{\"scripts\":[{\"path\":\"autorun.sh\"}]}}')\n"
        "    z.writestr('Gridzilla - V1/autorun/autorun.sh', "
        "'#!/bin/sh\\nsh ../WaveGrid/start.sh\\n')\n"
        "elif kind == 'bt':\n"
        "    z.writestr('BeatTrain/Songs/song1.txt', 'song\\n')\n"
        "    z.writestr('autorun/autorun.json', "
        "'{\"value0\":{\"scripts\":[{\"path\":\"autorun.sh\"}]}}')\n"
        "    z.writestr('autorun/autorun.sh', '#!/bin/sh\\necho bt\\n')\n"
        "z.close()\n");
    write_file(path, script);

    snprintf(zpath, sizeof zpath, "%s/My Cool App.zip", apps_dir);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkdrop.py' takeover '%s'", dir, zpath);
    assert(system(cmd) == 0);

    snprintf(zpath, sizeof zpath, "%s/Gridzilla - V1.zip", apps_dir);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkdrop.py' card '%s'", dir, zpath);
    assert(system(cmd) == 0);

    snprintf(zpath, sizeof zpath, "%s/bt.zip", apps_dir);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkdrop.py' bt '%s'", dir, zpath);
    assert(system(cmd) == 0);

    snprintf(zpath, sizeof zpath, "%s/._My Cool App.zip", apps_dir);
    write_file(zpath, "appledouble");
    snprintf(zpath, sizeof zpath, "%s/broken.zip", apps_dir);
    write_file(zpath, "not a zip file");
    snprintf(zpath, sizeof zpath, "%s/already.zip.failed", apps_dir);
    write_file(zpath, "old failure");

    assert(app_install_pending_zips(card_zip, NULL, NULL, &sum) == 0);
    assert(sum.installed == 3);
    assert(sum.failed == 1);
    assert(strcmp(sum.last_failed_name, "broken.zip") == 0);

    snprintf(path, sizeof path, "%s/My Cool App.zip", apps_dir);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/Gridzilla - V1.zip", apps_dir);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/bt.zip", apps_dir);
    assert(access(path, F_OK) != 0);

    snprintf(path, sizeof path, "%s/._My Cool App.zip", apps_dir);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/already.zip.failed", apps_dir);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/broken.zip", apps_dir);
    assert(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/broken.zip.failed", apps_dir);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/broken.zip.error.txt", apps_dir);
    assert(access(path, F_OK) == 0);

    snprintf(path, sizeof path, "%s/my-cool-app/boot.entry", apps_dir);
    {
      char *body = read_all(path);
      assert(strstr(body, "title=My Cool App\n") != NULL);
      assert(strstr(body, "takeover=1\n") != NULL);
      free(body);
    }
    snprintf(path, sizeof path, "%s/BeatTrain/Songs/song1.txt", card_zip);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/WaveGrid/start.sh", card_zip);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/autorun/boot-menu/gridzilla-v1.entry",
             card_zip);
    {
      char *body = read_all(path);
      assert(strstr(body, "title=Gridzilla - V1\n") != NULL);
      assert(strstr(body, "run=WaveGrid/start.sh\n") != NULL);
      assert(strstr(body, "card=1\n") != NULL);
      assert(strstr(body, "dirs=WaveGrid\n") != NULL);
      free(body);
    }

    struct menu zm;
    assert(menu_load(&zm, card_zip) == 4);

    /* Dropping the same zip again updates in place cleanly */
    snprintf(zpath, sizeof zpath, "%s/My Cool App.zip", apps_dir);
    snprintf(cmd, sizeof cmd, "python3 '%s/mkdrop.py' takeover '%s'", dir, zpath);
    assert(system(cmd) == 0);
    assert(app_install_pending_zips(card_zip, NULL, NULL, &sum) == 0);
    assert(sum.installed == 1 && sum.failed == 0);
    assert(access(zpath, F_OK) != 0);
    assert(menu_load(&zm, card_zip) == 4);

    /* USB App Drop UI flow & zip collection tests */
    {
      static uint32_t uax_px[UI_W * UI_H];
      struct ui uax_view = {.px = uax_px};
      struct boot_ui uax;
      struct input uin;
      boot_ui_init(&uax, &zm, 0, 0);
      boot_ui_set_card(&uax, card_zip);
      ui_usb_apps_set_mock(1, 0);

      /* B1 opens the dedicated USB screen; Install App is row 0. */
      input_begin(&uin, 0);
      uin.down = 1u << IN_B1;
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.screen == BOOT_SCREEN_USB);
      assert(uax.usb_ls.cursor == 0);

      input_begin(&uin, 0);
      uin.encoder_press = 1;
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.screen == BOOT_SCREEN_USB_APPS);
      assert(uax.usb_apps.phase == UI_USB_APPS_PHASE_PREPARING);
      assert(strcmp(uax.usb_apps.card, card_zip) == 0);

      input_begin(&uin, 0);
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.usb_apps.phase == UI_USB_APPS_PHASE_ACTIVE);
      assert(ui_usb_apps_is_active(&uax.usb_apps));

      input_begin(&uin, 0);
      uin.down = 1u << IN_B1;
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.usb_apps.phase == UI_USB_APPS_PHASE_EXITING);

      input_begin(&uin, 0);
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.action == BOOT_ACT_USB_APPS_DONE);
      assert(!ui_usb_apps_is_active(&uax.usb_apps));

      /* Failed start returns to the USB screen. */
      ui_usb_apps_set_mock(1, 1);
      uax.action = BOOT_ACT_NONE;
      uax.screen = BOOT_SCREEN_USB;
      uax.usb_ls.cursor = 0;
      input_begin(&uin, 0);
      uin.encoder_press = 1;
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.screen == BOOT_SCREEN_USB_APPS);
      input_begin(&uin, 0);
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.usb_apps.phase == UI_USB_APPS_PHASE_ERROR);
      assert(!ui_usb_apps_is_active(&uax.usb_apps));
      input_begin(&uin, 0);
      uin.encoder_press = 1;
      boot_frame(&uax_view, &uin, &uax);
      assert(uax.screen == BOOT_SCREEN_USB);

      /* Emergency stop clears active binding */
      ui_usb_apps_set_mock(1, 0);
      uax.usb_ls.cursor = 0;
      input_begin(&uin, 0);
      uin.encoder_press = 1;
      boot_frame(&uax_view, &uin, &uax);
      input_begin(&uin, 0);
      boot_frame(&uax_view, &uin, &uax);
      assert(ui_usb_apps_is_active(&uax.usb_apps));
      assert(ui_usb_apps_stop(&uax.usb_apps) == 0);
      assert(!ui_usb_apps_is_active(&uax.usb_apps));

      /* Collect .zip packages from mock USB drop directory */
      char drop_dir[512];
      snprintf(drop_dir, sizeof drop_dir, "%s/drop_mnt", dir);
      mkdir(drop_dir, 0755);
      snprintf(path, sizeof path, "%s/sub.dir", drop_dir);
      mkdir(path, 0755);
      snprintf(path, sizeof path, "%s/Drop .zip apps here.txt", drop_dir);
      write_file(path, "readme\n");
      snprintf(path, sizeof path, "%s/._USB Dropped.zip", drop_dir);
      write_file(path, "appledouble\n");
      snprintf(path, sizeof path, "%s/USB Dropped.zip", drop_dir);
      snprintf(cmd, sizeof cmd, "python3 '%s/mkdrop.py' bt '%s'", dir, path);
      assert(system(cmd) == 0);

      ui_usb_apps_set_mock_dir(drop_dir);
      int copied = 0;
      char uerr[128] = {0};
      assert(usb_apps_collect_zips(card_zip, &copied, uerr, sizeof uerr) == 0);
      assert(copied == 1);
      ui_usb_apps_set_mock_dir(NULL);

      snprintf(path, sizeof path, "%s/._USB Dropped.zip", apps_dir);
      assert(access(path, F_OK) != 0);
      snprintf(path, sizeof path, "%s/USB Dropped.zip", apps_dir);
      assert(access(path, F_OK) == 0);

      assert(app_install_pending_zips(card_zip, NULL, NULL, &sum) == 0);
      assert(sum.installed == 1 && sum.failed == 0);
      assert(menu_load(&zm, card_zip) == 5);
    }
  }

  /* Screen capture & PNG encoding round-trip */
  {
    uint32_t *src_px =
        malloc((size_t)MPC_SCREEN_W * MPC_SCREEN_H * sizeof(uint32_t));
    uint32_t *dec_px =
        malloc((size_t)MPC_SCREEN_W * MPC_SCREEN_H * sizeof(uint32_t));
    unsigned char *png_buf = NULL;
    size_t png_len = 0;
    char source[32] = {0};
    assert(src_px && dec_px);
    for (int i = 0; i < MPC_SCREEN_W * MPC_SCREEN_H; i++) {
      src_px[i] = (uint32_t)(((i * 73) & 0xff) << 16) |
                  (uint32_t)(((i * 19) & 0xff) << 8) |
                  (uint32_t)((i * 251) & 0xff);
    }
    assert(screen_encode_png(src_px, MPC_SCREEN_W, MPC_SCREEN_H, &png_buf,
                             &png_len) == 0);
    assert(png_buf && png_len > 64);

    snprintf(path, sizeof path, "%s/screen-roundtrip.png", dir);
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite(png_buf, 1, png_len, fp) == png_len);
    fclose(fp);
    free(png_buf);
    png_buf = NULL;

    assert(png_load_screen(path, dec_px) == 0);
    assert(memcmp(src_px, dec_px,
                  (size_t)MPC_SCREEN_W * MPC_SCREEN_H * sizeof(uint32_t)) == 0);

    assert(screen_capture_png(&png_buf, &png_len, source, sizeof source) == 0);
    assert(png_buf && png_len > 64 && source[0] != 0);
    free(png_buf);
    free(src_px);
    free(dec_px);
  }

  puts("selftest ok");
  return 0;
}
