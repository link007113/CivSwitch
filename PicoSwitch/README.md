# PicoSwitch (Macro build)

Raspberry Pi Pico firmware (Pico 2 / Pico 2 W with RP2350, or Pico 1 with RP2040): Switch gamepad + USB keyboard macro that types the CIV7 chat-XSS payload once, ~1 s after boot (`MACRO_START_DELAY_MS` in `include/MacroEngine.h`).
LED is lit while the macro runs.

The Pico 2 W uses its CYW43 onboard LED. This USB macro build does not
connect to Wi-Fi or enable Bluetooth; the wireless driver is used only for
the LED. If CYW43 initialization fails, USB macro playback continues.

## Payload pipeline

```
<payload.js>                      any JS for the chat XSS (loader bootstrap etc.)
  -> python tools/make_payload.py <payload.js>     stages it verbatim (CRLF-normalized)
  -> ../core.js
  -> node compress.js              terser -> deflateRaw -> base-25 chunked lines (<=254 chars)
  -> payload.txt
  -> cmake --build <build>         payload_to_macro.py -> include/macro_script.h -> firmware
```

The chat eval context is synchronous: the payload must not use top-level await (`node --check core.js` must pass).
If yours needs it, wrap it in its own `(async function(){ ...
})();` before staging.

The shipped `../core.js`/`payload.txt` carry the **Remote JS Loader** bootstrap (exploit core + TCP loader on :9026): ~87 typed lines, ~20 min.
After it runs, send the persistent installer from the PC with `../installer-creator/`.

## Build

All three boards build from this one tree via `CMakePresets.json`, each into
its own build directory:

| Board | Configure/build preset | UF2 output |
| --- | --- | --- |
| Pico 2 W | `pico2-w` | `build-pico2-w/src/civ7-pico2-w.uf2` |
| Pico 2 (default) | `pico2` | `build-pico2/src/civ7-pico2.uf2` |
| Pico 1 | `pico1` | `build-pico1/src/civ7-pico1.uf2` |

The SDK board name for the Pico 2 W is `pico2_w` (underscore); the preset
name is `pico2-w` (hyphen). A manual configure with `-DPICO_BOARD=pico2_w`
also selects the correct output name and RP2350 UF2 family.

Needs the pico-sdk (set `PICO_SDK_PATH`; 2.3.1), ARM toolchain, Ninja, Node + terser, and a modern host gcc.

```
cd PicoSwitch
npm install                                        # terser for compress.js
node compress.js                                   # reads ../core.js -> payload.txt (<=254-char lines)
python tools/make_payload.py <payload.js>         # optional: stage a custom payload into ../core.js
export PICO_SDK_PATH=<sdk>
cmake --preset pico2 && cmake --build --preset pico2
cmake --preset pico2-w && cmake --build --preset pico2-w
cmake --preset pico1 && cmake --build --preset pico1
```

On Ubuntu 24.04 the full toolchain installs with `sudo apt install -y git build-essential cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib python3 nodejs npm`.
On Windows, prefix the cmake commands with a modern mingw FIRST on PATH (`PATH=<modern-mingw>/bin:$PATH cmake ...`) — CMake may otherwise pick an ancient host gcc that cannot build pioasm.
Alternatively, use the official prebuilt `pioasm` matching SDK 2.3.1 from
[pico-sdk-tools](https://github.com/raspberrypi/pico-sdk-tools/releases/tag/v2.3.1-0)
and add `-Dpioasm_DIR=<path-to-extracted-pioasm-directory>` when configuring.
This avoids needing a host C++ compiler. The ARM cross-compiler is still required.

## Flash the Pico 2 W

1. Disconnect the board. Hold **BOOTSEL** while connecting it to the PC with
   a USB data cable, then release BOOTSEL.
2. Copy `build-pico2-w/src/civ7-pico2-w.uf2` onto the **RP2350** USB drive.
   The drive disconnects and the board restarts automatically.
3. The macro starts automatically after reboot, including immediately after
   flashing. Prepare the console's chat screen before connecting the board
   to the console. Disconnect the board from the PC after copying the UF2;
   it presents itself as a keyboard and gamepad.

Use the Pico 2 W UF2 for this board. Both Pico 2 boards need the RP2350 UF2
family; the Pico 1 uses RP2040. No external LED or wiring is needed.

Build verification does not prove physical USB enumeration, LED behavior,
or successful typing on a console; those require a Pico 2 W hardware test.

UF2 generation does not use picotool (its `uf2 convert` segfaults on some hosts): the build packs the UF2 straight from the .bin with `pack_uf2.py`, byte-identical to picotool's output on both platforms (rp2040 data blocks; rp2350-arm-s data blocks plus the RP2350-E10 abs block with its RP2-ignore-block extension tag). To pack one manually:

```
<toolchain>/arm-none-eabi-objcopy -Obinary build-pico2/src/civ7-pico2.elf build-pico2/src/civ7-pico2.bin
python tools/pack_uf2.py build-pico2/src/civ7-pico2.bin civ7-pico2.uf2 [rp2040|rp2350]
```

## Macro shape

Preamble: gamepad A x5.
Per payload line: Esc, wait 750 ms, A, wait 550 ms, type line, Enter x2, then A between batches.
Timings are hardware-tested constants at the top of `tools/payload_to_macro.py` - do not lower without re-testing.
