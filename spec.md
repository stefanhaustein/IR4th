# IR4th Language Specification

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, monomorphic concatenative intermediate language designed for retro-computing CPUs (such as the Z80 and 6502) and constrained embedded systems.

IR4th compiles via direct 2-pass machine-code generation (with dead-code elimination) to native Z80, 6502, RISC-V (RV32), and ARM64 binaries.

## 1. Lexical Structure

### 1.1 Tokenization & Whitespace
* Programs consist of **whitespace-delimited tokens** (spaces, tabs, newlines).
* Line comments start with `\` (followed by whitespace or end-of-line) and extend to the end of the line.

### 1.2 Lexical Categories
* **Type Tokens (`TYPE`):** Any token ending with a trailing colon (`:`) is strictly a type specifier (`i:`, `a:`, `f:`).
* **Keywords:** Reserved syntax and control-flow tokens (`fn`, `end`, `require`, `extern`, `buffer:`, `create`, `i8,`, `i16,`, `,`, `align`, `->`, `exit`, `if`, `else`, `then`, `begin`, `while`, `repeat`, `until`, `{`, `}`, `|`, `--`).
* **Numeric Literals:** Decimal (`0`, `42`, `-10`), hexadecimal (`0xFF`, `$4000`), or floating-point (`3.14`, `-0.5`).
* **Identifiers (`IDENT`):** Any non-numeric token without a trailing colon that is not a keyword.

## 2. Type System (v0.3)

IR4th v0.3 defines **three stack types** on a **single unified stack** with **type-directed operator overloading** for arithmetic and comparisons. Nothing smaller than a native machine word `i:` exists on the stack; byte- and 16-bit memory accesses (`u8@`, `i8@`, `i8!`, `u16@`, `i16@`, `i16!`) implicitly widen to and truncate from `i:`.

| Type Token | Name | Width & Semantics | Z80 / 6502 | ARM32 / RISC-V32 | ARM64 / x86-64 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`i:`** | Native Word | Target-native signed integer (`isize`) | 2 bytes (`i16`) | 4 bytes (`i32`) | 8 bytes (`i64`) |
| **`a:`** | Address | Target-width memory pointer (`usize`) | 2 bytes (`u16`) | 4 bytes (`u32`) | 8 bytes (`u64`) |
| **`f:`** | Float | Target-native floating-point | **6 bytes** (Z80) / **4 bytes** (6502) | **4 bytes** (IEEE-754 `binary32`) | **8 bytes** (IEEE-754 `binary64`) |

### 2.1 Literal Typing Rules
* **Bare Integer Literals** (`0`, `1`, `-42`, `0x10`, `$FF`) implicitly default to **`i:`**.
* **Explicitly Typed Literals** prefix the value with a `TYPE` token separated by whitespace:
  * `a: 0x0400` — pushes target-width memory address `0x0400` (`a:`)
  * `f: 3.14` — pushes target-native floating-point value `3.14` (`f:`)
* **Literal Width Restriction:** In the current implementation, integer and address literals are restricted to 32-bit values (`-2147483648..2147483647` for `i:`, `0..0xFFFFFFFF` for `a:`), even on 64-bit targets.

## 3. Functions, Parameters & Locals

Functions are defined at the top level using standard Forth `fn ... end` syntax with a Forth-style `{ ... }` signature header.

```forth
fn <name> { <inputs> | <locals> -- <outputs> }
  <body>
end
```

* **Inputs (before `|` or `--`):**
  * Declared as `TYPE IDENT` pairs (e.g., `a: ptr i: count`). On entry, arguments are popped from the unified data stack in standard Forth order (rightmost parameter popped from the top of the stack) and bound to their local names. Unnamed input parameters are not permitted.
* **Scratch Locals (optional, after `|` and before `--`):**
  * Declared as `TYPE IDENT` pairs (e.g., `| i: i i: sum f: acc`).
  * Scratch locals do **not** pop from the caller's stack; they allocate local storage slots initialized to `0` (`0.0` for `f:`) on function entry.
* **Outputs (after `--`):**
  * A nameless sequence of `TYPE` tokens specifying the exact stack state required at `end` or `exit` (e.g., `-- i:`, `-- f:`, `-- a: i:`, or `--` for `void`).

### 3.1 Accessing, Mutating & Addressing Locals
* **Read (`<ident>`):** Pushes the value of parameter or local `<ident>` (of its declared type `T`) onto the stack.
* **Write (`-> <ident>`):** Pops the top value from the stack (verifying its type matches `<ident>`'s declared type `T`) and stores it into `<ident>`.
* **Address (`& <ident>`):** Pushes the memory address of parameter or local `<ident>` onto the stack with type `a:`.

### 3.2 Named Data Buffers (`buffer:`)
Top-level fixed-size writable buffers can be declared outside word definitions:

```forth
<size> bufferfn <name>
```

* `<size>` is a positive byte count. Each buffer is allocated in the target's writable data segment, aligned to the target's native word size.
* Referencing `<name>` inside a word pushes its base address onto the stack with type **`a:`** (`( -- a: )`).

### 3.2b Pre-Initialized Data (`create`, `i8,`, `i16,`, `,`, `align`)
Top-level read-only tables and constants can be defined outside word definitions:

```forth
create <name>
  10 i8, -5 i8, 1000 i16,
align
create words_tbl
  100 , -200 ,
```

* `create <name>` aligns the data pointer to the target's native word size and binds `<name>` (`( -- a: )`).
* `i8,` emits 1 byte (`-128..255`). `i16,` emits 2 little-endian bytes (`-32768..65535`). `,` emits 1 native word (`word_bytes`).
* `align` advances the data pointer to the next multiple of `word_bytes`.
* `create` data is semantically read-only.

### 3.3 Including Files (`require`)
Top-level `require` loads another `.ir4` source file at most once (include-once semantics):

```forth
require <filename>
```

* `<filename>` is resolved relative to the directory of the file containing `require`.
* For relative paths, `require` searches in order:
  1. `<dir>/<platform>/<filename>` (when `--platform <name>`, `--cpm`, or `--sim65` is active, e.g. `cpm/io.ir4` or `sim65/io.ir4`)
  2. `<dir>/<cpu>/<filename>` (e.g. `arm64/io.ir4` or `z80/math.ir4`)
  3. `<dir>/<filename>` (portable fallback)
* Repeated `require` directives for an already-loaded file path are ignored.

### 3.4 External Functions (`extern`)
Top-level `extern` declares an external platform/OS function:

```forth
extern putchar { i: c -- i: }                  \ ARM64 JIT (dlsym, C ABI)
extern bdos 0x0005 { i: C i: DE -- i: A }      \ Z80 CP/M (fixed addr + registers)
```

* Signatures use `{ inputs -- outputs }` without `|` locals and with at most 1 return value.
* On ARM64 JIT (`--run`), no address is given; symbols are resolved via `dlsym` and called using the ARM64 C ABI (`X0..X7`, `D0..D7`).
* On RISC-V RV32 (`--obj`), symbolic `extern` calls emit ELF relocations (`R_RISCV_HI20` / `R_RISCV_LO12_I`) using the `ilp32` C ABI (`a0..a7`); fixed-address `extern` declarations are also supported for bare-metal targets (`--virt`).
* On 16-bit targets (Z80 / 6502), a fixed 16-bit address is required and parameter/return names specify CPU registers (`A, B, C, D, E, H, L, BC, DE, HL` on Z80; `A, X, Y` on 6502).

## 4. Core Instruction Set

In stack-effect notation `( before -- after )`, the rightmost item is the top of the stack. All types—including `f:`—reside on the single unified stack.

### 4.1 Stack Manipulation
*(Stack primitives move existing typed slots without altering their types).*

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `dup` | `( t -- t t )` | Duplicate top stack item |
| `drop` | `( t -- )` | Discard top stack item |
| `swap` | `( t1 t2 -- t2 t1 )` | Exchange top two stack items |
| `over` | `( t1 t2 -- t1 t2 t1 )` | Copy second stack item to top |

### 4.2 Word Arithmetic & Bitwise Operations (`i:`)

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `+` | `( i: i: -- i: )` | Word addition |
| `-` | `( i: i: -- i: )` | Word subtraction (`a - b`) |
| `*` | `( i: i: -- i: )` | Word multiplication |
| `/` | `( i: i: -- i: )` | Signed word division (`a / b`) |
| `mod` | `( i: i: -- i: )` | Signed word remainder (`a mod b`) |
| `u/` | `( i: i: -- i: )` | Unsigned word division |
| `umod` | `( i: i: -- i: )` | Unsigned word remainder |
| `and` | `( i: i: -- i: )` | Bitwise AND |
| `or` | `( i: i: -- i: )` | Bitwise OR |
| `xor` | `( i: i: -- i: )` | Bitwise XOR |
| `invert` | `( i: -- i: )` | Bitwise one's complement |
| `lshift` | `( i: i: -- i: )` | Logical shift left (`a << b`) |
| `arshift` | `( i: i: -- i: )` | Arithmetic (sign-preserving) shift right (`a >> b`) |
| `rshift` | `( i: i: -- i: )` | Logical (zero-filling) shift right (`a u>> b`) |

### 4.3 Address Arithmetic (`a:`)
Because `i:` and `a:` always share the same register width, address offsets require no sign-extension.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `+` | `( a: i: -- a: )` | Add byte offset `i:` to address `a:` |
| `-` | `( a: i: -- a: )` | Subtract byte offset `i:` from address `a:` |
| `-` | `( a: a: -- i: )` | Signed byte distance between two addresses (`a1 - a2`) |

### 4.4 Floating-Point Operations (`f:`)

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `+` | `( f: f: -- f: )` | Floating-point addition |
| `-` | `( f: f: -- f: )` | Floating-point subtraction (`a - b`) |
| `*` | `( f: f: -- f: )` | Floating-point multiplication |
| `/` | `( f: f: -- f: )` | Floating-point division (`a / b`) |
| `fnegate` | `( f: -- f: )` | Floating-point negation (`-a`) |

### 4.5 Comparisons (`i:`, `a:`, and `f:`)
All comparisons consume two values and push a boolean flag of type **`i:`** (`-1` for true, `0` for false).

| Instruction | Stack Effect | Semantics |
| :--- | :--- | :--- |
| `=` , `<>` | `( i: i: -- i: )` | Word equal / not equal |
| `<` , `>` , `<=` , `>=` | `( i: i: -- i: )` | Signed word comparisons |
| `u<` , `u>` , `u<=` , `u>=` | `( i: i: -- i: )` | Unsigned word comparisons |
| `=` , `<>` , `u<` , `u>` | `( a: a: -- i: )` | Address equality and unsigned pointer comparisons |
| `=` , `<>` , `<` , `>` | `( f: f: -- i: )` | Floating-point comparisons |

### 4.6 Memory Load & Store (Standard Forth Operators)
Fixed-width memory operations (`u8@`, `i8@`, `i8!`, `u16@`, `i16@`, `i16!`) widen to `i:` on load and truncate the low bits of `i:` on store.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `i8@` | `( a: -- i: )` | Fetch signed 8-bit byte from address, sign-extended to `i:` |
| `u8@` | `( a: -- i: )` | Fetch unsigned 8-bit byte from address, zero-extended to `i:` |
| `i8!` | `( i: a: -- )` | Store low 8 bits of `i:` to address |
| `i16@` | `( a: -- i: )` | Fetch signed 16-bit integer from address, sign-extended to `i:` |
| `u16@` | `( a: -- i: )` | Fetch unsigned 16-bit integer from address, zero-extended to `i:` |
| `i16!` | `( i: a: -- )` | Store low 16 bits of `i:` to address |
| `@` | `( a: -- i: )` | Fetch native word (`i:`) from address |
| `!` | `( i: a: -- )` | Store native word (`i:`) to address |
| `a@` | `( a: -- a: )` | Fetch target-width pointer (`a:`) from address |
| `a!` | `( a: a: -- )` | Store target-width pointer (`a:`) to address |
| `f@` | `( a: -- f: )` | Fetch target-width float (`f:`) from address |
| `f!` | `( f: a: -- )` | Store target-width float (`f:`) to address |

### 4.7 Explicit Type Conversions

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `sext` | `( i: -- i: )` | Sign-extend low 8 bits of `i:` to full word width `i:` |
| `w>a` | `( i: -- a: )` | Reinterpret native word `i:` as address `a:` |
| `a>w` | `( a: -- i: )` | Reinterpret address `a:` as native word `i:` |
| `w>f` | `( i: -- f: )` | Convert signed native word `i:` to float `f:` |
| `f>w` | `( f: -- i: )` | Truncate float `f:` to signed native word `i:` |

## 5. Control Flow & Static Stack Invariants

* **Conditional Branches (`<cond:i:> if ... else ... then`):** Pops a `i:` flag. Both branches must leave the type stack with identical depth and types. If `else` is omitted, the true branch must leave the stack completely even (unchanged).
* **Pre-Tested Loops (`begin <cond:i:> while ... repeat`):** `while` pops a `i:` flag. The combined condition and loop body must leave the type stack completely even at `repeat`.
* **Post-Tested Loops (`begin ... <cond:i:> until`):** `until` pops a `i:` flag. The loop body must leave the type stack completely even before the condition is pushed.
* **Calls & Early Return (`<word_name>`, `exit`):** Validates caller stack against callee inputs and pushes outputs. `exit` verifies the stack matches the word's declared output types after `--`.

## 6. Complete Example Program

```forth
\ Fill a memory buffer with a byte value and return the end address
fn memset { a: dst i: val i: len | i: i -- a: }
  0 -> i
  begin
    i len u<
  while
    val dst i + i8!
    i 1 + -> i
  repeat
  dst len +
end

\ Sum an array of signed 8-bit bytes into a native word
fn sum_signed_bytes { a: ptr i: count | i: i i: acc -- i: }
  0 -> i
  0 -> acc
  begin
    i count <
  while
    ptr i + i8@
    acc + -> acc
    i 1 + -> i
  repeat
  acc
end
```
