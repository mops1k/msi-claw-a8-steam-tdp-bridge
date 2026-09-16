# steam-tdp-bridge

[![Release](https://img.shields.io/github/v/release/mops1k/msi-claw-a8-steam-tdp-bridge)](https://github.com/mops1k/msi-claw-a8-steam-tdp-bridge/releases)
[![License](https://img.shields.io/github/license/mops1k/msi-claw-a8-steam-tdp-bridge)](LICENSE)

TDP-прослойка для **MSI Claw A8 (Ryzen Z2 Extreme)** под SteamOS-совместимые
дистрибутивы (проверено на CachyOS `deckify`). Даёт менять TDP устройства через
**стандартное управление питанием Steam** в игровом режиме
(QAM → Производительность → TDP Limit) — без Decky-плагинов.

> English version: [README.md](README.md).

## Зачем это нужно

`steamos-manager` предоставляет Steam D-Bus-интерфейс
`com.steampowered.SteamOSManager1.TdpLimit1`. На этом устройстве в стоковом
device-TOML объявлен *локальный* метод TDP, который включается только в профиле
`performance`, поэтому интерфейс вообще не публикуется, а ползунок в QAM ничего
не делает. Вдобавок MSI EC применяет firmware-лимиты `ppt_*` только в профиле
`performance`.

Проект закрывает недостающую часть: небольшой root-демон реализует `TdpLimit1`
как *remote*-интерфейс для `steamos-manager`, пишет реальные лимиты в sysfs и
держит профиль в соответствии с галочкой TDP в Steam.

## Возможности

- Штатный ползунок TDP в QAM — без Decky и патчей Steam.
- Три лимита мощности, как в `steamos-manager`
  (`ppt_pl1_spl`, `ppt_pl2_sppt`, `ppt_pl3_fppt`).
- Следует галочке **TDP Limit** в Steam: форсит `performance` только пока она
  включена и возвращает профиль при выключении.
- Синхронизирует выбор профиля в списке QAM (через сам клиент Steam).
- Восстанавливает лимит и профиль после **сна/пробуждения**.
- Не мешает загрузке игрового режима (см. [про фикс чёрного экрана](#про-фикс-чёрного-экрана)).

## Как это работает

```
Steam QAM (TdpLimit)
   │ session bus
   ▼
com.steampowered.SteamOSManager1.TdpLimit1   (steamos-manager, user daemon)
   │ remote interface relay (system bus)
   ▼
com.steampowered.TdpBridge  →  steam-tdp-bridge (root, C+GIO)
   │  │ запись в sysfs
   │  ▼
   │ /sys/class/firmware-attributes/.../ppt_*/current_value
   │
   └─ профиль в UI: helper steam-set-profile.py
      (webhelper debug 127.0.0.1:8080; только когда профиль реально меняется)
```

- `steamos-manager` умеет отдавать `TdpLimit1` внешнему процессу (remote
  interface). Установщик bind-mount'ом подменяет `msi-claw-amd.toml` копией без
  секции `[tdp_limit]`, поэтому `steamos-manager` переключается на
  `RemoteInterfaceLimitManager` и ретранслирует вызовы нашему демону.
- Демон пишет `ppt_pl1_spl` (SPL = заданное значение), `ppt_pl2_sppt` и
  `ppt_pl3_fppt` (не ниже их минимумов).
- **EC применяет лимиты только в профиле `performance`.**
- Демон читает галочку **TDP Limit** напрямую из `config.vdf`
  (`SteamOS → TDPLimitEnabled`), **не обращаясь к Steam** — это безопасно на
  старте. `GFileMonitor` на файл ловит смену галочки, даже если Steam прислал
  лимит раньше, чем записал файл.
  - галочка **включена** → форсит и удерживает `profile_name` (`performance`);
  - галочка **снята** → возвращает прежний профиль и больше его не трогает.
- Чтобы список профилей в QAM совпадал с фактическим, демон меняет профиль
  **через сам Steam** (`SteamClient.Settings.SetSetting`, helper
  `steam-set-profile.py`), но только когда целевой профиль отличается от
  текущего. Лимиты всегда пишутся в sysfs.
- **Сон:** systemd sleep-хук (`--restore`) переприменяет последнее значение и
  профиль после пробуждения.

### Про фикс чёрного экрана

Обращения к webhelper Steam (скан webpack-модулей + `SetSetting`) в горячем пути
на старте игрового режима давали чёрный экран. Теперь взаимодействие вынесено из
этого пути: галочка читается из `config.vdf`, `apply_profile()` ничего не делает,
если профиль уже совпадает, helper запускается fire-and-forget (`g_spawn_async`),
а возврат профиля при дрейфе идёт только через sysfs.

## Требования

- `steamos-manager` с поддержкой remote interface (SteamOS 3.x / `deckify`).
- `glib2` / `gio-2.0`, `dbus`.
- MSI Claw A8 (`msi-wmi-platform` + platform-profile).

## Установка

Из релиза (рекомендуется):

```sh
sudo pacman -U steam-tdp-bridge-<версия>-1-x86_64.pkg.tar.zst
```

Из исходников:

```sh
sudo ./install.sh          # из репозитория
# или
makepkg -si                # из каталога с PKGBUILD
```

Оба способа ставят одни и те же файлы — не устанавливайте один поверх другого.
Перед сменой способа сделайте `sudo ./uninstall.sh`.

Затем выйти/войти в игровой режим (установщик также перезапускает
`steamos-manager` для активных сессий).

## Использование

После установки пользуйтесь штатными средствами Steam:

- **QAM → Производительность → TDP Limit** — двигайте слайдер; значение
  применяется сразу и сохраняется.
- Поле **Performance Profile** будет показывать `performance`, пока лимит
  включён, и снова станет свободным после его выключения.

Проверка из командной строки:

```sh
steam-tdp-bridge --get         # текущее значение (Вт)
steam-tdp-bridge --apply 15    # выставить 15 Вт
steamosctl get-tdp-limit       # через steamos-manager
```

## Конфигурация

`/etc/steam-tdp-bridge/config.ini`:

| ключ                | смысл                                                            |
|---------------------|------------------------------------------------------------------|
| `device`            | имя firmware-attributes устройства (`msi-wmi-platform`)          |
| `attribute_spl/sppt/fppt` | имена атрибутов PL1/PL2/PL3                                |
| `platform_profile`  | имя провайдера platform-profile (`msi-wmi-platform`)             |
| `profile_policy`    | `always` (по умолчанию) / `auto` / `never` — держать ли профиль  |
| `profile_name`      | профиль для TDP (по умолчанию `performance`)                     |
| `honor_steam_toggle`| `true` (по умолчанию) — следовать галочке «TDP Limit» из Steam   |
| `enforce_profile`   | фолбэк, когда галочку прочитать нельзя: `true` — удерживать `profile_name` |
| `steam_config`      | путь к `config.vdf`; пусто — автоопределение                    |
| `restore_last`      | восстанавливать последнее значение при старте                    |
| `default_limit`     | значение, если нет сохранённого и EC отдаёт `0`                  |
| `state_path`        | файл состояния (по умолчанию `/var/lib/steam-tdp-bridge/tdp`)    |

## Инструменты проверки

```sh
sudo tools/verify-tdp.py 7 20 28        # грузит все ядра и меряет PPT + RAPL
sudo python3 tools/verify-suspend.py --limit 18   # цикл сон/пробуждение
```

`verify-tdp.py` при лимите 7 Вт показывает ~9–10 Вт (буст SPPT/FPPT), при 20 Вт —
~18–20 Вт; `verify-suspend.py` печатает `PASS`, если после пробуждения
восстановлены профиль и все три лимита.

## Диагностика

```sh
systemctl status steam-tdp-bridge
busctl --system introspect com.steampowered.TdpBridge /com/steampowered/TdpBridge
busctl --user introspect com.steampowered.SteamOSManager1 /com/steampowered/SteamOSManager1 | grep Tdp
cat /sys/class/firmware-attributes/msi-wmi-platform/attributes/ppt_pl1_spl/current_value
```

Если `TdpLimit1` не появился у `steamos-manager`:

- проверьте, что bind-mount активен: `mount | grep msi-claw-amd.toml`;
- проверьте отсутствие `[tdp_limit]` в `/etc/steam-tdp-bridge/msi-claw-amd.toml`;
- перезапустите сессионный демон: `systemctl --user restart steamos-manager`.

## Обновление device-TOML

`/usr/share/steamos-manager/devices/msi-claw-amd.toml` может меняться с
обновлениями `steamos-manager`. После крупного апдейта сравните его с
`/etc/steam-tdp-bridge/msi-claw-amd.toml` и перенесите изменения (кроме
удалённого `[tdp_limit]`).

## Удаление

```sh
sudo ./uninstall.sh
```

Скрипт останавливает и удаляет сервисы (снимая bind-mount), файлы и `remotes.d`,
затем перезапускает `steamos-manager` — стоковый device-TOML возвращается.

## Релизы

Workflow [`Release`](.github/workflows/release.yml) запускается вручную
(`Actions → Release → Run workflow`). Он бампает `pkgver`/`pkgrel` в `PKGBUILD`
и версию в `meson.build`, коммитит и тегирует, собирает Arch-пакет и отдельный
бинарник в контейнере `archlinux` и публикует GitHub-релиз с обоими артефактами.

Требуется `Settings → Actions → General → Workflow permissions` =
**Read and write permissions** (workflow пушит коммит/тег версии и создаёт релиз).

## Лицензия

[MIT](LICENSE).
