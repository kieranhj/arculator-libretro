#!/usr/bin/env python3
"""Build an RT_KEYS string for scripts/rt.c that types text on the emulated
machine. Usage: rt-keys.py <start-frame> <text>   ('\n' = Return, '{F12}' = F12)
Prints the key list and, on stderr, the frame at which typing finishes."""
import sys
shift = {':': ';', '$': '4', '!': '1', '*': '8', '_': '-', '"': '2', '&': '7', '(': '9', ')': '0', '<': ',', '>': '.', '?': '/', '+': '=', '@': "'"}
f = int(sys.argv[1]); text = sys.argv[2].replace(chr(92) + 'n', chr(10))
seq = []
i = 0
while i < len(text):
    ch = text[i]
    if text.startswith('{F12}', i):
        seq.append((f, 293, 3)); f += 8; i += 5; continue
    if ch == '\n': seq.append((f, 13, 3)); f += 6
    elif ch in shift: seq.append((f, 304, 7)); seq.append((f + 2, ord(shift[ch]), 3)); f += 8
    elif ch.isupper(): seq.append((f, 304, 7)); seq.append((f + 2, ord(ch.lower()), 3)); f += 8
    else: seq.append((f, ord(ch), 3)); f += 6
    i += 1
print(",".join("%d:%d:%d" % s for s in seq))
sys.stderr.write("%d\n" % f)
