#include "libcflat.h"
#include "apic.h"
#include "vm.h"
#include "smp.h"
#include "desc.h"
#include "isr.h"
#include "msr.h"
#include "atomic.h"
#include "fwcfg.h"

#define MAX_TPR			0xf

static bool is_apic_hw_enabled(void)
{
	return rdmsr(MSR_IA32_APICBASE) & APIC_EN;
}

static bool is_apic_sw_enabled(void)
{
	return apic_read(APIC_SPIV) & APIC_SPIV_APIC_ENABLED;
}

static bool is_x2apic_enabled(void)
{
	return (rdmsr(MSR_IA32_APICBASE) & (APIC_EN | APIC_EXTD)) == (APIC_EN | APIC_EXTD);
}

static bool is_xapic_enabled(void)
{
	return (rdmsr(MSR_IA32_APICBASE) & (APIC_EN | APIC_EXTD)) == APIC_EN;
}

static void test_lapic_existence(void)
{
	u8 version;

	version = (u8)apic_read(APIC_LVR);
	printf("apic version: %x\n", version);
	report(version >= 0x10 && version <= 0x15, "apic existence");
}

#define TSC_DEADLINE_TIMER_VECTOR 0xef
#define BROADCAST_VECTOR 0xcf

static int tdt_count;

static void tsc_deadline_timer_isr(isr_regs_t *regs)
{
	++tdt_count;
	eoi();
}

static void __test_tsc_deadline_timer(void)
{
	handle_irq(TSC_DEADLINE_TIMER_VECTOR, tsc_deadline_timer_isr);

	wrmsr(MSR_IA32_TSCDEADLINE, rdmsr(MSR_IA32_TSC));
	asm volatile ("nop");
	report(tdt_count == 1, "tsc deadline timer");
	report(rdmsr(MSR_IA32_TSCDEADLINE) == 0, "tsc deadline timer clearing");
}

static int enable_tsc_deadline_timer(void)
{
	uint32_t lvtt;

	if (this_cpu_has(X86_FEATURE_TSC_DEADLINE_TIMER)) {
		lvtt = APIC_LVT_TIMER_TSCDEADLINE | TSC_DEADLINE_TIMER_VECTOR;
		apic_write(APIC_LVTT, lvtt);
		return 1;
	} else {
		return 0;
	}
}

static void test_tsc_deadline_timer(void)
{
	if(enable_tsc_deadline_timer())
		__test_tsc_deadline_timer();
	else
		report_skip("tsc deadline timer not detected");
}

static void do_write_apicbase(void *data)
{
	wrmsr(MSR_IA32_APICBASE, *(u64 *)data);
}

static bool test_write_apicbase_exception(u64 data)
{
	return test_for_exception(GP_VECTOR, do_write_apicbase, &data);
}

static void test_enable_x2apic(void)
{
	u64 apicbase = rdmsr(MSR_IA32_APICBASE);

	if (enable_x2apic()) {
		printf("x2apic enabled\n");

		apicbase &= ~(APIC_EN | APIC_EXTD);
		report(test_write_apicbase_exception(apicbase | APIC_EXTD),
			"x2apic enabled to invalid state");
		report(test_write_apicbase_exception(apicbase | APIC_EN),
			"x2apic enabled to apic enabled");

		report(!test_write_apicbase_exception(apicbase | 0),
			"x2apic enabled to disabled state");
		report(test_write_apicbase_exception(apicbase | APIC_EXTD),
			"disabled to invalid state");
		report(test_write_apicbase_exception(apicbase | APIC_EN | APIC_EXTD),
			"disabled to x2apic enabled");

		report(!test_write_apicbase_exception(apicbase | APIC_EN),
			"apic disabled to apic enabled");
		report(test_write_apicbase_exception(apicbase | APIC_EXTD),
			"apic enabled to invalid state");
	} else {
		printf("x2apic not detected\n");

		report(test_write_apicbase_exception(APIC_EN | APIC_EXTD),
		       "enable unsupported x2apic");
	}
}

static void verify_disabled_apic_mmio(void)
{
	volatile u32 *lvr = (volatile u32 *)(APIC_DEFAULT_PHYS_BASE + APIC_LVR);
	volatile u32 *tpr = (volatile u32 *)(APIC_DEFAULT_PHYS_BASE + APIC_TASKPRI);
	u32 cr8 = read_cr8();

	memset((void *)APIC_DEFAULT_PHYS_BASE, 0xff, PAGE_SIZE);
	report(*lvr == ~0, "*0xfee00030: %x", *lvr);
	report(read_cr8() == cr8, "CR8: %lx", read_cr8());
	write_cr8(cr8 ^ MAX_TPR);
	report(read_cr8() == (cr8 ^ MAX_TPR), "CR8: %lx", read_cr8());
	report(*tpr == ~0, "*0xfee00080: %x", *tpr);
	write_cr8(cr8);
}

static void test_apic_disable(void)
{
	volatile u32 *lvr = (volatile u32 *)(APIC_DEFAULT_PHYS_BASE + APIC_LVR);
	volatile u32 *tpr = (volatile u32 *)(APIC_DEFAULT_PHYS_BASE + APIC_TASKPRI);
	u32 apic_version = apic_read(APIC_LVR);
	u32 cr8 = read_cr8();

	report_prefix_push("apic_disable");

	disable_apic();
	report(!is_apic_hw_enabled(), "Local apic disabled");
	report(!this_cpu_has(X86_FEATURE_APIC),
	       "CPUID.1H:EDX.APIC[bit 9] is clear");
	verify_disabled_apic_mmio();

	reset_apic();
	report(is_xapic_enabled(), "Local apic enabled in xAPIC mode");
	report(this_cpu_has(X86_FEATURE_APIC), "CPUID.1H:EDX.APIC[bit 9] is set");
	report(*lvr == apic_version, "*0xfee00030: %x", *lvr);
	report(*tpr == cr8, "*0xfee00080: %x", *tpr);
	write_cr8(cr8 ^ MAX_TPR);
	report(*tpr == (cr8 ^ MAX_TPR) << 4, "*0xfee00080: %x", *tpr);
	write_cr8(cr8);

	if (enable_x2apic()) {
		report(is_x2apic_enabled(), "Local apic enabled in x2APIC mode");
		report(this_cpu_has(X86_FEATURE_APIC),
		       "CPUID.1H:EDX.APIC[bit 9] is set");
		verify_disabled_apic_mmio();
	}
	report_prefix_pop();
}

#define ALTERNATE_APIC_BASE	0xfed40000

static void test_apicbase(void)
{
	u64 orig_apicbase = rdmsr(MSR_IA32_APICBASE);
	u32 lvr = apic_read(APIC_LVR);
	u64 value;

	wrmsr(MSR_IA32_APICBASE, orig_apicbase & ~(APIC_EN | APIC_EXTD));
	wrmsr(MSR_IA32_APICBASE, ALTERNATE_APIC_BASE | APIC_BSP | APIC_EN);

	report_prefix_push("apicbase");

	report(*(volatile u32 *)(ALTERNATE_APIC_BASE + APIC_LVR) == lvr,
	       "relocate apic");

	value = orig_apicbase | (1UL << cpuid_maxphyaddr());
	report(test_for_exception(GP_VECTOR, do_write_apicbase, &value),
	       "reserved physaddr bits");

	value = orig_apicbase | 1;
	report(test_for_exception(GP_VECTOR, do_write_apicbase, &value),
	       "reserved low bits");

	/* Restore the APIC address, the "reset" helpers leave it as is. */
	wrmsr(MSR_IA32_APICBASE, orig_apicbase);

	report_prefix_pop();
}

static void do_write_apic_id(void *id)
{
	apic_write(APIC_ID, *(u32 *)id);
}

static void __test_apic_id(void * unused)
{
	u32 id, newid;
	u8  initial_xapic_id = cpuid(1).b >> 24;
	u32 initial_x2apic_id = cpuid(0xb).d;
	bool x2apic_mode = is_x2apic_enabled();

	if (x2apic_mode)
		reset_apic();

	id = apic_id();
	report(initial_xapic_id == id, "xapic id matches cpuid");

	newid = (id + 1) << 24;
	report(!test_for_exception(GP_VECTOR, do_write_apic_id, &newid) &&
	       (id == apic_id() || id + 1 == apic_id()),
	       "writeable xapic id");

	if (!enable_x2apic())
		goto out;

	report(test_for_exception(GP_VECTOR, do_write_apic_id, &newid),
	       "non-writeable x2apic id");
	report(initial_xapic_id == (apic_id() & 0xff), "sane x2apic id");

	/* old QEMUs do not set initial x2APIC ID */
	report(initial_xapic_id == (initial_x2apic_id & 0xff) && 
	       initial_x2apic_id == apic_id(),
	       "x2apic id matches cpuid");

out:
	reset_apic();

	report(initial_xapic_id == apic_id(), "correct xapic id after reset");

	/* old KVMs do not reset xAPIC ID */
	if (id != apic_id())
		apic_write(APIC_ID, id << 24);

	if (x2apic_mode)
		enable_x2apic();
}

static void test_apic_id(void)
{
	if (cpu_count() < 2)
		return;

	on_cpu(1, __test_apic_id, NULL);
}

static atomic_t ipi_count;

static void handle_ipi(isr_regs_t *regs)
{
	atomic_inc(&ipi_count);
	eoi();
}

static void __test_self_ipi(void)
{
	u64 start = rdtsc();
	int vec = 0xf1;

	handle_irq(vec, handle_ipi);
	apic_icr_write(APIC_DEST_SELF | APIC_DEST_PHYSICAL | APIC_DM_FIXED | vec,
		       id_map[0]);

	do {
		pause();
	} while (rdtsc() - start < 1000000000 && atomic_read(&ipi_count) == 0);
}

static void test_self_ipi_xapic(void)
{
	report_prefix_push("self_ipi_xapic");

	/* Reset to xAPIC mode. */
	reset_apic();
	report(is_xapic_enabled(), "Local apic enabled in xAPIC mode");

	atomic_set(&ipi_count, 0);
	__test_self_ipi();
	report(atomic_read(&ipi_count) == 1, "self ipi");

	report_prefix_pop();
}

static void test_self_ipi_x2apic(void)
{
	report_prefix_push("self_ipi_x2apic");

	if (enable_x2apic()) {
		report(is_x2apic_enabled(), "Local apic enabled in x2APIC mode");

		atomic_set(&ipi_count, 0);
		__test_self_ipi();
		report(atomic_read(&ipi_count) == 1, "self ipi");
	} else {
		report_skip("x2apic not detected");
	}

	report_prefix_pop();
}

volatile int nmi_counter_private, nmi_counter, nmi_hlt_counter, sti_loop_active;

static void test_sti_nop(char *p)
{
	asm volatile (
		  ".globl post_sti \n\t"
		  "sti \n"
		  /*
		   * vmx won't exit on external interrupt if blocked-by-sti,
		   * so give it a reason to exit by accessing an unmapped page.
		   */
		  "post_sti: testb $0, %0 \n\t"
		  "nop \n\t"
		  "cli"
		  : : "m"(*p)
		  );
	nmi_counter = nmi_counter_private;
}

static void sti_loop(void *ignore)
{
	unsigned k = 0;

	while (sti_loop_active)
		test_sti_nop((char *)(ulong)((k++ * 4096) % (128 * 1024 * 1024)));
}

static void nmi_handler(isr_regs_t *regs)
{
	extern void post_sti(void);
	++nmi_counter_private;
	nmi_hlt_counter += regs->rip == (ulong)post_sti;
}

static void test_sti_nmi(void)
{
	unsigned old_counter;

	if (cpu_count() < 2)
		return;

	handle_irq(2, nmi_handler);
	on_cpu(1, update_cr3, (void *)read_cr3());

	sti_loop_active = 1;
	on_cpu_async(1, sti_loop, 0);
	while (nmi_counter < 30000) {
		old_counter = nmi_counter;
		apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_NMI | APIC_INT_ASSERT, id_map[1]);
		while (nmi_counter == old_counter)
			;
	}
	sti_loop_active = 0;
	report(nmi_hlt_counter == 0, "nmi-after-sti");
}

static volatile bool nmi_done, nmi_flushed;
static volatile int nmi_received;
static volatile int cpu0_nmi_ctr1, cpu1_nmi_ctr1;
static volatile int cpu0_nmi_ctr2, cpu1_nmi_ctr2;

static void multiple_nmi_handler(isr_regs_t *regs)
{
	++nmi_received;
}

static void kick_me_nmi(void *blah)
{
	while (!nmi_done) {
		++cpu1_nmi_ctr1;
		while (cpu1_nmi_ctr1 != cpu0_nmi_ctr1 && !nmi_done)
			pause();

		if (nmi_done)
			return;

		apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_NMI | APIC_INT_ASSERT, id_map[0]);
		/* make sure the NMI has arrived by sending an IPI after it */
		apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_FIXED | APIC_INT_ASSERT
				| 0x44, id_map[0]);
		++cpu1_nmi_ctr2;
		while (cpu1_nmi_ctr2 != cpu0_nmi_ctr2 && !nmi_done)
			pause();
	}
}

static void flush_nmi(isr_regs_t *regs)
{
	nmi_flushed = true;
	apic_write(APIC_EOI, 0);
}

static void test_multiple_nmi(void)
{
	int i;
	bool ok = true;

	if (cpu_count() < 2)
		return;

	sti();
	handle_irq(2, multiple_nmi_handler);
	handle_irq(0x44, flush_nmi);
	on_cpu_async(1, kick_me_nmi, 0);
	for (i = 0; i < 100000; ++i) {
		nmi_flushed = false;
		nmi_received = 0;
		++cpu0_nmi_ctr1;
		while (cpu1_nmi_ctr1 != cpu0_nmi_ctr1)
			pause();

		apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_NMI | APIC_INT_ASSERT, id_map[0]);
		while (!nmi_flushed)
			pause();

		if (nmi_received != 2) {
			ok = false;
			break;
		}

		++cpu0_nmi_ctr2;
		while (cpu1_nmi_ctr2 != cpu0_nmi_ctr2)
			pause();
	}
	nmi_done = true;
	report(ok, "multiple nmi");
}

static void pending_nmi_handler(isr_regs_t *regs)
{
	int i;

	if (++nmi_received == 1) {
		for (i = 0; i < 10; ++i)
			apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_NMI, 0);
	}
}

static void test_pending_nmi(void)
{
	int i;

	handle_irq(2, pending_nmi_handler);
	for (i = 0; i < 100000; ++i) {
		nmi_received = 0;

		apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_NMI, 0);
		while (nmi_received < 2)
			pause();

		if (nmi_received != 2)
			break;
	}
	report(nmi_received == 2, "pending nmi");
}

static volatile int lvtt_counter = 0;

static void lvtt_handler(isr_regs_t *regs)
{
	lvtt_counter++;
	eoi();
}

static void test_apic_timer_one_shot(void)
{
	uint64_t tsc1, tsc2;
	static const uint32_t interval = 0x10000;

#define APIC_LVT_TIMER_VECTOR    (0xee)

	handle_irq(APIC_LVT_TIMER_VECTOR, lvtt_handler);

	/* One shot mode */
	apic_write(APIC_LVTT, APIC_LVT_TIMER_ONESHOT |
		   APIC_LVT_TIMER_VECTOR);
	/* Divider == 1 */
	apic_write(APIC_TDCR, 0x0000000b);

	tsc1 = rdtsc();
	/* Set "Initial Counter Register", which starts the timer */
	apic_write(APIC_TMICT, interval);
	while (!lvtt_counter);
	tsc2 = rdtsc();

	/*
	 * For LVT Timer clock, SDM vol 3 10.5.4 says it should be
	 * derived from processor's bus clock (IIUC which is the same
	 * as TSC), however QEMU seems to be using nanosecond. In all
	 * cases, the following should satisfy on all modern
	 * processors.
	 */
	report((lvtt_counter == 1) && (tsc2 - tsc1 >= interval),
	       "APIC LVT timer one shot");
}

static atomic_t broadcast_counter;

static void broadcast_handler(isr_regs_t *regs)
{
	atomic_inc(&broadcast_counter);
	eoi();
}

static bool broadcast_received(unsigned ncpus)
{
	unsigned counter;
	u64 start = rdtsc();

	do {
		counter = atomic_read(&broadcast_counter);
		if (counter >= ncpus)
			break;
		pause();
	} while (rdtsc() - start < 1000000000);

	atomic_set(&broadcast_counter, 0);

	return counter == ncpus;
}

static void test_physical_broadcast(void)
{
	unsigned ncpus = cpu_count();
	unsigned long cr3 = read_cr3();
	u32 broadcast_address = enable_x2apic() ? 0xffffffff : 0xff;

	handle_irq(BROADCAST_VECTOR, broadcast_handler);
	for (int c = 1; c < ncpus; c++)
		on_cpu(c, update_cr3, (void *)cr3);

	printf("starting broadcast (%s)\n", enable_x2apic() ? "x2apic" : "xapic");
	apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_FIXED | APIC_INT_ASSERT |
		       BROADCAST_VECTOR, broadcast_address);
	report(broadcast_received(ncpus), "APIC physical broadcast address");

	apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_FIXED | APIC_INT_ASSERT |
		       BROADCAST_VECTOR | APIC_DEST_ALLINC, 0);
	report(broadcast_received(ncpus), "APIC physical broadcast shorthand");
}

static void wait_until_tmcct_common(uint32_t initial_count, bool stop_when_half, bool should_wrap_around)
{
	uint32_t tmcct = apic_read(APIC_TMCCT);

	if (tmcct) {
		while (tmcct > (initial_count / 2))
			tmcct = apic_read(APIC_TMCCT);

		if ( stop_when_half )
			return;

		/* Wait until the counter reach 0 or wrap-around */
		while ( tmcct <= (initial_count / 2) && tmcct > 0 )
			tmcct = apic_read(APIC_TMCCT);

		/* Wait specifically for wrap around to skip 0 TMCCR if we were asked to */
		while (should_wrap_around && !tmcct)
			tmcct = apic_read(APIC_TMCCT);
	}
}

static void wait_until_tmcct_is_zero(uint32_t initial_count, bool stop_when_half)
{
	return wait_until_tmcct_common(initial_count, stop_when_half, false);
}

static void wait_until_tmcct_wrap_around(uint32_t initial_count, bool stop_when_half)
{
	return wait_until_tmcct_common(initial_count, stop_when_half, true);
}

static inline void apic_change_mode(unsigned long new_mode)
{
	uint32_t lvtt;

	lvtt = apic_read(APIC_LVTT);
	apic_write(APIC_LVTT, (lvtt & ~APIC_LVT_TIMER_MASK) | new_mode);
}

static void test_apic_change_mode(void)
{
	uint32_t tmict = 0x999999;

	printf("starting apic change mode\n");

	apic_write(APIC_TMICT, tmict);

	apic_change_mode(APIC_LVT_TIMER_PERIODIC);

	report(apic_read(APIC_TMICT) == tmict, "TMICT value reset");

	/* Testing one-shot */
	apic_change_mode(APIC_LVT_TIMER_ONESHOT);
	apic_write(APIC_TMICT, tmict);
	report(apic_read(APIC_TMCCT), "TMCCT should have a non-zero value");

	wait_until_tmcct_is_zero(tmict, false);
	report(!apic_read(APIC_TMCCT), "TMCCT should have reached 0");

	/*
	 * Write TMICT before changing mode from one-shot to periodic TMCCT should
	 * be reset to TMICT periodicly
	 */
	apic_write(APIC_TMICT, tmict);
	wait_until_tmcct_is_zero(tmict, true);
	apic_change_mode(APIC_LVT_TIMER_PERIODIC);
	report(apic_read(APIC_TMCCT), "TMCCT should have a non-zero value");

	/*
	 * After the change of mode, the counter should not be reset and continue
	 * counting down from where it was
	 */
	report(apic_read(APIC_TMCCT) < (tmict / 2),
	       "TMCCT should not be reset to TMICT value");
	/*
	 * Specifically wait for timer wrap around and skip 0.
	 * Under KVM lapic there is a possibility that a small amount of consecutive
	 * TMCCR reads return 0 while hrtimer is reset in an async callback
	 */
	wait_until_tmcct_wrap_around(tmict, false);
	report(apic_read(APIC_TMCCT) > (tmict / 2),
	       "TMCCT should be reset to the initial-count");

	wait_until_tmcct_is_zero(tmict, true);
	/*
	 * Keep the same TMICT and change timer mode to one-shot
	 * TMCCT should be > 0 and count-down to 0
	 */
	apic_change_mode(APIC_LVT_TIMER_ONESHOT);
	report(apic_read(APIC_TMCCT) < (tmict / 2),
	       "TMCCT should not be reset to init");
	wait_until_tmcct_is_zero(tmict, false);
	report(!apic_read(APIC_TMCCT), "TMCCT should have reach zero");

	/* now tmcct == 0 and tmict != 0 */
	apic_change_mode(APIC_LVT_TIMER_PERIODIC);
	report(!apic_read(APIC_TMCCT), "TMCCT should stay at zero");
}

#define KVM_HC_SEND_IPI 10

static void test_pv_ipi(void)
{
	int ret;
	unsigned long a0 = 0xFFFFFFFF, a1 = 0, a2 = 0xFFFFFFFF, a3 = 0x0;

	if (!test_device_enabled())
		return;

	asm volatile("vmcall" : "=a"(ret) :"a"(KVM_HC_SEND_IPI), "b"(a0), "c"(a1), "d"(a2), "S"(a3));
	report(!ret, "PV IPIs testing");
}

#define APIC_LDR_CLUSTER_FLAG	BIT(31)

static void set_ldr(void *__ldr)
{
	u32 ldr = (unsigned long)__ldr;

	if (ldr & APIC_LDR_CLUSTER_FLAG)
		apic_write(APIC_DFR, APIC_DFR_CLUSTER);
	else
		apic_write(APIC_DFR, APIC_DFR_FLAT);

	apic_write(APIC_LDR, ldr << 24);
}

static int test_fixed_ipi(u32 dest_mode, u8 dest, u8 vector,
			  int nr_ipis_expected, const char *mode_name)
{
	u64 start = rdtsc();
	int got;

	atomic_set(&ipi_count, 0);

	/*
	 * Wait for vCPU1 to get back into HLT, i.e. into the host so that
	 * KVM must handle incomplete AVIC IPIs.
	 */
	do {
		pause();
	} while (rdtsc() - start < 1000000);

	start = rdtsc();

	apic_icr_write(dest_mode | APIC_DM_FIXED | vector, dest);

	do {
		pause();
	} while (rdtsc() - start < 1000000000 &&
		 atomic_read(&ipi_count) != nr_ipis_expected);

	/* Only report failures to cut down on the spam. */
	got = atomic_read(&ipi_count);
	if (got != nr_ipis_expected)
		report_fail("Want %d IPI(s) using %s mode, dest = %x, got %d IPI(s)",
			    nr_ipis_expected, mode_name, dest, got);
	atomic_set(&ipi_count, 0);

	return got == nr_ipis_expected ? 0 : 1;
}

static int test_logical_ipi_single_target(u8 logical_id, bool cluster, u8 dest,
					  u8 vector)
{
	/* Disallow broadcast, there are at least 2 vCPUs. */
	if (dest == 0xff)
		return 0;

	set_ldr((void *)0);
	on_cpu(1, set_ldr,
	       (void *)((u32)logical_id | (cluster ? APIC_LDR_CLUSTER_FLAG : 0)));
	return test_fixed_ipi(APIC_DEST_LOGICAL, dest, vector, 1,
			      cluster ? "logical cluster" : "logical flat");
}

static int test_logical_ipi_multi_target(u8 vcpu0_logical_id, u8 vcpu1_logical_id,
					 bool cluster, u8 dest, u8 vector)
{
	/* Allow broadcast unless there are more than 2 vCPUs. */
	if (dest == 0xff && cpu_count() > 2)
		return 0;

	set_ldr((void *)((u32)vcpu0_logical_id | (cluster ? APIC_LDR_CLUSTER_FLAG : 0)));
	on_cpu(1, set_ldr,
	       (void *)((u32)vcpu1_logical_id | (cluster ? APIC_LDR_CLUSTER_FLAG : 0)));
	return test_fixed_ipi(APIC_DEST_LOGICAL, dest, vector, 2,
			      cluster ? "logical cluster" : "logical flat");
}

static void test_logical_ipi_xapic(void)
{
	int c, i, j, k, f;
	u8 vector = 0xf1;

	if (cpu_count() < 2)
		return;

	/*
	 * All vCPUs must be in xAPIC mode, i.e. simply resetting this vCPUs
	 * APIC is not sufficient.
	 */
	if (is_x2apic_enabled())
		return;

	handle_irq(vector, handle_ipi);

	/* Flat mode.  8 bits for logical IDs (one per bit). */
	f = 0;
	for (i = 0; i < 8; i++) {
		/*
		 * Test all possible destination values.  Non-existent targets
		 * should be ignored.  vCPU is always targeted, i.e. should get
		 * an IPI.
		 */
		for (k = 0; k < 0xff; k++) {
			/*
			 * Skip values that overlap the actual target the
			 * resulting combination will be covered by other
			 * numbers in the sequence.
			 */
			if (BIT(i) & k)
				continue;

			f += test_logical_ipi_single_target(BIT(i), false,
							    BIT(i) | k, vector);
		}
	}
	report(!f, "IPI to single target using logical flat mode");

	/* Cluster mode.  4 bits for the cluster, 4 bits for logical IDs. */
	f = 0;
	for (c = 0; c < 0xf; c++) {
		for (i = 0; i < 4; i++) {
			/* Same as above, just fewer bits... */
			for (k = 0; k < 0x10; k++) {
				if (BIT(i) & k)
					continue;

				test_logical_ipi_single_target(c << 4 | BIT(i), true,
							       c << 4 | BIT(i) | k, vector);
			}
		}
	}
	report(!f, "IPI to single target using logical cluster mode");

	/* And now do it all over again targeting both vCPU0 and vCPU1. */
	f = 0;
	for (i = 0; i < 8 && !f; i++) {
		for (j = 0; j < 8 && !f; j++) {
			if (i == j)
				continue;

			for (k = 0; k < 0x100 && !f; k++) {
				if ((BIT(i) | BIT(j)) & k)
					continue;

				f += test_logical_ipi_multi_target(BIT(i), BIT(j), false,
								   BIT(i) | BIT(j) | k, vector);
				if (f)
					break;
				f += test_logical_ipi_multi_target(BIT(i) | BIT(j),
								   BIT(i) | BIT(j), false,
								   BIT(i) | BIT(j) | k, vector);
			}
		}
	}
	report(!f, "IPI to multiple targets using logical flat mode");

	f = 0;
	for (c = 0; c < 0xf && !f; c++) {
		for (i = 0; i < 4 && !f; i++) {
			for (j = 0; j < 4 && !f; j++) {
				if (i == j)
					continue;

				for (k = 0; k < 0x10 && !f; k++) {
					if ((BIT(i) | BIT(j)) & k)
						continue;

					f += test_logical_ipi_multi_target(c << 4 | BIT(i),
									   c << 4 | BIT(j), true,
									   c << 4 | BIT(i) | BIT(j) | k, vector);
					if (f)
						break;
					f += test_logical_ipi_multi_target(c << 4 | BIT(i) | BIT(j),
									   c << 4 | BIT(i) | BIT(j), true,
									   c << 4 | BIT(i) | BIT(j) | k, vector);
				}
			}
		}
	}
	report(!f, "IPI to multiple targets using logical cluster mode");
}

static void set_xapic_physical_id(void *apic_id)
{
	apic_write(APIC_ID, (unsigned long)apic_id << 24);
}

static void handle_aliased_ipi(isr_regs_t *regs)
{
	u32 apic_id = apic_read(APIC_ID) >> 24;

	if (apic_id == 0xff)
		apic_id = smp_id();
	else
		apic_id++;
	apic_write(APIC_ID, (unsigned long)apic_id << 24);

	/*
	 * Handle the IPI after updating the APIC ID, as the IPI count acts as
	 * synchronization barrier before vCPU0 sends the next IPI.
	 */
	handle_ipi(regs);
}

static void test_aliased_xapic_physical_ipi(void)
{
	u8 vector = 0xf1;
	int i, f;

	if (cpu_count() < 2)
		return;

	/*
	 * All vCPUs must be in xAPIC mode, i.e. simply resetting this vCPUs
	 * APIC is not sufficient.
	 */
	if (is_x2apic_enabled())
		return;

	/*
	 * By default, KVM doesn't follow the x86 APIC architecture for aliased
	 * APIC IDs if userspace has enabled KVM_X2APIC_API_USE_32BIT_IDS.
	 * If x2APIC is supported, assume the userspace VMM has enabled 32-bit
	 * IDs and thus activated KVM's quirk.  Delete this code to run the
	 * aliasing test on x2APIC CPUs, e.g. to run it on bare metal.
	 */
	if (this_cpu_has(X86_FEATURE_X2APIC))
		return;

	handle_irq(vector, handle_aliased_ipi);

	/*
	 * Set both vCPU0 and vCPU1's APIC IDs to 0, then start the chain
	 * reaction of IPIs from APIC ID 0..255.  Each vCPU will increment its
	 * APIC ID in the handler, and then "reset" to its original ID (using
	 * smp_id()) after the last IPI.  Using on_cpu() to set vCPU1's ID
	 * after this point won't work due to on_cpu() using physical mode.
	 */
	on_cpu(1, set_xapic_physical_id, (void *)0ul);
	set_xapic_physical_id((void *)0ul);

	f = 0;
	for (i = 0; i < 0x100; i++)
		f += test_fixed_ipi(APIC_DEST_PHYSICAL, i, vector, 2, "physical");

	report(!f, "IPI to aliased xAPIC physical IDs");
}

typedef void (*apic_test_fn)(void);

int main(void)
{
	bool is_x2apic = is_x2apic_enabled();
	u32 spiv = apic_read(APIC_SPIV);
	int i;

	const apic_test_fn tests[] = {
		test_lapic_existence,

		test_apic_disable,
		test_enable_x2apic,

		test_self_ipi_xapic,
		test_self_ipi_x2apic,
		test_physical_broadcast,
		test_logical_ipi_xapic,

		test_pv_ipi,

		test_sti_nmi,
		test_multiple_nmi,
		test_pending_nmi,

		test_apic_timer_one_shot,
		test_apic_change_mode,
		test_tsc_deadline_timer,

		/*
		 * KVM may disable APICv if the APIC ID and/or APIC_BASE is
		 * modified, keep these tests at the end so that the test as a
		 * whole provides coverage for APICv (when it's enabled).
		 */
		test_apic_id,
		test_apicbase,
		test_aliased_xapic_physical_ipi,
	};

	assert_msg(is_apic_hw_enabled() && is_apic_sw_enabled(),
		   "APIC should be fully enabled by startup code.");

	setup_vm();

	mask_pic_interrupts();
	sti();

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		tests[i]();

		if (is_x2apic)
			enable_x2apic();
		else
			reset_apic();

		apic_write(APIC_SPIV, spiv);
	}

	return report_summary();
}
