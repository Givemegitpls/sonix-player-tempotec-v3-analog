#include "semver.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>


// The part of a version that names the player build: everything after the last
// '/'. "<base>/<tag>" -> "<tag>", "<base>/#abc1234" -> "#abc1234", and a version
// with no slash at all is taken whole (a build made outside the Makefile).
static const char *build_part(const char *version) {
	const char *slash = version ? strrchr(version, '/') : NULL;
	return slash ? slash + 1 : version;
}

bool version_is_dirty(const char *version) {
	const char *p = build_part(version);
	size_t n = p ? strlen(p) : 0;
	static const char suffix[] = "-dirty";
	size_t s = sizeof(suffix) - 1;
	return n > s && strcmp(p + n - s, suffix) == 0;
}

bool version_is_tag(const char *version) {
	const char *p = build_part(version);
	if (!p || !*p || *p == '#') {
		return false;
	}

	// Numeric components separated by dots, at least one. "1", "1.3", "1.3.0"
	// are all tags; anything with a letter, a '-' or more than three components
	// is not.
	int components = 0;
	int digits = 0;
	for (; *p; p++) {
		if (isdigit((unsigned char)*p)) {
			digits++;
			if (digits > 4) {
				return false;
			}
			continue;
		}
		if (*p == '.') {
			if (digits == 0) {
				return false; // "1..2", ".1"
			}
			components++;
			digits = 0;
			if (components >= 3) {
				return false; // a fourth component is not a version we produce
			}
			continue;
		}
		break;
	}
	if (digits == 0) {
		return false;
	}
	// Past the numbers only "-dirty" may follow.
	if (*p == 0) {
		return true;
	}
	return strcmp(p, "-dirty") == 0;
}

// Compares the numeric components, ignoring "-dirty" (which is handled by the
// caller: a dirty build is never newer than the tag it dirties).
static int tag_compare(const char *a, const char *b) {
	for (int i = 0; i < 3; i++) {
		long va = 0, vb = 0;
		while (isdigit((unsigned char)*a)) {
			va = va * 10 + (*a - '0');
			a++;
		}
		while (isdigit((unsigned char)*b)) {
			vb = vb * 10 + (*b - '0');
			b++;
		}
		if (va != vb) {
			return va < vb ? -1 : 1;
		}
		if (*a == '.') {
			a++;
		}
		if (*b == '.') {
			b++;
		}
	}
	return 0;
}

version_verdict_t version_decide(const char *candidate, const char *current) {
	if (!candidate || !*candidate || !current || !*current) {
		return VERSION_ASK;
	}
	if (strcmp(candidate, current) == 0) {
		return VERSION_SAME;
	}

	bool cand_tag = version_is_tag(candidate);
	bool cur_tag = version_is_tag(current);

	if (cand_tag && !cur_tag) {
		// A published build over a commit or a dirty tree: the one case that is
		// unambiguously an upgrade.
		return VERSION_NEWER;
	}
	if (!cand_tag || version_is_dirty(candidate)) {
		// Commit hashes have no order, and a dirty tree is not a version at all:
		// same string is SAME (handled above), everything else asks. The number a
		// dirty build happens to carry must not let it install by itself.
		return VERSION_ASK;
	}
	if (!cur_tag) {
		// A clean release over a commit hash or a packer md5 suffix: the one case
		// that is unambiguously an upgrade.
		return VERSION_NEWER;
	}

	// Both are tags. The numbers decide; a dirty build of the same number loses to
	// its own release, and a dirty build does not make an older tag newer.
	int cmp = tag_compare(build_part(candidate), build_part(current));
	if (cmp > 0) {
		return VERSION_NEWER;
	}
	if (cmp < 0) {
		return VERSION_OLDER;
	}
	return version_is_dirty(current) ? VERSION_NEWER : VERSION_SAME;

	return VERSION_SAME;
}

const char *version_verdict_name(version_verdict_t v) {
	switch (v) {
	case VERSION_NEWER:
		return "newer";
	case VERSION_SAME:
		return "same";
	case VERSION_ASK:
		return "ask";
	case VERSION_OLDER:
		break;
	}
	return "older";
}
