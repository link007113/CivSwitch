#!/usr/bin/env python3
"""Compile PicoSwitch payload.txt to the Flipper's streamed HID format.

Use the original generator for ordering and timing, then translate its small
macro vocabulary. Unknown operations fail closed rather than changing input.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
USAGES = {f'HID_KEY_{c.upper()}': i + 4 for i, c in enumerate('abcdefghijklmnopqrstuvwxyz')}
USAGES.update({f'HID_KEY_{c}': i + 30 for i, c in enumerate('1234567890')})
USAGES.update({f'HID_KEY_{name}': i + 40 for i, name in enumerate([
    'ENTER', 'ESCAPE', 'BACKSPACE', 'TAB', 'SPACE', 'MINUS', 'EQUAL',
    'BRACKET_LEFT', 'BRACKET_RIGHT', 'BACKSLASH', 'EUROPE_1', 'SEMICOLON',
    'APOSTROPHE', 'GRAVE', 'COMMA', 'PERIOD', 'SLASH'])})


def compile_payload(source, target):
    lines = source.read_text(encoding='utf-8').splitlines()
    used = [line for line in lines if line.strip()]
    if not used:
        raise ValueError('payload is empty')
    for n, line in enumerate(lines, 1):
        if len(line) > 254:
            raise ValueError(f'line {n} exceeds the 254-character chat limit')
        if any(c != '\t' and not 32 <= ord(c) <= 126 for c in line):
            raise ValueError(f'line {n} contains unsupported characters; use US ASCII')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent) as temp:
        header = Path(temp) / 'macro.h'
        subprocess.run([sys.executable, str(ROOT / 'PicoSwitch/tools/payload_to_macro.py'),
                        str(source), str(header)], check=True, capture_output=True, text=True)
        generated = header.read_text(encoding='utf-8')
    records = []
    for line in generated.splitlines():
        if not line.startswith('    ') or line.strip().startswith('//'):
            continue
        match = re.fullmatch(r'\s+(\w+)\(([^)]*)\),', line)
        if not match:
            raise ValueError(f'unrecognized upstream macro: {line}')
        op, args = match.groups()
        args = [a.strip() for a in args.split(',')]
        keyboard, buttons = bytes(8), 0
        if op == 'END':
            duration = 32  # Explicitly release the last Enter before reporting completion.
        elif op == 'WAIT':
            duration = int(args[0])
        elif op == 'PAD_BTN' and args[1] == 'SWITCH_USB_MASK_A':
            duration, buttons = int(args[0]), 4
        elif op == 'KEYS' and len(args) == 8:
            duration = int(args[0])
            mods = {'0': 0, 'KEYBOARD_MODIFIER_LEFTSHIFT': 2}[args[1]]
            keyboard = bytes([mods, 0] + [0 if k == '0' else USAGES[k] for k in args[2:]])
        else:
            raise ValueError(f'unsupported upstream macro: {line}')
        if not 1 <= duration <= 65535:
            raise ValueError(f'invalid duration: {duration}')
        records.append(struct.pack('<H8sH', duration, keyboard, buttons))
    if not 1 <= len(records) <= 100000 or len(used) > 10000:
        raise ValueError('payload exceeds the app limits')
    content = b'CIV7FZ01' + struct.pack('<II', len(records), len(used)) + b''.join(records)
    # Validation and generation finish before replacing an existing payload.
    target.write_bytes(content)
    seconds = sum(struct.unpack_from('<H', r)[0] for r in records) / 1000
    print(f'{len(used)} lines, {len(records)} reports, {len(content)} bytes, '
          f'{seconds / 60:.1f} min nominal -> {target}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('target', type=Path)
    args = parser.parse_args()
    try:
        compile_payload(args.source, args.target)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'compile_payload: {exc}\n')


if __name__ == '__main__':
    main()
