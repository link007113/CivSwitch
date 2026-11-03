import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = ROOT / 'FlipperSwitch/tools/compile_payload.py'
UPSTREAM = ROOT / 'PicoSwitch/tools/payload_to_macro.py'


class CompileTests(unittest.TestCase):
    def compile(self, text):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'payload.txt'
            target = Path(temp) / 'payload.civ'
            source.write_text(text, encoding='utf-8')
            result = subprocess.run([sys.executable, str(COMPILER), str(source), str(target)],
                                    capture_output=True, text=True)
            return result, target.read_bytes() if target.exists() else b''

    def test_matches_upstream_macro_for_every_printable_character(self):
        # Compare every timed report, including releases and duplicate keys,
        # against the existing Pico generator, independently of our compiler.
        text = ''.join(chr(i) for i in range(32, 127)) + 'AaAaa11!!\t'
        result, data = self.compile(text + '\r\n\r\nsecond line\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(data[:8], b'CIV7FZ01')
        count, lines = struct.unpack_from('<II', data, 8)
        self.assertEqual(lines, 2)
        self.assertEqual(len(data), 16 + count * 12)
        spec = importlib.util.spec_from_file_location('pico_macro', UPSTREAM)
        pico = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pico)
        import re
        with tempfile.TemporaryDirectory() as temp:
            src, hdr = Path(temp) / 'payload.txt', Path(temp) / 'macro.h'
            src.write_text(text + '\n\nsecond line\n', encoding='utf-8')
            subprocess.run([sys.executable, str(UPSTREAM), str(src), str(hdr)], check=True,
                           capture_output=True)
            macros = re.findall(r'^\s+(KEYS|WAIT|PAD_BTN|END)\(([^)]*)\),',
                                hdr.read_text(encoding='utf-8'), re.M)
        # Decode the binary back to HID events, then compare semantic names.
        usages = {f'HID_KEY_{c.upper()}': i + 4 for i, c in enumerate('abcdefghijklmnopqrstuvwxyz')}
        usages.update({f'HID_KEY_{c}': i + 30 for i, c in enumerate('1234567890')})
        usages.update(dict(zip(['ENTER', 'ESCAPE', 'BACKSPACE', 'TAB', 'SPACE', 'MINUS',
                              'EQUAL', 'BRACKET_LEFT', 'BRACKET_RIGHT', 'BACKSLASH',
                              'EUROPE_1', 'SEMICOLON', 'APOSTROPHE', 'GRAVE', 'COMMA',
                              'PERIOD', 'SLASH'], range(40, 57))))
        usages.update({'HID_KEY_' + k: v for k, v in list(usages.items()) if not k.startswith('HID_')})
        expected = []
        for op, args in macros:
            a = [x.strip() for x in args.split(',')]
            if op == 'END':
                expected.append((32, bytes(8), 0))
            elif op == 'WAIT':
                expected.append((int(a[0]), bytes(8), 0))
            elif op == 'PAD_BTN':
                expected.append((int(a[0]), bytes(8), 4))
            else:
                expected.append((int(a[0]), bytes([2 if a[1] != '0' else 0, 0,
                                                   usages[a[2]], 0, 0, 0, 0, 0]), 0))
        actual = [struct.unpack_from('<H8sH', data, 16 + i * 12) for i in range(count)]
        self.assertEqual(actual, expected)

    def test_rejects_payload_corruption_instead_of_dropping_characters(self):
        for text in ['', '\n \n', 'caf\u00e9', 'a\x00b', 'x' * 255]:
            result, data = self.compile(text)
            self.assertNotEqual(result.returncode, 0, repr(text))
            self.assertEqual(data, b'')

    def test_invalid_source_preserves_an_existing_compiled_payload(self):
        with tempfile.TemporaryDirectory() as temp:
            source, target = Path(temp) / 'payload.txt', Path(temp) / 'payload.civ'
            source.write_text('x' * 255, encoding='utf-8')
            target.write_bytes(b'existing valid output')
            result = subprocess.run([sys.executable, str(COMPILER), str(source), str(target)],
                                    capture_output=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(target.read_bytes(), b'existing valid output')

    def test_descriptors_preserve_composite_switch_device(self):
        import re
        source = (ROOT / 'PicoSwitch/include/SwitchConsts.h').read_text(encoding='utf-8')
        port = (ROOT / 'FlipperSwitch/civ_descriptors.h').read_text(encoding='utf-8')

        def descriptor(text, name):
            body = re.search(name + r'\[\] = \{(.*?)\};', text, re.S).group(1)
            body = re.sub(r'//[^\n]*', '', body)
            return bytes(int(x.strip(), 0) for x in body.split(',') if x.strip())

        for name in ['switch_usb_device_descriptor', 'switch_usb_configuration_descriptor',
                     'switch_usb_report_descriptor', 'keyboard_usb_report_descriptor']:
            expected = bytearray(descriptor(source, name))
            if name == 'switch_usb_device_descriptor':
                expected[7] = 8  # Flipper HAL control endpoint, not the Pico's 64.
            self.assertEqual(bytes(expected), descriptor(port, name), name)
        config = descriptor(port, 'switch_usb_configuration_descriptor')
        self.assertEqual(struct.unpack_from('<H', config, 2)[0], len(config))
        self.assertEqual(config[4], 2)
        offset, endpoints, report_lengths = 0, [], []
        while offset < len(config):
            length, kind = config[offset:offset + 2]
            self.assertGreater(length, 0)
            if kind == 5:
                endpoints.append(config[offset + 2])
            if kind == 0x21:
                report_lengths.append(struct.unpack_from('<H', config, offset + 7)[0])
            offset += length
        self.assertEqual(offset, len(config))
        self.assertEqual(endpoints, [0x02, 0x81, 0x83, 0x03])
        self.assertEqual(report_lengths, [86, 63])

    def test_device_control_packets_match_flipper_driver(self):
        import re
        text = (ROOT / 'FlipperSwitch/civ_descriptors.h').read_text(encoding='utf-8')
        body = re.search(r'switch_usb_device_descriptor\[\] = \{(.*?)\};', text, re.S).group(1)
        device = bytes(int(x.strip(), 0) for x in body.split(',') if x.strip())
        # Official f7 HAL initializes EP0 with USB_EP0_SIZE=8. Advertising the
        # Pico's 64 makes the first 8-byte reply look like a short packet: the
        # host stops reading the 18-byte device / 73-byte configuration early.
        self.assertEqual(device[7], 8)
        config = bytes(73)
        received = bytearray()
        for offset in range(0, len(config), 8):
            packet = config[offset:offset + 8]
            received.extend(packet)
            if len(packet) < device[7]:
                break
        self.assertEqual(len(received), 73)


if __name__ == '__main__':
    unittest.main()
