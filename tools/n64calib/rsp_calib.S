# RSP calibration overlay for rspq.
#   0x0 SpinScalar(iters)          3 scalar instructions per iteration, including the delay slot
#   0x1 SpinVector(iters)          8 vector + 3 scalar instructions per iteration, including the delay slot
#   0x2 DmaIn(size, rdram, iters)  RDRAM -> DMEM, size bytes (<= 1024), waits each time
#   0x3 DmaOut(size, rdram, iters) DMEM -> RDRAM
#include <rsp_queue.inc>

    .set noreorder
    .set at

    .data

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand CalCmd_SpinScalar, 8     # 0x0
        RSPQ_DefineCommand CalCmd_SpinVector, 8     # 0x1
        RSPQ_DefineCommand CalCmd_DmaIn,      12    # 0x2
        RSPQ_DefineCommand CalCmd_DmaOut,     12    # 0x3
    RSPQ_EndOverlayHeader

    RSPQ_BeginSavedState
CAL_STATE:  .long 0
    RSPQ_EndSavedState

    .align 4
CAL_BUF:    .ds.b 1024

    .text

CalCmd_SpinScalar:
    beqz a1, 2f
    nop
1:  addiu a1, -1
    bnez a1, 1b
    nop
2:  jr ra
    nop

    # Uses $v01-$v24 only; leaves $v00 and $v25-$v31 (rspq/rdpq constants) alone.
CalCmd_SpinVector:
    beqz a1, 2f
    nop
1:  vaddc $v01, $v02, $v03
    vaddc $v04, $v05, $v06
    vaddc $v07, $v08, $v09
    vaddc $v10, $v11, $v12
    vmudh $v13, $v14, $v15
    vmudh $v16, $v17, $v18
    vmudh $v19, $v20, $v21
    vmudh $v22, $v23, $v24
    addiu a1, -1
    bnez a1, 1b
    nop
2:  jr ra
    nop

CalCmd_DmaIn:
    j CalDma
    li t5, 0

CalCmd_DmaOut:
    li t5, 1

CalDma:
    move t3, ra
    andi t4, a0, 0xFFFF
    beqz a2, 3f
    addiu t4, -1            # DMA_SIZE(size, 1)
1:  move t0, t4
    li t1, 0
    move s0, a1
    li s4, %lo(CAL_BUF)
    bnez t5, 2f
    nop
    jal DMAIn
    nop
    b 4f
    nop
2:  jal DMAOut
    nop
4:  addiu a2, -1
    bnez a2, 1b
    nop
3:  jr t3
    nop
