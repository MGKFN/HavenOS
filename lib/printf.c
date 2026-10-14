/* SPDX-License-Identifier: MIT
 *
 * printf.c — the kernel's only format engine.
 *
 * Supported: %c %s %d %i %u %x %X %o %p %% and %b (binary), with the
 * length modifiers l, ll, z, and the flags '-', '0', '+', ' ', '#',
 * a numeric or '*' field width, and a numeric or '*' precision (which
 * bounds %s and sets minimum digits for integers).
 *
 * Floating point is deliberately absent: the kernel does not enable SSE
 * for its own use and has nothing to print that needs it.  A %f in kernel
 * code is a bug, so it renders as "%f" rather than being silently dropped.
 *
 * Every write goes through emit(), which counts unconditionally but only
 * stores while there is room.  That is what lets the function return the
 * length it *wanted*, the way C99 snprintf does, so callers can detect
 * truncation.  HavenDOS's vga_printf() had no return value at all and
 * wrote directly to video memory, so truncation was invisible.
 */
#include <haven/printf.h>
#include <haven/string.h>

struct sink {
	char  *buf;
	size_t size;   /* bytes available in buf, including the NUL slot */
	size_t count;  /* bytes the caller asked us to produce, total */
};

static void emit(struct sink *s, char c)
{
	if (s->buf && s->count + 1 < s->size)
		s->buf[s->count] = c;
	s->count++;
}

static void emit_str(struct sink *s, const char *str, size_t len)
{
	for (size_t i = 0; i < len; i++)
		emit(s, str[i]);
}

struct spec {
	bool left;      /* '-' */
	bool zero;      /* '0' */
	bool plus;      /* '+' */
	bool space;     /* ' ' */
	bool alt;       /* '#' */
	bool upper;     /* %X */
	int  width;
	int  prec;      /* -1 = unspecified */
};

/* Render one already-stringified number body with sign/prefix, honouring
   width, zero padding and left alignment.  Kept separate from the digit
   conversion so %d, %u, %x, %o, %p and %b all share the padding rules. */
static void pad_number(struct sink *s, const struct spec *sp,
                       const char *sign, const char *prefix, const char *body)
{
	size_t slen = strlen(sign);
	size_t plen = strlen(prefix);
	size_t blen = strlen(body);

	/* Precision on an integer means "at least this many digits". */
	size_t zeros = 0;
	if (sp->prec >= 0 && blen < (size_t)sp->prec)
		zeros = (size_t)sp->prec - blen;
	/* '0' flag pads with zeros to the field width, but an explicit
	   precision overrides it (C99 7.19.6.1). */
	else if (sp->zero && !sp->left && sp->width > 0) {
		size_t have = slen + plen + blen;
		if ((size_t)sp->width > have)
			zeros = (size_t)sp->width - have;
	}

	size_t total = slen + plen + zeros + blen;
	size_t padding = ((size_t)sp->width > total) ? (size_t)sp->width - total : 0;

	if (!sp->left)
		for (size_t i = 0; i < padding; i++)
			emit(s, ' ');

	emit_str(s, sign, slen);
	emit_str(s, prefix, plen);
	for (size_t i = 0; i < zeros; i++)
		emit(s, '0');
	emit_str(s, body, blen);

	if (sp->left)
		for (size_t i = 0; i < padding; i++)
			emit(s, ' ');
}

static void fmt_unsigned(struct sink *s, const struct spec *sp,
                         uint64_t val, int base, const char *prefix)
{
	char body[66];
	utoa64(val, body, sizeof(body), base);

	if (sp->upper)
		str_upper(body, sizeof(body));

	/* '#' on zero prints no prefix, matching C99. */
	pad_number(s, sp, "", (val != 0 && sp->alt) ? prefix : "", body);
}

static void fmt_signed(struct sink *s, const struct spec *sp, int64_t val)
{
	char body[66];
	uint64_t mag;

	if (val < 0)
		/* Negate through unsigned so INT64_MIN is representable. */
		mag = (uint64_t)(-(val + 1)) + 1ULL;
	else
		mag = (uint64_t)val;

	utoa64(mag, body, sizeof(body), 10);

	const char *sign = "";
	if (val < 0)
		sign = "-";
	else if (sp->plus)
		sign = "+";
	else if (sp->space)
		sign = " ";

	pad_number(s, sp, sign, "", body);
}

static void fmt_string(struct sink *s, const struct spec *sp, const char *str)
{
	/* A null %s is a bug at the call site, but printing "(null)" beats
	   faulting inside the logger that is trying to report the bug. */
	if (!str)
		str = "(null)";

	size_t len = (sp->prec >= 0) ? strnlen(str, (size_t)sp->prec)
	                             : strlen(str);
	size_t padding = ((size_t)sp->width > len) ? (size_t)sp->width - len : 0;

	if (!sp->left)
		for (size_t i = 0; i < padding; i++)
			emit(s, ' ');
	emit_str(s, str, len);
	if (sp->left)
		for (size_t i = 0; i < padding; i++)
			emit(s, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct sink s = { .buf = buf, .size = size, .count = 0 };

	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			emit(&s, *p);
			continue;
		}

		p++;
		if (*p == '\0')
			break;
		if (*p == '%') {
			emit(&s, '%');
			continue;
		}

		struct spec sp = { .prec = -1 };

		/* Flags */
		for (;; p++) {
			if (*p == '-')      sp.left  = true;
			else if (*p == '0') sp.zero  = true;
			else if (*p == '+') sp.plus  = true;
			else if (*p == ' ') sp.space = true;
			else if (*p == '#') sp.alt   = true;
			else                break;
		}

		/* Field width */
		if (*p == '*') {
			int w = va_arg(ap, int);
			if (w < 0) {
				sp.left = true;
				/* INT_MIN has no positive counterpart; clamp. */
				sp.width = (w == INT32_MIN) ? INT32_MAX : -w;
			} else {
				sp.width = w;
			}
			p++;
		} else {
			while (*p >= '0' && *p <= '9') {
				sp.width = sp.width * 10 + (*p - '0');
				p++;
			}
		}

		/* Precision */
		if (*p == '.') {
			p++;
			sp.prec = 0;
			if (*p == '*') {
				int pr = va_arg(ap, int);
				/* A negative precision is as if omitted. */
				sp.prec = (pr < 0) ? -1 : pr;
				p++;
			} else {
				while (*p >= '0' && *p <= '9') {
					sp.prec = sp.prec * 10 + (*p - '0');
					p++;
				}
			}
		}

		/* Length modifiers.  'h'/'hh' are accepted and ignored because
		   default promotion already widened the argument to int. */
		int longness = 0;
		for (;;) {
			if (*p == 'l') {
				longness++;
				p++;
			} else if (*p == 'z' || *p == 't') {
				longness = 2;
				p++;
			} else if (*p == 'h') {
				p++;
			} else {
				break;
			}
		}

		switch (*p) {
		case 'c': {
			char c = (char)va_arg(ap, int);
			char one[2] = { c, '\0' };
			struct spec cs = sp;
			cs.prec = -1;            /* precision is meaningless for %c */
			fmt_string(&s, &cs, one);
			break;
		}
		case 's':
			fmt_string(&s, &sp, va_arg(ap, const char *));
			break;
		case 'd':
		case 'i':
			if (longness >= 2)
				fmt_signed(&s, &sp, va_arg(ap, int64_t));
			else if (longness == 1)
				fmt_signed(&s, &sp, (int64_t)va_arg(ap, long));
			else
				fmt_signed(&s, &sp, (int64_t)va_arg(ap, int));
			break;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'b': {
			int base = (*p == 'u') ? 10
			         : (*p == 'o') ? 8
			         : (*p == 'b') ? 2
			                       : 16;
			const char *prefix = (base == 16) ? "0x"
			                   : (base == 8)  ? "0"
			                   : (base == 2)  ? "0b"
			                                  : "";
			sp.upper = (*p == 'X');

			uint64_t v;
			if (longness >= 2)
				v = va_arg(ap, uint64_t);
			else if (longness == 1)
				v = (uint64_t)va_arg(ap, unsigned long);
			else
				v = (uint64_t)va_arg(ap, unsigned int);

			fmt_unsigned(&s, &sp, v, base, prefix);
			break;
		}
		case 'p': {
			/* %p always prints 0x-prefixed lower-case hex.  Kernel
			   logs are read side by side with disassembly, so a
			   bare address with no prefix is a transcription hazard. */
			void    *ptr = va_arg(ap, void *);
			struct spec ps = sp;
			ps.alt  = true;
			ps.prec = -1;
			if (!ptr) {
				fmt_string(&s, &sp, "(nil)");
				break;
			}
			fmt_unsigned(&s, &ps, (uint64_t)(uintptr_t)ptr, 16, "0x");
			break;
		}
		default:
			/* Unknown conversion: echo it verbatim so the bad format
			   string is visible in the log instead of vanishing. */
			emit(&s, '%');
			emit(&s, *p);
			break;
		}
	}

	if (s.buf && s.size > 0) {
		size_t term = (s.count < s.size - 1) ? s.count : s.size - 1;
		s.buf[term] = '\0';
	}

	return (int)s.count;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
