/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Various utility for unit testing */

#ifndef __CROS_EC_TEST_UTIL_H
#define __CROS_EC_TEST_UTIL_H

#include "common.h"
#include "console.h"

#define RUN_TEST(n) \
	do { \
		ccprintf("Running %s...", #n); \
		cflush(); \
		if (n() == EC_SUCCESS) { \
			ccputs("OK\n"); \
		} else { \
			ccputs("Fail\n"); \
			__test_error_count++; \
		} \
	} while (0)

#define TEST_ASSERT(n) \
	do { \
		if (!(n)) { \
			ccprintf("%d: ASSERTION failed: %s\n", __LINE__, #n); \
			return EC_ERROR_UNKNOWN; \
		} \
	} while (0)

#define __ABS(n) ((n) > 0 ? (n) : -(n))

#define TEST_ASSERT_ABS_LESS(n, t) \
	do { \
		if (__ABS(n) >= t) { \
			ccprintf("%d: ASSERT_ABS_LESS failed: abs(%d) is " \
				 "not less than %d\n", __LINE__, n, t); \
			return EC_ERROR_UNKNOWN; \
		} \
	} while (0)

#define TEST_CHECK(n) \
	do { \
		if (n) \
			return EC_SUCCESS; \
		else \
			return EC_ERROR_UNKNOWN; \
	} while (0)

void run_test(void);

void test_reset(void);

void test_print_result(void);

extern int __test_error_count;

#endif /* __CROS_EC_TEST_UTIL_H */