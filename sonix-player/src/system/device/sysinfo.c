#include "sysinfo.h"

#include "board.h"

#include "src/system/core/respath.h"

#include "src/system/device/system.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>

#define SYSINFO_FILE SONIX_RESOURCE_DIR "/components/system-info.json"

static char os_version[64];
static char build_version[64];
static char device_name[64];
static char dac_info[96];
static char ota_repo[128];


// ---------------------------------------------------------------------------
// the version file
// ---------------------------------------------------------------------------

// Case-insensitive strstr. Needed because the key name is typed by hand by
// whoever assembles the firmware image, and "os_version" instead of
// "OS_version" would leave the page showing two dashes and no explanation.
// (strcasestr exists but is a GNU extension needing a feature-test macro; ten
// lines cost less.)
static const char *find_nocase(const char *haystack, const char *needle) {
	size_t n = strlen(needle);
	if (n == 0) {
		return haystack;
	}
	for (const char *p = haystack; *p; p++) {
		size_t i = 0;
		while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == n) {
			return p;
		}
	}
	return NULL;
}

// A miniature JSON reader, deliberately so: it looks for "key", then the colon,
// then the string that follows. It handles no numbers, nesting or escape
// sequences, and does not need to -- the file is defined here and holds two
// strings. All it has to do is survive a badly written file: on anything
// unexpected the field stays empty and the page shows a dash.
static bool json_string_field(const char *text, const char *key, char *out, size_t out_size) {
	out[0] = '\0';

	// The key in quotes, so "Build_version" is not matched inside a longer key
	// that contains it.
	char quoted[64];
	if (snprintf(quoted, sizeof(quoted), "\"%s\"", key) >= (int)sizeof(quoted)) {
		return false;
	}

	const char *p = find_nocase(text, quoted);
	if (!p) {
		return false;
	}
	p += strlen(quoted);

	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p != ':') {
		return false;
	}
	p++;
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p != '"') {
		return false;
	}
	p++;

	const char *end = strchr(p, '"');
	if (!end) {
		return false;
	}

	size_t len = (size_t)(end - p);
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

void sysinfo_load(void) {
	os_version[0] = '\0';
	build_version[0] = '\0';
	device_name[0] = '\0';
	dac_info[0] = '\0';
	ota_repo[0] = '\0';

	FILE *f = fopen(SYSINFO_FILE, "rb");
	if (!f) {
		printf("sysinfo: %s missing, versions not available\n", SYSINFO_FILE);
		return;
	}

	// The file is a handful of lines; anything longer is not the expected file
	// and gets truncated without ceremony.
	char buf[1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	// The names the file uses, matched case-insensitively by find_nocase.
	json_string_field(buf, "OS_version", os_version, sizeof(os_version));
	json_string_field(buf, "Build_version", build_version, sizeof(build_version));
	json_string_field(buf, "device-name", device_name, sizeof(device_name));
	json_string_field(buf, "dac-info", dac_info, sizeof(dac_info));
	json_string_field(buf, "ota-repo", ota_repo, sizeof(ota_repo));

	// The board is what this binary was built for; the file is the label the
	// packer wrote. The two are compared, not chosen between: a binary built for
	// another board lays the interface out for the wrong glass and reaches for
	// hardware that is not there, so a mismatch is worth saying out loud.
	if (device_name[0] && strcasecmp(device_name, SONIX_BOARD_NAME) != 0) {
		printf("sysinfo: WARNING: this build is for '%s' but the device says '%s'; "
			   "the layout and the hardware settings are this build's own\n",
			   SONIX_BOARD_NAME, device_name);
	}
	printf("sysinfo: device '%s' (this build is for '%s'), system '%s', build '%s'\n", device_name,
		   SONIX_BOARD_NAME, os_version, build_version);
}

const char *sysinfo_os_version(void) { return os_version; }
const char *sysinfo_build_version(void) { return build_version; }
const char *sysinfo_device_name(void) { return device_name; }
const char *sysinfo_dac_info(void) { return dac_info; }
const char *sysinfo_ota_repo(void) { return ota_repo; }

// Matched without regard to case or to the spelling of "HiBy": the name is
// typed by hand into a text file, and "Hiby R1" is the same player as
// "HiBy R1".
const char *sysinfo_user_data_dir(void) {
	const sysinfo_model_t *model = sysinfo_model();
	return (model && model->user_data_dir) ? model->user_data_dir : "/usr/data";
}

// The board this binary was built for (board.h). Not looked up, not read from
// the device: the same answer every time it is asked, and a constant the
// compiler can see through.
const sysinfo_model_t *sysinfo_model(void) {
	return &BOARD_MODEL;
}

// The board, when the size asked about is its own panel, and NULL otherwise.
// For sizing the interface from the framebuffer's report; there is only ever
// one answer, because there is only one board in this binary.
const sysinfo_model_t *sysinfo_model_by_panel(int width, int height) {
	if (BOARD_MODEL.panel_width == width && BOARD_MODEL.panel_height == height) {
		return &BOARD_MODEL;
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// space on the card
// ---------------------------------------------------------------------------

bool sysinfo_sd_usage(sysinfo_storage_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	const char *root = storage_sd_root();
	if (!root) {
		return false;
	}

	struct statvfs st;
	if (statvfs(root, &st) != 0) {
		return false;
	}

	// f_frsize is the block size f_blocks is counted in; f_bsize is the
	// preferred I/O size and not the same thing, even though they match on many
	// filesystems. Some leave f_frsize at zero.
	uint64_t unit = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
	if (unit == 0 || st.f_blocks == 0) {
		return false;
	}

	out->present = true;
	out->total = (uint64_t)st.f_blocks * unit;
	// f_bfree counts reserved blocks too, f_bavail only those an ordinary
	// process can really use. For "how much is left" f_bavail is the honest
	// answer; used space is measured against f_bfree, or the figures would not
	// add up to the total.
	out->free = (uint64_t)st.f_bavail * unit;
	out->used = out->total - (uint64_t)st.f_bfree * unit;

	if (out->used > out->total) {
		out->used = out->total;
	}
	out->used_percent = (int)((out->used * 100 + out->total / 2) / out->total);
	if (out->used_percent > 100) {
		out->used_percent = 100;
	}
	return true;
}

void sysinfo_format_size(uint64_t bytes, char *out, size_t out_size) {
	const uint64_t KB = 1024;
	const uint64_t MB = KB * 1024;
	const uint64_t GB = MB * 1024;

	if (bytes >= GB) {
		// One decimal: "58.9 GB" says what is needed, "58.94 GB" does not.
		unsigned long long whole = bytes / GB;
		unsigned long long tenths = ((bytes % GB) * 10) / GB;
		snprintf(out, out_size, "%llu.%llu GB", whole, tenths);
	} else if (bytes >= MB) {
		snprintf(out, out_size, "%llu MB", (unsigned long long)((bytes + MB / 2) / MB));
	} else {
		snprintf(out, out_size, "%llu KB", (unsigned long long)((bytes + KB / 2) / KB));
	}
}

// ---------------------------------------------------------------------------
// serial number
// ---------------------------------------------------------------------------

// The serial number's own part out of one efuse node.
//
// `digits` above zero takes that many leading hex characters, upper case, which
// is the shape the HiBy boards' node has: "CHIP_ID: 90a70a428496c4012f146c04
// 04000001", where the box shows the model's prefix and the first eight
// ("R3PII" + 90A70A42, verified on a real R3 Pro II). Zero takes the node's
// whole first line instead, trimmed and uppercased, which is the shape the V3
// Analog's serial node has: the number itself and nothing else. Either way the
// answer is "" when the node is absent, empty, or holds less than was asked
// for, and the caller has a second node to try.
static void efuse_serial_part(const char *node, int digits, char *out, size_t out_size) {
	out[0] = '\0';

	FILE *f = fopen(node, "r");
	if (!f) {
		return; // the host build, or a firmware without the efuse module
	}
	char line[96] = "";
	bool got = fgets(line, sizeof(line), f) != NULL;
	fclose(f);
	if (!got) {
		return;
	}

	// A node that prefixes its number ("CHIP_ID: ...") is read past the colon.
	const char *at = strchr(line, ':');
	at = at ? at + 1 : line;
	while (*at == ' ' || *at == '\t') {
		at++;
	}

	if (digits <= 0) {
		size_t n = strcspn(at, "\r\n");
		while (n > 0 && (at[n - 1] == ' ' || at[n - 1] == '\t')) {
			n--;
		}
		if (n == 0 || n >= out_size) {
			return;
		}
		for (size_t i = 0; i < n; i++) {
			out[i] = (char)toupper((unsigned char)at[i]);
		}
		out[n] = '\0';
		return;
	}

	int n = 0;
	while (n < digits && isxdigit((unsigned char)at[n]) && (size_t)n + 1 < out_size) {
		out[n] = (char)toupper((unsigned char)at[n]);
		n++;
	}
	out[n] = '\0';
	if (n != digits) {
		out[0] = '\0'; // fewer characters than a serial has: not a serial
	}
}

// The stock player's check digit: an EAN/UPC-style one over the whole string it
// displays (prefix and serial together). Every character is worth its base-36
// value (the serial is hex, the prefix is letters and digits), every other
// character counting from the right is weighted 3, and the digit is
// (10 - sum % 10) % 10. The stock appends it on every model; this port only
// where the result has been checked against the box, which is the V3 Analog.
static char serial_check_digit(const char *text, size_t length) {
	unsigned sum = 0;
	for (size_t i = 0; i < length; i++) {
		unsigned char c = (unsigned char)text[i];
		unsigned value = 0;
		if (c >= '0' && c <= '9') {
			value = (unsigned)(c - '0');
		} else if (c >= 'A' && c <= 'Z') {
			value = (unsigned)(c - 'A') + 10;
		} else if (c >= 'a' && c <= 'z') {
			value = (unsigned)(c - 'a') + 10;
		}
		// The rightmost character is weighted 3, the one before it 1, and so on.
		sum += ((length - 1 - i) % 2 == 0) ? 3u * value : value;
	}
	return (char)('0' + (10u - sum % 10u) % 10u);
}

const char *sysinfo_serial_number(void) {
	// Read once and kept: the efuses are burned at the factory.
	static char serial[64];
	static bool tried;
	if (tried) {
		return serial;
	}
	tried = true;
	serial[0] = '\0';

	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		return serial; // no prefix to build it from
	}

	// The model's own node first, with the model's own idea of how much of it is
	// the number. A model that names no node reads the HiBy boards' node and
	// nothing else, so those boards behave exactly as they did; a model that
	// names one gets the HiBy node as a second try, since a path or a shape that
	// is not what was expected must not leave the About page without a serial
	// number at all.
	char part[40];
	efuse_serial_part(model->serial_node ? model->serial_node : "/proc/jz/efuse/efuse_chip_id",
					  model->serial_node ? model->serial_digits : 8, part, sizeof(part));
	if (!part[0] && model->serial_node) {
		efuse_serial_part("/proc/jz/efuse/efuse_chip_id", 8, part, sizeof(part));
	}
	if (part[0]) {
		int n = snprintf(serial, sizeof(serial), "%s%s", model->serial_prefix, part);
		if (model->serial_check_digit && n > 0 && (size_t)n + 2 < sizeof(serial)) {
			serial[n] = serial_check_digit(serial, (size_t)n);
			serial[n + 1] = '\0';
		}
	}
	return serial;
}
