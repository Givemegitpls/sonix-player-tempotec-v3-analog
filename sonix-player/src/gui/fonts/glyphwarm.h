#ifndef GLYPHWARM_H
#define GLYPHWARM_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// Getting the letters of a text into FreeType's caches ahead of the label that
// will show it, a few at a time, on the interface thread (FreeType and LVGL are
// not thread-safe, so nowhere else).
//
// The first time a letter is wanted at a size, FreeType has to find it in the
// font file -- on the device a squashfs block to unpack -- and hint and
// rasterise it; a title in kanji is mostly letters nobody has asked for yet,
// and the cost lands inside the layout of whatever shows it. Asking for them
// here first, in slices short enough not to be felt, moves it out of that
// layout. Same thing player.c's glyph warm-up does for the player's titles.

// One letter at one size the way drawing would: the metrics (what layout
// measures with), then the bitmap (what the first paint needs).
void glyphwarm_letter(const lv_font_t *font, uint32_t letter);

// Letters of `text` from byte offset *pos on, until `budget_us` microseconds
// are used. True when the end of the text was reached.
bool glyphwarm_text(const lv_font_t *font, const char *text, uint32_t *pos, uint32_t budget_us);

// The letters the library itself uses, warmed at the sizes a list row draws its
// title and artist in (24 and 20), whatever the language: a background thread
// reads the album, artist and title names from the database, counts the letters
// that are not plain ASCII and keeps the most used few hundred; the interface
// thread then asks FreeType for them in 5 ms slices while nobody is touching
// the screen. An English library has next to nothing to warm; a Japanese one
// gets its kana and common kanji, a Russian one its Cyrillic, a Korean one its
// Hangul. Call after the index is open, and again after a scan. Off with
// [ui] warm_glyphs = 0.
void glyphwarm_library_refresh(void);

// Monotonic microseconds, for budgets.
uint64_t glyphwarm_now_us(void);

#endif /* GLYPHWARM_H */
