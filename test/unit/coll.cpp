#include <iostream>
#include <vector>
#include <algorithm>
#include <string.h>
#include <assert.h>
#include <msgpuck.h>
#include <diag.h>
#include <fiber.h>
#include <memory.h>
#include "coll/coll_def.h"
#include "coll/coll.h"
#include "unit.h"
#include "third_party/PMurHash.h"

using namespace std;

enum { HASH_SEED = 13 };

struct comp {
	struct coll *coll;
	comp(struct coll *coll_) : coll(coll_) {}
	bool operator()(const char *a, const char *b) const
	{
		int cmp = coll->cmp(a, strlen(a), b, strlen(b), coll);
		return cmp < 0;
	}
};

void
test_sort_strings(vector<const char *> &strings, struct coll *coll)
{
	sort(strings.begin(), strings.end(), comp(coll));
	note("%s", strings[0]);
	for (size_t i = 1; i < strings.size(); i++) {
		int cmp = coll->cmp(strings[i], strlen(strings[i]),
				    strings[i - 1], strlen(strings[i - 1]),
				    coll);
		note("# %s %s", strings[i],
		    (cmp < 0 ? " LESS" : cmp > 0 ? " GREATER " : " EQUAL"));
	}
};

void
manual_test()
{
	header();
	plan(7);

	vector<const char *> strings;
	struct coll_def def;
	memset(&def, 0, sizeof(def));
	snprintf(def.locale, sizeof(def.locale), "%s", "ru_RU");
	def.type = COLL_TYPE_ICU;
	def.icu.strength = COLL_ICU_STRENGTH_IDENTICAL;
	struct coll *coll;

	note("-- default ru_RU --");
	coll = coll_new(&def);
	fail_if(coll != NULL);
	strings = {"Б", "бб", "е", "ЕЕЕЕ", "ё", "Ё", "и", "И", "123", "45" };
	test_sort_strings(strings, coll);
	coll_unref(coll);

	note("-- --||-- + upper first --");
	def.icu.case_first = COLL_ICU_CF_UPPER_FIRST;
	coll = coll_new(&def);
	fail_if(coll != NULL);
	strings = {"Б", "бб", "е", "ЕЕЕЕ", "ё", "Ё", "и", "И", "123", "45" };
	test_sort_strings(strings, coll);
	coll_unref(coll);

	note("-- --||-- + lower first --");
	def.icu.case_first = COLL_ICU_CF_LOWER_FIRST;
	coll = coll_new(&def);
	fail_if(coll != NULL);
	strings = {"Б", "бб", "е", "ЕЕЕЕ", "ё", "Ё", "и", "И", "123", "45" };
	test_sort_strings(strings, coll);
	coll_unref(coll);

	note("-- --||-- + secondary strength + numeric --");
	def.icu.strength = COLL_ICU_STRENGTH_SECONDARY;
	def.icu.numeric_collation = COLL_ICU_ON;
	coll = coll_new(&def);
	fail_if(coll != NULL);
	strings = {"Б", "бб", "е", "ЕЕЕЕ", "ё", "Ё", "и", "И", "123", "45" };
	test_sort_strings(strings, coll);
	coll_unref(coll);

	note("-- --||-- + case level --");
	def.icu.case_level = COLL_ICU_ON;
	coll = coll_new(&def);
	fail_if(coll != NULL);
	strings = {"Б", "бб", "е", "ЕЕЕЕ", "ё", "Ё", "и", "И", "123", "45" };
	test_sort_strings(strings, coll);
	coll_unref(coll);

	note("-- en_EN --");
	snprintf(def.locale, sizeof(def.locale), "%s", "en_EN-EN");
	coll = coll_new(&def);
	fail_if(coll != NULL);
	strings = {"aa", "bb", "cc", "ch", "dd", "gg", "hh", "ii" };
	test_sort_strings(strings, coll);
	coll_unref(coll);

	note("-- cs_CZ --");
	snprintf(def.locale, sizeof(def.locale), "%s", "cs_CZ");
	coll = coll_new(&def);
	fail_if(coll != NULL);
	strings = {"aa", "bb", "cc", "ch", "dd", "gg", "hh", "ii" };
	test_sort_strings(strings, coll);
	coll_unref(coll);

	check_plan();
	footer();
}

unsigned calc_hash(const char *str, struct coll *coll)
{
	size_t str_len = strlen(str);
	uint32_t h = HASH_SEED;
	uint32_t carry = 0;
	uint32_t actual_len = coll->hash(str, str_len, &h, &carry, coll);
	return PMurHash32_Result(h, carry, actual_len);

}

void
hash_test()
{
	header();
	plan(8);

	struct coll_def def;
	memset(&def, 0, sizeof(def));
	snprintf(def.locale, sizeof(def.locale), "%s", "ru_RU");
	def.type = COLL_TYPE_ICU;
	def.icu.strength = COLL_ICU_STRENGTH_IDENTICAL;
	struct coll *coll;

	/* Case sensitive */
	coll = coll_new(&def);
	fail_if(coll != NULL);
	note("Case sensitive");
	isnt(calc_hash("ае", coll), calc_hash("аё", coll), "ае != аё");
	isnt(calc_hash("ае", coll), calc_hash("аЕ", coll), "ае != аЕ");
	isnt(calc_hash("аЕ", coll), calc_hash("аё", coll), "аЕ != аё");
	coll_unref(coll);

	/* Case insensitive */
	def.icu.strength = COLL_ICU_STRENGTH_SECONDARY;
	coll = coll_new(&def);
	fail_if(coll != NULL);
	note("Case insensitive");
	isnt(calc_hash("ае", coll), calc_hash("аё", coll), "ае != аё");
	is(calc_hash("ае", coll), calc_hash("аЕ", coll), "ае == аЕ");
	isnt(calc_hash("аЕ", coll), calc_hash("аё", coll), "аЕ != аё");
	coll_unref(coll);

	check_plan();
	footer();
}

void
cache_test()
{
	header();
	plan(2);

	struct coll_def def;
	memset(&def, 0, sizeof(def));
	snprintf(def.locale, sizeof(def.locale), "%s", "ru_RU");
	def.type = COLL_TYPE_ICU;

	struct coll *coll1 = coll_new(&def);
	struct coll *coll2 = coll_new(&def);
	is(coll1, coll2,
	   "collations with the same definition are not duplicated");
	coll_unref(coll2);
	snprintf(def.locale, sizeof(def.locale), "%s", "en_EN");
	coll2 = coll_new(&def);
	isnt(coll1, coll2,
	     "collations with different definitions are different objects");
	coll_unref(coll2);
	coll_unref(coll1);

	check_plan();
	footer();
}

int
main(int, const char**)
{
	plan(0);
	coll_init();
	memory_init();
	fiber_init(fiber_c_invoke);
	manual_test();
	hash_test();
	cache_test();
	fiber_free();
	memory_free();
	coll_free();
	check_plan();
}
