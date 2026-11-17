# IR4th

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, monomorphic concatenative intermediate language designed for retro-computing CPUs (such as Z80 and 6502) and constrained embedded systems.

IR4th programs are compiled via a 2-pass direct machine-code compiler (with reachability dead-code elimination) using `ir4c` (written in strict C89).

See [spec.md](spec.md) for the full language specification.

This project is based on [hi5](https://github.com/stefanhaustein/hi5), an earlier IR stack language prototype implemented in KMP with Z80 support.


## Targets

- **Z80** (`-t z80`): Generates Z80 machine code, with optional `--cpm` support to emit a CP/M `.com` entry stub that prints `main`'s return value.
- **6502** (`-t 6502`): Generates 6502 machine code (BSS stack/locals by default, optional Zero Page data stack via `--stack-zp`, 4-byte Zero Page scratch configurable via `--zp`), with `--sim65` for `cc65`'s `sim65` simulator and `--c64` for Commodore 64 `.prg` output (BASIC `SYS` stub, KERNAL `CHROUT`, BASIC ROM unmapping, and `$20..$8F` ZP data stack by default; disable with `--no-stack-zp`).
- **ARM64** (`-t arm64`): Generates AArch64 machine code, with `--run` support for direct JIT execution on macOS Apple Silicon.
- **RISC-V RV32IM** (`-t rv32`): Generates RV32IM machine code, with `--virt` for standalone QEMU `virt` ELF32 execution and `--obj` for relocatable ELF32 `.o` output (linking `extern` symbols and `libgcc` soft-float).

## Examples

Hello world ([examples/hello.ir4](examples/hello.ir4)):

```forth
require "stdio.ir4"

data msg
  "Hello, world!\n" 0 :i8
end

fn main ( -> :i )
  msg prints
  0
end
```

Typed memory loop & recursion ([tests/fib.ir4](tests/fib.ir4), [lib/mem.ir4](lib/mem.ir4)):

```forth
fn fib ( n :i -> :i )
  n 2 < if n return then
  n 1 - fib n 2 - fib +
end
```

See [tests/](tests/) for the full test suite.

## Compiler Optimizations

- **Dead-code elimination:** Only functions reachable from `main` and `init` blocks are emitted.
- **Constant folding & casts:** Compile-time folding for integer arithmetic/comparisons and literal casts (`:a`, `:i32`, `:u32`).
- **Strength reduction:** Power-of-2 `*`, `u/`, and `umod` by constants lower to shifts (`<<`, `u>>`) and masks (`and`); multiply/unsigned-divide by `1` is elided.
- **Immediate lowering:** Constant-operand `+`/`-`, comparisons, shifts, and base+offset memory accesses emit immediate instructions where supported by the target.
- **Static locals (Z80 / 6502):** Non-recursive functions place locals in static RAM rather than frame-pointer stacks.
- **On-demand runtimes:** Math, `:i32`, and floating-point helpers are embedded only when referenced.

## Building & Testing

```sh
make              # Build build/ir4c (C89)
make test         # Compile tests for Z80, 6502, ARM64 and run ARM64 JIT tests
make test-rv32    # Build QEMU virt ELF32 binaries and run them via qemu-system-riscv32 (if installed)
make test-6502    # Build sim65 binaries and run them via sim65 (if installed)
make test-z80     # Build CP/M .com binaries and run them via tnylpo (if installed)
```

## Usage

Compile with `ir4c`:

```sh
./build/ir4c [-t z80|6502|arm64|rv32] [-b hex_addr] [-o out.bin] [-I dir] [--platform name] [--zp hex_addr] [--stack-zp hex_addr[:init_x]] [--no-stack-zp] [--run] [--cpm] [--sim65] [--c64] [--virt] [--obj] <input.ir4>
```

Pass `-I dir` (repeatable) to add library search directories ahead of the built-in `lib/` directory.

## Repository Layout

- [src/](src/) — Compiler frontend, code buffer, and target backends
- [lib/](lib/) — Portable standard library and platform I/O modules
- [examples/](examples/) — Standalone example programs
- [tests/](tests/) — Cross-target test suite and negative compiler tests in [tests/errors/](tests/errors/)
- [tools/](tools/) — Relocatable runtime generators
- [spec.md](spec.md) — IR4th v0.3 language specification

## License

Licensed under the **GNU General Public License v3.0 or later (GPLv3+)** with a compiled-output exception and separate notices for third-party float runtimes (`math48` and `wozfp`). See [LICENSE](LICENSE) (and [GPLv3](GPLv3) for the full GPLv3 text).
