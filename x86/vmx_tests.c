/*
 * All test cases of nested virtualization should be in this file
 *
 * Author : Arthur Chunqi Li <yzt356@gmail.com>
 */
#include "vmx.h"
#include "msr.h"
#include "processor.h"
#include "vm.h"
#include "io.h"
#include "fwcfg.h"
#include "isr.h"
#include "apic.h"

u64 ia32_pat;
u64 ia32_efer;
void *io_bitmap_a, *io_bitmap_b;
u16 ioport;

unsigned long *pml4;
u64 eptp;
void *data_page1, *data_page2;

static inline void vmcall()
{
	asm volatile("vmcall");
}

void basic_guest_main()
{
}

int basic_exit_handler()
{
	report("Basic VMX test", 0);
	print_vmexit_info();
	return VMX_TEST_EXIT;
}

void vmenter_main()
{
	u64 rax;
	u64 rsp, resume_rsp;

	report("test vmlaunch", 1);

	asm volatile(
		"mov %%rsp, %0\n\t"
		"mov %3, %%rax\n\t"
		"vmcall\n\t"
		"mov %%rax, %1\n\t"
		"mov %%rsp, %2\n\t"
		: "=r"(rsp), "=r"(rax), "=r"(resume_rsp)
		: "g"(0xABCD));
	report("test vmresume", (rax == 0xFFFF) && (rsp == resume_rsp));
}

int vmenter_exit_handler()
{
	u64 guest_rip;
	ulong reason;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	switch (reason) {
	case VMX_VMCALL:
		if (regs.rax != 0xABCD) {
			report("test vmresume", 0);
			return VMX_TEST_VMEXIT;
		}
		regs.rax = 0xFFFF;
		vmcs_write(GUEST_RIP, guest_rip + 3);
		return VMX_TEST_RESUME;
	default:
		report("test vmresume", 0);
		print_vmexit_info();
	}
	return VMX_TEST_VMEXIT;
}

u32 preempt_scale;
volatile unsigned long long tsc_val;
volatile u32 preempt_val;
u64 saved_rip;

int preemption_timer_init()
{
	if (!(ctrl_pin_rev.clr & PIN_PREEMPT)) {
		printf("\tPreemption timer is not supported\n");
		return VMX_TEST_EXIT;
	}
	vmcs_write(PIN_CONTROLS, vmcs_read(PIN_CONTROLS) | PIN_PREEMPT);
	preempt_val = 10000000;
	vmcs_write(PREEMPT_TIMER_VALUE, preempt_val);
	preempt_scale = rdmsr(MSR_IA32_VMX_MISC) & 0x1F;

	if (!(ctrl_exit_rev.clr & EXI_SAVE_PREEMPT))
		printf("\tSave preemption value is not supported\n");

	return VMX_TEST_START;
}

void preemption_timer_main()
{
	tsc_val = rdtsc();
	if (ctrl_exit_rev.clr & EXI_SAVE_PREEMPT) {
		vmx_set_test_stage(0);
		vmcall();
		if (vmx_get_test_stage() == 1)
			vmcall();
	}
	vmx_set_test_stage(1);
	while (vmx_get_test_stage() == 1) {
		if (((rdtsc() - tsc_val) >> preempt_scale)
				> 10 * preempt_val) {
			vmx_set_test_stage(2);
			vmcall();
		}
	}
	tsc_val = rdtsc();
	asm volatile ("hlt");
	vmcall();
	vmx_set_test_stage(5);
	vmcall();
}

int preemption_timer_exit_handler()
{
	bool guest_halted;
	u64 guest_rip;
	ulong reason;
	u32 insn_len;
	u32 ctrl_exit;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	insn_len = vmcs_read(EXI_INST_LEN);
	switch (reason) {
	case VMX_PREEMPT:
		switch (vmx_get_test_stage()) {
		case 1:
		case 2:
			report("busy-wait for preemption timer",
			       ((rdtsc() - tsc_val) >> preempt_scale) >=
			       preempt_val);
			vmx_set_test_stage(3);
			vmcs_write(PREEMPT_TIMER_VALUE, preempt_val);
			return VMX_TEST_RESUME;
		case 3:
			guest_halted =
				(vmcs_read(GUEST_ACTV_STATE) == ACTV_HLT);
			report("preemption timer during hlt",
			       ((rdtsc() - tsc_val) >> preempt_scale) >=
			       preempt_val && guest_halted);
			vmx_set_test_stage(4);
			vmcs_write(PIN_CONTROLS,
				   vmcs_read(PIN_CONTROLS) & ~PIN_PREEMPT);
			vmcs_write(GUEST_ACTV_STATE, ACTV_ACTIVE);
			return VMX_TEST_RESUME;
		case 4:
			report("preemption timer with 0 value",
			       saved_rip == guest_rip);
			break;
		default:
			printf("Invalid stage.\n");
			print_vmexit_info();
			break;
		}
		break;
	case VMX_VMCALL:
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		switch (vmx_get_test_stage()) {
		case 0:
			report("Keep preemption value",
			       vmcs_read(PREEMPT_TIMER_VALUE) == preempt_val);
			vmx_set_test_stage(1);
			vmcs_write(PREEMPT_TIMER_VALUE, preempt_val);
			ctrl_exit = (vmcs_read(EXI_CONTROLS) |
				EXI_SAVE_PREEMPT) & ctrl_exit_rev.clr;
			vmcs_write(EXI_CONTROLS, ctrl_exit);
			return VMX_TEST_RESUME;
		case 1:
			report("Save preemption value",
			       vmcs_read(PREEMPT_TIMER_VALUE) < preempt_val);
			return VMX_TEST_RESUME;
		case 2:
			report("busy-wait for preemption timer", 0);
			vmx_set_test_stage(3);
			vmcs_write(PREEMPT_TIMER_VALUE, preempt_val);
			return VMX_TEST_RESUME;
		case 3:
			report("preemption timer during hlt", 0);
			vmx_set_test_stage(4);
			/* fall through */
		case 4:
			vmcs_write(PIN_CONTROLS,
				   vmcs_read(PIN_CONTROLS) | PIN_PREEMPT);
			vmcs_write(PREEMPT_TIMER_VALUE, 0);
			saved_rip = guest_rip + insn_len;
			return VMX_TEST_RESUME;
		case 5:
			report("preemption timer with 0 value (vmcall stage 5)", 0);
			break;
		default:
			// Should not reach here
			printf("ERROR : unexpected stage, %d\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		break;
	default:
		printf("Unknown exit reason, %d\n", reason);
		print_vmexit_info();
	}
	vmcs_write(PIN_CONTROLS, vmcs_read(PIN_CONTROLS) & ~PIN_PREEMPT);
	return VMX_TEST_VMEXIT;
}

void msr_bmp_init()
{
	void *msr_bitmap;
	u32 ctrl_cpu0;

	msr_bitmap = alloc_page();
	memset(msr_bitmap, 0x0, PAGE_SIZE);
	ctrl_cpu0 = vmcs_read(CPU_EXEC_CTRL0);
	ctrl_cpu0 |= CPU_MSR_BITMAP;
	vmcs_write(CPU_EXEC_CTRL0, ctrl_cpu0);
	vmcs_write(MSR_BITMAP, (u64)msr_bitmap);
}

static int test_ctrl_pat_init()
{
	u64 ctrl_ent;
	u64 ctrl_exi;

	msr_bmp_init();
	ctrl_ent = vmcs_read(ENT_CONTROLS);
	ctrl_exi = vmcs_read(EXI_CONTROLS);
	vmcs_write(ENT_CONTROLS, ctrl_ent | ENT_LOAD_PAT);
	vmcs_write(EXI_CONTROLS, ctrl_exi | (EXI_SAVE_PAT | EXI_LOAD_PAT));
	ia32_pat = rdmsr(MSR_IA32_CR_PAT);
	vmcs_write(GUEST_PAT, 0x0);
	vmcs_write(HOST_PAT, ia32_pat);
	return VMX_TEST_START;
}

static void test_ctrl_pat_main()
{
	u64 guest_ia32_pat;

	guest_ia32_pat = rdmsr(MSR_IA32_CR_PAT);
	if (!(ctrl_enter_rev.clr & ENT_LOAD_PAT))
		printf("\tENT_LOAD_PAT is not supported.\n");
	else {
		if (guest_ia32_pat != 0) {
			report("Entry load PAT", 0);
			return;
		}
	}
	wrmsr(MSR_IA32_CR_PAT, 0x6);
	vmcall();
	guest_ia32_pat = rdmsr(MSR_IA32_CR_PAT);
	if (ctrl_enter_rev.clr & ENT_LOAD_PAT) {
		if (guest_ia32_pat != ia32_pat) {
			report("Entry load PAT", 0);
			return;
		}
		report("Entry load PAT", 1);
	}
}

static int test_ctrl_pat_exit_handler()
{
	u64 guest_rip;
	ulong reason;
	u64 guest_pat;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	switch (reason) {
	case VMX_VMCALL:
		guest_pat = vmcs_read(GUEST_PAT);
		if (!(ctrl_exit_rev.clr & EXI_SAVE_PAT)) {
			printf("\tEXI_SAVE_PAT is not supported\n");
			vmcs_write(GUEST_PAT, 0x6);
		} else {
			if (guest_pat == 0x6)
				report("Exit save PAT", 1);
			else
				report("Exit save PAT", 0);
		}
		if (!(ctrl_exit_rev.clr & EXI_LOAD_PAT))
			printf("\tEXI_LOAD_PAT is not supported\n");
		else {
			if (rdmsr(MSR_IA32_CR_PAT) == ia32_pat)
				report("Exit load PAT", 1);
			else
				report("Exit load PAT", 0);
		}
		vmcs_write(GUEST_PAT, ia32_pat);
		vmcs_write(GUEST_RIP, guest_rip + 3);
		return VMX_TEST_RESUME;
	default:
		printf("ERROR : Undefined exit reason, reason = %d.\n", reason);
		break;
	}
	return VMX_TEST_VMEXIT;
}

static int test_ctrl_efer_init()
{
	u64 ctrl_ent;
	u64 ctrl_exi;

	msr_bmp_init();
	ctrl_ent = vmcs_read(ENT_CONTROLS) | ENT_LOAD_EFER;
	ctrl_exi = vmcs_read(EXI_CONTROLS) | EXI_SAVE_EFER | EXI_LOAD_EFER;
	vmcs_write(ENT_CONTROLS, ctrl_ent & ctrl_enter_rev.clr);
	vmcs_write(EXI_CONTROLS, ctrl_exi & ctrl_exit_rev.clr);
	ia32_efer = rdmsr(MSR_EFER);
	vmcs_write(GUEST_EFER, ia32_efer ^ EFER_NX);
	vmcs_write(HOST_EFER, ia32_efer ^ EFER_NX);
	return VMX_TEST_START;
}

static void test_ctrl_efer_main()
{
	u64 guest_ia32_efer;

	guest_ia32_efer = rdmsr(MSR_EFER);
	if (!(ctrl_enter_rev.clr & ENT_LOAD_EFER))
		printf("\tENT_LOAD_EFER is not supported.\n");
	else {
		if (guest_ia32_efer != (ia32_efer ^ EFER_NX)) {
			report("Entry load EFER", 0);
			return;
		}
	}
	wrmsr(MSR_EFER, ia32_efer);
	vmcall();
	guest_ia32_efer = rdmsr(MSR_EFER);
	if (ctrl_enter_rev.clr & ENT_LOAD_EFER) {
		if (guest_ia32_efer != ia32_efer) {
			report("Entry load EFER", 0);
			return;
		}
		report("Entry load EFER", 1);
	}
}

static int test_ctrl_efer_exit_handler()
{
	u64 guest_rip;
	ulong reason;
	u64 guest_efer;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	switch (reason) {
	case VMX_VMCALL:
		guest_efer = vmcs_read(GUEST_EFER);
		if (!(ctrl_exit_rev.clr & EXI_SAVE_EFER)) {
			printf("\tEXI_SAVE_EFER is not supported\n");
			vmcs_write(GUEST_EFER, ia32_efer);
		} else {
			if (guest_efer == ia32_efer)
				report("Exit save EFER", 1);
			else
				report("Exit save EFER", 0);
		}
		if (!(ctrl_exit_rev.clr & EXI_LOAD_EFER)) {
			printf("\tEXI_LOAD_EFER is not supported\n");
			wrmsr(MSR_EFER, ia32_efer ^ EFER_NX);
		} else {
			if (rdmsr(MSR_EFER) == (ia32_efer ^ EFER_NX))
				report("Exit load EFER", 1);
			else
				report("Exit load EFER", 0);
		}
		vmcs_write(GUEST_PAT, ia32_efer);
		vmcs_write(GUEST_RIP, guest_rip + 3);
		return VMX_TEST_RESUME;
	default:
		printf("ERROR : Undefined exit reason, reason = %d.\n", reason);
		break;
	}
	return VMX_TEST_VMEXIT;
}

u32 guest_cr0, guest_cr4;

static void cr_shadowing_main()
{
	u32 cr0, cr4, tmp;

	// Test read through
	vmx_set_test_stage(0);
	guest_cr0 = read_cr0();
	if (vmx_get_test_stage() == 1)
		report("Read through CR0", 0);
	else
		vmcall();
	vmx_set_test_stage(1);
	guest_cr4 = read_cr4();
	if (vmx_get_test_stage() == 2)
		report("Read through CR4", 0);
	else
		vmcall();
	// Test write through
	guest_cr0 = guest_cr0 ^ (X86_CR0_TS | X86_CR0_MP);
	guest_cr4 = guest_cr4 ^ (X86_CR4_TSD | X86_CR4_DE);
	vmx_set_test_stage(2);
	write_cr0(guest_cr0);
	if (vmx_get_test_stage() == 3)
		report("Write throuth CR0", 0);
	else
		vmcall();
	vmx_set_test_stage(3);
	write_cr4(guest_cr4);
	if (vmx_get_test_stage() == 4)
		report("Write through CR4", 0);
	else
		vmcall();
	// Test read shadow
	vmx_set_test_stage(4);
	vmcall();
	cr0 = read_cr0();
	if (vmx_get_test_stage() != 5) {
		if (cr0 == guest_cr0)
			report("Read shadowing CR0", 1);
		else
			report("Read shadowing CR0", 0);
	}
	vmx_set_test_stage(5);
	cr4 = read_cr4();
	if (vmx_get_test_stage() != 6) {
		if (cr4 == guest_cr4)
			report("Read shadowing CR4", 1);
		else
			report("Read shadowing CR4", 0);
	}
	// Test write shadow (same value with shadow)
	vmx_set_test_stage(6);
	write_cr0(guest_cr0);
	if (vmx_get_test_stage() == 7)
		report("Write shadowing CR0 (same value with shadow)", 0);
	else
		vmcall();
	vmx_set_test_stage(7);
	write_cr4(guest_cr4);
	if (vmx_get_test_stage() == 8)
		report("Write shadowing CR4 (same value with shadow)", 0);
	else
		vmcall();
	// Test write shadow (different value)
	vmx_set_test_stage(8);
	tmp = guest_cr0 ^ X86_CR0_TS;
	asm volatile("mov %0, %%rsi\n\t"
		"mov %%rsi, %%cr0\n\t"
		::"m"(tmp)
		:"rsi", "memory", "cc");
	if (vmx_get_test_stage() != 9)
		report("Write shadowing different X86_CR0_TS", 0);
	else
		report("Write shadowing different X86_CR0_TS", 1);
	vmx_set_test_stage(9);
	tmp = guest_cr0 ^ X86_CR0_MP;
	asm volatile("mov %0, %%rsi\n\t"
		"mov %%rsi, %%cr0\n\t"
		::"m"(tmp)
		:"rsi", "memory", "cc");
	if (vmx_get_test_stage() != 10)
		report("Write shadowing different X86_CR0_MP", 0);
	else
		report("Write shadowing different X86_CR0_MP", 1);
	vmx_set_test_stage(10);
	tmp = guest_cr4 ^ X86_CR4_TSD;
	asm volatile("mov %0, %%rsi\n\t"
		"mov %%rsi, %%cr4\n\t"
		::"m"(tmp)
		:"rsi", "memory", "cc");
	if (vmx_get_test_stage() != 11)
		report("Write shadowing different X86_CR4_TSD", 0);
	else
		report("Write shadowing different X86_CR4_TSD", 1);
	vmx_set_test_stage(11);
	tmp = guest_cr4 ^ X86_CR4_DE;
	asm volatile("mov %0, %%rsi\n\t"
		"mov %%rsi, %%cr4\n\t"
		::"m"(tmp)
		:"rsi", "memory", "cc");
	if (vmx_get_test_stage() != 12)
		report("Write shadowing different X86_CR4_DE", 0);
	else
		report("Write shadowing different X86_CR4_DE", 1);
}

static int cr_shadowing_exit_handler()
{
	u64 guest_rip;
	ulong reason;
	u32 insn_len;
	u32 exit_qual;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	insn_len = vmcs_read(EXI_INST_LEN);
	exit_qual = vmcs_read(EXI_QUALIFICATION);
	switch (reason) {
	case VMX_VMCALL:
		switch (vmx_get_test_stage()) {
		case 0:
			if (guest_cr0 == vmcs_read(GUEST_CR0))
				report("Read through CR0", 1);
			else
				report("Read through CR0", 0);
			break;
		case 1:
			if (guest_cr4 == vmcs_read(GUEST_CR4))
				report("Read through CR4", 1);
			else
				report("Read through CR4", 0);
			break;
		case 2:
			if (guest_cr0 == vmcs_read(GUEST_CR0))
				report("Write through CR0", 1);
			else
				report("Write through CR0", 0);
			break;
		case 3:
			if (guest_cr4 == vmcs_read(GUEST_CR4))
				report("Write through CR4", 1);
			else
				report("Write through CR4", 0);
			break;
		case 4:
			guest_cr0 = vmcs_read(GUEST_CR0) ^ (X86_CR0_TS | X86_CR0_MP);
			guest_cr4 = vmcs_read(GUEST_CR4) ^ (X86_CR4_TSD | X86_CR4_DE);
			vmcs_write(CR0_MASK, X86_CR0_TS | X86_CR0_MP);
			vmcs_write(CR0_READ_SHADOW, guest_cr0 & (X86_CR0_TS | X86_CR0_MP));
			vmcs_write(CR4_MASK, X86_CR4_TSD | X86_CR4_DE);
			vmcs_write(CR4_READ_SHADOW, guest_cr4 & (X86_CR4_TSD | X86_CR4_DE));
			break;
		case 6:
			if (guest_cr0 == (vmcs_read(GUEST_CR0) ^ (X86_CR0_TS | X86_CR0_MP)))
				report("Write shadowing CR0 (same value)", 1);
			else
				report("Write shadowing CR0 (same value)", 0);
			break;
		case 7:
			if (guest_cr4 == (vmcs_read(GUEST_CR4) ^ (X86_CR4_TSD | X86_CR4_DE)))
				report("Write shadowing CR4 (same value)", 1);
			else
				report("Write shadowing CR4 (same value)", 0);
			break;
		default:
			// Should not reach here
			printf("ERROR : unexpected stage, %d\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		return VMX_TEST_RESUME;
	case VMX_CR:
		switch (vmx_get_test_stage()) {
		case 4:
			report("Read shadowing CR0", 0);
			vmx_inc_test_stage();
			break;
		case 5:
			report("Read shadowing CR4", 0);
			vmx_inc_test_stage();
			break;
		case 6:
			report("Write shadowing CR0 (same value)", 0);
			vmx_inc_test_stage();
			break;
		case 7:
			report("Write shadowing CR4 (same value)", 0);
			vmx_inc_test_stage();
			break;
		case 8:
		case 9:
			// 0x600 encodes "mov %esi, %cr0"
			if (exit_qual == 0x600)
				vmx_inc_test_stage();
			break;
		case 10:
		case 11:
			// 0x604 encodes "mov %esi, %cr4"
			if (exit_qual == 0x604)
				vmx_inc_test_stage();
			break;
		default:
			// Should not reach here
			printf("ERROR : unexpected stage, %d\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		return VMX_TEST_RESUME;
	default:
		printf("Unknown exit reason, %d\n", reason);
		print_vmexit_info();
	}
	return VMX_TEST_VMEXIT;
}

static int iobmp_init()
{
	u32 ctrl_cpu0;

	io_bitmap_a = alloc_page();
	io_bitmap_a = alloc_page();
	memset(io_bitmap_a, 0x0, PAGE_SIZE);
	memset(io_bitmap_b, 0x0, PAGE_SIZE);
	ctrl_cpu0 = vmcs_read(CPU_EXEC_CTRL0);
	ctrl_cpu0 |= CPU_IO_BITMAP;
	ctrl_cpu0 &= (~CPU_IO);
	vmcs_write(CPU_EXEC_CTRL0, ctrl_cpu0);
	vmcs_write(IO_BITMAP_A, (u64)io_bitmap_a);
	vmcs_write(IO_BITMAP_B, (u64)io_bitmap_b);
	return VMX_TEST_START;
}

static void iobmp_main()
{
	// stage 0, test IO pass
	vmx_set_test_stage(0);
	inb(0x5000);
	outb(0x0, 0x5000);
	if (vmx_get_test_stage() != 0)
		report("I/O bitmap - I/O pass", 0);
	else
		report("I/O bitmap - I/O pass", 1);
	// test IO width, in/out
	((u8 *)io_bitmap_a)[0] = 0xFF;
	vmx_set_test_stage(2);
	inb(0x0);
	if (vmx_get_test_stage() != 3)
		report("I/O bitmap - trap in", 0);
	else
		report("I/O bitmap - trap in", 1);
	vmx_set_test_stage(3);
	outw(0x0, 0x0);
	if (vmx_get_test_stage() != 4)
		report("I/O bitmap - trap out", 0);
	else
		report("I/O bitmap - trap out", 1);
	vmx_set_test_stage(4);
	inl(0x0);
	if (vmx_get_test_stage() != 5)
		report("I/O bitmap - I/O width, long", 0);
	// test low/high IO port
	vmx_set_test_stage(5);
	((u8 *)io_bitmap_a)[0x5000 / 8] = (1 << (0x5000 % 8));
	inb(0x5000);
	if (vmx_get_test_stage() == 6)
		report("I/O bitmap - I/O port, low part", 1);
	else
		report("I/O bitmap - I/O port, low part", 0);
	vmx_set_test_stage(6);
	((u8 *)io_bitmap_b)[0x1000 / 8] = (1 << (0x1000 % 8));
	inb(0x9000);
	if (vmx_get_test_stage() == 7)
		report("I/O bitmap - I/O port, high part", 1);
	else
		report("I/O bitmap - I/O port, high part", 0);
	// test partial pass
	vmx_set_test_stage(7);
	inl(0x4FFF);
	if (vmx_get_test_stage() == 8)
		report("I/O bitmap - partial pass", 1);
	else
		report("I/O bitmap - partial pass", 0);
	// test overrun
	vmx_set_test_stage(8);
	memset(io_bitmap_a, 0x0, PAGE_SIZE);
	memset(io_bitmap_b, 0x0, PAGE_SIZE);
	inl(0xFFFF);
	if (vmx_get_test_stage() == 9)
		report("I/O bitmap - overrun", 1);
	else
		report("I/O bitmap - overrun", 0);
	vmx_set_test_stage(9);
	vmcall();
	outb(0x0, 0x0);
	report("I/O bitmap - ignore unconditional exiting",
	       vmx_get_test_stage() == 9);
	vmx_set_test_stage(10);
	vmcall();
	outb(0x0, 0x0);
	report("I/O bitmap - unconditional exiting",
	       vmx_get_test_stage() == 11);
}

static int iobmp_exit_handler()
{
	u64 guest_rip;
	ulong reason, exit_qual;
	u32 insn_len, ctrl_cpu0;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	exit_qual = vmcs_read(EXI_QUALIFICATION);
	insn_len = vmcs_read(EXI_INST_LEN);
	switch (reason) {
	case VMX_IO:
		switch (vmx_get_test_stage()) {
		case 0:
		case 1:
			vmx_inc_test_stage();
			break;
		case 2:
			if ((exit_qual & VMX_IO_SIZE_MASK) != _VMX_IO_BYTE)
				report("I/O bitmap - I/O width, byte", 0);
			else
				report("I/O bitmap - I/O width, byte", 1);
			if (!(exit_qual & VMX_IO_IN))
				report("I/O bitmap - I/O direction, in", 0);
			else
				report("I/O bitmap - I/O direction, in", 1);
			vmx_inc_test_stage();
			break;
		case 3:
			if ((exit_qual & VMX_IO_SIZE_MASK) != _VMX_IO_WORD)
				report("I/O bitmap - I/O width, word", 0);
			else
				report("I/O bitmap - I/O width, word", 1);
			if (!(exit_qual & VMX_IO_IN))
				report("I/O bitmap - I/O direction, out", 1);
			else
				report("I/O bitmap - I/O direction, out", 0);
			vmx_inc_test_stage();
			break;
		case 4:
			if ((exit_qual & VMX_IO_SIZE_MASK) != _VMX_IO_LONG)
				report("I/O bitmap - I/O width, long", 0);
			else
				report("I/O bitmap - I/O width, long", 1);
			vmx_inc_test_stage();
			break;
		case 5:
			if (((exit_qual & VMX_IO_PORT_MASK) >> VMX_IO_PORT_SHIFT) == 0x5000)
				vmx_inc_test_stage();
			break;
		case 6:
			if (((exit_qual & VMX_IO_PORT_MASK) >> VMX_IO_PORT_SHIFT) == 0x9000)
				vmx_inc_test_stage();
			break;
		case 7:
			if (((exit_qual & VMX_IO_PORT_MASK) >> VMX_IO_PORT_SHIFT) == 0x4FFF)
				vmx_inc_test_stage();
			break;
		case 8:
			if (((exit_qual & VMX_IO_PORT_MASK) >> VMX_IO_PORT_SHIFT) == 0xFFFF)
				vmx_inc_test_stage();
			break;
		case 9:
		case 10:
			ctrl_cpu0 = vmcs_read(CPU_EXEC_CTRL0);
			vmcs_write(CPU_EXEC_CTRL0, ctrl_cpu0 & ~CPU_IO);
			vmx_inc_test_stage();
			break;
		default:
			// Should not reach here
			printf("ERROR : unexpected stage, %d\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		return VMX_TEST_RESUME;
	case VMX_VMCALL:
		switch (vmx_get_test_stage()) {
		case 9:
			ctrl_cpu0 = vmcs_read(CPU_EXEC_CTRL0);
			ctrl_cpu0 |= CPU_IO | CPU_IO_BITMAP;
			vmcs_write(CPU_EXEC_CTRL0, ctrl_cpu0);
			break;
		case 10:
			ctrl_cpu0 = vmcs_read(CPU_EXEC_CTRL0);
			ctrl_cpu0 = (ctrl_cpu0 & ~CPU_IO_BITMAP) | CPU_IO;
			vmcs_write(CPU_EXEC_CTRL0, ctrl_cpu0);
			break;
		default:
			// Should not reach here
			printf("ERROR : unexpected stage, %d\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		return VMX_TEST_RESUME;
	default:
		printf("guest_rip = 0x%llx\n", guest_rip);
		printf("\tERROR : Undefined exit reason, reason = %d.\n", reason);
		break;
	}
	return VMX_TEST_VMEXIT;
}

#define INSN_CPU0		0
#define INSN_CPU1		1
#define INSN_ALWAYS_TRAP	2

#define FIELD_EXIT_QUAL		(1 << 0)
#define FIELD_INSN_INFO		(1 << 1)

asm(
	"insn_hlt: hlt;ret\n\t"
	"insn_invlpg: invlpg 0x12345678;ret\n\t"
	"insn_mwait: mwait;ret\n\t"
	"insn_rdpmc: rdpmc;ret\n\t"
	"insn_rdtsc: rdtsc;ret\n\t"
	"insn_cr3_load: mov cr3,%rax; mov %rax,%cr3;ret\n\t"
	"insn_cr3_store: mov %cr3,%rax;ret\n\t"
#ifdef __x86_64__
	"insn_cr8_load: mov %rax,%cr8;ret\n\t"
	"insn_cr8_store: mov %cr8,%rax;ret\n\t"
#endif
	"insn_monitor: monitor;ret\n\t"
	"insn_pause: pause;ret\n\t"
	"insn_wbinvd: wbinvd;ret\n\t"
	"insn_cpuid: cpuid;ret\n\t"
	"insn_invd: invd;ret\n\t"
);
extern void insn_hlt();
extern void insn_invlpg();
extern void insn_mwait();
extern void insn_rdpmc();
extern void insn_rdtsc();
extern void insn_cr3_load();
extern void insn_cr3_store();
#ifdef __x86_64__
extern void insn_cr8_load();
extern void insn_cr8_store();
#endif
extern void insn_monitor();
extern void insn_pause();
extern void insn_wbinvd();
extern void insn_cpuid();
extern void insn_invd();

u32 cur_insn;
u64 cr3;

struct insn_table {
	const char *name;
	u32 flag;
	void (*insn_func)();
	u32 type;
	u32 reason;
	ulong exit_qual;
	u32 insn_info;
	// Use FIELD_EXIT_QUAL and FIELD_INSN_INFO to define
	// which field need to be tested, reason is always tested
	u32 test_field;
};

/*
 * Add more test cases of instruction intercept here. Elements in this
 * table is:
 *	name/control flag/insn function/type/exit reason/exit qulification/
 *	instruction info/field to test
 * The last field defines which fields (exit_qual and insn_info) need to be
 * tested in exit handler. If set to 0, only "reason" is checked.
 */
static struct insn_table insn_table[] = {
	// Flags for Primary Processor-Based VM-Execution Controls
	{"HLT",  CPU_HLT, insn_hlt, INSN_CPU0, 12, 0, 0, 0},
	{"INVLPG", CPU_INVLPG, insn_invlpg, INSN_CPU0, 14,
		0x12345678, 0, FIELD_EXIT_QUAL},
	{"MWAIT", CPU_MWAIT, insn_mwait, INSN_CPU0, 36, 0, 0, 0},
	{"RDPMC", CPU_RDPMC, insn_rdpmc, INSN_CPU0, 15, 0, 0, 0},
	{"RDTSC", CPU_RDTSC, insn_rdtsc, INSN_CPU0, 16, 0, 0, 0},
	{"CR3 load", CPU_CR3_LOAD, insn_cr3_load, INSN_CPU0, 28, 0x3, 0,
		FIELD_EXIT_QUAL},
	{"CR3 store", CPU_CR3_STORE, insn_cr3_store, INSN_CPU0, 28, 0x13, 0,
		FIELD_EXIT_QUAL},
#ifdef __x86_64__
	{"CR8 load", CPU_CR8_LOAD, insn_cr8_load, INSN_CPU0, 28, 0x8, 0,
		FIELD_EXIT_QUAL},
	{"CR8 store", CPU_CR8_STORE, insn_cr8_store, INSN_CPU0, 28, 0x18, 0,
		FIELD_EXIT_QUAL},
#endif
	{"MONITOR", CPU_MONITOR, insn_monitor, INSN_CPU0, 39, 0, 0, 0},
	{"PAUSE", CPU_PAUSE, insn_pause, INSN_CPU0, 40, 0, 0, 0},
	// Flags for Secondary Processor-Based VM-Execution Controls
	{"WBINVD", CPU_WBINVD, insn_wbinvd, INSN_CPU1, 54, 0, 0, 0},
	// Instructions always trap
	{"CPUID", 0, insn_cpuid, INSN_ALWAYS_TRAP, 10, 0, 0, 0},
	{"INVD", 0, insn_invd, INSN_ALWAYS_TRAP, 13, 0, 0, 0},
	// Instructions never trap
	{NULL},
};

static int insn_intercept_init()
{
	u32 ctrl_cpu;

	ctrl_cpu = ctrl_cpu_rev[0].set | CPU_SECONDARY;
	ctrl_cpu &= ctrl_cpu_rev[0].clr;
	vmcs_write(CPU_EXEC_CTRL0, ctrl_cpu);
	vmcs_write(CPU_EXEC_CTRL1, ctrl_cpu_rev[1].set);
	cr3 = read_cr3();
	return VMX_TEST_START;
}

static void insn_intercept_main()
{
	char msg[80];

	for (cur_insn = 0; insn_table[cur_insn].name != NULL; cur_insn++) {
		vmx_set_test_stage(cur_insn * 2);
		if ((insn_table[cur_insn].type == INSN_CPU0 &&
		     !(ctrl_cpu_rev[0].clr & insn_table[cur_insn].flag)) ||
		    (insn_table[cur_insn].type == INSN_CPU1 &&
		     !(ctrl_cpu_rev[1].clr & insn_table[cur_insn].flag))) {
			printf("\tCPU_CTRL%d.CPU_%s is not supported.\n",
			       insn_table[cur_insn].type - INSN_CPU0,
			       insn_table[cur_insn].name);
			continue;
		}

		if ((insn_table[cur_insn].type == INSN_CPU0 &&
		     !(ctrl_cpu_rev[0].set & insn_table[cur_insn].flag)) ||
		    (insn_table[cur_insn].type == INSN_CPU1 &&
		     !(ctrl_cpu_rev[1].set & insn_table[cur_insn].flag))) {
			/* skip hlt, it stalls the guest and is tested below */
			if (insn_table[cur_insn].insn_func != insn_hlt)
				insn_table[cur_insn].insn_func();
			snprintf(msg, sizeof(msg), "execute %s",
				 insn_table[cur_insn].name);
			report(msg, vmx_get_test_stage() == cur_insn * 2);
		} else if (insn_table[cur_insn].type != INSN_ALWAYS_TRAP)
			printf("\tCPU_CTRL%d.CPU_%s always traps.\n",
			       insn_table[cur_insn].type - INSN_CPU0,
			       insn_table[cur_insn].name);

		vmcall();

		insn_table[cur_insn].insn_func();
		snprintf(msg, sizeof(msg), "intercept %s",
			 insn_table[cur_insn].name);
		report(msg, vmx_get_test_stage() == cur_insn * 2 + 1);

		vmx_set_test_stage(cur_insn * 2 + 1);
		vmcall();
	}
}

static int insn_intercept_exit_handler()
{
	u64 guest_rip;
	u32 reason;
	ulong exit_qual;
	u32 insn_len;
	u32 insn_info;
	bool pass;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	exit_qual = vmcs_read(EXI_QUALIFICATION);
	insn_len = vmcs_read(EXI_INST_LEN);
	insn_info = vmcs_read(EXI_INST_INFO);

	if (reason == VMX_VMCALL) {
		u32 val = 0;

		if (insn_table[cur_insn].type == INSN_CPU0)
			val = vmcs_read(CPU_EXEC_CTRL0);
		else if (insn_table[cur_insn].type == INSN_CPU1)
			val = vmcs_read(CPU_EXEC_CTRL1);

		if (vmx_get_test_stage() & 1)
			val &= ~insn_table[cur_insn].flag;
		else
			val |= insn_table[cur_insn].flag;

		if (insn_table[cur_insn].type == INSN_CPU0)
			vmcs_write(CPU_EXEC_CTRL0, val | ctrl_cpu_rev[0].set);
		else if (insn_table[cur_insn].type == INSN_CPU1)
			vmcs_write(CPU_EXEC_CTRL1, val | ctrl_cpu_rev[1].set);
	} else {
		pass = (cur_insn * 2 == vmx_get_test_stage()) &&
			insn_table[cur_insn].reason == reason;
		if (insn_table[cur_insn].test_field & FIELD_EXIT_QUAL &&
		    insn_table[cur_insn].exit_qual != exit_qual)
			pass = false;
		if (insn_table[cur_insn].test_field & FIELD_INSN_INFO &&
		    insn_table[cur_insn].insn_info != insn_info)
			pass = false;
		if (pass)
			vmx_inc_test_stage();
	}
	vmcs_write(GUEST_RIP, guest_rip + insn_len);
	return VMX_TEST_RESUME;
}


static int setup_ept()
{
	int support_2m;
	unsigned long end_of_memory;

	if (!(ept_vpid.val & EPT_CAP_UC) &&
			!(ept_vpid.val & EPT_CAP_WB)) {
		printf("\tEPT paging-structure memory type "
				"UC&WB are not supported\n");
		return 1;
	}
	if (ept_vpid.val & EPT_CAP_UC)
		eptp = EPT_MEM_TYPE_UC;
	else
		eptp = EPT_MEM_TYPE_WB;
	if (!(ept_vpid.val & EPT_CAP_PWL4)) {
		printf("\tPWL4 is not supported\n");
		return 1;
	}
	eptp |= (3 << EPTP_PG_WALK_LEN_SHIFT);
	pml4 = alloc_page();
	memset(pml4, 0, PAGE_SIZE);
	eptp |= virt_to_phys(pml4);
	vmcs_write(EPTP, eptp);
	support_2m = !!(ept_vpid.val & EPT_CAP_2M_PAGE);
	end_of_memory = fwcfg_get_u64(FW_CFG_RAM_SIZE);
	if (end_of_memory < (1ul << 32))
		end_of_memory = (1ul << 32);
	setup_ept_range(pml4, 0, end_of_memory, 0, support_2m,
			EPT_WA | EPT_RA | EPT_EA);
	return 0;
}

static int ept_init()
{
	unsigned long base_addr1, base_addr2;
	u32 ctrl_cpu[2];

	if (!(ctrl_cpu_rev[0].clr & CPU_SECONDARY) ||
	    !(ctrl_cpu_rev[1].clr & CPU_EPT)) {
		printf("\tEPT is not supported");
		return VMX_TEST_EXIT;
	}

	ctrl_cpu[0] = vmcs_read(CPU_EXEC_CTRL0);
	ctrl_cpu[1] = vmcs_read(CPU_EXEC_CTRL1);
	ctrl_cpu[0] = (ctrl_cpu[0] | CPU_SECONDARY)
		& ctrl_cpu_rev[0].clr;
	ctrl_cpu[1] = (ctrl_cpu[1] | CPU_EPT)
		& ctrl_cpu_rev[1].clr;
	vmcs_write(CPU_EXEC_CTRL0, ctrl_cpu[0]);
	vmcs_write(CPU_EXEC_CTRL1, ctrl_cpu[1]);
	if (setup_ept())
		return VMX_TEST_EXIT;
	data_page1 = alloc_page();
	data_page2 = alloc_page();
	memset(data_page1, 0x0, PAGE_SIZE);
	memset(data_page2, 0x0, PAGE_SIZE);
	*((u32 *)data_page1) = MAGIC_VAL_1;
	*((u32 *)data_page2) = MAGIC_VAL_2;
	base_addr1 = (unsigned long)data_page1 & PAGE_MASK_2M;
	base_addr2 = (unsigned long)data_page2 & PAGE_MASK_2M;
	setup_ept_range(pml4, base_addr1, base_addr1 + PAGE_SIZE_2M, 0, 0,
			EPT_WA | EPT_RA | EPT_EA);
	setup_ept_range(pml4, base_addr2, base_addr2 + PAGE_SIZE_2M, 0, 0,
			EPT_WA | EPT_RA | EPT_EA);
	install_ept(pml4, (unsigned long)data_page1, (unsigned long)data_page2,
			EPT_RA | EPT_WA | EPT_EA);
	return VMX_TEST_START;
}

static void ept_main()
{
	vmx_set_test_stage(0);
	if (*((u32 *)data_page2) != MAGIC_VAL_1 ||
			*((u32 *)data_page1) != MAGIC_VAL_1)
		report("EPT basic framework - read", 0);
	else {
		*((u32 *)data_page2) = MAGIC_VAL_3;
		vmcall();
		if (vmx_get_test_stage() == 1) {
			if (*((u32 *)data_page1) == MAGIC_VAL_3 &&
					*((u32 *)data_page2) == MAGIC_VAL_2)
				report("EPT basic framework", 1);
			else
				report("EPT basic framework - remap", 1);
		}
	}
	// Test EPT Misconfigurations
	vmx_set_test_stage(1);
	vmcall();
	*((u32 *)data_page1) = MAGIC_VAL_1;
	if (vmx_get_test_stage() != 2) {
		report("EPT misconfigurations", 0);
		goto t1;
	}
	vmx_set_test_stage(2);
	vmcall();
	*((u32 *)data_page1) = MAGIC_VAL_1;
	if (vmx_get_test_stage() != 3) {
		report("EPT misconfigurations", 0);
		goto t1;
	}
	report("EPT misconfigurations", 1);
t1:
	// Test EPT violation
	vmx_set_test_stage(3);
	vmcall();
	*((u32 *)data_page1) = MAGIC_VAL_1;
	if (vmx_get_test_stage() == 4)
		report("EPT violation - page permission", 1);
	else
		report("EPT violation - page permission", 0);
	// Violation caused by EPT paging structure
	vmx_set_test_stage(4);
	vmcall();
	*((u32 *)data_page1) = MAGIC_VAL_2;
	if (vmx_get_test_stage() == 5)
		report("EPT violation - paging structure", 1);
	else
		report("EPT violation - paging structure", 0);
}

static int ept_exit_handler()
{
	u64 guest_rip;
	ulong reason;
	u32 insn_len;
	u32 exit_qual;
	static unsigned long data_page1_pte, data_page1_pte_pte;

	guest_rip = vmcs_read(GUEST_RIP);
	reason = vmcs_read(EXI_REASON) & 0xff;
	insn_len = vmcs_read(EXI_INST_LEN);
	exit_qual = vmcs_read(EXI_QUALIFICATION);
	switch (reason) {
	case VMX_VMCALL:
		switch (vmx_get_test_stage()) {
		case 0:
			if (*((u32 *)data_page1) == MAGIC_VAL_3 &&
					*((u32 *)data_page2) == MAGIC_VAL_2) {
				vmx_inc_test_stage();
				install_ept(pml4, (unsigned long)data_page2,
						(unsigned long)data_page2,
						EPT_RA | EPT_WA | EPT_EA);
			} else
				report("EPT basic framework - write\n", 0);
			break;
		case 1:
			install_ept(pml4, (unsigned long)data_page1,
 				(unsigned long)data_page1, EPT_WA);
			ept_sync(INVEPT_SINGLE, eptp);
			break;
		case 2:
			install_ept(pml4, (unsigned long)data_page1,
 				(unsigned long)data_page1,
 				EPT_RA | EPT_WA | EPT_EA |
 				(2 << EPT_MEM_TYPE_SHIFT));
			ept_sync(INVEPT_SINGLE, eptp);
			break;
		case 3:
			data_page1_pte = get_ept_pte(pml4,
				(unsigned long)data_page1, 1);
			set_ept_pte(pml4, (unsigned long)data_page1, 
				1, data_page1_pte & (~EPT_PRESENT));
			ept_sync(INVEPT_SINGLE, eptp);
			break;
		case 4:
			data_page1_pte = get_ept_pte(pml4,
				(unsigned long)data_page1, 2);
			data_page1_pte &= PAGE_MASK;
			data_page1_pte_pte = get_ept_pte(pml4, data_page1_pte, 2);
			set_ept_pte(pml4, data_page1_pte, 2,
				data_page1_pte_pte & (~EPT_PRESENT));
			ept_sync(INVEPT_SINGLE, eptp);
			break;
		// Should not reach here
		default:
			printf("ERROR - unexpected stage, %d.\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		return VMX_TEST_RESUME;
	case VMX_EPT_MISCONFIG:
		switch (vmx_get_test_stage()) {
		case 1:
		case 2:
			vmx_inc_test_stage();
			install_ept(pml4, (unsigned long)data_page1,
 				(unsigned long)data_page1,
 				EPT_RA | EPT_WA | EPT_EA);
			ept_sync(INVEPT_SINGLE, eptp);
			break;
		// Should not reach here
		default:
			printf("ERROR - unexpected stage, %d.\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		return VMX_TEST_RESUME;
	case VMX_EPT_VIOLATION:
		switch(vmx_get_test_stage()) {
		case 3:
			if (exit_qual == (EPT_VLT_WR | EPT_VLT_LADDR_VLD |
					EPT_VLT_PADDR))
				vmx_inc_test_stage();
			set_ept_pte(pml4, (unsigned long)data_page1, 
				1, data_page1_pte | (EPT_PRESENT));
			ept_sync(INVEPT_SINGLE, eptp);
			break;
		case 4:
			if (exit_qual == (EPT_VLT_RD | EPT_VLT_LADDR_VLD))
				vmx_inc_test_stage();
			set_ept_pte(pml4, data_page1_pte, 2,
				data_page1_pte_pte | (EPT_PRESENT));
			ept_sync(INVEPT_SINGLE, eptp);
			break;
		default:
			// Should not reach here
			printf("ERROR : unexpected stage, %d\n",
			       vmx_get_test_stage());
			print_vmexit_info();
			return VMX_TEST_VMEXIT;
		}
		return VMX_TEST_RESUME;
	default:
		printf("Unknown exit reason, %d\n", reason);
		print_vmexit_info();
	}
	return VMX_TEST_VMEXIT;
}

#define TIMER_VECTOR	222

static volatile bool timer_fired;

static void timer_isr(isr_regs_t *regs)
{
	timer_fired = true;
	apic_write(APIC_EOI, 0);
}

static int interrupt_init(struct vmcs *vmcs)
{
	msr_bmp_init();
	vmcs_write(PIN_CONTROLS, vmcs_read(PIN_CONTROLS) & ~PIN_EXTINT);
	handle_irq(TIMER_VECTOR, timer_isr);
	return VMX_TEST_START;
}

static void interrupt_main(void)
{
	long long start, loops;

	vmx_set_test_stage(0);

	apic_write(APIC_LVTT, TIMER_VECTOR);
	irq_enable();

	apic_write(APIC_TMICT, 1);
	for (loops = 0; loops < 10000000 && !timer_fired; loops++)
		asm volatile ("nop");
	report("direct interrupt while running guest", timer_fired);

	apic_write(APIC_TMICT, 0);
	irq_disable();
	vmcall();
	timer_fired = false;
	apic_write(APIC_TMICT, 1);
	for (loops = 0; loops < 10000000 && !timer_fired; loops++)
		asm volatile ("nop");
	report("intercepted interrupt while running guest", timer_fired);

	irq_enable();
	apic_write(APIC_TMICT, 0);
	irq_disable();
	vmcall();
	timer_fired = false;
	start = rdtsc();
	apic_write(APIC_TMICT, 1000000);

	asm volatile ("sti; hlt");

	report("direct interrupt + hlt",
	       rdtsc() - start > 1000000 && timer_fired);

	apic_write(APIC_TMICT, 0);
	irq_disable();
	vmcall();
	timer_fired = false;
	start = rdtsc();
	apic_write(APIC_TMICT, 1000000);

	asm volatile ("sti; hlt");

	report("intercepted interrupt + hlt",
	       rdtsc() - start > 10000 && timer_fired);

	apic_write(APIC_TMICT, 0);
	irq_disable();
	vmcall();
	timer_fired = false;
	start = rdtsc();
	apic_write(APIC_TMICT, 1000000);

	irq_enable();
	asm volatile ("nop");
	vmcall();

	report("direct interrupt + activity state hlt",
	       rdtsc() - start > 10000 && timer_fired);

	apic_write(APIC_TMICT, 0);
	irq_disable();
	vmcall();
	timer_fired = false;
	start = rdtsc();
	apic_write(APIC_TMICT, 1000000);

	irq_enable();
	asm volatile ("nop");
	vmcall();

	report("intercepted interrupt + activity state hlt",
	       rdtsc() - start > 10000 && timer_fired);

	apic_write(APIC_TMICT, 0);
	irq_disable();
	vmx_set_test_stage(7);
	vmcall();
	timer_fired = false;
	apic_write(APIC_TMICT, 1);
	for (loops = 0; loops < 10000000 && !timer_fired; loops++)
		asm volatile ("nop");
	report("running a guest with interrupt acknowledgement set", timer_fired);
}

static int interrupt_exit_handler(void)
{
	u64 guest_rip = vmcs_read(GUEST_RIP);
	ulong reason = vmcs_read(EXI_REASON) & 0xff;
	u32 insn_len = vmcs_read(EXI_INST_LEN);

	switch (reason) {
	case VMX_VMCALL:
		switch (vmx_get_test_stage()) {
		case 0:
		case 2:
		case 5:
			vmcs_write(PIN_CONTROLS,
				   vmcs_read(PIN_CONTROLS) | PIN_EXTINT);
			break;
		case 7:
			vmcs_write(EXI_CONTROLS, vmcs_read(EXI_CONTROLS) | EXI_INTA);
			vmcs_write(PIN_CONTROLS,
				   vmcs_read(PIN_CONTROLS) | PIN_EXTINT);
			break;
		case 1:
		case 3:
			vmcs_write(PIN_CONTROLS,
				   vmcs_read(PIN_CONTROLS) & ~PIN_EXTINT);
			break;
		case 4:
		case 6:
			vmcs_write(GUEST_ACTV_STATE, ACTV_HLT);
			break;
		}
		vmx_inc_test_stage();
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		return VMX_TEST_RESUME;
	case VMX_EXTINT:
		if (vmcs_read(EXI_CONTROLS) & EXI_INTA) {
			int vector = vmcs_read(EXI_INTR_INFO) & 0xff;
			handle_external_interrupt(vector);
		} else {
			irq_enable();
			asm volatile ("nop");
			irq_disable();
		}
		if (vmx_get_test_stage() >= 2) {
			vmcs_write(GUEST_ACTV_STATE, ACTV_ACTIVE);
			vmcs_write(GUEST_RIP, guest_rip + insn_len);
		}
		return VMX_TEST_RESUME;
	default:
		printf("Unknown exit reason, %d\n", reason);
		print_vmexit_info();
	}

	return VMX_TEST_VMEXIT;
}

static int dbgctls_init(struct vmcs *vmcs)
{
	u64 dr7 = 0x402;
	u64 zero = 0;

	msr_bmp_init();
	asm volatile(
		"mov %0,%%dr0\n\t"
		"mov %0,%%dr1\n\t"
		"mov %0,%%dr2\n\t"
		"mov %1,%%dr7\n\t"
		: : "r" (zero), "r" (dr7));
	wrmsr(MSR_IA32_DEBUGCTLMSR, 0x1);
	vmcs_write(GUEST_DR7, 0x404);
	vmcs_write(GUEST_DEBUGCTL, 0x2);

	vmcs_write(ENT_CONTROLS, vmcs_read(ENT_CONTROLS) | ENT_LOAD_DBGCTLS);
	vmcs_write(EXI_CONTROLS, vmcs_read(EXI_CONTROLS) | EXI_SAVE_DBGCTLS);

	return VMX_TEST_START;
}

static void dbgctls_main(void)
{
	u64 dr7, debugctl;

	asm volatile("mov %%dr7,%0" : "=r" (dr7));
	debugctl = rdmsr(MSR_IA32_DEBUGCTLMSR);
	/* Commented out: KVM does not support DEBUGCTL so far */
	report("Load debug controls", dr7 == 0x404 /* && debugctl == 0x2 */);

	dr7 = 0x408;
	asm volatile("mov %0,%%dr7" : : "r" (dr7));
	wrmsr(MSR_IA32_DEBUGCTLMSR, 0x3);

	vmx_set_test_stage(0);
	vmcall();
	report("Save debug controls", vmx_get_test_stage() == 1);

	if (ctrl_enter_rev.set & ENT_LOAD_DBGCTLS ||
	    ctrl_exit_rev.set & EXI_SAVE_DBGCTLS) {
		printf("\tDebug controls are always loaded/saved\n");
		return;
	}
	vmx_set_test_stage(2);
	vmcall();

	asm volatile("mov %%dr7,%0" : "=r" (dr7));
	debugctl = rdmsr(MSR_IA32_DEBUGCTLMSR);
	/* Commented out: KVM does not support DEBUGCTL so far */
	report("Guest=host debug controls", dr7 == 0x402 /* && debugctl == 0x1 */);

	dr7 = 0x408;
	asm volatile("mov %0,%%dr7" : : "r" (dr7));
	wrmsr(MSR_IA32_DEBUGCTLMSR, 0x3);

	vmx_set_test_stage(3);
	vmcall();
	report("Don't save debug controls", vmx_get_test_stage() == 4);
}

static int dbgctls_exit_handler(void)
{
	unsigned int reason = vmcs_read(EXI_REASON) & 0xff;
	u32 insn_len = vmcs_read(EXI_INST_LEN);
	u64 guest_rip = vmcs_read(GUEST_RIP);
	u64 dr7, debugctl;

	asm volatile("mov %%dr7,%0" : "=r" (dr7));
	debugctl = rdmsr(MSR_IA32_DEBUGCTLMSR);

	switch (reason) {
	case VMX_VMCALL:
		switch (vmx_get_test_stage()) {
		case 0:
			if (dr7 == 0x400 && debugctl == 0 &&
			    vmcs_read(GUEST_DR7) == 0x408 /* &&
			    Commented out: KVM does not support DEBUGCTL so far
			    vmcs_read(GUEST_DEBUGCTL) == 0x3 */)
				vmx_inc_test_stage();
			break;
		case 2:
			dr7 = 0x402;
			asm volatile("mov %0,%%dr7" : : "r" (dr7));
			wrmsr(MSR_IA32_DEBUGCTLMSR, 0x1);
			vmcs_write(GUEST_DR7, 0x404);
			vmcs_write(GUEST_DEBUGCTL, 0x2);

			vmcs_write(ENT_CONTROLS,
				vmcs_read(ENT_CONTROLS) & ~ENT_LOAD_DBGCTLS);
			vmcs_write(EXI_CONTROLS,
				vmcs_read(EXI_CONTROLS) & ~EXI_SAVE_DBGCTLS);
			break;
		case 3:
			if (dr7 == 0x400 && debugctl == 0 &&
			    vmcs_read(GUEST_DR7) == 0x404 /* &&
			    Commented out: KVM does not support DEBUGCTL so far
			    vmcs_read(GUEST_DEBUGCTL) == 0x2 */)
				vmx_inc_test_stage();
			break;
		}
		vmcs_write(GUEST_RIP, guest_rip + insn_len);
		return VMX_TEST_RESUME;
	default:
		printf("Unknown exit reason, %d\n", reason);
		print_vmexit_info();
	}
	return VMX_TEST_VMEXIT;
}

/* name/init/guest_main/exit_handler/syscall_handler/guest_regs */
struct vmx_test vmx_tests[] = {
	{ "null", NULL, basic_guest_main, basic_exit_handler, NULL, {0} },
	{ "vmenter", NULL, vmenter_main, vmenter_exit_handler, NULL, {0} },
	{ "preemption timer", preemption_timer_init, preemption_timer_main,
		preemption_timer_exit_handler, NULL, {0} },
	{ "control field PAT", test_ctrl_pat_init, test_ctrl_pat_main,
		test_ctrl_pat_exit_handler, NULL, {0} },
	{ "control field EFER", test_ctrl_efer_init, test_ctrl_efer_main,
		test_ctrl_efer_exit_handler, NULL, {0} },
	{ "CR shadowing", NULL, cr_shadowing_main,
		cr_shadowing_exit_handler, NULL, {0} },
	{ "I/O bitmap", iobmp_init, iobmp_main, iobmp_exit_handler,
		NULL, {0} },
	{ "instruction intercept", insn_intercept_init, insn_intercept_main,
		insn_intercept_exit_handler, NULL, {0} },
	{ "EPT framework", ept_init, ept_main, ept_exit_handler, NULL, {0} },
	{ "interrupt", interrupt_init, interrupt_main,
		interrupt_exit_handler, NULL, {0} },
	{ "debug controls", dbgctls_init, dbgctls_main, dbgctls_exit_handler,
		NULL, {0} },
	{ NULL, NULL, NULL, NULL, NULL, {0} },
};