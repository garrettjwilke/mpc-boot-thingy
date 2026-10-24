#pragma once
#include <stdint.h>

/* One line in card/autorun/boot-theme. A missing or unknown file is Amber. */
struct boot_theme {
    const char *id;
    const char *name;
    uint32_t bg, text, dim, hl, bar, box, menu_fill, border, active_text;
};

int theme_count(void);
const struct boot_theme *theme_at(int index);
int theme_load(const char *card);
int theme_save(const char *card, int index);
