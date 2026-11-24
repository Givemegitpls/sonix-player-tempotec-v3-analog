#include "version.h"

// The Makefile passes this on every compile; a translation unit built outside
// it (an IDE, a one-off cc for a tool) gets "dev" rather than a wrong build.
#ifndef SONIX_BUILD_VERSION
#define SONIX_BUILD_VERSION "dev"
#endif

#ifndef SONIX_BOARD_ID
#define SONIX_BOARD_ID "unknown"
#endif

const char *player_version(void) { return SONIX_BUILD_VERSION; }

const char *player_board_id(void) { return SONIX_BOARD_ID; }
