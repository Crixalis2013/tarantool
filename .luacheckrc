std = "luajit"
globals = {"box", "_TARANTOOL"}
ignore = {
    "212/self", -- Unused argument <self>.
    "411",      -- Redefining a local variable.
    "431",      -- Shadowing an upvalue.
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
        "122", -- https://github.com/tarantool/tarantool/issues/4929
    },
}
