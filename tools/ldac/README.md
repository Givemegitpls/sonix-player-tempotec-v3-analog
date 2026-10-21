# LDAC build inputs (saved 2026-10-16)

These are the inputs bluealsa needs to be built with `--enable-ldac` on the target.
They used to live only in a scratch directory and were nearly lost; they live here now.

## What is here

- `ldacBT.h` — the AOSP `platform/external/libldac` header, as shipped by the Arch
  package `libldac` 2.0.2.6 (`/usr/include/ldac/ldacBT.h`). It is architecture
  independent: what matters for a cross build is the declarations, not the code.
- `ldacBT-enc.pc`, `ldacBT-abr.pc` — the matching pkg-config files from the same
  package (`Libs: -lldacBT_enc` and `Libs: -lldacBT_abr`, the latter privately
  linking the encoder).
- `ldacBT-dec.pc` — written by hand for this project. The **decoder** does not come
  from AOSP (upstream master keeps only the encoder and ABR): it is the stock
  combined `libldacBT.so` that HiBy ships and drives in both directions. That file
  is inside the stock firmware's rootfs (`SYSTEM/usr/lib/libldacBT.so` when the
  vendor's `.upt` is unpacked — not part of this repository; see
  `docs/V3-ANALOG.md`, "LDAC receive, from the firmware's own library"). ABI:
  MIPS32 rel2, `NEEDED` only `libc.so.6`, max `@GLIBC_2.2`, so it runs on the
  device's glibc 2.16 unchanged.

## Two prototypes that have to be added by hand

The AOSP header declares the encoder only. For the decoder build, `ldacBT.h` needs
the two decoder entry points the stock library exports:

```c
int  ldacBT_init_handle_decode(...);
int  ldacBT_decode(...);
```

(Exact signatures: take them from the stock library's exported symbols —
`readelf --dyn-syms <stock rootfs>/usr/lib/libldacBT.so` — and match the
argument shapes the bluealsa decoder path calls.)

Link the decoder with `-Wl,--allow-shlib-undefined`: the stock library has
unresolved `ptn0`/`ptn3`/`AS` symbols that nothing on the device defines; in the
stock firmware they are never called (lazy binding).

## Built libraries (already in the tree, no rebuild needed to ship)

- `sonix-packer/assets/V3ANALOG/usr/lib/libldacBT_enc.so.2` — the encoder,
  built from AOSP `src/ldaclib.c src/ldacBT.c` (the rest of the `.c` files are
  `#include`d from `ldaclib.c`).
- `sonix-packer/assets/V3ANALOG/usr/lib/libldacBT_abr.so.2` — ABR, linked against
  the **static** `libldacBT_enc.a` (as Android.bp's `static_libs` does).

The runtime also needs `libfdk-aac.so.2` (AAC) and `libopenaptx.so.0`
(aptX/aptX-HD) — see `sonix-packer/assets/` and the chronological log
(`NOTES.md` in the original working copy, outside this repository).

## Rebuilding the encoder from AOSP (only if ever needed)

`https://android.googlesource.com/platform/external/libldac` (a GitHub mirror
works too). Sources: `src/ldaclib.c`, `src/ldacBT.c`; the remaining sources are
included by `ldaclib.c`. Then build with the target toolchain
(`sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc`, `-O2 -march=mips32r2`)
and install into `sonix-packer/assets/V3ANALOG/usr/lib/`.

The full recipe, including how bluealsa was configured, is in
`docs/V3-ANALOG.md` ("The build recipe") and in the chronological log
(`NOTES.md` in the original working copy; sections "Кодеки: LDAC/AAC/aptX
собраны и работают" and "LDAC-приём и «транспорт за прошивкой»").

## One LDAC arm, three .pc files

All three files above point at the **combined** `-lldacBT` (the stock library that has the
encoder too), not at `libldacBT_enc`/`libldacBT_abr`. The AOSP encoder libraries and the stock
combined one export the same `ldacBT_*` symbols, so linking both into one process makes
`ldacBT_get_handle()` resolve to one library while the handle is used by the other's code --
the receiver then dies with SIGSEGV on the first real frame. The upstream `.pc` files (which
name `_enc` and `_abr`) are kept here only as a record of where the files came from; what a
build of this port must use are these, pointing at `-lldacBT`.

`bt-src/pc/` holds the copies the bluealsa build actually uses, and `bt-src/build.sh` rebuilds
the whole thing from them.
