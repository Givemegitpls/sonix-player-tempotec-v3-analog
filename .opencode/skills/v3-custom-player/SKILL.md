---
name: v3-custom-player
description: Сборка и запуск своего плеера (open_hiby_player, sonix) под TempoTec Variations V3 (Ingenic X1000, MIPS, glibc 2.16) и родственные HiBy-плееры - MIPS-тулчейн rockboxdev, проверка ABI против glibc устройства и фикс clock_gettime@GLIBC_2.17 через .symver + -lrt, вывод на SLCD-панель через FBIOPAN_DISPLAY, профиль борда (screen 320x480, board_profile), маршрутизация ALSA (card1 AK4493, PCM по свитчам наушников, Output Port Switch), узлы event1/event2, сборка target, запуск с карты, диагностика зависаний. Триггеры: собрать плеер под V3, MIPS toolchain, rockboxdev, glibc 2.16 ABI, clock_gettime GLIBC_2.17, FBIOPAN_DISPLAY, SLCD, LVGL fbdev, board_profile, AK4493, hw:1,0, open_hiby_player, sonix_player, blaze-port, sonix-player-tempotec-v3-blaze.
---

# Свой плеер на TempoTec V3

Проект — корень этого репозитория: плеер `sonix-player/`, упаковщик `sonix-packer/`, инструменты
`tools/`, правки ядра `patch/`; полный контекст — `docs/V3-ANALOG.md` и `.opencode/NOTES.md`.

## Что уже работает

- **`open_hiby_player`** (простой LVGL-плеер, `hiby-modding/open_hiby_player`): собран, **проверен на
  устройстве** — экран 320x480, тач, физические кнопки, звук (FLAC, 4.4 мм → `pcmC1D0p`). Его правки
  (device-config, `device_config.h`, `glibc_compat.c`) в этом репозитории не лежат — здесь основная
  база другая, см. следующий пункт.
- **`sonix-player`** (богатый UI, LVGL 9.6). **Основная база (2026-10-12): форк
  `carmine-bin/sonix-player-tempotec-v3-blaze` (ветка `blaze-port`)** — порт sonix на V3 Blaze, самая
  близкая железка (та же панель 320x480, тот же AK4493, тот же `S92_03` + `hiby_player.sh`, те же грабли
  «644 = бутлуп», «выход процесса = reboot»). Дерево порта — `sonix-player/`. Собран под наш тулчейн,
  ABI ок.
  Старая база — форк **`mr-f0xx/sonix-player-tempotec-v1`** с `board_profile` (компактная геометрия
  320x480, фикс SLCD-пробуждения, парковка при панике); остаётся запасным вариантом (см. ниже
  «Профиль борда»).

## Тулчейн

Нужен MIPS-тулчейн под **glibc ≤ 2.16** устройства. Его собирает сам Makefile (в docker-образе,
первый прогон 20–60 минут), рабочий скрипт — их же rockboxdev:

```sh
cd sonix-player
RBDEV_DOWNLOAD=<каталог-кэш-архивов> ./rockboxdev/rockboxdev.sh   # кэш архивов переиспользуется
```

Даёт `mipsel-rockbox-linux-gnu` (binutils 2.38, gcc 9.5.0, headers 3.10.108, glibc 2.27
с `--enable-oldest-abi=2.16`, в sysroot alsa-lib/glib/expat/dbus). Готовая сборка —
`sonix-player/rockbox-toolchain` (артефакт сборки, не пакет). Другое место — переменная
`ROCKBOX_TOOLCHAIN_DIR` (Makefile берёт её из окружения, по умолчанию `$(CURDIR)/rockbox-toolchain`;
`sonix-player/docker/build.sh` её пробрасывает).

Особенности:
- Загрузка идёт через прокси (`ALL_PROXY='socks5h://<user>:<pass>@127.0.0.1:1080'`), иначе объёмные
  архивы обрываются (у пользователя ISP режет длинные потоки). Креденшелы спрашивать, в файлы не писать.
- Docker-вариант `mipsel-toolchain:latest` (Ubuntu 24.04 + `gcc-mipsel-linux-gnu` 12.4.0) целится
  в glibc 2.39 — для V3 не годится в динамике, а статически мешает отсутствие статической libasound.

## База blaze-port: сборка target (проверено)

```sh
cd sonix-player
./docker/build.sh                    # = make target в образе sonix-player/docker (docker — единственная зависимость хоста)
# LVGL 9.6 клонируется самим Makefile'ом в ./lvgl
# Makefile: DEVICE_GLIBC_MINOR = 16   (у Blaze 22)
# glibc_compat.h: __asm__(".symver clock_gettime,clock_gettime@GLIBC_2.2");
# TARGET_LDFLAGS: += -lrt
```

Результат: `check-abi: no symbol beyond glibc 2.16`, максимум в бинаре — `GLIBC_2.9`;
`sonix_player` 9.1 МБ, NEEDED = libpthread/libdl/libasound/librt/libm/libc/ld.so.1; `sonix_launch` ~2 КБ.

Грабли загрузки: savannah (freetype) на этом канале виснет — положить `freetype-2.13.3.tar.xz`
рядом заранее (sourceforge-зеркало), тогда Makefile не зовёт wget; xiph/wavpack/LVGL качаются нормально.

Что адаптировать под V3 Analog (детали — `docs/V3-ANALOG.md`):
`hw:0` → **`hw:1`** в `alsa-controls.c`; PCM `default` → **`plughw:1,0`** (у нас `default` = card0 sa_spdif);
запись модели в `sysinfo.c` + `system-info.json`; **дисплей**: их `try_panned_display()` требует 16 bpp, у нас
32 bpp BGRA → рисуем макет 480x720 в RAM и блитим 2/3 в BGRA-страницу с `FBIOPAN_DISPLAY`. Кривые AK4493
(HDB/MDB/LDB) в их коде **совпадают** с нашими `ot_devices.json` — не трогать. `/usr/bin/bootmode.sh` у нас
нет → in-app OTA переделывать, флеш через recovery-ядро работает.

**Модель стала носителем всех device-различий** (`sysinfo_model_t`): `backlight_dir`, `user_data_dir`
(`/data` вместо `/usr/data`), `buttons_device_name` (`gpio-keys`), `mixer_card` (1), `pcm_device`
(`plughw:1,0`), `media_keys_swapped` (true), `single_fb_page`, `boot_logo_flash` (false — иначе он erase'ит
eraseblock в **mtd5 = `/data`**), `pmic_charger` (false — нет `/dev/i2c-0`).

**Дисплей: одна страница, а не две.** Драйвер отдаёт две (`smem_len` 1228800, `yres_virtual=960`), но на нашей
SLCD это даёт кадр «двумя кадрами назад» (оверлей громкости появлялся только со следующим событием) и
`copy 26 ms (100 % экрана)` на каждый кадр (страницы «должны» друг другу изменения). С `single_fb_page=true`
и блитом по повреждённой области: `copy 2 ms (11 %)`, до 47 fps, пользователь: «стало сильно шустрее».
Диагностика дисплея — `tools/panctl.c` (статический MIPS-бинарь: печатает режим fb,
делает `FBIOPAN_DISPLAY` с заданным yoffset; цветной тест «какая страница на стекле» через `cat red.bin > /dev/fb0`).

## ABI: обязательная проверка

Их компадж-хедер рассчитан на glibc 2.22; у V3 **2.16**, поэтому проверять всегда:

```sh
readelf --version-info <bin> | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV | tail -3
readelf --dyn-syms --wide <bin> | grep -E 'GLIBC_2\.(1[7-9]|[2-9][0-9])'   # пусто = ок
```

Единственный типовой лишний символ — **`clock_gettime@GLIBC_2.17`** (в 2.17 переехал из librt в libc).
Фикс — попросить старый символ и слинковать librt:

```c
__asm__(".symver clock_gettime,clock_gettime@GLIBC_2.2");
```
```make
TARGET_LDFLAGS += -lrt
```

На устройстве `librt.so.1` этот символ отдаёт (проверено `objdump -T`). После правки максимум
требуемых версий должен стать ≤ `GLIBC_2.16`. ALSA-ссылки `ALSA_0.9` на устройской библиотеке
(без version-секции) дают лишь предупреждение `no version information available`.

## Экран: SLCD требует пана

Панель V3 — **SLCD (smart LCD)**: кадр доходит до неё **только через `FBIOPAN_DISPLAY`** — драйвер
(`ingenicfb_pan_display`, ветка `LCD_TYPE_SLCD`) переставляет дескриптор и стартует SLCDC DMA. Обычная
запись в память fb **не видна вообще**, экран остаётся на последнем DMA-кадре (симптом: в памяти кадр
правильный, на экране «мусор»).

Правка в LVGL (`lvgl/src/drivers/display/fb/lv_linux_fbdev.c`, конец `flush_cb`):

```c
if(ioctl(dsc->fbfd, FBIOPAN_DISPLAY, &(dsc->vinfo)) == -1) { /* not fatal */ }
```

Плюс, если включается режим `FULL`: `lv_display_set_resolution()` должен вызываться **до**
`lv_display_set_buffers()` — иначе проверка размера буфера идёт против дефолтных 800x480, буферы не
выставляются и refresh **зацикливается навсегда** (CPU 100 %, ноль syscalls). Это баг апстрима LVGL 9.1.

Готовые правки уже в дереве: `fb_pan_flush_cb()` и `try_panned_display()` в `sonix-player/src/main.c`;
разбор пути вывода — `docs/DISPLAY.md`.
Порядок байт фреймбуфера — **BGRA** (`[B,G,R,A]`).

## Профиль борда (320x480) — альтернативный путь (форк V1)

Только если остаёмся на базе `mr-f0xx/sonix-player-tempotec-v1` (у blaze-port вместо этого макет
480x720, сжимаемый 2/3 — см. выше).

В форке V1 профиль борда (`board_profile.h`/`.c`) расширен до трёх состояний (`hiby` 480 /
`v1` 240×320 / **`v3` 320×480**), добавлена `bp_is_compact()` (= V1 **или** V3). 25 файлов UI
переименованы с `bp_is_tempotec_v1()` на неё — это и даёт V3 компактную геометрию (значения V1
подобраны под 240 px и в 320 не переполняются), при своём размере экрана и кеглях на ступень выше.

Выбор профиля: `BOARD=tempotec_v3` (дефолт; в симуляторе тоже). `SONIX_PANEL` в этой ветке меняет
только размер окна, вёрстку — нет (см. скилл `sonix-panel-rescale`).

Известный краш обоих деревьев **без `system-info.json`**: `musicsettings.c`, `vlimit_has_balanced()`
разыменовывал `sysinfo_model()` без проверки → SIGSEGV при `gui_init`. В этом дереве модели в JSON нет
(`sysinfo_model()` возвращает `&BOARD_MODEL`), а `vlimit_has_balanced()` проверяет NULL.

## ALSA (AK4493, card1)

- Микшер — **card1** (`hw:1`), не card0. Контролы: `AK4493 Gain Control`, `AK4493 Soft Mute`,
  `Digital Filter`, `Left/Right Playback Volume` (0..255), `Output Port Switch` (0..6).
- **PCM зависит от подключённого выхода** (замер fd): ничего не подключено → card0 (`default`,
  `pcmC0D0p`, оптика/SPDIF); 3.5 мм (`headset=1`) или 4.4 мм (`balance=1`) → **`hw:1,0`** (`pcmC1D0p`).
  Читать свитчи напрямую: у `detect_output()` значение 2 — ещё и «по умолчанию», по нему нельзя
  отличить «воткнут 3.5 мм» от «ничего не воткнуто».
- Громкость **обратная** (Volume Up уменьшает регистр); `Left/Right Playback Volume` **всегда читается 0** —
  проверять только на слух. `amixer set "<name>"` не находит контрол → `amixer -c 1 cset numid=N <val>`.
- `hw:1,0` без наушников отдаёт EPERM — это не гейт, а неактивный маршрут.

## Ввод и прочая периферия

`event0 earpods_wire`, **`event1 gpio-keys`** (все кнопки: 114 vol-, 115 vol+, 116 power, 163 next,
164 play/pause, 165 prev), **`event2 GT9XX-TS`** (тач, `INPUT_PROP_DIRECT` — их авто-поиск тача
найдёт его сам). Подсветка: `/sys/class/backlight/pwm-backlight.0/brightness` (max 255).
Свитчи: `/sys/class/switch/{headset,balance,digital_dt2}/state`. Сон: `/sys/power/state` = `freeze mem`.
SD уже смонтирована прошивкой в `/mnt/sd_0` — свой плеер **не должен** её перемонтировать.

## Сборка и запуск

```sh
make host    -j$(nproc)     # SDL2-симулятор, на машине (нужны sdl2, freetype2, opusfile, wavpack)
./docker/build.sh           # MIPS-бинарь: make target в образе sonix-player/docker
```

- Деревья требуют **LVGL 9.6** (sonix; Makefile клонирует его сам в `sonix-player/lvgl/`) / **9.1**
  (open_hiby_player; клонировать отдельно и симлинкнуть `lvgl`).
- `open_hiby_player`-Makefile **не трекает заголовки** → после правки `lv_conf.h` нужен `rm -rf build_target`.
- В их Makefile был баг (таб вне правила → `recipe commences before first target`); в форке исправлено.
- Симулятор: `SONIX_PANEL=320x480 SONIX_SD_ROOT=<каталог-карта> ./sonix_player_host`. Он **пишет** индекс
  и кэш обложек в каталог карты — не указывать настоящую библиотеку, делать копию пары альбомов.
- Языковые файлы: `usr/resource/sonix/language/{English,Russian}.ini` (брать из репозитория,
  `sonix-packer/assets/BLAZE/usr/resource/sonix/language/`).

## Диагностика

- Зависание в userspace: `strace -p <pid>` **без фильтра** — если syscalls нет вообще, крутится
  userspace-цикл (так нашли зацикливание refresh).
- Падение: символизация PIE через `/proc/<pid>/maps` (первая строка с путём бинаря = база) и
  `addr2line -f -C -i -e <bin> <addr-base>`. Чужие crash-обработчики (sonix) печатают pc/ra/стек сами.
- Свои `printf` **буферизуются** при перенаправлении в файл — при отладке ставить
  `setvbuf(stdout, NULL, _IONBF, 0)`, иначе лог пуст и не видно, до какого места дошло.
- `pkill -f <pattern>` убивает и собственный shell, если шаблон встречается в его командной строке.
  Глушить по точному имени (`pkill -x <comm>`, comm обрезан до 15 символов) или по PID.
- `/proc/asound/card*/pcm*/sub*/{hw_params,status}` в этом ядре пусты — как диагностика не годятся.
