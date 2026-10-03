(function (g) {
  var host = g.__adobe_cep__;

  function decodePath(uri) {
    var p = decodeURI(uri).replace('file://', '');
    return /^\/[A-Za-z]:/.test(p) ? p.slice(1) : p;
  }

  g.cep = {
    available: !!host,
    evalScript: function (script) {
      return new Promise(function (resolve) { host.evalScript(script, resolve); });
    },
    extensionPath: function () {
      return decodePath(host.getSystemPath('extension'));
    },
    hostEnvironment: function () {
      try { return JSON.parse(host.getHostEnvironment()); } catch (e) { return null; }
    },
    onThemeChange: function (cb) {
      host.addEventListener('com.adobe.csxs.events.ThemeColorChanged', cb);
    }
  };
})(window);
