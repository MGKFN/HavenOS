/* SPDX-License-Identifier: MIT
 *
 * selftest -- the user half of the kernel self-test suite.
 *
 * The kernel's own tests (kernel/test/selftest.c) can check an allocator
 * or a scheduler from the inside, but they cannot tell whether a program
 * in ring 3 can make a system call, because they are not one.  For the
 * whole of HavenOS 0.1.0 nothing was: the suite passed 82 of 82 checks
 * while SYSCALL destroyed its own call number, copy_from_user() never
 * returned, and preempting a user thread crashed the machine.
 *
 * This program is built with the kernel, embedded in it, and run by the
 * kernel suite on every boot test.  It is told what to do by argv[1]:
 *
 *   basic            system calls, bad pointers, heap, files, register
 *                    preservation across a system call and a preemption
 *   pattern <c>      fill memory with <c>, yield and spin, check that it
 *                    is still <c>.  Two of these run at once at the same
 *                    virtual addresses: only separate address spaces and
 *                    a correct switch between them let both pass
 *   fault-read       read kernel memory; must be killed, not answered
 *   fault-write      write to the program's own code; must be killed
 *   fault-exec       execute the stack; must be killed
 *
 * The exit status is 0 for success, or the number of the first check that
 * failed.  The fault modes exit with 99 if they SURVIVE, so that survival
 * can never be mistaken for the expected death.
 */
#include <haven/user.h>

#define SURVIVED 99

static size_t slen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void say(const char *s)
{
	sys_write(1, s, slen(s));
}

static int check_no;

/* Evaluate in order; the first failure is the exit status. */
#define CHECK(cond)                                   \
	do {                                          \
		check_no++;                           \
		if (!(cond)) {                        \
			say("selftest(user): FAILED check\n"); \
			return check_no;              \
		}                                     \
	} while (0)

/*
 * Load every callee-saved register with a known value, make a system
 * call, and report whether they all came back.  Written in assembly
 * because the compiler must not be allowed to save and restore them
 * around the call on our behalf -- that is the kernel's job here.
 */
static int regs_survive_syscall(void)
{
	long bad;

	__asm__ volatile(
		"pushq %%rbx\n\tpushq %%rbp\n\t"
		"pushq %%r12\n\tpushq %%r13\n\tpushq %%r14\n\tpushq %%r15\n\t"
		"movq $0x1111, %%rbx\n\t"
		"movq $0x2222, %%rbp\n\t"
		"movq $0x3333, %%r12\n\t"
		"movq $0x4444, %%r13\n\t"
		"movq $0x5555, %%r14\n\t"
		"movq $0x6666, %%r15\n\t"
		"movq $0x7777, %%rdi\n\t"
		"movq $0x8888, %%r8\n\t"
		"movl %[nr], %%eax\n\t"
		"syscall\n\t"
		"xorl %%eax, %%eax\n\t"
		"cmpq $0x1111, %%rbx\n\tjne 1f\n\t"
		"cmpq $0x2222, %%rbp\n\tjne 1f\n\t"
		"cmpq $0x3333, %%r12\n\tjne 1f\n\t"
		"cmpq $0x4444, %%r13\n\tjne 1f\n\t"
		"cmpq $0x5555, %%r14\n\tjne 1f\n\t"
		"cmpq $0x6666, %%r15\n\tjne 1f\n\t"
		/* The kernel preserves argument registers too: only rax, rcx
		   and r11 are documented as changed. */
		"cmpq $0x7777, %%rdi\n\tjne 1f\n\t"
		"cmpq $0x8888, %%r8\n\tjne 1f\n\t"
		"jmp 2f\n"
		"1:\n\tmovl $1, %%eax\n"
		"2:\n\t"
		"popq %%r15\n\tpopq %%r14\n\tpopq %%r13\n\tpopq %%r12\n\t"
		"popq %%rbp\n\tpopq %%rbx\n\t"
		: "=a"(bad)
		: [nr] "i"(SYS_getpid)
		: "rcx", "r11", "rdi", "r8", "memory", "cc");

	return bad == 0;
}

/*
 * Hold known values in registers across a long computation with no
 * system call in it.  The only way out of this loop is the timer
 * interrupt, so it checks that a thread preempted in ring 3 gets its
 * registers and its stack back.
 */
static int regs_survive_preemption(void)
{
	long bad;

	__asm__ volatile(
		"movq $0xAAAA, %%r12\n\t"
		"movq $0xBBBB, %%r13\n\t"
		"movq $200000000, %%rcx\n"
		"1:\n\t"
		"decq %%rcx\n\t"
		"jnz 1b\n\t"
		"xorl %%eax, %%eax\n\t"
		"cmpq $0xAAAA, %%r12\n\tjne 2f\n\t"
		"cmpq $0xBBBB, %%r13\n\tjne 2f\n\t"
		"jmp 3f\n"
		"2:\n\tmovl $1, %%eax\n"
		"3:\n\t"
		: "=a"(bad)
		:
		: "rcx", "r12", "r13", "cc");

	return bad == 0;
}

static int mode_basic(void)
{
	say("selftest(user): running in ring 3\n");

	/* 1: a system call returns a sensible value at all. */
	CHECK(sys_getpid() > 0);

	/* 2-3: the kernel refuses pointers it must not follow. */
	CHECK(sys_write(1, (const void *)0xdead0000UL, 16) == -EFAULT);
	CHECK(sys_write(1, (const void *)0xffffffff80200000UL, 16) == -EFAULT);

	/* 4-5: an unknown call number and a closed descriptor are errors,
	   not crashes. */
	CHECK(__sc(4000, 0, 0, 0, 0) == -ENOSYS);
	CHECK(sys_write(57, "x", 1) == -EBADF);

	/* 6-8: the heap grows and the new memory is usable and zeroed. */
	long brk0 = sys_brk(0);
	CHECK(brk0 > 0);
	CHECK(sys_brk((unsigned long)brk0 + 3 * 4096) == brk0 + 3 * 4096);
	{
		volatile unsigned char *heap = (unsigned char *)brk0;
		int ok = heap[0] == 0 && heap[3 * 4096 - 1] == 0;
		heap[0] = 0x5A;
		heap[3 * 4096 - 1] = 0xA5;
		CHECK(ok && heap[0] == 0x5A && heap[3 * 4096 - 1] == 0xA5);
	}

	/* 9: anonymous mmap. */
	{
		long m = sys_mmap(0, 8192, PROT_READ | PROT_WRITE, 0);
		volatile unsigned char *p = (unsigned char *)m;
		int ok = m > 0;
		if (ok) {
			p[0] = 1;
			p[8191] = 2;
			ok = p[0] == 1 && p[8191] == 2;
		}
		CHECK(ok);
	}

	/* 10-13: a file written through the VFS reads back. */
	{
		static const char text[] = "written from ring 3";
		char back[sizeof(text)] = { 0 };
		const char *path = "/tmp/selftest-user.txt";

		int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		CHECK(fd >= 3);
		CHECK(sys_write(fd, text, sizeof(text)) == (ssize_t)sizeof(text));
		sys_close(fd);

		fd = sys_open(path, O_RDONLY, 0);
		CHECK(fd >= 3 &&
		      sys_read(fd, back, sizeof(back)) == (ssize_t)sizeof(back));
		sys_close(fd);

		int same = 1;
		for (size_t i = 0; i < sizeof(text); i++)
			same &= back[i] == text[i];
		CHECK(same);
		sys_unlink(path);
	}

	/* 14-15: registers across a system call, and across preemption. */
	CHECK(regs_survive_syscall());
	CHECK(regs_survive_preemption());

	say("selftest(user): all checks passed\n");
	return 0;
}

#define PATTERN_BYTES (64 * 1024)

static unsigned char pattern_data[4096] = { 1 };   /* in .data */

static int mode_pattern(unsigned char c)
{
	long brk0 = sys_brk(0);
	if (brk0 <= 0 ||
	    sys_brk((unsigned long)brk0 + PATTERN_BYTES) != brk0 + PATTERN_BYTES)
		return 1;

	volatile unsigned char *heap = (unsigned char *)brk0;

	for (int round = 0; round < 40; round++) {
		for (size_t i = 0; i < PATTERN_BYTES; i++)
			heap[i] = c;
		for (size_t i = 0; i < sizeof(pattern_data); i++)
			pattern_data[i] = c;

		/* Give the other instance every chance to run in between:
		   an explicit yield, and a spin long enough to be preempted. */
		sys_yield();
		for (volatile long spin = 0; spin < 2000000; spin++)
			;

		for (size_t i = 0; i < PATTERN_BYTES; i++)
			if (heap[i] != c)
				return 2;
		for (size_t i = 0; i < sizeof(pattern_data); i++)
			if (pattern_data[i] != c)
				return 3;
	}
	return 0;
}

static void func_on_stack_target(void) { }

int main(int argc, char **argv)
{
	if (argc < 2)
		return 100;

	if (streq(argv[1], "basic"))
		return mode_basic();

	if (streq(argv[1], "pattern") && argc >= 3)
		return mode_pattern((unsigned char)argv[2][0]);

	if (streq(argv[1], "fault-read")) {
		volatile unsigned char v =
			*(volatile unsigned char *)0xffffffff80200000UL;
		(void)v;
		return SURVIVED;
	}

	if (streq(argv[1], "fault-write")) {
		*(volatile unsigned char *)(unsigned long)main = 0xC3;
		return SURVIVED;
	}

	if (streq(argv[1], "fault-exec")) {
		/* A single `ret` on the stack.  If the stack were executable
		   this would return harmlessly. */
		volatile unsigned char code[16] = { 0xC3 };
		void (*volatile fn)(void) = func_on_stack_target;
		fn = (void (*)(void))(unsigned long)code;
		fn();
		return SURVIVED;
	}

	return 101;
}
