#include "lvgl/lvgl.h"

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <sys/mman.h>
#include <pthread.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/nowplaying/cover.h"
#include "src/system/audio/waveform.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/frameprof.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/powersettings.h"
#include "src/gui/settings/settings.h"
#include "src/system/device/adb.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/audio/audio.h"
#include "src/system/library/albumart.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btlog.h"
#include "src/system/bluetooth/btplayer.h"
#include "src/system/device/clock.h"
#include "src/system/device/factoryreset.h"
#include "src/system/core/config.h"
#include "src/system/core/profiler.h"
#include "src/system/core/lang.h"
#include "src/system/device/led.h"
#include "src/system/audio/eq.h"
#include "src/system/audio/headset.h"
#include "src/system/input/keymap.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/tidal.h"
#include "src/system/remote/dlna.h"
#include "src/system/gearboy/gbdb.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/core/panel.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/lastfm/lastfm.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/playback/playlist.h"
#include "src/system/library/playlists.h"
#include "src/system/device/power.h"
#include "src/system/streaming/radio.h"
#include "src/system/device/screenshot.h"
#include "src/system/device/sysinfo.h"
#include "src/system/net/tls.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/core/utils.h"
#include "src/system/net/webcgi.h"
#include "src/system/net/wifi.h"


static volatile sig_atomic_t running = 1;

#ifdef HOST_BUILD
// Where the simulator keeps what the device keeps on its writable partition.
//
// The host has no /data and no card of its own, so without a file of its own the
// simulator starts every run as a first boot: the language, the time zone, the
// clock's own "configured" flag and the reader's bookmarks are all settings, and
// settings that cannot be written are settings that get asked for again. Under
// /tmp they survive the process, so a walk through the interface starts where the
// last one left off. SONIX_CONFIG and SONIX_EBOOK_CONFIG still override the
// paths, and nothing here is used on the device.
#define SONIX_HOST_STATE_DIR "/tmp/sonix/data"

// Screenshot trigger on the simulator. The device uses volume-up plus power,
// physical keys that do not exist here, so creating /tmp/sonix/shot takes their
// place and runs the same screenshot path (copy, flash, PNG writer).
// Scripted touches on the simulator: "x y" in /tmp/sonix/tap taps there,
// "drag x1 y1 x2 y2" swipes. A second pointer, read alongside the mouse.
#define HOST_TAP_STEPS 64
static struct {
	int16_t x, y;
	bool pressed;
} host_tap_queue[HOST_TAP_STEPS];
static int host_tap_len, host_tap_pos;
static lv_point_t host_tap_last;

static void host_tap_push(int x, int y, bool pressed) {
	if (host_tap_len < HOST_TAP_STEPS) {
		host_tap_queue[host_tap_len].x = (int16_t)x;
		host_tap_queue[host_tap_len].y = (int16_t)y;
		host_tap_queue[host_tap_len].pressed = pressed;
		host_tap_len++;
	}
}

static void host_tap_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	if (host_tap_pos < host_tap_len) {
		host_tap_last.x = host_tap_queue[host_tap_pos].x;
		host_tap_last.y = host_tap_queue[host_tap_pos].y;
		data->state = host_tap_queue[host_tap_pos].pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
		if (++host_tap_pos == host_tap_len) {
			host_tap_pos = host_tap_len = 0;
		}
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
	data->point = host_tap_last;
}

static void host_tap_poll(void) {
	if (host_tap_len) {
		return; // still playing the previous gesture
	}
	FILE *f = fopen("/tmp/sonix/tap", "r");
	if (!f) {
		return;
	}
	char line[128] = "";
	if (!fgets(line, sizeof(line), f)) {
		line[0] = '\0';
	}
	fclose(f);
	remove("/tmp/sonix/tap");
	int x1, y1, x2, y2;
	if (sscanf(line, "drag %d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
		for (int i = 0; i <= 20; i++) {
			host_tap_push(x1 + (x2 - x1) * i / 20, y1 + (y2 - y1) * i / 20, true);
		}
		host_tap_push(x2, y2, false);
	} else if (sscanf(line, "%d %d", &x1, &y1) == 2) {
		for (int i = 0; i < 3; i++) {
			host_tap_push(x1, y1, true);
		}
		host_tap_push(x1, y1, false);
	}
}

static void host_shot_poll_cb(lv_timer_t *timer) {
	(void)timer;
	host_tap_poll();
	if (access("/tmp/sonix/shot", F_OK) == 0) {
		remove("/tmp/sonix/shot");
		screenshot_request();
	}
}
#endif

// "Remember track", deferred: restoring the remembered track kicks off a cover
// decode and an audio open, which must not run while the very first frame is
// still being put together. It is scheduled a moment after the UI has painted.
// True when a track was put back.
static bool restore_saved_track(void) {
	char saved_track[512];
	double saved_pos = 0;
	if (!library_playback_state_load(saved_track, sizeof(saved_track), &saved_pos) ||
		access(saved_track, R_OK) != 0) {
		return false;
	}

	// The queue is saved next to the track, so a library list or a shuffled
	// order comes back as the queue the user left behind rather than as a
	// folder queue built out of the remembered file.
	// A queue that was a library list comes back as the query it was, re-run
	// against whatever the library holds now, so a rescan since the last boot
	// is reflected rather than restored around.
	library_index_spec_t spec;
	int spec_pos = 0;
	bool had_query = library_queue_load_query(&spec, &spec_pos);
	if (had_query) {
		library_index_t *ix = library_index_open(spec.kind, spec.filter, spec.value, spec.order, spec.desc);
		if (library_index_count(ix) > 0) {
			// Whatever was added to that queue by hand rides in the same table
			// the path form uses; here it is a handful of rows, not a library.
			// Each carries the slot it sat at, so they go back where they were
			// instead of in front of whatever is playing.
			char **extra = NULL;
			int *extra_slots = NULL;
			int extra_count = library_queue_load_extra(&extra, &extra_slots);
			player_restore_index(ix, spec_pos, (const char *const *)extra, extra_slots, extra_count, saved_track,
								 saved_pos);
			library_queue_free(extra, extra_count);
			free(extra_slots);
			return true;
		}
		library_index_close(ix);
	}

	// The rows form, but only when there was no query: with one, whatever is in
	// that table is the handful of tracks added to the library queue by hand,
	// and restoring those alone comes back as a two-track folder queue.
	char **queue = NULL;
	int queue_index = 0;
	bool queue_custom = false;
	int queue_count = had_query ? 0 : library_queue_load(&queue, &queue_index, &queue_custom);
	if (queue_count > 0) {
		player_restore_list((const char *const *)queue, queue_count, queue_index, queue_custom, saved_track, saved_pos);
		library_queue_free(queue, queue_count);
		return true;
	}

	player_restore_track(saved_track, saved_pos);
	return true;
}

static void restore_track_timer_cb(lv_timer_t *timer) {
	lv_timer_delete(timer);
	restore_saved_track();
}

#ifndef HOST_BUILD
// Powering the Bluetooth radio down waits for three daemons to take their
// SIGTERM, so it happens off the interface thread. See the call site.
static void *bluetooth_off_thread(void *arg) {
	(void)arg;
	power_bluetooth_off();
	return NULL;
}
#endif

static void sigint_handler(int sig) {
	(void)sig;
	running = 0;
}

// ---------------------------------------------------------------------------
// staying alive
//
// sonix_player.sh starts the player through sonix_launch, which does
// `sleep 1; reboot` when it exits (the shell does it on a rootfs without it).
// Whatever the reason -- a clean exit, a failed display init, a segfault, a
// stray SIGPIPE -- the moment this process goes away the device reboots, so a
// bug that would be a crash on a PC is a boot loop here, with no console.
//
// Two rules follow. Nothing may kill the process by default, and no failure
// path may return from main(): it parks instead, which leaves the device up,
// ADB reachable and the log readable.
// ---------------------------------------------------------------------------

// Everything the crash handler writes is also kept here, so the same text can
// be handed to the black box in one go. Static, because a signal handler has
// no business allocating.
static char crash_text[512];
static size_t crash_used;

static void crash_keep(const char *text, size_t len) {
	if (crash_used + len >= sizeof(crash_text)) {
		len = sizeof(crash_text) - 1 - crash_used;
	}
	if (len == 0) {
		return;
	}
	memcpy(crash_text + crash_used, text, len);
	crash_used += len;
}

static void write_raw(const char *text) {
	size_t len = strlen(text);
	ssize_t ignored = write(STDERR_FILENO, text, len);
	(void)ignored;
	crash_keep(text, len);
}

// Writes an unsigned value as hex. Async-signal-safe: no snprintf, no malloc.
static void write_hex(unsigned long value) {
	static const char digits[] = "0123456789abcdef";
	char buf[2 + 2 * sizeof(unsigned long)];
	char *p = buf + sizeof(buf);
	do {
		*--p = digits[value & 0xf];
		value >>= 4;
	} while (value);
	*--p = 'x';
	*--p = '0';
	size_t len = (size_t)(buf + sizeof(buf) - p);
	ssize_t ignored = write(STDERR_FILENO, p, len);
	(void)ignored;
	crash_keep(p, len);
}

// The same as a plain decimal. A pid in hex is a pid nobody can match against
// what `ps` prints, and matching it is the whole point of printing it.
static void write_dec(unsigned long value) {
	char buf[3 * sizeof(unsigned long) + 1];
	char *p = buf + sizeof(buf);
	do {
		*--p = (char)('0' + (value % 10));
		value /= 10;
	} while (value);
	size_t len = (size_t)(buf + sizeof(buf) - p);
	ssize_t ignored = write(STDERR_FILENO, p, len);
	(void)ignored;
	crash_keep(p, len);
}

// ---------------------------------------------------------------------------
// what the machine was holding when it fell over
//
// On this device the kernel reports every user fault with si_code SI_KERNEL
// (0x80) and si_addr zero, so the address that faulted is NOT in the siginfo
// and a logged "fault address: 0x0" is an empty field, not a null pointer. It
// has to be worked out instead: disassemble the instruction at the program
// counter and read the register it used, which means the registers have to be
// in the log.
//
// Same reason for the stack. -O3 with no frame pointer leaves nothing a
// backtrace can walk, but return addresses are still lying in the frames, so
// reading the stack and keeping every word that falls inside the program's own
// code gives the chain of callers back. Both are plain memory reads, which is
// all a signal handler is allowed to do.
// ---------------------------------------------------------------------------

// The program's code, for telling a return address from a number that happens
// to be on the stack. Weak: a toolchain that defines neither simply gets no
// trace rather than a link error.
extern char _ftext[] __attribute__((weak));
extern char __executable_start[] __attribute__((weak));
extern char etext[] __attribute__((weak));

static uintptr_t text_begin(void) {
	if (_ftext) {
		return (uintptr_t)_ftext; // what the MIPS linker calls it
	}
	return (uintptr_t)__executable_start;
}

#if defined(__GLIBC__) && defined(__mips__)
#define CRASH_REGS 32
static const char *const REG_NAMES[CRASH_REGS] = {
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",	 "t3", "t4", "t5", "t6", "t7",
	"s0",	"s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0",	 "k1", "gp", "sp", "s8", "ra",
};
static uintptr_t crash_reg(const ucontext_t *uc, int i) { return (uintptr_t)uc->uc_mcontext.gregs[i]; }
static uintptr_t crash_pc(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.pc; }
static uintptr_t crash_sp(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[29]; }
#elif defined(__GLIBC__) && defined(__x86_64__)
// The host build, so the whole report can be exercised in the simulator and
// not only on a device nobody can attach a debugger to.
//
// The REG_* names live behind _GNU_SOURCE, which this file does not ask for;
// the numbers are fixed by the x86-64 ABI's gregset_t and are only ever used
// here.
#ifndef REG_RSP
#define REG_RSP 15
#define REG_RIP 16
#endif
#define CRASH_REGS 16
static const char *const REG_NAMES[CRASH_REGS] = {
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rdi", "rsi", "rbp", "rbx", "rdx", "rax", "rcx", "rsp",
};
static uintptr_t crash_reg(const ucontext_t *uc, int i) { return (uintptr_t)uc->uc_mcontext.gregs[i]; }
static uintptr_t crash_pc(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]; }
static uintptr_t crash_sp(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RSP]; }
#endif

#ifdef CRASH_REGS
// Four to a line, named, because a column of bare numbers is unreadable and the
// name is what says which one the faulting instruction was using.
static void write_registers(const ucontext_t *uc) {
	for (int i = 0; i < CRASH_REGS; i++) {
		write_raw((i % 4) == 0 ? "*** " : "  ");
		write_raw(REG_NAMES[i]);
		write_raw("=");
		write_hex(crash_reg(uc, i));
		if ((i % 4) == 3) {
			write_raw("\n");
		}
	}
}

// Where the stack pointer sits between the two ends of the stack. A fault on a
// store to sp with a handful of bytes left below it is a stack overflow and
// needs no further explanation; a fault with megabytes to spare is something
// else entirely, and knowing which is which is the difference between looking
// for the right bug and looking for the wrong one.
static void write_stack_room(uintptr_t sp) {
	uintptr_t low = 0, high = 0;
	write_raw("*** sp: ");
	write_hex(sp);
	if (!thread_stack_bounds(&low, &high)) {
		write_raw("  (stack bounds unknown on this thread)\n");
		return;
	}
	write_raw("  stack ");
	write_hex(low);
	write_raw("..");
	write_hex(high);
	if (sp < low || sp >= high) {
		write_raw("  OUTSIDE ITS STACK\n");
		return;
	}
	write_raw("  room left ");
	write_dec((unsigned long)(sp - low));
	write_raw(" bytes, used ");
	write_dec((unsigned long)(high - sp));
	write_raw("\n");
}

// The first CRASH_TRACE_MAX words still on the stack that point into the
// program's own code. Not a backtrace -- there is no unwinding here and dead
// frames leave stale addresses behind -- but running them through addr2line
// names the functions that were on the way in.
#define CRASH_TRACE_MAX 24

static void write_stack_trace(uintptr_t sp) {
	uintptr_t low = 0, high = 0;
	uintptr_t code_begin = text_begin();
	uintptr_t code_end = (uintptr_t)etext;

	// Only within bounds that are known: reading past the end of a stack would
	// fault a second time, inside the handler, and the log would say nothing at
	// all -- which is the one failure this whole function exists to prevent.
	if (!code_begin || !code_end || code_begin >= code_end) {
		return;
	}
	if (!thread_stack_bounds(&low, &high) || sp < low || sp >= high) {
		return;
	}

	write_raw("*** on the stack, addr2line these:\n*** ");
	int shown = 0;
	for (uintptr_t at = sp; at + sizeof(uintptr_t) <= high && shown < CRASH_TRACE_MAX;
		 at += sizeof(uintptr_t)) {
		uintptr_t word;
		memcpy(&word, (const void *)at, sizeof(word));
		if (word < code_begin || word >= code_end) {
			continue;
		}
		write_hex(word);
		shown++;
		write_raw((shown % 6) == 0 ? "\n*** " : " ");
	}
	write_raw("\n");
}
#endif

// The three breadcrumbs, whichever of them has something to say. They cost
// nothing when empty, and together they are what says which file the player
// had in hand when it fell over.
static void write_crumbs(void) {
	static const struct {
		const char *label;
		const char *(*read)(void);
	} CRUMBS[] = {
		{"*** last file the player touched: ", crumb_get},
		{"*** track: ", crumb_track},
		{"*** decoding the artwork of: ", crumb_artwork},
	};

	for (size_t i = 0; i < sizeof(CRUMBS) / sizeof(CRUMBS[0]); i++) {
		const char *what = CRUMBS[i].read();
		if (what && what[0]) {
			write_raw(CRUMBS[i].label);
			write_raw(what);
			write_raw("\n");
		}
	}
}

// Runs on SIGSEGV and friends. Only async-signal-safe calls here: write(2) to
// the log, then park. Deliberately does not re-raise, because dying is what
// causes the reboot.
//
// With the program counter in the log,
//   mipsel-rockbox-linux-gnu-addr2line -f -e sonix_player 0x...
// names the function and the line.
static void crash_handler(int sig, siginfo_t *info, void *context) {
	const char *name = "signal";
	switch (sig) {
	case SIGSEGV:
		name = "SIGSEGV (bad memory access)";
		break;
	case SIGBUS:
		name = "SIGBUS (misaligned or bad address)";
		break;
	case SIGILL:
		name = "SIGILL (illegal instruction)";
		break;
	case SIGFPE:
		name = "SIGFPE (arithmetic error)";
		break;
	case SIGABRT:
		name = "SIGABRT (abort)";
		break;
	default:
		break;
	}

	write_raw("\n*** sonix_player crashed: ");
	write_raw(name);
	write_raw(" ***\n");

	if (info) {
		write_raw("*** fault address: ");
		write_hex((unsigned long)info->si_addr);
		write_raw("  code: ");
		write_hex((unsigned long)info->si_code);
		write_raw("\n");
	}

#ifdef CRASH_REGS
	if (context) {
		const ucontext_t *uc = (const ucontext_t *)context;
		uintptr_t sp = crash_sp(uc);

		write_raw("*** pc: ");
		write_hex(crash_pc(uc));
#if defined(__mips__)
		write_raw("  ra: ");
		write_hex(crash_reg(uc, 31));
#endif
		write_raw("\n*** addr2line -f -C -i -e sonix_player <pc> <ra>\n");

		// The stack first: it is the one question with a yes-or-no answer, and
		// it decides whether the registers below are even worth reading.
		write_stack_room(sp);
		write_registers(uc);
		write_stack_trace(sp);
	}
#else
	(void)context;
#endif

	write_crumbs();

	// Ask the scan to stop. Two stores to a bool, which is all this call is, so
	// it is safe from here -- and without it the scan thread keeps naming files
	// and the report ends up buried in the middle of the log.
	library_scan_stop();

	write_raw("*** staying alive so the device does not reboot; kill it over ADB to exit ***\n");

	// The log lives on the microSD and the card is never unmounted on the way
	// out, so a device whose power is pulled while frozen loses whatever the
	// kernel had not written back. fsync + sync put the crash block on the
	// card before anything else happens. Both are async-signal-safe.
	fsync(STDERR_FILENO);
	sync();

	// The charger back on. The player is about to sit here doing nothing, and
	// if it went down holding the charge limit the battery would never fill
	// again -- the charger driver keeps that bit until the next boot. One write
	// to one sysfs node, which is safe enough to do from here.
	power_charging_release();

	for (;;) {
		pause();
	}
}

#ifndef HOST_BUILD
// Called instead of returning from main() when something has gone wrong.
static void park(const char *reason) {
	fprintf(stderr, "*** %s\n", reason);
	fprintf(stderr, "*** staying alive so the device does not reboot; kill it over ADB to exit\n");
	fflush(stderr);

	while (running) {
		sleep(3600);
	}
}
#endif

#ifndef HOST_BUILD
// A couple of lines of process and system memory state. The same ones the
// watchdog prints when the interface locks up, but said once at boot so two
// boots can be compared.
static void report_memory(const char *when) {
	// For the process: VmSize is how much is mapped, VmRSS how much is really
	// in RAM, VmLck how much is pinned there and cannot be freed. VmLck is the
	// line that counts: it should be the code and nothing else.
	FILE *st = fopen("/proc/self/status", "r");
	if (st) {
		char line[128];
		while (fgets(line, sizeof(line), st)) {
			if (strncmp(line, "VmSize:", 7) == 0 || strncmp(line, "VmRSS:", 6) == 0 ||
				strncmp(line, "VmLck:", 6) == 0) {
				fprintf(stderr, "memory (%s): %s", when, line);
			}
		}
		fclose(st);
	}

	FILE *mi = fopen("/proc/meminfo", "r");
	if (mi) {
		char line[128];
		while (fgets(line, sizeof(line), mi)) {
			if (strncmp(line, "MemTotal:", 9) == 0 || strncmp(line, "MemFree:", 8) == 0 ||
				strncmp(line, "MemAvailable:", 13) == 0) {
				fprintf(stderr, "memory (%s): %s", when, line);
			}
		}
		fclose(mi);
	}
}


// Pins the executable pages -- the player's and the libraries' -- in RAM, and
// nothing else.
//
// Under memory pressure the kernel evicts clean code pages; the playback
// thread then page-faults in the middle of a FLAC frame with the card busy on
// a cover, stalls for seconds, and the starved audio stream dies with EIO. A
// fault in an icon only delays a redraw, so everything that is not code is
// left to the kernel: mlockall(MCL_CURRENT) would hold the untouched .bss and
// megabytes of icons in RAM at full price, on a machine with fifty-five.
//
// "Holds code" means executable AND NOT WRITABLE. The x alone is not enough:
// an ELF carrying no PT_GNU_STACK segment, or one marked executable, gets
// READ_IMPLIES_EXEC, and from then on the data segment, the .bss, the heap and
// the stack all come out of /proc/self/maps as "rwxp". Code is never writable,
// so the test is "r-x". The writable-and-executable bytes are counted and
// reported rather than silently dropped, because their being anything other
// than zero is what says this process runs under that flag.
//
// The map cannot tell code from read-only data either: this binary uses the
// classic two-segment layout, and the R E LOAD carries
//
//     .init .text .MIPS.stubs .fini .rodata .eh_frame_hdr .eh_frame
//
// in a single mapping. The linker places `etext` between .fini and .rodata, so
// the one mapping that holds this very function is locked only as far as
// etext and the read-only tail behind it is left to the kernel. Every other
// mapping, libraries included, is pinned whole: reading the marker wrong must
// cost protection, never correctness.
//
// If /proc/self/maps cannot be read, mlockall(MCL_CURRENT) is the fallback --
// pinning too much costs memory, never correctness.

// Provided by the linker: the first address past the code.
extern char etext[];

// Measured on the V3 Blaze, the pinning reads some 9 MB of code off the
// squashfs: a quarter of a second spent before the first frame. So it is done
// in two steps. lock_code_plan() reads the map where the pinning used to
// happen, which costs nothing and keeps the same set of mappings -- not the
// libraries opened later, OpenSSL and libjpeg among them. lock_code_start()
// then pins that set on a worker below the interface once the main loop runs,
// a quarter megabyte per call: mlock faults its range in under the address
// space lock, and in small pieces a page fault of the interface or of the
// playback thread never waits behind a whole library.
#define LOCK_RANGES_MAX 64
#define LOCK_CHUNK (256u * 1024u)

static struct {
	unsigned long start, stop;
} lock_ranges[LOCK_RANGES_MAX];
static int lock_range_count;
static bool lock_use_mlockall;
static unsigned long lock_writable, lock_readonly;

static void lock_code_plan(void) {
	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps) {
		lock_use_mlockall = true;
		return;
	}

	long page_size = sysconf(_SC_PAGESIZE);
	unsigned long page = page_size > 0 ? (unsigned long)page_size : 4096u;
	unsigned long code_end = (unsigned long)etext;
	unsigned long here = (unsigned long)&lock_code_plan;

	char line[256];
	while (fgets(line, sizeof(line), maps)) {
		unsigned long start = 0;
		unsigned long end = 0;
		char perms[8] = "";
		if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3 || end <= start) {
			continue;
		}
		if (perms[2] != 'x') {
			continue;
		}
		// Executable and writable: data wearing the x this process gives to
		// everything readable. See the note above.
		if (perms[1] == 'w') {
			lock_writable += end - start;
			continue;
		}

		unsigned long stop = end;
		// The player's own mapping, and the marker inside it: everything past
		// etext is .rodata and the unwind tables.
		if (here >= start && here < end && code_end > start && code_end < end) {
			stop = (code_end + page - 1) & ~(page - 1);
			if (stop > end) {
				stop = end;
			}
			lock_readonly += end - stop;
		}

		if (lock_range_count < LOCK_RANGES_MAX) {
			lock_ranges[lock_range_count].start = start;
			lock_ranges[lock_range_count].stop = stop;
			lock_range_count++;
		} else {
			fprintf(stderr, "memory: more than %d code mappings, %lx-%lx left unlocked\n", LOCK_RANGES_MAX, start,
					stop);
		}
	}
	fclose(maps);
}

static void *lock_code_thread(void *arg) {
	(void)arg;
	// Not idle class: preempted for long while holding the address space
	// lock, it would leave every page fault queued behind it.
	thread_be_low_priority("code lock");

	if (lock_use_mlockall) {
		if (mlockall(MCL_CURRENT) != 0) {
			perror("mlockall");
		}
		return NULL;
	}

	unsigned long locked = 0;
	int regions = 0;
	int refused = 0;
	for (int i = 0; i < lock_range_count; i++) {
		bool ok = true;
		for (unsigned long at = lock_ranges[i].start; at < lock_ranges[i].stop; at += LOCK_CHUNK) {
			unsigned long len = lock_ranges[i].stop - at;
			if (len > LOCK_CHUNK) {
				len = LOCK_CHUNK;
			}
			if (mlock((void *)at, (size_t)len) == 0) {
				locked += len;
			} else {
				ok = false;
			}
		}
		// The kernel keeps a few mappings of its own that cannot be locked;
		// they are not the player's code.
		if (ok) {
			regions++;
		} else {
			refused++;
		}
	}

	fprintf(stderr,
			"memory: %d code regions locked in RAM (%lu kB), %d refused, "
			"left %lu kB writable and %lu kB read-only\n",
			regions, locked / 1024, refused, lock_writable / 1024, lock_readonly / 1024);
	return NULL;
}

static void lock_code_start(void) {
	pthread_t thread;
	if (pthread_create(&thread, NULL, lock_code_thread, NULL) == 0) {
		pthread_detach(thread);
	} else {
		fprintf(stderr, "memory: no thread to lock the code pages; left unlocked\n");
	}
}

// Tells the kernel who to kill when memory runs out. The player is the last
// thing that should go, because killing it reboots the device. The flip side
// of being this protected is that the player must never ask for more memory
// than the device can give -- see the decode budget in cover.c, which keeps a
// big cover from turning this protection into a thrashing kernel.
static void set_oom_priority(const char *value) {
	FILE *f = fopen("/proc/self/oom_score_adj", "w");
	if (!f) {
		return;
	}
	fputs(value, f);
	fclose(f);
}
#endif

// ---------------------------------------------------------------------------
// the interface watchdog
//
// If the interface thread ever stops turning the event loop, the device looks
// frozen and the log says nothing. This thread watches a heartbeat the main
// loop increments; when it stalls for a few seconds, every thread's name,
// state and kernel wait channel goes to the log, which names the stuck thread
// and the exact kernel call it is stuck in.
// ---------------------------------------------------------------------------

static volatile uint32_t ui_heartbeat;

#define WATCHDOG_STALL_SECONDS 5

// And how long a play request may sit unclaimed before the same dump is taken
// for the playback thread. Generous: opening a device can legitimately take a
// couple of seconds against a card that is still tearing the last stream down,
// and a track that is starting has already cleared the request by then.
#define WATCHDOG_PLAY_STALL_MS 10000

static void watchdog_dump_threads(void) {
	// Memory first: a stall with MemAvailable on the floor is the kernel
	// thrashing, not a bug in any one thread. Dirty and Writeback are pages
	// waiting to reach the card, so when those are most of what is missing the
	// card is the bottleneck and nobody has allocated anything. VmRSS answers
	// the other half -- whether the player itself has grown.
	FILE *mi = fopen("/proc/meminfo", "r");
	if (mi) {
		char line[128];
		while (fgets(line, sizeof(line), mi)) {
			if (strncmp(line, "MemFree:", 8) == 0 || strncmp(line, "MemAvailable:", 13) == 0 ||
				strncmp(line, "Dirty:", 6) == 0 || strncmp(line, "Writeback:", 10) == 0) {
				fprintf(stderr, "watchdog:   %s", line);
			}
		}
		fclose(mi);
	}

	FILE *st = fopen("/proc/self/status", "r");
	if (st) {
		char line[128];
		while (fgets(line, sizeof(line), st)) {
			if (strncmp(line, "VmRSS:", 6) == 0 || strncmp(line, "Threads:", 8) == 0) {
				fprintf(stderr, "watchdog:   %s", line);
			}
		}
		fclose(st);
	}

	const char *doing = crumb_get();
	if (doing && doing[0]) {
		fprintf(stderr, "watchdog:   last file: %s\n", doing);
	}
	if (crumb_track()[0]) {
		fprintf(stderr, "watchdog:   track: %s\n", crumb_track());
	}
	if (crumb_artwork()[0]) {
		fprintf(stderr, "watchdog:   decoding artwork of: %s\n", crumb_artwork());
	}

	DIR *tasks = opendir("/proc/self/task");
	if (!tasks) {
		return;
	}

	struct dirent *de;
	while ((de = readdir(tasks)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}

		char path[96], comm[32] = "?", state = '?', wchan[128] = "?";

		// Task ids are small integers, so the explicit precision only bounds a
		// case that cannot happen, and keeps the compiler quiet about it.
		snprintf(path, sizeof(path), "/proc/self/task/%.32s/comm", de->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (fgets(comm, sizeof(comm), f)) {
				comm[strcspn(comm, "\n")] = '\0';
			}
			fclose(f);
		}

		snprintf(path, sizeof(path), "/proc/self/task/%.32s/stat", de->d_name);
		f = fopen(path, "r");
		if (f) {
			// pid (comm) state ... -- the comm can hold spaces, so find the
			// closing parenthesis rather than trusting scanf.
			char line[256];
			if (fgets(line, sizeof(line), f)) {
				char *close = strrchr(line, ')');
				if (close && close[1] == ' ') {
					state = close[2];
				}
			}
			fclose(f);
		}

		snprintf(path, sizeof(path), "/proc/self/task/%.32s/wchan", de->d_name);
		f = fopen(path, "r");
		if (f) {
			size_t n = fread(wchan, 1, sizeof(wchan) - 1, f);
			wchan[n] = '\0';
			fclose(f);
		}

		fprintf(stderr, "watchdog:   tid %s (%s) state=%c wchan=%s\n", de->d_name, comm, state, wchan);
	}
	closedir(tasks);
	fflush(stderr);
}

static void *watchdog_thread(void *arg) {
	(void)arg;

	// Above the playback thread, which runs SCHED_RR at 10.
	//
	// The whole point of this thread is to report a device that has stopped
	// answering, and the way it stops answering on one core is a real-time
	// thread that never blocks. At normal priority the watchdog is exactly as
	// starved as the interface it is meant to report on -- which is why a freeze
	// can leave a log with nothing in it at all. It sleeps a second at a time
	// and then reads a few files, so it cannot take anything from the audio.
	thread_be_realtime("watchdog", 20);

	uint32_t last = ui_heartbeat;
	unsigned last_turns = audio_loop_turns();
	int stalled_for = 0;
	bool dumped = false;

	// The other thread worth watching. A stalled interface is obvious to the
	// person holding the device; a stalled playback thread is not -- the
	// interface answers, the track is logged as requested, and no sound comes
	// out, from any output. wchan in the dump names the syscall it went into
	// and did not come back from.
	bool play_dumped = false;

	for (;;) {
		sleep(1);

		// Above every other real-time thread, so this still runs while one of
		// them holds the core.
		logging_follow_kernel();
		logging_sync();

		unsigned turns = audio_loop_turns();
		unsigned turns_this_second = turns - last_turns;
		last_turns = turns;

		long waiting_ms = audio_play_request_age_ms();
		if (waiting_ms >= WATCHDOG_PLAY_STALL_MS) {
			if (!play_dumped) {
				play_dumped = true;
				fprintf(stderr, "watchdog: the playback thread has not taken a play request for %ld ms -- thread dump:\n",
						waiting_ms);
				watchdog_dump_threads();
				logging_sync();
			}
		} else {
			play_dumped = false;
		}

		uint32_t now = ui_heartbeat;
		if (now != last) {
			if (dumped) {
				fprintf(stderr, "watchdog: UI recovered after ~%d s\n", stalled_for);
			}
			last = now;
			stalled_for = 0;
			dumped = false;
			continue;
		}

		stalled_for++;
		if (stalled_for >= WATCHDOG_STALL_SECONDS && !dumped) {
			fprintf(stderr, "watchdog: UI thread has not run for %d s (playback loop %u turns in the last second) "
							"-- thread dump:\n",
					stalled_for, turns_this_second);
			watchdog_dump_threads();
			logging_sync();
			dumped = true; // once per stall, not once per second
		}
	}

	return NULL;
}

static void watchdog_start(void) {
	pthread_t thread;
	if (pthread_create(&thread, NULL, watchdog_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

// Runs on SIGTERM and the other "stop now" signals.
//
// These are not faults, so the crash handler above never sees them. The
// interesting part is not the signal but the sender: SA_SIGINFO carries the pid
// of whoever asked, so a supervisor script killing the player and the user
// holding the power button can be told apart afterwards.
//
// It reports and then dies the way it would have died anyway: default action,
// re-raise. Nothing about the shutdown changes.
static void terminate_handler(int sig, siginfo_t *info, void *context) {
	(void)context;

	const char *name = "signal";
	switch (sig) {
	case SIGTERM:
		name = "SIGTERM (asked to stop)";
		break;
	case SIGINT:
		name = "SIGINT";
		break;
	case SIGQUIT:
		name = "SIGQUIT";
		break;
	case SIGHUP:
		name = "SIGHUP";
		break;
	default:
		break;
	}

	write_raw("\n*** sonix_player is being stopped: ");
	write_raw(name);
	write_raw(" ***\n");

	// si_pid is 0 when the kernel sent it rather than a process.
	if (info) {
		write_raw("*** sent by pid ");
		write_dec((unsigned long)info->si_pid);
		write_raw(" uid ");
		write_dec((unsigned long)info->si_uid);
		write_raw("\n");
	}

	write_crumbs();

	library_scan_stop();

	fsync(STDERR_FILENO);
	sync();

	signal(sig, SIG_DFL);
	raise(sig);
}

static void install_signal_guards(void) {
	// A closed socket or pipe must never be fatal: a failed sys_server
	// handshake would otherwise be a boot loop.
	signal(SIGPIPE, SIG_IGN);

	// A stack overflow raises SIGSEGV with no room left to run the handler in,
	// and the handler then faults again and the process dies with nothing in
	// the log. SA_ONSTACK sends it to the separate stack instead; the kernel
	// ignores the flag on a thread that has none.
	thread_signal_stack();

	// sigaction, not signal(): SA_SIGINFO is what carries the faulting
	// address and the register state the log needs.
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);

	// The other way a process ends: someone asks it to. SIGKILL is missing
	// from this list because it cannot be caught -- what catches that one is
	// the kernel's own log, read back at the next start (see
	// logging_report_previous_run).
	struct sigaction term;
	memset(&term, 0, sizeof(term));
	term.sa_sigaction = terminate_handler;
	term.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&term.sa_mask);

	sigaction(SIGTERM, &term, NULL);
	sigaction(SIGINT, &term, NULL);
	sigaction(SIGQUIT, &term, NULL);
	sigaction(SIGHUP, &term, NULL);
}

#ifdef HOST_BUILD
#include <SDL2/SDL.h>

#define GET_MUSIC_DIR() (snprintf((char[256]){0}, 256, "%s/Music", getenv("HOME") ? getenv("HOME") : ""))
#else
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

// The longest the main loop will ever sleep between passes of
// lv_timer_handler(). See the loop itself for why this cap exists.
#define MAIN_LOOP_MAX_SLEEP_MS 250

// ---------------------------------------------------------------------------
// Kinetic scroll tuning.
//
// LVGL has kinetic scrolling of its own -- the list glides and brakes when the
// finger lifts -- but with the factory values it is barely noticeable on this
// screen. Two knobs, both per-indev:
//
//   * scroll_throw: the percentage the velocity drops on each frame of the
//     throw animation. The default 10 kills the glide in half a second; at 5
//     it lasts about twice as long and travels about twice as far.
//   * scroll_limit: how many pixels the finger must move before LVGL calls it
//     a scroll rather than a tap. The default 10 is a lot on a 480x720 panel
//     and the scroll starts late; at 4 the list latches on immediately.
//
// Applied to both the device touch panel and the simulator mouse, so the two
// feel the same.
// ---------------------------------------------------------------------------

#define SCROLL_THROW_DECAY_PCT 5
#define SCROLL_START_LIMIT_PX 4

// ---------------------------------------------------------------------------
// A drag is not a tap.
// ---------------------------------------------------------------------------
//
// LVGL drops the click only when a scrollable container claimed the gesture.
// This drops it for every other drag too: an indev event callback remembers
// where the finger landed and, when SHORT_CLICKED or CLICKED arrives more than
// TAP_SLOP_PX away from there, lv_indev_stop_processing() keeps the object from
// receiving it. RELEASED still arrives, for the handlers that drive a drag.
//
// The distance is from the press point to the release point; the indev
// callback is not offered PRESSING, so the path in between is not seen.
#define TAP_SLOP_PX SC(12)

static void tap_guard_cb(lv_event_t *e) {
	lv_indev_t *indev = lv_indev_active();
	if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
		return;
	}

	// One point for all pointers: there is only ever one pointer pressed.
	static lv_point_t press_at;

	lv_point_t p;
	lv_indev_get_point(indev, &p);

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		press_at = p;
		return;
	}
	if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) {
		return;
	}

	int32_t dx = p.x - press_at.x;
	int32_t dy = p.y - press_at.y;
	if (dx * dx + dy * dy > TAP_SLOP_PX * TAP_SLOP_PX) {
		lv_indev_stop_processing(indev);
	}
}

static void tune_kinetic_scroll(lv_indev_t *indev) {
	if (!indev) {
		return;
	}
	lv_indev_set_scroll_throw(indev, SCROLL_THROW_DECAY_PCT);
	lv_indev_set_scroll_limit(indev, SCROLL_START_LIMIT_PX);
}

// Kinetic scrolling and the tap guard, for the touch panel and the simulator mouse.
static void tune_pointer(lv_indev_t *indev) {
	if (!indev) {
		return;
	}
	tune_kinetic_scroll(indev);
	lv_indev_add_event_cb(indev, tap_guard_cb, LV_EVENT_ALL, NULL);
}

// The panel of the player this binary was first written for, and what an
// unrecognised device-name falls back to.
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 720

// The smallest panel the pages can be laid out on. Comfortably under either
// real one: this is not a size anybody should reach, it is the floor under
// which the arithmetic in the pages stops meaning anything.
#define PANEL_MIN_WIDTH 320
#define PANEL_MIN_HEIGHT 480

// The panel size for gui_config_t, in this order: SONIX_PANEL ("480x800"); the
// model named in system-info.json; the model whose panel exactly matches the
// framebuffer (fb_w/fb_h, 0 where there is none); SCREEN_WIDTH x SCREEN_HEIGHT.
// A framebuffer size that matches no model is never used: the pages subtract
// from these unsigned numbers, and an unexpected size wraps them.
static void panel_size(int *width, int *height, int fb_w, int fb_h) {
	const sysinfo_model_t *model = sysinfo_model();
	if (!model && fb_w > 0 && fb_h > 0) {
		model = sysinfo_model_by_panel(fb_w, fb_h);
		if (model) {
			printf("panel: system-info.json names no known model; the framebuffer is %dx%d, the %s's panel\n",
				   fb_w, fb_h, model->name);
		}
	}
	*width = model ? model->panel_width : SCREEN_WIDTH;
	*height = model ? model->panel_height : SCREEN_HEIGHT;

	const char *env = getenv("SONIX_PANEL");
	int w = 0, h = 0;
	if (env && sscanf(env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
		*width = w;
		*height = h;
	}

	// Whatever the answer came from, it has to be a panel this interface can be
	// laid out on. The pages subtract a title row and a bar from the height in
	// unsigned arithmetic, so anything shorter than that does not crowd the
	// layout, it wraps: the fallback is the only safe answer.
	if (*width < PANEL_MIN_WIDTH || *height < PANEL_MIN_HEIGHT) {
		printf("panel: %dx%d is too small to lay out, using %dx%d\n", *width, *height, SCREEN_WIDTH, SCREEN_HEIGHT);
		*width = SCREEN_WIDTH;
		*height = SCREEN_HEIGHT;
	}
}

static uint32_t custom_tick_get(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void post_gui_popup(const char *message, void *user_data) {
	(void)user_data;
	gui_notify_popup(message);
}

#ifdef HOST_BUILD
static lv_display_t *host_display;

// The keyboard stands in for the buttons on the case.
//
// On the R3 Pro II the two skip keys arrive from the kernel swapped --
// NEXTSONG is the upper button and PREVIOUSSONG the lower one (see
// keymap_button_for_code) -- so the codes below are crossed to match what that
// device really sends. On the R1, whose one skip key sends NEXTSONG, B is that
// key and M stands for a previous key the R1 does not have.
static const struct {
	SDL_Keycode key;
	int code;
} HOST_BUTTONS[] = {
	{SDLK_p, KEY_POWER},		{SDLK_u, KEY_VOLUMEUP},		{SDLK_i, KEY_VOLUMEDOWN},
	{SDLK_b, KEY_NEXTSONG},		{SDLK_n, KEY_PLAYPAUSE},	{SDLK_m, KEY_PREVIOUSSONG},
};

static int host_key_watch(void *userdata, SDL_Event *event) {
	(void)userdata;
	if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP) {
		return 1;
	}
	if (event->key.repeat) {
		return 1;
	}
	for (size_t i = 0; i < sizeof(HOST_BUTTONS) / sizeof(HOST_BUTTONS[0]); i++) {
		if (HOST_BUTTONS[i].key == event->key.keysym.sym) {
			input_host_button(HOST_BUTTONS[i].code, event->type == SDL_KEYDOWN);
			break;
		}
	}
	return 1;
}

static lv_display_t *init_host_display(void) {
	int width = 0, height = 0;
	panel_size(&width, &height, 0, 0); // the window is made from it: nothing to ask

	lv_display_t *disp = lv_sdl_window_create(width, height);
	if (!disp) {
		fprintf(stderr, "Error: Failed to create SDL2 window\n");
		return NULL;
	}

	// Non-resizeable, which also makes tiling window managers float it.
	lv_sdl_window_set_resizeable(disp, false);

	// Flushing the backbuffers once here avoids the initial viewport shift.
	SDL_Renderer *renderer = (SDL_Renderer *)lv_sdl_window_get_renderer(disp);
	if (renderer) {
		SDL_RenderClear(renderer);	 // Clear backbuffer state
		SDL_RenderPresent(renderer); // Flush context out to Wayland surface
		SDL_Delay(50);				 // Brief pause to allow the compositor coordinates to settle
	}

	lv_indev_t *mouse = lv_sdl_mouse_create();
	if (mouse) {
		lv_indev_set_display(mouse, disp);
		tune_pointer(mouse);
	}

	lv_indev_t *scripted = lv_indev_create();
	lv_indev_set_type(scripted, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(scripted, host_tap_read_cb);
	lv_indev_set_display(scripted, disp);
	tune_pointer(scripted);

	host_display = disp;

	lv_indev_t *kbd = lv_sdl_keyboard_create();
	if (kbd) {
		lv_indev_set_display(kbd, disp);
	}

	SDL_AddEventWatch(host_key_watch, NULL);
	fprintf(stderr, "input: keyboard buttons -- p power, u vol+, i vol-, b prev, n play/pause, m next\n");

	return disp;
}

// No panel to re-arm on the host: the SDL window never blanks.
void display_wake_begin(lv_display_t *disp) { (void)disp; }
void display_wake_end(lv_display_t *disp) { (void)disp; }

// Nothing to disarm either: power.c calls this after a blank it does not
// perform here. It has no host counterpart otherwise, which is what the link
// error without this line was.
void display_panel_disarmed(void) {}

// The folder that stands in for the memory card: SONIX_SD_ROOT, else the
// documents folder named by XDG, else ~/Documents or ~/Documenti.
static void host_card_root(char *out, size_t size) {
	const char *forced = getenv("SONIX_SD_ROOT");
	if (forced && forced[0]) {
		snprintf(out, size, "%s", forced);
		return;
	}

	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		home = ".";
	}

	char conf[1024];
	snprintf(conf, sizeof(conf), "%s/.config/user-dirs.dirs", home);
	FILE *f = fopen(conf, "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			const char *eq = strchr(line, '=');
			if (!eq || strncmp(line, "XDG_DOCUMENTS_DIR", 17) != 0) {
				continue;
			}
			char value[1024];
			snprintf(value, sizeof(value), "%s", eq + 1);
			value[strcspn(value, "\r\n")] = '\0';

			char *v = value;
			size_t len = strlen(v);
			if (len >= 2 && v[0] == '"' && v[len - 1] == '"') {
				v[len - 1] = '\0';
				v++;
			}
			if (strncmp(v, "$HOME", 5) == 0) {
				snprintf(out, size, "%s%s", home, v + 5);
			} else {
				snprintf(out, size, "%s", v);
			}
			fclose(f);
			if (out[0]) {
				return;
			}
			f = NULL;
			break;
		}
		if (f) {
			fclose(f);
		}
	}

	snprintf(out, size, "%s/Documents", home);
	if (access(out, F_OK) == 0) {
		return;
	}
	char italian[1024];
	snprintf(italian, sizeof(italian), "%s/Documenti", home);
	if (access(italian, F_OK) == 0) {
		snprintf(out, size, "%s", italian);
	}
}

// The simulator draws through SDL, which owns its own texture: there is no
// framebuffer page to rotate into. The setting is still remembered so the
// switch behaves the same way, it just has nothing to turn upside down.
static bool host_rotated;
bool display_rotation_supported(void) { return false; }
bool display_get_rotated(void) { return host_rotated; }
void display_set_rotated(bool rotated) { host_rotated = rotated; }

// The touch panel on the simulator is the mouse inside the SDL window: there
// is no evdev node to hand over, and nothing to switch off.
void panel_touch_enable(bool enabled) { (void)enabled; }
const char *panel_touch_device(void) { return NULL; }

// There is no framebuffer to read on the simulator, but the SDL renderer can
// be read back. The rest of the path is identical (same copy, same flash, same
// PNG writer); only the source of the pixels differs.
uint16_t *display_capture_frame(int *out_w, int *out_h) {
	SDL_Renderer *renderer = host_display ? (SDL_Renderer *)lv_sdl_window_get_renderer(host_display) : NULL;
	if (!renderer) {
		return NULL;
	}

	int w = 0, h = 0;
	if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0 || w <= 0 || h <= 0) {
		return NULL;
	}

	// A single reused block, the same rule as the framebuffer version below,
	// so callers behave identically in both builds.
	static uint16_t *copy;
	static size_t copy_size;
	size_t want = (size_t)w * (size_t)h * sizeof(uint16_t);
	if (!copy || copy_size != want) {
		free(copy);
		copy = malloc(want);
		copy_size = copy ? want : 0;
	}
	if (!copy) {
		return NULL;
	}
	if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB565, copy, w * (int)sizeof(uint16_t)) != 0) {
		fprintf(stderr, "fb: SDL_RenderReadPixels: %s\n", SDL_GetError());
		return NULL;
	}

	if (out_w) {
		*out_w = w;
	}
	if (out_h) {
		*out_h = h;
	}
	return copy;
}
#else
// ---------------------------------------------------------------------------
// Double-buffered framebuffer with page flipping.
//
// LVGL's stock fbdev driver memcpy()s every dirty area straight into the one
// visible buffer, so the panel scans out a half-updated frame and the
// interface tears whenever it moves. Here LVGL draws directly into the
// off-screen half of a double-height framebuffer, refr_sync_areas() keeps the
// two pages coherent, and the flush of the last area pans with
// FBIOPAN_DISPLAY, which the display driver latches on vsync. A framebuffer
// that cannot give two pages falls back to LVGL's plain driver.
// ---------------------------------------------------------------------------

static int fb_fd = -1;
static uint8_t *fb_mem;
static size_t fb_page_size;
static size_t fb_map_size;
static struct fb_var_screeninfo fb_var;

// How wide one framebuffer pixel is, and how the panel is driven. A 16-bit
// RGB565 page is what the Blaze and the R3 Pro II scan; the V3 Analog wants 32
// bits in BGRA order with exactly one page of memory, so it is single buffered
// and every pixel has to be rewritten on the way out. The bits-per-pixel and
// the page count are kept here because both the flush callback and the blit
// branch on them, and they are only read after try_panned_display() sets them.
static int fb_bytespp = 2;
static bool fb_single_page;

// Set when a pan was refused with EINVAL, which is what a blank or unblank does
// to the mode underneath us: the next frame reads the screen info again before
// it decides which page to draw into, so the copy and the pan agree with what
// the driver now holds. See the flip thread.
static bool fb_mode_stale;

// The flip itself, on a thread of its own. FBIOPAN_DISPLAY blocks until the
// panel takes the new page, 10 to 16 ms on the Blaze, and on the interface
// thread that wait came on top of every frame. Where LVGL draws into a buffer
// of its own and the flush only copies into the panel page -- shrunk or turned
// -- the interface can draw the next frame while the last one is being
// flipped; the copy of the next frame waits for that flip to be done, since it
// writes into the page going off screen. Where LVGL draws straight into the
// panel pages the flip stays where it was, inline.
static pthread_mutex_t flip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flip_cond = PTHREAD_COND_INITIALIZER;
static bool flip_pending, flip_thread_on;
static struct fb_var_screeninfo flip_var;

// The frame log (see the ft block further down, with the framebuffer code) is
// fed from here as well: the flip thread is the one that pans, so it is the one
// that can say where the panel's own frame grid is.
static uint64_t ft_now_us(void);
static void pace_note(uint64_t glass_us);

// A pan has been asked for since the last copy: the panel is about to take the
// frame, and the page it is taking it from is the one the next copy wants.
static bool pan_outstanding;

// ---------------------------------------------------------------------------
// the panel's own transfer, read from the LCDC's registers
// ---------------------------------------------------------------------------
//
// A frame reaches this panel by DMA: FBIOPAN_DISPLAY starts it, and the panel
// then reads the whole framebuffer page. Measured on the device (tools/panwatch.c),
// the transfer begins at the panel's next frame boundary -- 1.7 to 10 ms after
// the ioctl -- and BUSY in SLCDC_STATE is set for 12.3 ms of the panel's 13.2 ms
// frame. With one page that read and this copy are the same memory, so the copy
// has to wait for it: without the wait the panel takes the old picture for the
// first part of the frame and the new one for the rest, which is the tearing
// seen on every scroll and every quick-panel slide.
//
// The registers are mapped the way tools/fbtool.c maps them for diagnosis, and
// the bits are the ones read out of the vendor driver (NOTES.md): SLCDC_CTRL
// (+0xa4) bit0 is DMA_EN, SLCDC_STATE (+0xa8) bit0 is BUSY. A board without
// /dev/mem, or a panel that never reports anything, simply gets no wait -- the
// copy is then what it was before, which is the behaviour this replaces.
#define LCDC_BASE 0x13050000u
#define SLCDC_STATE_OFF 0xa8u

static volatile uint32_t *lcdc_regs;

// Where the panel's own frame grid was, as of the last transfer it was given,
// and when that pan was issued. Written by the flip thread (the one that pans),
// read by the interface thread for the log and for the wait; both are only ever
// compared with each other, so a torn read would be a wrong deadline and
// nothing else.
static uint64_t panel_rise_us;
static uint64_t fb_pan_us;

// The panel's own frame, measured on this unit from its TE line (gpio-51, the
// slcd_vsync interrupt): 758 ticks in 10.03 s -- 75.57 Hz, 13232 us. It is not
// the 75.0 Hz the pixel clock suggests, and it is the grid the panel starts a
// transfer on: a frame handed to it is read on the next boundary of that grid,
// never in between (tools/panwatch.c shows the transfer beginning 1.7 to 13 ms
// after the pan, never at the pan itself).
#define PANEL_FRAME_US 13232u

// How long the panel reads the page for once it has begun: measured 12.29 ms
// (tools/panwatch.c). The margin is for a frame that runs slower than this
// unit's.
#define PAN_TRANSFER_US 12290u
#define PAN_TRANSFER_MARGIN_US 1500u

// The pan's cadence, in panel frames. Two of them (26.46 ms, 37.8 frames a
// second) is the fastest the panel can be handed frames -- its transfer takes
// 12.29 ms of its 13.23 ms frame, so a frame every panel frame leaves the copy
// nothing. Three (39.7 ms, 25.2 a second) leaves the frame a lot of slack, and
// with the work measured at 21-23 ms either way, a spike in the drawing has
// nowhere to show. Which picture is the better one is a matter of taste:
// [screen] cadence_frames in device_config.ini, read at startup.
//
// What is *not* on offer is anything in between: the glass can only be handed a
// frame on the panel's own grid, so a cadence that is not a whole number of
// panel frames (30 frames a second is 2.52 of them) makes the glass take 26.4
// and 39.7 ms alternately -- the judder this whole path exists to remove.
static uint32_t pan_cadence_frames = 2;

// How long before the panel's own boundary the pan is issued. The ioctl itself
// is 20 us (tools/panwatch.c), so most of this is the flip thread's own
// wake-up. Its point is that the transfer then begins at *that* boundary
// instead of the next one, which is what leaves the copy the rest of the slot.
#define PAN_GUARD_US 2000u

// Frame timing, for the log: how long LVGL took to draw a frame, the copy onto
// the panel page and the page flip. Summed over a second and printed for every
// second that had frames in it, so an idle player says nothing and an animation
// says where the time went.
//
// The waits are counted apart, because they are different things (see
// panel_wait_transfer): waiting for the flip thread to have panned, waiting for
// the panel's transfer to begin (the interface's phase against the glass), and
// waiting for that transfer to end before the copy may write the page it reads.
// `over` counts the frames where the transfer was already finished when the
// wait looked -- the wait then costs nothing, and it is the number to watch: a
// wait that misses this case burns its deadline on a frame that is long on the
// glass.
//
// `glass` is the one line that is about the picture rather than about our own
// work: the instants the panel began reading a frame, and the gaps between
// them. Frames in a counter say nothing about judder; the spread of those gaps
// is what a hand sees. It is fed from the flip thread (pace_note).
// The work a frame costs the interface thread: drawing it, waiting for the
// panel's transfer to begin, and the copy phase (the copy and the waits for the
// panel to have read the rows it writes). The wait for the flip thread to have
// panned is *not* part of it -- that is the slot's own slack, the thread sitting
// still until the panel's grid comes round, and counting it as work would call
// every frame slow.
static uint32_t frame_slow_us(void) {
	return pan_cadence_frames * PANEL_FRAME_US;
}

// A gap longer than this is the interface sitting idle (nothing on the screen
// changed), not the picture's cadence, and is left out of the statistics.
#define PACE_IDLE_US 200000

static struct {
	uint64_t refr_start_us, second_start_us;
	int frames, slow;
	uint64_t copied_px;              // layout pixels shrunk onto the panel, summed
	uint32_t render_max, render_sum; // the drawing
	uint32_t snap_max, snap_sum;     // copying the damage into the snapshot page
	uint32_t hold_max, hold_sum;     // waiting for the flip thread to be done with it
	uint32_t rise_max, rise_sum;     // waiting for the panel's transfer to begin
	uint32_t blit_max, blit_sum;     // the copy phase (the copy and its row waits)
	uint32_t pace_max, pace_sum;     // the flip thread waiting for the panel's grid
	uint32_t work_max, work_sum;     // what the slot has to hold (see ft_frame_done)
	int rise_over;                   // frames whose transfer was already finished
	int dropped;                     // frames the flip thread had no slot left for
	uint64_t glass_prev_us;          // the last instant the glass was handed a frame
	uint64_t pace_gap_sum, pace_gap_sq, pace_gap_min, pace_gap_max;
	int pace_gap_n;
	// The frame being reported, as the snapshot measured it.
	uint32_t snap_last_us, hold_last_us;
} ft;

static uint64_t ft_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// One more gap between the instants the panel began reading a frame. Called
// from the flip thread; the statistics are read (and cleared) from the
// interface thread's log line, so a number can be a frame stale. That is fine
// for a log, and the alternative -- a lock around a printf's worth of counters
// -- would put the flip thread behind the interface thread.
static void pace_note(uint64_t glass_us) {
	if (ft.glass_prev_us && glass_us > ft.glass_prev_us) {
		uint64_t gap = glass_us - ft.glass_prev_us;
		if (gap < PACE_IDLE_US) {
			ft.pace_gap_n++;
			ft.pace_gap_sum += gap;
			ft.pace_gap_sq += gap * gap;
			if (!ft.pace_gap_min || gap < ft.pace_gap_min) {
				ft.pace_gap_min = gap;
			}
			if (gap > ft.pace_gap_max) {
				ft.pace_gap_max = gap;
			}
		}
	}
	ft.glass_prev_us = glass_us;
}


// What this frame touched, and what each framebuffer page has not been given
// yet. Empty is x1 > x2.
//
// A short list of rectangles, not one bounding box. A frame's damage is often
// two small places far apart -- the title scrolling at the top of the player,
// the progress bar at the bottom -- and the box around both is nearly the
// whole screen, shrunk again every frame for a few dozen changed rows. In a
// profile on the device that copy was the biggest single cost of the
// interface thread.
#define DIRTY_MAX 8

typedef struct {
	int n;
	lv_area_t r[DIRTY_MAX];
} dirty_t;

// The waits of one frame, in the order they happen. Times are microseconds, the
// unit they are taken in; the log line turns them into milliseconds.
typedef struct {
	uint32_t flip_us; // waiting for the flip thread to have panned (the slot's slack)
	uint32_t rise_us; // waiting for the panel's transfer of the previous frame to begin
	uint32_t xfer_us; // waiting for the panel to have read the rows the copy writes
	uint32_t copy_us; // the copy phase as a whole: the copy plus those waits
	bool rise_over;   // the transfer was already finished when the wait looked
} frame_waits_t;

// ---------------------------------------------------------------------------
// the snapshot: the drawn frame, handed to the flip thread
// ---------------------------------------------------------------------------
//
// The copy onto the panel has to wait for the panel's own read of the page it
// writes (see panel_wait_row), which is 12 ms of the 26 ms slot -- and while it
// waits, the interface thread has nothing to do: the page the panel reads is the
// framebuffer, not the page LVGL draws into. So the frame is handed over
// instead. The interface thread copies what changed into a page of its own (the
// snapshot) and goes straight back to drawing the next frame, and the flip
// thread rides the panel's read with the snapshot and then hands the panel the
// frame.
//
// Measured on the device, that is the difference between the interface thread
// spending 22-23 ms of every slot (draw 10 + the copy phase 12) and spending 13
// (draw 10 + the snapshot 3): the copy phase moves off the critical path, and
// the slot has the slack it needs when a frame's drawing happens to be
// expensive.
//
// The snapshots: one page is being copied onto the panel by the flip thread
// while the interface thread fills the other. Two of them, because one is a
// serialization: the interface thread would have to wait for the copy of the
// previous frame before it could put the next one anywhere, and that wait is on
// the same thread as the animation timers -- measured, a panel sliding open
// updates its position 5 to 23 ms late and irregularly, which reads as a slide
// that jerks.
//
// The freshest frame wins: if the interface thread has filled a second page by
// the time the flip thread comes round, the older one is dropped rather than
// copied late.
static uint8_t *snap_pages[2];  // RGB565, log_w x log_h; NULL: no pipeline
static bool snap_filled[2];    // a frame is in it, waiting for the flip thread
static bool snap_reading[2];   // the flip thread is copying out of it
static int snap_next;          // the page the interface thread fills next
static int snap_job = -1;      // the page the flip thread should copy
// What has to be copied out of each page. Per page, not one for the job: the
// interface thread fills the other page while the flip thread copies this one,
// and a shared list would be rewritten under the copy -- which shows up as the
// frame read from beyond the snapshot, tiled into vertical strips (measured).
static dirty_t snap_jobs[2];
static bool fb_pipelined;      // the above are in use (see try_panned_display)

// Filled by fb_copy_frame for the pan that follows it.
static bool fb_copy_second_page;

// Defined with the framebuffer code, below: the snapshot needs the dirty
// rectangle list and the copy needs the panel's registers.
static void snap_hand_over(const uint8_t *page, const dirty_t *dirty);
static void fb_copy_frame(uint8_t *page, const uint8_t *src, const dirty_t *dirty);

static void lcdc_map(void) {
	if (lcdc_regs) {
		return;
	}
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("fb: /dev/mem is not there; the copy will not wait for the panel's transfer\n");
		return;
	}
	void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LCDC_BASE);
	close(fd);
	if (m == MAP_FAILED) {
		printf("fb: /dev/mem cannot be mapped; the copy will not wait for the panel's transfer\n");
		return;
	}
	lcdc_regs = m;
}

// The copy trails the panel's own read of the page.
//
// The transfer reads the framebuffer from the top down, so a row is free the
// moment the panel has passed it -- which means the copy does not have to wait
// for the whole transfer, only for the rows it is about to write. The dirty
// rectangles are therefore copied in horizontal bands, each band after the panel
// has read it, which rides the copy along with the transfer instead of waiting
// it out.
//
// What that buys, measured: the transfer is 12.29 ms of the 26.46 ms slot and
// the copy another 5, so waiting the transfer out before starting left the
// interface thread 11.5 ms of every slot idle and only ~2 ms of slack in the
// slot -- and a frame that cost a little more than usual dropped it (the hitch).
// Banded, the copy finishes ~13.1 ms into the slot instead of 19.3, and the
// slack is back.
//
// The bands are only cut where the mapping is one row to one row: a scaled
// layout or a rotated screen maps layout rows to panel rows in a way the safe
// time would have to be turned inside out for, and those paths wait the
// transfer out as before.
#define PAN_BAND_ROWS 60
#define PAN_ROW_MARGIN_US 300u

// When the panel began reading the page this frame is going into, and whether it
// is reading it at all. Set by panel_transfer_start, used by panel_wait_row; a
// zero start means there is nothing to wait for and the copy goes in at once.
static uint64_t panel_xfer_start_us;

// Asks the panel what it is doing with the page. Returns true when it is (or is
// about to be) reading it -- the copy then waits per band, see panel_wait_row --
// and false when there is nothing to wait for: the transfer is over, or the
// panel never began one.
//
// The one fact that makes the short answer possible: a transfer begins within
// one panel frame of the pan (its grid) and then reads for 12.3 ms. So one panel
// frame after the pan the transfer must have begun -- if BUSY is low then, it
// has also finished, and the page is ours.
static bool panel_transfer_start(uint64_t pan_us, uint32_t *out_rise_us, bool *out_over) {
	*out_rise_us = 0;
	*out_over = false;
	panel_xfer_start_us = 0;
	if (!lcdc_regs || !pan_us) {
		return false;
	}
	volatile uint32_t *state = &lcdc_regs[SLCDC_STATE_OFF / 4];
	struct timespec step = {0, 200000}; // 200 us

	// The flip thread watches the same register right after the pan, so its
	// answer is usually already here and exact.
	uint64_t start = panel_rise_us >= pan_us ? panel_rise_us : 0;

	uint64_t t0 = ft_now_us();
	uint64_t deadline = pan_us + PANEL_FRAME_US + PAN_TRANSFER_MARGIN_US;
	if (!start && t0 >= deadline) {
		if (!(*state & 1u)) {
			*out_over = true; // it must have begun, and it is not running: it is done
			return false;
		}
	}
	for (;;) {
		if (*state & 1u) {
			if (!start) {
				start = ft_now_us(); // seen mid-transfer: read as if it began now
			}
			break;
		}
		if (ft_now_us() >= deadline) {
			*out_over = true; // nothing came: a panel that is not running, or a refused pan
			return false;
		}
		nanosleep(&step, NULL);
	}
	*out_rise_us = (uint32_t)(ft_now_us() - t0);
	panel_xfer_start_us = start;
	return true;
}

// Waits until the panel has read the rows up to `y2` of the page being copied.
// The transfer goes top to bottom, so this is where the copy may write: it is
// the panel's own progress rather than a guess about the whole transfer, and it
// is the reason a band near the top of the screen can go in almost at once.
static void panel_wait_row(uint32_t y2, uint32_t height, uint32_t *out_wait_us) {
	if (!panel_xfer_start_us || !height) {
		return;
	}
	uint64_t t0 = ft_now_us();
	uint64_t safe = panel_xfer_start_us + (uint64_t)(y2 + 1) * PAN_TRANSFER_US / height + PAN_ROW_MARGIN_US;
	for (;;) {
		uint64_t now = ft_now_us();
		if (now >= safe) {
			break;
		}
		uint64_t left = safe - now;
		struct timespec ts = {0, (long)(left < 200 ? left : 200) * 1000};
		nanosleep(&ts, NULL);
	}
	*out_wait_us += (uint32_t)(ft_now_us() - t0);
}

// Where the panel's own frame grid is: waits for the transfer this pan started
// to begin and notes the instant, which is what the next pan is measured from
// (see the flip thread). Runs in the flip thread, after the interface thread
// has been let go, so it holds nothing up -- the same register is polled by the
// copy, and whoever sees the transfer first is right.
//
// It does not give up when the next frame is already waiting: the instant is the
// grid the pan has to be measured from, and without it the pan drifts off the
// panel's boundaries (measured: copy phases of 24 ms instead of 14, and a glass
// cadence of 50 to 130 ms). The wait is short -- the transfer begins about
// PAN_GUARD_US after the pan -- and a transfer that does not come is bounded by
// the deadline.
static void panel_watch_rise(void) {
	if (!lcdc_regs) {
		return;
	}
	volatile uint32_t *state = &lcdc_regs[SLCDC_STATE_OFF / 4];
	struct timespec step = {0, 200000};
	uint64_t deadline = ft_now_us() + PANEL_FRAME_US + PAN_TRANSFER_MARGIN_US;
	for (;;) {
		if (*state & 1u) {
			panel_rise_us = ft_now_us();
			pace_note(panel_rise_us);
			return;
		}
		if (ft_now_us() >= deadline) {
			return;
		}
		nanosleep(&step, NULL);
	}
}


// Whether the last pan was refused (only for saying so once, not for deciding
// whether to keep asking).
static bool fb_pan_refused;

// Whether the panel's own DMA is armed.
//
// Only FBIOPUT_VSCREENINFO arms it. An unblank on its own brings
// LCDC_CTRL.ENA back but leaves SLCDC_CTRL.DMA_EN at 0, and a pan in that
// state is *accepted* and transfers nothing: the picture stops while every
// register looks healthy. Measured on the device (2026-10-13): after
// FB_BLANK_POWERDOWN, ENA=1 / DMA_EN=0 and a pan reports success with no
// SLCDC activity at all; after the PUT, DMA_EN=1 and the frame goes. The
// registers are identical in the working and the wedged state, so the state
// cannot be read back -- the port keeps it itself: every blank disarms, every
// PUT arms.
static bool fb_armed;

// Told by the power path that the panel has just been taken down, so the next
// wake knows it has to arm it again.
//
// The flip thread's own state goes with it: the panel's grid sample, the pan it
// is still waiting to see, and the one-shot refusal notice. They describe a
// transfer that the sleep ended; carried over, the next wake would pace against
// a rise that will never come and copy behind a read the panel is not doing.
void display_panel_disarmed(void) {
	fb_armed = false;
	pan_outstanding = false;
	panel_rise_us = 0;
	panel_xfer_start_us = 0;
	fb_pan_refused = false;
}

// Unblank and put the mode back with FORCE, in that order: the pair is what
// arms the SLCDC. No pan here, so a caller that wants the picture on the glass
// pans afterwards.
static void fb_arm(void) {
	ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
	fb_var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var) < 0) {
		perror("fb: FBIOPUT_VSCREENINFO (arm)");
		return;
	}
	fb_armed = true;
}

// Ask the panel to show what has been written into the page.
//
// On a single-page panel the pan is not a page change: it is the driver's cue
// to re-read the page it is displaying. Without it the glass keeps whatever it
// latched last, which is why a refused pan looks like a frozen picture sitting
// on top of a perfectly live interface -- the buttons work, the clock in the
// buffer is right, and none of it is on the glass.
//
// A refusal has two causes. The ordinary one: the panel is blanked, the driver
// says so ("pan_display: lcd is disable?"), and it has also reset the mode, so
// the offset that was right before no longer fits. Re-applying that offset --
// which is what this used to do -- refuses every frame for the rest of the
// session, because nothing ever changes it back. So on a refusal the mode is
// read again, the offset is recomputed for the mode the driver now holds, and
// the pan is retried once.
//
// The other cause is that the panel is disarmed (a blank did it, and an
// unblank does not undo it). Then even a correct offset is refused, and the
// thing to do is to arm the panel and ask again -- what this must never do is
// go quiet, because a port that has stopped asking on a disarmed panel is
// exactly the frozen picture this whole path exists for.
static void fb_pan_show(unsigned yoffset) {
	fb_var.yoffset = yoffset;
	if (ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var) == 0) {
		fb_pan_refused = false;
		return;
	}

	unsigned want = yoffset;
	ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var);
	if (fb_var.yres_virtual >= fb_var.yres * 2 && want >= fb_var.yres && want < fb_var.yres_virtual) {
		fb_var.yoffset = want; // the second page is still there and still this far in
	} else {
		fb_var.yoffset = 0; // one page (or a shorter one): the top of it is all there is
	}
	if (ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var) == 0) {
		fb_mode_stale = true; // the next frame lays itself out against this mode
		fb_pan_refused = false;
		return;
	}

	// Still refused. While the screen is meant to be on, arm the panel and
	// ask once more; while it is off, stay quiet (the backlight is off, the
	// panel is meant to be blanked, and arming it would only power it).
	if (!power_screen_is_on()) {
		fb_pan_refused = false;
		return;
	}
	if (!fb_pan_refused) {
		fprintf(stderr, "fb: the panel refused the pan twice; arming it and asking again (offset %u)\n", want);
		fb_pan_refused = true;
	}
	fb_arm();
	if (ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var) == 0) {
		fb_mode_stale = true;
		fb_pan_refused = false;
	}
}

// When the next pan is wanted, on the panel's own frame grid. The pan is issued
// PAN_GUARD_US before a boundary and the next one two panel frames later, so
// the glass is handed a frame exactly every PAN_CADENCE_US whatever the
// interface thread's own timing does in between -- and the phase is taken from
// the boundary the last pan landed on, so it cannot drift away from the panel's.
static uint64_t pan_target_us;

// How often the interface thread was late for its slot, and by how much. Written
// by the flip thread, printed (and cleared) by the interface thread's log line.
static int pan_late_n;
static uint32_t pan_late_max;

static void pan_pace_wait(void) {
	for (;;) {
		uint64_t now = ft_now_us();
		if (now >= pan_target_us) {
			return;
		}
		uint64_t left = pan_target_us - now;
		struct timespec ts = {0, (long)(left < 2000 ? left : 2000) * 1000};
		nanosleep(&ts, NULL);
	}
}

static void *flip_thread(void *arg) {
	(void)arg;
	thread_be_realtime("fb flip", 5);
	for (;;) {
		pthread_mutex_lock(&flip_lock);
		while (!flip_pending) {
			pthread_cond_wait(&flip_cond, &flip_lock);
		}
		struct fb_var_screeninfo var = flip_var;
		flip_pending = false;
		// A frame to put on the panel: the interface thread left it in one of
		// the snapshots and went back to drawing (see the snapshot note).
		int idx = snap_job;
		bool copy_here = fb_pipelined && idx >= 0 && snap_filled[idx];
		if (copy_here) {
			snap_filled[idx] = false;
			snap_reading[idx] = true;
			snap_job = -1;
			// Anything filled that is not this one is older: the glass gets the
			// freshest frame, so it is dropped rather than copied late.
			for (int i = 0; i < 2; i++) {
				if (i != idx && snap_filled[i]) {
					snap_filled[i] = false;
					ft.dropped++;
				}
			}
		}
		pthread_cond_broadcast(&flip_cond);
		pthread_mutex_unlock(&flip_lock);

		if (copy_here) {
			// The copy rides the panel's read of the page (see fb_copy_frame),
			// and the pan below does not touch the snapshot, so the interface
			// thread has it back as soon as the copy is in.
			fb_copy_frame(fb_mem, snap_pages[idx], &snap_jobs[idx]);
			pthread_mutex_lock(&flip_lock);
			snap_reading[idx] = false;
			pthread_cond_broadcast(&flip_cond);
			pthread_mutex_unlock(&flip_lock);
		}

		// When the next pan is wanted, on the panel's own frame grid: the pan is
		// issued PAN_GUARD_US before a boundary, so the glass is handed a frame
		// every pan_cadence_frames of them whatever the interface thread's own
		// timing does in between.
		uint64_t now = ft_now_us();
		if (!pan_target_us) {
			pan_target_us = now;
		}
		// How late the interface thread was for the slot it asked for: the pan
		// then lands on the next boundary instead of the one wanted, and that
		// lateness is the hitch a hand feels. Counted here, printed with the
		// glass cadence.
		uint64_t asked_us = now;
		if (asked_us > pan_target_us) {
			uint64_t late = asked_us - pan_target_us;
			pan_late_n++;
			if (late > pan_late_max) {
				pan_late_max = (uint32_t)late;
			}
		}
		uint64_t pace0 = ft_now_us();
		pan_pace_wait();
		uint32_t pace_ms = (uint32_t)((ft_now_us() - pace0) / 1000u);
		ft.pace_sum += pace_ms;
		ft.pace_max = pace_ms > ft.pace_max ? pace_ms : ft.pace_max;

		fb_pan_us = ft_now_us();
		fb_pan_show(var.yoffset);
		pan_outstanding = true; // the panel is about to read the page the copy writes

		// Where the panel's grid is, and the next slot on it: the rise this pan
		// leads to is the boundary to measure the next one from, so the phase is
		// re-taken every frame and cannot drift away.
		panel_watch_rise();

		// The next pan goes on that grid: pan_cadence_frames panel frames after
		// the boundary just used, which is the cadence the picture is meant to
		// have. A slot the interface thread was too late for is *dropped*, not
		// carried: the next boundary is taken instead of the next-but-one, so a
		// hitch costs one panel frame rather than two.
		uint64_t base = panel_rise_us >= fb_pan_us ? panel_rise_us : fb_pan_us;
		now = ft_now_us();
		uint64_t steps = pan_cadence_frames ? pan_cadence_frames : 2;
		while (base + steps * PANEL_FRAME_US - PAN_GUARD_US <= now) {
			steps++;
		}
		pan_target_us = base + steps * PANEL_FRAME_US - PAN_GUARD_US;
	}
	return NULL;
}

// Returns once no flip is under way: the page off screen is then really off
// screen, and fb_var.yoffset is the page being shown.
static void fb_flip_wait(void) {
	pthread_mutex_lock(&flip_lock);
	while (flip_pending) {
		pthread_cond_wait(&flip_cond, &flip_lock);
	}
	pthread_mutex_unlock(&flip_lock);
}

static void fb_flip_start(void) {
	pthread_t thread;
	flip_thread_on = pthread_create(&thread, NULL, flip_thread, NULL) == 0;
	if (flip_thread_on) {
		pthread_detach(thread);
	}
}

// ---------------------------------------------------------------------------
// 180 degree rotation ("Rotate screen")
//
// LVGL 9.1 has no display rotation of its own (it arrives in 9.2), and the
// panel cannot be told to scan out upside down. So when the option is on the
// rendering moves off the framebuffer: LVGL draws into ONE full-screen RAM
// page, and what it draws is copied into the framebuffer turned through 180
// degrees. Unrotated, LVGL renders straight into the mapped pages, with no
// copy at all.
//
// One RAM page, not two, deliberately. With two, LVGL's direct mode keeps
// them coherent by re-drawing the previous frame's areas into the buffer it
// is rendering into, but it does not flush those areas, because for LVGL the
// buffer is the framebuffer -- and with the copy in between, the framebuffer
// page would never receive them. A single RAM page always holds the whole
// current frame, and the code below keeps its own record of what each
// framebuffer page is still missing.
//
// The touch panel is turned with it, by inverting the evdev calibration.
// ---------------------------------------------------------------------------

static lv_display_t *fb_display;   // the page-flipping display, NULL on the fallback
static lv_indev_t *fb_touch;	   // so the calibration can be flipped with the screen
static bool fb_rotated;
static uint8_t *rot_page;		   // the RAM page LVGL draws into while rotated

// 2/3 scaling, for a panel exactly two thirds of the model's layout (the V3
// Blaze: 320x480 glass, 480x720 interface). LVGL draws the full layout into
// one RAM page and every flush shrinks what changed into the framebuffer page,
// using the same per-page bookkeeping as the rotation. The touch panel follows
// by itself: evdev maps its raw range onto the display's (logical) resolution.
static bool fb_scaled;
static uint8_t *scale_page;
static int32_t log_w, log_h; // the resolution LVGL renders at; the panel's unless scaled

// True when LVGL draws into scale_page instead of the framebuffer: always when
// the layout is scaled down, and always at 32 bpp, where LVGL's RGB565 cannot
// be handed to the BGRA page as it sits and has to be rewritten by the copy.
static bool fb_ram_page;


static dirty_t rot_frame_dirty;
static dirty_t rot_page_pending[2];

static bool area_is_empty(const lv_area_t *a) { return a->x1 > a->x2 || a->y1 > a->y2; }

static void area_merge(lv_area_t *into, const lv_area_t *add) {
	if (area_is_empty(add)) {
		return;
	}
	if (area_is_empty(into)) {
		*into = *add;
		return;
	}
	if (add->x1 < into->x1) {
		into->x1 = add->x1;
	}
	if (add->y1 < into->y1) {
		into->y1 = add->y1;
	}
	if (add->x2 > into->x2) {
		into->x2 = add->x2;
	}
	if (add->y2 > into->y2) {
		into->y2 = add->y2;
	}
}

// In the coordinates LVGL renders in (the panel's, unless scaled).
static void area_set_full(lv_area_t *a) {
	a->x1 = 0;
	a->y1 = 0;
	a->x2 = log_w - 1;
	a->y2 = log_h - 1;
}

static int32_t area_size(const lv_area_t *a) {
	return area_is_empty(a) ? 0 : (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
}

static void dirty_clear(dirty_t *d) { d->n = 0; }

static void dirty_set_full(dirty_t *d) {
	d->n = 1;
	area_set_full(&d->r[0]);
}

// Adds a rectangle. It joins one already there when the box around the two
// costs little more than the two apart -- overlapping, touching, or side by
// side -- and otherwise takes a slot of its own. With the list full it goes
// into whichever rectangle it grows least.
static void dirty_add(dirty_t *d, const lv_area_t *a) {
	if (area_is_empty(a)) {
		return;
	}
	int best = -1;
	int32_t best_growth = 0;
	for (int i = 0; i < d->n; i++) {
		lv_area_t box = d->r[i];
		area_merge(&box, a);
		int32_t growth = area_size(&box) - area_size(&d->r[i]);
		if (area_size(&box) <= (area_size(&d->r[i]) + area_size(a)) * 5 / 4) {
			d->r[i] = box;
			return;
		}
		if (best < 0 || growth < best_growth) {
			best = i;
			best_growth = growth;
		}
	}
	if (d->n < DIRTY_MAX) {
		d->r[d->n++] = *a;
	} else {
		area_merge(&d->r[best], a);
	}
}

static void dirty_add_all(dirty_t *d, const dirty_t *from) {
	for (int i = 0; i < from->n; i++) {
		dirty_add(d, &from->r[i]);
	}
}

// RGB565 spread out so the three fields have room to grow: green moves up to
// bits 21..26, red stays at 11..15 and blue at 0..4, with at least four free
// bits above each. A weighted sum of total weight 16 then adds all three
// channels in one 32-bit addition.
static inline __attribute__((always_inline)) uint32_t spread565(uint32_t p) { return (p | (p << 16)) & 0x07E0F81Fu; }

// A 9/3/3/1 sum of four spread pixels (weight 16 in all) back to RGB565: all
// three fields rounded (+8) and divided by 16 in one shift, then folded back.
// The weights are the bilinear ones, which is what makes this a shift; the
// area weights (4/2/2/1, over 9) took three multiplications a pixel, and on
// the device this was a quarter of the whole copy: 6.3 ms a full frame against
// 8.2, with a difference measured at 40 dB on real screens.
static inline __attribute__((always_inline)) uint16_t pack16(uint32_t s) {
	uint32_t t = ((s + 0x01004008u) >> 4) & 0x07E0F81Fu;
	return (uint16_t)(t | (t >> 16));
}

// Writes one finished RGB565 pixel into a framebuffer page. At 16 bpp the page
// wants the two bytes as packed. At 32 bpp the panel scans out four bytes a
// pixel in BGRA order -- blue first and an alpha last, and it is the LCDC that
// reads that alpha -- so the three fields are taken apart and widened, the low
// bits repeated into the high ones so full scale stays full scale, and the
// alpha byte is opaque: this interface has no translucency of its own to keep.
// One RGB565 pixel as the four bytes the 32-bit panel scans, in the order it
// scans them: blue, green, red, opaque. The 5- and 6-bit fields are widened by
// repeating their low bits into the high ones, so full scale stays full scale.
// The masks the widening used to carry are dead -- neither (v << 3) | (v >> 2)
// nor (v << 2) | (v >> 4) can leave a byte -- and the alpha is opaque because
// this interface has no translucency of its own to keep.
static inline __attribute__((always_inline)) uint32_t rgb565_to_bgra(uint16_t pixel) {
	uint32_t b = pixel & 0x1Fu, g = (pixel >> 5) & 0x3Fu, r = (pixel >> 11) & 0x1Fu;
	return ((b << 3) | (b >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((r << 3) | (r >> 2)) << 16) | 0xFF000000u;
}

static inline __attribute__((always_inline)) void fb_store_pixel(uint8_t *at, uint16_t pixel) {
	// One store per pixel, in the width the page wants. The addresses handed in
	// are always a whole number of pixels into the page, and the page is page
	// aligned, so a 4-byte page pixel is 4-byte aligned and a 2-byte one is
	// 2-byte aligned: the two cases are one unaligned-looking store that is not.
	if (fb_bytespp == 4) {
		*(uint32_t *)at = rgb565_to_bgra(pixel);
	} else {
		*(uint16_t *)at = pixel;
	}
}

// Shrinks a dirty rectangle of the 3:2-larger layout `src` into `dst` (a whole
// framebuffer page), turned through 180 degrees when `rotated`. Each panel
// pixel covers 1.5 layout pixels per axis: the nearer one weighs 3, the
// shared one 1, so every 3x3 block of the layout becomes a 2x2 block.
//
// The filter is separable, so each source column of a row pair is first
// folded vertically -- 3 * outer row + the shared middle row -- and then each
// output pixel horizontally from two such folds: 3 * outer column + middle
// column. A 3x3 block is loaded once for its four output pixels, the middle
// row and column shared between them.
//
// The finished RGB565 pixel goes through fb_store_pixel(), which knows how wide
// a framebuffer pixel is: the two bytes of the Blaze's page or the four of the
// V3 Analog's BGRA. Everything above that call is the same arithmetic either
// way, so the panel paying for 32 bits changes nothing in the filter.
static void blit_scaled_2_3(uint8_t *dst, const uint16_t *src, const lv_area_t *area, bool rotated) {
	int32_t pw = (int32_t)fb_var.xres, ph = (int32_t)fb_var.yres;
	int bpp = fb_bytespp;
	int32_t x1 = area->x1 < 0 ? 0 : area->x1;
	int32_t y1 = area->y1 < 0 ? 0 : area->y1;
	int32_t x2 = area->x2 >= log_w ? log_w - 1 : area->x2;
	int32_t y2 = area->y2 >= log_h ? log_h - 1 : area->y2;
	if (x1 > x2 || y1 > y2) {
		return;
	}

	// Every panel pixel that reads any layout pixel of the rectangle.
	int32_t px1 = x1 * 2 / 3, px2 = (x2 * 2 + 1) / 3;
	int32_t py1 = y1 * 2 / 3, py2 = (y2 * 2 + 1) / 3;
	if (px2 >= pw) {
		px2 = pw - 1;
	}
	if (py2 >= ph) {
		py2 = ph - 1;
	}

	for (int32_t py = py1; py <= py2;) {
		int32_t k = py / 2 * 3;
		// Both rows of the pair when the rectangle has them, else the one.
		bool even = !(py & 1);
		bool pair = even && py + 1 <= py2;
		const uint16_t *r0 = src + (size_t)k * log_w;	// outer row of the even line
		const uint16_t *r1 = r0 + log_w;				// the shared middle row
		const uint16_t *r2 = r1 + log_w;				// outer row of the odd line
		const uint16_t *ra = even ? r0 : r2;			// the outer row of a lone line
		uint8_t *oe = dst + (size_t)(rotated ? ph - 1 - py : py) * pw * bpp;
		uint8_t *oo = dst + (size_t)(rotated ? ph - 2 - py : py + 1) * pw * bpp;

		for (int32_t px = px1; px <= px2;) {
			int32_t j = px / 2 * 3;
			bool left = !(px & 1);
			bool both = left && px + 1 <= px2;
			int32_t xl = rotated ? pw - 1 - px : px;

			if (pair) {
				uint32_t m0 = spread565(r1[j]), m1 = spread565(r1[j + 1]), m2 = spread565(r1[j + 2]);
				uint32_t e0 = 3 * spread565(r0[j]) + m0, e1 = 3 * spread565(r0[j + 1]) + m1,
						 e2 = 3 * spread565(r0[j + 2]) + m2;
				uint32_t o0 = 3 * spread565(r2[j]) + m0, o1 = 3 * spread565(r2[j + 1]) + m1,
						 o2 = 3 * spread565(r2[j + 2]) + m2;
				if (both) {
					int32_t xr = rotated ? xl - 1 : xl + 1;
					fb_store_pixel(oe + (size_t)xl * bpp, pack16(3 * e0 + e1));
					fb_store_pixel(oe + (size_t)xr * bpp, pack16(3 * e2 + e1));
					fb_store_pixel(oo + (size_t)xl * bpp, pack16(3 * o0 + o1));
					fb_store_pixel(oo + (size_t)xr * bpp, pack16(3 * o2 + o1));
					px += 2;
				} else {
					fb_store_pixel(oe + (size_t)xl * bpp, pack16(3 * (left ? e0 : e2) + e1));
					fb_store_pixel(oo + (size_t)xl * bpp, pack16(3 * (left ? o0 : o2) + o1));
					px++;
				}
			} else {
				uint32_t v1 = 3 * spread565(ra[j + 1]) + spread565(r1[j + 1]);
				if (both) {
					int32_t xr = rotated ? xl - 1 : xl + 1;
					fb_store_pixel(oe + (size_t)xl * bpp, pack16(3 * (3 * spread565(ra[j]) + spread565(r1[j])) + v1));
					fb_store_pixel(oe + (size_t)xr * bpp,
								   pack16(3 * (3 * spread565(ra[j + 2]) + spread565(r1[j + 2])) + v1));
					px += 2;
				} else {
					int32_t c = left ? j : j + 2;
					fb_store_pixel(oe + (size_t)xl * bpp, pack16(3 * (3 * spread565(ra[c]) + spread565(r1[c])) + v1));
					px++;
				}
			}
		}
		py += pair ? 2 : 1;
	}
}

// Copies one dirty rectangle of `src` into `dst` (a whole framebuffer page),
// turned through 180 degrees when `rotated`: the pixel at (x, y) lands at
// (W-1-x, H-1-y). This is the copy the native panel makes every frame, so it is
// the one place on the interface thread worth shaping by hand.
//
// The framebuffer's width is decided once, before the loops, and the turn has a
// loop of its own. It used to be fb_store_pixel(), which read the global
// fb_bytespp on every pixel -- and because the loop writes through a pointer
// the compiler must assume can alias that global, the test could not be hoisted
// and every pixel paid for it. Measured on the host, that change alone is a
// third off a full-screen copy; the two-pixel stores and the 5->8 / 6->8
// lookup tables the same measurement tried were both slower, so they are not
// here.
static void blit_rotated_180(uint8_t *dst, const uint16_t *src, const lv_area_t *area, int32_t width,
							 int32_t height, bool rotated) {
	int32_t x1 = area->x1 < 0 ? 0 : area->x1;
	int32_t y1 = area->y1 < 0 ? 0 : area->y1;
	int32_t x2 = area->x2 >= width ? width - 1 : area->x2;
	int32_t y2 = area->y2 >= height ? height - 1 : area->y2;

	if (fb_bytespp == 4) {
		// The V3 Analog's page: RGB565 in, BGRA out, one pixel for one.
		if (rotated) {
			for (int32_t y = y1; y <= y2; y++) {
				const uint16_t *s = src + (size_t)y * width;
				uint32_t *d = (uint32_t *)(dst + (size_t)(height - 1 - y) * width * 4);
				for (int32_t x = x1; x <= x2; x++) {
					d[width - 1 - x] = rgb565_to_bgra(s[x]);
				}
			}
		} else {
			for (int32_t y = y1; y <= y2; y++) {
				const uint16_t *s = src + (size_t)y * width;
				uint32_t *d = (uint32_t *)(dst + (size_t)y * width * 4);
				for (int32_t x = x1; x <= x2; x++) {
					d[x] = rgb565_to_bgra(s[x]);
				}
			}
		}
		return;
	}

	// A 16-bit page (the Blaze and the R3 Pro II): the two bytes as they sit.
	if (rotated) {
		for (int32_t y = y1; y <= y2; y++) {
			const uint16_t *s = src + (size_t)y * width;
			uint16_t *d = (uint16_t *)(dst + (size_t)(height - 1 - y) * width * 2);
			for (int32_t x = x1; x <= x2; x++) {
				d[width - 1 - x] = s[x];
			}
		}
	} else {
		for (int32_t y = y1; y <= y2; y++) {
			const uint16_t *s = src + (size_t)y * width;
			uint16_t *d = (uint16_t *)(dst + (size_t)y * width * 2);
			for (int32_t x = x1; x <= x2; x++) {
				d[x] = s[x];
			}
		}
	}
}

static void ft_refr_start_cb(lv_event_t *e) {
	(void)e;
	ft.refr_start_us = ft_now_us();
}

// Called once per frame by the interface thread, when the frame is on its way
// (handed to the flip thread, or copied and panned here when there is no flip
// thread). What the columns mean:
//
//   draw   the interface thread drawing the frame (LVGL)
//   snap   copying its damage into the snapshot page (nothing when the copy is
//          done here instead, see fb_copy_frame)
//   hold   waiting for the flip thread to be through with the snapshot
//   copy   the copy phase, by whichever thread did it: the copy itself plus the
//          waits for the panel to have read the rows it writes
//   pace   the flip thread waiting for the panel's grid -- the slot's slack
//   work   what the slot has to hold: draw + snap when the copy has been handed
//          over, and draw + rise + copy when it has not
static void ft_frame_done(uint64_t flush_us, uint64_t hand_us, uint64_t pan_done_us) {
	uint32_t draw = ft.refr_start_us ? (uint32_t)(flush_us - ft.refr_start_us) / 1000u : 0;
	uint32_t snap = ft.snap_last_us / 1000u;
	uint32_t hold = ft.hold_last_us / 1000u;
	uint32_t work = fb_pipelined ? draw + snap + hold : draw;
	if (!ft.second_start_us) {
		ft.second_start_us = pan_done_us;
	}
	ft.frames++;
	ft.slow += work * 1000u > frame_slow_us();
	ft.render_sum += draw;
	ft.snap_sum += snap;
	ft.hold_sum += hold;
	ft.work_sum += work;
	ft.render_max = draw > ft.render_max ? draw : ft.render_max;
	ft.snap_max = snap > ft.snap_max ? snap : ft.snap_max;
	ft.hold_max = hold > ft.hold_max ? hold : ft.hold_max;
	ft.work_max = work > ft.work_max ? work : ft.work_max;
	if (pan_done_us - ft.second_start_us >= 1000000u) {
		if (ft.frames) {
			printf("frames: %d in %u ms, %d slow; draw avg %u max %u, snap avg %u max %u, hold avg %u max %u, copy "
				   "avg %u max %u (%u%% of a screen), pace avg %u max %u, work avg %u max %u (%d over, %d dropped)\n",
				   ft.frames, (unsigned)((pan_done_us - ft.second_start_us) / 1000u), ft.slow, ft.render_sum / ft.frames,
				   ft.render_max, ft.snap_sum / ft.frames, ft.snap_max, ft.hold_sum / ft.frames, ft.hold_max,
				   ft.blit_sum / ft.frames, ft.blit_max,
				   (unsigned)(ft.copied_px * 100u / ((uint64_t)ft.frames * (uint64_t)(log_w * log_h))),
				   ft.pace_sum / ft.frames, ft.pace_max, ft.work_sum / ft.frames, ft.work_max, ft.rise_over, ft.dropped);
		}
		if (ft.pace_gap_n >= 8) {
			double avg = (double)ft.pace_gap_sum / (double)ft.pace_gap_n;
			double var = (double)ft.pace_gap_sq / (double)ft.pace_gap_n - avg * avg;
			printf("glass: %d gaps, avg %.1f ms sd %.1f, min %.1f max %.1f ms; %d late (max %u us)\n", ft.pace_gap_n,
				   avg / 1000.0, var > 0 ? sqrt(var) / 1000.0 : 0.0, (double)ft.pace_gap_min / 1000.0,
				   (double)ft.pace_gap_max / 1000.0, pan_late_n, pan_late_max);
		}
		uint64_t keep_glass = ft.glass_prev_us;
		memset(&ft, 0, sizeof(ft));
		ft.glass_prev_us = keep_glass;
		pan_late_n = 0;
		pan_late_max = 0;
	}
}

// What has to go onto the panel this frame: the damage just drawn plus whatever
// the page it goes to still owes (the full redraw a rotation or a scaling change
// asks for). Returns that page and sets fb_copy_second_page for the pan.
static uint8_t *fb_frame_page(dirty_t *out) {
	if (fb_single_page) {
		// One page keeps what was written to it, so this frame's own damage is
		// all that has to go in; pending[0] exists only for the full redraws.
		*out = rot_page_pending[0];
		dirty_add_all(out, &rot_frame_dirty);
		dirty_clear(&rot_page_pending[0]);
		dirty_clear(&rot_frame_dirty);
		fb_copy_second_page = false;
		return fb_mem;
	}

	fb_copy_second_page = (fb_var.yoffset == 0); // flip to the other page
	int target = fb_copy_second_page ? 1 : 0;
	*out = rot_page_pending[target];
	dirty_add_all(out, &rot_frame_dirty);
	dirty_clear(&rot_page_pending[target]);
	dirty_add_all(&rot_page_pending[target ? 0 : 1], &rot_frame_dirty); // the other page still owes it
	dirty_clear(&rot_frame_dirty);
	return fb_mem + (fb_copy_second_page ? fb_page_size : 0);
}

// Copies `dirty` out of `src` (RGB565, the layout's own size) onto `page`, riding
// the panel's read of that page where that is possible (see panel_wait_row), and
// records what the copy phase cost.
//
// This is the interface thread's old copy phase, and it runs on whichever thread
// does the copying: the flip thread when the frame was handed over (see the
// snapshot note), the interface thread itself when there is no flip thread to
// hand it to.
static void fb_copy_frame(uint8_t *page, const uint8_t *src, const dirty_t *dirty) {
	uint64_t t0 = ft_now_us();
	frame_waits_t waits = {0};

	// With one page the pan is a promise that the panel will come and read this
	// very memory, so the copy goes in behind the panel's own read of it. With
	// two pages it goes into the page the panel is not showing, and there is
	// nothing to wait for.
	bool banded = false;
	if (fb_single_page && pan_outstanding) {
		banded = panel_transfer_start(fb_pan_us, &waits.rise_us, &waits.rise_over);
		pan_outstanding = false;
	}

	// A pan that was refused because the mode changed (see fb_pan_show) leaves
	// the copy working from a screen info the driver no longer holds: read it
	// again before the frame is laid out.
	if (fb_mode_stale) {
		fb_mode_stale = false;
		ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var);
	}

	uint32_t band_wait_us = 0;
	// A scaled layout or a rotated screen maps layout rows to panel rows in a
	// way the band's safe time would have to be turned inside out for, so those
	// paths wait the whole transfer out before the first rect.
	if (banded && (fb_scaled || fb_rotated)) {
		panel_wait_row((uint32_t)fb_var.yres - 1, (uint32_t)fb_var.yres, &band_wait_us);
	}
	for (int i = 0; i < dirty->n; i++) {
		ft.copied_px += (uint32_t)area_size(&dirty->r[i]);
		if (fb_scaled) {
			blit_scaled_2_3(page, (const uint16_t *)src, &dirty->r[i], fb_rotated);
		} else if (!banded || fb_rotated) {
			blit_rotated_180(page, (const uint16_t *)src, &dirty->r[i], (int32_t)fb_var.xres, (int32_t)fb_var.yres,
							 fb_rotated);
		} else {
			// One band at a time, each as soon as the panel has read past it.
			const lv_area_t *r = &dirty->r[i];
			for (int32_t y = r->y1; y <= r->y2; y += PAN_BAND_ROWS) {
				lv_area_t band = *r;
				band.y1 = y;
				band.y2 = y + PAN_BAND_ROWS - 1 < r->y2 ? y + PAN_BAND_ROWS - 1 : r->y2;
				panel_wait_row((uint32_t)band.y2, (uint32_t)log_h, &band_wait_us);
				blit_rotated_180(page, (const uint16_t *)src, &band, (int32_t)fb_var.xres, (int32_t)fb_var.yres,
								 fb_rotated);
			}
		}
	}

	waits.xfer_us = band_wait_us;
	waits.copy_us = (uint32_t)(ft_now_us() - t0);
	uint32_t rise_ms = waits.rise_us / 1000u, copy_ms = waits.copy_us / 1000u;
	ft.rise_sum += rise_ms;
	ft.blit_sum += copy_ms;
	ft.rise_over += waits.rise_over;
	ft.rise_max = rise_ms > ft.rise_max ? rise_ms : ft.rise_max;
	ft.blit_max = copy_ms > ft.blit_max ? copy_ms : ft.blit_max;
}

// Waits until the page it is about to fill is free, fills it with this frame's
// damage and hands it over. The wait is for the *other* page in the steady
// state, which the flip thread let go of a whole frame ago, so there is nothing
// to wait for -- and if there is, it is counted in the log's hold column.
static void snap_hand_over(const uint8_t *page, const dirty_t *dirty) {
	int idx = snap_next;
	uint64_t t0 = ft_now_us();
	pthread_mutex_lock(&flip_lock);
	while (snap_filled[idx] || snap_reading[idx]) {
		pthread_cond_wait(&flip_cond, &flip_lock);
	}
	pthread_mutex_unlock(&flip_lock);
	ft.hold_last_us = (uint32_t)(ft_now_us() - t0);

	// A plain rectangle copy, in the layout's own coordinates: the turn and the
	// shrink are the copy's business when the panel is written.
	t0 = ft_now_us();
	for (int i = 0; i < dirty->n; i++) {
		const lv_area_t *r = &dirty->r[i];
		int32_t x1 = r->x1 < 0 ? 0 : r->x1;
		int32_t y1 = r->y1 < 0 ? 0 : r->y1;
		int32_t x2 = r->x2 >= log_w ? log_w - 1 : r->x2;
		int32_t y2 = r->y2 >= log_h ? log_h - 1 : r->y2;
		if (x1 > x2 || y1 > y2) {
			continue;
		}
		size_t row = (size_t)(x2 - x1 + 1) * 2u;
		for (int32_t y = y1; y <= y2; y++) {
			size_t off = ((size_t)y * (size_t)log_w + (size_t)x1) * 2u;
			memcpy(snap_pages[idx] + off, page + off, row);
		}
	}
	ft.snap_last_us = (uint32_t)(ft_now_us() - t0);

	pthread_mutex_lock(&flip_lock);
	snap_jobs[idx] = *dirty;
	snap_filled[idx] = true;
	snap_job = idx;
	snap_next = idx ^ 1;
	pthread_cond_broadcast(&flip_cond);
	pthread_mutex_unlock(&flip_lock);
}

static void fb_pan_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
	uint64_t flush_us = ft_now_us(), hand_us = flush_us;
	bool copied = fb_rotated || fb_ram_page;

	if (copied) {
		// Collect this frame's damage; the frame goes on its way once, on the
		// last area -- handed to the flip thread when there is one to hand it
		// to, and copied and panned here when there is not.
		dirty_add(&rot_frame_dirty, area);
		if (!lv_display_flush_is_last(disp)) {
			lv_display_flush_ready(disp);
			return;
		}

		dirty_t redraw;
		uint8_t *page = fb_frame_page(&redraw);

		if (fb_pipelined) {
			// The copy rides the panel's read of the page and takes 12 of the
			// 26 ms slot; the flip thread does it, with the snapshot this hands
			// it, and this thread goes straight back to drawing (see the
			// snapshot note). Nothing here waits for the panel.
			snap_hand_over(px, &redraw);
		} else {
			// Nothing may be written while the panel's DMA still reads the page:
			// with two pages this waits for the flip thread to finish putting
			// the other one on screen, and with one page there is no other page
			// to put up, so the same call waits for the previous pan of this
			// very page. Both are the same guarantee, that the memory below is
			// off the panel.
			uint64_t wait0 = ft_now_us();
			fb_flip_wait();
			ft.hold_last_us = (uint32_t)(ft_now_us() - wait0);
			ft.snap_last_us = 0;
			fb_copy_frame(page, px, &redraw);
		}
		hand_us = ft_now_us();
	} else {
		fb_flip_wait(); // one left by a copied frame, before the rotation was turned off
		hand_us = ft_now_us();
		fb_copy_second_page = (px >= fb_mem + fb_page_size);
	}

	if (lv_display_flush_is_last(disp) && copied && flip_thread_on) {
		fb_var.yoffset = fb_copy_second_page ? fb_var.yres : 0;
		pthread_mutex_lock(&flip_lock);
		flip_var = fb_var;
		flip_pending = true;
		pthread_cond_signal(&flip_cond);
		pthread_mutex_unlock(&flip_lock);
		ft_frame_done(flush_us, hand_us, hand_us);
	} else if (lv_display_flush_is_last(disp)) {
		// The pan path itself arms a disarmed panel and asks again (see
		// fb_pan_show): a refused pan is never a reason to stop asking.
		fb_pan_show(fb_copy_second_page ? fb_var.yres : 0);
		pan_outstanding = true; // the panel is about to read the page being written
		ft_frame_done(flush_us, hand_us, ft_now_us());
	}
	lv_display_flush_ready(disp);
}

static lv_display_t *try_panned_display(void) {
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		perror("fb: /dev/fb0");
		return NULL;
	}

	struct fb_fix_screeninfo fix;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("fb: cannot read the screen info");
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	// Said out loud before anything is decided from it. The panel is the one
	// thing about this device the player cannot work out for itself, it is what
	// the whole interface is then laid out against, and it is the first thing
	// anyone needs when the interface does not appear.
	printf("fb: %ux%u (virtual %ux%u), %u bpp, stride %u, %u bytes of memory\n", fb_var.xres, fb_var.yres,
		   fb_var.xres_virtual, fb_var.yres_virtual, fb_var.bits_per_pixel, fix.line_length, fix.smem_len);

	// LVGL renders with a stride of xres * bytespp; a padded framebuffer line
	// would shear the picture, so refuse and fall back. Both 16 and 32 bits a
	// pixel are laid out tightly on the panels this player drives -- the
	// Blaze's RGB565 and the V3 Analog's BGRA -- and the copy below knows how
	// to write either one.
	uint32_t bytespp = fb_var.bits_per_pixel / 8;
	if ((bytespp != 2 && bytespp != 4) || fix.line_length != fb_var.xres * bytespp) {
		printf("fb: not 16 or 32 bits with a tight stride, so no page flipping\n");
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}
	fb_bytespp = (int)bytespp;

	fb_page_size = (size_t)fix.line_length * fb_var.yres;

	// How many whole pages the driver already has, before asking it for
	// anything: its memory divided by one screen. This is what picks between
	// the two display paths -- the V3 Analog reports one page, and its 32-bit
	// panel cannot be given a second, so for it the double-height request
	// below is skipped and the single page is panned instead.
	size_t pages = fb_page_size ? (size_t)fix.smem_len / fb_page_size : 0;
	fb_single_page = pages < 2;

	// The model may refuse the second page even where the driver offers it
	// (sysinfo.h, single_fb_page). The V3 Analog does, and for a reason that is
	// worth writing down: its panel is an SLCD whose DMA reads one page, so with
	// two the frame on the glass is the one drawn two frames ago -- the volume
	// pill appears on the next scroll and nothing else does either -- and each
	// page owes the other every change made while it was off screen, which on
	// this device measured a full-screen copy (26 ms) per frame. One page, one
	// pan, and only the damage copied.
	if (sysinfo_model() && sysinfo_model()->single_fb_page) {
		fb_single_page = true;
	}

	bool want_two = !fb_single_page || bytespp == 2;
	if (want_two) {
		// Ask for two pages.
		fb_var.yres_virtual = fb_var.yres * 2;
		fb_var.xres_virtual = fb_var.xres;
		fb_var.yoffset = 0;
		if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var) < 0) {
			close(fb_fd);
			fb_fd = -1;
			return NULL;
		}
		ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix); // smem_len may have been updated

		fb_page_size = (size_t)fix.line_length * fb_var.yres;
		if ((size_t)fix.smem_len < fb_page_size * 2) {
			printf("fb: %u bytes of memory is under the %zu two pages need, so no page flipping\n", fix.smem_len,
				   fb_page_size * 2);
			close(fb_fd);
			fb_fd = -1;
			return NULL;
		}
		fb_single_page = false;
	}
	// With one page there is nothing to ask for: the mode is left exactly as
	// the kernel set it up, so the panel is driven the way the stock firmware
	// drives it.

	fb_map_size = fb_page_size * (fb_single_page ? 1 : 2);
	fb_mem = mmap(NULL, fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	// The layout page. LVGL always draws the interface into a RAM page in
	// RGB565, and the flush copies what changed onto the panel in one pass. It
	// is never drawn straight into the page the panel is scanning: a tile
	// written there is on the glass the instant it is written, so the panel
	// shows the frame half old and half new until the whole thing is drawn --
	// the torn picture, a horizontal seam through the cover on the player and a
	// list body that is a flat fill for several frames while its rows are still
	// being built. One pass onto the page, after the frame is complete, is what
	// keeps the glass whole.
	//
	// Two cases want the page. A 32-bit panel, because LVGL's RGB565 cannot be
	// memcpy()d into a BGRA page and has to be widened by the copy. And the
	// shrunk layout, where the page is also larger than the panel. On the
	// native panel (sysinfo.h, native_panel) the page is the panel's own size
	// and the copy is one pixel in, one pixel out -- no filter and no resample,
	// the interface simply laid out at 320x480.
	log_w = (int32_t)fb_var.xres;
	log_h = (int32_t)fb_var.yres;
	const sysinfo_model_t *model = sysinfo_model();
	bool shrunk = model && model->panel_width * 2 == log_w * 3 && model->panel_height * 2 == log_h * 3;
	if (shrunk) {
		size_t size = (size_t)model->panel_width * (size_t)model->panel_height * 2;
		scale_page = malloc(size);
		if (scale_page) {
			memset(scale_page, 0, size);
			log_w = model->panel_width;
			log_h = model->panel_height;
			fb_scaled = true;
		} else {
			fprintf(stderr, "fb: no memory for the %zu-byte layout page, drawing at the panel's size\n", size);
		}
	} else if (fb_bytespp == 4) {
		// A 32-bit panel at the layout's own size -- the V3 Analog. The same
		// RAM page, one pixel in for one out, so that LVGL's RGB565 can be
		// widened into the BGRA the panel scans.
		size_t size = (size_t)log_w * (size_t)log_h * 2;
		scale_page = malloc(size);
		if (scale_page) {
			memset(scale_page, 0, size);
		} else {
			fprintf(stderr, "fb: no memory for the %zu-byte layout page at 32 bpp, drawing nothing\n", size);
		}
	}
	fb_ram_page = scale_page != NULL;

	// The snapshots the flip thread copies from: the drawn frame is handed over
	// in one of them, so the interface thread can draw the next one while the
	// panel is being written (see the snapshot note). Two pages, and only when
	// the pipeline is actually wanted: they are 300 KB each, this device has
	// about five megabytes to spare at startup, and a frame that is never handed
	// over has no use for them. Measured: the pair does not always fit, which is
	// why the answer is read before the memory is asked for.
	//
	// OFF by default ([screen] pipeline = 1 turns it on): on this unit the
	// hand-over writes garbage into the panel's page in the contexts that
	// animate (the player page opening, the control centre sliding), and the
	// garbage stays there until something redraws it -- measured, and not yet
	// understood. The smoothness it buys on a scrolling list is real (the copy
	// phase leaves the interface thread), so the code stays.
	bool want_pipeline = fb_ram_page && config_get_bool("screen", "pipeline", false);
	if (want_pipeline) {
		size_t size = (size_t)log_w * (size_t)log_h * 2;
		for (int i = 0; i < 2; i++) {
			snap_pages[i] = malloc(size);
			if (!snap_pages[i]) {
				fprintf(stderr, "fb: no memory for the %zu-byte snapshot page; the copy stays on the interface "
								"thread\n",
						size);
				for (int j = 0; j < 2; j++) {
					free(snap_pages[j]);
					snap_pages[j] = NULL;
				}
				break;
			}
			memset(snap_pages[i], 0, size);
		}
	}

	// At 32 bpp there is no direct form: without the RAM page there is nothing
	// to convert through, and letting LVGL draw RGB565 straight into the
	// panel's BGRA would put two pixels where one belongs.
	if (fb_bytespp == 4 && !fb_ram_page) {
		fprintf(stderr, "fb: 32 bpp needs a RAM page to convert through, so no page flipping\n");
		munmap(fb_mem, fb_map_size);
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	lv_display_t *disp = lv_display_create(log_w, log_h);
	if (!disp) {
		munmap(fb_mem, fb_map_size);
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	// RGB565 always, whatever the panel scans: the RAM page is the format LVGL
	// blends fastest in, and the copy is what turns it into the panel's own.
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	if (fb_ram_page) {
		// Both framebuffer pages owe a full frame.
		dirty_clear(&rot_frame_dirty);
		dirty_set_full(&rot_page_pending[0]);
		if (!fb_single_page) {
			dirty_set_full(&rot_page_pending[1]);
		}
		lv_display_set_buffers(disp, scale_page, NULL, (uint32_t)(log_w * log_h * 2), LV_DISPLAY_RENDER_MODE_DIRECT);
		if (fb_scaled && fb_single_page) {
			// The shrunk layout, said in one line so the log on the device
			// shows which of the two display paths the panel took.
			printf("fb: %u bpp, one page: the interface at %dx%d is shrunk 2/3 onto the %ux%u panel\n",
				   (unsigned)(fb_bytespp * 8), (int)log_w, (int)log_h, fb_var.xres, fb_var.yres);
		} else if (fb_scaled) {
			printf("fb: interface at %dx%d, shrunk 2/3 onto the %ux%u panel\n", (int)log_w, (int)log_h, fb_var.xres,
				   fb_var.yres);
		} else if (model && model->native_panel) {
			// The V3 Analog's path: the interface is laid out at the panel's
			// own size, in RGB565, and copied onto the BGRA page one pixel in,
			// one pixel out, in one pass per frame.
			printf("fb: %u bpp, one page: the interface is drawn at the panel's own %dx%d (RGB565, copied one to "
				   "one onto %ux%u)\n",
				   (unsigned)(fb_bytespp * 8), (int)log_w, (int)log_h, fb_var.xres, fb_var.yres);
		} else {
			printf("fb: %u bpp, %s, copied one to one onto the %ux%u panel\n", (unsigned)(fb_bytespp * 8),
				   fb_single_page ? "one page" : "two pages", fb_var.xres, fb_var.yres);
		}
	} else {
		lv_display_set_buffers(disp, fb_mem, fb_single_page ? NULL : fb_mem + fb_page_size, (uint32_t)fb_page_size,
							   LV_DISPLAY_RENDER_MODE_DIRECT);
	}
	lv_display_set_flush_cb(disp, fb_pan_flush_cb);
	lv_display_add_event_cb(disp, ft_refr_start_cb, LV_EVENT_REFR_START, NULL);
	// The pipeline needs both a snapshot page and the flip thread to hand it to,
	// and it only makes sense on one page: with two, the copy goes into the page
	// the panel is not showing and there is nothing to ride. Decided before the
	// thread starts, so it never sees the question half answered.
	fb_pipelined = snap_pages[0] != NULL && snap_pages[1] != NULL && fb_single_page;
	fb_flip_start();
	// The panel's own transfer, read from the LCDC: the copy waits for it on the
	// single-page path (see panel_transfer_start and panel_wait_row).
	lcdc_map();
	fb_display = disp;

	// The pipeline needs a flip thread to hand frames to, and the switch above
	// (read before the snapshots were asked for) has to have wanted it.
	fb_pipelined = fb_pipelined && flip_thread_on && want_pipeline;
	if (fb_pipelined) {
		printf("fb: the frame is handed to the flip thread through one of two %u KB snapshots, the copy rides the "
			   "panel's read\n",
			   (unsigned)((size_t)log_w * (size_t)log_h * 2 / 1024u));
	} else if (snap_pages[0]) {
		printf("fb: the frame is copied by the interface thread ([screen] pipeline = 1 hands it to the flip thread)\n");
	}

	if (fb_single_page) {
		printf("fb: single page, pan on FBIOPAN_DISPLAY (%ux%u)\n", fb_var.xres, fb_var.yres);
	} else {
		printf("fb: double-buffered, page flip on FBIOPAN_DISPLAY (%ux%u)\n", fb_var.xres, fb_var.yres);
	}
	return disp;
}

// A copy of what the panel is showing right now.
//
// fb_var.yoffset says which of the two pages is on screen, and the flush
// updates it at every swap: zero is the first page, yres the second. Nothing
// else is needed, not even whether rotation is on, because while rotated the
// framebuffer already holds the turned image, which is exactly what is
// visible. With one page there is no second page to be showing, so the first
// is always the one to read, whatever yoffset happens to hold.
//
// The block handed back is RGB565, the format the PNG writer wants. On the V3
// Analog's 32-bit page that is not what the framebuffer holds, so the BGRA
// pixels are unpacked and packed down to 565 here rather than copied raw.
uint16_t *display_capture_frame(int *out_w, int *out_h) {
	if (!fb_mem || fb_page_size == 0) {
		return NULL; // plain fbdev fallback, no second page
	}

	// One block, kept for the life of the process instead of allocating one
	// per shot: nearly 700 KB contiguous, which is exactly the request malloc
	// refuses on this device while Qobuz is downloading and a decoder is open.
	//
	// The caller must not free it (see screenshot.c): the block stays here.
	static uint16_t *copy;
	static size_t copy_size;
	size_t want = (size_t)fb_var.xres * (size_t)fb_var.yres * sizeof(uint16_t);
	if (!copy || copy_size != want) {
		free(copy);
		copy = malloc(want);
		copy_size = copy ? want : 0;
	}
	if (!copy) {
		fprintf(stderr, "fb: no memory for the screen copy (%zu bytes)\n", want);
		return NULL;
	}

	fb_flip_wait();
	const uint8_t *page = fb_mem + (!fb_single_page && fb_var.yoffset ? fb_page_size : 0);
	if (fb_bytespp == 4) {
		const uint8_t *src = page;
		size_t pixels = (size_t)fb_var.xres * (size_t)fb_var.yres;
		for (size_t i = 0; i < pixels; i++) {
			uint8_t b = src[i * 4], g = src[i * 4 + 1], r = src[i * 4 + 2];
			copy[i] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
		}
	} else {
		memcpy(copy, page, want);
	}

	if (out_w) {
		*out_w = (int)fb_var.xres;
	}
	if (out_h) {
		*out_h = (int)fb_var.yres;
	}
	return copy;
}

// ---------------------------------------------------------------------------
// Wake-from-blank kick (called by power.c around the wake repaint).
//
// The sysfs blank/unblank cycle tears down the panel's scan-out, and some
// drivers also reset the video mode -- which for the page-flipping display
// means losing the double-height virtual resolution, after which every
// FBIOPAN_DISPLAY to the second page fails and the panel stays black. So on
// wake the mode is put back explicitly (ACTIVATE_NOW | FORCE re-arms scan-out
// even when nothing changed) and the current page is re-panned. The plain
// fbdev fallback has its own mechanism: force_refresh makes its next flush
// issue the same re-arming FBIOPUT_VSCREENINFO.
//
// lv_linux_fbdev_set_force_refresh() must only be called on that fallback: on
// the page-flipping display it dereferences fbdev driver data the display does
// not have, and the SIGSEGV parks the process with the panel dark.
// ---------------------------------------------------------------------------

void display_wake_begin(lv_display_t *disp) {
	if (fb_fd >= 0) {
		fb_flip_wait();
		// The mode is put back with FORCE and the page panned at every wake,
		// even when the blank left the mode as it was: on the Blaze, skipping
		// it (and leaving the kick to the repaint's own flush) woke to a black
		// panel. It costs ~45 ms of a wake from a blanked panel; a wake inside
		// the fast window does not come through here.
		//
		// It is also the only thing that arms the panel again: a blank
		// disarms it, an unblank does not undo that, and the pan that
		// follows here is what puts the frame on the glass.
		fb_arm();
		if (ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var) < 0) {
			perror("fb: FBIOPAN_DISPLAY (wake)");
		}
		return;
	}
	lv_linux_fbdev_set_force_refresh(disp, true);
}

void display_wake_end(lv_display_t *disp) {
	if (fb_fd >= 0) {
		return; // the wake repaint's own flush pans to the freshly drawn page
	}
	lv_linux_fbdev_set_force_refresh(disp, false);
}

// Frees the RAM pages once the refresh that may still be reading them is
// over: an async call runs at the top of the next lv_timer_handler pass.
static void free_rot_pages_cb(void *unused) {
	(void)unused;
	if (fb_rotated) {
		return; // turned back on in the meantime
	}
	free(rot_page);
	rot_page = NULL;
}

bool display_rotation_supported(void) { return fb_display != NULL; }

// The node lv_evdev_create() actually opened. Remembered because the emulator
// must open exactly that one instead of guessing: touch is event1 on this
// device, but the event0 fallback exists because that is not guaranteed.
static char fb_touch_node[32];

const char *panel_touch_device(void) { return fb_touch_node[0] ? fb_touch_node : NULL; }

// Hands the touch panel over and takes it back. While disabled the indev is
// not polled and stops reading the node, so events pile up unread in the
// kernel queue. Whatever re-enables it must wait a moment before trusting the
// first touch (see gearboyplay.c).
void panel_touch_enable(bool enabled) {
	if (fb_touch) {
		lv_indev_enable(fb_touch, enabled);
	}
}

bool display_get_rotated(void) { return fb_rotated; }

void display_set_rotated(bool rotated) {
	if (!fb_display || rotated == fb_rotated) {
		return;
	}

	if (fb_ram_page) {
		// Already drawing into a RAM page: only the copy turns, and both
		// framebuffer pages owe a full frame the new way round.
		fb_rotated = rotated;
		dirty_clear(&rot_frame_dirty);
		dirty_set_full(&rot_page_pending[0]);
		if (!fb_single_page) {
			dirty_set_full(&rot_page_pending[1]);
		}
	} else if (rotated) {
		if (!rot_page) {
			rot_page = malloc(fb_page_size);
		}
		if (!rot_page) {
			fprintf(stderr, "fb: no memory for the rotated page (%zu bytes)\n", fb_page_size);
			return;
		}
		memset(rot_page, 0, fb_page_size);

		// Both framebuffer pages still hold the picture the right way up, so
		// they each owe a full frame.
		dirty_clear(&rot_frame_dirty);
		dirty_set_full(&rot_page_pending[0]);
		if (!fb_single_page) {
			dirty_set_full(&rot_page_pending[1]);
		}

		lv_display_set_buffers(fb_display, rot_page, NULL, (uint32_t)fb_page_size, LV_DISPLAY_RENDER_MODE_DIRECT);
		fb_rotated = true;
	} else {
		lv_display_set_buffers(fb_display, fb_mem, fb_single_page ? NULL : fb_mem + fb_page_size,
							   (uint32_t)fb_page_size, LV_DISPLAY_RENDER_MODE_DIRECT);
		fb_rotated = false;
		lv_async_call(free_rot_pages_cb, NULL);
	}

	// The touch panel turns with the picture: an inverted calibration maps
	// the top-left corner of the glass to the bottom-right of the screen.
	if (fb_touch) {
		if (fb_rotated) {
			lv_evdev_set_calibration(fb_touch, (int)fb_var.xres - 1, (int)fb_var.yres - 1, 0, 0);
		} else {
			lv_evdev_set_calibration(fb_touch, 0, 0, (int)fb_var.xres - 1, (int)fb_var.yres - 1);
		}
	}

	// Both framebuffer pages still hold the picture the other way up. One
	// invalidation is enough: the page bookkeeping above already has each
	// page owing a full frame, so the next two flips repaint them both. No
	// lv_refr_now() here, because this runs inside a settings event handler
	// and rendering from one is unsafe.
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
	lv_obj_invalidate(lv_layer_sys());

	printf("fb: rotation %s\n", fb_rotated ? "on (180 degrees)" : "off");
}

// The touch panel among /dev/input/event*.
//
// Its number is not fixed: it is the order the drivers probed in, and a touch
// controller that comes up late -- the R1's after a firmware update, with its
// loader script broken -- lands on a later one, while event1 goes to a key
// device. Opened as the pointer, that leaves a screen that looks frozen. So
// the panel is recognised by what it reports: absolute coordinates, and
// preferably the "direct" property a touchscreen sets (a touchpad does not).
// The cable remote, which also lives here, is never it.
//
// Waits up to TOUCH_PROBE_WAIT_MS for one to appear, in case it is still
// probing; false when none does.
#define TOUCH_PROBE_WAIT_MS 2000

static bool input_bit(const unsigned long *bits, int bit) {
	return (bits[bit / (8 * (int)sizeof(long))] >> (bit % (8 * (int)sizeof(long)))) & 1UL;
}

static bool find_touch_node(char *out, size_t out_size) {
	for (int waited = 0; waited <= TOUCH_PROBE_WAIT_MS; waited += 100) {
		int fallback = -1;
		for (int n = 0; n < 16; n++) {
			char node[32];
			snprintf(node, sizeof(node), "/dev/input/event%d", n);
			int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0) {
				continue;
			}
			unsigned long types[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
			unsigned long props[(INPUT_PROP_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
			char name[64] = {0};
			bool abs = ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) >= 0 && input_bit(types, EV_ABS);
			bool direct = ioctl(fd, EVIOCGPROP(sizeof(props)), props) >= 0 && input_bit(props, INPUT_PROP_DIRECT);
			ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
			close(fd);
			if (!abs || strncmp(name, "earpods_", 8) == 0) {
				continue;
			}
			if (direct) {
				snprintf(out, out_size, "%s", node);
				printf("input: %s is the touch panel (%s)\n", node, name);
				return true;
			}
			if (fallback < 0) {
				fallback = n;
			}
		}
		if (fallback >= 0) {
			snprintf(out, out_size, "/dev/input/event%d", fallback);
			printf("input: %s is the touch panel (absolute axes, no direct property)\n", out);
			return true;
		}
		usleep(100 * 1000);
	}
	fprintf(stderr, "input: no touch panel among the event devices\n");
	return false;
}

// How often the interface is redrawn, and how often the touch panel is read.
// LVGL would use LV_DEF_REFR_PERIOD for both (16 ms in lv_conf.h); the two are
// set apart here, and why is written where they are used, in
// init_target_display(). The frame period is a default only: [screen]
// frame_period_ms in device_config.ini overrides it.
//
// The touch is read more often than the interface is redrawn, and that is about
// the finger rather than the glass: a drag moves the content by whatever the
// touch said last, so a sample that is 16 ms old is 16 ms of the content
// trailing the finger. Measured on the device, the interface thread has room
// for it (one read per sample, and the panel reports at about 100 Hz).
//
// 26 ms rather than 20: the glass is handed a frame every 26.464 ms (two panel
// frames, see pan_cadence_frames), and a thread that starts drawing sooner than
// the glass can take the frames is drawing ones that will be dropped -- measured
// 0 to 9 a second that way. The draw and the copy phase together are 22 to 26 ms
// anyway, so this is about the period being honest rather than early.
#define FRAME_PERIOD_MS 26
#define TOUCH_PERIOD_MS 8

static lv_display_t *init_target_display(void) {
	// Tear-free page flipping when the framebuffer supports it...
	lv_display_t *disp = try_panned_display();

	// ...and LVGL's plain single-buffer driver when it does not.
	if (!disp) {
		fprintf(stderr, "fb: page flipping unavailable, falling back to the plain fbdev driver\n");
		disp = lv_linux_fbdev_create();
		if (!disp) {
			fprintf(stderr, "Error: Failed to create Linux framebuffer display\n");
			return NULL;
		}
		lv_linux_fbdev_set_file(disp, "/dev/fb0");
	}

	// The panel as the kernel describes it, not by its number; event1 and then
	// event0, as before, only when nothing answers to that description.
	char found[sizeof(fb_touch_node)];
	lv_indev_t *touch = NULL;
	if (find_touch_node(found, sizeof(found))) {
		touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, found);
		if (touch) {
			snprintf(fb_touch_node, sizeof(fb_touch_node), "%s", found);
		}
	}
	if (!touch) {
		touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
		if (touch) {
			snprintf(fb_touch_node, sizeof(fb_touch_node), "%s", "/dev/input/event1");
		}
	}
	if (!touch) {
		fprintf(stderr, "Warning: Failed to open /dev/input/event1. Trying event0...\n");
		touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
		if (touch) {
			snprintf(fb_touch_node, sizeof(fb_touch_node), "%s", "/dev/input/event0");
		}
	}

	if (touch) {
		lv_indev_set_display(touch, disp);
		tune_pointer(touch);
		fb_touch = touch;
		// Scaled, the raw range has to be stretched over the larger layout, and
		// it cannot come from the device: the Blaze's gt9xx declares only the
		// ABS_MT axes, so evdev reads an empty ABS_X/ABS_Y range and passes raw
		// panel coordinates through -- every touch would land at 2/3 of where
		// it is. The panel's own size is the range, as the rotation uses.
		if (fb_scaled) {
			lv_evdev_set_calibration(touch, 0, 0, (int)fb_var.xres - 1, (int)fb_var.yres - 1);
		}
		printf("Touch screen input driver successfully registered.\n");
	} else {
		fprintf(stderr, "Warning: No touch input device found.\n");
	}

	// The cadence the glass is handed frames at, in panel frames (see
	// pan_cadence_frames): two of them is the fastest this panel can take, three
	// is the same picture with room to spare.
	int cadence = (int)config_get_int("screen", "cadence_frames", 2);
	if (cadence < 2) {
		cadence = 2;
	} else if (cadence > 4) {
		cadence = 4;
	}
	pan_cadence_frames = (uint32_t)cadence;

	// The interface thread's own pace. The glass is no longer set by this
	// number: the pan is what hands it a frame, and the flip thread paces that
	// on the panel's own frame grid (see pan_cadence_frames and the flip
	// thread). What this decides is how much earlier than the pan the interface
	// thread starts drawing -- and it has to be *earlier* than the pan, or the
	// pan misses its slot and the picture drops a frame. The copy phase and the
	// draw together cost 20 to 25 ms, so twenty leaves the cadence its room; the
	// thirty-three this used to be is more than the cadence itself and would
	// make every pan late.
	//
	// Configurable, because it is a matter of taste and of what the interface
	// costs on screen: [screen] frame_period_ms in device_config.ini.
	int period = (int)config_get_int("screen", "frame_period_ms", FRAME_PERIOD_MS);
	if (period < 16) {
		period = 16;
	} else if (period > (int)(pan_cadence_frames * PANEL_FRAME_US / 1000u)) {
		period = (int)(pan_cadence_frames * PANEL_FRAME_US / 1000u);
	}
	lv_timer_set_period(lv_display_get_refr_timer(disp), (uint32_t)period);

	// The touch keeps its 16 ms, so a tap is still read twice per frame and not
	// answered a frame late. LV_DEF_REFR_PERIOD in lv_conf.h stays at 16 as
	// well: it is the default for every display and every input device LVGL
	// creates, and this is the one place that knows what this board's panel
	// costs. The simulator is left at 60 -- its frames cost a PC's time, not
	// this panel's, and there the smoother picture is the honest one.
	if (touch) {
		lv_timer_set_period(lv_indev_get_read_timer(touch), TOUCH_PERIOD_MS);
	}
	printf("fb: the interface refreshes every %d ms (%d frames a second), the touch is read every %d ms, the glass "
		   "every %u panel frames (%u ms, %u frames a second)\n",
		   period, 1000 / (period > 0 ? period : 1), TOUCH_PERIOD_MS, pan_cadence_frames,
		   pan_cadence_frames * PANEL_FRAME_US / 1000u, 1000000u / (pan_cadence_frames * PANEL_FRAME_US));

	return disp;
}
#endif

// The developer's sampling profiler (profiler.h) says when it starts and stops,
// so whoever is testing knows when to do the slow thing. Untranslated: only a
// file put on the card by hand turns it on.
static void profiler_toast_cb(void *started) { toast_plain(started ? "Perfilando..." : "Perfil guardado"); }
static void profiler_notify(bool started) { gui_post(profiler_toast_cb, started ? (void *)1 : NULL); }

#ifndef HOST_BUILD
// The kernel's real-time throttling: of every sched_rt_period_us, at most
// sched_rt_runtime_us goes to real-time threads and the rest to everything
// else. At -1 there is no limit, and one real-time thread that stops blocking
// holds the single core for good: the interface, the card's I/O and the kernel
// threads never run again, and the device resets with nothing on the card.
// Held to nine tenths of the period, so a spin slows the device down instead.
#define RT_SHARE_TENTHS 9

static long read_proc_long(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	long v = 0;
	if (fscanf(f, "%ld", &v) != 1) {
		v = 0;
	}
	fclose(f);
	return v;
}

static void limit_realtime_share(void) {
	long period = read_proc_long("/proc/sys/kernel/sched_rt_period_us");
	long runtime = read_proc_long("/proc/sys/kernel/sched_rt_runtime_us");
	if (period <= 0) {
		return;
	}
	long wanted = period / 10 * RT_SHARE_TENTHS;
	if (runtime >= 0 && runtime <= wanted) {
		return;
	}
	FILE *f = fopen("/proc/sys/kernel/sched_rt_runtime_us", "w");
	if (f) {
		fprintf(f, "%ld", wanted);
		fclose(f);
	}
}
#endif

// See the call in main().
static void *tls_warm_thread(void *arg) {
	(void)arg;
	thread_be_low_priority("tls warm-up"); // not idle class: radio pages wait on its lock
	if (!tls_available()) {
		fprintf(stderr, "tls: unavailable; https radio stations cannot be opened\n");
	}
	return NULL;
}

int main(int argc, char **argv) {
	// Before anything at all -- before the signal guards, before the allocator
	// is tuned, before a display is opened. thttpd runs this same binary as the
	// transfer page's directory listing (see src/system/webcgi.h), and in that
	// role it is a small program that prints JSON and exits. None of the player
	// belongs on that path.
	if (webcgi_should_run(argc, argv)) {
		return webcgi_main();
	}

	// On this device dying means rebooting (see above).
	install_signal_guards();

	// How the allocator behaves matters more here than anywhere else. glibc
	// raises its mmap threshold on its own once it sees large blocks being
	// freed, after which a decoded cover comes out of the arena instead of its
	// own mapping -- and the arena never shrinks. Pinning both thresholds
	// keeps every large, short-lived allocation in a mapping that is handed
	// straight back to the kernel when it is freed.
	mallopt(M_MMAP_THRESHOLD, 96 * 1024);
	mallopt(M_TRIM_THRESHOLD, 128 * 1024);
	signal(SIGINT, sigint_handler);

	logging_init();

	printf("Starting sonix_player...\n");

	// After logging_init and not before it: both of these say what they did,
	// and anything printed before the log is holding output goes to whatever
	// the launcher left on stdout, which on this device is nothing.
	//
	// Nothing between here and there maps executable pages, so the set this
	// walks is the same one it would find a few lines earlier.
#ifndef HOST_BUILD
	set_oom_priority("-500");

	lock_code_plan(); // pinned once the main loop runs: lock_code_start()

	// Before the first real-time thread starts.
	limit_realtime_share();
#endif

#ifndef HOST_BUILD
	// Straight away, before anything is opened: how much RAM the binary takes
	// and how much is left for the rest of the device. Here and not up at
	// lock_code_plan() because nothing collects stderr before logging_init().
	//
	// Printed every time, because the player sets oom_score_adj to -500: when
	// RAM runs out the kernel kills something else, and if that is a system
	// service the device restarts with nothing in the player's log to say so.
	report_memory("startup");
#endif

	// Which player this is, read before anything is decided by it: the panel
	// the pages are laid out for, the update file's name, the writable volume
	// the settings go to, the card the mixer controls are on. On the simulator
	// this also opens the window at the model's panel size, so it has to come
	// before the display either way. (The rest of what the file carries -- the
	// converter's name, the serial -- is read from the same load.)
	sysinfo_load();

	// Settings live on the UBIFS partition, the one writable place that is
	// always present: the rootfs is read-only and the card may be missing.
	// SONIX_CONFIG points it somewhere else for the host build and for testing.
	const char *config_file = getenv("SONIX_CONFIG");
#ifndef HOST_BUILD
	if (!config_file) {
		// The writable volume's name is the model's (sysinfo.h): /usr/data on
		// the HiBy boards and the Blaze, /data here. A path that does not exist
		// costs the player every setting it has -- language, time zone, volume,
		// the chosen scan folders -- silently, one "cannot write" line per save.
		static char config_path[96];
		snprintf(config_path, sizeof(config_path), "%s/device_config.ini", sysinfo_user_data_dir());
		config_file = config_path;
	}
	struct stat config_st;
	bool first_start = stat(config_file, &config_st) != 0;
#else
	// The simulator's own settings file, made once and then reused (see
	// SONIX_HOST_STATE_DIR): the first run asks for the language and the time,
	// and every run after it starts on the menu.
	mkdir("/tmp/sonix", 0755);
	mkdir(SONIX_HOST_STATE_DIR, 0755);
	static char host_config_path[128];
	if (!config_file) {
		snprintf(host_config_path, sizeof(host_config_path), "%s/device_config.ini", SONIX_HOST_STATE_DIR);
		config_file = host_config_path;
	}
#endif
	config_init(config_file);
#ifndef HOST_BUILD
	// Before the card is mounted and before either radio starts.
	factoryreset_clear_stock_data(first_start);
#endif

	// The reader's own file, beside the settings. SONIX_EBOOK_CONFIG moves it for
	// the host build the same way SONIX_CONFIG moves the other one; with neither
	// set on the host both stay unset and the reader keeps its state in memory
	// for the session.
	const char *ebook_file = getenv("SONIX_EBOOK_CONFIG");
#ifndef HOST_BUILD
	if (!ebook_file) {
		static char ebook_path[96];
		snprintf(ebook_path, sizeof(ebook_path), "%s/ebook_config.ini", sysinfo_user_data_dir());
		ebook_file = ebook_path;
	}
#else
	// Beside the simulator's own settings, so reading positions survive a
	// restart too (SONIX_HOST_STATE_DIR above).
	static char host_ebook_path[128];
	if (!ebook_file) {
		snprintf(host_ebook_path, sizeof(host_ebook_path), "%s/ebook_config.ini", SONIX_HOST_STATE_DIR);
		ebook_file = host_ebook_path;
	}
#endif
	config_store_init(config_ebook_store(), ebook_file);

	// A player updated in place has its reading positions in the main settings
	// file. They are moved once, here, and both files are written: leaving them
	// behind would reset every book on the shelf to its first page.
	if (config_store_move_section(config_main_store(), config_ebook_store(), "ebook") > 0) {
		config_store_save(config_ebook_store());
		config_save();
	}

	// The interface's own text, before any of it is built. SONIX_LANG_DIR points
	// it somewhere else for the host build, the same way SONIX_CONFIG does.
	const char *lang_dir = getenv("SONIX_LANG_DIR");
	lang_init(lang_dir && lang_dir[0] ? lang_dir : LANG_DIR);
	// The playback mode (repeat/shuffle) as it was left. Restored before the
	// GUI builds, so the repeat button is born showing the right icon.
	playlist_set_mode((playback_mode_t)config_get_int("player", "playback_mode", 0));
	// The equalizer's bands and switch, before any playback can start.
	eq_init();
	// Gapless playback, before anything can start: the engine reads this at
	// the end of every track.
	audio_set_gapless(config_get_int("audio", "gapless", 0) != 0);
	clock_init();
	adb_apply_saved_state();
	// The buttons on the headset cable: the kernel module starts disabled and
	// only remembers what was last written to it, so the configured state has
	// to be written back on every boot, on or off.
	headset_init();
	// What the three side keys do: the table is read once here, and the key
	// threads consult it on every press.
	keymap_init();

	// Which player this is. Before the display, because on the simulator the
	// window opens at the panel size of the model named in the file; on the
	// device the framebuffer answers that question itself and this is read for
	// the update file's name, the converter's name and the serial number.
	// (sysinfo_load() itself runs earlier still, before the settings path is
	// chosen from the model; this is only where the file is explained.)

	// The streaming services' application keys, and Qobuz with them. Before
	// the GUI builds its pages, because the Qobuz page is drawn one way or
	// the other depending on whether the keys are there.
	streamkeys_init();
	qobuz_init();
	tidal_init();
	lastfm_init();

	lv_init();

	// lv_init() seeds its generator with a literal (0x1234ABCD), so without
	// this the sequence is the same on every boot -- and lv_rand() picks the
	// track a shuffle starts on. The queue's own deal seeds from the clock
	// itself (playlist.c seed_once); only the starting point comes from here.
	//
	// After lv_init(), never before: it would overwrite this.
	lv_rand_set_seed((uint32_t)time(NULL) ^ (uint32_t)getpid());

	// The fonts, before anything can create a widget: every lv_font_t the
	// interface points at is filled in here, from /usr/resource/sonix/fonts through
	// FreeType -- the stock player's own mechanism. Without default.otf there
	// is no drawing so much as an error message, so that is fatal.
	if (!fonts_init()) {
#ifdef HOST_BUILD
		fprintf(stderr, "fonts: no default.otf in usr/resource/sonix/fonts or assets/fonts --\n"
						"       start the simulator from the folder that holds them\n");
		return 1;
#else
		park("fonts: /usr/resource/sonix/fonts/default.otf is missing or unreadable");
		return 1;
#endif
	}

	lv_tick_set_cb(custom_tick_get);

#ifdef HOST_BUILD
	printf("Initializing Host Build (SDL2 simulation)...\n");
	lv_display_t *disp = init_host_display();
	if (!disp) {
		return 1; // on a PC, exiting is the right thing to do
	}

	char music_path[1024];
	host_card_root(music_path, sizeof(music_path));
	fprintf(stderr, "card: %s\n", music_path);
#else
	printf("Initializing Target Build (Linux Framebuffer and EVDEV touch)...\n");
	lv_display_t *disp = init_target_display();
	if (!disp) {
		park("no framebuffer: cannot start the UI");
		return 1;
	}
#endif

	// Nothing in HiBy OS mounts the microSD on its own: mdev has no rule for
	// mmcblk, and sys_server -- the daemon that does the mounting -- only acts
	// on requests over its socket. The stock hiby_player is what sends those
	// requests, because it doubles as the hotplug manager. Replacing it means
	// taking over that job, which is what manage_mount does; the player still
	// stays out of the way if it finds the card already mounted.
	storage_config_t storage_cfg = {
		// Both NULL: the player probes the block devices and the mount-point
		// names the stock firmware uses and reports what it found in the log.
		.device = NULL,
		.mount_point = NULL,
#ifdef HOST_BUILD
		.manage_mount = false, // never touch the developer's own filesystem
#else
		.manage_mount = true,
#endif
	};

#ifdef HOST_BUILD
	// A PC has no battery here, so the status bar would always show an empty
	// shell. Point these at any two files to fake one while working on the UI:
	//   echo 80 > /tmp/cap; echo Charging > /tmp/stat
	//   SONIX_BATTERY_CAPACITY=/tmp/cap SONIX_BATTERY_STATUS=/tmp/stat ./sonix_player_host
	battery_config_t battery_cfg = {
		.battery_capacity_file = getenv("SONIX_BATTERY_CAPACITY"),
		.battery_status_file = getenv("SONIX_BATTERY_STATUS"),
	};
#else
	// Both NULL: the player enumerates /sys/class/power_supply and picks the
	// battery and the chargers out by their `type`, the same way the stock
	// firmware does. Hardcoding "battery" and "mp2731-charger" would work today
	// and break on the next revision.
	battery_config_t battery_cfg = {
		.battery_capacity_file = NULL,
		.battery_status_file = NULL,
	};
#endif

	system_config_t system_cfg = {
		.battery_cfg = &battery_cfg,
		.storage_cfg = &storage_cfg,
	};

	system_start_services(&system_cfg, post_gui_popup, NULL);

#ifndef HOST_BUILD
	logging_attach_sd(storage_sd_root());
	btlog_card_attach(storage_sd_root());
	// Now that the log is on the card: whatever the kernel said about the
	// previous run, which is the only place a SIGKILL leaves a trace.
	logging_report_previous_run();
#endif

	// The panel: the model, not the framebuffer. See panel_size().
	int panel_w = 0, panel_h = 0;
#ifndef HOST_BUILD
	int fb_w = (int)lv_display_get_horizontal_resolution(disp);
	int fb_h = (int)lv_display_get_vertical_resolution(disp);
	panel_size(&panel_w, &panel_h, fb_w, fb_h);

	// What the framebuffer says it is, beside what the interface is being laid
	// out at. They should agree; when they do not, this line is the one that
	// says so, and it costs nothing to print once.
	if (fb_w != panel_w || fb_h != panel_h) {
		printf("panel: the framebuffer reports %dx%d, the model says %dx%d -- laying out at the model's\n", fb_w,
			   fb_h, panel_w, panel_h);
	}
#else
	panel_size(&panel_w, &panel_h, 0, 0);
#endif

	{
		const sysinfo_model_t *model = sysinfo_model();
		printf("panel: laying the interface out at %dx%d (%s)\n", panel_w, panel_h,
			   model ? model->name : "no model this build knows, so the default");
	}

	gui_config_t gui_cfg = {
		.screen_width = (uint32_t)panel_w,
		.screen_height = (uint32_t)panel_h,
		// Both are panel pixels, so both go through SC(): 44 and 15 on the
		// 480x720 panels, 29 and 10 on the V3 Analog's 320x480. The pages read
		// them straight -- cfg->screen_width - 2 * SC(15) mixes the two
		// correctly, the config already in panel pixels and the literal turned
		// into them.
		.top_bar_height = (int8_t)SC(44),
		.padding = (int8_t)SC(15),
#ifdef HOST_BUILD
		.sd_root_path = music_path,
#else
		// Resolved by the mount code above rather than hardcoded: /mnt/sd_0,
		// /data/mnt/sd_0 and /usr/data/mnt/sd_0 are the same directory reached
		// through different symlinks, and which one is real varies by firmware.
		.sd_root_path = storage_sd_root(),
#endif
	};

	// Artwork thumbnails are kept on the card, next to the music, so that
	// browsing a library only ever decodes each cover once -- the same trick
	// the stock player uses with its own .hiby/.../cache folder.
	cover_set_cache_dir(gui_cfg.sd_root_path);
	profiler_set_notify(profiler_notify);
	profiler_start(gui_cfg.sd_root_path);

	// The shapes of tracks, for the player's alternative layout. The worker
	// sleeps until a page asks for one, so starting it costs a thread and
	// nothing else.
	waveform_set_cache_dir(gui_cfg.sd_root_path);
	waveform_start();

	// User playlists: plain .m3u files in <card>/Playlist.
	playlists_init(gui_cfg.sd_root_path);

	// And the tree "play by folder" is allowed to walk: the same card, so the
	// walk cannot wander off it.
	playlist_set_card_root(gui_cfg.sd_root_path);

	// MSEB presets: one small file each in <card>/.local/MSEB.
	mseb_presets_set_dir(gui_cfg.sd_root_path);
	eq_presets_set_dir(gui_cfg.sd_root_path);
	peq_presets_set_dir(gui_cfg.sd_root_path);

	// The music index, beside the thumbnails. Same schema as the stock
	// player's, so the two can read each other's database.
	if (gui_cfg.sd_root_path && gui_cfg.sd_root_path[0]) {
		// A card that cannot be written to keeps its databases in /tmp instead.
		// They do not survive a reboot, so the library has to be scanned again
		// each time -- but with no library.db at all, library_scan_start()
		// gives up at its first line and the scan button does nothing, for any
		// database and with no message. The thumbnail cache falls back the
		// same way.
		const char *db_root = gui_cfg.sd_root_path;
		if (!storage_sd_writable()) {
			db_root = "/tmp";
			mkdir("/tmp/.local", 0755);
			fprintf(stderr, "main: the card is read-only, so the databases live in /tmp for this session\n");
		}

		// Before the index opens, because the collation it registers reads it.
		library_set_skip_articles(config_get_int("player", "skip_articles", 1) != 0);

		char db_path[600];
		snprintf(db_path, sizeof(db_path), "%s/.local/library.db", db_root);
		library_open(db_path);
		// The same file the app downloads: one index, served as it is.
		sonixlink_set_db_path(db_path);

		// The thumbnails the player has already made for its own lists: the app
		// downloads that same file rather than having a second one built for it.
		char thumbs_path[600];
		snprintf(thumbs_path, sizeof(thumbs_path), "%s/.local/thumbnails", db_root);
		sonixlink_set_thumbs_path(thumbs_path);

		// The audiobook index lives beside it, in its own file.
		snprintf(db_path, sizeof(db_path), "%s/.local/audiobooks.db", db_root);
		audiobookdb_open(db_path);

		// And the radio's own: the starred stations and the last few played.
		radio_store_open(db_root);
	}

	// TLS, so an https:// station can be opened at all. Warmed up here rather
	// than on first use for two reasons: the first call parses a ~200 KB
	// certificate bundle, which on this CPU is long enough to be felt if it
	// happens while a list is being drawn, and the log line it prints says
	// which bundle was found -- which is the first thing to look at when
	// https stations fail.
	tls_set_card_root(gui_cfg.sd_root_path);
	// Where downloaded stream tracks go. The real card is used -- the same
	// root the Qobuz page and the card watcher use -- and not
	// gui_cfg.sd_root_path, which on the simulator is a different folder: the
	// cache and the code that cleans it must agree on one root.
	const char *card_root = storage_sd_root();
	qobuzcache_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	tidalcache_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	podcastcache_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	podcastsubs_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	// And where a track arriving over DLNA lands: same card, same reason.
	dlna_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	// And where the emulator looks for Games/GB and Games/GBC. Unlike the
	// lines above this uses gui_cfg.sd_root_path and not storage_sd_root():
	// those write a cache, which belongs on the real card, while games are
	// the user's own files and live where the user's music lives. On the
	// device the two are the same (see how gui_cfg is filled in); on the
	// simulator they are not, which is what makes the page testable without
	// the device.
	gbdb_set_root(gui_cfg.sd_root_path);

	// Rows pointing into the stream caches are dropped once at boot: a cache
	// file may be gone, and starring a streamed track goes to the account
	// rather than to the local database.
	library_forget_under(qobuzcache_dir());
	library_forget_under(tidalcache_dir());
	// OpenSSL is loaded on a worker below the interface: the libraries and the
	// certificate bundle take a quarter of a second on this core, and nothing
	// needs them until a stream is opened, which waits on the loader's lock.
	{
		pthread_t tls_thread;
		if (pthread_create(&tls_thread, NULL, tls_warm_thread, NULL) == 0) {
			pthread_detach(tls_thread);
		} else {
			fprintf(stderr, "tls: no thread for the warm-up; loaded on first use\n");
		}
	}

	// The radios, before the interface: both read their switch out of the
	// config here, and the Wireless pages are built from those switches, so a
	// Wi-Fi left on is drawn as on from the first frame. Each starts a worker
	// and returns at once; if a switch was on, the bring-up happens on that
	// worker while the interface carries on drawing.
	wifi_init();
	// Before the radio, so a bring-up that fails is in the Bluetooth log from
	// its first line.
	btlog_init();
	// The Bluetooth stack is not ported to the V3 Analog yet: the firmware's own
	// one is what runs there, with its own modules, scripts and firmware files,
	// and the port's management of it (bring it down at boot, run its own) has
	// nothing to talk to. Left running, it reads that stack as "the radio is on"
	// -- which it is, just not this player's -- and retries a media-player
	// registration against it every three seconds, forever, each attempt a failed
	// D-Bus round trip and four lines written to the log on the card. The model
	// says whether the stack is this port's (sysinfo.h, bluetooth_stack).
	if (!sysinfo_model() || sysinfo_model()->bluetooth_stack) {
		bluetooth_init();
	} else {
		printf("bluetooth: not this port's stack on this model; left alone\n");
	}
	// Its worker starts here but stays idle: no socket is opened until the
	// switch is on, which the same config read decides.
	sonixlink_init();

	// The sleep timers, as they were left. Before the interface: the music,
	// audiobook and podcast pages read their switch and their wheels while
	// they are built, and read before this they all say off, 0 minutes.
	sleeptimer_init();

	gui_init(&gui_cfg);

	// "Rotate screen", as it was left. After gui_init so there are screens to
	// redraw, and before the first frame the user sees.
	if (config_get_int("screen", "rotate_180", 0)) {
		display_set_rotated(true);
	}

	// The USB-C port, allowed to take a peripheral. Before the volume restore
	// below, so that a DAC already plugged in at boot is found and the level
	// lands on it rather than on a CS43198 nobody is listening to.
	usbaudio_init();
	usbaudio_poll();

	// "Remember volume": the saved levels replace audio_init's default. Every
	// output's, not only the one in use, because which output is in force at
	// boot depends on what is plugged in or connected: loading one level would
	// start another output at the wrong level and then write that level over
	// the right one.
	//
	// The two sockets fall back to the 3.5 mm level and USB-C to whatever the
	// jack was left at: a player updating from a build that kept one level for
	// all of them comes up exactly where it was, and the levels part company
	// from the first time each output is used.
	if (config_get_int("player", "remember_volume", 0)) {
		int wired = (int)config_get_int("player", "volume", get_volume_percent());
		int balanced = (int)config_get_int("player", "volume_balanced", wired);
		int usb = (int)config_get_int("player", "volume_usb", wired);
		int bt = (int)config_get_int("wireless", "bt_volume", wired);
		volume_profile_init(wired, bt);
		volume_profile_set_level(VOLUME_OUTPUT_BALANCED, balanced);
		volume_profile_set_level(VOLUME_OUTPUT_USB, usb);

		// Which one the player is actually on right now. The routing watcher
		// would settle it within a second anyway, but a second of the wrong
		// loudness in a pair of headphones is exactly the second that matters.
		int level = wired;
		if (bluetooth_audio_active()) {
			level = bt;
			volume_profile_set_output(VOLUME_OUTPUT_BLUETOOTH);
		} else if (usbaudio_active()) {
			level = usb;
			volume_profile_set_output(VOLUME_OUTPUT_USB);
		} else if (headphone_jack_state() == JACK_BALANCED) {
			level = balanced;
			volume_profile_set_output(VOLUME_OUTPUT_BALANCED);
		}
		set_volume_percent(level);
	}

	// "Remember track", once the first frames are on screen (see the timer).
	//
	// Except with "Show Now Playing at startup" (Music > Display options): then
	// the first frame is the now-playing page, so the track is put back here,
	// before it, and the page opened over the home screen without the slide.
	// The artwork still arrives from its worker a moment later.
	if (config_get_int("player", "remember_track", 0)) {
		if (config_get_bool("music", "nowplaying_at_boot", false)) {
			if (restore_saved_track()) {
				player_sheet_open(false);
			}
		} else {
			lv_timer_create(restore_track_timer_cb, 600, NULL);
		}
	}


#ifdef HOST_BUILD
	lv_timer_create(host_shot_poll_cb, 200, NULL); // see host_shot_poll_cb
#endif

	// The media player bluez shows to the headphones. After gui_init, because
	// the transport commands it receives are sent onto the GUI thread; it does
	// nothing at all until Bluetooth is switched on -- and on a model whose
	// Bluetooth stack is not this port's (sysinfo.h, bluetooth_stack) it is not
	// started either, for the reason given at bluetooth_init() above.
	if (!sysinfo_model() || sysinfo_model()->bluetooth_stack) {
		btplayer_init();
	}

	// The status LED: aqua from power-on, exactly like the stock firmware.
	led_init();

#ifndef HOST_BUILD
	// USB mass storage: hand the card to a PC when one is attached, exactly
	// like the stock firmware (see usb.h).
	usb_start(storage_sd_device(), storage_sd_root());
#endif

	// power / display idle management (backlight, screen-off, SoC suspend).
	// On the host build every device path is left NULL so nothing touches the
	// developer's own backlight/framebuffer/suspend.
#ifndef HOST_BUILD
	// The backlight node is named by the kernel driver, not by the player: the
	// HiBy boards register "backlight_pwm0", the V3 Analog "pwm-backlight.0".
	// The model says which (sysinfo.h, backlight_dir). Getting it wrong is the
	// quietest failure in the whole player: every write goes nowhere, the panel
	// keeps whatever level the process before it left it at -- often nought,
	// because a player that is killed on the way out does not put it back --
	// and the screen looks dead while the interface behind it is drawing
	// perfectly well, frame after frame, into a framebuffer nobody can see.
	static char brightness_path[96];
	static char max_brightness_path[96];
	{
		const sysinfo_model_t *model = sysinfo_model();
		const char *dir = (model && model->backlight_dir) ? model->backlight_dir
														  : "/sys/class/backlight/backlight_pwm0";
		snprintf(brightness_path, sizeof(brightness_path), "%s/brightness", dir);
		snprintf(max_brightness_path, sizeof(max_brightness_path), "%s/max_brightness", dir);
	}
#endif

	power_config_t power_cfg = {
#ifdef HOST_BUILD
		.brightness_path = NULL,
		.max_brightness_path = NULL,
		.blank_path = NULL,
#else
		.brightness_path = brightness_path,
		.max_brightness_path = max_brightness_path,
		// Screen on/off through the framebuffer blank node -- the same
		// mechanism Rockbox's HiBy port uses, and right where a blank is
		// really just a blank. Not on the V3 Analog: its stock player writes
		// no blank at all (see sysinfo.h, blank_panel), because on that panel
		// the blank leaves the SLCDC disarmed in a way an unblank does not
		// undo -- and the glass then shows nothing until something
		// power-cycles the panel.
		.blank_path = (sysinfo_model() && sysinfo_model()->blank_panel) ? "/sys/class/graphics/fb0/blank" : NULL,
		// ...and, where the stock firmware powers the panel down itself, its
		// own ioctl instead: SET_SLEEP on /dev/fb0 (docs/PANEL-POWER.md). One
		// value from the board profile; 0 on every model that has no such path.
		.panel_sleep_ioctl = sysinfo_model() ? sysinfo_model()->panel_sleep_ioctl : 0,
		// The suspend-to-RAM prototype writes "mem" here. NULL on the host
		// build like every other device path, so nothing touches the
		// developer's own machine.
		.power_state_path = "/sys/power/state",
#endif
		.brightness = -1, // adopt the panel's current level as the on-brightness
		// Overwritten just below from the Screen page's setting; the values
		// here are that setting's own default (SCREEN_OFF_DEFAULT_S) so this
		// struct cannot claim a timeout the player is not using.
		.screen_off_enabled = true,
		.screen_off_timeout_ms = SCREEN_OFF_DEFAULT_S * 1000,
	};
	// Whatever was chosen in Settings last time.
	power_cfg.screen_off_timeout_ms = (uint32_t)config_get_int("power", "screen_off_seconds", SCREEN_OFF_DEFAULT_S) * 1000;
	power_cfg.screen_off_enabled = (power_cfg.screen_off_timeout_ms > 0);
	// The framebuffer the panel sleep ioctl goes to, opened by the display path
	// above; -1 on the host, where there is none.
#ifdef HOST_BUILD
	power_cfg.fb_fd = -1;
#else
	power_cfg.fb_fd = fb_fd;
#endif
	// The brightness picked in Settings > Screen; -1 (not set yet) keeps
	// the adopt-the-panel's-level behaviour above.
	power_cfg.brightness = config_get_int("screen", "brightness", -1);
	// Double-tap wake as it was left: the touch controller's gesture switch
	// must be told again on every boot.
	power_set_double_tap_wake(config_get_int("screen", "double_tap_wake", 0) != 0);

	power_init(&power_cfg, disp);

	// Charge limit, automatic shutdown and standby, as they were left. The
	// screen timeout is the Screen page's setting and comes from there.
	powersettings_apply();
	settings_apply_screen_off();

#ifndef HOST_BUILD
	// Bluetooth off. A firmware whose /etc/init.d/S80_bt_init has not been
	// replaced powers the radio up at every boot -- rfkill on, patchram,
	// bluetoothd, bluealsa -- and with the switch off nothing in this player
	// talks to it, so it is current spent on nothing. Done on a thread of its
	// own because it waits for the daemons to go, and the interface must not
	// stall for it.
	//
	// Not when Bluetooth is switched on in Wireless: the stack that is already
	// up is the one bluetooth_init() is about to want.
	if (config_get_int("power", "bluetooth_off", 1) && !config_get_int("wireless", "bluetooth", 0)) {
		pthread_t bt_thread;
		if (pthread_create(&bt_thread, NULL, bluetooth_off_thread, NULL) == 0) {
			pthread_detach(bt_thread);
		}
	}
#endif

	// The main event loop: run whatever LVGL timers are due, then sleep until
	// the next one is.
	//
	// The sleep is capped, and that cap is not a nicety. lv_timer_handler()
	// returns LV_NO_TIMER_READY (0xFFFFFFFF) when NO timer is scheduled at
	// all -- every one of them paused, which is exactly what the standby path
	// arranges. Multiplied by 1000 in 32-bit arithmetic that wraps to about
	// 2295 seconds, so uncapped the player would go deaf and blind for
	// thirty-eight minutes: the panel frozen on whatever it last drew, touches
	// unread (the input device is polled by a timer too), the clock stopped.
	//
	// A quarter of a second is the ceiling because that is the standby budget
	// this player runs to: four wakeups a second with nothing to do.
	//
	// The wait is a poll() on the gui_post wake descriptor rather than a
	// usleep() (see gui.c), so a thread that posts wakes the loop at once and
	// nothing wakes it while the queue is empty. With no descriptor (eventfd
	// not opened) poll() on a negative fd is exactly a timed sleep, and the
	// bridge timer polls fast in that case.
	watchdog_start();
	// On only when asked for: <card>/.local/frameprof, or the simulator's
	// variable, which also lowers the threshold (the computer is many times
	// faster than the device).
	frameprof_init(frameprof_wanted(), getenv("SONIX_FRAMEPROF") ? (unsigned)atoi(getenv("SONIX_FRAMEPROF")) : 0);

#ifndef HOST_BUILD
	lock_code_start();
#endif

	printf("Entering main event loop...\n");
	while (running) {
		ui_heartbeat++;
		frameprof_pass_begin();
		uint32_t time_till_next = lv_timer_handler();
		frameprof_pass_end();
		if (time_till_next > MAIN_LOOP_MAX_SLEEP_MS) {
			time_till_next = MAIN_LOOP_MAX_SLEEP_MS; // also catches LV_NO_TIMER_READY
		}

		struct pollfd wake = {.fd = gui_post_wake_fd(), .events = POLLIN};
		if (poll(&wake, 1, (int)time_till_next) > 0) {
			// Something was posted: drain the queue now instead of waiting
			// for the bridge's own timer to come round.
			gui_post_service();
		}
	}

	printf("\n");

	return 0;
}
