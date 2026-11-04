#ifndef ADDON_NOTIFY_H
#define ADDON_NOTIFY_H

// The one call from the storage layer into the addons, without the interface
// headers it would otherwise drag in.
//
// The card can arrive long after the player started, and the tiles are built
// from the folder it holds: the card supervisor says so through this, and the
// addon registry re-reads the folder on the UI thread and has the Streaming and
// More pages rebuilt.

// `root` is the card's mount point when it is known, NULL otherwise (a card
// that went away). Safe from any thread: it only posts to the UI thread.
void addon_card_changed(const char *root);

// The other half of it: the card went away and the tiles go with it. This one
// reads nothing -- the mount may be half alive or already dead, and what the
// pages show must not depend on which. Unlike addon_card_changed() this WAITS:
// the caller is on its way to unmounting the card, and a library still mapped
// from it is what makes that unmount fail. False when the GUI thread did not
// answer in time, or when an addon said it could not be unloaded.
bool addon_card_gone(void);
#endif /* ADDON_NOTIFY_H */
