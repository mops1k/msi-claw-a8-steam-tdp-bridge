# План: прослойка Steam→TDP для MSI Claw A8 (без Decky)

Статусы: `[ ]` не начато, `[~]` в работе, `[x]` готово, `[!]` блокер.

## 1. Подтверждённые факты

- Устройство: `MSI / MS-1T8K / Claw A8 BZ2EM`, Ryzen Z2 Extreme, CachyOS `deckify`, Steam запущен с `-steamdeck`.
- Штатный UI Steam дёргает D-Bus `com.steampowered.SteamOSManager1.TdpLimit1` (подтверждено строками в `steamclient.so` и XML интерфейса).
- Сейчас `TdpLimit1` **не экспортируется** (`steamosctl get-tdp-limit` → `UnknownInterface`), поэтому Steam пишет `TDP limiting is disabled` и слайдера нет даже на профиле Performance.
- `steamos-manager` знает устройство (`Manager2.DeviceModel = "claw"/"Claw A8 BZ2EM"`) и берёт конфиг `/usr/share/steamos-manager/devices/msi-claw-amd.toml`:
  `[tdp_limit] method="firmware_attribute"`, `performance_profile="performance"`.
  Интерфейс регистрируется только при `is_active()` (профиль == performance).
- Реальный регулятор: `/sys/class/firmware-attributes/msi-wmi-platform/attributes/ppt_pl1_spl|ppt_pl2_sppt|ppt_pl3_fppt/current_value`
  (диапазоны SPL 7–35, SPPT 9–40, FPPT 10–48 Вт).
- Официальная точка расширения — **remote interface**: демон на **системной шине** + TOML в `/etc/steamos-manager/remotes.d/`
  (секция `[TdpLimit1]`, ключи `bus_name`, `object_path`). `TdpLimit1` в списке разрешённых.
  Remote **не может** переопределить локальную реализацию → нужен override device-TOML.
- `msi-wmi-platform` предоставляет только `low-power/balanced/balanced-performance/performance` (без `custom`).
- Инструментарий: `gcc`, `meson`, `ninja`, `gio-2.0 2.88` (заголовки есть), `glib2`. Rust/cargo нет.
- `deck` в группе `wheel`; sudo требует пароль (для спайка/интеграции нужен root).

## 2. Архитектура

```
Steam QAM (TdpLimit)
   │ session bus
   ▼
com.steampowered.SteamOSManager1.TdpLimit1   (steamos-manager user daemon)
   │ relaying remote interface (system bus)
   ▼
com.steampowered.TdpBridge  →  steam-tdp-bridge (root, C+GIO)
   │ sysfs write
   ▼
/sys/class/firmware-attributes/msi-wmi-platform/attributes/ppt_*/current_value
```

Ключевой приём: device-TOML переопределяется (bind-mount) так, чтобы убрать локальный `[tdp_limit]`,
тогда `steamos-manager` создаёт `RemoteInterfaceLimitManager` и «дырка» честно заполняется нашим демоном.

## 3. Компоненты и файлы

```
plan.md
source/
  bridge.h          # общие структуры/прототипы
  main.c            # GMainLoop, own name, регистрация объекта
  sysfs.c           # чтение min/max, запись current_value, поиск каталогов, retry
  config.c          # GKeyFile-конфиг
  tdp.c             # высокоуровневый get/set, маппинг PL, profile policy, state
meson.build
data/
  msi-claw-a8-steam-tdp-bridge.service
  msi-claw-a8-steam-tdp-bridge-devicetoml.service  # bind-mount override device TOML
  msi-claw-amd.toml                     # копия device-TOML без [tdp_limit]
  msi-claw-a8-steam-tdp-bridge.remotes.toml        # remotes.d
  com.steampowered.TdpBridge.conf       # dbus policy
  config.ini
install.sh
uninstall.sh
PKGBUILD
README.md
```

Устанавливается: `/usr/bin/msi-claw-a8-steam-tdp-bridge`, юниты в `/usr/lib/systemd/system/`,
remotes в `/etc/steamos-manager/remotes.d/`, override device-TOML и конфиг в
`/etc/msi-claw-a8-steam-tdp-bridge/`.

## 4. Поведение

- D-Bus: `com.steampowered.TdpBridge`, объект `/com/steampowered/TdpBridge`,
  интерфейс `com.steampowered.SteamOSManager1.TdpLimit1`:
  - `TdpLimit` (u32, rw) — get/set + `PropertiesChanged`;
  - `TdpLimitMin`/`TdpLimitMax` (u32, r) — из `ppt_pl1_spl/{min,max}_value` (7/35), с фолбэком.
- Маппинг как в steamos-manager: `SPL=limit`, `SPPT=max(limit,sppt_min)`, `FPPT=max(limit,fppt_min)`, с клампом.
- Свой custom-режим: пишем PPT напрямую. `profile_policy` в конфиге:
  - `auto` (по умолчанию): пишем как есть; при ошибке — ставим `msi-wmi-platform=performance`, пишем, запоминаем;
  - `always`: держать `performance`, пока задан custom-TDP;
  - `never`: писать как есть.
- Персистентность: последнее значение в `/var/lib/msi-claw-a8-steam-tdp-bridge/tdp`; при старте (`restore_last=true`) применяем.

## 5. Этапы

- [x] **Этап 0. plan.md** — записан, обновляется по ходу.
- [x] **Этап 1. Спайк sysfs** — запись `ppt_*` работает **и без смены профиля**: `--apply 15` → readback 15;
      `7` → spl=7/sppt=9/fppt=10; запись разрешена уже на `balanced-performance`.
- [x] **Этап 2. Спайк remote** — remote TdpLimit1 **регистрируется**, но без override использовал локальный
      firmware-менеджер (gated профилем) → `TdpLimit=0`. С override device-TOML (убрать `[tdp_limit]`)
      менеджер становится RemoteInterface и проксирует в наш демон → `steamosctl get-tdp-limit = 15`.
      **Вывод: override обязателен.**
- [x] **Этап 3. Демон** — C+GIO: интерфейс, свойства (+PropertiesChanged), sysfs, маппинг, конфиг, состояние.
- [x] **Этап 4. Интеграция** — dbus-policy, systemd units (`steam-tdp-bridge.service`,
      `steam-tdp-bridge-devicetoml.service`), bind-mount override (проверен через systemd).
- [x] **Этап 5. E2E** — слайдер в Steam QAM появился; `steamosctl` get/set и sysfs подтверждены.
- [x] **Этап 6. Поставка** — install.sh/uninstall.sh, PKGBUILD, README, `tools/verify-tdp.py`.
- [~] **Этап 7. Финальная проверка** — подтверждено применение лимита под нагрузкой (7 Вт → ~10 Вт,
      20 Вт → ~18 Вт), монитор профиля, sleep-хук, чистая сборка, `install.sh` идемпотентен.
      Осталась проверка реального suspend/resume и перезагрузки.
- [x] **Этап 8. Уточнения по железу** — TDP применяется только в профиле `performance`
      (`custom` драйвер не принимает) → `profile_policy=always` + монитор профиля;
      обработка `TdpLimit=0`; sleep-хук `steam-tdp-bridge` (`--restore` после пробуждения).

## 6. Риски и нюансы

- Нужен root для записи в sysfs → демон системный.
- bind-mount `/usr`-файла может отставать от обновлений `steamos-manager` (в README — как обновлять копию).
- Если будущий `steamos-manager` даст рабочую локальную реализацию — наш remote всё равно перекроет (осознанный выбор; есть uninstall).
- Диапазон слайдера 7–35 Вт (по SPL). FPPT до 48 В остаётся бустом; максимум настраиваемый.
- Переключение профиля из прослойки может рассинхронизировать UI профиля Steam; по умолчанию `auto` минимизирует переключения.

## 7. Журнал выполнения

- 2026-09-16: план записан.
- 2026-09-16: проект собран (meson/ninja), демон запущен как systemd-сервис, владеет `com.steampowered.TdpBridge`.
- 2026-09-16: спайк sysfs — запись `ppt_*` успешна без смены профиля; маппинг/клампы верны.
- 2026-09-16: спайк remote — интерфейс регистрируется, но локальный менеджер перекрывает; override device-TOML
  переключает на RemoteInterface, `steamosctl get/set-tdp-limit` работает (15/20/7/28, клампы 9/10).
- 2026-09-16: bind-mount закреплён юнитом `steam-tdp-bridge-devicetoml.service`.
- 2026-09-16: `enforce_profile` (по умолчанию `false`): монитор профиля больше не возвращает
  `performance` насильно — ручная смена профиля сохраняется. Проверено: после `set-tdp-limit 20`
  профиль `balanced` остаётся.
- 2026-09-16: фикс чёрного экрана в game mode: демон больше **не форсит профиль до старта Steam**
  (если состояние галочки из Steam не читается — профиль не трогаем), убран `Before=graphical-session.target`,
  восстановление значения вынесено в idle после захвата D-Bus имени (Type=dbus не задерживает сессию),
  таймауты helper снижены. Проверено: без helper/Steam профиль остаётся `balanced`, с Steam — `performance`.
- 2026-09-16: UI-синхронизация профиля: смена профиля делается через сам Steam
  (`SteamClient.Settings.SetSetting` через webhelper debug, helper `steam-set-profile.py`),
  лимиты — в sysfs. Статические id модулей не хардкодятся: helper находит нужный
  protobuf-класс/стор сканом webpack по именам полей.
- 2026-09-16: **чтение галочки TDP** тоже переведено на Steam (`SteamClient.Settings` в памяти;
  настройки `steamos_tdp_limit_enabled`/`steamos_tdp_limit`), с кэшем ~1с и фолбэком на `config.vdf`.
  Проверен цикл on→performance / off→возврат / on→performance.
- 2026-09-16: выяснено, что Steam не передаёт состояние галочки через D-Bus: при снятой галочке
  шлёт `TdpLimit = TdpLimitMax` (35), а состояние хранит в `config/config.vdf`
  (`SteamOS → TDPLimitEnabled`, ключ `121675982` — стабильный per-install id, не appid).
  Добавлен `source/steam.c`: чтение галочки (автоопределение `config.vdf`), `honor_steam_toggle`.
  Логика: галочка вкл → форсить/удерживать `performance`; галочка выкл → вернуть прежний профиль
  и не трогать. Проверены все сценарии (вкл/выкл/ручная смена), фолбэк `enforce_profile`.
- 2026-09-16: выяснилось, что EC применяет `ppt_*` только в профиле `performance` (в `balanced-performance`
  запись принимается, но не действует; `custom` драйвер отвергает). `profile_policy=always` + GFileMonitor
  на профиль; проверка нагрузкой: 7 Вт → PPT ~10 Вт/RAPL ~10.8, 20 Вт → ~18 Вт.
- 2026-09-16: sleep-хук `/usr/lib/systemd/system-sleep/steam-tdp-bridge` (`--restore` on post);
  проверено на симуляции сброса EC (15 → 25).
- 2026-09-16: добавлена обработка `TdpLimit = 0`: интерфейс `TdpLimit1` требует значение в `[min,max]`,
  поэтому 0 никогда не публикуется. При старте: сохранённое → текущее в sysfs (если >0) → `default_limit`
  (по умолчанию 15). `--apply 0` — no-op (не меняет текущее). Проверено: геттер/steamosctl не отдают 0.
- 2026-09-16: **исправлен чёрный экран в game mode.** Причина — CDP-хелпер
  (`steam-set-profile.py`: скан всех webpack-модулей + `SetSetting`) вызывался в горячем пути
  при старте Steam. Решения: чтение галочки снова из `config.vdf` (без CDP в горячем пути);
  `apply_profile` выходит, если профиль уже равен целевому (не дёргает Steam); `steam_set_ui_profile`
  асинхронный (fire-and-forget `g_spawn_async`); re-assert профиля при дрейфе — только через sysfs;
  добавлен `bridge_watch_config` (GFileMonitor на `config.vdf`, реагирует только на смену галочки).
  Проверено пользователем: game mode стартует, смена TDP работает, при включённой галочке
  UI профиля показывает `performance`. Снятие галочки возвращает прежний профиль (проверено
  пользователем) — UI профиля снова свободен.
- 2026-09-16: suspend/resume в game mode — проверено пользователем визуально, ок
  (лимит и профиль восстанавливаются; хук `--restore` на post).
- 2026-09-16: добавлен `tools/verify-suspend.py` — «жёсткая» проверка: ставит лимит через
  демон (D-Bus), снимает `profile` + `ppt_pl1_spl/pl2/pl3` + `TdpLimit` демона, уходит в
  `systemctl suspend`, после resume ждёт восстановление (поллинг до таймаута) и сравнивает.
  Прогон `--limit 18`: `PASS` (before/after: profile=performance, spl=sppt=fppt=18)
- 2026-09-16: **финализация обвязки.** README переписан под текущее поведение (галочка из
  `config.vdf`, guard `apply_profile`, helper только при смене профиля, монитор `config.vdf`,
  проверка suspend/resume). Добавлен `LICENSE` (MIT).
- 2026-09-16: PKGBUILD — сборка в `_build/` (не конфликтует с `build/` от `install.sh`),
  установка `LICENSE`; проверена реальная сборка `makepkg`. Исходники переименованы
  `src/` → `source/`: makepkg использует `$startdir/src` как `$srcdir`, и `makepkg -C`
  удалял бы исходники (проверено/исправлено).
- 2026-09-16: `uninstall.sh` — защитный `umount` перед удалением файлов + удаление `licenses`;
  проверено: bind-mount снят, файлы удалены, сток возвращён (`[tdp_limit]` на месте, штатный
  `TdpLimit1` 18/7/35), повторная установка проходит. `install.sh` — установка `LICENSE` и
  `chown` каталога сборки обратно вызывающему пользователю.
- 2026-09-16: **публикация на GitHub** `mops1k/msi-claw-a8-steam-tdp-bridge`. README.md (EN) +
  README.ru.md (RU): что это, зачем, как работает, установка, использование, конфиг, диагностика,
  инструменты проверки, удаление. `.github/workflows/release.yml` — ручной запуск
  (`workflow_dispatch`, выбор `patch`/`minor`/`major` + `prerelease`): бампает версию
  (`tools/bump-version.sh` → `pkgver`/`pkgrel` в PKGBUILD + `version` в meson.build),
  коммитит и тегирует, собирает Arch-пакет и отдельный бинарник в контейнере `archlinux`,
  создаёт GitHub-релиз с артефактами. Репозиторий инициализирован, запушен по SSH
  (ветка `main`).
- 2026-09-16: **полный ренейм** `steam-tdp-bridge` → `msi-claw-a8-steam-tdp-bridge`
  (по имени репозитория): пакет (`pkgname`), бинарник, systemd-юниты, sleep-хук, helper,
  каталоги `/etc`, `/var/lib`, `/usr/lib`, `remotes.d`, артефакты релиза. D-Bus-имена
  (`com.steampowered.TdpBridge`, `com.steampowered.SteamOSManager1.TdpLimit1`) не менялись —
  это контракт совместимости со SteamOS.
- 2026-09-16: фикс CI: в job `release` нет checkout, поэтому `gh` не находил репозиторий
  («failed to run git: not a git repository») — добавлен `--repo "${{ github.repository }}"`.
  Непринятый тег `v0.1.1` (от упавшего прогона) удалён, версия откачена к `0.1.0`.
