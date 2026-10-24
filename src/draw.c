#include "draw.h"
#include "font.h"
#include <string.h>

enum { BG = 0x101014, TEXT = 0xe8e8e8, DIM = 0x9aa0a6, SEL = 0xffb000, BAR = 0x2a2418 };

static void fill(uint32_t *fb, int stride, int w, int h, int x0, int y0, int rw, int rh, uint32_t c) {
    for (int y = y0 < 0 ? 0 : y0; y < y0 + rh && y < h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x0 + rw && x < w; x++) fb[y * stride + x] = c;
}

static void glyph(uint32_t *fb, int stride, int w, int h, int x, int y, const unsigned char *g, int gw, int gh, int scale, uint32_t c) {
    for (int r = 0; r < gh; r++)
        for (int col = 0; col < gw; col++) {
            if (!(g[r] & (0x80 >> col))) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx, py = y + r * scale + sy;
                    if (px >= 0 && py >= 0 && px < w && py < h) fb[py * stride + px] = c;
                }
        }
}

static void text(uint32_t *fb, int stride, int w, int h, int x, int y, const char *s, uint32_t c, int x_max) {
    for (; *s && x + FONT_S_W <= x_max && x + FONT_S_W <= w; s++, x += FONT_S_W) {
        int ch = (unsigned char)*s;
        glyph(fb, stride, w, h, x, y, FONT_S[(ch < 32 || ch > 126 ? '?' : ch) - 32], FONT_S_W, FONT_S_H, 1, c);
    }
}

static void text_big(uint32_t *fb, int stride, int w, int h, int x, int y, const char *s, uint32_t c) {
    int scale = 2, gw = FONT_L_W * scale;
    for (; *s && x + gw <= w; s++, x += gw) {
        int ch = (unsigned char)*s;
        glyph(fb, stride, w, h, x, y, FONT_L[(ch < 32 || ch > 126 ? '?' : ch) - 32], FONT_L_W, FONT_L_H, scale, c);
    }
}

void draw_menu(uint32_t *fb, int stride, int w, int h, const struct menu *m, int sel) {
    if (!fb || !m || m->n < 1) return;
    if (sel < 0) sel = 0;
    if (sel >= m->n) sel = m->n - 1;
    fill(fb, stride, w, h, 0, 0, w, h, BG);
    text(fb, stride, w, h, 8, 8, "mpc-boot-thingy", SEL, w - 8);
    text(fb, stride, w, h, 8, 24, "encoder moves, press confirms", DIM, w - 8);

    int row_h = FONT_S_H + 4;
    int top = 48;
    int visible = (h - top) / row_h;
    if (visible < 1) visible = 1;
    int first = 0;
    if (sel >= visible) first = sel - visible + 1;
    for (int i = first; i < m->n && i < first + visible; i++) {
        int y = top + (i - first) * row_h;
        if (i == sel) fill(fb, stride, w, h, 4, y - 2, w - 8, row_h, BAR);
        text(fb, stride, w, h, 12, y, m->items[i].title, i == sel ? SEL : TEXT, w - 8);
    }
}

void draw_prompt(uint32_t *fb, int stride, int w, int h) {
    if (!fb) return;
    fill(fb, stride, w, h, 0, 0, w, h, BG);
    const char *a = "Press SHIFT";
    const char *b = "for boot thingy";
    int scale = 2, lh = FONT_L_H * scale;
    int aw = (int)strlen(a) * FONT_L_W * scale;
    int bw = (int)strlen(b) * FONT_L_W * scale;
    int y = (h - (lh * 2 + 12)) / 2;
    if (y < 8) y = 8;
    text_big(fb, stride, w, h, (w - aw) / 2, y, a, SEL);
    text_big(fb, stride, w, h, (w - bw) / 2, y + lh + 12, b, TEXT);
}
