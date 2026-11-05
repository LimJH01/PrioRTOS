#include <clint.h>
#include "prio_rtos.h"

/* 마지막으로 예약한 Timer interrupt deadline */
static uint64_t g_last_mtimecmp;

void clint_software_interrupt_init(void) {
    // 남아있는 인터럽트 요청을 지우는 것
    CLINT_MSIP = 0U;

    uint32_t mie;
    asm volatile("csrr %0, mie" : "=r"(mie));
    mie |= 1U << 3; // MSIE = bit 3
    asm volatile("csrw mie, %0" :: "r"(mie));
}

void clint_send_software_interrupt(void) {
    // Order kernel state updates before the interrupt request reaches the hart.
    // 순서 유지가 중요 (컴파일러가 딴짓 못하게)
    asm volatile("fence iorw, iorw" ::: "memory");
    // Cause 3 (Software Interrupt 실행)
    CLINT_MSIP = 1U;
}

void clint_clear_software_interrupt(void) {
    CLINT_MSIP = 0U;
}

uint64_t clint_get_mtime(void){
    uint32_t high;
    uint32_t low;
    uint32_t high_again;

    for (uint32_t attempt = 0; attempt < CLINT_MTIME_READ_ATTEMPTS; ++attempt) {
        high = CLINT_MTIME_HIGH;
        low = CLINT_MTIME_LOW;
        high_again = CLINT_MTIME_HIGH;

        if (high == high_again) {
            return ((uint64_t)high << 32) | low;
        }
    }

    kernel_panic("CLINT mtime read failed");
}

void clint_set_mtimecmp(uint64_t compare_value){
    // Prevent a transient compare match while updating the 64-bit register on RV32.
    CLINT_MTIMECMP_LOW = UINT32_MAX;
    CLINT_MTIMECMP_HIGH = (uint32_t)(compare_value >> 32);
    CLINT_MTIMECMP_LOW = (uint32_t)compare_value;
}

// 부팅시 단 한번만 실행됨
/* RTOS가 시작될 때 첫 번째 타이머 인터럽트가 발생할 최초의 기준 시각을 설정하고, 
CPU의 타이머 인터럽트 수신 스위치를 켜는 것 */
void clint_timer_init(void){
    uint64_t current_time = clint_get_mtime();

    g_last_mtimecmp = current_time + TIMER_INTERVAL;
    // 첫번째 알림을 미래로 보냄
    /*아직 첫 번째 태스크와 OS 스케줄러 초기화가 끝나지도 않았는데 
    부팅 0.00001초 만에 알람이 울려 인터럽트로 납치당하는 것을 방지하기 위함*/ 
    clint_set_mtimecmp(g_last_mtimecmp);

    /*CPU 내부의 32개 인터럽트 스위치 중
    7번 스위치(타이머 인터럽트 허용)만 콕 집어서 1(ON)로 켜는 작업*/ 
    uint32_t mie;
    asm volatile("csrr %0, mie" : "=r"(mie));
    mie |= (1 << 7); // MTIE = bit 7
    asm volatile("csrw mie, %0" :: "r"(mie));
}

/* 매 1s마다 인터럽트 핸들러 내부에서 실행
알람이 울려서 timer_isr로 들어왔을 때, 알람을 끄고 다음 1s 주기를 예약하는 것 */
void clint_set_next_timer(void){
    /*
     정상 경로에서는 마지막 deadline에 TIMER_INTERVAL을 더해
     주기를 유지한다. ISR 처리가 늦어 다음 deadline을 이미 지난
     경우에는 현재 시각 + TIMER_INTERVAL로 재동기화한다.
    */
    uint64_t current_time = clint_get_mtime();
    uint64_t next_time = g_last_mtimecmp + TIMER_INTERVAL;

    if ((int64_t)(current_time - next_time) >= 0){
        next_time = current_time + TIMER_INTERVAL;
    }
    g_last_mtimecmp = next_time;
    clint_set_mtimecmp(next_time);
}