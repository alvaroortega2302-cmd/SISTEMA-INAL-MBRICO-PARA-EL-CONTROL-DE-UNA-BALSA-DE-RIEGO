#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

// =====================================================
// CONFIGURACION USUARIO
// =====================================================
const char* DEVICE_ID = "riego_esp32_1";

const char* WIFI_SSID = "TU_SSID";
const char* WIFI_PASSWORD = "TU_PASSWORD";

const char* SERVER_EVENT_URL = "http://10.240.43.99:5000/riego/evento";  //CAMBIAR IP
const char* SERVER_CMD_URL   = "http://10.240.43.99:5000/riego/comando?device_id=riego_esp32_1"; //CAMBIAR IP

const unsigned long INTERVALO_ENVIO_ESTADO_MS  = 60000;
const unsigned long INTERVALO_POLL_COMANDOS_MS = 2000;

// =====================================================
// PINES
// =====================================================
const int PIN_FLUJO1 = 32;
const int PIN_FLUJO2 = 33;

const int PIN_MOTOR1 = 25;
const int PIN_MOTOR2 = 26;

const int LED_VERDE_B1 = 16;
const int LED_ROJO_B1  = 17;
const int LED_VERDE_B2 = 18;
const int LED_ROJO_B2  = 19;

// =====================================================
// AJUSTES DE MEDIDA
// =====================================================
unsigned long MIN_PULSO_US_1 = 15000;
unsigned long MIN_PULSO_US_2 = 15000;

float PULSOS_POR_LITRO_B1 = 1150.0;
float PULSOS_POR_LITRO_B2 = 1150.0;

// =====================================================
// SEGURIDAD
// =====================================================
float UMBRAL_CAUDAL_MIN_B1_LH = 7.0;
float UMBRAL_CAUDAL_MIN_B2_LH = 7.0;

unsigned long TIEMPO_GRACIA_ARRANQUE_MS = 8000;   // 8 s por bomba
unsigned long INTERVALO_PARPADEO_MS      = 250;

// Ventanas deslizantes: 3 ventanas de 2 s
const int NUM_VENTANAS = 3;
float ventana_caudal1[NUM_VENTANAS] = {0, 0, 0};
float ventana_caudal2[NUM_VENTANAS] = {0, 0, 0};
int   segundos_en_ventana = 0;
unsigned long pulsos1_ventana = 0;
unsigned long pulsos2_ventana = 0;
bool  ventanas_validas = false;

// =====================================================
// VARIABLES GLOBALES
// =====================================================
volatile unsigned long pulsos1_total   = 0;
volatile unsigned long pulsos2_total   = 0;
volatile unsigned long pulsos1_segundo = 0;
volatile unsigned long pulsos2_segundo = 0;
volatile unsigned long ultimoPulsoUs1  = 0;
volatile unsigned long ultimoPulsoUs2  = 0;

float litros_acumulados_B1 = 0.0;
float litros_acumulados_B2 = 0.0;

float ultimo_caudal1_lh = 0.0;  // instantáneo 1 s
float ultimo_caudal2_lh = 0.0;

bool motor1_on = false;
bool motor2_on = false;

bool emergencia_activa   = false;
bool sin_luz_motores     = false; // fallo general energia/caudalímetros
String motivo_emergencia = "";
int bomba_fallida        = 0;     // 0 => general, 1/2 => bomba concreta

unsigned long instante_arranque_m1 = 0;
unsigned long instante_arranque_m2 = 0;

bool estado_parpadeo      = false;
unsigned long ultimoParpadeo = 0;

unsigned long ultimoReporte       = 0;
unsigned long ultimoEnvioEstado   = 0;
unsigned long ultimoPollComandos  = 0;

// =====================================================
// WIFI
// =====================================================
void conectarWiFi() {
  Serial.print("Conectando a WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi conectada");
  Serial.print("IP ESP32 RIEGO: ");
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

// =====================================================
// HTTP
// =====================================================
void postJSON(const String& url, const String& json) {
  asegurarWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sin WiFi: no se envia POST");
    return;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(json);
  Serial.print("HTTP POST code: ");
  Serial.println(code);

  if (code > 0) {
    String respuesta = http.getString();
    Serial.println("Respuesta servidor:");
    Serial.println(respuesta);
  }

  http.end();
}

String getTextoBool(bool v) {
  return v ? "true" : "false";
}

void enviarEventoRiego(const String& tipo, const String& detalle) {
  String json = "{";
  json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  json += "\"tipo\":\"" + tipo + "\",";
  json += "\"detalle\":\"" + detalle + "\",";
  json += "\"motor1_on\":"      + getTextoBool(motor1_on) + ",";
  json += "\"motor2_on\":"      + getTextoBool(motor2_on) + ",";
  json += "\"emergencia\":"     + getTextoBool(emergencia_activa) + ",";
  json += "\"sin_luz_motores\":"+ getTextoBool(sin_luz_motores) + ",";
  json += "\"bomba_fallida\":"  + String(bomba_fallida) + ",";
  json += "\"caudal1_lh\":"     + String(ultimo_caudal1_lh, 2) + ",";
  json += "\"caudal2_lh\":"     + String(ultimo_caudal2_lh, 2) + ",";
  json += "\"litros1\":"        + String(litros_acumulados_B1, 3) + ",";
  json += "\"litros2\":"        + String(litros_acumulados_B2, 3) + ",";
  json += "\"litros_total\":"   + String(litros_acumulados_B1 + litros_acumulados_B2, 3);
  json += "}";

  postJSON(SERVER_EVENT_URL, json);
}

String getComandoPendiente() {
  asegurarWiFi();
  if (WiFi.status() != WL_CONNECTED) return "";

  WiFiClient client;
  HTTPClient http;
  http.begin(client, SERVER_CMD_URL);

  int code    = http.GET();
  String payload = "";

  if (code > 0) {
    payload = http.getString();
  }

  http.end();
  return payload;
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

// =====================================================
// ISR
// =====================================================
void IRAM_ATTR isrFlujo1() {
  unsigned long ahora = micros();
  if (ahora - ultimoPulsoUs1 >= MIN_PULSO_US_1) {
    pulsos1_total++;
    pulsos1_segundo++;
    ultimoPulsoUs1 = ahora;
  }
}

void IRAM_ATTR isrFlujo2() {
  unsigned long ahora = micros();
  if (ahora - ultimoPulsoUs2 >= MIN_PULSO_US_2) {
    pulsos2_total++;
    pulsos2_segundo++;
    ultimoPulsoUs2 = ahora;
  }
}

// =====================================================
// CONTROL MOTORES (RELÉS ACTIVOS EN HIGH)
// =====================================================
void apagarMotores() {
  digitalWrite(PIN_MOTOR1, LOW);
  digitalWrite(PIN_MOTOR2, LOW);
  motor1_on = false;
  motor2_on = false;
}

void encenderMotor1() {
  if (emergencia_activa) return;
  digitalWrite(PIN_MOTOR1, HIGH);
  motor1_on            = true;
  instante_arranque_m1 = millis();
}

void apagarMotor1() {
  digitalWrite(PIN_MOTOR1, LOW);
  motor1_on = false;
}

void encenderMotor2() {
  if (emergencia_activa) return;
  digitalWrite(PIN_MOTOR2, HIGH);
  motor2_on            = true;
  instante_arranque_m2 = millis();
}

void apagarMotor2() {
  digitalWrite(PIN_MOTOR2, LOW);
  motor2_on = false;
}

// =====================================================
// LEDS
// =====================================================
void actualizarLeds() {
  if (emergencia_activa) {
    if (millis() - ultimoParpadeo >= INTERVALO_PARPADEO_MS) {
      ultimoParpadeo = millis();
      estado_parpadeo = !estado_parpadeo;
    }

    digitalWrite(LED_VERDE_B1, LOW);
    digitalWrite(LED_VERDE_B2, LOW);

    if (bomba_fallida == 1) {
      digitalWrite(LED_ROJO_B1, estado_parpadeo ? HIGH : LOW);
      digitalWrite(LED_ROJO_B2, HIGH);
    } else if (bomba_fallida == 2) {
      digitalWrite(LED_ROJO_B1, HIGH);
      digitalWrite(LED_ROJO_B2, estado_parpadeo ? HIGH : LOW);
    } else {
      digitalWrite(LED_ROJO_B1, estado_parpadeo ? HIGH : LOW);
      digitalWrite(LED_ROJO_B2, estado_parpadeo ? HIGH : LOW);
    }
    return;
  }

  digitalWrite(LED_VERDE_B1, motor1_on ? HIGH : LOW);
  digitalWrite(LED_ROJO_B1, motor1_on ? LOW : HIGH);

  digitalWrite(LED_VERDE_B2, motor2_on ? HIGH : LOW);
  digitalWrite(LED_ROJO_B2, motor2_on ? LOW : HIGH);
}

// =====================================================
// SEGURIDAD
// =====================================================
void activarEmergenciaBomba(const String& motivo, int bombaQueFalla) {
  emergencia_activa = true;
  sin_luz_motores   = false;
  motivo_emergencia = motivo;
  bomba_fallida     = bombaQueFalla;

  apagarMotores();

  Serial.println("***** EMERGENCIA BOMBA *****");
  Serial.println(motivo_emergencia);

  enviarEventoRiego("emergencia", motivo_emergencia);
}

void activarEmergenciaFaltaLuz() {
  emergencia_activa = true;
  sin_luz_motores   = true;
  motivo_emergencia =
    "Posible falta de alimentacion en motores o fallo de caudalimetros";
  bomba_fallida = 0;

  apagarMotores();

  Serial.println("***** EMERGENCIA FALTA LUZ *****");
  Serial.println(motivo_emergencia);

  enviarEventoRiego("emergencia", motivo_emergencia);
}

// Revisión basada en las últimas 3 ventanas de 2 s
void revisarSeguridadVentanas() {
  if (!ventanas_validas || emergencia_activa) return;

  unsigned long ahora = millis();

  bool gracia1 = motor1_on && (ahora - instante_arranque_m1 >= TIEMPO_GRACIA_ARRANQUE_MS);
  bool gracia2 = motor2_on && (ahora - instante_arranque_m2 >= TIEMPO_GRACIA_ARRANQUE_MS);

  int   malas1 = 0, malas2 = 0;
  float suma1  = 0.0f, suma2 = 0.0f;

  for (int i = 0; i < NUM_VENTANAS; i++) {
    if (ventana_caudal1[i] < UMBRAL_CAUDAL_MIN_B1_LH) malas1++;
    if (ventana_caudal2[i] < UMBRAL_CAUDAL_MIN_B2_LH) malas2++;
    suma1 += ventana_caudal1[i];
    suma2 += ventana_caudal2[i];
  }

  bool ambasMotoresON     = motor1_on && motor2_on;
  bool ambasFullGracia    = gracia1 && gracia2;
  bool ambasMalas         = (malas1 == NUM_VENTANAS && malas2 == NUM_VENTANAS);
  bool ambasCero          = (suma1 <= 0.01f && suma2 <= 0.01f);

  // Falta de alimentación: 3 ventanas malas, caudal total ~0 en las dos
  if (ambasMotoresON && ambasFullGracia && ambasMalas && ambasCero) {
    activarEmergenciaFaltaLuz();
    return;
  }

  // Fallo de bomba 1: 3 ventanas malas, pero ha pasado algo de agua (suma > 0)
  if (gracia1 && malas1 == NUM_VENTANAS) {
    bool motor2_funcionando = motor2_on && (malas2 < NUM_VENTANAS);
    bool solo_motor1_on     = motor1_on && !motor2_on;
    if (motor2_funcionando || solo_motor1_on) {
      activarEmergenciaBomba("Fallo de caudal en bomba 1", 1);
      return;
    }
  }

  if (gracia2 && malas2 == NUM_VENTANAS) {
    bool motor1_funcionando = motor1_on && (malas1 < NUM_VENTANAS);
    bool solo_motor2_on     = motor2_on && !motor1_on;
    if (motor1_funcionando || solo_motor2_on) {
      activarEmergenciaBomba("Fallo de caudal en bomba 2", 2);
      return;
    }
  }
}
}

// =====================================================
// RESET
// =====================================================
void resetSistema() {
  noInterrupts();
  pulsos1_total   = 0;
  pulsos2_total   = 0;
  pulsos1_segundo = 0;
  pulsos2_segundo = 0;
  interrupts();

  litros_acumulados_B1 = 0.0;
  litros_acumulados_B2 = 0.0;
  ultimo_caudal1_lh    = 0.0;
  ultimo_caudal2_lh    = 0.0;

  // Ventanas
  for (int i = 0; i < NUM_VENTANAS; i++) {
    ventana_caudal1[i] = 0.0f;
    ventana_caudal2[i] = 0.0f;
  }
  segundos_en_ventana = 0;
  pulsos1_ventana     = 0;
  pulsos2_ventana     = 0;
  ventanas_validas    = false;

  emergencia_activa = false;
  sin_luz_motores   = false;
  motivo_emergencia = "";
  bomba_fallida     = 0;
  estado_parpadeo   = false;

  apagarMotores();
}

void resetSoloAlarmas() {
  // No tocamos litros acumulados ni contadores de pulsos
  emergencia_activa = false;
  sin_luz_motores   = false;
  motivo_emergencia = "";
  bomba_fallida     = 0;
  estado_parpadeo   = false;

  apagarMotores();
}

// =====================================================
// COMANDOS REMOTOS
// =====================================================
void ejecutarComandoRemoto(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  Serial.print("Comando remoto recibido: ");
  Serial.println(cmd);

  if (cmd == "M1ON") {
    encenderMotor1();
    enviarEventoRiego("motor", "motor_1_encendido");
  }
  else if (cmd == "M1OFF") {
    apagarMotor1();
    enviarEventoRiego("motor", "motor_1_apagado");
  }
  else if (cmd == "M2ON") {
    encenderMotor2();
    enviarEventoRiego("motor", "motor_2_encendido");
  }
  else if (cmd == "M2OFF") {
    apagarMotor2();
    enviarEventoRiego("motor", "motor_2_apagado");
  }
  else if (cmd == "ALLON") {
    encenderMotor1();
    encenderMotor2();
    enviarEventoRiego("motor", "motores_encendidos");
  }
  else if (cmd == "ALLOFF") {
    apagarMotores();
    enviarEventoRiego("motor", "motores_apagados");
  }
  else if (cmd == "STATUS") {
    enviarEventoRiego("status", "status_solicitado");
  }
  else if (cmd == "RESET_TOTAL") {
    resetSistema();
    enviarEventoRiego("sistema", "reset_total_remoto");
  }
  else if (cmd == "RESET_ALARMAS") {
    resetSoloAlarmas();
    enviarEventoRiego("sistema", "reset_alarmas_remoto");
  }
  else if (cmd == "LITROS") {
    enviarEventoRiego("litros", "litros_totales_solicitados");
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_FLUJO1, INPUT);
  pinMode(PIN_FLUJO2, INPUT);

  pinMode(PIN_MOTOR1, OUTPUT);
  pinMode(PIN_MOTOR2, OUTPUT);

  pinMode(LED_VERDE_B1, OUTPUT);
  pinMode(LED_ROJO_B1,  OUTPUT);
  pinMode(LED_VERDE_B2, OUTPUT);
  pinMode(LED_ROJO_B2,  OUTPUT);

  apagarMotores();
  actualizarLeds();

  attachInterrupt(digitalPinToInterrupt(PIN_FLUJO1), isrFlujo1, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_FLUJO2), isrFlujo2, FALLING);

  conectarWiFi();
  enviarEventoRiego("sistema", "esp32_riego_iniciado");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // Cálculo de caudal cada ~1 s
  if (millis() - ultimoReporte >= 1000) {
  ultimoReporte = millis();

  noInterrupts();
  unsigned long p1s = pulsos1_segundo;
  unsigned long p2s = pulsos2_segundo;
  pulsos1_segundo   = 0;
  pulsos2_segundo   = 0;
  interrupts();

  ultimo_caudal1_lh = (p1s * 3600.0) / PULSOS_POR_LITRO_B1;
  ultimo_caudal2_lh = (p2s * 3600.0) / PULSOS_POR_LITRO_B2;

  litros_acumulados_B1 += (float)p1s / PULSOS_POR_LITRO_B1;
  litros_acumulados_B2 += (float)p2s / PULSOS_POR_LITRO_B2;

  // DEBUG: caudales instantáneos y litros acumulados
  Serial.print("[RIEGO] Caudal1 = ");
  Serial.print(ultimo_caudal1_lh, 2);
  Serial.print(" L/h  Caudal2 = ");
  Serial.print(ultimo_caudal2_lh, 2);
  Serial.print(" L/h  Litros1 = ");
  Serial.print(litros_acumulados_B1, 3);
  Serial.print("  Litros2 = ");
  Serial.print(litros_acumulados_B2, 3);
  Serial.print("  Total = ");
  Serial.print(litros_acumulados_B1 + litros_acumulados_B2, 3);
  Serial.println(" L");

  // resto de lógica de ventanas y seguridad
  pulsos1_ventana += p1s;
  pulsos2_ventana += p2s;
  segundos_en_ventana++;
  


    if (segundos_en_ventana >= 2) {
      float litros1_win = pulsos1_ventana / PULSOS_POR_LITRO_B1;
      float litros2_win = pulsos2_ventana / PULSOS_POR_LITRO_B2;
      float caudal1_win_lh = litros1_win * 3600.0 / 2.0;
      float caudal2_win_lh = litros2_win * 3600.0 / 2.0;

      // Desplazamos buffer (ventana más reciente en índice 0)
      for (int i = NUM_VENTANAS - 1; i > 0; i--) {
        ventana_caudal1[i] = ventana_caudal1[i - 1];
        ventana_caudal2[i] = ventana_caudal2[i - 1];
      }
      ventana_caudal1[0] = caudal1_win_lh;
      ventana_caudal2[0] = caudal2_win_lh;

      ventanas_validas = true;
      segundos_en_ventana = 0;
      pulsos1_ventana = 0;
      pulsos2_ventana = 0;

      // Revisamos seguridad con las 3 ventanas de 2 s
      revisarSeguridadVentanas();
    }
  }

  // Estado periódico cada 60 s solo mientras haya algún motor ON
  if (millis() - ultimoEnvioEstado >= INTERVALO_ENVIO_ESTADO_MS) {
    ultimoEnvioEstado = millis();

    if (motor1_on || motor2_on) {
      enviarEventoRiego("estado", "estado_periodico");
    }
  }

  // Poll de comandos del servidor
  if (millis() - ultimoPollComandos >= INTERVALO_POLL_COMANDOS_MS) {
    ultimoPollComandos = millis();

    String payload = getComandoPendiente();
    if (payload.length() > 0) {
      String ok      = extraerValorJSON(payload, "ok");
      String comando = extraerValorJSON(payload, "command");

      ok.trim();
      comando.trim();

      if (ok == "true" && comando.length() > 0) {
        ejecutarComandoRemoto(comando);
      }
    }
  }

  actualizarLeds();
}