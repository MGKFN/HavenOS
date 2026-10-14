/* SPDX-License-Identifier: MIT
 *
 * mem.c — the small pseudo-devices every system needs.
 *
 *   /dev/console   the kernel log, writable; reads come from the serial
 *                  console when one is present
 *   /dev/null      discards writes, reads as end of file
 *   /dev/zero      reads as an endless run of zero bytes
 *   /dev/random    a non-cryptographic entropy source
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had kernel/entropy.c but no way for anything outside the
 * kernel to reach it, and no /dev at all, so output always went through
 * vga_puts() and could not be redirected.  These four nodes are what make
 * `>`, `<` and `2>` mean something.
 */
#include <haven/devfs.h>
#include <haven/console.h>
#include <haven/string.h>
#include <haven/errno.h>
#include <haven/arch.h>
#include <haven/time.h>
#include <haven/random.h>

/* ── /dev/console ────────────────────────────────────────────────────── */

static ssize_t_k console_dev_write(void *data, const void *buf, size_t len,
                                   haven_off_t offset)
{
	(void)data;
	(void)offset;   /* a console has no position */

	if (len == 0)
		return 0;

	/* Written raw, with no level prefix or timestamp: this is a program's
	   output, not a kernel message, and decorating it would corrupt
	   anything being piped. */
	kwrite_raw((const char *)buf, len);
	return (ssize_t_k)len;
}

static ssize_t_k console_dev_read(void *data, void *buf, size_t len,
                                 haven_off_t offset)
{
	(void)data;
	(void)offset;

	if (len == 0)
		return 0;

	/* Non-blocking: return whatever is waiting.  A blocking console read
	   needs a wait queue fed by the keyboard interrupt, which arrives
	   with the input layer; until then returning 0 means "nothing yet"
	   rather than pretending end of file. */
	uint8_t *out = buf;
	size_t   got = 0;

	while (got < len) {
		int c = serial_getc_nonblock();
		if (c < 0)
			break;
		out[got++] = (uint8_t)c;
	}

	return (ssize_t_k)got;
}

static const struct devfs_driver console_driver = {
	.name  = "console",
	.read  = console_dev_read,
	.write = console_dev_write,
};

/* ── /dev/null ───────────────────────────────────────────────────────── */

static ssize_t_k null_write(void *data, const void *buf, size_t len,
                            haven_off_t offset)
{
	(void)data; (void)buf; (void)offset;
	/* Accepts everything and reports it all written, which is the
	   contract: a program writing to /dev/null must not see a short
	   write and retry forever. */
	return (ssize_t_k)len;
}

static ssize_t_k null_read(void *data, void *buf, size_t len,
                           haven_off_t offset)
{
	(void)data; (void)buf; (void)len; (void)offset;
	return 0;   /* immediate end of file */
}

static const struct devfs_driver null_driver = {
	.name  = "null",
	.read  = null_read,
	.write = null_write,
};

/* ── /dev/zero ───────────────────────────────────────────────────────── */

static ssize_t_k zero_read(void *data, void *buf, size_t len,
                           haven_off_t offset)
{
	(void)data; (void)offset;
	memset(buf, 0, len);
	return (ssize_t_k)len;
}

static const struct devfs_driver zero_driver = {
	.name  = "zero",
	.read  = zero_read,
	.write = null_write,   /* writes are discarded, as on /dev/null */
};

/* ── /dev/random ─────────────────────────────────────────────────────── */

/*
 * A xoshiro256** generator, seeded from the TSC and from timing jitter.
 *
 * This is explicitly NOT cryptographic, and saying so matters: a caller
 * that needs key material must not get it from here believing otherwise.
 * HavenDOS's kernel/entropy.c had the same property and the same
 * limitation; what changes here is that the limitation is stated at the
 * point of use.
 *
 * A real CSPRNG needs an entropy pool fed by interrupt timing and a
 * cryptographic mixer (ChaCha20 or similar), which belongs with the
 * security-hardening phase; see docs/DESIGN_DECISIONS.md.
 */
static uint64_t prng_state[4];
static bool     prng_seeded;

static inline uint64_t rotl64(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t splitmix64(uint64_t *x)
{
	uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void prng_seed(void)
{
	/* Mix several independent sources, because any one of them is weak:
	   the clock is predictable if the boot is reproducible, and the
	   loop jitter is small.  Together they are adequate for the
	   non-cryptographic uses this device is documented for. */
	uint64_t seed = time_ns();

	for (int i = 0; i < 16; i++) {
		uint64_t before = time_ns();
		for (volatile int spin = 0; spin < 97; spin++)
			;
		seed ^= rotl64(time_ns() - before, i * 4);
	}

	seed ^= (uint64_t)(vaddr_t)&prng_state;   /* layout entropy */

	for (int i = 0; i < 4; i++)
		prng_state[i] = splitmix64(&seed);

	prng_seeded = true;
}

static uint64_t prng_next(void)
{
	if (!prng_seeded)
		prng_seed();

	uint64_t result = rotl64(prng_state[1] * 5, 7) * 9;
	uint64_t t      = prng_state[1] << 17;

	prng_state[2] ^= prng_state[0];
	prng_state[3] ^= prng_state[1];
	prng_state[1] ^= prng_state[2];
	prng_state[0] ^= prng_state[3];
	prng_state[2] ^= t;
	prng_state[3]  = rotl64(prng_state[3], 45);

	return result;
}

static ssize_t_k random_read(void *data, void *buf, size_t len,
                             haven_off_t offset)
{
	(void)data; (void)offset;

	uint8_t *out  = buf;
	size_t   done = 0;

	while (done < len) {
		uint64_t word  = prng_next();
		size_t   chunk = len - done;
		if (chunk > sizeof(word))
			chunk = sizeof(word);
		memcpy(out + done, &word, chunk);
		done += chunk;
	}

	return (ssize_t_k)done;
}

static ssize_t_k random_write(void *data, const void *buf, size_t len,
                              haven_off_t offset)
{
	(void)data; (void)offset;

	/* Writing to /dev/random contributes entropy.  Mixing rather than
	   replacing matters: a caller writing predictable data must not be
	   able to make the generator predictable. */
	const uint8_t *in = buf;
	for (size_t i = 0; i < len; i++) {
		uint64_t seed = prng_state[i % 4] ^ in[i];
		prng_state[i % 4] = splitmix64(&seed);
	}
	return (ssize_t_k)len;
}

static const struct devfs_driver random_driver = {
	.name  = "random",
	.read  = random_read,
	.write = random_write,
};

/* Kernel-internal entropy, for anything that needs a random number
   without going through the filesystem. */
uint64_t haven_random64(void)
{
	return prng_next();
}

void mem_devices_init(void)
{
	devfs_register("console", &console_driver, NULL, VFS_NODE_CHARDEV,
	               DEV_MAJOR_CONSOLE, 0, 0622);
	devfs_register("null", &null_driver, NULL, VFS_NODE_CHARDEV,
	               DEV_MAJOR_NULL, 0, 0666);
	devfs_register("zero", &zero_driver, NULL, VFS_NODE_CHARDEV,
	               DEV_MAJOR_ZERO, 0, 0666);
	devfs_register("random", &random_driver, NULL, VFS_NODE_CHARDEV,
	               DEV_MAJOR_RANDOM, 0, 0666);

	kinfo("devfs: /dev/random is a xoshiro256** generator seeded from "
	      "clock jitter -- adequate for general use, NOT for keys");
}
