/* The kernel header mtd-utils asks for and emscripten's sysroot has no copy of.
 *
 * `include/mtd/ubifs-media.h` and `mkfs.ubifs.h` include <linux/types.h> for the
 * __u8/__le32 family, which is all of it this build needs: mkfs.ubifs writes an
 * image in the on-disk types, and wasm32 has the same widths as any 32-bit
 * target. Nothing here is Linux-specific beyond the names.
 */
#ifndef WASM_LINUX_TYPES_H
#define WASM_LINUX_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;

typedef uint16_t __le16;
typedef uint16_t __be16;
typedef uint32_t __le32;
typedef uint32_t __be32;
typedef uint64_t __le64;
typedef uint64_t __be64;

/* The kernel's own scalar names, which mtd-abi.h uses in its ioctl macros
 * (MEMGETBADBLOCK is _IOW('M', 11, __kernel_loff_t)). */
typedef long long __kernel_loff_t;
typedef long long __kernel_off_t;
typedef long __kernel_ssize_t;
typedef unsigned long __kernel_size_t;
typedef long __kernel_time_t;

#ifndef __bitwise
#define __bitwise
#endif
typedef __u16 __bitwise __sum16;
typedef __u32 __bitwise __wsum;

#endif
