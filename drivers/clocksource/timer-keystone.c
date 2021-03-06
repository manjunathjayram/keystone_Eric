/*
 * keystone timer implementation
 * Copyright 2013 Texas Instruments, Inc.
 *
 * Based on arch/arm/mach-davinci/time.c.  Original copyrights follow
 *
 * Author: Kevin Hilman <khilman@linaro.com>
 *
 * 2007 (c) MontaVista Software, Inc. This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

static struct clock_event_device keystone_event;
static unsigned int keystone_clock_tick_rate;

/*
 * This driver configures one 32 bit counter from the 64 counter for clockevent
 */

/* values for 'opts' field of struct timer_s */
#define TIMER_OPTS_DISABLED		0x01
#define TIMER_OPTS_ONESHOT		0x02
#define TIMER_OPTS_PERIODIC		0x04
#define TIMER_OPTS_STATE_MASK		0x07

/* Timer register offsets */
#define PID12			0x00
#define TIM12			0x10
#define TIM34			0x14
#define PRD12			0x18
#define PRD34			0x1c
#define TCR			0x20
#define TGCR			0x24

/* Timer register bitfields */
#define TCR_ENAMODE_DISABLE          0x0
#define TCR_ENAMODE_ONESHOT          0x1
#define TCR_ENAMODE_PERIODIC         0x2
#define TCR_ENAMODE_MASK             0x3

#define TGCR_TIMMODE_SHIFT           2
#define TGCR_TIMMODE_64BIT_GP        0x0
#define TGCR_TIMMODE_32BIT_UNCHAINED 0x1
#define TGCR_TIMMODE_32BIT_CHAINED   0x3

#define TGCR_TIM12RS_SHIFT           0
#define TGCR_TIM34RS_SHIFT           1
#define TGCR_RESET                   0x0
#define TGCR_UNRESET                 0x1
#define TGCR_RESET_MASK              0x3

struct timer_s {
	char *name;
	unsigned long period;
	unsigned long opts;
	unsigned long flags;
	void __iomem *base;
	unsigned long tim_off;
	unsigned long prd_off;
	unsigned long enamode_shift;
	struct irqaction irqaction;
};

static int timer32_config(struct timer_s *t)
{
	u32 tcr;

	tcr = readl(t->base + TCR);

	/* disable timer */
	tcr &= ~(TCR_ENAMODE_MASK << t->enamode_shift);
	writel(tcr, t->base + TCR);

	/* reset counter to zero, set new period */
	writel(0, t->base + t->tim_off);
	writel(t->period, t->base + t->prd_off);

	/* Set enable mode */
	if (t->opts & TIMER_OPTS_ONESHOT)
		tcr |= TCR_ENAMODE_ONESHOT << t->enamode_shift;
	else if (t->opts & TIMER_OPTS_PERIODIC)
		tcr |= TCR_ENAMODE_PERIODIC << t->enamode_shift;

	writel(tcr, t->base + TCR);

	return 0;
}

static inline u32 timer32_read(struct timer_s *t)
{
	return readl(t->base + t->tim_off);
}

static irqreturn_t timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = &keystone_event;

	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static struct timer_s timer = {
	.name		= "timer64-event",
	.opts		= TIMER_OPTS_DISABLED,
	.enamode_shift	= 6,
	.tim_off	= TIM12,
	.prd_off	= PRD12,
	.irqaction = {
		.flags   = IRQF_DISABLED | IRQF_TIMER,
		.handler = timer_interrupt,
	}
};

/*
 * clockevent
 */
static int keystone_set_next_event(unsigned long cycles,
				  struct clock_event_device *evt)
{
	struct timer_s *t = &timer;

	t->period = cycles;
	timer32_config(t);
	return 0;
}

static void keystone_set_mode(enum clock_event_mode mode,
			     struct clock_event_device *evt)
{
	struct timer_s *t = &timer;

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		t->period = keystone_clock_tick_rate / (HZ);
		t->opts &= ~TIMER_OPTS_STATE_MASK;
		t->opts |= TIMER_OPTS_PERIODIC;
		timer32_config(t);
		break;
	case CLOCK_EVT_MODE_ONESHOT:
		t->opts &= ~TIMER_OPTS_STATE_MASK;
		t->opts |= TIMER_OPTS_ONESHOT;
		break;
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
		t->opts &= ~TIMER_OPTS_STATE_MASK;
		t->opts |= TIMER_OPTS_DISABLED;
		break;
	case CLOCK_EVT_MODE_RESUME:
		break;
	}
}

static struct clock_event_device keystone_event = {
	.features       = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.shift		= 32,
	.set_next_event	= keystone_set_next_event,
	.set_mode	= keystone_set_mode,
};

static void __init keystone_timer_init(struct device_node *np)
{
	void __iomem *base;
	struct clk *clk;
	int irq, error;
	u32 tgcr;

	irq  = irq_of_parse_and_map(np, 0);
	if (irq == NO_IRQ) {
		pr_err("keystone-timer: failed to map interrupts\n");
		return;
	}

	base = of_iomap(np, 0);
	if (!base) {
		pr_err("keystone-timer: failed to map registers\n");
		return;
	}

	clk = of_clk_get(np, 0);
	if (!clk) {
		pr_err("keystone-timer: failed to get clock\n");
		iounmap(base);
		return;
	}

	error = clk_prepare_enable(clk);
	if (error) {
		pr_err("keystone-timer: failed to enable clock\n");
		iounmap(base);
		clk_put(clk);
		return;
	}

	/* Disabled, Internal clock source */
	writel(0, base + TCR);

	/* reset both timers, no pre-scaler for timer34 */
	tgcr = 0;
	writel(tgcr, base + TGCR);

	/* Set both timers to unchained 32-bit */
	tgcr = TGCR_TIMMODE_32BIT_UNCHAINED << TGCR_TIMMODE_SHIFT;
	writel(tgcr, base + TGCR);

	/* Unreset timers */
	tgcr |= (TGCR_UNRESET << TGCR_TIM12RS_SHIFT) |
		(TGCR_UNRESET << TGCR_TIM34RS_SHIFT);
	writel(tgcr, base + TGCR);

	/* Init both counters to zero */
	writel(0, base + TIM12);
	writel(0, base + TIM34);

	/* Init of each timer as a 32-bit timer */
	timer.base = base;
	timer.irqaction.name = timer.name;
	timer.irqaction.dev_id = (void *)&timer;
	setup_irq(irq, &timer.irqaction);

	keystone_clock_tick_rate = clk_get_rate(clk);

	/* setup clockevent */
	keystone_event.name = timer.name;
	keystone_event.mult = div_sc(keystone_clock_tick_rate, NSEC_PER_SEC,
					 keystone_event.shift);
	keystone_event.max_delta_ns =
		clockevent_delta2ns(0xfffffffe, &keystone_event);
	keystone_event.min_delta_ns = 50000; /* 50 usec */

	keystone_event.cpumask = cpumask_of(0);
	clockevents_register_device(&keystone_event);

	timer32_config(&timer);

	pr_info("keystone timer clock @%d MHz\n", keystone_clock_tick_rate);
}

CLOCKSOURCE_OF_DECLARE(keystone_timer, "ti,keystone-timer",
					keystone_timer_init);
