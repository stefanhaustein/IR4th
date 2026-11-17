; Z80 32-bit integer primitives for IR4th (converted via tools/asm2ir4.py).
; Uses SP/IX/IY stack scratch (zero relocations).

; @code i32* ( a :i32 b :i32 -> :i32 )
    POP IY
    LD HL, 0
    PUSH HL
    PUSH HL
    LD IX, 0
    ADD IX, SP
    LD B, 32
mul_loop:
    SRL (IX+7)
    RR (IX+6)
    RR (IX+5)
    RR (IX+4)
    JR NC, mul_noadd
    LD A, (IX+0)
    ADD A, (IX+8)
    LD (IX+0), A
    LD A, (IX+1)
    ADC A, (IX+9)
    LD (IX+1), A
    LD A, (IX+2)
    ADC A, (IX+10)
    LD (IX+2), A
    LD A, (IX+3)
    ADC A, (IX+11)
    LD (IX+3), A
mul_noadd:
    SLA (IX+8)
    RL (IX+9)
    RL (IX+10)
    RL (IX+11)
    DJNZ mul_loop
    LD A, (IX+0)
    LD (IX+8), A
    LD A, (IX+1)
    LD (IX+9), A
    LD A, (IX+2)
    LD (IX+10), A
    LD A, (IX+3)
    LD (IX+11), A
    LD HL, 8
    ADD HL, SP
    LD SP, HL
    JP (IY)

; @code u32/ ( a :i32 b :i32 -> :i32 )
    POP IY
    LD HL, 0
    PUSH HL
    PUSH HL
    LD IX, 0
    ADD IX, SP
    LD B, 32
udiv_loop:
    SLA (IX+8)
    RL (IX+9)
    RL (IX+10)
    RL (IX+11)
    RL (IX+0)
    RL (IX+1)
    RL (IX+2)
    RL (IX+3)
    OR A
    LD A, (IX+0)
    SBC A, (IX+4)
    LD E, A
    LD A, (IX+1)
    SBC A, (IX+5)
    LD D, A
    LD A, (IX+2)
    SBC A, (IX+6)
    LD C, A
    LD A, (IX+3)
    SBC A, (IX+7)
    JR C, udiv_skip
    LD (IX+3), A
    LD (IX+2), C
    LD (IX+1), D
    LD (IX+0), E
    INC (IX+8)
udiv_skip:
    DJNZ udiv_loop
    LD HL, 8
    ADD HL, SP
    LD SP, HL
    JP (IY)

; @code u32mod ( a :i32 b :i32 -> :i32 )
    POP IY
    LD HL, 0
    PUSH HL
    PUSH HL
    LD IX, 0
    ADD IX, SP
    LD B, 32
umod_loop:
    SLA (IX+8)
    RL (IX+9)
    RL (IX+10)
    RL (IX+11)
    RL (IX+0)
    RL (IX+1)
    RL (IX+2)
    RL (IX+3)
    OR A
    LD A, (IX+0)
    SBC A, (IX+4)
    LD E, A
    LD A, (IX+1)
    SBC A, (IX+5)
    LD D, A
    LD A, (IX+2)
    SBC A, (IX+6)
    LD C, A
    LD A, (IX+3)
    SBC A, (IX+7)
    JR C, umod_skip
    LD (IX+3), A
    LD (IX+2), C
    LD (IX+1), D
    LD (IX+0), E
    INC (IX+8)
umod_skip:
    DJNZ umod_loop
    LD A, (IX+0)
    LD (IX+8), A
    LD A, (IX+1)
    LD (IX+9), A
    LD A, (IX+2)
    LD (IX+10), A
    LD A, (IX+3)
    LD (IX+11), A
    LD HL, 8
    ADD HL, SP
    LD SP, HL
    JP (IY)

; @code i32<< ( a :i32 cnt :i -> :i32 )
    POP IY
    POP BC
    LD A, C
    AND 31
    JR Z, shl_done
    LD B, A
    LD IX, 0
    ADD IX, SP
shl_loop:
    SLA (IX+0)
    RL (IX+1)
    RL (IX+2)
    RL (IX+3)
    DJNZ shl_loop
shl_done:
    JP (IY)

; @code u32>> ( a :i32 cnt :i -> :i32 )
    POP IY
    POP BC
    LD A, C
    AND 31
    JR Z, shr_done
    LD B, A
    LD IX, 0
    ADD IX, SP
shr_loop:
    SRL (IX+3)
    RR (IX+2)
    RR (IX+1)
    RR (IX+0)
    DJNZ shr_loop
shr_done:
    JP (IY)

; @code i32>> ( a :i32 cnt :i -> :i32 )
    POP IY
    POP BC
    LD A, C
    AND 31
    JR Z, ashr_done
    LD B, A
    LD IX, 0
    ADD IX, SP
ashr_loop:
    SRA (IX+3)
    RR (IX+2)
    RR (IX+1)
    RR (IX+0)
    DJNZ ashr_loop
ashr_done:
    JP (IY)
