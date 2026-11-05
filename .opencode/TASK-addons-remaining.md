# Задача: остаток сессии ses_e7732d89fffe89BaIwYiGQ8onS

Статус на **2026-10-31** (пауза по просьбе владельца). Дерево:
`/home/alex/Projects/own/tempotec-v3-main-wt`, плеер в `sonix-player/`.
Прошито и проверено на устройстве: образ `311020260234`.
Коммиты: `8c17d97 wip tidal, qobuz, zvuk, yandex, spotify`, `1441c16 addons rescan fix`.

Баг с извлечением карты — отдельный файл `.opencode/TASK-addon-card-rescan.md`, закрыт.

## Сводка по фазам

| фаза | состояние |
|---|---|
| A — ключи принадлежат аддонам | **сделано**, миграция логинов на месте |
| B — аддон Zvuk | **сделано** (системная часть + страница на `listrow`), живьём не проверено |
| C — аддон Yandex | **системная часть сделана**, страница на Graphite не перенесена; параллельно сделана страница device-flow входа |
| D — Spotify | **в работе**: toolchain и librespot на MIPS доказаны; движок (сессия/каталог/аудио) не закончен |
| E — документация | **сделано**, но раздел про Spotify-движок устарел после шага D |

## Фаза A — сделано

- `src/system/streaming/streamkeys.{c,h}`: только Podcast Index и Last.fm. Убраны статики
  `qobuz_*`/`tidal_*`/`yandex_*`, разбор секций, геттеры, `save_*`, карточные сессии
  (`sonix/streaming.json`, `sonix/streaming-session.json`). `tools/seal_streamkeys.py` и
  `build_gen/streamkeys_key.h` остались — нужны podcast/lastfm.
- Оставлена одна **временная** функция `streamkeys_legacy_get(section, key, out, size)`:
  аддон сам не может открыть запечатанный `.bin`, а пара `app_id/secret` бывает только там.
  Снять её и её двух вызывающих (`addons/qobuz/src/system/streaming/qobuzconf.c`,
  `addons/tidal/src/system/streaming/tidalconf.c`) через релиз.
- `addons/qobuz`: `qobuzconf.{c,h}` — `settings.ini` (app_id, app_secret, by_user, creds_at,
  migrated) и `session.ini` через `common/store`.
- `addons/tidal`: `tidalconf.{c,h}` — `settings.ini` через `store`, но `session.ini` со
  **своим** writer'ом: токен Tidal 800–900 символов, `STORE_VALUE_MAX` = 256. Расширять
  `store_t` отказались: +33 КБ в каждой копии стора в каждом аддоне.
- Миграция (обязательная, логины владельца): при первом запуске — `<card>/sonix/streaming.json`,
  затем поверх него `<card>/sonix/streaming-session.json`, затем, если пары нет,
  `streamkeys_legacy_get()`. Пишутся `settings.ini` + `session.ini`, ставится `migrated = 1`;
  старые файлы не удаляются. Разлогин не воскресает: `migrated` проверяется всегда.
- `addons/exports.base` перегенерирован `make addons-exports-base` (3545 имён, −167/+2:
  ушли `streamkeys_qobuz_*`/`tidal_*`/`save_*` и протухшие `qobuz_*`/`tidal_*`/`qobuzcache_*`,
  добавились `streamkeys_legacy_get`, `service_forget_declared`).
- `streaming-keys.ini.example`: только `[podcast]`, `[lastfm]`.
- `streamkeys_attach_card()` была пустой заглушкой, вызов из `main.c` и объявление удалены.

Проверено: `make host`, `addons-host`, `addons target` (docker), `addons-checks`;
`rg "streamkeys_(qobuz|tidal|yandex)" src addons` пуст. Миграция проверена в симуляторе на
подставных данных, не на живом логине.

## Фаза B — Zvuk, сделано

`sonix-player/addons/zvuk/`, `order 3`, `"service": true`, кэш `.local/zvuk-cache/`.

- Шесть модулей форка (`zvuk`, `zvukdl`, `zvukcache`, `zvukplay`, `zvuklist`, `zvuksync`)
  плюс `streamcover`, пути `src/system/streaming/` сохранены — внутренние `#include` не правились.
- `zvukservice.c` — таблица `sonix_addon_service_t` вместо `stream_provider_t`;
  `zvukconf.c` — настройки/сессия в `<card>/Addons/zvuk/` (`settings.ini`, `zvuk_session.ini`)
  через `store`, `[zvuk]` из `device_config.ini` только как fallback; `zvukcompat.c` — шим
  недостающих функций плеера; `streamerr` — свой шим.
- Токен: `<card>/zvuk_token.txt`, читает сам аддон; без токена — экран входа и **ни одного
  сетевого запроса**.
- `zvukpage.c` (2082 строки) — перенос страницы форка (2261) с Graphite на
  `listrow`/`settingsrow`/`listart`/`popover`/`spinner`; все размеры через `SC()`
  (`tools/scale_gui.py --check` чисто). Выброшено: очередь «играть следующим» (нет публичного
  API), `playbackplace`, `cardimport`, `streamerror`, badge на обложке.
- Проверено в симуляторе: `addon: zvuk opened`, `service: zvuk registered`, вход/выход/повторный
  вход, списки, «Saved on the card», история. Скриншоты: `/tmp/opencode/sd-zvuk/Screenshots/`
  (временный каталог).

Не проверено: живой вход и загрузка списков (токена нет), обложки в `listrow`
(`[streaming] list_art` по умолчанию выключен).

## Фаза C — Yandex, частично

`sonix-player/addons/yandex/`, `order 4`, `"service": true`, кэш `.local/yandex-cache/`.
36 файлов, ~10 280 строк.

- Восемь модулей форка (`yandex`, `yandexdl`, `yandexcache`, `yandexplay`, `yandexlist`,
  `yandexsaved`, `yandexsync`, `yandexwave`) с сохранением путей; `yandexservice.c` — таблица
  сервиса; `yandexconf.c` — ключи `client_id`/`client_secret`/`sign_key`/`client_header` в
  `<card>/Addons/yandex/settings.ini` (пусто = сервис выключен, ничего не захардкожено),
  сессия в `yandex_session.ini`; `yandexcompat.c` — шим.
- Принесён AES: `src/system/net/crypto.{c,h}` + `openssl_runtime.{c,h}` (в дереве плеера AES
  нет вообще), ~330 строк, `dlopen` libcrypto.
- Вход: импорт токена `<card>/yandex_token.txt` (BOM, кавычки, `token=`, целая строка
  `Authorization: OAuth …`) + один `/account/status`.
- Вместо страницы — экран состояния `yandexstate.c` на `settingsrow_*`.
- **Параллельной сессией** в `yandexstate.c` добавлен OAuth device-flow: `yandex_login_begin()`/
  `yandex_login_poll()` уже были в `yandex.c`, страницы не было. На паузе это не закоммичено
  (`M yandexstate.c`, `?? yandex/settings.ini.example`).

Не сделано: полная страница Yandex (`yandexpage.c`, 2693 строки, Graphite) — переписывание
разметки, как у Zvuk. Ключи Yandex владелец даст позже.

## Фаза D — Spotify, в работе

Решение владельца: **Rust, статический musl-бинарник** (схема rekkod: движок — отдельный
процесс, связь с аддоном по локальному каналу). Важно: Spotify **не требует пересборки
прошивки** — аддон и движок живут на карте; рефлеш нужен только при правках `src/` или
`exports.base`, а движку хватает существующего экспорта.

Сделано:

- `sonix-player/docker/Dockerfile`: rustup 1.28.2 → nightly-2026-10-01 (rustc 1.101.0-nightly)
  + компонент `rust-src`; `RUSTUP_HOME=/opt/rustup`, `CARGO_HOME=/opt/cargo` (сборка идёт как
  вызывающий пользователь с `HOME=/tmp`, тулчейн в `/root` был бы невидим). Плюс
  `musl.cc/mipsel-linux-musl-cross.tgz` (gcc 11.2.1, musl 1.2) в `/opt` и `libunwind.a`
  как копия `libgcc_eh.a`. Образ 1.36 → 2.17 GB. Старый `rustc/cargo` из apt убран.
- Рабочая команда (завёрнута в `addons/spotify/engine-mips/Makefile`):
  `RUSTFLAGS="-C linker=mipsel-linux-musl-gcc -C link-self-contained=no -C target-feature=-soft-float,+crt-static"`
  `cargo +nightly-2026-10-01 build --release --target mipsel-unknown-linux-musl -Z build-std=std,panic_abort`.
  Ловушки: `cargo +nightly` без даты докачивает свежий nightly без `rust-src`; без
  `-C link-self-contained=no` нет `crt1.o`; без `-soft-float` **тихо** ломается FP ABI
  (`strtod("2.5")` → 0 вместо 2.5).
- `addons/spotify/engine-mips/`: librespot 0.8 (core/audio/metadata/protocol, `default-features
  = false`, `rustls-tls-webpki-roots`), tokio, serde. 245 пакетов под таргет. Единственный крейт
  с C-кодом — `ring`. `Cargo.lock` **часть рецепта**: пин `vergen 9.0.6`, иначе build-script
  `librespot-core` не компилируется.
- Доказано на устройстве: статический `ELF 32-bit LSB, MIPS32r2, stripped` (593 КБ),
  `readelf -d` пуст; `probe` → `{"ok":true,"protocol":1,"librespot":"0.8.0","arch":"mipsel"}`;
  `--self-test` подтвердил `getrandom` (fallback на `/dev/urandom` — ядро 3.10.14),
  SHA-256 в C-половине `ring`, audio/protocol/metadata исполняются.
- `addons/spotify/` (аддон): экран на `settingsrow_*`, `store` для сессии, `service: true`
  с таблицей-заглушкой; сейчас транспорт — `dlopen` старой заглушки `engine/` (cdylib,
  без зависимостей). На устройстве поэтому пишет `engine not built`.

Не сделано:

- Шаг B (код есть, 2496 строк: `main.rs` 582, `stream.rs` 600, `catalog.rs` 543,
  `session.rs` 454, `http.rs` 317): сессия по username/password из карты, каталог
  (`status`/`login`/`logout`/`search`/`playlists`/`playlist_tracks`/`album_tracks`/`track`),
  аудио по `127.0.0.1:<port>/stream/<id>`, `--self-test`. **Собирается** (4.09 МБ, статический
  ELF MIPS32r2), и на устройстве проверено: `probe` → `{"ok":true,"protocol":1,"librespot":"0.8.0",
  "arch":"mipsel","http_port":18080}`, `status` → `signed_out`, `search` → `not_signed_in`,
  неизвестный op → `unknown op`, `/health` → `{"ok":true}`, `/stream/x` → `401`, SIGTERM
  удаляет сокет.
- **Найдено на железе:** устройство грузится с выключенным `lo` (`<LOOPBACK>` без `UP`, адреса
  `127.0.0.1` нет), поэтому bind даёт `EADDRNOTAVAIL` (`fatal: Address not available (os error 126)`).
  Проверено: после `ip link set lo up` всё работает. В движок добавлен `loopback_up()` (libc
  `SIOCGIFFLAGS`/`SIOCSIFFLAGS`), но **на паузе он ещё не компилируется** — не сходятся типы
  аргументов `ioctl` (`c_ulong`), сборка осталась красной. Это первое, что надо добить.
- Аддонная половина: заменить `dlopen`-заглушку на запуск процесса + сокет-клиент, экран входа,
  воспроизведение через `radio_play()`. Правок `src/` не требуется.
- Живой логин: нужен Premium-аккаунт; проверка структурная.
- `tools/probe.sh`/`probe-device.sh` устарели: движку теперь нужны `--http-port` и `--state-dir`,
  скрипты зовут его по-старому.
- Сборка крейтов в контейнере требует прокси: `docker run --network host` +
  `ALL_PROXY=socks5h://…@127.0.0.1:1080` (иначе `crates.io` отдаёт «transfer too slow»).

## Фаза E — документация, сделано (кроме Spotify)

- `docs/ADDONS.md`: раздел «Keys and sessions» (модель ключей, порядок миграции,
  `streamkeys_legacy_get()` как временная), разделы `zvuk`/`yandex`/`spotify`, таблица
  «The services, at a glance» (tidal 1, qobuz 2, zvuk 3, yandex 4, spotify 9), обновлён
  раздел про `listrow` (первая страница на нём — `zvukpage.c`).
- `.opencode/NOTES.md`: раздел про аддоны и фазу A, ограничения API, toolchain.
- **Устарело после шага D**: в разделе про Spotify написано, что MIPS-половины нет.
  Тулчейн и librespot на MIPS уже доказаны; переписать при следующем заходе.

## Ограничения API плеера, найденные при переносе

Одним списком (обойдено шимами, ни одна правка не делалась):

- нет `http_stream_open_range`/`http_stream_resume`/`http_stream_skip` — нет Range-докачки:
  обрыв = трек качается заново;
- нет `http_last_outcome`/`http_outcome_t` — отказ нельзя классифицировать;
- нет `playlist_replace_path` — очередь пересобирается через `playlist_load_paths`
  (теряется «appended», шаффл раздаёт «дальше» заново);
- нет `storage_pin_card` — проверка по inode вместо epoch;
- нет `sleeptimer_expiration_serial` — защита вырождается в 0;
- нет `device_state_note_metadata_available` — теги стрим-трека появляются позже;
- нет `mp4flac_payload`/`mp4flac_total_samples`/`mp4flac_patch_total_samples` — счётчик
  сэмплов не ведётся;
- нет `http_set_thread_timeout_cap` (no-op), `gui_post_or_free` (шим);
- в `sonix_addon_service_t` нет хуков «сохранить в библиотеку»/`track_saved`/`peek_metadata`,
  которые были в `stream_provider_t` — `yandexdl` собирается, но звать его пока некому;
- `track_id` в интерфейсе — `int`; нечисловой id даёт 0.

## Открытые вопросы владельцу

1. Ключи Yandex (`client_id`/`client_secret`/`sign_key`/`client_header`) — значения в
   `Addons/yandex/settings.ini` или вход только по токену.
2. Токен Zvuk для живой проверки страницы.
3. Premium-аккаунт Spotify для живой проверки движка (или приёмка остаётся структурной).
4. Нужен ли сброс кэшей сервисов (`<card>/.local/<service>-cache/`) при уходе карты — отдельной
   задачей или вместе с прочими кэшами путей.
