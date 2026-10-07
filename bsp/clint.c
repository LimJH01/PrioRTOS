#include <clint.h>

// 부팅시 단 한번만 실행됨
/* RTOS가 시작될 때 첫 번째 타이머 인터럽트가 발생할 최초의 기준 시각을 설정하고, 
CPU의 타이머 인터럽트 수신 스위치를 켜는 것 */
void clint_timer_init(){
    uint64_t current_time = clint_get_mtime;
    // 첫번째 알림을 미래로 보냄
    /*아직 첫 번째 태스크와 OS 스케줄러 초기화가 끝나지도 않았는데 
    부팅 0.00001초 만에 알람이 울려 인터럽트로 납치당하는 것을 방지하기 위함*/ 
    clint_set_mtimecmp(current_time + TIMER_INTERVAL);

    /*CPU 내부의 32개 인터럽트 스위치 중
    7번 스위치(타이머 인터럽트 허용)만 콕 집어서 1(ON)로 켜는 작업*/ 
    uint32_t mie;
    asm volatile("csrr %0, mie" : "=r"(mie));
    mie |= (1 << 7); // MTIE = bit 7
    asm volatile("csrw mie, %0" :: "r"(mie));
}

/* 매 1ms마다 인터럽트 핸들러 내부에서 실행
알람이 울려서 timer_isr로 들어왔을 때, 알람을 끄고 다음 1ms 주기를 예약하는 것 */
void clint_set_next_timer(){
    // 현재 설정된 mtimecmp를 읽어서 더하는 대신, 
    // 현재 하드웨어 시각 기준으로 안전하게 다음 주기를 더함
    uint64_t next_time = clint_get_mtime() + TIMER_INTERVAL;
    clint_set_mtimecmp(next_time);
}