#ifndef UART_H
#define UART_H

typedef enum {
    UART_OK = 0,
    UART_ERR_TX_TIMEOUT = -1
} UartResult_t;

// UART 초기화, 출력 함수 프로토타입
void uart_init(void);
UartResult_t uart_putc(char c);
UartResult_t uart_puts(const char *str);

#endif