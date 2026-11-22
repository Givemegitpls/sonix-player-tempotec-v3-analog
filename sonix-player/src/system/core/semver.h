#ifndef SEMVER_H
#define SEMVER_H

#include <stdbool.h>

// Is the package on the card newer than the build that is running?
//
// Versions are the string the Makefile compiles in (version.h): "<base>/<tag>"
// on a release, "<base>/#<commit>" off one, "<base>/<md5>" for a tree that is not
// a commit, with "-dirty" on top when the tree carries changes. Only the part
// after the last '/' is compared: the base names the vendor firmware the rootfs
// came from and says nothing about the player.
//
// The rule, decided deliberately and documented in docs/PLAYER-UPDATE.md:
//
//   * a tag beats anything that is not a tag. A release is a statement that this
//     build was tested and published; a commit hash is a statement that someone
//     ran make;
//   * two tags compare numerically, component by component, missing components
//     being zero ("1.3.0" == "1.3.0.0");
//   * two non-tags are SAME when the strings are equal, ASK otherwise. A commit
//     hash has no order, and a dirty build is not a version at all -- the player
//     will install one only after the user says so, whatever number it carries;
//   * "-dirty" never makes a build newer. A clean tag over a dirty build of a
//     smaller number is still an upgrade, because a release is a statement that
//     the build was tested and published.
typedef enum {
	VERSION_OLDER = 0, // the package is older: refuse, with the reason
	VERSION_SAME,	   // the same build: nothing to do
	VERSION_NEWER,	   // install it
	VERSION_ASK,	   // no order (commit hashes, dirty builds): ask the user
} version_verdict_t;

// The decision for installing `candidate` over `current`.
version_verdict_t version_decide(const char *candidate, const char *current);

// True when the part after the last '/' is a numeric tag ("1.3.0", "1.3.0-dirty").
bool version_is_tag(const char *version);

// True when the version carries the "-dirty" suffix.
bool version_is_dirty(const char *version);

// "older", "same", "newer", "ask" -- for the log and for --version-check.
const char *version_verdict_name(version_verdict_t verdict);

#endif /* SEMVER_H */
