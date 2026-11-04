import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { PDFDocument } from 'pdf-lib';

const root = new URL('../', import.meta.url);
const text = name => readFile(new URL(name, root), 'utf8');
const readme = await text('README.md');
assert.match(readme, /https:\/\/wokwi\.com\/projects\/\d+/);
assert.match(readme, /https:\/\/github\.com\/sidcardoso\/Ano2_Fase3/);
const diagram = JSON.parse(await text('firmware/diagram.json'));
const types = new Set(diagram.parts.map(part => part.type));
for (const type of ['board-esp32-devkit-c-v4', 'wokwi-dht22', 'wokwi-mpu6050', 'wokwi-pushbutton']) {
  assert.ok(types.has(type), `Componente ausente: ${type}`);
}
const flow = JSON.parse(await text('node-red/flow.json'));
const ids = new Set(flow.map(node => node.id));
assert.equal(ids.size, flow.length, 'IDs duplicados no fluxo');
for (const node of flow) {
  for (const reference of ['broker', 'group', 'tab', 'z']) {
    if (reference === 'broker' && node.type === 'mqtt-broker') continue;
    if (node[reference]) assert.ok(ids.has(node[reference]), `Referencia invalida ${node.id}.${reference}`);
  }
  for (const wire of (node.wires || []).flat()) assert.ok(ids.has(wire), `Wire invalido: ${wire}`);
}
for (const [file, minimum] of [['relatorio-edge.pdf', 1], ['relatorio-mqtt.pdf', 2]]) {
  const pdf = await PDFDocument.load(await readFile(new URL(`docs/${file}`, root)));
  assert.ok(pdf.getPageCount() >= minimum, `Paginas insuficientes: ${file}`);
  console.log(`${file}: ${pdf.getPageCount()} paginas`);
}
for (const name of ['dashboard-normal.png', 'dashboard-alerta.png', 'dashboard-offline.png', 'dashboard-esp32.png', 'dashboard-esp32-alerta.png', 'wokwi-online.png', 'wokwi-offline.png', 'wokwi-sincronizado.png', 'wokwi-alerta.png']) {
  const image = await readFile(new URL(`evidencias/${name}`, root));
  assert.equal(image.subarray(0, 8).toString('hex'), '89504e470d0a1a0a', `PNG invalido: ${name}`);
}
assert.match(await text('evidencias/testes-cpp.txt'), /64 verificacoes, 0 falhas/);
console.log('ENTREGA VALIDADA: links, sensores, fluxo, PDFs e evidencias presentes.');