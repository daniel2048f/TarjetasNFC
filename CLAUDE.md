# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash Commands

```bash
# Compile only
platformio run --environment esp32dev

# Compile and upload to ESP32
platformio run --environment esp32dev --target upload

# Open serial monitor (115200 baud)
platformio run --environment esp32dev --target monitor

# Upload filesystem (LittleFS) — needed if adding/modifying filesystem assets
platformio run --environment esp32dev --target uploadfs

# Build + upload + monitor in one step
platformio run --environment esp32dev --target upload && platformio device monitor
```

## Project Overview

ESP32-based NFC access control system. Reads NFC card UIDs via a PN532 reader, maps UIDs to user names, logs all access events with timestamps, and exposes a local WiFi AP web interface for management.

**WiFi AP**: SSID `NFC`, password `12345678`, IP `192.168.4.1`

## Hardware

| Bus            | Pins (SDA/SCL) | Device                        |
| -------------- | -------------- | ----------------------------- |
| Wire (I2C 0)   | 21 / 22        | PN532 NFC reader + LCD (0x27) |
| busRtc (I2C 1) | 19 / 18        | SD3078 RTC module             |

LCD is a 20×4 `LiquidCrystal_I2C` at address `0x27`, sharing `Wire` with the PN532.

**SD3078 battery quirk**: The write-protection registers (`0x0F`, `0x10`) and the oscillator-enable bit (EOSC) reset to 0 when main power is cut. On every boot, `rtcReiniciarRegistros()` re-reads the time values and rewrites them with the correct control bits (EOSC=1, 24H mode) so the oscillator keeps running on battery. On a new firmware build, `rtcAjustarHora()` is called instead via `rtcSincronizarSiNecesario()`.

## Architecture

All logic lives in [src/main.cpp](src/main.cpp) (~650 lines). There are no header files or split modules.

**Storage layers:**

- **NVS (Preferences)** namespace `"nfc"`: UID→name registry, code registry, state mailboxes, RTC sync signature, last-cleanup date.
- **LittleFS**: Append-only log file `/logs.txt`. Each line is `timestamp|uid|nombre|codigo` (4 fields; parser accepts 3-field legacy entries). Capped at 300 displayed entries via two-pass streaming to avoid loading the file fully into RAM.

**NVS key schema:**

| Key           | Value                                           |
| ------------- | ----------------------------------------------- |
| `<uid>`       | User name string                                |
| `"k" + <uid>` | User code (1–6 alphanumeric chars)              |
| `buildID`     | `__DATE__ + __TIME__` — detects new firmware    |
| `lastClean`   | `YYYY-MM-DD` — prevents double auto-cleanup     |
| `lastReg`     | Polling mailbox: `OK\|uid\|name\|code\|ts`      |
| `lastDel`     | Polling mailbox: `DELETED\|…` or `NOT_FOUND\|…` |

**Web server routes:**

| Route           | Method | Purpose                                    |
| --------------- | ------ | ------------------------------------------ |
| `/`             | GET    | Home: status + live clock                  |
| `/register`     | GET    | Registration form (name + 1–6 char code)   |
| `/saveName`     | POST   | Sets `esperandoTarjeta = true`, waits card |
| `/deleteUser`   | GET    | Sets `modoEliminar = true`, waits card     |
| `/status`       | GET    | Polling endpoint (700 ms interval)         |
| `/done`         | GET    | Registration confirmation (from poll)      |
| `/deleted`      | GET    | Deletion confirmation (from poll)          |
| `/cancelar`     | GET    | Clears all pending state flags             |
| `/logs`         | GET    | Log HTML table (latest 300)                |
| `/downloadLogs` | GET    | Log as plain-text download                 |
| `/clearLogs`    | GET    | Deletes `/logs.txt`                        |
| `/time`         | GET    | Current timestamp (used by live clock JS)  |

**Async polling pattern:**

1. User triggers register/delete → ESP32 sets a state flag and returns an HTML page.
2. Browser polls `GET /status` every 700 ms.
3. `loop()` detects a card tap and writes the result to the NVS mailbox (`lastReg` / `lastDel`).
4. Next `/status` poll reads the mailbox, clears it, and responds with the result → browser redirects to `/done` or `/deleted`.

**LCD non-blocking timer:** `tiempoLcdHasta` (ms) + `lcdPost` enum control display: after showing a name for 3–4 s, `loop()` reverts the screen to `"Esperando tarjeta..."` (idle) or `"Listo"` without blocking.

**Auto-cleanup:** Checked every 60 s; runs at hour 00:xx on the 15th and last day of each month. Deletes `/logs.txt` only — NVS registry is preserved. `lastClean` NVS key prevents double-execution on the same day.

**RTC sync:** On first boot after a new firmware upload (`buildID` mismatch), the RTC is set to compile-time `__DATE__`/`__TIME__`. On subsequent boots the time is left intact but control registers are restored. Timestamp format: `YYYY-MM-DD HH:MM:SS`.

## Key Functions

| Function                      | Purpose                                                         |
| ----------------------------- | --------------------------------------------------------------- |
| `leerUidUnaVez()`             | Returns a card UID only once per placement (debounced)          |
| `uidAHex()`                   | Converts byte array UID to uppercase hex string                 |
| `lcdMostrar()`                | Clears LCD and prints up to 4 lines (truncates at 20 chars)     |
| `lcdMostrarNombre()`          | Word-splits a full name across lines; code right-aligned row 4  |
| `rtcLeer()` / `rtcEscribir()` | Raw I2C register access for SD3078                              |
| `rtcHabilitarEscritura()`     | 3-step sequence to unlock SD3078 time registers                 |
| `rtcAjustarHora()`            | Sets RTC time using BCD encoding                                |
| `rtcReiniciarRegistros()`     | Restores control bits (EOSC, 24H) without changing the time     |
| `rtcSincronizarSiNecesario()` | Auto-syncs RTC to compile time on new firmware                  |
| `parsearCompilacion()`        | Parses `__DATE__`/`__TIME__` macros into numeric fields         |
| `obtenerTimestamp()`          | Returns `YYYY-MM-DD HH:MM:SS` string from RTC (validates range) |
| `logAgregar()`                | Appends a `timestamp\|uid\|nombre\|codigo` line to LittleFS     |
| `logParsear()`                | Parses a log line; handles 3- and 4-field formats               |
| `logHtml()` / `logTexto()`    | Renders logs as HTML table or plain-text download               |
| `autoLimpiarLogs()`           | Bi-monthly log auto-cleanup (called from `loop()`)              |
| `pagina()`                    | Wraps HTML body with doctype, viewport meta, and shared styles  |

## Dependencies

Declared in [platformio.ini](platformio.ini):

- `adafruit/Adafruit PN532 @ ^1.2.4`
- `marcoschwartz/LiquidCrystal_I2C @ ^1.1.4`

All other libraries (`WiFi`, `WebServer`, `Preferences`, `Wire`, `LittleFS`) are part of the ESP32 Arduino framework and require no extra declaration.

## Convenciones

- Todo el código en español (variables, funciones, comentarios)
- Un solo archivo: `src/main.cpp`, no crear módulos separados salvo que se pida
- Nombrar variables en camelCase
- Antes de cualquier cambio, verificar que compile con `platformio run`
