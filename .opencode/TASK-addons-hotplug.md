# Задача: хотплаг аддонов (2026-11-02)

Ветка `feature/addons-system`. Владелец: «обновил аддон и запустил без перезагрузки — плеер
завис», «аддоны остаются активны, когда вынимается карта — тоже зависание», «хочется хотплаг
систему».

## Причина (класс, не случай)

1. Загруженный аддон никогда не выгружался (`addon.h`: «there is no deinit»), а `dlopen()`ed `.so`
   — маппинг файла на карте. Маппинг держит ФС: `umount` → EBUSY → `MNT_DETACH`, мёртвый
   superblock живёт в процессе, чтения с него блокируются в контроллере карты.
2. `card_gone_on_ui()` очищал слоты, не закрывая handle; на новом маунте `carry_loaded()` уже не
   сопоставлял слоты, и тот же путь dlopen'ился второй раз — две копии аддона, два набора
   статиков и воркеров.
3. Воркеры аддонов — detached `for(;;)` без флага остановки; их `gui_post`-колбэки приходят после
   исчезновения экрана.
4. Детч карты не закрывал thumbnails DB, sonixlink thumbs, `radio.db`, waveform-кэш;
   `audio_stop()` асинхронный, а unmount шёл сразу за ним.
5. Отписок не было ни для чего: `service_register`, `theme_register_refresh`,
   `device_state_add_prepare_cb`, back guard, `streamturn_register_abandon`, `power_*_in_standby`.

## Сделано

- **ABI**: `sonix_addon_stop_entry` → `sonix_addon_stop_t { api; bool (*stop)(void); }`.
  Нет хука — отказ на загрузке. `false` — маппинг остаётся, слот помечается `unload_refused`.
- **Загрузчик** (`src/addon/addon.c`): `addon_unload()` (уйти с экрана → `switcher_forget_screen`
  + `player_forget_screen` → `stop()` → удалить все экраны → `gui_post_drop_range` по диапазону
  библиотеки из `dl_iterate_phdr` → `dlclose`), `sonix_addon_own_screen()`, сравнение `stat`
  `addon.so`/`addon.json` при тапе (хотплаг без перезагрузки), `addon_card_gone()` через
  `gui_call()` (синхронно, 4 с).
- **GUI**: `gui_call()`, `gui_post_drop_range()`, `gui_note_thread()`; `switcher_forget_screen()`;
  `player_forget_screen()`; `keyboard.c` снимает клавиатуру по `LV_EVENT_DELETE` трея;
  `settingsrow.c` — слайдер из `built_sliders`.
- **Плеер**: `theme_unregister_refresh`, `service_unregister`, `device_state_remove_prepare_cb`,
  `power_forget_timer`, `streamturn_unregister_abandon`, `audio_wait_stopped`.
- **Детч карты** (`system.c`): синхронная выгрузка аддонов, `service_set_root("")`,
  `audio_wait_stopped(2000)`, закрытие cover/waveform/sonixlink/radio; на attach — `radio_store_open`.
- **Аддоны** (10): `stop()` с флагом остановки воркеров, ограниченным ожиданием счётчиком потоков
  и join там, где поток joinable; сохранение и удаление таймеров; отписки; регистрация экранов;
  spotify без `atexit` + убийство engine-процесса; yandex закрывает libssl/libcrypto.
- `addons/common/listrow.c`: `listrow_stop()`.
- `addons/exports.base` перегенерирован (+163 имени), `make addons-checks` чист.

## Проверка

- `make addons`, `make addons-host`, `make target`, `make addons-checks` — чисто.
- Симулятор (`SONIX_SD_ROOT=/tmp/opencode/sd`): подмена `addon.so` и повторный тап по плитке →
  `addon: dino changed on the card, reloading` / `stopped` / `opened`, без перезапуска.
- Устройство: заменить `Addon.so` на карте и открыть без ребута; выдернуть карту при открытом
  аддоне (ожидание в логе `addon: <id> stopped`, отсутствие `was busy; detached it lazily`,
  пустой `grep Addons /proc/<pid>/maps`); вставить карту и открыть плитку.

## Осталось / ограничения

- HTTP-запрос нельзя прервать извне: воркер внутри `http_request` переживает 2 с, тогда аддон
  возвращает false и остаётся замапленным (утечка, не краш). Полное лечение — abort для
  `http_request` (нет в `http.h`).
- `streamturn_wait_idle()`/`acquire()` без дедлайна: prefetch может ждать чужую загрузку.
- Сообщения вида `worker still inside` в лог — это точки, где нужен abort.

## Второй круг (2026-11-02, после первых проверок на устройстве)

- `storage_recheck_card()` выходил при `is_mounted && mount_is_alive`, не глядя на
  `card_attached`: потерянный insert-uevent оставлял плеер detached с живой картой навсегда.
  Теперь при живом маунте и `card_attached == false` идёт `card_databases_attach()`.
- `addon_card_gone()` возвращает bool (все ли библиотеки реально выгружены) — это видно в
  журнале карты.
- Диагностический журнал `storage_card_note()` → **/data/card-events.log** (шаги
  detach/attach/unmount/mount с таймстампами) был добавлен на время разбора и **удалён**: логи
  остаются на карте, `sonix_player.log`. Причина, по которой он понадобился, записана в
  `.opencode/NOTES.md`, «Лог на карте — плохой свидетель»: строки между смертью устройства и
  `logging_suspend_for_usb()` в карточный лог не попадают.
- `net/wifitransfer.c`: `prepare_web_root()` больше не пытается копировать 7 МБ веб-дерева в
  `/usr/data` (на этом rootfs такого раздела нет, а в rootfs 6.7 МБ свободно — `cp` умирал
  посередине): дерево раздаётся из `/usr/share/web`, куда всё равно кладутся страница и
  CGI-симлинки. Проверено: `curl http://<device>/` → 200, 95 КБ, thttpd на 80 и 4399, mDNS
  `sonix-transfer.local`.
