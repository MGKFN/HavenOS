/* SPDX-License-Identifier: MIT
 *
 * test_vfs.c — exercise the real fs/vfs.c and fs/ramfs.c.
 *
 * The path-normalisation tests are the most important here, because
 * vfs_normalise_path() is a security boundary: without it a process could
 * reach any file in the system from any starting directory.  They are
 * written as the attacks they defend against.
 *
 * The ramfs tests target the four limits HavenDOS's fixed-array ramfs had
 * -- 64 files, 2 KiB per file, truncation reported as success, and
 * directories faked with slashes in names -- so each test would have
 * failed against that design.
 */
#include "harness.h"

#include <haven/vfs.h>
#include <haven/ramfs.h>
#include <haven/errno.h>
#include <haven/mutex.h>

/* Scheduler stubs: mutexes block, and nothing here contends, so a mutex
   reduces to a no-op.  Providing these rather than linking the scheduler
   keeps the test to one subject. */
struct thread;
struct thread *thread_current(void) { return NULL; }
void sched_block_on(struct list_head *q) { (void)q; }
void sched_wake_one(struct list_head *q) { (void)q; }
void sched_wake_all(struct list_head *q) { (void)q; }
void sched_wake(struct thread *t) { (void)t; }
void sched_yield(void) { }
bool sched_active(void) { return false; }
bool in_interrupt(void) { return false; }
void preempt_disable(void) { }
void preempt_enable(void) { }

static void norm(const char *in, char *out, size_t sz)
{
	int rc = vfs_normalise_path(in, out, sz);
	if (rc != 0)
		snprintf(out, sz, "<error %d>", rc);
}

static void test_path_normalisation(void)
{
	TEST_GROUP("path normalisation");

	char out[512];

	norm("/a/b/c", out, sizeof(out));
	CHECK_STR("an already-clean path is unchanged", out, "/a/b/c");

	norm("a/b", out, sizeof(out));
	CHECK_STR("a relative path becomes absolute", out, "/a/b");

	norm("/", out, sizeof(out));
	CHECK_STR("the root stays the root", out, "/");

	norm("", out, sizeof(out));
	CHECK_STR("an empty path is the root", out, "/");

	norm("///a////b//", out, sizeof(out));
	CHECK_STR("repeated and trailing separators collapse", out, "/a/b");

	norm("/a/./b/./c", out, sizeof(out));
	CHECK_STR("'.' components are dropped", out, "/a/b/c");

	norm("/a/b/../c", out, sizeof(out));
	CHECK_STR("'..' removes the previous component", out, "/a/c");

	norm("/a/b/c/../../d", out, sizeof(out));
	CHECK_STR("consecutive '..' each remove one", out, "/a/d");

	norm("/a/..", out, sizeof(out));
	CHECK_STR("'..' back to the root yields the root", out, "/");
}

static void test_path_traversal_attacks(void)
{
	TEST_GROUP("path traversal is contained");

	char out[512];

	/* The classic attack.  If any of these escaped, a process could
	   read any file on the system from anywhere. */
	norm("../../../../etc/passwd", out, sizeof(out));
	CHECK_STR("leading '..' cannot escape above the root",
	          out, "/etc/passwd");

	norm("/a/../../../../../../etc/shadow", out, sizeof(out));
	CHECK_STR("more '..' than depth is clamped at the root",
	          out, "/etc/shadow");

	norm("/safe/../../../root/.ssh/id_rsa", out, sizeof(out));
	CHECK_STR("a traversal from a subdirectory is clamped",
	          out, "/root/.ssh/id_rsa");

	norm("/a/b/../../..", out, sizeof(out));
	CHECK_STR("ending in '..' at the root is the root", out, "/");

	/* "..." and "..foo" are ordinary names, not traversals, and must
	   not be treated as "..". */
	norm("/a/.../b", out, sizeof(out));
	CHECK_STR("'...' is an ordinary name", out, "/a/.../b");

	norm("/a/..b/c", out, sizeof(out));
	CHECK_STR("'..b' is an ordinary name", out, "/a/..b/c");

	norm("/a/.hidden", out, sizeof(out));
	CHECK_STR("a leading dot is an ordinary name", out, "/a/.hidden");

	/* An output buffer too small must fail rather than truncate: a
	   truncated path is a different path, and acting on it would be
	   acting on a file nobody named. */
	char tiny[8];
	CHECK_EQ("a path too long for the buffer is rejected, not truncated",
	         vfs_normalise_path("/aaaa/bbbb/cccc", tiny, sizeof(tiny)),
	         -HAVEN_ENAMETOOLONG);
}

static void mount_fresh(void)
{
	haven_test_make_machine(32ULL << 20);
	haven_test_reset_pmm();
	vfs_init();
	int rc = ramfs_mount_at("/");
	CHECK_EQ("the root filesystem mounts", rc, 0);
}

static void test_mount(void)
{
	TEST_GROUP("mounting");

	mount_fresh();

	CHECK("the root resolves", vfs_exists("/"));
	CHECK("the root is a directory", vfs_is_dir("/"));

	/* A second mount at the same point must be refused: which one
	   find_mount() would choose would otherwise depend on table order,
	   which no caller can reason about. */
	CHECK_EQ("mounting twice at the same point is refused",
	         ramfs_mount_at("/"), -HAVEN_EBUSY);

	CHECK_EQ("a nested mount succeeds", ramfs_mount_at("/mnt"), 0);

	haven_test_free_machine();
}

static void test_file_lifecycle(void)
{
	TEST_GROUP("create, write, read, delete");

	mount_fresh();

	CHECK("a file does not exist before it is created",
	      !vfs_exists("/hello.txt"));

	const char *msg = "HavenOS filesystem test\n";
	ssize_t_k n = vfs_write_file("/hello.txt", msg, strlen(msg));
	CHECK_EQ("writing a new file writes every byte", n,
	         (long long)strlen(msg));
	CHECK("the file now exists", vfs_exists("/hello.txt"));
	CHECK("and is not a directory", !vfs_is_dir("/hello.txt"));

	char buf[128];
	memset(buf, 0, sizeof(buf));
	n = vfs_read_file("/hello.txt", buf, sizeof(buf));
	CHECK_EQ("reading it back returns the same length", n,
	         (long long)strlen(msg));
	CHECK_STR("and the same contents", buf, msg);

	struct vfs_stat st;
	CHECK_EQ("stat succeeds", vfs_stat_path("/hello.txt", &st), 0);
	CHECK_EQ("the size is right", st.size, strlen(msg));
	CHECK_EQ("the type is file", st.type, VFS_NODE_FILE);

	CHECK_EQ("unlink succeeds", vfs_unlink("/hello.txt"), 0);
	CHECK("the file is gone", !vfs_exists("/hello.txt"));
	CHECK_EQ("unlinking it again reports ENOENT",
	         vfs_unlink("/hello.txt"), -HAVEN_ENOENT);

	haven_test_free_machine();
}

static void test_large_file(void)
{
	TEST_GROUP("files larger than HavenDOS's 2 KiB per-file limit");

	mount_fresh();

	/* HavenDOS's ramfs had `#define MAX_DATA 2048` and a write of more
	   than that did `if (size > MAX_DATA) size = MAX_DATA;` and then
	   returned the TRUNCATED size as if it had written everything.  So
	   a 64 KiB write is the exact case that failed silently there. */
	const size_t big = 64 * 1024;
	uint8_t *src = malloc(big);
	uint8_t *dst = malloc(big);
	for (size_t i = 0; i < big; i++)
		src[i] = (uint8_t)(i * 31 + 7);

	ssize_t_k n = vfs_write_file("/big.bin", src, big);
	CHECK_EQ("a 64 KiB write reports all 64 KiB written", n, (long long)big);

	struct vfs_stat st;
	vfs_stat_path("/big.bin", &st);
	CHECK_EQ("the file is 64 KiB", st.size, big);

	memset(dst, 0, big);
	n = vfs_read_file("/big.bin", dst, big);
	CHECK_EQ("reading it back returns 64 KiB", n, (long long)big);
	CHECK_EQ("every byte survived the round trip",
	         memcmp(src, dst, big), 0);

	free(src);
	free(dst);
	haven_test_free_machine();
}

static void test_many_files(void)
{
	TEST_GROUP("more files than HavenDOS's 64-slot limit");

	mount_fresh();

	/* HavenDOS had `#define MAX_FILES 64` and ramfs_alloc() returned -1
	   past it, which most callers ignored.  200 files is the case that
	   silently failed there. */
	int created = 0;
	for (int i = 0; i < 200; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/f%03d.txt", i);
		char body[32];
		int blen = snprintf(body, sizeof(body), "file %d", i);
		if (vfs_write_file(path, body, (size_t)blen) == blen)
			created++;
	}
	CHECK_EQ("all 200 files were created", created, 200);

	/* And every one is readable with its own contents, so they are not
	   aliasing each other. */
	int correct = 0;
	for (int i = 0; i < 200; i++) {
		char path[64], want[32], got[64];
		snprintf(path, sizeof(path), "/f%03d.txt", i);
		int wlen = snprintf(want, sizeof(want), "file %d", i);
		memset(got, 0, sizeof(got));
		if (vfs_read_file(path, got, sizeof(got)) == wlen &&
		    memcmp(got, want, (size_t)wlen) == 0)
			correct++;
	}
	CHECK_EQ("every file reads back its own contents", correct, 200);

	haven_test_free_machine();
}

static void test_directories(void)
{
	TEST_GROUP("real directories, nested arbitrarily deep");

	mount_fresh();

	CHECK_EQ("mkdir /a", vfs_mkdir("/a", 0755), 0);
	CHECK_EQ("mkdir /a/b", vfs_mkdir("/a/b", 0755), 0);
	CHECK_EQ("mkdir /a/b/c", vfs_mkdir("/a/b/c", 0755), 0);
	CHECK_EQ("mkdir /a/b/c/d", vfs_mkdir("/a/b/c/d", 0755), 0);

	/* HavenDOS faked directories by putting a slash in the file name
	   and its ramfs_list_dir() explicitly refused to go deeper than one
	   level: "Don't recurse into deeper subdirs -- only one level."
	   Four levels is the case that could not exist there. */
	CHECK("a four-level directory exists", vfs_is_dir("/a/b/c/d"));

	const char *deep = "deep file\n";
	CHECK_EQ("a file four levels down can be written",
	         vfs_write_file("/a/b/c/d/x.txt", deep, strlen(deep)),
	         (long long)strlen(deep));

	char buf[64];
	memset(buf, 0, sizeof(buf));
	CHECK_EQ("and read back", vfs_read_file("/a/b/c/d/x.txt", buf,
	                                       sizeof(buf)),
	         (long long)strlen(deep));
	CHECK_STR("with its contents intact", buf, deep);

	CHECK_EQ("mkdir of an existing directory reports EEXIST",
	         vfs_mkdir("/a", 0755), -HAVEN_EEXIST);
	CHECK_EQ("mkdir inside a missing parent reports ENOENT",
	         vfs_mkdir("/nope/x", 0755), -HAVEN_ENOENT);

	/* A path continuing past a file is ENOTDIR, which is the specific
	   answer rather than the generic "not found" HavenDOS would give. */
	vfs_write_file("/plain.txt", "x", 1);
	struct vfs_stat st;
	CHECK_EQ("a path through a file reports ENOTDIR",
	         vfs_stat_path("/plain.txt/more", &st), -HAVEN_ENOTDIR);

	/* A non-empty directory must not be removable. */
	CHECK_EQ("rmdir of a non-empty directory reports ENOTEMPTY",
	         vfs_unlink("/a/b"), -HAVEN_ENOTEMPTY);

	haven_test_free_machine();
}

static void test_readdir(void)
{
	TEST_GROUP("readdir enumerates and terminates");

	mount_fresh();

	vfs_mkdir("/d", 0755);
	vfs_write_file("/d/one", "1", 1);
	vfs_write_file("/d/two", "22", 2);
	vfs_write_file("/d/three", "333", 3);

	int found = 0;
	bool saw_one = false, saw_two = false, saw_three = false;

	for (uint32_t i = 0; i < 32; i++) {
		struct vfs_dirent de;
		int rc = vfs_readdir_path("/d", i, &de);
		if (rc == -HAVEN_ENOENT)
			break;
		CHECK_EQ("readdir of a valid index succeeds", rc, 0);
		found++;
		if (strcmp(de.name, "one") == 0)   saw_one = true;
		if (strcmp(de.name, "two") == 0)   saw_two = true;
		if (strcmp(de.name, "three") == 0) saw_three = true;
	}

	CHECK_EQ("exactly three entries", found, 3);
	CHECK("'one' was listed", saw_one);
	CHECK("'two' was listed", saw_two);
	CHECK("'three' was listed", saw_three);

	struct vfs_dirent de;
	CHECK_EQ("an index past the end reports ENOENT, which is how a "
	         "caller's loop terminates",
	         vfs_readdir_path("/d", 99, &de), -HAVEN_ENOENT);
	CHECK_EQ("readdir on a file reports ENOTDIR",
	         vfs_readdir_path("/d/one", 0, &de), -HAVEN_ENOTDIR);

	haven_test_free_machine();
}

static void test_offsets_and_holes(void)
{
	TEST_GROUP("offsets, sparse writes and truncation");

	mount_fresh();

	struct vfs_file *f = NULL;
	CHECK_EQ("open for writing with create",
	         vfs_open("/sparse.bin", O_RDWR | O_CREAT, 0644, &f), 0);

	/* Write at a large offset, leaving a hole.  The hole must read as
	   zeroes, not as whatever those pages held before -- which would be
	   an information leak from whatever freed them. */
	CHECK_EQ("seek to 20000", vfs_seek(f, 20000, SEEK_SET_), 20000);
	CHECK_EQ("write 4 bytes there", vfs_write(f, "DATA", 4), 4);

	struct vfs_stat st;
	vfs_stat_path("/sparse.bin", &st);
	CHECK_EQ("the file is 20004 bytes", st.size, 20004);

	uint8_t probe[64];
	memset(probe, 0xFF, sizeof(probe));
	CHECK_EQ("seek back into the hole", vfs_seek(f, 1000, SEEK_SET_), 1000);
	CHECK_EQ("read from the hole", vfs_read(f, probe, sizeof(probe)),
	         (long long)sizeof(probe));

	bool all_zero = true;
	for (size_t i = 0; i < sizeof(probe); i++) {
		if (probe[i] != 0) {
			all_zero = false;
			break;
		}
	}
	CHECK("a hole reads as zeroes, with no stale data", all_zero);

	/* Read past the end returns 0, not an error. */
	CHECK_EQ("seek past the end", vfs_seek(f, 99999, SEEK_SET_), 99999);
	CHECK_EQ("reading past the end returns 0, meaning end of file",
	         vfs_read(f, probe, sizeof(probe)), 0);

	CHECK_EQ("a negative seek is rejected",
	         vfs_seek(f, -1, SEEK_SET_), -HAVEN_EINVAL);

	vfs_close(f);

	CHECK_EQ("truncate to 100", vfs_truncate("/sparse.bin", 100), 0);
	vfs_stat_path("/sparse.bin", &st);
	CHECK_EQ("the size follows", st.size, 100);

	CHECK_EQ("truncate to 0", vfs_truncate("/sparse.bin", 0), 0);
	vfs_stat_path("/sparse.bin", &st);
	CHECK_EQ("the file is empty", st.size, 0);

	haven_test_free_machine();
}

static void test_open_flags(void)
{
	TEST_GROUP("open flags mean what they say");

	mount_fresh();

	struct vfs_file *f = NULL;

	CHECK_EQ("opening a missing file without O_CREAT reports ENOENT",
	         vfs_open("/missing", O_RDONLY, 0, &f), -HAVEN_ENOENT);

	CHECK_EQ("O_CREAT creates it",
	         vfs_open("/made", O_RDWR | O_CREAT, 0644, &f), 0);
	vfs_close(f);

	CHECK_EQ("O_CREAT|O_EXCL on an existing file reports EEXIST, which "
	         "is what makes a lock file work",
	         vfs_open("/made", O_RDWR | O_CREAT | O_EXCL, 0644, &f),
	         -HAVEN_EEXIST);

	vfs_write_file("/made", "0123456789", 10);

	CHECK_EQ("O_TRUNC empties it",
	         vfs_open("/made", O_WRONLY | O_TRUNC, 0, &f), 0);
	struct vfs_stat st;
	vfs_stat_path("/made", &st);
	CHECK_EQ("the file is now empty", st.size, 0);
	vfs_close(f);

	/* A read on a write-only descriptor must be refused: honouring it
	   would ignore the access mode the caller asked for. */
	CHECK_EQ("open write-only", vfs_open("/made", O_WRONLY, 0, &f), 0);
	char c;
	CHECK_EQ("reading a write-only descriptor reports EBADF",
	         vfs_read(f, &c, 1), -HAVEN_EBADF);
	vfs_close(f);

	CHECK_EQ("open read-only", vfs_open("/made", O_RDONLY, 0, &f), 0);
	CHECK_EQ("writing a read-only descriptor reports EBADF",
	         vfs_write(f, "x", 1), -HAVEN_EBADF);
	vfs_close(f);

	vfs_mkdir("/adir", 0755);
	CHECK_EQ("opening a directory for writing reports EISDIR",
	         vfs_open("/adir", O_WRONLY, 0, &f), -HAVEN_EISDIR);
	CHECK_EQ("O_DIRECTORY on a file reports ENOTDIR",
	         vfs_open("/made", O_RDONLY | O_DIRECTORY, 0, &f),
	         -HAVEN_ENOTDIR);

	haven_test_free_machine();
}

static void test_append(void)
{
	TEST_GROUP("O_APPEND");

	mount_fresh();

	vfs_write_file("/log", "first\n", 6);

	struct vfs_file *f = NULL;
	CHECK_EQ("open for append",
	         vfs_open("/log", O_WRONLY | O_APPEND, 0, &f), 0);
	CHECK_EQ("append writes", vfs_write(f, "second\n", 7), 7);
	vfs_close(f);

	char buf[64];
	memset(buf, 0, sizeof(buf));
	vfs_read_file("/log", buf, sizeof(buf));
	CHECK_STR("the append went to the end, not the start",
	          buf, "first\nsecond\n");

	haven_test_free_machine();
}

static void test_rename(void)
{
	TEST_GROUP("rename");

	mount_fresh();

	vfs_mkdir("/x", 0755);
	vfs_mkdir("/y", 0755);
	vfs_write_file("/x/a.txt", "contents", 8);

	CHECK_EQ("rename within a directory",
	         vfs_rename("/x/a.txt", "/x/b.txt"), 0);
	CHECK("the new name exists", vfs_exists("/x/b.txt"));
	CHECK("the old name is gone", !vfs_exists("/x/a.txt"));

	CHECK_EQ("rename across directories",
	         vfs_rename("/x/b.txt", "/y/c.txt"), 0);
	CHECK("the file moved", vfs_exists("/y/c.txt"));

	char buf[32];
	memset(buf, 0, sizeof(buf));
	vfs_read_file("/y/c.txt", buf, sizeof(buf));
	CHECK_STR("and its contents moved with it", buf, "contents");

	CHECK_EQ("renaming a missing file reports ENOENT",
	         vfs_rename("/x/nothere", "/x/z"), -HAVEN_ENOENT);

	vfs_write_file("/y/taken", "t", 1);
	CHECK_EQ("renaming onto an existing name reports EEXIST",
	         vfs_rename("/y/c.txt", "/y/taken"), -HAVEN_EEXIST);

	/* Moving a directory into its own subtree would detach it from the
	   root: the subtree becomes unreachable and a path walk through it
	   never terminates. */
	vfs_mkdir("/p", 0755);
	vfs_mkdir("/p/q", 0755);
	CHECK_EQ("moving a directory into its own subtree is refused",
	         vfs_rename("/p", "/p/q/p"), -HAVEN_EINVAL);

	haven_test_free_machine();
}

static void test_permissions(void)
{
	TEST_GROUP("permissions reach the owning filesystem");

	mount_fresh();

	vfs_write_file("/perm.txt", "x", 1);

	CHECK_EQ("chmod succeeds", vfs_chmod("/perm.txt", 0600), 0);

	struct vfs_stat st;
	vfs_stat_path("/perm.txt", &st);
	CHECK_EQ("the mode was stored", st.perm, 0600);

	CHECK_EQ("chown succeeds", vfs_chown("/perm.txt", 1000, 1000), 0);
	vfs_stat_path("/perm.txt", &st);
	CHECK_EQ("the uid was stored", st.uid, 1000);
	CHECK_EQ("the gid was stored", st.gid, 1000);

	/* Octal permissions can express "readable by anyone, writable by
	   its owner", which HavenDOS's single admin_only boolean could
	   not. */
	vfs_chmod("/perm.txt", 0644);
	vfs_stat_path("/perm.txt", &st);
	CHECK("owner may write", (st.perm & VFS_PERM_OWNER_W) != 0);
	CHECK("others may read", (st.perm & VFS_PERM_OTHER_R) != 0);
	CHECK("others may not write", (st.perm & VFS_PERM_OTHER_W) == 0);

	haven_test_free_machine();
}

int main(void)
{
	printf("HavenOS host tests: virtual filesystem and ramfs\n");
	printf("================================================\n");
	printf("linking the real fs/vfs.c and fs/ramfs.c\n");

	haven_test_capture_log(true);

	test_path_normalisation();
	test_path_traversal_attacks();
	test_mount();
	test_file_lifecycle();
	test_large_file();
	test_many_files();
	test_directories();
	test_readdir();
	test_offsets_and_holes();
	test_open_flags();
	test_append();
	test_rename();
	test_permissions();

	haven_test_capture_log(false);
	return haven_test_report("vfs");
}
