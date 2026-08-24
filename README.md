# GooseOverlay

Нативный 64-битный плагин TeamSpeak 3, который показывает участников текущего голосового канала и подсвечивает говорящего поверх **Battles for Glory**.

Оверлей реализован отдельным прозрачным окном без внедрения кода в процесс игры. Поэтому игра должна работать в **оконном** или **безрамочном оконном** режиме. В эксклюзивном полноэкранном режиме Windows не может гарантировать показ такого окна поверх DirectX.

## Возможности

- список пользователей текущего канала TeamSpeak;
- зелёный индикатор активной речи, жёлтый — whisper;
- красный значок перечёркнутого микрофона у пользователей с выключенным микрофоном;
- значок перечёркнутого динамика у пользователей с отключённым входящим звуком;
- автоматический поиск окна Battles for Glory;
- скрытие, когда игра неактивна;
- клики и ввод проходят сквозь оверлей;
- настройка положения, размера, прозрачности и имён процессов через INI;
- встроенное окно настроек в интерфейсе дополнений TeamSpeak;
- команда и горячая клавиша для быстрого отключения.

## Сборка

Требуется Windows 10/11 и Visual Studio 2022 Build Tools с компонентом **Desktop development with C++**.

Клонируйте репозиторий вместе с официальным TeamSpeak SDK:

```powershell
git clone --recursive https://github.com/artesk/GooseOverlay.git
cd GooseOverlay
```

```powershell
.\build.ps1
```

Скрипт создаёт:

- `dist\package\plugins\bfg_overlay_win64.dll`;
- `dist\GooseOverlay.ts3_plugin` — установочный пакет TeamSpeak.

Также проект можно собрать CMake, если он установлен:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Установка и запуск

1. Дважды щёлкните `dist\GooseOverlay.ts3_plugin` и подтвердите установку.
2. Перезапустите TeamSpeak 3 и проверьте, что **GooseOverlay** включён в `Tools → Options → Addons`.
3. Подключитесь к голосовому каналу.
4. Запустите Battles for Glory в безрамочном или оконном режиме.

Команда `/goose status` в чате TeamSpeak покажет, найдено ли окно игры.

## Команды

```text
/goose toggle
/goose show
/goose hide
/goose opacity 40..255
/goose reload
/goose status
```

В настройках горячих клавиш TeamSpeak доступно действие `Toggle Battles for Glory overlay`.

## Настройка

Нажмите **Настроить** у `GooseOverlay` в списке дополнений TeamSpeak. В окне можно изменить положение панели, прозрачность, ширину, размер шрифта, высоту строк, число отображаемых пользователей и параметры поиска игры. Кнопка **Применить** обновляет оверлей без перезапуска.

Настройки сохраняются в пользовательском каталоге конфигурации TeamSpeak (`%APPDATA%\TS3Client\goose_overlay.ini` при стандартной установке), поэтому для их изменения не нужны права администратора. При обновлении параметры из старого `bfg_overlay.ini` переносятся автоматически.

Если `/goose status` сообщает `waiting for game`, добавьте фактическое имя EXE игры в `processes` либо исправьте `title_contains`, затем выполните `/goose reload`.

```ini
[game]
processes=BattlesForGlory.exe,Battles For Glory.exe,BFG.exe
title_contains=Battles for Glory
hide_when_unfocused=1

[overlay]
enabled=1
opacity=225
width=310
offset_x=24
offset_y=72
row_height=30
font_size=17
max_clients=16
```

## Ограничения

- Только TeamSpeak 3 Client API 26 и Windows x64.
- Эксклюзивный fullscreen не поддерживается; используйте borderless/windowed.
- Плагин не читает память игры и не внедряется в неё. Это сделано намеренно для совместимости и снижения риска конфликтов с античитом.

SDK загружен из официального репозитория [TeamSpeak Client Plugin SDK](https://github.com/teamspeak/ts3client-pluginsdk). Требования игры указывают Windows 10 x64 и DirectX 11: [Steam — Battles for Glory](https://store.steampowered.com/app/3646700/Battles_For_Glory/?l=russian).
