#pragma once
#include <stddef.h>

/* One boot option. Stock is built in and is always items[0]. */
#define MENU_MAX 32
#define MENU_TITLE 64
#define MENU_RUN 256
#define MENU_ARGS 256
#define MENU_PATH 512
#define MENU_ID 288
#define MENU_PATCHES 768

/* The web page sends this to /api/boot-now to open the menu. It is not a
   menu row. Saved as the default, power-on opens the menu instead of an app. */
#define MENU_BOOT_THINGY_ID "boot-thingy"

/* menu_default_index result when the saved default is MENU_BOOT_THINGY_ID. */
#define MENU_DEFAULT_BOOT_THINGY (-1)

struct menu_entry {
    char title[MENU_TITLE];
    char run[MENU_RUN];
    char args[MENU_ARGS];
    char base[MENU_PATH]; /* directory a relative run path is joined to */
    char id[MENU_ID];     /* "stock", "app:<folder>", or "entry:<file>" */
    char patches[MENU_PATCHES]; /* comma-separated catalog ids, when boot_ac50 */
    char dirs[MENU_PATCHES]; /* card package: space-separated card-root dirs to remove */
    char icon[MENU_PATH]; /* PNG drawn as this row's tile, or "" for a generated tile */
    int stock;
    int boot_ac50; /* run is a Boot Edit script; launch hands off like Stock */
    int card_pkg;  /* run is a card-root script (e.g. WaveGrid/start.sh); launch hands off like Stock */
    int takeover;  /* firmware/takeover package (e.g. AM Mod): requires stock running and card autorun */
};

struct menu {
    struct menu_entry items[MENU_MAX];
    int n;
};

/* Stock is always first. Then, sorted by name:
   every .entry file in card/autorun/boot-menu (run is relative to the card root) and
   card/autorun/apps/<name>/boot.entry (run is relative to that folder).
   An app folder with no boot.entry is still listed when it holds
   autorun/autorun.json or autorun/autorun.sh. Its title is the folder name
   and its run is autorun/<script>. A file without both title and run is skipped.
   A missing card yields Stock only.
   Each row gets an id: "stock", "app:<folder>", or "entry:<file>".
   icon is the first regular file of: the icon= key (joined to base unless it
   starts with /), <folder>/icon.png, <folder>/autorun/icon.png, then for an
   app the shallowest launch picture up to three directories below <folder>:
   splash-screen.png, else a .png/.ppm/.pgm named *splash*, else a *splash*.raw
   of one 320x240 screen. An entry row uses <name>.png beside <name>.entry,
   then a launch picture under each card-root directory in its dirs= key.
   Stock uses card/autorun/system/stock.png. */
int menu_load(struct menu *m, const char *card);

/* Parse a single .entry file into e. Returns 0 on success, -1 on failure. */
int menu_parse_entry(const char *path, struct menu_entry *e);

/* app_dir is autorun/apps/<name>. When it contains a nested autorun package,
   run receives the path relative to app_dir (autorun/<script>).
   The script is the first "path" in autorun.json, or autorun.sh when that
   file has no path. The script must be a regular file inside the package.
   Returns 0 when a script is found. */
int menu_nested_run(const char *app_dir, char *run, size_t n);

/* script_path is the program a menu row would run.
   1 when it is the entry script of a nested package
   (autorun/apps/<name>/autorun/<script>) and a .sh file in that package
   contains "ac50" or "LD_PRELOAD" in its first 64KB. That package replaces
   the stock app itself.
   0 when it is that entry script and no .sh file contains either marker.
   The firmware still has to start stock after the script.
   -1 when the path is not that entry script.
   The entry script is autorun.sh, or the first "path" in autorun.json.
   Symlinks are ignored. A missing entry script still returns 0 or 1. */
int menu_autorun_kind(const char *script_path);

/* Index of the id saved in card/autorun/system/boot-default.
   A missing file, a blank line, "stock", or an unknown id returns 0.
   MENU_BOOT_THINGY_ID returns MENU_DEFAULT_BOOT_THINGY (-1): the menu itself. */
int menu_default_index(const struct menu *m, const char *card);

/* Writes id and a newline to card/autorun/system/boot-default. 0 on success. */
int menu_set_default(const char *card, const char *id);

/* How the apps screen draws its rows: a list of names, or a grid of tiles. */
enum { MENU_VIEW_LIST = 0, MENU_VIEW_GRID = 1 };

/* card/autorun/system/boot-view holds "list" or "grid". A missing or unknown
   file is the list. */
int menu_view_load(const char *card);

/* Writes the view's name and a newline to card/autorun/system/boot-view.
   0 on success. */
int menu_view_save(const char *card, int view);

/* Split args on spaces into out[0..). Copies into buf, which must stay alive.
   Returns the number of words. */
int menu_split_args(const char *args, char *buf, size_t buf_n, char **out, int max);

/* Remove the card-root directories named by a card package's "dirs=" key in
   entry_path. Each name must be a plain single component. Returns 0 when there
   is nothing to remove, -1 on an invalid path. */
int menu_card_dirs_remove(const char *card, const char *entry_path);
