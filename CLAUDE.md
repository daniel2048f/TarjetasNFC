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

| Bus            | Pins (SDA/SCL) | Device            |
| -------------- | -------------- | ----------------- |
| Wire (I2C 0)   | 21 / 22        | PN532 NFC reader  |
| busRtc (I2C 1) | 19 / 18        | SD3078 RTC module |

## Architecture

All logic lives in [src/main.cpp](src/main.cpp) (~475 lines). There are no header files or split modules.

**Storage layers:**

- **NVS (Preferences)**: UID→name registry, state flags (`esperandoTarjeta`, `modoEliminar`), RTC sync signature, last-cleanup date.
- **LittleFS**: Append-only log file. Each line is `timestamp|uid|name`. Capped at 300 displayed entries; file is streamed to avoid loading it fully into RAM.

**Web server flow (async polling pattern):**

1. User triggers an action (register / delete) via the browser.
2. ESP32 sets a state flag and waits for a card tap.
3. Browser polls `GET /status` every 700 ms.
4. On card tap, result is stored in NVS; server responds to the next poll with a redirect to `/done` or `/deleted`.

**Auto-cleanup:** At midnight on the 15th and last day of each month, all log entries are deleted (user registry is preserved). An NVS key prevents the cleanup from running more than once per day.

**RTC sync:** On first boot after a new firmware upload, the RTC is set to the compile-time `__DATE__`/`__TIME__`. Detection uses a signature stored in NVS.

## Key Functions

| Function                      | Purpose                                                |
| ----------------------------- | ------------------------------------------------------ |
| `leerUidUnaVez()`             | Returns a card UID only once per placement (debounced) |
| `uidAHex()`                   | Converts byte array UID to hex string                  |
| `rtcLeer()` / `rtcEscribir()` | Raw I2C register access for SD3078                     |
| `rtcAjustarHora()`            | Sets RTC time using BCD encoding                       |
| `rtcSincronizarSiNecesario()` | Auto-syncs RTC to compile time on new firmware         |
| `obtenerTimestamp()`          | Returns `DD/MM/YYYY HH:MM:SS` string from RTC          |
| `logAgregar()`                | Appends a `timestamp\|uid\|name` line to LittleFS      |
| `logParsear()`                | Parses a log line; skips malformed entries             |
| `logHtml()` / `logTexto()`    | Renders logs as HTML table or plain text               |

## Dependencies

Declared in [platformio.ini](platformio.ini):

- `adafruit/Adafruit PN532 @ ^1.2.4`

All other libraries (`WiFi`, `WebServer`, `Preferences`, `Wire`, `LittleFS`) are part of the ESP32 Arduino framework and require no extra declaration.

## Convenciones

- Todo el código en español (variables, funciones, comentarios)
- Un solo archivo: `src/main.cpp`, no crear módulos separados salvo que se pida
- Nombrar variables en camelCase
- Antes de cualquier cambio, verificar que compile con `platformio run`
