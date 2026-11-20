# Agon Light / Console8 (`--agon`)

## Compiler Flags

```sh
./build/ir4c -t ez80 --agon -o out.bin input.bin
```

- **Target / Platform:** `-t ez80 --agon` (platform name `"agon"`)
- **Base address:** `0x40000` (MOS 24-bit ADL executable with `MOS` header at `+0x40`, `ADL = 1`)
- **Fallback chain:** `lib/agon/` → `lib/ez80/` → `lib/`

## Memory Layout

- **24-bit eZ80 ADL mode:** `JP entry` at `0x40000`, `MOS\0\1` header at `0x40040`, code + data + BSS + return stack following.
- **`main` signature:** 0 inputs, at most 1 output.

## Assumptions & Notes

- **Graphics (`lib/agon/gfx.ir4`):** Uses VDP VDU `22` (mode), `18` (color), `25` (`PLOT` pixel/line/fillrect).
- **Modes:** Supports Agon VDP modes `8` (`320×240×6`), `9–11` (`320×240` 4/2/1-bpp), `1–2` (`512×384`), `0`/`4` (`640×480`), `12` (`320×200×6`), `16` (`640×240×6`).
- **`gfxrgb`:** Maps `0..255` RGB to Agon's default 16-color VGA palette (`0..15`, where `15` = bright white and `8` = dark gray) for 4/6-bpp modes, plus 1-bpp and 2-bpp handling.
- **Running in `fab-agon-emulator`:** Place `out.bin` on the emulated SD card alongside `MOS.bin`/`firmware.bin`, then `load out.bin` and `run` (or use `SDL_AUDIODRIVER=dummy` for silent headless runs).
