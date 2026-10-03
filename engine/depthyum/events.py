"""Eventi JSON su stdout, una riga per evento. Il pannello li legge riga per riga."""
import json
import sys


def emit(event, **fields):
    fields["event"] = event
    sys.stdout.write(json.dumps(fields, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def log(message):
    sys.stderr.write(message + "\n")
    sys.stderr.flush()
