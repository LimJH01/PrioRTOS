#ifndef UART_H
#define UART_H

#define UART_PUTS_MAX_BYTES 128U

typedef enum {
    UART_OK = 0,
    UART_ERR_TX_TIMEOUT = -1,
    UART_ERR_STRING_TOO_LONG = -2,
    UART_ERR_INVALID_ARGUMENT = -3
} UartResult_t;

// UART 초기화, 출력 함수 프로토타입
void uart_init(void);
UartResult_t uart_putc(char c);
UartResult_t uart_puts(const char *str);

#endif