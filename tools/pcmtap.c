// pcmtap -- the PCM frames that reach the sound card, dumped to a file.
//
// Two players, two ways of reaching the card, one dump format:
//
//   - the port writes through alsa-lib (`snd_pcm_writei`, audio.c);
//   - the stock `hiby_player` opens `/dev/snd/pcmC%dD%dp` itself and talks to the
//     driver with ioctls (`(%d)Unable to open PCM /dev/snd/pcmC%uD%u%c.` is its
//     own string). Its libasound is used for the record path and the USB DAC,
//     not for playback -- that is measured, not assumed: with the shim loaded
//     the stock player holds the PCM device and the shim sees no
//     `snd_pcm_open` at all.
//
// So the shim has two layers and they are independent:
//
//   ALSA: `snd_pcm_open`, `..._set_format`, `..._set_channels`,
//         `..._set_rate_near`, `snd_pcm_writei`;
//   raw:  `open`/`openat` of a `/dev/snd/pcm*` node, and `ioctl` on it --
//         `SNDRV_PCM_IOCTL_HW_PARAMS` (which format/channels/rate the card was
//         configured with) and `SNDRV_PCM_IOCTL_WRITEI_FRAMES` (the frames
//         themselves).
//
// The dump is the stream as the driver receives it: after any conversion inside
// alsa-lib, before any SoC-side resampling or the AK4493's own filter and
// hardware volume. That is the boundary userspace can see; for the digital path
// the ground truth beyond it is the optical output and an external S/PDIF
// capture, for the analogue one an ADC.
//
// Use:
//
//   LD_PRELOAD=/data/pcmtap.so PCMTAP_OUT=/data/tap.raw /usr/bin/...
//
// `PCMTAP_OUT` is the raw dump (interleaved, the card's own format), and
// `PCMTAP_OUT.log` gets the device names, the configured format/channels/rate,
// every distinct ioctl seen on the node, and the totals at exit.
//
// A frame size the card reports is trusted only when it is self-consistent
// (exactly one format bit, 1..8 channels, a real rate); otherwise the log says
// so and `PCMTAP_FRAME_BYTES` pins it. Never guess: a wrong stride produces a
// dump that looks fine and misaligns everything after the first write.
//
// Build (static is impossible for a preload, so it links the toolchain's glibc
// and must ask for nothing newer than 2.16 -- check the version-info output):
//
//   mipsel-rockbox-linux-gnu-gcc -shared -fPIC -O2 -Wall -o pcmtap.so pcmtap.c -ldl

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

// The pieces of sound/asound.h the raw layer needs. The ioctl *numbers* are not
// spelled out -- they are recognised by type and number out of the request
// word, which is the same on every ABI, so a mistake in a struct size here
// cannot make the shim blind to the call.
#define IOCTL_TYPE(req) ((unsigned)((req) >> 8) & 0xffu)
#define IOCTL_NR(req) ((unsigned)(req) & 0xffu)
#define IOCTL_SIZE(req) ((unsigned)((req) >> 16) & 0x1fffu)
#define SNDRV_IOCTL_HW_PARAMS 0x11u
#define SNDRV_IOCTL_WRITEI_FRAMES 0x50u

// struct snd_xferi: the result, the user buffer, the frame count.
struct snd_xferi {
	long result;
	void *buf;
	unsigned long frames;
};

// Only the head of struct snd_pcm_hw_params is read: `flags`, then the masks,
// of which the first is the format set. Everything else in the struct depends on
// counts from asound.h, so it is parsed defensively.
#define HW_PARAMS_HEAD 604

// The layout of struct snd_pcm_hw_params as this kernel (3.10.14) has it, read
// back off the device on a stream whose format was known (tinyplay, 16 bit /
// 44100 / 2 ch): flags(4), masks[3](96), mres[5](160), then intervals of **12**
// bytes -- snd_interval packs its four flags into bit fields, which is why the
// struct is 604 bytes and not the 856 a 6-word interval would give -- channels
// at interval 2, rate at 3, then ires[9], the rmask/cmask/info/msbits fields,
// rate_num/rate_den, fifo_size and 64 reserved.
#define HW_PARAMS_INTERVALS 260
#define HW_PARAMS_INTERVAL 12

static snd_pcm_sframes_t (*real_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
static int (*real_open_pcm)(snd_pcm_t **, const char *, int, int);
static int (*real_close_pcm)(snd_pcm_t *);
static int (*real_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
static int (*real_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
static int (*real_set_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *);
static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_openat64)(int, const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *raw;
static FILE *meta;
static size_t frame_bytes;
static int want_bits;
static unsigned int want_channels;
static int want_format = -2;
static unsigned int want_rate;
static unsigned long long open_count, write_calls, frames_total, ioctl_dumps;

// The /dev/snd node a raw write is going to. Kept small: a player has one
// playback node open at a time, and a table is cheaper than a hash.
#define MAX_PCM_FD 8
static struct {
	int fd;
	char path[48];
	FILE *raw;
	size_t frames;
} pcm_fd[MAX_PCM_FD];

#define MAX_SEEN_IOCTL 32
static struct {
	unsigned long req;
	unsigned long long count;
} seen_ioctl[MAX_SEEN_IOCTL];
static int seen_ioctl_count;

// ---------------------------------------------------------------------------
// The sidecar log. Opened on first use: a process loaded with the shim may never
// touch a PCM, and then the only trace wanted is the line that says so.
// ---------------------------------------------------------------------------
static const char *out_path(void) {
	static char path[256];
	const char *env = getenv("PCMTAP_OUT");
	if (!path[0]) {
		snprintf(path, sizeof(path), "%s", env && env[0] ? env : "/data/pcmtap.raw");
	}
	return path;
}

static void meta_line(const char *fmt, ...) {
	if (!meta) {
		char path[320];
		snprintf(path, sizeof(path), "%s.log", out_path());
		meta = fopen(path, "a");
		if (!meta) {
			return;
		}
		setvbuf(meta, NULL, _IOLBF, 0);
	}
	fprintf(meta, "[%d] ", (int)getpid());
	va_list ap;
	va_start(ap, fmt);
	vfprintf(meta, fmt, ap);
	va_end(ap);
	fputc('\n', meta);
}

static void open_dump_locked(void) {
	const char *env = getenv("PCMTAP_FRAME_BYTES");
	size_t pinned = env && env[0] ? (size_t)strtoul(env, NULL, 10) : 0;

	raw = fopen(out_path(), "wb");
	if (!raw) {
		meta_line("cannot open %s: %s", out_path(), strerror(errno));
		return;
	}
	setvbuf(raw, NULL, _IOFBF, 256 * 1024);
	if (pinned) {
		frame_bytes = pinned;
		meta_line("dumping to %s, frame size pinned to %lu bytes", out_path(), (unsigned long)pinned);
	} else {
		meta_line("dumping to %s", out_path());
	}
}

// ---------------------------------------------------------------------------
// The frame size, from whatever the stream said about itself. Recomputable: a
// player that reconfigures between tracks is followed.
// ---------------------------------------------------------------------------
static int format_bits(int format) {
	static const signed char known[] = {
	    8,  8,  16, 16, 16, 16, 32, 32, 32, 32, // S8..U24_BE
	    32, 32, 32, 32, 32, 32, 64, 64,         // S32..FLOAT64_BE
	    32, 32, 8,  8,  8,  8,  8,  8,          // IEC958..GSM
	};
	if (format >= 0 && format < (int)(sizeof(known) / sizeof(known[0])) && known[format] > 0) {
		return known[format];
	}
	// The 24- and 20-bit packed forms live above the older enum: S24_3LE is 32
	// and the three around it follow in order, then S20_3LE at 36.
	if (format >= 32 && format <= 35) {
		return 24;
	}
	if (format >= 36 && format <= 39) {
		return 24;
	}
	return 0;
}

static void recompute_frame_size_locked(const char *why) {
	if (getenv("PCMTAP_FRAME_BYTES")) {
		return;
	}
	if (want_bits > 0 && want_channels > 0) {
		size_t next = (size_t)(want_bits / 8) * want_channels;
		if (next != frame_bytes) {
			frame_bytes = next;
			meta_line("frame size %lu bytes (%d bits x %u ch, from %s)",
			          (unsigned long)frame_bytes, want_bits, want_channels, why);
		}
	}
}

// Parses the head of a struct snd_pcm_hw_params the driver just filled in. Only
// accepted when the three pieces agree with each other; a dump built on a
// half-understood struct is worse than none.
static void parse_hw_params_locked(const unsigned char *head, size_t len) {
	if (len < HW_PARAMS_HEAD) {
		return;
	}
	unsigned int flags;
	memcpy(&flags, head, sizeof(flags));

	// masks[0] is the format set: one bit per allowed snd_pcm_format_t.
	unsigned int words[8];
	memcpy(words, head + 4, sizeof(words));
	int bit = -1, bits = 0;
	for (int w = 0; w < 8; w++) {
		for (int b = 0; b < 32; b++) {
			if (words[w] & (1u << b)) {
				bit = w * 32 + b;
				bits++;
			}
		}
	}

	unsigned int ch_min = 0, ch_max = 0, rate_min = 0, rate_max = 0;
	memcpy(&ch_min, head + HW_PARAMS_INTERVALS + 2 * HW_PARAMS_INTERVAL, 4);
	memcpy(&ch_max, head + HW_PARAMS_INTERVALS + 2 * HW_PARAMS_INTERVAL + 4, 4);
	memcpy(&rate_min, head + HW_PARAMS_INTERVALS + 3 * HW_PARAMS_INTERVAL, 4);
	memcpy(&rate_max, head + HW_PARAMS_INTERVALS + 3 * HW_PARAMS_INTERVAL + 4, 4);

	// The frame size is the one thing here that must not be inferred: the
	// format mask turned out to report the same bit for a 16-bit and a 24-bit
	// stream on this kernel (measured on tinyplay and on the port), so it is
	// logged and not trusted. Period and buffer geometry are unambiguous --
	// bytes divided by frames is the stride the driver was configured with.
	unsigned int period_size = 0, period_bytes = 0, buffer_size = 0, buffer_bytes = 0;
	memcpy(&period_size, head + HW_PARAMS_INTERVALS + 5 * HW_PARAMS_INTERVAL, 4);
	memcpy(&period_bytes, head + HW_PARAMS_INTERVALS + 6 * HW_PARAMS_INTERVAL, 4);
	memcpy(&buffer_size, head + HW_PARAMS_INTERVALS + 9 * HW_PARAMS_INTERVAL, 4);
	memcpy(&buffer_bytes, head + HW_PARAMS_INTERVALS + 10 * HW_PARAMS_INTERVAL, 4);

	size_t stride = 0;
	if (period_size > 0 && period_bytes >= period_size && period_bytes % period_size == 0) {
		stride = period_bytes / period_size;
	} else if (buffer_size > 0 && buffer_bytes >= buffer_size && buffer_bytes % buffer_size == 0) {
		stride = buffer_bytes / buffer_size;
	}

	if (bits == 1 && ch_min == ch_max && ch_min >= 1 && ch_min <= 8 && rate_min == rate_max &&
	    rate_min >= 8000 && rate_min <= 768000) {
		want_format = bit;
		want_channels = ch_min;
		want_rate = rate_min;
		if (stride >= 1 && stride <= 32) {
			want_bits = (int)(stride / ch_min * 8);
			recompute_frame_size_locked("hw_params geometry");
		}
		meta_line("hw_params: format bit %d (not trusted), %u ch, %u Hz, period %u/%u B, buffer %u/%u B -> stride %lu",
		          bit, ch_min, rate_min, period_size, period_bytes, buffer_size, buffer_bytes,
		          (unsigned long)stride);
		return;
	}
	meta_line("hw_params: format bits=%d at %d, channels %u..%u, rate %u..%u -- not self-consistent",
	          bits, bit, ch_min, ch_max, rate_min, rate_max);
	meta_line("hw_params: not self-consistent, frame size still %lu",
	          (unsigned long)frame_bytes);
}

// Which layer writes the dump. Both are on by default; a run that compares two
// players wants the *same* one on each side, and the stock only has the raw one.
static int layer_on(const char *want) {
	const char *env = getenv("PCMTAP_LAYER");
	if (!env || !env[0] || strcmp(env, "both") == 0) {
		return 1;
	}
	return strcmp(env, want) == 0;
}

static void record_to(FILE *out, const void *buf, unsigned long frames, const char *why) {
	if (!out) {
		return;
	}
	if (frame_bytes) {
		fwrite(buf, frame_bytes, (size_t)frames, out);
		frames_total += frames;
	} else if (write_calls < 8 || write_calls % 200 == 0) {
		meta_line("write %llu (%s): frame size unknown (format %d, %d ch)", write_calls, why,
		          want_format, (int)want_channels);
	}
}

static void record_locked(int fd, const void *buf, unsigned long frames, const char *why) {
	write_calls++;
	if (!raw) {
		open_dump_locked();
	}
	if (raw) {
		record_to(raw, buf, frames, why);
	}
	for (int i = 0; i < MAX_PCM_FD; i++) {
		if (pcm_fd[i].fd == fd) {
			if (!pcm_fd[i].raw) {
				char path[320];
				snprintf(path, sizeof(path), "%s.fd%d", out_path(), fd);
				pcm_fd[i].raw = fopen(path, "wb");
				if (pcm_fd[i].raw) {
					setvbuf(pcm_fd[i].raw, NULL, _IOFBF, 64 * 1024);
					meta_line("per-fd dump %s", path);
				}
			}
			record_to(pcm_fd[i].raw, buf, frames, why);
			pcm_fd[i].frames += frames;
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// The raw layer: which fd is a PCM node, and what is written to it.
// ---------------------------------------------------------------------------
static void track_locked(int fd, const char *path) {
	// The PCM nodes are the card; /dev/sa* is the vendor's own DMA node, which
	// the stock player also holds. Both are worth knowing about; only the first
	// is a PCM.
	if (fd < 0 || !path || (strncmp(path, "/dev/snd/pcm", 12) != 0 && strncmp(path, "/dev/sa", 7) != 0)) {
		return;
	}
	for (int i = 0; i < MAX_PCM_FD; i++) {
		if (pcm_fd[i].fd == fd) {
			snprintf(pcm_fd[i].path, sizeof(pcm_fd[i].path), "%s", path);
			return;
		}
	}
	for (int i = 0; i < MAX_PCM_FD; i++) {
		if (pcm_fd[i].fd == 0) {
			pcm_fd[i].fd = fd;
			snprintf(pcm_fd[i].path, sizeof(pcm_fd[i].path), "%s", path);
			meta_line("opened %s (fd %d)", path, fd);
			return;
		}
	}
}

static void untrack_locked(int fd) {
	for (int i = 0; i < MAX_PCM_FD; i++) {
		if (pcm_fd[i].fd == fd) {
			meta_line("closed %s (fd %d)", pcm_fd[i].path, fd);
			pcm_fd[i].fd = 0;
			pcm_fd[i].path[0] = 0;
		}
	}
}


static void note_ioctl_locked(unsigned long req) {
	for (int i = 0; i < seen_ioctl_count; i++) {
		if (seen_ioctl[i].req == req) {
			seen_ioctl[i].count++;
			return;
		}
	}
	if (seen_ioctl_count < MAX_SEEN_IOCTL) {
		seen_ioctl[seen_ioctl_count].req = req;
		seen_ioctl[seen_ioctl_count].count = 1;
		seen_ioctl_count++;
		meta_line("ioctl 0x%lx: dir %u size %u type 0x%x nr 0x%x", req, (unsigned)(req >> 29) & 7u,
		          IOCTL_SIZE(req), IOCTL_TYPE(req), IOCTL_NR(req));
	}
}

// ---------------------------------------------------------------------------
// open / openat, so a raw fd can be recognised.
// ---------------------------------------------------------------------------
static void note_open_path(int fd, const char *path) {
	if (fd < 0 || !path) {
		return;
	}
	pthread_mutex_lock(&lock);
	track_locked(fd, path);
	pthread_mutex_unlock(&lock);
}

int open(const char *path, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, unsigned int);
		va_end(ap);
	}
	if (!real_open) {
		real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
	}
	int fd = real_open ? real_open(path, flags, mode) : -ENOSYS;
	note_open_path(fd, path);
	return fd;
}

int open64(const char *path, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, unsigned int);
		va_end(ap);
	}
	if (!real_open64) {
		real_open64 = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open64");
	}
	int fd = real_open64 ? real_open64(path, flags, mode) : -ENOSYS;
	note_open_path(fd, path);
	return fd;
}

int openat(int dirfd, const char *path, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, unsigned int);
		va_end(ap);
	}
	if (!real_openat) {
		real_openat = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat");
	}
	int fd = real_openat ? real_openat(dirfd, path, flags, mode) : -ENOSYS;
	note_open_path(fd, path);
	return fd;
}

int openat64(int dirfd, const char *path, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, unsigned int);
		va_end(ap);
	}
	if (!real_openat64) {
		real_openat64 = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat64");
	}
	int fd = real_openat64 ? real_openat64(dirfd, path, flags, mode) : -ENOSYS;
	note_open_path(fd, path);
	return fd;
}

// ---------------------------------------------------------------------------
// What a vendor ioctl is handed. The stock player's audio buffer is described to
// /dev/sa_hgl_dma once (address and size), and after that the samples are plain
// memory stores -- no syscall per period -- so the arguments are the only place
// the buffer can be learned from userspace. Reads go through /proc/self/mem, so
// an address that is not mapped answers EIO instead of killing the player.
// ---------------------------------------------------------------------------
static int self_mem_fd = -1;

static void log_pointer_context_locked(unsigned long addr) {
	if (addr < 0x1000 || addr > 0xfffff000UL) {
		meta_line("  arg 0x%lx: not a pointer", addr);
		return;
	}

	if (self_mem_fd < 0) {
		self_mem_fd = open("/proc/self/mem", O_RDONLY);
	}
	unsigned char buf[32];
	ssize_t n = -1;
	if (self_mem_fd >= 0) {
		n = pread(self_mem_fd, buf, sizeof(buf), (off_t)addr);
	}
	if (n <= 0) {
		meta_line("  arg 0x%lx: not readable (%s)", addr, n < 0 ? strerror(errno) : "eof");
		return;
	}
	char hex[sizeof(buf) * 3];
	hex[0] = 0;
	for (ssize_t i = 0; i < n; i++) {
		char one[4];
		snprintf(one, sizeof(one), "%02x", buf[i]);
		strcat(hex, one);
	}
	meta_line("  arg 0x%lx: %s", addr, hex);

	// And which mapping it belongs to: a DMA buffer the vendor driver mapped is
	// visible as its own anonymous or character-device mapping.
	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), maps)) {
		unsigned long lo = 0, hi = 0;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && addr >= lo && addr < hi) {
			line[strcspn(line, "\n")] = 0;
			meta_line("  mapping %s", line);
			break;
		}
	}
	fclose(maps);
}

// ---------------------------------------------------------------------------
// ioctl: the whole raw path. STRICTLY a pass-through -- the request, the
// argument and the return value are the caller's, and nothing here allocates on
// a path the audio thread runs.
// ---------------------------------------------------------------------------
int ioctl(int fd, unsigned long req, ...) {
	void *arg = NULL;
	va_list ap;
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!real_ioctl) {
		real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
	}

	// ALSA's requests are `type 'A'`; that is what makes this work without
	// knowing which API opened the node. The stock player uses `openat64`,
	// which used to slip past the fd table entirely -- the ioctl itself does
	// not.
	int alsa_req = IOCTL_TYPE(req) == 'A';
	pthread_mutex_lock(&lock);
	int known_fd = 0;
	for (int i = 0; i < MAX_PCM_FD; i++) {
		if (pcm_fd[i].fd == fd) {
			known_fd = 1;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	if (alsa_req || known_fd) {
		pthread_mutex_lock(&lock);
		note_ioctl_locked(req);
		if (known_fd && !alsa_req) {
			meta_line("vendor ioctl fd %d req 0x%lx", fd, req);
			log_pointer_context_locked((unsigned long)arg);
		}
		pthread_mutex_unlock(&lock);
	}

	int ret = real_ioctl ? real_ioctl(fd, req, arg) : -ENOSYS;

	if (!alsa_req || ret < 0) {
		return ret;
	}

	if (IOCTL_TYPE(req) == 'A' && IOCTL_NR(req) == SNDRV_IOCTL_HW_PARAMS && arg) {
		pthread_mutex_lock(&lock);
		parse_hw_params_locked((const unsigned char *)arg, HW_PARAMS_HEAD);
		pthread_mutex_unlock(&lock);
	} else if (IOCTL_NR(req) == SNDRV_IOCTL_WRITEI_FRAMES && arg) {
		struct snd_xferi *x = (struct snd_xferi *)arg;
		unsigned long done = x->result > 0 ? (unsigned long)x->result : 0;
		if (done && x->buf) {
			pthread_mutex_lock(&lock);
			ioctl_dumps++;
			if (!layer_on("raw")) {
				pthread_mutex_unlock(&lock);
				return ret;
			}
			if (ioctl_dumps < 40 || ioctl_dumps % 500 == 0) {
				meta_line("writei fd %d: frames %lu, result %ld, dump #%llu", fd, x->frames,
				          (long)x->result, ioctl_dumps);
			}
			record_locked(fd, x->buf, done, "ioctl");
			pthread_mutex_unlock(&lock);
		}
	}
	return ret;
}

// ---------------------------------------------------------------------------
// write(): the vendor node's other way in. /dev/sa_hgl_dma is the path the
// stock player is actually audible through -- its ALSA node carries a stream
// that is not the track (measured) -- so if the samples are handed over with
// write(2) rather than an ioctl, this is where they are.
// ---------------------------------------------------------------------------
ssize_t write(int fd, const void *buf, size_t count) {
	static ssize_t (*real_write)(int, const void *, size_t);
	if (!real_write) {
		real_write = (ssize_t(*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
	}

	if (fd >= 0 && buf) {
		int vendor = 0;
		pthread_mutex_lock(&lock);
		for (int i = 0; i < MAX_PCM_FD; i++) {
			if (pcm_fd[i].fd == fd && strncmp(pcm_fd[i].path, "/dev/sa", 7) == 0) {
				vendor = 1;
				if (!pcm_fd[i].raw) {
					char path[320];
					snprintf(path, sizeof(path), "%s.sa", out_path());
					pcm_fd[i].raw = fopen(path, "wb");
					if (pcm_fd[i].raw) {
						setvbuf(pcm_fd[i].raw, NULL, _IOFBF, 64 * 1024);
						meta_line("vendor dump %s", path);
					}
				}
				if (pcm_fd[i].raw) {
					fwrite(buf, 1, count, pcm_fd[i].raw);
					pcm_fd[i].frames += count;
					if (pcm_fd[i].frames < 8 * 1024 * 1024 && pcm_fd[i].frames % (256 * 1024) < count) {
						meta_line("vendor write fd %d: %lu bytes (total %lu)", fd, (unsigned long)count,
						          (unsigned long)pcm_fd[i].frames);
					}
				}
				break;
			}
		}
		pthread_mutex_unlock(&lock);
		(void)vendor;
	}

	ssize_t n = real_write ? real_write(fd, buf, count) : (ssize_t)-ENOSYS;
	return n;
}

// ---------------------------------------------------------------------------
// mmap: how the vendor node and any DMA buffer is handed out. Only logged -- the
// shim must not change an address.
// ---------------------------------------------------------------------------
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long offset) {
	static void *(*real_mmap)(void *, size_t, int, int, int, long);
	if (!real_mmap) {
		real_mmap = (void *(*)(void *, size_t, int, int, int, long))dlsym(RTLD_NEXT, "mmap");
	}
	void *ret = real_mmap ? real_mmap(addr, len, prot, flags, fd, offset) : (void *)-1;
	if (fd >= 0) {
		pthread_mutex_lock(&lock);
		meta_line("mmap fd %d len %lu prot 0x%x flags 0x%x off %ld -> %p", fd, (unsigned long)len,
		          prot, flags, offset, ret);
		pthread_mutex_unlock(&lock);
	}
	return ret;
}

// ---------------------------------------------------------------------------
// The alsa-lib layer: the port's own path.
// ---------------------------------------------------------------------------
int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode) {
	if (!real_open_pcm) {
		real_open_pcm = (int (*)(snd_pcm_t **, const char *, int, int))dlsym(RTLD_NEXT, "snd_pcm_open");
	}
	int err = real_open_pcm ? real_open_pcm(pcm, name, stream, mode) : -ENOSYS;
	pthread_mutex_lock(&lock);
	open_count++;
	meta_line("alsa open '%s' stream=%d -> %d", name ? name : "(null)", stream, err);
	pthread_mutex_unlock(&lock);
	return err;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int format) {
	if (!real_set_format) {
		real_set_format = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, int))
		    dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_format");
	}
	int ret = real_set_format ? real_set_format(pcm, params, format) : -ENOSYS;
	if (ret >= 0) {
		pthread_mutex_lock(&lock);
		want_format = format;
		want_bits = format_bits(format);
		meta_line("alsa format %d -> %d bits/sample", format, want_bits);
		recompute_frame_size_locked("alsa set_format");
		pthread_mutex_unlock(&lock);
	}
	return ret;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int channels) {
	if (!real_set_channels) {
		real_set_channels = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int))
		    dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_channels");
	}
	int ret = real_set_channels ? real_set_channels(pcm, params, channels) : -ENOSYS;
	if (ret >= 0) {
		pthread_mutex_lock(&lock);
		want_channels = channels;
		meta_line("alsa %u channel(s)", channels);
		recompute_frame_size_locked("alsa set_channels");
		pthread_mutex_unlock(&lock);
	}
	return ret;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *rate, int *dir) {
	if (!real_set_rate_near) {
		real_set_rate_near = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *))
		    dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_rate_near");
	}
	unsigned int asked = rate ? *rate : 0;
	int ret = real_set_rate_near ? real_set_rate_near(pcm, params, rate, dir) : -ENOSYS;
	if (ret >= 0) {
		pthread_mutex_lock(&lock);
		want_rate = rate ? *rate : 0;
		meta_line("alsa rate asked %u -> %u", asked, want_rate);
		pthread_mutex_unlock(&lock);
	}
	return ret;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t frames) {
	if (!real_writei) {
		real_writei = (snd_pcm_sframes_t(*)(snd_pcm_t *, const void *, snd_pcm_uframes_t))
		    dlsym(RTLD_NEXT, "snd_pcm_writei");
	}
	if (!real_writei) {
		return -ENOSYS;
	}
	snd_pcm_sframes_t written = real_writei(pcm, buf, frames);
	if (written > 0 && buf && layer_on("alsa")) {
		pthread_mutex_lock(&lock);
		record_locked(-1, buf, (unsigned long)written, "alsa");
		pthread_mutex_unlock(&lock);
	} else if (written < 0 && written != -EAGAIN) {
		pthread_mutex_lock(&lock);
		meta_line("writei returned %ld at call %llu", (long)written, write_calls);
		pthread_mutex_unlock(&lock);
	}
	return written;
}

int snd_pcm_close(snd_pcm_t *pcm) {
	if (!real_close_pcm) {
		real_close_pcm = (int (*)(snd_pcm_t *))dlsym(RTLD_NEXT, "snd_pcm_close");
	}
	pthread_mutex_lock(&lock);
	meta_line("alsa close: %llu calls, %llu frames, %lu bytes/frame, %u Hz", write_calls, frames_total,
	          (unsigned long)frame_bytes, want_rate);
	if (raw) {
		fflush(raw);
	}
	pthread_mutex_unlock(&lock);
	return real_close_pcm ? real_close_pcm(pcm) : 0;
}

// close() has to be tracked as well, or a reopened node inherits an old entry.
int close(int fd) {
	static int (*real_close)(int);
	if (!real_close) {
		real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
	}
	pthread_mutex_lock(&lock);
	untrack_locked(fd);
	pthread_mutex_unlock(&lock);
	return real_close ? real_close(fd) : 0;
}

__attribute__((destructor)) static void pcmtap_exit(void) {
	pthread_mutex_lock(&lock);
	meta_line("exit: %llu alsa opens, %llu writes (%llu by ioctl), %llu frames, %lu bytes/frame, %u Hz",
	          open_count, write_calls, ioctl_dumps, frames_total, (unsigned long)frame_bytes, want_rate);
	if (raw) {
		fclose(raw);
		raw = NULL;
	}
	if (meta) {
		fclose(meta);
		meta = NULL;
	}
	pthread_mutex_unlock(&lock);
}
