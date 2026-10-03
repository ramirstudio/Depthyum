const test = require('node:test');
const assert = require('node:assert');
const lib = require('../extension/lib.js');

const info = {
  path: '/v/clip.mov', srcStart: 1.5, srcDur: 4, srcFps: 23.976, fileName: 'clip.mov',
  srcWidth: 1920, srcHeight: 1080, compName: 'Comp 1', layerName: 'Scena è 2'
};

test('buildArgs passa tempi e opzioni al motore', () => {
  const a = lib.buildArgs(info, { model: 'base', size: 756, range: 'shot', smooth: 0.4, invert: true }, '/out');
  assert.deepStrictEqual(a.slice(0, 3), ['-m', 'depthyum', 'run']);
  const get = (k) => a[a.indexOf(k) + 1];
  assert.strictEqual(get('--input'), '/v/clip.mov');
  assert.strictEqual(get('--start'), '1.5');
  assert.strictEqual(get('--duration'), '4');
  assert.strictEqual(get('--fps'), '23.976');
  assert.strictEqual(get('--model'), 'base');
  assert.strictEqual(get('--size'), '756');
  assert.strictEqual(get('--range'), 'shot');
  assert.strictEqual(get('--smooth'), '0.4');
  assert.ok(a.includes('--invert'));
});

test('buildArgs scarta valori fuori elenco e limita la fluidità', () => {
  const a = lib.buildArgs(info, { model: 'x; rm -rf', size: 9999, range: '??', smooth: 5 }, '/out');
  const get = (k) => a[a.indexOf(k) + 1];
  assert.strictEqual(get('--model'), 'small');
  assert.strictEqual(get('--size'), '518');
  assert.strictEqual(get('--range'), 'smooth');
  assert.strictEqual(get('--smooth'), '0.95');
  assert.ok(!a.includes('--invert'));
});

test('nome cartella senza caratteri problematici', () => {
  const n = lib.outputName(info, new Date(2026, 9, 3, 7, 5, 9));
  assert.strictEqual(n, 'Comp_1_Scena_2_20261003-070509');
});

test('venvPython per piattaforma', () => {
  assert.strictEqual(lib.venvPython('/r/engine', 'darwin'), '/r/engine/.venv/bin/python');
  assert.strictEqual(lib.venvPython('C:\\r\\engine', 'win32'), 'C:\\r\\engine\\.venv\\Scripts\\python.exe');
});

test('parseLine ignora righe che non sono eventi', () => {
  assert.strictEqual(lib.parseLine('Downloading model...'), null);
  assert.strictEqual(lib.parseLine('{rotto'), null);
  assert.deepStrictEqual(lib.parseLine('{"event":"done","count":3}\r'), { event: 'done', count: 3 });
});

test('describe', () => {
  assert.strictEqual(lib.describe(info), 'clip.mov · 23.976 fps · 4,0 s · 1920×1080');
});
