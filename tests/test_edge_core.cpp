// Testes da lógica de Edge Computing executados no PC (g++ -std=c++17).
#include <cstdio>
#include <cstring>

#include "../firmware/edge_core.h"

using namespace cardio;

static int failures = 0;
static int checks = 0;

#define CHECK(condition)                                                   \
  do {                                                                     \
    ++checks;                                                              \
    if (!(condition)) {                                                    \
      ++failures;                                                          \
      std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                      \
  } while (0)

static bool contains(const char* text, const char* fragment) { return std::strstr(text, fragment) != nullptr; }

static void testRingBufferKeepsOrder() {
  RingBuffer<int, 3> queue;
  CHECK(queue.empty());
  CHECK(queue.push(1));
  CHECK(queue.push(2));
  CHECK(queue.front() == 1);
  queue.pop();
  CHECK(queue.front() == 2);
  CHECK(queue.size() == 1);
}

static void testRingBufferDropsOldestWhenFull() {
  RingBuffer<int, 3> queue;
  for (int value = 1; value <= 3; ++value) CHECK(queue.push(value));
  CHECK(queue.full());
  CHECK(!queue.push(4));
  CHECK(!queue.push(5));
  CHECK(queue.size() == 3);
  CHECK(queue.dropped() == 2);
  CHECK(queue.at(0) == 3);
  CHECK(queue.at(2) == 5);
}

static void testQueueCapacityMatchesBusinessModel() {
  CHECK(QUEUE_CAPACITY == 4320);
  CHECK(OFFLINE_AUTONOMY_S == 6 * 3600);
  CHECK(sizeof(Sample) * QUEUE_CAPACITY == 69120);
}

static void testBpmFromRegularBeats() {
  BpmEstimator estimator;
  uint32_t now = 1000;
  for (int beat = 0; beat < 6; ++beat, now += 750) estimator.addBeat(now);
  CHECK(estimator.bpm(now - 750) == 80);
}

static void testBpmRejectsBouncesAndExpires() {
  BpmEstimator estimator;
  CHECK(estimator.bpm(0) == 0);
  CHECK(estimator.addBeat(1000));
  CHECK(!estimator.addBeat(1100));
  CHECK(estimator.addBeat(1500));
  CHECK(estimator.bpm(1500) == 120);
  CHECK(estimator.bpm(1500 + BpmEstimator::PULSE_TIMEOUT_MS + 1) == 0);
}

static void testBpmResetsAfterLongPause() {
  BpmEstimator estimator;
  estimator.addBeat(0);
  estimator.addBeat(400);
  estimator.addBeat(5000);
  CHECK(estimator.bpm(5000) == 0);
  estimator.addBeat(6000);
  CHECK(estimator.bpm(6000) == 60);
}

static void testDebouncerIgnoresGlitches() {
  Debouncer debouncer(20, false);
  CHECK(!debouncer.update(true, 0));
  CHECK(!debouncer.update(false, 5));
  CHECK(!debouncer.update(true, 10));
  CHECK(!debouncer.update(true, 25));
  CHECK(debouncer.update(true, 30));
  CHECK(debouncer.state());
}

static void testAlertsForNormalVitals() {
  Thresholds limits;
  CHECK(evaluateAlerts(36.5f, 50, true, 75, 1.0f, true, limits) == 0);
}

static void testAlertsForEachThreshold() {
  Thresholds limits;
  CHECK(evaluateAlerts(38.5f, 50, true, 75, 1.0f, true, limits) == ALERT_FEVER);
  CHECK(evaluateAlerts(34.0f, 50, true, 75, 1.0f, true, limits) == ALERT_HYPOTHERMIA);
  CHECK(evaluateAlerts(36.5f, 50, true, 130, 1.0f, true, limits) == ALERT_TACHYCARDIA);
  CHECK(evaluateAlerts(36.5f, 50, true, 40, 1.0f, true, limits) == ALERT_BRADYCARDIA);
  CHECK(evaluateAlerts(36.5f, 50, true, 0, 1.0f, true, limits) == ALERT_NO_PULSE);
  CHECK(evaluateAlerts(36.5f, 85, true, 75, 1.0f, true, limits) == ALERT_HUMIDITY);
  CHECK(evaluateAlerts(36.5f, 50, true, 75, 3.0f, true, limits) == ALERT_FALL);
}

static void testAlertsIgnoreFailedSensors() {
  Thresholds limits;
  CHECK(evaluateAlerts(99.0f, 0, false, 75, 9.0f, false, limits) == 0);
}

static void testThresholdsAreExclusive() {
  Thresholds limits;
  CHECK(evaluateAlerts(38.0f, 50, true, 120, 1.0f, true, limits) == 0);
  CHECK(evaluateAlerts(38.0f, 50, true, 50, 1.0f, true, limits) == 0);
}

static Sample makeSample() {
  Sample sample{};
  sample.seq = 42;
  sample.uptimeS = 100;
  sample.tempC10 = 385;
  sample.humidity = 55;
  sample.bpm = 130;
  sample.motionMg = 1020;
  sample.alerts = ALERT_FEVER | ALERT_TACHYCARDIA;
  sample.status = STATUS_DHT_OK | STATUS_MPU_OK;
  return sample;
}

static void testJsonForLiveSample() {
  Thresholds limits;
  char json[384];
  int length = formatSampleJson(json, sizeof(json), makeSample(), "PAC-001", 7, 103, 0, limits);
  CHECK(length > 0 && length < static_cast<int>(sizeof(json)));
  CHECK(contains(json, "\"device\":\"PAC-001\""));
  CHECK(contains(json, "\"seq\":42"));
  CHECK(contains(json, "\"age_s\":3"));
  CHECK(contains(json, "\"temp_c\":38.5"));
  CHECK(contains(json, "\"bpm\":130"));
  CHECK(contains(json, "\"activity\":\"repouso\""));
  CHECK(contains(json, "\"alerts\":[\"febre\",\"taquicardia\"]"));
  CHECK(contains(json, "\"buffered\":false"));
}

static void testJsonForBufferedAndFailedSensors() {
  Thresholds limits;
  Sample sample = makeSample();
  sample.status = STATUS_RESTORED;
  sample.alerts = 0;
  char json[384];
  formatSampleJson(json, sizeof(json), sample, "PAC-001", 7, 5000, 12, limits);
  CHECK(contains(json, "\"age_s\":-1"));
  CHECK(contains(json, "\"temp_c\":null"));
  CHECK(contains(json, "\"motion_g\":null"));
  CHECK(contains(json, "\"alerts\":[]"));
  CHECK(contains(json, "\"buffered\":true"));
  CHECK(contains(json, "\"queue\":12"));
}

static void testJsonWorstCaseFitsBuffer() {
  Thresholds limits;
  Sample sample = makeSample();
  sample.seq = 4294967295u;
  sample.uptimeS = 0;
  sample.alerts = 0x7F;
  char json[384];
  int length = formatSampleJson(json, sizeof(json), sample, "PAC-001", 65535, 4294967295u, 4320, limits);
  CHECK(length < static_cast<int>(sizeof(json)));
}

static void testCsvRoundTrip() {
  Sample original = makeSample();
  char line[96];
  formatSampleCsv(line, sizeof(line), original);
  Sample parsed{};
  CHECK(parseSampleCsv(line, parsed));
  CHECK(std::memcmp(&original, &parsed, sizeof(Sample)) == 0);
  CHECK(!parseSampleCsv("linha,invalida", parsed));
}

int main() {
  testRingBufferKeepsOrder();
  testRingBufferDropsOldestWhenFull();
  testQueueCapacityMatchesBusinessModel();
  testBpmFromRegularBeats();
  testBpmRejectsBouncesAndExpires();
  testBpmResetsAfterLongPause();
  testDebouncerIgnoresGlitches();
  testAlertsForNormalVitals();
  testAlertsForEachThreshold();
  testAlertsIgnoreFailedSensors();
  testThresholdsAreExclusive();
  testJsonForLiveSample();
  testJsonForBufferedAndFailedSensors();
  testJsonWorstCaseFitsBuffer();
  testCsvRoundTrip();

  std::printf("%d verificacoes, %d falhas\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
