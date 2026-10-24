<!--
Thanks for contributing to Sonix Player!

Read the whole checklist BEFORE opening the PR. A PR that skips it is closed
or sent back without a full review. Every box must be ticked, or answered
with "N/A" and a reason.
-->

## What does this PR do?

<!-- One or two sentences, from the user's point of view. -->

## Why?

<!-- The problem it solves. Link the issue if there is one: "Closes #123". -->

## How was it tested?

- [ ] On a real **TempoTec V3 Analog** (the board this tree is ported to)
- [ ] In the simulator (`make host`)

<!-- What you did and what you saw. Screenshots or a short video for anything visible on screen. -->

---

## Checklist

### Scope
- [ ] The PR does **one thing**. Unrelated changes go in their own PR.
- [ ] **No files or folders were moved or renamed** unless the PR is about that and it was agreed in an issue first.
- [ ] **No markdown file was moved.** `README.md`, `FEATURES.md`, `PATCHES.md` stay in the **repository root**; the guides this port adds live in `docs/` and a new one goes there.
- [ ] It is rebased on the current `main`, and the commit titles say what changes (not "fix", "update", "add X").

### Both devices
- [ ] Anything model-specific is a field of the board profile (`sonix-player/src/system/device/board.h`, `sysinfo.h`), not a literal in a page, and it is hidden on the models it does not apply to.

### Translations
- [ ] Every user-visible string is a key present in the language files this image ships (`sonix-packer/assets/V3ANALOG/usr/resource/sonix/language/`, merged from the Blaze set -- see `docs/UPDATING.md`). No hard-coded text in C.

### Binaries and third-party code
- [ ] **No prebuilt binaries** (`.so`, executables, `.ko`) built on your machine, except the firmware's own Bluetooth userland, which this image ships deliberately (`docs/BLUETOOTH.md`). Everything else is built from a pinned source against the device's **glibc 2.16**, and `make check-abi` refuses anything newer.
- [ ] Any third-party code is at its **latest release with no known CVEs**. The version is given in the PR description.
- [ ] No HiBy firmware, HiBy binaries, keys, passwords or `streaming-keys.*` files.

### User experience
- [ ] A user feature lives on the page where a user would look for it, **not in Developer options**. Developer options are for debugging only.
- [ ] A user never needs `adb`, a terminal or a computer to set the feature up or use it. Passwords, paths and options are set from the player's UI.
- [ ] Errors are shown to the user with a message. Nothing fails silently.

### Power and storage
- [ ] Anything that runs in the background, uses Wi-Fi/Bluetooth or writes to the card works with the **screen off and standby** (see `power.c`: `wifi_in_use()`, the suspend checks), or keeps the device awake while it works.
- [ ] Nothing writes to the card when it is **not mounted** or is **exported over USB**, because then the path lands on the internal volume.
- [ ] Nothing writes to the read-only root filesystem. Persistent data goes to the model's `user_data_dir` (`/data` here, `/usr/data` on the HiBy boards) or the card, temporary data to `/tmp`.

### Network services
- [ ] Any server runs **only while its page is open** and stops when the page is left, like Wi-Fi transfer (`src/gui/wireless/wifitransfer.c`).
- [ ] It requires a password set from the UI, and does not rely on "LAN only" for security.

### Code
- [ ] Formatted with the repo's `.clang-format` (tabs, not spaces).
- [ ] Builds with no new warnings: `make host` and `make target`.
- [ ] No dead code, and no copy-pasted helpers: shared code goes in one shared place.
- [ ] Comments in English, ASCII only. They describe the code as it is, not its history ("changed", "copied from", "used to").
- [ ] `.gitignore` only gets entries for files the repo's own build produces, not leftovers of your local setup.
- [ ] `FEATURES.md` (in the root) is updated if the PR adds or changes a feature.
