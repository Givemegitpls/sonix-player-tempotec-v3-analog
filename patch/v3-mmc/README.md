# The card that stalls: patches for the stock 3.10.14 kernel

**Status: P1 (`--poll`, `MMC_CAP_NEEDS_POLL`) flashed on 2026-11-26 and watching for its marker.**
`patch_kernel.py` here produces a uImage from the stock kernel; the artifact stays out of git (HiBy
never published that kernel's source) and is built from your own vendor `.upt`. The image on the card
is `v3_analog.upt` (`16947a04…`, stock 48 MHz + this bit); `v3_analog_clean48.upt` is the same image
without it, and the earlier ones are `v3_analog_clean48.upt`, `v3_analog_24mhz.upt`,
`v3_analog_e1.upt`, `v3_analog_e1b.upt` and `v3_analog_before-e1.upt`.

E1 (`--freq 24`) was flashed earlier the same day and **refuted by an A/B on the same 1 GiB write**:
24 timeouts and five `-EIO` at 24 MHz against zero of either at 48 MHz, twice. The clock is stock
48 MHz again; the numbers are in `docs/KERNEL-MMC.md`.

The problem, from the kernel's side, is in [docs/KERNEL-MMC.md](../../docs/KERNEL-MMC.md):
`jzmmc_request_timeout` gives the card 3 s, re-arms for another 3 s, and then fails the request
with `-EIO`. `mmc_blk_reset()` cannot do anything about it (`host->ops->hw_reset` is NULL, the
driver's `.remove` is `__exit_p()` and is NULL in a built-in kernel, so even sysfs `unbind` is
refused), the FAT driver answers `Filesystem has been set read-only`, and the card — still
physically present — is only re-initialised by a physical reinsert. The other half is the detect
path: `jzmmc_detect_change()` takes `present_old && host->mmc->card` as a removal, and
`mmc_rescan()` then sends CMD13 to a card that may be mid-programming; if the pin also reads
"no card" at that instant, `mmc_power_off()` runs and nothing ever probes again
(`MMC_CAP_NEEDS_POLL` is not set).

Measured on the device (2026-11-26, 11.6 h uptime, stock kernel): 3161 write requests, 6.7 MB,
25 ms average, one `timeout 3000ms op:25 w sz:29696 … blks:19/58` — 14 s after a resume from
`mem`, and it completed inside the second window (no `request time out` line followed). So the
card does answer again; the driver simply has no way back to it.

## The switches

Each is a few words in the decompressed image; `patch_kernel.py --help` lists them.

| switch | words | what it does | why |
|---|---|---|---|
| `--freq 24` (default) | 1 | `jzmmc_platform_data.max_freq` 48 000 000 → 24 000 000 (file `0x63e1f4`) | the cheap test for "the card is marginal at 48 MHz". Cost: half the read throughput. **E1** |
| `--tick2-ms N` | 2 | the second window of `jzmmc_request_timeout`: the delay it re-arms and the number its first tick prints (`0x37957c`, `0x379498`) | a patient window means the timer never fails a request the card was about to finish. `N=20000` gives 3 s + 20 s. **S2b** |
| `--retry-errno` | 2 | a timed-out request completes with `-EILSEQ` instead of `-EIO` (`0x379730`, `0x37975c`) | `mmc_blk_cmd_error()` answers `ERR_RETRY` for a CRC error and `mmc_blk_issue_rw_rq()` re-sends the request up to five times with no reset needed. Only helps if the card answers the status command, so it is the riskier of the two. **S2a** |
| `--poll` | 1 | `MMC_CAP_NEEDS_POLL` (bit 5) in the board's `.capacity` (`0x63e1ec`, `7 -> 0x27`) | `mmc_rescan()` then runs once a second and re-probes a card the core has no object for, instead of waiting for a pin edge that never comes. The answer to "the card is in, the player says it is not". **P1** |
| `--timeout-ms N` | 3 | both timeout windows and the number the timer prints (`0x378f30`, `0x37957c`, `0x379498`) | **not a fix, a fault injector:** any request longer than `2N` ms fails with `-EIO` through the driver's own path, so the FAT/remount chain can be reproduced on demand. **I1** |

E1 and S2b are independent and can be flashed together (`--freq 24 --tick2-ms 20000`), but E1's
A/B says not to: 24 MHz was worse under the same load. The two cheap switches to reach for now are
`--poll` (the card comes back by itself) and, if a stall is what kills the filesystem rather than a
detect bounce, `--tick2-ms 20000`.

**S2 as "re-initialise on timeout" cannot call `jzmmc_detect_change(host)`.** That vendor function
does `del_timer_sync(&host->request_timer)`, and from inside that timer's own callback the
`del_timer_sync` loop never ends (`base->running_timer` is cleared only after the callback
returns). A version of S2 that re-inits the card has to repeat those steps in its own bytes, minus
the `del_timer_sync`, which needs a code cave and a live stall to verify against -- hence the two
cheap switches first.

## Measured (2026-11-26)

The owner's unit, before and after flashing `--freq 24`:

| | 48 MHz (stock) | 24 MHz (E1) |
|---|---|---|
| `/sys/kernel/debug/mmc0/ios` `clock:` | 48000000 Hz | 24000000 Hz |
| raw read, 100 MiB from `/dev/mmcblk0` (`drop_caches` first) | 17.9 MB/s | 10.2 MB/s |
| what the card negotiated | `sd high-speed`, 4 bits | `sd high-speed`, 4 bits |
| 1 GiB `dd` write, `timeout 3000ms` | 0 (two runs) | **24** |
| the same write, `request time out` (`-EIO`) | 0 (two runs) | **5** |
| the same write, wall time | 110 s / 99 s | 289 s |

E1 lost: the driver's windows are fixed at 3 s, so halving the clock halves the margin. Stock 48 MHz
is what the device runs.

The stall metric is the count of `timeout 3000ms` in `dmesg` and in the player's log
(`grep -c "timeout 3000ms" /mnt/sd_0/.local/sonix_player.log`); the log's history holds hundreds
across boots, so the comparison is per hour of uptime, not per session. Written-down baseline:
**one timeout in 11.6 h** on the 48 MHz kernel, 2026-11-26 (`op:25 w sz:29696 … blks:19/58`, 14 s
after a resume from `mem`).


## Building, flashing, checking

```sh
mkdir -p /tmp/kv3/out && cd /tmp/kv3
7z x -y <vendor-v3_analog>.upt
python3 -c 'import gzip; d=open("UIMAGE.BIN","rb").read(); open("kernel.bin","wb").write(gzip.decompress(d[64:]))'
md5sum kernel.bin                                  # aaa60f4fc2b4d592f92bc5a405e10695, the script checks it
<repo>/patch/v3-mmc/patch_kernel.py kernel.bin UIMAGE.BIN out/UIMAGE.BIN --freq 48 --poll
cp out/UIMAGE.BIN <repo>/sonix-packer/kernel/UIMAGE.BIN
cd <repo>/sonix-packer && STOCK=$PWD/v3_analog_original.upt ./v3_analog_packer.sh
```

The packer refuses a kernel bigger than the stock one; a patched payload comes out **smaller**
(3 234 216 vs 3 236 378 bytes), so the rule holds. `UPDATE.TXT`'s kernel digest is recomputed by
the packer's own `fix_update_md5.py`.

Flash per `docs/UPDATING.md`/the `v3-adb-flash` skill, then check:

```sh
adb shell 'dmesg | grep -E "mmc0:|jzmmc|timeout|read-only"'   # mmc0: new high speed SDXC card
adb shell 'cat /sys/block/mmcblk0/stat'                         # sector counts and latencies
grep -c "timeout 3000ms" /mnt/sd_0/.local/sonix_player.log      # the metric: stalls per session
```

The player copies kernel warnings into its own log (`KMSG_LEVEL_MAX` in `logging.c`), so the
stall count is readable over adb without a running `dmesg` loop.

## Risks

* **The running kernel is no longer the stock one.** Keep `kernel.bin`/`UIMAGE.BIN` and the
  stock `v3_analog.upt`; the way back is an image built without `sonix-packer/kernel/UIMAGE.BIN`.
* `--freq 24` halves the card's read speed **and** loses the A/B above: it makes stalls more likely,
  not less. It is kept only as a tool.
* `--poll` wakes the CPU once a second and sends the card a CMD13 per second. A card that stops
  answering for more than the 3 s window is then re-initialised within the next second, which drops
  the mount -- the player's supervisor remounts it, but a stall during playback now costs a remount
  instead of one failed request.
* `--tick2-ms` makes a genuinely dead card sit in the queue for 3 s + N instead of 6 s. The
  player's own watchdog reports the stall, and the supervisor still has to survive it.
* `--retry-errno` changes the reported class of the failure: `mmc_blk_cmd_error()` will print
  "response CRC error" for what is really a timeout. Read the dmesg lines as "the driver gave
  up retryably", not as a real CRC fault.
* A patched kernel that does not boot is recovered through recovery (the update file on the
  card); the bootloader is untouched, so nothing here can brick the device.
