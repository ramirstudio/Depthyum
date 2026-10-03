(function () {
  'use strict';

  var lib = window.DepthyumLib;
  var fs = require('fs');
  var path = require('path');
  var os = require('os');
  var cp = require('child_process');

  var $ = function (id) { return document.getElementById(id); };
  var KEY = 'depthyum.settings.v1';

  var state = { info: null, proc: null, busy: false, cancelled: false };

  // ---- impostazioni ------------------------------------------------------

  var FIELDS = ['model', 'size', 'range', 'smooth', 'invert', 'python', 'outroot'];

  function loadSettings() {
    var saved = {};
    try { saved = JSON.parse(localStorage.getItem(KEY)) || {}; } catch (e) {}
    FIELDS.forEach(function (id) {
      if (!(id in saved)) return;
      var el = $(id);
      if (el.type === 'checkbox') el.checked = !!saved[id]; else el.value = saved[id];
    });
    showSmooth();
  }

  function saveSettings() {
    var out = {};
    FIELDS.forEach(function (id) {
      var el = $(id);
      out[id] = el.type === 'checkbox' ? el.checked : el.value;
    });
    try { localStorage.setItem(KEY, JSON.stringify(out)); } catch (e) {}
  }

  function options() {
    return {
      model: $('model').value,
      size: Number($('size').value),
      range: $('range').value,
      smooth: Number($('smooth').value) / 100,
      invert: $('invert').checked
    };
  }

  function showSmooth() { $('smooth-out').textContent = $('smooth').value + '%'; }

  // ---- percorsi ----------------------------------------------------------

  function engineDir() {
    // L'installer collega la cartella extension/ nelle estensioni CEP: si risale al repository.
    var real = fs.realpathSync(cep.extensionPath());
    return path.join(path.dirname(real), 'engine');
  }

  function pythonPath() {
    var custom = $('python').value.trim();
    return custom || lib.venvPython(engineDir(), process.platform);
  }

  function outputRoot(info) {
    var custom = $('outroot').value.trim();
    if (custom) return custom;
    if (info.projectDir) return path.join(info.projectDir, 'Depthyum');
    return path.join(os.homedir(), 'Depthyum');
  }

  // ---- interfaccia -------------------------------------------------------

  function say(text, isError) {
    var m = $('msg');
    m.textContent = text || '';
    m.className = isError ? 'error' : '';
  }

  function setBusy(busy) {
    state.busy = busy;
    $('go').hidden = busy;
    $('stop').hidden = !busy;
    $('bar').hidden = !busy;
    $('opts').querySelectorAll('select, input').forEach(function (el) { el.disabled = busy; });
    refreshGo();
  }

  function refreshGo() { $('go').disabled = state.busy || !state.info; }

  function showLayer(res) {
    var box = $('layer');
    if (res && res.ok) {
      state.info = res;
      box.className = '';
      $('layer-name').textContent = res.layerName;
      $('layer-meta').textContent = lib.describe(res);
    } else {
      state.info = null;
      box.className = 'error';
      $('layer-name').textContent = 'Nessun layer';
      $('layer-meta').textContent = (res && res.error) || 'Nessuna risposta da After Effects.';
    }
    refreshGo();
  }

  function setProgress(stage, done, total) {
    var label = stage === 'depth' ? 'Analisi' : 'Scrittura';
    $('bar').max = total || 1;
    $('bar').value = done;
    say(label + ' ' + done + ' / ' + total);
  }

  // ---- After Effects -----------------------------------------------------

  function host(call) {
    return cep.evalScript(call).then(function (raw) {
      try { return JSON.parse(raw); } catch (e) { return { ok: false, error: String(raw) }; }
    });
  }

  function pollSelection() {
    if (state.busy || document.hidden) return;
    host('depthyum_getSelection()').then(function (res) { if (!state.busy) showLayer(res); });
  }

  function importResult(result) {
    var info = state.info;
    var payload = {
      dir: result.dir, first: result.first, fps: result.fps,
      width: result.width, height: result.height,
      layerName: info.layerName, layerIndex: info.layerIndex, compId: info.compId,
      compIn: info.compIn, compOut: info.compOut, stretch: info.stretch,
      srcWidth: info.srcWidth, srcHeight: info.srcHeight
    };
    var call = 'depthyum_importDepth(' + JSON.stringify(JSON.stringify(payload)) + ')';
    return host(call).then(function (res) {
      if (res.ok) {
        say(res.attached
          ? 'Layer depth aggiunto sopra «' + info.layerName + '», ' + result.count + ' frame.'
          : 'Layer depth aggiunto, ' + result.count + ' frame. Originale non trovato, nessun aggancio.');
      } else {
        say(res.error + ' La sequenza è in ' + result.dir, true);
      }
    });
  }

  // ---- motore ------------------------------------------------------------

  function generate() {
    host('depthyum_getSelection()').then(function (fresh) {
      showLayer(fresh);
      if (!fresh.ok) return say(fresh.error, true);

      var py = pythonPath();
      if (!fs.existsSync(py)) {
        return say('Python non trovato in ' + py + '. Esegui install.sh (macOS) o install.ps1 (Windows).', true);
      }

      var info = fresh;
      var outDir = path.join(outputRoot(info), lib.outputName(info, new Date()));
      try { fs.mkdirSync(outDir, { recursive: true }); } catch (e) { return say('Cartella di output non scrivibile: ' + outDir, true); }

      state.cancelled = false;
      setBusy(true);
      $('bar').value = 0;
      say('Avvio motore');

      var env = Object.assign({}, process.env, {
        PYTHONUNBUFFERED: '1', PYTHONIOENCODING: 'utf-8', PYTHONPATH: engineDir()
      });
      var proc = cp.spawn(py, lib.buildArgs(info, options(), outDir), { cwd: engineDir(), env: env, windowsHide: true });
      state.proc = proc;

      var result = null, failure = null, buffer = '', stderr = '';

      function handle(line) {
        var ev = lib.parseLine(line);
        if (!ev) return;
        if (ev.event === 'status') say(ev.message);
        else if (ev.event === 'progress') setProgress(ev.stage, ev.done, ev.total);
        else if (ev.event === 'done') result = ev;
        else if (ev.event === 'error') failure = ev.message;
      }

      proc.stdout.on('data', function (chunk) {
        buffer += chunk.toString('utf8');
        var lines = buffer.split('\n');
        buffer = lines.pop();
        lines.forEach(handle);
      });
      proc.stderr.on('data', function (chunk) { stderr = (stderr + chunk.toString('utf8')).slice(-2000); });
      proc.on('error', function (err) { failure = err.message; });

      proc.on('close', function (code) {
        if (buffer) handle(buffer);
        state.proc = null;
        var finish = function () { setBusy(false); };
        if (state.cancelled) { say('Annullato.'); return finish(); }
        if (result && code === 0) {
          say('Importazione in After Effects');
          return importResult(result).then(finish, function () { finish(); });
        }
        var tail = stderr.trim().split('\n').slice(-2).join(' ');
        say(failure || tail || ('Il motore è terminato con codice ' + code), true);
        finish();
      });
    });
  }

  function cancel() {
    if (!state.proc) return;
    state.cancelled = true;
    state.proc.kill();
  }

  // ---- stato del motore e tema ------------------------------------------

  function checkEngine() {
    var py;
    try { py = pythonPath(); } catch (e) { $('engine').textContent = 'Motore non raggiungibile.'; return; }
    if (!fs.existsSync(py)) { $('engine').textContent = 'Motore non installato.'; return; }
    cp.execFile(py, ['-m', 'depthyum', 'check'], { cwd: engineDir(), env: Object.assign({}, process.env, { PYTHONPATH: engineDir() }), windowsHide: true, timeout: 60000 },
      function (err, out) {
        var info = lib.parseLine(String(out || '').trim().split('\n').pop());
        if (!info || info.error) { $('engine').textContent = 'Motore con errori: ' + ((info && info.error) || (err && err.message) || 'nessuna risposta'); return; }
        var where = info.gpu ? info.device + ' (' + info.gpu + ')' : info.device;
        var model = $('model').value;
        var note = info.cached && info.cached[model] === false ? ' · il modello verrà scaricato al primo uso' : '';
        $('engine').textContent = 'Motore su ' + where + note;
      });
  }

  function applyTheme() {
    var env = cep.hostEnvironment();
    var c = env && env.appSkinInfo && env.appSkinInfo.panelBackgroundColor && env.appSkinInfo.panelBackgroundColor.color;
    if (!c) return;
    var root = document.documentElement.style;
    var light = lib.textColorFor(c.red, c.green, c.blue) === '#1a1a1a';
    root.setProperty('--bg', 'rgb(' + Math.round(c.red) + ',' + Math.round(c.green) + ',' + Math.round(c.blue) + ')');
    root.setProperty('--fg', light ? '#1a1a1a' : '#e6e6e6');
    root.setProperty('--muted', light ? '#5d5d5d' : '#9a9a9a');
    root.setProperty('--line', light ? '#b5b5b5' : '#3a3a3a');
    root.setProperty('--field', light ? '#ffffff' : '#2e2e2e');
  }

  // ---- avvio -------------------------------------------------------------

  if (!cep.available) {
    say('Questo pannello gira solo dentro After Effects.', true);
    return;
  }

  loadSettings();
  applyTheme();
  cep.onThemeChange(applyTheme);

  $('opts').addEventListener('input', function () { showSmooth(); saveSettings(); });
  $('opts').addEventListener('change', function () { saveSettings(); checkEngine(); });
  $('settings').addEventListener('change', function () { saveSettings(); checkEngine(); });
  $('go').addEventListener('click', generate);
  $('stop').addEventListener('click', cancel);

  checkEngine();
  pollSelection();
  setInterval(pollSelection, 1500);
})();
