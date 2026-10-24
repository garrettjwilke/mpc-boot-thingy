#pragma once
#include <stdint.h>

/* DRM dumb-buffer screen. Same open path as mpc-sample-lab/firmware/hw.c. */
extern uint32_t *hw_fb;
extern int hw_stride;
void hw_screen_open(void);
void hw_screen_flush(int x0, int y0, int x1, int y1);
void hw_screen_close(void);
