# Unloading an app addon when its screen is left

Status: implemented (firmware `addon_unload_offstage()`, addons' manifests and the
store's `stop()`), verified on the device. The ABI is unchanged; no new symbol is
published, and an old player ignores the key.

What was seen, 2026-11-21, on the V3 Analog with all eleven app addons carrying
the key and every service without it: each app addon logged `addon: <id> opened`
on the tap and `addon: <id> stopped` on the way back, and its `addon.so`
disappeared from `/proc/<pid>/maps`; `yandex` (a service) opened and stayed
mapped; the store was stopped twice, once with a download in flight; gravity was
stopped with its game thread and returned the process to its exact idle RSS
(20 184 kB -> 22 148 kB -> 20 184 kB). No `cannot unload on leave`, no
`stays loaded`, no crash in any of it.

The simulator run before it: a `stop()` that returns `false` leaves the slot
intact and is not asked twice; a `"service": true` addon with the key is never
unloaded; an addon without the key stays mapped.

## Summary

Today an addon's `.so` is opened the first time its tile is tapped and stays
mapped until the card leaves, its files change on the card, or the player exits
(`src/addon/addon.c`, `addon_open()` / `addon_card_gone()`). Games, the quiz and
the store keep their library and their widgets alive long after the user has
walked away from them.

The rule: an **app** addon is stopped and `dlclose()`d as soon as none of its
screens is on the display. A **service** addon (`"service": true`) is never
unloaded this way. The behaviour is opt-in per addon through a manifest key, so
that neither an old addon on new firmware nor a new addon on old firmware
changes its mind about what is safe:

```json
{
  "api": 1,
  "title": "Quiz",
  "section": "more",
  "unload": "leave"
}
```

## Why opt-in, and not "all apps by default"

`addon_unload()` has a hard edge: a `stop()` that returns `false` (a worker is
still inside the library) makes the slot `unload_refused`, deletes its screens
and refuses to open that addon again for the rest of the run. That is the right
answer while the card is leaving — the mapping pins the filesystem — but it is a
terrible answer for a page the user has simply navigated away from: one slow
worker and the tile is gone until the next reboot.

Two consequences:

1. only addons whose `stop()` is known to finish quickly opt in;
2. the leave path gets its own soft-failure order (below), which keeps a
   library that says "not yet" exactly where it is instead of half-unloading it.

## Mechanism

### Slot state

`addon_slot_t` (`src/addon/addon.c`) gains:

```c
bool unload_on_leave;   // manifest: "unload": "leave"
bool leave_refused;     // stop() said no once; do not ask again this run
```

`manifest_read()` reads `"unload"` and accepts only the value `"leave"`; every
other value, and an absent key, is `false`.

### The hook

```c
// src/addon/addon.c -- what the scan does, one slot at a time
static void offstage_scan(void)
{
    offstage_pending = false;
    lv_obj_t *active = lv_screen_active();
    for (int i = 0; i < slot_count; i++) {
        addon_slot_t *slot = &slots[i];
        if (!slot->loaded || slot->is_service || !slot->unload_on_leave || slot->leave_refused) {
            continue;
        }
        bool on_display = false;
        for (int s = 0; s < slot->screen_count; s++) {
            if (slot->screens[s] && slot->screens[s] == active) {
                on_display = true;
                break;
            }
        }
        if (!on_display) {
            addon_unload_on_leave(i);
        }
    }
}

void addon_unload_offstage(void)
{
    if (offstage_pending) {
        return; // one scan per cycle: it re-reads the active screen when it runs
    }
    offstage_pending = true;
    lv_async_call(offstage_async, NULL); // runs off the current event/timer cycle
}
```

Called as the last statement of `load_screen()` in
`src/gui/shell/switcher.c`, after `lv_screen_load(target)`. Every page change
goes through that function, including `switch_screen_no_history()` (the player
sheet putting its opener back) and `switch_screen_return_to()`.

**The call only queues the work.** `load_screen()` is reached from a tile's event
callback, so the widgets being deleted belong to a page that may still have an
event in flight, and `dlclose()` must not run with the library's own code on the
stack. `lv_async_call` runs on the GUI thread after the current event and timer
cycle, which is exactly the boundary wanted; the deferred scan re-reads
`lv_screen_active()`, so a page change in between is still handled.

A sub-page of the same addon (quiz -> its Settings/Records, the store -> an
addon's page) is still "on display": the test is the active `lv_obj_t`, not the
previous page. Opening the player sheet itself does not call `load_screen`, so a
sheet over an addon does not unload it.

### Soft failure: `addon_unload_on_leave()`

Different order from `addon_unload()`:

1. call `slot->stop()`;
2. if it returns `false` — change nothing: no screen deletion, no
   `switcher_forget_screen()`, no `dlclose()`. Log
   `addon: <id> cannot unload on leave; keeping it loaded`, set `leave_refused`,
   and leave the tile working. The library is exactly as it was (`loaded`,
   screens alive, `init()` not called again);
3. if it returns `true` — forget the screens in switcher and player,
   `service_unregister()`, delete the widgets, drop pending `gui_post` calls from
   the library's range, `dlclose()`, clear the slot (log `addon: <id> stopped`).

The teardown itself is shared with the card path: `slot_forget_screens()`,
`slot_release(slot, safe)` and `drop_pending_calls()` are the same functions
`addon_unload()` walks, so there is one order and one set of pointer rules.
`unload_refused` keeps its present meaning and its present path (card removal),
where nothing can be kept.

### What is not changed

- `sonix_addon_entry()` / `sonix_addon_stop_entry()` and both structures: no
  field, no new symbol, `SONIX_ADDON_API` stays `1`.
- `addon_card_gone()` still unloads every addon, services included.
- Services, `service_register_paths()`, caches and queues are untouched.

## Compatibility

The manifest key is the only negotiation, and an old player ignores unknown
keys. No addon may reference a symbol added for this feature: a missing symbol
is `undefined symbol` at `dlopen`, and the addon would not open at all.

| firmware | addon | behaviour |
|---|---|---|
| old | new, `"unload": "leave"` | key ignored; the library stays until the card leaves; everything else works |
| old | old | as today |
| new | old (no key) | not unloaded on leave; as today |
| new | new, with the key | `stop()` + `dlclose()` when its screens are left |

### A new addon on old firmware

The library is not unloaded, `init()` is not called again, the same
`lv_obj_t`s come back. Therefore:

- everything that must end when the user walks away (session, sound, timers,
  workers) ends in `SCREEN_UNLOADED`, never in `stop()`;
- the screen is rebuilt/reset in `SCREEN_LOADED`, not by `init()`;
- large buffers are per screen, freed on unload of the screen — not per library;
- `stop()` still has to work for the card-removal path, so the worker accounting
  the new firmware needs is the same work the old firmware already exercises;
- user-visible behaviour must be identical on both firmwares; the difference is
  memory only.

### An old addon on new firmware

Without the key nothing is unloaded on leave, so an old release keeps its
present `stop()` timing and cannot be bricked by the new path. Add the key to a
release only after a device run of that release.

## Addon audit (the opt-in list)

- `quiz` — already had the accounting the first draft of this document asked
  for: `round_enter()`/`round_leave()` and `round_wait("stop")`, so `stop()` is
  false only while a round worker is really inside the library. No code change;
  the key is what was missing.
- `addonstore` — the download could sit in `http_stream_read()` for up to the
  30 s receive timeout, which on the leave path is a `stop()` that returns false
  and a library that stays mapped for the rest of the run. Fixed in
  `addonstore/asnet.c`: the live `http_stream_t` is published behind a mutex and
  `as_cancel()` calls `http_stream_wake()` on it, so the read returns at once and
  the existing bounded wait in `store_stop()` finishes in milliseconds. The
  refresh/scan jobs go through `http_get_ex()`, which cannot be woken; those are
  what the bounded wait is still for.
- Games (`2048`, `dino`, `flappybird`, `gearboy`, `gravity`, `tictactoe`),
  `hello`, `cassette`, `daily` — already survive the card-removal unload; their
  `stop()`s reset pointers, delete timers and withdraw hooks, and the games keep
  their tick timer paused while off-screen. Keys added; no code change.
- Services (`qobuz`, `subsonic`, `yandex`, `tidal`, `zvuk`, `spotify`, `piped`)
  — never unloaded on leave; they keep `"service": true` and no key. Their
  retained memory is page state, which they can free in `SCREEN_UNLOADED`
  themselves; that is their repository's business, not this hook's.
- `piped` is a service in the manifest sense but has no `"service": true` today;
  it cannot opt in by accident because no key is added to it.

## What the win actually is

Measured on the built libraries, `.text`+data of a mapped slot is small: `hello`
25 KB, `cassette` 43, `daily` 45, `tictactoe` 60, `dino` 63, `2048` 67,
`flappybird` 68, `quiz` 117, `addonstore` 202, `gearboy` 2.1 MB, `gravity`
2.1 MB. The file sizes under `build/target/` (20 MB for `gravity`) are debug
sections, which never become memory.

So the memory an unload returns is the addon's *state*, not its code: the widget
tree, the decoded images, and whatever the addon keeps between pages —
`gravity`'s 307 KB frame (480x320 RGB565) plus its game statics, `gearboy`'s
cartridge (allocated whole, `new u8[allocatedSize]`, 0.5-8 MB), `quiz`'s deck and
the hearts/trophy sprites it only frees in `stop()`, `addonstore`'s parsed
release lists and HTTP buffers, `daily`'s cover slot, `cassette`'s two FreeType
faces. The threads matter more than the bytes: `gearboy` and `gravity` keep an
emulator/game thread and its stack, and the games' timers are only paused, not
gone.

The two heaviest addons are therefore `gearboy` and `gravity`; then `quiz` and
the store. Everything else is small enough that the win is housekeeping.

Not done, and deliberately: the decoded images an addon registered with LVGL's
global image cache outlive the `dlclose()`, because deleting widgets does not
evict them. `lv_image_cache_drop(NULL)` after an unload would return that memory
too, at the price of re-decoding the menu's own icons; a range-based drop
(`lv_image_cache_iter_create()`, whose iterator lives in LVGL's private
`lv_iter_private.h`) would drop only the addon's. Left to the cache's own
eviction until the numbers say otherwise.

## Tests

Build (local toolchain, no docker image pull):

```
make -C sonix-player target
make -C <addons> target ADDON_IDS="quiz addonstore"
make -C <addons> checks
```

Verified on the simulator (2026-11-21, `make host` in both trees, three probe
addons: one with the key, one service with the key, one without any key):

| case | expected | seen |
|---|---|---|
| `tictactoe`, `quiz`, `hello` entered then left | `addon: <id> stopped` | yes |
| the same addon re-entered | `addon: <id> opened`, screen rebuilt | yes |
| `stop()` returning `false` | `addon: <id> cannot unload on leave; keeping it loaded`, screens and tile intact, no second attempt | yes |
| `"service": true` with the key | opened, never stopped | yes |
| manifest without the key | opened, never stopped | yes |

Device, new firmware and old firmware:

1. quiz: play a round, leave to More -> log `addon: quiz stopped` on new
   firmware, no `stopped` on old; re-enter starts clean on both; `MemFree`
   returns to its baseline;
2. 2048: start, leave, re-enter; same;
3. store: leave with a download in flight -> worker cancelled, `stopped`, and
   the tile opens again (new firmware); on old firmware the session simply stays
   mapped;
4. `yandex`: navigate around -> never `yandex stopped` (service);
5. sheet over quiz: open and close the player sheet -> quiz not unloaded, the
   run continues (loaded again, session resumed);
6. card removal: everything unloads, services included;
7. an addon whose `stop()` returns `false`: log `cannot unload on leave`, tile
   and screen keep working, no second attempt;
8. `gravity` and `gearboy`: enter, leave, re-enter -> a ride and a ROM start
   again, no crash in the emulator/game thread, `MemFree` delta reported;
9. `addonstore`: leave with no job running -> stopped; with a download running
   -> stopped within a few hundred milliseconds, not `STOP_WAIT_MS`.

What the 2026-11-21 device pass covered (the old-firmware half of each is the
manifest key being ignored, which the simulator case above already proves):

| case | result |
|---|---|
| gearboy, quiz, 2048, tictactoe, dino, daily, cassette, gravity opened and left | `addon: <id> opened` / `stopped` each time; `Addons/<id>/addon.so` gone from `/proc/<pid>/maps` (gearboy: 3 mappings -> 0) |
| addonstore left with a download in flight (13:24:41 -> 13:24:47) | `stopped` in 6 s, not `STOP_WAIT_MS`; the tile opened again |
| yandex (service) opened and left | never stopped, still mapped |
| gravity open/close | RSS 20 184 -> 22 148 -> 20 184 kB; the game thread joined, chevron back worked |
| the whole pass | no `cannot unload on leave`, no `stays loaded`, no crash |

The other half of the compatibility table, on the device: the same card, the
same eleven manifests with the key, flashed back to the build from 2026-11-16
that predates the hook. All addons scan, `gearboy` opens on its tile, and
leaving its page logs nothing and leaves `Addons/gearboy/addon.so` mapped
(3 mappings before and after). Nothing needs the new firmware to keep working.

Not covered yet: re-entering each addon after an unload (done for quiz on the
simulator), the sheet over an addon, and card removal with the keys present;
`MemFree` on the device is too noisy (page cache) to quote, RSS is the number
that moved.

## Rollout

1. Firmware with the hook, the manifest key and the soft-failure order; no
   manifest carries the key yet, so the release changes nothing for existing
   cards. Done here (the addons' repository got the keys in the same change, one
   release ahead of the first firmware that understands them — harmless).
2. `"unload": "leave"` is added to one addon release at a time after its device
   run (quiz first, then the games, then the store). This needs no firmware.
3. Rollback is removing the key in the next addon release; already-flashed
   devices then go back to keeping it loaded.

## Open questions

- key spelling and values: `"unload": "leave"` is what shipped; alternatives are
  `"unload_on_leave": true` or a per-mode string, but one value is enough today;
- whether the store should show "will stay loaded on this firmware" — it cannot
  detect the firmware without a new exported symbol, so probably not;
- whether `addon_unload_on_leave()` should stop the app's audio itself or leave
  it to `SCREEN_UNLOADED` — the addon's unload hook already runs there, and the
  player must not guess which sound belongs to the addon;
- whether the decoded images of an unloaded addon should be dropped from LVGL's
  image cache with it (see "What the win actually is").
