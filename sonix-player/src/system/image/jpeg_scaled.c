#include "jpeg_scaled.h"

#include <dlfcn.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libjpeg9/jpeglib.h"

// The vendored TJpgDec is compiled into this translation unit under renamed
// entry points, because LVGL links its own copy of the same library (with
// scaling compiled OUT) and the two would otherwise collide at link time.
#define jd_prepare sonix_jd_prepare
#define jd_decomp sonix_jd_decomp
#define jd_mcu_load sonix_jd_mcu_load
#define jd_mcu_output sonix_jd_mcu_output
#define jd_restart sonix_jd_restart
#include "tjpgd/tjpgd_impl.inc"

// Work pool for the decoder's tables and MCU buffers. The documented minimum
// is ~3.1 KB; double it for comfort with 4:2:0 files.
#define JPEG_POOL_BYTES 8192

typedef struct {
	const uint8_t *data; // the picture in memory, or
	FILE *file;			 // read from here, `size` bytes from where it stands
	size_t size;
	size_t pos;

	uint8_t *out; // RGB888, out_w * out_h * 3
	int out_w;
	int out_h;
} jpeg_io_t;

// TJpgDec pulls compressed bytes through this. A NULL buffer means "skip".
static size_t in_func(JDEC *jd, uint8_t *buf, size_t len) {
	jpeg_io_t *io = jd->device;

	size_t remaining = io->size - io->pos;
	if (len > remaining) {
		len = remaining;
	}
	if (io->file) {
		if (buf) {
			len = fread(buf, 1, len, io->file);
		} else if (fseek(io->file, (long)len, SEEK_CUR) != 0) {
			return 0;
		}
	} else if (buf) {
		memcpy(buf, io->data + io->pos, len);
	}
	io->pos += len;
	return len;
}

// Receives one decoded MCU rectangle, already scaled.
//
// NOTE the channel order: LVGL's copy of TJpgDec is patched to emit B,G,R
// (see the /*B*/ /*G*/ /*R*/ lines in tjpgd_impl.inc's mcu_output) where the
// upstream library emits R,G,B. The swap below turns it back into the RGB888
// everything downstream expects.
static int out_func(JDEC *jd, void *bitmap, JRECT *rect) {
	jpeg_io_t *io = jd->device;

	int rect_w = rect->right - rect->left + 1;
	int rect_h = rect->bottom - rect->top + 1;
	const uint8_t *src = bitmap;

	for (int y = 0; y < rect_h; y++) {
		int out_y = rect->top + y;
		if (out_y >= io->out_h) {
			break;
		}

		int copy_w = rect_w;
		if (rect->left + copy_w > io->out_w) {
			copy_w = io->out_w - rect->left;
		}
		if (copy_w <= 0) {
			continue;
		}

		const uint8_t *s = src + (size_t)y * rect_w * 3;
		uint8_t *d = io->out + ((size_t)out_y * io->out_w + rect->left) * 3;
		for (int x = 0; x < copy_w; x++) {
			d[0] = s[2]; // R
			d[1] = s[1]; // G
			d[2] = s[0]; // B
			s += 3;
			d += 3;
		}
	}

	return 1; // keep going
}

// The reduction, as a power of two (0..3), for a picture whose short side is
// `short_side`.
static uint8_t pick_scale(int short_side, int min_box) {
	// The strongest reduction whose output still gives the caller's resampler
	// at least two source pixels per destination pixel. Below that ratio the
	// area-averaging filter degenerates towards nearest-neighbour and the result
	// looks grainy; beyond it is wasted memory and decode time.
	if (min_box < 1) {
		min_box = 1;
	}
	int quality_floor = min_box * 2;
	uint8_t scale = 0;
	while (scale < 3) {
		int next_short = short_side >> (scale + 1);
		if (next_short < quality_floor) {
			break;
		}
		scale++;
	}

	// The memory ceiling wins over quality: the two-source-pixels rule on its own
	// lets a very large source produce an output too big to allocate in RGB888.
	// Above this short side the reduction steps up again, as long as that keeps
	// the output at or above the requested box.
	#define JPEG_OUT_MAX_SHORT 1024
	while (scale < 3) {
		int cur_short = short_side >> scale;
		int next_short = short_side >> (scale + 1);
		if (cur_short <= JPEG_OUT_MAX_SHORT || next_short < min_box) {
			break; // either it fits already, or halving would fall below the requested box
		}
		scale++;
	}
	return scale;
}

// ---------------------------------------------------------------------------
// The system's libjpeg
//
// The stock firmware carries the IJG's libjpeg 9 (/usr/lib/libjpeg.so.9), and
// it is the better decoder here on two counts. Its Huffman decoding, where a
// big cover spends nearly all its time, is table driven where TJpgDec goes bit
// by bit. And it reads every sampling layout: TJpgDec takes only the usual
// three, so a baseline file sampled 1x2 in all three components (it happens)
// fell to the plane decoder, whose full-resolution planes the device's memory
// budget refuses -- no cover at all. It scales in the IDCT just the same, so
// the memory stays a few rows.
//
// Opened at run time and only used when it is there and is the version these
// headers describe (libjpeg9/README); otherwise everything goes through
// TJpgDec as before. A progressive file is taken only when the whole image's
// coefficients, which libjpeg has to keep for it, fit LJ_PROGRESSIVE_MAX_BYTES;
// a larger one is left to the caller's own fallback.
// ---------------------------------------------------------------------------

static struct {
	struct jpeg_error_mgr *(*std_error)(struct jpeg_error_mgr *);
	void (*create)(j_decompress_ptr, int, size_t);
	void (*destroy)(j_decompress_ptr);
	void (*mem_src)(j_decompress_ptr, const unsigned char *, size_t);
	void (*stdio_src)(j_decompress_ptr, FILE *);
	int (*read_header)(j_decompress_ptr, boolean);
	boolean (*has_multiple_scans)(j_decompress_ptr);
	boolean (*start)(j_decompress_ptr);
	JDIMENSION (*read_scanlines)(j_decompress_ptr, JSAMPARRAY, JDIMENSION);
} lj;
static bool lj_ok;

// Two bytes for each of the 64 coefficients of every block. A 1400x1400 4:2:0
// picture needs 5.9 MB; the plane decoder the caller falls back on reads the
// same file in under three, but several times more slowly.
#define LJ_PROGRESSIVE_MAX_BYTES (6u * 1024u * 1024u)

static size_t lj_coefficient_bytes(j_decompress_ptr cinfo) {
	int max_h = 1, max_v = 1;
	for (int c = 0; c < cinfo->num_components; c++) {
		max_h = cinfo->comp_info[c].h_samp_factor > max_h ? cinfo->comp_info[c].h_samp_factor : max_h;
		max_v = cinfo->comp_info[c].v_samp_factor > max_v ? cinfo->comp_info[c].v_samp_factor : max_v;
	}
	size_t blocks = 0;
	for (int c = 0; c < cinfo->num_components; c++) {
		size_t bw = ((size_t)cinfo->image_width * cinfo->comp_info[c].h_samp_factor + max_h * 8 - 1) / (max_h * 8);
		size_t bh = ((size_t)cinfo->image_height * cinfo->comp_info[c].v_samp_factor + max_v * 8 - 1) / (max_v * 8);
		// Rounded up to whole MCUs, which is how libjpeg allocates them.
		bw = (bw + cinfo->comp_info[c].h_samp_factor - 1) / cinfo->comp_info[c].h_samp_factor *
			 cinfo->comp_info[c].h_samp_factor;
		bh = (bh + cinfo->comp_info[c].v_samp_factor - 1) / cinfo->comp_info[c].v_samp_factor *
			 cinfo->comp_info[c].v_samp_factor;
		blocks += bw * bh;
	}
	return blocks * 64 * 2;
}
static pthread_once_t lj_once = PTHREAD_ONCE_INIT;

static void lj_load(void) {
	void *h = dlopen("libjpeg.so.9", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		printf("jpeg: no libjpeg.so.9, covers decode with TJpgDec\n");
		return;
	}
	lj.std_error = (struct jpeg_error_mgr * (*)(struct jpeg_error_mgr *)) dlsym(h, "jpeg_std_error");
	lj.create = (void (*)(j_decompress_ptr, int, size_t))dlsym(h, "jpeg_CreateDecompress");
	lj.destroy = (void (*)(j_decompress_ptr))dlsym(h, "jpeg_destroy_decompress");
	lj.mem_src = (void (*)(j_decompress_ptr, const unsigned char *, size_t))dlsym(h, "jpeg_mem_src");
	lj.stdio_src = (void (*)(j_decompress_ptr, FILE *))dlsym(h, "jpeg_stdio_src");
	lj.read_header = (int (*)(j_decompress_ptr, boolean))dlsym(h, "jpeg_read_header");
	lj.has_multiple_scans = (boolean(*)(j_decompress_ptr))dlsym(h, "jpeg_has_multiple_scans");
	lj.start = (boolean(*)(j_decompress_ptr))dlsym(h, "jpeg_start_decompress");
	lj.read_scanlines = (JDIMENSION(*)(j_decompress_ptr, JSAMPARRAY, JDIMENSION))dlsym(h, "jpeg_read_scanlines");
	lj_ok = lj.std_error && lj.create && lj.destroy && lj.mem_src && lj.stdio_src && lj.read_header &&
			lj.has_multiple_scans && lj.start && lj.read_scanlines;
	if (!lj_ok) {
		dlclose(h);
	}
	printf("jpeg: %s\n", lj_ok ? "libjpeg.so.9 decodes covers" : "libjpeg.so.9 is incomplete, covers decode with TJpgDec");
}

typedef struct {
	struct jpeg_error_mgr mgr;
	jmp_buf jump;
} lj_error_t;

static void lj_error_exit(j_common_ptr cinfo) { longjmp(((lj_error_t *)cinfo->err)->jump, 1); }
static void lj_silent(j_common_ptr cinfo) { (void)cinfo; }

// From memory (`data`) or from the file `f` stands in. NULL when libjpeg is
// not there or will not take the picture; the caller then tries TJpgDec.
static uint8_t *lj_decode(const uint8_t *data, size_t size, FILE *f, int min_box, int *out_w, int *out_h,
						  int *src_w, int *src_h) {
	pthread_once(&lj_once, lj_load);
	if (!lj_ok) {
		return NULL;
	}

	struct jpeg_decompress_struct cinfo;
	lj_error_t err;
	uint8_t *volatile out = NULL;
	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.err = lj.std_error(&err.mgr);
	err.mgr.error_exit = lj_error_exit;
	err.mgr.output_message = lj_silent;
	if (setjmp(err.jump)) {
		lj.destroy(&cinfo);
		free(out);
		return NULL;
	}
	lj.create(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
	if (f) {
		lj.stdio_src(&cinfo, f);
	} else {
		lj.mem_src(&cinfo, data, size);
	}
	if (lj.read_header(&cinfo, TRUE) != JPEG_HEADER_OK ||
		(lj.has_multiple_scans(&cinfo) && lj_coefficient_bytes(&cinfo) > LJ_PROGRESSIVE_MAX_BYTES) ||
		(cinfo.jpeg_color_space != JCS_GRAYSCALE && cinfo.num_components != 3)) {
		lj.destroy(&cinfo);
		return NULL;
	}

	int w = (int)cinfo.image_width, h = (int)cinfo.image_height;
	cinfo.scale_num = 1;
	cinfo.scale_denom = 1u << pick_scale(w < h ? w : h, min_box);
	cinfo.out_color_space = JCS_RGB;
	cinfo.dct_method = JDCT_ISLOW;
	lj.start(&cinfo);

	int ow = (int)cinfo.output_width, oh = (int)cinfo.output_height;
	if (ow < 1 || oh < 1 || cinfo.output_components != 3) {
		lj.destroy(&cinfo);
		return NULL;
	}
	out = malloc((size_t)ow * oh * 3);
	if (!out) {
		lj.destroy(&cinfo);
		return NULL;
	}
	while (cinfo.output_scanline < cinfo.output_height) {
		JSAMPROW row = out + (size_t)cinfo.output_scanline * ow * 3;
		lj.read_scanlines(&cinfo, &row, 1);
	}
	// No jpeg_finish_decompress: it would read on to the end-of-image marker,
	// and from a file whatever follows the picture, for nothing.
	lj.destroy(&cinfo);

	*out_w = ow;
	*out_h = oh;
	if (src_w) {
		*src_w = w;
	}
	if (src_h) {
		*src_h = h;
	}
	return out;
}

static uint8_t *decode_io(jpeg_io_t io, int min_box, int *out_w, int *out_h, int *src_w, int *src_h) {
	void *pool = malloc(JPEG_POOL_BYTES);
	if (!pool) {
		return NULL;
	}

	JDEC jd;
	if (sonix_jd_prepare(&jd, in_func, pool, JPEG_POOL_BYTES, &io) != JDR_OK) {
		free(pool);
		return NULL; // progressive, broken, or not a JPEG: caller falls back
	}

	uint8_t scale = pick_scale(jd.width < jd.height ? jd.width : jd.height, min_box);

	io.out_w = jd.width >> scale;
	io.out_h = jd.height >> scale;
	if (io.out_w < 1 || io.out_h < 1) {
		free(pool);
		return NULL;
	}

	io.out = malloc((size_t)io.out_w * io.out_h * 3);
	if (!io.out) {
		free(pool);
		return NULL;
	}

	JRESULT res = sonix_jd_decomp(&jd, out_func, scale);
	free(pool);

	if (res != JDR_OK) {
		free(io.out);
		return NULL;
	}

	*out_w = io.out_w;
	*out_h = io.out_h;
	if (src_w) {
		*src_w = jd.width;
	}
	if (src_h) {
		*src_h = jd.height;
	}
	return io.out;
}

uint8_t *jpeg_scaled_decode(const uint8_t *data, size_t size, int min_box, int *out_w, int *out_h,
							int *src_w, int *src_h) {
	if (!data || size == 0 || !out_w || !out_h) {
		return NULL;
	}
	uint8_t *out = lj_decode(data, size, NULL, min_box, out_w, out_h, src_w, src_h);
	return out ? out : decode_io((jpeg_io_t){.data = data, .size = size}, min_box, out_w, out_h, src_w, src_h);
}
