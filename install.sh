#!/usr/bin/env bash
# macOS: crea il venv del motore, collega il pannello ad After Effects e abilita le estensioni non firmate.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"
VENV="$ROOT/engine/.venv"

"$PY" -m venv "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip
"$VENV/bin/python" -m pip install -r "$ROOT/engine/requirements.txt"
"$VENV/bin/python" -m depthyum check || true

if [ "$(uname)" = "Darwin" ]; then
  EXT="$HOME/Library/Application Support/Adobe/CEP/extensions"
  mkdir -p "$EXT"
  ln -sfn "$ROOT/extension" "$EXT/com.depthyum.panel"
  for v in 9 10 11 12 13; do
    defaults write "com.adobe.CSXS.$v" PlayerDebugMode 1
  done
  echo "Pannello collegato in $EXT. Riavvia After Effects: Finestra > Estensioni > Depthyum."
else
  echo "Motore pronto in $VENV. Il collegamento ad After Effects si fa solo su macOS e Windows."
fi
