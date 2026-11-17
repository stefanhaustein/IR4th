; 6502 32-bit integer arithmetic & shift runtime for IR4th.
; Preserves X (IR4th data stack pointer). Uses ZP $40..$51.

A0   = $40
A1   = $41
A2   = $42
A3   = $43
B0   = $44
B1   = $45
B2   = $46
B3   = $47
R0   = $48
R1   = $49
R2   = $4A
R3   = $4B
T0   = $4C
T1   = $4D
T2   = $4E
SGN  = $4F
QSGN = $50

    .code
    JMP MUL32
    JMP UDIVMOD32
    JMP SDIVMOD32
    JMP SHL32
    JMP SHR32
    JMP ASHR32

; A = A * B (32-bit unsigned/signed)
MUL32:
    LDA #0
    STA R0
    STA R1
    STA R2
    STA R3
    LDY #32
@loop:
    LSR B3
    ROR B2
    ROR B1
    ROR B0
    BCC @noadd
    CLC
    LDA R0
    ADC A0
    STA R0
    LDA R1
    ADC A1
    STA R1
    LDA R2
    ADC A2
    STA R2
    LDA R3
    ADC A3
    STA R3
@noadd:
    ASL A0
    ROL A1
    ROL A2
    ROL A3
    DEY
    BNE @loop
    LDA R0
    STA A0
    LDA R1
    STA A1
    LDA R2
    STA A2
    LDA R3
    STA A3
    RTS

; Unsigned A / B -> quotient in A, remainder in R
UDIVMOD32:
    LDA #0
    STA R0
    STA R1
    STA R2
    STA R3
    LDY #32
@loop:
    ASL A0
    ROL A1
    ROL A2
    ROL A3
    ROL R0
    ROL R1
    ROL R2
    ROL R3
    SEC
    LDA R0
    SBC B0
    STA T0
    LDA R1
    SBC B1
    STA T1
    LDA R2
    SBC B2
    STA T2
    LDA R3
    SBC B3
    BCC @skip
    STA R3
    LDA T2
    STA R2
    LDA T1
    STA R1
    LDA T0
    STA R0
    INC A0
@skip:
    DEY
    BNE @loop
    RTS

NEG_A:
    SEC
    LDA #0
    SBC A0
    STA A0
    LDA #0
    SBC A1
    STA A1
    LDA #0
    SBC A2
    STA A2
    LDA #0
    SBC A3
    STA A3
    RTS

NEG_B:
    SEC
    LDA #0
    SBC B0
    STA B0
    LDA #0
    SBC B1
    STA B1
    LDA #0
    SBC B2
    STA B2
    LDA #0
    SBC B3
    STA B3
    RTS

NEG_R:
    SEC
    LDA #0
    SBC R0
    STA R0
    LDA #0
    SBC R1
    STA R1
    LDA #0
    SBC R2
    STA R2
    LDA #0
    SBC R3
    STA R3
    RTS

; Signed A / B -> quotient in A, remainder in R (truncating toward 0)
SDIVMOD32:
    LDA A3
    STA SGN
    EOR B3
    STA QSGN
    LDA A3
    BPL @chk_b
    JSR NEG_A
@chk_b:
    LDA B3
    BPL @do_div
    JSR NEG_B
@do_div:
    JSR UDIVMOD32
    LDA QSGN
    BPL @chk_rem
    JSR NEG_A
@chk_rem:
    LDA SGN
    BPL @done
    JSR NEG_R
@done:
    RTS

; A = A << (B0 & 31)
SHL32:
    LDA B0
    AND #31
    BEQ @done
    TAY
@loop:
    ASL A0
    ROL A1
    ROL A2
    ROL A3
    DEY
    BNE @loop
@done:
    RTS

; A = (u32)A >> (B0 & 31)
SHR32:
    LDA B0
    AND #31
    BEQ @done
    TAY
@loop:
    LSR A3
    ROR A2
    ROR A1
    ROR A0
    DEY
    BNE @loop
@done:
    RTS

; A = (i32)A >> (B0 & 31)
ASHR32:
    LDA B0
    AND #31
    BEQ @done
    TAY
@loop:
    LDA A3
    ASL A
    ROR A3
    ROR A2
    ROR A1
    ROR A0
    DEY
    BNE @loop
@done:
    RTS
