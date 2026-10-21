# Panel power: what the stock firmware does, and what the port does not

This document corrects `docs/DISPLAY.md` ("The freeze (fixed)" and "Power: what 'off' can
and cannot mean") and `docs/V3-ANALOG.md` ("Power: the backlight goes out, then the SoC
sleeps"). Those sections assert that this panel can only be powered down by
suspend-to-RAM, and that the stock player never does anything to it but dim the backlight.
That is not what the stock firmware does. The stock player powers the panel down — rails,
reset, pinmux, touch — **on every screen-off, while the SoC stays awake**, through a
proprietary ioctl on `/dev/fb0` that the port has never issued.

All evidence below is static: the stock player `firmware/tt/SYSTEM/usr/bin/hiby_player`
(md5 `3a5db0fa7d2536ae564841f24b5f8b54`, ELF vaddr, `file offset = vaddr - 0x400000`) and
the stock kernel `firmware/tt/kernel.bin` (md5 `aaa60f4fc2b4d592f92bc5a405e10695`,
base `0x80010000`, `file offset = virt - 0x80010000`). Disassembly:
`toolchain/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-objdump -D -b binary -m
mips:isa32 -EL`, or `-D` on the ELF. No device was touched.

## Verdict

- The stock screen-off is **two writes**: `/sys/class/backlight/pwm-backlight.0/brightness`
  = 0, then `ioctl(fd_fb0, 0x80044620, &one)`. The ioctl is the player's "SET_SLEEP"
  (`_IOW('F', 0x20, int)` on MIPS) and it runs the kernel's full panel power-down:
  wait for the SLCDC transfer, `LCDC_CTRL.ENA = 0`, gate the LCD clocks, disable the AXP2101
  rails `LDO7`/`LDO8`, drive `gpio16` (lcd rst) and `gpio48` (lcd rd) low, pinmux the SLCD
  pins away, put the touch controller to doze.
- The stock screen-on is the same ioctl with 0: clocks on, pinmux back, rails on, `lcd rd`
  and then `lcd rst` high (reset released last), the 343-entry panel table, `SLCDC_CTRL.DMA_EN`,
  `LCDC_CTRL.ENA`, then `FBIOPAN_DISPLAY`, then a **100 ms** wait and the backlight back up.
- `FBIOBLANK` is not involved anywhere. The stock player does not use it (there is no
  `0x4611` in the binary). The freeze class that `DISPLAY.md` describes is real for
  `FBIOBLANK`, but it is not the stock path and it is not the only way to switch the panel off.
- Suspend-to-RAM is a **second, later** event (`echo mem > /sys/power/state`, its own thread),
  not the thing that turns the screen off. It reaches the same `lcd_power_off` through
  `jzfb_suspend_pm`, which is why the PM path looked like the only one.

## 1. What the stock player does on screen off

### The call sites

The player has one screen-off routine, `0x473320`, and all four call sites in the binary go
through it: a key handler at `0x441940` (gated at `0x441900` by an internal key id 74), the
input dispatcher at `0x4e8b98`, a helper at `0x4733f0`, and the early-suspend hook at
`0x475070`. Its body (`0x473320`..`0x4733a4`, file `0x73320`):

```
473320: lw   v0, 4(s0)             ; s0 = &state (0xbad0fc); already off -> return
473334: beq  v0, s1, 47336c
473344: lw   v0, 0xab5850          ; "discard" flag -> log "Discard backlight off message"
473354: addiu a0, a0, 0x6878       ; 0x946878 = "/sys/class/backlight/pwm-backlight.0/brightness"
473358: jal  0x713d00              ; write "0" (a1 = 0)
473360: jal  0x471020              ; -> 0x47db60  fb_suspend()
473368: sw   s1, 4(s0)             ; state = 1 (off)
```

The wake routine `0x4732a0` mirrors it:

```
4732cc: jal  0x471040              ; -> 0x47da80  fb_resume()
4732d4: jal  0x471560 (a0=100)     ; usleep(100 * 1000)   (0x471560 multiplies by 1000)
4732dc: lw   a0, 0xbad120          ; the brightness saved before the screen went off
4732e0: jal  0x472dc0              ; write it back to pwm-backlight.0/brightness
4732e8: sw   zero, 4(s0)           ; state = 0 (on)
```

The 100 ms gap is the same ordering the V1 port needed: panel init first, backlight after.

### The ioctl: SET_SLEEP, not FBIOBLANK

`0x47db60` is the player's fb suspend (file `0x7db60`):

```
47db60: lw   a0, 0xab595c          ; fd of /dev/fb0
47db78: lw   v1, 0xbad6a0          ; "suspended" flag
47db80: bnez v1, 47dbd8            ; already suspended -> "fb has already suspended"
47db84: lui  a1, 0x8004
47db88: li   v1, 1
47db8c: addiu a1, a1, 0x4620       ; a1 = 0x80044620
47db90: addiu a2, v0, -10592       ; a2 = &flag  (0xbad6a0)
47db94: jal  ioctl@plt
47db98: sw   v1, -10592(v0)        ; delay slot: flag = 1
```

`0x47da80` is the resume (file `0x7da80`), and it shows the argument convention:

```
47daa8: lui  a1, 0x8004
47dab0: addiu a1, a1, 0x4620       ; 0x80044620
47dab4: addiu a2, v0, -10592       ; &flag
47dab8: jal  ioctl@plt
47dabc: sw   zero, -10592(v0)      ; flag = 0 -> the ioctl argument is 0
47dac4: ...                        ; then FBIOPAN_DISPLAY (a1 = 0x4606, 0x47dad0)
```

A full scan of the binary's ioctl constants confirms the picture (only these are present):

| constant | decode | where |
|---|---|---|
| `0x4600` | `FBIOGET_VSCREENINFO` | `0x47d714` (fb init) |
| `0x4602` | `FBIOGET_FSCREENINFO` | `0x47d6fc` (fb init) |
| `0x4606` | **`FBIOPAN_DISPLAY`** | `0x47dad0`, only inside `fb_resume` |
| `0x80044620` | **`SET_SLEEP`** — `_IOW('F',0x20,int)` | `0x47dab8` (resume, 0), `0x47db94` (suspend, 1) |
| `0x80044660` | `GET_EMPTY_BUF` (on `/dev/sa_hgl_dma`) | per-frame submit |
| `0x80044661` | `START_DMA` (on `/dev/sa_hgl_dma`) | per-frame submit |
| `0x80044662` | `JZFB_ROTATE` | `0x47d9f4` |

There is no `FBIOBLANK` (`0x4611`) and no `FBIOPUT_VSCREENINFO` (`0x4601`). So the sentence
"its binary has no FBIOBLANK, no FBIOPAN_DISPLAY and no FBIOPUT_VSCREENINFO in it" is
**two-thirds true and misleading**: it has no `FBIOBLANK`, but it does have
`FBIOPAN_DISPLAY` (`0x4606`), and it powers the panel down through a vendor ioctl that no
standard-fb search would find. The strings are in the binary as well: `(hgl_fb: W) failed to
access ioctl SET_SLEEP` (`0x949514`, file `0x549514`) and `... FBIOPAN_DISPLAY` (`0x949544`).

Frames are not pushed with `FBIOPAN_DISPLAY` in the stock player: it opens `/dev/fb0` plus
`/dev/sa_hgl_dma` (`0x47d680`: open, `FBIOGET_FSCREENINFO`, `FBIOGET_VSCREENINFO`, `mmap`;
`0x94943c` = `/dev/sa_hgl_dma`, mmap 6 MB at `0x47d7e8`) and submits each frame through the
`GET_EMPTY_BUF` / `START_DMA` pair. The single `FBIOPAN_DISPLAY` is the one in `fb_resume`,
i.e. the first frame after the panel comes back.

### The separate suspend

`0x474fa0` is the player's early-suspend hook: it logs `PM: early suspend`, turns the screen
off (calls `0x473320`, so `fb_suspend` runs first), and runs `/usr/bin/bt_suspend`
(`0x946e4c`). The worker `0x475100` then logs `PM: sleep` and runs
`system("echo mem > /sys/power/state")` (`0x475160`, string `0x946e7c`). That is the
standby, and it is **after** the screen is already dark and the panel already unpowered.
It is not what makes the power button kill the panel.

## 2. What the stock kernel does with SET_SLEEP

`jzfb_ioctl` (`0x80207110`) dispatches `0x80044620` at `0x802073e4`. It copies the 4-byte
argument and branches:

```
80207418: bnez v0, 0x80207444        ; arg != 0 -> suspend
8020741c..8020743c                    ; arg == 0 -> if (flag420) jzfb_resume(), flag420 = 0
80207444..80207490                    ; arg != 0 -> if (!flag420) jzfb_suspend(), flag420 = 1
```

So **arg 1 = panel off, arg 0 = panel on**, and the `flag420` (`ingenicfb+0x1a4`) is the
"is suspended" latch shared with the PM path.

### jzfb_suspend (`0x8020482c`, file `0x1f482c`)

1. Poll `SLCDC_CTRL` (`base+0xa4`) bit 1 (`DMA_START`) until it clears, `__udelay(1000)`,
   at most 500 rounds (`0x802048a0`..`0x802048c4`) — up to ~500 ms for the frame in flight.
2. `jzfb_disable()` (`0x80203f20`): `LCDC_CTRL &= ~0x8` (ENA), `is_lcd_en = 0`. It does
   **not** touch `DMA_EN`.
3. `clk_disable()` on `ingenicfb+408` and `+412` (`0x802048d4`, `0x802048f8`).
4. Tail call to `pdata+0x6c` (`0x80204918`) = **`lcd_power_off`**.

### lcd_power_off (`0x8001a038`, file `0x1a038`)

Guarded by a global (`0x80702338`) so a second call is a no-op. In order:

1. `regulator_get(NULL,"LDO7")` → `regulator_disable` → `regulator_put`.
2. Same for `"LDO8"` (the strings are at file `0x5c4660`/`0x5c4668`; the error strings call
   them `lcd_3v3` and `lcd_1v8`, file `0x5c6150`/`0x5c6170`).
3. `gpio_direction_output(16, 0)` — `lcd rst` low.
4. `gpio_direction_output(48, 0)` — `lcd rd` low.
5. The SLCD pin group is switched to **function 4** (`jzgpio_set_func(pin, 4, ...)`, the loop
   comparing names against `"slcd"`, file `0x5c5f34`).
6. `goodix_ts_close()` — the GT911 is put to doze (`gtp_enter_doze`).

Rails first, reset second, pinmux third, touch last. That is the vendor's order, odd as it
looks.

### jzfb_resume (`0x80206614`, file `0x1f6614`)

1. `clk_enable()` on `+408` and `+412`.
2. If `is_lcd_en == 0` (`0x80206684`):
   - `pdata+0x68` = **`lcd_power_on`** (`0x80206690`),
   - `jzfb_set_par` (`0x80206698`) — sends the **343-entry** table and writes
     `SLCDC_CTRL` with `DMA_EN` (`0x80206598`),
   - `jzfb_enable` (`0x802066a0`) — waits `SLCDC_STATE` bit 0 (BUSY) clear, `LCDC_STATE=0`,
     `LCDC_CTRL |= 0x8`.
3. `init_vsync` / TE re-arm.

### lcd_power_on (`0x8001a1b0`, file `0x1a1b0`)

1. Pinmux the SLCD pins to **function 1**.
2. `regulator_get(NULL,"LDO7")` → `set_voltage(3000000,3000000)` → `enable` → `put`.
3. Same for `"LDO8"` at `1800000`.
4. `gpio_direction_output(48, 1)` (`lcd rd` high).
5. `gpio_direction_output(16, 1)` (`lcd rst` high — **reset released last**).
6. `goodix_ts_open()`.

No explicit delay between the rails and the reset in this function; the panel's own table
carries the `udelay` entries and the player waits 100 ms after resume before lighting the
backlight. `power_io_init` (`0x80019e20`) is what makes `gpio16`/`gpio48` `EBUSY` for
export: `gpio_request(16,"lcd rst")`, `gpio_request(48,"lcd rd")`.

### What about FBIOBLANK?

`jzfb_blank` (`0x80203f94`) is exactly what `DISPLAY.md` says it is, and it stays that way:

- `UNBLANK`: `LCDC_STATE = 0`, `LCDC_CTRL = (ctrl & ~0x10) | 0x8`, `is_lcd_en = 1`.
- any blank: `LCDC_CTRL &= ~0x8`, `SLCDC_CTRL &= ~0x1` (`DMA_EN`), `is_lcd_en = 0`.

No pdata call, no rails, no clocks. That is the freeze class: after POWERDOWN → UNBLANK,
`ENA=1` and `DMA_EN=0`; `jzfb_pan_display` then writes `DMA_START|DMA_MODE` into
`SLCDC_CTRL` (`0x80205974`) but the controller does not run, and the pan reports success.
The measurement in `DISPLAY.md` is right; the conclusion drawn from it was not.

`FBIOBLANK` is also not the stock path: the player never issues it, and the kernel's
`fb_blank` (`0x801fd83c`) additionally runs the fb notifier chain before and after the
driver hook. There is no `lcd_device` registered on this board (`lcd_device_register`
`0x80201718` has no caller in the image), so the notifier does not power anything either.

### The rails, in board terms

The panel rails are the AXP2101's vendor-named `LDO7` (ALDO4, enable bit `0x90` bit3,
voltage `0x95`) and `LDO8` (BLDO1, `0x90` bit4, `0x96`), 3.0 V and 1.8 V; `gpio16` is
`GPIO_PA(16)` `lcd rst`, `gpio48` is `GPIO_PB(16)` `lcd rd`; `gpio89` (`GPIO_PC(25)`) is
`BL PWR` and belongs to the backlight, not to the panel (`backlight_init`, `0x80019dc8`).
The register map and pin names are in `patch/v3-kernel/V3-BOARD.md`.

## 3. Does the stock stop refreshing while awake?

Yes, at the only level that matters: after `SET_SLEEP(1)` there is no panel to refresh.
`is_lcd_en = 0`, `ENA = 0`, the LCD clocks are gated, and the panel's two rails are off.
`jzfb_pan_display` refuses while `is_lcd_en == 0`:

```
802058f8: lw   v1, 0(jzfb)
802058fc: bnezl v1, 80205920       ; enabled -> the SLCD branch
80205910: jal  printk              ; "pan_display: lcd is disable?"  (file 0x5f3ae4)
8020591c: li   v0, -22             ; -EINVAL
```

So even if the player kept presenting, the driver would refuse every pan. Nothing can reach
the glass until `SET_SLEEP(0)` re-runs the whole init.

What the player's own render timer does while the screen is off is **not** proven by this
disassembly. The fb layer's "suspended" flag (`0xbad6a0`) is read only by `fb_suspend` and
`fb_resume`; the per-frame `START_DMA` path is not gated on it, and the screen-state global
`0xbad100` is read only by `0x473420` (the on/off test) and the early-suspend hook. The
player very likely keeps its UI thread alive (music keeps playing) but with the display
controller off; whether it still issues `START_DMA` calls is an open question, and it does
not change the outcome — the panel is unpowered. Cheapest confirmation on hardware:
`strace -f -p $(pidof hiby_player) -e trace=ioctl` on stock with the screen off, and look
for `0x80044660`/`0x80044661` continuing.

## 4. The exact known-good sequence stock uses

This is what a patch would have to reproduce. Everything below is the stock code path.

**Screen off (SoC awake, music can continue):**

1. `write("/sys/class/backlight/pwm-backlight.0/brightness", "0")`.
2. `ioctl(fb0, 0x80044620, &1)`:
   1. wait up to ~500 ms for `SLCDC_CTRL.DMA_START` (bit 1) to clear;
   2. `LCDC_CTRL &= ~0x8` (`ENA = 0`), `is_lcd_en = 0`;
   3. `clk_disable` on the two LCD clocks;
   4. `LDO7` off, `LDO8` off (AXP2101, 3.0 V / 1.8 V rails);
   5. `gpio16 = 0` (reset low), `gpio48 = 0`;
   6. SLCD pins → function 4;
   7. `goodix_ts_close()` (GT911 to doze; 58 ms `msleep` when it actually dozes).

**Screen on:**

1. `ioctl(fb0, 0x80044620, &0)`:
   1. `clk_enable` both LCD clocks;
   2. SLCD pins → function 1;
   3. `LDO7` = 3.0 V on, `LDO8` = 1.8 V on;
   4. `gpio48 = 1`, then `gpio16 = 1` (reset last);
   5. `goodix_ts_open()`;
   6. `jzfb_set_par`: send the 343-entry `{cmd,data,udelay}` table, wait `SLCDC_STATE.BUSY`
      clear, write `SLCDC_CTRL` with `DMA_EN`;
   7. `jzfb_enable`: wait BUSY clear, `LCDC_STATE = 0`, `LCDC_CTRL |= 0x8`.
2. `ioctl(fb0, FBIOPAN_DISPLAY, &var)` — the first frame.
3. `usleep(100 ms)`.
4. Restore the saved backlight level.

**Later, if the standby timer fires:** the early-suspend hook (screen already off) →
`/usr/bin/bt_suspend`; the PM worker → `system("echo mem > /sys/power/state")` →
`jzfb_suspend_pm` → the same `jzfb_suspend` (`flag420 == 0`).

Note the ordering that makes this safe: the power-down always goes through
`jzfb_disable` + clock gate + `lcd_power_off`, and the power-up always goes through
`lcd_power_on` + `set_par` + `enable`. `set_par` re-arms `DMA_EN`, so the
"unblank restored only ENA" state never occurs on this path.

## 5. Stock vs the port

| step | stock | port (`src/sonix-blaze/sonix-player`) |
|---|---|---|
| screen-off trigger | power key / UI handler → `0x473320` | `power_screen_off()` (`power.c:1408`) |
| backlight | write 0 to `pwm-backlight.0/brightness` (`0x473354`) | same (`power.c:1436`, `write_long_to_file(...,0)`) |
| panel power | `ioctl(fb0, 0x80044620, 1)` → `jzfb_suspend` → rails + reset + pinmux + touch | **nothing**: `blank_path = NULL` on this board (`board.h:141`, `main.c:3941`), and `screen_power()` returns early (`power.c:225`) |
| LCDC/SLCDC | `ENA = 0`, clocks gated; `DMA_EN` left as it was | `ENA = 1`, `DMA_EN = 1`, clocks running |
| render while dark | driver refuses pans (`-EINVAL`), panel unpowered | LVGL keeps drawing; `fb_pan_show()` still pans (only the *arm* is gated by `power_screen_is_on()`, `main.c:1949`) |
| wake | `ioctl(fb0, 0x80044620, 0)` → rails + table + `DMA_EN` + `ENA`, pan, 100 ms, backlight | backlight ramp only; no panel init (it never went off) |
| touch on screen-off | GT911 doze (`goodix_ts_close`) | LVGL indevs disabled only (`set_indevs_enabled(false)`) |
| later standby | `echo mem` thread, after screen off | `echo mem` after 60 s (`power.c:1172`) — the same PM path |
| freeze risk | none: resume re-runs `set_par` | none today only because the panel is never disarmed |

What the port already has, and can reuse:

- `fb_arm()` (`main.c:1895`): `FBIOBLANK(UNBLANK)` + `FBIOPUT_VSCREENINFO(FORCE)` — the
  `PUT` is what runs `jzfb_set_par` (table + `DMA_EN`). This is the **manual** equivalent of
  the tail of `jzfb_resume`.
- `fb_pan_show()` (`main.c:1925`): pan with mode re-read and a re-arm retry; already handles
  a live interface with a dead panel.
- `echo mem` standby (`power.c:1172`): the PM path that runs `jzfb_suspend`/`jzfb_resume`;
  verified on the device.
- `display_panel_disarmed()` / `fb_armed`: the port's own model of the panel state, which
  would still be needed for the `FBIOBLANK` path.

What is missing for the stock behaviour:

1. **The `SET_SLEEP` ioctl itself.** Nothing in the port calls `0x80044620`. This is the
   only missing call for a first-order reproduction: no kernel change, no rebuild.
2. **The wake ordering**: `SET_SLEEP(0)` → pan → ~100 ms → backlight. The port has no
   panel-on step to wait for, so its wake is just the backlight ramp.
3. **A decision about rendering while the panel is off**: with `SET_SLEEP(1)` the driver
   starts refusing pans with `-EINVAL`; `fb_pan_show()` already stays quiet when
   `power_screen_is_on()` is false, so it would not spam, but the flip thread would keep
   producing frames that go nowhere. Stock's panel is simply gone; the port should probably
   stop the flip/pan loop (or keep it, accepting refused pans) when it adopts this.
4. **The touch contract**: `lcd_power_off` puts the GT911 to doze and `lcd_power_on` wakes
   it. The port currently guards this by disabling LVGL input devices, which is not the same
   thing; with `SET_SLEEP` the touch driver is driven by the kernel, and the port's own
   `set_indevs_enabled()` remains useful only for its tap-through protection.

A subtlety for whoever implements it: `SET_SLEEP` is a **vendor ABI**, not a standard fb
ioctl. Its number is `_IOW('F', 0x20, int)` = `0x80044620` on MIPS, and the kernel tests the
raw value `0x80044620` (`lui 0x8004; addiu 0x4620`, `0x80207154`, dispatching at
`0x802073e4`). The argument is a pointer
to an `int`: 0 = on, non-zero = off. Keep the `/dev/fb0` fd open across the off period, as
stock does.

## 6. How the myth arose

The earlier research (session `ses_ec6820354ffeMOuM0X72my4TUI`, 2026-10-14) searched the
stock player for the standard fb ioctls and concluded:

> "он **вообще не трогает `fb0/blank`** — ни `FBIOBLANK` (нет ни одного `0x4611`), ни
> `FBIOPAN_DISPLAY` (`0x460A`), ни `FBIOPUT_VSCREENINFO` (`0x4601`). Только
> `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` + mmap, `JZFB_SET_VSYNCINT` ..."

Three things went wrong there:

1. **The constant for `FBIOPAN_DISPLAY` was wrong** (`0x460A`; it is `0x4606`), so the one
   pan call in `fb_resume` was missed, and with it the `SET_SLEEP` call two instructions
   earlier. `JZFB_SET_VSYNCINT` is not in this binary either; what is there is
   `JZFB_ROTATE`, `SET_SLEEP`, and the HGL DMA pair.
2. **The absence of `FBIOBLANK` was read as "the stock never powers the panel down".** The
   correct reading is "the stock does not use the buggy blank path; it uses a vendor path."
   The vendor path is invisible to a search for `FB_*` constants because the command is
   spelled `_IOW('F', 0x20, int)` and only its *string* says `SET_SLEEP`.
3. **The call graph inside the driver was read as a reachability statement.**
   `lcd_power_off` really is called only from `jzfb_suspend`, and `jzfb_suspend` really is
   the PM suspend hook — but it is **also** the handler of the `SET_SLEEP` ioctl
   (`0x80207458`), which any process holding `/dev/fb0` can call while the SoC is awake.
   "Only `jzfb_suspend` calls it" does not imply "only suspend-to-RAM reaches it".

The freeze experiment itself was sound: `FBIOBLANK(POWERDOWN)` then `UNBLANK` really does
leave `ENA=1`/`DMA_EN=0`, and a pan really does report success without a transfer. That is a
genuine bug in this vendor `jzfb_blank`, and it is still true. The mistake was the next step:
because the port had chosen `FBIOBLANK` as its screen-off mechanism, and `FBIOBLANK` is
broken, the docs concluded the panel could not be switched off at all and rebuilt the
product story around "screen-off is the backlight". The kernel offers two ways down; the
port picked the broken one, hit the bug, and then declared the working one nonexistent.

The failed kernel patch (`patch/v3-panel-blank/README.md`) reinforced this. It tried to make
`jzfb_blank` do the power cycle and Oopsed (`epc == 0`/`ra == 0` for both directions, under
`console_lock`), so the conclusion became "the blank path is unsafe on this kernel, therefore
we must never blank". That is true about `fb_blank`, but it says nothing about `SET_SLEEP`,
which is a plain unlocked ioctl handler. A first call in `jzfb_blank` also ran
`lcd_power_off` without the `jzfb_suspend` prelude (wait for DMA, disable the controller,
gate the clocks), and `lcd_power_off`'s `goodix_ts_close()` talks i2c to the touch after the
rails are gone; the stock path does the same thing but from a quiesced controller. The
exact cause of that `__delay` hang is still not proven; it is not needed for the
`SET_SLEEP` route, which is the path the stock firmware actually uses.

## 7. Confidence and the cheapest hardware confirmation

Proven from the binaries: the player's `SET_SLEEP` calls and their arguments; the kernel's
dispatch, `jzfb_suspend`/`jzfb_resume`, the rails/GPIO/pinmux order, and the fact that
`jzfb_blank` does none of it; the absence of `FBIOBLANK` and of an lcd-class device; the
`-EINVAL` pan refusal while disabled.

Inferred, not proven:

- that the power **button** (rather than only the UI/timeout path) calls `0x473320`. The
  routine is reached from the key/UI handlers, and the owner has seen the panel die on a
  power press; the evdev-code → handler mapping was not traced. Cheapest check: on stock,
  `strace -f -p $(pidof hiby_player) -e trace=ioctl,write` and press power; expect
  `ioctl(..., 0x80044620, [1])` and a write to `pwm-backlight.0/brightness`.
- that the player's UI thread keeps or stops submitting `START_DMA` while the screen is off
  (see §3). Same strace, longer window.
- that the TE/vsync interrupt stops when the panel is unpowered. Cheapest check:
  `/proc/interrupts` for `slcd_vsync`, sampled before and after a power press on stock.
- the exact cause of the `0x80203f94` patch's Oops. Not needed for the `SET_SLEEP` route.

Hardware checks that would settle the remaining questions, in one pass on stock:
`cat /sys/class/regulator/regulator.*/{name,state}` before and after the power press
(`LDO7`/`LDO8` should go `disabled`), `devmem 0x13050030` (`ENA` bit 3 → 0), and
`devmem 0x130500a4` (note that `DMA_EN` may stay 1 — `jzfb_disable` does not clear it; the
power is what is off). The same three on the port should show the rails still enabled and
`ENA` still 1.

## 8. First step on hardware: `tools/sleepfb`

The cheapest way to confirm the path on this firmware before any player change is
`tools/sleepfb` (`src/sonix-blaze/tools/sleepfb.c`). It calls exactly the stock ioctl on
`/dev/fb0`, prints `LCDC_CTRL.ENA`, `SLCDC_CTRL.DMA_EN/DMA_START`, `SLCDC_STATE.BUSY`, the
`lcd_3v3`/`lcd_1v8` rails and the backlight level around the call, and leaves the backlight
alone.

```
mipsel-rockbox-linux-gnu-gcc -static -O2 -o sleepfb sleepfb.c
adb push sleepfb /mnt/sd_0/
adb shell '/mnt/sd_0/sleepfb status'
adb shell '/mnt/sd_0/sleepfb cycle 5'
```

What the port should show once it is wired the stock way: `off` — ENA 0, both rails
disabled, ioctl returning after the DMA wait; `on` — ENA 1, rails back, a frame on the
glass. A frozen panel after `on` (registers healthy, glass stale) would mean the resume
sequence is not the stock one; that is the failure `DISPLAY.md` warns about, and the tool
prints enough state to tell it apart from a backlight-only "off".

## 9. On the device: what was measured, and what the port now does

Measured on the V3 Analog (stock kernel `35889275…`, player build `211020260040`):

- `SET_SLEEP(1)` on a live interface returns in **~65–68 ms**; `LDO7` and `LDO8` both go
  `enabled` → `disabled` and stay there (checked again five seconds later). `SET_SLEEP(0)`
  returns in **~320 ms**, the rails come back `enabled`. Register reads while the clocks are
  gated are garbage (`0x4000200d`), which is why the rails and the glass, not `ENABLE`, are
  the things to look at while it is down.
- After `on` the glass is not automatically right: an early test showed **garbage** (bare
  panel RAM) and then, once a pan had been forced, the **last frame, static**. The frame
  only came back when something else pushed a full one. So the resume needs the pan the
  port already has (`display_wake_begin` → `fb_arm` → `FBIOPAN_DISPLAY`) plus the full
  repaint of the wake; the ioctl alone is not enough.
- One confound worth remembering: **opening `/dev/fb0` re-runs the driver's panel init**
  (`jzfb_open` → `slcd_cfg_new`). A test tool that opens the node between `off` and `on`
  silently re-arms the panel and hides the failure; `sleepfb cycle` keeps one descriptor
  for that reason.

The port implements the stock path in `power.c` / `sysinfo.h` / `board.h`:

- `panel_sleep_ioctl = 0x80044620` in the V3 board profile (field of `sysinfo_model_t`,
  copied into `power_config_t`); `blank_path` stays NULL, so `FBIOBLANK` is never used.
- `screen_power()` issues the ioctl on the framebuffer `main.c` already holds (`cfg->fb_fd`;
  a second `open()` would re-init the panel). Off happens after the backlight reaches zero,
  stock order; on is followed by the configured settle (100 ms by default on this path),
  then the wake repaint.
- The fast-wake window is disabled on this path: the stock firmware takes the panel down at
  once, and holding the rails up to save a wake would be a different behaviour.
- `display_panel_disarmed()` now also clears the flip thread's panel-grid sample, the
  outstanding pan, the transfer start time and the one-shot refusal flag; carried across a
  sleep they would pace the next frames against a transfer that is never coming.

Verified on the device with the camera on the glass and the rails read over adb: power
button off → backlight 0, `LDO7`/`LDO8` disabled; power button on → rails enabled,
backlight at the configured level, the interface live (a volume overlay appeared on the
glass). A track playing through the cycle kept playing.

## Appendix: addresses used

| what | virt | file |
|---|---|---|
| player screen-off / screen-on | `0x473320` / `0x4732a0` | `0x73320` / `0x732a0` |
| player fb_suspend / fb_resume | `0x47db60` / `0x47da80` | `0x7db60` / `0x7da80` |
| player early suspend / PM worker | `0x474fa0` / `0x475100` | `0x74fa0` / `0x75100` |
| player fb init (`/dev/fb0`, `/dev/sa_hgl_dma`) | `0x47d680` | `0x7d680` |
| `jzfb_ioctl` | `0x80207110` | `0x1f7110` |
| `jzfb_suspend` / `_pm` | `0x8020482c` / `0x80204958` | `0x1f482c` / `0x1f4958` |
| `jzfb_resume` / `_pm` | `0x80206614` / `0x80206748` | `0x1f6614` / `0x1f6748` |
| `jzfb_disable` / `enable` / `blank` | `0x80203f20` / `0x80204794` / `0x80203f94` | `0x1f3f20` / `0x1f4794` / `0x1f3f94` |
| `jzfb_set_par` | `0x80205ae8` | `0x1f5ae8` |
| `jzfb_pan_display` | `0x802058f4` | `0x1f58f4` |
| `lcd_power_on` / `off` | `0x8001a1b0` / `0x8001a038` | `0x1a1b0` / `0x1a038` |
| `power_io_init` / `backlight_init` | `0x80019e20` / `0x80019dc8` | `0x19e20` / `0x19dc8` |
| `fb_blank` (core) | `0x801fd83c` | `0x1ed83c` |
| `lcd_device_register` (no callers) | `0x80201718` | `0x201718` |

Strings: `/sys/class/backlight/pwm-backlight.0/brightness` `0x946878`; `echo mem >
/sys/power/state` `0x946e7c`; `(hgl_fb: W) failed to access ioctl SET_SLEEP` `0x949514`;
`... FBIOPAN_DISPLAY` `0x949544`; `/dev/sa_hgl_dma` `0x94943c`.
