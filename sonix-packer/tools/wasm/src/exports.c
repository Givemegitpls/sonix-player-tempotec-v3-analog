/* What the module exports to JavaScript besides mkfs.ubifs' own main().
 *
 * The page reads the vendor's image with the *same* LZO library that wrote it:
 * 16 485 of the stock rootfs's 18 060 data nodes are LZO-compressed, and the
 * decompressor is the one implementation that is already in the build. So the
 * module exposes lzo_init and the safe decompressor, taking plain lengths and a
 * caller-allocated output buffer, and the reader calls them through the wasm
 * heap.
 */
#include <errno.h>
#include <lzo/lzo1x.h>

int wasm_lzo_init(void)
{
	return lzo_init();
}

int wasm_lzo1x_decompress_safe(const unsigned char *in, unsigned long in_len,
			       unsigned char *out, unsigned long *out_len)
{
	return lzo1x_decompress_safe(in, in_len, out, out_len, NULL);
}

/*
 * mkfs.ubifs asks a file for its inode flags twice (FS_IOC_GETFLAGS) to clear
 * append-only/immutable/compressed while it copies. Emscripten's ioctl syscall
 * aborts on a request it does not know, so the tool is built with
 * -Dioctl=wasm_ioctl_stub and gets ENOTTY here instead -- which its code reads as
 * "this file has no flags", exactly what a tree built in memory has.
 */
int wasm_ioctl_stub(int fd, int request, ...)
{
	(void)fd;
	(void)request;
	errno = ENOTTY;
	return -1;
}
