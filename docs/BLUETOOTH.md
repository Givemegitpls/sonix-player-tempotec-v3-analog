# Bluetooth on the V3 Analog

Consolidated from `.opencode/NOTES.md` (the chronological log) and the session archive
(`tools/history.sh`; session ids at the end). Read this before touching anything BT.

## What runs

- **BlueZ 5.66** (`bluetoothd`) + **bluealsa 4.0.0** + the ALSA plugin, in place of HiBy's
  BlueZ 4.101. The port's own code is written against BlueZ 5 D-Bus (`org.bluez.Adapter1`,
  `Media1`, `RegisterPlayer`), which 4.101 cannot serve. The stock's LDAC was a HiBy patch
  inside their BlueZ 4 (`a2dp_ldac_support`, `LDACSinks=1` in their `audio.conf`), not a
  library.
- **D-Bus policy**: `etc/dbus-1/system.d/bluealsa.conf` (owning `org.bluealsa`) and
  `bluez5-agent.conf`, both in `sonix-packer/assets/<model>/etc/dbus-1/system.d/`. Without
  them, pairing from a phone fails.
- **The chip is brought up by the firmware, not the player** (`bt_chip_by_firmware`): the
  stock `/usr/bin/bt_init` runs `brcm_patchram_plus` on `/dev/ttyS0`. Two of those on one UART
  kill the adapter (`hci0 command 0x1003 tx timeout`, then `Number of controllers: 0`).
  Recovery: `echo 0 > /sys/class/rfkill/rfkill0/state; sleep 2; echo 1 > …; /usr/bin/bt_init`.
- The player starts bluealsa per mode: `-p a2dp-source` normally, adding `-p a2dp-sink` while
  the receiver page is open; `--a2dp-volume --keep-alive=0 --ldac-abr -c aptX -c aptX-HD -c LDAC`.

## Codecs and how each is built

- **LDAC (encoder)**: AOSP `libldac` — built into
  `assets/V3ANALOG/usr/lib/libldacBT_enc.so.2` and `libldacBT_abr.so.2`.
- **LDAC (decoder)**: the **stock combined `libldacBT.so`** (in
  `firmware/tt/SYSTEM/usr/lib/`), ABI `GLIBC_2.2`, runs on the device's glibc 2.16 as it is.
- **AAC**: `libfdk-aac.so.2` (Debian non-free `libfdk-aac-dev` 2.0.2, mipsel).
- **aptX / aptX-HD**: `libopenaptx.so.0` (`libopenaptx-dev` 0.2.0 — bluez-alsa 4.0.0 knows only
  `libopenaptx`, hence `--with-libopenaptx`; `libfreeaptx` is not accepted).
- SBC is built in.
- Build inputs for the LDAC arm are kept in **`tools/ldac/`** (header, three `.pc` files,
  README with provenance and the two decoder prototypes that the AOSP header lacks).

## The defects that cost two days, and their fixes

1. **The phone's LDAC encoder never started** — `api_error = 1027`
   (`LDACBT_ERR_ILL_MTU_SIZE`), `MTU=660, peer_mtu=660`. BlueZ 5.66 announces no receive MTU
   for an AVDTP channel (`bt_io_accept()` on the media channel takes no options;
   `l2cap_connect()` passes `BT_IO_OPT_IMTU, 0`), so the L2CAP default 672 minus the 12-byte
   AVDTP media header = 660, and libldac refuses anything below **679**. Fix: announce our own
   receive MTU (1024) before accepting, on both the incoming and the outgoing path —
   `tools/bluez-5.66-avdtp-transport-mtu.patch` (the flashed `bluetoothd` is `3ed64b9c…`).
2. **Two LDAC implementations in one process** — the AOSP encoder libraries and the stock
   combined one export the same `ldacBT_*` names, so `ldacBT_get_handle()` resolved to one
   library while the handle was used by the other's code: SIGSEGV on the first real frame.
   Fix: all three `.pc` files point at the combined `-lldacBT`, and bluealsa is built with a
   single LDAC arm (`460c0f69…`). `tools/ldacdec-test.c` shows the difference in isolation.
3. **The sink advertised AVDTP 1.3 where the stock advertises 1.2.** BlueZ 5.66 hardcodes
   `0x0103` (`profiles/audio/a2dp.c:1583`); the stock derived the version from `audio.conf` and
   had no `DelayReporting` key, so it said **1.2**. AOSP picks its capability request from that
   number (`bta_av_aact.cc:344-347`): at 1.3 the phone sends `GET_ALL_CAPABILITIES`, at 1.2
   `GET_CAPABILITIES` — and BlueZ adds Delay Reporting to every `GET_ALL_CAPABILITIES` reply
   while bluealsa registers its endpoints with `DelayReporting = TRUE`. Fix:
   `tools/bluez-5.66-a2dp-sink-avdtp12.patch`.

## The encoder's priority (music stutters while scrolling)

Measured with the player's own health lines (10 s period, `bluealsa %` printed only when the
output is Bluetooth):

```
idle 61%, interface  0%, bluealsa 18%   <- idle, playing
idle 22%, interface 42%, bluealsa 15%   <- scrolling
idle 12%, interface 48%, bluealsa 18%   <- scrolling
idle 77%, interface  4%, bluealsa 19%   <- released
```

The interface takes 42-48 % of the single core and bluealsa's share *falls* — the encoder is
**preempted**, not CPU-starved. Its thread (`ba-a2dp-ldac`) is `SCHED_OTHER`, while the player's
playback thread is `SCHED_RR 10` and the flip thread `RR 5`.

- bluealsa **4.0.0 has no `--io-rt-priority`** (added in 4.2.0). Neither 4.2.0 nor 4.3.1 can be
  built here: both require `glib-2.0 >= 2.58.2`, the device has 2.46.1.
- Verified live with `chrt -f -p 8 <tid>` and `-p 12`: **both remove the stutter** (owner's
  ears), and 12 does not make the scroll sluggish — so the player's default
  `[bluetooth] io_rt_priority = 12` is the value to ship.
- The encoder thread is **recreated per track/transport**, so `chrt` is a test tool, not a fix.
  The fix is either a bluealsa patched to accept `--io-rt-priority` (the player already probes
  for the option and passes the configured value) or the player raising the thread itself.

## Tools

`tools/btprobe.c` (adapter + bus check), `tools/btadm.c`, `tools/hciwatch.c`,
`tools/rxprobe.c` (receiver PCM reader), `tools/ldacdec-test.c`, and `btmon` built for
MIPS/glibc 2.16 — the HCI/L2CAP/AVDTP decoder that found the MTU defect.

## Verifying

- Stack: `btprobe -d`; log lines `bluetooth: stack up in … ms`, `bluealsa v4.0.0 on the bus`,
  `btplayer: registered with bluez on /org/bluez/hci0`.
- Codec in use: `audio: pcm | BlueALSA Bluetooth codec: LDAC`, and the daemon's argv in `ps`.
- Priorities: `/proc/$(pidof bluealsa)/task/*/stat` — **field 40 is `rt_priority`** (field 39 is
  the CPU number; reading the wrong one makes every thread look non-RT).
- The stack in isolation, bypassing the player:
  `aplay -D bluealsa:DEV=<MAC>,PROFILE=a2dp /mnt/sd_0/tone.wav`.
- After installing `20-bluealsa.conf`, restart the player: alsa-lib caches the config per
  process, and the symptom of a stale cache is `Unknown PCM bluealsa:…` followed by a fallback
  to `plughw:1,0`.

## Archive

- `ses_ec949e19dffexQPevt0PWvY4s3` — "Дебаг LDAC между телефоном и плеером" (280 messages): the
  MTU defect, the single-LDAC-arm defect, the flash verification by md5.
- `ses_ecb67e559ffeLHvKg7KCdbvLxe` — "Research and fix BT A2DP sink": the stock-vs-BlueZ-5.66
  AVDTP comparison behind the 1.2/1.3 patch.
- `ses_ebead8b79ffezgw6hBiDS3uE4P` — the stutter diagnosis (the numbers above).
