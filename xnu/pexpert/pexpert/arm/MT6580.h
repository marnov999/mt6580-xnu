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

/* SoC physical aperture.  The stock MT6580 device tree has no Apple
 * "arm-io" node, so the platform expert uses this base for the few
 * devices it needs during early bring-up. */
#define MT6580_SOC_PHYS_BASE		0x10000000

/* GIC-400 */
#define MT6580_GIC_DIST_BASE		0x10211000
#define MT6580_GIC_CPU_BASE		0x10212000
#define MT6580_GIC_DIST_SIZE		0x1000
#define MT6580_GIC_CPU_SIZE		0x2000
#define MT6580_GIC_CPU_OFFSET		0x1000

/* GIC-400 register offsets (relative to the distributor / CPU interface) */
#define MT6580_GICD_CTLR		0x000
#define MT6580_GICD_IGROUPR0		0x080
#define MT6580_GICD_ISENABLER0		0x100
#define MT6580_GICD_IPRIORITYR		0x400
#define MT6580_GICD_ICFGR1		0xC04
#define MT6580_GICC_CTLR		0x000
#define MT6580_GICC_PMR			0x004
#define MT6580_GICC_BPR			0x008
#define MT6580_GICC_IAR			0x00C
#define MT6580_GICC_EOIR		0x010
#define MT6580_GICC_DIR			0x1000

/* ARM generic timer */
#define MT6580_CNTFRQ			13000000
#define MT6580_TIMER_IRQ		27	/* virtual timer PPI 11 -> GIC ID 27 */

/* Early platform clock estimates.  These are only used for timebase
 * calibration and bus/peripheral timeout scaling; the CPU PLL values
 * can be refined once the clock driver is available. */
#define MT6580_CPU_FREQ			1300000000
#define MT6580_BUS_FREQ			400000000
#define MT6580_PRF_FREQ			100000000

/* Stock panel on this handset (hi8561 452x960 DSI video panel).  The
 * framebuffer pitch is aligned to 32 pixels by LK. */
#define MT6580_PANEL_WIDTH		452
#define MT6580_PANEL_HEIGHT		960
#define MT6580_PANEL_ROWBYTES		(480 * 4)
#define MT6580_PANEL_DEPTH		32

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
