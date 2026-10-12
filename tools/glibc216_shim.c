// glibc216_shim -- the few symbols the V3 Analog's glibc 2.16 does not have.
//
// The Bluetooth userland (BlueZ 5.66's bluetoothd, bluez-alsa's ALSA plugin) is
// built with a toolchain whose glibc is newer, and a binary that asks for a
// versioned symbol the device's libc does not export does not start at all --
// the loader stops, and on this device nothing says why. Two symbols are all
// that stand between the built binaries and the device:
//
//   getrandom@GLIBC_2.25   bluetoothd (key generation). The syscall arrived in
//                          Linux 3.17 and this device runs 3.10, so /dev/urandom
//                          is the way in; it is the same pool.
//   clock_gettime@GLIBC_2.17   bluez-alsa's plugin. On 2.16 the function lives in
//                          librt as GLIBC_2.2, and defining it here (the syscall
//                          directly) takes the versioned reference out of the
//                          binary entirely.
//
// Defining a symbol in the executable (or in the plugin) is what removes the
// dynamic reference: the linker resolves it locally and no version check is left
// for the loader to fail on. Nothing else in these two binaries asks for
// anything newer than 2.16 -- checked with readelf on both.
//
// Build: the same cross compiler, -O2 -march=mips32r2, and link the object into
// each target (see the build recipe in the project's .opencode/NOTES.md).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
	(void)flags;

	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		errno = EIO;
		return -1;
	}

	size_t got = 0;
	while (got < buflen) {
		ssize_t n = read(fd, (char *)buf + got, buflen - got);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			errno = EIO;
			return -1;
		}
		if (n == 0) {
			close(fd);
			errno = EIO;
			return -1;
		}
		got += (size_t)n;
	}

	close(fd);
	return (ssize_t)got;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
	return (int)syscall(SYS_clock_gettime, clk_id, tp);
}
