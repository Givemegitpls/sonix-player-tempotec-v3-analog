# Unloading an app addon when its screen is left

Status: proposal, not implemented. The ABI is unchanged; no new symbol is
published.

## Summary

Today an addon's `.so` is opened the first time its tile is tapped and stays
mapped until the card leaves, its files change on the card, or the player exits
(`src/addon/addon.c`, `addon_open()` / `addon_card_gone()`). Games, the quiz and
the store keep their library and their widgets alive long after the user has
walked away from them.

The proposal: an **app** addon is stopped and `dlclose()`d as soon as none of its
screens is on the display. A **service** addon (`"service": true`) is never
unloaded this way. The behaviour is opt-in per addon through a manifest key, so
that neither an old addon on new firmware nor a new addon on old firmware
changes its mind about what is safe.

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
// src/addon/addon.c
// Unloads every app addon whose screens are no longer the one on display.
// Services stay: their workers and callbacks are the player's own dependencies.
void addon_unload_offstage(void)
{
    lv_obj_t *active = lv_screen_active();
    for (int i = 0; i < slot_count; i++) {
        addon_slot_t *slot = &slots[i];
        if (!slot->loaded || slot->is_service || !slot->unload_on_leave || slot->leave_refused) {
            continue;
        }
        bool on_display = false;
        for (int s = 0; s < slot->screen_count; s++) {
            if (slot->screens[s] == active) {
                on_display = true;
            }
        }
        if (!on_display) {
            addon_unload_on_leave(i);
        }
    }
}
```

Called as the last statement of `load_screen()` in
`src/gui/shell/switcher.c`, after `lv_screen_load(target)`. Every page change
goes through that function, including `switch_screen_no_history()` (the player
sheet putting its opener back) and `switch_screen_return_to()`.

A sub-page of the same addon (quiz -> its Settings/Records, the store -> an
addon's page) is still "on display": the test is the active `lv_obj_t`, not the
previous page. Opening the player sheet itself does not call `load_screen`, so a
sheet over an addon does not unload it.

### Soft failure: `addon_unload_on_leave()`

Different order from `addon_unload()`:

1. call `slot->stop()`;
2. if it returns `false` — change nothing: no screen deletion, no
   `switcher_forget_screen()`, no `dlclose()`. Log
   `addon: <id> cannot unload on leave`, set `leave_refused`, and leave the tile
   working. The library is exactly as it was (`loaded`, screens alive);
3. if it returns `true` — forget the screens in switcher and player, delete the
   widgets, drop pending `gui_post` calls from the library's range, `dlclose()`,
   clear the slot.

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

- `quiz` — needs a round-worker counter and a bounded wait in `stop()`; today
  the worker is detached and unaccounted, and returning `true` with it alive
  would let it jump into unmapped code.
- `addonstore` — its download may sit in `http_stream_read` for up to the read
  timeout. Keep the active `http_stream_t` behind a mutex and call
  `http_stream_wake()` from `stop()`; then the existing wait finishes.
- Games (`2048`, `dino`, `flappybird`, `gearboy`, `gravity`, `tictactoe`),
  `hello`, `cassette`, `daily` — already survive the card-removal unload; they
  need a device run of leaving and re-entering, no code change expected.
- Services (`qobuz`, `subsonic`, `yandex`, `tidal`, `zvuk`, `spotify`, `piped`)
  — never unloaded on leave; they keep `"service": true`.

## Tests

Build (local toolchain, no docker image pull):

```
make -C sonix-player target
make -C <addons> target ADDON_IDS="quiz addonstore"
make -C <addons> checks
```

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
   and screen keep working, no second attempt.

## Rollout

1. Firmware with the hook, the manifest key and the soft-failure order; no
   manifest carries the key yet, so the release changes nothing for existing
   cards.
2. `"unload": "leave"` is added to one addon release at a time after its device
   run (quiz first, then the games, then the store). This needs no firmware.
3. Rollback is removing the key in the next addon release; already-flashed
   devices then go back to keeping it loaded.

## Open questions

- key spelling and values: `"unload": "leave"` is the proposal; alternatives are
  `"unload_on_leave": true` or a per-mode string, but one value is enough today;
- whether the store should show "will stay loaded on this firmware" — it cannot
  detect the firmware without a new exported symbol, so probably not;
- whether `addon_unload_on_leave()` should stop the app's audio itself or leave
  it to `SCREEN_UNLOADED` — the addon's unload hook already runs there, and the
  player must not guess which sound belongs to the addon.
