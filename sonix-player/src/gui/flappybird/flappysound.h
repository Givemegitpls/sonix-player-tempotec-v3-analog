#ifndef GUI_FLAPPYSOUND_H
#define GUI_FLAPPYSOUND_H

#include <stdbool.h>

// Flappy Bird's sound effects, from usr/resource/sonix/gui/flappybird/sounds.
//
// A thread mixes whatever is sounding into the external PCM (audio.h), writing
// silence in between, from flappysound_start() to flappysound_stop(). Music and
// radio are stopped first: the PCM takes a single writer.

typedef enum {
	SFX_WING,
	SFX_POINT,
	SFX_HIT,
	SFX_DIE,
	SFX_SWOOSH,
	SFX_COUNT,
} sfx_t;

// Starts a thread that loads the sounds and then runs the mixer, and returns at
// once. False only when the thread cannot be made. Until a sound has loaded, or
// when none can be or there is no PCM, the game runs silently and
// flappysound_play() does nothing.
bool flappysound_start(void);

// Starts `sfx` from its beginning, over whatever else is sounding. Any thread.
void flappysound_play(sfx_t sfx);

// Stops the mixer, closes the PCM and frees the sounds.
void flappysound_stop(void);

#endif /* GUI_FLAPPYSOUND_H */
