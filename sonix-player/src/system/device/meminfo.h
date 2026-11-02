#ifndef MEMINFO_H
#define MEMINFO_H

// Available RAM in kilobytes -- the one memory number the budget calculations
// need.
//
// MemAvailable is a 3.14 addition and the stock kernel on this player is 3.10,
// so the line is simply absent here: every reader that parsed only that line
// got 0 or -1, the cover decode budget sat on its 2 MB floor, and the log read
// `cover: 0 KB free`, which looks like a full card and is not one.
//
// Missing MemAvailable falls back to MemFree + Buffers + Cached, the estimate
// the processes page has always used.
//
// -1 when /proc/meminfo cannot be read at all.
long meminfo_available_kb(void);

#endif
