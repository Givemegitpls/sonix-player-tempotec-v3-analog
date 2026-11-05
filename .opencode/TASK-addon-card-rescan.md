# Задача: аддоны не возвращаются после извлечения/вставки карты — ЗАКРЫТА

Статус: **сделано и проверено на устройстве** 2026-10-31, образ `311020260234`.
Дерево: `/home/alex/Projects/own/tempotec-v3-main-wt`, плеер в `sonix-player/`.
Коммит: `1441c16 addons rescan fix` (на момент записи).

Ниже — что было, что сделано, чем доказано и что осталось. Разбор исходного дефекта
сохранён, потому что он объясняет форму правки.

## Симптом (как было)

Вынуть и вставить карту — тайлы аддонов в Streaming и More исчезают до перезагрузки.
Не «тайл есть, а приложение не стартует», а именно тайлов нет.

## Причина

Три дыры, все подтвердились:

1. **H1, ранний выход из attach.** `card_databases_attach()` начиналась с
   `if (card_attached) return;`. Потерянный `remove@` (или быстрый перетык) оставлял
   `card_attached = true`, attach выходил до пересканирования, а супервизор молчал:
   `is_mounted && mount_is_alive` истинны, чинить нечего.
2. **H2, скан об уходе карты шёл до отмонтирования.** `addon_card_changed(NULL)` стоял в
   начале `card_databases_detach()`, а `unmount_sd()` — позже, в uevent-потоке. Результат
   скана зависел от планировщика.
3. **H3, неудачный скан стирал реестр.** `addon_scan()` сначала обнуляла
   `slot_count`/`slots`/`info`, и только потом делала `opendir()`; неудача — `return` без
   слова. Любой скан в неудачный момент уничтожал тайлы.

Сопутствующее, тоже исправлено: `host.cfg` указывал на стек `card_changed_on_ui()`;
`memset(slots, …)` терял `slots[i].art.data` (буфер иконки из `calloc()` в `icon_pack()`) —
утечка на каждом скане; `service_register_paths()` никогда не чистил `declared[]`.

## Что сделано

`sonix-player/src/addon/addon.c`:

- `addon_scan()`: проверка `cfg`/`sd_root_path` → `opendir()` → и **только потом** реестр.
  Неудача: `addon: cannot read <dir>: <errno>` и выход без изменения `slots`/`info`/
  `slot_count`/`slots_before`.
- Иконки предыдущего скана освобождаются (`free()`, парная к `calloc()`) перед `memset`.
- `host.cfg = &scanned_cfg`, `host.sd_root = scanned_root`, `scanned_cfg.sd_root_path`
  переставлен на `scanned_root`: ничего не указывает на стек вызывающего.
- Новое `addon_card_gone()` + `card_gone_on_ui()` (прототип в `addon_notify.h`): явный путь
  «карта ушла» — освобождает иконки, обнуляет реестр, дёргает `refresh_hook()` и **ничего
  не читает с ФС**. Поэтому его можно звать в detach, где монтирование ещё живо.
- `card_changed_on_ui()` зовёт `addon_scan(&scanned_cfg)` без копии на стеке.

`sonix-player/src/system/device/system.c`:

- `card_databases_attach()`: `addon_card_changed(root)` — первой строкой, до раннего выхода
  по `card_attached`; из конца функции убран.
- `card_databases_detach()`: `addon_card_gone()` + `service_forget_declared()`.
- Дубли `addon_card_changed(NULL)` после `unmount_sd()`/`force_unmount()` убраны: очистка
  в detach и ФС не трогает, гонки H2 больше нет.

`sonix-player/src/system/streaming/service.{c,h}`: `service_forget_declared()` — сброс
`declared[]`/`declared_count` при уходе карты.

## Грабли, найденные по дороге (важно для будущих правок)

Первая версия правки сама сломала скан: `scanned_cfg.sd_root_path` указывал на
`scanned_root`, а `addon_scan()` делала
`snprintf(scanned_root, sizeof(scanned_root), "%s", cfg->sd_root_path)` — копирование
буфера **самого в себя**. Это UB, glibc отдаёт пустую строку, пути аддонов становятся
`/Addons/…`, каждый манифест печатает `addon: … is unreadable`, реестр пуст (образ
`311020260224`, лог 02:31:16). Лечение: `snprintf` только когда источник — другой буфер.

## Доказательство (устройство)

Образ `311020260234`, лог `<card>/.local/sonix_player.log`:

```
02:35:45.550 storage: card databases reopened on /mnt/sd_0
02:35:45.595 addon: subsonic  'Subsonic' (streaming)
02:35:45.639 addon: gearboy  'Gearboy' (more)
02:35:45.670 addon: flappybird  'Flappy Bird' (more)
02:35:45.687 addon: dino  'Dino' (more)
02:35:45.717 addon: qobuz  'Qobuz' (streaming)
02:35:45.751 addon: tidal  'Tidal' (streaming)
02:35:45.776 addon: zvuk  'Zvuk' (streaming)
02:35:45.801 addon: yandex  'Yandex Music' (streaming)
02:35:45.817 addon: spotify  'Spotify' (streaming)
```

Тайлы вернулись без перезагрузки. Хост-сборки и `make addons-checks` зелёные
(`check-abi` glibc 2.16, `check-exports`).

## Что осталось

- **Негативный контроль не прогнан**: сценарий с переименованной/отсутствующей папкой
  `<card>/Addons` — ожидается одна строка `addon: cannot read …: No such file or directory`
  и тайлов нет. Код это делает, на устройстве не проверялось.
- **Подложенные аддоны без ребута не видны**: скан запускается только по смене карты и при
  загрузке. Отложено владельцем отдельной задачей (пересканировать при входе в Streaming
  или по кнопке).
- Прочие кэши путей на карте (`podcastcache`, `podcastsubs`, `cover`, `playlists`) при уходе
  карты по-прежнему держат старые строки; `service_forget_declared()` закрывает только
  реестр манифестов. Отдельная задача.
