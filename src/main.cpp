#include <Arduino.h>

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LittleFS.h>           // sistema de archivos en flash para los logs
#include <Adafruit_PN532.h>

// I2C: busRtc(SDA=19,SCL=18)→RTC SD3078 | Wire(SDA=21,SCL=22)→PN532
#define RTC_SDA        19
#define RTC_SCL        18
#define RTC_DIRECCION  0x32
#define REG_FIRMA1     0x2C
#define REG_FIRMA2     0x2D
#define ARCHIVO_LOG    "/logs.txt"  // logs en LittleFS (~1-3 MB disponibles)

TwoWire          busRtc = TwoWire(1);
Adafruit_PN532   lectorNfc(21, 22);

const char* SSID_AP  = "NFC";
const char* CLAVE_AP = "12345678";
WebServer   servidor(80);
Preferences almacen;  // solo guarda usuarios (uid→nombre) y claves de estado

String nombrePendiente      = "";
bool   esperandoTarjeta     = false;
bool   modoEliminar         = false;
bool   resultadoEliminacion = false;
String ultimoUid            = "";
bool   tarjetaPresente      = false;

unsigned long ultimaLecturaNfc      = 0;
unsigned long ultimoChequeoLimpieza = 0;
const unsigned long INTERVALO_NFC      = 300;
const unsigned long INTERVALO_LIMPIEZA = 60000;
bool rtcDisponible = false;

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
  // El SD3078 tiene protección de escritura por hardware de dos pasos:
  // 1) activar WRTC1 en reg 0x10, luego 2) activar WRTC2+WRTC3 en reg 0x0F.
  // Se escribe 0x84 fijo (no read-modify-write) para evitar corrupción post-reset.
  rtcEscribir(0x10, 0x80); delay(5);
  rtcEscribir(0x0F, 0x84); delay(5);
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
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", anio, mes, dia, hora, minuto, seg);
  return String(buffer);
}
void parsearCompilacion(int &anio, int &mes, int &dia, int &hora, int &minuto, int &segundo) {
  const char *fecha = __DATE__, *tiempo = __TIME__;
  dia  = atoi(fecha + 4);
  anio = atoi(fecha + 7);
  // __DATE__ tiene el mes como texto ("Jan", "Feb"...). Se busca esas 3 letras
  // dentro del string de referencia; la posición dividida por 3 da el índice
  // base-0 del mes, y sumando 1 se obtiene el número de mes (1–12).
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

  // La "firma" son 2 bytes derivados de la fecha/hora de compilación.
  // Se guardan en la RAM interna del RTC (no se borran al apagar).
  // Si no coinciden con los guardados → es una compilación nueva → actualizar hora.
  int firma1 = dia ^ mes ^ (anio & 0xFF);
  int firma2 = hora ^ minuto ^ segundo;

  if (rtcLeer(REG_FIRMA1) != firma1 || rtcLeer(REG_FIRMA2) != firma2) {
    rtcAjustarHora(anio, mes, dia, hora, minuto, segundo);
    delay(10);  // dar tiempo al RTC para estabilizarse antes de escribir la firma
    rtcHabilitarEscritura();
    rtcEscribir(REG_FIRMA1, firma1);
    rtcEscribir(REG_FIRMA2, firma2);
    Serial.println("RTC: hora actualizada.");
  } else {
    Serial.println("RTC: misma compilacion, hora intacta.");
  }
  Serial.println("RTC: " + obtenerTimestamp());
}

// ── HTML ─────────────────────────────────────────────────────
const char ESTILOS[] =
  "<style>body{font-family:Arial;margin:24px;max-width:720px}"
  "a,button,input{font-size:18px}button{padding:10px 14px;cursor:pointer}"
  "input{padding:10px;width:100%;box-sizing:border-box}"
  ".card{border:1px solid #ddd;border-radius:10px;padding:16px;margin:12px 0}"
  ".muted{color:#666;font-size:14px}.ts{color:#444;font-size:13px;font-family:monospace}"
  ".btn-danger{background:#dc3545;color:white;font-weight:bold}</style>";

String pagina(const String& titulo, const String& cuerpo) {
  return "<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>" + titulo + "</title>" + ESTILOS + "</head><body>" + cuerpo + "</body></html>";
}

// ── Logs (LittleFS) ──────────────────────────────────────────
void logAgregar(const String& ts, const String& uid, const String& nombre) {
  // "a" = append: agrega al final del archivo sin borrar lo anterior
  File f = LittleFS.open(ARCHIVO_LOG, "a");
  if (f) { f.println(ts + "|" + uid + "|" + nombre); f.close(); }
}

// Separa una entrada "timestamp|uid|nombre" en sus tres partes.
// Retorna false si el formato no es válido (protección ante entradas corruptas).
bool logParsear(const String& entrada, String &ts, String &uid, String &nombre) {
  int pos1 = entrada.indexOf('|'), pos2 = entrada.indexOf('|', pos1 + 1);
  if (pos1 < 0 || pos2 < 0) return false;
  ts = entrada.substring(0, pos1); uid = entrada.substring(pos1+1, pos2); nombre = entrada.substring(pos2+1);
  return true;
}

String logHtml() {
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (!f || !f.size())
    return "<div class='card'><div class='muted'>No hay eventos registrados.</div></div>";

  // Primera pasada: contar líneas totales del archivo
  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.seek(0);  // volver al inicio del archivo para la segunda pasada

  // Segunda pasada: cargar en memoria solo las últimas 300 entradas para no
  // saturar la RAM del ESP32. new/delete[] reserva y libera el arreglo en heap.
  int mostrar = min(total, 300);
  int saltar  = total - mostrar;  // si hay más de 300, ignorar las más antiguas
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
  for (int i = n - 1; i >= 0; i--) {  // recorrido inverso: más reciente primero
    String ts, uid, nombre;
    if (!logParsear(buf[i], ts, uid, nombre)) continue;
    resultado += "<div><b>" + nombre + "</b> <span class='muted'>(UID: " + uid + ")</span><br>"
                 "<span class='ts'>&#128336; " + ts + "</span></div><hr>";
  }
  delete[] buf;  // liberar el arreglo de la memoria heap
  return resultado + "</div>";
}

String logTexto() {
  // Lee el archivo completo línea por línea sin cargarlo entero en RAM
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (!f || !f.size()) return "Sin registros.\n";
  String resultado = "=== REGISTRO DE ACCESOS ===\nGenerado: " +
                     (rtcDisponible ? obtenerTimestamp() : String("sin RTC")) + "\n\n";
  int n = 1;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    String ts, uid, nombre;
    if (!logParsear(l, ts, uid, nombre)) continue;
    char linea[8]; snprintf(linea, sizeof(linea), "%4d. ", n++);
    resultado += String(linea) + ts + "   " + nombre + "   (UID: " + uid + ")\n";
  }
  f.close();
  return resultado + "\nFin del registro.\n";
}

void autoLimpiarLogs() {
  if (!rtcDisponible) return;
  // Leer solo la fecha del RTC; los primeros 2 bytes (seg, min) se descartan
  busRtc.beginTransmission(RTC_DIRECCION); busRtc.write(0x00);
  busRtc.endTransmission(false); busRtc.requestFrom(RTC_DIRECCION, 7);
  busRtc.read(); busRtc.read();              // seg, min (no se necesitan)
  int hora = bcd2bin(busRtc.read() & 0x3F); // hora actual
  busRtc.read();                             // día de semana, se descarta
  int dia  = bcd2bin(busRtc.read() & 0x3F);
  int mes  = bcd2bin(busRtc.read() & 0x1F);
  int anio = 2000 + bcd2bin(busRtc.read());

  if (hora != 0) return;  // solo ejecutar durante la hora 00:xx (medianoche)

  // Calcular último día del mes con corrección para año bisiesto en febrero
  const int diasPorMes[] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
  bool esBisiesto = (anio % 4 == 0 && (anio % 100 != 0 || anio % 400 == 0));
  int ultimoDia   = diasPorMes[mes] + (mes == 2 && esBisiesto ? 1 : 0);
  if (dia != 15 && dia != ultimoDia) return;

  // Guardar fecha de última limpieza en NVS para no borrar dos veces el mismo día
  char hoy[11]; snprintf(hoy, sizeof(hoy), "%04d-%02d-%02d", anio, mes, dia);
  if (almacen.getString("lastClean", "") == String(hoy)) return;

  LittleFS.remove(ARCHIVO_LOG);  // borrar el archivo completo de logs
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
  servidor.send(200, "text/html", pagina("ESP32 NFC",
    "<h2>ESP32 NFC</h2><div class='card'>"
    "<p>Estado: <b>" + estado + "</b></p>"
    "<p class='ts' id='clk'>&#128336; " + ts + "</p>"
    // Script que actualiza el reloj en pantalla cada segundo consultando /time
    "<script>setInterval(async()=>{document.getElementById('clk').innerHTML="
    "'&#128336; '+await(await fetch('/time')).text();},1000);</script>"
    "<p class='muted'>IP: 192.168.4.1</p><br>"
    "<a href='/register'><button>Registrar usuario</button></a> "
    "<a href='/logs'><button>Ver logs</button></a></div>"));
}
void handleRegisterForm() {
  servidor.send(200, "text/html", pagina("Registrar",
    "<h2>Registrar usuario</h2><div class='card'>"
    "<form method='POST' action='/saveName'>"
    "<label>Nombre:</label><br><input name='name' placeholder='Ej: Juan Perez' required>"
    "<br><br><button type='submit'>Guardar</button></form>"
    "<p class='muted'>Despues de guardar, acerca la tarjeta al lector.</p></div>"
    "<div class='card' style='border-color:#dc3545'>"
    "<h3 style='color:#dc3545'>Borrar usuario</h3>"
    "<a href='/deleteUser'><button class='btn-danger'>&#128465; Borrar usuario con tarjeta</button></a></div>"
    "<a href='/'><button>Volver</button></a>"));
}
void handleSaveName() {
  if (!servidor.hasArg("name")) { servidor.send(400, "text/plain", "Falta 'name'"); return; }
  nombrePendiente = servidor.arg("name"); nombrePendiente.trim();
  if (!nombrePendiente.length()) { servidor.send(400, "text/plain", "Nombre vacio"); return; }
  esperandoTarjeta = true; modoEliminar = false;
  servidor.send(200, "text/html", pagina("Acerca la tarjeta",
    "<h2>Acerca la tarjeta</h2><div class='card'>"
    "<p>Nombre: <b>" + nombrePendiente + "</b></p>"
    "<p id='st'>Esperando...</p>"
    // Polling cada 700ms a /status; cuando recibe "OK|..." redirige a /done
    "<script>setInterval(async()=>{let t=await(await fetch('/status')).text();"
    "if(t.startsWith('OK|'))location.href='/done?d='+encodeURIComponent(t);"
    "else document.getElementById('st').innerText=t;},700);</script>"
    "<a href='/cancelar'><button>Cancelar</button></a></div>"));
}
void handleDeleteUser() {
  esperandoTarjeta = false; modoEliminar = true; nombrePendiente = "";
  servidor.send(200, "text/html", pagina("Borrar usuario",
    "<h2>Borrar usuario</h2><div class='card' style='border-color:#dc3545'>"
    "<p style='color:#dc3545;font-weight:bold'>&#9888; Acerca la tarjeta para BORRAR</p>"
    "<p id='st'>Esperando tarjeta...</p>"
    // Mismo patrón de polling; redirige al recibir DELETED| o NOT_FOUND|
    "<script>setInterval(async()=>{let t=await(await fetch('/status')).text();"
    "if(t.startsWith('DELETED|')||t.startsWith('NOT_FOUND|'))location.href='/deleted?d='+encodeURIComponent(t);"
    "else document.getElementById('st').innerText=t;},700);</script>"
    "<a href='/cancelar'><button>Cancelar</button></a></div>"));
}
void handleStatus() {
  // Buzón entre loop() y el navegador: loop deposita resultado, browser lo lee una sola vez
  // y luego se limpia de NVS para no devolverlo dos veces
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
    int pos1 = datos.indexOf('|'), pos2 = datos.indexOf('|', pos1+1), pos3 = datos.indexOf('|', pos2+1);
    cuerpo += "<p>UID: <b>" + datos.substring(pos1+1, pos2) + "</b></p>"
              "<p>Nombre: <b>" + datos.substring(pos2+1, pos3>0 ? pos3 : datos.length()) + "</b></p>";
    if (pos3 > 0) cuerpo += "<p class='ts'>&#128336; " + datos.substring(pos3+1) + "</p>";
  } else cuerpo += "<p class='muted'>Sin datos.</p>";
  cuerpo += "<a href='/'><button>Inicio</button></a> <a href='/logs'><button>Ver logs</button></a></div>";
  servidor.send(200, "text/html", pagina("Registrado", cuerpo));
}
void handleDeleted() {
  String datos  = servidor.hasArg("d") ? servidor.arg("d") : "";
  String cuerpo = "<h2>Usuario borrado</h2><div class='card'>";
  if (datos.startsWith("DELETED|")) {
    int pos1 = datos.indexOf('|'), pos2 = datos.indexOf('|', pos1+1);
    cuerpo += "<p style='color:#dc3545;font-weight:bold'>&#10003; Usuario eliminado</p>"
              "<p>UID: <b>" + datos.substring(pos1+1, pos2) + "</b></p>"
              "<p>Nombre: <b>" + datos.substring(pos2+1) + "</b></p>";
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
    "<div style='margin:20px 0'><a href='/'><button>Volver</button></a> "
    "<a href='/downloadLogs'><button style='background:#28a745;color:white;font-weight:bold'>&#128229; Descargar TXT</button></a> "
    "<a href='/clearLogs' onclick='return confirm(\"Seguro?\");'>"
    "<button class='btn-danger'>&#128465; BORRAR TODOS LOS LOGS</button></a></div>"));
}
void handleClearLogs() {
  // Contar entradas antes de borrar para mostrarlo en la confirmación
  int total = 0;
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (f) { while (f.available()) { if (f.read() == '\n') total++; } f.close(); }
  LittleFS.remove(ARCHIVO_LOG);  // borrar el archivo completo
  servidor.send(200, "text/html", pagina("Logs borrados",
    "<h2>Logs borrados</h2><div class='card'>"
    "<p>&#10003; Eliminados <b>" + String(total) + "</b> eventos.</p>"
    "<p class='muted'>Las asociaciones UID&rarr;Nombre NO fueron eliminadas.</p>"
    "<a href='/logs'><button>Ver logs</button></a> <a href='/'><button>Inicio</button></a></div>"));
}
void handleDownloadLogs() {
  String ts = rtcDisponible ? obtenerTimestamp().substring(0, 10) : "logs";
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"logs_" + ts + ".txt\"");
  servidor.send(200, "text/plain; charset=utf-8", logTexto());
}

void handleCancelar() {
  esperandoTarjeta = false;
  modoEliminar     = false;
  nombrePendiente  = "";
  servidor.send(200, "text/html", pagina("Cancelado",
    "<h2>Operacion cancelada</h2><div class='card'>"
    "<p class='muted'>No se realizó ningun cambio.</p>"
    "<a href='/'><button>Inicio</button></a> "
    "<a href='/register'><button>Registrar usuario</button></a>"
    "</div>"));
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
    // Sin tarjeta: si antes había una, registrar que fue retirada y resetear el anti-repetición
    if (tarjetaPresente) { tarjetaPresente = false; ultimoUid = ""; Serial.println("Tarjeta retirada"); }
    return false;
  }
  String hex = uidAHex(uid, longitud);
  if (tarjetaPresente && hex == ultimoUid) return false;  // anti-lectura repetida
  tarjetaPresente = true; ultimoUid = uidSalida = hex; return true;
}

// ── Setup & Loop ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(500);
  Wire.begin(21, 22);
  busRtc.begin(RTC_SDA, RTC_SCL); delay(50);
  busRtc.beginTransmission(RTC_DIRECCION);
  rtcDisponible = (busRtc.endTransmission() == 0);
  if (rtcDisponible) rtcSincronizarSiNecesario();
  else Serial.println("AVISO: RTC no encontrado. Usando millis().");

  almacen.begin("nfc", false);
  LittleFS.begin(true);  // montar LittleFS (true = formatear si detecta corrupción)

  lectorNfc.begin();
  uint32_t versionFirmware = lectorNfc.getFirmwareVersion();
  Serial.println(versionFirmware
    ? "PN532 OK. Fw v" + String((versionFirmware >> 16) & 0xFF)
    : "ERROR: PN532 no detectado!");
  lectorNfc.SAMConfig();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(SSID_AP, CLAVE_AP, 6);  // canal 6
  Serial.println("AP: " + String(SSID_AP) + " | IP: " + WiFi.softAPIP().toString());

  servidor.on("/time",         handleTime);
  servidor.on("/",             handleHome);
  servidor.on("/register",     handleRegisterForm);
  servidor.on("/saveName",     HTTP_POST, handleSaveName);
  servidor.on("/deleteUser",   handleDeleteUser);
  servidor.on("/deleted",      handleDeleted);
  servidor.on("/status",       handleStatus);
  servidor.on("/done",         handleDone);
  servidor.on("/logs",         handleLogs);
  servidor.on("/downloadLogs", handleDownloadLogs);
  servidor.on("/clearLogs",    handleClearLogs);
  servidor.on("/cancelar", handleCancelar);
  servidor.begin();
  Serial.println("Servidor web iniciado!");
}

void loop() {
  servidor.handleClient();
  unsigned long ahora = millis();
  if (ahora - ultimoChequeoLimpieza >= INTERVALO_LIMPIEZA) { ultimoChequeoLimpieza = ahora; autoLimpiarLogs(); }
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
    } else {
      almacen.remove(uid.c_str());
      almacen.putString("lastDel", "DELETED|" + uid + "|" + nombre);
      Serial.println("[" + ts + "] BORRADO: " + uid + " -> " + nombre);
    }
    resultadoEliminacion = true; modoEliminar = false; return;
  }

  if (esperandoTarjeta) {
    almacen.putString(uid.c_str(), nombrePendiente);
    almacen.putString("lastReg", "OK|" + uid + "|" + nombrePendiente + "|" + ts);
    Serial.println("[" + ts + "] REGISTRADO: " + uid + " -> " + nombrePendiente);
    esperandoTarjeta = false; nombrePendiente = ""; return;
  }

  String nombre = almacen.getString(uid.c_str(), "NO_REGISTRADO");
  logAgregar(ts, uid, nombre);
  Serial.println("[" + ts + "] LECTURA: " + uid + " -> " + nombre);
}