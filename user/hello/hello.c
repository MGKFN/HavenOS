/* SPDX-License-Identifier: MIT
 *
 * hello -- the first program shipped with HavenOS.
 *
 * It exists so that a freshly booted system has something to `run`, and
 * as the smallest complete example of a HavenOS program: no C library,
 * just the system-call wrappers in <haven/user.h> and the crt0 that calls
 * main().  Build your own the same way; see docs/USER.md.
 */
#include <haven/user.h>

static size_t slen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static void say(const char *s)
{
	sys_write(1, s, slen(s));
}

static void say_number(long v)
{
	char buf[24];
	int  i = (int)sizeof(buf);

	buf[--i] = '\0';
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v > 0);
	say(&buf[i]);
}

int main(int argc, char **argv)
{
	say("Hello from user mode.\n");
	say("  this is process ");
	say_number(sys_getpid());
	say(", running in ring 3 with its own address space\n");

	for (int i = 1; i < argc; i++) {
		say("  argument ");
		say_number(i);
		say(": ");
		say(argv[i]);
		say("\n");
	}

	return 0;
}
