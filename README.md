# IR4th

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, monomorphic concatenative intermediate language designed for retro-computing CPUs (such as Z80 and 6502) and constrained embedded systems.

Word-only IR4th programs can be executed directly under **Gforth** using the `ir4th.fs` prelude, or compiled in a single pass to native machine code via `ir4c` (written in strict C89).

See [spec.md](spec.md) for the full language specification.

## Targets

- **Z80** (`-t z80`): Generates Z80 machine code, with optional `--cpm` support to emit a CP/M `.com` entry stub that prints `main`'s return value.
- **6502** (`-t 6502`): Generates 6502 machine code.
- **ARM64** (`-t arm64`): Generates AArch64 machine code, with `--run` support for direct JIT execution on macOS Apple Silicon.

## Building & Testing

```sh
make              # Build build/ir4c (C89)
make test         # Compile tests for Z80, 6502, ARM64 and run ARM64 JIT tests
make test-z80     # Build CP/M .com binaries and run them via tnylpo (if installed)
make test-gforth  # Run tests under Gforth using ir4th.fs
```

## Usage

Compile with `ir4c`:

```sh
./build/ir4c [-t z80|6502|arm64] [-b hex_addr] [-o out.bin] [--run] [--cpm] <input.ir4>
```

Run directly with Gforth:

```sh
gforth ir4th.fs tests/fib.ir4 -e "main . cr bye"
```

## Repository Layout

- `src/` — Compiler sources (`ir4c.c`, `ir4c.h`, `frontend.c`, `codebuf.c`, `be_z80.c`, `be_6502.c`, `be_arm64.c`)
- `ir4th.fs` — Gforth compatibility prelude
- `spec.md` — IR4th v0.3 language specification
- `tests/` — Test programs (`fib.ir4`, `core.ir4`, `muldiv.ir4`, `mem.ir4`, `float.ir4`)
- `tools/` — Helper scripts (`gen_math48.py` for regenerating `src/be_z80_math48.h`)

## License

Licensed under the **GNU General Public License v3.0 or later (GPLv3+)** with a compiled-output exception and a separate notice for Z80 `math48`. See [LICENSE](LICENSE) (and [GPLv3](GPLv3) for the full GPLv3 text).
