#ifndef GUI_FLAPPYBIRD_H
#define GUI_FLAPPYBIRD_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Flappy Bird, from the More page.
//
// The artwork is read from usr/resource/sonix/gui/flappybird when the game is
// opened and freed when it is left: nothing of it stays in memory in between.
// The best score is kept in the config, [flappybird] best, and whether the
// game plays its own sounds -- stopping the music -- in [flappybird] sound.

extern lv_obj_t *flappybird_screen;

void flappybird_init(gui_config_t *cfg);

// Loads the artwork and opens the game on its title screen. When a file is
// missing or unreadable it says so in a pop-up and stays where it is.
void flappybird_open(void);

#endif /* GUI_FLAPPYBIRD_H */
