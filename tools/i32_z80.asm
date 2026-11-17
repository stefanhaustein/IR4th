; Z80 32-bit integer arithmetic & shift runtime for IR4th.
; Preserves IX (IR4th locals frame pointer).

    JP MUL32
    JP UDIVMOD32
    JP SDIVMOD32
    JP SHL32
    JP SHR32
    JP ASHR32

I32_A:  DEFB 0, 0, 0, 0
I32_B:  DEFB 0, 0, 0, 0
I32_R:  DEFB 0, 0, 0, 0
I32_S:  DEFB 0, 0

; I32_A = I32_A * I32_B
MUL32:
    LD HL, 0
    LD (I32_R), HL
    LD (I32_R+2), HL
    LD B, 32
mul_loop:
    LD HL, I32_B+3
    SRL (HL)
    DEC HL
    RR (HL)
    DEC HL
    RR (HL)
    DEC HL
    RR (HL)
    JR NC, mul_noadd
    LD HL, (I32_R)
    LD DE, (I32_A)
    ADD HL, DE
    LD (I32_R), HL
    LD HL, (I32_R+2)
    LD DE, (I32_A+2)
    ADC HL, DE
    LD (I32_R+2), HL
mul_noadd:
    LD HL, I32_A
    SLA (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    DJNZ mul_loop
    LD HL, (I32_R)
    LD (I32_A), HL
    LD HL, (I32_R+2)
    LD (I32_A+2), HL
    RET

; Unsigned I32_A / I32_B -> quotient in I32_A, remainder in I32_R
UDIVMOD32:
    LD HL, 0
    LD (I32_R), HL
    LD (I32_R+2), HL
    LD B, 32
udiv_loop:
    LD HL, I32_A
    SLA (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    LD HL, I32_R
    RL (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    LD HL, (I32_R)
    LD DE, (I32_B)
    OR A
    SBC HL, DE
    EX DE, HL
    LD HL, (I32_R+2)
    PUSH BC
    LD BC, (I32_B+2)
    SBC HL, BC
    POP BC
    JR C, udiv_skip
    LD (I32_R), DE
    LD (I32_R+2), HL
    LD HL, I32_A
    INC (HL)
udiv_skip:
    DJNZ udiv_loop
    RET

; Negate 4-byte little-endian integer at (HL)
NEG32_HL:
    LD B, 4
    OR A
neg_loop:
    LD A, 0
    SBC A, (HL)
    LD (HL), A
    INC HL
    DJNZ neg_loop
    RET

; Signed I32_A / I32_B -> quotient in I32_A, remainder in I32_R
SDIVMOD32:
    LD A, (I32_A+3)
    LD (I32_S), A
    LD HL, I32_B+3
    XOR (HL)
    LD (I32_S+1), A
    LD A, (I32_A+3)
    BIT 7, A
    JR Z, sdiv_chkb
    LD HL, I32_A
    CALL NEG32_HL
sdiv_chkb:
    LD A, (I32_B+3)
    BIT 7, A
    JR Z, sdiv_go
    LD HL, I32_B
    CALL NEG32_HL
sdiv_go:
    CALL UDIVMOD32
    LD A, (I32_S+1)
    BIT 7, A
    JR Z, sdiv_chkrem
    LD HL, I32_A
    CALL NEG32_HL
sdiv_chkrem:
    LD A, (I32_S)
    BIT 7, A
    RET Z
    LD HL, I32_R
    JP NEG32_HL

; I32_A = I32_A << ((I32_B) & 31)
SHL32:
    LD A, (I32_B)
    AND 31
    RET Z
    LD B, A
shl_loop:
    LD HL, I32_A
    SLA (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    INC HL
    RL (HL)
    DJNZ shl_loop
    RET

; I32_A = (u32)I32_A >> ((I32_B) & 31)
SHR32:
    LD A, (I32_B)
    AND 31
    RET Z
    LD B, A
shr_loop:
    LD HL, I32_A+3
    SRL (HL)
    DEC HL
    RR (HL)
    DEC HL
    RR (HL)
    DEC HL
    RR (HL)
    DJNZ shr_loop
    RET

; I32_A = (i32)I32_A >> ((I32_B) & 31)
ASHR32:
    LD A, (I32_B)
    AND 31
    RET Z
    LD B, A
ashr_loop:
    LD HL, I32_A+3
    SRA (HL)
    DEC HL
    RR (HL)
    DEC HL
    RR (HL)
    DEC HL
    RR (HL)
    DJNZ ashr_loop
    RET
