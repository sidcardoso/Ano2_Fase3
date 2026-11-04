import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import mqtt from 'mqtt';

const scenario = process.argv[2] || 'normal';
assert.ok(['normal', 'alerta', 'offline'].includes(scenario));
const client = await mqtt.connectAsync('mqtt://broker.hivemq.com:1883', {
  clientId: `cardioia-test-${randomUUID()}`,
  connectTimeout: 15000,
  reconnectPeriod: 0,
});

try {
  const topic = `fiap/cardioia/PAC-001/validation/${randomUUID()}`;
  await client.subscribeAsync(topic, { qos: 1 });
  const expected = JSON.stringify({ source: 'teste-integracao', scenario });
  const received = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Broker nao devolveu a mensagem em 15 s')), 15000);
    client.on('message', (messageTopic, payload) => {
      if (messageTopic !== topic) return;
      clearTimeout(timer);
      resolve(payload.toString());
    });
  });
  await client.publishAsync(topic, expected, { qos: 1, retain: false });
  assert.equal(await received, expected);

  const prefix = 'fiap/cardioia/PAC-001';
  if (scenario === 'offline') {
    await client.publishAsync(`${prefix}/status`, JSON.stringify({
      device: 'PAC-001', state: 'offline', queue: 12, dropped: 0, source: 'teste-integracao',
    }), { qos: 1, retain: false });
  } else {
    for (let sequence = 1; sequence <= 8; sequence += 1) {
      const alerted = scenario === 'alerta';
      await client.publishAsync(`${prefix}/vitals`, JSON.stringify({
        device: 'PAC-001', boot: 0, seq: sequence, up: sequence * 5,
        age_s: alerted ? 60 : 0,
        temp_c: alerted ? 38.5 : 36.5,
        hum_pct: 50, bpm: alerted ? 130 : 75 + sequence % 3,
        motion_g: 1, activity: 'repouso',
        alerts: alerted ? ['febre', 'taquicardia'] : [],
        buffered: alerted, queue: alerted ? 8 - sequence : 0,
        source: 'teste-integracao',
      }), { qos: 1, retain: false });
    }
  }
  console.log(`MQTT VALIDADO: ida e volta pelo broker; cenario ${scenario} publicado sem retain.`);
  console.log('Origem: gerador Node.js de teste, nao firmware ESP32.');
} finally {
  await client.endAsync();
}