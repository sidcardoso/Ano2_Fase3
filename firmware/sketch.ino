/*
 * CardioIA - Fase 3: Monitoramento Contínuo com IoT (ESP32 + Wokwi)
 *
 * Wearable simulado que:
 *   1. Lê sinais vitais: DHT22 (temperatura/umidade), botão de pulso (BPM) e MPU6050 (movimento/queda).
 *   2. Processa na borda (Edge): calcula BPM, avalia alertas e aciona LED/buzzer mesmo sem internet.
 *   3. Armazena cada leitura numa fila local de 4320 posições (6 h de autonomia offline).
 *   4. Quando o Wi-Fi simulado está "conectado", sincroniza a fila: imprime cada leitura no
 *      Serial (Serial.println) e publica no broker MQTT; só então remove a leitura da fila.
 *
 * Wi-Fi simulado: chave deslizante (direita = online) ou comandos "online"/"offline" no Monitor Serial.
 * SPIFFS: desligado por padrão porque é volátil no simulador; ative USE_SPIFFS em um ESP32 físico.
 */

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <DHTesp.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>

#include "edge_core.h"

#define USE_SPIFFS 0
#if USE_SPIFFS
#include <SPIFFS.h>
#endif

using namespace cardio;

// ---------------------------------------------------------------------------
// Pinagem (igual ao diagram.json)
// ---------------------------------------------------------------------------
constexpr uint8_t PIN_DHT = 15;
constexpr uint8_t PIN_PULSE_BUTTON = 4;
constexpr uint8_t PIN_WIFI_SWITCH = 27;
constexpr uint8_t PIN_LED_ONLINE = 26;
constexpr uint8_t PIN_LED_ALERT = 25;
constexpr uint8_t PIN_LED_PULSE = 33;
constexpr uint8_t PIN_BUZZER = 32;
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;

// ---------------------------------------------------------------------------
// Rede e MQTT (broker público: use apenas dados simulados e ID pseudônimo)
// ---------------------------------------------------------------------------
constexpr char WIFI_SSID[] = "Wokwi-GUEST";
constexpr char WIFI_PASSWORD[] = "";
constexpr uint8_t WIFI_CHANNEL = 6;  // canal fixo do Wokwi: evita a varredura e acelera a conexão
constexpr char MQTT_HOST[] = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char DEVICE_ID[] = "PAC-001";  // pseudônimo: nunca enviar nome ou documento do paciente
constexpr char TOPIC_VITALS[] = "fiap/cardioia/PAC-001/vitals";
constexpr char TOPIC_ALERTS[] = "fiap/cardioia/PAC-001/alerts";
constexpr char TOPIC_STATUS[] = "fiap/cardioia/PAC-001/status";

// ---------------------------------------------------------------------------
// Temporização (loop sem delay: cada tarefa roda no seu intervalo)
// ---------------------------------------------------------------------------
constexpr uint32_t MOTION_INTERVAL_MS = 100;    // MPU6050 a 10 Hz para capturar picos de impacto
constexpr uint32_t STATUS_INTERVAL_MS = 30000;  // heartbeat de status para o dashboard
constexpr uint32_t SYNC_INTERVAL_MS = 200;      // ritmo de envio dos lotes
constexpr size_t SYNC_BATCH = 10;               // leituras enviadas por ciclo de sincronização
constexpr uint32_t RECONNECT_MIN_MS = 1000;
constexpr uint32_t RECONNECT_MAX_MS = 30000;

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
DHTesp dht;
Adafruit_MPU6050 mpu;
WiFiClient netClient;
PubSubClient mqtt(netClient);

RingBuffer<Sample, QUEUE_CAPACITY> queue;  // fila offline em RAM (~69 KB)
BpmEstimator bpmEstimator;
Debouncer pulseButton(20, false);
Debouncer wifiSwitch(50, false);
Thresholds limits;

bool wifiConectado = false;      // conectividade simulada exigida pelo enunciado
bool serialOverride = false;     // comando no Serial tem prioridade sobre a chave até ela mudar
bool mpuOk = false;
uint16_t bootId = 0;             // diferencia reinícios (seq volta a 1 após reboot)
uint32_t nextSeq = 1;
uint32_t lastSampleMs = 0;
uint32_t lastMotionMs = 0;
uint32_t lastStatusMs = 0;
uint32_t lastSyncMs = 0;
uint32_t nextReconnectMs = 0;
uint32_t reconnectDelayMs = RECONNECT_MIN_MS;
uint32_t pulseLedOffMs = 0;
uint32_t autoPulseIntervalMs = 0;  // 0 = desligado; comando "pulso N" simula N bpm sem apertar o botão
uint32_t nextAutoPulseMs = 0;
bool buzzerOn = false;
float peakMotionG = 1.0f;        // maior módulo de aceleração desde a última leitura
uint8_t lastAlerts = 0;
uint32_t sentCount = 0;
bool syncing = false;
String serialLine;

uint32_t uptimeSeconds() { return millis() / 1000; }

// ---------------------------------------------------------------------------
// SPIFFS opcional (ESP32 físico): espelha a fila em CSV para sobreviver a reinícios
// ---------------------------------------------------------------------------
#if USE_SPIFFS
constexpr char QUEUE_FILE[] = "/fila.csv";
constexpr size_t SPIFFS_COMPACT_SLACK = 256;  // limita o arquivo a 4320 + 256 registros
size_t spiffsRecordCount = 0;

void spiffsRewrite();

void spiffsAppend(const Sample& sample) {
  File file = SPIFFS.open(QUEUE_FILE, FILE_APPEND);
  if (!file) return;
  char line[64];
  formatSampleCsv(line, sizeof(line), sample);
  size_t written = file.println(line);
  file.close();
  if (!written) return;
  ++spiffsRecordCount;
  if (spiffsRecordCount >= QUEUE_CAPACITY + SPIFFS_COMPACT_SLACK) spiffsRewrite();
}

// Compacta periodicamente; leituras ja sincronizadas podem ser reenviadas apos reinicio.
void spiffsRewrite() {
  if (queue.empty()) {
    SPIFFS.remove(QUEUE_FILE);  // "apaga o arquivo local" após sincronizar tudo
    spiffsRecordCount = 0;
    return;
  }
  File file = SPIFFS.open(QUEUE_FILE, FILE_WRITE);
  if (!file) return;
  char line[64];
  size_t written = 0;
  for (size_t i = 0; i < queue.size(); ++i) {
    formatSampleCsv(line, sizeof(line), queue.at(i));
    if (file.println(line)) ++written;
  }
  file.close();
  spiffsRecordCount = written;
}

void spiffsRestore() {
  File file = SPIFFS.open(QUEUE_FILE, FILE_READ);
  if (!file) return;
  size_t restored = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    Sample sample{};
    if (parseSampleCsv(line.c_str(), sample)) {
      sample.status |= STATUS_RESTORED;  // o relógio reiniciou: a idade da leitura é desconhecida
      queue.push(sample);
      ++restored;
      ++spiffsRecordCount;
    }
  }
  file.close();
  if (spiffsRecordCount >= QUEUE_CAPACITY + SPIFFS_COMPACT_SLACK) spiffsRewrite();
  Serial.printf("[SPIFFS] %u leituras restauradas da fila persistente\n", static_cast<unsigned>(restored));
}
#endif

// ---------------------------------------------------------------------------
// Saídas locais: LEDs e buzzer refletem o estado mesmo offline
// ---------------------------------------------------------------------------
void updateIndicators(uint32_t now) {
  digitalWrite(PIN_LED_ONLINE, wifiConectado && mqtt.connected());

  bool critical = lastAlerts & CRITICAL_ALERTS;
  bool warning = lastAlerts & ~CRITICAL_ALERTS;
  bool blink = (now / 500) % 2;
  digitalWrite(PIN_LED_ALERT, critical || (warning && blink));  // fixo = crítico, piscando = atenção

  // tone() só é chamado na troca de estado para não encher a fila interna do LEDC.
  bool wantBuzzer = critical && blink;
  if (wantBuzzer != buzzerOn) {
    buzzerOn = wantBuzzer;
    if (buzzerOn) {
      tone(PIN_BUZZER, 2000);
    } else {
      noTone(PIN_BUZZER);
    }
  }

  if (pulseLedOffMs && static_cast<int32_t>(now - pulseLedOffMs) >= 0) {
    digitalWrite(PIN_LED_PULSE, LOW);
    pulseLedOffMs = 0;
  }
}

// ---------------------------------------------------------------------------
// Conectividade simulada
// ---------------------------------------------------------------------------
void publishStatus(const char* state) {
  if (!mqtt.connected()) return;
  char payload[200];
  snprintf(payload, sizeof(payload),
           "{\"device\":\"%s\",\"state\":\"%s\",\"boot\":%u,\"up\":%lu,\"queue\":%u,\"dropped\":%lu,\"sent\":%lu}",
           DEVICE_ID, state, bootId, static_cast<unsigned long>(uptimeSeconds()), static_cast<unsigned>(queue.size()),
           static_cast<unsigned long>(queue.dropped()), static_cast<unsigned long>(sentCount));
  mqtt.publish(TOPIC_STATUS, payload, true);  // retained: o dashboard vê o último estado ao abrir
}

void setConnectivity(bool online, const char* origin) {
  if (online == wifiConectado) return;
  if (!online) publishStatus("offline");  // aviso gracioso antes de desconectar
  wifiConectado = online;
  Serial.printf("[WIFI] %s via %s | fila: %u leituras\n", online ? "CONECTADO" : "DESCONECTADO", origin,
                static_cast<unsigned>(queue.size()));
  if (online) {
    reconnectDelayMs = RECONNECT_MIN_MS;
    nextReconnectMs = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  } else {
    mqtt.disconnect();
    syncing = false;
  }
}

// Reconexão não bloqueante com backoff exponencial (1 s, 2 s, 4 s ... 30 s).
void maintainConnection(uint32_t now) {
  if (!wifiConectado) return;
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if (static_cast<int32_t>(now - nextReconnectMs) < 0) return;
  if (WiFi.status() != WL_CONNECTED) {
    nextReconnectMs = now + 500;  // aguarda o Wi-Fi associar sem bloquear o loop
    return;
  }

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "cardioia-%s-%04X", DEVICE_ID, bootId);
  char will[96];
  snprintf(will, sizeof(will), "{\"device\":\"%s\",\"state\":\"offline\",\"reason\":\"lwt\"}", DEVICE_ID);

  Serial.printf("[MQTT] conectando a %s:%u ... ", MQTT_HOST, MQTT_PORT);
  // Last Will: se o ESP32 cair sem aviso, o broker publica "offline" em nome dele.
  if (mqtt.connect(clientId, TOPIC_STATUS, 1, true, will)) {
    Serial.println("ok");
    reconnectDelayMs = RECONNECT_MIN_MS;
    publishStatus("online");
    syncing = !queue.empty();
  } else {
    Serial.printf("falhou (rc=%d), nova tentativa em %lu ms\n", mqtt.state(),
                  static_cast<unsigned long>(reconnectDelayMs));
    nextReconnectMs = now + reconnectDelayMs;
    reconnectDelayMs = reconnectDelayMs * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : reconnectDelayMs * 2;
  }
}

// ---------------------------------------------------------------------------
// Coleta de sinais vitais
// ---------------------------------------------------------------------------
void registerBeat(uint32_t now) {
  if (bpmEstimator.addBeat(now)) {
    digitalWrite(PIN_LED_PULSE, HIGH);
    pulseLedOffMs = now + 80;
  }
}

void handlePulseButton(uint32_t now) {
  // INPUT_PULLUP: pressionado = LOW. Cada pressão representa um batimento.
  if (pulseButton.update(digitalRead(PIN_PULSE_BUTTON) == LOW, now) && pulseButton.state()) {
    registerBeat(now);
  }
  if (autoPulseIntervalMs && static_cast<int32_t>(now - nextAutoPulseMs) >= 0) {
    nextAutoPulseMs = now + autoPulseIntervalMs;
    registerBeat(now);
  }
}

void handleWifiSwitch(uint32_t now) {
  bool switchOnline = digitalRead(PIN_WIFI_SWITCH) == LOW;
  if (wifiSwitch.update(switchOnline, now)) {
    serialOverride = false;  // mexer na chave devolve o controle a ela
    setConnectivity(wifiSwitch.state(), "chave");
  } else if (!serialOverride) {
    setConnectivity(wifiSwitch.state(), "chave");
  }
}

void sampleMotion() {
  if (!mpuOk) return;
  sensors_event_t accel, gyro, temp;
  mpu.getEvent(&accel, &gyro, &temp);
  float g = sqrtf(accel.acceleration.x * accel.acceleration.x + accel.acceleration.y * accel.acceleration.y +
                  accel.acceleration.z * accel.acceleration.z) /
            SENSORS_GRAVITY_STANDARD;
  if (fabsf(g - 1.0f) > fabsf(peakMotionG - 1.0f)) peakMotionG = g;  // guarda o maior desvio de 1 g
}

void announceAlertChanges(const Sample& sample) {
  uint8_t raised = sample.alerts & ~lastAlerts;
  uint8_t cleared = lastAlerts & ~sample.alerts;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    uint8_t flag = static_cast<uint8_t>(1u << bit);
    if (!((raised | cleared) & flag)) continue;
    bool active = raised & flag;
    Serial.printf("[ALERTA] %s %s (seq %lu)\n", alertName(flag), active ? "ATIVADO" : "normalizado",
                  static_cast<unsigned long>(sample.seq));
    if (mqtt.connected()) {
      char payload[160];
      snprintf(payload, sizeof(payload), "{\"device\":\"%s\",\"seq\":%lu,\"alert\":\"%s\",\"active\":%s,\"up\":%lu}",
               DEVICE_ID, static_cast<unsigned long>(sample.seq), alertName(flag), active ? "true" : "false",
               static_cast<unsigned long>(sample.uptimeS));
      mqtt.publish(TOPIC_ALERTS, payload);
    }
  }
  lastAlerts = sample.alerts;
}

void collectSample(uint32_t now) {
  TempAndHumidity reading = dht.getTempAndHumidity();
  bool dhtOk = dht.getStatus() == DHTesp::ERROR_NONE && !isnan(reading.temperature) && !isnan(reading.humidity);

  Sample sample{};
  sample.seq = nextSeq++;
  sample.uptimeS = uptimeSeconds();
  sample.bpm = bpmEstimator.bpm(now);
  sample.status = (dhtOk ? STATUS_DHT_OK : 0) | (mpuOk ? STATUS_MPU_OK : 0);
  if (dhtOk) {
    sample.tempC10 = static_cast<int16_t>(lroundf(reading.temperature * 10));
    sample.humidity = static_cast<uint8_t>(constrain(lroundf(reading.humidity), 0, 100));
  }
  sample.motionMg = static_cast<uint16_t>(constrain(lroundf(peakMotionG * 1000), 0, 65535));
  sample.alerts = evaluateAlerts(reading.temperature, reading.humidity, dhtOk, sample.bpm, peakMotionG, mpuOk, limits);
  peakMotionG = 1.0f;

  if (!queue.push(sample)) {
    Serial.printf("[FILA] cheia: leitura mais antiga descartada (total descartado: %lu)\n",
                  static_cast<unsigned long>(queue.dropped()));
  }
#if USE_SPIFFS
  spiffsAppend(sample);
#endif

  Serial.printf("[LEITURA] seq=%lu temp=%s%.1fC umid=%u%% bpm=%u mov=%.2fg alertas=0x%02X | %s | fila=%u/%u\n",
                static_cast<unsigned long>(sample.seq), dhtOk ? "" : "ERRO ", sample.tempC10 / 10.0,
                sample.humidity, sample.bpm, sample.motionMg / 1000.0, sample.alerts,
                wifiConectado && mqtt.connected() ? "online" : "OFFLINE", static_cast<unsigned>(queue.size()),
                static_cast<unsigned>(QUEUE_CAPACITY));

  announceAlertChanges(sample);
  if (wifiConectado && mqtt.connected()) syncing = true;
}

// ---------------------------------------------------------------------------
// Sincronização com a nuvem: Serial.println + MQTT, remoção só após sucesso
// ---------------------------------------------------------------------------
void syncQueue(uint32_t now) {
  if (!syncing || !wifiConectado || !mqtt.connected()) return;
  if (now - lastSyncMs < SYNC_INTERVAL_MS) return;
  lastSyncMs = now;

  size_t pending = queue.size();
  if (pending > SYNC_BATCH) {
    Serial.printf("[SYNC] enviando lote de %u (pendentes: %u)\n", static_cast<unsigned>(SYNC_BATCH),
                  static_cast<unsigned>(pending));
  }

  char json[384];
  for (size_t sent = 0; sent < SYNC_BATCH && !queue.empty(); ++sent) {
    const Sample& sample = queue.front();
    formatSampleJson(json, sizeof(json), sample, DEVICE_ID, bootId, uptimeSeconds(), queue.size() - 1, limits);
    Serial.print("[NUVEM] ");
    Serial.println(json);  // envio "para a nuvem" via Serial, conforme o enunciado
    if (!mqtt.publish(TOPIC_VITALS, json)) {
      Serial.println("[SYNC] falha ao publicar; leitura mantida na fila para nova tentativa");
      return;
    }
    queue.pop();  // remove somente após publicação bem-sucedida
    ++sentCount;
  }

  if (queue.empty()) {
    syncing = false;
#if USE_SPIFFS
    spiffsRewrite();
#endif
    if (pending > 1) {
      Serial.printf("[SYNC] concluída: %u leituras enviadas, fila local apagada\n", static_cast<unsigned>(pending));
      publishStatus("online");
    }
  }
}

// ---------------------------------------------------------------------------
// Comandos do Monitor Serial
// ---------------------------------------------------------------------------
void printStatus() {
  Serial.println("----------------------------------------------");
  Serial.printf("Dispositivo: %s | boot %u | uptime %lus\n", DEVICE_ID, bootId,
                static_cast<unsigned long>(uptimeSeconds()));
  Serial.printf("Wi-Fi simulado: %s | MQTT: %s\n", wifiConectado ? "conectado" : "desconectado",
                mqtt.connected() ? "conectado" : "desconectado");
  Serial.printf("Fila: %u/%u leituras (%.1f%%) | descartadas: %lu | enviadas: %lu\n",
                static_cast<unsigned>(queue.size()), static_cast<unsigned>(QUEUE_CAPACITY),
                100.0 * queue.size() / QUEUE_CAPACITY, static_cast<unsigned long>(queue.dropped()),
                static_cast<unsigned long>(sentCount));
  Serial.printf("Autonomia offline: %lu h | heap livre: %u bytes\n",
                static_cast<unsigned long>(OFFLINE_AUTONOMY_S / 3600), ESP.getFreeHeap());
  Serial.println("Comandos: online | offline | auto | status | limpar | pulso <bpm> (0 desliga)");
  Serial.println("----------------------------------------------");
}

void handleSerialCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (command == "online" || command == "offline") {
    serialOverride = true;
    setConnectivity(command == "online", "comando serial");
  } else if (command == "auto") {
    serialOverride = false;
    Serial.println("[CMD] conectividade volta a seguir a chave deslizante");
  } else if (command == "status") {
    printStatus();
  } else if (command == "limpar") {
    queue.clear();
#if USE_SPIFFS
    SPIFFS.remove(QUEUE_FILE);
    spiffsRecordCount = 0;
#endif
    Serial.println("[CMD] fila local apagada");
  } else if (command.startsWith("pulso")) {
    long bpm = command.substring(5).toInt();
    if (bpm <= 0) {
      autoPulseIntervalMs = 0;
      Serial.println("[CMD] pulso automático desligado; use o botão verde");
    } else {
      bpm = constrain(bpm, 20, 240);
      autoPulseIntervalMs = 60000UL / static_cast<uint32_t>(bpm);
      nextAutoPulseMs = millis();
      Serial.printf("[CMD] pulso automático: %ld bpm\n", bpm);
    }
  } else if (command.length()) {
    Serial.printf("[CMD] desconhecido: %s\n", command.c_str());
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      handleSerialCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 32) {
      serialLine += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Setup e loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_PULSE_BUTTON, INPUT_PULLUP);
  pinMode(PIN_WIFI_SWITCH, INPUT_PULLUP);
  pinMode(PIN_LED_ONLINE, OUTPUT);
  pinMode(PIN_LED_ALERT, OUTPUT);
  pinMode(PIN_LED_PULSE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  bootId = static_cast<uint16_t>(esp_random());
  dht.setup(PIN_DHT, DHTesp::DHT22);
  Wire.begin(PIN_SDA, PIN_SCL);
  mpuOk = mpu.begin();
  if (mpuOk) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);  // faixa ampla para registrar impactos de queda
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  WiFi.mode(WIFI_STA);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);

#if USE_SPIFFS
  if (SPIFFS.begin(true)) spiffsRestore();
#endif

  Serial.println();
  Serial.println("=== CardioIA - Monitor cardiaco vestivel (Fase 3) ===");
  Serial.printf("MPU6050: %s | fila offline: %u leituras (%lu h)\n", mpuOk ? "ok" : "nao encontrado",
                static_cast<unsigned>(QUEUE_CAPACITY), static_cast<unsigned long>(OFFLINE_AUTONOMY_S / 3600));
  Serial.println("Pressione o botao verde no ritmo do batimento. Chave a direita = Wi-Fi conectado.");

  bool startOnline = digitalRead(PIN_WIFI_SWITCH) == LOW;
  wifiSwitch = Debouncer(50, startOnline);
  setConnectivity(startOnline, "estado inicial da chave");
  printStatus();
}

void loop() {
  uint32_t now = millis();

  readSerial();
  handleWifiSwitch(now);
  handlePulseButton(now);

  if (now - lastMotionMs >= MOTION_INTERVAL_MS) {
    lastMotionMs = now;
    sampleMotion();
  }
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    collectSample(now);
  }
  if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = now;
    publishStatus(wifiConectado ? "online" : "offline");
  }

  maintainConnection(now);
  syncQueue(now);
  updateIndicators(now);
}
