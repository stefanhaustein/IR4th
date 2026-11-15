# IR4th Language Specification (v0.1)

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, monomorphic concatenative intermediate language designed for retro-computing CPUs (such as the Z80 and 6502) and constrained embedded systems.

IR4th is designed so that word-only programs are directly executable under **Gforth** (with a minimal compatibility prelude) while compiling via single-pass direct machine-code generation to native Z80 and 6502 binaries.

---

## 1. Lexical Structure

### 1.1 Tokenization & Whitespace
* Programs consist of **whitespace-delimited tokens** (spaces, tabs, newlines).
* Line comments start with `\` (followed by whitespace or end-of-line) and extend to the end of the line.

### 1.2 Lexical Categories
* **Type Tokens (`TYPE`):** Any token ending with a trailing colon (`:`) is strictly a type specifier (`C:`, `W:`, `A:`, `F:`).
* **Keywords:** Reserved syntax and control-flow tokens (`:`, `;`, `to`, `exit`, `if`, `else`, `then`, `begin`, `while`, `repeat`, `until`, `{`, `}`, `|`, `--`).
* **Numeric Literals:** Decimal (`0`, `42`, `-10`), hexadecimal (`0xFF`, `$4000`), or floating-point (`3.14`, `-0.5`).
* **Identifiers (`IDENT`):** Any non-numeric token without a trailing colon that is not a keyword.

---

## 2. Type System (Monomorphic v0.1)

IR4th v0.1 defines **four primitive types** on a **single unified stack** and **no operator polymorphism**. Integer widths (`C:` and `W:`) are strictly fixed across all architectures, while `A:` and `F:` match the target platform's native pointer and floating-point formats.

| Type Token | Name | Width & Semantics | Z80 | 6502 | ARM / RISC-V / x86-64 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`C:`** | Char / Byte | Fixed 8-bit integer (`i8`/`u8`) | 1 byte | 1 byte | 1 byte |
| **`W:`** | Word | **Fixed 16-bit integer** (`i16`/`u16`) | 2 bytes | 2 bytes | 2 bytes |
| **`A:`** | Address | **Target-width memory pointer** (`usize`) | 2 bytes | 2 bytes | 4 or 8 bytes |
| **`F:`** | Float | **Target-native floating-point** | **6 bytes** (48-bit) | **5 bytes** (40-bit MBF) | **4 or 8 bytes** (IEEE-754) |

### 2.1 Literal Typing Rules
* **Bare Integer Literals** (`0`, `1`, `-42`, `0x10`) implicitly default to **`W:`** (16-bit word).
* **Explicitly Typed Literals** prefix the value with a `TYPE` token separated by whitespace:
  * `C: 65` — pushes 8-bit byte `65` (`C:`)
  * `A: 0x0400` — pushes target-width memory address `0x0400` (`A:`)
  * `F: 3.14` — pushes target-native floating-point value `3.14` (`F:`)

---

## 3. Functions, Parameters & Locals

Functions are defined at the top level using standard Forth `: ... ;` syntax with a Gforth-compatible `{ ... }` signature header.

### 3.1 Signature Syntax
```forth
: <name> { <inputs> | <locals> -- <outputs> }
  <body>
;
```

* **Inputs (before `|` or `--`):**
  * **Named parameters:** `TYPE IDENT` pairs (e.g., `A: ptr W: count`). On entry, arguments are popped from the unified data stack in standard Forth order (rightmost parameter popped from the top of the stack) and bound to their local names.
  * **Unnamed parameters:** Standalone `TYPE` tokens (e.g., `W: W:`). Arguments remain on the evaluation stack.
* **Scratch Locals (optional, after `|` and before `--`):**
  * Declared as `TYPE IDENT` pairs (e.g., `| W: i W: sum F: acc`).
  * Scratch locals do **not** pop from the caller's stack; they allocate local storage slots initialized to `0` (`0.0` for `F:`) on function entry.
* **Outputs (after `--`):**
  * A nameless sequence of `TYPE` tokens specifying the exact stack state required at `;` or `exit` (e.g., `-- W:`, `-- F:`, `-- A: W:`, or `--` for `void`).

### 3.2 Accessing & Mutating Locals
* **Read (`<ident>`):** Pushes the value of parameter or local `<ident>` (of its declared type `T`) onto the stack.
* **Write (`to <ident>`):** Pops the top value from the stack (verifying its type matches `<ident>`'s declared type `T`) and stores it into `<ident>`.

---

## 4. Core Instruction Set (Non-Polymorphic)

In stack-effect notation `( before -- after )`, the rightmost item is the top of the stack. All types—including `F:`—reside on the single unified stack.

### 4.1 Stack Manipulation
*(Note: Stack primitives move existing typed slots without altering their types).*

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `dup` | `( t -- t t )` | Duplicate top stack item |
| `drop` | `( t -- )` | Discard top stack item |
| `swap` | `( t1 t2 -- t2 t1 )` | Exchange top two stack items |
| `over` | `( t1 t2 -- t1 t2 t1 )` | Copy second stack item to top |
| `rot` | `( t1 t2 t3 -- t2 t3 t1 )` | Rotate third item to top |

### 4.2 Word Arithmetic & Bitwise Operations (`W:`)
All standard Forth math and bitwise operators operate strictly on **fixed 16-bit words (`W:`)** with two's complement wraparound semantics.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `+` | `( W: W: -- W: )` | 16-bit word addition |
| `-` | `( W: W: -- W: )` | 16-bit word subtraction (`a - b`) |
| `*` | `( W: W: -- W: )` | 16-bit word multiplication (low 16 bits) |
| `/` | `( W: W: -- W: )` | Signed 16-bit word division (`a / b`) |
| `mod` | `( W: W: -- W: )` | Signed 16-bit word remainder (`a mod b`) |
| `u/` | `( W: W: -- W: )` | Unsigned 16-bit word division |
| `umod` | `( W: W: -- W: )` | Unsigned 16-bit word remainder |
| `and` | `( W: W: -- W: )` | 16-bit bitwise AND |
| `or` | `( W: W: -- W: )` | 16-bit bitwise OR |
| `xor` | `( W: W: -- W: )` | 16-bit bitwise XOR |
| `invert` | `( W: -- W: )` | 16-bit bitwise one's complement |
| `lshift` | `( W: W: -- W: )` | 16-bit logical shift left (`a << b`) |
| `arshift` | `( W: W: -- W: )` | 16-bit arithmetic (sign-preserving) shift right (`a >> b`) |
| `rshift` | `( W: W: -- W: )` | 16-bit logical (zero-filling) shift right (`a u>> b`) |

### 4.3 Address Arithmetic (`A:`)
Pointer offset math uses dedicated address operators. On 32-bit and 64-bit targets, `a+` and `a-` sign-extend the 16-bit `W:` offset to the width of `A:` before adding or subtracting.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `a+` | `( A: W: -- A: )` | Add signed 16-bit byte offset `W:` to address `A:` |
| `a-` | `( A: W: -- A: )` | Subtract signed 16-bit byte offset `W:` from address `A:` |
| `adiff` | `( A: A: -- W: )` | Byte distance between two addresses (`a1 - a2`), truncated to 16-bit `W:` |

### 4.4 Floating-Point Operations (`F:`)
All floating-point operations operate on platform-native `F:` values on the unified stack.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `f+` | `( F: F: -- F: )` | Floating-point addition |
| `f-` | `( F: F: -- F: )` | Floating-point subtraction (`a - b`) |
| `f*` | `( F: F: -- F: )` | Floating-point multiplication |
| `f/` | `( F: F: -- F: )` | Floating-point division (`a / b`) |
| `fnegate` | `( F: -- F: )` | Floating-point negation (`-a`) |

### 4.5 Comparisons (`W:`, `A:`, and `F:`)
All comparisons consume two values and push a boolean flag of type **`W:`** (`-1` (`0xFFFF`) or non-zero for true, `0` for false).

| Instruction | Stack Effect | Semantics |
| :--- | :--- | :--- |
| `=` , `<>` | `( W: W: -- W: )` | 16-bit word equal / not equal |
| `<` , `>` , `<=` , `>=` | `( W: W: -- W: )` | Signed 16-bit word comparisons |
| `u<` , `u>` , `u<=` , `u>=` | `( W: W: -- W: )` | Unsigned 16-bit word comparisons |
| `a=` , `a<>` , `au<` , `au>` | `( A: A: -- W: )` | Address equality and unsigned pointer comparisons |
| `f=` , `f<>` , `f<` , `f>` | `( F: F: -- W: )` | Floating-point comparisons |

### 4.6 Memory Load & Store (Standard Forth Operators)
Memory operations take a target-width address of type `A:` on top of the stack.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `c@` | `( A: -- C: )` | Fetch 8-bit byte from address |
| `c!` | `( C: A: -- )` | Store 8-bit byte to address |
| `@` | `( A: -- W: )` | Fetch 16-bit word (`W:`) from address |
| `!` | `( W: A: -- )` | Store 16-bit word (`W:`) to address |
| `a@` | `( A: -- A: )` | Fetch target-width pointer (`A:`) from address |
| `a!` | `( A: A: -- )` | Store target-width pointer (`A:`) to address |
| `f@` | `( A: -- F: )` | Fetch target-width float (`F:`) from address |
| `f!` | `( F: A: -- )` | Store target-width float (`F:`) to address |

### 4.7 Explicit Type Conversions

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `c>w` | `( C: -- W: )` | Zero-extend 8-bit `C:` to 16-bit `W:` |
| `sext` | `( C: -- W: )` | Sign-extend 8-bit `C:` to 16-bit `W:` |
| `w>c` | `( W: -- C: )` | Truncate 16-bit `W:` to low 8 bits `C:` |
| `w>a` | `( W: -- A: )` | Zero-extend 16-bit `W:` to target address width `A:` |
| `a>w` | `( A: -- W: )` | Truncate target address `A:` to low 16 bits `W:` |
| `w>f` | `( W: -- F: )` | Convert signed 16-bit word `W:` to float `F:` |
| `f>w` | `( F: -- W: )` | Truncate float `F:` to signed 16-bit word `W:` |

---

## 5. Control Flow & Static Stack Invariants

### 5.1 Conditional Branches (`if ... else ... then`)
```forth
<cond:W:> if
  <true-branch>
else              \ optional
  <false-branch>
then
```
* `if` pops a `W:` flag. If zero, execution jumps to `else` (or `then`).
* **Stack Invariant:** Both branches must leave the type stack with identical depth and types. If `else` is omitted, `<true-branch>` must leave the stack completely even (unchanged).

### 5.2 Pre-Tested Loops (`begin ... while ... repeat`)
```forth
begin
  <cond:W:>
while
  <loop-body>
repeat
```
* `while` pops a `W:` flag. If zero, execution exits past `repeat`.
* **Stack Invariant:** `<cond>` + `<loop-body>` must leave the type stack completely even at `repeat`.

### 5.3 Post-Tested Loops (`begin ... until`)
```forth
begin
  <loop-body>
  <cond:W:>
until
```
* `until` pops a `W:` flag. If zero (false), execution jumps back to `begin`.
* **Stack Invariant:** `<loop-body>` must leave the type stack completely even before `<cond:W:>` is pushed.

### 5.4 Calls & Early Return
* **`<word_name>`:** Validates argument types against the callee's inputs and pushes its outputs.
* **`exit`:** Exits the current word immediately. The type stack at `exit` must match the word's declared output types after `--`.

---

## 6. Gforth Compatibility Prelude (`ir4th.fs`)

Loading this prelude allows valid IR4th v0.1 files to execute directly under Gforth (including bridging Gforth's separate F-stack to IR4th's unified stack):

```forth
\ ir4th.fs - Gforth compatibility prelude for IR4th v0.1
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
: c>w $FF and ;
: sext $FF and dup $80 and if $FF00 or then ;
: w>c $FF and ;
: w>a $FFFF and ;
: a>w $FFFF and ;

\ Bridge Gforth's separate F-stack to IR4th's unified stack via scratch float slot
fvariable ir4-ftmp
: F: parse-name >float drop ir4-ftmp f! ir4-ftmp @ ;
: f+ >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ f+ ir4-ftmp f! ir4-ftmp @ ;
: f- >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ fswap f- ir4-ftmp f! ir4-ftmp @ ;
: f* >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ f* ir4-ftmp f! ir4-ftmp @ ;
: f/ >r ir4-ftmp ! ir4-ftmp f@ r> ir4-ftmp ! ir4-ftmp f@ fswap f/ ir4-ftmp f! ir4-ftmp @ ;
: w>f s>f ir4-ftmp f! ir4-ftmp @ ;
: f>w ir4-ftmp ! ir4-ftmp f@ f>s ;
```

---

## 7. Complete Example Program

```forth
\ Fill a memory buffer with a byte value and return the end address
: memset { A: dst C: val W: len | W: i -- A: }
  0 to i
  begin
    i len u<
  while
    val dst i a+ c!
    i 1 + to i
  repeat
  dst len a+
;

\ Sum an array of signed 8-bit bytes into a 16-bit word
: sum_signed_bytes { A: ptr W: count | W: i W: acc -- W: }
  0 to i
  0 to acc
  begin
    i count <
  while
    ptr i a+ c@ sext
    acc + to acc
    i 1 + to i
  repeat
  acc
;
```
