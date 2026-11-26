# The MMC driver in the stock kernel: what it does with a card that stalls

Read off the stock `UIMAGE.BIN` (the vendor's 3.10.14, md5 `35889275f8bed2f967a18f6a8b66495c`), with
the SDK tree `kernel-src/ingenic-x1000-3.10.14` as the model of the source: the vendor's
`drivers/mmc/host/jzmmc_v12.c` is a different revision of the same file (its debug line numbers in
`dmesg` do not match the SDK's), so **every claim below was checked against the image**, and the
offsets are given so it can be re-checked.

Symbols come from the image's own kallsyms; `docs/KERNEL.md` has the offsets of the tables. A `jal`
target printed by `objdump -b binary` is a low address: add `0x80000000` to it. File offset =
virt − `0x80010000`.

This is the "the player hangs and the card vanishes" pair seen from the kernel side. The player's
own side of it (write pressure, `sync`, the databases on the card) is in `POWER.md` and in
`.opencode/NOTES.md`; this file is what a kernel patch would have to change.

## The card, as the board declares it

`jzmmc_platform_data` of the card's controller (MMC0), at virtual `0x8064e1e4` (file `0x63e1e4`):

| field | value | file offset |
|---|---|---|
| `removal` | `2` = **REMOVABLE** (`sdio_clk` 0) | `0x63e1e4` |
| `ocr_avail` | `0x300000` (3.2-3.3 V and 3.3-3.4 V) | `0x63e1e8` |
| `capacity` | `0x7` = 4-bit + MMC high-speed + SD high-speed | `0x63e1ec` |
| `pm_flags` | `0x6` | `0x63e1f0` |
| `max_freq` | **48 000 000** (48 MHz) | `0x63e1f4` |
| `recovery_info` | NULL | `0x63e1f8` |
| `gpio` | `0x8064e218` | `0x63e1fc` |
| vmmc name (inferred: a vendor field the SDK does not have; the pointer is the `"LDO6"` string at file `0x5c4658`) | `"LDO6"` | `0x63e200` |
| `pio_mode` | 0 — descriptor DMA, not PIO | `0x63e204` |

`card_gpio` at `0x8064e218` (file `0x5c4218`): `wp` −1, **`cd` = gpio 86, `LOW_ENABLE`**,
`pwr`/`rst` −1. The dmesg on the device agrees: `jzmmc_gpio_init 1758 card_gpio->cd.num =86`.

The second controller (MMC1, the SDIO Wi-Fi) is `MANUAL` removal with `private_init` set — not the
card.

Consequences worth remembering:

- The card runs **high-speed, 4-bit, up to 48 MHz**, in DMA mode. There is no DDR and no tuning.
- `removal` being `REMOVABLE` means the sysfs `present` store **refuses** to work: the vendor's
  `jzmmc_present_store` first checks `pdata->removal != NONREMOVABLE` and prints
  `can't set present`. The `present` node cannot be used to re-arm the card on this board. (This
  corrects an earlier guess of mine; see `.opencode/NOTES.md`.)
- Power to the slot is a regulator by name (`LDO6`), so a card power cycle is a
  `regulator_disable()`/`enable()` pair — reachable only from the driver, not from userspace.

The ops table the driver registers, at `0x80574e34` (file `0x595e34`):

```
+0x00 jzmmc_request        0x80388bd8
+0x04 jzmmc_set_ios        0x803899dc
+0x08 jzmmc_get_read_only  0x80388770
+0x0c jzmmc_get_card_detect 0x80388718
+0x10 jzmmc_enable_sdio_irq 0x8038a044
+0x14 0  …
```

**`hw_reset` is NULL**, and `MMC_CAP_HW_RESET` is not in `caps`. This is the root of the missing
recovery, below.

## One request, and what happens when the card stops answering

`jzmmc_request` (`0x80388bd8`) starts the command and arms `request_timer` for
`TIMEOUT_PERIOD` = **3000 ms** (the constant is visible twice in `jzmmc_request_timeout`, file
`0x379498` and `0x37957c`).

`jzmmc_request_timeout` (`0x80389444`, file `0x379444`) then runs at most **twice**:

1. first tick (`host->timeout_cnt` was 0): print
   `timeout %dms op:%d %s sz:%d state:%d STAT:… DMALEN:… blks:%d/%d clk:… clk_gate:…`, re-arm the
   timer for another 3000 ms (`0x379490`: `bgtz a3` skips the print path once the counter is
   non-zero);
2. second tick: print `request time out, op=… arg=… sz:…B state=… status=… pending=… nr_desc=…`,
   dump the controller registers, and — in DMA mode — dump **128** DMA descriptors
   (`MAX_SEGS`), then set `cmd->error = -EIO` (`li a0,-5` at file `0x379730` and `0x37975c`) and
   complete the request.

So a stalled transfer dies after about **6 s** with `-EIO` — no controller reset, no card re-init,
no power cycle. The `timeout 3000ms` line in the player's log is tick 1; the 5-second UI watchdog
that fires at the same time is measuring this window.

## What the block layer does with that error

`mmc_blk_err_check` (`0x8038502c`) → `mmc_blk_cmd_recovery` → `mmc_blk_cmd_error`. That function
switches on the *error code*:

- `-EILSEQ` → retry;
- `-ETIMEDOUT` → retry only when the status command failed or the card answered CRC/illegal-command;
- anything else, **including `-EIO` and the driver's `-1`** → `ERR_ABORT`.

`ERR_ABORT` becomes `MMC_BLK_ABORT`, and `mmc_blk_issue_rw_rq` (`0x803858e4`) then does:

```c
case MMC_BLK_ABORT:
        if (!mmc_blk_reset(md, card->host, type))
                break;
        goto cmd_abort;
```

`mmc_blk_reset` (`0x803844bc`) calls `mmc_hw_reset()`, which requires
`host->ops->hw_reset` and `MMC_CAP_HW_RESET`; both are absent, so it returns `-EOPNOTSUPP`, `goto
cmd_abort`, and the request ends in `-EIO` for the caller.

That `-EIO` is what the FAT driver sees: `fat_fs_error()` prints `Filesystem has been set read-only`
and the mount is gone until it is remounted. Nothing retries, nothing re-initialises the card, and
the card is *not* marked removed — the kernel still believes it is there.

## The other half: the card really disappearing

`jzmmc_detect_interrupt` (`0x80388684`) debounces the card-detect GPIO for **1200 ms**
(`msecs_to_jiffies(1200)` at file `0x3786f0`) and then `jzmmc_detect_change`
(`0x803897b8`) samples the pin once (`get_pin_status`, `0x80388644`).

- Removal: `jzmmc_reset()`, the in-flight request is completed with `-ENOMEDIUM`, and
  `mmc_detect_change(host->mmc, 0)` removes the card.
- Insert: `mmc_detect_change(host->mmc, msecs_to_jiffies(1000))` (file `0x379958`).

Two things follow. A static discharge shorter than 1.2 s cannot make the card "disappear" through
this path; and a card that vanishes while the device is in the hand is either losing contact for
longer than that, or its controller really did reset itself. The `-ENOMEDIUM` path is also the only
one that removes the card, which is why "the player says the card is not inserted, re-inserting
helps" and "the player froze and the filesystem went read-only" are two different failures that
share a trigger only when a stall happens to be followed by a detect edge.

### The detect path does not check the card first, and nothing retries

Read out of the SDK's `drivers/mmc/host/jzmmc_v12.c` and the stock image's `mmc_rescan` call sites;
the two agree on the order of events.

`jzmmc_detect_change` enters its removal branch on `(present != present_old) || (present_old &&
host->mmc->card)` -- the second term means a detect interrupt fired while a card object still
exists is treated as a removal *even when the pin still reads "present"* (it prints
`rapidly remove`). It then clears `JZMMC_CARD_PRESENT`, resets the host, completes the in-flight
request with `-ENOMEDIUM`, and calls `mmc_detect_change(host->mmc, 0)`.

`mmc_rescan()` (stock `0x8037872c`) then asks the bus to `detect` -- `mmc_sd_detect` sends **CMD13**
to the card. A card that is in the middle of its own programming, i.e. the one that just caused a
stall, does not answer; the bus is marked dead and the card is removed. What happens next depends
on the pin alone:

- `get_cd()` says "no card" -> `mmc_power_off()` and stop. Nothing probes again:
  `MMC_CAP_NEEDS_POLL` is **not** set on this host, and no other path re-arms the slot. This is the
  "card is in, the player says it is not" state, and only a physical reinsert (a new pin edge)
  leaves it.
- `get_cd()` says "present" -> `mmc_rescan_try_freq()` re-initialises the card from CMD0, which is
  the recovery we want and the one the patch below reaches for.

Consequences worth knowing, all checked against the device's own `kallsyms`:

- There is **no `jzmmc_remove`**: the driver registers `.remove = __exit_p(jzmmc_remove)`, and for a
  built-in driver `__exit_p` is NULL. `bind`/`unbind` through
  `/sys/bus/platform/drivers/jzmmc_v1.2/` therefore does nothing useful.
- `jzmmc_manual_detect` (`0x8038a1d0`) exists and is exported, but it only walks `manual_list`,
  which holds hosts declared non-removable; MMC0 is `REMOVABLE`, so it answers `-1` (and its error
  path dereferences the NULL host). Not a lever.
- `/sys/class/regulator/regulator.11` is `LDO6`, the card's rail, and its `state` is read-only;
  there is no `/dev/i2c-*` to drive the AXP2101 directly. A userspace power cycle of the card does
  not exist.
- `mount -t debugfs none /sys/kernel/debug` gives `mmc0/ios`, whose `clock:` line is the card's
  actual bus clock. That is how a clock patch is verified.

## What was done about it (2026-11-26)

The order of work follows "cheapest first", and the player's own side is
`docs/POWER.md`, "The card stops answering".

**P1 -- poll the card (`--poll`), flashed 2026-11-26.** `MMC_CAP_NEEDS_POLL` (bit 5) added to the
board's `.capacity` (`0x63e1ec`, `7 -> 0x27`; `jzmmc_probe` ORs that word into `mmc->caps`). With
that bit the core's `mmc_rescan` re-arms itself every `HZ` (`mmc_schedule_delayed_work(&host->detect,
HZ)` at file `0x368a3c`..`0x368a50`), and a host with no card object takes the re-probe path:
`get_cd()` says "present" -> `mmc_rescan_try_freq()` -> `mmc_power_up` -> `jzmmc_power_on` ->
CMD0/ACMD41/CMD2/CMD3. This is the state that used to need a physical reinsert.

What it costs, measured or read off the kernel: `wake_lock()` next to the reschedule is a no-op
here (the image has no `CONFIG_WAKELOCK`; `/sys/power` carries no `wake_lock` node), so nothing
holds suspend off; what remains is one timer wakeup per second. Every rescan of a *present* card
sends CMD13 (`mmc_sd_detect`), so a card that stops answering for more than the driver's 3 s window
is now marked removed and re-initialised within the following second instead of staying dead.

**Status: flashed, effect not yet seen live.** The image on the card (`16947a04…`, build
`261120261814`) carries it and `mmc0/ios` still reads `clock: 48000000 Hz`; a 256 MiB write ran at
13.2 MB/s with zero timeouts. The evidence that the poll actually runs is not yet captured: there is
no ftrace in this kernel, no sysfs counter, and `/dev/mem` reads of the kernel's `.data` came back
zero at every address tried, so the bit cannot be confirmed from userspace. The marker to look for
is a `mmc0: new high speed SDXC card at address 0001` line in `dmesg` **without** the card having
been taken out -- that line has so far only ever appeared at boot or after a reinsert.

**The `LDO6` warning is cosmetic.** `WARNING: unbalanced disables for LDO6` from `jzmmc_power_off`
appears on every suspend (ten in the log archive). It is not a drift: `jzmmc_power_on` enables the
rail only `if (!regulator_is_enabled(...))`, and at boot the rail is already on from the bootloader,
so the driver's own `regulator_enable()` never runs; `regulator.11/num_users` reads **0** with
`state` `enabled`, and the later `regulator_disable()` is refused by the core with that warning.
The rail therefore never changes, and no amount of suspends can make the driver lose the card.
That earlier theory is dead.

**E1 -- 24 MHz, flashed, then refuted.** `patch/v3-mmc/patch_kernel.py --freq 24` (B1). Built from the vendor
`.upt`, flashed over adb on 2026-11-26. Verified on the device: `mmc0/ios` reads
`clock: 24000000 Hz`, the card still comes up as `sd high-speed`, and a raw read of
`/dev/mmcblk0` drops from **17.9 MB/s** to **10.2 MB/s**.

The A/B that ended it: the same 1 GiB write (`dd if=/dev/zero of=/mnt/sd_0/.stress.bin bs=1M
count=1024`) on the same card, only the kernel differing (both images carry the same player):

| | 48 MHz (stock) run 1 | 48 MHz run 2 | 24 MHz (E1) |
|---|---|---|---|
| wall time | 110 s | 99 s | 289 s |
| `timeout 3000ms` | 0 | 0 | **24** |
| `request time out` (-EIO) | 0 | 0 | **5** |
| filesystem went read-only | no | no | no |

So the "the card is marginal at 48 MHz" hypothesis is wrong: halving the clock made the driver's
fixed 3 s window *less* adequate, not more, and turned a clean 1 GiB write into 24 timeouts and
five dead requests. E1 is reverted; the device runs stock 48 MHz again. A 20 ms fault-injection
window (`--timeout-ms 200`) did not fire even once in the same write, which also says the card's own
pauses at 48 MHz are shorter than 200 ms — the stalls at 24 MHz were the clock, not the card.

**S2a -- time out with `-EILSEQ` (`--retry-errno`), built, not flashed.** `mmc_blk_cmd_error()`
answers `ERR_RETRY` for a CRC error and `mmc_blk_issue_rw_rq()` re-sends the request up to five
times *without* calling `mmc_blk_reset()` (`if (retry++ < 5) break;`), so this would turn a stall
into a retry rather than a dead filesystem -- but only when the card answers the CMD13 that
`mmc_blk_cmd_recovery()` sends first. Against a wedged card it ends in `ERR_ABORT` anyway.

**S2b -- a patient second window (`--tick2-ms N`), built, not flashed.** The second tick's re-arm
is 3000 ms; `N=20000` makes a request wait 3 s + 20 s before it is failed. The one stall captured
live completed inside the second window, so this is the smallest change that would have made it
invisible to the FAT -- at the cost of a longer freeze when the card is really gone.

**S2 -- re-initialise on timeout (not built).** The intended shape is: on the second tick, reset
the host, complete the request with `-ENOMEDIUM` and call `mmc_detect_change(host->mmc, 0)`, i.e.
the vendor's own removal path minus the pin check. It cannot simply call
`jzmmc_detect_change(host)`: that function runs `del_timer_sync(&host->request_timer)`, and from
inside that timer's own callback the `del_timer_sync` loop never terminates (`running_timer` is
cleared only after the callback returns). A hand-written sequence in the function's own bytes has
to skip that call; it needs a code cave and a live stall to verify against, so it waits until E1's
result is known.

**I1 -- a timeout injector (`--timeout-ms N`), built and flashed, kept as a tool.** Both windows of
`jzmmc_request_timeout` and the arming in `jzmmc_request` set to `N` ms (`0x378f30`, `0x37957c`,
`0x379498`), so any request that outlives `2N` ends with `-EIO` through the driver's own path. This
is how the "write fails -> vfat read-only -> the player must remount" chain is reproduced on the
spot. What it taught, on the device:

- `N=200` (400 ms) never fired during a 1 GiB write: the card's own pauses at 48 MHz are shorter.
- `N=10` (20 ms) is too coarse: it kills the *status* command (CMD13/CMD18) that follows a write
  while the card is still programming, so the block layer aborts reads of metadata and the write
  never reaches the FAT -- the filesystem stayed `rw` through a 55 MB `adb push`. A working injector
  has to fail the write itself, not the status poll behind it, which a constant window cannot
  separate.
- Consequence for the player-side remount test: the real chain did happen once, unplanned, during
  those pushes (`remote Input/output error`, then `remote Read-only file system` from vfat), but the
  supervisor's log lives on the card that just went read-only, so its own report is lost exactly
  when it matters. Remounting was therefore confirmed only by state, not by its log line: the mount
  came back `rw` on the next boot.
- Writing to a card that is failing requests: `busybox dd` has no `oflag=direct`, and `adb push`
  aborts on the first `-EIO`. `echo 2 > /sys/block/mmcblk0/queue/nomerges` plus
  `mount -o remount,errors=continue` keeps the filesystem writable, but does not make 55 MB get
  through -- every write still ends in a CMD13 that can time out. Pulling a new image onto such a
  card is not reliable; flash an image that is already there (a rename costs no payload write).

**S3 -- smaller transfers (not built).** `mmc->max_req_size` is `4096 * 512` = 2 MB without
`CONFIG_MMC_BLOCK_BOUNCE` (the observed `blks:20/1024` is a 512 KB write inside one request), so a
single stall can lose a long transfer. Capping it (256-512 KB) shortens the worst case and makes a
retry cheap.

**S1 -- a real `hw_reset` (source, not possible in a byte patch).** Power the card off/on through
`LDO6`, re-init it, set `MMC_CAP_HW_RESET`; `mmc_blk_reset()` then succeeds and the block layer
retries the request, so the FAT never sees the error at all. This needs the kernel source, i.e. a
kernel that boots -- the state of that work is `.opencode/NOTES.md`, "Own kernel on the V3".

**Not available:** the sysfs `present` re-arm (needs `NONREMOVABLE`), `bind`/`unbind` (no `remove`),
`jzmmc_manual_detect` (MMC0 is REMOVABLE), and any userspace power cycle of the slot rail.


**Not available:** the sysfs `present` re-arm (needs `NONREMOVABLE`), and anything that would
re-enumerate the card from userspace — `mmc_detect_change` is not reachable from `/sys` on this
board.

## How to tell which class the device is in

- `dmesg` has `timeout 3000ms op:25 …` and then `Filesystem has been set read-only`: the stall
  class. Look at `blks:done/total` (how far the transfer got), `clk:enable`, and `STAT`. With the
  player from 2026-11-26 the follow-up line is `storage: /mnt/sd_0 went read-only under the player
  … remounting it`, then either `the card is back on …` or `the card is present but its mount is
  read-only; putting it back` with growing intervals.
- `dmesg` has `card removed, state=…` without a preceding `timeout`: the detect class (contact or
  the card's own controller reset).
- `dmesg` has `rapidly remove`: the pin bounced while a card was mounted. In the player,
  `storage: the card is present but its mount is dead; putting it back` is that class seen from
  userspace: the block device still exists, so the player keeps trying rather than reporting an
  empty slot.
- The clock actually in use is `/sys/kernel/debug/mmc0/ios` (`mount -t debugfs none
  /sys/kernel/debug` first) -- the only way to confirm a `max_freq` change from the running system.

## Reproducing the numbers

```sh
gzip -d -c sonix-packer/build/iso/UIMAGE.BIN | tail -c +65 > /tmp/kernel.bin   # uImage header is 64 B
sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-objdump -D -b binary -m mips:isa32 -EL \
    --start-address=0x379444 --stop-address=0x3797b8 /tmp/kernel.bin
```

kallsyms: addresses at file `0x5739f0` (`0x53c3` of them), the strings at `0x588910`, the token
table at `0x5c3080` with `kallsyms_token_index` at `0x5c33c0`; an entry is `len` + compressed bytes,
and expansion **drops the first character** of the concatenated token expansion (the symbol type).
