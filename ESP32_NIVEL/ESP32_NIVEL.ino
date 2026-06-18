#include <WiFi.h>
#include <HTTPClient.h>

// ------------------------
// CONFIGURACION
// ------------------------
const char* WIFI_SSID     = "WIFI_SSID";
const char* WIFI_PASSWORD = "WIFI_PASSWORD";

const char* SERVER_EVENT_URL = "http://10.240.43.99:5000/evento"; //CAMBIAR IP
const char* SERVER_CMD_URL   = "http://10.240.43.99:5000/nivel/comando"; //CAMBIAR IP

const char* SENSOR_ID = "deposito_1";

const int PIN_SENSOR   = 34;
const int UMBRAL_LLENO = 1400;

bool estadoLleno = false;

// ------------------------
// HEARTBEAT  <-- NUEVO
// ------------------------
unsigned long ultimoHeartbeat = 0;
const unsigned long HEARTBEAT_MS = 30000; // cada 30 s

// ------------------------
// WIFI
// ------------------------
void conectarWiFi() {
  Serial.print("Conectando a WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi conectada");
  Serial.print("IP ESP32 NIVEL: ");
  Serial.println(WiFi.localIP());
}

void asegurarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi perdida. Reintentando...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
}

// ------------------------
// HTTP
// ------------------------
void enviarEvento(const char* estadoTexto) {
  asegurarWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NIVEL] No hay WiFi. No se envia evento.");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  http.begin(client, SERVER_EVENT_URL);
  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"sensor\":\"" + String(SENSOR_ID) + "\",";
  json += "\"estado\":\"" + String(estadoTexto) + "\"";
  json += "}";

  Serial.print("[NIVEL] POST -> ");
  Serial.println(json);

  int code = http.POST(json);
  Serial.print("[NIVEL] HTTP code: ");
  Serial.println(code);

  if (code > 0) {
    String respuesta = http.getString();
    Serial.print("[NIVEL] Respuesta servidor: ");
    Serial.println(respuesta);
  }

  http.end();
}

String extraerValorJSON(String json, String clave) {
  String patron = "\"" + clave + "\":";
  int pos = json.indexOf(patron);
  if (pos == -1) return "";

  pos += patron.length();
  while (pos < (int)json.length() && json[pos] == ' ') pos++;

  if (pos < (int)json.length() && json[pos] == '"') {
    pos++;
    int fin = json.indexOf("\"", pos);
    if (fin == -1) return "";
    return json.substring(pos, fin);
  } else {
    int fin = json.indexOf(",", pos);
    if (fin == -1) fin = json.indexOf("}", pos);
    if (fin == -1) return "";
    return json.substring(pos, fin);
  }
}

void procesarComando(const String& cmd) {
  String c = cmd;
  c.trim();
  c.toUpperCase();

  Serial.print("[NIVEL] Comando remoto recibido: ");
  Serial.println(c);

  if (c == "RESET_TOTAL") {
    if (estadoLleno) {
      Serial.println("[NIVEL] RESET_TOTAL: forzamos estado a nolleno y enviamos evento");
      estadoLleno = false;
      enviarEvento("nolleno");
    } else {
      Serial.println("[NIVEL] RESET_TOTAL: ya estaba en nolleno, se ignora");
    }
  }
}

void comprobarComandoServidor() {
  static unsigned long ultimoPoll = 0;
  const unsigned long POLL_MS = 1000;

  if (millis() - ultimoPoll < POLL_MS) return;
  ultimoPoll = millis();

  asegurarWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NIVEL] No hay WiFi, no se consulta comando");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String(SERVER_CMD_URL) + "?device_id=" + SENSOR_ID;
  http.begin(client, url);

  Serial.print("[NIVEL] GET comando -> ");
  Serial.println(url);

  int code = http.GET();
  Serial.print("[NIVEL] HTTP code comando: ");
  Serial.println(code);

  if (code > 0) {
    String payload = http.getString();
    payload.trim();
    Serial.print("[NIVEL] Respuesta comando: '");
    Serial.print(payload);
    Serial.println("'");

    if (payload.length() > 0) {
      String ok      = extraerValorJSON(payload, "ok");
      String comando = extraerValorJSON(payload, "command");
      ok.trim();
      comando.trim();
      if (ok == "true" && comando.length() > 0) {
        procesarComando(comando);
      }
    }
  } else {
    Serial.println("[NIVEL] Error al hacer GET de comando");
  }

  http.end();
}

// ------------------------
// SENSOR
// ------------------------
bool leerSensorLleno() {
  int valor = analogRead(PIN_SENSOR);
  Serial.print("[NIVEL] Valor sensor: ");
  Serial.println(valor);
  return (valor >= UMBRAL_LLENO);
}

// ------------------------
// SETUP / LOOP
// ------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SENSOR, ADC_11db);

  conectarWiFi();
  Serial.println("[NIVEL] Sistema de nivel iniciado");

  bool llenoAhora = leerSensorLleno();
  estadoLleno = llenoAhora;

  Serial.print("[NIVEL] Estado inicial: ");
  Serial.println(estadoLleno ? "lleno" : "nolleno");

  // Heartbeat inicial al arrancar  <-- NUEVO
  enviarEvento("heartbeat");
  ultimoHeartbeat = millis();
}

void loop() {
  bool llenoAhora = leerSensorLleno();

  // Cambio de estado por sensor
  if (llenoAhora != estadoLleno) {
    estadoLleno = llenoAhora;
    if (estadoLleno) {
      Serial.println("[NIVEL] TRANSICION nolleno -> lleno, enviando evento");
      enviarEvento("lleno");
    } else {
      Serial.println("[NIVEL] TRANSICION lleno -> nolleno, enviando evento");
      enviarEvento("nolleno");
    }
  }

  // Heartbeat cada 30 s  <-- NUEVO
  if (millis() - ultimoHeartbeat >= HEARTBEAT_MS) {
    ultimoHeartbeat = millis();
    enviarEvento("heartbeat");
    Serial.println("[NIVEL] Heartbeat enviado");
  }

  // Comandos remotos (RESET_TOTAL)
  comprobarComandoServidor();

  delay(500);
}
