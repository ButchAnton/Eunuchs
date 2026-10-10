#ifndef EUNUCHS_CONSOLE_H
#define EUNUCHS_CONSOLE_H

#include <types.h>

/* Console: the UART once initialized, SBI legacy console before that. */
void console_init(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_write_locked(const char *s, size_t n);   /* serialized with printk */
int console_getc_nowait(void);   /* -1 if no input */
int console_getc(void);          /* blocks (polling) */

/* Interrupt-driven input (after plic_init); Ctrl-] enters the debug monitor. */
void console_enable_irq(void);
void console_rx_char(char c);              /* from the UART interrupt */

/* drivers/uart16550.c */
void uart_enable_rx_irq(void);
void uart_disable_rx_irq(void);
void uart_init(uintptr_t base);
void uart_putc(char c);
int uart_getc(void);

#endif
