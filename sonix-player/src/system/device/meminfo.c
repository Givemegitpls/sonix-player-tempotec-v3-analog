#include "meminfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One value out of /proc/meminfo, -1 when the key or the file is not there.
static long meminfo_kb(const char *key) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return -1;
	}

	char line[160];
	size_t klen = strlen(key);
	long val = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
			val = atol(line + klen + 1);
			break;
		}
	}

	fclose(f);
	return val;
}

long meminfo_available_kb(void) {
	long avail = meminfo_kb("MemAvailable");
	if (avail >= 0) {
		return avail;
	}

	// Pre-3.14 kernel: the two halves of what the kernel would have subtracted.
	// Cached leaves out SReclaimable, which is the usual approximation and the
	// one the processes page reports too.
	long free_kb = meminfo_kb("MemFree");
	long buffers = meminfo_kb("Buffers");
	long cached = meminfo_kb("Cached");
	if (free_kb < 0 && buffers < 0 && cached < 0) {
		return -1;
	}

	return (free_kb < 0 ? 0 : free_kb) + (buffers < 0 ? 0 : buffers) + (cached < 0 ? 0 : cached);
}
