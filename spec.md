# IR4th Language Specification

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, monomorphic concatenative intermediate language designed for retro-computing CPUs (such as the Z80 and 6502) and constrained embedded systems.

IR4th is designed so that programs (with compatible `extern` bindings) are directly executable under **Gforth** (via `ir4th.fs`) while compiling via direct 2-pass machine-code generation (with dead-code elimination) to native Z80, 6502, RISC-V (RV32), and ARM64 binaries.

## 1. Lexical Structure

### 1.1 Tokenization & Whitespace
* Programs consist of **whitespace-delimited tokens** (spaces, tabs, newlines).
* Line comments start with `\` (followed by whitespace or end-of-line) and extend to the end of the line.

### 1.2 Lexical Categories
* **Type Tokens (`TYPE`):** Any token ending with a trailing colon (`:`) is strictly a type specifier (`W:`, `A:`, `F:`).
* **Keywords:** Reserved syntax and control-flow tokens (`:`, `;`, `require`, `extern`, `buffer:`, `create`, `i8,`, `i16,`, `,`, `align`, `to`, `exit`, `if`, `else`, `then`, `begin`, `while`, `repeat`, `until`, `{`, `}`, `|`, `--`).
* **Numeric Literals:** Decimal (`0`, `42`, `-10`), hexadecimal (`0xFF`, `$4000`), or floating-point (`3.14`, `-0.5`).
* **Identifiers (`IDENT`):** Any non-numeric token without a trailing colon that is not a keyword.

## 2. Type System (Monomorphic v0.3)

IR4th v0.3 defines **three stack types** on a **single unified stack** and **no operator polymorphism**. Nothing smaller than a native machine word `W:` exists on the stack; byte- and 16-bit memory accesses (`u8@`, `i8@`, `i8!`, `u16@`, `i16@`, `i16!`) implicitly widen to and truncate from `W:`.

| Type Token | Name | Width & Semantics | Z80 / 6502 | ARM32 / RISC-V32 | ARM64 / x86-64 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`W:`** | Native Word | Target-native signed integer (`isize`) | 2 bytes (`i16`) | 4 bytes (`i32`) | 8 bytes (`i64`) |
| **`A:`** | Address | Target-width memory pointer (`usize`) | 2 bytes (`u16`) | 4 bytes (`u32`) | 8 bytes (`u64`) |
| **`F:`** | Float | Target-native floating-point | **6 bytes** (Z80) / **4 bytes** (6502) | **4 bytes** (IEEE-754 `binary32`) | **8 bytes** (IEEE-754 `binary64`) |

### 2.1 Literal Typing Rules
* **Bare Integer Literals** (`0`, `1`, `-42`, `0x10`, `$FF`) implicitly default to **`W:`**.
* **Explicitly Typed Literals** prefix the value with a `TYPE` token separated by whitespace:
  * `A: 0x0400` — pushes target-width memory address `0x0400` (`A:`)
  * `F: 3.14` — pushes target-native floating-point value `3.14` (`F:`)
* **Literal Width Restriction:** In the current implementation, integer and address literals are restricted to 32-bit values (`-2147483648..2147483647` for `W:`, `0..0xFFFFFFFF` for `A:`), even on 64-bit targets.

## 3. Functions, Parameters & Locals

Functions are defined at the top level using standard Forth `: ... ;` syntax with a Gforth-compatible `{ ... }` signature header.

```forth
: <name> { <inputs> | <locals> -- <outputs> }
  <body>
;
```

* **Inputs (before `|` or `--`):**
  * Declared as `TYPE IDENT` pairs (e.g., `A: ptr W: count`). On entry, arguments are popped from the unified data stack in standard Forth order (rightmost parameter popped from the top of the stack) and bound to their local names. Unnamed input parameters are not permitted.
* **Scratch Locals (optional, after `|` and before `--`):**
  * Declared as `TYPE IDENT` pairs (e.g., `| W: i W: sum F: acc`).
  * Scratch locals do **not** pop from the caller's stack; they allocate local storage slots initialized to `0` (`0.0` for `F:`) on function entry.
* **Outputs (after `--`):**
  * A nameless sequence of `TYPE` tokens specifying the exact stack state required at `;` or `exit` (e.g., `-- W:`, `-- F:`, `-- A: W:`, or `--` for `void`).

### 3.1 Accessing & Mutating Locals
* **Read (`<ident>`):** Pushes the value of parameter or local `<ident>` (of its declared type `T`) onto the stack.
* **Write (`to <ident>`):** Pops the top value from the stack (verifying its type matches `<ident>`'s declared type `T`) and stores it into `<ident>`.

### 3.2 Named Data Buffers (`buffer:`)
Top-level fixed-size writable buffers can be declared outside word definitions:

```forth
<size> buffer: <name>
```

* `<size>` is a positive byte count. Each buffer is allocated in the target's writable data segment, aligned to the target's native word size.
* Referencing `<name>` inside a word pushes its base address onto the stack with type **`A:`** (`( -- A: )`).

### 3.2b Pre-Initialized Data (`create`, `i8,`, `i16,`, `,`, `align`)
Top-level read-only tables and constants can be defined outside word definitions:

```forth
create <name>
  10 i8, -5 i8, 1000 i16,
align
create words_tbl
  100 , -200 ,
```

* `create <name>` aligns the data pointer to the target's native word size and binds `<name>` (`( -- A: )`).
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
  2. `<dir>/<cpu>/<filename>` (e.g. `arm64/io.ir4`, `z80/math.ir4`, or `gforth/io.ir4` under `ir4th.fs`)
  3. `<dir>/<filename>` (portable fallback)
* Repeated `require` directives for an already-loaded file path are ignored.

### 3.4 External Functions (`extern`)
Top-level `extern` declares an external platform/OS function:

```forth
extern putchar { W: c -- W: }                  \ ARM64 JIT (dlsym, C ABI)
extern bdos 0x0005 { W: C W: DE -- W: A }      \ Z80 CP/M (fixed addr + registers)
```

* Signatures use `{ inputs -- outputs }` without `|` locals and with at most 1 return value.
* On ARM64 JIT (`--run`), no address is given; symbols are resolved via `dlsym` and called using the ARM64 C ABI (`X0..X7`, `D0..D7`).
* On RISC-V RV32 (`--obj`), symbolic `extern` calls emit ELF relocations (`R_RISCV_HI20` / `R_RISCV_LO12_I`) using the `ilp32` C ABI (`a0..a7`); fixed-address `extern` declarations are also supported for bare-metal targets (`--virt`).
* On 16-bit targets (Z80 / 6502), a fixed 16-bit address is required and parameter/return names specify CPU registers (`A, B, C, D, E, H, L, BC, DE, HL` on Z80; `A, X, Y` on 6502).

## 4. Core Instruction Set (Non-Polymorphic)

In stack-effect notation `( before -- after )`, the rightmost item is the top of the stack. All types—including `F:`—reside on the single unified stack.

### 4.1 Stack Manipulation
*(Stack primitives move existing typed slots without altering their types).*

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `dup` | `( t -- t t )` | Duplicate top stack item |
| `drop` | `( t -- )` | Discard top stack item |
| `swap` | `( t1 t2 -- t2 t1 )` | Exchange top two stack items |
| `over` | `( t1 t2 -- t1 t2 t1 )` | Copy second stack item to top |

### 4.2 Word Arithmetic & Bitwise Operations (`W:`)

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `+` | `( W: W: -- W: )` | Word addition |
| `-` | `( W: W: -- W: )` | Word subtraction (`a - b`) |
| `*` | `( W: W: -- W: )` | Word multiplication |
| `/` | `( W: W: -- W: )` | Signed word division (`a / b`) |
| `mod` | `( W: W: -- W: )` | Signed word remainder (`a mod b`) |
| `u/` | `( W: W: -- W: )` | Unsigned word division |
| `umod` | `( W: W: -- W: )` | Unsigned word remainder |
| `and` | `( W: W: -- W: )` | Bitwise AND |
| `or` | `( W: W: -- W: )` | Bitwise OR |
| `xor` | `( W: W: -- W: )` | Bitwise XOR |
| `invert` | `( W: -- W: )` | Bitwise one's complement |
| `lshift` | `( W: W: -- W: )` | Logical shift left (`a << b`) |
| `arshift` | `( W: W: -- W: )` | Arithmetic (sign-preserving) shift right (`a >> b`) |
| `rshift` | `( W: W: -- W: )` | Logical (zero-filling) shift right (`a u>> b`) |

### 4.3 Address Arithmetic (`A:`)
Because `W:` and `A:` always share the same register width, address offsets require no sign-extension.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `a+` | `( A: W: -- A: )` | Add byte offset `W:` to address `A:` |
| `a-` | `( A: W: -- A: )` | Subtract byte offset `W:` from address `A:` |
| `adiff` | `( A: A: -- W: )` | Signed byte distance between two addresses (`a1 - a2`) |

### 4.4 Floating-Point Operations (`F:`)

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `f+` | `( F: F: -- F: )` | Floating-point addition |
| `f-` | `( F: F: -- F: )` | Floating-point subtraction (`a - b`) |
| `f*` | `( F: F: -- F: )` | Floating-point multiplication |
| `f/` | `( F: F: -- F: )` | Floating-point division (`a / b`) |
| `fnegate` | `( F: -- F: )` | Floating-point negation (`-a`) |

### 4.5 Comparisons (`W:`, `A:`, and `F:`)
All comparisons consume two values and push a boolean flag of type **`W:`** (`-1` for true, `0` for false).

| Instruction | Stack Effect | Semantics |
| :--- | :--- | :--- |
| `=` , `<>` | `( W: W: -- W: )` | Word equal / not equal |
| `<` , `>` , `<=` , `>=` | `( W: W: -- W: )` | Signed word comparisons |
| `u<` , `u>` , `u<=` , `u>=` | `( W: W: -- W: )` | Unsigned word comparisons |
| `a=` , `a<>` , `au<` , `au>` | `( A: A: -- W: )` | Address equality and unsigned pointer comparisons |
| `f=` , `f<>` , `f<` , `f>` | `( F: F: -- W: )` | Floating-point comparisons |

### 4.6 Memory Load & Store (Standard Forth Operators)
Fixed-width memory operations (`u8@`, `i8@`, `i8!`, `u16@`, `i16@`, `i16!`) widen to `W:` on load and truncate the low bits of `W:` on store.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `i8@` | `( A: -- W: )` | Fetch signed 8-bit byte from address, sign-extended to `W:` |
| `u8@` | `( A: -- W: )` | Fetch unsigned 8-bit byte from address, zero-extended to `W:` |
| `i8!` | `( W: A: -- )` | Store low 8 bits of `W:` to address |
| `i16@` | `( A: -- W: )` | Fetch signed 16-bit integer from address, sign-extended to `W:` |
| `u16@` | `( A: -- W: )` | Fetch unsigned 16-bit integer from address, zero-extended to `W:` |
| `i16!` | `( W: A: -- )` | Store low 16 bits of `W:` to address |
| `@` | `( A: -- W: )` | Fetch native word (`W:`) from address |
| `!` | `( W: A: -- )` | Store native word (`W:`) to address |
| `a@` | `( A: -- A: )` | Fetch target-width pointer (`A:`) from address |
| `a!` | `( A: A: -- )` | Store target-width pointer (`A:`) to address |
| `f@` | `( A: -- F: )` | Fetch target-width float (`F:`) from address |
| `f!` | `( F: A: -- )` | Store target-width float (`F:`) to address |

### 4.7 Explicit Type Conversions

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `sext` | `( W: -- W: )` | Sign-extend low 8 bits of `W:` to full word width `W:` |
| `w>a` | `( W: -- A: )` | Reinterpret native word `W:` as address `A:` |
| `a>w` | `( A: -- W: )` | Reinterpret address `A:` as native word `W:` |
| `w>f` | `( W: -- F: )` | Convert signed native word `W:` to float `F:` |
| `f>w` | `( F: -- W: )` | Truncate float `F:` to signed native word `W:` |

## 5. Control Flow & Static Stack Invariants

* **Conditional Branches (`<cond:W:> if ... else ... then`):** Pops a `W:` flag. Both branches must leave the type stack with identical depth and types. If `else` is omitted, the true branch must leave the stack completely even (unchanged).
* **Pre-Tested Loops (`begin <cond:W:> while ... repeat`):** `while` pops a `W:` flag. The combined condition and loop body must leave the type stack completely even at `repeat`.
* **Post-Tested Loops (`begin ... <cond:W:> until`):** `until` pops a `W:` flag. The loop body must leave the type stack completely even before the condition is pushed.
* **Calls & Early Return (`<word_name>`, `exit`):** Validates caller stack against callee inputs and pushes outputs. `exit` verifies the stack matches the word's declared output types after `--`.

## 6. Gforth Compatibility Prelude (`ir4th.fs`)

```forth
\ ir4th.fs - Gforth compatibility prelude for IR4th v0.3
get-order wordlist dup >r set-current
: A: postpone W: ; immediate
set-order r> >order

: a+ + ;
: a- - ;
: adiff - ;
: a= = ;
: a<> <> ;
: au< u< ;
: au> u> ;
: a@ @ ;
: a! ! ;
: sext $FF and dup $80 and if -256 or then ;
: w>a ;
: a>w ;
: buffer: ( n "name" -- ) create align allot ;
: i8, ( n -- ) c, ;
: i16, ( n -- ) dup c, 8 rshift c, ;

\ Bridge Gforth's separate F-stack to IR4th's unified stack (32-bit IEEE float)
fvariable ir4-ftmp
: ir4-fpop ( w -- F: r ) ir4-ftmp ! ir4-ftmp sf@ ;
: ir4-fpush ( F: r -- w ) 0 ir4-ftmp ! ir4-ftmp sf! ir4-ftmp @ ;
: ir4-f2pop ( w1 w2 -- F: r1 r2 ) >r ir4-fpop r> ir4-fpop ;
: F: parse-name >float 0= abort" invalid float literal" ir4-fpush state @ if postpone literal then ; immediate
' f+ constant ir4-gf-f+   : f+ ir4-f2pop ir4-gf-f+ execute ir4-fpush ;
' f- constant ir4-gf-f-   : f- ir4-f2pop ir4-gf-f- execute ir4-fpush ;
' f* constant ir4-gf-f*   : f* ir4-f2pop ir4-gf-f* execute ir4-fpush ;
' f/ constant ir4-gf-f/   : f/ ir4-f2pop ir4-gf-f/ execute ir4-fpush ;
' fnegate constant ir4-gf-fnegate : fnegate ir4-fpop ir4-gf-fnegate execute ir4-fpush ;
' f= constant ir4-gf-f=   : f= ir4-f2pop ir4-gf-f= execute ;
' f<> constant ir4-gf-f<> : f<> ir4-f2pop ir4-gf-f<> execute ;
' f< constant ir4-gf-f<   : f< ir4-f2pop ir4-gf-f< execute ;
' f> constant ir4-gf-f>   : f> ir4-f2pop ir4-gf-f> execute ;
: w>f s>f ir4-fpush ;
: f>w ir4-fpop f>s ;
: f! ( w addr -- ) swap ir4-ftmp ! ir4-ftmp swap 4 move ;
: f@ ( addr -- w ) 0 ir4-ftmp ! ir4-ftmp 4 move ir4-ftmp @ ;
```

## 7. Complete Example Program

```forth
\ Fill a memory buffer with a byte value and return the end address
: memset { A: dst W: val W: len | W: i -- A: }
  0 to i
  begin
    i len u<
  while
    val dst i a+ i8!
    i 1 + to i
  repeat
  dst len a+
;

\ Sum an array of signed 8-bit bytes into a native word
: sum_signed_bytes { A: ptr W: count | W: i W: acc -- W: }
  0 to i
  0 to acc
  begin
    i count <
  while
    ptr i a+ i8@
    acc + to acc
    i 1 + to i
  repeat
  acc
;
```
