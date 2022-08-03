#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/clk-provider.h>
#include <linux/irqchip.h>
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <asm/prom.h>
#include <asm/irq_cpu.h>
#include <asm/time.h>
#include <asm/mips-cps.h>
#include <asm/irq_cpu.h>
#include <asm/bootinfo.h>
#include <asm/setup.h>

void __init plat_time_init(void)
{
	mips_hpt_frequency = 33000000;
}

void __init plat_mem_setup(void)
{
	__dt_setup_arch(get_fdt());
}

const char *get_system_type(void)
{
	const char *str;
	int err;

	err = of_property_read_string_index(of_root, "compatible", 0, &str);
	if (!err)
		return str;

	return "Unknown";
}
