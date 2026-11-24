/* The kernel header mtd-utils asks for and emscripten's sysroot has no copy of.
 *
 * mkfs.ubifs reads a file's inode flags (FS_IOC_GETFLAGS) to decide whether a
 * file is append-only, immutable, compressed or directory-synchronous, and
 * clears them while it copies. There is no such ioctl on the wasm filesystem it
 * runs on: the call fails with ENOTTY and mkfs.ubifs treats that as "no flags",
 * which is what a freshly built tree has. The constants exist so that the code
 * compiles; the ioctl numbers match Linux's, for the record.
 */
#ifndef WASM_LINUX_FS_H
#define WASM_LINUX_FS_H

#include <sys/ioctl.h>

#define FS_IOC_GETFLAGS _IOR('f', 1, long)
#define FS_IOC_SETFLAGS _IOW('f', 2, long)

#define FS_SECRM_FL 0x00000001
#define FS_UNRM_FL 0x00000002
#define FS_COMPR_FL 0x00000004
#define FS_SYNC_FL 0x00000008
#define FS_IMMUTABLE_FL 0x00000010
#define FS_APPEND_FL 0x00000020
#define FS_NODUMP_FL 0x00000040
#define FS_NOATIME_FL 0x00000080
#define FS_DIRSYNC_FL 0x00000080

#endif
