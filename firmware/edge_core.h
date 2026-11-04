// CardioIA - Fase 3: lógica de Edge Computing independente de hardware.
// Este arquivo não depende do Arduino: o mesmo código roda no ESP32 e nos testes do PC.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace cardio {

// ---------------------------------------------------------------------------
// Parâmetros do modelo de negócio (monitoramento domiciliar)
// ---------------------------------------------------------------------------
constexpr uint32_t SAMPLE_INTERVAL_MS = 5000;  // uma leitura consolidada a cada 5 s
// 6 h de autonomia offline: 6 h * 3600 s / 5 s = 4320 leituras (~69 KB de RAM).
constexpr size_t QUEUE_CAPACITY = 4320;
constexpr uint32_t OFFLINE_AUTONOMY_S = QUEUE_CAPACITY * (SAMPLE_INTERVAL_MS / 1000);

// ---------------------------------------------------------------------------
// Alertas clínicos avaliados localmente (funcionam mesmo sem internet)
// ---------------------------------------------------------------------------
enum AlertFlag : uint8_t {
  ALERT_FEVER = 1 << 0,        // temperatura acima do limite
  ALERT_HYPOTHERMIA = 1 << 1,  // temperatura abaixo do limite
  ALERT_TACHYCARDIA = 1 << 2,  // BPM acima do limite
  ALERT_BRADYCARDIA = 1 << 3,  // BPM abaixo do limite (com pulso presente)
  ALERT_NO_PULSE = 1 << 4,     // nenhum batimento registrado recentemente
  ALERT_HUMIDITY = 1 << 5,     // umidade ambiente fora da faixa de conforto
  ALERT_FALL = 1 << 6,         // pico de aceleração compatível com queda/impacto
};

// Alertas que exigem ação imediata (LED vermelho fixo + buzzer).
constexpr uint8_t CRITICAL_ALERTS =
    ALERT_FEVER | ALERT_HYPOTHERMIA | ALERT_TACHYCARDIA | ALERT_BRADYCARDIA | ALERT_FALL;

struct Thresholds {
  float feverC = 38.0f;
  float hypothermiaC = 35.0f;
  uint8_t bpmHigh = 120;
  uint8_t bpmLow = 50;
  uint8_t humidityLow = 30;
  uint8_t humidityHigh = 70;
  float fallG = 2.5f;     // módulo da aceleração (g) que caracteriza impacto
  float activeG = 0.15f;  // desvio de 1 g a partir do qual há movimento
};

inline uint8_t evaluateAlerts(float tempC, float humidity, bool dhtOk, uint8_t bpm, float peakMotionG,
                              bool mpuOk, const Thresholds& limits) {
  uint8_t flags = 0;
  if (dhtOk) {
    if (tempC > limits.feverC) flags |= ALERT_FEVER;
    if (tempC < limits.hypothermiaC) flags |= ALERT_HYPOTHERMIA;
    if (humidity < limits.humidityLow || humidity > limits.humidityHigh) flags |= ALERT_HUMIDITY;
  }
  if (bpm == 0) {
    flags |= ALERT_NO_PULSE;
  } else if (bpm > limits.bpmHigh) {
    flags |= ALERT_TACHYCARDIA;
  } else if (bpm < limits.bpmLow) {
    flags |= ALERT_BRADYCARDIA;
  }
  if (mpuOk && peakMotionG >= limits.fallG) flags |= ALERT_FALL;
  return flags;
}

inline const char* alertName(uint8_t flag) {
  switch (flag) {
    case ALERT_FEVER: return "febre";
    case ALERT_HYPOTHERMIA: return "hipotermia";
    case ALERT_TACHYCARDIA: return "taquicardia";
    case ALERT_BRADYCARDIA: return "bradicardia";
    case ALERT_NO_PULSE: return "sem_pulso";
    case ALERT_HUMIDITY: return "umidade";
    case ALERT_FALL: return "queda";
    default: return "desconhecido";
  }
}

inline const char* activityName(float motionG, const Thresholds& limits) {
  if (motionG >= limits.fallG) return "impacto";
  return fabsf(motionG - 1.0f) >= limits.activeG ? "atividade" : "repouso";
}

// ---------------------------------------------------------------------------
// Leitura compacta armazenada na fila (16 bytes)
// ---------------------------------------------------------------------------
enum SampleStatus : uint8_t {
  STATUS_DHT_OK = 1 << 0,
  STATUS_MPU_OK = 1 << 1,
  STATUS_RESTORED = 1 << 2,  // recuperada do SPIFFS após reinício: idade desconhecida
};

struct Sample {
  uint32_t seq;        // número sequencial (detecta perdas e duplicidades)
  uint32_t uptimeS;    // segundos desde o boot no momento da coleta
  int16_t tempC10;     // temperatura em décimos de °C
  uint8_t humidity;    // umidade relativa (%)
  uint8_t bpm;         // batimentos por minuto (0 = sem pulso)
  uint16_t motionMg;   // pico de aceleração no intervalo, em mili-g
  uint8_t alerts;      // AlertFlag combinados
  uint8_t status;      // SampleStatus combinados
};
static_assert(sizeof(Sample) == 16, "Sample deve ocupar 16 bytes para caber 4320 leituras na RAM");

// ---------------------------------------------------------------------------
// Fila circular: quando cheia, descarta a leitura mais antiga (FIFO)
// ---------------------------------------------------------------------------
template <typename T, size_t N>
class RingBuffer {
 public:
  // Retorna false quando a fila estava cheia e a leitura mais antiga foi descartada.
  bool push(const T& item) {
    bool kept = true;
    if (count_ == N) {
      head_ = (head_ + 1) % N;
      --count_;
      ++dropped_;
      kept = false;
    }
    items_[(head_ + count_) % N] = item;
    ++count_;
    return kept;
  }

  const T& front() const { return items_[head_]; }
  const T& at(size_t index) const { return items_[(head_ + index) % N]; }

  void pop() {
    if (count_ == 0) return;
    head_ = (head_ + 1) % N;
    --count_;
  }

  void clear() {
    head_ = 0;
    count_ = 0;
  }

  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == N; }
  uint32_t dropped() const { return dropped_; }
  static constexpr size_t capacity() { return N; }

 private:
  T items_[N];
  size_t head_ = 0;
  size_t count_ = 0;
  uint32_t dropped_ = 0;
};

// ---------------------------------------------------------------------------
// Debounce por tempo: o estado só muda após permanecer estável
// ---------------------------------------------------------------------------
class Debouncer {
 public:
  explicit Debouncer(uint32_t stableMs = 20, bool initial = false)
      : stableMs_(stableMs), stable_(initial), lastRaw_(initial) {}

  // Retorna true quando o estado estável muda.
  bool update(bool raw, uint32_t nowMs) {
    if (raw != lastRaw_) {
      lastRaw_ = raw;
      lastChangeMs_ = nowMs;
    }
    if (stable_ != lastRaw_ && nowMs - lastChangeMs_ >= stableMs_) {
      stable_ = lastRaw_;
      return true;
    }
    return false;
  }

  bool state() const { return stable_; }

 private:
  uint32_t stableMs_;
  bool stable_;
  bool lastRaw_;
  uint32_t lastChangeMs_ = 0;
};

// ---------------------------------------------------------------------------
// BPM: média móvel dos intervalos entre batimentos (cliques no botão)
// ---------------------------------------------------------------------------
class BpmEstimator {
 public:
  static constexpr uint32_t MIN_INTERVAL_MS = 250;     // teto de 240 bpm
  static constexpr uint32_t MAX_INTERVAL_MS = 3000;    // piso de 20 bpm
  static constexpr uint32_t PULSE_TIMEOUT_MS = 10000;  // sem batimento por 10 s = sem pulso
  static constexpr size_t WINDOW = 5;

  // Retorna false quando o batimento é rejeitado por estar rápido demais.
  bool addBeat(uint32_t nowMs) {
    if (hasLast_) {
      uint32_t interval = nowMs - lastBeatMs_;
      if (interval < MIN_INTERVAL_MS) return false;
      if (interval > MAX_INTERVAL_MS) {
        count_ = 0;  // pausa longa: recomeça a média para não misturar ritmos
        next_ = 0;
      } else {
        intervals_[next_] = interval;
        next_ = (next_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
      }
    }
    lastBeatMs_ = nowMs;
    hasLast_ = true;
    return true;
  }

  uint8_t bpm(uint32_t nowMs) const {
    if (!hasLast_ || count_ == 0 || nowMs - lastBeatMs_ > PULSE_TIMEOUT_MS) return 0;
    uint32_t sum = 0;
    for (size_t i = 0; i < count_; ++i) sum += intervals_[i];
    uint32_t value = (60000UL * count_ + sum / 2) / sum;
    return value > 255 ? 255 : static_cast<uint8_t>(value);
  }

 private:
  uint32_t intervals_[WINDOW] = {};
  size_t next_ = 0;
  size_t count_ = 0;
  uint32_t lastBeatMs_ = 0;
  bool hasLast_ = false;
};

// ---------------------------------------------------------------------------
// Serialização: JSON para a nuvem e CSV para o SPIFFS
// ---------------------------------------------------------------------------
inline void alertsToJsonArray(uint8_t flags, char* out, size_t size) {
  size_t used = static_cast<size_t>(snprintf(out, size, "["));
  bool first = true;
  for (uint8_t bit = 0; bit < 8 && used < size; ++bit) {
    uint8_t flag = static_cast<uint8_t>(1u << bit);
    if (!(flags & flag)) continue;
    used += static_cast<size_t>(snprintf(out + used, size - used, "%s\"%s\"", first ? "" : ",", alertName(flag)));
    first = false;
  }
  if (used < size) snprintf(out + used, size - used, "]");
}

inline long sampleAgeSeconds(const Sample& sample, uint32_t nowUptimeS) {
  if ((sample.status & STATUS_RESTORED) || sample.uptimeS > nowUptimeS) return -1;
  return static_cast<long>(nowUptimeS - sample.uptimeS);
}

// Retorna o tamanho do JSON gerado (snprintf) ou valor >= size se não couber.
inline int formatSampleJson(char* out, size_t size, const Sample& sample, const char* deviceId, uint16_t bootId,
                            uint32_t nowUptimeS, size_t queueSize, const Thresholds& limits) {
  char alerts[96];
  alertsToJsonArray(sample.alerts, alerts, sizeof(alerts));

  char temp[12];
  char humidity[8];
  if (sample.status & STATUS_DHT_OK) {
    snprintf(temp, sizeof(temp), "%.1f", sample.tempC10 / 10.0);
    snprintf(humidity, sizeof(humidity), "%u", static_cast<unsigned>(sample.humidity));
  } else {
    snprintf(temp, sizeof(temp), "null");
    snprintf(humidity, sizeof(humidity), "null");
  }

  float motionG = sample.motionMg / 1000.0f;
  char motion[12];
  char activity[16];
  if (sample.status & STATUS_MPU_OK) {
    snprintf(motion, sizeof(motion), "%.2f", motionG);
    snprintf(activity, sizeof(activity), "\"%s\"", activityName(motionG, limits));
  } else {
    snprintf(motion, sizeof(motion), "null");
    snprintf(activity, sizeof(activity), "null");
  }

  long age = sampleAgeSeconds(sample, nowUptimeS);
  bool buffered = age < 0 || age > static_cast<long>(SAMPLE_INTERVAL_MS / 1000) + 2;

  return snprintf(out, size,
                  "{\"device\":\"%s\",\"boot\":%u,\"seq\":%lu,\"up\":%lu,\"age_s\":%ld,\"temp_c\":%s,"
                  "\"hum_pct\":%s,\"bpm\":%u,\"motion_g\":%s,\"activity\":%s,\"alerts\":%s,\"flags\":%u,"
                  "\"buffered\":%s,\"queue\":%lu}",
                  deviceId, static_cast<unsigned>(bootId), static_cast<unsigned long>(sample.seq),
                  static_cast<unsigned long>(sample.uptimeS), age, temp, humidity,
                  static_cast<unsigned>(sample.bpm), motion, activity, alerts, static_cast<unsigned>(sample.alerts),
                  buffered ? "true" : "false", static_cast<unsigned long>(queueSize));
}

inline int formatSampleCsv(char* out, size_t size, const Sample& sample) {
  return snprintf(out, size, "%lu,%lu,%d,%u,%u,%u,%u,%u", static_cast<unsigned long>(sample.seq),
                  static_cast<unsigned long>(sample.uptimeS), static_cast<int>(sample.tempC10),
                  static_cast<unsigned>(sample.humidity), static_cast<unsigned>(sample.bpm),
                  static_cast<unsigned>(sample.motionMg), static_cast<unsigned>(sample.alerts),
                  static_cast<unsigned>(sample.status));
}

inline bool parseSampleCsv(const char* line, Sample& sample) {
  unsigned long seq, uptime;
  int temp;
  unsigned humidity, bpm, motion, alerts, status;
  if (sscanf(line, "%lu,%lu,%d,%u,%u,%u,%u,%u", &seq, &uptime, &temp, &humidity, &bpm, &motion, &alerts,
             &status) != 8) {
    return false;
  }
  sample.seq = static_cast<uint32_t>(seq);
  sample.uptimeS = static_cast<uint32_t>(uptime);
  sample.tempC10 = static_cast<int16_t>(temp);
  sample.humidity = static_cast<uint8_t>(humidity);
  sample.bpm = static_cast<uint8_t>(bpm);
  sample.motionMg = static_cast<uint16_t>(motion);
  sample.alerts = static_cast<uint8_t>(alerts);
  sample.status = static_cast<uint8_t>(status);
  return true;
}

}  // namespace cardio
