---
name: v3-firmware
description: Работа с прошивкой, recovery и прошиванием TempoTec Variations V3 (и родственных HiBy-плееров): разметка SPI NAND (mtd0 uboot / mtd3 recovery / mtd4 rootfs), обновление с карты через recovery-ядро и recoveryd, имя файла v3_analog.upt (стем модели; стоковое update.upt порт не ищет), проверка md5 в UPDATE.TXT, риск кирпича при записи U-Boot, вход в recovery по ADC-клавише, бэкапы mtd0/mtd3, Ingenic Cloner как последний рубеж. Триггеры: прошивка V3, recovery, update.upt, v3_analog.upt, recoveryd, mtd, uboot, кирпич, brick, Cloner, перепрошить плеер, TempoTec V3 firmware.
---

# Прошивка и recovery TempoTec V3

Каталог проекта — корень этого репозитория: упаковщик `sonix-packer/v3_analog_packer.sh` (образ
`sonix-packer/out/v3_analog.upt`), распакованный сток — `sonix-packer/build/iso/` (появляется
после прогона упаковщика), содержимое образа — `docs/V3-ANALOG.md`, «The image: `v3_analog.upt`».

## Устройство

TempoTec Variations V3-A / V3 Analog, HiBy OS, SoC **Ingenic X1000E**, ядро **Linux 3.10.14**,
**glibc 2.16**, панель **320x480** (SLCD), 2×AK4493SEQ, SD монтируется в **`/mnt/sd_0`**.
Полные данные — `docs/V3-ANALOG.md`, `README.md` и `.opencode/NOTES.md`.

## Разметка NAND (`/proc/mtd`, всего 128 МБ)

| mtd | размер | имя |
|---|---|---|
| mtd0 | 512 КБ | **uboot** |
| mtd1 | 2.5 МБ | logo |
| mtd2 | 6 МБ | kernel |
| mtd3 | **7 МБ** | **recovery** |
| mtd4 | 64 МБ | rootfs |
| mtd5 | 48 МБ | data |

`/data` — это `ubi1_0` (mtd5), корень — `ubi0:rootfs` (mtd4). **rootfs смонтирован rw** → правки
rootfs можно применять живьём по adb, без перепаковки и прошивки.

## Как происходит прошивка с карты

1. **Триггер**: ADC-клавиша (**prev-track**) при включении. U-Boot вместо `bootcmd` выполняет
   `sfcnand read 0x900000 0x500000 0x80600000 ;bootm` → грузится **recovery-ядро из mtd3**.
2. Его initramfs: `rcS` → `/etc/init.d/S90user` (`dd if=/dev/zero of=/dev/fb0`, затем `/sbin/recoveryd &`).
3. mdev-hotplug подхватывает образ с карты и монтирует его loop'ом как ISO:
   ```
   mount /dev/$MDEV /mnt/sdcard
   mount -o loop /mnt/sdcard/<stem>.upt /mnt/autoupdate/autoupdate
   ```
   `<stem>` — из таблицы моделей порта; у V3 Analog это **`v3_analog.upt`**
   (`update_stem` в `sonix-player/src/system/device/board.h`, `docs/BUILD.md` §6 «Flash it»).
4. `/sbin/recoveryd`: читает `autoupdate/update.txt` + `version.txt`, парсит `name= ver= md5= file_path=
   full_upgrade=`, по компонентам **bootloader(uboot) / kernel / rootfs / splash(logo) / userfs(data)**
   сверяет md5, стирает eraseblock'и, пишет через `/dev/mtd%d`, печатает `upgrading process %d%%`.

Структура образа: `v3_analog.upt` = ISO9660, внутри `UBOOT.BIN`, `UIMAGE.BIN`, `SYSTEM.UBI`, `UPDATE.TXT`,
`VERSION.TXT`, `_GITIGNO`. Подписи нет — только md5 в открытом `UPDATE.TXT`.

## Риск кирпича — ровно один

`recoveryd` **пишет bootloader в mtd0**. Плохой/обрезанный `UBOOT.BIN` или прерывание записи mtd0 →
нет U-Boot → NAND-загрузка мертва и recovery недостижим. Всё остальное восстановимо: **recovery-ядро
(mtd3) в `.upt` не входит** и никогда не перезаписывается.

Правила:
1. **`UBOOT.BIN` не менять** — держать стоковый байт-в-байт (`1f54858d6b4e6f482a81a3378c0e5bf8`).
2. Для прошивки через recovery файл на карте обязан называться **`v3_analog.upt`** — стем из
   таблицы моделей (`update_stem` в `board.h`), стоковое `update.upt` порт не ищет.
3. **Проверять вход в recovery до** рискованных операций.
4. Держать бэкапы mtd0/mtd3 (снимаются `dd`'ом, см. «Снятие дампов»).
5. Не трогать `system.ubi` без нужды: безопаснее менять rootfs **живьём** (он rw) — это вообще без прошивки.

Последний рубеж — **USB-boot режим ROM + Ingenic Cloner** (github.com/Ingenic-community/Cloner,
бинарники в Releases, исходников нет).

Замечание: md5 U-Boot в NAND (`b895f63a697e0a6031a7e5ab5a9361c4`) ≠ `UBOOT.BIN` стокового `.upt`
(`1f54858d6b4e6f482a81a3378c0e5bf8`) — U-Boot уже перезаписывался (пользователь прошивал ~66 кастомных
образов).

## Установка своего плеера на устройство (без прошивки)

Ручной запуск (для отладки):

```sh
adb push <bin> /mnt/sd_0/<name> && adb shell 'chmod +x /mnt/sd_0/<name>'
adb shell 'killall -9 hiby_player.sh; sleep 1; killall -9 hiby_player'   # порядок обязателен!
adb shell 'cd /mnt/sd_0 && nohup ./<name> > /mnt/sd_0/run.log 2>&1 &'
```

Порядок обязателен: `hiby_player.sh` при выходе плеера делает `sleep 1; reboot`.
После убийства стока стоковый UI **сам не возвращается** — только перезагрузкой (`adb shell reboot`;
`adb reboot` на этой прошивке отвечает `reboot failed: Bad address`).

Постоянный запуск с карты (без прошивки, штатным механизмом): launcher
`/etc/init.d/S92_03_start_music_player` предпочитает **`/ui_data/player`**, иначе `/usr/bin/hiby_player.sh`.
`/ui_data` не существует и не является точкой монтирования, но rootfs rw, поэтому достаточно:

```sh
adb shell 'ln -s /mnt/sd_0/ui_data /ui_data'
mkdir -p /mnt/sd_0/ui_data && cp <player> /mnt/sd_0/ui_data/player
```

Свойства: ноль прошивок; **нет карты → сток** (fallback `[ -f ] ||`); порядок инита подходящий
(`S91_early_mount.sh` монтирует карту до `S92…`). Обновление прошивки сотрёт симлинк.

## Снятие дампов и бэкап

```sh
adb shell 'dd if=/dev/mtd3 of=/data/mtd3.bin bs=1M 2>/dev/null'   # в /data (ubifs), не в /tmp (tmpfs)
adb pull /data/mtd3.bin mtd3_recovery.bin && adb shell 'rm -f /data/mtd3.bin'
```

`adb exec-out 'cat /dev/mtd0'` работает ненадёжно (отдаёт 0 байт) — использовать `dd` + `adb pull`.
Recovery-ядро внутри mtd3 — это uImage (gzip); распаковка: пропустить 64-байтный заголовок и `gunzip`,
далее initramfs = сpio → внутри XZ → `recoveryd` и hotplug-скрипты.

## Диагностика

- `/proc/mtd` — разметка; `cat /proc/mounts` — что смонтировано (карта, ubifs).
- Панель: `cat /sys/class/graphics/fb0/{bits_per_pixel,virtual_size,stride}`. Здесь **32 bpp**, stride 1280.
- Дамп кадра: `dd if=/dev/fb0 of=/data/fb.bin bs=1024 count=600` → конвертировать как **BGRA**
  (`[B,G,R,A]`); PIL `'RGBA'` даёт R↔B-переворот, нужна ручная перестановка.
