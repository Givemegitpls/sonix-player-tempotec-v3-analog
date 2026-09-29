#include "flappysound.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/audio/audio.h"
#include "src/system/core/respath.h"
#include "src/system/decode/stb_vorbis_decl.h"
#include "src/system/streaming/radio.h"

#define SOUND_DIR SONIX_RESOURCE_DIR "/gui/flappybird/sounds"

#define RATE 44100
#define MIX_FRAMES 441 // 10 ms
#define BUFFER_MS 46

// Samples at or below this level at the end of a file are cut off.
#define TAIL_SILENCE 64

static const char *const FILES[SFX_COUNT] = {
	[SFX_WING] = "sfx_wing.ogg",	 [SFX_POINT] = "sfx_point.ogg",			[SFX_HIT] = "sfx_hit.ogg",
	[SFX_DIE] = "sfx_die.ogg",		 [SFX_SWOOSH] = "sfx_swooshing.ogg",
};

typedef struct {
	int16_t *pcm; // interleaved, `channels` of them (1 or 2)
	int channels;
	int frames;
	int pos; // next frame to play; `frames` when silent
} sound_t;

static sound_t sounds[SFX_COUNT];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer;
static bool mixer_live;
static volatile bool stop_flag;

static bool sound_load(sound_t *s, const char *name) {
	char path[256];
	snprintf(path, sizeof(path), SOUND_DIR "/%s", name);

	int channels = 0, rate = 0;
	short *pcm = NULL;
	int frames = stb_vorbis_decode_filename(path, &channels, &rate, &pcm);
	if (frames <= 0 || !pcm || (channels != 1 && channels != 2) || rate != RATE) {
		printf("flappysound: %s cannot be used (%d frames, %d channels, %d Hz)\n", path, frames, channels, rate);
		free(pcm);
		return false;
	}

	while (frames > 0) {
		const short *last = pcm + (size_t)(frames - 1) * channels;
		bool quiet = true;
		for (int c = 0; c < channels; c++) {
			quiet = quiet && abs(last[c]) <= TAIL_SILENCE;
		}
		if (!quiet) {
			break;
		}
		frames--;
	}

	s->pcm = pcm;
	s->channels = channels;
	s->frames = frames;
	s->pos = frames;
	return true;
}

static void sounds_free(void) {
	for (int i = 0; i < SFX_COUNT; i++) {
		free(sounds[i].pcm);
		memset(&sounds[i], 0, sizeof(sounds[i]));
	}
}

// Adds what is sounding into `mix` (stereo) and moves each sound on.
static void mix_into(int32_t *mix) {
	memset(mix, 0, sizeof(int32_t) * MIX_FRAMES * 2);
	pthread_mutex_lock(&lock);
	for (int i = 0; i < SFX_COUNT; i++) {
		sound_t *s = &sounds[i];
		int n = s->frames - s->pos;
		if (n <= 0) {
			continue;
		}
		if (n > MIX_FRAMES) {
			n = MIX_FRAMES;
		}
		const int16_t *src = s->pcm + (size_t)s->pos * s->channels;
		for (int f = 0; f < n; f++) {
			int32_t l = src[f * s->channels];
			int32_t r = s->channels == 2 ? src[f * 2 + 1] : l;
			mix[f * 2] += l;
			mix[f * 2 + 1] += r;
		}
		s->pos += n;
	}
	pthread_mutex_unlock(&lock);
}

// Decodes the sounds, one at a time, each installed under the lock as soon as
// it is ready. Stops early when flappysound_stop() is waiting. Returns how many
// loaded.
static int load_all(void) {
	int loaded = 0;
	for (int i = 0; i < SFX_COUNT && !stop_flag; i++) {
		sound_t s = {0};
		if (sound_load(&s, FILES[i])) {
			pthread_mutex_lock(&lock);
			sounds[i] = s;
			pthread_mutex_unlock(&lock);
			loaded++;
		}
	}
	return loaded;
}

// Loads the sounds, takes the output from music and radio, then mixes. Paced by
// the PCM: every write blocks until there is room, so the loop runs at the rate
// the DAC plays. All of it off the interface thread: decoding five Ogg files
// and stopping a radio stream take long enough to be seen as a freeze.
static void *mixer_main(void *unused) {
	(void)unused;
	int loaded = load_all();
	printf("flappysound: %d of %d sounds loaded\n", loaded, SFX_COUNT);
	if (loaded == 0 || stop_flag) {
		return NULL;
	}

	audio_stop();
	if (radio_is_playing()) {
		radio_stop();
	}

	if (stop_flag || !audio_external_begin_latency(RATE, 2, 16, BUFFER_MS)) {
		if (!stop_flag) {
			fprintf(stderr, "flappysound: no PCM; the game plays silently\n");
		}
		return NULL;
	}

	int32_t mix[MIX_FRAMES * 2];
	int16_t out[MIX_FRAMES * 2];
	while (!stop_flag) {
		mix_into(mix);
		for (int i = 0; i < MIX_FRAMES * 2; i++) {
			int32_t v = mix[i];
			out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		audio_external_write(out, MIX_FRAMES);
	}

	audio_external_end();
	return NULL;
}

bool flappysound_start(void) {
	flappysound_stop();

	stop_flag = false;
	if (pthread_create(&mixer, NULL, mixer_main, NULL) != 0) {
		return false;
	}
	mixer_live = true;
	return true;
}

void flappysound_play(sfx_t sfx) {
	if (sfx < 0 || sfx >= SFX_COUNT) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (sounds[sfx].pcm) {
		sounds[sfx].pos = 0;
	}
	pthread_mutex_unlock(&lock);
}

void flappysound_stop(void) {
	if (mixer_live) {
		stop_flag = true;
		pthread_join(mixer, NULL);
		mixer_live = false;
	}
	pthread_mutex_lock(&lock);
	sounds_free();
	pthread_mutex_unlock(&lock);
}
