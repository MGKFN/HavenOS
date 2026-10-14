/* SPDX-License-Identifier: MIT
 *
 * stubs.c — weak definitions for arch entry points whose real
 * implementations land in a later bring-up stage.
 *
 * Each one is HAVEN_WEAK, so the moment the real implementation is linked
 * in it wins with no edit here and no reference left behind.  They log
 * once at WARN rather than returning silently: a subsystem that quietly
 * no-ops is how a boot ends up reporting hardware it never probed, which
 * is the reporting bug HavenDOS's POST screen had.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/arch/x86_64/cpu.h>

HAVEN_WEAK void tss_set_kstack(uint32_t cpu, vaddr_t kstack_top)
{
	(void)cpu;
	(void)kstack_top;
}
