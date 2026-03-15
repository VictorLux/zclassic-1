/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_BITMAP_FONT_H
#define ZCL_BITMAP_FONT_H

#include <stdint.h>
#include <stddef.h>

/* 6x10 bitmap font for chart labels. Pure C23, no dependencies.
 * Each glyph is 6 pixels wide, 10 pixels tall. */

#define FONT_W 6
#define FONT_H 10

/* Draw a string onto an RGB pixel buffer.
 * img: row-major RGB buffer (3 bytes/pixel)
 * img_w, img_h: image dimensions
 * x, y: top-left position of first character
 * r, g, b: text color
 * scale: integer scaling factor (1=6x10, 2=12x20, etc.) */
void font_draw_string(uint8_t *img, int img_w, int img_h,
                      int x, int y, const char *text,
                      uint8_t r, uint8_t g, uint8_t b, int scale);

#endif
