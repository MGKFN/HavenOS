/* SPDX-License-Identifier: MIT
 *
 * selftest.h — in-kernel tests that need real hardware.
 */
#ifndef _HAVEN_SELFTEST_H
#define _HAVEN_SELFTEST_H

#include <haven/types.h>

/* Run every in-kernel test.  Returns true when all checks passed.  Safe
   to call on a live kernel: no test leaves state behind, and a test whose
   prerequisites are absent reports skipped rather than failing. */
bool selftest_run_all(void);

void selftest_summary(unsigned *run, unsigned *failed);

#endif /* _HAVEN_SELFTEST_H */
