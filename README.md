# Sistema de Control de Acceso NFC — ESP32

Sistema embebido de control de acceso basado en tarjetas NFC. Lee UIDs de tarjetas Mifare ISO14443A, los mapea a nombres de usuarios registrados, registra entradas y salidas con marca de tiempo, y expone una interfaz web local para gestión completa del sistema.

---

## Contenido

1. [Componentes de hardware](#componentes-de-hardware)
2. [Conexiones físicas](#conexiones-físicas)
3. [Instalación del firmware](#instalación-del-firmware)
4. [Configuración inicial](#configuración-inicial)
5. [Interfaz web](#interfaz-web)
6. [Indicadores LED RGB](#indicadores-led-rgb)
7. [Indicadores de la matriz LED](#indicadores-de-la-matriz-led)
8. [Mensajes de la LCD](#mensajes-de-la-lcd)
9. [Comportamiento ante cortes de alimentación](#comportamiento-ante-cortes-de-alimentación)
10. [Sistema de correo automático](#sistema-de-correo-automático)
11. [Solución de problemas](#solución-de-problemas)

---

## Componentes de hardware

| Componente           | Referencia / Descripción                                          |
| -------------------- | ------------------------------------------------------------------ |
| Microcontrolador     | ESP32 DevKit v1 (o compatible, 38 pines)                           |
| Lector NFC           | PN532 NFC/RFID Controller, en modo **HSU (UART serial)**           |
| Reloj en tiempo real | DS3231 RTC Module (con batería CR2032), vía librería RTClib        |
| Pantalla             | LCD 20×4 caracteres con módulo I2C PCF8574 (dirección 0x27)        |
| LED indicador        | LED RGB de 4 pines, ánodo común o cátodo común                     |
| Matriz de LEDs       | Matriz WS2812B 8×8 (64 LEDs direccionables), vía librería FastLED  |
| Tarjetas             | Tarjetas o llaveros Mifare ISO14443A (compatibles con PN532)       |

> El PN532 debe configurarse en modo **HSU / UART** (puentes o resistencias de selección de protocolo del módulo en la posición de comunicación serial, **no** I2C ni SPI — consultar el datasheet del módulo específico). El firmware se compila con el flag `-DNFC_INTERFACE_HSU` (ver [platformio.ini](platformio.ini)) y usa la librería [PN532 de Seeed-Studio](https://github.com/Seeed-Studio/PN532), no la de Adafruit.

---

## Conexiones físicas

### UART2 (pines 16/17): PN532 en modo HSU

| Señal | ESP32 GPIO | PN532        |
| ----- | ---------- | ------------ |
| RX2   | 16         | TX (salida)  |
| TX2   | 17         | RX (entrada) |
| VCC   | 3.3 V      | VCC (3.3 V)  |
| GND   | GND        | GND          |

> El PN532 se comunica por **UART serial (HSU)**, no por I2C. Usa el controlador `Serial2` del ESP32 a través de la librería `PN532_HSU` (Seeed-Studio) y no comparte bus con ningún otro periférico del sistema. No requiere pines IRQ ni RESET físicos.

### Bus I2C 0 — Wire (pines 21/22): LCD

| Señal | ESP32 GPIO | LCD (PCF8574)                  |
| ----- | ---------- | ------------------------------- |
| SDA   | 21         | SDA                             |
| SCL   | 22         | SCL                             |
| VCC   | 3.3 V      | VCC (5 V o 3.3 V según módulo)  |
| GND   | GND        | GND                             |

> Este bus está dedicado exclusivamente a la LCD (dirección I2C 0x27). El PN532 ya no lo comparte, por lo que no existe riesgo de bloqueo del bus por interferencia entre ambos dispositivos.

### Bus I2C 1 — busRtc (pines 19/18): DS3231

| Señal | ESP32 GPIO | DS3231 |
| ----- | ---------- | ------ |
| SDA   | 19         | SDA    |
| SCL   | 18         | SCL    |
| VCC   | 3.3 V      | VCC    |
| GND   | GND        | GND    |

> El DS3231 usa un tercer bus I2C independiente (segundo controlador I2C del ESP32) para evitar cualquier interferencia con la LCD.

### LED RGB (pines 25/26/27)

El tipo de LED (ánodo o cátodo común) se configura con la constante `LED_ANODO_COMUN` en `src/main.cpp`. El valor por defecto actual es `false` (**cátodo común**).

**Cátodo común** (`LED_ANODO_COMUN false`, configuración por defecto):

| Pin LED            | Conexión                | ESP32 GPIO |
| ------------------- | ------------------------ | ---------- |
| Cátodo común (GND) | GND                      | —          |
| R (rojo)            | Resistencia 100–220 Ω → | GPIO 25    |
| G (verde)           | Resistencia 100–220 Ω → | GPIO 26    |
| B (azul)            | Resistencia 100–220 Ω → | GPIO 27    |

**Ánodo común** (`LED_ANODO_COMUN true`):

| Pin LED            | Conexión                | ESP32 GPIO |
| ------------------- | ------------------------ | ---------- |
| Ánodo común (VCC)  | 3.3 V                    | —          |
| R (rojo)            | Resistencia 100–220 Ω → | GPIO 25    |
| G (verde)           | Resistencia 100–220 Ω → | GPIO 26    |
| B (azul)            | Resistencia 100–220 Ω → | GPIO 27    |

> Los GPIOs 25, 26 y 27 son de propósito general y no están usados por ningún otro periférico del sistema.

### Matriz LED WS2812B 8×8 (pin 23)

| Señal      | ESP32 GPIO | Matriz WS2812B        |
| ---------- | ---------- | ---------------------- |
| DIN (dato) | 23         | DIN (entrada de datos) |
| VCC        | 5 V        | VCC                     |
| GND        | GND        | GND                     |

> Los 64 LEDs se controlan por una sola línea de datos (protocolo WS2812B) mediante la librería FastLED. El brillo está limitado por firmware a 60/255 para evitar calentamiento excesivo y reducir el consumo. GPIO 23 es de propósito general y no está usado por ningún otro periférico.

---

## Instalación del firmware

### Requisitos

- [PlatformIO](https://platformio.org/) (extensión de VS Code o CLI)
- Python 3.x instalado
- ESP32 conectado por USB

### Dependencias (se instalan automáticamente con PlatformIO)

| Librería          | Versión | Fuente                                   |
| ----------------- | ------- | ------------------------------------------ |
| PN532             | —       | github.com/Seeed-Studio/PN532 (modo HSU) |
| LiquidCrystal_I2C | ^1.1.4  | marcoschwartz/LiquidCrystal_I2C          |
| ESP Mail Client   | ^3.4.19 | mobizt/ESP Mail Client                   |
| RTClib            | ^2.1.4  | adafruit/RTClib                          |
| FastLED           | ^3.9.0  | fastled/FastLED                          |

Las siguientes librerías son parte del framework Arduino para ESP32 y **no** requieren declaración adicional:

`WiFi`, `WebServer`, `Preferences`, `Wire`, `LittleFS`

> El proyecto usa la partición `min_spiffs.csv` para maximizar el espacio disponible para LittleFS. Esta partición viene incluida en el framework de ESP32 para PlatformIO.
>
> El `build_flags = -DNFC_INTERFACE_HSU` en [platformio.ini](platformio.ini) le indica a la librería PN532 que se comunique por UART (HSU) en vez de I2C. No quitar este flag: sin él, la librería intenta hablar por I2C con el chip y la lectura de tarjetas falla.

### Pasos de instalación

```bash
# 1. Compilar y subir el firmware
platformio run --environment esp32dev --target upload

# 2. Subir el sistema de archivos LittleFS (solo si se agregan activos)
platformio run --environment esp32dev --target uploadfs

# 3. Abrir monitor serie (opcional, para diagnóstico)
platformio run --environment esp32dev --target monitor
```

> En la primera carga, el RTC se sincroniza automáticamente con la hora de compilación del firmware. Esto es normal y esperado.

### Ajuste del tipo de LED

Antes de compilar, verificar en `src/main.cpp`:

```cpp
#define LED_ANODO_COMUN false   // true = ánodo común | false = cátodo común (valor por defecto)
```

---

## Configuración inicial

### Acceso a la red WiFi del dispositivo

El ESP32 crea un punto de acceso WiFi propio:

| Parámetro          | Valor          |
| ------------------ | -------------- |
| SSID               | `NFC`          |
| Contraseña         | `1234567890`   |
| IP del dispositivo | `192.168.4.1`  |

Conectarse a esta red desde cualquier dispositivo y abrir `http://192.168.4.1` en el navegador.

### Configurar WiFi con internet (para NTP y correo)

1. Ir a **Configuración** (`/config`) desde la página de inicio.
2. Ingresar el SSID y contraseña de la red WiFi local con acceso a internet.
3. Configurar el email de destino para los reportes automáticos.
4. Ajustar la zona horaria (offset UTC en horas, por ejemplo `-5` para Colombia UTC-5).
5. Guardar y reconectar. El ESP32 intentará conectarse a la red configurada manteniendo el AP propio activo simultáneamente.

### Sincronizar el reloj por NTP

Una vez conectado a internet, ir a **Configuración → Sincronizar hora por NTP ahora** o esperar a que el sistema lo haga automáticamente al detectar la conexión WiFi.

### Configurar el correo de envío (SMTP)

Las credenciales del remitente están definidas como constantes en `src/main.cpp`. Para Gmail:

```cpp
#define SMTP_HOST        "smtp.gmail.com"
#define SMTP_PORT        587
#define REMITENTE_EMAIL  "tu.correo@gmail.com"
#define REMITENTE_CLAVE  "abcdabcdabcdabcd"  // App Password de 16 caracteres
```

> Se debe usar una **App Password** de Google, no la contraseña habitual de Gmail. Generarla en: Cuenta de Google → Seguridad → Verificación en dos pasos → Contraseñas de aplicación.

---

## Interfaz web

Todas las rutas son accesibles desde `http://192.168.4.1`.

| Ruta                | Método | Función                                                                    |
| ------------------- | ------ | -------------------------------------------------------------------------- |
| `/`                 | GET    | Página de inicio: estado del sistema y reloj en tiempo real                |
| `/register`         | GET    | Formulario para registrar un nuevo usuario                                 |
| `/saveName`         | POST   | Inicia el proceso de registro (pide nombre y código, luego espera tarjeta) |
| `/deleteUser`       | GET    | Activa el modo de borrado (espera tarjeta para eliminar usuario)           |
| `/cancelar`         | GET    | Cancela cualquier operación pendiente (registro o borrado)                 |
| `/status`           | GET    | Endpoint de polling interno del navegador (respuesta en texto plano)       |
| `/done`             | GET    | Confirmación de registro exitoso                                           |
| `/deleted`          | GET    | Confirmación de borrado                                                    |
| `/logs`             | GET    | Tabla HTML con los últimos 300 eventos de acceso                           |
| `/downloadLogs`     | GET    | Descarga el log de accesos en formato CSV                                  |
| `/entradas`         | GET    | Tabla HTML con los últimos 300 registros de entradas y salidas             |
| `/downloadEntradas` | GET    | Descarga las entradas/salidas en formato CSV                               |
| `/clearRegistros`   | GET    | Borra todos los registros (logs + entradas/salidas) y resetea contadores   |
| `/usuarios`         | GET    | Lista de todos los usuarios registrados                                    |
| `/downloadUsuarios` | GET    | Descarga la lista de usuarios en formato CSV                               |
| `/config`           | GET    | Página de configuración (WiFi, email, zona horaria)                        |
| `/saveConfig`       | POST   | Guarda la configuración y reconecta el WiFi                                |
| `/sendEmail`             | GET    | Envía manualmente un correo con todos los registros adjuntos               |
| `/syncNtp`               | GET    | Sincroniza el reloj con servidores NTP manualmente                         |
| `/time`                  | GET    | Devuelve la hora actual como texto (usado por el reloj en vivo de la web)  |
| `/confirmarEmailPend`    | GET    | Limpia la bandera de reporte pendiente (accionado desde el banner de alerta)|

### Proceso de registro de usuario

1. Ir a `/register` e ingresar nombre (libre) y código (1–6 caracteres alfanuméricos).
2. Enviar el formulario → la LCD muestra "Registrando: [nombre] / Acerca tarjeta".
3. Acercar la tarjeta NFC al lector. El sistema guarda el UID con el nombre y código.
4. La web redirige automáticamente a la página de confirmación.

### Proceso de borrado de usuario

1. Ir a `/register` → "Borrar usuario con tarjeta".
2. La LCD muestra "Modo eliminar / Acerca tarjeta".
3. Acercar la tarjeta del usuario a borrar. El sistema elimina el registro de NVS y de LittleFS.
4. Si la tarjeta no está registrada, la web informa "Tarjeta no registrada".

---

## Indicadores LED RGB

| Estado / Evento                 | Color    | Comportamiento | Duración                           |
| ------------------------------- | -------- | -------------- | ---------------------------------- |
| Sistema en espera (idle)        | Amarillo | Fijo           | Permanente                         |
| Tarjeta registrada leída        | Verde    | Fijo           | Igual que el mensaje en LCD (~4 s) |
| Tarjeta NO registrada           | Rojo     | Fijo           | Igual que el mensaje en LCD (~4 s) |
| Nuevo usuario registrado        | Verde    | Fijo           | Igual que el mensaje en LCD (~4 s) |
| Usuario eliminado               | Verde    | Fijo           | Igual que el mensaje en LCD (~3 s) |
| Tarjeta no encontrada al borrar | Rojo     | Fijo           | Igual que el mensaje en LCD (~3 s) |
| Correo enviado correctamente    | Verde    | 5 destellos    | ~2.25 s total                      |
| Error al enviar correo          | Rojo     | 5 destellos    | ~2.25 s total                      |
| Registros borrados manualmente  | Verde    | 5 destellos    | ~2.25 s total                      |

> Los registros borrados **automáticamente** junto con el envío de correo en los días programados no generan el parpadeo de borrado; el parpadeo por envío de correo exitoso ya cubre ese evento.

### Protección de la LCD (backlight)

Después de 1 segundo sin actividad, el backlight de la LCD entra en un parpadeo continuo (100 ms encendido / 30 ms apagado) mientras el sistema permanece inactivo, para reducir el desgaste del panel. Durante la fase "apagado" de cada ciclo, la matriz de LEDs también se apaga momentáneamente, así ambos indicadores parpadean de forma sincronizada. Cualquier lectura de tarjeta o acción desde la web restaura el backlight y la matriz de inmediato (`lcdWakeUp()`).

---

## Indicadores de la matriz LED

La matriz WS2812B 8×8 muestra un ícono (patrón de píxeles) según el estado del sistema, en paralelo a la LCD y al LED RGB. Cada patrón se dibuja con `matrizMostrar()` y puede ser temporal (vuelve sola al ícono de espera tras N milisegundos) o permanente hasta el siguiente evento.

| Ícono                     | Color                  | Significado                                             | Duración                                |
| -------------------------- | ----------------------- | --------------------------------------------------------- | ------------------------------------------ |
| Marco (cuadro vacío)      | Amarillo tenue          | Sistema en espera (idle) — equivalente al LED amarillo   | Permanente hasta la próxima lectura      |
| Flecha apuntando arriba   | Verde                   | Tarjeta registrada leída, acceso permitido                | ~4 s (igual que el mensaje en LCD)       |
| X                         | Rojo                    | Tarjeta NO registrada, borrado fallido o error del lector | ~3–4 s según el evento                   |
| Chulito (checkmark)       | Verde                   | Éxito: registro OK, borrado OK, correo enviado, registros borrados | ~3–5 s según el evento          |

> El ícono de espera (marco) es el mismo que se restaura automáticamente cuando expira el temporizador de cualquier otro patrón, y también es el que aparece al terminar cada ciclo de parpadeo del backlight de la LCD.

---

## Mensajes de la LCD

La LCD de 20×4 muestra los siguientes mensajes según el estado del sistema:

| Mensaje                                  | Descripción                                                            |
| ---------------------------------------- | ---------------------------------------------------------------------- |
| `Esperando tarjeta...`                   | Sistema en modo de lectura normal, listo para recibir tarjetas         |
| `[Nombre del usuario]` (multilínea)      | Nombre del usuario leído, con su código en la esquina inferior derecha |
| `NO_REGISTRADO`                          | Se leyó una tarjeta sin nombre asociado en el sistema                  |
| `Registrando: [nombre] / Acerca tarjeta` | Esperando que se acerque la tarjeta para completar el registro         |
| `Modo eliminar / Acerca tarjeta`         | Esperando que se acerque la tarjeta a borrar                           |
| `No registrado`                          | Se intentó borrar una tarjeta que no existe en el sistema              |
| `Cancelado`                              | El usuario canceló una operación de registro o borrado                 |
| `Sistema NFC / Iniciando...`             | Secuencia de arranque del sistema                                      |
| `Sistema NFC / Listo...`                 | Sistema completamente iniciado                                         |
| `NFC ERROR / Revisar lector`             | El PN532 no responde tras el intento de recuperación automática        |

---

## Comportamiento ante cortes de alimentación

El sistema está diseñado para ser completamente resistente a cortes de alimentación.

### Al arrancar después de un corte

1. El RTC DS3231 mantiene la hora con su batería CR2032, incluso sin alimentación principal. Si la batería también falla, el sistema detecta `lostPower()` y reajusta la hora a la fecha de compilación del firmware.
2. `cerrarDiaArranque()` se ejecuta al inicio (antes de cualquier lectura de tarjeta) y recorre todos los usuarios registrados. Para cada usuario cuya última fecha de acceso guardada en NVS sea anterior a la fecha actual y cuyo contador sea impar (Entrada sin Salida registrada), genera automáticamente un registro de **"Salida: Pendiente"** y resetea el contador. Esto garantiza que ninguna entrada quede huérfana por un corte de luz.
3. Los contadores de NVS persisten entre reinicios ya que NVS es almacenamiento no volátil.
4. Los archivos de LittleFS (`/logs.txt`, `/entradas.txt`) también persisten en flash.

### Dato guardado por tarjeta en NVS

Por cada usuario, el sistema mantiene en NVS:

- `c<UID>` — contador de accesos (par = último fue Salida, impar = último fue Entrada)
- `f<UID>` — fecha del último acceso en formato `YYYY-MM-DD`

Esta información permite detectar correctamente si hubo una Entrada sin Salida al cruzar la medianoche, aunque el ESP32 haya estado apagado.

---

## Sistema de correo automático

### Cuándo se envía

El sistema envía automáticamente un correo en los **días 10, 20 y el último día del mes**. Hay dos ventanas de envío por día:

| Ventana | Hora       | Comportamiento                                                      |
| ------- | ---------- | ------------------------------------------------------------------- |
| 1.ª     | Medianoche | Primer intento automático                                           |
| 2.ª     | Mediodía   | Reintento si la primera ventana falló (sin internet, sin corriente) |

El correo solo se envía si:

- El WiFi con internet está conectado en el momento del intento.
- Hay un email de destino configurado.
- No se ha enviado ya el correo ese mismo día (clave NVS `lastEmail`).

**Si el ESP32 estaba apagado en la ventana de medianoche**, al arrancar entre las 0:01 y las 11:59 el sistema espera la ventana del mediodía para reintentar. Si arranca después de las 12:59 sin haber enviado, activa la alerta de reporte pendiente.

**Si el ESP32 estuvo apagado todo el día de envío** y arranca al día siguiente o más tarde, el sistema detecta el envío perdido comparando `lastEmail` con el último día de envío que debió haber ocurrido y activa la alerta de reporte pendiente si hay datos en los archivos de registro.

**Primera instalación:** En el primer arranque con código nuevo, el sistema registra la fecha de compilación como punto de partida para evitar falsos positivos. La primera fecha real de envío automático será el siguiente día programado (10, 20 o último del mes) después de la instalación.

### Contenido del correo

Tres archivos CSV adjuntos:

1. **Log de accesos** (`accesos_FECHAINICIO__FECHAFIN - N.csv`) — todos los eventos de acceso con timestamp, UID, nombre y código.
2. **Entradas/Salidas** (`entradas_FECHAINICIO__FECHAFIN - N.csv`) — registros de tipo Entrada, Salida y Salida Pendiente.
3. **Lista de usuarios** (`usuarios_FECHA - N.csv`) — todos los usuarios registrados al momento del envío.

El sufijo ` - N` es un contador de correos enviados en el día (empieza en 1 y se reinicia cada día); permite distinguir varios envíos manuales del mismo día sin sobrescribir adjuntos con el mismo nombre. El asunto del correo también incluye la fecha y ese mismo contador, ej.: `Registros NFC - 2026-07-18 - 2`.

### Qué ocurre después del envío automático

Tras un envío automático exitoso, el sistema **borra ambos archivos de registro** (`/logs.txt` y `/entradas.txt`) y **resetea a 0 los contadores** de todas las tarjetas. La lista de usuarios (NVS) **no se borra**.

El envío **manual** (botón "Enviar por email" en la web) genera el mismo correo pero **no borra los archivos**.

### Alerta de reporte pendiente

Si ambas ventanas del día de envío pasan sin que el correo se haya podido enviar (sin internet en todo el día, o el ESP32 estuvo apagado en ambas ventanas), el sistema activa una **bandera de reporte pendiente** que se guarda en NVS y persiste entre reinicios.

Mientras la bandera esté activa, **todas las páginas de la interfaz web** muestran un banner de advertencia:

> ⚠ **Correo con logs no enviado y archivos no eliminados de memoria. Por favor descárgalos y borra la memoria manualmente.**

El banner incluye un botón **✓ Confirmar**. Al hacer clic:
1. El navegador pide confirmación: *"¿Ya descargaste los logs y borraste la memoria?"*
2. Si el usuario confirma, la bandera se limpia y el mensaje desaparece.
3. Si cancela, el mensaje sigue apareciendo.

La responsabilidad de descargar los logs manualmente y borrar la memoria antes de confirmar es del usuario. Un envío automático exitoso posterior también limpia la bandera.

---

## Solución de problemas

### El PN532 no detecta tarjetas

**Síntoma:** La LCD muestra "NFC ERROR / Reiniciando..." o las tarjetas no se leen aunque estén bien acercadas.

**Causas y soluciones:**

- **PN532 no configurado en modo HSU/UART:** Verificar que los puentes (jumpers) o resistencias de selección de protocolo en el módulo PN532 estén en la posición de comunicación **serial (HSU)**, no I2C ni SPI (consultar el datasheet del módulo específico).
- **Conexiones cruzadas o sueltas:** Verificar RX2 (GPIO 16) del ESP32 conectado al TX del PN532, y TX2 (GPIO 17) del ESP32 conectado al RX del PN532. Si están invertidos, el chip no responde.
- **Chip sin respuesta puntual:** El sistema tiene un watchdog que verifica el PN532 cada 5 segundos y, si no responde, reinicia automáticamente el UART2 (`nfcReinicializar()`). Si falla 5 veces seguidas (~25 s sin recuperación), el ESP32 se reinicia solo.
- **Lecturas lentas repetidas:** Si el monitor serie muestra "NFC: lectura lenta (Xms)" de forma constante, puede indicar ruido en el cableado UART o una fuente de alimentación inestable para el PN532.
- **Tarjeta incompatible:** El sistema lee tarjetas Mifare ISO14443A. Tarjetas de otros protocolos (como ISO14443B) no son soportadas.

### El RTC pierde la hora al reiniciar

**Síntoma:** Después de un apagado o al encender, el timestamp vuelve a la fecha de compilación del firmware.

**Causas y soluciones:**

- **Batería CR2032 agotada o ausente:** Reemplazar la batería del módulo DS3231. Sin batería, el oscilador del DS3231 se detiene al quitar la alimentación principal y el chip pierde la hora.
- **Firmware nuevo cargado:** En cada nueva compilación y carga de firmware, el sistema detecta el cambio por el `buildID` y sincroniza el RTC con la hora de compilación. Esto es comportamiento normal. Configurar NTP para corregirlo.
- **Verificar conexiones del bus RTC:** SDA → GPIO 19, SCL → GPIO 18. Es un bus I2C independiente, distinto al de la LCD y sin relación con el PN532 (que usa UART).

### Problemas de autenticación SMTP (correo no se envía)

**Síntoma:** El LED parpadea en rojo al intentar enviar correo. En el monitor serie aparece "SMTP conexion FALLO" o "SMTP envio FALLO".

**Causas y soluciones:**

- **Contraseña incorrecta:** El campo `REMITENTE_CLAVE` en el código debe ser un **App Password** de Google de 16 caracteres (sin espacios), **no** la contraseña habitual de la cuenta Gmail.
- **Verificación en dos pasos no activada en Gmail:** Google requiere tener habilitada la verificación en dos pasos antes de poder generar App Passwords.
- **Sin conexión a internet:** Verificar que el WiFi esté conectado en la página de inicio (`/`). El ESP32 necesita conectividad STA (no solo el AP propio) para acceder a smtp.gmail.com.
- **Puerto bloqueado por la red:** Algunos routers o redes corporativas bloquean el puerto 587. Intentar con el puerto 465 y `SMTP_PORT 465` (SSL implícito requiere ajustar también `config.secure.startTLS = false`).

### El LED no enciende o enciende de forma incorrecta

**Síntoma:** El LED siempre está apagado, o los colores no corresponden (amarillo aparece como otro color, etc.).

**Causas y soluciones:**

- **Tipo de LED incorrecto:** Verificar si el LED es de **ánodo común** o **cátodo común** y ajustar `#define LED_ANODO_COMUN` en `src/main.cpp` (`true` para ánodo común, `false` para cátodo común — el valor por defecto del código es `false`).
- **Sin resistencias limitadoras:** Los pines GPIO del ESP32 no toleran corriente directa sin resistencia. Conectar una resistencia de 100–220 Ω en serie con cada canal (R, G, B).
- **Pines incorrectos:** Los pines del LED son GPIO 25 (R), GPIO 26 (G), GPIO 27 (B). Verificar conexiones.
- **Canal R y G invertidos:** El amarillo resulta de activar R y G simultáneamente. Si aparece un color distinto al esperado, los canales físicos pueden no corresponder al orden R-G-B del módulo. Intercambiar los pines en las constantes del código.

### La matriz de LEDs no enciende o muestra colores incorrectos

**Síntoma:** La matriz WS2812B permanece apagada, muestra píxeles de colores aleatorios, o solo enciende parcialmente.

**Causas y soluciones:**

- **Pin de datos incorrecto o suelto:** La matriz se controla por GPIO 23 (`MATRIZ_PIN`). Verificar la conexión DIN de la matriz a ese pin.
- **Alimentación insuficiente:** 64 LEDs WS2812B pueden exigir más corriente de la que entrega el regulador del ESP32 si el brillo fuera alto. El firmware limita el brillo a 60/255 precisamente para mitigar esto; si aun así hay parpadeo o colores erráticos, alimentar la matriz con una fuente de 5 V externa (compartiendo GND con el ESP32).
- **Orden de color equivocado:** El firmware asume LEDs `GRB` (estándar WS2812B). Si los colores aparecen intercambiados (verde donde debería ir rojo, etc.), el módulo podría usar un orden distinto.

### La LCD muestra caracteres extraños o está en blanco

**Síntoma:** La LCD muestra bloques, caracteres aleatorios, o no muestra nada con el backlight encendido.

**Causas y soluciones:**

- **Dirección I2C incorrecta:** La dirección predeterminada del módulo PCF8574 es `0x27`. Algunos módulos usan `0x3F`. Verificar con un escáner I2C o revisar las soldaduras A0/A1/A2 del módulo.
- **Contraste desajustado:** El módulo LCD tiene un potenciómetro de contraste (tornillo pequeño en la parte trasera). Ajustarlo hasta que los caracteres sean visibles.
- **Conexiones del bus Wire:** La LCD tiene su propio bus I2C (SDA GPIO 21, SCL GPIO 22), sin compartirlo con el PN532 (que usa UART) ni con el RTC (que usa su propio bus I2C en pines 19/18). Si el problema es intermitente, verificar las conexiones físicas de ese bus específico.

### Aparece el banner "Correo con logs no enviado"

**Síntoma:** Todas las páginas de la web muestran un banner amarillo de advertencia sobre un reporte no enviado.

**Cuándo aparece:**

- **Ambas ventanas de envío fallaron en el mismo día:** El sistema intentó a medianoche y al mediodía, pero ambas fallaron (sin WiFi o error SMTP). Los archivos no fueron borrados.
- **El sistema estuvo apagado el día de envío y arrancó después:** Si el ESP32 volvió a encender en un día posterior al día de envío y el correo nunca se mandó, el banner aparece al detectar el historial sin enviar.

> **Nota:** El banner persiste en todos los reinicios hasta que el usuario lo confirma manualmente. Es intencional que no desaparezca solo.

**Qué hacer:**

1. Ir a `/logs` y descargar el CSV de accesos (botón "Descargar CSV").
2. Ir a `/entradas` y descargar el CSV de entradas/salidas (botón "Descargar CSV").
3. Opcionalmente, usar el botón **"Enviar por email"** en `/logs`. Esto envía los archivos al correo configurado **pero no los borra** (el envío manual nunca borra archivos). Para borrar, ir a `/clearRegistros`.
4. Una vez que hayas guardado los datos y borrado la memoria (o decidido no hacerlo), hacer clic en **✓ Confirmar** del banner. El navegador preguntará "¿Ya descargaste los logs y borraste la memoria?". Al confirmar, el banner desaparece.

> **Importante:** El banner desaparece únicamente al hacer clic en **✓ Confirmar** y aceptar el diálogo. No desaparece solo al descargar ni al borrar los archivos — la confirmación es siempre explícita. Si el usuario confirma sin haber descargado los datos, el sistema acepta esa decisión.

### El sistema no detecta el cambio de día (Salidas Pendientes no aparecen)

**Síntoma:** Usuarios que registraron Entrada el día anterior no aparecen con "Salida: Pendiente" al día siguiente.

**Causas y soluciones:**

- **RTC sin batería:** Si el DS3231 pierde la hora, la detección del cambio de día falla. Verificar la batería.
- **Fecha del RTC incorrecta:** Sincronizar manualmente vía NTP desde `/config → Sincronizar hora por NTP`.
- **Sistema recién instalado sin historial:** El mecanismo de Salidas Pendientes requiere que los usuarios hayan sido leídos previamente (que exista la clave `f<UID>` en NVS). Usuarios nuevos que nunca han pasado tarjeta no generarán "Salida: Pendiente" en su primera lectura.
