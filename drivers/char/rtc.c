/* SPDX-License-Identifier: MIT
 *
 * rtc.c — the CMOS real-time clock.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * Inherited from HavenDOS's drivers/rtc.c, which read the same registers
 * for the same purpose -- its desktop showed a live clock and its
 * taskbar showed the date.  Two things are added.
 *
 * THE UPDATE-IN-PROGRESS RACE
 * The CMOS clock increments its registers one at a time.  Reading at the
 * wrong moment can see 01:59:60 becoming 01:00:00 -- the hour not yet
 * carried while the minute has wrapped -- which is an hour wrong, once an
 * hour.  The fix is to read twice and accept the value only when two
 * consecutive reads agree, with the update-in-progress flag checked
 * first.  It is a small amount of code for a bug that is very hard to
 * reproduce deliberately.
 *
 * BCD IS NOT GUARANTEED
 * Most machines report binary-coded decimal, but status register B says
 * which, and a few report binary.  Assuming BCD on a binary machine turns
 * 10 into 16.
 */
#include <haven/time.h>
#include <haven/devfs.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/arch/x86_64/io.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

#define CMOS_SECOND  0x00
#define CMOS_MINUTE  0x02
#define CMOS_HOUR     0x04
#define CMOS_WEEKDAY  0x06
#define CMOS_DAY      0x07
#define CMOS_MONTH    0x08
#define CMOS_YEAR     0x09
#define CMOS_CENTURY  0x32
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80
#define STATUS_B_24HOUR   0x02
#define STATUS_B_BINARY   0x04

static bool rtc_present;

static uint8_t cmos_read(uint8_t reg)
{
	/* Bit 7 of the index port is the NMI-disable bit.  Clearing it
	   while writing the index -- which a naive `outb(0x70, reg)` does
	   -- enables NMI as a side effect.  Preserving it is why the index
	   is written as (reg | 0x80) only when NMI was already masked;
	   here NMI is left as firmware set it. */
	outb(CMOS_ADDRESS, reg);
	io_wait();
	return inb(CMOS_DATA);
}

static bool rtc_updating(void)
{
	return (cmos_read(CMOS_STATUS_A) & STATUS_A_UPDATING) != 0;
}

static uint8_t from_bcd(uint8_t v)
{
	return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static void read_raw(struct haven_tm *out, uint8_t *century)
{
	out->second  = cmos_read(CMOS_SECOND);
	out->minute  = cmos_read(CMOS_MINUTE);
	out->hour    = cmos_read(CMOS_HOUR);
	out->weekday = cmos_read(CMOS_WEEKDAY);
	out->day     = cmos_read(CMOS_DAY);
	out->month   = cmos_read(CMOS_MONTH);
	out->year    = cmos_read(CMOS_YEAR);
	*century     = cmos_read(CMOS_CENTURY);
}

int platform_read_rtc(struct haven_tm *out)
{
	if (!out)
		return -HAVEN_EINVAL;
	if (!rtc_present)
		return -HAVEN_ENODEV;

	struct haven_tm a, b;
	uint8_t cent_a = 0, cent_b = 0;

	/* Read twice and require agreement.  Bounded, because a clock that
	   never settles must not hang the caller -- and an RTC whose
	   update-in-progress flag is stuck high is a real failure mode on
	   damaged hardware. */
	int attempts = 0;
	do {
		if (++attempts > 100) {
			kerr("rtc: the clock never settled over 100 reads; "
			     "the hardware is not responding correctly");
			return -HAVEN_EIO;
		}

		int spins = 0;
		while (rtc_updating() && ++spins < 1000000)
			;

		read_raw(&a, &cent_a);

		while (rtc_updating())
			;

		read_raw(&b, &cent_b);
	} while (memcmp(&a, &b, sizeof(a)) != 0 || cent_a != cent_b);

	uint8_t status_b = cmos_read(CMOS_STATUS_B);
	bool    binary   = (status_b & STATUS_B_BINARY) != 0;
	bool    hour24   = (status_b & STATUS_B_24HOUR) != 0;

	/* In 12-hour mode bit 7 of the hour register marks PM, and it must
	   be stripped before the BCD conversion or the result is nonsense. */
	bool pm = !hour24 && (a.hour & 0x80);
	a.hour &= 0x7F;

	if (!binary) {
		a.second  = from_bcd((uint8_t)a.second);
		a.minute  = from_bcd((uint8_t)a.minute);
		a.hour    = from_bcd((uint8_t)a.hour);
		a.day     = from_bcd((uint8_t)a.day);
		a.month   = from_bcd((uint8_t)a.month);
		a.year    = from_bcd((uint8_t)a.year);
		a.weekday = from_bcd((uint8_t)a.weekday);
		cent_a    = from_bcd(cent_a);
	}

	if (!hour24) {
		/* 12 AM is hour 0 and 12 PM is hour 12; the naive
		   "add 12 if PM" turns 12 PM into 24. */
		if (a.hour == 12)
			a.hour = pm ? 12 : 0;
		else if (pm)
			a.hour = (uint8_t)(a.hour + 12);
	}

	/* The century register is not implemented on every machine and
	   reads as 0 or 0xFF when absent.  The 1970-2069 windowing rule is
	   what every other system uses for that case. */
	uint16_t year;
	if (cent_a >= 19 && cent_a <= 21)
		year = (uint16_t)(cent_a * 100 + a.year);
	else
		year = (uint16_t)(a.year < 70 ? 2000 + a.year : 1900 + a.year);

	out->year    = year;
	out->month   = a.month;
	out->day     = a.day;
	out->hour    = a.hour;
	out->minute  = a.minute;
	out->second  = a.second;
	/* The CMOS weekday is 1-7 with Sunday as 1; struct haven_tm uses
	   0-6 with Sunday as 0. */
	out->weekday = (uint8_t)(a.weekday ? a.weekday - 1 : 0);

	/* A plainly impossible date means the battery is dead or the
	   registers are not an RTC.  Reporting it beats displaying it:
	   HavenDOS showed whatever CMOS held with no indication of
	   whether it was trustworthy. */
	if (out->month < 1 || out->month > 12 ||
	    out->day < 1 || out->day > 31 ||
	    out->hour > 23 || out->minute > 59 || out->second > 59 ||
	    year < 1970 || year > 2200) {
		kwarn("rtc: the clock reports %04u-%02u-%02u %02u:%02u:%02u, "
		      "which is not a valid date; the CMOS battery may be "
		      "flat", year, out->month, out->day,
		      out->hour, out->minute, out->second);
		return -HAVEN_ECORRUPT;
	}

	return 0;
}

static ssize_t_k rtc_devfs_read(void *data, void *buf, size_t len,
                                haven_off_t offset)
{
	(void)data;

	struct haven_tm tm;
	if (platform_read_rtc(&tm) != 0)
		return -HAVEN_EIO;

	char line[64];
	time_format(&tm, line, sizeof(line));
	size_t n = strlen(line);
	if (n < sizeof(line) - 1)
		line[n++] = '\n';

	if (offset >= n)
		return 0;

	size_t avail = n - (size_t)offset;
	if (len > avail)
		len = avail;
	memcpy(buf, line + offset, len);
	return (ssize_t_k)len;
}

static const struct devfs_driver rtc_devfs = {
	.name = "rtc",
	.read = rtc_devfs_read,
};

void rtc_init(void)
{
	/* A machine with no RTC reads 0xFF from every CMOS register. */
	uint8_t a = cmos_read(CMOS_STATUS_A);
	uint8_t b = cmos_read(CMOS_STATUS_B);

	if (a == 0xFF && b == 0xFF) {
		kwarn("rtc: no CMOS real-time clock; HavenOS has no wall "
		      "clock on this machine");
		return;
	}

	rtc_present = true;

	struct haven_tm tm;
	int rc = platform_read_rtc(&tm);
	if (rc != 0) {
		kerr("rtc: present but unreadable: %s", haven_strerror(rc));
		return;
	}

	char when[64];
	time_format(&tm, when, sizeof(when));
	kinfo("rtc: %s (%s, %s-hour)", when,
	      (b & STATUS_B_BINARY) ? "binary" : "BCD",
	      (b & STATUS_B_24HOUR) ? "24" : "12");

	devfs_register("rtc", &rtc_devfs, NULL, VFS_NODE_CHARDEV,
	               DEV_MAJOR_RTC, 0, 0444);
}
