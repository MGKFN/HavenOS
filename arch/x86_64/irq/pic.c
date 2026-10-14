/* SPDX-License-Identifier: MIT
 *
 * pic.c — the legacy 8259A pair.
 *
 * WHY THIS STILL EXISTS ON A 64-BIT SYSTEM
 * The PIC is obsolete and the APIC replaces it, but two things make it
 * worth keeping.  First, it must be explicitly reprogrammed and masked
 * before the APIC is used, or it keeps delivering interrupts on vectors
 * that now mean something else.  Second, it is a working fallback on a
 * machine whose APIC is absent or broken, which is the difference between
 * a boot and a hang.
 *
 * THE REMAPPING IS NOT OPTIONAL
 * From power-on the master 8259 delivers IRQ0-7 on vectors 8-15, which in
 * long mode are #DF, #TS, #NP, #SS, #GP, #PF, vector 15 and #MF.  A timer
 * tick would therefore arrive as a double fault.  HavenDOS remapped them
 * to 0x20-0x2F for exactly this reason and that choice is kept.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/idt.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_ICW4       0x01   /* a fourth initialisation word follows */
#define ICW1_INIT       0x10
#define ICW4_8086       0x01   /* 8086/88 mode rather than MCS-80/85 */
#define PIC_EOI         0x20
#define PIC_READ_ISR    0x0B

static uint16_t cached_mask = 0xFFFF;   /* all masked */

void pic_init(void)
{
	/* Save nothing: the firmware's mask is not worth preserving, since
	   we are about to own every line. */

	outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();
	outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();

	/* ICW2: the vector base for each chip. */
	outb(PIC1_DATA, VEC_IRQ_BASE);
	io_wait();
	outb(PIC2_DATA, VEC_IRQ_BASE + 8);
	io_wait();

	/* ICW3: how the two are wired together.  The slave is cascaded onto
	   the master's line 2, which is why IRQ2 is never a real device and
	   why unmasking any line 8-15 also requires unmasking line 2. */
	outb(PIC1_DATA, 1 << 2);
	io_wait();
	outb(PIC2_DATA, 2);
	io_wait();

	outb(PIC1_DATA, ICW4_8086);
	io_wait();
	outb(PIC2_DATA, ICW4_8086);
	io_wait();

	/* Everything masked.  Lines are unmasked by irq_register() when a
	   handler is actually in place — not before, or an interrupt can
	   arrive with nothing to service it. */
	cached_mask = 0xFFFF;
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);

	kinfo("pic: 8259A pair remapped to vectors %d-%d, all lines masked",
	      VEC_IRQ_BASE, VEC_IRQ_BASE + 15);
}

void pic_set_mask(uint32_t irq, bool masked)
{
	if (irq >= 16)
		return;

	if (masked)
		cached_mask |= (uint16_t)(1u << irq);
	else
		cached_mask &= (uint16_t)~(1u << irq);

	/* Unmasking anything on the slave requires the cascade line open
	   too, or the slave's output never reaches the CPU.  Forgetting
	   this is why a correctly written mouse or RTC driver appears to
	   receive no interrupts at all. */
	if (irq >= 8 && !masked)
		cached_mask &= (uint16_t)~(1u << 2);

	outb(PIC1_DATA, (uint8_t)(cached_mask & 0xFF));
	outb(PIC2_DATA, (uint8_t)(cached_mask >> 8));
}

void pic_mask_all(void)
{
	cached_mask = 0xFFFF;
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);
}

/*
 * Is this a spurious interrupt?
 *
 * The 8259 delivers IRQ7 (and IRQ15) when a line is asserted and then
 * withdrawn before the CPU acknowledges it.  Such an interrupt must NOT
 * be acknowledged — the chip never raised it in its In-Service Register,
 * so an EOI would clear a real pending interrupt belonging to something
 * else.  Reading the ISR is the only way to tell the difference.
 */
static bool pic_spurious(uint32_t irq)
{
	if (irq != 7 && irq != 15)
		return false;

	uint16_t port = (irq == 7) ? PIC1_CMD : PIC2_CMD;
	outb(port, PIC_READ_ISR);
	uint8_t isr = inb(port);

	return (isr & (1u << (irq & 7))) == 0;
}

void pic_eoi(uint32_t irq)
{
	if (irq >= 16)
		return;

	if (pic_spurious(irq)) {
		/* A spurious line 15 still needs the MASTER acknowledged,
		   because the master did see the cascade assert even though
		   the slave did not latch a real interrupt. */
		if (irq == 15)
			outb(PIC1_CMD, PIC_EOI);
		return;
	}

	if (irq >= 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);
}

void pic_disable(void)
{
	/* Mask everything permanently.  The APIC is now in charge; leaving
	   the PIC unmasked means two controllers delivering the same device
	   on two different vectors. */
	pic_mask_all();
	kinfo("pic: disabled in favour of the local APIC");
}
