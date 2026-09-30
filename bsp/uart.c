#include "uart.h"
#include <stdint.h>

// QEMU virt 보드의 16550 UART 물리 주소 베이스
#define UART_BASE 0x10000000UL

// 하드웨어 레지스터 MMIO 매핑
#define UART_THR (*(volatile uint8_t *)(UART_BASE + 0x00)) // 송신 버퍼
#define UART_DLL (*(volatile uint8_t *)(UART_BASE + 0x00)) // 보레이트 하위 비트 (DLAB=1)
#define UART_DLM (*(volatile uint8_t *)(UART_BASE + 0x01)) // 보레이트 상위 비트 (DLAB=1)
#define UART_IER (*(volatile uint8_t *)(UART_BASE + 0x01)) // 인터럽트 허용/차단 제어
#define UART_FCR (*(volatile uint8_t *)(UART_BASE + 0x02)) // FIFO 제어
#define UART_LCR (*(volatile uint8_t *)(UART_BASE + 0x03)) // 통신 라인 제어 (DLAB 포함)
#define UART_LSR (*(volatile uint8_t *)(UART_BASE + 0x05)) // 라인 상태 (버퍼 비었는지 확인)

#define LSR_TX_EMPTY (1 << 5) // 5번 비트: 송신 버퍼 빔 (THRE)

void uart_init(void) {
    // 1. 초기에는 UART 자체 인터럽트를 끄고 폴링(Polling) 방식으로 동작시킴
    // 인터럽트 허용 레지스터
    UART_IER = 0x00;

    // 2. 보레이트(전송 속도)를 설정하기 위해 DLAB 비트 켜기 (0x80)
    UART_LCR = 0x80;

    // 3. 115200bps 설정 (표준 클럭 기준 0x03, 0x00)
    UART_DLL = 0x03;
    UART_DLM = 0x00;

    // 4. DLAB 비트 끄기 + 8비트 데이터, 패리티 없음, 1 정지 비트 (8-N-1 설정 = 0x03)
    UART_LCR = 0x03;

    // 5. FIFO 버퍼 활성화 및 초기화
    // FIFO Control Register
    UART_FCR = 0x07;
}

// 한글자 전송
void uart_putc(char c) {
    // 하드웨어 송신 버퍼가 비워질 때까지 대기 (무한 루프 방어는 일단 생략)
    while ((UART_LSR & LSR_TX_EMPTY) == 0);
    
    // 버퍼가 비었으면 1바이트 전송
    UART_THR = (uint8_t)c;
}

// 문자열 출력 및 줄바꿈 처리
// 포인터로 전달하는 것이 메모리에 좋다
void uart_puts(const char *str) {
    while (*str) {
        if (*str == '\n') {
            uart_putc('\r'); // 터미널에서 줄바꿈이 계단처럼 밀리는 것 방지
        }
        uart_putc(*str++);
    }
}