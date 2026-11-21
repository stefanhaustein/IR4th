# IR4th Standard Library Reference

> Generated from `lib/*.ir4` by `tools/gen_lib_docs.py`.

## Modules

- [lib/stdio.ir4](#stdioir4) — Portable console/terminal I/O, formatting, and screen control
- [lib/mem.ir4](#memir4) — Memory operations and first-fit heap allocator starting at __heap_start
- [lib/math.ir4](#mathir4) — Portable floating-point math library for IR4th
- [lib/i32.ir4](#i32ir4) — Portable 32-bit signed division and modulo fallbacks (auto-required when i32/ or i32mod is used)
- [lib/gfx.ir4](#gfxir4) — Portable stateful pixel graphics (default: ANSI 8-color UTF-8 half-block backend)
- [lib/sprite.ir4](#spriteir4) — Portable hardware sprite API (optional override per platform)
- [lib/sfx.ir4](#sfxir4) — Portable minimal sound effects API (optional override per platform)
- [lib/joy.ir4](#joyir4) — Portable joystick / gamepad input API (optional override per platform)

## lib/stdio.ir4

Portable console/terminal I/O, formatting, and screen control
Platform overrides implement raw `emit` (and optionally the `default fn` entries below).

| Function | Description |
| --- | --- |
| **xemit** ( c :i ) | Emit character *c* with platform text mapping (defaults to raw emit; PETSCII on C64) |
| **cr** ( ) | Emit a newline |
| **type** ( s :a len :i ) | Print *len* bytes starting at *s* |
| **type0** ( s :a ) | Print NUL-terminated string at *s* |
| **typeu** ( n :i ) | Print *n* as minimal unsigned decimal |
| **typei** ( n :i ) | Print *n* as signed decimal |
| **typei_w** ( n :i w :i ) | Print *n* as signed decimal, right-aligned with spaces to width *w* |
| **typeh_w** ( n :i w :i ) | Print *n* as *w* zero-padded uppercase hex digits |
| **typeh** ( n :i ) | Print *n* as minimal uppercase hex |
| **cls** ( ) | Clear screen and home cursor (ANSI default) |
| **gotoxy** ( x :i y :i ) | Move text cursor to 0-based column *x*, row *y* (ANSI default) |
| **termwidth** ( -> :i ) | Return terminal width in columns (default: 80) |
| **termheight** ( -> :i ) | Return terminal height in rows (default: 24) |
| **fgcolor** ( c :i ) | Set foreground text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default) |
| **bgcolor** ( c :i ) | Set background text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default) |

## lib/mem.ir4

Memory operations and first-fit heap allocator starting at __heap_start

| Function | Description |
| --- | --- |
| **fillchar** ( dst :a len :i val :i ) | Fill *len* bytes starting at *dst* with byte value *val* |
| **memcpy** ( dst :a src :a len :i ) | Copy *len* bytes from *src* to *dst* (overlap-safe) |
| **memcmp** ( a :a b :a len :i -> :i ) | Compare *len* bytes at *a* and *b*; returns first nonzero (*a*[i] - *b*[i]) or 0 if equal |
| **heap_reset** ( ) | Reset the heap to empty (invalidates all prior allocations) |
| **heap_alloc** ( bytes :i -> :a ) | Allocate at least *bytes* bytes on the heap; returns block address |
| **heap_free** ( p :a ) | Free block *p* previously returned by heap_alloc (coalesces adjacent free blocks) |

## lib/math.ir4

Portable floating-point math library for IR4th

| Function | Description |
| --- | --- |
| **fabs** ( x :f -> :f ) | Absolute value of *x* |
| **fint** ( x :f -> :f ) | Truncate *x* toward zero, returning float |
| **frac** ( x :f -> :f ) | Fractional part of *x* (*x* - fint(*x*)) |
| **fround** ( x :f -> :i ) | Round *x* to nearest integer (half away from zero) |
| **sqrt** ( x :f -> :f ) | Square root of *x* (returns 0.0 for *x* <= 0.0) |
| **sin** ( x :f -> :f ) | Sine of *x* (radians) |
| **cos** ( x :f -> :f ) | Cosine of *x* (radians) |
| **arctan** ( x :f -> :f ) | Arctangent of *x* in [-pi/2, pi/2] |
| **atan2** ( y :f x :f -> :f ) | Two-argument arctangent atan2(*y*, *x*) in [-pi, pi] |
| **exp** ( x :f -> :f ) | Exponential function e^*x* |
| **ln** ( x :f -> :f ) | Natural logarithm ln(*x*) (returns 0.0 for *x* <= 0.0) |

## lib/i32.ir4

Portable 32-bit signed division and modulo fallbacks (auto-required when i32/ or i32mod is used)

| Function | Description |
| --- | --- |
| **i32/** ( a :i32 b :i32 -> :i32 ) | Signed 32-bit truncating division: *a* / *b* |
| **i32mod** ( a :i32 b :i32 -> :i32 ) | Signed 32-bit remainder (sign matches dividend *a*): *a* mod *b* |

## lib/gfx.ir4

Portable stateful pixel graphics (default: ANSI 8-color UTF-8 half-block backend)
Platform overrides implement the `default fn` entries below.

| Function | Description |
| --- | --- |
| **gfxmodes** ( -> :a :i ) | Returns (addr, count) of :i16 4-tuples: (mode, width, height, depth_bpp) |
| **gfxwidth** ( -> :i ) | Current graphics screen width in pixels |
| **gfxheight** ( -> :i ) | Current graphics screen height in pixels |
| **gfxdepth** ( -> :i ) | Per-pixel color depth in bits (1 on attribute-cell targets like C64/Spectrum, where gfxrgb may still return palette colors per cell) |
| **gfxvsync** ( ) | Wait for vertical blank / next frame sync (default: no-op) |
| **gfxticks** ( -> :i ) | Return platform tick counter (wraps monotonically; default: 0) |
| **gfxmode** ( mode :i ) | Switch to graphics mode index *mode* (default: ANSI half-block buffer) |
| **gfxdone** ( ) | Restore text mode and release graphics resources |
| **gfxinit** ( w :i h :i depth :i ) | Enter best-matching mode from gfxmodes for (*w*, *h*, *depth*); (0, 0, 0) selects entry 0 |
| **gfxrgb** ( r :i g :i b :i -> :i ) | Map 8-bit RGB (*r*, *g*, *b* in 0..255) to the platform's native color value for gfxcolor. On attribute-cell targets (gfxdepth = 1), non-zero colors may still set per-cell attributes. |
| **gfxcolor** ( c :i ) | Set current drawing color *c* (value returned by gfxrgb) |
| **gfxpx** ( x :i y :i ) | Plot a pixel at (*x*, *y*) in the current drawing color |
| **gfxhline** ( x :i y :i w :i ) | Draw a horizontal line of width *w* starting at (*x*, *y*) (default: gfxpx loop) |
| **gfxvline** ( x :i y :i h :i ) | Draw a vertical line of height *h* starting at (*x*, *y*) (default: gfxpx loop) |
| **gfxline** ( x1 :i y1 :i x2 :i y2 :i ) | Draw a line from (*x1*, *y1*) to (*x2*, *y2*) (default: Bresenham via gfxhline/gfxvline/gfxpx) |
| **gfxrect** ( x :i y :i w :i h :i ) | Draw a rectangle outline at (*x*, *y*) with size *w* x *h* |
| **gfxfillrect** ( x :i y :i w :i h :i ) | Draw a filled rectangle at (*x*, *y*) with size *w* x *h* (default: gfxhline loop) |
| **gfxclear** ( ) | Clear the graphics screen to the current drawing color |
| **gfxcircle** ( cx :i cy :i r :i ) | Draw a circle outline of radius *r* centered at (*cx*, *cy*) using the midpoint algorithm |

## lib/sprite.ir4

Portable hardware sprite API (optional override per platform)

| Function | Description |
| --- | --- |
| **sprcount** ( -> :i ) | Number of hardware sprite slots (0 = hardware sprites unsupported) |
| **sprdepth** ( -> :i ) | Sprite color depth in bits per pixel (default: 0) |
| **sprmaxw** ( -> :i ) | Maximum sprite width in pixels (default: 0) |
| **sprmaxh** ( -> :i ) | Maximum sprite height in pixels (default: 0) |
| **sprzdir** ( -> :i ) | Z-order direction: 1 if higher sprite id renders in front, -1 if lower id renders in front (default: 1) |
| **sprimg** ( img :i w :i h :i data :a pal :a ncolors :i -> :i ) | Define sprite image/frame *img* of screen size *w* x *h*. *data* holds *w* * *h* unsigned bytes (0 = transparent, 1..*ncolors* = 1-based palette index). *pal* holds *ncolors* unsigned bytes (color values returned by gfxrgb). Returns -1 on success, 0 if unsupported or out of sprite resources. |
| **sprset** ( id :i img :i ) | Bind image *img* to hardware sprite *id* (see sprzdir for overlap priority; default: no-op). |
| **sprpos** ( id :i x :i y :i ) | Move sprite *id* to screen position (*x*, *y*) (default: no-op) |
| **sprshow** ( id :i on :i ) | Show (*on* <> 0) or hide (*on* = 0) sprite *id* (default: no-op) |

## lib/sfx.ir4

Portable minimal sound effects API (optional override per platform)
Waveforms: 0 = pulse/square, 1 = sawtooth, 2 = triangle, 3 = noise.

| Function | Description |
| --- | --- |
| **sfxcount** ( -> :i ) | Query number of hardware sound channels (0 = sound unsupported) |
| **sfxplay** ( ch :i freq :i vol :i wave :i dur_ms :i ) | Play a tone on channel *ch* at *freq* Hz, volume *vol* (0..15), waveform *wave* (0..3), for *dur_ms* ms (0 = until sfxstop; default: no-op). |
| **sfxstop** ( ch :i ) | Silence channel *ch* (default: no-op). |
| **sfxtick** ( ) | Advance software duration timers once per frame (required on platforms without hardware duration, e.g. C64/X16; default: no-op). |

## lib/joy.ir4

Portable joystick / gamepad input API (optional override per platform)
Bitmask returned by joyread:
1 = Up, 2 = Down, 4 = Left, 8 = Right, 16 = Fire/Btn1, 32 = Btn2, 64 = Select, 128 = Start

| Function | Description |
| --- | --- |
| **joycount** ( -> :i ) | Query number of joystick/gamepad ports (0 = joystick input unsupported) |
| **joyread** ( port :i -> :i ) | Read current button bitmask for *port* (0-based; returns 0 if unsupported or out of range) |
