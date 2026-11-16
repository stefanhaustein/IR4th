# IR4th

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, monomorphic concatenative intermediate language designed for retro-computing CPUs (such as Z80 and 6502) and constrained embedded systems.

IR4th programs (with compatible `extern` bindings) can be executed directly under **Gforth** using the `ir4th.fs` prelude, or compiled via a 2-pass direct machine-code compiler (with reachability dead-code elimination) using `ir4c` (written in strict C89).

See [spec.md](spec.md) for the full language specification.

## Targets

- **Z80** (`-t z80`): Generates Z80 machine code, with optional `--cpm` support to emit a CP/M `.com` entry stub that prints `main`'s return value.
- **6502** (`-t 6502`): Generates 6502 machine code (BSS stack/locals, 4-byte Zero Page scratch configurable via `--zp`), with optional `--sim65` support for `cc65`'s `sim65` simulator.
- **ARM64** (`-t arm64`): Generates AArch64 machine code, with `--run` support for direct JIT execution on macOS Apple Silicon.
- **RISC-V RV32IM** (`-t rv32`): Generates RV32IM machine code, with `--virt` for standalone QEMU `virt` ELF32 execution and `--obj` for relocatable ELF32 `.o` output (linking `extern` symbols and `libgcc` soft-float).

## Building & Testing

```sh
make              # Build build/ir4c (C89)
make test         # Compile tests for Z80, 6502, ARM64 and run ARM64 JIT tests
make test-rv32    # Build QEMU virt ELF32 binaries and run them via qemu-system-riscv32 (if installed)
make test-6502    # Build sim65 binaries and run them via sim65 (if installed)
make test-z80     # Build CP/M .com binaries and run them via tnylpo (if installed)
make test-gforth  # Run tests under Gforth using ir4th.fs
```

## Usage

Compile with `ir4c`:

```sh
./build/ir4c [-t z80|6502|arm64|rv32] [-b hex_addr] [-o out.bin] [--platform name] [--zp hex_addr] [--run] [--cpm] [--sim65] [--virt] [--obj] <input.ir4>
```

Run directly with Gforth:

```sh
gforth ir4th.fs tests/fib.ir4 -e "main . cr bye"
```

## Repository Layout

- `src/` — Compiler sources (`ir4c.c`, `ir4.h`, `frontend.c`, `frontend.h`, `codebuf.c`, `codebuf.h`, `be_z80*`, `be_6502*`, `be_arm64*`, `be_rv32*`)
- `ir4th.fs` — Gforth compatibility prelude
- `spec.md` — IR4th v0.3 language specification
- `tests/` — Test programs (`fib.ir4`, `core.ir4`, `muldiv.ir4`, `mem.ir4`, `float.ir4`, `req.ir4`, `data.ir4`, `ext.ir4`) and platform/CPU subdirectories (`cpm/`, `sim65/`, `arm64/`, `virt/`, `gforth/`)
- `tools/` — Runtime generators (`rt_util.py`, `gen_math48.py`, `gen_wozfp.py`, `gen_rv32_fp.py`, `rv32_fp.c`)

## License

Licensed under the **GNU General Public License v3.0 or later (GPLv3+)** with a compiled-output exception and a separate notice for Z80 `math48`. See [LICENSE](LICENSE) (and [GPLv3](GPLv3) for the full GPLv3 text).
