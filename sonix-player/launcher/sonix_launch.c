// sonix_launch: starts the player and reboots the device when it exits.
//
//     sonix_launch [/path/to/player [args...]]
//
// sonix_player.sh ends with `exec sonix_launch /usr/bin/sonix_player`, so this
// replaces the shell that would otherwise sit in memory for the whole session
// waiting to run `sleep 1; reboot`. It does exactly that and nothing else: the
// player runs as its child with the same environment, limits and signal
// dispositions, and whatever ends it (an exit, a kill, a loader that refuses
// the binary) is followed by one second and a reboot through init.
//
// Freestanding: no libc, raw o32 system calls, one static binary of a few kB
// that depends on nothing in the rootfs. MIPS o32 Linux only.

#include <asm/unistd.h>

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define EINTR 4

#define DEFAULT_PLAYER "/usr/bin/sonix_player"

// Where the launcher's own line about the end of a session is kept.
//
// It writes to fd 2, and on this device fd 2 is the console: nothing keeps it.
// The one question after a reboot -- did the player exit, or was it killed, and
// with which signal -- was therefore unanswerable, and a device that restarts
// itself at night is exactly when that question is asked. /data is the only
// place that survives a reboot (the log is on the card, which the reboot
// unmounts), so the same sentence goes there as well.
#define LAUNCH_LOG "/data/sonix-launch.log"
// The MIPS values, not the x86 ones: this is raw o32, so there is no libc to
// translate them. `asm/fcntl.h` in the toolchain's sysroot is the source.
#define O_WRONLY 0x0001
#define O_CREAT 0x0100
#define O_APPEND 0x0008

// o32: number in $2, arguments in $4-$7. On return $2 holds the result and $7
// is non-zero when $2 is an errno, handed back here as a negative value.
static long sys3(long n, long a, long b, long c) {
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7");
	register long r2 __asm__("$2");
	__asm__ __volatile__("addu $2, $0, %2\n\tsyscall"
						 : "=&r"(r2), "=r"(r7)
						 : "ir"(n), "r"(r4), "r"(r5), "r"(r6)
						 : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi",
						   "lo", "memory");
	return r7 && r2 > 0 ? -r2 : r2;
}

static long sys4(long n, long a, long b, long c, long d) {
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7") = d;
	register long r2 __asm__("$2");
	__asm__ __volatile__("addu $2, $0, %2\n\tsyscall"
						 : "=&r"(r2), "+r"(r7)
						 : "ir"(n), "r"(r4), "r"(r5), "r"(r6)
						 : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi",
						   "lo", "memory");
	return r7 && r2 > 0 ? -r2 : r2;
}

static long launch_log = -1;

static void say(const char *s) {
	long n = 0;
	while (s[n]) {
		n++;
	}
	sys3(__NR_write, 2, (long)s, n);
	if (launch_log >= 0) {
		sys3(__NR_write, launch_log, (long)s, n);
	}
}

static void say_num(unsigned v) {
	char buf[12];
	int i = sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i > 0);
	say(buf + i);
}

static void run(const char *path, char **argv, char **envp) {
	sys3(__NR_execve, (long)path, (long)argv, (long)envp);
}


// ---------------------------------------------------------------------------
// The over-the-air half
//
// docs/PLAYER-UPDATE.md has the whole picture; this is the part that has to
// survive the player. A package the player has already verified sits on the card
// as <card>/sonix/player.new with its digest in player.sha256 and a marker in
// pending. Before the player is started -- so with no process holding the binary
// in the rootfs -- this does, in order:
//
//   * a lock in /data that says a swap is in progress;
//   * a copy of what is there now, on the card, verified by reading it back;
//   * the swap itself: the old file is unlinked, the new one copied in, synced,
//     chmod 755, and read back to compare the digest.
//
// A player that comes up writes /data/update.ok; that is the only thing that
// finishes the transaction. So on the next boot: ok + lock -> the update landed,
// clean up the card; lock without ok -> the new player never got that far, put
// the backup back. Nothing here needs the player to be running, which is why the
// swap can be done at all: the file is not mapped.
//
// Deliberately dumb: no sha256 here (the player's --verify does it, and a second
// copy of that code would be a second thing to get wrong), no JSON in here. A
// file whose digest the player has confirmed is the only thing this installs.
// ---------------------------------------------------------------------------

#define CARD_DIR "/mnt/sd_0/sonix"
#define PENDING CARD_DIR "/pending"
#define PAYLOAD CARD_DIR "/player.new"
#define PAYLOAD_SHA CARD_DIR "/player.sha256"
#define PREVIOUS CARD_DIR "/player.prev"
#define UPDATE_TAR CARD_DIR "/update.tar"
#define LOCK_FILE "/data/update.lock"
#define PLAYER_BIN "/usr/bin/sonix_player"

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT 0x0100
#define O_TRUNC 0x0200

#define EINTR 4

static char io_buf[65536];

static int streq(const char *a, const char *b) {
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int has_prefix(const char *text, const char *prefix) {
	while (*prefix) {
		if (*text != *prefix) {
			return 0;
		}
		text++;
		prefix++;
	}
	return 1;
}

// Reads a whole small file into `out`; false when it is not there.
static int read_small_file(const char *path, char *out, int size) {
	long fd = sys3(__NR_open, (long)path, O_RDONLY, 0);
	if (fd < 0) {
		return 0;
	}
	long n = sys3(__NR_read, fd, (long)out, size - 1);
	sys3(__NR_close, fd, 0, 0);
	if (n <= 0) {
		return 0;
	}
	out[n] = 0;
	return 1;
}

// "... \"build_version\": \"1.3.0/70b798e7\" ..." -> "1.3.0/70b798e7".
//
// The rootfs names its own build there, and the update lock remembers what it was
// when the swap began. A flash rewrites the rootfs and leaves /data alone, so a
// lock whose build is not the current one belongs to a rootfs that no longer
// exists: nothing must be restored from it.
static void rootfs_build_id(char *out, int size) {
	out[0] = 0;
	static char text[1024];
	if (!read_small_file("/usr/resource/sonix/components/system-info.json", text, sizeof(text))) {
		return;
	}
	const char *key = "\"build_version\"";
	const char *p = text;
	while (*p && !has_prefix(p, key)) {
		p++;
	}
	if (!*p) {
		return;
	}
	while (*p && *p != ':') {
		p++;
	}
	while (*p && *p != '"') {
		p++;
	}
	if (!*p) {
		return;
	}
	p++;
	int n = 0;
	while (*p && *p != '"' && n < size - 1) {
		out[n++] = *p++;
	}
	out[n] = 0;
}

static int file_exists(const char *path) {
	long fd = sys3(__NR_open, (long)path, O_RDONLY, 0);
	if (fd < 0) {
		return 0;
	}
	sys3(__NR_close, fd, 0, 0);
	return 1;
}

static int read_small(const char *path, char *out, int size) {
	long fd = sys3(__NR_open, (long)path, O_RDONLY, 0);
	if (fd < 0) {
		return 0;
	}
	long n = sys3(__NR_read, fd, (long)out, size - 1);
	sys3(__NR_close, fd, 0, 0);
	if (n <= 0) {
		return 0;
	}
	out[n] = 0;
	// A sha256sum line is "digest  name": the digest is the first word.
	for (long i = 0; i < n; i++) {
		if (out[i] == ' ' || out[i] == '\t' || out[i] == '\n' || out[i] == '\r') {
			out[i] = 0;
			break;
		}
	}
	return out[0] != 0;
}

// Copy with a read-back that has to agree, because the card is the only thing
// standing between a bad update and a device without a player.
static int copy_file(const char *from, const char *to, long mode) {
	long in = sys3(__NR_open, (long)from, O_RDONLY, 0);
	if (in < 0) {
		return 0;
	}
	long out = sys3(__NR_open, (long)to, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) {
		sys3(__NR_close, in, 0, 0);
		return 0;
	}
	long total = 0;
	for (;;) {
		long n = sys3(__NR_read, in, (long)io_buf, sizeof(io_buf));
		if (n < 0) {
			if (n == -EINTR) {
				continue;
			}
			break;
		}
		if (n == 0) {
			break;
		}
		long off = 0;
		while (off < n) {
			long w = sys3(__NR_write, out, (long)(io_buf + off), n - off);
			if (w <= 0) {
				if (w == -EINTR) {
					continue;
				}
				sys3(__NR_close, in, 0, 0);
				sys3(__NR_close, out, 0, 0);
				return 0;
			}
			off += w;
		}
		total += n;
	}
	sys3(__NR_fsync, out, 0, 0);
	sys3(__NR_close, in, 0, 0);
	sys3(__NR_close, out, 0, 0);
	sys3(__NR_chmod, (long)to, mode, 0);
	sys3(__NR_sync, 0, 0, 0);
	return total > 0;
}

// /usr/bin/sonix_player --verify <file> <sha256>: the player does the hashing and
// the board check, we only read its exit status.
static int verify_payload(const char *path, const char *sha, char **envp) {
	long pid = sys3(__NR_fork, 0, 0, 0);
	if (pid == 0) {
		static char player[] = PLAYER_BIN;
		static char flag[] = "--verify";
		char *args[] = {player, flag, (char *)path, (char *)sha, 0};
		sys3(__NR_execve, (long)player, (long)args, (long)envp);
		sys3(__NR_exit, 127, 0, 0);
	}
	if (pid < 0) {
		return 0;
	}
	int status = 0;
	long r;
	do {
		r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	} while (r == -EINTR);
	return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0;
}

// "build=<id>" out of the lock, or an empty string.
static void lock_build_id(char *out, int size) {
	out[0] = 0;
	static char text[256];
	if (!read_small_file(LOCK_FILE, text, sizeof(text))) {
		return;
	}
	const char *p = text;
	while (*p && !has_prefix(p, "build=")) {
		p++;
	}
	if (!*p) {
		return;
	}
	p += 6;
	int n = 0;
	while (*p && *p != '\n' && *p != '\r' && n < size - 1) {
		out[n++] = *p++;
	}
	out[n] = 0;
}

static void write_lock(const char *sha) {
	long fd = sys3(__NR_open, (long)LOCK_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		return;
	}
	char line[96];
	int n = 0;
	const char *p = "new=";
	while (*p) {
		line[n++] = *p++;
	}
	for (int i = 0; sha[i] && n < 90; i++) {
		line[n++] = sha[i];
	}
	line[n++] = '\n';
	// The build this lock belongs to: a flash replaces the rootfs and leaves
	// /data alone, and a lock from the rootfs that is gone must not restore
	// anything. See lock_build_id() and the check below.
	char build[64];
	rootfs_build_id(build, sizeof(build));
	const char *b = "build=";
	while (*b && n < (int)sizeof(line) - 1) {
		line[n++] = *b++;
	}
	for (int i = 0; build[i] && n < (int)sizeof(line) - 1; i++) {
		line[n++] = build[i];
	}
	line[n++] = '\n';
	sys3(__NR_write, fd, (long)line, n);
	sys3(__NR_fsync, fd, 0, 0);
	sys3(__NR_close, fd, 0, 0);
	sys3(__NR_sync, 0, 0, 0);
}

static void drop(const char *path) { sys3(__NR_unlink, (long)path, 0, 0); }

// Called once, before the player is started. Returns nothing: whatever happened,
// a player is started afterwards, because a device that does not start its
// player is a device that only recovery can fix.
static void apply_pending_update(char **envp) {
	int have_lock = file_exists(LOCK_FILE);
	int have_pending = file_exists(PENDING);
	say("sonix_launch: update state lock=");
	say_num((unsigned)have_lock);
	say(" pending=");
	say_num((unsigned)have_pending);
	say("\n");

	if (have_lock) {
		char lock_build[64], now_build[64];
		lock_build[0] = 0;
		now_build[0] = 0;
		lock_build_id(lock_build, sizeof(lock_build));
		rootfs_build_id(now_build, sizeof(now_build));

		if (!lock_build[0] || !now_build[0] || !streq(lock_build, now_build)) {
			// A flash rewrote the rootfs after this lock was written; the update it
			// describes is history, and restoring its backup would put the image's
			// own player back to something older than what was just flashed.
			say("sonix_launch: a lock from another rootfs (the rootfs was replaced), dropping it\n");
		} else if (copy_file(PREVIOUS, PLAYER_BIN, 0755)) {
			say("sonix_launch: rolled back to the previous player\n");
		} else {
			say("sonix_launch: the update failed and there is no backup on the card\n");
		}
		drop(LOCK_FILE);
		drop(PENDING);
		drop(PAYLOAD);
		drop(PAYLOAD_SHA);
		drop(PREVIOUS);
		drop(UPDATE_TAR);
	}

	if (!file_exists(PENDING)) {
		return;
	}

	static char sha[80];
	if (!read_small(PAYLOAD_SHA, sha, sizeof(sha))) {
		say("sonix_launch: pending without a digest, dropping it\n");
		drop(PENDING);
		return;
	}
	if (!verify_payload(PAYLOAD, sha, envp)) {
		say("sonix_launch: package refused by --verify\n");
		drop(PENDING);
		drop(PAYLOAD);
		drop(PAYLOAD_SHA);
		return;
	}

	// The backup before the swap: a card that cannot hold the old binary cannot
	// hold a rollback either, and an update with no way back is not one.
	if (!copy_file(PLAYER_BIN, PREVIOUS, 0755)) {
		say("sonix_launch: cannot back up the player, update dropped\n");
		drop(PENDING);
		return;
	}

	write_lock(sha);

	drop(PLAYER_BIN);
	if (copy_file(PAYLOAD, PLAYER_BIN, 0755)) {
		say("sonix_launch: installed the new player, starting it\n");
	} else {
		// Half a binary in place is worse than the old one: put the backup back
		// and forget the transaction, so the next boot is an ordinary one.
		if (copy_file(PREVIOUS, PLAYER_BIN, 0755)) {
			say("sonix_launch: the copy failed, the previous player is back\n");
		} else {
			say("sonix_launch: the copy failed and the backup did not come back\n");
		}
		drop(LOCK_FILE);
		drop(PENDING);
	}
}

__attribute__((noreturn, used)) void launch_main(long *sp) {
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;

	static char default_player[] = DEFAULT_PLAYER;
	static char *default_argv[] = {default_player, 0};
	char **player_argv = argc > 1 ? argv + 1 : default_argv;

	// Opened before anything is said, so the "cannot start" line is kept too.
	// Mode 0644: root's, and readable when the card is mounted on a desk.
	// open(2), not openat: the mode is the third argument, and this file is
	// looked at by a person, never by the player.
	static char launch_log_path[] = LAUNCH_LOG;
	launch_log = sys3(__NR_open, (long)launch_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

	// The update, if one is pending, happens here: no process holds the player
	// in the rootfs yet, and the card is mounted (init mounts it before this
	// runs). Whatever the outcome, a player is started below.
	apply_pending_update(envp);

	say("sonix_launch: starting ");
	say(player_argv[0]);
	say("\n");

	long pid = sys3(__NR_fork, 0, 0, 0);
	if (pid == 0) {
		run(player_argv[0], player_argv, envp);
		say("sonix_launch: cannot start ");
		say(player_argv[0]);
		say("\n");
		sys3(__NR_exit, 127, 0, 0);
	}
	if (pid < 0) {
		// No second process: the player takes this one, as a plain exec would.
		run(player_argv[0], player_argv, envp);
	} else {
		int status = 0;
		long r;
		do {
			r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
		} while (r == -EINTR);

		if ((status & 0x7f) == 0) {
			say("sonix_launch: player exited with status ");
			say_num((unsigned)(status >> 8) & 0xff);
		} else {
			say("sonix_launch: player killed by signal ");
			say_num((unsigned)status & 0x7f);
		}
		say(", rebooting\n");
	}

	long pause[2] = {1, 0}; // struct timespec: one second
	while (sys3(__NR_nanosleep, (long)pause, (long)pause, 0) == -EINTR) {
	}

	// The reboot applet goes through init, which runs the shutdown scripts and
	// unmounts the card. The system call is only for a rootfs without one.
	static char reboot_name[] = "reboot";
	static char *reboot_argv[] = {reboot_name, 0};
	run("/sbin/reboot", reboot_argv, envp);
	run("/bin/reboot", reboot_argv, envp);
	run("/usr/sbin/reboot", reboot_argv, envp);
	run("/usr/bin/reboot", reboot_argv, envp);
	sys3(__NR_sync, 0, 0, 0);
	sys4(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART, 0);
	for (;;) {
		sys3(__NR_exit, 1, 0, 0);
	}
}

// The kernel enters with $sp on argc, argv[] and envp[]. The o32 calling
// convention wants 16 bytes of argument space below the caller's frame.
__asm__(".text\n"
		".globl __start\n"
		".type __start, @function\n"
		".set push\n"
		".set noreorder\n"
		"__start:\n"
		"	move $4, $sp\n"
		"	li $8, -8\n"
		"	and $sp, $sp, $8\n"
		"	addiu $sp, $sp, -16\n"
		"	jal launch_main\n"
		"	nop\n"
		".set pop\n");
