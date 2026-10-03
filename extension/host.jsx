// ExtendScript (ES3): niente let/const, arrow function, JSON, Array.map.

var DEPTHYUM_STILL_EXT = {
  png: 1, jpg: 1, jpeg: 1, tif: 1, tiff: 1, exr: 1, dpx: 1, psd: 1,
  tga: 1, bmp: 1, gif: 1, hdr: 1, cin: 1, ai: 1, svg: 1, pdf: 1
};

function depthyum_json(v) {
  var t = typeof v;
  if (v === null || v === undefined) return "null";
  if (t === "number") return isFinite(v) ? String(v) : "null";
  if (t === "boolean") return v ? "true" : "false";
  if (t === "string") {
    return '"' + v.replace(/\\/g, "\\\\").replace(/"/g, '\\"')
      .replace(/\r/g, "\\r").replace(/\n/g, "\\n").replace(/\t/g, "\\t") + '"';
  }
  var parts = [], i, k;
  if (v instanceof Array) {
    for (i = 0; i < v.length; i++) parts.push(depthyum_json(v[i]));
    return "[" + parts.join(",") + "]";
  }
  for (k in v) {
    if (v.hasOwnProperty(k)) parts.push(depthyum_json(String(k)) + ":" + depthyum_json(v[k]));
  }
  return "{" + parts.join(",") + "}";
}

function depthyum_err(message) {
  return depthyum_json({ ok: false, error: message });
}

function depthyum_ext(name) {
  var m = /\.([^.]+)$/.exec(name);
  return m ? m[1].toLowerCase() : "";
}

function depthyum_ping() {
  return "ok";
}

// Layer selezionato -> informazioni per il motore, oppure un errore leggibile.
function depthyum_getSelection() {
  try {
    var comp = app.project.activeItem;
    if (!comp || !(comp instanceof CompItem)) return depthyum_err("Apri una composizione e seleziona un layer video.");

    var sel = comp.selectedLayers;
    if (sel.length === 0) return depthyum_err("Seleziona un layer video nella composizione.");
    if (sel.length > 1) return depthyum_err("Seleziona un solo layer.");

    var layer = sel[0];
    if (!(layer instanceof AVLayer) || !layer.source || !(layer.source instanceof FootageItem)) {
      return depthyum_err("Il layer non è un footage. Per una precomposizione, renderizzala prima in un file video.");
    }
    var item = layer.source;
    var src = item.mainSource;
    if (!(src instanceof FileSource)) return depthyum_err("Il layer non usa un file (solido o altra sorgente).");
    if (src.isStill || !item.hasVideo) return depthyum_err("Serve un video, il layer è un'immagine fissa.");

    var file = src.file;
    if (!file || !file.exists) return depthyum_err("File sorgente non trovato (footage offline).");
    if (DEPTHYUM_STILL_EXT[depthyum_ext(file.name)]) {
      return depthyum_err("Le sequenze di immagini non sono supportate. Usa un file video.");
    }
    if (layer.timeRemapEnabled) return depthyum_err("Il time remap non è supportato. Disattivalo o applicalo con 'Sequenzia layer'.");
    if (layer.stretch <= 0) return depthyum_err("Stretch negativo o nullo non supportato.");

    var stretch = layer.stretch;
    var srcStart = (layer.inPoint - layer.startTime) * 100 / stretch;
    var srcDur = (layer.outPoint - layer.inPoint) * 100 / stretch;
    if (srcStart < 0) { srcDur += srcStart; srcStart = 0; }
    if (srcDur > item.duration - srcStart) srcDur = item.duration - srcStart;
    if (srcDur <= 0) return depthyum_err("Il layer non mostra nessuna parte del footage.");

    return depthyum_json({
      ok: true,
      layerName: layer.name, layerIndex: layer.index,
      compId: comp.id, compName: comp.name,
      path: file.fsName, fileName: file.name,
      srcFps: item.frameRate, srcStart: srcStart, srcDur: srcDur,
      srcWidth: item.width, srcHeight: item.height,
      compIn: layer.inPoint, compOut: layer.outPoint, stretch: stretch,
      projectDir: app.project.file ? app.project.file.parent.fsName : ""
    });
  } catch (e) {
    return depthyum_err("Script host: " + e.toString());
  }
}

// Importa la sequenza depth, la porta allo stesso timing del layer e la aggancia al suo posto.
function depthyum_importDepth(payload) {
  var p;
  try {
    p = eval("(" + payload + ")");
  } catch (e) {
    return depthyum_err("Dati di importazione non validi.");
  }

  app.beginUndoGroup("Depthyum");
  try {
    var first = new File(p.dir + "/" + p.first);
    if (!first.exists) return depthyum_err("Sequenza non trovata: " + first.fsName);

    var io = new ImportOptions(first);
    io.sequence = true;
    io.forceAlphabetical = false;
    var item = app.project.importFile(io);
    item.mainSource.conformFrameRate = p.fps;
    item.name = "Depth - " + p.layerName;

    var comp = app.project.itemByID(p.compId);
    if (!comp || !(comp instanceof CompItem)) comp = app.project.activeItem;
    if (!comp || !(comp instanceof CompItem)) return depthyum_err("Composizione non trovata.");

    var orig = null;
    if (p.layerIndex >= 1 && p.layerIndex <= comp.numLayers) {
      var candidate = comp.layer(p.layerIndex);
      if (candidate.name === p.layerName) orig = candidate;
    }

    var layer = comp.layers.add(item);
    layer.name = "Depth - " + p.layerName;

    // Il frame 0 della depth corrisponde al punto di ingresso del layer originale.
    layer.stretch = p.stretch;
    layer.startTime = p.compIn;
    if (Math.abs(layer.inPoint - p.compIn) > 0.0005) layer.startTime = layer.startTime + (p.compIn - layer.inPoint);
    if (layer.outPoint > p.compOut + 0.0005) layer.outPoint = p.compOut;

    if (orig) {
      // Figlio dell'originale: segue posizione, scala e rotazione anche se animate.
      // La posizione del figlio è relativa al punto di ancoraggio del genitore.
      var oa = orig.property("ADBE Transform Group").property("ADBE Anchor Point").value;
      var t = layer.property("ADBE Transform Group");
      layer.parent = orig;
      t.property("ADBE Anchor Point").setValue([p.width / 2, p.height / 2]);
      t.property("ADBE Scale").setValue([p.srcWidth / p.width * 100, p.srcHeight / p.height * 100]);
      t.property("ADBE Position").setValue([p.srcWidth / 2 - oa[0], p.srcHeight / 2 - oa[1]]);
      layer.moveBefore(orig);
    }

    return depthyum_json({ ok: true, layerName: layer.name, attached: !!orig });
  } catch (e) {
    return depthyum_err("Importazione: " + e.toString());
  } finally {
    app.endUndoGroup();
  }
}
