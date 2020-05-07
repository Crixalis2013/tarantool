std = "luajit"
globals = {"box", "_TARANTOOL"}
ignore = {
    -- Unused argument <self>.
    "212/self",
    -- Redefining a local variable.
    "411",
    -- Shadowing an upvalue.
    "431",
}

include_files = {
    "**/*.lua",
    "extra/dist/tarantoolctl.in",
}

exclude_files = {
    "build/**/*.lua",
    "src/**/*.lua",
    "test-run/**/*.lua",
    "test/**/*.lua",
    "third_party/**/*.lua",
    ".rocks/**/*.lua",
    ".git/**/*.lua",
}

files["extra/dist/tarantoolctl.in"] = {
    ignore = {
        -- https://github.com/tarantool/tarantool/issues/4929
        "122",
    },
}
files["src/lua/help.lua"] = {
    -- Globals defined for interactive mode.
    globals = {"help", "tutorial"},
}
files["src/lua/init.lua"] = {
    -- Miscellaneous global function definition.
    globals = {"dostring"},
    ignore = {
        -- Set tarantool specific behaviour for os.exit.
        "122/os",
        -- Add custom functions into Lua package namespace.
        "142/package",
    },
}
