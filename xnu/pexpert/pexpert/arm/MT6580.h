/*
 * MediaTek MT6580 platform definitions for XNU.
 *
 * This is intentionally a small, self-contained board header.  The MT6580
 * is a Cortex-A7 (ARMv7-A) SoC with a GIC-400 interrupt controller and the
 * ARM generic timer.  The bootloader (LK) leaves the GIC and generic timer
 * in a usable state; XNU programs them again during platform init.
 */

#ifndef _PEXPERT_ARM_MT6580_H
#define _PEXPERT_ARM_MT6580_H

/* GIC-400 */
#define MT6580_GIC_DIST_BASE		0x10211000
#define MT6580_GIC_CPU_BASE		0x10212000
#define MT6580_GIC_DIST_SIZE		0x1000
#define MT6580_GIC_CPU_SIZE		0x2000

/* ARM generic timer */
#define MT6580_CNTFRQ			13000000
#define MT6580_TIMER_IRQ		27	/* virtual timer PPI */

/* MediaTek GPT (kept for the bootloader-compatible timer path) */
#define MT6580_GPT_BASE			0x10008000
#define MT6580_GPT_IRQ			124

/* UARTs (polled console, unused for the first framebuffer boot) */
#define MT6580_UART0_BASE		0x11005000
#define MT6580_UART1_BASE		0x11006000
#define MT6580_UART2_BASE		0x11007000

/* Display / DSI */
#define MT6580_DISP_BASE		0x14000000
#define MT6580_DSI_BASE			0x14012000

#ifndef ASSEMBLER

#include <stdint.h>

static inline uint32_t
mt6580_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
mt6580_write32(uintptr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

#endif /* !ASSEMBLER */

#endif /* ! _PEXPERT_ARM_MT6580_H */
