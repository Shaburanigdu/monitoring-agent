# Monitoring Agent (C++17)

Прототип системного агента мониторинга активности пользователя. Работает в фоне,
каждые 5 секунд собирает метрики, буферизует их и отправляет POST-запросом на
демонстрационный HTTP-сервер.

## Возможности

- Сбор метрик каждые 5 секунд:
  - имя процесса активного окна (`chrome.exe`, `Code.exe`, ...),
  - заголовок окна,
  - факт физической активности пользователя (мышь/клавиатура за последние 5 с).
- Потокобезопасная очередь до 100 записей.
- Отправка JSON POST с `Content-Type: application/json`.
- Retry-политика: при недоступности сервера данные остаются в буфере
  и отправляются при восстановлении соединения.
- Graceful shutdown по `Ctrl+C` / `SIGINT` / `SIGTERM`.
- Персистентность: при выходе неотправленные метрики сохраняются в `backup.json`.

## Технологии

- C++17
- CMake 3.16+
- [nlohmann/json](https://github.com/nlohmann/json) — сериализация JSON
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — HTTP-клиент
- Win32 API (`GetForegroundWindow`, `GetLastInputInfo`) — сбор метрик под Windows

Обе библиотеки подтягиваются автоматически через `FetchContent` при первой сборке
(нужен доступ в интернет).

## Требования

- Windows 10/11
- Visual Studio 2019/2022 (MSVC) с компонентом **Desktop development with C++**
- CMake 3.16+
- Python 3.8+ — только для запуска демо-сервера

## Сборка

```bash
git clone <your-repo-url>
cd monitoring-agent
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --config Release
```

Готовый exe: `build/Release/monitoring_agent.exe`.

### Настройка IntelliSense (VS Code)

Если VS Code не видит заголовки — убедитесь, что в `.vscode/settings.json` указано:

```json
{
    "C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json",
    "C_Cpp.default.cppStandard": "c++17",
    "C_Cpp.default.intelliSenseMode": "windows-msvc-x64"
}
```

## Запуск

### 1. Демо-сервер

```bash
py demo_server.py
# Demo server on http://127.0.0.1:8080
```

Сервер принимает POST-запросы и печатает полученный JSON в консоль.

### 2. Агент

В отдельном окне:

```powershell
.\build\Release\monitoring_agent.exe
```

Ожидаемый вывод:

```
Agent started. Press Ctrl+C to stop.
[sender] HTTP 200 body={"status":"ok"} records=1
[sender] HTTP 200 body={"status":"ok"} records=1
```

Остановка — `Ctrl+C`. Неотправленные метрики сохранятся в `backup.json`.

## Формат данных

POST на `http://127.0.0.1:8080/`, заголовок `Content-Type: application/json`:

```json
{
  "agent_id": "DESKTOP-XXXXX",
  "timestamp": 1792147320,
  "payload": [
    {
      "time": "2026-09-15 13:55:00",
      "process_name": "chrome.exe",
      "window_title": "ИНСАЙДЕР — Система мониторинга сотрудников",
      "user_active": true
    }
  ]
}
```

## Архитектура

```
[collector thread] --(SafeQueue)--> [sender thread] --HTTP--> [demo server]
       │                                    │
       │ GetForegroundWindow                │ retry + push_all_front
       │ GetLastInputInfo                   │
       │                                    ▼
       │                            backup.json (on shutdown)
```

- **Коллектор** раз в 5 секунд собирает метрики и кладёт в очередь.
- **Sender** забирает батч из очереди и отправляет POST. При ошибке возвращает
  батч в начало очереди и ждёт 5 секунд перед следующей попыткой.
- **Очередь** ограничена 100 записями (drop oldest).
- **Сигнал завершения** поднимает флаг `g_stop`, оба потока выходят из циклов,
  очередь сбрасывается в `backup.json`.

## Ограничения прототипа

- Сбор метрик реализован только под Windows. Для Linux есть заглушка
  `collect_metrics_stub()` — расширяется под X11 (`XGetInputFocus`,
  `XQueryTree`, `XFetchName`).
- Кейлоггер не пишется — фиксируется только факт ввода через `GetLastInputInfo`.
- HTTPS не используется (демо-сервер работает по HTTP).