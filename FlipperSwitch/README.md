# CivSwitch for Flipper Zero

Native USB port of [PicoSwitch](../PicoSwitch). The Flipper presents the same
two HID interfaces: a Switch-compatible controller and a boot keyboard. It
streams a compiled macro from the SD card, retaining the original A presses,
US keyboard reports and minimum hold/gap timings. No Pico, GPIO module or
custom firmware is required by the source API.

**Status:** version **0.2** builds with official firmware SDK **1.5.1**, target
**f7**, API **88.2**. Version 0.1 loaded on the user's Flipper but stalled at USB
enumeration: it advertised the Pico's 64-byte EP0 while the Flipper HAL uses
8-byte control packets. Version 0.2 fixes that mismatch and binds the descriptor
to the running firmware's actual EP0 size. Switch enumeration and a complete
Civ7 run still require a physical retest. A firmware with another API may
require rebuilding with its SDK.

## Install and use

Copy these files onto the Flipper SD card using qFlipper or a card reader:

| Local file | Flipper SD path |
| --- | --- |
| `dist/civswitch.fap` | `/apps/USB/civswitch.fap` |
| `generated/payload.civ` | `/apps_data/civswitch/payload.civ` |

1. Disconnect the Flipper from the PC after copying. Set the Switch's USB
   keyboard layout to English (US), and follow the upstream game preparation
   in the [main README](../README.md). Upstream targets the physical Switch 1
   edition of Civilization VII **1.0.0**, running on a Switch 1 or Switch 2.
2. Open the multiplayer wireless lobby chat. Connect the Flipper to the Switch
   dock using a **USB data cable**.
3. On the Flipper, open **Apps -> USB -> CivSwitch**, then press **OK**.
   After checking the complete SD file, the app waits for enumeration, waits
   one second and starts the five A presses followed by the typed batches.
   If the host does not configure USB within 30 seconds, it stops with a timeout.
4. Leave the game and cable alone while typing. The default payload contains
   87 lines; nominal macro time is about 16.5 minutes, plus SD/USB overhead.
   The screen displays line progress and report percentage.
5. **Back** stops playback, releases inputs and exits. Disconnect, suspend or
   USB timeout aborts playback; there is no automatic resume inside a line.
   Restart from a fresh lobby/session after an interrupted payload.
6. `Done` means the USB reports were sent. Check the Switch screen to establish
   whether the payload ran. The upstream PC installer remains a separate step;
   this app does not install the persistent save trigger.

Only press OK while connected to the Switch. A PC also accepts HID reports,
so starting while plugged into a PC would type into its active application.

## Build

From the repository root, with Python 3.12 and Node/npm installed:

```sh
python -m pip install ufbt==0.2.6
cd PicoSwitch
npm install
node compress.js
cd ..
python FlipperSwitch/tools/compile_payload.py PicoSwitch/payload.txt FlipperSwitch/generated/payload.civ
python -m unittest discover -s FlipperSwitch/tests -v
ufbt update --channel=release
cd FlipperSwitch
ufbt
```

`compress.js` uses the existing `core.js`; this port does not change that
payload. For custom JavaScript, use the upstream staging/compression workflow
before compiling `payload.txt`. The compiler rejects non-ASCII characters,
empty payloads and chat lines longer than 254 characters, rather than silently
dropping characters. Blank lines are ignored as in PicoSwitch.

If PicoSwitch descriptors change, run `python FlipperSwitch/tools/sync_descriptors.py`
from the repo root and rebuild. Its report descriptors, USB IDs, interface
ordering and endpoint addresses are copied verbatim.
The device descriptor's control endpoint packet size is adapted to the Flipper
HAL; it must not retain the Pico's 64-byte value.

## Hardware acceptance still needed

- Load version 0.2 on the installed firmware without missing-symbol/API errors.
- Confirm controller + keyboard enumeration on the Switch, and the five A taps.
- Run a short harmless chat payload and verify capitalization, punctuation and
  repeated characters. Example source text: `CivSwitch test AaA aa 11 !! [] {} ?`.
- Complete all 87 lines, check the loader result, then test Back and disconnect
  handling. Verify that exiting restores normal Flipper USB access.

## Implementation and credits

The file format starts with `CIV7FZ01`, a little-endian 32-bit report count and
line count. Each 12-byte record stores a little-endian 16-bit minimum hold time,
an 8-byte boot keyboard report and little-endian 16-bit controller button mask.
Only neutral/A and single-key reports are accepted. The final record releases
all inputs. Files are checked for exact length and valid records before USB
mode changes; playback reads 768-byte blocks instead of loading the payload
into RAM. Each transfer is acknowledged by the host before timing begins.

The app restores the previous USB interface when it exits. Its source uses the
public `FuriHalUsbInterface` API and the SDK's inline `usbd_*` driver calls;
it does not import private firmware functions.

Original CivSwitch / PicoSwitch: **Gezine**, MIT, see [LICENSE](../LICENSE).
Pico controller lineage: **Element2424**,
[Pico-Switch-Controller](https://github.com/Element2424/Pico-Switch-Controller).
USB transport API: [Flipper Devices firmware](https://github.com/flipperdevices/flipperzero-firmware).
The exploit payload and its credits remain unchanged; see the main README.
