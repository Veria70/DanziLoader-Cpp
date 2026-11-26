# SPDX-License-Identifier: GPL-3.0-or-later
"""Differential test of src/core/toml.cpp against the `toml` crate the
reference uses (build/toml-oracle, same version and features).

Builds a synthetic corpus (hand-written cases for every feature and error
class, the reference's victory.example.toml, random documents from a
generator and byte mutations of them), runs both sides over it and compares
their outputs byte for byte: the parsed table (canonical dump plus the
crate's `Value` display) or the error's `Display`.

usage (from the repository root, with `python -I`):
  python -I tools/check_toml.py run [--seed N] [--generated N] [--mutated N] [--focused N]
  python -I tools/check_toml.py run-config [--seed N] [--generated N] [--mutated N]
      (config::parse_file vs the reference's FileConfig; generated/5 documents
      and mutated/15 mutations)
  python -I tools/check_toml.py corpus OUT.bin [...]
  python -I tools/check_toml.py compare CORPUS.bin ORACLE.bin PORT.bin

`run` expects build/toml-target/release/toml-oracle.exe and
build/cpp-release/toml-check.exe (or --oracle / --port).
"""

import argparse
import os
import re
import random
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REFERENCE_EXAMPLE = os.path.join(os.environ.get("DANZI_REFERENCE", os.path.join(ROOT, "..", "DanziLoader-reference")), "victory.example.toml")


HAND = [

    b"", b"\n", b"\r\n", b"   ", b"\t\n \n", b"# only a comment", b"# c\n# d\n",
    b"\xef\xbb\xbf", b"\xef\xbb\xbfa = 1", b"\xef\xbb\xbf\n= 1", b"a = 1\xef\xbb\xbf",
    b"a = 1", b"a = 1\n", b"a = 1\r\n", b"a=1", b"a =1 # c", b"a = 1 #c\r\nb = 2",

    b"bare_key-1 = 1", b"1234 = 1", b"-_- = 1", b'"quoted key" = 1', b"'literal key' = 1",
    b'"" = 1', b"'' = 1", b'"a.b" = 1', b"a.b.c = 1", b"a . b . c = 1", b"a .b= 1",
    b'"a"."b" = 1', b"'a'.\"b\".c = 1", b'"\\u00e9" = 1', b'"\\x41" = 1', b'"tab\\t" = 1',
    b"\xc3\xa9 = 1", b"a$b = 1", b"a$$b = 1", b"a$b$ = 1", b"$ = 1", b"a b = 1", b"= 1",
    b"a. = 1", b".a = 1", b"a..b = 1", b"a.\n= 1", b"'''ml''' = 1", b'"""ml""" = 1',
    b"a\n", b"a", b"a.b", b"a = ", b"a =", b"a = # c", b"a = \n", b"a == 1", b"a = = 1",
    b"a = 1 2", b"a = 1 b = 2", b"a = 1, b = 2", b"a = 1\na = 2", b"a = 1\nA = 2",
    b'a = 1\n"a" = 2', b"a = 1\n'a' = 2", b"a.b = 1\na = 2", b"a = 1\na.b = 2",
    b"a.b = 1\na.b.c = 2", b"a.b = 1\na.c = 2", b'"\\ud800" = 1', b"\"a\nb\" = 1",
    b"[a]\nb.c = 1\n[a.b]", b"[a.b]\n[a]\nb.c = 1",

    b"a = 0", b"a = +0", b"a = -0", b"a = 42", b"a = +42", b"a = -42", b"a = 1_000",
    b"a = 1__000", b"a = _1", b"a = 1_", b"a = 01", b"a = 00", b"a = 0_0", b"a = 1_a",
    b"a = 9223372036854775807", b"a = 9223372036854775808", b"a = -9223372036854775808",
    b"a = -9223372036854775809", b"a = 18446744073709551615", b"a = 18446744073709551616",
    b"a = 170141183460469231731687303715884105727", b"a = 170141183460469231731687303715884105728",
    b"a = -170141183460469231731687303715884105728", b"a = -170141183460469231731687303715884105729",
    b"a = 340282366920938463463374607431768211455", b"a = 340282366920938463463374607431768211456",
    b"a = 99999999999999999999999999999999999999999999",
    b"a = 0x0", b"a = 0xdead_BEEF", b"a = 0xDEADbeef", b"a = 0x", b"a = 0x_1", b"a = 0x1_",
    b"a = 0xg", b"a = 0X1", b"a = -0x1", b"a = +0x1", b"a = 0x+1", b"a = 0x-1",
    b"a = 0x7FFFFFFFFFFFFFFF", b"a = 0x8000000000000000", b"a = 0xFFFFFFFFFFFFFFFF",
    b"a = 0x10000000000000000", b"a = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    b"a = 0x100000000000000000000000000000000", b"a = 0o0", b"a = 0o777", b"a = 0o8",
    b"a = 0O7", b"a = 0b0", b"a = 0b1010_1010", b"a = 0b2", b"a = 0B1", b"a = 0d10",
    b"a = 0D10", b"a = -0d1", b"a = 0x 1", b"a = ++1", b"a = +-1", b"a = -", b"a = +",
    b"a = 1.", b"a = 1e", b"a = 1e+", b"a = 0o", b"a = 0b", b"a = 0b_", b"a = 0o1__2",
    b"a = 1 _000", b"a = 12abc", b"a = 1-", b"a = 1:", b"a = 123_456_789",

    b"a = 1.0", b"a = +1.0", b"a = -1.0", b"a = 3.1415", b"a = -0.01", b"a = 5e+22",
    b"a = 1e06", b"a = -2E-2", b"a = 6.626e-34", b"a = 224_617.445_991_228", b"a = 1e1_0",
    b"a = 0.0", b"a = -0.0", b"a = +0.0", b"a = 0e0", b"a = 1.5e", b"a = .5", b"a = 5.",
    b"a = 1.e5", b"a = 1._5", b"a = 1_.5", b"a = 01.5", b"a = 00.5", b"a = 1.05",
    b"a = 0.000001", b"a = 1e-7", b"a = 123456789012345678901234567890.0", b"a = 1e300",
    b"a = 1e308", b"a = 1.7976931348623157e308", b"a = 1.7976931348623159e308", b"a = 1e309",
    b"a = -1e400", b"a = 1e-400", b"a = -1e-400", b"a = 4.9e-324", b"a = 2.4e-324",
    b"a = 2.5e-324", b"a = 2.2250738585072014e-308", b"a = 0.1", b"a = 0.30000000000000004",
    b"a = 100.0", b"a = 1e16", b"a = 1.5e-5", b"a = 9007199254740993.0", b"a = -.5",
    b"a = +.5", b"a = 1e5.5", b"a = 1ee5", b"a = 1e+-5", b"a = 1.2.3", b"a = 1..2",
    b"a = 3.14e", b"a = 1e0_", b"a = 1E_5", b"a = 1e5_",
    b"a = inf", b"a = +inf", b"a = -inf", b"a = nan", b"a = +nan", b"a = -nan", b"a = Inf",
    b"a = NaN", b"a = -Inf", b"a = -NaN", b"a = infinity", b"a = nan1", b"a = -infx",
    b"a = i", b"a = n", b"a = inf inf",

    b"a = true", b"a = false", b"a = True", b"a = FALSE", b"a = tru", b"a = falsey",
    b"a = true false", b"a = t", b"a = f",

    b"a = hello", b"a = hello world", b"a = 'abc", b"a = abc'", b"a = abc\"", b"a = abc'''",
    b'a = abc"""', b"a = @", b"a = -hello", b"a = 1.5 x",

    b'a = "hello"', b'a = ""', b'a = "\\""', b'a = "\\\\"', b'a = "\\b\\t\\n\\f\\r\\e"',
    b'a = "\\x41\\xff\\x00"', b'a = "\\u00e9\\U0001F600"', b'a = "\\u0000"', b'a = "\\uD800"',
    b'a = "\\U00110000"', b'a = "\\U0010FFFF"', b'a = "\\u12"', b'a = "\\x4"', b'a = "\\U1234567"',
    b'a = "\\uGGGG"', b'a = "\\q"', b'a = "\\ "', b'a = "\\', b'a = "abc\\"', b'a = "abc',
    b'a = "abc\nb = 1', b'a = "a\tb"', b'a = "a\x01b"', b'a = "a\x7fb"', b'a = "a\x00b"',
    b'a = "a\rb"', b'a = "\xc3\xa9\xe2\x82\xac"', b'a = "a\x1b\x1c"', b'a = "\\\xc3\xa9"',
    b'a = "x" "y"', b'a = "x"y', b'a = "a\\u00"', b'a = "\\U0000004"',

    b"a = 'hello'", b"a = ''", b"a = 'C:\\Users\\x'", b"a = 'a\tb'", b"a = 'a\x01b'",
    b"a = 'a\x7fb'", b"a = 'abc", b"a = 'abc\nb = 1", b"a = 'a\rb'", b"a = 'it''s'",
    b"a = '\xc3\xa9'",

    b'a = """\nhello"""', b'a = """hello"""', b'a = """\r\nhello"""', b'a = """\n\nhello"""',
    b'a = """a\nb\r\nc"""', b'a = """a\\\n   b"""', b'a = """a\\   \n\n   b"""',
    b'a = """a\\\r\n  b"""', b'a = """a\\  b"""', b'a = """a\\\rb"""', b'a = """a\rb"""',
    b'a = """a""""', b'a = """a"""""', b'a = """a""""""', b'a = """""""', b'a = """"""',
    b'a = """ "" """', b'a = """ \\""" """', b'a = """abc', b'a = """abc\n', b'a = """a\x01b"""',
    b'a = """a\tb"""', b'a = """\\e\\x41"""', b'a = """a\\\n\r\n b"""', b'a = """a\\\n \r b"""',
    b'a = """\\', b'a = """\\"""', b'a = """a\\', b'a = """ \\\n"""',

    b"a = '''\nhello'''", b"a = '''hello'''", b"a = '''a\nb\r\nc'''", b"a = '''a'''''",
    b"a = ''''a''''", b"a = ''''''", b"a = '''a''''''", b"a = '''abc", b"a = '''a\rb'''",
    b"a = '''a\x01b'''", b"a = '''a\tb'''", b"a = '''\r\nx'''", b"a = ''''''''",

    b"a = 1979-05-27T07:32:00Z", b"a = 1979-05-27T00:32:00-07:00",
    b"a = 1979-05-27T00:32:00.999999-07:00", b"a = 1979-05-27 07:32:00Z",
    b"a = 1979-05-27t07:32:00z", b"a = 1979-05-27T07:32:00", b"a = 1979-05-27T00:32:00.999999",
    b"a = 1979-05-27", b"a = 07:32:00", b"a = 00:32:00.999999", b"a = 07:32",
    b"a = 1979-05-27T07:32", b"a = 1979-05-27T07:32Z", b"a = 1979-05-27 07:32",
    b"a = 1979-05-27T07:32:00.5", b"a = 1979-05-27T07:32:00.000", b"a = 1979-05-27T07:32:00.1234567891",
    b"a = 1979-05-27T07:32:00+00:00", b"a = 1979-05-27T07:32:00-00:00", b"a = 1979-05-27T07:32:00+23:59",
    b"a = 1979-05-27T07:32:00+24:00", b"a = 1979-05-27T07:32:00+05:60", b"a = 1979-05-27T07:32:00+5:00",
    b"a = 1979-05-27T07:32:00+05", b"a = 1979-05-27T07:32:00+05:", b"a = 1979-05-27T07:32:00+05:0",
    b"a = 1979-05-27T07:32:00X", b"a = 1979-05-27T07:32:00Zx", b"a = 1979-13-27", b"a = 1979-00-27",
    b"a = 1979-02-29", b"a = 2000-02-29", b"a = 1900-02-29", b"a = 2024-02-29", b"a = 2023-02-30",
    b"a = 2023-04-31", b"a = 2023-04-30", b"a = 2023-01-32", b"a = 2023-01-00", b"a = 79-05-27",
    b"a = 1979-5-27", b"a = 1979-05-7", b"a = 1979-05", b"a = 1979-", b"a = 1979-05-",
    b"a = 1979--27", b"a = 1979-05-27T", b"a = 1979-05-27T07", b"a = 1979-05-27T07:",
    b"a = 1979-05-27T7:32:00", b"a = 1979-05-27T07:2:00", b"a = 1979-05-27T07:32:0",
    b"a = 1979-05-27T24:00:00", b"a = 1979-05-27T23:60:00", b"a = 1979-05-27T23:59:60",
    b"a = 1979-05-27T23:59:61", b"a = 1979-05-27T07:32:00.", b"a = 1979-05-27T07:32:00.Z",
    b"a = 1979-05-27X07:32:00", b"a = 1979-05-27  07:32:00", b"a = 1979-05-27\t07:32:00",
    b"a = 1979-05-27 07:32:00 extra", b"a = 07:32:00Z", b"a = 07:32:00+01:00", b"a = 7:32:00",
    b"a = 07:32:", b"a = 07:", b"a = 07:32:00.", b"a = 07-32", b"a = 1979-05-27Z",
    b"a = +1979-05-27", b"a = -07:32:00", b"a = 1979-05-27T07:32:00.999999999999",
    b"a = 0000-01-01", b"a = 9999-12-31T23:59:59.999999999+23:59", b"a = 1979-05-27T07:32:00.000000001Z",
    b"a = 12:", b"a = 1:2", b"a = 1979-05-27 07:32:00.5 # c", b"a = [1979-05-27 07:32:00, 1979-05-27]",
    b"a = {d = 1979-05-27 07:32:00}", b"a = 1979-05-27T07:32:00.5-07:00 ", b"a = 1979-05-27 x",

    b"a = []", b"a = [ ]", b"a = [1]", b"a = [1,]", b"a = [1, 2, 3]", b"a = [1,2,3,]",
    b"a = [,]", b"a = [1,,2]", b"a = [,1]", b"a = [1 2]", b"a = [1\n,2]", b"a = [\n1,\n2,\n]",
    b"a = [ # c\n 1, # d\n 2 # e\n]", b"a = [1, 'a', 2.0, true, [], {}]", b"a = [[1], [2, [3]]]",
    b"a = [", b"a = [1", b"a = [1,", b"a = [1, 2\nb = 1", b"a = ]", b"a = [1]]", b"a = [1 = 2]",
    b"a = [{]", b"a = [}]", b"a = [1 {}]", b"a = [1 []]", b"a = [1 }]", b"a = [{a = 1}, {a = 2}]",
    b"a = [\r\n1,\r\n]", b"a = [1,\r2]", b"a = [ # \x01\n]", b"a = [1]\na = [2]",
    b"a = [[[[[[[[[[1]]]]]]]]]]",

    b"a = {}", b"a = { }", b"a = {b = 1}", b"a = {b = 1,}", b"a = {b = 1, c = 2}",
    b"a = {\n b = 1,\n c = 2,\n}", b"a = { # c\n b = 1 # d\n}", b"a = {b.c = 1, b.d = 2}",
    b"a = {b.c = 1, b = 2}", b"a = {b = 1, b.c = 2}", b"a = {b = 1, b = 2}", b"a = {b = {}, b.c = 1}",
    b"a = {b.c = 1, b.c.d = 2}", b"a = {b = [], b.c = 1}", b"a = {,}", b"a = {b = 1,,}",
    b"a = {b}", b"a = {b =}", b"a = {b = }", b"a = {= 1}", b"a = {.b = 1}", b"a = {b. = 1}",
    b"a = {b = 1 c = 2}", b"a = {b = 1 = 2}", b"a = {b c}", b"a = {b {}}", b"a = {b []}",
    b"a = {{}}", b"a = {[]}", b"a = {b = 1 {}}", b"a = {b = 1 []}", b"a = {]}", b"a = {b ]}",
    b"a = {b = ]}", b"a = {b = 1 ]}", b"a = {", b"a = {b = 1", b"a = {b", b"a = {b =",
    b"a = {b = 1,", b"a = }", b"a = {b = {c = {d = 1}}}", b"a = {'b' = 1, \"c\" = 2}",
    b"a = {b = 1}\na.c = 2", b"a = {b = 1}\n[a]", b"a = {b = 1}\n[a.c]", b"a = {}\n[[a]]",
    b"a = {b = 1}\na.b = 2", b"a = {b.c = 1}\n[a.b]", b"a = {b = 1}}", b"a = {b = 1}, c = 2",
    b"a = {b = 1,\r\n}", b"a = {b = 1\r}",

    b"[a]", b"[a]\nb = 1", b"[ a ]", b"[ a . b ]", b"[a.b.c]", b'["a b"]', b"['a.b']",
    b"[a]\n[b]", b"[a]\n[a]", b"[a.b]\n[a]", b"[a]\n[a.b]", b"[a.b]\n[a.b]", b"[a]\nb = 1\n[a.b]",
    b"[a]\nb.c = 1\n[a.b.c]", b"[a]\nb.c = 1\n[a.b.d]", b"[]", b"[ ]", b"[.]", b"[a.]", b"[.a]",
    b"[a..b]", b"[a", b"[a\n", b"[a b]", b"[a]b", b"[a] b = 1", b"[a] # c", b"[a]]", b"]",
    b"[a]\n]", b"[[a]", b"[a]]", b"[a=1]", b"[a,b]", b"[a.\"b\".c]", b"[ [a] ]", b"[ [a]]",
    b"[\n", b"[", b"[#]", b"['''a''']", b"[\"\"\"a\"\"\"]", b"[a]\r\nb = 1\r\n", b"[a.b]\nc = 1\n[a]\nd = 2",
    b"a = 1\n[a]", b"a = 1\n[a.b]", b"a.b = 1\n[a]", b"a.b = 1\n[a.b]", b"a.b.c = 1\n[a.b]",
    b"[a]\nb = 1\n[a]\nc = 2", b"x = 1\n[a]\nx = 2", b"[a]\n[A]", b"[a]\n['a']",
    b"[x]\n[x.y]\n[x.y.z]\n[x]", b"[a.b.c]\n[a.b]\n[a]", b"[a]\nb = {}\n[a.b]",
    b"[a]\nb = []\n[a.b]", b"[a]\nb = [{}]\n[a.b]", b"[a]\nb = 1\n[a.b.c]",

    b"[[a]]", b"[[a]]\n[[a]]", b"[[a]]\nb = 1\n[[a]]\nb = 2", b"[[ a ]]", b"[[a.b]]\n[[a.b]]",
    b"[[a]]\n[a.b]", b"[[a]]\n[a.b]\nc = 1\n[[a]]\n[a.b]", b"[[a]]\n[[a.b]]\n[[a.b]]\n[[a]]\n[[a.b]]",
    b"a = []\n[[a]]", b"a = [1]\n[[a]]", b"[a]\n[[a]]", b"[[a]]\n[a]", b"a = 1\n[[a]]",
    b"[[a]]\nb.c = 1\n[a.b]", b"[[a]] # c", b"[[a]] x", b"[[a] ]", b"[[a]", b"[[a", b"[[]]",
    b"[[a.]]", b"[[a]]\n[[a.b]]\nc = 1\n[a.b]", b"a = [{}]\n[a.b]", b"a = [{}]\n[[a]]",
    b"[[a]]\nb = 1\n[a]", b"[[a.b]]\n[a]\nc = 1", b"[[a.b]]\n[a]\nb = 1",
    b"[a]\nb = 1\n[[a.c]]\n[[a.c]]\nd = 2", b"[[a]]\n[[a]]\nb.c = 1\nb.d = 2",
    b"a.b = 1\n[[a.c]]", b"a = 1\n[[a.b]]", b"[[a]]\nx=1\n[[a.b]]\nx=2\n[a.b.c]\nx=3",

    b"# \x01", b"# \x7f", b"# \t ok", b"# \x00", b"a = 1 # \x1b", b"# \r", b"# a\rb",
    b"a = 1 \r", b"a = 1\r", b"\r", b"\ra = 1", b"a = 1 # \xc3\xa9", b"# \xef\xbb\xbf",
    b"#\x0c", b"a = 1\x0c", b"a\x00 = 1", b"\x00", b"a = 1\n\x01",

    b"a = \"\xff\"", b"# \xc0\xaf", b"a = '\xed\xa0\x80'", b"a = '\xe2\x82'", b"a = 1\n\xf0\x9f",
    b"\xf4\x90\x80\x80 = 1", b"a = '\xc3'", b"\x80", b"a = '\xf8\x88\x80\x80\x80'",

    b"a = 1\n\n\nb = 2\n", b"  a = 1", b"\ta\t=\t1\t", b"a = 1  b", b"a\n= 1", b"a =\n1",
    b"[a]\n  b = 1\n  [a.c]\n    d = 2", b"a = 1 # c # d",

    b"[a]\n\"$__toml_private_datetime\" = \"1979-05-27\"",
    b"[a]\n\"$__toml_private_datetime\" = \"bad\"",
    b"[a]\n\"$__toml_private_datetime\" = 1", b"[a]\n\"$__toml_private_datetime\" = 1.5",
    b"[a]\n\"$__toml_private_datetime\" = 1e300", b"[a]\n\"$__toml_private_datetime\" = true",
    b"[a]\n\"$__toml_private_datetime\" = []", b"[a]\n\"$__toml_private_datetime\" = {}",
    b"[a]\n\"$__toml_private_datetime\" = 1979-05-27",
    b"[a]\n\"$__toml_private_datetime\" = 18446744073709551615",
    b"[a]\n\"$__toml_private_datetime\" = \"07:32\"\nz = 99999999999999999999",
    b"\"$__toml_private_datetime\" = \"1979-05-27\"",
    b"a = {\"$__toml_private_datetime\" = \"1979-05-27T07:32:00Z\"}",
    b"a = {\"!\" = 1, \"$__toml_private_datetime\" = \"x\"}",

    b"b = 99999999999999999999\na = 1e999", b"a = 99999999999999999999\nb = 1e999",
    b"[z]\na = 1e999\n[y]\nb = 0xFFFFFFFFFFFFFFFF", b"a = [1, 2, 1e999]", b"a = 1e999\nb = \"\\q\"",

    b"a = 'x\"y'", b"a = \"x'y\"", b"a = \"x'\\\"y\"", b"a = \"a\\nb\"", b"a = \"a\\nb'''\"",
    b"a = \"a\\nb\\\"\\\"\\\"\"", b"a = \"a\\n'''\\\"\\\"\\\"\"", b"a = \"a\\tb\"", b"a = \"a\\u007f\"",
    b"a = \"a\\\\b\"", b"a = \"a\\\\b'\"", b"a = \"\\\\\\n\"", b"a = \"\\n\"", b"a = \"''\\n\"",
    b"a = \"/path/to: x.y\"", b"a = \"\"\"\"\"\"\"", b"a = \"\\\"\\\"\\\"\\\"\"", b"\"a\\tb\" = 1",
    b"\"a'b\" = 1", b"\"a\\\"b\" = 1", b"\"a\\\\b\" = 1", b"\"a\\u0001b\" = 1", b"\"a'\\\"b\" = 1",
    b"\"\" = ''", b"\"a b\" = 'c d'", b"a = \"\\r\\n\"", b"a = \"\\u00e9\\n\"",
    b"a = 1e-7\nb = 1.5e300\nc = 123.456\nd = -0.0\ne = 1e22\nf = 1e21\ng = 0.1e-10",

    b"a = " + b"[" * 80 + b"]" * 80, b"a = " + b"[" * 81 + b"]" * 81,
    b"a = " + b"{b=" * 80 + b"1" + b"}" * 80, b"a = " + b"{b=" * 81 + b"1" + b"}" * 81,
    b".".join([b"k"] * 80) + b" = 1", b".".join([b"k"] * 81) + b" = 1",
    b"[" + b".".join([b"k"] * 81) + b"]", b"[" + b".".join([b"k"] * 80) + b"]",
    b"a = " + b"[" * 79 + b"{b=1}" + b"]" * 79, b"a = " + b"[" * 80 + b"{b=1}" + b"]" * 80,
    b"a = " + b"[" * 100,

    b"a = 1\n" * 12 + b"b = \"\\q\"", b"x = '\xc3\xa9\xc3\xa9' y", b"\xc3\xa9\xc3\xa9 = 1",
    b"a = \"\xc3\xa9\\q\"", b"a = 1\n" * 120 + b"b = @", b"a = \"" + b"x" * 300 + b"\\q\"",
]


SAMPLE = b"""# This is a TOML document

title = "TOML Example"

[owner]
name = "Tom Preston-Werner"
dob = 1979-05-27T07:32:00-08:00

[database]
enabled = true
ports = [ 8000, 8001, 8002 ]
data = [ ["delta", "phi"], [3.14] ]
temp_targets = { cpu = 79.5, case = 72.0 }

[servers]

[servers.alpha]
ip = "10.0.0.1"
role = "frontend"

[servers.beta]
ip = "10.0.0.2"
role = "backend"

[[products]]
name = "Hammer"
sku = 738594937

[[products]]  # empty table within the array

[[products]]
name = "Nail"
sku = 284758393

color = "gray"
"""


class Gen:
    def __init__(self, rng):
        self.r = rng

    def choice(self, xs):
        return xs[self.r.randrange(len(xs))]

    def key_part(self):
        r = self.r.random()
        pool = ["a", "b", "c", "key", "x-y", "k_1", "1", "A", "true", "inf", "nan", "0x1"]
        if r < 0.6:
            return self.choice(pool)
        if r < 0.75:
            return '"' + self.basic_body(short=True) + '"'
        if r < 0.85:
            return "'" + self.literal_body(short=True) + "'"
        if r < 0.9:
            return self.choice(['""', "''", '"a.b"', '"a b"', '"\\u00e9"', "'$'"])
        return "".join(self.choice("abcxyz_-0123") for _ in range(self.r.randint(1, 6)))

    def key(self, dotted=True):
        n = 1
        if dotted and self.r.random() < 0.3:
            n = self.r.randint(2, 4)
        sep = self.choice([".", ".", " . ", ". ", " ."])
        return sep.join(self.key_part() for _ in range(n))

    def basic_body(self, short=False):
        parts = []
        for _ in range(self.r.randint(0, 3 if short else 8)):
            r = self.r.random()
            if r < 0.5:
                parts.append(self.choice(["a", "hello", " ", "x y", "/p:q.", "'", "\u00e9", "\u20ac",
                                          "\U0001F600", "\t", "#", "=", "[", "{"]))
            elif r < 0.8:
                parts.append(self.choice(["\\n", "\\t", "\\\\", '\\"', "\\b", "\\f", "\\r", "\\e",
                                          "\\x41", "\\xff", "\\u00e9", "\\U0001F600", "\\u0000",
                                          "\\u001f", "\\x7f"]))
            else:
                parts.append(self.choice(['\\"\\"\\"', "''", "'''", "\\u0022"]))
        return "".join(parts)

    def literal_body(self, short=False):
        parts = []
        for _ in range(self.r.randint(0, 3 if short else 6)):
            parts.append(self.choice(["a", "C:\\dir\\f", " ", '"', '""', "\\n", "\u00e9", "\t", "#", "x.y"]))
        return "".join(parts)

    def ml_basic(self):
        nl = self.choice(["\n", "\r\n"])
        parts = []
        for _ in range(self.r.randint(0, 6)):
            parts.append(self.choice(["a", "line", nl, nl, '"', '""', "\\" + nl + "   ", "\\   " + nl,
                                      "\\n", "\\t", "\t", "'''", " ", "\\u00e9", "\u00e9", "\\\\"]))
        start = self.choice(["", nl])
        return '"""' + start + "".join(parts) + '"""'

    def ml_literal(self):
        nl = self.choice(["\n", "\r\n"])
        parts = []
        for _ in range(self.r.randint(0, 6)):
            parts.append(self.choice(["a", "line", nl, "'", "''", '"""', "\\", "\t", " ", "\u00e9"]))
        start = self.choice(["", nl])
        body = "".join(parts)
        if body.endswith("'"):
            body += "x"
        return "'''" + start + body + "'''"

    def integer(self):
        r = self.r.random()
        if r < 0.4:
            v = self.r.choice([0, 1, 7, 42, 1000, 123456789, 2**31, 2**53, 2**63 - 1])
            s = str(v)
            if self.r.random() < 0.3 and len(s) > 1:
                i = self.r.randint(1, len(s) - 1)
                s = s[:i] + "_" + s[i:]
            return self.choice(["", "+", "-"]) + s
        if r < 0.55:
            return self.choice(["0x", "0o", "0b"]) + self.choice(["0", "1", "10", "1_0"]) + (
                self.choice(["", "ff", "DEAD_beef"]) if self.r.random() < 0.3 else "")
        if r < 0.65:
            return self.choice(["0xdeadbeef", "0xDEADBEEF", "0o755", "0b1101", "0x7fffffffffffffff",
                                "0xffffffffffffffff", "9223372036854775808", "-9223372036854775808",
                                "-9223372036854775809", "18446744073709551616",
                                "170141183460469231731687303715884105728", "0x1_0000_0000_0000_0000"])
        return str(self.r.randint(-10**6, 10**6))

    def floating(self):
        r = self.r.random()
        if r < 0.15:
            return self.choice(["inf", "+inf", "-inf", "nan", "+nan", "-nan"])
        if r < 0.3:
            return self.choice(["0.0", "-0.0", "+0.0", "1e300", "1e-300", "1e309", "1e-400",
                                "5e-324", "1.7976931348623157e308", "0.1", "1e22", "1e21",
                                "123456789.123456789", "6.02e23", "1_000.000_1", "1e0_0"])
        mant = str(self.r.randint(0, 10**self.r.randint(1, 8)))
        frac = "" if self.r.random() < 0.3 else "." + str(self.r.randint(0, 10**self.r.randint(1, 8)))
        exp = ""
        if self.r.random() < 0.4 or not frac:
            exp = self.choice(["e", "E"]) + self.choice(["", "+", "-"]) + str(self.r.randint(0, 330))
        return self.choice(["", "+", "-"]) + mant + frac + exp

    def datetime(self):
        y = self.r.choice([1979, 2000, 1900, 2024, 2023, 1, 9999])
        mo = self.r.randint(1, 12)
        d = self.r.randint(1, 31 if self.r.random() < 0.9 else 32)
        h = self.r.randint(0, 23 if self.r.random() < 0.95 else 25)
        mi = self.r.randint(0, 59)
        s = self.r.randint(0, 60)
        date = "%04d-%02d-%02d" % (y, mo, d)
        time = "%02d:%02d" % (h, mi)
        if self.r.random() < 0.8:
            time += ":%02d" % s
            if self.r.random() < 0.4:
                time += "." + "".join(self.choice("0123456789") for _ in range(self.r.randint(1, 11)))
        off = self.choice(["Z", "z", "+00:00", "-07:00", "+05:30", "-00:00", "+23:59"])
        sep = self.choice(["T", "t", " "])
        kind = self.r.randrange(4)
        if kind == 0:
            return date + sep + time + off
        if kind == 1:
            return date + sep + time
        if kind == 2:
            return date
        return time

    def value(self, depth=0):
        r = self.r.random()
        if depth < 3 and r < 0.1:
            return self.array(depth + 1)
        if depth < 3 and r < 0.18:
            return self.inline_table(depth + 1)
        r = self.r.random()
        if r < 0.18:
            return self.integer()
        if r < 0.32:
            return self.floating()
        if r < 0.38:
            return self.choice(["true", "false"])
        if r < 0.5:
            return self.datetime()
        if r < 0.7:
            return '"' + self.basic_body() + '"'
        if r < 0.82:
            return "'" + self.literal_body() + "'"
        if r < 0.92:
            return self.ml_basic()
        return self.ml_literal()

    def ws(self):
        return self.choice(["", " ", " ", "  ", "\t"])

    def nl(self):
        return self.choice(["\n", "\n", "\n", "\r\n"])

    def comment(self):
        return self.choice(["", "", "", " # c", "# note \u00e9", " #", "\t# x"])

    def array(self, depth):
        items = [self.value(depth) for _ in range(self.r.randint(0, 4))]
        if self.r.random() < 0.3:
            sep = "," + self.nl() + self.ws()
            body = sep.join(items)
            if items and self.r.random() < 0.5:
                body += ","
            return "[" + self.nl() + self.ws() + body + self.comment() + self.nl() + "]"
        body = ("," + self.ws()).join(items)
        if items and self.r.random() < 0.3:
            body += ","
        return "[" + self.ws() + body + self.ws() + "]"

    def inline_table(self, depth):
        items = [self.key() + self.ws() + "=" + self.ws() + self.value(depth)
                 for _ in range(self.r.randint(0, 3))]
        if self.r.random() < 0.25:
            body = ("," + self.nl() + "  ").join(items)
            if items and self.r.random() < 0.5:
                body += ","
            return "{" + self.nl() + "  " + body + self.nl() + "}"
        body = ("," + self.ws()).join(items)
        if items and self.r.random() < 0.2:
            body += ","
        return "{" + self.ws() + body + self.ws() + "}"

    def keyval(self):
        return self.ws() + self.key() + self.ws() + "=" + self.ws() + self.value() + self.comment() + self.nl()

    def header(self):
        k = self.key()
        if self.r.random() < 0.3:
            return "[[" + self.ws() + k + self.ws() + "]]" + self.comment() + self.nl()
        return "[" + self.ws() + k + self.ws() + "]" + self.comment() + self.nl()

    def document(self):
        out = []
        if self.r.random() < 0.05:
            out.append("\ufeff")
        for _ in range(self.r.randint(0, 4)):
            out.append(self.keyval())
        for _ in range(self.r.randint(0, 4)):
            if self.r.random() < 0.2:
                out.append(self.nl())
            out.append(self.header())
            for _ in range(self.r.randint(0, 4)):
                out.append(self.keyval())
        return "".join(out).encode("utf-8")


def json_free_string(rng, pieces, n):
    """A basic-string body: printable pieces kept, others as \\u escapes."""
    out = []
    for _ in range(n):
        if rng.random() < 0.7:
            c = rng.choice(pieces)
        else:
            c = chr(rng.choice([rng.randint(0, 0x7F), rng.randint(0x80, 0xD7FF), rng.randint(0xE000, 0xFFFF),
                                rng.randint(0x10000, 0x10FFFF)]))
        for ch in c:
            if ch == "\\":
                out.append("\\\\")
            elif ch == '"':
                out.append('\\"')
            elif ord(ch) < 0x20 or ord(ch) == 0x7F:
                out.append("\\u%04x" % ord(ch))
            else:
                out.append(ch)
    return "".join(out)


def focused(rng, n):
    """Mostly valid one-value documents aimed at number parsing and printing,
    string and key escaping, and date-times."""
    out = []
    pieces = [chr(c) for c in range(0x20)] + ["\x7f", "a", " ", "'", "''", "'''", '"', '""', '"""', "\\",
                                              "é", "€", "\U0001F600", ".", "/", ":", "-", "_", "#",
                                              "=", "x"]
    for _ in range(n):
        kind = rng.randrange(9)
        if kind == 0:

            f = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
            if rng.random() < 0.4:


                f = rng.choice([rng.randint(2**49, 2**53) + rng.randint(0, 7) / 8,
                                rng.randint(1, 2**53) * 2.0 ** rng.randint(-1074, 971)])
            text = repr(f)
            if text not in ("inf", "-inf", "nan") and "e" not in text and "." not in text:
                text += ".0"
            doc = "v = " + text
        elif kind == 1:

            mant = "".join(rng.choice("0123456789") for _ in range(rng.randint(1, 40))).lstrip("0") or "0"
            frac = "".join(rng.choice("0123456789") for _ in range(rng.randint(1, 40)))
            exp = rng.choice(["", "e%d" % rng.randint(-340, 320), "E+%d" % rng.randint(0, 320)])
            doc = "v = " + rng.choice(["", "-", "+"]) + mant + "." + frac + exp
        elif kind == 2:

            v = rng.choice([rng.getrandbits(rng.randint(1, 130)),
                            2**63 + rng.randint(-3, 3), 2**64 + rng.randint(-3, 3),
                            2**127 + rng.randint(-3, 3), 2**128 + rng.randint(-3, 3)])
            fmt = rng.randrange(4)
            if fmt == 0:
                doc = "v = " + rng.choice(["", "-", "+"]) + str(v)
            elif fmt == 1:
                doc = "v = 0x" + (rng.choice(["%x", "%X"]) % v)
            elif fmt == 2:
                doc = "v = 0o%o" % v
            else:
                doc = "v = 0b{:b}".format(v)
        elif kind == 3:

            doc = 'v = "' + json_free_string(rng, pieces, rng.randint(0, 8)) + '"'
        elif kind == 4:

            key = '"' + json_free_string(rng, pieces, rng.randint(0, 5)) + '"'
            doc = key + " = 1\n[t]\n" + key + " = { " + key + " = 2 }"
        elif kind == 5:
            g = Gen(rng)
            doc = "v = " + g.datetime() + "\nw = [" + g.datetime() + ", " + g.datetime() + "]"
        elif kind == 6:

            nl = rng.choice(["\n", "\r\n"])
            body = "".join(rng.choice(["a", nl, '"', '""', "'", "''", "\\t", "\\\\", "\t", " "])
                           for _ in range(rng.randint(0, 10)))
            doc = 'v = """' + body + 'x"""'
        elif kind == 7:

            g = Gen(rng)
            doc = "\n".join("k%d = %s" % (i, g.value()) for i in range(rng.randint(1, 6)))
        else:
            g = Gen(rng)
            lines = []
            for t in range(rng.randint(1, 4)):
                lines.append(rng.choice(["[t%d]", "[[t%d]]", "[s.t%d]", "[[s.t%d]]"]) % t)
                for i in range(rng.randint(0, 3)):
                    lines.append("k%d = %s" % (i, g.value()))
            doc = "\n".join(lines)
        out.append(doc.encode("utf-8"))
    return out


INSERTS = [b"=",b"[", b"]", b"{", b"}", b'"', b"'", b".", b"#", b"\n", b"\r", b"\t", b" ",
           b",", b"\\", b"_", b"+", b"-", b":", b"0", b"e", b"x", b"T", b"Z", b"'''", b'"""',
           b"\r\n", b"[[", b"]]", b"\x00", b"\x01", b"\x1b", b"\x7f", b"\x0c", b"\xff", b"\xc3",
           b"\xed\xa0\x80", b"\xc3\xa9", b"\xef\xbb\xbf", b"\xf0\x9f\x98\x80", b"\x80", b"a"]


def mutate(rng, doc):
    data = bytearray(doc)
    for _ in range(rng.randint(1, 3)):
        r = rng.random()
        pos = rng.randint(0, len(data))
        if r < 0.3 and data:
            end = min(len(data), pos + rng.randint(1, 3))
            del data[pos:end]
        elif r < 0.75:
            data[pos:pos] = rng.choice(INSERTS)
        elif r < 0.85 and data:
            data = data[:pos]
        elif r < 0.95 and data:
            p = min(pos, len(data) - 1)
            data[p] = rng.choice(b"=[]{}\"'.#\n\r\t ,\\_+-:0ex")
        else:
            lines = bytes(data).split(b"\n")
            if len(lines) > 1:
                i = rng.randrange(len(lines))
                lines.insert(rng.randrange(len(lines)), lines[i])
            data = bytearray(b"\n".join(lines))
    return bytes(data)


def build_corpus(seed, generated, mutated, focus):
    rng = random.Random(seed)
    gen = Gen(rng)
    cases = list(HAND)
    cases.append(SAMPLE)
    cases.append(SAMPLE.replace(b"\n", b"\r\n"))
    if os.path.isfile(REFERENCE_EXAMPLE):
        with open(REFERENCE_EXAMPLE, "rb") as f:
            example = f.read()
        cases.append(example)
        cases.append(example.replace(b"# data_dir", b"data_dir").replace(b"# cache_dir", b"cache_dir"))
    else:
        print("note: reference example not found:", REFERENCE_EXAMPLE)
    docs = [gen.document() for _ in range(generated)]
    cases.extend(docs)
    cases.extend(focused(rng, focus))
    seeds = docs + list(HAND) + [SAMPLE]
    for _ in range(mutated):
        cases.append(mutate(rng, rng.choice(seeds)))
    return cases


CONFIG_HAND = [
    b"", b"[paths]", b"paths = {}", b"[paths]\ngame_dir = 'D:\\g'", b"game = 'x'", b"\ngame = 'x'",
    b"[paths]\ngame = 'x'", b"[paths]\ngame_dir = 5", b"[paths]\ngame_dir = 'a'\ncache_dir = 'b'\ndata_dir = 'c'\ntest_titles = ['x', 'y']",
    b"paths = 1", b"paths = []", b"paths = ['a']", b"paths = ['a', 'b', 'c']", b"paths = ['a', 'b', 'c', ['t']]",
    b"paths = ['a', 'b', 'c', 't']", b"paths = ['a', 'b', 'c', [], 'extra']", b"paths = [1]", b"paths = 'x'",
    b"paths = \"a\\nb\\t\\u00e9\\u0301\\u200b\\ufeff\\u0085\\u00ad\\u0007\\\\\\\"'\"", b"paths = true",
    b"paths = 1.5", b"paths = 1e300", b"paths = -0.0", b"paths = nan", b"paths = -inf", b"paths = 1e999",
    b"paths = 99999999999999999999", b"paths = 18446744073709551615", b"paths = -9223372036854775809",
    b"paths = 0xffffffffffffffffffffffffffffffffff", b"paths = 1979-05-27", b"paths = 07:32",
    b"[[paths]]", b"[[paths]]\ngame_dir = 'a'", b"[[paths]]\n[[paths]]", b"paths.game_dir = 'x'",
    b"paths.game_dir = 5", b"paths.game = 5", b"paths.game_dir.x = 5", b"[paths.game_dir]",
    b"[paths.sub]", b"[paths]\ntest_titles = 'x'", b"[paths]\ntest_titles = ['x', 5]",
    b"[paths]\ntest_titles = {a = 1}", b"[paths]\ntest_titles = 1979-05-27T07:32:00Z",
    b"[paths]\ntest_titles = [[]]", b"[paths]\ntest_titles = [{}]", b"[[paths.test_titles]]",
    b"[paths]\ngame_dir = []", b"[paths]\ngame_dir = {}", b"[paths]\ngame_dir = 1979-05-27",
    b"[paths]\ngame_dir = 1e999", b"[paths]\ngame_dir = 99999999999999999999999999999999999999999",
    b"[paths]\ngame_dir = -170141183460469231731687303715884105729", b"[paths]\ngame_dir = 0x8000000000000000",
    b"[paths]\ngame_dir = 3.0", b"[paths]\ngame_dir = 1e20", b"[paths]\ngame_dir = 0.1",
    b"[paths]\nzzz = 99999999999999999999", b"zzz = 1e999\n[paths]", b"a = 1\npaths = 2", b"z = 1\npaths = 2",
    b"[paths]\n\"$__toml_private_datetime\" = 'x'", b"\"$__toml_private_datetime\" = 'x'",
    b"paths = { game_dir = 'a', bad = 1 }", b"paths = { game_dir = 5 }", b"[paths]\n\xc3\xa9 = 1",
    b"[paths]\n'game dir' = 1", b"[paths]\n\"\" = 1", b"paths = 'x\n", b"[paths]\ngame_dir = 'x'\n[paths]",
    b"[paths]\ngame_dir = \"C:\\\\Games\\\\x\"", b"[paths]\ngame_dir = '\xc3\xa9\xe2\x82\xac'",
    b"\xef\xbb\xbf[paths]\ngame = 1", b"[paths]\r\ngame = 1\r\n", b"[ paths ]\n  game = 1",
    b"paths = [\n  'a',\n  5,\n]", b"[paths]\ntest_titles = [\n  'a',\n  {b = 1},\n]",
]


def config_doc(rng):
    g = Gen(rng)
    strings = ["'D:\\victory-data\\game'", "'C:/x'", '"a\\tb"', "''", '"\\u00e9\\u0301"', "'\u00e9'",
               '"\\u200b"', '"\\ufeff"', '"x\\ny"', '"\\u0085"', "'''ml\nx'''", '"""ml"""']

    def scalar():
        r = rng.random()
        if r < 0.55:
            return rng.choice(strings)
        if r < 0.7:
            return rng.choice([g.integer(), g.floating(), "true", g.datetime()])
        if r < 0.85:
            return "[" + ", ".join(rng.choice(strings + ["1", "[]", "{}"]) for _ in range(rng.randint(0, 3))) + "]"
        return "{" + ", ".join("%s = %s" % (rng.choice(["a", "game_dir"]), rng.choice(strings))
                               for _ in range(rng.randint(0, 2))) + "}"

    fields = ["game_dir", "cache_dir", "data_dir", "test_titles"]
    bad = ["game", "a", "zzz", "Game_dir", '"cache dir"', "test_title", "paths"]

    def field():
        k = rng.choice(fields) if rng.random() < 0.85 else rng.choice(bad)
        if k == "test_titles" and rng.random() < 0.7:
            v = "[" + ", ".join(rng.choice(strings) if rng.random() < 0.9 else scalar()
                                for _ in range(rng.randint(0, 3))) + "]"
        else:
            v = rng.choice(strings) if rng.random() < 0.75 else scalar()
        return k, v

    lines = []
    top = [k for k in ["a", "z", "game"] if rng.random() < 0.08]
    for k in top:
        lines.append("%s = %s" % (k, scalar()))
    form = rng.randrange(8)
    fs = {}
    for _ in range(rng.randint(0, 4)):
        k, v = field()
        fs[k] = v
    if form <= 2:
        lines.append(rng.choice(["[paths]", "[ paths ]", '["paths"]']))
        lines += ["%s = %s" % kv for kv in fs.items()]
    elif form == 3:
        lines.append("paths = { " + ", ".join("%s = %s" % kv for kv in fs.items()) + " }")
    elif form == 4:
        lines += ["paths.%s = %s" % kv for kv in fs.items()]
    elif form == 5:
        lines.append("[[paths]]")
        lines += ["%s = %s" % kv for kv in fs.items()]
    elif form == 6:
        lines.append("paths = " + scalar())
    else:
        n = rng.randint(0, 5)
        lines.append("paths = [" + ", ".join(rng.choice(strings) if rng.random() < 0.8 else scalar()
                                              for _ in range(n)) + "]")
    if rng.random() < 0.1:
        lines.append(rng.choice(["[paths.sub]", "[other]", "x = 1"]))
    rng.shuffle(lines) if rng.random() < 0.05 else None
    return "\n".join(lines).encode("utf-8")


def build_config_corpus(seed, generated, mutated):
    rng = random.Random(seed)
    cases = list(CONFIG_HAND)
    if os.path.isfile(REFERENCE_EXAMPLE):
        with open(REFERENCE_EXAMPLE, "rb") as f:
            example = f.read()
        cases.append(example)
        cases.append(example.replace(b"# data_dir", b"data_dir").replace(b"# cache_dir", b"cache_dir"))
        cases.append(example.replace(b"test_titles = []", b"test_titles = ['D:\\t1', 'D:\\t2']"))
    docs = [config_doc(rng) for _ in range(generated)]
    cases.extend(docs)
    seeds = docs + list(CONFIG_HAND)
    while mutated > 0:
        case = mutate(rng, rng.choice(seeds))
        try:
            case.decode("utf-8")   
        except UnicodeDecodeError:
            continue
        cases.append(case)
        mutated -= 1
    return cases


def write_records(path, records):
    with open(path, "wb") as f:
        for r in records:
            f.write(struct.pack("<I", len(r)))
            f.write(r)


def read_records(path):
    with open(path, "rb") as f:
        data = f.read()
    out = []
    at = 0
    while at < len(data):
        (n,) = struct.unpack_from("<I", data, at)
        out.append(data[at + 4:at + 4 + n])
        at += 4 + n
    return out


def safe(b):
    """Bytes as printable ASCII for any console."""
    return b.decode("utf-8", "replace").encode("ascii", "backslashreplace").decode("ascii")


def compare(corpus, oracle, port, show=10):
    if not (len(corpus) == len(oracle) == len(port)):
        print("record counts differ: corpus %d, oracle %d, port %d" % (len(corpus), len(oracle), len(port)))
        return 1
    ok = sum(1 for o in oracle if o.startswith(b"OK\n"))
    err = sum(1 for o in oracle if o.startswith(b"ERR\n"))
    panic = sum(1 for o in oracle if o.startswith(b"PANIC"))
    classes = {}
    diffs = []
    for i, (c, o, p) in enumerate(zip(corpus, oracle, port)):
        if o.startswith(b"ERR\n"):
            last = o.rstrip(b"\n").split(b"\n")[-1]
            key = re.sub(rb"[0-9]+", b"N", last.split(b",")[0])
            classes[key] = classes.get(key, 0) + 1
        if o != p:
            diffs.append(i)
    print("cases: %d  valid: %d  errors: %d  panics: %d" % (len(corpus), ok, err, panic))
    print("error classes (by message head): %d" % len(classes))
    for k, n in sorted(classes.items(), key=lambda kv: -kv[1])[:60]:
        print("  %6d  %s" % (n, safe(k)))
    print("differences: %d" % len(diffs))
    for i in diffs[:show]:
        print("---- case %d input: %r" % (i, corpus[i]))
        print("oracle:\n" + safe(oracle[i]))
        print("port:\n" + safe(port[i]))
    return 1 if diffs else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("run", "corpus", "run-config"):
        p = sub.add_parser(name)
        if name == "corpus":
            p.add_argument("out")
        p.add_argument("--seed", type=int, default=20261126)
        p.add_argument("--generated", type=int, default=25000)
        p.add_argument("--mutated", type=int, default=45000)
        p.add_argument("--focused", type=int, default=30000)
        if name in ("run", "run-config"):
            p.add_argument("--oracle", default=os.path.join(ROOT, "build", "toml-target", "release", "toml-oracle.exe"))
            p.add_argument("--port", default=os.path.join(ROOT, "build", "cpp-release", "toml-check.exe"))
            p.add_argument("--work", default=os.path.join(ROOT, "build", "cpp-release", "m7-toml"))
            p.add_argument("--show", type=int, default=10)
    p = sub.add_parser("compare")
    p.add_argument("corpus")
    p.add_argument("oracle")
    p.add_argument("port")
    p.add_argument("--show", type=int, default=10)
    a = ap.parse_args()

    if a.cmd == "corpus":
        write_records(a.out, build_corpus(a.seed, a.generated, a.mutated, a.focused))
        return 0
    if a.cmd == "compare":
        return compare(read_records(a.corpus), read_records(a.oracle), read_records(a.port), a.show)

    os.makedirs(a.work, exist_ok=True)
    config = a.cmd == "run-config"
    stem = "config-" if config else ""
    corpus_path = os.path.join(a.work, stem + "corpus.bin")
    oracle_out = os.path.join(a.work, stem + "oracle.bin")
    port_out = os.path.join(a.work, stem + "port.bin")
    if config:
        cases = build_config_corpus(a.seed, a.generated // 5, a.mutated // 15)
    else:
        cases = build_corpus(a.seed, a.generated, a.mutated, a.focused)
    write_records(corpus_path, cases)
    mode = ["config"] if config else []
    subprocess.run([a.oracle] + mode + [corpus_path, oracle_out], check=True)
    subprocess.run([a.port] + mode + [corpus_path, port_out], check=True)
    return compare(cases, read_records(oracle_out), read_records(port_out), a.show)


if __name__ == "__main__":
    sys.exit(main())
