/* SPDX-License-Identifier: MIT
 *
 * serial.c — 16550 UART console on COM1.
 *
 * WHY THIS IS THE FIRST DRIVER TO INITIALISE
 * HavenDOS's hardest bug to diagnose was its UEFI framebuffer bring-up:
 * if the display did not come up, there was no output at all, because
 * every log line went through vga_printf().  The developer's own notes
 * describe demonstrating a boot by photographing a laptop screen.
 *
 * A UART needs no memory, no interrupts, no PCI enumeration and no
 * firmware cooperation — just four port writes.  So HavenOS brings it up
 * before anything else and registers it as the first console sink, which
 * means every line from "entering long mode" onward is capturable over a
 * cable or by `qemu -serial stdio` on a machine whose display is dead.
 */
#include <haven/console.h>
#include <haven/types.h>
#include <haven/arch/x86_64/io.h>

#define COM1_BASE 0x3F8

/* 16550 register offsets.  When DLAB (LCR bit 7) is set, offsets 0 and 1
   become the low and high halves of the baud divisor instead of data and
   interrupt-enable. */
#define UART_DATA        0   /* RBR / THR */
#define UART_IER         1
#define UART_DLL         0   /* with DLAB */
#define UART_DLM         1   /* with DLAB */
#define UART_FCR         2   /* write: FIFO control */
#define UART_IIR         2   /* read:  interrupt identification */
#define UART_LCR         3
#define UART_MCR         4
#define UART_LSR         5
#define UART_MSR         6
#define UART_SCRATCH     7

#define LCR_8N1          0x03
#define LCR_DLAB         0x80
#define FCR_ENABLE_CLEAR 0xC7   /* enable FIFOs, clear both, 14-byte trigger */
#define MCR_DTR_RTS_OUT2 0x0B
#define MCR_LOOPBACK     0x10
#define LSR_THR_EMPTY    0x20
#define LSR_DATA_READY   0x01

/* 115200 baud = 1843200 / 16 / divisor, so divisor 1. */
#define BAUD_DIVISOR_115200 1

static bool uart_present;

static bool uart_probe(void)
{
	/* The scratch register is the standard presence test: it is the one
	   register with no hardware meaning, so a value written to it reads
	   back only if something is actually there.  Without this check a
	   machine with no UART would see every console write spin on a
	   status bit that is wired to 0xFF, and 0xFF has LSR_THR_EMPTY set,
	   so it would appear to work and silently discard the log. */
	outb(COM1_BASE + UART_SCRATCH, 0xA5);
	if (inb(COM1_BASE + UART_SCRATCH) != 0xA5)
		return false;
	outb(COM1_BASE + UART_SCRATCH, 0x5A);
	if (inb(COM1_BASE + UART_SCRATCH) != 0x5A)
		return false;
	return true;
}

static bool uart_loopback_test(void)
{
	/* Put the UART in loopback and confirm a byte makes the round trip.
	   This catches the case where the ports decode but nothing is
	   wired behind them. */
	outb(COM1_BASE + UART_MCR, MCR_LOOPBACK | MCR_DTR_RTS_OUT2);
	outb(COM1_BASE + UART_DATA, 0xAE);

	for (int spin = 0; spin < 100000; spin++) {
		if (inb(COM1_BASE + UART_LSR) & LSR_DATA_READY) {
			uint8_t got = inb(COM1_BASE + UART_DATA);
			outb(COM1_BASE + UART_MCR, MCR_DTR_RTS_OUT2);
			return got == 0xAE;
		}
		io_wait();
	}

	outb(COM1_BASE + UART_MCR, MCR_DTR_RTS_OUT2);
	return false;
}

static void uart_putc(char c)
{
	/* Bounded wait.  An unbounded `while (!(lsr & THR_EMPTY))` hangs the
	   whole kernel if the UART stops draining — and the kernel log is
	   exactly the wrong place to be able to hang. */
	for (int spin = 0; spin < 100000; spin++) {
		if (inb(COM1_BASE + UART_LSR) & LSR_THR_EMPTY)
			break;
		io_wait();
	}
	outb(COM1_BASE + UART_DATA, (uint8_t)c);
}

static void serial_write(const char *buf, size_t len)
{
	if (!uart_present)
		return;

	for (size_t i = 0; i < len; i++) {
		/* A terminal expects CRLF; the kernel log stores LF.  Expand
		   here rather than storing CRLF in the ring, so the ring
		   stays byte-for-byte what `dmesg` should print. */
		if (buf[i] == '\n')
			uart_putc('\r');
		uart_putc(buf[i]);
	}
}

static struct console serial_console = {
	.name      = "serial-com1",
	.write     = serial_write,
	.min_level = LOG_DEBUG,
};

void serial_early_init(void)
{
	if (!uart_probe())
		return;

	outb(COM1_BASE + UART_IER, 0x00);                  /* no interrupts */
	outb(COM1_BASE + UART_LCR, LCR_DLAB);
	outb(COM1_BASE + UART_DLL, BAUD_DIVISOR_115200 & 0xFF);
	outb(COM1_BASE + UART_DLM, BAUD_DIVISOR_115200 >> 8);
	outb(COM1_BASE + UART_LCR, LCR_8N1);               /* clears DLAB */
	outb(COM1_BASE + UART_FCR, FCR_ENABLE_CLEAR);
	outb(COM1_BASE + UART_MCR, MCR_DTR_RTS_OUT2);

	if (!uart_loopback_test())
		return;

	uart_present = true;
	console_register(&serial_console);
}

bool serial_available(void)
{
	return uart_present;
}

int serial_getc_nonblock(void)
{
	if (!uart_present)
		return -1;
	if (!(inb(COM1_BASE + UART_LSR) & LSR_DATA_READY))
		return -1;
	return (int)inb(COM1_BASE + UART_DATA);
}
