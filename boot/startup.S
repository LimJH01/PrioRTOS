.section .text.boot
.global _start

_start:
    # 1. sp 레지스터에 .bss의 g_system_stack 꼭대기 주입
    la sp, g_system_stack + 2048

    # 2. .bss 영역 0 초기화
    la t0, __bss_start
    la t1, __bss_end

clear_bss_loop:
    bge t0, t1, bss_done
    sw zero, 0(t0)
    addi t0, t0, 4
    j clear_bss_loop

bss_done:
    # 3. main() 함수 진입
    call main

hang:
    wfi
    j hang