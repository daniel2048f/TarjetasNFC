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
7. [Mensajes de la LCD](#mensajes-de-la-lcd)
8. [Comportamiento ante cortes de alimentación](#comportamiento-ante-cortes-de-alimentación)
9. [Sistema de correo automático](#sistema-de-correo-automático)
10. [Solución de problemas](#solución-de-problemas)

---

## Componentes de hardware

| Componente           | Referencia / Descripción                                     |
| -------------------- | ------------------------------------------------------------ |
| Microcontrolador     | ESP32 DevKit v1 (o compatible, 38 pines)                     |
| Lector NFC           | Adafruit PN532 NFC/RFID Controller (I2C)                     |
| Reloj en tiempo real | DS3231 RTC Module (con batería CR2032)                       |
| Pantalla             | LCD 20×4 caracteres con módulo I2C PCF8574 (dirección 0x27)  |
| LED indicador        | LED RGB de 4 pines, ánodo común o cátodo común               |
| Tarjetas             | Tarjetas o llaveros Mifare ISO14443A (compatibles con PN532) |

> El PN532 debe configurarse en modo **I2C** (puentes de selección de protocolo en posición I2C antes de instalarlo).

---

## Conexiones físicas

### Bus I2C 0 — Wire (pines 21/22): PN532 + LCD

| Señal | ESP32 GPIO | PN532       | LCD (PCF8574)                  |
| ----- | ---------- | ----------- | ------------------------------ |
| SDA   | 21         | SDA         | SDA                            |
| SCL   | 22         | SCL         | SCL                            |
| VCC   | 3.3 V      | VCC (3.3 V) | VCC (5 V o 3.3 V según módulo) |
| GND   | GND        | GND         | GND                            |

> Ambos dispositivos comparten el mismo bus I2C. El PN532 tiene dirección I2C 0x48 (por defecto en modo I2C) y el módulo LCD tiene dirección 0x27.

### Bus I2C 1 — busRtc (pines 19/18): DS3231

| Señal | ESP32 GPIO | DS3231 |
| ----- | ---------- | ------ |
| SDA   | 19         | SDA    |
| SCL   | 18         | SCL    |
| VCC   | 3.3 V      | VCC    |
| GND   | GND        | GND    |

> El DS3231 usa un bus I2C independiente para evitar interferencias con el PN532 y la LCD.

### LED RGB (pines 25/26/27)

El tipo de LED (ánodo o cátodo común) se configura con la constante `LED_ANODO_COMUN` en `src/main.cpp`. El valor por defecto es `true` (ánodo común).

**Ánodo común** (`LED_ANODO_COMUN true`, configuración por defecto):

| Pin LED           | Conexión                | ESP32 GPIO |
| ----------------- | ----------------------- | ---------- |
| Ánodo común (VCC) | 3.3 V                   | —          |
| R (rojo)          | Resistencia 100–220 Ω → | GPIO 25    |
| G (verde)         | Resistencia 100–220 Ω → | GPIO 26    |
| B (azul)          | Resistencia 100–220 Ω → | GPIO 27    |

**Cátodo común** (`LED_ANODO_COMUN false`):

| Pin LED            | Conexión                | ESP32 GPIO |
| ------------------ | ----------------------- | ---------- |
| Cátodo común (GND) | GND                     | —          |
| R (rojo)           | Resistencia 100–220 Ω → | GPIO 25    |
| G (verde)          | Resistencia 100–220 Ω → | GPIO 26    |
| B (azul)           | Resistencia 100–220 Ω → | GPIO 27    |

> Los GPIOs 25, 26 y 27 son de propósito general y no están usados por ningún otro periférico del sistema.

---

## Instalación del firmware

### Requisitos

- [PlatformIO](https://platformio.org/) (extensión de VS Code o CLI)
- Python 3.x instalado
- ESP32 conectado por USB

### Dependencias (se instalan automáticamente con PlatformIO)

| Librería          | Versión | Fuente                          |
| ----------------- | ------- | ------------------------------- |
| Adafruit PN532    | ^1.2.4  | adafruit/Adafruit PN532         |
| LiquidCrystal_I2C | ^1.1.4  | marcoschwartz/LiquidCrystal_I2C |
| ESP Mail Client   | ^3.4.19 | mobizt/ESP Mail Client          |
| RTClib            | ^2.1.4  | adafruit/RTClib                 |

Las siguientes librerías son parte del framework Arduino para ESP32 y **no** requieren declaración adicional:

`WiFi`, `WebServer`, `Preferences`, `Wire`, `LittleFS`

> El proyecto usa la partición `min_spiffs.csv` para maximizar el espacio disponible para LittleFS. Esta partición viene incluida en el framework de ESP32 para PlatformIO.

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
#define LED_ANODO_COMUN true   // true = ánodo común | false = cátodo común
```

---

## Configuración inicial

### Acceso a la red WiFi del dispositivo

El ESP32 crea un punto de acceso WiFi propio:

| Parámetro          | Valor         |
| ------------------ | ------------- |
| SSID               | `NFC`         |
| Contraseña         | `12345678`    |
| IP del dispositivo | `192.168.4.1` |

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
| `/sendEmail`        | GET    | Envía manualmente un correo con todos los registros adjuntos               |
| `/syncNtp`          | GET    | Sincroniza el reloj con servidores NTP manualmente                         |
| `/time`             | GET    | Devuelve la hora actual como texto (usado por el reloj en vivo de la web)  |

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

Después de 2 minutos sin actividad, el backlight de la LCD parpadea brevemente cada 30 segundos (3 s encendido / 1 s apagado) para reducir el desgaste. Cualquier lectura de tarjeta o acción desde la web restaura el backlight inmediatamente.

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

El sistema envía automáticamente un correo en los **días 10, 20 y el último día del mes**, a medianoche (hora 0). El correo solo se envía si:

- El WiFi con internet está conectado.
- Hay un email de destino configurado.
- No se ha enviado ya el correo ese mismo día (clave NVS `lastEmail`).

### Contenido del correo

Tres archivos CSV adjuntos:

1. **Log de accesos** (`accesos_FECHAINICIO__FECHAFIN.csv`) — todos los eventos de acceso con timestamp, UID, nombre y código.
2. **Entradas/Salidas** (`entradas_FECHAINICIO__FECHAFIN.csv`) — registros de tipo Entrada, Salida y Salida Pendiente.
3. **Lista de usuarios** (`usuarios_FECHA.csv`) — todos los usuarios registrados al momento del envío.

### Qué ocurre después del envío automático

Tras un envío automático exitoso, el sistema **borra ambos archivos de registro** (`/logs.txt` y `/entradas.txt`) y **resetea a 0 los contadores** de todas las tarjetas. La lista de usuarios (NVS) **no se borra**.

El envío **manual** (botón "Enviar por email" en la web) genera el mismo correo pero **no borra los archivos**.

### Limpieza automática de registros

Adicionalmente, los días **15 y el último día del mes** a medianoche, el sistema borra ambos archivos de registro y resetea contadores aunque no haya envío de correo configurado. Esta limpieza es independiente del correo.

---

## Solución de problemas

### El PN532 no detecta tarjetas

**Síntoma:** La LCD muestra "NFC ERROR / Revisar lector" o las tarjetas no se leen aunque estén bien acercadas.

**Causas y soluciones:**

- **PN532 no configurado en modo I2C:** Verificar que los dos puentes (jumpers) o resistencias de selección de protocolo en el módulo PN532 estén en la posición I2C (generalmente SEL0=ON/HIGH, SEL1=OFF/LOW — consultar el datasheet del módulo específico).
- **Bus I2C bloqueado:** El sistema tiene un watchdog que intenta recuperar el bus cada 15 segundos automáticamente. Si el problema persiste, reiniciar el ESP32.
- **Conexiones sueltas:** Verificar continuidad en SDA (GPIO 21) y SCL (GPIO 22) desde el ESP32 al PN532.
- **Tarjeta incompatible:** El sistema lee tarjetas Mifare ISO14443A. Tarjetas de otros protocolos (como ISO14443B) no son soportadas.

### El RTC pierde la hora al reiniciar

**Síntoma:** Después de un apagado o al encender, el timestamp vuelve a la fecha de compilación del firmware.

**Causas y soluciones:**

- **Batería CR2032 agotada o ausente:** Reemplazar la batería del módulo DS3231. Sin batería, el oscilador del DS3231 se detiene al quitar la alimentación principal y el chip pierde la hora.
- **Firmware nuevo cargado:** En cada nueva compilación y carga de firmware, el sistema detecta el cambio por el `buildID` y sincroniza el RTC con la hora de compilación. Esto es comportamiento normal. Configurar NTP para corregirlo.
- **Verificar conexiones del bus RTC:** SDA → GPIO 19, SCL → GPIO 18. Son pines distintos al bus del PN532.

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

- **Tipo de LED incorrecto:** Verificar si el LED es de **ánodo común** o **cátodo común** y ajustar `#define LED_ANODO_COMUN` en `src/main.cpp` (`true` para ánodo común, `false` para cátodo común).
- **Sin resistencias limitadoras:** Los pines GPIO del ESP32 no toleran corriente directa sin resistencia. Conectar una resistencia de 100–220 Ω en serie con cada canal (R, G, B).
- **Pines incorrectos:** Los pines del LED son GPIO 25 (R), GPIO 26 (G), GPIO 27 (B). Verificar conexiones.
- **Canal R y G invertidos:** El amarillo en ánodo común resulta de activar R y G simultáneamente. Si aparece un color distinto al esperado, los canales físicos pueden no corresponder al orden R-G-B del módulo. Intercambiar los pines en las constantes del código.

### La LCD muestra caracteres extraños o está en blanco

**Síntoma:** La LCD muestra bloques, caracteres aleatorios, o no muestra nada con el backlight encendido.

**Causas y soluciones:**

- **Dirección I2C incorrecta:** La dirección predeterminada del módulo PCF8574 es `0x27`. Algunos módulos usan `0x3F`. Verificar con un escáner I2C o revisar las soldaduras A0/A1/A2 del módulo.
- **Contraste desajustado:** El módulo LCD tiene un potenciómetro de contraste (tornillo pequeño en la parte trasera). Ajustarlo hasta que los caracteres sean visibles.
- **Bus I2C compartido con PN532 interrumpido:** El sistema incluye recuperación automática del bus. Si el problema es intermitente, el watchdog del PN532 debería resolverlo. Si es persistente, verificar las conexiones físicas.

### El sistema no detecta el cambio de día (Salidas Pendientes no aparecen)

**Síntoma:** Usuarios que registraron Entrada el día anterior no aparecen con "Salida: Pendiente" al día siguiente.

**Causas y soluciones:**

- **RTC sin batería:** Si el DS3231 pierde la hora, la detección del cambio de día falla. Verificar la batería.
- **Fecha del RTC incorrecta:** Sincronizar manualmente vía NTP desde `/config → Sincronizar hora por NTP`.
- **Sistema recién instalado sin historial:** El mecanismo de Salidas Pendientes requiere que los usuarios hayan sido leídos previamente (que exista la clave `f<UID>` en NVS). Usuarios nuevos que nunca han pasado tarjeta no generarán "Salida: Pendiente" en su primera lectura.
