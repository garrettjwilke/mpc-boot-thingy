#pragma once
#include <stdint.h>
#include "menu.h"

/* Draw the menu into an XRGB8888 buffer. sel is the highlighted row. */
void draw_menu(uint32_t *fb, int stride, int w, int h, const struct menu *m, int sel);

/* Full-screen prompt shown before the menu. */
void draw_prompt(uint32_t *fb, int stride, int w, int h);
