(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.DepthyumLib = factory();
})(typeof window !== 'undefined' ? window : this, function () {
  var RESOLUTIONS = [364, 518, 756, 1036];
  var MODELS = ['small', 'base', 'large'];
  var RANGES = ['smooth', 'shot', 'frame'];

  function pad(n) { return (n < 10 ? '0' : '') + n; }

  function clean(s) {
    return String(s).replace(/[^A-Za-z0-9._-]+/g, '_').replace(/^_+|_+$/g, '') || 'x';
  }

  function stamp(d) {
    return d.getFullYear() + pad(d.getMonth() + 1) + pad(d.getDate()) + '-' +
      pad(d.getHours()) + pad(d.getMinutes()) + pad(d.getSeconds());
  }

  function outputName(info, now) {
    return clean(info.compName) + '_' + clean(info.layerName) + '_' + stamp(now);
  }

  function venvPython(engineDir, platform) {
    return platform === 'win32'
      ? engineDir + '\\.venv\\Scripts\\python.exe'
      : engineDir + '/.venv/bin/python';
  }

  function pick(value, allowed, fallback) {
    return allowed.indexOf(value) >= 0 ? value : fallback;
  }

  function buildArgs(info, opts, outDir) {
    var smooth = Math.min(0.95, Math.max(0, Number(opts.smooth) || 0));
    var args = [
      '-m', 'depthyum', 'run',
      '--input', info.path,
      '--out', outDir,
      '--start', String(info.srcStart),
      '--duration', String(info.srcDur),
      '--fps', String(info.srcFps),
      '--model', pick(opts.model, MODELS, 'small'),
      '--size', String(RESOLUTIONS.indexOf(Number(opts.size)) >= 0 ? Number(opts.size) : 518),
      '--range', pick(opts.range, RANGES, 'smooth'),
      '--smooth', String(smooth)
    ];
    if (opts.invert) args.push('--invert');
    return args;
  }

  function parseLine(line) {
    line = String(line).trim();
    if (!line || line.charAt(0) !== '{') return null;
    try { return JSON.parse(line); } catch (e) { return null; }
  }

  function seconds(s) {
    return s.toFixed(s < 10 ? 1 : 0).replace('.', ',') + ' s';
  }

  function describe(info) {
    return [
      info.fileName,
      Number(info.srcFps.toFixed(3)) + ' fps',
      seconds(info.srcDur),
      info.srcWidth + '×' + info.srcHeight
    ].join(' · ');
  }

  function textColorFor(r, g, b) {
    return (0.299 * r + 0.587 * g + 0.114 * b) > 140 ? '#1a1a1a' : '#e6e6e6';
  }

  return {
    buildArgs: buildArgs, parseLine: parseLine, outputName: outputName, venvPython: venvPython,
    describe: describe, textColorFor: textColorFor, clean: clean
  };
});
