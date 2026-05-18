#include <Arduino.h>

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>

// I2C: busRtc(SDA=19,SCL=18)→RTC SD3078 | Wire(SDA=21,SCL=22)→PN532+LCD(0x27)
#define RTC_SDA        19
#define RTC_SCL        18
#define RTC_DIRECCION  0x32
#define REG_FIRMA1     0x2C
#define REG_FIRMA2     0x2D
#define ARCHIVO_LOG    "/logs.txt"

TwoWire           busRtc = TwoWire(1);
Adafruit_PN532    lectorNfc(21, 22);
LiquidCrystal_I2C lcd(0x27, 20, 4);

const char* SSID_AP  = "NFC";
const char* CLAVE_AP = "12345678";
WebServer   servidor(80);
Preferences almacen;

String nombrePendiente      = "";
String codigoPendiente      = "";
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

  // Primera pasada: contar líneas totales del archivo
  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.seek(0);  // volver al inicio del archivo para la segunda pasada

  // Segunda pasada: cargar en memoria solo las últimas 300 entradas para no
  // saturar la RAM del ESP32. new/delete[] reserva y libera el arreglo en heap.
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
  for (int i = n - 1; i >= 0; i--) {  // recorrido inverso: más reciente primero
    String ts, uid, nombre, codigo;
    if (!logParsear(buf[i], ts, uid, nombre, codigo)) continue;
    resultado += "<div><b>" + nombre + "</b>";
    if (codigo.length()) resultado += " <span class='muted'>[" + codigo + "]</span>";
    resultado += " <span class='muted'>(UID: " + uid + ")</span><br>"
                 "<span class='ts'>&#128336; " + ts + "</span></div><hr>";
  }
  delete[] buf;  // liberar el arreglo de la memoria heap
  return resultado + "</div>";
}

String logTexto() {
  File f = LittleFS.open(ARCHIVO_LOG, "r");
  if (!f || !f.size()) return "Sin registros.\n";
  String resultado = "=== REGISTRO DE ACCESOS ===\nGenerado: " +
                     (rtcDisponible ? obtenerTimestamp() : String("sin RTC")) + "\n\n";
  int n = 1;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    String ts, uid, nombre, codigo;
    if (!logParsear(l, ts, uid, nombre, codigo)) continue;
    char linea[8]; snprintf(linea, sizeof(linea), "%4d. ", n++);
    resultado += String(linea) + ts + "   " + nombre;
    if (codigo.length()) resultado += " [" + codigo + "]";
    resultado += "   (UID: " + uid + ")\n";
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
  int hora = bcd2bin(busRtc.read() & 0x3F);
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
  lcdMostrar("Registrando:", nombrePendiente, "Acerca tarjeta", "");
  servidor.send(200, "text/html", pagina("Acerca la tarjeta",
    "<h2>Acerca la tarjeta</h2><div class='card'>"
    "<p>Nombre: <b>" + nombrePendiente + "</b></p>"
    "<p>Codigo: <b>" + codigoPendiente + "</b></p>"
    "<p id='st'>Esperando...</p>"
    // Polling cada 700ms a /status; cuando recibe "OK|..." redirige a /done
    "<script>setInterval(async()=>{let t=await(await fetch('/status')).text();"
    "if(t.startsWith('OK|'))location.href='/done?d='+encodeURIComponent(t);"
    "else document.getElementById('st').innerText=t;},700);</script>"
    "<a href='/cancelar'><button>Cancelar</button></a></div>"));
}
void handleDeleteUser() {
  esperandoTarjeta = false; modoEliminar = true; nombrePendiente = ""; codigoPendiente = "";
  lcdMostrar("Modo eliminar", "Acerca tarjeta", "", "");
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
    // Formato: OK|uid|nombre|codigo|timestamp
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
    // Formato: DELETED|uid|nombre|codigo
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
  LittleFS.remove(ARCHIVO_LOG);
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
  codigoPendiente  = "";
  lcdMostrar("Cancelado", "", "", "");
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

  Wire.begin(21, 22);  // debe inicializarse antes del LCD y el PN532

  lcd.init(); lcd.backlight();
  lcdMostrar("Sistema NFC", "Iniciando...", "", "");
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
  servidor.on("/cancelar",     handleCancelar);
  servidor.begin();
  Serial.println("Servidor web iniciado!");

  lcdMostrar("Sistema NFC", "Listo...", "", "");
}

void loop() {
  servidor.handleClient();
  unsigned long ahora = millis();

  // Cuando expira el timer del LCD, volver al mensaje correspondiente
  if (tiempoLcdHasta && ahora >= tiempoLcdHasta) {
    tiempoLcdHasta = 0;
    if (lcdPost == LCD_IDLE) lcdMostrar("Esperando", "tarjeta...", "", "");
    else                     lcdMostrar("Listo", "", "", "");
  }

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
      lcdMostrar("No registrado", "", "", "");
      tiempoLcdHasta = millis() + 3000;
      lcdPost = LCD_IDLE;
    } else {
      String claveCode = "k" + uid;
      String codigo = almacen.getString(claveCode.c_str(), "");
      almacen.remove(uid.c_str());
      almacen.remove(claveCode.c_str());
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
    almacen.putString(uid.c_str(), nombrePendiente);
    almacen.putString(claveCode.c_str(), codigoPendiente);
    almacen.putString("lastReg", "OK|" + uid + "|" + nombrePendiente + "|" + codigoPendiente + "|" + ts);
    Serial.println("[" + ts + "] REGISTRADO: " + uid + " -> " + nombrePendiente + " (" + codigoPendiente + ")");
    lcdMostrarNombre(nombrePendiente, codigoPendiente);
    tiempoLcdHasta = millis() + 4000;
    lcdPost = LCD_LISTO;
    esperandoTarjeta = false; codigoPendiente = ""; nombrePendiente = ""; return;
  }

  String nombre = almacen.getString(uid.c_str(), "NO_REGISTRADO");
  String claveCode = "k" + uid;
  String codigo = almacen.getString(claveCode.c_str(), "");
  logAgregar(ts, uid, nombre, codigo);
  Serial.println("[" + ts + "] LECTURA: " + uid + " -> " + nombre);
  lcdMostrarNombre(nombre, codigo);
  tiempoLcdHasta = millis() + 4000;
  lcdPost = LCD_IDLE;
}
