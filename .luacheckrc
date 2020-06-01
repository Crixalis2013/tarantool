std = "luajit"
globals = {"box", "_TARANTOOL", "tonumber64"}
ignore = {
    "143/debug",  -- Accessing an undefined field of a global variable <debug>.
    "143/string", -- Accessing an undefined field of a global variable <string>.
    "143/table",  -- Accessing an undefined field of a global variable <table>.
    "212/self",   -- Unused argument <self>.
    "411",        -- Redefining a local variable.
    "412",        -- Redefining an argument.
    "421",        -- Shadowing a local variable.
    "431",        -- Shadowing an upvalue.
    "432",        -- Shadowing an upvalue argument.
}


include_files = {
    "**/*.lua",
    "extra/dist/tarantoolctl.in",
}

exclude_files = {
    "build/**/*.lua",
    "src/box/lua/serpent.lua", -- third-party source code
    "test-run/**/*.lua",
    "test/**/*.lua",
    "third_party/**/*.lua",
    ".rocks/**/*.lua",
    ".git/**/*.lua",
}

files["extra/dist/tarantoolctl.in"] = {
    ignore = {
        "122", -- https://github.com/tarantool/tarantool/issues/4929
    },
}
files["src/lua/help.lua"] = {
    globals = {"help", "tutorial"}, -- globals defined for interactive mode.
}
files["src/lua/init.lua"] = {
    globals = {"dostring"}, -- miscellaneous global function definition.
    ignore = {
        "122/os",      -- set tarantool specific behaviour for os.exit.
        "142/package", -- add custom functions into Lua package namespace.
    },
}
files["src/lua/swim.lua"] = {
    ignore = {
        "212/m", -- respect swim module code style.
    },
}
files["src/box/lua/console.lua"] = {
    ignore = {
        "212", -- https://github.com/tarantool/tarantool/issues/5032
    }
}
