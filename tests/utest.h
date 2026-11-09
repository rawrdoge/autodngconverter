/*
 * utest.h - minimal single-header unit test harness for C17.
 *
 * Zero dependencies, zero configuration, zero build steps: drop this file in
 * your tree, #include it, and use the TEST_* macros. Public domain.
 *
 * Usage:
 *   #define UTEST_MAIN
 *   #include "utest.h"
 *
 *   TEST(my_test) { ... }
 *
 *   int main(void) { return utest_main(); }
 */
#ifndef OBS_NEUTRAL_BALANCE_UTEST_H
#define OBS_NEUTRAL_BALANCE_UTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(__cplusplus)
// Portable C++ static-registration fallback for MSVC and other compilers.
struct utest_registrar_t {
    explicit utest_registrar_t(utest_entry_t *e) { utest_register(e); }
};
#define UTEST_AUTO_REG(name) static utest_registrar_t utest_auto_reg_##name{&utest_reg_##name}
#elif defined(__GNUC__) || defined(__clang__)
#define UTEST_AUTO_REG(name)                                                       \
    __attribute__((constructor)) static void utest_auto_reg_##name(void) {       \
        utest_register(&utest_reg_##name);                                       \
    }
#else
#error utest.h: automatic test registration requires GCC/Clang or MSVC (C++ mode).
#endif

#define UTEST_STR_(x) #x
#define UTEST_STR(x)  UTEST_STR_(x)

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */
#define UTEST_ASSERT(cond)                                                       \
	do {                                                                    \
		if (!(cond)) {                                                  \
			utest_fail(__FILE__, __LINE__, UTEST_STR(cond));         \
		}                                                               \
	} while (0)

#define UTEST_ASSERT_EQ(a, b)                                                    \
	do {                                                                    \
		if ((a) != (b)) {                                              \
			utest_fail_eq(__FILE__, __LINE__, UTEST_STR(a),        \
				UTEST_STR(b), (long)(a), (long)(b));             \
		}                                                               \
	} while (0)

#define UTEST_ASSERT_DOUBLE(a, b, eps)                                           \
	do {                                                                    \
		utest_assert_double(__FILE__, __LINE__, UTEST_STR(a),          \
			UTEST_STR(b), (double)(a), (double)(b), (double)(eps)); \
	} while (0)

#define UTEST_ASSERT_STRING(a, b)                                                \
	do {                                                                    \
		if (strcmp((a), (b)) != 0) {                                  \
			utest_fail_str(__FILE__, __LINE__, UTEST_STR(a),       \
				UTEST_STR(b), (a), (b));                        \
		}                                                               \
	} while (0)

/* ------------------------------------------------------------------ */
/* Test registration                                                   */
/* ------------------------------------------------------------------ */

#define TEST(name)                                                               \
    static void utest_fn_##name(void);                                        \
    static utest_entry_t utest_reg_##name = {                              \
        #name, utest_fn_##name, __FILE__, __LINE__                          \
    };                                                                       \
    UTEST_AUTO_REG(name)                                                     \
    static void utest_fn_##name(void)

#define UTEST_ENTRY extern utest_entry_t utest_reg_##name

typedef struct utest_entry {
	const char *name;
	void (*fn)(void);
	const char *file;
	int line;
} utest_entry_t;

extern utest_entry_t *utest_entries[];
extern size_t utest_entry_count;

static inline void utest_register(utest_entry_t *e)
{
	utest_entries[utest_entry_count++] = e;
}

/* ------------------------------------------------------------------ */
/* Implementation                                                      */
/* ------------------------------------------------------------------ */
static int utest_failures = 0;
static int utest_total = 0;

utest_entry_t *utest_entries[256];
size_t utest_entry_count = 0;

static void utest_fail(const char *file, int line, const char *expr)
{
	utest_failures++;
	fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
}

static void utest_fail_eq(const char *file, int line,
			  const char *astr, const char *bstr,
			  long a, long b)
{
	utest_failures++;
	fprintf(stderr, "  FAIL %s:%d: %s != %s  (%ld vs %ld)\n",
		file, line, astr, bstr, a, b);
}

static void utest_fail_str(const char *file, int line,
			   const char *astr, const char *bstr,
			   const char *a, const char *b)
{
	utest_failures++;
	fprintf(stderr, "  FAIL %s:%d: %s != %s  (\"%s\" vs \"%s\")\n",
		file, line, astr, bstr, a ? a : "(null)", b ? b : "(null)");
}

static void utest_assert_double(const char *file, int line,
				const char *astr, const char *bstr,
				double a, double b, double eps)
{
	double d = a - b;
	if (d < 0) d = -d;
	if (d <= eps)
		return;
	utest_failures++;
	fprintf(stderr, "  FAIL %s:%d: %s != %s  (%.10g vs %.10g, tol %.3g)\n",
		file, line, astr, bstr, a, b, eps);
}

static inline void utest_run_one(const utest_entry_t *e)
{
	utest_total++;
	int before = utest_failures;
	fprintf(stderr, "  test %-42s ... ", e->name);
	(*e->fn)();
	if (utest_failures == before)
		fprintf(stderr, "ok\n");
}

static inline int utest_main(void)
{
	for (size_t i = 0; i < utest_entry_count; i++)
		utest_run_one(utest_entries[i]);

	fprintf(stderr, "\n%d/%d passed, %d failed\n",
		utest_total - utest_failures, utest_total, utest_failures);
	return utest_failures == 0 ? 0 : 1;
}

#endif /* OBS_NEUTRAL_BALANCE_UTEST_H */
