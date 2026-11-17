; 6502 32-bit integer primitives for IR4th (converted via tools/asm2ir4.py).
; Preserves X (IR4th data stack pointer). Uses ZP $02..$10.

A0 = $02
A1 = $03
A2 = $04
A3 = $05
B0 = $06
B1 = $07
B2 = $08
B3 = $09
R0 = $0A
R1 = $0B
R2 = $0C
R3 = $0D
T0 = $0E
T1 = $0F
T2 = $10

; @code i32* ( a :i32 b :i32 -> :i32 )
    LDA STACK+0,X
    STA B0
    LDA STACK+1,X
    STA B1
    LDA STACK+2,X
    STA B2
    LDA STACK+3,X
    STA B3
    INX
    INX
    INX
    INX
    LDA STACK+0,X
    STA A0
    LDA STACK+1,X
    STA A1
    LDA STACK+2,X
    STA A2
    LDA STACK+3,X
    STA A3
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
    STA STACK+0,X
    LDA R1
    STA STACK+1,X
    LDA R2
    STA STACK+2,X
    LDA R3
    STA STACK+3,X
    RTS

; @code u32/ ( a :i32 b :i32 -> :i32 )
    LDA STACK+0,X
    STA B0
    LDA STACK+1,X
    STA B1
    LDA STACK+2,X
    STA B2
    LDA STACK+3,X
    STA B3
    INX
    INX
    INX
    INX
    LDA STACK+0,X
    STA A0
    LDA STACK+1,X
    STA A1
    LDA STACK+2,X
    STA A2
    LDA STACK+3,X
    STA A3
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
    LDA A0
    STA STACK+0,X
    LDA A1
    STA STACK+1,X
    LDA A2
    STA STACK+2,X
    LDA A3
    STA STACK+3,X
    RTS

; @code u32mod ( a :i32 b :i32 -> :i32 )
    LDA STACK+0,X
    STA B0
    LDA STACK+1,X
    STA B1
    LDA STACK+2,X
    STA B2
    LDA STACK+3,X
    STA B3
    INX
    INX
    INX
    INX
    LDA STACK+0,X
    STA A0
    LDA STACK+1,X
    STA A1
    LDA STACK+2,X
    STA A2
    LDA STACK+3,X
    STA A3
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
    LDA R0
    STA STACK+0,X
    LDA R1
    STA STACK+1,X
    LDA R2
    STA STACK+2,X
    LDA R3
    STA STACK+3,X
    RTS

; @code i32<< ( a :i32 cnt :i -> :i32 )
    LDA STACK+0,X
    INX
    INX
    AND #31
    BEQ @done
    TAY
@loop:
    ASL STACK+0,X
    LDA STACK+1,X
    ROL A
    STA STACK+1,X
    LDA STACK+2,X
    ROL A
    STA STACK+2,X
    LDA STACK+3,X
    ROL A
    STA STACK+3,X
    DEY
    BNE @loop
@done:
    RTS

; @code u32>> ( a :i32 cnt :i -> :i32 )
    LDA STACK+0,X
    INX
    INX
    AND #31
    BEQ @done
    TAY
@loop:
    LSR STACK+3,X
    ROR STACK+2,X
    ROR STACK+1,X
    ROR STACK+0,X
    DEY
    BNE @loop
@done:
    RTS

; @code i32>> ( a :i32 cnt :i -> :i32 )
    LDA STACK+0,X
    INX
    INX
    AND #31
    BEQ @done
    TAY
@loop:
    LDA STACK+3,X
    ASL A
    ROR STACK+3,X
    ROR STACK+2,X
    ROR STACK+1,X
    ROR STACK+0,X
    DEY
    BNE @loop
@done:
    RTS
