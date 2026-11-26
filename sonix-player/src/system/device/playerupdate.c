#include "playerupdate.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/semver.h"
#include "src/system/core/sha256.h"
#include "src/system/core/version.h"
#include "src/system/device/sysinfo.h"
#include "src/system/net/http.h"

// Where the launcher keeps its state. Both files are the launcher's, and this
// side only reads the lock's existence and writes the mark (docs/PLAYER-UPDATE.md).
#define UPDATE_LOCK "/data/update.lock"

// A package is a player binary; anything past this is not one, and reading it
// would only be a way to fill the heap on a device with 56 MB.
#define PACKAGE_MAX (24L * 1024 * 1024)

static bool package_download(const char *url, const char *dest, char *why, size_t why_n);
static const char *card_root(void);

bool playerupdate_should_run(int argc, char **argv) {
	if (argc < 2) {
		return false;
	}
	return strcmp(argv[1], "--verify") == 0 || strcmp(argv[1], "--version-check") == 0 ||
		   strcmp(argv[1], "--install-package") == 0 || strcmp(argv[1], "--install-update") == 0 ||
		   strcmp(argv[1], "--update-from-index") == 0;
}

// "Is this package newer than me?" -- the rule lives in semver.c, and this is
// how it is exercised without a device: the host build answers for the compiled-in
// version, so a table of cases can be run in a script.
static int version_check_main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s --version-check <candidate>\n", argv[0]);
		return 2;
	}
	version_verdict_t v = version_decide(argv[2], player_version());
	printf("%s: %s (running %s, package %s)\n", version_verdict_name(v), argv[2], player_version(), argv[2]);
	return (int)v;
}

static bool file_read_all(const char *path, unsigned char **out, long *out_len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long len = ftell(f);
	if (len <= 0 || len > PACKAGE_MAX || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	unsigned char *buf = malloc((size_t)len);
	if (!buf) {
		fclose(f);
		return false;
	}
	bool ok = fread(buf, 1, (size_t)len, f) == (size_t)len;
	fclose(f);
	if (!ok) {
		free(buf);
		return false;
	}
	*out = buf;
	*out_len = len;
	return true;
}

// 32-bit little-endian MIPS, which is the only thing this device can run. The
// loader would refuse anything else anyway, but it would refuse it *after* the
// swap, with the old binary already gone.
static bool elf_is_ours(const unsigned char *buf, long len) {
	if (len < 64) {
		return false;
	}
	if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
		return false;
	}
	if (buf[4] != 1 /* ELFCLASS32 */ || buf[5] != 1 /* ELFDATA2LSB */) {
		return false;
	}
	// e_machine at 18, little-endian: 8 is EM_MIPS.
	if (buf[18] != 8 || buf[19] != 0) {
		return false;
	}
	// e_type at 16: 2 (ET_EXEC) or 3 (ET_DYN), never a relocatable object.
	unsigned type = (unsigned)buf[16] | ((unsigned)buf[17] << 8);
	return type == 2 || type == 3;
}

// The board id is compiled into both, so a package for another device is
// recognised without trusting the manifest to say the truth.
static bool names_this_board(const unsigned char *buf, long len) {
	const char *id = player_board_id();
	size_t n = strlen(id);
	if (n < 4 || n > 64) {
		return true; // no id to look for (a build outside the Makefile): do not block
	}
	for (long i = 0; i + (long)n <= len; i++) {
		if (buf[i] == (unsigned char)id[0] && memcmp(buf + i, id, n) == 0) {
			return true;
		}
	}
	return false;
}

// `--install-package [path]`: the same path the menu takes, from a shell. Without
// a path the same places the start-up check looks at are searched.
static int install_package_main(int argc, char **argv) {
	char path[300];
	if (argc >= 3) {
		snprintf(path, sizeof(path), "%s", argv[2]);
	} else if (!playerupdate_find_package(path, sizeof(path))) {
		printf("install: no package on the card\n");
		return 1;
	}

	char why[192] = "";
	playerupdate_result_t r = playerupdate_install(path, why, sizeof(why));
	printf("install: %s (%s): %s\n", playerupdate_verdict_name(r), path, why[0] ? why : "ok");
	return r == PLAYERUPDATE_OK || r == PLAYERUPDATE_ASK ? 0 : 1;
}

// `--install-update <url>`: download, then the same checks and staging as the
// card path. This is the whole update minus the menu.
static int install_update_main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s --install-update <url>\n", argv[0]);
		return 2;
	}
	const char *root = card_root();
	if (!root || !*root) {
		printf("install: no card\n");
		return 1;
	}

	char dir[256], path[320], why[192] = "";
	snprintf(dir, sizeof(dir), "%s/sonix", root);
	snprintf(path, sizeof(path), "%s/update.tar", dir);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		printf("install: cannot create %s\n", dir);
		return 1;
	}
	if (!package_download(argv[2], path, why, sizeof(why))) {
		printf("install: %s\n", why);
		return 1;
	}

	playerupdate_result_t r = playerupdate_install(path, why, sizeof(why));
	printf("install: %s: %s\n", playerupdate_verdict_name(r), why[0] ? why : "ok");
	return r == PLAYERUPDATE_OK || r == PLAYERUPDATE_ASK ? 0 : 1;
}

// `--update-from-index`: the whole network path in one command -- fetch the index,
// decide, download, verify, stage. This is what the menu's "OTA update" does,
// minus the cards.
static int update_from_index_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	playerupdate_remote_t remote;
	char why[192] = "";
	printf("update: checking the index\n");
	if (!playerupdate_remote_check(&remote, why, sizeof(why))) {
		printf("update: %s\n", why);
		return 1;
	}
	version_verdict_t v = version_decide(remote.version, player_version());
	printf("update: index offers %s (%ld bytes), running %s -> %s\n", remote.version, remote.size,
		   player_version(), version_verdict_name(v));
	if (v != VERSION_NEWER && v != VERSION_ASK) {
		printf("update: nothing to do\n");
		return 1;
	}
	if (!remote.notes[0]) {
		printf("update: (no notes for this release)\n");
	}

	playerupdate_result_t r = playerupdate_remote_install(&remote, why, sizeof(why));
	printf("update: %s: %s\n", playerupdate_verdict_name(r), why[0] ? why : "ok");
	return r == PLAYERUPDATE_OK || r == PLAYERUPDATE_ASK ? 0 : 1;
}

int playerupdate_verify_main(int argc, char **argv) {
	if (argc >= 2 && strcmp(argv[1], "--version-check") == 0) {
		return version_check_main(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "--install-package") == 0) {
		return install_package_main(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "--install-update") == 0) {
		return install_update_main(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "--update-from-index") == 0) {
		return update_from_index_main(argc, argv);
	}
	if (argc < 4) {
		fprintf(stderr, "usage: %s --verify <file> <sha256>\n", argv[0]);
		return 2;
	}
	const char *path = argv[2];
	const char *expected = argv[3];

	unsigned char *buf = NULL;
	long len = 0;
	if (!file_read_all(path, &buf, &len)) {
		fprintf(stderr, "verify: cannot read %s\n", path);
		return 1;
	}

	if (!elf_is_ours(buf, len)) {
		fprintf(stderr, "verify: %s is not a 32-bit little-endian MIPS executable\n", path);
		free(buf);
		return 1;
	}
	if (!names_this_board(buf, len)) {
		fprintf(stderr, "verify: %s was built for another device (this one is %s)\n", path, player_board_id());
		free(buf);
		return 1;
	}

	sha256_ctx sha;
	sha256_init(&sha);
	sha256_update(&sha, buf, (size_t)len);
	free(buf);
	char hex[SHA256_HEX_LEN];
	sha256_final_hex(&sha, hex);

	if (strcasecmp(hex, expected) != 0) {
		fprintf(stderr, "verify: sha256 %s, the package says %s\n", hex, expected);
		return 1;
	}

	printf("verify: %s ok, %ld bytes, %s\n", path, len, hex);
	return 0;
}

void playerupdate_mark_healthy(void) {
	if (access(UPDATE_LOCK, F_OK) != 0) {
		return; // no update in flight: nothing to confirm
	}

	// This process *is* the confirmation: the launcher writes the lock before it
	// swaps the binary in, and a player that reaches this point got far enough to
	// be believed. Deleting the lock is what ends the transaction -- there is no
	// second marker, because a marker left behind by one update is exactly what
	// made the next one roll back to the build before it.
	const char *root = card_root();
	if (root && *root) {
		char path[320];
		static const char *const leftovers[] = {"player.new", "player.sha256", "pending", "update.tar",
											   "player.prev"};
		for (size_t i = 0; i < sizeof(leftovers) / sizeof(leftovers[0]); i++) {
			snprintf(path, sizeof(path), "%s/sonix/%s", root, leftovers[i]);
			unlink(path);
		}
	}
	if (unlink(UPDATE_LOCK) != 0) {
		fprintf(stderr, "update: cannot remove %s; the launcher will roll back\n", UPDATE_LOCK);
	}
	sync();
	printf("update: running %s on %s, transaction complete\n", player_version(), player_board_id());
}

// ---------------------------------------------------------------------------
// The package
//
// A plain tar (see tools/make_update_package.sh for why not a .tar.gz): a
// manifest first, the binary second, both 512-byte blocked. The manifest names
// the board, the version and the file's size and sha256, and everything is
// checked before a byte lands in <card>/sonix: a package for another device, an
// older version, or a digest that does not match never reaches the launcher.
//
// The install writes three files and nothing else:
//
//   <card>/sonix/player.new      the binary, streamed out of the tar while it is
//                                hashed, so a 7 MB file never sits in RAM
//   <card>/sonix/player.sha256   its digest, which the launcher hands to --verify
//   <card>/sonix/pending         "there is something to install at the next boot"
//
// The reboot is the caller's: this module only prepares the card.
// ---------------------------------------------------------------------------

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include <pthread.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/net/http.h"
#include "src/system/core/semver.h"
#include "src/system/device/system.h"

#define PACKAGE_MANIFEST_MAX 8192

// Where the update index lives. Hardcoded on purpose: this player updates from
// one repository, the one it is built in (git remote), and a device that had to
// be told where to look would be a device whose updates depend on a setting
// nobody remembers. The URL is the release's own asset path, so it always names
// the newest release without an API call; the digest in that file is the anchor
// the package is checked against. [update] index_url in device_config.ini
// overrides it, which is how a local server is tested against.
#define PLAYERUPDATE_INDEX_URL                                                                                     \
	"https://github.com/Givemegitpls/sonix-player-tempotec-v3-analog/releases/latest/download/index.json"
#define TAR_BLOCK 512
#define PACKAGE_NAME_MAX 96

typedef struct {
	char version[64];
	char board[32];
	char file[PACKAGE_NAME_MAX];
	long size;
	char sha256[SHA256_HEX_LEN];
	char requires_image[32]; // the rootfs base this package needs, or empty
	long data_offset; // where the file's bytes start in the package
} package_t;

static unsigned long long tar_octal(const char *field, size_t len) {
	unsigned long long v = 0;
	for (size_t i = 0; i < len && field[i]; i++) {
		if (field[i] == ' ') {
			continue;
		}
		if (field[i] < '0' || field[i] > '7') {
			break;
		}
		v = v * 8 + (unsigned)(field[i] - '0');
	}
	return v;
}

// One tar member at `offset`: name, size and where its data lives. False at the
// end of the archive (a zero block) or on a malformed header.
static bool tar_member(FILE *f, long offset, char *name, size_t name_size, long *size, long *data_offset) {
	unsigned char header[TAR_BLOCK];
	if (fseek(f, offset, SEEK_SET) != 0 || fread(header, 1, TAR_BLOCK, f) != TAR_BLOCK) {
		return false;
	}
	bool zero = true;
	for (int i = 0; i < TAR_BLOCK; i++) {
		if (header[i]) {
			zero = false;
			break;
		}
	}
	if (zero) {
		return false;
	}
	// Without the magic this is not a tar we wrote.
	if (memcmp(header + 257, "ustar", 5) != 0) {
		return false;
	}
	snprintf(name, name_size, "%.*s", 100, (const char *)header);
	*size = (long)tar_octal((const char *)header + 124, 12);
	*data_offset = offset + TAR_BLOCK;
	return true;
}

// Reads the manifest into `pkg`. On success pkg->data_offset points at the
// binary's first byte.
static bool package_read(const char *path, package_t *pkg, char *why, size_t why_n) {
	memset(pkg, 0, sizeof(*pkg));
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(why, why_n, "cannot open %s", path);
		return false;
	}

	char name[PACKAGE_NAME_MAX];
	long size = 0, data = 0;
	if (!tar_member(f, 0, name, sizeof(name), &size, &data) || strcmp(name, "manifest.json") != 0) {
		snprintf(why, why_n, "no manifest.json first in the archive");
		fclose(f);
		return false;
	}
	if (size <= 0 || size > PACKAGE_MANIFEST_MAX) {
		snprintf(why, why_n, "the manifest is %ld bytes", size);
		fclose(f);
		return false;
	}

	char text[PACKAGE_MANIFEST_MAX + 1];
	if (fseek(f, data, SEEK_SET) != 0 || fread(text, 1, (size_t)size, f) != (size_t)size) {
		snprintf(why, why_n, "cannot read the manifest");
		fclose(f);
		return false;
	}
	text[size] = 0;

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		snprintf(why, why_n, "the manifest is not valid JSON");
		fclose(f);
		return false;
	}

	int root = json_root(&doc);
	char kind[16] = "";
	long format = json_obj_long(&doc, root, "format", 0);
	bool ok = false;

	if (!json_obj_str(&doc, root, "kind", kind, sizeof(kind)) || strcmp(kind, "player") != 0) {
		snprintf(why, why_n, "not a player package (kind '%s')", kind);
	} else if (format != 1) {
		snprintf(why, why_n, "package format %ld, this player reads 1", format);
	} else if (!json_obj_str(&doc, root, "version", pkg->version, sizeof(pkg->version)) ||
			   !json_obj_str(&doc, root, "board", pkg->board, sizeof(pkg->board))) {
		snprintf(why, why_n, "the manifest names no version or no board");
	} else {
		int files = json_get(&doc, root, "files");
		int first = files >= 0 ? json_at(&doc, files, 0) : -1;
		if (first < 0) {
			snprintf(why, why_n, "the manifest lists no files");
		} else if (!json_obj_str(&doc, first, "name", pkg->file, sizeof(pkg->file))) {
			snprintf(why, why_n, "the manifest's file has no name");
		} else {
			pkg->size = json_obj_long(&doc, first, "size", 0);
			json_obj_str(&doc, first, "sha256", pkg->sha256, sizeof(pkg->sha256));
			json_obj_str(&doc, root, "requires_image", pkg->requires_image, sizeof(pkg->requires_image));
			ok = true;
		}
	}
	json_free(&doc);

	if (ok) {
		// The binary has to be the next member and to be the size the manifest
		// promised -- a package whose two halves disagree is not ours.
		long next = data + ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
		if (!tar_member(f, next, name, sizeof(name), &size, &data) || strcmp(name, pkg->file) != 0) {
			snprintf(why, why_n, "the archive holds no '%s' after the manifest", pkg->file);
			ok = false;
		} else if (pkg->size <= 0 || size != pkg->size) {
			snprintf(why, why_n, "'%s' is %ld bytes, the manifest says %ld", pkg->file, size, pkg->size);
			ok = false;
		} else {
			pkg->data_offset = data;
		}
	}

	fclose(f);
	return ok;
}

// Streams the binary out of the package and hashes it on the way, so a 7 MB
// file costs 64 KB of buffer and not 7 MB of heap.
static bool package_extract(const char *package, const package_t *pkg, const char *dest, char *why, size_t why_n) {
	FILE *in = fopen(package, "rb");
	FILE *out = in ? fopen(dest, "wb") : NULL;
	if (!in || !out) {
		snprintf(why, why_n, "cannot open %s", in ? dest : package);
		if (in) {
			fclose(in);
		}
		if (out) {
			fclose(out);
		}
		return false;
	}

	static unsigned char buf[65536];
	sha256_ctx sha;
	sha256_init(&sha);
	long left = pkg->size;
	bool ok = fseek(in, pkg->data_offset, SEEK_SET) == 0;
	while (ok && left > 0) {
		size_t want = left < (long)sizeof(buf) ? (size_t)left : sizeof(buf);
		size_t got = fread(buf, 1, want, in);
		if (got != want || fwrite(buf, 1, got, out) != got) {
			ok = false;
			break;
		}
		sha256_update(&sha, buf, got);
		left -= (long)got;
	}
	if (ok) {
		fflush(out);
		fsync(fileno(out));
	}
	fclose(in);
	fclose(out);
	if (!ok) {
		snprintf(why, why_n, "cannot write %s (is the card full?)", dest);
		unlink(dest);
		return false;
	}

	char hex[SHA256_HEX_LEN];
	sha256_final_hex(&sha, hex);
	if (strcasecmp(hex, pkg->sha256) != 0) {
		snprintf(why, why_n, "sha256 %s, the manifest says %s", hex, pkg->sha256);
		unlink(dest);
		return false;
	}
	return true;
}

static bool write_text(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	if (!f) {
		return false;
	}
	bool ok = fputs(text, f) >= 0 && fflush(f) == 0;
	if (ok) {
		fsync(fileno(f));
	}
	fclose(f);
	return ok;
}

const char *playerupdate_verdict_name(playerupdate_result_t r) {
	switch (r) {
	case PLAYERUPDATE_OK:
		return "ready";
	case PLAYERUPDATE_SAME:
		return "same";
	case PLAYERUPDATE_ASK:
		return "ask";
	case PLAYERUPDATE_OLDER:
		return "older";
	case PLAYERUPDATE_WRONG_BOARD:
		return "wrong board";
	case PLAYERUPDATE_NEEDS_IMAGE:
		return "needs image";
	case PLAYERUPDATE_NO_CARD:
		return "no card";
	case PLAYERUPDATE_NOT_A_PACKAGE:
		break;
	}
	return "not a package";
}

// The card's root. The player learns it from the storage layer at start-up; the
// CLI modes run before that, so on a host build the environment's SONIX_SD_ROOT
// is the answer -- which is what makes the package reader testable without a
// device at all.
static const char *card_root(void) {
#ifdef HOST_BUILD
	// The simulator's card is the folder SONIX_SD_ROOT names (main.c's
	// host_card_root), while storage_sd_root() answers with the device's own
	// /mnt/sd_0 -- so the environment comes first here.
	const char *host_root = getenv("SONIX_SD_ROOT");
	if (host_root && *host_root) {
		return host_root;
	}
#endif
	const char *root = storage_sd_root();
	if (root && *root) {
		return root;
	}
	// The storage layer has not run yet, so the candidates it will choose from are
	// the answer: /mnt/sd_0 on this firmware, and the two names other boards give
	// it (system.h). Whichever exists is mounted; nothing is created here.
	size_t count = 0;
	const char *const *names = storage_sd_root_aliases(&count);
	for (size_t i = 0; i < count; i++) {
		if (names[i] && access(names[i], R_OK) == 0) {
			return names[i];
		}
	}
	return NULL;
}

playerupdate_result_t playerupdate_install(const char *package_path, char *why, size_t why_n) {
	const char *root = card_root();
	if (!root || !*root) {
		snprintf(why, why_n, "no card");
		return PLAYERUPDATE_NO_CARD;
	}

	package_t pkg;
	if (!package_read(package_path, &pkg, why, why_n)) {
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}

	if (strcmp(pkg.board, player_board_id()) != 0) {
		snprintf(why, why_n, "package for '%s', this device is '%s'", pkg.board, player_board_id());
		return PLAYERUPDATE_WRONG_BOARD;
	}

	// The package may require a rootfs base it was built against (a launcher change,
	// say): "1.3.0" means the image must be that vendor base or newer. The running
	// image's own version is where the answer comes from, not the player's.
	if (pkg.requires_image[0]) {
		char image[64];
		snprintf(image, sizeof(image), "%s", sysinfo_os_version());
		char *slash = strchr(image, '/');
		if (slash) {
			*slash = 0;
		}
		char need[32];
		snprintf(need, sizeof(need), "%s", pkg.requires_image);
		slash = strchr(need, '/');
		if (slash) {
			*slash = 0;
		}
		// "Is the image the package needs newer than the one running?" -- NEWER is
		// the refusal, not OLDER: get this backwards and the package the image
		// cannot support installs anyway.
		version_verdict_t image_verdict = version_decide(need, image);
		if (image_verdict == VERSION_NEWER || image_verdict == VERSION_ASK) {
			snprintf(why, why_n, "needs firmware image %s, this one is %s", need, image[0] ? image : "?");
			return PLAYERUPDATE_NEEDS_IMAGE;
		}
	}

	version_verdict_t verdict = version_decide(pkg.version, player_version());
	if (verdict == VERSION_SAME) {
		snprintf(why, why_n, "%s is what is running", pkg.version);
		return PLAYERUPDATE_SAME;
	}
	if (verdict == VERSION_OLDER) {
		snprintf(why, why_n, "%s is older than %s", pkg.version, player_version());
		return PLAYERUPDATE_OLDER;
	}

	char dir[256], binary[300], sidecar[300], pending[300];
	snprintf(dir, sizeof(dir), "%s/sonix", root);
	snprintf(binary, sizeof(binary), "%s/%s", dir, "player.new");
	snprintf(sidecar, sizeof(sidecar), "%s/%s", dir, "player.sha256");
	snprintf(pending, sizeof(pending), "%s/%s", dir, "pending");
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		snprintf(why, why_n, "cannot create %s", dir);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}

	if (!package_extract(package_path, &pkg, binary, why, why_n)) {
		// Nothing of this package may be left looking installable: a pending marker
		// with no binary behind it is exactly the state the launcher has to refuse.
		unlink(sidecar);
		unlink(pending);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}

	char line[SHA256_HEX_LEN + 2];
	snprintf(line, sizeof(line), "%s\n", pkg.sha256);
	if (!write_text(sidecar, line) || !write_text(pending, pkg.version)) {
		snprintf(why, why_n, "cannot write the marker files");
		unlink(binary);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}
	sync();

	printf("update: %s staged, %s over %s\n", pkg.version, pkg.version, player_version());
	return verdict == VERSION_NEWER ? PLAYERUPDATE_OK : PLAYERUPDATE_ASK;
}

// Downloads a package to the card and installs from there. Streamed into a file,
// so a 7 MB package never sits in RAM: the player's whole heap is smaller than
// that on a good day.
static bool package_download(const char *url, const char *dest, char *why, size_t why_n) {
	if (!url || strncmp(url, "http://", 7) != 0) {
		snprintf(why, why_n, "only http:// is accepted here, not '%s'", url ? url : "");
		return false;
	}

	FILE *f = fopen(dest, "wb");
	if (!f) {
		snprintf(why, why_n, "cannot create %s", dest);
		return false;
	}

	http_stream_t st;
	memset(&st, 0, sizeof(st));
	st.fd = -1;
	if (!http_stream_open(&st, url, 30)) {
		const char *err = http_last_error();
		snprintf(why, why_n, "download did not start: %s", err && err[0] ? err : "no reason");
		fclose(f);
		unlink(dest);
		return false;
	}

	static char chunk[32768];
	long total = 0;
	bool ok = true;
	for (;;) {
		int got = http_stream_read(&st, chunk, (int)sizeof(chunk));
		if (got == 0) {
			break;
		}
		if (got < 0) {
			ok = false;
			break;
		}
		if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
			ok = false;
			break;
		}
		total += got;
	}
	http_stream_close(&st);
	if (ok) {
		fflush(f);
		fsync(fileno(f));
	}
	fclose(f);

	if (!ok || total <= 0) {
		snprintf(why, why_n, "the download stopped at %ld bytes", total);
		unlink(dest);
		return false;
	}
	printf("update: downloaded %ld bytes from %s\n", total, url);
	return true;
}

// The places a package is looked for, in order: the CI's own name for this board
// at the card's root, the generic name, then anything under <card>/sonix/ that
// looks like one.
bool playerupdate_find_package(char *out, size_t out_size) {
	const char *root = card_root();
	if (!root || !*root) {
		return false;
	}
	snprintf(out, out_size, "%s/sonix-player-update-%s.tar", root, player_board_id());
	if (access(out, R_OK) == 0) {
		return true;
	}
	snprintf(out, out_size, "%s/sonix-player-update.tar", root);
	if (access(out, R_OK) == 0) {
		return true;
	}

	char dir[256];
	snprintf(dir, sizeof(dir), "%s/sonix", root);
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}
	struct dirent *e;
	bool found = false;
	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);
		if (n > 4 && strcmp(e->d_name + n - 4, ".tar") == 0) {
			snprintf(out, out_size, "%s/%s", dir, e->d_name);
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

// ---------------------------------------------------------------------------
// The network path
//
// One file to fetch before anything else: the release's index.json, which names,
// per board, the version, the package, its size, its sha256 and the tag message
// as notes. The URL is compiled in (PLAYERUPDATE_INDEX_URL) and the digest in
// that file is what the archive is held to -- the trust anchor recorded in
// docs/PLAYER-UPDATE.md.
//
// The package URL is derived from the index URL rather than stored: the index
// lives at .../releases/latest/download/index.json, so the asset beside it is
// .../releases/latest/download/<package>, and GitHub redirects that to the
// tagged file. An explicit "url" in the entry overrides it, which is what a
// mirror or a local test server uses.
// ---------------------------------------------------------------------------

#define INDEX_LIMIT (256 * 1024)
#define INDEX_TIMEOUT_SECS 20
#define NOTES_MAX 2048

bool playerupdate_remote_index_url(char *out, size_t size) {
	// The environment first: the CLI modes run before the settings are read, and a
	// test that points the player at a local server must not need a config file it
	// cannot reach yet.
	const char *env = getenv("SONIX_UPDATE_INDEX_URL");
	if (env && *env) {
		snprintf(out, size, "%s", env);
		return true;
	}
	const char *configured = config_get("update", "index_url", "");
	snprintf(out, size, "%s", configured && *configured ? configured : PLAYERUPDATE_INDEX_URL);
	return out[0] != 0;
}

// .../latest/download/index.json + "x.tar" -> .../latest/download/x.tar
static void derive_package_url(const char *index_url, const char *package, char *out, size_t size) {
	const char *slash = strrchr(index_url, '/');
	if (!slash) {
		snprintf(out, size, "%s", package);
		return;
	}
	size_t prefix = (size_t)(slash - index_url) + 1;
	if (prefix >= size) {
		prefix = size - 1;
	}
	memcpy(out, index_url, prefix);
	out[prefix] = 0;
	snprintf(out + prefix, size - prefix, "%s", package);
}

bool playerupdate_remote_check(playerupdate_remote_t *out, char *why, size_t why_n) {
	memset(out, 0, sizeof(*out));
	char url[320];
	playerupdate_remote_index_url(url, sizeof(url));

	char *body = NULL;
	size_t len = 0;
	if (!http_get(url, &body, &len, INDEX_LIMIT, INDEX_TIMEOUT_SECS) || !body) {
		const char *err = http_last_error();
		snprintf(why, why_n, "%s", err && err[0] ? err : "the index did not download");
		free(body);
		return false;
	}
	printf("update: index %s, %zu bytes\n", url, len);

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		snprintf(why, why_n, "the index is not valid JSON");
		free(body);
		return false;
	}

	bool ok = false;
	int devices = json_get(&doc, json_root(&doc), "devices");
	int count = devices >= 0 ? json_len(&doc, devices) : 0;
	for (int i = 0; i < count && !ok; i++) {
		int d = json_at(&doc, devices, i);
		char board[32] = "";
		if (d < 0 || !json_obj_str(&doc, d, "board", board, sizeof(board)) ||
			strcmp(board, player_board_id()) != 0) {
			continue;
		}
		long size = json_obj_long(&doc, d, "size", 0);
		char version[64] = "", package[PACKAGE_NAME_MAX] = "", sha[SHA256_HEX_LEN] = "", explicit_url[320] = "";
		if (!json_obj_str(&doc, d, "version", version, sizeof(version)) ||
			!json_obj_str(&doc, d, "package", package, sizeof(package)) ||
			!json_obj_str(&doc, d, "sha256", sha, sizeof(sha))) {
			snprintf(why, why_n, "the index's entry for %s is incomplete", player_board_id());
			break;
		}
		snprintf(out->version, sizeof(out->version), "%s", version);
		snprintf(out->package, sizeof(out->package), "%s", package);
		snprintf(out->sha256, sizeof(out->sha256), "%s", sha);
		out->size = size;
		if (json_obj_str(&doc, d, "notes", out->notes, sizeof(out->notes)) == false) {
			out->notes[0] = 0;
		}
		if (json_obj_str(&doc, d, "url", explicit_url, sizeof(explicit_url)) && explicit_url[0]) {
			snprintf(out->url, sizeof(out->url), "%s", explicit_url);
		} else {
			derive_package_url(url, package, out->url, sizeof(out->url));
		}
		ok = true;
	}
	json_free(&doc);
	free(body);

	if (!ok && why[0] == 0) {
		snprintf(why, why_n, "no package in the index for %s", player_board_id());
	}
	return ok;
}

// Progress for the card: polled on the UI thread while a worker downloads.
static volatile long dl_done, dl_total;
static volatile bool dl_cancel;

void playerupdate_progress(long *done, long *total) {
	*done = dl_done;
	*total = dl_total;
}

void playerupdate_cancel(void) { dl_cancel = true; }

// Downloads the package named by the index, holds it to the index's digest, then
// hands it to the same install path the card uses. Blocking: call from a worker.
playerupdate_result_t playerupdate_remote_install(const playerupdate_remote_t *remote, char *why, size_t why_n) {
	const char *root = card_root();
	if (!root || !*root) {
		snprintf(why, why_n, "no card");
		return PLAYERUPDATE_NO_CARD;
	}

	char dir[256], path[320];
	snprintf(dir, sizeof(dir), "%s/sonix", root);
	snprintf(path, sizeof(path), "%s/update.tar", dir);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		snprintf(why, why_n, "cannot create %s", dir);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}

	dl_done = 0;
	dl_total = remote->size > 0 ? remote->size : 0;
	dl_cancel = false;

	FILE *f = fopen(path, "wb");
	if (!f) {
		snprintf(why, why_n, "cannot create %s", path);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}

	http_stream_t st;
	memset(&st, 0, sizeof(st));
	st.fd = -1;
	if (!http_stream_open(&st, remote->url, 30)) {
		const char *err = http_last_error();
		snprintf(why, why_n, "download did not start: %s", err && err[0] ? err : "no reason");
		fclose(f);
		unlink(path);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}

	static char chunk[32768];
	sha256_ctx sha;
	sha256_init(&sha);
	bool ok = true;
	for (;;) {
		if (dl_cancel) {
			snprintf(why, why_n, "cancelled");
			ok = false;
			break;
		}
		int got = http_stream_read(&st, chunk, (int)sizeof(chunk));
		if (got == 0) {
			break;
		}
		if (got < 0) {
			ok = false;
			break;
		}
		if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
			ok = false;
			break;
		}
		sha256_update(&sha, chunk, (size_t)got);
		dl_done += got;
	}
	http_stream_close(&st);
	if (ok) {
		fflush(f);
		fsync(fileno(f));
	}
	fclose(f);

	if (!ok) {
		snprintf(why, why_n, why[0] ? why : "the download stopped at %ld bytes", dl_done);
		unlink(path);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}
	if (dl_done <= 0) {
		snprintf(why, why_n, "the download was empty");
		unlink(path);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}

	// The archive's own digest, from the index: the one check that a card path
	// does not have, because there the card is the source.
	char hex[SHA256_HEX_LEN];
	sha256_final_hex(&sha, hex);
	if (remote->sha256[0] && strcasecmp(hex, remote->sha256) != 0) {
		snprintf(why, why_n, "the archive is sha256 %s, the index says %s", hex, remote->sha256);
		unlink(path);
		return PLAYERUPDATE_NOT_A_PACKAGE;
	}
	printf("update: downloaded %ld bytes, sha256 %s\n", dl_done, hex);

	playerupdate_result_t result = playerupdate_install(path, why, why_n);
	if (result != PLAYERUPDATE_OK && result != PLAYERUPDATE_ASK) {
		unlink(path); // a package that will not be installed is not worth keeping
	}
	return result;
}
