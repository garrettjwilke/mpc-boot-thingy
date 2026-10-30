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

/* The web page sends this to /api/boot-now to open the menu.
   It is not a menu row and cannot be saved as the default. */
#define MENU_BOOT_THINGY_ID "boot-thingy"

struct menu_entry {
    char title[MENU_TITLE];
    char run[MENU_RUN];
    char args[MENU_ARGS];
    char base[MENU_PATH]; /* directory a relative run path is joined to */
    char id[MENU_ID];     /* "stock", "app:<folder>", or "entry:<file>" */
    char patches[MENU_PATCHES]; /* comma-separated catalog ids, when boot_ac50 */
    int stock;
    int boot_ac50; /* run is a Boot Edit script; launch hands off like Stock */
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
   Each row gets an id: "stock", "app:<folder>", or "entry:<file>". */
int menu_load(struct menu *m, const char *card);

/* Parse a single .entry file into e. Returns 0 on success, -1 on failure. */
int menu_parse_entry(const char *path, struct menu_entry *e);

/* app_dir is autorun/apps/<name>. When it contains a nested autorun package,
   run receives the path relative to app_dir (autorun/<script>).
   The script is the first "path" in autorun.json, or autorun.sh when that
   file has no path. The script must be a regular file inside the package.
   Returns 0 when a script is found. */
int menu_nested_run(const char *app_dir, char *run, size_t n);

/* Index of the id saved in card/autorun/system/boot-default.
   A missing file, a blank line, "stock", or an unknown id returns 0. */
int menu_default_index(const struct menu *m, const char *card);

/* Writes id and a newline to card/autorun/system/boot-default. 0 on success. */
int menu_set_default(const char *card, const char *id);

/* Split args on spaces into out[0..). Copies into buf, which must stay alive.
   Returns the number of words. */
int menu_split_args(const char *args, char *buf, size_t buf_n, char **out, int max);
