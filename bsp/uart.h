#ifndef UART_H
#define UART_H

// UART 초기화, 출력 함수 프로토타입
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *str);

#endif