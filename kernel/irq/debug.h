/*
 * Copyright (c) 2016-2019 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */


#include <lego/kernel.h>
#include <lego/kallsyms.h>

#define ___P(f) if (desc->status_use_accessors & f) printk("%14s set\n", #f)
#define ___PS(f) if (desc->istate & f) printk("%14s set\n", #f)
/* FIXME */
#define ___PD(f) do { } while (0)

static inline void print_irq_desc(unsigned int irq, struct irq_desc *desc)
{
	printk("irq %d, desc: %p, depth: %d, count: %d, unhandled: %d\n",
		irq, desc, desc->depth, desc->irq_count, desc->irqs_unhandled);
	printk("->handle_irq():  %p, ", desc->handle_irq);
	print_symbol("%s\n", (unsigned long)desc->handle_irq);
	printk("->irq_data.chip(): %p, ", desc->irq_data.chip);
	print_symbol("%s\n", (unsigned long)desc->irq_data.chip);
	printk("->action(): %p\n", desc->action);
	if (desc->action) {
		printk("->action->handler(): %p, ", desc->action->handler);
		print_symbol("%s\n", (unsigned long)desc->action->handler);
	}

	___P(IRQ_LEVEL);
	___P(IRQ_PER_CPU);
	___P(IRQ_NOPROBE);
	___P(IRQ_NOREQUEST);
	___P(IRQ_NOTHREAD);
	___P(IRQ_NOAUTOEN);

	___PS(IRQS_AUTODETECT);
	___PS(IRQS_REPLAY);
	___PS(IRQS_WAITING);
	___PS(IRQS_PENDING);

	___PD(IRQS_INPROGRESS);
	___PD(IRQS_DISABLED);
	___PD(IRQS_MASKED);
}

#undef ___P
#undef ___PS
#undef ___PD
