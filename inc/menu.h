#pragma once
#include <stddef.h>

/* One boot option. Stock is built in and is always items[0]. */
#define MENU_MAX 32
#define MENU_TITLE 64
#define MENU_RUN 256
#define MENU_ARGS 256
#define MENU_PATH 512

struct menu_entry {
    char title[MENU_TITLE];
    char run[MENU_RUN];
    char args[MENU_ARGS];
    char base[MENU_PATH]; /* directory a relative run path is joined to */
    int stock;
};

struct menu {
    struct menu_entry items[MENU_MAX];
    int n;
};

/* Stock is always first. Then, sorted by name:
   every .entry file in card/autorun/boot-menu (run is relative to the card root) and
   card/apps/<name>/boot.entry (run is relative to that folder).
   A file without both title and run is skipped. A missing card yields Stock only. */
int menu_load(struct menu *m, const char *card);

/* Split args on spaces into out[0..). Copies into buf, which must stay alive.
   Returns the number of words. */
int menu_split_args(const char *args, char *buf, size_t buf_n, char **out, int max);
