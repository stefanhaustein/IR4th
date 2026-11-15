# IR4th Language Specification (v0.1)

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, concatenative intermediate language designed for retro-computing CPUs (such as the Z80 and 6502) and constrained embedded systems.

It combines Forth's low-overhead stack execution and Gforth-style local variables with compile-time static type checking, explicit bit-width types, and single-pass direct machine-code generation.

---

## 1. Lexical Structure

### 1.1 Tokenization & Whitespace
* Programs are sequences of **whitespace-delimited tokens** (spaces, tabs, newlines).
* Line comments begin with a backslash `\` (followed by whitespace or end-of-line) and extend to the end of the current line.

### 1.2 Lexical Categories
* **Type Tokens (`TYPE`):** Any token ending with a trailing colon (`:`) is unconditionally a type specifier (`C:`, `W:`, `D:`, `A:`, `F:`).
* **Keywords:** Reserved control-flow and declaration words (`fn`, `end`, `to`, `if`, `else`, `then`, `begin`, `while`, `repeat`, `until`, `return`, `{`, `}`, `|`, `--`).
* **Numeric Literals:** Decimal (`42`, `-10`), hexadecimal (`0xFF`, `0x4000`), or floating-point (`3.14`).
* **Identifiers (`IDENT`):** Any non-numeric token without a trailing colon that is not a keyword.

---

## 2. Type System

IR4th uses a static, unified type stack. All primitive types have fixed, target-defined widths.

| Type Token | Name | Semantics | 8/16-bit Target (Z80, 6502) | 32-bit Target (ARM, RISC-V) |
| :--- | :--- | :--- | :--- | :--- |
| **`C:`** | Char / Byte | Signed 8-bit integer | 1 byte (`i8`) | 1 byte (`i8`) |
| **`W:`** | Native Word | Signed native machine word | 2 bytes (`i16`) | 4 bytes (`i32`) |
| **`D:`** | Double Word | Signed double-width integer | 4 bytes (`i32`) | 8 bytes (`i64`) |
| **`A:`** | Address | Memory pointer | 2 bytes (`u16`) | 4 bytes (`u32`) |
| **`F:`** | Float *(optional)* | IEEE-754 single-precision float | 4 bytes (`f32`) | 4 bytes (`f32`) |

### 2.1 Literal Typing Rules
* **Bare Integer Literals** (`0`, `1`, `-42`, `0x10`) implicitly default to type **`W:`**.
* **Explicitly Typed Literals** prefix the value with a `TYPE` token separated by whitespace:
  * `C: 65` (pushes 8-bit char `65`)
  * `D: 100000` (pushes 32-bit double word `100000`)
  * `A: 0x0400` (pushes memory address `0x0400`)
  * `F: 3.14` (pushes 32-bit float `3.14`)

---

## 3. Functions, Parameters & Locals

Every function is defined at the top level using `fn ... end` and a Gforth-inspired `{ ... }` signature header.

### 3.1 Signature Syntax
```forth
fn <name> { <inputs> | <locals> -- <outputs> }
  <body>
end
```

* **Inputs (before `|` or `--`):**
  * **Named parameters:** `TYPE IDENT` pairs (e.g., `A: ptr W: count`). On entry, arguments are popped from the data stack in standard Forth order (rightmost parameter popped from the top of the stack) and bound to their respective local names.
  * **Unnamed parameters:** Standalone `TYPE` tokens (e.g., `W: W:`). Arguments remain on the evaluation stack.
* **Scratch Locals (optional, after `|` and before `--`):**
  * Declared as `TYPE IDENT` pairs (e.g., `| W: i W: sum`).
  * Scratch locals do **not** pop values from the caller's stack; they allocate local storage slots initialized to `0` on function entry.
* **Outputs (after `--`):**
  * A nameless sequence of `TYPE` tokens specifying the exact stack state required upon reaching `end` or `return` (e.g., `-- W:`, `-- A: W:`, or `--` for `void`).

### 3.2 Accessing & Mutating Locals
* **Read (`<ident>`):** Pushes the current value of parameter or local `<ident>` (of its declared type `T`) onto the stack.
* **Write (`to <ident>`):** Pops the top value from the stack (verifying its type matches `<ident>`'s declared type `T`) and stores it into `<ident>`.

---

## 4. Core Instruction Set

In the stack-effect notation `( before -- after )`, `T` denotes any matching primitive type (`C:`, `W:`, `D:`, `A:`, `F:`), and the rightmost item is the top of the stack.

### 4.1 Stack Manipulation

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `dup` | `( T -- T T )` | Duplicate top stack item |
| `drop` | `( T -- )` | Discard top stack item |
| `swap` | `( T1 T2 -- T2 T1 )` | Exchange top two stack items |
| `over` | `( T1 T2 -- T1 T2 T1 )` | Copy second stack item to top |
| `rot` | `( T1 T2 T3 -- T2 T3 T1 )` | Rotate third item to top |

### 4.2 Arithmetic & Bitwise Operations
Standard arithmetic and right-shift operators treat integers as **signed** (two's complement). Unsigned variants carry a `u` prefix.

| Instruction | Stack Effect | Type Rules & Description |
| :--- | :--- | :--- |
| `+` | `( T T -- T )` | Addition. Special rule: `( A: W: -- A: )` allowed for pointer offsets. |
| `-` | `( T T -- T )` | Subtraction. Special rule: `( A: W: -- A: )` allowed for pointer offsets. |
| `*` | `( T T -- T )` | Multiplication (`T` in `{C:, W:, D:, F:}`). |
| `/` | `( T T -- T )` | Signed division (`T` in `{C:, W:, D:, F:}`). |
| `mod` | `( T T -- T )` | Signed remainder (`T` in `{C:, W:, D:}`). |
| `u/` | `( T T -- T )` | Unsigned division (`T` in `{C:, W:, D:}`). |
| `umod` | `( T T -- T )` | Unsigned remainder (`T` in `{C:, W:, D:}`). |
| `and` | `( T T -- T )` | Bitwise AND (`T` in `{C:, W:, D:}`). |
| `or` | `( T T -- T )` | Bitwise OR (`T` in `{C:, W:, D:}`). |
| `xor` | `( T T -- T )` | Bitwise XOR (`T` in `{C:, W:, D:}`). |
| `not` | `( T -- T )` | Bitwise one's complement (`T` in `{C:, W:, D:}`). |
| `<<` | `( T T -- T )` | Logical shift left (`T` in `{C:, W:, D:}`). |
| `>>` | `( T T -- T )` | Arithmetic (sign-preserving) shift right (`T` in `{C:, W:, D:}`). |
| `u>>` | `( T T -- T )` | Logical (zero-filling) shift right (`T` in `{C:, W:, D:}`). |

### 4.3 Comparisons
All comparisons consume two values of identical type `T` and push a boolean flag of type **`W:`** (`1` for true, `0` for false).

| Instruction | Stack Effect | Semantics |
| :--- | :--- | :--- |
| `=` | `( T T -- W: )` | Equal |
| `!=` | `( T T -- W: )` | Not equal |
| `<` , `>` , `<=` , `>=` | `( T T -- W: )` | Signed less-than, greater-than, less-or-equal, greater-or-equal |
| `u<` , `u>` , `u<=` , `u>=` | `( T T -- W: )` | Unsigned less-than, greater-than, less-or-equal, greater-or-equal |

### 4.4 Memory Load & Store
Memory operations take an address of type `A:` on top of the stack.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `load.c` | `( A: -- C: )` | Load 8-bit byte from address |
| `load.w` | `( A: -- W: )` | Load native word from address |
| `load.d` | `( A: -- D: )` | Load double-width integer from address |
| `load.a` | `( A: -- A: )` | Load pointer address from address |
| `store.c` | `( C: A: -- )` | Store 8-bit byte to address |
| `store.w` | `( W: A: -- )` | Store native word to address |
| `store.d` | `( D: A: -- )` | Store double-width integer to address |
| `store.a` | `( A: A: -- )` | Store pointer address to address |

### 4.5 Type Conversions

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `to.c` | `( T -- C: )` | Truncate to 8-bit byte |
| `to.w` | `( T -- W: )` | Zero-extend (from `C:`) or truncate (from `D:`, `A:`) to native word |
| `to.d` | `( T -- D: )` | Zero-extend to double word |
| `to.a` | `( T -- A: )` | Zero-extend/truncate integer (`W:`, `D:`) to address pointer |
| `sext.w` | `( C: -- W: )` | Sign-extend 8-bit `C:` to native word `W:` |
| `sext.d` | `( T -- D: )` | Sign-extend `C:` or `W:` to double word `D:` |

---

## 5. Control Flow & Static Stack Invariants

IR4th enforces strict compile-time stack-balance rules across all control-flow structures so stack depth is statically known at every program point.

### 5.1 Conditional Branches (`if ... else ... then`)
```forth
<cond:W:> if
  <true-branch>
else              \ optional
  <false-branch>
then
```
* `if` pops a `W:` condition flag. If zero, execution jumps to `else` (or `then`).
* **Stack Invariant:** Both `<true-branch>` and `<false-branch>` must leave the type stack with the **exact same depth and types**. If `else` is omitted, `<true-branch>` must leave the stack completely unchanged ("even").

### 5.2 Pre-Tested Loops (`begin ... while ... repeat`)
```forth
begin
  <cond:W:>
while
  <loop-body>
repeat
```
* `while` pops a `W:` condition flag. If zero, execution exits past `repeat`.
* **Stack Invariant:** The combined `<cond>` + `<loop-body>` sequence must leave the type stack **completely even** (identical depth and types at `repeat` as at `begin`).

### 5.3 Post-Tested Loops (`begin ... until`)
```forth
begin
  <loop-body>
  <cond:W:>
until
```
* `until` pops a `W:` condition flag. If zero (false), execution jumps back to `begin`.
* **Stack Invariant:** `<loop-body>` must leave the type stack **completely even** before `<cond:W:>` is pushed.

### 5.4 Function Calls & Returns
* **`<fn_name>`:** Pops arguments matching the callee's input types in reverse order and pushes the callee's output types.
* **`return`:** Exits the current function immediately. The type stack at `return` must match the function's declared output signature after `--`.

---

## 6. Complete Example Program

```forth
\ Fill a memory buffer with a constant byte value and return the end address
fn memset { A: dst C: val W: len | W: i -- A: }
  0 to i
  begin
    i len u<
  while
    val dst i + store.c
    i 1 + to i
  repeat
  dst len +
end

\ Sum an array of signed 8-bit bytes into a native word
fn sum_signed_bytes { A: ptr W: count | W: i W: acc -- W: }
  0 to i
  0 to acc
  begin
    i count <
  while
    ptr i + load.c sext.w
    acc + to acc
    i 1 + to i
  repeat
  acc
end
```
