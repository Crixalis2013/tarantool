#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(77)

--
-- Make sure there are no implicit casts during assignment,
-- except for the implicit cast between numeric values.
--
test:execsql([[
    CREATE TABLE ti (a INT PRIMARY KEY AUTOINCREMENT, i INTEGER);
    CREATE TABLE td (a INT PRIMARY KEY AUTOINCREMENT, d DOUBLE);
    CREATE TABLE tb (a INT PRIMARY KEY AUTOINCREMENT, b BOOLEAN);
    CREATE TABLE tt (a INT PRIMARY KEY AUTOINCREMENT, t TEXT);
    CREATE TABLE tv (a INT PRIMARY KEY AUTOINCREMENT, v VARBINARY);
    CREATE TABLE ts (a INT PRIMARY KEY AUTOINCREMENT, s SCALAR);
]])

test:do_catchsql_test(
    "gh-3809-1",
    [[
        INSERT INTO ti(i) VALUES (11)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-2",
    [[
        INSERT INTO ti(i) VALUES (100000000000000000000000000000000.1)
    ]], {
        1, "Type mismatch: can not convert '1.0e+32' (type: real) to integer"
    })

test:do_catchsql_test(
    "gh-3809-3",
    [[
        INSERT INTO ti(i) VALUES (33.0)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-4",
    [[
        INSERT INTO ti(i) VALUES (true)
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to integer"
    })

test:do_catchsql_test(
    "gh-3809-5",
    [[
        INSERT INTO ti(i) VALUES ('33')
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to integer"
    })

test:do_catchsql_test(
    "gh-3809-6",
    [[
        INSERT INTO ti(i) VALUES (X'3434')
    ]], {
        1, "Type mismatch: can not convert varbinary to integer"
    })

test:do_execsql_test(
    "gh-3809-7",
    [[
        SELECT * FROM ti;
    ]], {
        1, 11, 2, 33
    })

test:do_catchsql_test(
    "gh-3809-8",
    [[
        INSERT INTO td(d) VALUES (11)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-9",
    [[
        INSERT INTO td(d) VALUES (100000000000000001);
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-10",
    [[
        INSERT INTO td(d) VALUES (22.2)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-11",
    [[
        INSERT INTO td(d) VALUES (true)
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to double"
    })

test:do_catchsql_test(
    "gh-3809-12",
    [[
        INSERT INTO td(d) VALUES ('33')
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to double"
    })

test:do_catchsql_test(
    "gh-3809-13",
    [[
        INSERT INTO td(d) VALUES (X'3434')
    ]], {
        1, "Type mismatch: can not convert varbinary to double"
    })

test:do_execsql_test(
    "gh-3809-14",
    [[
        SELECT * FROM td;
    ]], {
        1, 11, 2, 100000000000000000, 3, 22.2
    })

test:do_catchsql_test(
    "gh-3809-15",
    [[
        INSERT INTO tb(b) VALUES (11)
    ]], {
        1, "Type mismatch: can not convert '11' (type: unsigned) to boolean"
    })

test:do_catchsql_test(
    "gh-3809-16",
    [[
        INSERT INTO tb(b) VALUES (22.2)
    ]], {
        1, "Type mismatch: can not convert '22.2' (type: real) to boolean"
    })

test:do_catchsql_test(
    "gh-3809-17",
    [[
        INSERT INTO tb(b) VALUES (true)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-18",
    [[
        INSERT INTO tb(b) VALUES ('33')
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to boolean"
    })

test:do_catchsql_test(
    "gh-3809-19",
    [[
        INSERT INTO tb(b) VALUES (X'3434')
    ]], {
        1, "Type mismatch: can not convert varbinary to boolean"
    })

test:do_execsql_test(
    "gh-3809-20",
    [[
        SELECT * FROM tb;
    ]], {
        1, true
    })

test:do_catchsql_test(
    "gh-3809-21",
    [[
        INSERT INTO tt(t) VALUES (11)
    ]], {
        1, "Type mismatch: can not convert '11' (type: unsigned) to string"
    })

test:do_catchsql_test(
    "gh-3809-22",
    [[
        INSERT INTO tt(t) VALUES (22.2)
    ]], {
        1, "Type mismatch: can not convert '22.2' (type: real) to string"
    })

test:do_catchsql_test(
    "gh-3809-23",
    [[
        INSERT INTO tt(t) VALUES (true)
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to string"
    })

test:do_catchsql_test(
    "gh-3809-24",
    [[
        INSERT INTO tt(t) VALUES ('33')
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-25",
    [[
        INSERT INTO tt(t) VALUES (X'3434')
    ]], {
        1, "Type mismatch: can not convert varbinary to string"
    })

test:do_execsql_test(
    "gh-3809-26",
    [[
        SELECT * FROM tt;
    ]], {
        1, "33"
    })

test:do_catchsql_test(
    "gh-3809-27",
    [[
        INSERT INTO tv(v) VALUES (11)
    ]], {
        1, "Type mismatch: can not convert '11' (type: unsigned) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-28",
    [[
        INSERT INTO tv(v) VALUES (22.2)
    ]], {
        1, "Type mismatch: can not convert '22.2' (type: real) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-29",
    [[
        INSERT INTO tv(v) VALUES (true)
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-30",
    [[
        INSERT INTO tv(v) VALUES ('33')
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-31",
    [[
        INSERT INTO tv(v) VALUES (X'3434')
    ]], {
        0
    })

test:do_execsql_test(
    "gh-3809-32",
    [[
        SELECT * FROM tv;
    ]], {
        1, "44"
    })

test:do_catchsql_test(
    "gh-3809-33",
    [[
        INSERT INTO ts(s) VALUES (11)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-34",
    [[
        INSERT INTO ts(s) VALUES (22.2)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-35",
    [[
        INSERT INTO ts(s) VALUES (true)
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-36",
    [[
        INSERT INTO ts(s) VALUES ('33')
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-37",
    [[
        INSERT INTO ts(s) VALUES (X'3434')
    ]], {
        0
    })

test:do_execsql_test(
    "gh-3809-38",
    [[
        SELECT * FROM ts;
    ]], {
        1, 11, 2, 22.2, 3, true, 4, "33", 5, "44"
    })

--
-- This test suite verifies that ASSIGNMENT is working correctly
-- during an UPDATE.
--
test:execsql([[
    DELETE FROM ti;
    DELETE FROM td;
    DELETE FROM tb;
    DELETE FROM tt;
    DELETE FROM tv;
    DELETE FROM ts;
    INSERT INTO ti(a) VALUES(1);
    INSERT INTO td(a) VALUES(1);
    INSERT INTO tb(a) VALUES(1);
    INSERT INTO tt(a) VALUES(1);
    INSERT INTO tv(a) VALUES(1);
    INSERT INTO ts(a) VALUES(1);
]])

test:do_execsql_test(
    "gh-3809-39",
    [[
        SELECT * FROM ti, td, tb, tt, tv, ts;
    ]], {
        1, "", 1, "", 1, "", 1, "", 1, "", 1, ""
    })

test:do_catchsql_test(
    "gh-3809-40",
    [[
        UPDATE ti SET i = 11 WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-41",
    [[
        UPDATE ti SET i = 100000000000000000000000000000000.1 WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '1.0e+32' (type: real) to integer"
    })

test:do_catchsql_test(
    "gh-3809-42",
    [[
        UPDATE ti SET i = 33.0 WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-43",
    [[
        UPDATE ti SET i = true WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to integer"
    })

test:do_catchsql_test(
    "gh-3809-44",
    [[
        UPDATE ti SET i = '33' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to integer"
    })

test:do_catchsql_test(
    "gh-3809-45",
    [[
        UPDATE ti SET i = X'3434' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert varbinary to integer"
    })

test:do_execsql_test(
    "gh-3809-46",
    [[
        SELECT * FROM ti;
    ]], {
        1, 33
    })

test:do_catchsql_test(
    "gh-3809-47",
    [[
        UPDATE td SET d = 11 WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-48",
    [[
        UPDATE td SET d = 100000000000000001 WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-49",
    [[
        UPDATE td SET d = 22.2 WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-50",
    [[
        UPDATE td SET d = true WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to double"
    })

test:do_catchsql_test(
    "gh-3809-51",
    [[
        UPDATE td SET d = '33' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to double"
    })

test:do_catchsql_test(
    "gh-3809-52",
    [[
        UPDATE td SET d = X'3434' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert varbinary to double"
    })

test:do_execsql_test(
    "gh-3809-53",
    [[
        SELECT * FROM td;
    ]], {
        1, 22.2
    })

test:do_catchsql_test(
    "gh-3809-54",
    [[
        UPDATE tb SET b = 11 WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '11' (type: unsigned) to boolean"
    })

test:do_catchsql_test(
    "gh-3809-55",
    [[
        UPDATE tb SET b = 22.2 WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '22.2' (type: real) to boolean"
    })

test:do_catchsql_test(
    "gh-3809-56",
    [[
        UPDATE tb SET b = true WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-57",
    [[
        UPDATE tb SET b = '33' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to boolean"
    })

test:do_catchsql_test(
    "gh-3809-58",
    [[
        UPDATE tb SET b = X'3434' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert varbinary to boolean"
    })

test:do_execsql_test(
    "gh-3809-59",
    [[
        SELECT * FROM tb;
    ]], {
        1, true
    })

test:do_catchsql_test(
    "gh-3809-60",
    [[
        UPDATE tt SET t = 11 WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '11' (type: unsigned) to string"
    })

test:do_catchsql_test(
    "gh-3809-61",
    [[
        UPDATE tt SET t = 22.2 WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '22.2' (type: real) to string"
    })

test:do_catchsql_test(
    "gh-3809-62",
    [[
        UPDATE tt SET t = true WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to string"
    })

test:do_catchsql_test(
    "gh-3809-63",
    [[
        UPDATE tt SET t = '33' WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-64",
    [[
        UPDATE tt SET t = X'3434' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert varbinary to string"
    })

test:do_execsql_test(
    "gh-3809-65",
    [[
        SELECT * FROM tt;
    ]], {
        1, "33"
    })

test:do_catchsql_test(
    "gh-3809-66",
    [[
        UPDATE tv SET v = 11 WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '11' (type: unsigned) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-67",
    [[
        UPDATE tv SET v = 22.2 WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '22.2' (type: real) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-68",
    [[
        UPDATE tv SET v = true WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert 'TRUE' (type: boolean) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-69",
    [[
        UPDATE tv SET v = '33' WHERE a = 1;
    ]], {
        1, "Type mismatch: can not convert '33' (type: text) to varbinary"
    })

test:do_catchsql_test(
    "gh-3809-70",
    [[
        UPDATE tv SET v = X'3434' WHERE a = 1;
    ]], {
        0
    })

test:do_execsql_test(
    "gh-3809-71",
    [[
        SELECT * FROM tv;
    ]], {
        1, "44"
    })

test:do_catchsql_test(
    "gh-3809-72",
    [[
        UPDATE ts SET s = 11 WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-73",
    [[
        UPDATE ts SET s = 22.2 WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-74",
    [[
        UPDATE ts SET s = true WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-75",
    [[
        UPDATE ts SET s = '33' WHERE a = 1;
    ]], {
        0
    })

test:do_catchsql_test(
    "gh-3809-76",
    [[
        UPDATE ts SET s = X'3434' WHERE a = 1;
    ]], {
        0
    })

test:do_execsql_test(
    "gh-3809-77",
    [[
        SELECT * FROM ts;
    ]], {
        1, "44"
    })

test:finish_test()
