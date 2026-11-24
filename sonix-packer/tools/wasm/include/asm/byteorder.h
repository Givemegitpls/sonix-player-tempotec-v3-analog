/* The kernel header `mtd/ubifs-media.h` includes for endianness.
 *
 * mtd-utils' ubifs-media.h does `#include <asm/byteorder.h>` and then relies on
 * `__BYTE_ORDER`/`__LITTLE_ENDIAN` to pick its byte-swapping macros. wasm32 is
 * little endian, and musl's <endian.h> defines those; the arch-specific kernel
 * path is all that is missing. <byteswap.h> comes along because the same header
 * reaches for the bswap primitives.
 */
#ifndef WASM_ASM_BYTEORDER_H
#define WASM_ASM_BYTEORDER_H

#include <endian.h>
#include <byteswap.h>
/* On Linux this path reaches <linux/types.h> as well, and users of the header
   (mtd/ubi-media.h) rely on it: it names __be32 without including anything
   else. */
#include <linux/types.h>

#endif
