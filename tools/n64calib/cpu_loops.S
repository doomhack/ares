# VR4300 timing loops for the calibration ROM.
# Every loop body is fixed so the cycles per iteration can be compared
# between real hardware and the emulator. Only caller-saved registers are used.

    .set noreorder
    .set noat
    .text

# void cal_alu_loop(uint32_t iters): 9 instructions per iteration, including the delay slot
    .align 5
    .globl cal_alu_loop
cal_alu_loop:
1:  addu  $8, $8, $9
    addu  $9, $9, $10
    addu  $10, $10, $11
    addu  $11, $11, $8
    addu  $12, $12, $13
    addu  $13, $13, $12
    addiu $4, $4, -1
    bnez  $4, 1b
    nop
    jr    $31
    nop

# void cal_mult_loop(uint32_t iters): dependent MULT + MFLO chain
    .align 5
    .globl cal_mult_loop
cal_mult_loop:
    li    $9, 3
1:  mult  $8, $9
    mflo  $8
    addiu $4, $4, -1
    bnez  $4, 1b
    nop
    jr    $31
    nop

# void cal_div_loop(uint32_t iters): dependent DIV + MFLO chain
    .align 5
    .globl cal_div_loop
cal_div_loop:
    li    $8, 0x7fffffff
    li    $9, 3
1:  div   $0, $8, $9
    mflo  $10
    addiu $4, $4, -1
    bnez  $4, 1b
    nop
    jr    $31
    nop

# void cal_fmul_loop(uint32_t iters): dependent MUL.S chain (1.0 * 1.0)
    .align 5
    .globl cal_fmul_loop
cal_fmul_loop:
    lui   $8, 0x3f80
    mtc1  $8, $f0
    mtc1  $8, $f2
1:  mul.s $f0, $f0, $f2
    addiu $4, $4, -1
    bnez  $4, 1b
    nop
    jr    $31
    nop

# void cal_fmul_b2b_loop(uint32_t iters): 8 dependent MUL.S back to back per iteration.
# With nothing to overlap, this shows the real MUL.S latency.
    .align 5
    .globl cal_fmul_b2b_loop
cal_fmul_b2b_loop:
    lui   $8, 0x3f80
    mtc1  $8, $f0
    mtc1  $8, $f2
1:  mul.s $f0, $f0, $f2
    mul.s $f0, $f0, $f2
    mul.s $f0, $f0, $f2
    mul.s $f0, $f0, $f2
    mul.s $f0, $f0, $f2
    mul.s $f0, $f0, $f2
    mul.s $f0, $f0, $f2
    mul.s $f0, $f0, $f2
    addiu $4, $4, -1
    bnez  $4, 1b
    nop
    jr    $31
    nop

# void cal_fdiv_loop(uint32_t iters): dependent DIV.S chain (1.0 / 1.0)
    .align 5
    .globl cal_fdiv_loop
cal_fdiv_loop:
    lui   $8, 0x3f80
    mtc1  $8, $f0
    mtc1  $8, $f2
1:  div.s $f0, $f0, $f2
    addiu $4, $4, -1
    bnez  $4, 1b
    nop
    jr    $31
    nop

# void cal_load_loop(void *p, uint32_t count, uint32_t stride)
    .align 5
    .globl cal_load_loop
cal_load_loop:
1:  lw    $8, 0($4)
    addu  $4, $4, $6
    addiu $5, $5, -1
    bnez  $5, 1b
    nop
    jr    $31
    nop

# void cal_store_loop(void *p, uint32_t count, uint32_t stride)
    .align 5
    .globl cal_store_loop
cal_store_loop:
1:  sw    $0, 0($4)
    addu  $4, $4, $6
    addiu $5, $5, -1
    bnez  $5, 1b
    nop
    jr    $31
    nop

# Straight-line code blocks for I-cache tests (I-cache is 16 KB, 32 B lines).
# cal_code_4k fits in the cache; cal_code_32k misses on every line.
    .align 5
    .globl cal_code_4k
cal_code_4k:
    .rept 1022
    nop
    .endr
    jr    $31
    nop

    .align 5
    .globl cal_code_32k
cal_code_32k:
    .rept 8190
    nop
    .endr
    jr    $31
    nop
