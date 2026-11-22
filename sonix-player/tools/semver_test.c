// semver_test -- the "is this package newer?" table, as an executable.
//
// The rule lives in src/system/core/semver.c, which depends on nothing but
// libc, so both a developer's machine and the CI can build this file with the
// host compiler and run it -- no SDL, no LVGL, no MIPS toolchain:
//
//   cc -O2 -o /tmp/semver_test tools/semver_test.c src/system/core/semver.c
//   /tmp/semver_test
//
// Every case below is a decision someone had to make, not a permutation: they
// are the ones in docs/PLAYER-UPDATE.md, "Which version wins".
#include <stdio.h>
#include <string.h>

#include "src/system/core/semver.h"

static int failures;

static void expect(version_verdict_t want, const char *candidate, const char *current, const char *why) {
	version_verdict_t got = version_decide(candidate, current);
	if (got == want) {
		printf("ok    %-24s over %-24s -> %-5s (%s)\n", candidate, current, version_verdict_name(got), why);
		return;
	}
	printf("FAIL  %-24s over %-24s -> %s, want %s (%s)\n", candidate, current, version_verdict_name(got),
		   version_verdict_name(want), why);
	failures++;
}

int main(void) {
	// Tags compare numerically, component by component, missing ones being zero.
	expect(VERSION_NEWER, "1.3.0/0.4.1", "1.3.0/0.4.0", "patch up");
	expect(VERSION_OLDER, "1.3.0/0.3.9", "1.3.0/0.4.0", "patch down");
	expect(VERSION_NEWER, "1.3.0/0.10.0", "1.3.0/0.9.0", "numeric, not lexical");
	expect(VERSION_SAME, "1.3.0/1.3.0", "1.3.0/1.3.0", "the same release");
	expect(VERSION_SAME, "1.3.0/1.3", "1.3.0/1.3.0", "a missing component is zero");
	expect(VERSION_NEWER, "1.4.0/9.9.9", "1.3.0/0.1.0", "the base is the vendor firmware, not the player");

	// A tag beats a commit hash or a dirty tree; the other way round needs the user.
	expect(VERSION_NEWER, "1.3.0/1.3.0", "1.3.0/#abc1234", "a release over a commit");
	expect(VERSION_NEWER, "1.3.0/1.3.0", "1.3.0/0.4.0-dirty", "a release over a dirty build");
	expect(VERSION_NEWER, "1.3.0/0.5.0", "1.3.0/0.4.0-dirty", "a newer release over a dirty build");
	expect(VERSION_OLDER, "1.3.0/0.1.0", "1.3.0/0.4.0-dirty", "an older release is older, dirty or not");
	expect(VERSION_ASK, "1.3.0/#abc1234", "1.3.0/1.3.0", "a commit over a release asks");
	expect(VERSION_ASK, "1.3.0/0.4.1-dirty", "1.3.0/0.4.0", "a dirty build asks even with a bigger number");

	// Commit hashes have no order at all: same string is SAME, everything else asks.
	expect(VERSION_ASK, "1.3.0/#def5678", "1.3.0/#abc1234", "two commits ask");
	expect(VERSION_SAME, "1.3.0/#abc1234", "1.3.0/#abc1234", "the same commit");
	expect(VERSION_ASK, "1.3.0/9f2c1a4b", "1.3.0/1.3.0", "a packer md5 suffix asks");
	expect(VERSION_ASK, "1.3.0/9f2c1a4b", "1.3.0/7d0b3c1e", "two md5 suffixes ask");

	// Dirty never wins on its own.
	expect(VERSION_ASK, "1.3.0/1.3.0-dirty", "1.3.0/1.3.0", "a dirty build of the release still asks");

	// Shapes that are not versions we produce are not tags.
	expect(VERSION_ASK, "1.3.0/1.3.0.1", "1.3.0/1.3.0", "four components are not a tag");
	expect(VERSION_ASK, "", "1.3.0/1.3.0", "an empty version asks nothing of itself");
	expect(VERSION_ASK, "1.3.0/v1.3.0", "1.3.0/1.3.0", "a leading v is not our tag shape");

	printf("\n%s: %d failure%s\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
