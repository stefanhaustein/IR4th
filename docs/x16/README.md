# Commander X16 (`--x16`)

## Compiler Flags

```sh
./build/ir4c -t 6502 --x16 -o out.prg input.ir4
```

- **Target / Platform:** `-t 6502 --x16` (platform name `"x16"`)
- **Base address:** `0x0801` (BASIC `10 SYS ...` autostart `.prg` header)
- **Fallback chain:** `lib/x16/` → `lib/c64/` → `lib/6502/` → `lib/`

## Memory Layout

- **Zero Page:**
  - `default_zp`: `0x7C`
  - `default_stack_zp`: `0x22..0x5A`
  - `tmp`: `0x02/0x03`
- **`ram_top` / `bfp`:** `0x9F00` (below `$9F00–$9FFF` I/O)
- **Program / BSS / Heap:** `0x0801..0x9EFF` (~37.7 KB contiguous)

## Assumptions & Notes

- **No `$01` ROM unmapping:** Unlike C64, `$01` is the X16 ROM/RAM bank register and is left untouched.
- **Terminal (`lib/x16/stdio.ir4`):** `80×60` text mode (`termwidth = 80`, `termheight = 60`), PETSCII color codes, inherits KERNAL `emit` (`$FFD2`) and `inkey` (`$FFE4`) from `lib/c64/stdio.ir4`.
- **Graphics (`lib/x16/gfx.ir4`):**
  - Mode `128`: `320×240×8bpp` (VERA Mode `$80` via KERNAL `screen_mode` `$FF5F`)
  - Mode `1`: `160×120×8bpp` (VERA Mode `$80` with `DC_HSCALE`/`DC_VSCALE = 32`)
  - `gfxmode` programs VERA palette indices `16..231` (`$1FA20`) to a `6×6×6` RGB cube (`v * 3` per 4-bit channel) for `gfxrgb`.
  - `gfxhline` uses VERA `+1` (`$10`) auto-increment; `gfxvline` uses VERA `+320` (`$E0`) auto-increment.
- **Running in `x16emu`:**
  ```sh
  x16emu -rom /path/to/rom.bin -prg out.prg -run
  ```
