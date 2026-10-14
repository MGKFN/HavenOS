/* SPDX-License-Identifier: MIT
 *
 * cmdline.c — whole-word kernel command-line parsing.
 *
 * Every option HavenOS understands is listed in docs/USER.md.  Adding one
 * means adding it there too: an option the GRUB menu offers and the kernel
 * ignores is worse than no option at all, because the menu entry is a
 * promise the system does not keep.  That is exactly the state the
 * generated GRUB menu was in before this file existed — it passed
 * `loglevel=debug` and `console=serial` to a kernel that looked at neither.
 */
#include <haven/cmdline.h>
#include <haven/console.h>
#include <haven/string.h>

static bool serial_only;

bool cmdline_has(const struct boot_info *bi, const char *flag)
{
	size_t flen = strlen(flag);
	const char *p = bi->cmdline;

	while (*p) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;

		const char *start = p;
		while (*p && *p != ' ')
			p++;

		size_t tlen = (size_t)(p - start);
		if (tlen == flen && strncmp(start, flag, flen) == 0)
			return true;
	}
	return false;
}

bool cmdline_value(const struct boot_info *bi, const char *key,
                   char *out, size_t outsz)
{
	size_t klen = strlen(key);
	const char *p = bi->cmdline;

	if (!out || outsz == 0)
		return false;
	out[0] = '\0';

	while (*p) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;

		const char *start = p;
		while (*p && *p != ' ')
			p++;

		size_t tlen = (size_t)(p - start);
		/* The token must be exactly `key=` followed by the value, so
		   "loglevel" alone and "myloglevel=debug" both miss. */
		if (tlen > klen + 1 && start[klen] == '=' &&
		    strncmp(start, key, klen) == 0) {
			const char *val = start + klen + 1;
			size_t vlen = tlen - klen - 1;

			if (vlen >= outsz)
				vlen = outsz - 1;
			memcpy(out, val, vlen);
			out[vlen] = '\0';
			return true;
		}
	}
	return false;
}

static bool parse_level(const char *s, enum log_level *out)
{
	static const struct { const char *name; enum log_level level; } names[] = {
		{ "panic", LOG_PANIC }, { "err",   LOG_ERR },
		{ "error", LOG_ERR },   { "warn",  LOG_WARN },
		{ "info",  LOG_INFO },  { "debug", LOG_DEBUG },
		{ "quiet", LOG_WARN },
	};

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (strcmp(s, names[i].name) == 0) {
			*out = names[i].level;
			return true;
		}
	}

	/* A single digit is accepted too, because that is what a person who
	   has used another kernel will try first. */
	if (s[0] >= '0' && s[0] <= '9' && s[1] == '\0') {
		unsigned n = (unsigned)(s[0] - '0');
		if (n < LOG_LEVEL_COUNT) {
			*out = (enum log_level)n;
			return true;
		}
	}
	return false;
}

void cmdline_apply_early(const struct boot_info *bi)
{
	char value[16];

	if (cmdline_value(bi, "loglevel", value, sizeof(value))) {
		enum log_level level;

		if (parse_level(value, &level)) {
			console_set_level(level);
			kinfo("cmdline: log level set to '%s'", value);
		} else {
			kwarn("cmdline: loglevel='%s' not recognised; "
			      "keeping the default", value);
		}
	}

	if (cmdline_value(bi, "console", value, sizeof(value))) {
		if (strcmp(value, "serial") == 0) {
			serial_only = true;
			kinfo("cmdline: console=serial; a firmware framebuffer "
			      "will not be taken over");
		} else if (strcmp(value, "both") == 0 ||
		           strcmp(value, "fb") == 0) {
			serial_only = false;
		} else {
			kwarn("cmdline: console='%s' not recognised; "
			      "using every console found", value);
		}
	}
}

bool cmdline_console_serial_only(void)
{
	return serial_only;
}
