// rxprobe -- read the Bluetooth receiver's PCM and say what came out of it.
//
// The player's own receiver is a large state machine: it waits for a sender,
// opens the bluealsa capture PCM, converts, and gives up after a few silent
// sessions. When it reports "no audio", the question is whether bluealsa is
// delivering anything at all -- and that question is best asked without the
// state machine in the way.
//
//   rxprobe <MAC> [seconds]
//
// Opens bluealsa:DEV=<MAC>,PROFILE=a2dp for capture at whatever format the PCM
// reports (S32, S24 or S16), reads for the given time, and prints the frames it
// got, their rate and the peak sample.
//
// Two things matter about how it reads:
//
//   * The PCM belongs to one transport, and a phone that reconnects gets a new
//     one. When the stream dies the probe closes the PCM and opens it again,
//     the way the player's receiver does, instead of reading from a dead
//     transport for the rest of the run.
//   * It reads as fast as the daemon produces. A slow reader leaves the
//     daemon's Bluetooth socket undrained, the controller's buffers fill, and
//     the link is dropped with a hardware failure -- which the phone shows as
//     the music pausing. Reading speed is not a detail here, it is the thing
//     being measured.
//
// Build (against the device's alsa):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -O2 -o rxprobe rxprobe.c \
//       -I<sysroot>/usr/include -L<sysroot>/usr/lib -lasound -lrt

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Open the PCM, waiting for it to exist: the object only appears once the
// sender has a transport, which can be seconds after the phone is told to play.
static snd_pcm_t *open_waiting(const char *device, int *attempts) {
	snd_pcm_t *pcm = NULL;
	for (int tries = 0; tries < 200; tries++) {
		(*attempts)++;
		if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0) >= 0) {
			return pcm;
		}
		usleep(200 * 1000);
	}
	return NULL;
}

// Take whatever the PCM offers. Returns 0 on success and fills in the shape of
// the stream, so that a re-open can ask for the same thing.
static int configure(snd_pcm_t *pcm, unsigned *rate, unsigned *channels, snd_pcm_format_t *format) {
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	if (snd_pcm_hw_params_any(pcm, hw) < 0 ||
			snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
		return -1;
	}

	static const snd_pcm_format_t formats[] = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S16_LE};
	snd_pcm_format_t chosen = SND_PCM_FORMAT_UNKNOWN;
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (snd_pcm_hw_params_test_format(pcm, hw, formats[i]) == 0) {
			chosen = formats[i];
			break;
		}
	}
	if (chosen == SND_PCM_FORMAT_UNKNOWN) {
		return -1;
	}
	snd_pcm_hw_params_set_format(pcm, hw, chosen);

	unsigned rate_wanted = *rate ? *rate : 44100;
	snd_pcm_hw_params_set_rate_near(pcm, hw, &rate_wanted, NULL);
	unsigned channels_wanted = *channels ? *channels : 2;
	snd_pcm_hw_params_set_channels_near(pcm, hw, &channels_wanted);
	unsigned buffer_us = 200000;
	snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buffer_us, NULL);

	if (snd_pcm_hw_params(pcm, hw) < 0) {
		return -1;
	}

	// A capture stream has to be started: a PCM with no reader is a PCM the
	// daemon will not fill.
	snd_pcm_start(pcm);

	*rate = rate_wanted;
	*channels = channels_wanted;
	*format = chosen;
	return 0;
}

static double now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: rxprobe <MAC> [seconds]\n");
		return 1;
	}
	int seconds = argc > 2 ? atoi(argv[2]) : 15;
	if (seconds <= 0) {
		seconds = 15;
	}

	char device[96];
	snprintf(device, sizeof(device), "bluealsa:DEV=%s,PROFILE=a2dp", argv[1]);

	unsigned long long total_frames = 0;
	unsigned long long total_reads = 0;
	unsigned long long errors = 0;
	int peak = 0;
	int sessions = 0, opens = 0;
	double started = now_seconds();

	// Non-blocking with an explicit wait, so that the time budget is honoured
	// even when nothing ever arrives.
	unsigned char *buf = NULL;
	size_t buf_bytes = 0;

	while (now_seconds() - started < seconds) {
		snd_pcm_t *pcm = open_waiting(device, &opens);
		if (!pcm) {
			fprintf(stderr, "rxprobe: no PCM for %s\n", device);
			break;
		}

		unsigned rate = 0, channels = 0;
		snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
		if (configure(pcm, &rate, &channels, &format) < 0) {
			fprintf(stderr, "rxprobe: could not configure the PCM\n");
			snd_pcm_close(pcm);
			break;
		}

		sessions++;
		fprintf(stderr, "rxprobe: session %d: open at %u Hz, %u ch, %s (after %d open attempts)\n", sessions, rate,
				channels, snd_pcm_format_name(format), opens);

		size_t frame_bytes = (size_t)channels * (format == SND_PCM_FORMAT_S16_LE ? 2 : 4);
		if (buf_bytes < 4096 * frame_bytes) {
			free(buf);
			buf = malloc(4096 * frame_bytes);
			buf_bytes = buf ? 4096 * frame_bytes : 0;
		}
		if (!buf) {
			fprintf(stderr, "rxprobe: out of memory\n");
			snd_pcm_close(pcm);
			break;
		}

		snd_pcm_nonblock(pcm, 1);

		unsigned long long session_frames = 0;
		double reported = 0;
		double session_started = now_seconds();

		for (;;) {
			double elapsed = now_seconds() - session_started;
			if (now_seconds() - started >= seconds) {
				break;
			}

			snd_pcm_sframes_t got = snd_pcm_readi(pcm, buf, 4096);
			if (got > 0) {
				session_frames += (unsigned long long)got;
				total_frames += (unsigned long long)got;
				total_reads++;
				// Peak of the first channel, just to say whether it is silence.
				for (snd_pcm_sframes_t i = 0; i < got; i++) {
					int v;
					if (format == SND_PCM_FORMAT_S16_LE) {
						v = ((short *)buf)[i * channels];
					} else {
						v = (int)(((unsigned int *)buf)[i * channels] << 8) >> 8;
					}
					if (v < 0) {
						v = -v;
					}
					if (v > peak) {
						peak = v;
					}
				}
				if (elapsed - reported >= 5) {
					reported = elapsed;
					fprintf(stderr, "rxprobe: at %.0f s: %llu frames, %.2f s of audio\n", elapsed, session_frames,
							session_frames / (double)(rate ? rate : 44100));
				}
			} else if (got == -EAGAIN) {
				snd_pcm_wait(pcm, 500);
			} else if (got == -ENODEV) {
				// The transport is gone: this session is over, and the phone
				// is about to reconnect and make a new one.
				break;
			} else {
				errors++;
				if (snd_pcm_recover(pcm, (int)got, 1) < 0) {
					break;
				}
			}
		}

		double alive = now_seconds() - session_started;
		fprintf(stderr, "rxprobe: session %d ended after %.1f s with %llu frames (%.2f s of audio)\n", sessions, alive,
				session_frames, session_frames / (double)(rate ? rate : 44100));

		snd_pcm_close(pcm);

		if (session_frames == 0 && sessions >= 3) {
			fprintf(stderr, "rxprobe: three sessions with no audio; stopping\n");
			break;
		}
	}

	double secs = total_frames / 44100.0;
	printf("rxprobe: %llu frames in %.0f s (%llu reads, %llu errors, %d sessions), %.2f s of audio, peak %d\n",
		   total_frames, now_seconds() - started, total_reads, errors, sessions, secs, peak);

	free(buf);
	return total_frames > 0 ? 0 : 2;
}
