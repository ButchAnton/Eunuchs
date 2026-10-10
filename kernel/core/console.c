#include <console.h>
#include <fs.h>
#include <debug.h>
#include <irq.h>
#include <percpu.h>
#include <platform.h>
#include <riscv.h>
#include <sbi.h>
#include <sched.h>

#define BREAK_CHAR 0x1d     /* Ctrl-] */
static bool uart_ready;

void console_init(void)
{
    if (platform.uart.base) {
        uart_init(platform.uart.base);
        uart_ready = true;
    }
}

void console_putc(char c)
{
    if (c == '\n')
        console_putc('\r');
    if (uart_ready)
        uart_putc(c);
    else
        sbi_console_putchar(c);
}

void console_write(const char *s, size_t n)
{
    while (n--)
        console_putc(*s++);
}

int console_getc_nowait(void)
{
    return uart_ready ? uart_getc() : sbi_console_getchar();
}

/* Input from the UART interrupt: Ctrl-] is the monitor's; the rest is the tty's. */
void console_rx_char(char c)
{
    if (c == BREAK_CHAR) {
        monitor_enter("console break (Ctrl-])", this_cpu()->irq_tf, true);
        return;
    }
    tty_input(c);
}

static void uart_irq(void *arg)
{
    (void)arg;
    int c;
    while ((c = uart_getc()) >= 0)
        console_rx_char((char)c);
}

void console_enable_irq(void)
{
    if (!uart_ready || !platform.uart.irq)
        return;
    if (irq_register(platform.uart.irq, "uart", uart_irq, NULL) == 0)
        uart_enable_rx_irq();
}



/*
 * Blocking read. Don't spin on the UART: in QEMU every status-register read
 * is an MMIO exit that takes the global lock, which starves the thread that
 * delivers keystrokes (input then shows up late and in bursts) and burns a
 * host core. Instead, sleep ~10ms between polls. Phase 3 replaces this with a
 * UART receive interrupt.
 */
int console_getc(void)
{
    int c;
    while ((c = console_getc_nowait()) < 0)
        cpu_wait_ticks((platform.timebase ? platform.timebase : 10000000) / 100);
    return c;
}
