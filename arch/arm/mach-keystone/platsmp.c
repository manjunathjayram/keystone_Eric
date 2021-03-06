/*
 * Copyright 2012 Texas Instruments, Inc.
 *
 * Based on platsmp.c, Copyright 2010-2011 Calxeda, Inc.
 * Based on platsmp.c, Copyright (C) 2002 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/of.h>

#include <asm/smp.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/memory.h>
#include <asm/psci.h>

#include "keystone.h"
asm(".arch_extension sec\n\t");

static void __cpuinit keystone_smp_secondary_initmem(void)
{
#ifdef CONFIG_ARM_LPAE
	pgd_t *pgd0 = pgd_offset_k(0);
	cpu_set_ttbr(1, __pa(pgd0) + TTBR1_OFFSET);
	local_flush_tlb_all();
#endif
}

static void __cpuinit keystone_smp_secondary_init(unsigned int cpu)
{
	keystone_smp_secondary_initmem();
}

static int __cpuinit
keystone_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long start = virt_to_idmap(&secondary_startup);
	int error;

	pr_debug("keystone-smp: booting cpu %d, vector %08lx\n",
		 cpu, start);

	asm volatile (
		"mov    r0, #0\n"	/* power on cmd	*/
		"mov    r1, %1\n"	/* cpu		*/
		"mov    r2, %2\n"	/* start	*/
		"smc	#0\n"		/* SMI		*/
		"mov    %0, r0\n"
		: "=r" (error)
		: "r"(cpu), "r"(start)
		: "cc", "r0", "r1", "r2", "memory"
	);

	pr_debug("keystone-smp: monitor returned %d\n", error);

	return error;
}

#ifdef CONFIG_HOTPLUG_CPU
static void keystone_cpu_die(unsigned int cpu)
{
#ifdef CONFIG_ARM_PSCI
	struct psci_power_state pwr_state = {0, 0, 0};

	printk(KERN_INFO "keystone_cpu_die(%d) from %d using PSCI\n", cpu,
	       smp_processor_id());

	if (psci_ops.cpu_off)
		psci_ops.cpu_off(pwr_state);
#else
	/*
	 * We may want to add here a direct smc call to monitor
	 * if the kernel doesn't support PSCI API
	 */
#endif

	/*
	 * we shouldn't come here. But in case something went
	 * wrong the code below prevents kernel from crush
	 */
	while (1)
		cpu_do_idle();
}
#endif

struct smp_operations keystone_smp_ops __initdata = {
	.smp_secondary_init	= keystone_smp_secondary_init,
	.smp_boot_secondary	= keystone_smp_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= keystone_cpu_die,
#endif
};
