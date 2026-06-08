# Documentación técnica del código — Sistema NFC ESP32

Referencia para desarrolladores que lean, entiendan o modifiquen `src/main.cpp`.

---

## Contenido

1. [Arquitectura general](#arquitectura-general)
2. [Organización del código](#organización-del-código)
3. [Sistema de timers no bloqueantes con millis()](#sistema-de-timers-no-bloqueantes-con-millis)
4. [Máquina de estados del LED RGB](#máquina-de-estados-del-led-rgb)
5. [Sistema de cierre de día](#sistema-de-cierre-de-día)
6. [Bus I2C compartido PN532 + LCD y watchdog](#bus-i2c-compartido-pn532--lcd-y-watchdog)
7. [Anti-rebote y cooldown de tarjetas](#anti-rebote-y-cooldown-de-tarjetas)
8. [Por qué existe ARCHIVO_UIDS además de NVS](#por-qué-existe-archivo_uids-además-de-nvs)
9. [Lógica par/impar para entradas y salidas](#lógica-parimpar-para-entradas-y-salidas)
10. [Secuencia de recuperación del bus I2C](#secuencia-de-recuperación-del-bus-i2c)
11. [Consideraciones del DS3231 con RTClib](#consideraciones-del-ds3231-con-rtclib)
12. [Decisiones de diseño no obvias](#decisiones-de-diseño-no-obvias)

---

## Arquitectura general

Todo el código reside en un único archivo `src/main.cpp` (~1600 líneas). No hay módulos separados ni cabeceras propias. La única cabecera externa incluida directamente es `nvs.h` del ESP-IDF, necesaria para iterar las claves NVS.

### Capas de almacenamiento

```
┌──────────────────────────────────────────────────────┐
│ NVS (Preferences, namespace "nfc")                   │
│  <uid>      → nombre del usuario                     │
│  k<uid>     → código del usuario                     │
│  c<uid>     → contador entrada/salida (par/impar)    │
│  f<uid>     → fecha último acceso "YYYY-MM-DD"       │
│  buildID    → __DATE__ + __TIME__ (detección firmware)│
│  lastCierre → "YYYY-MM-DD" último cierre de día      │
│  lastEmail  → "YYYY-MM-DD" último email automático   │
│  emailPend  → bool: bandera de reporte pendiente sin enviar│
│  lastReg    → buzón registro: "OK|uid|nombre|codigo|ts"│
│  lastDel    → buzón borrado: "DELETED|..." / "NOT_FOUND|..."│
│  emailDest  → email de destino para reportes         │
│  ssidWifi   → SSID de la red WiFi con internet       │
│  claveWifi  → contraseña WiFi                        │
│  ntpOffset  → offset UTC en segundos (default -18000)│
└──────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────┐
│ LittleFS (flash, partición min_spiffs)               │
│  /logs.txt     → ts|uid|nombre|codigo                │
│  /entradas.txt → ts|uid|nombre|codigo|tipo           │
│  /uids.txt     → un UID hex por línea                │
└──────────────────────────────────────────────────────┘
```

### Flujo principal en `loop()`

```
loop()
 ├── servidor.handleClient()         ← atender peticiones HTTP
 ├── lcdActualizarParpadeo()         ← backlight protector LCD
 ├── ledActualizar()                 ← máquina de estados LED
 ├── Expiración tiempoLcdHasta       ← revertir LCD al estado real
 ├── Detección WiFi nueva → NTP      ← sincronizar reloj al conectar
 ├── verificarNtpPendiente()         ← aplicar respuesta NTP async
 ├── Reintento WiFi (cada 30 s)      ← reconectar si se perdió
 ├── Tareas periódicas (cada 60 s)   ← cerrarDia + autoEnviarEmail
 ├── Watchdog PN532 (cada 15 s)      ← verificar salud del chip NFC
 └── Lectura NFC (cada 300 ms)       ← leerUidUnaVez → procesar tarjeta
```

---

## Organización del código

| Sección | Líneas aprox. | Contenido |
|---|---|---|
| Includes y defines | 1–30 | Librerías, pines hardware, pines LED, archivos LittleFS |
| Variables globales | 31–125 | Estado del sistema, timers, estado LED, estado NFC |
| LED RGB — funciones | 126–185 | `ledAplicar`, `ledFijar`, `ledParpadear`, `ledActualizar`, `ledSetup` |
| LCD — funciones | 186–220 | `lcdWakeUp`, `lcdActualizarParpadeo`, `lcdMostrar`, `lcdMostrarNombre` |
| RTC DS3231 | 221–280 | Lectura, ajuste, sincronización con buildID y NTP |
| HTML / pagina() | 281–310 | Estilos CSS inline, wrapper HTML de página |
| Logs (LittleFS) | 311–370 | `logAgregar`, `logParsear`, `logHtml`, `logCsv` |
| Entradas/salidas | 371–470 | `entradaAgregar`, `entradaParsear`, `entradaHtml`, `entradaCsv` |
| Gestión de UIDs | 471–530 | `uidRegistrar`, `uidEliminar`, `reconstruirArchivoUids`, `usuariosHtml`, `usuariosCsv` |
| NTP | 531–610 | `aplicarTiempoNtp`, `iniciarNtp`, `verificarNtpPendiente`, `sincronizarNtpManual` |
| Limpieza atómica | 611–640 | `resetearContadoresNvs`, `limpiarRegistros` |
| Email SMTP | 641–780 | `enviarEmail`, `nombreAdjunto`, `tsParaNombre`, `smtpCallback` |
| Cierre de día | 781–880 | `cerrarDiaArranque`, `cerrarDia`, `autoEnviarEmail` |
| Handlers web | 881–1350 | Un handler por cada ruta HTTP |
| Watchdog NFC | 1351–1450 | `nfcReinicializar` |
| NFC lectura | 1451–1480 | `uidAHex`, `leerUidUnaVez` |
| `setup()` | 1481–1570 | Inicialización de todos los periféricos |
| `loop()` | 1571–fin | Bucle principal |

---

## Sistema de timers no bloqueantes con millis()

El sistema nunca usa `delay()` en el flujo normal (solo en `nfcReinicializar()` para la secuencia de recuperación del bus, donde es intencionalmente bloqueante). Todos los eventos periódicos se gestionan comparando `millis()` con un timestamp guardado.

### Timers activos

| Variable | Intervalo | Propósito |
|---|---|---|
| `ultimaLecturaNfc` | 300 ms | Cadencia de lectura del PN532 |
| `nfcUltimoCheck` | 15 000 ms | Watchdog: verificar salud del PN532 |
| `ultimoChequeoLimpieza` | 60 000 ms | Disparar `cerrarDia` y `autoEnviarEmail` |
| `ultimaReconexionWifi` | 30 000 ms | Reintentar WiFi si se perdió la conexión |
| `tiempoLcdHasta` | variable | Duración del mensaje temporal en LCD (3–4 s típico) |
| `lcdIdleDesde` | 120 000 ms | Retardo antes de iniciar el parpadeo del backlight |
| `lcdParpadeoNext` | 1 000 / 3 000 ms | Fase apagado / encendido del parpadeo backlight |
| `ledFijoHasta` | variable | Duración del color fijo en el LED |
| `ledProxCambio` | 200 / 250 ms | Fases apagado / encendido del parpadeo LED |
| `tiempoUltimoUid` | 3 000 ms | Cooldown del mismo UID (anti-rebote temporal) |
| `ultimaNtpSync` | referencia | Solo para mostrar "hace X min" en la web |

### Patrón del timer LCD (`tiempoLcdHasta`)

```cpp
// Al mostrar un mensaje temporal:
tiempoLcdHasta = millis() + DURACION_MS;

// En loop(), cada iteración:
if (tiempoLcdHasta && ahora >= tiempoLcdHasta) {
    tiempoLcdHasta = 0;
    // Mostrar mensaje según estado REAL del sistema en este momento:
    if      (esperandoTarjeta) lcdMostrar("Registrando:", ...);
    else if (modoEliminar)     lcdMostrar("Modo eliminar", ...);
    else                       lcdMostrar("Esperando", "tarjeta...", ...);
    lcdIdleDesde    = ahora;  // el contador de blink empieza desde cero
    lcdParpadeoNext = 0;
}
```

El bloque de expiración consulta el **estado real del sistema** (`esperandoTarjeta`, `modoEliminar`) en el momento de expirar, no un valor precapturado. Esto evita que el mensaje idle incorrecto aparezca si el sistema cambió de estado mientras el timer estaba corriendo.

### Protección del backlight LCD

Cuando `lcdIdleDesde == 0`, el parpadeo está completamente desactivado (`lcdActualizarParpadeo()` retorna inmediatamente). Se activa solo cuando `lcdIdleDesde` recibe un valor no nulo. `lcdWakeUp()` pone `lcdIdleDesde = 0` para suprimir el parpadeo mientras hay actividad. El bloque de expiración del timer reinicia `lcdIdleDesde = ahora` para que el conteo de inactividad empiece desde cero.

**Regla:** cualquier función que muestre un mensaje temporal en la LCD debe llamar `lcdWakeUp()` antes de `lcdMostrar()` y, si el mensaje no tiene `tiempoLcdHasta` asociado (como en modos de espera indefinida), también debe poner `tiempoLcdHasta = 0` para cancelar cualquier timer pendiente anterior.

---

## Máquina de estados del LED RGB

```
         ┌────────────────────────────────────────┐
         │          LED_IDLE (amarillo fijo)       │◄──────────────┐
         └──────────────┬──────────────────────────┘               │
                        │ ledFijar(color, ms)                      │
                        ▼                                          │
         ┌────────────────────────────────────────┐    expiró      │
         │          LED_FIJO (color fijo)          ├───────────────►│
         └──────────────────────────────────────── ┘   ledFijoHasta│
                                                                   │
         ┌────────────────────────────────────────┐    ciclosRest  │
         │    LED_PARPADEO (N destellos)           ├═══════0═══════►│
         │  ON ──250ms──► OFF ──200ms──► ON ...    │
         └─────────────────────────────────────────┘
```

### Transiciones

- `ledFijar(color, ms)` → entra en `LED_FIJO`. Al expirar `ledFijoHasta`, vuelve a `LED_IDLE` y aplica amarillo.
- `ledParpadear(color, n)` → entra en `LED_PARPADEO`. Alterna encendido (250 ms) / apagado (200 ms) `n` veces. Al completar, vuelve a `LED_IDLE` y aplica amarillo.
- Desde cualquier estado, una nueva llamada a `ledFijar` o `ledParpadear` sobreescribe el estado anterior. Esto es intencional: el evento más reciente siempre toma prioridad.

### Lógica de inversión para ánodo/cátodo común

```cpp
void ledAplicar(LedColor c) {
    bool inv = LED_ANODO_COMUN;
    digitalWrite(LED_PIN_R, inv ? !c.r : c.r);
    // ídem para G y B
}
```

`LedColor` usa `bool` por canal (on/off). Cuando `LED_ANODO_COMUN = true`, los valores se invierten antes de escribir al GPIO: `true` (canal activo) → `LOW` en el pin. Cambiar la constante en tiempo de compilación es suficiente para soportar ambos tipos de LED.

---

## Sistema de cierre de día

El problema que resuelve: si un usuario registró una Entrada pero no una Salida antes de que cambiara el día (medianoche), su contador queda impar. Al día siguiente, ese contador debería interpretarse como "hubo una Entrada sin Salida" y generar un registro de **"Salida: Pendiente"**. Luego el contador debe resetearse a 0 para que el nuevo día comience desde Entrada.

Hay dos funciones que resuelven esto, porque el ESP32 puede o no estar encendido en el momento exacto de la medianoche.

### `cerrarDia()` — caso normal (ESP32 encendido al cruzar medianoche)

- Se llama desde `loop()` en el chequeo periódico cada 60 segundos.
- Protegida por `lastCierre` (NVS): si ya corrió hoy, retorna inmediatamente.
- Itera todos los UIDs encontrados en `ARCHIVO_UIDS` y en `ARCHIVO_ENT` (por si algún UID no estaba en el primero).
- Por cada UID registrado en NVS (`nombre != ""`), si su contador `c<UID>` es impar → genera "Salida: Pendiente" y resetea el contador a 0.
- **Limitación:** usa solo el contador, no la fecha por tarjeta. No distingue si el contador impar pertenece al día anterior o al actual.

### `cerrarDiaArranque()` — caso de corte de alimentación (ESP32 apagado a medianoche)

- Se llama desde `setup()`, después de inicializar el RTC, antes de entrar a `loop()`.
- **No** está protegida por `lastCierre`. Corre siempre en cada arranque.
- Itera solo `ARCHIVO_UIDS` (más rápido, no requiere parsear `ARCHIVO_ENT`).
- Por cada UID registrado, lee `f<UID>` (fecha del último acceso). Si esa fecha es **estrictamente anterior** a la fecha actual del RTC → verifica si el contador es impar → genera "Salida: Pendiente" y resetea.
- La condición `fechaUltima < fechaHoy` (comparación lexicográfica de `YYYY-MM-DD`) garantiza que no se confunda un contador impar del día actual (acumulado antes del arranque dentro del mismo día) con uno del día anterior.
- Cuando `cerrarDia()` corre 60 segundos después del arranque, los contadores ya están en 0 para las tarjetas procesadas → no genera duplicados.

### Por qué `cerrarDia()` no usa `f<UID>`

`cerrarDia()` fue diseñada para correr a medianoche con el sistema encendido. En ese momento, cualquier contador impar **necesariamente** pertenece al día que acaba de terminar. No necesita verificar la fecha por tarjeta porque el contexto temporal está garantizado por el guard de `lastCierre`.

### Capa de seguridad por tarjeta en `loop()`

Además de las dos funciones anteriores, la lectura normal en `loop()` también detecta cambios de día por tarjeta individual: compara `f<UID>` con la fecha actual en cada pasada de tarjeta. Si difieren y el contador es impar (y la tarjeta está registrada), genera "Salida: Pendiente". Esta capa cubre el caso en que una tarjeta específica no fue procesada por `cerrarDia()` ni por `cerrarDiaArranque()`.

---

## Bus I2C compartido PN532 + LCD y watchdog

### El problema

El PN532 (NFC) y la LCD 20×4 comparten el mismo bus I2C (Wire, pines 21/22). El PN532 usa timeouts internos y secuencias de inicialización que mantienen el bus activo durante períodos prolongados. Si la LCD realiza una transacción I2C mientras el PN532 tiene una lectura en progreso, puede dejar el bus en un estado inconsistente: uno de los dispositivos puede quedar con SDA en nivel bajo, bloqueando indefinidamente el bus.

Con `Wire.setTimeOut(200)`, cada transacción tiene un máximo de 200 ms. Sin este timeout, una transacción bloqueada detendría el sistema completo.

### El watchdog PN532

Cada `NFC_CHECK_INTERVALO` (15 segundos), `loop()` llama a `lectorNfc.getFirmwareVersion()`. Si retorna 0, el chip no responde y se invoca `nfcReinicializar()`. Ver la sección siguiente para el detalle de esa función.

```cpp
if (ahora - nfcUltimoCheck >= NFC_CHECK_INTERVALO) {
    nfcUltimoCheck = ahora;
    if (!lectorNfc.getFirmwareVersion()) {
        nfcReinicializar();
        return;  // reiniciar el loop con el estado restaurado
    }
}
```

El `return` después de `nfcReinicializar()` es deliberado: evita que el resto del loop procese una lectura NFC inmediatamente después de la recuperación, cuando el bus podría estar todavía asentándose.

### Por qué el lector NFC usa el mismo bus que la LCD

El ESP32 tiene dos controladores I2C hardware (`Wire` y `Wire1`, usados aquí como `Wire` y `busRtc`). El RTC DS3231 usa `busRtc` (pines 19/18). El PN532 y la LCD deben compartir `Wire` (pines 21/22) porque no hay un tercer bus I2C hardware disponible y una solución por software (bit-banging) añadiría complejidad incompatible con la librería del PN532.

---

## Anti-rebote y cooldown de tarjetas

`leerUidUnaVez()` implementa dos niveles de filtrado para evitar registros duplicados:

### Nivel 1: presencia física

```cpp
if (hex == ultimoUid && (tarjetaPresente || (ahora - tiempoUltimoUid < COOLDOWN_TARJETA))) {
    tarjetaPresente = true;
    return false;
}
```

Si el UID leído es el mismo que el anterior Y la tarjeta todavía está físicamente presente (o el cooldown no expiró), se descarta. `tarjetaPresente` se pone a `false` solo cuando `readPassiveTargetID` no encuentra ninguna tarjeta, lo que indica que fue retirada.

### Nivel 2: cooldown temporal

`COOLDOWN_TARJETA = 3000 ms`. Incluso si la tarjeta se retira y se vuelve a acercar dentro de los 3 segundos, el UID sigue siendo ignorado. Esto cubre el efecto "flicker" del PN532 en I2C: el chip a veces deja de reportar una tarjeta presente por un instante aunque la tarjeta no se haya movido, porque una transacción I2C de la LCD interrumpió su ciclo de polling.

### Por qué `ultimoUid` no se borra al retirar la tarjeta

```cpp
if (!hayTarjeta) {
    if (tarjetaPresente) { tarjetaPresente = false; }
    return false;  // ultimoUid se mantiene
}
```

`ultimoUid` conserva el último UID visto para que el cooldown temporal siga protegiendo aunque la tarjeta se retire y se acerque rápidamente. Si se borrara al retirar, el cooldown por `tiempoUltimoUid` quedaría inefectivo.

---

## Por qué existe ARCHIVO_UIDS además de NVS

NVS (la librería `Preferences` de Arduino para ESP32) permite leer y escribir claves por nombre pero **no ofrece una API estándar para iterar todas las claves de un namespace** de forma eficiente desde el framework Arduino.

Para funciones como `cerrarDia()`, `cerrarDiaArranque()`, `resetearContadoresNvs()` y `usuariosHtml()`, el sistema necesita conocer la lista completa de UIDs registrados. Sin una forma de enumerarlos, estas operaciones serían imposibles.

`/uids.txt` en LittleFS actúa como índice: una línea por UID, en mayúsculas hexadecimales.

### Riesgo de desincronización

Si `ARCHIVO_UIDS` y NVS divergen (por ejemplo, por un corte de luz durante un registro), el archivo podría contener UIDs sin nombre en NVS, o NVS podría tener UIDs no listados en el archivo. Por eso existe `reconstruirArchivoUids()`:

- Se llama en cada arranque desde `setup()`.
- Usa la API de bajo nivel `nvs_entry_find` / `nvs_entry_next` del ESP-IDF para iterar **todas** las claves del namespace `"nfc"`.
- Filtra las claves que son UIDs válidos: cadenas hexadecimales en mayúsculas de 6 a 14 caracteres. Todas las claves del sistema (`buildID`, `lastClean`, `k<uid>`, `c<uid>`, `f<uid>`, etc.) contienen al menos un carácter minúsculo o especial y no pasan este filtro.
- Reescribe `/uids.txt` completamente, garantizando coherencia en cada arranque.

---

## Lógica par/impar para entradas y salidas

El estado Entrada/Salida de cada usuario se determina por el **contador `c<UID>` en NVS**:

```
c<UID> == 0  → próxima pasada será Entrada  (el contador empieza en 0)
c<UID> == 1  → fue Entrada (impar), próxima será Salida
c<UID> == 2  → fue Salida (par),   próxima será Entrada
...
conteo % 2 == 1  → Entrada
conteo % 2 == 0  → Salida (excepto 0 = primer acceso o post-reset)
```

```cpp
int    conteo = almacen.getInt(claveCont.c_str(), 0) + 1;
String tipo   = (conteo % 2 == 1) ? "Entrada" : "Salida";
```

El contador se incrementa **antes** de escribir, de modo que el valor guardado en NVS siempre refleja cuántos accesos ha habido en el día actual.

### Casos borde

**Cambio de día con Entrada sin Salida:** El contador queda en valor impar. `cerrarDia()` o `cerrarDiaArranque()` escriben "Salida: Pendiente" y resetean el contador a 0. Al día siguiente, la primera pasada es Entrada (contador 1).

**Tarjeta NO_REGISTRADO:** El nombre se obtiene con valor por defecto `"NO_REGISTRADO"` pero el contador existe y se incrementa igualmente. La lógica de cierre de día tiene una guarda explícita: no genera "Salida: Pendiente" para UIDs sin nombre en NVS (solo verifica con `almacen.getString(uid.c_str(), "").length() > 0`). Esto evita un "Salida: Pendiente fantasma" para tarjetas sin historial real registrado.

**Reset explícito:** Al registrar una tarjeta, borrar un usuario, limpiar registros manualmente, o al cierre de día, el contador se pone a 0 explícitamente con `almacen.putInt(claveCont.c_str(), 0)`.

---

## Secuencia de recuperación del bus I2C

`nfcReinicializar()` implementa la secuencia estándar de recuperación de bus I2C (IEEE §3.1.16) adaptada al ESP32:

```
Paso 1: Wire.end()
        Libera el controlador I2C del ESP32. Sin esto, los pines siguen
        controlados por el periférico y las manipulaciones GPIO directas
        del paso 2 entran en conflicto.

Paso 2: 9 pulsos en SCL (pin 22 como salida, SDA pin 21 como entrada)
        Un dispositivo I2C que quedó en medio de una transmisión puede
        tener SDA en nivel bajo (esperando enviar más bits). La especificación
        I2C indica que 9 pulsos de reloj liberan cualquier slave bloqueado,
        porque ninguna trama válida tiene más de 9 bits consecutivos de datos.

Paso 3: Condición STOP (SDA sube mientras SCL está en alto)
        Indica a todos los dispositivos del bus que la transacción ha
        terminado y el bus vuelve al estado libre (SDA=HIGH, SCL=HIGH).

Paso 4: Wire.begin(21, 22) + Wire.setTimeOut(200)
        Reinicializa el controlador I2C del ESP32 con el timeout de seguridad.

Paso 5: lcd.init() + lcd.backlight()
        La LCD puede haber perdido su estado de inicialización durante la
        recuperación (los pulsos de SCL también la alcanzan). Se reinicializa
        para garantizar que vuelva a mostrar correctamente.

Paso 6: lectorNfc.begin() + Wire.begin(21, 22) nuevamente
        lectorNfc.begin() incluye un pulso RESET en el pin configurado
        del PN532. En este hardware el pin RESET del PN532 no está separado
        del SCL (comparten el pin 22), lo que puede reconfigurar el pin
        como GPIO. Por eso se llama a Wire.begin() nuevamente después de
        begin() para restaurar la configuración I2C.
```

Por qué el segundo `Wire.begin()` después de `lectorNfc.begin()`: la librería Adafruit_PN532 puede internamente llamar a funciones que reconfiguran los pines del bus. Llamar `Wire.begin()` nuevamente es un seguro que no tiene coste adicional y garantiza que el bus queda configurado correctamente.

---

## Consideraciones del DS3231 con RTClib

### Bus independiente

El DS3231 usa `TwoWire busRtc = TwoWire(1)` (controlador I2C número 1 del ESP32, pines 19/18). Esto lo aísla completamente del PN532 y la LCD, que están en `Wire` (controlador 0, pines 21/22). Un bus colgado en Wire no afecta al RTC.

### Detección de pérdida de alimentación

```cpp
bool perdioAlim = rtcDs3231.lostPower();
```

`lostPower()` en RTClib lee el flag OSF (Oscillator Stop Flag) del registro de estado del DS3231. Este flag se pone a 1 cuando el oscilador se detuvo (batería agotada o primer encendido sin batería). Si es `true`, el tiempo guardado no es fiable y se reajusta.

### Detección de firmware nuevo

```cpp
String buildIdActual = String(__DATE__) + __TIME__;
bool firmwareNuevo = (almacen.getString("buildID", "") != buildIdActual);
```

`__DATE__` y `__TIME__` son macros del preprocesador que el compilador sustituye por la fecha y hora de compilación. Si difieren de lo guardado en NVS, hay un firmware nuevo y el RTC se sincroniza con esa fecha/hora. Luego se actualiza `buildID` en NVS. En arranques posteriores del mismo firmware, el RTC no se toca.

### Formato de timestamp

```cpp
snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
         anio, mes, dia, hora, minuto, seg);
```

Todos los timestamps en logs y NVS usan el formato ISO 8601 `YYYY-MM-DD HH:MM:SS`. La comparación de fechas para el cierre de día y la detección de cambio de día usa las primeras 10 caracteres (`YYYY-MM-DD`), que son directamente comparables como strings (comparación lexicográfica equivale a comparación cronológica en este formato).

### Validación de rango

`obtenerTimestamp()` valida que los valores leídos del DS3231 estén dentro de rangos razonables (año 2020–2099, mes 1–12, etc.) antes de formatear. Si la validación falla, retorna `"RTC-ERROR"`, que no es un timestamp válido y es ignorado por las funciones que lo reciben.

---

## Decisiones de diseño no obvias

### Patrón de polling asíncrono para registro y borrado

El registro y borrado de usuarios requieren interacción física (acercar la tarjeta al lector). No es posible mantener una petición HTTP abierta hasta que ocurra el evento físico (timeout del servidor). La solución:

1. El handler web (`handleSaveName`, `handleDeleteUser`) activa un flag (`esperandoTarjeta` / `modoEliminar`) y responde inmediatamente con una página HTML que inicia un polling JavaScript cada 700 ms a `/status`.
2. `loop()` detecta la tarjeta y escribe el resultado en un "buzón" NVS (`lastReg` / `lastDel`).
3. El siguiente poll de `/status` lee el buzón, lo borra y retorna el resultado al navegador.
4. El JavaScript redirige automáticamente a `/done` o `/deleted`.

Este patrón permite que el servidor web sea completamente no bloqueante: ningún handler espera ni bloquea, y `loop()` sigue corriendo con normalidad.

### Borrado de registros como operación atómica

`limpiarRegistros()` siempre borra `ARCHIVO_LOG`, `ARCHIVO_ENT` **y** resetea todos los contadores NVS en una sola función. Si se borraran los archivos sin resetear los contadores, el sistema interpretaría que los usuarios con contador impar están "dentro" aunque no haya ningún registro que lo respalde. Si se resetearan los contadores sin borrar los archivos, los archivos mostrarían registros que el sistema ya no reconoce. La consistencia requiere que las tres acciones sean siempre conjuntas.

### Partición `min_spiffs`

El ESP32 divide su flash en particiones. La partición por defecto de Arduino reserva muy poco espacio para SPIFFS/LittleFS. `min_spiffs.csv` redistribuye el espacio dejando ~1.8 MB para LittleFS a costa de reducir el OTA partition. Dado que este proyecto no usa OTA, el intercambio es aceptable.

### Límite de 300 entradas mostradas en la web

Los archivos de log pueden crecer indefinidamente. Cargar el archivo completo en RAM del ESP32 (520 KB de SRAM, de los que solo una fracción está disponible para el heap) causaría un OOM (Out Of Memory) con archivos grandes. La solución es un two-pass streaming:

1. **Primer pass:** contar el total de líneas leyendo el archivo byte a byte (solo incrementa un contador al detectar `\n`).
2. **Segundo pass:** saltar las primeras `total - 300` líneas y cargar solo las últimas 300 en un array dinámico.

Esto mantiene el consumo de RAM acotado independientemente del tamaño del archivo.

### `ARCHIVO_UIDS` se reconstruye en cada arranque

Podría parecer redundante reconstruir `ARCHIVO_UIDS` en cada arranque si ya existe. Sin embargo, el archivo puede haberse desincronizado de NVS (cortes de luz durante un registro, borrado manual de archivos desde LittleFS, etc.). La reconstrucción garantiza coherencia en O(n) en el arranque, que es el único momento donde un coste fijo de unos pocos milisegundos es aceptable sin impacto en el usuario.

### `lcdPost` como variable obsoleta

La variable `lcdPost` (enum `LcdPost { LCD_IDLE, LCD_LISTO }`) se asigna en varios puntos del código pero ya no se usa en la decisión de qué mostrar al expirar `tiempoLcdHasta`. El bloque de expiración fue reescrito para consultar directamente `esperandoTarjeta` y `modoEliminar`. Las asignaciones de `lcdPost` son código inerte que permanece por compatibilidad con versiones anteriores; puede eliminarse sin efecto funcional.

### Sistema de dos ventanas para auto-envío de correo y bandera de pendiente

El borrado automático de archivos solo ocurre tras un envío de correo exitoso con `borrarTras=true`. No existe limpieza automática independiente del correo. El razonamiento: borrar sin enviar significaría pérdida de datos irrecuperable; el correo es la confirmación de que los datos están en destino.

`autoEnviarEmail()` intenta el envío en dos ventanas horarias del día programado:

```
Hora 0 (medianoche) → intentar si WiFi disponible
Hora 1–11           → no hacer nada (esperar mediodía)
Hora 12 (mediodía)  → reintentar si WiFi disponible y no enviado aún
Hora 13+            → ambas ventanas agotadas → activar emailPendienteFlag
```

La función corre cada 60 segundos. Dentro de cada ventana horaria, retrying each minute is useful for temporary WiFi drops. El guard `lastEmail == today` previene doble envío.

La **bandera `emailPendienteFlag`** es un `bool` en RAM respaldado por `emailPend` (bool en NVS). Se inicializa desde NVS en `setup()` para sobrevivir reinicios. Se activa cuando `hora > 12` en un día de envío sin `lastEmail == today`. Se desactiva:
1. Cuando `enviarEmail()` con `borrarTras=true` completa con éxito (situación resuelta automáticamente).
2. Cuando el usuario hace clic en **✓ Confirmar** del banner y confirma en el diálogo JS (`handleConfirmarEmailPend()` limpia la bandera en NVS y redirige a `/`).

`bannerAlerta()` es llamada desde `pagina()` en cada respuesta HTTP — si `emailPendienteFlag` es `false` retorna `""` y el overhead es nulo. Si es `true`, inyecta el div de advertencia al inicio del `<body>` en todas las páginas sin necesidad de modificar cada handler individualmente.
