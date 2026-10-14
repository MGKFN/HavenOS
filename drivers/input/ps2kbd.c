/* SPDX-License-Identifier: MIT
 *
 * ps2kbd.c — PS/2 keyboard.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * Inherited in substance from HavenDOS's drivers/keyboard.c, with the
 * scancode tables kept because they were correct.  What changes is the
 * delivery model.
 *
 * HavenDOS's keyboard_getchar() polled: the shell called it in a loop and
 * the loop was the system.  With a scheduler, polling is wrong twice
 * over -- it burns a CPU, and it means a program not currently calling
 * getchar() loses keystrokes.  Here the interrupt handler puts the
 * character in a ring buffer and wakes anyone waiting, so a key pressed
 * while nothing is reading is still there when something does.
 *
 * WHY A RING BUFFER AND NOT A SINGLE CHARACTER
 * A single-character buffer drops the second key of a fast pair, which is
 * immediately noticeable to anyone who types at speed.  256 entries is
 * more than a human can produce between two scheduler ticks.
 */
#include <haven/device.h>
#include <haven/devfs.h>
#include <haven/irq.h>
#include <haven/sched.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/spinlock.h>
#include <haven/input.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/idt.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_MOUSE_DATA  0x20

#define KBD_IRQ IRQ_LINE_LEGACY(1)

/* Scancode set 1, the set every PC keyboard controller emits by default.
   Index is the scancode; the value is the character for an unshifted
   key.  0 means a key with no character (a modifier, a function key). */
static const char scancode_plain[128] = {
	0,    0,   '1', '2', '3', '4', '5', '6',
	'7',  '8', '9', '0', '-', '=', '\b', '\t',
	'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
	'o',  'p', '[', ']', '\n', 0,  'a', 's',
	'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
	'\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
	'b',  'n', 'm', ',', '.', '/', 0,   '*',
	0,    ' ', 0,   0,   0,   0,   0,   0,
};

static const char scancode_shift[128] = {
	0,    0,   '!', '@', '#', '$', '%', '^',
	'&',  '*', '(', ')', '_', '+', '\b', '\t',
	'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
	'O',  'P', '{', '}', '\n', 0,  'A', 'S',
	'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
	'"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
	'B',  'N', 'M', '<', '>', '?', 0,   '*',
	0,    ' ', 0,   0,   0,   0,   0,   0,
};

/* Scancodes for the modifier and lock keys. */
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D
#define SC_ALT      0x38
#define SC_CAPSLOCK 0x3A
#define SC_RELEASE  0x80

#define KBD_RING_SIZE 256

static struct {
	char     ring[KBD_RING_SIZE];
	uint32_t head, tail;
	struct spinlock  lock;
	struct list_head waiters;

	bool shift, ctrl, alt, caps;
	bool extended;        /* the previous byte was the 0xE0 prefix */
	bool present;

	uint64_t keys_seen;
	uint64_t dropped;
} kbd;

static void ring_put(char c)
{
	uint32_t next = (kbd.head + 1) % KBD_RING_SIZE;

	if (next == kbd.tail) {
		/* Full.  Dropping the NEWEST keystroke rather than the
		   oldest is deliberate: a program reading slowly should see
		   what was typed first, in order, not a window of the most
		   recent. */
		kbd.dropped++;
		return;
	}

	kbd.ring[kbd.head] = c;
	kbd.head = next;
}

static enum irq_result kbd_interrupt(uint32_t irq, void *context)
{
	(void)irq;
	(void)context;

	/* The 8042 is shared with the mouse.  A byte with the mouse-data
	   bit set belongs to the mouse, and consuming it here would make
	   the keyboard interpret mouse movement as keystrokes. */
	uint8_t status = inb(PS2_STATUS);
	if (!(status & PS2_STATUS_OUTPUT_FULL))
		return IRQ_NONE;
	if (status & PS2_STATUS_MOUSE_DATA)
		return IRQ_NONE;

	uint8_t sc = inb(PS2_DATA);

	/* 0xE0 prefixes the extended keys (arrows, right-hand modifiers).
	   The byte after it is a different key with the same code as an
	   unprefixed one, so it must not be looked up in the same table. */
	if (sc == 0xE0) {
		kbd.extended = true;
		return IRQ_HANDLED;
	}

	bool released = (sc & SC_RELEASE) != 0;
	uint8_t code  = sc & 0x7F;

	if (kbd.extended) {
		kbd.extended = false;
		/* The extended keys produce no characters yet; they are
		   consumed so they do not appear as their unprefixed
		   equivalents, which is what would make the up arrow type
		   a letter. */
		return IRQ_HANDLED;
	}

	switch (code) {
	case SC_LSHIFT:
	case SC_RSHIFT:
		kbd.shift = !released;
		return IRQ_HANDLED;
	case SC_CTRL:
		kbd.ctrl = !released;
		return IRQ_HANDLED;
	case SC_ALT:
		kbd.alt = !released;
		return IRQ_HANDLED;
	case SC_CAPSLOCK:
		/* A lock toggles on press only.  Toggling on release as well
		   would make every press a no-op. */
		if (!released)
			kbd.caps = !kbd.caps;
		return IRQ_HANDLED;
	default:
		break;
	}

	if (released)
		return IRQ_HANDLED;

	char c = kbd.shift ? scancode_shift[code] : scancode_plain[code];
	if (c == 0)
		return IRQ_HANDLED;

	/* Caps lock affects letters only, not digits or punctuation --
	   which is why it cannot simply reuse the shift table. */
	if (kbd.caps) {
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		else if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
	}

	/* Control characters: Ctrl-A is 1, Ctrl-C is 3, and so on. */
	if (kbd.ctrl) {
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 1);
		else if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 1);
	}

	kbd.keys_seen++;

	/* The ring is touched from the interrupt handler and from a reading
	   thread, so it needs the lock -- and the lock must be the spinning
	   kind, because this is interrupt context. */
	irqflags_t f = spin_lock(&kbd.lock);
	ring_put(c);
	spin_unlock(&kbd.lock, f);

	/* Wake a blocked reader.  sched_wake_all() is interrupt-safe, which
	   is what makes a blocking read possible at all. */
	sched_wake_all(&kbd.waiters);

	return IRQ_HANDLED;
}

int kbd_getchar_nonblock(void)
{
	if (!kbd.present)
		return -1;

	irqflags_t f = spin_lock(&kbd.lock);
	if (kbd.head == kbd.tail) {
		spin_unlock(&kbd.lock, f);
		return -1;
	}

	char c = kbd.ring[kbd.tail];
	kbd.tail = (kbd.tail + 1) % KBD_RING_SIZE;
	spin_unlock(&kbd.lock, f);

	return (int)(uint8_t)c;
}

int kbd_getchar(void)
{
	for (;;) {
		int c = kbd_getchar_nonblock();
		if (c >= 0)
			return c;
		if (!kbd.present)
			return -1;

		/* Block rather than spin.  HavenDOS's shell polled, which
		   was the only option without a scheduler and which means
		   the CPU runs flat out while waiting for a keypress. */
		if (sched_active())
			sched_block_on(&kbd.waiters);
		else
			return -1;
	}
}

static ssize_t_k kbd_devfs_read(void *data, void *buf, size_t len,
                                haven_off_t offset)
{
	(void)data;
	(void)offset;

	uint8_t *out = buf;
	size_t   got = 0;

	/* Block for the first character, then take whatever else is
	   waiting.  A read that returned 0 because nothing had been typed
	   yet would make a shell spin. */
	while (got < len) {
		int c = got == 0 ? kbd_getchar() : kbd_getchar_nonblock();
		if (c < 0)
			break;
		out[got++] = (uint8_t)c;
	}

	return (ssize_t_k)got;
}

static const struct devfs_driver kbd_devfs = {
	.name = "keyboard",
	.read = kbd_devfs_read,
};

/* Drain anything the firmware left in the controller.  A byte still
   sitting in the output buffer means the controller never asserts its
   interrupt again, so the keyboard appears dead from the first
   keypress. */
static void ps2_drain(void)
{
	for (int i = 0; i < 64; i++) {
		if (!(inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL))
			return;
		(void)inb(PS2_DATA);
	}
}

void ps2kbd_init(void)
{
	memset(&kbd, 0, sizeof(kbd));
	spin_lock_init(&kbd.lock, "ps2kbd");
	list_init(&kbd.waiters);

	ps2_drain();

	int rc = irq_request(KBD_IRQ, kbd_interrupt, NULL, "ps2-keyboard");
	if (rc != 0) {
		kerr("ps2kbd: cannot take IRQ 1: %s", haven_strerror(rc));
		return;
	}

	kbd.present = true;
	devfs_register("keyboard", &kbd_devfs, NULL, VFS_NODE_CHARDEV,
	               DEV_MAJOR_KBD, 0, 0444);

	kinfo("ps2kbd: scancode set 1, %d-entry ring buffer, interrupt-driven "
	      "(a key pressed while nothing is reading is not lost)",
	      KBD_RING_SIZE);
}

void kbd_get_stats(uint64_t *seen, uint64_t *dropped)
{
	if (seen)
		*seen = kbd.keys_seen;
	if (dropped)
		*dropped = kbd.dropped;
}

bool kbd_available(void)
{
	return kbd.present;
}
