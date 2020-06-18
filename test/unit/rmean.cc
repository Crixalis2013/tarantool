#include "rmean.h"
#include "memory.h"
#include "unit.h"
#include "fiber.h"

int print_stat(const char *name, int rps, int64_t total, void* ctx)
{
	note("%s: rps %d, total %d%c", name, rps, (int)total,
	     name[2] == '2' ? '\n' : '\t');
	return 0;
}

void test_100rps(rmean *st)
{
	header();
	note("Send 100 requests every second for 10 seconds");
	note("Calc rps at third and last second");
	for(int i = 0; i < 10; i++) { /* 10 seconds */
		rmean_collect(st, 0, 100); /* send 100 requests */
		rmean_roll(st->stats[0].value, 1);
		rmean_roll(st->stats[1].value, 1);
		if (i == 2 || i == 9) { /* two checks */
			print_stat(st->stats[0].name,
				   rmean_mean(st, 0),
				   rmean_total(st, 0), NULL);
			print_stat(st->stats[1].name,
				   rmean_mean(st, 1),
				   rmean_total(st, 1), NULL);
		}
	}
	/* 10 seconds, 1000 in EV1, 100 rps */
	footer();
}

void test_mean15rps(rmean *st)
{
	header();
	note("Send 15 rps on the average, and 3 rps to EV2");
	for(int i = 0; i < 10; i++) { /* 10 seconds */
		for(int j = 0; j < 15; j++) {
			rmean_collect(st, 0, 1); /* send 15 requests */
			if((i * 3 + 2 + j) % 15 == 0) {
				rmean_roll(st->stats[0].value, 1);
				rmean_roll(st->stats[1].value, 1);
			}
		}
		rmean_collect(st, 1, 3);
	}
	print_stat(st->stats[0].name,
		   rmean_mean(st, 0),
		   rmean_total(st, 0), NULL);
	print_stat(st->stats[1].name,
		   rmean_mean(st, 1),
		   rmean_total(st, 1), NULL);
	/* 10 seconds, 1000 + 150 in EV1, 15 rps. 30 in EV2, 3 rps*/
	footer();
}

int main()
{
	plan(0);
	note("Stat. 2 names, timer simulation");

	memory_init();
	fiber_init(fiber_cxx_invoke);

	struct rmean *st;
	const char *name[] = {"EV1", "EV2"};
	st = rmean_new(name, 2);

	test_100rps(st);
	test_mean15rps(st);

	rmean_delete(st);

	fiber_free();
	memory_free();
	check_plan();

	return 0;
}
