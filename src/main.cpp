#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>
#include <ESP_Mail_Client.h>

// ── Hardware ──────────────────────────────────────────────────
// I2C: busRtc(SDA=19,SCL=18)→RTC SD3078 | Wire(SDA=21,SCL=22)→PN532+LCD(0x27)
#define RTC_SDA        19
#define RTC_SCL        18
#define RTC_DIRECCION  0x32
#define ARCHIVO_LOG    "/logs.txt"
#define ARCHIVO_ENT    "/entradas.txt"

// ── Credenciales SMTP del remitente (cambiar antes de compilar) ──
#define SMTP_HOST        "smtp.gmail.com"
#define SMTP_PORT        587   // 587=STARTTLS (Gmail recomendado) | 465=SSL implicito
#define REMITENTE_EMAIL  "dcangrejo37@gmail.com"          // ← Tu cuenta Gmail
#define REMITENTE_CLAVE  "Lapatapaty1*"         // ← App Password de Gmail (sin espacios)

TwoWire           busRtc = TwoWire(1);
Adafruit_PN532    lectorNfc(21, 22);
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
unsigned long tiempoUltimoUid  = 0;    // ms del último UID procesado con éxito

unsigned long ultimaLecturaNfc      = 0;
unsigned long ultimoChequeoLimpieza = 0;
unsigned long ultimaReconexionWifi  = 0;
const unsigned long INTERVALO_NFC       = 300;
const unsigned long INTERVALO_LIMPIEZA  = 60000;
const unsigned long INTERVALO_WIFI      = 30000;
// Tiempo mínimo entre dos lecturas válidas del mismo UID.
// Protege contra: (a) doble lectura por fallo I2C transitorio que resetea
// tarjetaPresente mientras la tarjeta sigue físicamente cerca; (b) taps
// involuntarios en rápida sucesión.
const unsigned long COOLDOWN_TARJETA = 3000;
bool rtcDisponible = false;

// ── Estado NTP ───────────────────────────────────────────────
bool          ntpPendiente      = false;
String        ntpEstado         = "No sincronizado";
unsigned long ultimaNtpSync     = 0;
bool          wifiConectadoPrev = false;

// ── Estado email ─────────────────────────────────────────────
String emailEstado   = "Sin intentos";
String emailUltimoTs = "";

// Timer no bloqueante para volver al mensaje idle tras mostrar un nombre
unsigned long tiempoLcdHasta = 0;
enum LcdPost { LCD_IDLE, LCD_LISTO };
LcdPost lcdPost = LCD_IDLE;

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
  // Línea 4: token[3] relleno hasta col 14, código alineado a la derecha en cols 14-19
  String linea4 = tok[3].substring(0, min((int)tok[3].length(), 14));
  while ((int)linea4.length() < 14) linea4 += ' ';
  codigo = codigo.substring(0, min((int)codigo.length(), 6));
  String codAli = "";
  for (int i = (int)codigo.length(); i < 6; i++) codAli += ' ';
  codAli += codigo;
  lcdMostrar(tok[0], tok[1], tok[2], linea4 + codAli);
}

// ── RTC ──────────────────────────────────────────────────────
int bcd2bin(int v) { return (v >> 4) * 10 + (v & 0xF); }
int bin2bcd(int v) { return ((v / 10) << 4) | (v % 10); }

void rtcEscribir(int registro, int valor) {
  busRtc.beginTransmission(RTC_DIRECCION);
  busRtc.write(registro); busRtc.write(valor);
  busRtc.endTransmission();
}
int rtcLeer(int registro) {
  busRtc.beginTransmission(RTC_DIRECCION);
  busRtc.write(registro); busRtc.endTransmission(false);
  busRtc.requestFrom(RTC_DIRECCION, 1);
  return busRtc.read();
}
void rtcHabilitarEscritura() {
  // El SD3078 requiere una secuencia de TRES pasos para activar EOSC (bit 4 de 0x10):
  // El chip rechaza la escritura de EOSC si WRTC2/WRTC3 no están activos primero.
  // Paso 1: activar WRTC1 en 0x10 → habilita escritura en CTR1 (0x0F)
  // Paso 2: activar WRTC2+WRTC3 en 0x0F → habilita escritura en registros de hora
  // Paso 3: reescribir 0x10 con EOSC=1 → ahora aceptado porque WRTC1+2+3 están activos
  // EOSC (bit 4): oscilador activo en modo batería (VCC ausente). Sin él la hora congela.
  rtcEscribir(0x10, 0x80); delay(5);  // Paso 1: WRTC1=1
  rtcEscribir(0x0F, 0x84); delay(5);  // Paso 2: WRTC3=1, WRTC2=1, 24H, RTCF=0
  rtcEscribir(0x10, 0x90); delay(5);  // Paso 3: WRTC1=1 + EOSC=1
}
void rtcAjustarHora(int anio, int mes, int dia, int hora, int minuto, int segundo) {
  rtcHabilitarEscritura();
  busRtc.beginTransmission(RTC_DIRECCION);
  busRtc.write(0x00);
  busRtc.write(bin2bcd(segundo));
  busRtc.write(bin2bcd(minuto));
  busRtc.write(bin2bcd(hora) | 0x80);  // bit 7 en alto = modo 24 horas
  busRtc.write(1);                      // día de semana (no se usa en este sistema)
  busRtc.write(bin2bcd(dia));
  busRtc.write(bin2bcd(mes));
  busRtc.write(bin2bcd(anio - 2000));
  busRtc.endTransmission();
}
// Reescribe la hora actual en el RTC sin modificarla, restaurando los registros de
// control y los bits críticos que el SD3078 pierde al cortarse la alimentación:
//   - Registros 0x0F / 0x10 (write-protection): se resetean al perder VCC.
//   - Bit 7 del registro de segundos (0x00): en algunos ciclos queda en 1 (halt),
//     deteniendo el oscilador aunque la batería esté presente.
//   - Bit 7 del registro de horas (0x02): garantiza modo 24H.
void rtcReiniciarRegistros() {
  rtcHabilitarEscritura();
  busRtc.beginTransmission(RTC_DIRECCION);
  busRtc.write(0x00); busRtc.endTransmission(false);
  busRtc.requestFrom(RTC_DIRECCION, 7);
  int rSeg  = busRtc.read() & 0x7F;   // enmascarar bit 7 → oscilador activo
  int rMin  = busRtc.read() & 0x7F;
  int rHora = busRtc.read() & 0x3F;   // extraer horas sin bits de control
  busRtc.read();                       // día de semana, se descarta
  int rDia  = busRtc.read() & 0x3F;
  int rMes  = busRtc.read() & 0x1F;
  int rAnio = busRtc.read();
  busRtc.beginTransmission(RTC_DIRECCION);
  busRtc.write(0x00);
  busRtc.write(rSeg);           // bit 7 = 0 → oscilador activo
  busRtc.write(rMin);
  busRtc.write(rHora | 0x80);  // bit 7 = 1 → modo 24H garantizado
  busRtc.write(1);              // día de semana (no se usa)
  busRtc.write(rDia);
  busRtc.write(rMes);
  busRtc.write(rAnio);
  busRtc.endTransmission();
}
String obtenerTimestamp() {
  busRtc.beginTransmission(RTC_DIRECCION);
  busRtc.write(0x00); busRtc.endTransmission(false);
  busRtc.requestFrom(RTC_DIRECCION, 7);
  int seg    = bcd2bin(busRtc.read() & 0x7F);
  int minuto = bcd2bin(busRtc.read() & 0x7F);
  int hora   = bcd2bin(busRtc.read() & 0x3F);
  busRtc.read();  // día de semana, se descarta
  int dia  = bcd2bin(busRtc.read() & 0x3F);
  int mes  = bcd2bin(busRtc.read() & 0x1F);
  int anio = 2000 + bcd2bin(busRtc.read());
  if (anio < 2020 || anio > 2099 || mes < 1 || mes > 12 ||
      dia < 1 || dia > 31 || hora > 23 || minuto > 59 || seg > 59) {
    Serial.println("RTC ERROR: valores invalidos");
    return "RTC-ERROR";
  }
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", anio, mes, dia, hora, minuto, seg);
  return String(buffer);
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
  if (almacen.getString("buildID", "") != buildIdActual) {
    rtcAjustarHora(anio, mes, dia, hora, minuto, segundo);
    almacen.putString("buildID", buildIdActual);
    Serial.println("RTC: hora actualizada.");
  } else {
    rtcReiniciarRegistros();
    Serial.println("RTC: misma compilacion, hora intacta.");
  }
  Serial.println("RTC: " + obtenerTimestamp());
}

// ── Helpers RTC para leer fecha/hora sin timestamp completo ───
struct FechaHora { int anio, mes, dia, hora, minuto, segundo; };
FechaHora rtcLeerFechaHora() {
  FechaHora fh = {0,0,0,0,0,0};
  busRtc.beginTransmission(RTC_DIRECCION); busRtc.write(0x00);
  busRtc.endTransmission(false); busRtc.requestFrom(RTC_DIRECCION, 7);
  fh.segundo = bcd2bin(busRtc.read() & 0x7F);
  fh.minuto  = bcd2bin(busRtc.read() & 0x7F);
  fh.hora    = bcd2bin(busRtc.read() & 0x3F);
  busRtc.read();
  fh.dia  = bcd2bin(busRtc.read() & 0x3F);
  fh.mes  = bcd2bin(busRtc.read() & 0x1F);
  fh.anio = 2000 + bcd2bin(busRtc.read());
  return fh;
}
int ultimoDiaDelMes(int mes, int anio) {
  const int dias[] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
  bool bisiesto = (anio % 4 == 0 && (anio % 100 != 0 || anio % 400 == 0));
  return dias[mes] + (mes == 2 && bisiesto ? 1 : 0);
}

// ── HTML ─────────────────────────────────────────────────────
const char ESTILOS[] =
  "<style>body{font-family:Arial;margin:24px;max-width:720px}"
  "a,button,input{font-size:18px}button{padding:10px 14px;cursor:pointer}"
  "input{padding:10px;width:100%;box-sizing:border-box}"
  ".card{border:1px solid #ddd;border-radius:10px;padding:16px;margin:12px 0}"
  ".muted{color:#666;font-size:14px}.ts{color:#444;font-size:13px;font-family:monospace}"
  ".btn-danger{background:#dc3545;color:white;font-weight:bold}"
  ".btn-ok{background:#28a745;color:white;font-weight:bold}"
  ".btn-email{background:#0066cc;color:white;font-weight:bold}"
  ".entrada{color:#28a745;font-weight:bold}.salida{color:#dc3545;font-weight:bold}</style>";

String pagina(const String& titulo, const String& cuerpo) {
  return "<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>" + titulo + "</title>" + ESTILOS + "</head><body>" + cuerpo + "</body></html>";
}

// ── Logs de acceso (LittleFS) ─────────────────────────────────
void logAgregar(const String& ts, const String& uid, const String& nombre, const String& codigo) {
  File f = LittleFS.open(ARCHIVO_LOG, "a");
  if (f) { f.println(ts + "|" + uid + "|" + nombre + "|" + codigo); f.close(); }
}

// Parsea "timestamp|uid|nombre|codigo". El 4to campo es opcional para compatibilidad
// con logs viejos que solo tienen 3 campos.
bool logParsear(const String& entrada, String &ts, String &uid, String &nombre, String &codigo) {
  int pos1 = entrada.indexOf('|'), pos2 = entrada.indexOf('|', pos1 + 1);
  if (pos1 < 0 || pos2 < 0) return false;
  ts  = entrada.substring(0, pos1);
  uid = entrada.substring(pos1+1, pos2);
  int pos3 = entrada.indexOf('|', pos2 + 1);
  if (pos3 < 0) {
    nombre = entrada.substring(pos2+1);
    codigo = "";
  } else {
    nombre = entrada.substring(pos2+1, pos3);
    codigo = entrada.substring(pos3+1);
  }
  return true;
}

String logHtml() {
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (!f || !f.size())
    return "<div class='card'><div class='muted'>No hay eventos registrados.</div></div>";

  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.seek(0);

  int mostrar = min(total, 300);
  int saltar  = total - mostrar;
  String* buf = new String[mostrar];
  int n = 0, fila = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    if (fila++ < saltar) continue;
    buf[n++] = l;
  }
  f.close();

  String resultado = "<div class='card'><div class='muted'>Total: " + String(total) + " eventos";
  if (total > 300) resultado += " (mostrando los ultimos 300)";
  resultado += "</div><hr>";
  for (int i = n - 1; i >= 0; i--) {
    String ts, uid, nombre, codigo;
    if (!logParsear(buf[i], ts, uid, nombre, codigo)) continue;
    resultado += "<div><b>" + nombre + "</b>";
    if (codigo.length()) resultado += " <span class='muted'>[" + codigo + "]</span>";
    resultado += " <span class='muted'>(UID: " + uid + ")</span><br>"
                 "<span class='ts'>&#128336; " + ts + "</span></div><hr>";
  }
  delete[] buf;
  return resultado + "</div>";
}

// Genera CSV con encabezado para descarga y adjunto de email
String logCsv() {
  String resultado = "Timestamp,UID,Nombre,Codigo\n";
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (!f || !f.size()) return resultado;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    String ts, uid, nombre, codigo;
    if (!logParsear(l, ts, uid, nombre, codigo)) continue;
    resultado += ts + "," + uid + "," + nombre + "," + codigo + "\n";
  }
  f.close();
  return resultado;
}

// ── Entradas / Salidas (LittleFS) ────────────────────────────
void entradaAgregar(const String& ts, const String& uid, const String& nombre,
                    const String& codigo, const String& tipo) {
  File f = LittleFS.open(ARCHIVO_ENT, "a");
  if (f) { f.println(ts + "|" + uid + "|" + nombre + "|" + codigo + "|" + tipo); f.close(); }
}

bool entradaParsear(const String& linea, String &ts, String &uid, String &nombre,
                    String &codigo, String &tipo) {
  int p1 = linea.indexOf('|');
  int p2 = linea.indexOf('|', p1+1);
  int p3 = linea.indexOf('|', p2+1);
  int p4 = linea.indexOf('|', p3+1);
  if (p1<0 || p2<0 || p3<0 || p4<0) return false;
  ts     = linea.substring(0, p1);
  uid    = linea.substring(p1+1, p2);
  nombre = linea.substring(p2+1, p3);
  codigo = linea.substring(p3+1, p4);
  tipo   = linea.substring(p4+1);
  tipo.trim();
  return true;
}

// Vista HTML agrupada por UID/nombre, con entradas cronológicas dentro de cada grupo
String entradaHtml() {
  File f = LittleFS.open(ARCHIVO_ENT, "r");
  if (!f || !f.size())
    return "<div class='card'><div class='muted'>No hay registros de entradas/salidas.</div></div>";

  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.seek(0);

  int mostrar = min(total, 300);
  int saltar  = total - mostrar;
  String* buf = new String[mostrar];
  int n = 0, fila = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    if (fila++ < saltar) continue;
    buf[n++] = l;
  }
  f.close();

  // Recopilar UIDs únicos en orden de primera aparición (máx 100 usuarios)
  const int MAX_UIDS = 100;
  String uids[MAX_UIDS]; int nu = 0;
  for (int i = 0; i < n && nu < MAX_UIDS; i++) {
    String ts, uid, nombre, codigo, tipo;
    if (!entradaParsear(buf[i], ts, uid, nombre, codigo, tipo)) continue;
    bool existe = false;
    for (int j = 0; j < nu; j++) { if (uids[j] == uid) { existe = true; break; } }
    if (!existe) uids[nu++] = uid;
  }

  String resultado = "<div class='card'><div class='muted'>Total: " + String(total) + " registros";
  if (total > 300) resultado += " (mostrando los ultimos 300)";
  resultado += "</div><hr>";

  for (int u = 0; u < nu; u++) {
    // Obtener el nombre del primer registro de este UID
    String nombreGrupo = uids[u];
    for (int i = 0; i < n; i++) {
      String ts, uid, nombre, codigo, tipo;
      if (!entradaParsear(buf[i], ts, uid, nombre, codigo, tipo)) continue;
      if (uid == uids[u]) { nombreGrupo = nombre; break; }
    }
    resultado += "<div class='card' style='margin:6px 0'>"
                 "<b>" + nombreGrupo + "</b> <span class='muted'>(UID: " + uids[u] + ")</span>";
    for (int i = 0; i < n; i++) {
      String ts, uid, nombre, codigo, tipo;
      if (!entradaParsear(buf[i], ts, uid, nombre, codigo, tipo)) continue;
      if (uid != uids[u]) continue;
      String cls = (tipo == "Entrada") ? "entrada" : "salida";
      resultado += "<div style='padding:3px 0 3px 8px'>"
                   "<span class='" + cls + "'>&#x25cf; " + tipo + "</span>"
                   " <span class='ts'>&#128336; " + ts + "</span>";
      if (codigo.length()) resultado += " <span class='muted'>[" + codigo + "]</span>";
      resultado += "</div>";
    }
    resultado += "</div>";
  }

  delete[] buf;
  return resultado + "</div>";
}

// Genera CSV con encabezado para descarga y adjunto de email
String entradaCsv() {
  String resultado = "Timestamp,UID,Nombre,Codigo,Tipo\n";
  File f = LittleFS.open(ARCHIVO_ENT, "r");
  if (!f || !f.size()) return resultado;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    String ts, uid, nombre, codigo, tipo;
    if (!entradaParsear(l, ts, uid, nombre, codigo, tipo)) continue;
    resultado += ts + "," + uid + "," + nombre + "," + codigo + "," + tipo + "\n";
  }
  f.close();
  return resultado;
}

// ── NTP ──────────────────────────────────────────────────────
// Aplica la hora obtenida de NTP al RTC y actualiza el estado global.
void aplicarTiempoNtp(struct tm& info) {
  int anio = info.tm_year + 1900;
  int mes  = info.tm_mon  + 1;
  int dia  = info.tm_mday;
  int hora = info.tm_hour;
  int min  = info.tm_min;
  int seg  = info.tm_sec;
  if (anio < 2024 || anio > 2099) {
    ntpEstado = "NTP: anio invalido (" + String(anio) + ")";
    Serial.println(ntpEstado);
    return;
  }
  if (rtcDisponible) rtcAjustarHora(anio, mes, dia, hora, min, seg);
  ultimaNtpSync = millis();
  ntpEstado = "Sincronizado: " + obtenerTimestamp();
  Serial.println("NTP OK -> " + ntpEstado);
}

// Inicia sincronizacion NTP de forma no bloqueante.
// El resultado se aplica en verificarNtpPendiente() desde loop().
void iniciarNtp() {
  if (WiFi.status() != WL_CONNECTED) return;
  long offset = almacen.getInt("ntpOffset", -18000);  // UTC-5 Colombia por defecto
  configTime(offset, 0, "pool.ntp.org", "time.nist.gov");
  ntpPendiente = true;
  ntpEstado    = "Sincronizando...";
  Serial.printf("NTP: configurado offset=%lds, esperando respuesta...\n", offset);
}

// Llama desde loop(): si el NTP ya respondio, aplica la hora al RTC.
void verificarNtpPendiente() {
  if (!ntpPendiente) return;
  struct tm info;
  if (!getLocalTime(&info, 0)) return;  // aun no hay respuesta
  ntpPendiente = false;
  aplicarTiempoNtp(info);
}

// Sincronizacion bloqueante (max 10 s) para solicitud manual del usuario.
bool sincronizarNtpManual() {
  if (WiFi.status() != WL_CONNECTED) { ntpEstado = "Sin WiFi"; return false; }
  long offset = almacen.getInt("ntpOffset", -18000);
  configTime(offset, 0, "pool.ntp.org", "time.nist.gov");
  struct tm info;
  unsigned long inicio = millis();
  while (millis() - inicio < 10000) {
    if (getLocalTime(&info, 500)) { ntpPendiente = false; aplicarTiempoNtp(info); return true; }
  }
  ntpEstado = "NTP: timeout (10 s)";
  Serial.println(ntpEstado);
  return false;
}

// ── Email ─────────────────────────────────────────────────────
void smtpCallback(SMTP_Status status) {
  Serial.print("SMTP cb: "); Serial.println(status.info());
}

// Envía ambos CSV como adjuntos al email guardado en NVS.
// Devuelve true si el envío fue exitoso.
// Puerto 587 con STARTTLS (recomendado para Gmail con App Password).
// IMPORTANTE: REMITENTE_CLAVE debe ser un App Password de 16 chars (sin espacios),
// no la contraseña normal de la cuenta Gmail.
bool enviarEmail(const String& csvLog, const String& csvEntradas) {
  String destino = almacen.getString("emailDest", "");
  if (!destino.length()) { emailEstado = "Sin destinatario configurado"; return false; }
  if (WiFi.status() != WL_CONNECTED) { emailEstado = "Sin conexion WiFi"; return false; }

  String fechaHoy = rtcDisponible ? obtenerTimestamp().substring(0, 10) : "sin-fecha";
  String asunto   = "Registros NFC - " + fechaHoy;
  String cuerpo   = "Reporte automatico del sistema NFC.\n"
                    "Fecha: " + fechaHoy + "\n\n"
                    "Adjuntos:\n"
                    "  - accesos.csv  : log de accesos por tarjeta\n"
                    "  - entradas.csv : registro de entradas y salidas\n";

  Session_Config config;
  config.server.host_name  = SMTP_HOST;
  config.server.port       = SMTP_PORT;
  config.login.email       = REMITENTE_EMAIL;
  config.login.password    = REMITENTE_CLAVE;
  config.login.user_domain = "";
  // STARTTLS (port 587) o SSL implicito (port 465)
  config.secure.startTLS   = (SMTP_PORT == 587);
  // NTP interno de la libreria como respaldo para validar certificados TLS
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

  SMTP_Attachment attLog;
  attLog.descr.name              = F("accesos.csv");
  attLog.descr.mime              = F("text/csv");
  attLog.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
  attLog.blob.data               = (uint8_t*)csvLog.c_str();
  attLog.blob.size               = csvLog.length();
  message.addAttachment(attLog);

  SMTP_Attachment attEnt;
  attEnt.descr.name              = F("entradas.csv");
  attEnt.descr.mime              = F("text/csv");
  attEnt.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
  attEnt.blob.data               = (uint8_t*)csvEntradas.c_str();
  attEnt.blob.size               = csvEntradas.length();
  message.addAttachment(attEnt);

  smtp.debug(1);  // log completo por Serial de todo el dialogo SMTP
  smtp.callback(smtpCallback);

  Serial.printf("SMTP: conectando a %s:%d startTLS=%d remitente=%s dest=%s\n",
    SMTP_HOST, SMTP_PORT, (SMTP_PORT == 587),
    REMITENTE_EMAIL, destino.c_str());
  emailEstado = "Conectando a " + String(SMTP_HOST) + ":" + String(SMTP_PORT) + "...";

  if (!smtp.connect(&config)) {
    emailEstado = "Error conexion: " + smtp.errorReason();
    Serial.println("SMTP conexion FALLO: " + smtp.errorReason());
    smtp.closeSession();
    return false;
  }
  Serial.println("SMTP: conexion OK. Autenticando y enviando...");
  emailEstado = "Enviando mensaje...";

  bool ok = MailClient.sendMail(&smtp, &message, true);
  if (ok) {
    emailEstado   = "Enviado OK - " + (rtcDisponible ? obtenerTimestamp() : fechaHoy);
    emailUltimoTs = emailEstado.substring(emailEstado.indexOf('-') + 2);
    Serial.println("SMTP: envio EXITOSO");
  } else {
    emailEstado = "Error envio: " + smtp.errorReason();
    Serial.println("SMTP envio FALLO: " + smtp.errorReason());
  }
  smtp.closeSession();
  return ok;
}

// ── Auto-envío de email en días fijos del mes ─────────────────
// Días: 10, 20 y último del mes, a medianoche (hora 00:xx).
// Usa lastEmail en NVS (mismo patrón que lastClean) para evitar doble envío.
void autoEnviarEmail() {
  if (!rtcDisponible || WiFi.status() != WL_CONNECTED) return;
  if (!almacen.getString("emailDest", "").length()) return;

  FechaHora fh = rtcLeerFechaHora();
  if (fh.hora != 0) return;

  int ud = ultimoDiaDelMes(fh.mes, fh.anio);
  if (fh.dia != 10 && fh.dia != 20 && fh.dia != ud) return;

  char hoy[11]; snprintf(hoy, sizeof(hoy), "%04d-%02d-%02d", fh.anio, fh.mes, fh.dia);
  if (almacen.getString("lastEmail", "") == String(hoy)) return;

  Serial.println("AUTO-EMAIL: generando CSV...");
  String csvLog = logCsv();
  String csvEnt = entradaCsv();

  if (enviarEmail(csvLog, csvEnt)) {
    LittleFS.remove(ARCHIVO_LOG);
    LittleFS.remove(ARCHIVO_ENT);
    almacen.putString("lastEmail", String(hoy));
    Serial.println("AUTO-EMAIL enviado: " + String(hoy));
  } else {
    Serial.println("AUTO-EMAIL error: no se pudo enviar.");
  }
}

// ── Auto-limpieza de logs ─────────────────────────────────────
// Días: 15 y último del mes, a medianoche. El email (días 10/20/último) corre
// primero en loop(), así que si el email del último día falla, la limpieza
// elimina los archivos de todas formas para liberar LittleFS.
void autoLimpiarLogs() {
  if (!rtcDisponible) return;
  FechaHora fh = rtcLeerFechaHora();
  if (fh.hora != 0) return;

  int ud = ultimoDiaDelMes(fh.mes, fh.anio);
  if (fh.dia != 15 && fh.dia != ud) return;

  char hoy[11]; snprintf(hoy, sizeof(hoy), "%04d-%02d-%02d", fh.anio, fh.mes, fh.dia);
  if (almacen.getString("lastClean", "") == String(hoy)) return;

  LittleFS.remove(ARCHIVO_LOG);
  almacen.putString("lastClean", String(hoy));
  Serial.println("AUTO-LIMPIEZA: " + String(hoy));
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
  // Resetear estado NFC para que cualquier tarjeta sea aceptada inmediatamente,
  // sin que el cooldown o tarjetaPresente bloqueen la tarjeta que se va a registrar.
  tarjetaPresente = false; ultimoUid = ""; tiempoUltimoUid = 0;
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
  // Resetear estado NFC igual que en registro: cualquier tarjeta debe ser detectada
  // inmediatamente sin importar si ya fue leída antes en este ciclo.
  tarjetaPresente = false; ultimoUid = ""; tiempoUltimoUid = 0;
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
  if (ultimo.startsWith("OK|")) { almacen.putString("lastReg", ""); servidor.send(200, "text/plain", ultimo); return; }
  servidor.send(200, "text/plain", "Listo.");
}

void handleDone() {
  String datos  = servidor.hasArg("d") ? servidor.arg("d") : "";
  String cuerpo = "<h2>&#10003; Registrado</h2><div class='card'>";
  if (datos.startsWith("OK|")) {
    int pos1 = datos.indexOf('|'),
        pos2 = datos.indexOf('|', pos1+1),
        pos3 = datos.indexOf('|', pos2+1),
        pos4 = (pos3 > 0) ? datos.indexOf('|', pos3+1) : -1;
    cuerpo += "<p>UID: <b>" + datos.substring(pos1+1, pos2) + "</b></p>"
              "<p>Nombre: <b>" + datos.substring(pos2+1, pos3>0 ? pos3 : datos.length()) + "</b></p>";
    if (pos3 > 0) cuerpo += "<p>Codigo: <b>" + datos.substring(pos3+1, pos4>0 ? pos4 : datos.length()) + "</b></p>";
    if (pos4 > 0) cuerpo += "<p class='ts'>&#128336; " + datos.substring(pos4+1) + "</p>";
  } else cuerpo += "<p class='muted'>Sin datos.</p>";
  cuerpo += "<a href='/'><button>Inicio</button></a> <a href='/logs'><button>Ver logs</button></a></div>";
  servidor.send(200, "text/html", pagina("Registrado", cuerpo));
}

void handleDeleted() {
  String datos  = servidor.hasArg("d") ? servidor.arg("d") : "";
  String cuerpo = "<h2>Usuario borrado</h2><div class='card'>";
  if (datos.startsWith("DELETED|")) {
    int pos1 = datos.indexOf('|'),
        pos2 = datos.indexOf('|', pos1+1),
        pos3 = datos.indexOf('|', pos2+1);
    cuerpo += "<p style='color:#dc3545;font-weight:bold'>&#10003; Usuario eliminado</p>"
              "<p>UID: <b>" + datos.substring(pos1+1, pos2) + "</b></p>"
              "<p>Nombre: <b>" + datos.substring(pos2+1, pos3>0 ? pos3 : datos.length()) + "</b></p>";
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
    "<a href='/clearLogs' onclick='return confirm(\"Borrar todos los logs?\");'>"
    "<button class='btn-danger'>&#128465; Borrar logs</button></a></div>"));
}

void handleClearLogs() {
  int total = 0;
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (f) { while (f.available()) { if (f.read() == '\n') total++; } f.close(); }
  LittleFS.remove(ARCHIVO_LOG);
  servidor.send(200, "text/html", pagina("Logs borrados",
    "<h2>Logs borrados</h2><div class='card'>"
    "<p>&#10003; Eliminados <b>" + String(total) + "</b> eventos.</p>"
    "<p class='muted'>Las asociaciones UID&rarr;Nombre NO fueron eliminadas.</p>"
    "<a href='/logs'><button>Ver logs</button></a> "
    "<a href='/'><button>Inicio</button></a></div>"));
}

void handleDownloadLogs() {
  String ts = rtcDisponible ? obtenerTimestamp().substring(0, 10) : "logs";
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"accesos_" + ts + ".csv\"");
  servidor.send(200, "text/csv; charset=utf-8", logCsv());
}

void handleCancelar() {
  esperandoTarjeta = false;
  modoEliminar     = false;
  nombrePendiente  = "";
  codigoPendiente  = "";
  // Resetear estado NFC al cancelar para que la siguiente lectura normal funcione sin
  // cooldown residual de una operación de registro/eliminación que fue cancelada.
  tarjetaPresente = false; ultimoUid = ""; tiempoUltimoUid = 0;
  lcdMostrar("Cancelado", "", "", "");
  servidor.send(200, "text/html", pagina("Cancelado",
    "<h2>Operacion cancelada</h2><div class='card'>"
    "<p class='muted'>No se realizó ningun cambio.</p>"
    "<a href='/'><button>Inicio</button></a> "
    "<a href='/register'><button>Registrar usuario</button></a>"
    "</div>"));
}

// ── Handlers: Entradas/Salidas ────────────────────────────────
void handleEntradas() {
  servidor.send(200, "text/html", pagina("Entradas/Salidas",
    "<h2>Registros de Entradas/Salidas</h2>" + entradaHtml() +
    "<div style='margin:20px 0;display:flex;flex-wrap:wrap;gap:8px'>"
    "<a href='/'><button>Volver</button></a> "
    "<a href='/downloadEntradas'><button class='btn-ok'>&#128229; Descargar CSV</button></a> "
    "<a href='/clearEntradas' onclick='return confirm(\"Borrar todos los registros de entradas?\");'>"
    "<button class='btn-danger'>&#128465; Borrar registros</button></a></div>"));
}

void handleDownloadEntradas() {
  String ts = rtcDisponible ? obtenerTimestamp().substring(0, 10) : "entradas";
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"entradas_" + ts + ".csv\"");
  servidor.send(200, "text/csv; charset=utf-8", entradaCsv());
}

void handleClearEntradas() {
  int total = 0;
  File f = LittleFS.open(ARCHIVO_ENT, "r");
  if (f) { while (f.available()) { if (f.read() == '\n') total++; } f.close(); }
  LittleFS.remove(ARCHIVO_ENT);
  servidor.send(200, "text/html", pagina("Registros borrados",
    "<h2>Registros borrados</h2><div class='card'>"
    "<p>&#10003; Eliminados <b>" + String(total) + "</b> registros.</p>"
    "<a href='/entradas'><button>Ver entradas</button></a> "
    "<a href='/'><button>Inicio</button></a></div>"));
}

// ── Handler: Configuracion WiFi + NTP + email ────────────────
void handleConfig() {
  String ssidGuardado  = almacen.getString("ssidWifi",  "");
  String emailGuardado = almacen.getString("emailDest", "");
  String ultimoEmail   = almacen.getString("lastEmail", "(nunca)");
  String estadoWifi    = (WiFi.status() == WL_CONNECTED)
    ? "<span style='color:#28a745'>Conectado &mdash; " + WiFi.localIP().toString() + "</span>"
    : "<span style='color:#dc3545'>Desconectado</span>";

  // Offset NTP actual en horas (guardado en segundos)
  long   offsetSec = almacen.getInt("ntpOffset", -18000);
  String offsetHStr = String(offsetSec / 3600);

  // Tiempo desde ultimo sync NTP
  String ntpSyncStr = "Nunca";
  if (ultimaNtpSync > 0) {
    unsigned long segs = (millis() - ultimaNtpSync) / 1000;
    if (segs < 60)       ntpSyncStr = "hace " + String(segs) + " s";
    else if (segs < 3600) ntpSyncStr = "hace " + String(segs/60) + " min";
    else                  ntpSyncStr = "hace " + String(segs/3600) + " h";
  }

  // Color estado email
  String emailColor = "#666";
  if (emailEstado.startsWith("Enviado")) emailColor = "#28a745";
  else if (emailEstado.startsWith("Error")) emailColor = "#dc3545";

  servidor.send(200, "text/html", pagina("Configuracion",
    "<h2>&#9881; Configuracion</h2>"

    // ── WiFi ──
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
    "<p class='muted'>Reportes automaticos los dias 10, 20 y ultimo del mes a medianoche.</p>"
    "</div>"

    // ── NTP ──
    "<div class='card'><h3>&#128336; Sincronizacion NTP</h3>"
    "<p>Estado: <b>" + ntpEstado + "</b></p>"
    "<p class='muted'>Ultima sincronizacion: " + ntpSyncStr + "</p>"
    "<a href='/syncNtp'><button class='btn-ok'>&#8635; Sincronizar hora por NTP ahora</button></a>"
    "</div>"

    // ── Email ──
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

  // Guardar offset de zona horaria NTP
  if (servidor.hasArg("ntpOffsetH")) {
    long offsetH   = servidor.arg("ntpOffsetH").toInt();
    if (offsetH >= -12 && offsetH <= 14) {
      almacen.putInt("ntpOffset", (int)(offsetH * 3600));
      Serial.printf("NTP: zona horaria guardada UTC%+ld\n", offsetH);
    }
  }

  // Reconectar STA con las nuevas credenciales
  if (ssid.length()) {
    String claveUsar = clave.length() ? clave : almacen.getString("claveWifi", "");
    WiFi.disconnect(false);
    delay(200);
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

// ── Handler: Envío manual de email ───────────────────────────
void handleSendEmail() {
  String destino = almacen.getString("emailDest", "");
  if (!destino.length()) {
    servidor.send(200, "text/html", pagina("Sin destinatario",
      "<h2>Email no configurado</h2><div class='card'>"
      "<p>Configure el email destino en "
      "<a href='/config'>Configuracion</a>.</p>"
      "<a href='/logs'><button>Volver</button></a></div>"));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    servidor.send(200, "text/html", pagina("Sin conexion",
      "<h2>Sin conexion a internet</h2><div class='card'>"
      "<p>Configure la red WiFi en "
      "<a href='/config'>Configuracion</a>.</p>"
      "<a href='/logs'><button>Volver</button></a></div>"));
    return;
  }

  // Responder inmediatamente y enviar en background sería lo ideal, pero el WebServer
  // de Arduino no lo soporta bien. Se envía sincrónicamente y el navegador espera.
  servidor.sendHeader("Content-Type", "text/html; charset=utf-8");

  String csvLog = logCsv();
  String csvEnt = entradaCsv();
  bool ok = enviarEmail(csvLog, csvEnt);

  if (ok) {
    String hoy = rtcDisponible ? obtenerTimestamp().substring(0, 10) : "";
    if (hoy.length()) almacen.putString("lastEmail", hoy);
    LittleFS.remove(ARCHIVO_LOG);
    LittleFS.remove(ARCHIVO_ENT);
    servidor.send(200, "text/html", pagina("Email enviado",
      "<h2>&#10003; Email enviado</h2><div class='card'>"
      "<p>Archivos enviados a <b>" + destino + "</b>.</p>"
      "<p class='muted'>Los archivos de log y entradas han sido borrados.</p>"
      "<a href='/'><button>Inicio</button></a></div>"));
  } else {
    servidor.send(200, "text/html", pagina("Error al enviar",
      "<h2>&#10060; Error al enviar</h2><div class='card'>"
      "<p>No se pudo enviar el correo.</p>"
      "<p class='muted'>Verifique las credenciales SMTP en el firmware y que el WiFi tenga internet.</p>"
      "<p class='muted'>Los archivos NO fueron borrados.</p>"
      "<a href='/logs'><button>Volver a logs</button></a></div>"));
  }
}

// ── Handler: Sincronizacion NTP manual ───────────────────────
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

// ── NFC ──────────────────────────────────────────────────────
String uidAHex(byte* uid, byte longitud) {
  String hex = "";
  for (int i = 0; i < longitud; i++) { if (uid[i] < 16) hex += "0"; hex += String(uid[i], HEX); }
  hex.toUpperCase(); return hex;
}
bool leerUidUnaVez(String &uidSalida) {
  byte uid[7]; byte longitud = 0;
  if (!lectorNfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &longitud, 50)) {
    if (tarjetaPresente) {
      tarjetaPresente = false;
      Serial.println("Tarjeta retirada");
      // ultimoUid NO se borra: el cooldown por tiempo sigue protegiendo contra
      // re-lecturas causadas por pérdida I2C transitoria del PN532 mientras la
      // tarjeta sigue físicamente cerca del lector.
    }
    return false;
  }

  String hex = uidAHex(uid, longitud);
  unsigned long ahora = millis();

  // Doble condición de anti-repetición:
  //   1. Mismo UID y tarjeta físicamente presente (tarjetaPresente = true).
  //   2. O mismo UID y no han pasado COOLDOWN_TARJETA ms desde la última lectura válida.
  // Cualquiera de las dos condiciones bloquea una segunda lectura del mismo UID.
  // Esto cubre el caso de fallo I2C transitorio que pone tarjetaPresente=false
  // brevemente mientras la tarjeta sigue sobre el lector.
  if (hex == ultimoUid && (tarjetaPresente || (ahora - tiempoUltimoUid < COOLDOWN_TARJETA))) {
    tarjetaPresente = true;  // mantener estado coherente
    return false;
  }

  tarjetaPresente = true;
  ultimoUid       = uidSalida = hex;
  tiempoUltimoUid = ahora;
  return true;
}

// ── Setup & Loop ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(500);

  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  lcdMostrar("Sistema NFC", "Iniciando...", "", "");

  // NVS y LittleFS deben inicializarse antes que el RTC (buildID usa NVS)
  almacen.begin("nfc", false);
  LittleFS.begin(true);

  busRtc.begin(RTC_SDA, RTC_SCL); delay(50);
  busRtc.beginTransmission(RTC_DIRECCION);
  rtcDisponible = (busRtc.endTransmission() == 0);
  if (rtcDisponible) {
    rtcEscribir(0x0F, 0x80); delay(5);  // battery switchover antes de cualquier otra config
    rtcSincronizarSiNecesario();
    int dbg00 = rtcLeer(0x00);
    Serial.printf("RTC 0x00 (seg)  = 0x%02X  bit7(CH)=%d\n",  dbg00, (dbg00>>7)&1);
    Serial.printf("RTC 0x0E (CTR0) = 0x%02X\n",               rtcLeer(0x0E));
    Serial.printf("RTC 0x0F (CTR1) = 0x%02X  WRTC3=%d WRTC2=%d RTCF=%d 24H=%d\n",
      rtcLeer(0x0F),
      (rtcLeer(0x0F)>>7)&1, (rtcLeer(0x0F)>>2)&1,
      (rtcLeer(0x0F)>>1)&1, !((rtcLeer(0x0F))&1));
    Serial.printf("RTC 0x10 (CTR2) = 0x%02X  WRTC1=%d EOSC=%d\n",
      rtcLeer(0x10),
      (rtcLeer(0x10)>>7)&1, (rtcLeer(0x10)>>4)&1);
  } else {
    Serial.println("AVISO: RTC no encontrado. Usando millis().");
  }

  lectorNfc.begin();
  uint32_t versionFirmware = lectorNfc.getFirmwareVersion();
  Serial.println(versionFirmware
    ? "PN532 OK. Fw v" + String((versionFirmware >> 16) & 0xFF)
    : "ERROR: PN532 no detectado!");
  lectorNfc.SAMConfig();

  // Modo AP+STA: el ESP32 es Access Point para clientes locales Y cliente WiFi para internet
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(SSID_AP, CLAVE_AP, 6);
  Serial.println("AP: " + String(SSID_AP) + " | IP: " + WiFi.softAPIP().toString());

  // Si hay credenciales WiFi guardadas, intentar conectarse automáticamente
  String ssidGuardado = almacen.getString("ssidWifi", "");
  if (ssidGuardado.length()) {
    String claveGuardada = almacen.getString("claveWifi", "");
    WiFi.begin(ssidGuardado.c_str(), claveGuardada.c_str());
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
  servidor.on("/clearLogs",        handleClearLogs);
  servidor.on("/cancelar",         handleCancelar);
  servidor.on("/entradas",         handleEntradas);
  servidor.on("/downloadEntradas", handleDownloadEntradas);
  servidor.on("/clearEntradas",    handleClearEntradas);
  servidor.on("/config",           handleConfig);
  servidor.on("/saveConfig",       HTTP_POST, handleSaveConfig);
  servidor.on("/sendEmail",        handleSendEmail);
  servidor.on("/syncNtp",          handleSyncNtp);
  servidor.begin();
  Serial.println("Servidor web iniciado!");

  lcdMostrar("Sistema NFC", "Listo...", "", "");
}

void loop() {
  servidor.handleClient();
  unsigned long ahora = millis();

  // Volver al mensaje idle cuando expira el timer del LCD
  if (tiempoLcdHasta && ahora >= tiempoLcdHasta) {
    tiempoLcdHasta = 0;
    if (lcdPost == LCD_IDLE) lcdMostrar("Esperando", "tarjeta...", "", "");
    else                     lcdMostrar("Listo", "", "", "");
  }

  // Detectar cambio de estado WiFi: lanzar NTP al conectarse
  bool wifiAhora = (WiFi.status() == WL_CONNECTED);
  if (wifiAhora && !wifiConectadoPrev) {
    Serial.println("WiFi: conexion establecida. Iniciando NTP...");
    iniciarNtp();
  }
  wifiConectadoPrev = wifiAhora;

  // Verificar si el NTP ya respondio y aplicar hora al RTC
  verificarNtpPendiente();

  // Monitorear WiFi STA: reintentar conexión si se pierde
  if (ahora - ultimaReconexionWifi >= INTERVALO_WIFI) {
    ultimaReconexionWifi = ahora;
    if (!wifiAhora) {
      String ssid = almacen.getString("ssidWifi", "");
      if (ssid.length()) {
        String clave = almacen.getString("claveWifi", "");
        WiFi.begin(ssid.c_str(), clave.c_str());
      }
    }
  }

  // Auto-email primero, luego auto-limpieza (email debe correr antes para no perder datos)
  if (ahora - ultimoChequeoLimpieza >= INTERVALO_LIMPIEZA) {
    ultimoChequeoLimpieza = ahora;
    autoEnviarEmail();
    autoLimpiarLogs();
  }

  if (ahora - ultimaLecturaNfc < INTERVALO_NFC) return;
  ultimaLecturaNfc = ahora;

  String uid;
  if (!leerUidUnaVez(uid)) return;
  String ts = rtcDisponible ? obtenerTimestamp() : "ms:" + String(millis());

  if (modoEliminar) {
    String nombre = almacen.getString(uid.c_str(), "");
    if (!nombre.length()) {
      almacen.putString("lastDel", "NOT_FOUND|" + uid);
      Serial.println("TARJETA NO REGISTRADA: " + uid);
      lcdMostrar("No registrado", "", "", "");
      tiempoLcdHasta = millis() + 3000;
      lcdPost = LCD_IDLE;
    } else {
      String claveCode = "k" + uid;
      String codigo = almacen.getString(claveCode.c_str(), "");
      almacen.remove(uid.c_str());
      almacen.remove(claveCode.c_str());
      // Resetear el conteo de entradas/salidas al borrar el usuario
      String claveCont = "c" + uid;  // "c" + max 14 hex chars = 15 chars (límite NVS OK)
      almacen.remove(claveCont.c_str());
      almacen.putString("lastDel", "DELETED|" + uid + "|" + nombre + "|" + codigo);
      Serial.println("[" + ts + "] BORRADO: " + uid + " -> " + nombre);
      lcdMostrarNombre(nombre, codigo);
      tiempoLcdHasta = millis() + 3000;
      lcdPost = LCD_LISTO;
    }
    resultadoEliminacion = true; modoEliminar = false; return;
  }

  if (esperandoTarjeta) {
    String claveCode = "k" + uid;
    String claveCont = "c" + uid;
    almacen.putString(uid.c_str(), nombrePendiente);
    almacen.putString(claveCode.c_str(), codigoPendiente);
    // Resetear el contador de Entrada/Salida al registrar (o re-registrar) una tarjeta,
    // para que la primera pasada tras el registro siempre sea "Entrada".
    almacen.putInt(claveCont.c_str(), 0);
    almacen.putString("lastReg", "OK|" + uid + "|" + nombrePendiente + "|" + codigoPendiente + "|" + ts);
    Serial.println("[" + ts + "] REGISTRADO: " + uid + " -> " + nombrePendiente + " (" + codigoPendiente + ")");
    lcdMostrarNombre(nombrePendiente, codigoPendiente);
    tiempoLcdHasta = millis() + 4000;
    lcdPost = LCD_LISTO;
    esperandoTarjeta = false; codigoPendiente = ""; nombrePendiente = ""; return;
  }

  // Lectura normal: registrar acceso y determinar entrada/salida
  String nombre = almacen.getString(uid.c_str(), "NO_REGISTRADO");
  String claveCode = "k" + uid;
  String codigo = almacen.getString(claveCode.c_str(), "");

  // Determinar tipo ANTES de escribir, y escribir ambos archivos ANTES de
  // actualizar el contador NVS: si hay corte de luz entre los dos pasos,
  // los archivos quedan consistentes entre si y el contador se puede recalcular.
  String claveCont = "c" + uid;
  int conteo = almacen.getInt(claveCont.c_str(), 0) + 1;
  String tipo = (conteo % 2 == 1) ? "Entrada" : "Salida";
  logAgregar(ts, uid, nombre, codigo);
  entradaAgregar(ts, uid, nombre, codigo, tipo);
  almacen.putInt(claveCont.c_str(), conteo);  // actualizar despues de ambas escrituras

  Serial.println("[" + ts + "] " + tipo + ": " + uid + " -> " + nombre);
  lcdMostrarNombre(nombre, codigo);
  tiempoLcdHasta = millis() + 4000;
  lcdPost = LCD_IDLE;
}
