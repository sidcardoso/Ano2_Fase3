import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const flowPath = fileURLToPath(new URL('../node-red/flow.json', import.meta.url));
const flow = JSON.parse(await readFile(flowPath, 'utf8'));
const nodes = new Map(flow.map((node) => [node.id, node]));
const processor = new Function('msg', nodes.get('cardioia-normalize').func);

test('dashboard assina o broker e possui os widgets exigidos', () => {
  assert.equal(nodes.get('cardioia-broker').broker, 'broker.hivemq.com');
  assert.equal(nodes.get('cardioia-input').topic, 'fiap/cardioia/PAC-001/#');
  assert.deepEqual(
    flow.filter((node) => ['ui_chart', 'ui_gauge', 'ui_text'].includes(node.type)).map((node) => node.type),
    ['ui_chart', 'ui_gauge', 'ui_text', 'ui_text'],
  );
  assert.equal(nodes.get('cardioia-normalize').outputs, 4);
});

test('telemetria normal gera grafico, gauge e estado sem alerta', () => {
  const output = processor({
    topic: 'fiap/cardioia/PAC-001/vitals',
    payload: { bpm: 76, temp_c: 36.5, alerts: [], buffered: false, age_s: 5, queue: 0 },
  });

  assert.equal(output[0].payload, 76);
  assert.equal(output[0].topic, 'BPM');
  assert.equal(output[1].payload, 36.5);
  assert.equal(output[2].payload, 'NORMAL');
  assert.match(output[3].payload, /DADOS AO VIVO/);
});

test('alertas e leituras sincronizadas sao indicados no painel', () => {
  const output = processor({
    topic: 'fiap/cardioia/PAC-001/vitals',
    payload: { bpm: 130, temp_c: 38.5, alerts: ['febre', 'taquicardia'], buffered: true, age_s: -1, queue: 8 },
  });

  assert.equal(output[0].payload, 130);
  assert.equal(output[1].payload, 38.5);
  assert.equal(output[2].payload, 'ALERTA: febre, taquicardia');
  assert.match(output[3].payload, /DADOS SINCRONIZADOS/);
  assert.match(output[3].payload, /fila: 8/);
});

test('status do dispositivo mostra fila e leituras descartadas', () => {
  const output = processor({
    topic: 'fiap/cardioia/PAC-001/status',
    payload: { state: 'offline', queue: 12, dropped: 3 },
  });

  assert.equal(output[0], null);
  assert.match(output[3].payload, /OFFLINE/);
  assert.match(output[3].payload, /fila: 12/);
  assert.match(output[3].payload, /descartadas: 3/);
});
