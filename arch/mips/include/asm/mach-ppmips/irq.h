#ifndef __ASM_MACH_PPMIPS_IRQ_H
#define __ASM_MACH_PPMIPS_IRQ_H

#define MIPS_CPU_IRQ_BASE		0
#define MIPS_CPU_IRQ(x)			(MIPS_CPU_IRQ_BASE + (x))

#define SOFTINT0_IRQ			MIPS_CPU_IRQ(0)
#define SOFTINT1_IRQ			MIPS_CPU_IRQ(1)
#define INT0_IRQ			MIPS_CPU_IRQ(2)
#define INT1_IRQ			MIPS_CPU_IRQ(3)
#define INT2_IRQ			MIPS_CPU_IRQ(4)
#define INT3_IRQ			MIPS_CPU_IRQ(5)
#define INT4_IRQ			MIPS_CPU_IRQ(6)
#define TIMER_IRQ			MIPS_CPU_IRQ(7)		/* cpu timer */

#define MIPS_CPU_IRQS		(MIPS_CPU_IRQ(7) + 1 - MIPS_CPU_IRQ_BASE)

#define NR_IRQS			(MIPS_CPU_IRQS)

#endif
