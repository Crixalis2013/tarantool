#include "memory.h"
#include "fiber.h"
#include "unit.h"
#include "trivia/util.h"

static struct fiber_attr default_attr;

static int
noop_f(va_list ap)
{
	return 0;
}

static int
cancel_f(va_list ap)
{
	fiber_set_cancellable(true);
	while (true) {
		fiber_sleep(0.001);
		fiber_testcancel();
	}
	return 0;
}

static int
exception_f(va_list ap)
{
	tnt_raise(OutOfMemory, 42, "allocator", "exception");
	return 0;
}

static int
no_exception_f(va_list ap)
{
	try {
		tnt_raise(OutOfMemory, 42, "allocator", "exception");
	} catch (Exception *e) {
		;
	}
	return 0;
}

static int
cancel_dead_f(va_list ap)
{
	note("cancel dead has started");
	fiber_set_cancellable(true);
	tnt_raise(OutOfMemory, 42, "allocator", "exception");
	return 0;
}

static size_t stack_expand_limit;

static void NOINLINE
stack_expand(void *ptr)
{
	char buf[2048];
	memset(buf, 0x45, 2048);
	ptrdiff_t stack_diff = (buf - (char *)ptr);
	stack_diff = stack_diff >= 0 ? stack_diff : -stack_diff;
	if (stack_diff < (ptrdiff_t)stack_expand_limit)
		stack_expand(ptr);
}

static int
test_stack_f(va_list ap)
{
	char s;
	stack_expand(&s);
	return 0;
}

static void
fiber_join_test()
{
	header();

	int rc;
	struct fiber *fiber = NULL;
	fiber = fiber_new_xc("join", noop_f);
	fail_if(!fiber);
	fiber_set_joinable(fiber, true);
	fiber_wakeup(fiber);
	rc = fiber_join(fiber);
	fail_if(rc);

	fiber = fiber_new_xc("cancel", cancel_f);
	fail_if(!fiber);
	fiber_set_joinable(fiber, true);
	fiber_wakeup(fiber);
	fiber_sleep(0);
	fiber_cancel(fiber);
	rc = fiber_join(fiber);
	fail_if(!rc);

	fiber = fiber_new_xc("exception", exception_f);
	fail_if(!fiber);
	fiber_set_joinable(fiber, true);
	fiber_wakeup(fiber);
	try {
		if (fiber_join(fiber) != 0)
			diag_raise();
		fail("exception not raised", "");
	} catch (Exception *e) {
		note("exception propagated");
	}

	note("log uncaught errors (gh-1238)");
	fiber = fiber_new_xc("exception", exception_f);
	fail_if(!fiber);
	fiber_wakeup(fiber);

	/*
	 * A fiber which is using exception should not
	 * push them up the stack.
	 */
	fiber = fiber_new_xc("no_exception", no_exception_f);
	fail_if(!fiber);
	fiber_set_joinable(fiber, true);
	fiber_wakeup(fiber);
	rc = fiber_join(fiber);
	fail_if(rc);
	/*
	 * Trying to cancel a dead joinable cancellable fiber lead to
	 * a crash, because cancel would try to schedule it.
	 */
	fiber = fiber_new_xc("cancel_dead", cancel_dead_f);
	fail_if(!fiber);
	fiber_set_joinable(fiber, true);
	fiber_wakeup(fiber);
	/** Let the fiber schedule */
	fiber_wakeup(fiber());
	fiber_yield();
	note("by this time the fiber should be dead already");
	fiber_cancel(fiber);
	rc = fiber_join(fiber);
	fail_if(!rc);

	footer();
}

void
fiber_stack_test()
{
	header();

	struct fiber *fiber;
	struct fiber_attr *fiber_attr;

	/*
	 * Test a fiber with the default stack size.
	 */
	stack_expand_limit = default_attr.stack_size * 3 / 4;
	fiber = fiber_new_xc("test_stack", test_stack_f);
	fail_if(!fiber);
	fiber_wakeup(fiber);
	fiber_sleep(0);
	note("normal-stack fiber not crashed");

	/*
	 * Test a fiber with a custom stack size.
	 */
	fiber_attr = fiber_attr_new();
	fiber_attr_setstacksize(fiber_attr, default_attr.stack_size * 2);
	stack_expand_limit = default_attr.stack_size * 3 / 2;
	fiber = fiber_new_ex("test_stack", fiber_attr, test_stack_f);
	fiber_attr_delete(fiber_attr);
	if (fiber == NULL)
		diag_raise();
	fiber_wakeup(fiber);
	fiber_sleep(0);
	note("big-stack fiber not crashed");

	footer();
}

void
fiber_name_test()
{
	header();
	const char new_name[] = "Horace";
	note("name of a fiber: %s", fiber_name(fiber()));
	fiber_set_name(fiber(), new_name);
	const char* name = fiber_name(fiber());
	note("set new fiber name: %s", name);
	is(strcmp(new_name, name), 0, "fiber has a new name");

	char long_name[FIBER_NAME_MAX + 30];
	memset(long_name, 'a', sizeof(long_name));
	long_name[sizeof(long_name) - 1] = 0;

	fiber_set_name(fiber(), long_name);

	name = fiber_name(fiber());
	note("fiber name is truncated: %s", name);
	size_t truncated_len = strlen(name) + 1;
	is(truncated_len, FIBER_NAME_MAX,
	   "fiber name length == FIBER_NAME_MAX (%zu == %d)", truncated_len, FIBER_NAME_MAX);

	footer();
}

static int
main_f(va_list ap)
{
	plan(2);
	fiber_name_test();
	fiber_join_test();
	fiber_stack_test();
	ev_break(loop(), EVBREAK_ALL);
	check_plan();
	return 0;
}

int main()
{
	memory_init();
	fiber_init(fiber_cxx_invoke);
	fiber_attr_create(&default_attr);
	struct fiber *main = NULL;
	main = fiber_new_xc("main", main_f);
	fail_if(!main);
	fiber_wakeup(main);
	ev_run(loop(), 0);
	fiber_free();
	memory_free();
	return 0;
}
