import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { marked } from 'marked';
import { PDFDocument } from 'pdf-lib';

const root = fileURLToPath(new URL('../', import.meta.url));
const source = await readFile(path.join(root, 'relatorio.md'), 'utf8');
const boundary = source.indexOf('## 4.');
assert.ok(boundary > 0, 'Relatorio deve conter a secao MQTT');
const browser = [process.env.CHROME_PATH,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  '/usr/bin/google-chrome', '/usr/bin/chromium',
].find(candidate => candidate && existsSync(candidate));
assert.ok(browser, 'Instale Chrome/Edge ou defina CHROME_PATH');
const output = path.join(root, 'docs');
await mkdir(output, { recursive: true });
const reports = [
  { name: 'relatorio-edge', title: 'Parte 1 - Edge e resiliencia offline', markdown: source.slice(0, boundary), minimum: 1 },
  { name: 'relatorio-mqtt', title: 'Parte 2 - MQTT e dashboard', markdown: source.slice(boundary), minimum: 2 },
];
const results = [];

for (const report of reports) {
  const body = marked.parse(report.markdown);
  const htmlPath = path.join(output, `${report.name}.html`);
  const pdfPath = path.join(output, `${report.name}.pdf`);
  await writeFile(htmlPath, `<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><title>${report.title}</title>
<style>
@page { size: A4; margin: 18mm; }
body { font: 11pt/1.45 Georgia, serif; color: #17212b; letter-spacing: 0; }
h1 { font-size: 19pt; } h2 { font-size: 14pt; break-after: avoid; }
p { orphans: 3; widows: 3; } table { width: 100%; border-collapse: collapse; }
th, td { padding: 5px; border: 1px solid #c5cbd0; font-size: 9pt; overflow-wrap: anywhere; }
code { font: 9pt Consolas, monospace; overflow-wrap: anywhere; }
a { color: #155e75; } img { max-width: 100%; }
${report.minimum === 2 ? 'h2:nth-of-type(3) { break-before: page; }' : ''}
</style></head><body><header><strong>CardioIA | ${report.title}</strong></header>${body}</body></html>`, 'utf8');
  const profile = await mkdtemp(path.join(tmpdir(), 'cardioia-pdf-'));
  try {
    const execution = spawnSync(browser, [
      '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
      '--no-pdf-header-footer', `--user-data-dir=${profile}`, `--print-to-pdf=${pdfPath}`,
      pathToFileURL(htmlPath).href,
    ], { timeout: 90000, encoding: 'utf8' });
    if (execution.error) throw execution.error;
    assert.equal(execution.status, 0, execution.stderr);
    const pdf = await PDFDocument.load(await readFile(pdfPath));
    const pages = pdf.getPageCount();
    assert.ok(pages >= report.minimum, `${report.name}: minimo ${report.minimum} paginas`);
    results.push({ file: `${report.name}.pdf`, pages, minimum: report.minimum });
    console.log(`${report.name}.pdf: ${pages} paginas (minimo ${report.minimum})`);
  } finally {
    await rm(profile, { recursive: true, force: true, maxRetries: 3, retryDelay: 200 });
  }
}
await writeFile(path.join(output, 'validacao-relatorios.json'), JSON.stringify(results, null, 2) + '\n');