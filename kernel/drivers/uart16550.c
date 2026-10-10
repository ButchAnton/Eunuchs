/*
 * NS16550A UART. Receive is interrupt driven (characters go to the console
 * ring); transmit stays polled so printk/panic output is synchronous and
 * works in any context, including with interrupts off.
 */
#include <console.h>
#include <mm.h>

#define RBR 0   /* receive buffer (read) */
#define THR 0   /* transmit holding (write) */
#define DLL 0   /* divisor latch low (DLAB=1) */
#define IER 1
#define DLM 1   /* divisor latch high (DLAB=1) */
#define FCR 2
#define LCR 3
#define MCR 4
#define LSR 5

#define IER_RX    0x01   /* received data available */
#define MCR_OUT2  0x08   /* interrupt output enable on real 16550s */

#define LCR_DLAB  0x80
#define LCR_8N1   0x03
#define FCR_FIFO  0x07   /* enable + clear RX/TX FIFOs */
#define LSR_DR    0x01   /* data ready */
#define LSR_THRE  0x20   /* transmit holding register empty */

static volatile u8 *uart;

static inline u8 reg_read(int r) { return uart[r]; }
static inline void reg_write(int r, u8 v) { uart[r] = v; }

void uart_init(uintptr_t base)
{
    uart = ioremap(base, 8);
    reg_write(IER, 0);
    reg_write(LCR, LCR_DLAB);
    reg_write(DLL, 3);      /* 38400 baud; QEMU ignores it */
    reg_write(DLM, 0);
    reg_write(LCR, LCR_8N1);
    reg_write(FCR, FCR_FIFO);
}

void uart_putc(char c)
{
    while (!(reg_read(LSR) & LSR_THRE))
        ;
    reg_write(THR, (u8)c);
}

int uart_getc(void)
{
    if (reg_read(LSR) & LSR_DR)
        return reg_read(RBR);
    return -1;
}

void uart_enable_rx_irq(void)
{
    reg_write(MCR, MCR_OUT2);
    reg_write(IER, IER_RX);
}

void uart_disable_rx_irq(void)
{
    reg_write(IER, 0);
}
