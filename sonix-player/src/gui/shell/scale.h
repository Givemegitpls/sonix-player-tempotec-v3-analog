#ifndef SCALE_H
#define SCALE_H

#include <stdint.h>

#include "lvgl/lvgl.h"

// One interface, one panel per build.
//
// Every page in src/gui was written against a 480x720 panel -- the R3 Pro II's
// and the Blaze's -- and its sizes and positions are the numbers a designer
// would put on that screen: a 56-pixel button, a 15-pixel margin, a 44-pixel
// status bar. The TempoTec V3 Analog has a 320x480 glass, which is exactly two
// thirds of 480x720, so those same numbers have to come out two thirds smaller
// on it.
//
// The old answer was to lay the whole interface out at 480x720 in a RAM page
// and shrink it onto the glass in software: correct, but a resample of every
// changed rectangle on every frame -- on the device a quarter of the interface
// thread's time -- and the text was drawn at 480x720 and thrown away two thirds
// of the way in, which softens it.
//
// The firmware is not universal (see NOTES.md, "финальная прошивка собирается
// под конкретное железо"): BOARD in the Makefile is one piece of hardware, and
// SONIX_LAYOUT_SCALE is the one number that follows from it -- 3 where the
// interface was authored, 2 on the V3 Analog's 320x480. Because it is a
// constant of the build rather than a value read from the model at run time,
// SC() is folded by the compiler: a page's arithmetic becomes the same literal
// it would have been had someone written the smaller numbers by hand, and the
// layouts of the boards this binary is not for are not in it at all.
//
// Use it on absolute sizes, positions, offsets, paddings, radii and font
// sizes -- the numbers that describe the panel. Do NOT use it on:
//
//   * a percentage or LV_SIZE_CONTENT; those are not pixels, and LVGL's
//     coordinate type bits would be corrupted by the arithmetic;
//   * a value already read back from LVGL (lv_obj_get_width() and friends):
//     that is in the panel's own pixels already, and scaling it again would
//     compound;
//   * anything that is not a distance -- a colour, an opacity (0..255), an
//     index, a millisecond count, a scale factor, a bit shift, the number of
//     modules in a QR code, or a coordinate a page already computes from its
//     own constants (gearboyplay.c and its GAME_DW/GAME_DH);
//   * a radius that means "a circle" (LV_RADIUS_CIRCLE, or size/2): halving it
//     turns the round Play button into a rounded square.
//
// A value derived from gui_config_t needs no SC(): screen_width, screen_height,
// padding and top_bar_height are set in main.c at the panel's own size already
// (see the gui_cfg block), so cfg->screen_width - 2 * SC(15) mixes the two
// correctly -- the config in panel pixels, the literal turned into them.

// 3 where the interface was authored, 2 on the V3 Analog's 320x480. Set by
// BOARD in the Makefile; the default keeps an unqualified build on the layout
// every page was written for.
#ifndef SONIX_LAYOUT_SCALE
#define SONIX_LAYOUT_SCALE 3
#endif

// Kept as a function because it is called from a handful of places that scale a
// picture of their own by hand (fonts.c, powermenu.c); the compiler inlines it
// and folds the answer away.
static inline int ui_layout_scale(void) { return SONIX_LAYOUT_SCALE; }

// v, on a 480x720 panel; two thirds of v on the V3 Analog's 320x480.
#define SC(v) ((v) * SONIX_LAYOUT_SCALE / 3)

// The same coefficient as an LVGL image scale factor: 256 (LV_SCALE_NONE, no
// resampling at all) where the interface was authored, 170 on the V3 Analog.
// Only for pictures a page scales itself -- the game frame, the knob icon in
// the power menu. Icons are not scaled at all any more: the set the panel needs
// is chosen when the firmware is built (icons_select.h).
static inline uint32_t ui_image_scale(void) {
	return (uint32_t)(LV_SCALE_NONE * SONIX_LAYOUT_SCALE / 3);
}

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------
//
// The icons in src/gui/shell/icons.c and icons_small.c are bitmaps rasterised
// from the SVGs in assets/icons, at the size they are drawn on a 480x720 panel
// (36 px for a chevron, 34 for a corner button, 26 for the status bar) and at
// two thirds of it respectively. LVGL does not fit a bitmap to its object: it
// draws it at the size it was rasterised at, so a page that asks for a chevron
// gets whichever drawing its own panel needs, with no rescaling and no lookup.
//
// Both sets declare the same identifiers and both are generated and checked in
// (tools/svg_to_lvgl.py, run in the sonix-icons container --
// sonix-player/docker/icons.Dockerfile, the recipe docs/UPDATING.md uses).
// Exactly one of them is compiled: BOARD in the Makefile
// leaves the other out of APP_SRCS, and every page includes icons_select.h
// rather than one of the two headers, so which set is in play is decided in one
// place. Compiling both would be a link error on duplicate symbols, which is
// the intended failure.

#endif // SCALE_H
