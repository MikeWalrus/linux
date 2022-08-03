#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/clk-provider.h>
#include <linux/irqchip.h>
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/linkage.h>
#include <asm/prom.h>
#include <asm/irq_cpu.h>
#include <asm/time.h>
#include <asm/mips-cps.h>
#include <asm/irq_cpu.h>

void __init arch_init_irq(void)
{
	struct device_node *intc_node;
	intc_node = of_find_compatible_node(NULL, NULL,
					    "mti,cpu-interrupt-controller");
	write_c0_ebase(0x80000000);
	of_node_put(intc_node);
	irqchip_init();
}
