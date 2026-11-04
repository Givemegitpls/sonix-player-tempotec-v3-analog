#ifndef LISTART_H
#define LISTART_H

#include <stdbool.h>

#include "src/gui/nowplaying/cover.h"

// Cover art for the streaming-service lists, whichever service they belong to.
//
// coverloader.c is not enough: it loads the image of a file already on the
// card, while a streaming service's cover is a URL that has to be downloaded
// first. This is the missing link: a thread that downloads and decodes, with
// coverloader's slot shape -- a list has a fixed number of rows, each row is a
// slot, and asking a slot for something new abandons what it was waiting for,
// which is exactly what scrolling does.
//
// Downloaded images stay on the card in `.local/listart`, named by the MD5 of
// the URL, so looking at the same list a second time downloads nothing. That
// directory is not cleared along with the cached tracks -- it is tens of
// kilobytes in total, and refetching every time would waste far more than the
// space it occupies.
//
// Nothing in here knows about any one service: a URL goes in, an image comes
// out, and the name it is stored under is the MD5 of that URL, so two services
// cannot collide even deliberately. Qobuz, Tidal and the podcast feeds all use
// this, and so can an addon: any http(s) URL of any service is a cover.

// How many list rows may have a cover in flight: the rows that fit on screen,
// plus margin for scrolling.
#define LISTART_SLOTS 12

// Cover URLs are long but not unbounded.
#define LISTART_URL_MAX 512

// Where to keep the images. NULL or empty disables everything (no card, no
// covers).
void listart_set_root(const char *sd_root);

// The slots have one owner at a time, and `owner` is what says who.
//
// There are twelve of them and two pages use them, but only one page is on
// screen at a time, so the slots are handed over rather than shared. Whoever
// asks for a cover becomes the owner and whatever the other page was waiting
// for is dropped; the other page's take call answers "finished", which is what
// it needs to stop asking.
//
// `owner` is any pointer as long as it is stable and differs between the two
// pages -- the address of a static variable does. Without it each page would
// cancel the other's covers and draw them onto the other's rows.

// Requests the cover at `url`, square, `size` pixels, for `slot`. Returns
// immediately. Any earlier request on this slot is abandoned.
void listart_request(const void *owner, int slot, const char *url, int size);

// Forgets the slot: anything in flight is abandoned and anything ready is
// freed. Does nothing when the slots are owned by someone else.
void listart_release(const void *owner, int slot);

// Collects a finished image. False while the thread is still working on it, and
// false when there was nothing to show; `finished` says which of the two, so
// the caller can stop asking. For a caller that no longer owns the slots it is
// always false with `finished` true.
bool listart_take(const void *owner, int slot, cover_image_t *out, bool *finished);

// Throws away every downloaded image, for every service: the directory is
// shared, so this is the "log out of everything" hammer, not a per-service one.
// Goes with logging out -- what is on the card were the covers of what that
// account was listening to.
void listart_clear(void);

#endif /* LISTART_H */
