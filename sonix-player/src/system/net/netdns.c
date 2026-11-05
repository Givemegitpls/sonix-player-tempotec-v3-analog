#include "netdns.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"

// Where the resolver's configuration lives. On the device that is the file the
// DHCP client writes, and the only place glibc looks. The simulator must never
// be allowed anywhere near the machine's own /etc/resolv.conf -- a run of the
// host build would rewrite the developer's resolver -- so it gets its own file
// beside the simulator's settings (main.c's SONIX_HOST_STATE_DIR).
#ifdef HOST_BUILD
#define NETDNS_FILE "/tmp/sonix/data/resolv.conf"
#else
#define NETDNS_FILE "/etc/resolv.conf"
#endif

// The file is four lines on this device -- two addresses, a search domain and
// the blank line some editors leave. Eight kilobytes is a ceiling, not an
// expectation: anything larger is not a resolver configuration, and reading it
// whole keeps the rewrite atomic from the resolver's point of view (it sees
// either the old file or the new one, never half of each).
#define NETDNS_BUF 8192

// The override as stored, "" when there is none. The config is in memory, so
// this is a hash lookup and no card access.
static const char *stored(int slot) { return config_get("net", slot == 0 ? "dns1" : "dns2", ""); }

// Reads the whole file into `buf`, NUL-terminated, and returns its length. A
// missing file is not an error: it is what a device that has never associated
// looks like.
static size_t read_file(char *buf, size_t size) {
	buf[0] = '\0';
	FILE *f = fopen(NETDNS_FILE, "r");
	if (!f) {
		return 0;
	}
	size_t len = fread(buf, 1, size - 1, f);
	fclose(f);
	buf[len] = '\0';
	return len;
}

// True for a line this module wrote: a nameserver the DHCP client did not
// append, which it marks with " # <interface>". Everything else in the file
// belongs to somebody else and is copied through untouched.
static bool is_our_line(const char *line) {
	if (strncmp(line, "nameserver", 10) != 0) {
		return false;
	}
	return strchr(line, '#') == NULL;
}

// The first `max` nameserver addresses in the file, in file order. Ours are
// written first, so with an override in place those come out first.
static int file_addresses(char out[][NETDNS_ADDRESS_MAX], int max) {
	static char buf[NETDNS_BUF];
	read_file(buf, sizeof(buf));

	int count = 0;
	char *save = NULL;
	for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char address[NETDNS_ADDRESS_MAX];
		if (sscanf(line, "nameserver %15s", address) != 1) {
			continue;
		}
		if (count < max) {
			snprintf(out[count], NETDNS_ADDRESS_MAX, "%s", address);
		}
		count++;
	}
	return count < max ? count : max;
}

void netdns_get(char primary[NETDNS_ADDRESS_MAX], char secondary[NETDNS_ADDRESS_MAX]) {
	char found[8][NETDNS_ADDRESS_MAX];
	int count = file_addresses(found, 8);

	const char *first = stored(0);
	const char *second = stored(1);

	snprintf(primary, NETDNS_ADDRESS_MAX, "%s", first[0] ? first : (count > 0 ? found[0] : ""));
	snprintf(secondary, NETDNS_ADDRESS_MAX, "%s", second[0] ? second : (count > 1 ? found[1] : ""));
}

bool netdns_is_address(const char *text) {
	struct in_addr parsed;
	return text && text[0] && inet_pton(AF_INET, text, &parsed) == 1;
}

static bool apply_now(void) {
	static char buf[NETDNS_BUF];
	read_file(buf, sizeof(buf));

	char primary[NETDNS_ADDRESS_MAX];
	char secondary[NETDNS_ADDRESS_MAX];
	snprintf(primary, sizeof(primary), "%s", stored(0));
	snprintf(secondary, sizeof(secondary), "%s", stored(1));

	// Writing through the path rather than replacing the file: on some
	// firmwares /etc/resolv.conf is a symlink into /tmp, and a rename would
	// leave that link pointing at a file nobody updates any more.
	FILE *f = fopen(NETDNS_FILE, "w");
	if (!f) {
		printf("netdns: cannot write %s\n", NETDNS_FILE);
		return false;
	}

	if (primary[0]) {
		fprintf(f, "nameserver %s\n", primary);
	}
	if (secondary[0]) {
		fprintf(f, "nameserver %s\n", secondary);
	}

	char *save = NULL;
	for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (is_our_line(line)) {
			continue; // a copy of what was just written, or of an older override
		}
		fprintf(f, "%s\n", line);
	}
	fclose(f);
	return true;
}

void netdns_apply(void) { (void)apply_now(); }

bool netdns_set(const char *primary, const char *secondary) {
	config_set("net", "dns1", primary ? primary : "");
	config_set("net", "dns2", secondary ? secondary : "");
	config_save();
	return apply_now();
}
