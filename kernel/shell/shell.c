/* SPDX-License-Identifier: MIT
 *
 * shell.c — the kernel shell.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's shell was shell/shell.c, 3976 lines, running in ring 0 with
 * direct access to everything: it called vga_puts() to write, polled
 * keyboard_getchar() to read, and reached into fat16_write() and
 * ramfs_chmod() directly.  It was the system's only interface and
 * therefore had to contain everything -- a text editor, a calculator, a
 * package manager, a chat client, Tetris.
 *
 * HavenOS's shell is deliberately much smaller, and the reason is
 * structural rather than aesthetic.  A HavenOS program does not need to
 * be part of the shell, or part of the kernel, because there is an ELF
 * loader, a process model and a system-call interface: a text editor is
 * a file in /bin.  So this shell's job is only the part that must be in
 * the kernel -- inspecting and exercising the kernel itself before any
 * userland exists.
 *
 * It runs as a kernel thread, reads from the keyboard or the serial port,
 * and goes through the VFS rather than reaching into filesystems.  Every
 * command here is one that answers a question about the kernel, which is
 * why there is no editor and no game: those belong in /bin, and putting
 * them here would recreate exactly the arrangement that made HavenDOS's
 * shell 4000 lines long.
 */
#include <haven/types.h>
#include <haven/console.h>
#include <haven/printf.h>
#include <haven/string.h>
#include <haven/errno.h>
#include <haven/vfs.h>
#include <haven/mm.h>
#include <haven/sched.h>
#include <haven/smp.h>
#include <haven/proc.h>
#include <haven/time.h>
#include <haven/arch.h>
#include <haven/input.h>
#include <haven/block.h>
#include <haven/device.h>
#include <haven/irq.h>
#include <haven/syscall.h>
#include <haven/selftest.h>
#include <haven/version.h>
#include <haven/shell.h>

#define LINE_MAX   256
#define ARGV_MAX   16
#define HISTORY    8

struct shell_state {
	char     cwd[VFS_PATH_MAX];
	char     history[HISTORY][LINE_MAX];
	unsigned history_count;
	bool     running;
};

static struct shell_state sh;

/* Output goes through the kernel log's raw path rather than klog(), so a
   command's output has no timestamp or level prefix in front of it. */
static void out(const char *fmt, ...) HAVEN_PRINTF(1, 2);

static void out(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	kputs_raw(line);
}

/* Resolve a possibly-relative argument against the shell's cwd. */
static int resolve(const char *in, char *outbuf, size_t outsz)
{
	if (!in || !in[0])
		return vfs_normalise_path(sh.cwd, outbuf, outsz);

	if (in[0] == '/')
		return vfs_normalise_path(in, outbuf, outsz);

	char joined[VFS_PATH_MAX];
	size_t n = strlcpy(joined, sh.cwd, sizeof(joined));
	if (n >= sizeof(joined))
		return -HAVEN_ENAMETOOLONG;
	if (n > 1 && strlcat(joined, "/", sizeof(joined)) >= sizeof(joined))
		return -HAVEN_ENAMETOOLONG;
	if (strlcat(joined, in, sizeof(joined)) >= sizeof(joined))
		return -HAVEN_ENAMETOOLONG;

	return vfs_normalise_path(joined, outbuf, outsz);
}

/* ── Commands ────────────────────────────────────────────────────────── */

static int cmd_help(int argc, char **argv);

static int cmd_ls(int argc, char **argv)
{
	/* Two VFS_PATH_MAX buffers would be 2 KiB of a 16 KiB kernel stack,
	   which the build's -Wframe-larger-than=2048 refuses -- correctly,
	   since a command can be reached from a deep call chain.  So the
	   path scratch space is allocated. */
	struct {
		char path[VFS_PATH_MAX];
		char child[VFS_PATH_MAX];
	} *p = kmalloc(sizeof(*p));
	if (!p)
		return -HAVEN_ENOMEM;

	int rc = resolve(argc > 1 ? argv[1] : NULL, p->path, sizeof(p->path));
	if (rc != 0)
		goto done;

	if (!vfs_exists(p->path)) {
		out("ls: %s: %s\n", p->path, haven_strerror(HAVEN_ENOENT));
		rc = -HAVEN_ENOENT;
		goto done;
	}

	if (!vfs_is_dir(p->path)) {
		struct vfs_stat st;
		if (vfs_stat_path(p->path, &st) == 0)
			out("%-28s %10llu  %s\n", p->path,
			    (unsigned long long)st.size,
			    vfs_node_type_name(st.type));
		rc = 0;
		goto done;
	}

	unsigned count = 0;
	uint64_t total = 0;

	for (uint32_t i = 0; ; i++) {
		struct vfs_dirent de;
		if (vfs_readdir_path(p->path, i, &de) != 0)
			break;

		size_t plen = strlen(p->path);
		snprintf(p->child, sizeof(p->child), "%s%s%s", p->path,
		         (plen && p->path[plen - 1] == '/') ? "" : "/",
		         de.name);

		struct vfs_stat st;
		if (vfs_stat_path(p->child, &st) == 0) {
			char mode[11];
			snprintf(mode, sizeof(mode), "%c%c%c%c%c%c%c%c%c%c",
			         st.type == VFS_NODE_DIR ? 'd'
			         : st.type == VFS_NODE_CHARDEV ? 'c'
			         : st.type == VFS_NODE_BLOCKDEV ? 'b' : '-',
			         (st.perm & VFS_PERM_OWNER_R) ? 'r' : '-',
			         (st.perm & VFS_PERM_OWNER_W) ? 'w' : '-',
			         (st.perm & VFS_PERM_OWNER_X) ? 'x' : '-',
			         (st.perm & VFS_PERM_GROUP_R) ? 'r' : '-',
			         (st.perm & VFS_PERM_GROUP_W) ? 'w' : '-',
			         (st.perm & VFS_PERM_GROUP_X) ? 'x' : '-',
			         (st.perm & VFS_PERM_OTHER_R) ? 'r' : '-',
			         (st.perm & VFS_PERM_OTHER_W) ? 'w' : '-',
			         (st.perm & VFS_PERM_OTHER_X) ? 'x' : '-');
			out("%s %10llu  %s\n", mode,
			    (unsigned long long)st.size, de.name);
			total += st.size;
		} else {
			out("?????????? %10llu  %s\n",
			    (unsigned long long)de.size, de.name);
		}
		count++;
	}

	out("%u entr%s, %llu bytes\n", count, count == 1 ? "y" : "ies",
	    (unsigned long long)total);
	rc = 0;

done:
	kfree(p);
	return rc;
}

static int cmd_cat(int argc, char **argv)
{
	if (argc < 2) {
		out("usage: cat <file>...\n");
		return -HAVEN_EINVAL;
	}

	for (int a = 1; a < argc; a++) {
		char path[VFS_PATH_MAX];
		int rc = resolve(argv[a], path, sizeof(path));
		if (rc != 0)
			return rc;

		struct vfs_file *f = NULL;
		rc = vfs_open(path, O_RDONLY, 0, &f);
		if (rc != 0) {
			out("cat: %s: %s\n", argv[a], haven_strerror(rc));
			continue;
		}

		/* Stream in chunks rather than reading the whole file: a
		   command that allocates the file's size cannot print a file
		   larger than memory, and `cat` of a device has no size at
		   all. */
		char *buf = kmalloc(1024);
		if (!buf) {
			vfs_close(f);
			return -HAVEN_ENOMEM;
		}

		for (;;) {
			ssize_t_k got = vfs_read(f, buf, 1023);
			if (got <= 0)
				break;
			buf[got] = '\0';
			kwrite_raw(buf, (size_t)got);
		}

		kfree(buf);
		vfs_close(f);
	}
	return 0;
}

static int cmd_write(int argc, char **argv)
{
	if (argc < 3) {
		out("usage: write <file> <text>...\n");
		return -HAVEN_EINVAL;
	}

	char path[VFS_PATH_MAX];
	int rc = resolve(argv[1], path, sizeof(path));
	if (rc != 0)
		return rc;

	char body[LINE_MAX];
	body[0] = '\0';
	for (int a = 2; a < argc; a++) {
		if (a > 2)
			strlcat(body, " ", sizeof(body));
		strlcat(body, argv[a], sizeof(body));
	}
	strlcat(body, "\n", sizeof(body));

	ssize_t_k n = vfs_write_file(path, body, strlen(body));
	if (n < 0) {
		out("write: %s: %s\n", argv[1], haven_strerror((int)n));
		return (int)n;
	}

	out("wrote %lld bytes to %s\n", (long long)n, path);
	return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
	if (argc < 2) {
		out("usage: mkdir <path>...\n");
		return -HAVEN_EINVAL;
	}

	for (int a = 1; a < argc; a++) {
		char path[VFS_PATH_MAX];
		int rc = resolve(argv[a], path, sizeof(path));
		if (rc == 0)
			rc = vfs_mkdir(path, VFS_PERM_DEFAULT_DIR);
		if (rc != 0)
			out("mkdir: %s: %s\n", argv[a], haven_strerror(rc));
	}
	return 0;
}

static int cmd_rm(int argc, char **argv)
{
	if (argc < 2) {
		out("usage: rm <path>...\n");
		return -HAVEN_EINVAL;
	}

	for (int a = 1; a < argc; a++) {
		char path[VFS_PATH_MAX];
		int rc = resolve(argv[a], path, sizeof(path));
		if (rc == 0)
			rc = vfs_unlink(path);
		if (rc != 0)
			out("rm: %s: %s\n", argv[a], haven_strerror(rc));
	}
	return 0;
}

static int cmd_cd(int argc, char **argv)
{
	char path[VFS_PATH_MAX];
	int rc = resolve(argc > 1 ? argv[1] : "/", path, sizeof(path));
	if (rc != 0)
		return rc;

	/* Validate before changing: a working directory that does not
	   resolve makes every later relative path fail with a confusing
	   error about the wrong thing. */
	if (!vfs_exists(path)) {
		out("cd: %s: %s\n", path, haven_strerror(HAVEN_ENOENT));
		return -HAVEN_ENOENT;
	}
	if (!vfs_is_dir(path)) {
		out("cd: %s: %s\n", path, haven_strerror(HAVEN_ENOTDIR));
		return -HAVEN_ENOTDIR;
	}

	strlcpy(sh.cwd, path, sizeof(sh.cwd));
	return 0;
}

static int cmd_pwd(int argc, char **argv)
{
	(void)argc; (void)argv;
	out("%s\n", sh.cwd);
	return 0;
}

static int cmd_echo(int argc, char **argv)
{
	for (int a = 1; a < argc; a++)
		out("%s%s", argv[a], a + 1 < argc ? " " : "");
	out("\n");
	return 0;
}

static int cmd_hexdump(int argc, char **argv)
{
	if (argc < 2) {
		out("usage: hexdump <file> [bytes]\n");
		return -HAVEN_EINVAL;
	}

	char path[VFS_PATH_MAX];
	int rc = resolve(argv[1], path, sizeof(path));
	if (rc != 0)
		return rc;

	size_t want = argc > 2 ? (size_t)atoi(argv[2]) : 256;
	if (want == 0 || want > 4096)
		want = 256;

	uint8_t *buf = kzalloc(want);
	if (!buf)
		return -HAVEN_ENOMEM;

	ssize_t_k got = vfs_read_file(path, buf, want);
	if (got < 0) {
		out("hexdump: %s: %s\n", argv[1], haven_strerror((int)got));
		kfree(buf);
		return (int)got;
	}

	for (ssize_t_k off = 0; off < got; off += 16) {
		char line[128];
		int  pos = snprintf(line, sizeof(line), "%08llx  ",
		                    (unsigned long long)off);

		for (int i = 0; i < 16; i++) {
			if (off + i < got)
				pos += snprintf(line + pos,
				                sizeof(line) - (size_t)pos,
				                "%02x ", buf[off + i]);
			else
				pos += snprintf(line + pos,
				                sizeof(line) - (size_t)pos,
				                "   ");
			if (i == 7)
				pos += snprintf(line + pos,
				                sizeof(line) - (size_t)pos, " ");
		}

		pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " |");
		for (int i = 0; i < 16 && off + i < got; i++) {
			uint8_t c = buf[off + i];
			pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
			                "%c", (c >= 0x20 && c < 0x7F) ? c : '.');
		}
		snprintf(line + pos, sizeof(line) - (size_t)pos, "|\n");

		kputs_raw(line);
	}

	kfree(buf);
	return 0;
}

static int cmd_meminfo(int argc, char **argv)
{
	(void)argc; (void)argv;

	struct pmm_stats     pm;
	struct vmm_stats     vm;
	struct kmalloc_stats km;

	pmm_get_stats(&pm);
	vmm_get_stats(&vm);
	kmalloc_get_stats(&km);

	out("physical memory\n");
	out("  total        %10llu pages  %8llu MiB\n",
	    (unsigned long long)pm.total_pages,
	    (unsigned long long)(pm.total_pages * HAVEN_PAGE_SIZE >> 20));
	out("  free         %10llu pages  %8llu MiB\n",
	    (unsigned long long)pm.free_pages,
	    (unsigned long long)(pm.free_pages * HAVEN_PAGE_SIZE >> 20));
	out("  allocated    %10llu pages  %8llu MiB\n",
	    (unsigned long long)pm.allocated_pages,
	    (unsigned long long)(pm.allocated_pages * HAVEN_PAGE_SIZE >> 20));
	out("  reserved     %10llu pages  %8llu MiB\n",
	    (unsigned long long)pm.reserved_pages,
	    (unsigned long long)(pm.reserved_pages * HAVEN_PAGE_SIZE >> 20));
	out("  requests     %10llu alloc, %llu free, %llu failed\n",
	    (unsigned long long)pm.alloc_calls,
	    (unsigned long long)pm.free_calls,
	    (unsigned long long)pm.alloc_failures);

	out("virtual memory\n");
	out("  mapped       %10llu pages\n",
	    (unsigned long long)vm.mapped_pages);
	out("  arena used   %10llu KiB of %llu TiB\n",
	    (unsigned long long)(vm.arena_used_bytes >> 10),
	    (unsigned long long)(vm.arena_total_bytes >> 40));
	out("  page tables  %10llu pages\n",
	    (unsigned long long)vm.pagetable_pages);

	out("kernel allocator\n");
	out("  live         %10llu objects, %llu bytes\n",
	    (unsigned long long)km.live_objects,
	    (unsigned long long)km.live_bytes);
	out("  slab pages   %10llu\n", (unsigned long long)km.slab_pages);
	out("  requests     %10llu alloc, %llu free, %llu failed\n",
	    (unsigned long long)km.total_allocs,
	    (unsigned long long)km.total_frees,
	    (unsigned long long)km.failed_allocs);

	out("integrity      physical %s, objects %s\n",
	    pmm_check() ? "consistent" : "CORRUPT",
	    kmalloc_check() ? "consistent" : "CORRUPT");
	return 0;
}

static int cmd_cpuinfo(int argc, char **argv)
{
	(void)argc; (void)argv;

	out("model        %s\n", arch_cpu_model());
	out("vendor       %s\n", arch_cpu_vendor());
	out("processors   %u online of %u\n", smp_online_count(),
	    smp_cpu_count());

	struct time_stats ts;
	time_get_stats(&ts);
	out("clock        %llu.%03llu MHz, %s\n",
	    (unsigned long long)(ts.clock_hz / 1000000),
	    (unsigned long long)((ts.clock_hz / 1000) % 1000),
	    ts.clock_is_tsc ? "invariant" : "may drift with frequency");
	out("uptime       %llu.%03llu s, %llu ticks\n",
	    (unsigned long long)(time_ms() / 1000),
	    (unsigned long long)(time_ms() % 1000),
	    (unsigned long long)ts.ticks);

	for (uint32_t i = 0; i < smp_cpu_count(); i++) {
		struct haven_cpu *c = cpu_by_id(i);
		if (!c)
			continue;
		out("cpu %u        apic %u, %s, %llu switches, %llu ticks\n",
		    c->cpu_id, c->hw_id, c->online ? "online" : "offline",
		    (unsigned long long)c->context_switches,
		    (unsigned long long)c->ticks);
	}
	return 0;
}

static int cmd_ps(int argc, char **argv)
{
	(void)argc; (void)argv;
	sched_dump();
	proc_dump();
	return 0;
}

static int cmd_dmesg(int argc, char **argv)
{
	(void)argc; (void)argv;

	size_t size = console_ring_size();
	if (size == 0) {
		out("the kernel log ring is empty\n");
		return 0;
	}

	/* Copied in chunks: the ring is 64 KiB and a kernel stack is
	   16 KiB, so a single buffer would overflow it -- the same mistake
	   the console's own replay path made during bring-up. */
	char *chunk = kmalloc(4096);
	if (!chunk)
		return -HAVEN_ENOMEM;

	size_t n = console_ring_read(chunk, 4096);
	kwrite_raw(chunk, n);
	out("\n[showing the last %zu of %zu bytes of kernel log]\n",
	    n, size);

	kfree(chunk);
	return 0;
}

static int cmd_mount(int argc, char **argv)
{
	(void)argc; (void)argv;
	vfs_dump();
	return 0;
}

static int cmd_lsblk(int argc, char **argv)
{
	(void)argc; (void)argv;
	block_dump();
	return 0;
}

static int cmd_lsdev(int argc, char **argv)
{
	(void)argc; (void)argv;
	device_dump();
	return 0;
}

static int cmd_lsirq(int argc, char **argv)
{
	(void)argc; (void)argv;
	irq_dump();
	return 0;
}

static int cmd_uname(int argc, char **argv)
{
	(void)argc; (void)argv;
	out("HavenOS %s \"%s\" %s %s (rev %s)\n",
	    HAVENOS_VERSION, HAVENOS_CODENAME, HAVENOS_ARCH,
	    HAVENOS_BUILD_DATE, HAVENOS_BUILD_REV);
	out("%s\n", HAVENOS_COPYRIGHT);
	out("derived from HavenDOS by Tech Haven Studios\n");
	return 0;
}

static int cmd_date(int argc, char **argv)
{
	(void)argc; (void)argv;

	struct haven_tm tm;
	int rc = time_get_wallclock(&tm);
	if (rc != 0) {
		out("date: no usable wall clock: %s\n", haven_strerror(rc));
		return rc;
	}

	char when[64];
	time_format(&tm, when, sizeof(when));
	out("%s\n", when);
	return 0;
}

static int cmd_sync(int argc, char **argv)
{
	(void)argc; (void)argv;

	int rc = vfs_sync();
	out("sync: %s\n", rc == 0 ? "all filesystems flushed"
	                          : haven_strerror(rc));
	return rc;
}

static int cmd_selftest(int argc, char **argv)
{
	(void)argc; (void)argv;
	return selftest_run_all() ? 0 : -HAVEN_EIO;
}

static int cmd_sleep(int argc, char **argv)
{
	if (argc < 2) {
		out("usage: sleep <milliseconds>\n");
		return -HAVEN_EINVAL;
	}

	int ms = atoi(argv[1]);
	if (ms <= 0)
		return -HAVEN_EINVAL;

	uint64_t before = time_ms();
	time_sleep_ms((uint64_t)ms);
	out("slept %llu ms (asked for %d)\n",
	    (unsigned long long)(time_ms() - before), ms);
	return 0;
}

static int cmd_run(int argc, char **argv)
{
	if (argc < 2) {
		out("usage: run <path-to-elf>\n");
		return -HAVEN_EINVAL;
	}

	char path[VFS_PATH_MAX];
	int rc = resolve(argv[1], path, sizeof(path));
	if (rc != 0)
		return rc;

	const char *child_argv[2] = { path, NULL };
	pid_t_k pid = 0;

	rc = proc_spawn(path, child_argv, NULL, &pid);
	if (rc != 0) {
		out("run: %s: %s\n", path, haven_strerror(rc));
		return rc;
	}

	out("started pid %d\n", pid);
	return 0;
}

static int cmd_reboot(int argc, char **argv)
{
	(void)argc; (void)argv;
	out("flushing filesystems and rebooting...\n");
	vfs_sync();
	console_flush_all();
	arch_reboot();
}

static int cmd_halt(int argc, char **argv)
{
	(void)argc; (void)argv;
	out("flushing filesystems and halting...\n");
	vfs_sync();
	console_flush_all();
	arch_halt();
}

static int cmd_exit(int argc, char **argv)
{
	(void)argc; (void)argv;
	sh.running = false;
	return 0;
}

static int cmd_history(int argc, char **argv)
{
	(void)argc; (void)argv;
	unsigned shown = sh.history_count < HISTORY ? sh.history_count
	                                            : HISTORY;
	for (unsigned i = 0; i < shown; i++)
		out("%3u  %s\n", i + 1, sh.history[i]);
	return 0;
}

/* ── Dispatch table ──────────────────────────────────────────────────── */

struct command {
	const char *name;
	int       (*fn)(int argc, char **argv);
	const char *summary;
};

static const struct command commands[] = {
	{ "help",     cmd_help,     "list these commands" },
	{ "ls",       cmd_ls,       "list a directory" },
	{ "cat",      cmd_cat,      "print a file" },
	{ "hexdump",  cmd_hexdump,  "print a file as hex and ASCII" },
	{ "write",    cmd_write,    "write text to a file" },
	{ "mkdir",    cmd_mkdir,    "create a directory" },
	{ "rm",       cmd_rm,       "delete a file or empty directory" },
	{ "cd",       cmd_cd,       "change the working directory" },
	{ "pwd",      cmd_pwd,      "print the working directory" },
	{ "echo",     cmd_echo,     "print its arguments" },
	{ "mount",    cmd_mount,    "list mounted filesystems" },
	{ "sync",     cmd_sync,     "flush every filesystem to disk" },
	{ "meminfo",  cmd_meminfo,  "memory use and allocator integrity" },
	{ "cpuinfo",  cmd_cpuinfo,  "processors, clock and uptime" },
	{ "ps",       cmd_ps,       "threads and processes" },
	{ "lsblk",    cmd_lsblk,    "block devices" },
	{ "lsdev",    cmd_lsdev,    "devices and their drivers" },
	{ "lsirq",    cmd_lsirq,    "interrupt lines and their handlers" },
	{ "dmesg",    cmd_dmesg,    "the kernel log" },
	{ "uname",    cmd_uname,    "version and build information" },
	{ "date",     cmd_date,     "the wall-clock date and time" },
	{ "sleep",    cmd_sleep,    "block for a number of milliseconds" },
	{ "selftest", cmd_selftest, "run the in-kernel test suite" },
	{ "run",      cmd_run,      "load and run an ELF64 executable" },
	{ "history",  cmd_history,  "recent command lines" },
	{ "reboot",   cmd_reboot,   "flush and restart" },
	{ "halt",     cmd_halt,     "flush and stop" },
	{ "exit",     cmd_exit,     "leave the shell" },
};

static int cmd_help(int argc, char **argv)
{
	(void)argc; (void)argv;

	out("HavenOS kernel shell.  This shell exists to inspect and exercise\n"
	    "the kernel; ordinary programs are ELF files in /bin, loaded with\n"
	    "`run`, not commands built in here.\n\n");

	for (size_t i = 0; i < ARRAY_SIZE(commands); i++)
		out("  %-10s %s\n", commands[i].name, commands[i].summary);

	out("\n");
	return 0;
}

/* ── Line editing and input ──────────────────────────────────────────── */

/*
 * Read one character from whichever input exists.
 *
 * The keyboard when there is one, the serial port otherwise -- and the
 * serial port is checked even when a keyboard exists, because a machine
 * being driven over a cable has no keyboard to press.  HavenDOS polled
 * only the keyboard, which is why its shell could not be used over
 * serial at all.
 */
static int shell_getchar(void)
{
	for (;;) {
		int c = kbd_getchar_nonblock();
		if (c >= 0)
			return c;

		c = serial_getc_nonblock();
		if (c >= 0)
			return c;

		/* Nothing waiting.  Sleep rather than spin: a shell at a
		   prompt should cost no CPU, and on a single-processor
		   machine spinning here would starve everything else. */
		time_sleep_ms(10);
	}
}

static void read_line(char *buf, size_t bufsz)
{
	size_t len = 0;
	buf[0] = '\0';

	for (;;) {
		int c = shell_getchar();

		if (c == '\r' || c == '\n') {
			kputs_raw("\n");
			buf[len] = '\0';
			return;
		}

		if (c == '\b' || c == 0x7F) {
			if (len > 0) {
				len--;
				buf[len] = '\0';
				/* Erase visually as well as logically: a
				   backspace alone moves the cursor without
				   removing the character. */
				kputs_raw("\b \b");
			}
			continue;
		}

		if (c == 0x03) {            /* Ctrl-C */
			kputs_raw("^C\n");
			buf[0] = '\0';
			return;
		}

		if (c == 0x04) {            /* Ctrl-D */
			if (len == 0) {
				kputs_raw("exit\n");
				strlcpy(buf, "exit", bufsz);
				return;
			}
			continue;
		}

		if (c == 0x15) {            /* Ctrl-U: erase the line */
			while (len > 0) {
				kputs_raw("\b \b");
				len--;
			}
			buf[0] = '\0';
			continue;
		}

		if (c < 0x20 || c > 0x7E)
			continue;

		if (len + 1 >= bufsz)
			continue;           /* full; ignore rather than wrap */

		buf[len++] = (char)c;
		buf[len]   = '\0';

		char echo[2] = { (char)c, '\0' };
		kputs_raw(echo);
	}
}

static int split(char *line, char **argv, int max)
{
	int argc = 0;
	char *save = NULL;

	for (char *tok = strtok_r(line, " \t", &save);
	     tok && argc < max - 1;
	     tok = strtok_r(NULL, " \t", &save)) {
		argv[argc++] = tok;
	}
	argv[argc] = NULL;
	return argc;
}

static void remember(const char *line)
{
	if (!line[0])
		return;

	/* A ring of the last HISTORY lines, oldest first.  Shifting rather
	   than wrapping keeps `history` output in order without an index. */
	if (sh.history_count < HISTORY) {
		strlcpy(sh.history[sh.history_count], line, LINE_MAX);
		sh.history_count++;
	} else {
		for (unsigned i = 1; i < HISTORY; i++)
			memcpy(sh.history[i - 1], sh.history[i], LINE_MAX);
		strlcpy(sh.history[HISTORY - 1], line, LINE_MAX);
		sh.history_count++;
	}
}

void shell_run(void)
{
	memset(&sh, 0, sizeof(sh));
	strlcpy(sh.cwd, "/", sizeof(sh.cwd));
	sh.running = true;

	out("\n");
	out("HavenOS %s \"%s\" kernel shell\n",
	    HAVENOS_VERSION, HAVENOS_CODENAME);
	out("Type `help` for commands.  Input comes from the keyboard or the\n"
	    "serial console, whichever has something to say.\n\n");

	char line[LINE_MAX];
	char *argv[ARGV_MAX];

	while (sh.running) {
		out("haven:%s$ ", sh.cwd);
		read_line(line, sizeof(line));

		char saved[LINE_MAX];
		strlcpy(saved, line, sizeof(saved));

		int argc = split(line, argv, ARGV_MAX);
		if (argc == 0)
			continue;

		remember(saved);

		bool found = false;
		for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
			if (strcmp(argv[0], commands[i].name) != 0)
				continue;
			commands[i].fn(argc, argv);
			found = true;
			break;
		}

		if (!found) {
			/* A bare path that names an executable is run, which
			   is what a shell should do -- but say so explicitly
			   rather than silently doing nothing for a typo. */
			char path[VFS_PATH_MAX];
			if (resolve(argv[0], path, sizeof(path)) == 0 &&
			    vfs_exists(path) && !vfs_is_dir(path)) {
				argv[1] = NULL;
				char *run_argv[3] = { (char *)"run", path, NULL };
				cmd_run(2, run_argv);
			} else {
				out("%s: not a command and not an executable; "
				    "try `help`\n", argv[0]);
			}
		}
	}

	out("leaving the shell; the kernel continues running\n");
}

static void shell_thread(void *arg)
{
	(void)arg;

	/* Give the boot messages a moment to finish, so the prompt is not
	   interleaved with them on a serial console. */
	time_sleep_ms(100);

	for (;;) {
		shell_run();
		/* The shell exiting should not end the session: start it
		   again, which is what a login shell does. */
		time_sleep_ms(500);
	}
}

int shell_start(void)
{
	struct thread *t = thread_create("shell", shell_thread, NULL,
	                                 SCHED_PRIO_NORMAL);
	if (!t)
		return -HAVEN_ENOMEM;
	return 0;
}
