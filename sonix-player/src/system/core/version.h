#ifndef VERSION_H
#define VERSION_H

// The build's own name, as the Makefile computed it: "<base>/<tag>" on a
// release tag, "<base>/#<commit>" off one, "<base>/<md5>" in the packer's
// convention, and a "-dirty" suffix when the tree carries changes the commit
// does not. It is compiled in rather than read from the rootfs on purpose:
// system-info.json belongs to the image, and after a player-only update it
// would name the build that was there before.
//
// Used by the update path (a package is refused unless its version is newer
// than this one) and printed at start-up, which is what a log from a device
// someone else has handled needs first.
const char *player_version(void);

// The device this build is for, board.h's update stem ("v3_analog"): stable,
// lowercase, filesystem-safe. An update package names one, and both the player
// and the verifier refuse a package that names another -- two boards of this
// family differ in the panel, the legend and half of board.h, so the package
// for one is a broken device on the other.
const char *player_board_id(void);

#endif /* VERSION_H */
