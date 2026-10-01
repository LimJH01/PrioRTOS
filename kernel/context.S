.globl trap_handler
.globl trap_entry
.align 4

trap_entry:
    # main.c의 while(1)을 돌다가 HW trap이 발생하면 바로 mtvec에 기록된 trap_entry로 간다.
    # 1. 16개 레지스터를 위한 64바이트 공간 확보 (16 * 4 = 64)
    addi sp, sp, -64

    # 2. [전] C 함수가 오염시킬 Caller-saved 레지스터 16개 백업
    sw ra,  0(sp)
    sw t0,  4(sp)
    sw t1,  8(sp)
    sw t2, 12(sp)
    sw a0, 16(sp)
    sw a1, 20(sp)
    sw a2, 24(sp)
    sw a3, 28(sp)
    sw a4, 32(sp)
    sw a5, 36(sp)
    sw a6, 40(sp)
    sw a7, 44(sp)
    sw t3, 48(sp)
    sw t4, 52(sp)
    sw t5, 56(sp)
    sw t6, 60(sp)

    # 3. 순수 C 언어 핸들러 호출
    # 위에서 레지스터를 다 저장했으면 trap.c의 trap_handler로 간다.
    call trap_handler

    # 4. [후] 넣었던 위치 그대로 1:1 복원
    lw ra,  0(sp)
    lw t0,  4(sp)
    lw t1,  8(sp)
    lw t2, 12(sp)
    lw a0, 16(sp)
    lw a1, 20(sp)
    lw a2, 24(sp)
    lw a3, 28(sp)
    lw a4, 32(sp)
    lw a5, 36(sp)
    lw a6, 40(sp)
    lw a7, 44(sp)
    lw t3, 48(sp)
    lw t4, 52(sp)
    lw t5, 56(sp)
    lw t6, 60(sp)

    # 5. 사용했던 64바이트만큼 스택 반납
    addi sp, sp, 64

    # 6. 하드웨어 복귀
    mret