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
#define MT6580_FB_MARK_Y	832u

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

	kprintf("kprintf initialized\n");

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
	PE_create_console();

	/* setup console output */
	PE_init_printf(FALSE);

	/*
	 * MT6580 bring-up: PE_create_console() only initializes the
	 * framebuffer console.  vcattach() (via PE_init_printf(TRUE)) is
	 * what allocates the text buffers and enables output.  Switch to
	 * text mode first so vcattach() takes the graphics-console path.
	 */
	initialize_screen(NULL, kPETextMode);
	PE_init_printf(TRUE);
	mt6580_fb_mark(0xFFFF00FF, 0, FALSE);	/* console enabled */
	mt6580_fb_mark(0xFFFFFFFF, 1, FALSE);	/* PE_init_printf done */

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
	mt6580_fb_mark(0xFF808080, 2, FALSE);	/* PE_init_platform(TRUE) done */
	cpu_timebase_init(TRUE);
	mt6580_fb_mark(0xFF00FF80, 3, FALSE);	/* cpu_timebase_init done */
	fiq_context_init(TRUE);
	mt6580_fb_mark(0xFF800000, 4, FALSE);	/* fiq_context_init done */


	/*
	 * Initialize the stack protector for all future calls
	 * to C code. Since kernel_bootstrap() eventually
	 * switches stack context without returning through this
	 * function, we do not risk failing the check even though
	 * we mutate the guard word during execution.
	 */
	__stack_chk_guard = (unsigned long)early_random();
	/* Zero a byte of the protector to guard
	 * against string vulnerabilities
	 */
	__stack_chk_guard &= ~(0xFFULL << 8);
	mt6580_fb_mark(0xFF008080, 5, FALSE);	/* stack protector ready */
	mt6580_fb_mark(0xFFFFFF00, 6, FALSE);	/* entering machine_startup */
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
