#include <lua.h>              /* lua_*() */
#include <lauxlib.h>          /* luaL_*() */
#include <lualib.h>           /* luaL_openlibs() */
#include <string.h>           /* memcmp() */

#include "lua/msgpack.h"
#include "unit.h"

/*
 * test for https://github.com/tarantool/tarantool/issues/5017
 */


static int
lua_encode_unknown_mp(struct lua_State *L)
{
	const char test_str[] = "\xd4\x0f\x00";
	const char *data = (char *)test_str;
	luamp_decode(L, luaL_msgpack_default, &data);
	return 0;
}


static void
test_luamp_encode_extension_default(struct lua_State *L)
{
	plan(2);
	lua_pushcfunction(L, lua_encode_unknown_mp);
	ok(lua_pcall(L, 0, 0, 0) != 0,
	   "mplua_decode: unsupported extension raise error");
	const char err_msg[] = "msgpack.decode: unsupported extension: 15";
	ok(memcmp(lua_tostring(L, -1), err_msg, sizeof(err_msg)) == 0,
	   "mplua_decode: unsupported extension correct type");
}


int
main(void)
{
	header();

	struct lua_State *L = luaL_newstate();
	test_luamp_encode_extension_default(L);

	footer();
	check_plan();
}
