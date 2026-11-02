#ifndef MEMINFO_H
#define MEMINFO_H

// /proc/meminfo, in kilobytes, so that nobody has to parse it again.
//
// Two entry points, because the file answers two different questions.
//
// MemAvailable is a 3.14 addition and the stock kernel on this player is 3.10,
// so the line is simply absent here. Every reader that parsed only that line
// got 0 or -1 as a result: the audio stats printed `MemAvailable -1 kB`, the
// scan log said `0 kB free`, and the processes page showed no RAM at all --
// numbers that read like a device in trouble and were only a missing key.
//
// -1 means /proc/meminfo itself cannot be read.

// One value by key: "MemTotal", "MemFree", "Cached", ... -1 when the key or the
// file is not there. For sizing decisions this is the honest one: on this
// kernel MemAvailable is -1 and the caller keeps its own floor.
long meminfo_kb(const char *key);

// Available RAM in kilobytes, for anything that *reports* memory rather than
// allocating against it.
//
// Missing MemAvailable falls back to MemFree + Buffers + Cached, the estimate
// the processes page has always used. It counts page cache, which on this
// device is most of the figure: good enough to label a bar with, and not a
// budget to hand to one large allocation -- the cover decoder deliberately
// stays on meminfo_kb("MemAvailable") for that reason.
//
// -1 when /proc/meminfo cannot be read at all.
long meminfo_available_kb(void);

#endif
