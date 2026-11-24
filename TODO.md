# TODO — лицензионная гигиена: шрифты и кодеки

Дата: 2026-11-23. Ветка: рабочая копия `tempotec-v3-main-wt`.
Область: `sonix-packer/assets/BLAZE/*` (наша база образа), `sonix-player/src/gui/ebook/`,
`sonix-packer/v3_analog_packer.sh`, `sonix-packer/tools/`, `docs/`.

Решения владельца:

- **R1/R3PII не трогаем.** `sonix-packer/assets/R1`, `assets/R3PII`, `kernel/xImage-R1`,
  `kernel/xImage-R3PII`, `sonix_firmware_packer.sh` остаются как есть, хотя V3-сборка их не
  использует (~66 MB). Пункты ниже, где они упоминаются, — только для сведения.
- **Шрифты заменяем**, а не просто выкидываем.
- **Кодеки: FDK-AAC остаётся**, отдельная лицензия не покупается, faad2 — только если
  понадобится «ноль non-free».

---

## 0. Факты, на которых всё стоит (проверено 2026-11-23)

| Артефакт | Где | Лицензия | Что это значит |
|---|---|---|---|
| `libfdk-aac.so.2` | `assets/V3ANALOG/usr/lib/` (в `BLAZE` его нет) | Fraunhofer FDK AAC, «Software License for The Fraunhofer FDK AAC Codec Library» | бинарное распространение **разрешено** при трёх условиях (ниже); патентная лицензия **не** выдаётся |
| `Bookerly-{Regular,Italic,Bold,BoldItalic}.ttf` | `assets/BLAZE/.../fonts/ebook/` | Amazon / Dalton Maag EULA (`name ID 14` = daltonmaag.com/eula) | перераспространение не разрешено |
| `japanese.otf`, `japanese-bold.otf` | `assets/BLAZE/.../fonts/` | **FOT-RodinNTLG Pro**, Fontworks, `LIC000451`, `name ID 14` = fontworks.co.jp | коммерческий, перераспространение не разрешено |
| `korean.otf`, `korean-bold.otf` | `assets/BLAZE/.../fonts/` | SIL OFL 1.1 (`name ID 13/14`) | ок |
| `default.otf`, `bold.otf`, `arabic*.otf`, `thai*.otf` | `assets/BLAZE/.../fonts/` | MiSans, Xiaomi | встраивание разрешено, но с **обязательной атрибуцией**; отдельно распространять файлы шрифта нельзя; изменять нельзя |
| `bluealsa`, `bluetoothd`, `libbluetooth.so.3` | `assets/V3ANALOG/` | BlueZ GPL-2.0, bluealsa MIT, модуль ALSA MIT | ок |
| `libldacBT_enc/abr.so.2` | `assets/V3ANALOG/` | AOSP libldac, Apache-2.0 | ок |
| `libopenaptx.so.0` | `assets/V3ANALOG/` | LGPL-2.1 | нужна оферта исходников |
| `libcrypto/libssl 1.1.1` | `assets/V3ANALOG/` | OpenSSL, Apache-2.0 | ок |

Условия FDK (дословно из `NOTICE` upstream `mstorsjo/fdk-aac`):

1. полный текст лицензии — в документации и/или материалах, сопровождающих бинарную поставку;
2. получателям бесплатно предоставляется полный исходный код FDK;
3. изменённые версии помечаются, имя «Fraunhofer FDK AAC Codec Library for Android» заменяется.

Плюс раздел 3: «You may use this FDK AAC Codec software … only for purposes that are authorized by
appropriate patent licenses» — то есть отдельно лицензируются **патенты** (Via LA / Fraunhofer),
а не copyright. Для AAC-LC/HE-AAC основная часть пула истекла; для открытого образа это
регистрируемый риск, а не блокер, решение владельца — оставляем.

Как FDK попадает к нам: `bt-src/build.sh` качает `libfdk-aac-dev_2.0.1-1_mipsel.deb` из Debian
non-free и кладёт `.so` в `bt-src/sysroot/usr/lib/`; в git сейчас лежит уже оттуда скопированный
бинарь. `bt-src/sysroot/`, `bt-src/debs/` — в `.gitignore`.

Плеер грузит FDK через `dlopen` (`sonix-player/src/system/decode/aacdec.c`), bluealsa линкуется
(`--enable-aac` в `bt-src/build.sh`), но bluealsa — MIT. Связки GPL-3.0 с FDK в одном
произведении нет; «mere aggregation» в образе.

Опубликовано наружу у нас корректно: CI **не** собирает и **не** публикует `.upt`
(`.github/workflows/player-package.yml`, комментарий в шапке; `docs/BUILD.md` §8; `docs/FIRMWARE-WEB.md`).

---

## 1. Шрифты

- [ ] **1.1** В `assets/BLAZE/usr/resource/sonix/fonts/ebook/` положить
      `Literata-Regular.ttf`, `Literata-Italic.ttf`, `Literata-Bold.ttf`,
      `Literata-BoldItalic.ttf` (OFL 1.1), удалить `Bookerly-*.ttf`.
- [ ] **1.2** `japanese.otf`/`japanese-bold.otf` заменить на `japanese.ttf`/`japanese-bold.ttf`
      (Noto Sans JP, OFL, subset).
- [ ] **1.3** MiSans оставить, но выполнить условие атрибуции: добавить на экран About
      (или в `docs/`) строку «Fonts: MiSans (Xiaomi)…». Точное место — решить при реализации.
- [ ] **1.4** Исходники шрифтов в git **не** кладём: в git только готовые файлы (как сейчас),
      рецепт — в `tools/make_reader_fonts.py` (см. 1.6).

### 1.5 Рецепт, уже проверенный в этой сессии

Literata: из `google/fonts` `ofl/literata/Literata[opsz,wght].ttf` и
`Literata-Italic[opsz,wght].ttf`, инстансы `opsz=16`, `wght=400/700` через
`fontTools.varLib.instancer`, затем исправить `name` (1/2/4/6/16/17), `OS/2.usWeightClass`,
`OS/2.fsSelection` (0x40 regular / 0x01 italic / 0x20 bold / 0x21 bold italic), `head.macStyle`
(0 / 2 / 1 / 3). Результат ~271 / 260 / 271 / 260 KB (Bookerly был ~450 KB каждый).

Noto Sans JP: из `ofl/notosansjp/NotoSansJP[wght].ttf`, инстансы `wght=400/700`, затем
`fontTools.subset` по набору символов из cp932 (все двухбайтовые последовательности, декодированные
как cp932: 7326 символов, Noto покрывает 7325 — нет только `U+2252`) плюс ASCII, Latin-1,
Latin Extended-A, `U+2000–206F`, `U+2460–24FF`, `U+25A0–25FF`. Опции:
`hinting=False`, `notdef_outline=True`, `recommended_glyphs=True`,
`layout_features=["ccmp","locl","kern","liga"]`. Результат 2.39 MB каждый (Rodin был 2.50/2.56),
7751 глиф.

Итог по размеру образа: −6.9 MB (Bookerly+Rodin) + 5.9 MB (Literata+Noto) ≈ **−1 MB**.

Важно: нельзя брать cmap Родина как набор символов — там есть SJIS-псевдокоды
(`0x8140`, `0x8156`, …), и подмножество получается мусорным (проверено, 4949 «пропавших» глифов).

### 1.6 Генерация как инструмент

- [ ] Оформить рецепт 1.5 как `tools/make_reader_fonts.py` (скачивает/принимает файлы, делает
      инстансы и subset, печатает sha256), чтобы шрифты можно было воспроизвести после обновления
      upstream.

## 2. Код плеера

- [ ] **2.1** `sonix-player/src/gui/ebook/ebookfonts.c`:
      - `FACE_FILES` → `Literata-Regular.ttf`, `Literata-Italic.ttf`, `Literata-Bold.ttf`,
        `Literata-BoldItalic.ttf`;
      - `JAPANESE_FILES` → `japanese.ttf`, `NULL`, `japanese-bold.ttf`, `NULL`;
      - переписать комментарии про Bookerly; проверить, что override
        `line_height = size * 3 / 2` / `base_line = size * 37 / 100` для японского остаётся
        корректным для Noto Sans JP (метрики другие, проверить глазами).
- [ ] **2.2** `sonix-player/src/gui/ebook/ebookfonts.h`: комментарии (Bookerly, Rodin).
- [ ] **2.3** `sonix-player/src/system/ebook/ebook.h`: упоминание Bookerly в комментарии.
- [ ] **2.4** Локализация: ключ `ebookreader_bookerly_missing` → `ebookreader_font_missing`,
      текст «Bookerly» → «Literata» в 12 файлах
      `assets/BLAZE/usr/resource/sonix/language/*.ini` и в одной строке кода
      `sonix-player/src/gui/ebook/ebookreader.c:2318`.
      R1/R3PII не трогаем (там ключ остаётся старым — это их сборки).

## 3. Лицензии в репозитории и в образе

- [ ] **3.1** Каталог `licenses/` в корне:
      - `FDK-AAC-NOTICE.txt` (полный текст из upstream `mstorsjo/fdk-aac/NOTICE`);
      - `OFL-Literata.txt`, `OFL-NotoSansJP.txt` (OFL 1.1 из `google/fonts`);
      - `MiSans-License.txt` (соглашение Xiaomi, PDF-текст или ссылка + выжимка);
      - `README.md` — таблица «артефакт → лицензия → обязательство» (см. раздел 0) и правило:
        новый бинарь/шрифт в git без строки в этой таблице не проходит аудит.
- [ ] **3.2** Копия нотисов в образ: `assets/V3ANALOG/usr/share/licenses/{FDK-AAC,OFL,MiSans}/`
      (копруется `install_tree.sh` вместе с остальными ассетами; проверить, что `usr/share`
      попадает в squashfs и не мешает `trim_stock.py`).
- [ ] **3.3** LGPL-обязательство по `libopenaptx.so.0`: указать в `licenses/README.md`, откуда
      берётся исходник (`bt-src/build.sh`, версия из `bt-src/README.md`).

## 4. FDK-AAC: убрать бинарь из git

- [ ] **4.1** `git rm --cached sonix-packer/assets/V3ANALOG/usr/lib/libfdk-aac.so.2`
      (единственная копия в V3-пути; в `assets/BLAZE` её нет, в `assets/R1` — своя, не трогаем).
- [ ] **4.2** `sonix-packer/v3_analog_packer.sh`:
      - `BT_SYSROOT="${BT_SYSROOT:-$SCRIPT_DIR/../bt-src/sysroot}"`;
      - проверка на хосте: файл `$BT_SYSROOT/usr/lib/libfdk-aac.so.2` есть, иначе `die` с текстом
        «соберите bt-src: `bt-src/build.sh` кладёт его в `bt-src/sysroot/usr/lib`»;
      - второй `-v` в docker-run, который выполняет `tools/install_tree.sh`
        (`-v "$BT_SYSROOT:/bt-src-sysroot:ro"` + `-e BT_SYSROOT=/bt-src-sysroot`).
- [ ] **4.3** `sonix-packer/tools/install_tree.sh`: копировать
      `$BT_SYSROOT/usr/lib/libfdk-aac.so.2` в `$TREE/usr/lib/` (режим 0644, как сейчас),
      падать, если нет.
- [ ] **4.4** Проверить, что существующий `want("usr/lib/libfdk-aac.so.2")` в верификации пакера
      по-прежнему проходит.
- [ ] **4.5** `.gitignore`: ничего нового не нужно (`bt-src/sysroot/` уже там), но упомянуть
      в `docs/BUILD.md`, что образ теперь требует собранный `bt-src/sysroot` — как требует
      вендорский `v3_analog_original.upt`.

## 5. Аудит в CI

- [ ] **5.1** `tools/license_audit.py`: обойти `git ls-files`, отобрать ELF (по magic), `.ko`,
      `.ttf/.otf`, `*.dat`; для каждого найти строку в `licenses/README.md`; неизвестный
      артефакт или отсутствующий файл лицензии — `exit 1`.
- [ ] **5.2** Шаг «license audit» в `.github/workflows/player-package.yml` перед сборкой.
      Шаг должен работать без toolchain: только git и python3.
- [ ] **5.3** Учесть, что `assets/R1`, `assets/R3PII` остаются в git со своими Bookerly и
      `libfdk-aac` — либо добавить их в allowlist с пометкой «не входит в V3-образ», либо
      аудит проверяет только `assets/V3ANALOG` + `assets/BLAZE`. Решить при реализации,
      рекомендация — второй вариант плюс отдельная запись «R1/R3PII: не собираются».

## 6. Документация

- [ ] **6.1** `docs/BUILD.md`: новый раздел «Third-party licences» — таблица из раздела 0,
      три условия FDK, патентная оговорка, обязательство LGPL по `libopenaptx`,
      атрибуция MiSans. Сослаться из §8 «Distributing an image».
- [ ] **6.2** `docs/V3-ANALOG.md`: чем теперь набран ридер (Literata, Noto Sans JP), почему
      не Bookerly/Rodin.
- [ ] **6.3** `docs/UPDATING.md`: пункт «после merge upstream возвращает Bookerly и Rodin, а
      `ebookfonts.c` — старые имена файлов; возвращать нельзя» — и что именно проверять
      (`tools/license_audit.py`, `docs/V3-ANALOG.md`).
- [ ] **6.4** `README.md`: строка про лицензии образа (сейчас упоминается только GPL-3.0 кода).
- [x] **6.5** `README.md`, раздел **Updates** — исправлен 2026-11-24: плеер обновляется по сети
      («Via internet», релизы этого репозитория, `index.json`, пакет, sha256, подмена бинаря и
      откат), образ — по-прежнему карта/recovery, `ota-repo` нет.
- [x] **6.6** `README.md`: браузерный сборщик добавлен в шаг 3 Install со ссылкой на живую страницу
      (`https://givemegitpls.github.io/sonix-player-tempotec-v3-analog/`, 200) и на
      `docs/FIRMWARE-WEB.md`.
- [ ] **6.7** `fwupdate.c`: после `return` в `show_choice()` осталась мёртвая ветка со старым
      `online_clicked` — убрать вместе с самим путём обновления образа по сети (статическая
      функция станет неиспользуемой, это каскад). Тексты README и `docs/PLAYER-UPDATE.md` уже
      приведены к коду 2026-11-24, включая шапку **Status** и подпись пункта меню.
- [ ] **6.8** `README.md`, Building: «the host needs docker and nothing else» перестанет быть
      правдой после раздела 4 (FDK берётся из `bt-src/sysroot`) — дописать требование собрать
      `bt-src` и положить `libfdk-aac-dev` из Debian non-free.

## 7. Проверка после реализации

- [ ] **7.1** `make -C sonix-player host` и `make -C sonix-player test` — без новых варнингов.
- [ ] **7.2** Сборка образа (`sonix-packer/v3_analog_packer.sh`) на машине с собранным
      `bt-src/sysroot`; `verify` проходит.
- [ ] **7.3** На устройстве: английская книга (4 начертания, курсив/болд), японская книга
      (кана + кандзи JIS X 0208, интерлиньяж), отсутствие файлов шрифта → fallback на
      интерфейсный шрифт и корректная строка сообщения.
- [ ] **7.4** Размер образа: ожидается ≈ −1 MB против текущего.

## 8. Отложено: faad2 (только по отдельному решению)

Условие входа: требование «в образе нет non-free вообще». Тогда:

- [ ] `bt-src/build.sh`: `--disable-aac` (AAC в A2DP-приёмнике пропадает; SBC/aptX/aptX-HD/LDAC
      остаются), убрать `libfdk-aac-dev` из sysroot.
- [ ] `aacdec.c` переписать на faad2 2.10.1 (GPL-2+, совместим с GPL-3.0): ADTS и raw ASC,
      `NeAACDecInit`/`NeAACDecDecode`; проверить HE-AAC (SBR/PS) на слух и по спектру.
- [ ] Тесты: `.m4b`, HLS-радио (AAC), обычные `.m4a`.
- [ ] Обновить `licenses/README.md`, `docs/BLUETOOTH.md` (раздел про AAC-кодек).

## 9. Сознательно не делаем

- Не удаляем `assets/R1`, `assets/R3PII`, `kernel/xImage-*`, `sonix_firmware_packer.sh`
  (решение владельца; V3-сборкой не используются).
- Не публикуем `.upt` из CI — текущее поведение сохраняется.
- Не покупаем лицензию Fraunhofer/Via LA: copyright-лицензия FDK уже разрешает то, что мы делаем,
  патенты — зафиксированный риск, а не блокер.

---

## 10. План коммитов

Стиль репозитория — `<scope>: <что делает>`, императив, без «fix». `git commit` агенту запрещён
глобальным конфигом: коммиты готовятся (`git add`, файл с сообщением) и передаются владельцу.

1. `ebook: set books in Literata and Japanese in Noto Sans JP`
   — `assets/BLAZE/.../fonts/ebook/Literata-*.ttf`, `japanese{,-bold}.ttf`,
   `ebookfonts.c/h`, `ebook.h`, 12 × `language/*.ini`, `ebookreader.c`.
2. `licenses: ship the third-party notices and say what each file is`
   — `licenses/` (NOTICE, OFL ×2, MiSans, README), `assets/V3ANALOG/usr/share/licenses/`.
3. `packer: take libfdk-aac from bt-src/sysroot, not from the tree`
   — `v3_analog_packer.sh`, `tools/install_tree.sh`, `git rm --cached` библиотеки,
   строка в `docs/BUILD.md` про новое требование к сборке.
4. `system: name the fonts in Info`
   — `systempage.c` + ключ `system_fonts` в 12 файлах локализации.
5. `ci: refuse a binary or a font that has no licence entry`
   — `tools/license_audit.py`, шаг в `.github/workflows/player-package.yml`.
6. `docs: the licences of the image, and the reader's fonts`
   — `docs/BUILD.md` §8 + новый раздел, `docs/V3-ANALOG.md`, `docs/UPDATING.md`, `README.md`.

Порядок 1 → 2 → 3 → 5 → 4 → 6: аудит (5) должен увидеть уже новые имена файлов, но ещё не
требовать записей, которых нет; проще — 5 последним перед документацией.

## 11. Готовые фрагменты

### 11.1 `sonix-player/src/gui/ebook/ebookfonts.c`

```c
static const char *const FACE_FILES[FACE_COUNT] = {
	"Literata-Regular.ttf",
	"Literata-Italic.ttf",
	"Literata-Bold.ttf",
	"Literata-BoldItalic.ttf",
};
```

```c
// Noto Sans JP has no italic, so the slanted styles fall back to the upright
// ones, the same way Rodin did before it.
static const char *const JAPANESE_FILES[FACE_COUNT] = {
	"japanese.ttf",
	NULL,
	"japanese-bold.ttf",
	NULL,
};
```

Комментарий в `open_face()` про «Rodin's line is two ems tall» переписать под Noto Sans JP и
**проверить на устройстве**, оставлять ли `font->line_height = size * 3 / 2` и
`font->base_line = size * 37 / 100`: у Noto другие `hhea` ascender/descender/lineGap.

### 11.2 Локализация (12 файлов `assets/BLAZE/usr/resource/sonix/language/*.ini`)

```
-ebookreader_bookerly_missing = Bookerly is not in .local/fonts: reading with the interface font.
+ebookreader_font_missing = Literata is not in .local/fonts: reading with the interface font.
```

Остальные 11 — та же замена слова, остальной текст не трогается. В `ebookreader.c:2318`
`tr("ebookreader_bookerly_missing")` → `tr("ebookreader_font_missing")`.

### 11.3 Строка атрибуции шрифтов (`src/gui/settings/systempage.c`, блок About/Info)

Рядом с `info_row(container, "system_serial_number", &serial_value);`:

```c
// MiSans' licence asks that the software say it uses the font; Literata and
// Noto Sans JP are OFL and ask for the notice, which licenses/ carries.
info_row(container, "system_fonts", NULL);
```

```
system_fonts = Fonts: MiSans (Xiaomi), Literata, Noto Sans JP
```

Проверить, что строка влезает в 320 px (иначе `fit_row_label()` уронит шрифт до 20 px — см.
комментарий про «OS version» в том же файле).

### 11.4 `sonix-packer/tools/install_tree.sh`

```sh
# FDK-AAC is not in this repository: it is non-free, and the build gets it from
# bt-src/sysroot/usr/lib, where bt-src/build.sh leaves it. The player opens it
# with dlopen and bluealsa links it; without the file there is no AAC at all.
FDK="$BT_SYSROOT/usr/lib/libfdk-aac.so.2"
[ -f "$FDK" ] || { echo "install_tree: $FDK is missing -- run bt-src/build.sh first" >&2; exit 1; }
install -m 644 "$FDK" "$TREE/usr/lib/libfdk-aac.so.2"
```

### 11.5 `sonix-packer/v3_analog_packer.sh`

```sh
# libfdk-aac is the one non-free binary the image carries, so it is not in the
# tree either: bt-src/build.sh puts it in bt-src/sysroot/usr/lib and the packer
# copies it from there. Point BT_SYSROOT elsewhere when it lives elsewhere.
BT_SYSROOT="${BT_SYSROOT:-$SCRIPT_DIR/../bt-src/sysroot}"
```

проверка на хосте до первого `docker run` (там же, где проверяются `PLAYER_BIN`/`STOCK`):

```sh
[ -f "$BT_SYSROOT/usr/lib/libfdk-aac.so.2" ] || die "no libfdk-aac in $BT_SYSROOT:
     build bt-src first (bt-src/build.sh); the image needs Debian's non-free
     libfdk-aac-dev, and this repository does not carry the binary."
```

в docker-run, который выполняет `install_tree.sh` (функция `install_tree`, строка с
`fw bash tools/install_tree.sh` — заменить на прямой `docker run`):

```sh
docker run --rm -i -v "$SCRIPT_DIR:/packer" -v "$BT_SYSROOT:/bt-src-sysroot:ro" -w /packer \
	-e BUILD_STAMP="${BUILD_STAMP:-}" -e OS_VERSION="${OS_VERSION:-}" \
	-e BT_SYSROOT=/bt-src-sysroot "$DOCKER_IMAGE" bash tools/install_tree.sh
```

### 11.6 CI-шаг

```yaml
      - name: Licence audit
        # Every binary, kernel module and font in the tree, against licenses/README.md.
        # A file nobody has recorded is a file nobody has checked.
        run: python3 tools/license_audit.py
```

### 11.7 `licenses/README.md` (скелет)

```markdown
| artefact | where | licence | obligation |
|---|---|---|---|
| `libfdk-aac.so.2` | image, from `bt-src/sysroot` | Fraunhofer FDK AAC | notice in the image, source available, patent notice |
| `Literata-*.ttf` | `assets/BLAZE/.../fonts/ebook` | SIL OFL 1.1 | notice with the binary |
| `japanese{,-bold}.ttf` | `assets/BLAZE/.../fonts` | SIL OFL 1.1 (Noto Sans JP) | notice with the binary |
| `default.otf`, `bold.otf`, `arabic*.otf`, `thai*.otf` | `assets/BLAZE/.../fonts`, `sonix-player/assets/fonts` | MiSans, Xiaomi | say the software uses MiSans; do not redistribute the files on their own |
| `korean*.otf` | то же | SIL OFL 1.1 | notice |
| `libopenaptx.so.0` | `assets/V3ANALOG/usr/lib` | LGPL-2.1 | source available |
| `libldacBT_*.so.2` | `assets/V3ANALOG/usr/lib` | Apache-2.0 (AOSP libldac) | notice |
| `bluealsa`, `libasound_module_pcm_bluealsa.so` | `assets/V3ANALOG` | MIT | notice |
| `bluetoothd`, `libbluetooth.so.3` | `assets/V3ANALOG` | GPL-2.0 (BlueZ) | source available (patches in `bt-src/`) |
| `libcrypto.so.1.1`, `libssl.so.1.1` | `assets/V3ANALOG` | Apache-2.0 (OpenSSL) | notice |
| `assets/R1`, `assets/R3PII`, `kernel/xImage-*` | repo only | their own | not part of the V3 image |
```

## 12. Открытые вопросы (решать при реализации)

1. **MiSans, атрибуция.** Формулировка и место: ряд в Info (11.3) против строки в
   `docs/BUILD.md`. Соглашение требует «специально указать в программном обеспечении» — ряд в Info
   ближе к букве, но добавляет видимую строку в UI.
2. **MiSans, раздача файлов.** Соглашение запрещает «отдельно распространять» шрифт. У нас он
   лежит в публичном git как обычный файл и в `sonix-player/assets/fonts` для симулятора. Варианты:
   оставить (встраивание разрешено, репозиторий — не «отдельная продажа шрифта»), либо убрать из
   git и требовать локально. Решение владельца, в TODO зафиксировать выбранное.
3. **Область аудита.** `assets/R1`/`assets/R3PII` содержат свои Bookerly и `libfdk-aac`. Проверять
   только `V3ANALOG` + `BLAZE` (рекомендация) или всё дерево с пометкой «не входит в V3-образ».
4. **Расхождение с upstream.** `assets/BLAZE` — дерево апстрима. После merge туда вернутся
   Bookerly и Rodin, а `ebookfonts.c` — старые имена. Политика: чинить после merge по
   `docs/UPDATING.md`, аудит обязан это поймать.
5. **Имя файлов JP.** Выбрано `japanese.ttf` (Noto — TrueType). Расширение ничего не значит для
   FreeType, но имя `.otf` у TTF вводит в заблуждение; проверить, что `.local/fonts` пользователей
   с `japanese.otf` ничего не ломают (этот путь читается только для regular-лица, см.
   `ebookfonts_open`).

## 13. Definition of done

- [ ] `grep -rn "Bookerly\|Rodin" sonix-player/src assets/BLAZE` по V3-путям — пусто
      (кроме истории и R1/R3PII).
- [ ] `git ls-files` не содержит `libfdk-aac.so.2` в V3-пути.
- [ ] `python3 tools/license_audit.py` — зелёный; удаление любой записи из `licenses/README.md`
      его роняет.
- [ ] `sonix-packer/v3_analog_packer.sh` на чистом клоне без `bt-src/sysroot` падает с понятным
      текстом, с собранным — собирает образ и проходит `verify`.
- [ ] Экран Info показывает атрибуцию; строка не ломает вёрстку на 320 px.
- [ ] На устройстве: английская книга в 4 начертаниях, японская книга, отсутствие шрифтов →
      fallback и корректное сообщение.
- [ ] `docs/BUILD.md`, `docs/V3-ANALOG.md`, `docs/UPDATING.md`, `README.md` обновлены.

## 14. Оценка

Шрифты (файлы + код + локализация) — 2–3 ч, лицензии (тексты + копия в образ) — 1 ч,
пакер + `install_tree` — 30 мин, аудит + CI — 1–2 ч, документация — 1 ч, проверка на устройстве —
1 ч. Итого около одного рабочего дня. faad2 — отдельно, 1–2 дня, только по решению из раздела 8.

---

## 15. Зависимость от Blaze-форка (проверено 2026-11-24)

Состояние: `origin` — наш репозиторий, `upstream` — `Jepl4r/sonix-player`; blaze-форка в
ремоутах нет, обновлений из него не тянем. Но `blaze-port` — предок: merge-коммит `218bfee`
«Merge upstream/main into blaze-port» в истории, 44 коммита carmine-bin, 220 файлов им тронуто;
из его кода в дереве остались 14 файлов (профайлеры `frame/frameprof.c`, `profiler.c`,
`timerprof.c`, `eventprof.c`, `glyphwarm.c`, заголовки `libjpeg9/`).

`assets/BLAZE` — обязательный вход сборки: 44 файла, 27.2 MB (≈37 % распакованного дерева образа).
Шрифты 24.4 MB, 12 языковых файлов 0.8 MB, `gui/` 0.4 MB, `components/` 1.1 MB,
`bin/` 0.3 MB, BT-конфиги 0.2 MB. В апстриме 1.3.0 этого дерева нет вообще — только `R1` и
`R3PII`, поэтому мержи его не обновляют: шрифты, языки, картинки, базы Gearboy и BlueZ-userland
живут вручную.

- [ ] **15.1** `v3_analog_packer.sh`, блок `verify`: добавить проверку, что в собранном
      `system-info.json` **нет** `ota-repo`. Сейчас проверяется только `device-name`; в
      `assets/BLAZE/.../system-info.json` лежит `"ota-repo": "carmine-bin/sonix-player-tempotec-v3-blaze"`,
      и если порядок оверлеев сломается (BLAZE поверх V3ANALOG), плеер начнёт обновляться из
      чужого репозитория и скачает Blaze-прошивку `v3_analog_2025.upt`.
- [ ] **15.2** `tools/asset_overlap.py` (или шаг в `verify`): печатать карту перекрытий
      BLAZE→V3ANALOG. Сегодня их 6: идентичны `etc/dbus-1/system.d/bluez5-agent.conf`,
      `usr/bin/bluealsa`, `usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so`; перекрыты
      `etc/dbus-1/system.d/sonix-player.conf`, `usr/bin/sonix_player.sh`,
      `usr/resource/sonix/components/system-info.json`. Новое перекрытие должно быть осознанным.
- [ ] **15.3** Заголовок-маркер в `assets/BLAZE/` (например, `BLAZE-README.md`), чтобы следующий
      читатель знал: дерево — снимок чужого порта, апстрим его не двигает, всё в нём наше.
- [ ] **15.4** Политика расхождения с этой веткой (шрифты, локализация), когда пойдёт
      реализация разделов 1–3: `docs/UPDATING.md` должен называть конкретные файлы, которые после
      merge проверяются (`fonts/`, `language/`, `gui/`, `usr/{bin,lib}`).
