---
name: v3-adb-flash
description: "Use when flashing/updating the TempoTec V3 Analog (or a HiBy-family player) firmware from a shell instead of the recovery key combo: triggering recovery from adb, checking the update file, verifying the build afterwards, and the /data/recovery_all factory-reset trap. Trigger keywords: прошить V3, обновить прошивку по adb, v3_analog.upt, /proc/jz/reset/reset, recovery_all, recovery_data, сброс к заводским, bootmode.sh."
---

# Прошивка V3 Analog по adb (без клавиш)

Проверено на устройстве 2026-10-13 (порт sonix, образ `80635ead…`): обновление запускается
программно, adb возвращается примерно через 75 секунд после старта прошивки.

## Обновление (единственная нужная команда)

1. Положить образ на **корень карты** под именем `<update_stem>.upt` — у V3 Analog это
   `v3_analog.upt` (стем задаёт таблица модели сборки, `update_stem` в
   `sonix-player/src/system/device/board.h`; стоковое `update.upt` порт не принимает). Перед записью
   убедиться, где карта смонтирована: у порта это `/mnt/sd_0`, но встречалось и `/data/mnt/sd_0` —
   проверить `mount | grep sd_0`.
2. Сверить md5 файла с локальной сборкой (сборщик печатает его в конце).
3. Попросить SoC загрузить recovery-ядро:

   ```
   echo recovery > /proc/jz/reset/reset
   ```

   U-Boot грузит recovery (`mtd3`), тот читает `v3_analog.upt` с карты и пишет rootfs (`mtd4`) и
   ядро. `/data` при этом не затрагивается (`UPDATE.TXT` перечисляет только kernel и rootfs).
4. Проверить результат по штампу сборки:

   ```
   adb shell 'grep build_version /usr/resource/sonix/components/system-info.json'
   ```

   Значение — это mtime бинаря плеера на момент сборки (его печатает упаковщик).

## ЛОВУШКА: `/data/recovery_all` — это сброс к заводским, а не обновление

Строки стокового бинаря (`echo recovery_all > /data/recovery_all`, `echo recovery_data > …`,
`echo recovery > /proc/jz/reset/reset`) выглядят как один сценарий, но это **два разных**.

- `/data/recovery_all` читает init-скрипт `/etc/init.d/S39_recovery.recovery` **на каждой
  загрузке**; если файл есть, он выполняет `/usr/bin/recovery_all.sh`, а тот чистит
  `/var/lib/bluetooth`, `/var/lib/dbus`, `/usr/var` и делает `rm -rf /data/*`, после чего
  перезагружается.
- Слово внутри файла задаёт глубину: `recovery_all` — стереть всё (настройки, медиатека,
  сопряжения Bluetooth), `recovery_data` — пощадить только `usrlocal_media.db` стоковой прошивки.
- **К обновлению это отношения не имеет.** Первая версия правки в `firmware.c` писала
  `recovery_all` вместе с запросом recovery — и первая же прошивка через adb вытерла пользователю
  настройки и сопряжение телефона. Правильный запуск обновления — **без маркера вообще**.

Полное обоснование механизма — в самом порте: `sonix-player/src/system/device/factoryreset.c`
(там же: почему `rm -rf /data/*` безопасен только на S39, когда карта ещё не смонтирована плеером).

## Откат, если прошивка не поднялась

Оба образа держать на карте одновременно нельзя (recovery ищет только `<stem>.upt`), поэтому:

- стоковый образ для отката — держать рядом с другим именем и **переименовывать** в
  `v3_analog.upt` перед прошивкой;
- если система не поднимается, recovery доступен **физически**: Power + Prev при включении. Это
  работает и без работающей ОС и не зависит от `/proc/jz/reset/reset`.

## Грабли, проверенные на этом устройстве

- **`adb shell` портит бинарные данные** (pty, перевод строк): читать разделы через
  `dd if=/dev/mtdN of=/data/x.bin` + `adb pull`, а не `adb shell 'dd …' > file`.
- **busybox `setsid` переставляет опции**: `setsid bluealsa -p a2dp-sink` → `invalid option -- 'p'`;
  `setsid sh -c "exec cmd"` тоже ломается (busybox съедает `-c`). Работает только запуск
  скрипта-обёртки без опций: `setsid /tmp/start.sh`.
- **USB-режимом владеет плеер**: при включённом adb карта остаётся у плеера (см. модель
  `usb_gadget_legacy`), при выключенном — может быть отдана компьютеру как накопитель, и тогда
  `/mnt/sd_0` на устройстве пуст. Проверять `mount | grep sd_0` перед записью на карту.
- **После прошивки adb появляется, когда процесса плеера нет** (сторож в образе выставляет режим
  при загрузке). Если плеер уже поднялся, переключатель Developer options → ADB возвращает adb.
