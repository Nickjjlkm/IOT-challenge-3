/*
  =====================================================================
  HAPPY & MOIST — Challenge #3 IoT · ESP32     (version con modulo IA)
  Monitoreo hidrometeorologico con tablero web en la WLAN + MQTT + TinyML
  =====================================================================
  Que hace:
    - Mide nivel (JSN-SR04T), temperatura y humedad (DHT22),
      presion (BMP180) y luz/radiacion (BH1750).
    - Calcula evaporacion de agua libre con Penman (1948).
    - Alerta in situ: LCD 16x2 + buzzer.
    - [IA] Modelo TinyML (TensorFlow Lite Micro) que predice el nivel
      a TINYML_HORIZONTE_H horas y recomienda una accion a las
      autoridades: NORMAL / VIGILANCIA / PREVENTIVA / CRITICA.
    - Tablero web embebido en el ESP32: valor actual + historico + IA,
      con usuario/contrasena y lista blanca opcional de IPs.
    - Publica cada medicion por MQTT al broker (Raspberry Pi) y
      deja un Last Will para avisar si el ESP32 se desconecta.
    - Las autoridades pueden SILENCIAR la alarma fisica desde el tablero
      local (boton) o por MQTT (topico .../cmd, mensaje "silenciar").
      La alarma se rearma sola cuando la alerta desaparece.

  Concurrencia (FreeRTOS):
    - Tarea "medicion"  (nucleo 0): sensores, Penman, fusion, LCD, buzzer.
    - Tarea "mqtt"      (nucleo 1): conexion al broker y transmision.
    - loop() principal  (nucleo 1): servidor web + modulo de IA.
    Los datos compartidos se protegen con un mutex (mutexDatos).

  Topicos MQTT (ID = DEVICE_ID, por defecto "01"):
    happymoist/01/datos   JSON con las variables (+ ia_clase, ia_nivel)
    happymoist/01/estado  "online" al conectar / "offline" (Last Will)
    happymoist/01/cmd     comandos hacia el ESP32 ("silenciar")

  Librerias (Arduino IDE -> Administrar bibliotecas):
    - DHT sensor library (Adafruit) + Adafruit Unified Sensor
    - Adafruit BMP085 Library
    - BH1750 (Christopher Laws)
    - PubSubClient (Nick O'Leary)
    - [IA] Chirale_TensorFlowLite   (TensorFlow Lite Micro para ESP32)
  [IA] Archivo "modelo_tinyml.h" (lo genera entrenar_modelo.py) debe
       estar en la MISMA carpeta que este .ino.

  Placa: Herramientas -> Placa -> esp32 -> ESP32 Dev Module
  =====================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>
#include <BH1750.h>
#include <LiquidCrystal.h>
#include <math.h>

// [IA] TensorFlow Lite Micro
#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "modelo_tinyml.h"      // modelo + normalizacion (generado en Python)

// =====================================================================
// 1. CONFIGURACION — CAMBIA ESTO
// =====================================================================

// Red WiFi a la que se conecta el ESP32 (la misma del celular / PC)
const char* WIFI_SSID = "NOMBRE_DE_LA_RED";
const char* WIFI_PASS = "CLAVE_DE_LA_RED";

// Si no logra conectarse, crea su propia red con estos datos.
// Conectate a ella desde el celular y abre http://192.168.4.1
const char* AP_SSID = "HappyMoist";
const char* AP_PASS = "happymoist123";   // minimo 8 caracteres

// Usuario y contrasena del tablero
const char* WEB_USER = "admin";
const char* WEB_PASS = "moist2026";

// true  = valores simulados (para probar el tablero sin sensores)
// false = lectura real de los sensores
#define MODO_DEMO false

// [IA] true  = PRUEBAS: la tendencia se calcula sobre 60 s (la IA responde
//              en 1 minuto, pero las predicciones NO son confiables).
//      false = CAMPO: la tendencia se calcula sobre TINYML_VENTANA_H horas
//              (3 h). Requiere HISTORIAL_MS = 3600000 (1 registro por hora).
#define IA_PRUEBA true

// Lista blanca de IPs. Dejala en false hasta saber la IP de tu
// celular (aparece en el Monitor Serie cada vez que abres la pagina).
const bool WHITELIST_ACTIVA = false;
IPAddress WHITELIST[] = {
  IPAddress(192, 168, 1, 50),
  IPAddress(192, 168, 1, 51)
};
const int WHITELIST_N = sizeof(WHITELIST) / sizeof(WHITELIST[0]);

// Broker MQTT (Mosquitto en la Raspberry Pi). Usa la IP de la Pi
// (en la Pi: hostname -I) y reservala en el router para que no cambie.
const char* MQTT_HOST = "IP_DE_LA_RASPBERRY";
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "USUARIO_MQTT";
const char* MQTT_PASS = "CLAVE_MQTT";

// Identificador del dispositivo (debe ser distinto en cada ESP32)
#define DEVICE_ID "01"
const char* MQTT_CLIENT_ID    = "happymoist-" DEVICE_ID;
const char* MQTT_TOPIC_DATOS  = "happymoist/" DEVICE_ID "/datos";
const char* MQTT_TOPIC_ESTADO = "happymoist/" DEVICE_ID "/estado";
const char* MQTT_TOPIC_CMD    = "happymoist/" DEVICE_ID "/cmd";
const unsigned long MQTT_REINTENTO_MS = 5000;   // espera entre reconexiones

// =====================================================================
// 2. PINES (igual a la hoja de cableado ESP32)
// =====================================================================
#define PIN_TRIG     17
#define PIN_ECHO     18     // con divisor 1k/2k
#define PIN_DHT       4
#define PIN_BUZZER   25
#define PIN_SDA      21
#define PIN_SCL      22

#define LCD_RS 19
#define LCD_E  23
#define LCD_D4 32
#define LCD_D5 33
#define LCD_D6 26
#define LCD_D7 27

// =====================================================================
// 3. PARAMETROS DEL SISTEMA
// =====================================================================
const float ALTURA_TANQUE_CM   = 200.0;  // distancia del sensor al fondo
const float NIVEL_ALERTA_CM    = 40.0;   // nivel minimo antes de alertar
const float EVAP_ALERTA_MMDIA  = 4.0;    // evaporacion alta (mm/dia)
const float HUMEDAD_ALERTA     = 20.0;   // aire muy seco (%)
const float PRESION_ALERTA_HPA = 735.0;  // presion anomala para ~2600 m (normal ~745)
const float u2_fijo            = 2.0;    // viento asumido (m/s), FAO-56
const float ALBEDO_AGUA        = 0.08;

const unsigned long MEDICION_MS  = 5000;   // cada cuanto se mide
const unsigned long HISTORIAL_MS = 10000;  // cada cuanto se guarda en el historico
                                           // (en campo: 3600000 = 1 hora)
#define HISTORIAL_MAX 120                  // registros guardados (anillo)

// =====================================================================
// 4. ESTRUCTURAS COMPARTIDAS
// =====================================================================
struct Lectura {
  unsigned long t;      // segundos desde el arranque
  float nivel;          // cm
  float temp;           // C
  float hum;            // %
  float presion;        // hPa
  float radiacion;      // W/m2
  float evap;           // mm/dia
  bool  alerta;
  bool  aNivel, aEvap, aClima;
};

Lectura actual;
Lectura historial[HISTORIAL_MAX];
int histInicio = 0;
int histCuenta = 0;

SemaphoreHandle_t mutexDatos;

// Numero de medicion: la tarea de medicion lo incrementa; la tarea MQTT y el
// loop lo usan para saber si hay un dato nuevo que publicar / procesar con la IA.
uint32_t medicionSeq = 0;

// Silenciado de la alarma fisica. Lo activan las autoridades (tablero o MQTT)
// y la tarea de medicion lo vuelve a false cuando la alerta desaparece.
volatile bool alarmaSilenciada = false;

// =====================================================================
// 5. OBJETOS
// =====================================================================
DHT dht(PIN_DHT, DHT22);
Adafruit_BMP085 bmp;
BH1750 lightMeter;
LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool bmpOK = false;
bool bh1750OK = false;
bool modoAP = false;

// =====================================================================
// 5b. [IA] MODULO TinyML (se ejecuta en el loop principal, nucleo 1)
// =====================================================================
// Ventana sobre la que se calculan las tendencias (nivel y presion)
const unsigned long IA_VENTANA_S = IA_PRUEBA ? 60UL : (unsigned long)TINYML_VENTANA_H * 3600UL;

constexpr int IA_ARENA_BYTES = 16 * 1024;
static uint8_t ia_arena[IA_ARENA_BYTES] __attribute__((aligned(16)));

const tflite::Model*      iaModelo     = nullptr;
tflite::MicroInterpreter* iaInterprete = nullptr;
TfLiteTensor* iaEntrada  = nullptr;
TfLiteTensor* iaSalClase = nullptr;
TfLiteTensor* iaSalNivel = nullptr;
bool iaListo = false;

struct Prediccion {
  uint8_t estado;     // 0 = IA no disponible, 1 = reuniendo historico,
                      // 2 = faltan datos de sensores, 3 = OK
  int   clase;        // 0 NORMAL, 1 VIGILANCIA, 2 PREVENTIVA, 3 CRITICA
  float probs[4];
  float nivelF;       // nivel previsto a TINYML_HORIZONTE_H horas (cm)
  float tN;           // tendencia de nivel (cm/h)
  float tP;           // tendencia de presion (hPa/h)
};
Prediccion ia = {0, 0, {0, 0, 0, 0}, NAN, NAN, NAN};
uint32_t ultimoSeqIA = 0;

const char* IA_NOMBRES[4] = {"NORMAL", "VIGILANCIA", "PREVENTIVA", "CRITICA"};

void iniciarIA() {
  iaModelo = tflite::GetModel(modelo_tflite);
  if (iaModelo->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("[ia] version del modelo incompatible con la libreria");
    return;
  }

  // Solo las operaciones que usa nuestro modelo (ahorra memoria)
  static tflite::MicroMutableOpResolver<4> resolver;
  resolver.AddFullyConnected();
  resolver.AddRelu();
  resolver.AddSoftmax();
  resolver.AddReshape();

  static tflite::MicroInterpreter interprete(iaModelo, resolver, ia_arena, IA_ARENA_BYTES);
  iaInterprete = &interprete;

  if (iaInterprete->AllocateTensors() != kTfLiteOk) {
    Serial.println("[ia] AllocateTensors fallo (sube IA_ARENA_BYTES o falta una operacion)");
    return;
  }

  iaEntrada = iaInterprete->input(0);
  if (iaEntrada->type != kTfLiteFloat32 ||
      iaEntrada->bytes != TINYML_N_ENTRADAS * sizeof(float)) {
    Serial.println("[ia] la entrada del modelo no es la esperada");
    return;
  }

  // El orden de las salidas puede variar: se identifican por su tamano
  for (size_t i = 0; i < iaInterprete->outputs_size(); i++) {
    TfLiteTensor* o = iaInterprete->output(i);
    int n = o->dims->data[o->dims->size - 1];
    if (n == 4) iaSalClase = o;
    else if (n == 1) iaSalNivel = o;
  }
  if (!iaSalClase || !iaSalNivel) {
    Serial.println("[ia] no se encontraron las 2 salidas del modelo");
    return;
  }

  iaListo = true;
  Serial.printf("[ia] modelo cargado (%u bytes). Ventana de tendencia: %lu s\n",
                modelo_tflite_len, IA_VENTANA_S);
  if (!IA_PRUEBA && (unsigned long)HISTORIAL_MS * HISTORIAL_MAX < IA_VENTANA_S) {
    Serial.println("[ia] AVISO: HISTORIAL_MS es muy corto para la ventana; pon 3600000");
  }
}

// Cambia solo el estado de la prediccion (protegido con el mutex)
void ponerEstadoIA(uint8_t e) {
  xSemaphoreTake(mutexDatos, portMAX_DELAY);
  ia.estado = e;
  xSemaphoreGive(mutexDatos);
}

// Toma la ultima lectura + una lectura ~IA_VENTANA_S antes, arma las
// 8 entradas, corre el modelo y guarda el resultado en `ia`.
void ejecutarIA() {
  if (!iaListo) { ponerEstadoIA(0); return; }

  Lectura L{};            // se copia dentro del mutex (abajo)
  Lectura ref{};
  bool hayRef = false;

  xSemaphoreTake(mutexDatos, portMAX_DELAY);
  L = actual;
  for (int k = histCuenta - 1; k >= 0; k--) {          // del mas nuevo al mas viejo
    const Lectura &H = historial[(histInicio + k) % HISTORIAL_MAX];
    if (L.t >= H.t && (L.t - H.t) >= IA_VENTANA_S) { ref = H; hayRef = true; break; }
  }
  xSemaphoreGive(mutexDatos);

  if (isnan(L.nivel) || isnan(L.temp) || isnan(L.hum) ||
      isnan(L.presion) || isnan(L.radiacion) || isnan(L.evap)) {
    ponerEstadoIA(2); return;              // sensor caido: la IA no inventa datos
  }
  if (!hayRef) { ponerEstadoIA(1); return; }
  if (isnan(ref.nivel) || isnan(ref.presion)) { ponerEstadoIA(2); return; }

  float dtH = (L.t - ref.t) / 3600.0f;
  float tN = constrain((L.nivel   - ref.nivel)   / dtH, -TINYML_CLIP_DN, TINYML_CLIP_DN);
  float tP = constrain((L.presion - ref.presion) / dtH, -TINYML_CLIP_DP, TINYML_CLIP_DP);

  // MISMO orden que FEATURES en entrenar_modelo.py
  float f[TINYML_N_ENTRADAS] = {L.nivel, L.temp, L.hum, L.presion, L.radiacion, L.evap, tN, tP};
  for (int i = 0; i < TINYML_N_ENTRADAS; i++) {
    iaEntrada->data.f[i] = (f[i] - TINYML_MEDIA[i]) / TINYML_DESV[i];
  }

  if (iaInterprete->Invoke() != kTfLiteOk) {
    Serial.println("[ia] Invoke fallo");
    ponerEstadoIA(0); return;
  }

  Prediccion p;
  int mejor = 0;
  for (int i = 0; i < 4; i++) {
    p.probs[i] = iaSalClase->data.f[i];
    if (p.probs[i] > p.probs[mejor]) mejor = i;
  }
  p.clase  = mejor;
  p.nivelF = constrain(iaSalNivel->data.f[0] * TINYML_ALTURA, 0.0f, TINYML_ALTURA);
  p.tN = tN;
  p.tP = tP;
  p.estado = 3;

  // La tarea MQTT lee `ia` desde otro hilo: se escribe dentro del mutex
  xSemaphoreTake(mutexDatos, portMAX_DELAY);
  ia = p;
  xSemaphoreGive(mutexDatos);

  Serial.printf("[ia] %s (%.0f%%) | nivel en %dh: %.1f cm | dN=%.2f cm/h dP=%.2f hPa/h\n",
                IA_NOMBRES[p.clase], p.probs[p.clase] * 100.0f, TINYML_HORIZONTE_H,
                p.nivelF, tN, tP);
}

// Se llama en cada vuelta del loop: solo corre la IA si hay medicion nueva
void mantenerIA() {
  xSemaphoreTake(mutexDatos, portMAX_DELAY);
  uint32_t seq = medicionSeq;
  xSemaphoreGive(mutexDatos);
  if (seq == 0 || seq == ultimoSeqIA) return;
  ultimoSeqIA = seq;
  ejecutarIA();
}

// =====================================================================
// 6. LECTURA DE SENSORES
// =====================================================================
float leerNivel() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(20);           // el JSN-SR04T prefiere pulso >= 10 us
  digitalWrite(PIN_TRIG, LOW);

  long dur = pulseIn(PIN_ECHO, HIGH, 30000);
  if (dur == 0) return NAN;        // sin eco -> sin dato (no se asume vacio)

  float dist = dur * 0.0343 / 2.0;
  if (dist < 20.0 || dist > 450.0) return NAN;   // fuera del rango util

  float nivel = ALTURA_TANQUE_CM - dist;
  if (nivel < 0) nivel = 0;
  return nivel;
}

// Penman (1948), forma de flujo de masa. Devuelve mm/dia.
float calcularPenman(float T, float HR, float presion_hPa, float rad_Wm2, float u2) {
  if (isnan(T) || isnan(HR) || isnan(presion_hPa) || isnan(rad_Wm2)) return NAN;

  const float k = 0.41, z = 2.0, z0 = 0.0002, cp = 1005.0, Rd = 287.05, rhoW = 1000.0;
  float T_K = T + 273.15;
  float P   = presion_hPa * 100.0;

  float es  = 0.6108 * exp((17.27 * T) / (T + 237.3));     // kPa
  float ea  = es * (HR / 100.0);
  float de  = (es - ea) * 1000.0;                           // Pa
  float m   = 1000.0 * (4098.0 * es) / pow(T + 237.3, 2);   // Pa/K
  float lv  = (2.501 - 0.002361 * T) * 1.0e6;               // J/kg
  float gam = 0.665e-3 * P;                                 // Pa/K
  float rho = P / (Rd * T_K);                               // kg/m3
  float ga  = (k * k * u2) / pow(log(z / z0), 2);           // m/s
  float Rn  = rad_Wm2 * (1.0 - ALBEDO_AGUA);
  if (Rn < 0) Rn = 0;

  float E = (m * Rn + rho * cp * de * ga) / (lv * (m + gam)); // kg/(m2 s)
  if (E < 0) E = 0;
  return E / rhoW * 1000.0 * 86400.0;                        // mm/dia
}

// Valores simulados para probar el tablero sin hardware
void llenarDemo(Lectura &L) {
  float s = millis() / 1000.0;
  L.nivel     = 120.0 + 90.0 * sin(s / 60.0);          // sube y baja, cruza el umbral
  L.temp      = 14.0 + 3.0 * sin(s / 90.0);
  L.hum       = 75.0 - 10.0 * sin(s / 70.0);
  L.presion   = 744.0 + 2.0 * sin(s / 120.0);
  L.radiacion = 250.0 + 200.0 * sin(s / 45.0);
}

// =====================================================================
// 7. LCD Y BUZZER (solo los usa la tarea de medicion)
// =====================================================================
void imprimirValor(float v, int dec) {
  if (isnan(v)) lcd.print("--");
  else lcd.print(v, dec);
}

void mostrarLCD(const Lectura &L) {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (L.alerta) {
    lcd.print("ALERTA N:");
    imprimirValor(L.nivel, 0);
    lcd.print("cm");
  } else {
    lcd.print("N:");
    imprimirValor(L.nivel, 0);
    lcd.print("cm T:");
    imprimirValor(L.temp, 0);
    lcd.print("C");
  }
  lcd.setCursor(0, 1);
  lcd.print("H:");
  imprimirValor(L.hum, 0);
  lcd.print("% Ev:");
  imprimirValor(L.evap, 1);
}

void sonarAlerta() {
  for (int i = 0; i < 5; i++) {
    tone(PIN_BUZZER, 2000);
    vTaskDelay(pdMS_TO_TICKS(150));
    noTone(PIN_BUZZER);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =====================================================================
// 8. TAREA DE MEDICION (nucleo 0, separada del servidor web)
// =====================================================================
void tareaMedicion(void *param) {
  unsigned long ultimoHist = 0;

  for (;;) {
    Lectura L;
    L.t = millis() / 1000;

    unsigned long ahora = millis();

    if (MODO_DEMO) {
      llenarDemo(L);
    } else {
      L.nivel  = leerNivel();
      L.hum    = dht.readHumidity();
      L.temp   = dht.readTemperature();

      L.presion = bmpOK ? bmp.readPressure() / 100.0 : NAN;
      if (!isnan(L.presion) && (L.presion < 500.0 || L.presion > 1100.0)) L.presion = NAN;

      float lux = bh1750OK ? lightMeter.readLightLevel() : -1;
      L.radiacion = (lux >= 0) ? lux / 126.0 : NAN;
    }

    L.evap = calcularPenman(L.temp, L.hum, L.presion, L.radiacion, u2_fijo);

    // Fusion de senales. Un dato faltante (NAN) nunca dispara alerta:
    // asi un sensor caido no se confunde con una condicion real.
    L.aNivel = !isnan(L.nivel) && L.nivel <= NIVEL_ALERTA_CM;
    L.aEvap  = !isnan(L.evap)  && L.evap  >= EVAP_ALERTA_MMDIA;
    L.aClima = (!isnan(L.hum)     && L.hum     < HUMEDAD_ALERTA) ||
               (!isnan(L.presion) && L.presion < PRESION_ALERTA_HPA);
    L.alerta = L.aNivel && (L.aEvap || L.aClima);

    // Guardar en memoria compartida
    xSemaphoreTake(mutexDatos, portMAX_DELAY);
    actual = L;
    medicionSeq++;                 // avisa al loop que hay dato nuevo (IA y MQTT)
    if (ahora - ultimoHist >= HISTORIAL_MS || histCuenta == 0) {
      ultimoHist = ahora;
      int pos = (histInicio + histCuenta) % HISTORIAL_MAX;
      historial[pos] = L;
      if (histCuenta < HISTORIAL_MAX) histCuenta++;
      else histInicio = (histInicio + 1) % HISTORIAL_MAX;   // borra el mas viejo
    }
    xSemaphoreGive(mutexDatos);

    // Salidas in situ (la alarma NO depende de la IA: es determinista)
    mostrarLCD(L);
    Serial.printf("[med] N=%.1f T=%.1f H=%.1f P=%.1f R=%.1f E=%.2f ALERTA=%s\n",
                  L.nivel, L.temp, L.hum, L.presion, L.radiacion, L.evap,
                  L.alerta ? "SI" : "NO");

    if (!L.alerta) alarmaSilenciada = false;      // se rearma al terminar la alerta
    if (L.alerta && !alarmaSilenciada) sonarAlerta();
    else noTone(PIN_BUZZER);

    vTaskDelay(pdMS_TO_TICKS(MEDICION_MS));
  }
}

// =====================================================================
// 9. SERVIDOR WEB
// =====================================================================
const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Happy & Moist</title>
<style>
:root{--bg:#0B1E24;--panel:#12303A;--line:#24505C;--txt:#EAF4F4;--mut:#86A6AC;--cy:#4FC3E0;--am:#F2A65A;--rd:#E4572E;--gr:#8FD19E}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font-family:system-ui,sans-serif;padding:16px;max-width:900px;margin:auto}
h1{font-size:22px;margin-bottom:2px}.sub{color:var(--mut);font-size:13px;margin-bottom:14px}
h2{font-size:15px;margin-bottom:8px}
.estado{padding:12px 14px;border-radius:6px;font-weight:600;margin-bottom:14px;background:var(--panel);border-left:4px solid var(--gr)}
.estado.alerta{border-color:var(--rd);background:#3A1A16}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px;margin-bottom:16px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:12px}
.card .k{font-size:12px;color:var(--mut)}.card .v{font-size:24px;font-weight:600;margin-top:4px}
.card .u{font-size:13px;color:var(--mut);font-weight:400}
.card.on{border-color:var(--rd)}
.box{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:12px;margin-bottom:16px}
select{background:var(--bg);color:var(--txt);border:1px solid var(--line);padding:6px;border-radius:4px;margin-bottom:8px}
canvas{width:100%;height:220px;display:block}
table{width:100%;border-collapse:collapse;font-size:12.5px}
th,td{padding:6px;border-bottom:1px solid var(--line);text-align:right}th{color:var(--mut);font-weight:500}
td:first-child,th:first-child{text-align:left}
.scroll{max-height:320px;overflow:auto}
.sd{color:var(--mut)}
.iarec{padding:12px 14px;border-radius:6px;background:var(--bg);border-left:4px solid var(--gr);margin-bottom:12px;line-height:1.4;font-size:14px}
.iarec b{display:block;font-size:17px;margin-bottom:3px}
.pb{display:flex;align-items:center;gap:8px;font-size:12.5px;margin:4px 0}
.pb span{width:96px;color:var(--mut)}.pb em{width:44px;text-align:right;font-style:normal}
.pb div{flex:1;background:var(--bg);border-radius:4px;height:8px}.pb i{display:block;height:8px;border-radius:4px}
.nota{color:var(--mut);font-size:11.5px;margin-top:10px}
.btn{margin-top:10px;background:var(--rd);color:#fff;border:0;border-radius:4px;padding:8px 14px;font-weight:600;cursor:pointer}
</style></head><body>
<h1>Happy &amp; Moist</h1>
<div class="sub" id="sub">Conectando...</div>
<div class="estado" id="estado">Esperando datos...</div>
<div id="silencio" style="display:none;margin:-6px 0 14px"><button class="btn" onclick="silenciar()">Silenciar alarma física</button></div>
<div class="grid" id="grid"></div>
<div class="box" id="ia"><h2>Módulo de IA (TinyML)</h2><div class="sd">Esperando datos...</div></div>
<div class="box">
 <select id="var"></select>
 <canvas id="cv"></canvas>
</div>
<div class="box"><div class="scroll"><table id="tabla"></table></div></div>
<script>
const V=[["nivel","Nivel","cm",1],["temp","Temperatura","°C",1],
["hum","Humedad","%",1],["presion","Presión","hPa",1],["radiacion","Radiación","W/m²",1],["evap","Evaporación","mm/día",2]];
const IA_NOM=["NORMAL","VIGILANCIA","PREVENTIVA","CRÍTICA"];
const IA_COL=["var(--gr)","var(--cy)","var(--am)","var(--rd)"];
const IA_REC=[
"Operación normal. No se requieren acciones; continuar con el monitoreo.",
"Vigilar el sistema: el nivel previsto es bajo o hay condiciones climáticas desfavorables. Revisar el tablero con mayor frecuencia.",
"Acción preventiva: se prevé que el nivel cruce el umbral mínimo. Reducir el consumo y programar la recarga o avisar a las autoridades.",
"Acción inmediata: el nivel está bajo y se mantendrá así. Restringir el uso del agua, activar el plan de contingencia y notificar a las autoridades."];
const IA_MSG=["El módulo de IA no está disponible en este dispositivo.","Reuniendo histórico para calcular las tendencias...","Faltan datos de algún sensor; la IA no estima con datos incompletos."];
const sel=document.getElementById('var');
V.forEach(v=>{const o=document.createElement('option');o.value=v[0];o.textContent=v[1]+' ('+v[2]+')';sel.appendChild(o);});
sel.value='nivel';let hist=[];
const f=(x,d)=>x===null?'<span class="sd">sin dato</span>':x.toFixed(d);
function renderIA(a){
 const b=document.getElementById('ia');
 let h='<h2>Módulo de IA (TinyML)</h2>';
 if(!a||a.estado!==3){b.innerHTML=h+'<div class="sd">'+IA_MSG[a?a.estado:0]+'</div>';return;}
 h+='<div class="iarec" style="border-color:'+IA_COL[a.clase]+'"><b>'+IA_NOM[a.clase]+'</b>'+IA_REC[a.clase]+'</div>';
 h+='<div class="grid">'
  +'<div class="card"><div class="k">Nivel previsto en '+a.horizonte+' h</div><div class="v">'+a.nivelF.toFixed(1)+' <span class="u">cm</span></div></div>'
  +'<div class="card"><div class="k">Tendencia del nivel</div><div class="v">'+a.tN.toFixed(2)+' <span class="u">cm/h</span></div></div>'
  +'<div class="card"><div class="k">Tendencia de presión</div><div class="v">'+a.tP.toFixed(2)+' <span class="u">hPa/h</span></div></div></div>';
 h+=IA_NOM.map((n,i)=>'<div class="pb"><span>'+n+'</span><div><i style="width:'+(a.p[i]*100).toFixed(0)+'%;background:'+IA_COL[i]+'"></i></div><em>'+(a.p[i]*100).toFixed(0)+'%</em></div>').join('');
 h+='<div class="nota">Recomendación generada por un modelo entrenado con datos sintéticos; es una ayuda a la decisión y no reemplaza la alerta del sistema ni el criterio de las autoridades.</div>';
 b.innerHTML=h;
}
async function actual(){
 try{const r=await fetch('/api/actual');const d=await r.json();
 document.getElementById('sub').textContent='Modo '+d.modo+' · '+d.red+' · encendido hace '+d.t+' s';
 const e=document.getElementById('estado');
 e.className='estado'+(d.alerta?' alerta':'');
 e.textContent=d.alerta?'ALERTA CRÍTICA: nivel bajo + '+(d.aEvap?'evaporación alta':'condición climática anómala')+(d.silenciada?' (alarma física silenciada)':''):'Sin alerta';
 document.getElementById('silencio').style.display=(d.alerta&&!d.silenciada)?'block':'none';
 const on={nivel:d.aNivel,evap:d.aEvap,hum:d.aClima,presion:d.aClima};
 document.getElementById('grid').innerHTML=V.map(v=>'<div class="card'+(on[v[0]]?' on':'')+'"><div class="k">'+v[1]+'</div><div class="v">'+f(d[v[0]],v[3])+' <span class="u">'+v[2]+'</span></div></div>').join('');
 renderIA(d.ia);
 }catch(e){document.getElementById('sub').textContent='Sin conexión con el ESP32';}
}
async function silenciar(){
 try{await fetch('/api/silenciar',{method:'POST'});actual();}catch(e){}
}
async function historial(){
 try{const r=await fetch('/api/historial');hist=await r.json();dibujar();tabla();}catch(e){}
}
function dibujar(){
 const c=document.getElementById('cv'),x=c.getContext('2d'),dpr=window.devicePixelRatio||1;
 const W=c.clientWidth,H=c.clientHeight;c.width=W*dpr;c.height=H*dpr;x.scale(dpr,dpr);x.clearRect(0,0,W,H);
 const k=sel.value,pts=hist.map(h=>h[k]).filter(v=>v!==null);
 x.fillStyle='#86A6AC';x.font='12px system-ui';
 if(pts.length<2){x.fillText('Aún no hay suficientes datos',10,20);return;}
 let mn=Math.min(...pts),mx=Math.max(...pts);if(mn===mx){mn-=1;mx+=1;}
 const L=46,R=10,T=10,B=22,w=W-L-R,h=H-T-B;
 x.strokeStyle='#24505C';x.lineWidth=1;
 for(let i=0;i<=4;i++){const y=T+h*i/4;x.beginPath();x.moveTo(L,y);x.lineTo(W-R,y);x.stroke();
  x.fillText((mx-(mx-mn)*i/4).toFixed(1),4,y+4);}
 const thr={nivel:%NIVEL%,evap:%EVAP%,presion:%PRES%,hum:%HUM%}[k];
 if(thr!==undefined&&thr>=mn&&thr<=mx){const y=T+h*(1-(thr-mn)/(mx-mn));x.strokeStyle='#E4572E';x.setLineDash([5,4]);
  x.beginPath();x.moveTo(L,y);x.lineTo(W-R,y);x.stroke();x.setLineDash([]);x.fillStyle='#E4572E';x.fillText('umbral',W-R-44,y-4);}
 x.strokeStyle='#4FC3E0';x.lineWidth=2;x.beginPath();let first=true;
 hist.forEach((p,i)=>{const v=p[k];if(v===null){first=true;return;}
  const px=L+w*i/(hist.length-1),py=T+h*(1-(v-mn)/(mx-mn));
  if(first){x.moveTo(px,py);first=false;}else x.lineTo(px,py);});
 x.stroke();x.fillStyle='#86A6AC';
 x.fillText('t='+hist[0].t+' s',L,H-6);x.fillText('t='+hist[hist.length-1].t+' s',W-R-60,H-6);
}
function tabla(){
 let s='<tr><th>t (s)</th>'+V.map(v=>'<th>'+v[1]+'</th>').join('')+'<th>Alerta</th></tr>';
 hist.slice().reverse().forEach(h=>{s+='<tr><td>'+h.t+'</td>'+V.map(v=>'<td>'+f(h[v[0]],v[3])+'</td>').join('')+'<td>'+(h.alerta?'SÍ':'—')+'</td></tr>';});
 document.getElementById('tabla').innerHTML=s;
}
sel.onchange=dibujar;window.onresize=dibujar;
actual();historial();setInterval(actual,3000);setInterval(historial,10000);
</script></body></html>
)rawliteral";

String num(float v, int d) {
  if (isnan(v)) return "null";
  return String(v, d);
}

String lecturaJSON(const Lectura &L) {
  String s = "{";
  s += "\"t\":" + String(L.t);
  s += ",\"nivel\":" + num(L.nivel, 1);
  s += ",\"temp\":" + num(L.temp, 1);
  s += ",\"hum\":" + num(L.hum, 1);
  s += ",\"presion\":" + num(L.presion, 1);
  s += ",\"radiacion\":" + num(L.radiacion, 1);
  s += ",\"evap\":" + num(L.evap, 2);
  s += ",\"alerta\":" + String(L.alerta ? "true" : "false");
  s += ",\"aNivel\":" + String(L.aNivel ? "true" : "false");
  s += ",\"aEvap\":" + String(L.aEvap ? "true" : "false");
  s += ",\"aClima\":" + String(L.aClima ? "true" : "false");
  s += "}";
  return s;
}

// [IA] Fragmento JSON con la prediccion (se agrega a /api/actual)
String iaJSON() {
  String s = ",\"ia\":{\"estado\":" + String(ia.estado);
  s += ",\"horizonte\":" + String(TINYML_HORIZONTE_H);
  if (ia.estado == 3) {
    s += ",\"clase\":" + String(ia.clase);
    s += ",\"p\":[";
    for (int i = 0; i < 4; i++) { if (i) s += ","; s += num(ia.probs[i], 3); }
    s += "]";
    s += ",\"nivelF\":" + num(ia.nivelF, 1);
    s += ",\"tN\":" + num(ia.tN, 2);
    s += ",\"tP\":" + num(ia.tP, 2);
  }
  s += "}";
  return s;
}

// Devuelve true si la peticion puede pasar (lista blanca + login)
bool autorizado() {
  IPAddress ip = server.client().remoteIP();
  Serial.printf("[web] %s %s desde %s\n",
                server.method() == HTTP_GET ? "GET" : "POST",
                server.uri().c_str(), ip.toString().c_str());

  if (WHITELIST_ACTIVA) {
    bool ok = false;
    for (int i = 0; i < WHITELIST_N; i++) if (WHITELIST[i] == ip) { ok = true; break; }
    if (!ok) {
      server.send(403, "text/plain", "Acceso denegado: este dispositivo no esta autorizado.");
      return false;
    }
  }
  if (!server.authenticate(WEB_USER, WEB_PASS)) {
    server.requestAuthentication(BASIC_AUTH, "Happy & Moist", "Credenciales requeridas");
    return false;
  }
  return true;
}

void handleRaiz() {
  if (!autorizado()) return;
  String p = FPSTR(PAGINA);
  p.replace("%NIVEL%", String(NIVEL_ALERTA_CM, 1));
  p.replace("%EVAP%",  String(EVAP_ALERTA_MMDIA, 1));
  p.replace("%PRES%",  String(PRESION_ALERTA_HPA, 1));
  p.replace("%HUM%",   String(HUMEDAD_ALERTA, 1));
  server.send(200, "text/html; charset=utf-8", p);
}

void handleActual() {
  if (!autorizado()) return;
  xSemaphoreTake(mutexDatos, portMAX_DELAY);
  Lectura L = actual;
  xSemaphoreGive(mutexDatos);

  String s = lecturaJSON(L);
  s.remove(s.length() - 1);   // quita la } final para agregar campos
  s += ",\"modo\":\"" + String(MODO_DEMO ? "demo" : "sensores") + "\"";
  s += ",\"red\":\"" + String(modoAP ? "red propia (AP)" : WIFI_SSID) + "\"";
  s += ",\"silenciada\":" + String(alarmaSilenciada ? "true" : "false");
  s += iaJSON();              // [IA]
  s += "}";
  server.send(200, "application/json", s);
}

// POST /api/silenciar -> silencia el buzzer hasta que termine la alerta actual
void handleSilenciar() {
  if (!autorizado()) return;
  alarmaSilenciada = true;
  Serial.println("[alarma] silenciada desde el tablero local");
  server.send(200, "application/json", "{\"silenciada\":true}");
}

void handleHistorial() {
  if (!autorizado()) return;
  String s = "[";
  xSemaphoreTake(mutexDatos, portMAX_DELAY);
  for (int i = 0; i < histCuenta; i++) {
    if (i) s += ",";
    s += lecturaJSON(historial[(histInicio + i) % HISTORIAL_MAX]);
  }
  xSemaphoreGive(mutexDatos);
  s += "]";
  server.send(200, "application/json", s);
}

// =====================================================================
// 10. WIFI
// =====================================================================
void iniciarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Conectando a %s", WIFI_SSID);
  lcd.clear(); lcd.print("Conectando WiFi");

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  IPAddress ip;
  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP();
    Serial.printf("Conectado. Abre http://%s\n", ip.toString().c_str());
  } else {
    modoAP = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    ip = WiFi.softAPIP();
    Serial.printf("No se pudo conectar. Red propia \"%s\" (clave %s). Abre http://%s\n",
                  AP_SSID, AP_PASS, ip.toString().c_str());
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(modoAP ? "Red: HappyMoist" : "WiFi conectado");
  lcd.setCursor(0, 1); lcd.print(ip.toString());
  delay(4000);
}

// =====================================================================
// 11. MQTT (tarea propia "mqtt", nucleo 1)
// =====================================================================
unsigned long ultimoIntentoMqtt = 0;
uint32_t ultimoSeqPublicado = 0;

// Agrega "clave":valor al JSON solo si el dato es valido (un NAN se omite,
// asi un sensor caido no se confunde con un cero).
void agregarCampo(String &s, const char* clave, float v, int dec) {
  if (isnan(v)) return;
  s += "\"" + String(clave) + "\":" + String(v, dec) + ",";
}

// JSON para el gateway. "alerta" va como 0/1 (el formato que usa Ubidots).
String datosMQTT(const Lectura &L, const Prediccion &P) {
  String s = "{";
  agregarCampo(s, "nivel",     L.nivel,     1);
  agregarCampo(s, "temp",      L.temp,      1);
  agregarCampo(s, "hum",       L.hum,       1);
  agregarCampo(s, "presion",   L.presion,   1);
  agregarCampo(s, "radiacion", L.radiacion, 1);
  agregarCampo(s, "evap",      L.evap,      2);
  if (P.estado == 3) {                                    // [IA]
    s += "\"ia_clase\":" + String(P.clase) + ",";         // 0..3
    agregarCampo(s, "ia_nivel", P.nivelF, 1);             // cm previstos
  }
  s += "\"silenciada\":" + String(alarmaSilenciada ? 1 : 0) + ",";
  s += "\"alerta\":" + String(L.alerta ? 1 : 0);
  s += "}";
  return s;
}

void publicarDatos() {
  xSemaphoreTake(mutexDatos, portMAX_DELAY);
  uint32_t seq = medicionSeq;
  Lectura L = actual;
  Prediccion P = ia;
  xSemaphoreGive(mutexDatos);

  if (seq == 0 || seq == ultimoSeqPublicado) return;   // nada nuevo

  String json = datosMQTT(L, P);
  if (mqtt.publish(MQTT_TOPIC_DATOS, json.c_str())) {
    ultimoSeqPublicado = seq;
    Serial.printf("[mqtt] %s %s\n", MQTT_TOPIC_DATOS, json.c_str());
  } else {
    Serial.println("[mqtt] fallo al publicar");
  }
}

// Comandos recibidos por MQTT (por ejemplo, reenviados por el gateway desde Ubidots)
void alRecibirMQTT(char* topico, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("[mqtt] comando en %s: %s\n", topico, msg.c_str());
  if (msg.equalsIgnoreCase("silenciar") || msg == "1") {
    alarmaSilenciada = true;
    Serial.println("[alarma] silenciada por MQTT");
  }
}

void configurarMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(alRecibirMQTT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);       // el broker publica el Last Will ~1.5x este tiempo despues
  mqtt.setSocketTimeout(2);    // un broker caido no congela el servidor web
}

// La llama la tarea MQTT en cada vuelta: reconecta sin bloquear por mucho tiempo,
// atiende el protocolo y publica el dato nuevo.
void mantenerMQTT() {
  if (modoAP || WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    if (ultimoIntentoMqtt != 0 && millis() - ultimoIntentoMqtt < MQTT_REINTENTO_MS) return;
    ultimoIntentoMqtt = millis();

    Serial.printf("[mqtt] conectando a %s:%d ... ", MQTT_HOST, MQTT_PORT);
    // Last Will: si el ESP32 se cae, el broker publica "offline" (retenido, QoS 1)
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                     MQTT_TOPIC_ESTADO, 1, true, "offline")) {
      Serial.println("OK");
      mqtt.publish(MQTT_TOPIC_ESTADO, "online", true);   // retenido
      mqtt.subscribe(MQTT_TOPIC_CMD);                    // comandos (silenciar)
      ultimoSeqPublicado = 0;                            // reenvia la ultima lectura
    } else {
      Serial.printf("fallo (rc=%d)\n", mqtt.state());
    }
    return;
  }

  mqtt.loop();
  publicarDatos();
}

// Tarea FreeRTOS dedicada a la transmision (nucleo 1). El enunciado pide que
// la medicion y la transmision corran fuera del hilo principal.
void tareaMQTT(void *param) {
  for (;;) {
    mantenerMQTT();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// =====================================================================
// 12. SETUP Y LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  lcd.begin(16, 2);
  lcd.print("Happy & Moist");

  dht.begin();
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!MODO_DEMO) {
    bmpOK = bmp.begin();
    bh1750OK = lightMeter.begin();
    Serial.printf("BMP180: %s | BH1750: %s\n", bmpOK ? "OK" : "NO DETECTADO",
                  bh1750OK ? "OK" : "NO DETECTADO");
  }

  mutexDatos = xSemaphoreCreateMutex();
  actual.t = 0;
  actual.nivel = actual.temp = actual.hum = NAN;
  actual.presion = actual.radiacion = actual.evap = NAN;
  actual.alerta = actual.aNivel = actual.aEvap = actual.aClima = false;

  iniciarIA();                 // [IA] carga el modelo TinyML
  iniciarWiFi();
  configurarMQTT();

  server.on("/", handleRaiz);
  server.on("/api/actual", handleActual);
  server.on("/api/historial", handleHistorial);
  server.on("/api/silenciar", HTTP_POST, handleSilenciar);
  server.onNotFound([]() { server.send(404, "text/plain", "No encontrado"); });
  server.begin();

  // Medicion en el nucleo 0 y transmision MQTT en el nucleo 1,
  // ambas en tareas separadas del loop principal
  xTaskCreatePinnedToCore(tareaMedicion, "medicion", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(tareaMQTT,     "mqtt",     8192, NULL, 1, NULL, 1);
}

void loop() {
  // Hilo principal: servidor web + IA
  server.handleClient();
  mantenerIA();                // [IA] corre el modelo cuando hay medicion nueva
  delay(2);
}