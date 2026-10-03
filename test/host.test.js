const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const vm = require('vm');

// Stub minimi degli oggetti ExtendScript: verificano la logica del file, non la semantica di After Effects.
function makeHost(app, extra) {
  class CompItem {}
  class AVLayer {}
  class FootageItem {}
  class FileSource {}
  class ImportOptions { constructor(f) { this.file = f; } }
  class File { constructor(p) { this.fsName = p; this.exists = !/missing/.test(p); } }
  const ctx = vm.createContext(Object.assign({ CompItem, AVLayer, FootageItem, FileSource, ImportOptions, File, app }, extra));
  vm.runInContext(fs.readFileSync(__dirname + '/../extension/host.jsx', 'utf8'), ctx);
  return { ctx, CompItem, AVLayer, FootageItem, FileSource };
}

function scene(over = {}) {
  const probe = makeHost({});
  const { CompItem, AVLayer, FootageItem, FileSource } = probe;
  const src = Object.assign(new FileSource(), { isStill: false, file: { exists: true, fsName: '/v/clip.mov', name: 'clip.mov' } });
  const item = Object.assign(new FootageItem(), { mainSource: src, hasVideo: true, duration: 10, frameRate: 24, width: 1920, height: 1080 });
  const layer = Object.assign(new AVLayer(), {
    source: item, name: 'clip', index: 1, inPoint: 3, outPoint: 7, startTime: 2, stretch: 100, timeRemapEnabled: false
  }, over.layer);
  const comp = Object.assign(new CompItem(), { id: 5, name: 'Main', frameRate: 24, selectedLayers: [layer], numLayers: 1 }, over.comp);
  const app = { project: { activeItem: comp, file: null } };
  const h = makeHost(app);
  // gli stub devono condividere le classi con il contesto che esegue host.jsx
  Object.setPrototypeOf(comp, h.CompItem.prototype);
  Object.setPrototypeOf(layer, h.AVLayer.prototype);
  Object.setPrototypeOf(item, h.FootageItem.prototype);
  Object.setPrototypeOf(src, h.FileSource.prototype);
  return { h, app, comp, layer, item, src };
}

const call = (h, expr) => JSON.parse(vm.runInContext(expr, h.ctx));

test('tempi sorgente con ingresso spostato', () => {
  const { h } = scene();
  const r = call(h, 'depthyum_getSelection()');
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.srcStart, 1);
  assert.strictEqual(r.srcDur, 4);
  assert.strictEqual(r.path, '/v/clip.mov');
  assert.strictEqual(r.compIn, 3);
});

test('stretch 200% dimezza i secondi sorgente', () => {
  const { h } = scene({ layer: { stretch: 200 } });
  const r = call(h, 'depthyum_getSelection()');
  assert.strictEqual(r.srcStart, 0.5);
  assert.strictEqual(r.srcDur, 2);
});

test('durata limitata al footage', () => {
  const { h } = scene({ layer: { inPoint: 2, outPoint: 30, startTime: 2 } });
  assert.strictEqual(call(h, 'depthyum_getSelection()').srcDur, 10);
});

for (const [nome, over, testo] of [
  ['time remap', { layer: { timeRemapEnabled: true } }, /time remap/i],
  ['stretch negativo', { layer: { stretch: -100 } }, /stretch/i],
  ['nessuna selezione', { comp: { selectedLayers: [] } }, /Seleziona un layer/],
  ['due layer', { comp: { selectedLayers: [{}, {}] } }, /un solo layer/],
]) {
  test('errore: ' + nome, () => {
    const { h } = scene(over);
    const r = call(h, 'depthyum_getSelection()');
    assert.strictEqual(r.ok, false);
    assert.match(r.error, testo);
  });
}

test('errore: immagine fissa e file mancante e sequenza', () => {
  let s = scene(); s.src.isStill = true;
  assert.match(call(s.h, 'depthyum_getSelection()').error, /immagine fissa/);
  s = scene(); s.src.file = { exists: false, fsName: '/x', name: 'x.mov' };
  assert.match(call(s.h, 'depthyum_getSelection()').error, /non trovato/);
  s = scene(); s.src.file = { exists: true, fsName: '/x/seq_0001.png', name: 'seq_0001.png' };
  assert.match(call(s.h, 'depthyum_getSelection()').error, /sequenze/i);
});

test('importDepth: timing, parent, scala e posizione', () => {
  const s = scene();
  const calls = [];
  const prop = (name, store) => ({ setValue: (v) => store.push([name, v]), value: [100, 50] });
  const set = [];
  const newLayer = {
    startTime: 0, stretch: 100, name: '', parent: null,
    get inPoint() { return this.startTime; },
    get outPoint() { return this.startTime + 6; },
    set outPoint(v) { calls.push(['outPoint', v]); },
    property() { return { property: (n) => prop(n, set) }; },
    moveBefore: (l) => calls.push(['moveBefore', l.name]),
  };
  const orig = {
    name: 'clip',
    property() { return { property: () => ({ value: [100, 50] }) }; },
  };
  s.comp.layer = () => orig;
  s.comp.layers = { add: () => newLayer };
  s.app.project.itemByID = () => s.comp;
  s.app.project.importFile = (io) => { calls.push(['import', io.sequence, io.forceAlphabetical]); return { mainSource: {}, name: '' }; };
  s.app.beginUndoGroup = () => calls.push(['begin']);
  s.app.endUndoGroup = () => calls.push(['end']);

  const payload = JSON.stringify({
    dir: '/o', first: 'depth_00000.png', fps: 24, width: 1920, height: 1080,
    layerName: 'clip', layerIndex: 1, compId: 5, compIn: 3, compOut: 7, stretch: 100,
    srcWidth: 3840, srcHeight: 2160,
  });
  s.h.ctx.__payload = JSON.stringify(payload);
  const r = call(s.h, 'depthyum_importDepth(JSON.parse(__payload))');

  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.attached, true);
  assert.strictEqual(newLayer.startTime, 3);
  assert.strictEqual(newLayer.stretch, 100);
  assert.strictEqual(newLayer.parent, orig);
  assert.deepStrictEqual(calls[0], ['begin']);
  assert.deepStrictEqual(calls[1], ['import', true, false]);
  assert.deepStrictEqual(calls.at(-1), ['end']);
  const byName = Object.fromEntries(set.map(([n, v]) => [n, Array.from(v)]));
  assert.deepStrictEqual(byName['ADBE Scale'], [200, 200]);
  assert.deepStrictEqual(byName['ADBE Anchor Point'], [960, 540]);
  assert.deepStrictEqual(byName['ADBE Position'], [1920 - 100, 1080 - 50]);
});

test('importDepth chiude sempre il gruppo undo anche in errore', () => {
  const s = scene();
  const calls = [];
  s.app.beginUndoGroup = () => calls.push('begin');
  s.app.endUndoGroup = () => calls.push('end');
  s.app.project.importFile = () => { throw new Error('boom'); };
  s.h.ctx.__payload = JSON.stringify(JSON.stringify({ dir: '/o', first: 'a.png' }));
  const r = call(s.h, 'depthyum_importDepth(JSON.parse(__payload))');
  assert.strictEqual(r.ok, false);
  assert.match(r.error, /boom/);
  assert.deepStrictEqual(calls, ['begin', 'end']);
});

test('importDepth rifiuta payload non valido', () => {
  const s = scene();
  s.h.ctx.__bad = '{rotto';
  assert.strictEqual(call(s.h, 'depthyum_importDepth(__bad)').ok, false);
});
