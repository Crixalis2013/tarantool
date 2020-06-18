#include "memory.h"
#include "fiber.h"
#include "unit.h"

enum {
	ITERATIONS = 50000,
	FIBERS = 100
};

static int
yield_f(va_list ap)
{
	for (int i = 0; i < ITERATIONS; i++) {
		fiber_wakeup(fiber());
		fiber_yield();
	}
	return 0;
}

static int
benchmark_f(va_list ap)
{
	struct fiber *fibers[FIBERS];
	for (int i = 0; i < FIBERS; i++) {
		fibers[i] = fiber_new_xc("yield-wielder", yield_f);
		fail_if(fibers[i]);
		fiber_wakeup(fibers[i]);
	}
	/** Wait for fibers to die. */
	for (int i = 0; i < FIBERS; i++) {
		while (fibers[i]->fid > 0)
			fiber_sleep(0.001);
	}
	ev_break(loop(), EVBREAK_ALL);
	return 0;
}

int main()
{
	plan(0);
	memory_init();
	fiber_init(fiber_cxx_invoke);
	struct fiber *benchmark = fiber_new_xc("benchmark", benchmark_f);
	fiber_wakeup(benchmark);
	ev_run(loop(), 0);
	fiber_free();
	memory_free();
	check_plan();
	return 0;
}
