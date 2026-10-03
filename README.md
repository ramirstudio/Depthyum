# Depthyum

Pannello per After Effects che genera la depth map di un layer video, frame per frame, con Depth Anything V2 in locale. Il risultato è una sequenza PNG a 16 bit in scala di grigi, importata nella composizione con lo stesso timing del layer e agganciata al layer originale.

Nella depth map il bianco è vicino e il nero è lontano. Con "Vicino scuro, lontano chiaro" si inverte.

## Installazione

macOS: `./install.sh`. Windows: `.\install.ps1` da PowerShell. Su Windows con GPU NVIDIA, prima dello script, imposta `DEPTHYUM_TORCH_INDEX` all'indice CUDA di pytorch.org; senza, pip installa torch solo CPU.

Lo script crea `engine/.venv`, installa le dipendenze, collega `extension/` tra le estensioni CEP e abilita `PlayerDebugMode` (serve per le estensioni non firmate). Riavvia After Effects e apri Finestra > Estensioni > Depthyum. Il modello si scarica da Hugging Face al primo uso (Small circa 100 MB).

Richiede After Effects 2020 o successivo e Python 3.9 o successivo.

## Uso

Seleziona un solo layer video in una composizione e premi "Genera depth map". Il layer deve venire da un file video: precomposizioni, solidi e sequenze di immagini vanno prima renderizzati. Time remap e stretch negativo non sono supportati. Lo stretch positivo sì.

Le sequenze finiscono in `Depthyum/` accanto al progetto salvato, altrimenti in `~/Depthyum`. Il progetto After Effects le referenzia da lì, quindi non spostarle.

Il modello vede un frame alla volta, quindi la depth tremolerebbe. Il motore lo compensa in tre modi: gli estremi di normalizzazione vengono stabilizzati nel tempo dentro ogni inquadratura (gli stacchi si rilevano da soli), una media mobile attenua il flicker dei pixel senza lasciare scie sugli oggetti in movimento, e l'output è a 16 bit per evitare le bande nel blur di profondità.

## Modelli e licenze

Small è Apache 2.0. Base e Large sono CC BY-NC 4.0, quindi solo uso non commerciale. Se il lavoro è per un cliente, usa Small.

## Motore da riga di comando

```
cd engine
.venv/bin/python -m depthyum run --input clip.mp4 --out out --start 0 --duration 5 --fps 24 --model small
```

Stampa un evento JSON per riga (`status`, `progress`, `done`, `error`). `--model` accetta anche una cartella locale con un modello Hugging Face già scaricato.

## Test

```
cd engine && .venv/bin/python -m pytest
node --test test/*.test.js
```

I test Python usano un modello finto per la pipeline e un Depth Anything a pesi casuali per il percorso dei tensori. I test JS usano stub degli oggetti di After Effects: verificano la logica, non il comportamento reale di AE.
