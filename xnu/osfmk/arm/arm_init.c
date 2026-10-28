/*
 * Copyright (c) 2007-2009 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */

#include <debug.h>
#include <mach_ldebug.h>
#include <mach_kdp.h>

#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/timer_queue.h>
#include <kern/processor.h>
#include <kern/startup.h>
#include <kern/debug.h>
#include <prng/random.h>
#include <machine/machine_routines.h>
#include <machine/commpage.h>
#include <machine/io_map_entries.h>
/* ARM64_TODO unify boot.h */
#if __arm64__
#include <pexpert/arm64/boot.h>
#elif __arm__
#include <pexpert/arm/boot.h>
#else
#error Unsupported arch
#endif
#include <pexpert/arm/consistent_debug.h>
#include <pexpert/device_tree.h>
#include <arm/proc_reg.h>
#include <arm/pmap.h>
#include <arm/caches_internal.h>
#include <arm/cpu_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/misc_protos.h>
#include <arm/machine_cpu.h>
#include <arm/rtclock.h>
#include <vm/vm_map.h>

#include <libkern/kernel_mach_header.h>
#include <libkern/stack_protector.h>
#include <libkern/section_keywords.h>
#include <san/kasan.h>

#include <pexpert/pexpert.h>

#include <console/serial_protos.h>

#if CONFIG_TELEMETRY
#include <kern/telemetry.h>
#endif
#if MONOTONIC
#include <kern/monotonic.h>
#endif /* MONOTONIC */

extern void	patch_low_glo(void);
extern int	serial_init(void);
extern void sleep_token_buffer_init(void);

extern vm_offset_t intstack_top;
extern vm_offset_t fiqstack_top;
#if __arm64__
extern vm_offset_t excepstack_top;
#endif

extern const char version[];
extern const char version_variant[];
extern int      disableConsoleOutput;

int             pc_trace_buf[PC_TRACE_BUF_SIZE] = {0};
int             pc_trace_cnt = PC_TRACE_BUF_SIZE;
int             debug_task;

boolean_t up_style_idle_exit = 0;



#if INTERRUPT_MASKED_DEBUG
boolean_t interrupt_masked_debug = 1;
uint64_t interrupt_masked_timeout = 0xd0000;
#endif

boot_args const_boot_args __attribute__((section("__DATA, __const")));
boot_args      *BootArgs __attribute__((section("__DATA, __const")));

unsigned int arm_diag;
#ifdef	APPLETYPHOON
static unsigned cpus_defeatures = 0x0;
extern void cpu_defeatures_set(unsigned int);
#endif

#if __arm64__ && __ARM_GLOBAL_SLEEP_BIT__
extern volatile boolean_t arm64_stall_sleep;
#endif

extern boolean_t force_immediate_debug_halt;

#define MIN_LOW_GLO_MASK (0x144)

/*
 * MT6580 bring-up: draw progress bars directly into the visible display
 * layer.  The display controller scans the BOOT_MENU_LAYER at
 * 0xFFAC2000 with a 452-pixel (1808-byte) pitch, and it honours the
 * per-pixel alpha byte.  Before arm_vm_init() the shim's memSize makes
 * start.s map the framebuffer 1:1, so a direct physical write works;
 * after arm_vm_init() that mapping is gone and we map it with
 * io_map_spec(VM_WIMG_IO).  The bars live at the bottom of the panel so
 * the real console text above them stays readable.
 */
#if defined(ARM_BOARD_CLASS_MT6580)
#define MT6580_FB_PHYS		0xFFAC2000u
#define MT6580_FB_WIDTH		452u
#define MT6580_FB_HEIGHT	960u
#define MT6580_FB_MARK_Y	832u

extern unsigned char iso_font[256 * 16];

static volatile uint32_t *
mt6580_fb_base(boolean_t pre_vm)
{
	static vm_offset_t fb_va = 0;

	if (pre_vm) {
		return (volatile uint32_t *)MT6580_FB_PHYS;
	}
	if (fb_va == 0) {
		fb_va = io_map_spec(MT6580_FB_PHYS, 0x200000u, VM_WIMG_IO);
	}
	return (volatile uint32_t *)fb_va;
}

static uint64_t
mt6580_read_cntpct(void)
{
	uint32_t lo, hi, hi2;

	do {
		__asm__ volatile("mrrc p15, 0, %0, %1, c14"
		    : "=&r"(lo), "=&r"(hi));
		__asm__ volatile("mrrc p15, 0, %0, %1, c14"
		    : "=&r"(lo), "=&r"(hi2));
	} while (hi != hi2);

	return ((uint64_t)hi << 32) | lo;
}

/*
 * MT6580 TOPRGU watchdog.  LK programs it before handing off to the shim,
 * and the shim leaves it armed so a hung boot resets the phone instead of
 * leaving it stuck.  It is never disabled here -- only kicked.
 *
 * The register page is mapped once, lazily, and cached.  Calling
 * io_map_spec() repeatedly would leak kernel VA: before kernel_map exists
 * the pre-VM path just bumps virtual_space_start.  Mapping it early also
 * keeps petting cheap once the marker delays start running.
 */
#define MT6580_TOPRGU_BASE	0x10007000u
#define MT6580_WDT_MODE		(MT6580_TOPRGU_BASE + 0x00u)
#define MT6580_WDT_RESTART	(MT6580_TOPRGU_BASE + 0x08u)
#define MT6580_WDT_STATUS	(MT6580_TOPRGU_BASE + 0x0Cu)
#define MT6580_WDT_PET		0x1971u

static boolean_t mt6580_wdt_ready;
static volatile uint32_t *mt6580_toprgu;

/*
 * Map the TOPRGU page and return its virtual base.  Returns NULL if the
 * mapping could not be made.  Safe both before and after arm_vm_init().
 */
static volatile uint32_t *
mt6580_toprgu_base(void)
{
	vm_offset_t va;

	if (mt6580_toprgu != NULL) {
		return mt6580_toprgu;
	}
	va = io_map_spec(MT6580_TOPRGU_BASE, 0x1000u, VM_WIMG_IO);
	if (va == 0) {
		return NULL;
	}
	mt6580_toprgu = (volatile uint32_t *)va;
	return mt6580_toprgu;
}

/* Kick the watchdog.  No-op until the caller confirms the mapping works. */
static void
mt6580_wdt_pet(void)
{
	volatile uint32_t *wdt;

	if (!mt6580_wdt_ready) {
		return;
	}
	wdt = mt6580_toprgu_base();
	if (wdt != NULL) {
		*(volatile uint32_t *)((uintptr_t)wdt +
		    (MT6580_WDT_RESTART - MT6580_TOPRGU_BASE)) = MT6580_WDT_PET;
	}
}

/*
 * Hold for the requested time, but never for more than a bounded number
 * of iterations.  If CNTPCT is not advancing (the generic timer may not
 * be counting this early), the counter test alone decides when to stop,
 * so every call would run to the guard.  At the previous 0x10000000
 * guard that was ~2s per marker, and the 16 delayed markers before
 * kmem_init add up to ~30s -- exactly the TOPRGU timeout, which is why
 * the phone reset just after "vm_map_init ok".  The guard is now small
 * enough that a stalled counter costs ~50-100ms per marker.
 */
static void
mt6580_delay_ms(unsigned int ms)
{
	uint64_t start = mt6580_read_cntpct();
	uint64_t ticks = ((uint64_t)MT6580_CNTFRQ * ms) / 1000;
	uint32_t spins = 0;

	while ((mt6580_read_cntpct() - start) < ticks) {
		if (++spins > 0x800000u) {
			break;
		}
		if ((spins & 0xFFFFu) == 0) {
			mt6580_wdt_pet();
		}
	}
}

static void
mt6580_fb_fill(uint32_t color, boolean_t pre_vm)
{
	volatile uint32_t *fb = mt6580_fb_base(pre_vm);
	unsigned int x, y;

	if (fb == 0) {
		return;
	}
	for (y = 0; y < 960; y++) {
		for (x = 0; x < MT6580_FB_WIDTH; x++) {
			fb[y * MT6580_FB_WIDTH + x] = color;
		}
	}
	if (pre_vm) {
		FlushPoC_Dcache();
	}
}

static void
mt6580_fb_mark(uint32_t color, unsigned int row, boolean_t pre_vm)
{
	volatile uint32_t *fb = mt6580_fb_base(pre_vm);
	unsigned int x, y;

	if (fb == 0) {
		return;
	}
	for (y = MT6580_FB_MARK_Y + row * 16;
	    y < MT6580_FB_MARK_Y + (row + 1) * 16; y++) {
		for (x = 0; x < MT6580_FB_WIDTH; x++) {
			fb[y * MT6580_FB_WIDTH + x] = color;
		}
	}
	if (pre_vm) {
		FlushPoC_Dcache();
	}

	/*
	 * Bring-up only: hold each bottom bar so its colour can be
	 * identified before the next milestone draws over it.
	 */
	mt6580_delay_ms(1500);
}

/*
 * Draw the complete marker palette once, slowly, before any console
 * setup.  This lets the panel show the colours even if the console
 * enable path faults afterwards.
 */
static void
mt6580_fb_stripes_test(void)
{
	static const uint32_t colors[8] = {
		0xFFFF00FF,	/* magenta */
		0xFFFFFFFF,	/* white */
		0xFF00FF00,	/* green */
		0xFF808080,	/* gray */
		0xFF00FF80,	/* spring green */
		0xFF800000,	/* maroon */
		0xFF008080,	/* teal */
		0xFFFFFF00	/* yellow */
	};
	unsigned int i;

	for (i = 0; i < 8; i++) {
		mt6580_fb_mark(colors[i], i, FALSE);
	}
}

/*
 * Minimal direct framebuffer console.  The generic video console needs
 * PE_state.video to be encoded exactly as iBoot does; until that is fixed
 * it scribbles over the visible page.  These helpers write page 1 directly
 * with the known 452x960, 32-bpp, 1808-byte-pitch layout.
 */
static void
mt6580_fb_clear(uint32_t color)
{
	volatile uint32_t *fb = mt6580_fb_base(FALSE);
	unsigned int i;

	if (fb == 0) {
		return;
	}
	for (i = 0; i < MT6580_FB_WIDTH * MT6580_FB_HEIGHT; i++) {
		fb[i] = color;
	}
}

static void
mt6580_fb_char(unsigned int col, unsigned int row, unsigned char ch,
    uint32_t fg, uint32_t bg)
{
	volatile uint32_t *fb = mt6580_fb_base(FALSE);
	unsigned char *glyph;
	unsigned int x, y;
	unsigned int px = col * 8;
	unsigned int py = row * 16;

	if (fb == 0 || px + 8 > MT6580_FB_WIDTH ||
	    py + 16 > MT6580_FB_HEIGHT) {
		return;
	}

	glyph = iso_font + ((unsigned int)ch * 16);
	for (y = 0; y < 16; y++) {
		unsigned char bits = glyph[y];

		for (x = 0; x < 8; x++) {
			fb[(py + y) * MT6580_FB_WIDTH + px + x] =
			    (bits & (1u << x)) ? fg : bg;
		}
	}
}

static void
mt6580_fb_text(unsigned int col, unsigned int row, const char *s,
    uint32_t fg, uint32_t bg)
{
	while (*s != '\0') {
		if (*s == '\n') {
			col = 0;
			row++;
		} else {
			mt6580_fb_char(col, row, (unsigned char)*s, fg, bg);
			col++;
		}
		s++;
	}
}

/*
 * Called from kernel_bootstrap() so progress remains visible after the
 * direct early console has handed control to the generic kernel.
 *
 * Highest text row drawn so far.  Rows are handed out monotonically by
 * the boot stages (arm_init 0-14, kernel_bootstrap 15-22, vm_mem 23-52,
 * kalloc 53-58), so this tracks the current stage and tells the panic
 * mirror where it can write without erasing the marker that identifies
 * the failing stage.
 */
static unsigned int mt6580_fb_max_row;

/*
 * Bring-up diagnostic: first CNTPCT value seen, so each marker can show
 * the time elapsed since the direct console came up.  If this readout
 * stays at zero across markers the generic timer is not counting, which
 * also means every mt6580_delay_ms() call is running into its spin guard
 * instead of timing out normally.
 */
static uint64_t mt6580_t0;

void mt6580_early_printf(unsigned int row, const char *s);
void mt6580_early_sub(const char *s);

static void
mt6580_fb_u64(unsigned int col, unsigned int row, uint64_t v)
{
	char buf[21];
	int i = 0;

	if (v == 0) {
		buf[i++] = '0';
	}
	while (v != 0) {
		buf[i++] = '0' + (char)(v % 10);
		v /= 10;
	}
	/* Only six digits fit between column 44 and the "ms" suffix. */
	while (i > 6) {
		i--;
	}
	while (i > 0) {
		mt6580_fb_char(col++, row, (unsigned char)buf[--i],
		    0xFF00FF00, 0xFF000000);
	}
}

static void
mt6580_fb_hex(unsigned int col, unsigned int row, uint32_t v)
{
	static const char digits[] = "0123456789abcdef";
	int i;

	for (i = 7; i >= 0; i--) {
		mt6580_fb_char(col++, row,
		    (unsigned char)digits[(v >> (i * 4)) & 0xF],
		    0xFF00FF00, 0xFF000000);
	}
}

/*
 * Show the watchdog mode and the latched reset status.  Used during
 * bring-up to tell a watchdog reset apart from an exception: STATUS
 * latches the reason for the previous reset.  Bit 31 (HWWDT_RST) means
 * the last boot ended in a watchdog reset.
 */
static void
mt6580_fb_wdt_status(void)
{
	volatile uint32_t *wdt = mt6580_toprgu_base();
	uint32_t mode, status;

	if (wdt == NULL) {
		mt6580_fb_text(0, 2, "wdt map failed", 0xFFFF4040, 0xFF000000);
		return;
	}
	mode = *(volatile uint32_t *)((uintptr_t)wdt +
	    (MT6580_WDT_MODE - MT6580_TOPRGU_BASE));
	status = *(volatile uint32_t *)((uintptr_t)wdt +
	    (MT6580_WDT_STATUS - MT6580_TOPRGU_BASE));

	mt6580_fb_text(0, 2, "wdt md=", 0xFF00FFFF, 0xFF000000);
	mt6580_fb_hex(7, 2, mode);
	mt6580_fb_text(16, 2, "st=", 0xFF00FFFF, 0xFF000000);
	mt6580_fb_hex(19, 2, status);
}

/*
 * Draw the elapsed-milliseconds readout at the right end of a marker row,
 * so it never overwrites the stage label at column 0.  The panel is
 * 452px wide, so only columns 0-55 are drawable; keep the readout in
 * columns 44-49 to leave the label and the "ms" suffix room.
 */
static void
mt6580_fb_elapsed(unsigned int row)
{
	uint64_t now = mt6580_read_cntpct();
	uint64_t ms;

	ms = ((now - mt6580_t0) * 1000) / MT6580_CNTFRQ;
	mt6580_fb_u64(44, row, ms);
	mt6580_fb_char(50, row, 'm', 0xFF00FF00, 0xFF000000);
	mt6580_fb_char(51, row, 's', 0xFF00FF00, 0xFF000000);
}

void
mt6580_early_printf(unsigned int row, const char *s)
{
	if (mt6580_t0 == 0) {
		mt6580_t0 = mt6580_read_cntpct();
	}
	mt6580_fb_text(0, row, s, 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_elapsed(row);
	if (row > mt6580_fb_max_row) {
		mt6580_fb_max_row = row;
	}
	/*
	 * Bring-up only: hold the marker long enough for a 240 fps capture
	 * to catch it.  The panel refreshes at ~60 Hz, so without this each
	 * stage is replaced before it is ever scanned out.
	 */
	mt6580_delay_ms(50);
}

/*
 * Sub-stage line.  Row 37 is the one row left free between the vm_mem
 * markers, so a single line is reused and rewritten for each step inside
 * a stage.  The row is cleared first because successive labels have
 * different lengths.
 *
 * Row 37 is deliberately kept out of mt6580_fb_max_row: the panic mirror
 * writes at max_row + 1, and the sub-stage label is the single most
 * useful thing to preserve, so it must never be the line just below the
 * markers.
 */
void
mt6580_early_sub(const char *s)
{
	volatile uint32_t *fb = mt6580_fb_base(FALSE);
	unsigned int x, y;

	if (fb != 0) {
		for (y = 37 * 16; y < 38 * 16; y++) {
			for (x = 0; x < MT6580_FB_WIDTH; x++) {
				fb[y * MT6580_FB_WIDTH + x] = 0xFF000000;
			}
		}
	}
	mt6580_fb_text(0, 37, s, 0xFFFFFF00, 0xFF000000);
	mt6580_delay_ms(50);
}

/*
 * Panic mirror.  kprintf() is a no-op in this configuration, so a panic
 * and a hang are otherwise indistinguishable on the panel.  XNU calls
 * PE_arm_debug_panic_hook() early in panic_trap_to_debugger().
 *
 * Write below the highest marker already drawn, so the progress lines
 * that identify the failing stage survive.  The hook receives the panic
 * format string, not the formatted result, so printf escapes such as %p
 * appear literally.
 */
static void
mt6580_panic_display(const char *str)
{
	unsigned int row = mt6580_fb_max_row + 1;
	unsigned int col = 0;

	if (row + 2 >= MT6580_FB_HEIGHT / 16) {
		/* Not enough room below the markers; start at the top. */
		row = 0;
	}

	mt6580_fb_text(0, row++, "*** PANIC ***", 0xFFFF4040, 0xFF000000);

	while (*str != '\0' && row < MT6580_FB_HEIGHT / 16) {
		if (*str == '\n' || col >= MT6580_FB_WIDTH / 8) {
			col = 0;
			row++;
			if (*str == '\n') {
				str++;
			}
			continue;
		}
		mt6580_fb_char(col++, row, (unsigned char)*str, 0xFFFFFFFF,
		    0xFF000000);
		str++;
	}
}
#else
static void
mt6580_fb_fill(__unused uint32_t color, __unused boolean_t pre_vm)
{
}

static void
mt6580_fb_mark(__unused uint32_t color, __unused unsigned int row,
    __unused boolean_t pre_vm)
{
}

static void
mt6580_fb_stripes_test(void)
{
}

static void
mt6580_fb_clear(__unused uint32_t color)
{
}

static void
mt6580_fb_text(__unused unsigned int col, __unused unsigned int row,
    __unused const char *s, __unused uint32_t fg, __unused uint32_t bg)
{
}

void
mt6580_early_printf(__unused unsigned int row, __unused const char *s)
{
}

void
mt6580_early_sub(__unused const char *s)
{
}

static void
mt6580_fb_wdt_status(void)
{
}
#endif /* ARM_BOARD_CLASS_MT6580 */

/*
 * Forward definition
 */
void arm_init(boot_args * args);

#if __arm64__
unsigned int page_shift_user32;	/* for page_size as seen by a 32-bit task */
#endif /* __arm64__ */


/*
 *		Routine:		arm_init
 *		Function:
 */
void
arm_init(
	boot_args	*args)
{
	unsigned int    maxmem;
	uint32_t        memsize;
	uint64_t        xmaxmem;
	thread_t        thread;
	processor_t     my_master_proc;

	mt6580_fb_fill(0xFFFF0000, TRUE);	/* arm_init entered */

	/* If kernel integrity is supported, use a constant copy of the boot args. */
	const_boot_args = *args;
	BootArgs = &const_boot_args;

	cpu_data_init(&BootCpuData);

	PE_init_platform(FALSE, args);	/* Get platform expert set up */
	mt6580_fb_fill(0xFFFFFF00, TRUE);	/* PE_init_platform(FALSE) done */

#if __arm64__
	{
		unsigned int    tmp_16k = 0;

#ifdef	XXXX
		/*
		 * Select the advertised kernel page size; without the boot-arg
		 * we default to the hardware page size for the current platform.
		 */
		if (PE_parse_boot_argn("-vm16k", &tmp_16k, sizeof(tmp_16k)))
			PAGE_SHIFT_CONST = PAGE_MAX_SHIFT;
		else
			PAGE_SHIFT_CONST = ARM_PGSHIFT;
#else
		/*
		 * Select the advertised kernel page size; with the boot-arg
		 * use to the hardware page size for the current platform.
		 */
		int radar_20804515 = 1; /* default: new mode */
		PE_parse_boot_argn("radar_20804515", &radar_20804515, sizeof(radar_20804515));
		if (radar_20804515) {
			if (args->memSize > 1ULL*1024*1024*1024) {
				/*
				 * arm64 device with > 1GB of RAM:
				 * kernel uses 16KB pages.
				 */
				PAGE_SHIFT_CONST = PAGE_MAX_SHIFT;
			} else {
				/*
				 * arm64 device with <= 1GB of RAM:
				 * kernel uses hardware page size
				 * (4KB for H6/H7, 16KB for H8+).
				 */
				PAGE_SHIFT_CONST = ARM_PGSHIFT;
			}
			/* 32-bit apps always see 16KB page size */
			page_shift_user32 = PAGE_MAX_SHIFT;
		} else {
			/* kernel page size: */
			if (PE_parse_boot_argn("-use_hwpagesize", &tmp_16k, sizeof(tmp_16k)))
				PAGE_SHIFT_CONST = ARM_PGSHIFT;
			else
				PAGE_SHIFT_CONST = PAGE_MAX_SHIFT;
			/* old mode: 32-bit apps see same page size as kernel */
			page_shift_user32 = PAGE_SHIFT_CONST;
		}
#endif
#ifdef	APPLETYPHOON
		if (PE_parse_boot_argn("cpus_defeatures", &cpus_defeatures, sizeof(cpus_defeatures))) {
			if ((cpus_defeatures & 0xF) != 0)
				cpu_defeatures_set(cpus_defeatures & 0xF);
		}
#endif
        }
#endif

	ml_parse_cpu_topology();
	mt6580_fb_fill(0xFF00FF00, TRUE);	/* cpu topology parsed */

	master_cpu = ml_get_boot_cpu_number();
	assert(master_cpu >= 0 && master_cpu <= ml_get_max_cpu_number());

	BootCpuData.cpu_number = (unsigned short)master_cpu;
#if	__arm__
	BootCpuData.cpu_exc_vectors = (vm_offset_t)&ExceptionVectorsTable;
#endif
	BootCpuData.intstack_top = (vm_offset_t) & intstack_top;
	BootCpuData.istackptr = BootCpuData.intstack_top;
	BootCpuData.fiqstack_top = (vm_offset_t) & fiqstack_top;
	BootCpuData.fiqstackptr = BootCpuData.fiqstack_top;
#if __arm64__
	BootCpuData.excepstack_top = (vm_offset_t) & excepstack_top;
	BootCpuData.excepstackptr = BootCpuData.excepstack_top;
#endif
	BootCpuData.cpu_processor = cpu_processor_alloc(TRUE);
	BootCpuData.cpu_console_buf = (void *)NULL;
	CpuDataEntries[master_cpu].cpu_data_vaddr = &BootCpuData;
	CpuDataEntries[master_cpu].cpu_data_paddr = (void *)((uintptr_t)(args->physBase)
	                                            + ((uintptr_t)&BootCpuData
	                                            - (uintptr_t)(args->virtBase)));

	thread_bootstrap();
	thread = current_thread();
	/*
	 * Preemption is enabled for this thread so that it can lock mutexes without
	 * tripping the preemption check. In reality scheduling is not enabled until
	 * this thread completes, and there are no other threads to switch to, so 
	 * preemption level is not really meaningful for the bootstrap thread.
	 */
	thread->machine.preemption_count = 0;
	thread->machine.CpuDatap = &BootCpuData;
#if	__arm__ && __ARM_USER_PROTECT__
        {
                unsigned int ttbr0_val, ttbr1_val, ttbcr_val;
                __asm__ volatile("mrc p15,0,%0,c2,c0,0\n" : "=r"(ttbr0_val));
                __asm__ volatile("mrc p15,0,%0,c2,c0,1\n" : "=r"(ttbr1_val));
                __asm__ volatile("mrc p15,0,%0,c2,c0,2\n" : "=r"(ttbcr_val));
		thread->machine.uptw_ttb = ttbr0_val;
		thread->machine.kptw_ttb = ttbr1_val;
		thread->machine.uptw_ttc = ttbcr_val;
        }
#endif
	BootCpuData.cpu_processor->processor_data.kernel_timer = &thread->system_timer;
	BootCpuData.cpu_processor->processor_data.thread_timer = &thread->system_timer;

	cpu_bootstrap();

	rtclock_early_init();

	kernel_early_bootstrap();

	cpu_init();

	EntropyData.index_ptr = EntropyData.buffer;

	processor_bootstrap();
	my_master_proc = master_processor;

	(void)PE_parse_boot_argn("diag", &arm_diag, sizeof (arm_diag));

	if (PE_parse_boot_argn("maxmem", &maxmem, sizeof (maxmem)))
		xmaxmem = (uint64_t) maxmem *(1024 * 1024);
	else if (PE_get_default("hw.memsize", &memsize, sizeof (memsize)))
		xmaxmem = (uint64_t) memsize;
	else
		xmaxmem = 0;

	if (PE_parse_boot_argn("up_style_idle_exit", &up_style_idle_exit, sizeof(up_style_idle_exit))) {
		up_style_idle_exit = 1;
	}
#if INTERRUPT_MASKED_DEBUG
	int wdt_boot_arg = 0;
	/* Disable if WDT is disabled or no_interrupt_mask_debug in boot-args */
	if (PE_parse_boot_argn("no_interrupt_masked_debug", &interrupt_masked_debug,
				sizeof(interrupt_masked_debug)) || (PE_parse_boot_argn("wdt", &wdt_boot_arg,
				sizeof(wdt_boot_arg)) && (wdt_boot_arg == -1)))  {
		interrupt_masked_debug = 0;
	}

	PE_parse_boot_argn("interrupt_masked_debug_timeout", &interrupt_masked_timeout, sizeof(interrupt_masked_timeout));
#endif



	PE_parse_boot_argn("immediate_NMI", &force_immediate_debug_halt, sizeof(force_immediate_debug_halt));

#if __ARM_PAN_AVAILABLE__
	__builtin_arm_wsr("pan", 1);
#endif  /* __ARM_PAN_AVAILABLE__ */

	mt6580_fb_fill(0xFFFF8000, TRUE);	/* entering arm_vm_init */
	arm_vm_init(xmaxmem, args);
	mt6580_fb_fill(0xFF00FFFF, FALSE);	/* arm_vm_init done */

	uint32_t debugmode;
	if (PE_parse_boot_argn("debug", &debugmode, sizeof(debugmode)) &&
	   ((debugmode & MIN_LOW_GLO_MASK) == MIN_LOW_GLO_MASK))
		patch_low_glo();

	printf_init();
	panic_init();
#if __arm64__ && WITH_CLASSIC_S2R
	sleep_token_buffer_init();
#endif

	PE_consistent_debug_inherit();

	/* setup debugging output if one has been chosen */
	PE_init_kprintf(FALSE);
	mt6580_fb_fill(0xFF8000FF, FALSE);	/* PE_init_kprintf done */

	serialmode = 0;                                                      /* Assume normal keyboard and console */
	if (PE_parse_boot_argn("serial", &serialmode, sizeof(serialmode))) { /* Do we want a serial
	                                                                      * keyboard and/or
	                                                                      * console? */
		kprintf("Serial mode specified: %08X\n", serialmode);
		int force_sync = serialmode & SERIALMODE_SYNCDRAIN;
		if (force_sync || PE_parse_boot_argn("drain_uart_sync", &force_sync, sizeof(force_sync))) {
			if (force_sync) {
				serialmode |= SERIALMODE_SYNCDRAIN;
				kprintf(
				    "WARNING: Forcing uart driver to output synchronously."
				    "printf()s/IOLogs will impact kernel performance.\n"
				    "You are advised to avoid using 'drain_uart_sync' boot-arg.\n");
			}
		}
	}
	if (kern_feature_override(KF_SERIAL_OVRD)) {
		serialmode = 0;
	}

	if (serialmode & SERIALMODE_OUTPUT) {                 /* Start serial if requested */
		(void)switch_to_serial_console(); /* Switch into serial mode */
		disableConsoleOutput = FALSE;     /* Allow printfs to happen */
	}
	/*
	 * MT6580 bring-up: use a direct framebuffer text console instead
	 * of PE_create_console()/vcattach().  The generic console's
	 * PE_state.video depth/pitch does not match the visible layer yet,
	 * so it scribbles over the panel.  The direct writer uses the known
	 * 452x960, 32-bpp, 1808-byte-pitch BOOT_MENU_LAYER.
	 */
	mt6580_fb_clear(0xFF000000);
#if defined(ARM_BOARD_CLASS_MT6580)
	/*
	 * The VM now has kernel_map, so io_map_spec() can map device pages.
	 * Enable watchdog petting before the first marker hold, otherwise
	 * the ~2s of deliberate delay trips LK's TOPRGU timeout.
	 */
	mt6580_wdt_ready = TRUE;
	/*
	 * Make panics visible on the panel.  kprintf() output is a no-op
	 * here, so without this a panic in e.g. kalloc_init() looks
	 * identical to a hang.
	 */
	PE_arm_debug_panic_hook = mt6580_panic_display;
#endif
	mt6580_fb_text(0, 0, "MT6580 XNU 4570.71.2", 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_text(0, 1, "direct framebuffer console", 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_wdt_status();
	mt6580_fb_text(0, 3, "arm_init entered", 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_text(0, 4, "platform(FALSE) ok", 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_text(0, 5, "arm_vm_init ok", 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_text(0, 6, "PE_init_kprintf ok", 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_text(0, 8, "waiting for platform(TRUE)", 0xFFFFFFFF, 0xFF000000);

#if __arm64__
#if DEBUG
	dump_kva_space();
#endif
#endif

	cpu_machine_idle_init(TRUE);

#if	(__ARM_ARCH__ == 7)
	if (arm_diag & 0x8000)
		set_mmu_control((get_mmu_control()) ^ SCTLR_PREDIC);
#endif

	PE_init_platform(TRUE, &BootCpuData);
	mt6580_fb_text(0, 9, "platform(TRUE) ok", 0xFFFFFFFF, 0xFF000000);
	cpu_timebase_init(TRUE);
	mt6580_fb_text(0, 10, "timebase ok", 0xFFFFFFFF, 0xFF000000);
	fiq_context_init(TRUE);
	mt6580_fb_text(0, 11, "fiq ok", 0xFFFFFFFF, 0xFF000000);


	/*
	 * Initialize the stack protector for all future calls
	 * to C code. Since kernel_bootstrap() eventually
	 * switches stack context without returning through this
	 * function, we do not risk failing the check even though
	 * we mutate the guard word during execution.
	 */
	/*
	 * Bring-up: early_random() pulls in the corecrypto DRBG and is the
	 * next thing after fiq_context_init().  Use a fixed nonzero guard
	 * until the PRNG path is verified, so it cannot mask the real
	 * machine_startup milestone.
	 */
	mt6580_fb_text(0, 12, "setting stack guard", 0xFFFFFFFF, 0xFF000000);
	__stack_chk_guard = 0x5A5A1234UL;
	__stack_chk_guard &= ~(0xFFULL << 8);
	mt6580_fb_text(0, 13, "stack guard ok", 0xFFFFFFFF, 0xFF000000);
	mt6580_fb_text(0, 14, "machine_startup", 0xFFFFFFFF, 0xFF000000);
	machine_startup(args);
}

/*
 * Routine:        arm_init_cpu
 * Function:
 *    Re-initialize CPU when coming out of reset
 */

void
arm_init_cpu(
	cpu_data_t	*cpu_data_ptr)
{
#if __ARM_PAN_AVAILABLE__
	__builtin_arm_wsr("pan", 1);
#endif

	cpu_data_ptr->cpu_flags &= ~SleepState;
#if     __ARM_SMP__ && defined(ARMA7)
	cpu_data_ptr->cpu_CLW_active = 1;
#endif

	machine_set_current_thread(cpu_data_ptr->cpu_active_thread);

#if __arm64__
	/* Enable asynchronous exceptions */
        __builtin_arm_wsr("DAIFClr", DAIFSC_ASYNCF);
#endif

	cpu_machine_idle_init(FALSE);

	cpu_init();

#if	(__ARM_ARCH__ == 7)
	if (arm_diag & 0x8000)
		set_mmu_control((get_mmu_control()) ^ SCTLR_PREDIC);
#endif
#ifdef	APPLETYPHOON
	if ((cpus_defeatures & (0xF << 4*cpu_data_ptr->cpu_number)) != 0)
		cpu_defeatures_set((cpus_defeatures >> 4*cpu_data_ptr->cpu_number) & 0xF);
#endif
	/* Initialize the timebase before serial_init, as some serial
	 * drivers use mach_absolute_time() to implement rate control
	 */
	cpu_timebase_init(FALSE);

	if (cpu_data_ptr == &BootCpuData) {
#if __arm64__ && __ARM_GLOBAL_SLEEP_BIT__
		/*
		 * Prevent CPUs from going into deep sleep until all
		 * CPUs are ready to do so.
		 */
		arm64_stall_sleep = TRUE;
#endif
		serial_init();
		PE_init_platform(TRUE, NULL);
		commpage_update_timebase();
	}

	fiq_context_init(TRUE);
	cpu_data_ptr->rtcPop = EndOfAllTime;
	timer_resync_deadlines();

#if DEVELOPMENT || DEBUG
	PE_arm_debug_enable_trace();
#endif

	kprintf("arm_cpu_init(): cpu %d online\n", cpu_data_ptr->cpu_processor->cpu_id);

	if (cpu_data_ptr == &BootCpuData) {
#if CONFIG_TELEMETRY
		bootprofile_wake_from_sleep();
#endif /* CONFIG_TELEMETRY */
#if MONOTONIC && defined(__arm64__)
		mt_wake();
#endif /* MONOTONIC && defined(__arm64__) */
	}

	slave_main(NULL);
}

/*
 * Routine:        arm_init_idle_cpu
 * Function:
 */
void __attribute__((noreturn))
arm_init_idle_cpu(
	cpu_data_t	*cpu_data_ptr)
{
#if __ARM_PAN_AVAILABLE__
	__builtin_arm_wsr("pan", 1);
#endif
#if     __ARM_SMP__ && defined(ARMA7)
	cpu_data_ptr->cpu_CLW_active = 1;
#endif

	machine_set_current_thread(cpu_data_ptr->cpu_active_thread);

#if __arm64__
	/* Enable asynchronous exceptions */
        __builtin_arm_wsr("DAIFClr", DAIFSC_ASYNCF);
#endif

#if	(__ARM_ARCH__ == 7)
	if (arm_diag & 0x8000)
		set_mmu_control((get_mmu_control()) ^ SCTLR_PREDIC);
#endif
#ifdef	APPLETYPHOON
	if ((cpus_defeatures & (0xF << 4*cpu_data_ptr->cpu_number)) != 0)
		cpu_defeatures_set((cpus_defeatures >> 4*cpu_data_ptr->cpu_number) & 0xF);
#endif

	fiq_context_init(FALSE);

	cpu_idle_exit();
}
