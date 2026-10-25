---
name: pcm-capture
description: Use when the question is "is what I hear the file?" on the TempoTec V3 — recording the PCM frames a player hands the sound card and comparing them with the decoded source, on this port or on the stock firmware. Covers the pcmtap preload (ALSA and raw ioctl layers), the frame-size traps, jack vs optical routing, flashing stock and back, and the bit-exact comparison. Trigger keywords: снять семпл, что доходит до ЦАП, bit-exact, pcmtap, LD_PRELOAD sound card, сравнить с FLAC.
---

# Снятие и сверка PCM на V3 Analog

Задача, которую это решает: доказать (или опровергнуть), что плеер отдаёт в
карту ровно тот поток, который в файле — без ресемплинга, dither и ослабления.

Инструмент — `tools/pcmtap.c` в этом репозитории: прелоад, который копирует
кадры на границе с драйвером. Он не меняет поток, только читает.

## Что важно знать заранее

- У карты **нет** `/proc/asound/card*/pcm*/hw_params` (в каталоге только `id`),
  а маска формата в `struct snd_pcm_hw_params` отдаёт **один и тот же бит** и
  для 16-, и для 24-битного потока. Шаг кадра берётся только из
  `period_bytes / period_size` (и пары buffer) — шим так и делает.
- На этом ядре `struct snd_pcm_hw_params` = **604** байта: `snd_interval`
  упакован битовыми полями, интервал 12 байт, а не 24.
- Стоковый `hiby_player` **не использует alsa-lib для воспроизведения**: сам
  открывает `/dev/snd/pcmC%dD%dp` (через `openat64`) и работает ioctl'ами.
  Перехват одного `snd_pcm_writei` на стоке не увидит ничего.
- Громкость в семплы не входит: на джековом маршруте уровень держит AK4493.
  Дамп при любой громкости равен файлу (у порта `swvolume` — только USB-C).
- На порте 4.4 мм иногда роняет питание при вставке; для замеров лучше 3.5 мм
  либо вставлять джек до запуска.

## Сборка и запуск

```sh
cd tools
../sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc \
    -shared -fPIC -O2 -Wall -Wextra -march=mips32r2 -o pcmtap.so pcmtap.c -ldl
../sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-readelf --version-info pcmtap.so \
    | grep -oE 'GLIBC_[0-9.]+' | sort -u -V | tail -1    # должно быть не выше GLIBC_2.2
adb push pcmtap.so /data/pcmtap.so
```

Запуск (дамп — на карту: `/data` маленький, `/tmp` — tmpfs):

```sh
adb shell 'export LD_PRELOAD=/data/pcmtap.so PCMTAP_LAYER=raw \
           PCMTAP_OUT=/mnt/sd_0/tap.raw SONIX_LOG=/data/tap.log; \
           cd /; exec setsid /usr/bin/sonix_player' </dev/null >/dev/null 2>&1 &
```

- `PCMTAP_LAYER=raw|alsa|both` — сравнивать двух плееров надо **одним** слоем;
  `raw` работает на обоих, `alsa` только на порте.
- `PCMTAP_FRAME_BYTES` — прибить шаг вручную, если геометрия не сошлась.
- Рядом появятся `$PCMTAP_OUT.log` (устройства, геометрия, все встреченные
  ioctl) и `$PCMTAP_OUT.fdN` (дамп по каждому fd).

## Порт

`sonix_launch` останавливается, плеер запускается вручную (см. выше). Play можно
нажать самому: `/data/keyinj /dev/input/event1 116` (экран), затем `… 164`.

Проверка, что окно именно то — по логу плеера: `starting '<path>'` и
`PCM open on 'hw:0,0'` / `'hw:1,0'`.

## Сток

Вариант без перепаковки: на образе с adb (`v3_analog_azbar.upt` = сток +
`/etc/init.d/S98adb_debug`, которому нужен маркер `/mnt/sd_0/enable_adb`)
пропатчить `/usr/bin/hiby_player.sh` — две строки `export` перед запуском
плеера (`LD_PRELOAD`, `PCMTAP_LAYER=raw`, `PCMTAP_OUT`), бэкап в
`/data/init_backup/`, затем `reboot`.

Обязательно: **сначала убить обёртку** `hiby_player.sh`, иначе её `reboot`
после выхода плеера перезагрузит устройство. Найти по
`ps | grep hiby_player.sh`, убивать по pid.

Флеш в обе стороны (образ на карте должен называться ровно `v3_analog.upt`):

```sh
adb shell 'cp -f /mnt/sd_0/<образ>.upt /mnt/sd_0/v3_analog.upt; sync; \
           echo recovery > /proc/jz/reset/reset'
```

`adb` возвращается примерно через 2 минуты.

## Сверка

```sh
cd /tmp/opencode
adb pull /mnt/sd_0/tap.raw tap.raw
flac -d --force-raw-format --endian=little --sign=signed -o src.s24 file.flac
python3 -c "…"   # 3-байтовые сэмплы -> int32 со знаком (24 бита в младших)
```

Дальше — точный поиск окна: `src.find(dump[i:i+4096])` даёт позицию в треке
(плеер мог возобновить воспроизведение с любого места), после чего сравнивать
весь перекрывающийся кусок побайтово. Совпадение — «до карты доходит файл».

## Что уже измерено (не переделывать без нужды)

- порт: `hw:0,0` (оптика) и `hw:1,0` (джек) — бит-в-бит с FLAC;
- сток: `pcmC1D0p` (джек, 4.4 мм) — бит-в-бит;
- сток с пустыми разъёмами: узел `SA SPDIF Dummy` получает равномерно
  случайные байты (не аудио), а звук идёт через вендорный `/dev/sa_hgl_dma`
  без `write`/`mmap`/периодических ioctl — снимать оттуда пока нечем.

Подробности: `tools/README.md` (pcmtap) и `docs/V3-ANALOG.md`, «What actually
reaches the card, measured».
