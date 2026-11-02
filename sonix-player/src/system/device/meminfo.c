#include "meminfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long meminfo_kb(const char *key) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return -1;
	}

	char line[160];
	size_t klen = strlen(key);
	long val = -1;
	while (fgets(line, sizeof(line), f)) {
		// The ':' test is what keeps "MemFree" from matching a hypothetical
		// longer key, and it is safe to read line[klen] only once the prefix
		// has matched -- that guarantees the character is inside the line.
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
	// one the processes page has always shown.
	//
	// This is an estimate for *reporting*: it counts page cache, and the page
	// cache on this device is most of the number. Anything sizing a single
	// allocation wants meminfo_kb("MemAvailable") and its own floor instead --
	// see the decode budget in gui/nowplaying/cover.c.
	long free_kb = meminfo_kb("MemFree");
	long buffers = meminfo_kb("Buffers");
	long cached = meminfo_kb("Cached");
	if (free_kb < 0 && buffers < 0 && cached < 0) {
		return -1;
	}

	return (free_kb < 0 ? 0 : free_kb) + (buffers < 0 ? 0 : buffers) + (cached < 0 ? 0 : cached);
}
