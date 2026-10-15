// How fast can the frame copy be on this panel?
//
// The interface thread's copy (blit_rotated_180() in src/main.c) takes an
// RGB565 page LVGL drew and expands it into the panel's 32-bit BGRA page, one
// pixel per pixel. A sampling profile on the device put it at 37.5% of the
// interface thread's time -- the single largest cost in the player -- and the
// question is what, if anything, can be done about it.
//
// The earlier answer came from a host measurement: two-pixel stores and 5->8 /
// 6->8 lookup tables were both slower there, so they were left out. That
// measurement cannot be trusted for the device. On the host the destination is
// ordinary cached memory, where the loop is bound by the arithmetic; on the
// device the destination is the framebuffer, which is mapped uncached, where
// every 32-bit store is a bus transaction and the loop may well be bound by the
// number of stores instead. So this measures on the device, and against the
// real /dev/fb0 as well as against plain memory.
//
// Variants:
//
//   base     the loop that is in the player today, one 32-bit store per pixel
//   pair     two pixels an iteration, two 32-bit stores to adjacent addresses
//   pair64   two pixels an iteration, one 64-bit store (sdc1 through the FPU)
//   table    5->8 and 6->8 through 32- and 64-entry tables
//   unroll4  four pixels an iteration
//   memset   pure write bandwidth to the destination: the floor for anything
//   memcpy   a straight copy of the same number of bytes: the floor for the
//            case where the source is already in the destination's format
//
// Build (static, for the device):
//
//   mipsel-rockbox-linux-gnu-gcc -static -O2 -march=mips32r2 -mno-madd4 -o blit_bench blit_bench.c
//
// Run it from the card (/data is not exec on this device):
//
//   adb push blit_bench /mnt/sd_0/ && adb shell 'chmod +x /mnt/sd_0/blit_bench'
//   adb shell '/mnt/sd_0/blit_bench'            # plain memory
//   adb shell '/mnt/sd_0/blit_bench /dev/fb0'   # the real thing

#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 320
#define HEIGHT 480
#define PIXELS ((size_t)WIDTH * HEIGHT)
#define PASSES 20

static uint16_t src_page[PIXELS];
static uint8_t *reference; // what base wrote, for the other variants to match

static inline uint32_t expand(uint16_t p) {
	uint32_t b = p & 0x1Fu, g = (p >> 5) & 0x3Fu, r = (p >> 11) & 0x1Fu;
	return ((b << 3) | (b >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((r << 3) | (r >> 2)) << 16) | 0xFF000000u;
}

static uint32_t spread5[32];
static uint32_t spread6[64];

// One 64-bit store, which MIPS32 has only through the FPU (sdc1). The address
// must be 8-byte aligned; the callers keep the destination page aligned and
// handle pixels in pairs from an even offset.
static inline void store64(uint8_t *at, uint32_t lo, uint32_t hi) {
#if defined(__mips__)
	__asm__ volatile("mtc1 %0, $f0\n\t"
					 "mtc1 %1, $f1\n\t"
					 "sdc1 $f0, %2\n\t" ::"r"(lo),
					 "r"(hi), "m"(*(uint64_t *)at)
					 : "f0", "f1", "memory");
#else
	*(uint64_t *)at = (uint64_t)lo | ((uint64_t)hi << 32);
#endif
}

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// --- the variants ---------------------------------------------------------

static void copy_base(uint8_t *dst) {
	for (int y = 0; y < HEIGHT; y++) {
		const uint16_t *s = src_page + (size_t)y * WIDTH;
		uint32_t *d = (uint32_t *)(dst + (size_t)y * WIDTH * 4);
		for (int x = 0; x < WIDTH; x++) {
			d[x] = expand(s[x]);
		}
	}
}

static void copy_pair(uint8_t *dst) {
	for (int y = 0; y < HEIGHT; y++) {
		const uint16_t *s = src_page + (size_t)y * WIDTH;
		uint32_t *d = (uint32_t *)(dst + (size_t)y * WIDTH * 4);
		int x = 0;
		for (; x + 1 < WIDTH; x += 2) {
			d[x] = expand(s[x]);
			d[x + 1] = expand(s[x + 1]);
		}
		for (; x < WIDTH; x++) {
			d[x] = expand(s[x]);
		}
	}
}

static void copy_pair64(uint8_t *dst) {
	for (int y = 0; y < HEIGHT; y++) {
		const uint16_t *s = src_page + (size_t)y * WIDTH;
		uint8_t *row = dst + (size_t)y * WIDTH * 4;
		int x = 0;
		for (; x + 1 < WIDTH; x += 2) {
			store64(row + (size_t)x * 4, expand(s[x]), expand(s[x + 1]));
		}
		uint32_t *d = (uint32_t *)row;
		for (; x < WIDTH; x++) {
			d[x] = expand(s[x]);
		}
	}
}

static void copy_table(uint8_t *dst) {
	for (int y = 0; y < HEIGHT; y++) {
		const uint16_t *s = src_page + (size_t)y * WIDTH;
		uint32_t *d = (uint32_t *)(dst + (size_t)y * WIDTH * 4);
		for (int x = 0; x < WIDTH; x++) {
			uint16_t p = s[x];
			d[x] = spread5[p & 0x1Fu] | spread6[(p >> 5) & 0x3Fu] | (spread5[p >> 11] << 16) | 0xFF000000u;
		}
	}
}

static void copy_unroll4(uint8_t *dst) {
	for (int y = 0; y < HEIGHT; y++) {
		const uint16_t *s = src_page + (size_t)y * WIDTH;
		uint32_t *d = (uint32_t *)(dst + (size_t)y * WIDTH * 4);
		int x = 0;
		for (; x + 3 < WIDTH; x += 4) {
			uint32_t a = expand(s[x]), b = expand(s[x + 1]);
			uint32_t c = expand(s[x + 2]), e = expand(s[x + 3]);
			d[x] = a;
			d[x + 1] = b;
			d[x + 2] = c;
			d[x + 3] = e;
		}
		for (; x < WIDTH; x++) {
			d[x] = expand(s[x]);
		}
	}
}

static void copy_memcpy(uint8_t *dst, const uint8_t *cached_src, size_t bytes) {
	memcpy(dst, cached_src, bytes);
}

static void copy_memset(uint8_t *dst, size_t bytes) {
	memset(dst, 0xA5, bytes);
}

// --- harness --------------------------------------------------------------

typedef void (*variant_fn)(uint8_t *dst);

static void run(const char *name, variant_fn fn, uint8_t *dst, size_t bytes, bool check) {
	double best = 1e9;
	for (int i = 0; i < PASSES; i++) {
		double t0 = now_ms();
		fn(dst);
		double dt = now_ms() - t0;
		if (dt < best) {
			best = dt;
		}
	}
	double mb = (double)bytes / (1024.0 * 1024.0);
	bool same = true;
	if (check && reference) {
		same = memcmp(dst, reference, bytes) == 0;
	}
	printf("  %-8s %7.2f ms   %6.0f MB/s%s\n", name, best, mb / (best / 1000.0), check ? (same ? "   identical" : "   DIFFERENT") : "");
}

int main(int argc, char **argv) {
	for (int i = 0; i < (int)PIXELS; i++) {
		src_page[i] = (uint16_t)((i * 2654435761u) >> 13);
	}
	for (int i = 0; i < 32; i++) {
		spread5[i] = ((uint32_t)i << 3) | ((uint32_t)i >> 2);
	}
	for (int i = 0; i < 64; i++) {
		spread6[i] = (((uint32_t)i << 2) | ((uint32_t)i >> 4)) << 8;
	}

	const size_t bytes = PIXELS * 4;
	uint8_t *dst = NULL;
	const char *what = "plain memory";

	if (argc > 1) {
		int fd = open(argv[1], O_RDWR);
		if (fd < 0) {
			perror(argv[1]);
			return 1;
		}
		struct fb_var_screeninfo var;
		struct fb_fix_screeninfo fix;
		if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
			perror("FBIOGET_*SCREENINFO");
			return 1;
		}
		size_t map_len = fix.smem_len ? fix.smem_len : bytes;
		if (map_len < bytes) {
			map_len = bytes;
		}
		dst = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (dst == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		// "second" writes into the page the panel is not scanning: the memory is
		// the same (the mapping is uncached either way) and the picture on the
		// glass is left alone.
		if (argc > 2 && strcmp(argv[2], "second") == 0 && map_len >= 2 * bytes) {
			dst += map_len / 2;
			what = "the panel's second page";
		} else {
			what = argv[1];
		}
		printf("framebuffer %s: %ux%u, %u bpp, stride %u, smem %u\n", argv[1], var.xres, var.yres, var.bits_per_pixel,
			   fix.line_length, fix.smem_len);
	} else {
		dst = malloc(bytes);
		if (!dst) {
			return 1;
		}
	}

	reference = malloc(bytes);
	if (!reference) {
		return 1;
	}

	printf("destination: %s, %zu KB per pass\n", what, bytes / 1024);
	copy_base(reference);
	run("base", copy_base, dst, bytes, false);
	run("pair", copy_pair, dst, bytes, true);
	run("pair64", copy_pair64, dst, bytes, true);
	run("table", copy_table, dst, bytes, true);
	run("unroll4", copy_unroll4, dst, bytes, true);

	// The floors: how fast can this destination be written at all.
	uint8_t *cached = malloc(bytes);
	if (cached) {
		memcpy(cached, reference, bytes);
		double best = 1e9;
		for (int i = 0; i < PASSES; i++) {
			double t0 = now_ms();
			copy_memcpy(dst, cached, bytes);
			double dt = now_ms() - t0;
			if (dt < best) {
				best = dt;
			}
		}
		printf("  %-8s %7.2f ms   %6.0f MB/s\n", "memcpy", best, ((double)bytes / 1048576.0) / (best / 1000.0));
		free(cached);
	}
	{
		double best = 1e9;
		for (int i = 0; i < PASSES; i++) {
			double t0 = now_ms();
			copy_memset(dst, bytes);
			double dt = now_ms() - t0;
			if (dt < best) {
				best = dt;
			}
		}
		printf("  %-8s %7.2f ms   %6.0f MB/s\n", "memset", best, ((double)bytes / 1048576.0) / (best / 1000.0));
	}

	return 0;
}
