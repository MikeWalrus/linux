#include <linux/of_fdt.h>
#include <asm/prom.h>

#define EARLY_UART_BASE 0xbfe41000
#define UART_REG(offset) (uint32_t *)(EARLY_UART_BASE + (offset))
#define LSR UART_REG(0x14)
#define THR UART_REG(0x00)

#define UART_LSR_THRE 0x20 /* Xmit holding register empty */

void prom_putchar(char c)
{
	while (!(readl(LSR) & UART_LSR_THRE))
		;
	writeb(c, THR);
}

void prom_init(void)
{
}
