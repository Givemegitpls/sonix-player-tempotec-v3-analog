# The panel and the display path (V3 Analog)

Consolidated from `.opencode/NOTES.md` (the chronological log, which has the raw measurements)
and the session archive (`tools/history.sh`; session ids at the end).

## The panel

An **SLCD** (a smart panel with its own GRAM) at 320x480, 32 bpp BGRA, driven over an 8-bit
i80-style bus. A frame reaches the glass **only** through `FBIOPAN_DISPLAY`: the vendor driver
swaps the descriptor and restarts the controller's DMA. Writing the framebuffer memory alone
changes nothing visible.

The glass scans its own GRAM at its own rate, and that rate is **not ours to set**:

- the frame boundary is the panel's TE output, wired to `gpio-51 (lcd vsync)` — an **input**,
  IRQ `slcd_vsync`. Measured 75.57 Hz (13.232 ms per panel frame), and it keeps ticking with the
  LCDC disarmed (`ENA=0`) — the panel scans regardless of what we do.
- the LCD data clock (`cgu_lcd`) does **not** move it: 60/75/150 MHz all give 75.8-76.4 Hz. What
  it changes is the transfer: 12.28 ms at 75 MHz, 21.4 ms at 60 (longer than the frame), 23.0 ms
  at 40, and a corrupted screen at 150. 75 MHz is the working value (`pixclock 13333 ps`).
- `lcd` (24 MHz, parent `ext1`) is not writable at all.

So the transfer takes **93 % of the panel's frame**, which is why the port paces itself on the
panel's grid rather than on a timer.

## The pacing, and why the frame period is not a frame rate

- One framebuffer page (`single_fb_page`). Two pages were tried and rejected: the glass then
  always shows the frame drawn two frames ago, and every frame pays a full-screen copy.
- The pan is issued on the panel's grid: cadence = **2 panel frames = 26.46 ms** (37.8 fps). One
  frame is impossible — the transfer alone is 12.29 of 13.23 ms, leaving no room for the copy.
- The copy goes **band by band** behind the panel's read head (60 rows per band), so it starts
  before the transfer ends instead of waiting for it.
- `[screen] frame_period_ms` is the **interface thread's** pace, not the glass rate.
- The pipelined path (two snapshots, the copy moved into the flip thread) exists in `main.c` but
  is **off by default** (`[screen] pipeline`): with it on, animated pages showed vertical stripes
  that survive in the framebuffer, cause still unknown.
- Animations at 37.8 fps: a 480 px move over 7 frames is 70 px per step, which reads as a jerk.
  The quick panel and the track page therefore switch with no animation at all (owner's choice);
  the back gesture keeps a 140 ms linear animation.

## The freeze (fixed) and what it was

`FBIOBLANK(POWERDOWN)` clears `LCDC_CTRL.ENA` **and** `SLCDC_CTRL.DMA_EN`; `UNBLANK` restores only
`ENA`. After that a pan returns success without transferring anything (`SLCDC_STATE.BUSY` never
ticks), and in the worst case the registers read identical to a healthy boot while the glass is
frozen — only a cold panel init clears it. The port therefore **never blanks this panel**
(`blank_panel = false` in the model): screen-off is the backlight alone, and where arming is
needed it is `UNBLANK` → `FBIOPUT_VSCREENINFO(FB_ACTIVATE_NOW|FORCE)` → pan (`set_par` is what
restores `DMA_EN`).

## Power: what "off" can and cannot mean

- **Backlight** — `/sys/class/backlight/pwm-backlight.0/brightness`, written to 0 on screen-off.
  With it off the glass still shows the last frame (an LCD's pixels hold their state), which is
  visible in ambient light — that is the panel, not a backlight that failed to go out.
- **Controller/DMA** — `LCDC_CTRL`/`SLCDC_CTRL`; stopping them is not "off" and is the freeze
  path above.
- **Panel power** — the AXP20x rails `LDO7 = lcd_3v3` (3.0 V) and `LDO8 = lcd_1v8` (1.8 V), plus
  `gpio16 (lcd rst)` and `gpio48 (lcd rd)`. Only `lcd_power_off`/`lcd_power_on` touch them, and
  they are called only from `jzfb_suspend`/`jzfb_resume`. **No blank level touches them**, and
  there is no userspace path to them either: the regulator sysfs has no enable attribute, there
  is no `/dev/i2c*` (so the PMIC cannot be poked), and `gpio16`/`gpio48` are `gpio_request`ed by
  the driver (`EBUSY` on export).
- So **suspend-to-RAM is the only real panel power cycle**, and it is verified on the device
  (including a 598 s sleep, `suspend_stats success:3 fail:0`): the panel comes back armed and
  scanning, the touch controller alive. Standby is on by default, 60 s after the screen goes dark
  (`[power] standby_mem = 0` turns it off).
  **Corrected 2026-10-20 — this bullet and the one above are wrong.** There is a userspace path:
  the stock player powers the panel down on every screen-off with `ioctl(fb0, 0x80044620, &1)`
  (`SET_SLEEP`), which runs `jzfb_suspend` → `lcd_power_off` while the SoC stays awake;
  suspend-to-RAM is a second, later event. Evidence and the corrected sequence:
  [PANEL-POWER.md](PANEL-POWER.md).
- **Open item**: powering the panel off *while the SoC stays awake* (music playing, or the first
  60 s) needs a driver change — make `jzfb_blank` call `lcd_power_off` on POWERDOWN and
  `lcd_power_on` + `set_par` + `enable` on UNBLANK, i.e. reuse the resume sequence that is known
  to work. The port already has the arm path for boards that blank; the model flag would flip.
  **Superseded 2026-10-20:** no driver change is needed — the stock kernel's `SET_SLEEP` ioctl
  (`0x80044620`) already runs that sequence (see [PANEL-POWER.md](PANEL-POWER.md)).

## Instruments and what to read

- `tools/panwatch` — one pan, sampled from `/dev/mem`: when the transfer starts and how long it
  runs (`SLCDC_CTRL` +0xa4, `SLCDC_STATE` +0xa8).
- `tools/fbtool` — `regs`, `put`, `pan`, `poll`; `tools/panctl` — the framebuffer's own idea of
  where it is looking.
- The player's own log, per second: `frames: …` (`draw`, `snap`, `hold`, `copy`, `pace`, `work`)
  and `glass: N gaps, avg … sd … min … max …` — the second one is the glass's own pacing.
- TE rate in one line: read the `slcd_vsync` counter from `/proc/interrupts` twice, `sleep` in
  between.
- The panel's own rate is a **board constant** (`PANEL_FRAME_US`, 13.232 ms): it belongs in the
  board profile next to 320x480, not in a config key.

## Archive

- `ses_ebf126422ffeGc0oeYZiExbNNQ` — "Заставить экран плеера выдавать 40 Гц": the panel rate, the
  clock experiments, the pacing work.
- `ses_ec6820354ffeMOuM0X72my4TUI` — the kernel-side disassembly behind the freeze fix.
- `ses_ebeb2638affea8QgmaTqSWO3h2` — the panel power-off research (rails, blank levels, what
  userspace can and cannot reach).
