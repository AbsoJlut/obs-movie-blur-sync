# AbsoJlut AutoBlur Raspberry OBS DLL plugin — v0.1 source

Это исходники настоящего OBS-плагина DLL для AbsoJlut AutoBlur Raspberry.

## Что делает DLL

- Добавляет dock-панель `AbsoJlut AutoBlur Raspberry` прямо в OBS.
- Запускает локальный bridge-сервер `http://127.0.0.1:8799/push` внутри OBS-процесса.
- Получает `video.currentTime` из Raspberry/ReYohoho через Tampermonkey userscript.
- Включает/выключает выбранный OBS-фильтр блюра напрямую через libobs, без obs-websocket и без OCR.
- Поддерживает интервалы:

```text
00:12-00:28
00:38:44-00:39:10
01:04:10-01:05:00
```

## Важно

Архив содержит исходники, а не готовую DLL. Сборка Windows DLL требует OBS SDK/OBS plugin template, Visual Studio 2022, CMake и Qt той версии, с которой собран OBS.

## Как собрать через OBS plugin template

Самый надёжный путь:

1. Создать новый репозиторий из официального `obsproject/obs-plugintemplate`.
2. Заменить/добавить файлы из этого архива:
   - `src/plugin-main.cpp`
   - `CMakeLists.txt` или перенести зависимости `Qt6::Core`, `Qt6::Widgets`, `Qt6::Network` в CMake template.
   - `scripts/video_time_bridge.user.js`
3. Собрать на Windows через Visual Studio 2022/CMake или GitHub Actions template.
4. После сборки положить:

```text
absojlut-autoblur-raspberry.dll
```

в папку OBS-плагинов, обычно:

```text
C:\Program Files\obs-studio\obs-plugins\64bit\
```

Tampermonkey-скрипт `scripts/video_time_bridge.user.js` поставить в браузере, как в v2/v3.

## Почему пока исходники

В этой среде нет Windows OBS SDK/Visual Studio/Qt сборочного окружения, поэтому я не могу честно выдать проверенную `.dll`. Этот архив — основа для сборки DLL на Windows или в GitHub Actions.

