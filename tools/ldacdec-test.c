// ldacdec-test -- is the firmware's own libldacBT.so decode path usable?
//
// The stock library is a combined one (encode, decode and ABR), and the port
// uses it for the receive side: bluez-alsa's LDAC decoder calls
// ldacBT_init_handle_decode() and ldacBT_decode() straight out of it. On a real
// stream from a phone that process dies with SIGSEGV, and this is the smallest
// thing that asks whether the library is at fault rather than bluealsa: one
// handle, one decoder init, then a few frames of nonsense -- which a working
// decoder answers with an error code, not a crash.
//
// Build (only the stock library is linked: no AOSP libldacBT_enc/abr next to
// it, so there is exactly one definition of every ldacBT_* symbol):
//   mipsel-rockbox-linux-gnu-gcc -O2 -o ldacdec-test ldacdec-test.c \
//       -I<sysroot>/usr/include/ldac -L<sysroot>/usr/lib -lldacBT \
//       -Wl,--allow-shlib-undefined

#include <ldacBT.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
	int cm = argc > 1 ? atoi(argv[1]) : 1;   /* 1 = stereo in this API */
	int sf = argc > 2 ? atoi(argv[2]) : 48000;
	const char *file = argc > 3 ? argv[3] : NULL;

	HANDLE_LDAC_BT handle = ldacBT_get_handle();
	printf("handle %p\n", (void *)handle);
	if (handle == NULL) {
		return 1;
	}

	int r = ldacBT_init_handle_decode(handle, cm, sf, 0, 0, 0);
	printf("init_handle_decode(cm=%d sf=%d) -> %d, error %d\n", cm, sf, r, ldacBT_get_error_code(handle));
	if (r == -1) {
		return 2;
	}

	static unsigned char stream[8192];
	static int pcm[8192];
	size_t stream_len = 660;

	if (file) {
		FILE *f = fopen(file, "rb");
		if (!f) {
			perror(file);
			return 1;
		}
		stream_len = fread(stream, 1, sizeof(stream), f);
		fclose(f);
		printf("read %zu bytes from %s\n", stream_len, file);
	} else {
		memset(stream, 0x5a, stream_len);
	}

	int total = 0;
	for (int i = 0; i < 8; i++) {
		int used = 0, decoded = 0;
		int ret = ldacBT_decode(handle, stream, pcm, LDACBT_SMPL_FMT_S32, (int)stream_len, &used, &decoded);
		printf("decode %d -> %d, used %d, decoded %d, error %d\n", i, ret, used, decoded,
				ldacBT_get_error_code(handle));
		total += decoded;
		if (ret != 0) {
			break;
		}
	}
	printf("decoded %d frames total\n", total);

	ldacBT_free_handle(handle);
	printf("done\n");
	return 0;
}
