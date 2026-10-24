# Power: what costs what, and what the port got wrong

This document is the audit behind the power work of 2026-10-24. It records what the player
does that costs battery, what was measured on the device, and what changed. The panel's own
power sequence is in [PANEL-POWER.md](PANEL-POWER.md); this file is about the *system* around
it -- the SoC, the rails, the wake-ups and the sleep.

## The board has no DVFS

`/sys/devices/system/cpu/cpu0/` has **no `cpufreq` directory** at all: this kernel was built
without the Ingenic frequency-scaling driver, so there is no governor, no OPP table and no
`scaling_setspeed` to write. The X1000 runs at one clock whatever it is doing.

That settles what "saving power" can mean here: not a lower clock, only **less work** and
**more sleep**. Everything below is one of those two.

## The three states the device can be in

| state | SoC | panel | backlight | rails LDO7/LDO8 | who puts it there |
|---|---|---|---|---|---|
| screen on | awake | armed, scanning | written level | enabled | the player |
| screen off, awake | awake | `SET_SLEEP(1)` | 0 | disabled | `power_screen_off()` |
| standby (`mem`) | suspended | powered down by `jzfb_suspend_pm` | 0 | disabled | `suspend_to_ram()` |

The middle one is the one the product lives in most: music playing, or the first minute after
the screen goes dark. The third is entered 60 s later, and is the only state in which the SoC
itself costs nothing.

## What was wrong

Measured on the device on 2026-10-24 (build `221020260040`) unless a line says otherwise.
`tools/powsample` (`tools/powsample.c`) is what took the readings: one line every 20 s with the
battery current, the voltage, the level, the backlight, **both panel rails** and the
`slcd_vsync` interrupt count. The rails and the interrupt are what make the three states
distinguishable from outside -- the backlight alone reads 0 in all three.

### 1. The panel was left powered after a wake from standby

`SET_SLEEP(1)` leaves the driver's `is_lcd_en = 0` and its "suspended" latch set. The kernel's
own `jzfb_resume_pm` therefore runs `lcd_power_on` + `set_par` + `enable` on the way out of
`mem` -- the panel comes back **powered and scanning**, with the backlight at whatever the
backlight class last held (0). The player, meanwhile, comes back to a screen that is still
meant to be dark, and issued no `SET_SLEEP(1)` again: the only call sites were the screen-off
transition, which had already happened before the suspend.

Seen live: `LDO7` and `LDO8` `enabled` with `backlight 0` and `slcd_vsync` ticking, minutes
after the device had resumed from `mem` because a cable was plugged in. Fixed in
`suspend_to_ram()`: after a real suspend, if the screen is still off, the panel is taken down
again before anything else.

### 2. `FBIOBLANK` at start-up and twice per wake

`power_init()` gated its panel repair on `panel_blanking()`, which is true on this board
because the model carries `panel_sleep_ioctl`. So every boot ran
`display_wake_begin()` → `fb_arm()` → **`FBIOBLANK(UNBLANK)` + `FBIOPUT_VSCREENINFO(FORCE)`**,
the pair `docs/DISPLAY.md` and `AGENTS.md` forbid on this panel, to repair a state a reboot
cannot leave behind: the kernel's own `jzfb_open` re-runs the panel init.

The wake path had the same shape: `screen_power(true)` = `SET_SLEEP(0)` already runs
`lcd_power_on`, the panel's 343-entry table and `DMA_EN` in the kernel, and then
`display_wake_begin()` ran the blank-and-reinit pair on top of it -- a second panel init, about
45 ms, on a panel whose init had just finished.

Fixed: the boot repair is gated on `!panel_sleep_ioctl`, and the wake on that path uses
`display_wake_repan()` (a pan, nothing else).

### 3. A frame drawn, copied and panned after the panel was already dead

`power_screen_off()` powered the panel down first and then drew the wake-hook frame into it:
a full-screen draw, a 614 kB copy, two `FBIOPAN_DISPLAY` calls the driver refuses
(`-EINVAL`, "lcd is disable?") and up to 14.7 ms of `SLCDC_STATE` polling against gated clocks
-- once per screen-off, for a frame that only has to be in RAM.

Fixed: the wake-hook frame is drawn before `screen_power(false)`, and the flip thread skips the
pan and the transfer poll entirely while the screen is off (`continue` after the copy).

### 4. A pan racing the panel's power-down

`screen_power(false)` was called without waiting for the flip thread. The vendor driver takes
no lock around `jzfb_pan_display` and `jzfb_suspend`, and this is the kernel that has already
Oopsed once in that area (`patch/v3-panel-blank/`). Fixed: `display_flip_wait()` before the
ioctl.

### 5. The charge limit did not work, and the cable rule rested on it

`suspend_if_idle()` refuses to suspend whenever VBUS is present, because the charge limit is
enforced from the timer tick and nothing runs during `mem`. That rule is right -- but the
limit it protects had no way to act: the port looked for the MP2731's
`step_charging_enabled`, which this firmware does not have, so `power_charge_limit_supported()`
was false and a player on a charger charged straight past the limit the user had set.

That is fixed, and not through the MP2731: the AXP2101's own constant charge current
(`usb/constant_charge_current_max`, REG62h) is written to 0 to hold the battery and back to
its previous value to let it go. Verified at register level through the driver's raw node
`/sys/class/axp/axp_reg` (700 mA = `REG[0x62]=0x0d`, 0 = `0x00`, restore = `0x0d`, and the
input limit `REG[0x16]` does not move). The PMIC's other switch,
`charge_control_limit_max = 100`, is deliberately not used: in the same write it drops the
input current limit to 100 mA, which starves the system as well as the battery.

So the cable stays a reason of its own, and the tick that watches the level always runs. What
the cable is carrying (the card over USB, DAC mode, ADB) is checked as well and says so in the
log.

One casualty of the investigation is a wrong assumption worth recording: `axp_battery`'s
`current_now` is the constant `BAT_CURRENT_NOW` (200000) from the vendor driver, not a
measurement -- see "What can and cannot be measured here".

### 6. Six battery-check wakes a night

The RTC alarm looks in on the battery every 4 h above 15 % (20 min under 5 %), and each wake
brought 40 s of awake time and, per item 1, a powered panel. Above 15 % the level moves by about
1 % per 10-20 min of standby, so twelve hours answers the same question. Changed.

## The idle wake-ups, and what was cut

Everything here runs on the SoC while nothing is happening. The counts are nominal rates from
the code; the measurement column is what `powsample` recorded.

| what | before | after | notes |
|---|---|---|---|
| `lv_tick_get()` in the UI loop | ~84 `clock_gettime` per pass, 105 passes/s | 1 per pass | a real syscall here: **0.56 µs** measured on the device, not a vDSO read. Cached per pass, thread-local |
| battery walk (`power_supply/*`) | 250 ms | 1000 ms | each pass is a sysfs walk; the icon shows whole percent |
| `sonixlink` worker, feature off | 300 ms | waits on a condvar, 0 wake-ups | `sonixlink_set_enabled()` signals it |
| `profiler`, trigger absent | `fopen()` on the card every 2 s | blocking `inotify` watch | falls back to 5 s polling where inotify is unavailable |
| RTC battery check, >15 % | 4 h | 12 h | item 6 |
| suspend on a plain charger | never | allowed | item 5 |

Left as they were, deliberately:

- **The touch read timer at 8 ms** (125 Hz with the screen lit). It is the highest idle rate in
  the product, but it only runs with the screen on, where the backlight is the cost, and
  stretching it to 16-26 ms costs input latency on every drag. The pass it triggers is cheaper
  now (first row). Revisit only with a measured input-latency budget.
- **`btplayer` (0.33 Hz off), `sonixlink_bt` (0.5 Hz off), `airpods` (0.25 Hz)** -- all gated on
  a switch and all at sub-hertz rates; the log noise of changing them outweighs the saving.
- **The audio thread's 20 Hz poll while paused with the PCM held** (`audio.c`). It exists to
  notice a jack change before the device is released; it stops when the DAC is.

## What remains, as decisions rather than code

- **The Bluetooth chip is powered 24/7.** `bt_chip_by_firmware = true` on this board means the
  firmware's own `bt_init` attaches the controller over UART once, and the port never
  `rfkill`s it (doing the attach twice leaves the controller with no index). `--enable_lpm
  --tosleep=50000` mitigates it; the cost is believed to be 1-3 mA and is **not measured**.
  Turning it off needs a re-attach path that is known to work, i.e. hardware work.
- **SonixLink keeps both radios up and blocks suspend while it is switched on** -- by design,
  not by accident: a parked radio is one the phone cannot reach, and the whole point of the
  feature is to be reachable. `sonixlink_get_enabled()` is in `wifi_in_use()` and
  `bluetooth_in_use()`, so the radios never park and `mem` is refused for as long as the switch
  is on. Its own auto-off (`gui/wireless/sonixlink.c`, `auto_off_check()`) only cleans up the
  case where the user switched both radios off by hand; it cannot and should not fire on
  idleness. What follows is worth knowing rather than fixing: **with SonixLink on, the player
  never sleeps**, and switching it off releases the radios on the next power tick (screen dark,
  sixty seconds), not instantly.
  (`player_is_busy()` uses `sonixlink_is_connected()` and not the switch, so a SonixLink left
  on but idle does not hold off the automatic shutdown.)
- **AirPlay and DLNA are not persisted** and hold Wi-Fi, and block `mem`, for as long as they
  are on -- the same trade-off as SonixLink, and deliberate: they have to be found to be used.
  Not surviving a reboot is by design, not a defect.
- **A pair of headphones that is connected but silent blocks `mem`.** `bluetooth_in_use()`
  counts `bluetooth_connected_device()`, i.e. the link and not the sound, so headphones left
  connected (nothing playing) keep the device -- and the Wi-Fi radio -- awake. Deliberate:
  dropping the link a user is wearing is worse than the drain, and the comment over
  `wifi_wanted()` spells that out.

## What the device actually draws

Measured 2026-10-24 with an inline USB meter (5 V input) on the owner's unit, with the charge
current written to 0 (`usb/constant_charge_current_max`) wherever the row says "charge held",
so that the meter sees the system and not the charger. States were driven over adb.

| state | meter | input power | note |
|---|---|---|---|
| screen off, panel down, DAC released, no jack | 5.08 V, 0.00-0.05 A | **< 0.25 W** | spikes: the power tick and the battery poll |
| screen on, backlight 123, idle | 5.06 V, 0.09 A | **0.46 W** | backlight + panel + touch polling |
| screen off, panel down, playing to S/PDIF (no jack) | 5.08 V, 0.05 A | **0.25 W** | decoder and DMA only; the codec is not powered |
| screen off, panel down, playing through 4.4 mm | 4.92 V, 0.62 A | **3.05 W** | HBC3000 + AK4493 + the headphone amplifier |
| paused within 5 s of stopping, 4.4 mm | 5.03 V, 0.17 A | 0.86 W | the output is still up; it is released after 5 s |
| charging at 700 mA (no limit) + idle | 4.88-4.95 V, 0.56-0.78 A | 2.8-3.8 W | the only rows with a charge current in them |

Reading the table:

- **the analogue output dominates everything.** Playing through the jacks costs twelve times
  what the same track costs over S/PDIF, and the chain is what the owner recognises -- "this
  player ran warm on the stock firmware while playing too". Not a defect of the port, and
  nothing this work changed;
- the panel and its backlight are about 0.3 W (the screen-on row minus the idle row);
- the idle floor is under 0.25 W, i.e. twenty hours and more from the 5.9 Wh pack with nothing
  to do;
- these are **input-side** numbers: they include the PMIC's conversion loss (order 10-15 %),
  and on battery there is no 5 V stage at all, so run time is at least what the rows imply;
- the meter resolves 0.01 A (~50 mW), so the two smallest rows differ by two counts: the
  ordering is solid, the figures are not precise to 10 %;
- a `mem` figure cannot be taken this way: a cable in the port is a reason to stay awake (the
  charge limit is enforced from the tick), so the suspended state is only reachable on
  battery, where the meter is no longer in the path;
- one reading in this session is **not** in the table on purpose: 5.09 V / 0.00 A taken with the
  4.4 mm plug in and nothing playing was a device that had just **powered off**, not an idle
  one -- see the open question below. Only the no-jack row is a real idle figure.

What the software cannot do is answer the question at all: the AXP2101 driver's
`axp_battery/current_now` is the constant `BAT_CURRENT_NOW` (200000) from
`patch/v3-kernel/vendor-drivers/axp2101.c`, the CW2015 gauge behind
`/sys/class/power_supply/battery` exposes `voltage_now` and `capacity` only, and this firmware
has no `/dev/i2c*` to ask the PMIC directly. Anything in mA has to come from a meter.

The software measurements still worth having are the ones that found the defects below: the
**state** (rails, backlight, `slcd_vsync`, `/proc/stat`, per-thread ticks). A 60 s window with
the screen dark and the panel down reads 2.83 % of the core for the whole SoC, of which the
player is 1.68 % -- 1.42 s on its main thread, 150 ms in the USB watcher, 50 ms in the card
supervisor, 40 ms in the battery poller.

One suspend was captured end to end: the device slept **27 minutes** (`power: mem: awake after
1625 s`), the RTC alarm was armed for **43200 s** (12 h, the changed constant), and the first
sample after the resume showed the panel powered again -- the state item 1 repairs -- because
the resume and the screen wake that followed it were 673 ms apart.

## Open questions from the same session

**Inserting the 4.4 mm plug has powered the device off, twice out of three attempts.** The
device stops dead: `REG[0x20] = 0x01` afterwards says the last switch-on was the power key, so
it was a full power loss rather than a reboot, and it has to be switched on by hand. The
player's log ends **before** any line about the plug -- no `jack changed, rerouting`, no
`set output to` -- which puts the event outside the player and outside the port's code: the
supply is cut faster than a process can write a line. Once (pulling and reinserting the same
plug while a track played) it went through cleanly, so it is not deterministic.

The equipment is the prime suspect: 4.4 mm Pentaconn is a five-pin balanced connector, and a
plug, adapter or cable wired for something else (TRRS, single-ended) can short a balanced
amplifier output to ground, which is exactly a supply cut with no software trace. The
check is a different 4.4 mm cable or the same headphones without an adapter; if the stock
firmware powers off the same way with the same plug, it is not this port at all.

(The first occurrence ran the firmware's shutdown hook, `/usr/bin/bt_done` and an init
`SIGTERM`, which is what a cut supply looks like from inside; nothing in this audit touches a
shutdown path, automatic shutdown is off in the settings, and no power-key event was logged.)

**The charge voltage is 4.40 V against a 4.35 V design maximum.** `REG[0x64] = 0x5` is an
end-of-charge voltage of 4.40 V (`axp_cv_sel`), the gauge reads 4.41 V at 100 %, and the same
driver reports the pack's `voltage_max_design` as 4350 mV. The driver's own default is 4200 mV,
so something in the board's initialisation raised it. Two consequences, neither settled: a pack
charged above its design maximum ages faster, and a pack whose protection trips on
over-voltage cuts the power instantly -- no reboot, a device that has to be switched on again,
which is what the owner saw. The first step is to read the same register on the stock firmware:
if the vendor also charges to 4.40 V, the pack is a 4.4 V type and there is nothing to fix; if
the stock sits at 4.20-4.35 V, the port should match it (the register is writable through
`/sys/class/axp/axp_reg` for a test, and belongs in the port's init once the value is known).

## Measuring it again

```
# build the sampler (static, for the device)
cd tools && ../sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc \
    -static -O2 -march=mips32r2 -mno-madd4 -o powsample powsample.c

adb push powsample /data/powsample
adb shell 'nohup /data/powsample /data/powsample.log 20 >/dev/null 2>&1 &'
```

Then unplug the cable: **adb needs it**, so a discharge measurement is blind until the cable is
back -- the log is what survives. `boot - mono` grows by the length of each `mem`, and the
`slcd_vsync` delta is 0 only while the panel is really powered down.

Verified with it: screen off → backlight 0, both rails `disabled`, `vsync` frozen; screen on →
backlight at the configured level, rails `enabled`, `vsync` counting at the panel's rate.
