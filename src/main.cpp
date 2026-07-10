#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LittleFS.h>
#include <PN532_HSU.h>
#include <PN532.h>
#include <FastLED.h>
#include <LiquidCrystal_I2C.h>
#include <ESP_Mail_Client.h>
#include <RTClib.h>
#include "nvs.h"     // iterador NVS para reconstruir ARCHIVO_UIDS al arrancar

// ── Hardware ──────────────────────────────────────────────────
// I2C: busRtc(SDA=19,SCL=18)→RTC DS3231 | Wire(SDA=21,SCL=22)→LCD(0x27)
// UART2: RX2=GPIO16, TX2=GPIO17 → PN532 en modo HSU
#define RTC_SDA       19
#define RTC_SCL       18
#define ARCHIVO_LOG   "/logs.txt"
#define ARCHIVO_ENT   "/entradas.txt"
#define ARCHIVO_UIDS  "/uids.txt"

// ── LED RGB ───────────────────────────────────────────────────────────────────
// GPIOs libres (no usados por I2C ni otros perifericos del sistema).
// Pines ocupados: 18,19 (I2C RTC), 21,22 (I2C LCD), 16,17 (UART2 PN532).
#define LED_PIN_R       25
#define LED_PIN_G       26
#define LED_PIN_B       27
// true  = anodo comun   (HIGH = apagado, LOW = encendido en cada canal)
// false = catodo comun  (LOW  = apagado, HIGH = encendido en cada canal)
#define LED_ANODO_COMUN false

// ── Credenciales SMTP del remitente (cambiar antes de compilar) ──
#define SMTP_HOST        "smtp.gmail.com"
#define SMTP_PORT        587   // 587=STARTTLS (Gmail recomendado) | 465=SSL implicito
#define REMITENTE_EMAIL  "rafael.arias@institutotebaida.edu.co"
#define REMITENTE_CLAVE  "dqanxyjsspeotimc"  // App Password de Gmail (16 chars, sin espacios)

TwoWire           busRtc = TwoWire(1);
RTC_DS3231        rtcDs3231;
PN532_HSU         pn532hsu(Serial2);
PN532             lectorNfc(pn532hsu);
LiquidCrystal_I2C lcd(0x27, 20, 4);

const char* SSID_AP  = "NFC";
const char* CLAVE_AP = "12345678";
WebServer   servidor(80);
Preferences almacen;
SMTPSession smtp; 

String nombrePendiente      = "";
String codigoPendiente      = "";
bool   esperandoTarjeta     = false;
bool   modoEliminar         = false;
bool   resultadoEliminacion = false;
String        ultimoUid        = "";
bool          tarjetaPresente  = false;
unsigned long tiempoUltimoUid  = 0;

unsigned long ultimaLecturaNfc      = 0;
unsigned long ultimoChequeoLimpieza = 0;
unsigned long ultimaReconexionWifi  = 0;
const unsigned long INTERVALO_NFC       = 300;
const unsigned long INTERVALO_LIMPIEZA  = 60000;
const unsigned long INTERVALO_WIFI      = 30000;
// Tiempo minimo entre dos lecturas validas del mismo UID (anti-rebote + cooldown).
const unsigned long COOLDOWN_TARJETA = 3000;
bool rtcDisponible = false;

// ── Estado NTP ───────────────────────────────────────────────
bool          ntpPendiente      = false;
String        ntpEstado         = "No sincronizado";
unsigned long ultimaNtpSync     = 0;
bool          wifiConectadoPrev = false;

// ── Estado email ─────────────────────────────────────────────
String emailEstado        = "Sin intentos";
String emailUltimoTs      = "";
bool   emailPendienteFlag = false;  // true si ambas ventanas de auto-envio fallaron sin enviar

// ── Timer LCD no bloqueante ───────────────────────────────────
unsigned long tiempoLcdHasta = 0;
enum LcdPost { LCD_IDLE, LCD_LISTO };
LcdPost lcdPost = LCD_IDLE;

// ── Proteccion LCD: titileo no bloqueante en estado idle ──────
// Despues de LCD_IDLE_BLINK_INICIO ms sin actividad, el backlight parpadea
// brevemente cada 30 s para reducir el desgaste del panel.
// Solo ocurre en idle; cualquier lectura NFC lo cancela (lcdWakeUp).
bool          lcdBacklightOn  = true;
unsigned long lcdIdleDesde    = 0;     // millis() al entrar en idle; 0 = no idle
unsigned long lcdParpadeoNext = 0;     // proxima inversion del backlight
const unsigned long LCD_IDLE_BLINK_INICIO = 5000UL; // 5 seg idle antes de titular
const unsigned long LCD_BLINK_ON_MS       = 100UL;   // ms con luz encendida
const unsigned long LCD_BLINK_OFF_MS      = 30UL;    // ms con luz apagada

// ── PN532 watchdog ────────────────────────────────────────────
// El PN532 puede quedar en estado inconsistente tras operaciones de borrado
// o si hay ruido en el bus UART. El watchdog verifica el chip cada NFC_CHECK_INTERVALO ms;
// si no responde, reinicializa el puerto UART2 y el chip automaticamente.
unsigned long nfcUltimoCheck = 0;
const unsigned long NFC_CHECK_INTERVALO = 5000UL;   // verificar salud cada 5 s

// ── LED RGB: maquina de estados no bloqueante ─────────────────────────────────
// Estados:
//   LED_IDLE     → amarillo fijo (espera normal)
//   LED_FIJO     → color fijo durante 'ledFijoHasta' ms, luego vuelve a IDLE
//   LED_PARPADEO → N destellos del color activo, luego vuelve a IDLE
struct LedColor { bool r, g, b; };
const LedColor LED_APAGADO  = {false, false, false};
const LedColor LED_AMARILLO = {true,  true,  false};
const LedColor LED_VERDE    = {false, true,  false};
const LedColor LED_ROJO     = {true,  false, false};

enum LedEstado { LED_IDLE, LED_FIJO, LED_PARPADEO };
LedEstado     ledEstado      = LED_IDLE;
LedColor      ledColorParpad = LED_VERDE;  // color del parpadeo en curso
unsigned long ledFijoHasta   = 0;          // millis() hasta cuando mantener LED_FIJO
int           ledCiclosRest  = 0;          // destellos restantes en LED_PARPADEO
bool          ledFaseOn      = false;      // fase actual: true=encendido, false=apagado
unsigned long ledProxCambio  = 0;          // millis() del proximo cambio de fase

const unsigned long LED_BLINK_ON_MS  = 250;  // duracion fase encendida en parpadeo
const unsigned long LED_BLINK_OFF_MS = 200;  // duracion fase apagada en parpadeo
const int           LED_BLINK_N      =   5;  // numero de destellos por evento

// ── LED RGB: funciones ───────────────────────────────────────────────────────
// Escribe el color directamente respetando el tipo de LED (anodo/catodo comun).
void ledAplicar(LedColor c) {
  bool inv = LED_ANODO_COMUN;
  digitalWrite(LED_PIN_R, inv ? !c.r : c.r);
  digitalWrite(LED_PIN_G, inv ? !c.g : c.g);
  digitalWrite(LED_PIN_B, inv ? !c.b : c.b);
}

// Activa un color fijo temporal; regresa a amarillo transcurridos 'ms' ms.
void ledFijar(LedColor color, unsigned long ms) {
  ledEstado    = LED_FIJO;
  ledFijoHasta = millis() + ms;
  ledAplicar(color);
}

// Inicia secuencia de 'n' destellos del color dado; regresa a amarillo al terminar.
void ledParpadear(LedColor color, int n) {
  ledEstado      = LED_PARPADEO;
  ledColorParpad = color;
  ledCiclosRest  = n;
  ledFaseOn      = true;
  ledProxCambio  = millis() + LED_BLINK_ON_MS;
  ledAplicar(color);
}

// Actualizar maquina de estados; llamar en cada iteracion de loop().
void ledActualizar() {
  unsigned long ahora = millis();
  switch (ledEstado) {
    case LED_IDLE:
      break;  // amarillo ya aplicado al entrar en este estado

    case LED_FIJO:
      if (ahora >= ledFijoHasta) {
        ledEstado = LED_IDLE;
        ledAplicar(LED_AMARILLO);
      }
      break;

    case LED_PARPADEO:
      if (ahora < ledProxCambio) break;
      if (ledFaseOn) {
        // Fase encendida terminada: pasar a fase apagada
        ledFaseOn     = false;
        ledProxCambio = ahora + LED_BLINK_OFF_MS;
        ledAplicar(LED_APAGADO);
      } else {
        // Fase apagada terminada: decrementar y decidir si continuar
        if (--ledCiclosRest <= 0) {
          ledEstado = LED_IDLE;
          ledAplicar(LED_AMARILLO);
        } else {
          ledFaseOn     = true;
          ledProxCambio = ahora + LED_BLINK_ON_MS;
          ledAplicar(ledColorParpad);
        }
      }
      break;
  }
}

void ledSetup() {
  pinMode(LED_PIN_R, OUTPUT);
  pinMode(LED_PIN_G, OUTPUT);
  pinMode(LED_PIN_B, OUTPUT);
  ledAplicar(LED_AMARILLO);  // estado inicial: espera normal
}

// ── Matriz WS2812B 8×8 ───────────────────────────────────────────────────────
// 64 LEDs en topologia serpentina. Pin de datos: GPIO23.
// Filas pares  (0,2,4,6): izq→der.  Filas impares (1,3,5,7): der→izq.
#define MATRIZ_PIN    23
#define MATRIZ_LEDS   64
#define MATRIZ_BRILLO 60   // 0-255; suficiente para indicadores sin calentar la matriz

CRGB          matrizLeds[MATRIZ_LEDS];
unsigned long matrizHasta = 0;   // ms hasta cuando mostrar el patron; 0 = permanente

inline int matrizIdx(int col, int fila) {
  return fila * 8 + col;  // todas las filas izq->der (sin serpentina en impares)
}

// Marco de tarjeta — sistema disponible, esperando lectura (idle)
const uint8_t PATRON_DISPONIBLE[8] = {
  0b00000000,
  0b01111110,
  0b01000010,
  0b01011010,
  0b01011010,
  0b01000010,
  0b01111110,
  0b00000000,
};

// Flecha apuntando arriba — acceso permitido / tarjeta registrada
const uint8_t PATRON_FLECHA_ARRIBA[8] = {
  0b00011000,
  0b00111100,
  0b01111110,
  0b11111111,
  0b00011000,
  0b00011000,
  0b00011000,
  0b00011000,
};

// X roja — acceso denegado / tarjeta no registrada / error
const uint8_t PATRON_X[8] = {
  0b11000011,
  0b01100110,
  0b00111100,
  0b00011000,
  0b00011000,
  0b00111100,
  0b01100110,
  0b11000011,
};

// Chulito (checkmark) — exito: correo enviado, registro OK, borrado OK
const uint8_t PATRON_CHECK[8] = {
  0b00000000,
  0b00000001,
  0b00000011,
  0b00000110,
  0b11001100,
  0b01111000,
  0b00110000,
  0b00000000,
};

void matrizMostrar(const uint8_t patron[8], CRGB color, unsigned long duracionMs = 0) {
  for (int fila = 0; fila < 8; fila++) {
    uint8_t bits = patron[fila];
    for (int col = 0; col < 8; col++) {
      matrizLeds[matrizIdx(col, fila)] = ((bits >> (7 - col)) & 1) ? color : CRGB::Black;
    }
  }
  FastLED.show();
  matrizHasta = (duracionMs > 0) ? millis() + duracionMs : 0;
}

// Llamar en loop(): revierte al patron idle cuando expira el temporizador.
void matrizActualizar() {
  if (matrizHasta && millis() >= matrizHasta) {
    matrizHasta = 0;
    matrizMostrar(PATRON_DISPONIBLE, CRGB(100, 80, 0));  // amarillo tenue = idle
  }
}

// Cancela el modo idle y garantiza backlight encendido.
// Llamar antes de mostrar cualquier informacion activa en la LCD.
void lcdWakeUp() {
  lcdIdleDesde    = 0;
  lcdParpadeoNext = 0;
  if (!lcdBacklightOn) { lcd.backlight(); lcdBacklightOn = true; }
}

// Llama desde loop() en cada iteracion para gestionar el parpadeo.
void lcdActualizarParpadeo() {
  if (!lcdIdleDesde) return;
  unsigned long ahora = millis();
  if (ahora - lcdIdleDesde < LCD_IDLE_BLINK_INICIO) {
    if (!lcdBacklightOn) { lcd.backlight(); lcdBacklightOn = true; }
    return;
  }
  if (!lcdParpadeoNext) lcdParpadeoNext = ahora + LCD_BLINK_ON_MS;
  if (ahora >= lcdParpadeoNext) {
    if (lcdBacklightOn) {
      lcd.noBacklight(); lcdBacklightOn = false;
      lcdParpadeoNext = ahora + LCD_BLINK_OFF_MS;
    } else {
      lcd.backlight();   lcdBacklightOn = true;
      lcdParpadeoNext = ahora + LCD_BLINK_ON_MS;
    }
  }
}

// ── LCD ──────────────────────────────────────────────────────
void lcdMostrar(String l1, String l2, String l3, String l4) {
  lcd.clear();
  String ls[4] = { l1, l2, l3, l4 };
  for (int i = 0; i < 4; i++) {
    lcd.setCursor(0, i);
    if (ls[i].length() > 20) ls[i] = ls[i].substring(0, 20);
    lcd.print(ls[i]);
  }
}

void lcdMostrarNombre(String nombreCompleto, String codigo) {
  String tok[4] = {"", "", "", ""};
  int n = 0, ini = 0;
  for (int i = 0; i <= (int)nombreCompleto.length() && n < 4; i++) {
    if (i == (int)nombreCompleto.length() || nombreCompleto[i] == ' ') {
      if (i > ini) tok[n++] = nombreCompleto.substring(ini, i);
      ini = i + 1;
    }
  }
  // Linea 4: token[3] hasta col 14, codigo alineado a la derecha en cols 14-19
  String linea4 = tok[3].substring(0, min((int)tok[3].length(), 14));
  while ((int)linea4.length() < 14) linea4 += ' ';
  codigo = codigo.substring(0, min((int)codigo.length(), 6));
  String codAli = "";
  for (int i = (int)codigo.length(); i < 6; i++) codAli += ' ';
  codAli += codigo;
  lcdMostrar(tok[0], tok[1], tok[2], linea4 + codAli);
}

// ── RTC DS3231 (via RTClib) ───────────────────────────────────
void rtcAjustarHora(int anio, int mes, int dia, int hora, int minuto, int segundo) {
  rtcDs3231.adjust(DateTime(anio, mes, dia, hora, minuto, segundo));
}

String obtenerTimestamp() {
  if (!rtcDisponible) return "RTC-ERROR";
  DateTime now = rtcDs3231.now();
  int anio = now.year(), mes = now.month(), dia = now.day();
  int hora = now.hour(), minuto = now.minute(), seg = now.second();
  if (anio < 2020 || anio > 2099 || mes < 1 || mes > 12 ||
      dia < 1 || dia > 31 || hora > 23 || minuto > 59 || seg > 59) {
    Serial.println("RTC ERROR: valores invalidos");
    return "RTC-ERROR";
  }
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           anio, mes, dia, hora, minuto, seg);
  return String(buf);
}

void parsearCompilacion(int &anio, int &mes, int &dia, int &hora, int &minuto, int &segundo) {
  const char *fecha = __DATE__, *tiempo = __TIME__;
  dia  = atoi(fecha + 4);
  anio = atoi(fecha + 7);
  char nombreMes[4] = { fecha[0], fecha[1], fecha[2], 0 };
  const char* meses = "JanFebMarAprMayJunJulAugSepOctNovDec";
  mes     = (strstr(meses, nombreMes) - meses) / 3 + 1;
  hora    = atoi(tiempo);
  minuto  = atoi(tiempo + 3);
  segundo = atoi(tiempo + 6);
}

void rtcSincronizarSiNecesario() {
  int anio, mes, dia, hora, minuto, segundo;
  parsearCompilacion(anio, mes, dia, hora, minuto, segundo);
  String buildIdActual = String(__DATE__) + __TIME__;
  bool firmwareNuevo = (almacen.getString("buildID", "") != buildIdActual);
  bool perdioAlim    = rtcDs3231.lostPower();
  if (firmwareNuevo || perdioAlim) {
    rtcAjustarHora(anio, mes, dia, hora, minuto, segundo);
    if (firmwareNuevo) {
      almacen.putString("buildID", buildIdActual);
      // En primer arranque absoluto (lastEmail vacio), inicializar lastEmail a la fecha de
      // compilacion para evitar falso positivo de "envio perdido" si la carga ocurre en
      // un dia de envio. La primera fecha real de envio sera la proxima programada.
      if (!almacen.getString("lastEmail", "").length()) {
        char fechaComp[11]; snprintf(fechaComp, sizeof(fechaComp), "%04d-%02d-%02d", anio, mes, dia);
        almacen.putString("lastEmail", String(fechaComp));
        Serial.println("RTC: primer arranque — lastEmail inicializado a " + String(fechaComp));
      }
    }
    Serial.println(firmwareNuevo ? "RTC: hora actualizada (nuevo firmware)."
                                 : "RTC: hora actualizada (perdida de alimentacion).");
  } else {
    Serial.println("RTC: misma compilacion, hora intacta.");
  }
  Serial.println("RTC: " + obtenerTimestamp());
}

struct FechaHora { int anio, mes, dia, hora, minuto, segundo; };
FechaHora rtcLeerFechaHora() {
  FechaHora fh = {0, 0, 0, 0, 0, 0};
  if (!rtcDisponible) return fh;
  DateTime now = rtcDs3231.now();
  fh.anio    = now.year();   fh.mes     = now.month();  fh.dia    = now.day();
  fh.hora    = now.hour();   fh.minuto  = now.minute(); fh.segundo = now.second();
  return fh;
}

int ultimoDiaDelMes(int mes, int anio) {
  const int dias[] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
  bool bisiesto = (anio % 4 == 0 && (anio % 100 != 0 || anio % 400 == 0));
  return dias[mes] + (mes == 2 && bisiesto ? 1 : 0);
}

// ── HTML ─────────────────────────────────────────────────────
// Banner de alerta cuando el auto-envio de correo fallo en ambas ventanas del dia.
// Se inserta al inicio del body en todas las paginas mientras la bandera este activa.
String bannerAlerta() {
  if (!emailPendienteFlag) return "";
  return "<div style='background:#fff3cd;border:2px solid #e6a817;border-radius:8px;"
         "padding:14px 16px;margin:0 0 16px 0'>"
         "<b>&#9888; Correo con logs no enviado y archivos no eliminados de memoria. "
         "Por favor desc&aacute;rgalos y borra la memoria manualmente.</b> "
         "<a href='/confirmarEmailPend' "
         "onclick='return confirm(\"Ya descargaste los logs y borraste la memoria?\")'>"
         "<button style='background:#28a745;color:white;font-weight:bold;padding:8px 14px;"
         "border:none;border-radius:6px;cursor:pointer'>&#10003; Confirmar</button>"
         "</a></div>";
}

const char ESTILOS[] =
  "<style>body{font-family:Arial;margin:24px;max-width:720px}"
  "a,button,input{font-size:18px}button{padding:10px 14px;cursor:pointer}"
  "input{padding:10px;width:100%;box-sizing:border-box}"
  ".card{border:1px solid #ddd;border-radius:10px;padding:16px;margin:12px 0}"
  ".muted{color:#666;font-size:14px}.ts{color:#444;font-size:13px;font-family:monospace}"
  ".btn-danger{background:#dc3545;color:white;font-weight:bold}"
  ".btn-ok{background:#28a745;color:white;font-weight:bold}"
  ".btn-email{background:#0066cc;color:white;font-weight:bold}"
  ".entrada{color:#28a745;font-weight:bold}.salida{color:#dc3545;font-weight:bold}"
  ".pendiente{color:#ff9800;font-weight:bold}"
  ".btn-home{background:#555;color:white;border:none;border-radius:20px;"
  "padding:6px 13px;font-size:14px;cursor:pointer;opacity:0.88}</style>";

String pagina(const String& titulo, const String& cuerpo) {
  return "<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>" + titulo + "</title>" + ESTILOS + "</head><body>"
         "<a href='/' style='position:fixed;top:12px;right:12px;z-index:999;text-decoration:none'>"
         "<button class='btn-home'>&#127968; Inicio</button></a>"
         + bannerAlerta() + cuerpo + "</body></html>";
}

// ── Logs de acceso (LittleFS) ─────────────────────────────────
void logAgregar(const String& ts, const String& uid, const String& nombre, const String& codigo) {
  File f = LittleFS.open(ARCHIVO_LOG, "a");
  if (f) { f.println(ts + "|" + uid + "|" + nombre + "|" + codigo); f.close(); }
}

// Parsea "timestamp|uid|nombre|codigo". 4to campo opcional (compatibilidad legacy).
bool logParsear(const String& entrada, String &ts, String &uid, String &nombre, String &codigo) {
  int pos1 = entrada.indexOf('|'), pos2 = entrada.indexOf('|', pos1 + 1);
  if (pos1 < 0 || pos2 < 0) return false;
  ts  = entrada.substring(0, pos1);
  uid = entrada.substring(pos1+1, pos2);
  int pos3 = entrada.indexOf('|', pos2 + 1);
  if (pos3 < 0) { nombre = entrada.substring(pos2+1); codigo = ""; }
  else           { nombre = entrada.substring(pos2+1, pos3); codigo = entrada.substring(pos3+1); }
  return true;
}

// Mensaje de error de memoria reutilizable en logHtml / entradaHtml.
static String errorMemoria(const char* descarga, const char* prefijo) {
  return String("<div class='card' style='border-color:#dc3545'>"
         "<p><b>&#9888; Memoria insuficiente para mostrar los registros.</b></p>"
         "<p class='muted'>Descargue el CSV y luego borre los registros para liberar espacio.</p>"
         "<a href='") + descarga + "'><button class='btn-ok'>&#128229; Descargar CSV</button></a> "
         "<a href='/clearRegistros' onclick='return confirm(\"Borrar TODOS los registros?\")'>"
         "<button class='btn-danger'>&#128465; Borrar registros</button></a></div>";
}

String logHtml() {
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (!f || !f.size())
    return "<div class='card'><div class='muted'>No hay eventos registrados.</div></div>";
  if (ESP.getFreeHeap() < 45000) {
    f.close();
    return errorMemoria("/downloadLogs", "accesos");
  }
  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.seek(0);
  int mostrar = min(total, 300), saltar = total - mostrar;
  String* buf = new String[mostrar];
  if (!buf) { f.close(); return errorMemoria("/downloadLogs", "accesos"); }
  int n = 0, fila = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    if (fila++ < saltar) continue;
    buf[n++] = l;
  }
  f.close();
  String r = "<div class='card'><div class='muted'>Total: " + String(total) + " eventos";
  if (total > 300) r += " (mostrando los ultimos 300)";
  r += "</div><hr>";
  for (int i = n - 1; i >= 0; i--) {
    String ts, uid, nombre, codigo;
    if (!logParsear(buf[i], ts, uid, nombre, codigo)) continue;
    r += "<div><b>" + nombre + "</b>";
    if (codigo.length()) r += " <span class='muted'>[" + codigo + "]</span>";
    r += " <span class='muted'>(UID: " + uid + ")</span><br>"
         "<span class='ts'>&#128336; " + ts + "</span></div><hr>";
  }
  delete[] buf;
  return r + "</div>";
}

String logCsv() {
  String r = "Timestamp,UID,Nombre,Codigo\n";
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (!f || !f.size()) return r;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    String ts, uid, nombre, codigo;
    if (!logParsear(l, ts, uid, nombre, codigo)) continue;
    r += ts + "," + uid + "," + nombre + "," + codigo + "\n";
  }
  f.close();
  return r;
}

// ── Entradas / Salidas (LittleFS) ────────────────────────────
void entradaAgregar(const String& ts, const String& uid, const String& nombre,
                    const String& codigo, const String& tipo) {
  File f = LittleFS.open(ARCHIVO_ENT, "a");
  if (f) { f.println(ts + "|" + uid + "|" + nombre + "|" + codigo + "|" + tipo); f.close(); }
}

bool entradaParsear(const String& linea, String &ts, String &uid, String &nombre,
                    String &codigo, String &tipo) {
  int p1 = linea.indexOf('|'), p2 = linea.indexOf('|', p1+1);
  int p3 = linea.indexOf('|', p2+1), p4 = linea.indexOf('|', p3+1);
  if (p1<0 || p2<0 || p3<0 || p4<0) return false;
  ts     = linea.substring(0, p1);
  uid    = linea.substring(p1+1, p2);
  nombre = linea.substring(p2+1, p3);
  codigo = linea.substring(p3+1, p4);
  tipo   = linea.substring(p4+1); tipo.trim();
  return true;
}

String entradaHtml() {
  File f = LittleFS.open(ARCHIVO_ENT, "r");
  if (!f || !f.size())
    return "<div class='card'><div class='muted'>No hay registros de entradas/salidas.</div></div>";
  if (ESP.getFreeHeap() < 45000) {
    f.close();
    return errorMemoria("/downloadEntradas", "entradas");
  }
  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.seek(0);
  int mostrar = min(total, 300), saltar = total - mostrar;
  String* buf = new String[mostrar];
  if (!buf) { f.close(); return errorMemoria("/downloadEntradas", "entradas"); }
  int n = 0, fila = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    if (fila++ < saltar) continue;
    buf[n++] = l;
  }
  f.close();

  const int MAX_UIDS = 100;
  String uids[MAX_UIDS]; int nu = 0;
  for (int i = 0; i < n && nu < MAX_UIDS; i++) {
    String ts, uid, nombre, codigo, tipo;
    if (!entradaParsear(buf[i], ts, uid, nombre, codigo, tipo)) continue;
    bool existe = false;
    for (int j = 0; j < nu; j++) { if (uids[j] == uid) { existe = true; break; } }
    if (!existe) uids[nu++] = uid;
  }

  String r = "<div class='card'><div class='muted'>Total: " + String(total) + " registros";
  if (total > 300) r += " (mostrando los ultimos 300)";
  r += "</div><hr>";

  for (int u = 0; u < nu; u++) {
    String nombreGrupo = uids[u];
    for (int i = 0; i < n; i++) {
      String ts, uid, nombre, codigo, tipo;
      if (!entradaParsear(buf[i], ts, uid, nombre, codigo, tipo)) continue;
      if (uid == uids[u]) { nombreGrupo = nombre; break; }
    }
    r += "<div class='card' style='margin:6px 0'>"
         "<b>" + nombreGrupo + "</b> <span class='muted'>(UID: " + uids[u] + ")</span>";
    for (int i = 0; i < n; i++) {
      String ts, uid, nombre, codigo, tipo;
      if (!entradaParsear(buf[i], ts, uid, nombre, codigo, tipo)) continue;
      if (uid != uids[u]) continue;
      if (tipo == "Salida: Pendiente") {
        r += "<div style='padding:3px 0 3px 8px'>"
             "<span class='pendiente'>&#x25cf; Salida: Pendiente</span>"
             " <span class='muted'>(sin registro de salida)</span>";
      } else {
        String cls = (tipo == "Entrada") ? "entrada" : "salida";
        r += "<div style='padding:3px 0 3px 8px'>"
             "<span class='" + cls + "'>&#x25cf; " + tipo + "</span>"
             " <span class='ts'>&#128336; " + ts + "</span>";
      }
      if (codigo.length()) r += " <span class='muted'>[" + codigo + "]</span>";
      r += "</div>";
    }
    r += "</div>";
  }
  delete[] buf;
  return r + "</div>";
}

String entradaCsv() {
  String r = "Timestamp,UID,Nombre,Codigo,Tipo\n";
  File f = LittleFS.open(ARCHIVO_ENT, "r");
  if (!f || !f.size()) return r;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    String ts, uid, nombre, codigo, tipo;
    if (!entradaParsear(l, ts, uid, nombre, codigo, tipo)) continue;
    r += ts + "," + uid + "," + nombre + "," + codigo + "," + tipo + "\n";
  }
  f.close();
  return r;
}

// ── Usuarios (ARCHIVO_UIDS + NVS) ────────────────────────────
// Agrega un UID a ARCHIVO_UIDS si no esta ya presente.
void uidRegistrar(const String& uid) {
  File f = LittleFS.open(ARCHIVO_UIDS, "r");
  if (f) {
    while (f.available()) {
      String l = f.readStringUntil('\n'); l.trim();
      if (l == uid) { f.close(); return; }
    }
    f.close();
  }
  f = LittleFS.open(ARCHIVO_UIDS, "a");
  if (f) { f.println(uid); f.close(); }
}

// Elimina un UID de ARCHIVO_UIDS reescribiendo el archivo sin esa linea.
void uidEliminar(const String& uid) {
  File fin = LittleFS.open(ARCHIVO_UIDS, "r");
  if (!fin) return;
  String contenido = "";
  while (fin.available()) {
    String l = fin.readStringUntil('\n'); l.trim();
    if (l.length() && l != uid) contenido += l + "\n";
  }
  fin.close();
  File fout = LittleFS.open(ARCHIVO_UIDS, "w");
  if (fout) { fout.print(contenido); fout.close(); }
}

// Reconstruye ARCHIVO_UIDS leyendo el namespace "nfc" de NVS con el iterador de
// ESP-IDF. Se llama en setup() para que los usuarios registrados antes de este
// firmware (que solo existen en NVS) aparezcan en /usuarios desde el primer arranque.
// Las claves UID son cadenas hex en MAYUSCULAS (0-9, A-F), 6-14 chars, sin prefijos.
// Las demas claves del namespace ("buildID", "lastClean", "k"+uid, etc.) tienen al
// menos un caracter minusculo o especial, por lo que no se confunden con UIDs.
void reconstruirArchivoUids() {
  // API de ESP-IDF 4.x: nvs_entry_find retorna el iterador directamente (no esp_err_t).
  // nvs_entry_next avanza y retorna el siguiente; cuando agota, libera y retorna NULL.
  nvs_iterator_t it = nvs_entry_find("nvs", "nfc", NVS_TYPE_STR);

  File fout = LittleFS.open(ARCHIVO_UIDS, "w");
  int n = 0;

  while (it != nullptr) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);  // void en la API 4.x
    String key = String(info.key);
    // UID = cadena hex en MAYUSCULAS (0-9, A-F), 6-14 chars.
    // Ningun key del sistema cumple esta condicion: todos tienen al menos un
    // caracter minusculo ("buildID", "lastClean", "k"+uid, "emailDest", etc.).
    bool esUid = (key.length() >= 6 && key.length() <= 14);
    for (int i = 0; esUid && i < (int)key.length(); i++) {
      char c = key[i];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) esUid = false;
    }
    if (esUid && fout) { fout.println(key); n++; }
    it = nvs_entry_next(it);  // retorna NULL al agotar; el iterador queda liberado
  }
  if (fout) fout.close();
  Serial.printf("UIDS: %d usuarios reconstruidos desde NVS\n", n);
}

String usuariosCsv() {
  String r = "UID,Nombre,Codigo\n";
  File f = LittleFS.open(ARCHIVO_UIDS, "r");
  if (!f) return r;
  while (f.available()) {
    String uid = f.readStringUntil('\n'); uid.trim();
    if (!uid.length()) continue;
    String nombre = almacen.getString(uid.c_str(), "");
    if (!nombre.length()) continue;
    String codigo = almacen.getString(("k" + uid).c_str(), "");
    r += uid + "," + nombre + "," + codigo + "\n";
  }
  f.close();
  return r;
}

String usuariosHtml() {
  File f = LittleFS.open(ARCHIVO_UIDS, "r");
  if (!f || !f.size())
    return "<div class='card'><div class='muted'>No hay usuarios registrados.</div></div>";
  String r = "<div class='card'>"
    "<table style='width:100%;border-collapse:collapse;font-size:15px'>"
    "<tr style='background:#f5f5f5'>"
    "<th style='padding:6px;text-align:left'>UID</th>"
    "<th style='padding:6px;text-align:left'>Nombre</th>"
    "<th style='padding:6px;text-align:left'>Codigo</th></tr>";
  int total = 0;
  while (f.available()) {
    String uid = f.readStringUntil('\n'); uid.trim();
    if (!uid.length()) continue;
    String nombre = almacen.getString(uid.c_str(), "");
    if (!nombre.length()) continue;
    String codigo = almacen.getString(("k" + uid).c_str(), "");
    r += "<tr><td style='padding:5px;font-family:monospace' class='muted'>" + uid +
         "</td><td style='padding:5px'><b>" + nombre +
         "</b></td><td style='padding:5px'>" + codigo + "</td></tr>";
    total++;
  }
  f.close();
  return r + "</table><div class='muted' style='margin-top:8px'>Total: "
           + String(total) + " usuarios</div></div>";
}

// ── NTP ──────────────────────────────────────────────────────
void aplicarTiempoNtp(struct tm& info) {
  int anio = info.tm_year + 1900, mes  = info.tm_mon  + 1;
  int dia  = info.tm_mday,        hora = info.tm_hour;
  int min  = info.tm_min,         seg  = info.tm_sec;
  if (anio < 2024 || anio > 2099) {
    ntpEstado = "NTP: anio invalido (" + String(anio) + ")";
    Serial.println(ntpEstado); return;
  }
  if (rtcDisponible) rtcAjustarHora(anio, mes, dia, hora, min, seg);
  ultimaNtpSync = millis();
  ntpEstado = "Sincronizado: " + obtenerTimestamp();
  Serial.println("NTP OK -> " + ntpEstado);
}

void iniciarNtp() {
  if (WiFi.status() != WL_CONNECTED) return;
  long offset = almacen.getInt("ntpOffset", -18000);
  configTime(offset, 0, "pool.ntp.org", "time.nist.gov");
  ntpPendiente = true; ntpEstado = "Sincronizando...";
  Serial.printf("NTP: configurado offset=%lds, esperando respuesta...\n", offset);
}

void verificarNtpPendiente() {
  if (!ntpPendiente) return;
  struct tm info;
  if (!getLocalTime(&info, 0)) return;
  ntpPendiente = false; aplicarTiempoNtp(info);
}

bool sincronizarNtpManual() {
  if (WiFi.status() != WL_CONNECTED) { ntpEstado = "Sin WiFi"; return false; }
  long offset = almacen.getInt("ntpOffset", -18000);
  configTime(offset, 0, "pool.ntp.org", "time.nist.gov");
  struct tm info;
  unsigned long inicio = millis();
  while (millis() - inicio < 10000) {
    if (getLocalTime(&info, 500)) { ntpPendiente = false; aplicarTiempoNtp(info); return true; }
  }
  ntpEstado = "NTP: timeout (10 s)"; Serial.println(ntpEstado); return false;
}

// ── Limpieza atomica de registros ────────────────────────────
// Resetea en NVS todos los contadores "c"+uid a 0 leyendo ARCHIVO_UIDS.
// Garantiza que la proxima lectura de cualquier tarjeta sea siempre Entrada.
void resetearContadoresNvs() {
  File f = LittleFS.open(ARCHIVO_UIDS, "r");
  if (!f) return;
  int n = 0;
  while (f.available()) {
    String uid = f.readStringUntil('\n'); uid.trim();
    if (!uid.length()) continue;
    almacen.putInt(("c" + uid).c_str(), 0);
    n++;
  }
  f.close();
  Serial.printf("NVS: %d contadores reseteados a 0\n", n);
}

// Operacion atomica: borra AMBOS archivos de registro y resetea contadores NVS.
// Debe llamarse en TODOS los escenarios de borrado (manual y automatico) para
// mantener el sistema en un estado completamente consistente.
void limpiarRegistros() {
  LittleFS.remove(ARCHIVO_LOG);
  LittleFS.remove(ARCHIVO_ENT);
  resetearContadoresNvs();
  Serial.println("LIMPIEZA: logs, entradas y contadores NVS reseteados.");
}

// ── Email ─────────────────────────────────────────────────────
void smtpCallback(SMTP_Status status) {
  Serial.print("SMTP cb: "); Serial.println(status.info());
}

// Convierte "YYYY-MM-DD HH:MM:SS" a "YYYY_MM_DD_HH_MM_SS" para nombres de archivo.
String tsParaNombre(const String& ts) {
  String r = ts;
  r.replace('-', '_'); r.replace(' ', '_'); r.replace(':', '_');
  return r;
}

// Genera el nombre del adjunto leyendo el primer y ultimo timestamp valido del archivo.
// Formato: prefijo_TS1__TS2.csv | Si vacio: prefijo_sin_datos.csv
String nombreAdjunto(const char* rutaArchivo, const char* prefijo) {
  File f = LittleFS.open(rutaArchivo, "r");
  if (!f || !f.size()) { if (f) f.close(); return String(prefijo) + "_sin_datos.csv"; }
  String primero = "", ultimo = "";
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    int sep = l.indexOf('|');
    if (sep < 19) continue;  // timestamp minimo 19 chars "YYYY-MM-DD HH:MM:SS"
    String ts = l.substring(0, sep);
    if (!primero.length()) primero = ts;
    ultimo = ts;
  }
  f.close();
  if (!primero.length()) return String(prefijo) + "_sin_datos.csv";
  return String(prefijo) + "_" + tsParaNombre(primero) + "__" + tsParaNombre(ultimo) + ".csv";
}

// Envia logs, entradas y lista de usuarios como adjuntos al email configurado.
// borrarTras=true  → envio automatico: borra archivos y actualiza lastEmail.
// borrarTras=false → envio manual:    archivos intactos, lastEmail sin cambios.
bool enviarEmail(const String& csvLog, const String& csvEntradas, bool borrarTras) {
  String destino = almacen.getString("emailDest", "");
  if (!destino.length()) { emailEstado = "Sin destinatario configurado"; return false; }
  if (WiFi.status() != WL_CONNECTED) { emailEstado = "Sin conexion WiFi"; return false; }

  // Generar nombres de adjunto ANTES de cualquier posible borrado
  String nomLog = nombreAdjunto(ARCHIVO_LOG, "accesos");
  String nomEnt = nombreAdjunto(ARCHIVO_ENT, "entradas");

  String fechaHoy = rtcDisponible ? obtenerTimestamp().substring(0, 10) : "sin-fecha";
  String fechaUsr = fechaHoy; fechaUsr.replace("-", "_");
  String nomUsr   = "usuarios_" + fechaUsr + ".csv";

  // Contador de correos del dia (se resetea diariamente)
  int contadorEmail = 1;
  if (almacen.getString("emailCntDate", "") == fechaHoy)
    contadorEmail = almacen.getInt("emailCnt", 0) + 1;
  String sufijoContador = " - " + String(contadorEmail);
  if (nomLog.endsWith(".csv")) nomLog = nomLog.substring(0, nomLog.length()-4) + sufijoContador + ".csv";
  if (nomEnt.endsWith(".csv")) nomEnt = nomEnt.substring(0, nomEnt.length()-4) + sufijoContador + ".csv";
  if (nomUsr.endsWith(".csv")) nomUsr = nomUsr.substring(0, nomUsr.length()-4) + sufijoContador + ".csv";

  String asunto = "Registros NFC - " + fechaHoy + " - " + String(contadorEmail);
  String cuerpo = "Reporte " + String(borrarTras ? "automatico" : "manual")
                  + " del sistema NFC.\nFecha: " + fechaHoy + "\n\nAdjuntos:\n"
                  "  - " + nomLog + "\n"
                  "  - " + nomEnt + "\n"
                  "  - " + nomUsr + "\n";

  Session_Config config;
  config.server.host_name      = SMTP_HOST;
  config.server.port           = SMTP_PORT;
  config.login.email           = REMITENTE_EMAIL;
  config.login.password        = REMITENTE_CLAVE;
  config.login.user_domain     = "";
  config.secure.startTLS       = (SMTP_PORT == 587);
  config.time.ntp_server       = F("pool.ntp.org,time.nist.gov");
  config.time.gmt_offset       = almacen.getInt("ntpOffset", -18000);
  config.time.day_light_offset = 0;

  SMTP_Message message;
  message.sender.name  = F("ESP32 NFC");
  message.sender.email = REMITENTE_EMAIL;
  message.subject      = asunto.c_str();
  message.addRecipient(F("Destino"), destino.c_str());
  message.text.content = cuerpo.c_str();
  message.text.charSet = F("utf-8");

  // descr.name  → parametro "name" en Content-Type.
  // descr.filename → parametro "filename" en Content-Disposition (nombre visible).
  // Ambos son necesarios; sin descr.filename la libreria deja el adjunto como "noname".
  SMTP_Attachment attLog;
  attLog.descr.name = nomLog.c_str(); attLog.descr.filename = nomLog.c_str();
  attLog.descr.mime = F("text/csv");
  attLog.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
  attLog.blob.data = (uint8_t*)csvLog.c_str(); attLog.blob.size = csvLog.length();
  message.addAttachment(attLog);

  SMTP_Attachment attEnt;
  attEnt.descr.name = nomEnt.c_str(); attEnt.descr.filename = nomEnt.c_str();
  attEnt.descr.mime = F("text/csv");
  attEnt.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
  attEnt.blob.data = (uint8_t*)csvEntradas.c_str(); attEnt.blob.size = csvEntradas.length();
  message.addAttachment(attEnt);

  String csvUsr = usuariosCsv();
  SMTP_Attachment attUsr;
  attUsr.descr.name = nomUsr.c_str(); attUsr.descr.filename = nomUsr.c_str();
  attUsr.descr.mime = F("text/csv");
  attUsr.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
  attUsr.blob.data = (uint8_t*)csvUsr.c_str(); attUsr.blob.size = csvUsr.length();
  message.addAttachment(attUsr);

  smtp.debug(1);
  smtp.callback(smtpCallback);

  Serial.printf("SMTP: conectando a %s:%d startTLS=%d dest=%s\n",
    SMTP_HOST, SMTP_PORT, (SMTP_PORT == 587), destino.c_str());
  Serial.printf("SMTP: adj -> \"%s\" \"%s\" \"%s\"\n",
    nomLog.c_str(), nomEnt.c_str(), nomUsr.c_str());
  emailEstado = "Conectando a " + String(SMTP_HOST) + ":" + String(SMTP_PORT) + "...";

  if (!smtp.connect(&config)) {
    emailEstado = "Error conexion: " + smtp.errorReason();
    Serial.println("SMTP conexion FALLO: " + smtp.errorReason());
    smtp.closeSession(); return false;
  }
  Serial.println("SMTP: conexion OK. Enviando...");
  emailEstado = "Enviando mensaje...";

  bool ok = MailClient.sendMail(&smtp, &message, true);
  if (ok) {
    emailEstado   = "Enviado OK - " + (rtcDisponible ? obtenerTimestamp() : fechaHoy);
    emailUltimoTs = emailEstado.substring(emailEstado.indexOf('-') + 2);
    almacen.putString("emailCntDate", fechaHoy);
    almacen.putInt("emailCnt", contadorEmail);
    Serial.println("SMTP: envio EXITOSO");
    if (borrarTras) {
      limpiarRegistros();  // borra ambos archivos y resetea contadores NVS
      // Actualizar lastEmail para evitar reenvio automatico el mismo dia
      if (rtcDisponible) {
        FechaHora fh = rtcLeerFechaHora();
        char hoy[11]; snprintf(hoy, sizeof(hoy), "%04d-%02d-%02d", fh.anio, fh.mes, fh.dia);
        almacen.putString("lastEmail", String(hoy));
      }
      // Limpiar bandera de pendiente: el envio exitoso resuelve la situacion
      if (emailPendienteFlag) {
        emailPendienteFlag = false;
        almacen.putBool("emailPend", false);
      }
      Serial.println("EMAIL-AUTO: registros borrados, contadores reseteados, lastEmail actualizado.");
    }
    ledParpadear(LED_VERDE, LED_BLINK_N);
  } else {
    emailEstado = "Error envio: " + smtp.errorReason();
    Serial.println("SMTP envio FALLO: " + smtp.errorReason());
    ledParpadear(LED_ROJO, LED_BLINK_N);
  }
  smtp.closeSession();
  // Mostrar patron DESPUES de cerrar sesion: closeSession() puede tardar 1-5s
  // y si se llama antes, el timer de 3s expira mientras smtp cierra y loop()
  // muestra el patron idle en vez del check/X.
  matrizMostrar(ok ? PATRON_CHECK : PATRON_X,
                ok ? CRGB::Green  : CRGB::Red, 5000);
  return ok;
}

// ── Cierre de dia al arrancar ────────────────────────────────
// Cubre cortes de alimentacion: al arrancar, recorre ARCHIVO_UIDS y para
// cada UID registrado con fecha de ultimo acceso estrictamente anterior a
// hoy y contador impar, genera "Salida: Pendiente" y resetea el contador.
// Usa "f"+uid (no solo el contador) para descartar UIDs cuyo contador
// impar pertenece al dia actual, evitando falsos positivos.
// Debe llamarse en setup() despues de que el RTC esta inicializado y
// ARCHIVO_UIDS esta reconstruido, antes de entrar al loop().
void cerrarDiaArranque() {
  if (!rtcDisponible) return;
  FechaHora fh = rtcLeerFechaHora();
  char buf[11]; snprintf(buf, sizeof(buf), "%04d-%02d-%02d", fh.anio, fh.mes, fh.dia);
  String fechaHoy = String(buf);

  File f = LittleFS.open(ARCHIVO_UIDS, "r");
  if (!f) { Serial.println("ARRANQUE: ARCHIVO_UIDS no disponible, cierre de dia omitido"); return; }

  int pendientes = 0;
  while (f.available()) {
    String uid = f.readStringUntil('\n'); uid.trim();
    if (!uid.length()) continue;
    String nombre = almacen.getString(uid.c_str(), "");
    if (!nombre.length()) continue;  // tarjeta no registrada en NVS
    String fechaUltima = almacen.getString(("f" + uid).c_str(), "");
    // Comparacion lexicografica de "YYYY-MM-DD" equivale a numerica.
    // Omitir si no hay historial o si el ultimo acceso es del dia actual o futuro.
    if (!fechaUltima.length() || fechaUltima >= fechaHoy) continue;
    String claveCont = "c" + uid;
    int conteo = almacen.getInt(claveCont.c_str(), 0);
    if (conteo % 2 == 1) {
      String codigo = almacen.getString(("k" + uid).c_str(), "");
      entradaAgregar("Pendiente", uid, nombre, codigo, "Salida: Pendiente");
      Serial.println("ARRANQUE: " + uid + " -> Salida pendiente (corte de alimentacion)");
      pendientes++;
    }
    almacen.putInt(claveCont.c_str(), 0);  // reset: dia nuevo empieza desde Entrada
  }
  f.close();
  Serial.printf("ARRANQUE: cierre de dia completado — %d salidas pendientes registradas\n", pendientes);
}

// ── Cierre de dia ────────────────────────────────────────────
// Se ejecuta una sola vez por dia (clave NVS lastCierre) verificando si
// el dia cambio. Para cada UID con contador impar (Entrada sin Salida),
// agrega "Salida: Pendiente". Luego resetea todos los contadores a 0.
// Nota: los cortes de alimentacion son cubiertos por cerrarDiaArranque()
// en setup(); esta funcion cubre el caso normal en que el ESP32 sigue
// encendido al cambiar el dia o arranca pocos minutos despues de medianoche.
void cerrarDia() {
  if (!rtcDisponible) return;
  FechaHora fh = rtcLeerFechaHora();

  char hoy[11]; snprintf(hoy, sizeof(hoy), "%04d-%02d-%02d", fh.anio, fh.mes, fh.dia);
  if (almacen.getString("lastCierre", "") == String(hoy)) return;
  Serial.printf("CIERRE-DIA: %s\n", hoy);

  // Recolectar UIDs: primero de ARCHIVO_UIDS, luego de ARCHIVO_ENT (sin repetir)
  const int MAX_U = 60;
  String uidList[MAX_U], nomList[MAX_U], codList[MAX_U];
  int nu = 0;

  for (int pass = 0; pass < 2 && nu < MAX_U; pass++) {
    const char* archivo = (pass == 0) ? ARCHIVO_UIDS : ARCHIVO_ENT;
    File f = LittleFS.open(archivo, "r");
    if (!f) continue;
    while (f.available() && nu < MAX_U) {
      String l = f.readStringUntil('\n'); l.trim();
      if (!l.length()) continue;
      String uid = l;
      if (pass == 1) {
        // ARCHIVO_ENT: ts|uid|nombre|codigo|tipo
        String ts2, uid2, nom2, cod2, tip2;
        if (!entradaParsear(l, ts2, uid2, nom2, cod2, tip2)) continue;
        uid = uid2;
      }
      bool existe = false;
      for (int i = 0; i < nu; i++) if (uidList[i] == uid) { existe = true; break; }
      if (existe) continue;
      String nombre = almacen.getString(uid.c_str(), "");
      if (!nombre.length()) continue;
      uidList[nu] = uid;
      nomList[nu] = nombre;
      codList[nu] = almacen.getString(("k" + uid).c_str(), "");
      nu++;
    }
    f.close();
  }

  for (int i = 0; i < nu; i++) {
    String claveCont = "c" + uidList[i];
    int conteo = almacen.getInt(claveCont.c_str(), 0);
    if (conteo % 2 == 1) {
      // Contador impar = ultima accion fue Entrada sin Salida
      entradaAgregar("Pendiente", uidList[i], nomList[i], codList[i], "Salida: Pendiente");
      Serial.println("CIERRE-DIA: " + uidList[i] + " -> Salida pendiente");
    }
    almacen.putInt(claveCont.c_str(), 0);  // reset: proximo dia comienza en Entrada
  }

  almacen.putString("lastCierre", String(hoy));
  Serial.printf("CIERRE-DIA: completado, %d UIDs procesados\n", nu);
}

// Retorna la fecha "YYYY-MM-DD" del dia de envio mas reciente que ya ha pasado
// (estrictamente anterior al dia actual). Si hoy mismo es un dia de envio, ese dia
// solo se considera "pasado" si hora > 12 (ambas ventanas agotadas).
// Considera dias de envio: 10, 20 y ultimo dia del mes.
String calcularUltimoEnvioPasado(int anio, int mes, int dia, int ud) {
  int candidatos[] = {10, 20, ud};
  // Buscar el mayor dia de envio en el mes actual que sea < dia
  int mejorDia = -1;
  for (int i = 0; i < 3; i++) {
    if (candidatos[i] < dia && candidatos[i] > mejorDia) mejorDia = candidatos[i];
  }
  char buf[11];
  if (mejorDia > 0) {
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", anio, mes, mejorDia);
    return String(buf);
  }
  // Sin candidatos este mes → buscar en el mes anterior
  int mesPrev = mes - 1, anioPrev = anio;
  if (mesPrev == 0) { mesPrev = 12; anioPrev--; }
  if (anioPrev < 2020) return "";
  int udPrev = ultimoDiaDelMes(mesPrev, anioPrev);
  int diasPrev[] = {10, 20, udPrev};
  mejorDia = -1;
  for (int i = 0; i < 3; i++) if (diasPrev[i] > mejorDia) mejorDia = diasPrev[i];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", anioPrev, mesPrev, mejorDia);
  return String(buf);
}

// ── Auto-envio de email en dias fijos del mes ─────────────────
// Ventana 1: hora 0 (medianoche). Ventana 2: hora 12 (mediodia).
// Detecta ademas envios perdidos de dias anteriores (ESP apagado todo el dia de envio).
void autoEnviarEmail() {
  if (!rtcDisponible) return;
  if (!almacen.getString("emailDest", "").length()) return;

  FechaHora fh = rtcLeerFechaHora();
  int ud = ultimoDiaDelMes(fh.mes, fh.anio);
  char hoy[11]; snprintf(hoy, sizeof(hoy), "%04d-%02d-%02d", fh.anio, fh.mes, fh.dia);
  bool esDiaEnvio = (fh.dia == 10 || fh.dia == 20 || fh.dia == ud);
  String lastEmail = almacen.getString("lastEmail", "");

  // ── Deteccion de envio perdido en dias anteriores ─────────────
  // Solo cuando ya hubo un envio previo (lastEmail no vacio) y las ventanas del dia
  // de envio actual aun estan abiertas no se evalua (hora <= 12 en dia de envio).
  // Evita activar la bandera mientras el dia de envio actual todavia puede completarse.
  if (!emailPendienteFlag && lastEmail.length() > 0 && !(esDiaEnvio && fh.hora <= 12)) {
    String ultimoPasado = calcularUltimoEnvioPasado(fh.anio, fh.mes, fh.dia, ud);
    if (ultimoPasado.length() && lastEmail < ultimoPasado) {
      // Solo activar si hay datos que proteger (logs no vacios)
      File fLog = LittleFS.open(ARCHIVO_LOG, "r");
      bool hayDatos = fLog && fLog.size() > 0;
      if (fLog) fLog.close();
      if (hayDatos) {
        emailPendienteFlag = true;
        almacen.putBool("emailPend", true);
        Serial.println("AUTO-EMAIL: envio perdido (" + lastEmail + " < " + ultimoPasado + "), bandera activada");
      }
    }
  }

  // ── Intentar envio en dias y ventanas programadas ─────────────
  if (!esDiaEnvio) return;
  if (lastEmail == String(hoy)) return;  // ya enviado hoy

  if (fh.hora == 0 || fh.hora == 12) {
    if (WiFi.status() != WL_CONNECTED) return;
    Serial.printf("AUTO-EMAIL: ventana hora %d, generando CSV...\n", fh.hora);
    bool ok = enviarEmail(logCsv(), entradaCsv(), true);
    Serial.println(ok ? "AUTO-EMAIL enviado: " + String(hoy)
                      : "AUTO-EMAIL: fallo en ventana hora " + String(fh.hora));
  } else if (fh.hora > 12) {
    // Ambas ventanas del dia ya pasaron sin envio exitoso
    if (!emailPendienteFlag) {
      File fLog = LittleFS.open(ARCHIVO_LOG, "r");
      bool hayDatos = fLog && fLog.size() > 0;
      if (fLog) fLog.close();
      if (hayDatos) {
        emailPendienteFlag = true;
        almacen.putBool("emailPend", true);
        Serial.println("AUTO-EMAIL: ambas ventanas agotadas, bandera pendiente activada.");
      }
    }
  }
  // hora 1-11: esperar la ventana del mediodia; no hacer nada
}

// ── Handlers web ─────────────────────────────────────────────
void handleTime() {
  servidor.send(200, "text/plain", rtcDisponible ? obtenerTimestamp() : "ms:" + String(millis()));
}

void handleHome() {
  String ts     = rtcDisponible ? obtenerTimestamp() : "RTC no disponible";
  String estado = esperandoTarjeta ? "Esperando tarjeta para registrar"
                : modoEliminar     ? "Esperando tarjeta para borrar"
                :                    "Lectura normal";
  String staInfo = (WiFi.status() == WL_CONNECTED)
    ? "WiFi: " + WiFi.SSID() + " &nbsp;(" + WiFi.localIP().toString() + ")"
    : "WiFi internet: desconectado";

  servidor.send(200, "text/html", pagina("ESP32 NFC",
    "<h2>ESP32 NFC</h2><div class='card'>"
    "<p>Estado: <b>" + estado + "</b></p>"
    "<p class='ts' id='clk'>&#128336; " + ts + "</p>"
    "<script>setInterval(async()=>{document.getElementById('clk').innerHTML="
    "'&#128336; '+await(await fetch('/time')).text();},1000);</script>"
    "<p class='muted'>AP: 192.168.4.1 &nbsp;|&nbsp; " + staInfo + "</p><br>"
    "<a href='/register'><button>Registrar usuario</button></a> "
    "<a href='/usuarios'><button>&#128101; Usuarios</button></a> "
    "<a href='/logs'><button>Ver logs</button></a> "
    "<a href='/entradas'><button>Entradas/Salidas</button></a> "
    "<a href='/config'><button>&#9881; Configuracion</button></a></div>"));
}

void handleRegisterForm() {
  servidor.send(200, "text/html", pagina("Registrar",
    "<h2>Registrar usuario</h2><div class='card'>"
    "<form method='POST' action='/saveName'>"
    "<label>Nombre:</label><br><input name='name' placeholder='Ej: Juan Perez' required>"
    "<br><br><label>Codigo:</label><br>"
    "<input name='code' maxlength='6' pattern='[A-Za-z0-9]{1,6}' required placeholder='Ej: AB1234'>"
    "<br><br><button type='submit'>Guardar</button></form>"
    "<p class='muted'>Despues de guardar, acerca la tarjeta al lector.</p></div>"
    "<div class='card' style='border-color:#dc3545'>"
    "<h3 style='color:#dc3545'>Borrar usuario</h3>"
    "<a href='/deleteUser'><button class='btn-danger'>&#128465; Borrar usuario con tarjeta</button></a></div>"
    "<a href='/'><button>Volver</button></a>"));
}

void handleSaveName() {
  if (!servidor.hasArg("name") || !servidor.hasArg("code")) {
    servidor.send(400, "text/plain", "Faltan campos"); return;
  }
  nombrePendiente = servidor.arg("name"); nombrePendiente.trim();
  codigoPendiente = servidor.arg("code"); codigoPendiente.trim();
  if (!nombrePendiente.length()) { servidor.send(400, "text/plain", "Nombre vacio"); return; }
  if (!codigoPendiente.length() || codigoPendiente.length() > 6) {
    servidor.send(400, "text/plain", "Codigo invalido (1-6 caracteres)"); return;
  }
  esperandoTarjeta = true; modoEliminar = false;
  tarjetaPresente = false; ultimoUid = ""; tiempoUltimoUid = 0;
  tiempoLcdHasta = 0;  // cancelar timer pendiente; no debe sobreescribir ni activar blink
  lcdWakeUp();
  lcdMostrar("Registrando:", nombrePendiente, "Acerca tarjeta", "");
  servidor.send(200, "text/html", pagina("Acerca la tarjeta",
    "<h2>Acerca la tarjeta</h2><div class='card'>"
    "<p>Nombre: <b>" + nombrePendiente + "</b></p>"
    "<p>Codigo: <b>" + codigoPendiente + "</b></p>"
    "<p id='st'>Esperando...</p>"
    "<script>setInterval(async()=>{let t=await(await fetch('/status')).text();"
    "if(t.startsWith('OK|'))location.href='/done?d='+encodeURIComponent(t);"
    "else document.getElementById('st').innerText=t;},700);</script>"
    "<a href='/cancelar'><button>Cancelar</button></a></div>"));
}

void handleDeleteUser() {
  esperandoTarjeta = false; modoEliminar = true; nombrePendiente = ""; codigoPendiente = "";
  tarjetaPresente = false; ultimoUid = ""; tiempoUltimoUid = 0;
  tiempoLcdHasta = 0;  // cancelar timer pendiente; no debe sobreescribir ni activar blink
  lcdWakeUp();
  lcdMostrar("Modo eliminar", "Acerca tarjeta", "", "");
  servidor.send(200, "text/html", pagina("Borrar usuario",
    "<h2>Borrar usuario</h2><div class='card' style='border-color:#dc3545'>"
    "<p style='color:#dc3545;font-weight:bold'>&#9888; Acerca la tarjeta para BORRAR</p>"
    "<p id='st'>Esperando tarjeta...</p>"
    "<script>setInterval(async()=>{let t=await(await fetch('/status')).text();"
    "if(t.startsWith('DELETED|')||t.startsWith('NOT_FOUND|'))location.href='/deleted?d='+encodeURIComponent(t);"
    "else document.getElementById('st').innerText=t;},700);</script>"
    "<a href='/cancelar'><button>Cancelar</button></a></div>"));
}

void handleStatus() {
  if (modoEliminar || resultadoEliminacion) {
    String ultimo = almacen.getString("lastDel", "");
    if (ultimo.startsWith("DELETED|") || ultimo.startsWith("NOT_FOUND|")) {
      almacen.putString("lastDel", ""); resultadoEliminacion = false;
      servidor.send(200, "text/plain", ultimo); return;
    }
    servidor.send(200, "text/plain", "Esperando tarjeta para borrar..."); return;
  }
  if (esperandoTarjeta) { servidor.send(200, "text/plain", "Esperando tarjeta..."); return; }
  String ultimo = almacen.getString("lastReg", "");
  if (ultimo.startsWith("OK|")) {
    almacen.putString("lastReg", ""); servidor.send(200, "text/plain", ultimo); return;
  }
  servidor.send(200, "text/plain", "Listo.");
}

void handleDone() {
  String datos  = servidor.hasArg("d") ? servidor.arg("d") : "";
  String cuerpo = "<h2>&#10003; Registrado</h2><div class='card'>";
  if (datos.startsWith("OK|")) {
    int pos1 = datos.indexOf('|'), pos2 = datos.indexOf('|', pos1+1),
        pos3 = datos.indexOf('|', pos2+1), pos4 = (pos3>0) ? datos.indexOf('|', pos3+1) : -1;
    cuerpo += "<p>UID: <b>" + datos.substring(pos1+1, pos2) + "</b></p>"
              "<p>Nombre: <b>" + datos.substring(pos2+1, pos3>0?pos3:datos.length()) + "</b></p>";
    if (pos3 > 0) cuerpo += "<p>Codigo: <b>" + datos.substring(pos3+1, pos4>0?pos4:datos.length()) + "</b></p>";
    if (pos4 > 0) cuerpo += "<p class='ts'>&#128336; " + datos.substring(pos4+1) + "</p>";
  } else cuerpo += "<p class='muted'>Sin datos.</p>";
  cuerpo += "<a href='/'><button>Inicio</button></a> <a href='/logs'><button>Ver logs</button></a></div>";
  servidor.send(200, "text/html", pagina("Registrado", cuerpo));
}

void handleDeleted() {
  String datos  = servidor.hasArg("d") ? servidor.arg("d") : "";
  String cuerpo = "<h2>Usuario borrado</h2><div class='card'>";
  if (datos.startsWith("DELETED|")) {
    int pos1 = datos.indexOf('|'), pos2 = datos.indexOf('|', pos1+1),
        pos3 = datos.indexOf('|', pos2+1);
    cuerpo += "<p style='color:#dc3545;font-weight:bold'>&#10003; Usuario eliminado</p>"
              "<p>UID: <b>" + datos.substring(pos1+1, pos2) + "</b></p>"
              "<p>Nombre: <b>" + datos.substring(pos2+1, pos3>0?pos3:datos.length()) + "</b></p>";
    if (pos3 > 0) cuerpo += "<p>Codigo: <b>" + datos.substring(pos3+1) + "</b></p>";
  } else if (datos.startsWith("NOT_FOUND|")) {
    cuerpo += "<p style='color:#ff9800;font-weight:bold'>&#9888; Tarjeta no registrada</p>"
              "<p>UID: <b>" + datos.substring(datos.indexOf('|')+1) + "</b></p>";
  } else cuerpo += "<p class='muted'>Sin datos.</p>";
  cuerpo += "<a href='/'><button>Inicio</button></a> <a href='/register'><button>Registrar</button></a></div>";
  servidor.send(200, "text/html", pagina("Usuario borrado", cuerpo));
}

void handleLogs() {
  servidor.send(200, "text/html", pagina("Logs",
    "<h2>Logs de acceso</h2>" + logHtml() +
    "<div style='margin:20px 0;display:flex;flex-wrap:wrap;gap:8px'>"
    "<a href='/'><button>Volver</button></a> "
    "<a href='/downloadLogs'><button class='btn-ok'>&#128229; Descargar CSV</button></a> "
    "<a href='/sendEmail' onclick='return confirm(\"Enviar email con todos los registros ahora?\");'>"
    "<button class='btn-email'>&#128231; Enviar por email</button></a> "
    "<a href='/clearRegistros' onclick='return confirm(\"Borrar TODOS los registros?\\n"
    "Se eliminaran logs de acceso, entradas/salidas y se resetearan los contadores de tarjetas.\\n"
    "Las tarjetas registradas NO se eliminaran.\");'>"
    "<button class='btn-danger'>&#128465; Borrar todos los registros</button></a></div>"));
}

void handleClearRegistros() {
  // Contar registros antes de borrar para el mensaje de confirmacion
  int totalLog = 0, totalEnt = 0;
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (f) { while (f.available()) { if (f.read() == '\n') totalLog++; } f.close(); }
  f = LittleFS.open(ARCHIVO_ENT, "r");
  if (f) { while (f.available()) { if (f.read() == '\n') totalEnt++; } f.close(); }

  limpiarRegistros();  // borra ambos archivos y resetea contadores NVS
  ledParpadear(LED_VERDE, LED_BLINK_N);
  matrizMostrar(PATRON_CHECK, CRGB::Green, 3000);

  servidor.send(200, "text/html", pagina("Registros borrados",
    "<h2>&#10003; Todos los registros borrados</h2><div class='card'>"
    "<p>Eliminados <b>" + String(totalLog) + "</b> eventos de acceso (logs).</p>"
    "<p>Eliminados <b>" + String(totalEnt) + "</b> registros de entradas/salidas.</p>"
    "<p>Contadores de todas las tarjetas reseteados a 0.</p>"
    "<p class='muted'>La proxima lectura de cualquier tarjeta se registrara como <b>Entrada</b>.</p>"
    "<p class='muted'>Las asociaciones UID&rarr;Nombre NO fueron eliminadas.</p>"
    "<a href='/logs'><button>Ver logs</button></a> "
    "<a href='/'><button>Inicio</button></a></div>"));
}

void handleDownloadLogs() {
  String nombre = nombreAdjunto(ARCHIVO_LOG, "accesos");
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"" + nombre + "\"");
  servidor.send(200, "text/csv; charset=utf-8", logCsv());
}

void handleCancelar() {
  esperandoTarjeta = false; modoEliminar = false; nombrePendiente = ""; codigoPendiente = "";
  tarjetaPresente = false; ultimoUid = ""; tiempoUltimoUid = 0;
  lcdWakeUp();
  lcdMostrar("Cancelado", "", "", "");
  tiempoLcdHasta = millis() + 2000;  // transicion limpia a idle: blink empieza desde cero
  servidor.send(200, "text/html", pagina("Cancelado",
    "<h2>Operacion cancelada</h2><div class='card'>"
    "<p class='muted'>No se realizo ningun cambio.</p>"
    "<a href='/'><button>Inicio</button></a> "
    "<a href='/register'><button>Registrar usuario</button></a></div>"));
}

void handleEntradas() {
  servidor.send(200, "text/html", pagina("Entradas/Salidas",
    "<h2>Registros de Entradas/Salidas</h2>" + entradaHtml() +
    "<div style='margin:20px 0;display:flex;flex-wrap:wrap;gap:8px'>"
    "<a href='/'><button>Volver</button></a> "
    "<a href='/downloadEntradas'><button class='btn-ok'>&#128229; Descargar CSV</button></a> "
    "<a href='/clearRegistros' onclick='return confirm(\"Borrar TODOS los registros?\\n"
    "Se eliminaran logs de acceso, entradas/salidas y se resetearan los contadores de tarjetas.\\n"
    "Las tarjetas registradas NO se eliminaran.\");'>"
    "<button class='btn-danger'>&#128465; Borrar todos los registros</button></a></div>"));
}

void handleDownloadEntradas() {
  String nombre = nombreAdjunto(ARCHIVO_ENT, "entradas");
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"" + nombre + "\"");
  servidor.send(200, "text/csv; charset=utf-8", entradaCsv());
}


void handleUsuarios() {
  servidor.send(200, "text/html", pagina("Usuarios",
    "<h2>&#128101; Usuarios registrados</h2>" + usuariosHtml() +
    "<div style='margin:20px 0;display:flex;flex-wrap:wrap;gap:8px'>"
    "<a href='/'><button>Volver</button></a> "
    "<a href='/downloadUsuarios'><button class='btn-ok'>&#128229; Descargar CSV</button></a></div>"));
}

void handleDownloadUsuarios() {
  String fecha = rtcDisponible ? obtenerTimestamp().substring(0, 10) : "usuarios";
  fecha.replace("-", "_");
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"usuarios_" + fecha + ".csv\"");
  servidor.send(200, "text/csv; charset=utf-8", usuariosCsv());
}

// ── Handler: Configuracion WiFi + NTP + email ────────────────
void handleConfig() {
  String ssidGuardado  = almacen.getString("ssidWifi",  "");
  String emailGuardado = almacen.getString("emailDest", "");
  String ultimoEmail   = almacen.getString("lastEmail", "(nunca)");
  String estadoWifi    = (WiFi.status() == WL_CONNECTED)
    ? "<span style='color:#28a745'>Conectado &mdash; " + WiFi.localIP().toString() + "</span>"
    : "<span style='color:#dc3545'>Desconectado</span>";

  long   offsetSec  = almacen.getInt("ntpOffset", -18000);
  String offsetHStr = String(offsetSec / 3600);

  String ntpSyncStr = "Nunca";
  if (ultimaNtpSync > 0) {
    unsigned long segs = (millis() - ultimaNtpSync) / 1000;
    if (segs < 60)        ntpSyncStr = "hace " + String(segs) + " s";
    else if (segs < 3600) ntpSyncStr = "hace " + String(segs/60) + " min";
    else                  ntpSyncStr = "hace " + String(segs/3600) + " h";
  }

  String emailColor = "#666";
  if (emailEstado.startsWith("Enviado")) emailColor = "#28a745";
  else if (emailEstado.startsWith("Error")) emailColor = "#dc3545";

  servidor.send(200, "text/html", pagina("Configuracion",
    "<h2>&#9881; Configuracion</h2>"

    "<div class='card'><h3>Red WiFi con internet</h3>"
    "<p>Estado: " + estadoWifi + "</p>"
    "<form method='POST' action='/saveConfig'>"
    "<label>SSID (nombre de red):</label><br>"
    "<input name='ssid' value='" + ssidGuardado + "' placeholder='Nombre de tu WiFi'><br><br>"
    "<label>Contrasena WiFi:</label><br>"
    "<input type='password' name='clave' placeholder='(en blanco = no cambiar)'><br><br>"
    "<label>Email destino para reportes:</label><br>"
    "<input type='email' name='email' value='" + emailGuardado + "' placeholder='destino@ejemplo.com'><br><br>"
    "<label>Zona horaria (offset UTC en horas, ej: -5 para Colombia):</label><br>"
    "<input type='number' name='ntpOffsetH' min='-12' max='14' value='" + offsetHStr + "'><br><br>"
    "<button type='submit'>&#10003; Guardar y reconectar</button></form>"
    "<p class='muted'>Ultimo envio automatico de correo: " + ultimoEmail + "</p>"
    "<p class='muted'>Reportes automaticos los dias 10, 20 y ultimo del mes a medianoche "
    "(reintento al mediodia si falla). Si ambos fallan, aparece alerta en la web.</p>"
    "</div>"

    "<div class='card'><h3>&#128336; Sincronizacion NTP</h3>"
    "<p>Estado: <b>" + ntpEstado + "</b></p>"
    "<p class='muted'>Ultima sincronizacion: " + ntpSyncStr + "</p>"
    "<a href='/syncNtp'><button class='btn-ok'>&#8635; Sincronizar hora por NTP ahora</button></a>"
    "</div>"

    "<div class='card'><h3>&#128231; Estado del correo</h3>"
    "<p>Ultimo intento: <b style='color:" + emailColor + "'>" + emailEstado + "</b></p>"
    "<p class='muted'>Host SMTP: " + String(SMTP_HOST) + ":" + String(SMTP_PORT)
    + " (startTLS=" + (SMTP_PORT == 587 ? "si" : "no") + ")</p>"
    "<p class='muted'>Remitente: " + String(REMITENTE_EMAIL) + "</p>"
    "<p class='muted'>IMPORTANTE: la clave SMTP debe ser un App Password de Google"
    " (16 caracteres, sin espacios), NO la contrasena normal de Gmail.</p>"
    "</div>"

    "<a href='/'><button>Volver</button></a>"));
}

void handleSaveConfig() {
  String ssid  = servidor.hasArg("ssid")  ? servidor.arg("ssid")  : "";
  String clave = servidor.hasArg("clave") ? servidor.arg("clave") : "";
  String email = servidor.hasArg("email") ? servidor.arg("email") : "";
  ssid.trim(); clave.trim(); email.trim();

  if (ssid.length())  almacen.putString("ssidWifi",  ssid.c_str());
  if (clave.length()) almacen.putString("claveWifi", clave.c_str());
  if (email.length()) almacen.putString("emailDest", email.c_str());

  if (servidor.hasArg("ntpOffsetH")) {
    long offsetH = servidor.arg("ntpOffsetH").toInt();
    if (offsetH >= -12 && offsetH <= 14) {
      almacen.putInt("ntpOffset", (int)(offsetH * 3600));
      Serial.printf("NTP: zona horaria guardada UTC%+ld\n", offsetH);
    }
  }

  if (ssid.length()) {
    String claveUsar = clave.length() ? clave : almacen.getString("claveWifi", "");
    WiFi.disconnect(false); delay(200);
    WiFi.begin(ssid.c_str(), claveUsar.c_str());
    Serial.println("WiFi: reconectando a " + ssid);
  }

  servidor.send(200, "text/html", pagina("Configuracion guardada",
    "<h2>&#10003; Configuracion guardada</h2><div class='card'>"
    "<p>Configuracion actualizada correctamente.</p>"
    "<p class='muted'>Si se cambio el WiFi, la conexion puede tardar unos segundos.</p>"
    "<a href='/config'><button>Ver estado WiFi</button></a> "
    "<a href='/'><button>Inicio</button></a></div>"));
}

void handleSendEmail() {
  String destino = almacen.getString("emailDest", "");
  if (!destino.length()) {
    servidor.send(200, "text/html", pagina("Sin destinatario",
      "<h2>Email no configurado</h2><div class='card'>"
      "<p>Configure el email destino en <a href='/config'>Configuracion</a>.</p>"
      "<a href='/logs'><button>Volver</button></a></div>"));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    servidor.send(200, "text/html", pagina("Sin conexion",
      "<h2>Sin conexion a internet</h2><div class='card'>"
      "<p>Configure la red WiFi en <a href='/config'>Configuracion</a>.</p>"
      "<a href='/logs'><button>Volver</button></a></div>"));
    return;
  }
  servidor.sendHeader("Content-Type", "text/html; charset=utf-8");
  bool ok = enviarEmail(logCsv(), entradaCsv(), false);
  if (ok) {
    servidor.send(200, "text/html", pagina("Email enviado",
      "<h2>&#10003; Email enviado</h2><div class='card'>"
      "<p>Archivos enviados a <b>" + destino + "</b>.</p>"
      "<p class='muted'>Los archivos de log y entradas <b>NO han sido borrados</b>"
      " (envio manual).</p>"
      "<a href='/logs'><button>Ver logs</button></a> "
      "<a href='/'><button>Inicio</button></a></div>"));
  } else {
    servidor.send(200, "text/html", pagina("Error al enviar",
      "<h2>&#10060; Error al enviar</h2><div class='card'>"
      "<p>No se pudo enviar el correo.</p>"
      "<p class='muted'>Verifique las credenciales SMTP y que el WiFi tenga internet.</p>"
      "<p class='muted'>Los archivos NO fueron borrados.</p>"
      "<a href='/logs'><button>Volver a logs</button></a></div>"));
  }
}

void handleConfirmarEmailPend() {
  emailPendienteFlag = false;
  almacen.putBool("emailPend", false);
  servidor.sendHeader("Location", "/");
  servidor.send(302, "text/plain", "");
}

void handleSyncNtp() {
  if (WiFi.status() != WL_CONNECTED) {
    servidor.send(200, "text/html", pagina("Sin WiFi",
      "<h2>Sin conexion WiFi</h2><div class='card'>"
      "<p>Conecte el WiFi en <a href='/config'>Configuracion</a> primero.</p>"
      "<a href='/config'><button>Volver</button></a></div>"));
    return;
  }
  bool ok = sincronizarNtpManual();
  String color = ok ? "#28a745" : "#dc3545";
  String icono = ok ? "&#10003;" : "&#10060;";
  servidor.send(200, "text/html", pagina("Sincronizacion NTP",
    "<h2>" + String(icono) + " NTP</h2><div class='card'>"
    "<p style='color:" + color + ";font-weight:bold'>" + ntpEstado + "</p>"
    "<p class='ts'>Hora actual: " + (rtcDisponible ? obtenerTimestamp() : "RTC no disponible") + "</p>"
    "<a href='/config'><button>Volver</button></a></div>"));
}

// ── NFC watchdog: recuperacion UART robusta con reintentos y auto-restart ────
// Secuencia: flush → end/begin UART → preamble → flush → hasta 3 intentos getFirmwareVersion.
// Tras 5 fallos consecutivos (~25s sin recuperacion), reinicia el ESP32 automaticamente.
void nfcReinicializar() {
  static uint8_t fallosConsec = 0;
  fallosConsec++;
  Serial.printf("NFC WDG: recuperacion #%u — reinicializando UART2...\n", fallosConsec);

  // Paso 1: cerrar completamente el UART para limpiar el estado del driver ESP32
  Serial2.end();
  delay(300);

  // Paso 2: reiniciar con pines explícitos y vaciar el buffer de recepcion
  Serial2.begin(115200, SERIAL_8N1, 16, 17);
  delay(100);
  while (Serial2.available()) Serial2.read();

  // Paso 3: preamble puro (sin SAMConfig embebido) para restablecer sincronismo UART del PN532
  const uint8_t preamble[] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
  Serial2.write(preamble, sizeof(preamble));
  delay(100);
  while (Serial2.available()) Serial2.read();

  // Paso 4: hasta 3 intentos de comunicacion con flush y espera creciente entre cada uno
  uint32_t v = 0;
  for (int i = 0; i < 3 && !v; i++) {
    if (i > 0) { delay(300 * i); while (Serial2.available()) Serial2.read(); }
    v = lectorNfc.getFirmwareVersion();
    if (!v) while (Serial2.available()) Serial2.read();  // limpiar respuesta fallida
  }

  if (v) {
    fallosConsec = 0;
    lectorNfc.SAMConfig();
    Serial.println("NFC WDG: PN532 OK, Fw v" + String((v >> 16) & 0xFF));
    if (!tiempoLcdHasta) {
      if      (esperandoTarjeta) lcdMostrar("Registrando:", nombrePendiente, "Acerca tarjeta", "");
      else if (modoEliminar)     lcdMostrar("Modo eliminar", "Acerca tarjeta", "", "");
      else                       lcdMostrar("Esperando", "tarjeta...", "", "");
      lcdIdleDesde = millis(); lcdParpadeoNext = 0;
    }
  } else {
    Serial.printf("NFC WDG: PN532 sin respuesta (fallo consecutivo #%u)\n", fallosConsec);
    lcdWakeUp();
    lcdMostrar("NFC ERROR", "Reiniciando...", "", "");
    if (fallosConsec >= 5) {
      Serial.println("NFC WDG: demasiados fallos, reiniciando ESP32 automaticamente...");
      delay(1500);
      ESP.restart();
    }
  }
  nfcUltimoCheck = millis();
}

// ── NFC ──────────────────────────────────────────────────────
String uidAHex(byte* uid, byte longitud) {
  String hex = "";
  for (int i = 0; i < longitud; i++) { if (uid[i] < 16) hex += "0"; hex += String(uid[i], HEX); }
  hex.toUpperCase(); return hex;
}

bool leerUidUnaVez(String &uidSalida) {
  byte uid[7]; byte longitud = 0;

  // Timeout 100ms: menor que INTERVALO_NFC (300ms), garantiza que loop() tenga
  // ~200ms de ventana libre por ciclo para parpadeo LCD y otras tareas no bloqueantes.
  // Una tarjeta presente responde en <30ms; 100ms es mas que suficiente.
  unsigned long t0 = millis();
  bool hayTarjeta = lectorNfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &longitud, 100);
  unsigned long dt = millis() - t0;
  if (dt > 200) {
    Serial.printf("NFC: lectura lenta (%lums) — posible problema UART\n", dt);
    while (Serial2.available()) Serial2.read();  // vaciar bytes corruptos del buffer
    nfcUltimoCheck = 0;  // disparar watchdog en el proximo ciclo
  }

  if (!hayTarjeta) {
    if (tarjetaPresente) {
      tarjetaPresente = false;
      Serial.println("Tarjeta retirada");
      // ultimoUid NO se borra: el cooldown por tiempo sigue activo mientras la tarjeta
      // sigue fisicamente cerca (proteccion contra I2C flicker del PN532).
    }
    return false;
  }
  String hex = uidAHex(uid, longitud);
  unsigned long ahora = millis();
  if (hex == ultimoUid && (tarjetaPresente || (ahora - tiempoUltimoUid < COOLDOWN_TARJETA))) {
    tarjetaPresente = true; return false;
  }
  tarjetaPresente = true;
  ultimoUid = uidSalida = hex;
  tiempoUltimoUid = ahora;
  return true;
}

// ── Setup & Loop ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(500);

  // ── UART2 para PN532: pines explícitos antes de cualquier otro periférico ─────
  Serial2.begin(115200, SERIAL_8N1, 16, 17);

  // ── Wire, LCD y LED ───────────────────────────────────────────────────────────
  Wire.begin(21, 22);
  Wire.setTimeOut(200);  // limitar cada transaccion I2C a 200 ms; evita cuelgues
  lcd.init(); lcd.backlight();
  ledSetup();
  FastLED.addLeds<WS2812B, MATRIZ_PIN, GRB>(matrizLeds, MATRIZ_LEDS);
  FastLED.setBrightness(MATRIZ_BRILLO);
  matrizMostrar(PATRON_DISPONIBLE, CRGB(100, 80, 0));
  lcdMostrar("Sistema NFC", "Iniciando...", "", "");

  // ── NVS y LittleFS ───────────────────────────────────────────────────────────
  almacen.begin("nfc", false);
  emailPendienteFlag = almacen.getBool("emailPend", false);  // restaurar bandera tras reinicio
  LittleFS.begin(true);
  reconstruirArchivoUids();  // sincronizar ARCHIVO_UIDS con NVS en cada arranque

  // ── RTC ──────────────────────────────────────────────────────────────────────
  busRtc.begin(RTC_SDA, RTC_SCL); delay(50);
  rtcDisponible = rtcDs3231.begin(&busRtc);
  if (rtcDisponible) {
    rtcSincronizarSiNecesario();
    cerrarDiaArranque();  // barrido de Salidas Pendientes antes de la primera lectura
    Serial.printf("DS3231 OK. Temperatura: %.1f C\n", rtcDs3231.getTemperature());
  } else {
    Serial.println("AVISO: DS3231 no encontrado. Usando millis().");
  }

  // ── PN532 via UART2 ───────────────────────────────────────────────────────────
  // lectorNfc.begin() reinicializa el canal serial interno (igual que el sketch de prueba).
  // NO llamar wakeup(): esa funcion embebe SAMConfig en la trama y corrompe el buffer.
  lectorNfc.begin();
  uint32_t vFirmware = lectorNfc.getFirmwareVersion();
  Serial.println(vFirmware
    ? "PN532 OK. Fw v" + String((vFirmware >> 16) & 0xFF)
    : "ERROR: PN532 no detectado!");
  if (vFirmware) lectorNfc.SAMConfig();
  nfcUltimoCheck = millis();  // iniciar el intervalo del watchdog desde ahora

  // Modo AP+STA: Access Point para clientes locales + cliente WiFi para internet
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(SSID_AP, CLAVE_AP, 6);
  Serial.println("AP: " + String(SSID_AP) + " | IP: " + WiFi.softAPIP().toString());

  String ssidGuardado = almacen.getString("ssidWifi", "");
  if (ssidGuardado.length()) {
    WiFi.begin(ssidGuardado.c_str(), almacen.getString("claveWifi", "").c_str());
    Serial.println("WiFi STA: conectando a " + ssidGuardado + "...");
  }

  servidor.on("/time",             handleTime);
  servidor.on("/",                 handleHome);
  servidor.on("/register",         handleRegisterForm);
  servidor.on("/saveName",         HTTP_POST, handleSaveName);
  servidor.on("/deleteUser",       handleDeleteUser);
  servidor.on("/deleted",          handleDeleted);
  servidor.on("/status",           handleStatus);
  servidor.on("/done",             handleDone);
  servidor.on("/logs",             handleLogs);
  servidor.on("/downloadLogs",     handleDownloadLogs);
  servidor.on("/cancelar",         handleCancelar);
  servidor.on("/entradas",         handleEntradas);
  servidor.on("/downloadEntradas", handleDownloadEntradas);
  servidor.on("/clearRegistros",   handleClearRegistros);
  servidor.on("/usuarios",         handleUsuarios);
  servidor.on("/downloadUsuarios", handleDownloadUsuarios);
  servidor.on("/config",           handleConfig);
  servidor.on("/saveConfig",       HTTP_POST, handleSaveConfig);
  servidor.on("/sendEmail",           handleSendEmail);
  servidor.on("/syncNtp",             handleSyncNtp);
  servidor.on("/confirmarEmailPend",  handleConfirmarEmailPend);
  servidor.begin();
  Serial.println("Servidor web iniciado!");

  lcdMostrar("Sistema NFC", "Listo...", "", "");
  lcdIdleDesde = millis();  // iniciar proteccion LCD desde el arranque
}

void loop() {
  servidor.handleClient();
  unsigned long ahora = millis();

  // Proteccion LCD: titileo no bloqueante
  lcdActualizarParpadeo();
  ledActualizar();
  matrizActualizar();

  // Al expirar el timer, mostrar el mensaje que corresponde al estado real actual
  if (tiempoLcdHasta && ahora >= tiempoLcdHasta) {
    tiempoLcdHasta = 0;
    if      (esperandoTarjeta) lcdMostrar("Registrando:", nombrePendiente, "Acerca tarjeta", "");
    else if (modoEliminar)     lcdMostrar("Modo eliminar", "Acerca tarjeta", "", "");
    else                       lcdMostrar("Esperando", "tarjeta...", "", "");
    lcdIdleDesde    = ahora;  // activar proteccion LCD al entrar en idle
    lcdParpadeoNext = 0;
  }

  // Detectar conexion WiFi nueva: iniciar sincronizacion NTP
  bool wifiAhora = (WiFi.status() == WL_CONNECTED);
  if (wifiAhora && !wifiConectadoPrev) {
    Serial.println("WiFi: conexion establecida. Iniciando NTP...");
    iniciarNtp();
  }
  wifiConectadoPrev = wifiAhora;

  // Aplicar hora NTP al RTC si ya respondio el servidor
  verificarNtpPendiente();

  // Reintentar conexion WiFi si se pierde
  if (ahora - ultimaReconexionWifi >= INTERVALO_WIFI) {
    ultimaReconexionWifi = ahora;
    if (!wifiAhora) {
      String ssid = almacen.getString("ssidWifi", "");
      if (ssid.length()) WiFi.begin(ssid.c_str(), almacen.getString("claveWifi", "").c_str());
    }
  }

  // Tareas periodicas: cierre del dia y envio automatico de email
  if (ahora - ultimoChequeoLimpieza >= INTERVALO_LIMPIEZA) {
    ultimoChequeoLimpieza = ahora;
    cerrarDia();       // 1: marcar Salidas Pendientes y resetear contadores
    autoEnviarEmail(); // 2: enviar email (incluye las Salidas Pendientes recien marcadas)
  }

  // Watchdog PN532: verificar salud cada NFC_CHECK_INTERVALO ms.
  // Flush antes del chequeo para no leer bytes residuales de comandos anteriores.
  // Flush despues del chequeo exitoso para limpiar la respuesta del getFirmwareVersion.
  if (ahora - nfcUltimoCheck >= NFC_CHECK_INTERVALO) {
    nfcUltimoCheck = ahora;
    while (Serial2.available()) Serial2.read();
    if (!lectorNfc.getFirmwareVersion()) {
      while (Serial2.available()) Serial2.read();
      Serial.println("NFC WDG: chip no responde — iniciando recuperacion automatica");
      nfcReinicializar();
      return;
    }
    while (Serial2.available()) Serial2.read();
  }

  if (ahora - ultimaLecturaNfc < INTERVALO_NFC) return;
  ultimaLecturaNfc = ahora;

  String uid;
  if (!leerUidUnaVez(uid)) return;
  String ts = rtcDisponible ? obtenerTimestamp() : "ms:" + String(millis());

  if (modoEliminar) {
    String nombre = almacen.getString(uid.c_str(), "");
    lcdWakeUp();
    if (!nombre.length()) {
      almacen.putString("lastDel", "NOT_FOUND|" + uid);
      Serial.println("TARJETA NO REGISTRADA: " + uid);
      lcdMostrar("No registrado", "", "", "");
      tiempoLcdHasta = millis() + 3000; lcdPost = LCD_IDLE;
      ledFijar(LED_ROJO, 3000);
      matrizMostrar(PATRON_X, CRGB::Red, 3000);
    } else {
      String claveCode = "k" + uid;
      String codigo    = almacen.getString(claveCode.c_str(), "");
      almacen.remove(uid.c_str());
      almacen.remove(claveCode.c_str());
      almacen.remove(("c" + uid).c_str());  // contador entrada/salida
      almacen.remove(("f" + uid).c_str());  // fecha ultimo acceso
      uidEliminar(uid);  // quitar de ARCHIVO_UIDS
      almacen.putString("lastDel", "DELETED|" + uid + "|" + nombre + "|" + codigo);
      Serial.println("[" + ts + "] BORRADO: " + uid + " -> " + nombre);
      lcdMostrarNombre(nombre, codigo);
      tiempoLcdHasta = millis() + 3000; lcdPost = LCD_LISTO;
      ledFijar(LED_VERDE, 3000);
      matrizMostrar(PATRON_CHECK, CRGB::Green, 3000);
    }
    resultadoEliminacion = true; modoEliminar = false; return;
  }

  if (esperandoTarjeta) {
    String claveCode = "k" + uid;
    String claveCont = "c" + uid;
    almacen.putString(uid.c_str(), nombrePendiente);
    almacen.putString(claveCode.c_str(), codigoPendiente);
    almacen.putInt(claveCont.c_str(), 0);   // reset: primera pasada sera Entrada
    uidRegistrar(uid);                      // agregar a ARCHIVO_UIDS
    almacen.putString("lastReg", "OK|" + uid + "|" + nombrePendiente + "|" + codigoPendiente + "|" + ts);
    Serial.println("[" + ts + "] REGISTRADO: " + uid + " -> " + nombrePendiente + " (" + codigoPendiente + ")");
    lcdWakeUp();
    lcdMostrarNombre(nombrePendiente, codigoPendiente);
    tiempoLcdHasta = millis() + 4000; lcdPost = LCD_LISTO;
    ledFijar(LED_VERDE, 4000);
    matrizMostrar(PATRON_CHECK, CRGB::Green, 4000);
    esperandoTarjeta = false; codigoPendiente = ""; nombrePendiente = ""; return;
  }

  // Lectura normal: registrar acceso y determinar Entrada/Salida
  String nombre    = almacen.getString(uid.c_str(), "NO_REGISTRADO");
  String claveCode = "k" + uid;
  String codigo    = almacen.getString(claveCode.c_str(), "");
  String claveCont = "c" + uid;

  // Red de seguridad: si cerrarDia no corrio a medianoche (p.ej. corte de luz),
  // la primera lectura del nuevo dia detecta el cambio de fecha, resetea el contador
  // y agrega "Salida: Pendiente" si el dia anterior termino con Entrada sin Salida.
  // Clave "f"+uid (1+max14=15 chars, dentro del limite NVS) guarda la fecha del ultimo acceso.
  String claveFecha  = "f" + uid;
  String fechaUltima = almacen.getString(claveFecha.c_str(), "");
  String fechaHoy    = (ts.length() >= 10 && ts[0] >= '0' && ts[0] <= '9')
                       ? ts.substring(0, 10) : "";
  if (fechaHoy.length() && fechaUltima.length() && fechaUltima != fechaHoy) {
    int conteoAnterior = almacen.getInt(claveCont.c_str(), 0);
    // Solo marcar Salida Pendiente si la tarjeta esta registrada en NVS.
    // cerrarDia() aplica la misma guarda; sin ella, tarjetas NO_REGISTRADO
    // acumulan un contador impar que nunca se resetea y generan un fantasma.
    bool estaRegistrado = almacen.getString(uid.c_str(), "").length() > 0;
    if (conteoAnterior % 2 == 1 && estaRegistrado) {
      entradaAgregar("Pendiente", uid, nombre, codigo, "Salida: Pendiente");
      Serial.println("NUEVO-DIA: " + uid + " -> Salida pendiente (cerrarDia tardio)");
    }
    almacen.putInt(claveCont.c_str(), 0);
    Serial.println("NUEVO-DIA: " + uid + " -> contador reseteado por cambio de dia");
  }
  if (fechaHoy.length()) almacen.putString(claveFecha.c_str(), fechaHoy);

  // Determinar tipo y escribir. Archivos primero, NVS despues (consistencia ante corte).
  int    conteo = almacen.getInt(claveCont.c_str(), 0) + 1;
  String tipo   = (conteo % 2 == 1) ? "Entrada" : "Salida";
  logAgregar(ts, uid, nombre, codigo);
  entradaAgregar(ts, uid, nombre, codigo, tipo);
  almacen.putInt(claveCont.c_str(), conteo);

  Serial.println("[" + ts + "] " + tipo + ": " + uid + " -> " + nombre);
  lcdWakeUp();
  lcdMostrarNombre(nombre, codigo);
  tiempoLcdHasta = millis() + 4000; lcdPost = LCD_IDLE;
  ledFijar(nombre != "NO_REGISTRADO" ? LED_VERDE : LED_ROJO, 4000);
  matrizMostrar(nombre != "NO_REGISTRADO" ? PATRON_FLECHA_ARRIBA : PATRON_X,
                nombre != "NO_REGISTRADO" ? CRGB::Green : CRGB::Red, 4000);
}