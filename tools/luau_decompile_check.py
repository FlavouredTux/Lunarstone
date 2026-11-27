#!/usr/bin/env python3
"""Checks tools/luau_decompile.py the only way that counts: Luau source is
compiled, decompiled and run again, and both runs have to print the same.

    tools/luau_decompile_check.py --luau <dir> [--fuzz 100] [--seed 1] [--jobs N] [--keep <dir>]

<dir> holds Luau's luau and luau-compile, built from the copy Lunarstone
vendors so the bytecode is the same version a live game sends:

    cmake -S build/release/_deps/luau-src -B build/luau-cli -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DLUAU_BUILD_CLI=ON -DLUAU_BUILD_TESTS=OFF
    cmake --build build/luau-cli --target Luau.Repl.CLI Luau.Compile.CLI

Every program goes through both -O1 and -O2 (Roblox compiles with -O2,
which inlines small local functions and threads jumps, the shapes that are
hardest to read back). The programs are the hand-written cases below, each
aimed at one construct, and --fuzz random ones (nested ifs and elseifs,
numeric, generic, while and repeat loops with break and continue, early
returns, and/or values, closures, tables, functions -O2 inlines); a seed
reproduces its program. A failure leaves its source, bytecode and
decompiled output in --keep (default a temporary folder).
"""

import argparse
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

CASES = {
    "basics": r'''
local function f(a, b, ...)
    local x = a + b * 2 - (a - b) / 3
    local s = "v" .. tostring(a) .. ":" .. tostring(b)
    local n = select("#", ...)
    local t = {a, b, ..., k = "z", [a + 1] = b}
    print(x, s, n, #t, t.k, t[a + 1])
    return x, s, ...
end
print(f(3, 4, "p", "q"))
print((f(1, 2)))
local g = { list = {1, 2, 3}, nested = { deep = { value = 42 } } }
print(g.nested.deep.value, #g.list, g.list[2])
local str = "hello"
print(str:upper(), str:sub(2, 3), ("x"):rep(3))
local obj = {}
function obj.method(self, n) return self.base + n end
obj.base = 10
print(obj:method(5), obj.method(obj, 6))
print(-f(2, 2) , not nil, #"abc", 2 ^ 3 ^ 2, 7 // 2, 7 % 3)
''',
    "calls": r'''
local Obj = {}
Obj.__index = Obj
function Obj.new(name) return setmetatable({name = name, items = {}}, Obj) end
function Obj:add(x, ...) table.insert(self.items, x) if select("#", ...) > 0 then return self:add(...) end return self end
function Obj:get(i) return self.items[i] end
function Obj:find(f) for i, v in ipairs(self.items) do if f(v, i) then return v, i end end end
local o = Obj.new("o"):add(1, 2, 3, 4)
print(o:get(2), #o.items, o:find(function(v) return v > 2 end))
print(o:find(function(v, i) return i == 4 and v end))
local function multi() return 1, 2, 3 end
local t = {multi()}
local u = {multi(), multi()}
print(#t, #u, (multi()), select(2, multi()))
local function wrap(...) local n = select("#", ...) return n, ... end
print(wrap(nil, nil), wrap())
local s = "a,b,,c"
local parts = {}
for piece in string.gmatch(s, "([^,]*)") do parts[#parts + 1] = "[" .. piece .. "]" end
print(table.concat(parts), s:find(",", 1, true), s:gsub(",", ";"))
local cfg = {depth = {inner = {value = 5}}, flags = {on = true}}
local v = cfg.depth and cfg.depth.inner and cfg.depth.inner.value or 0
local w = cfg.missing and cfg.missing.x or "default"
print(v, w, cfg.flags.on and "yes" or "no", not cfg.flags.off)
local counter = 0
local function bump() counter = counter + 1 return counter end
if bump() > 0 and bump() > 5 then print("no") end
print(counter)
local r = bump() == 3 or bump() == 99
print(r, counter)
''',
    "closures": r'''
local function counter()
    local c = 0
    return function() c = c + 1 return c end, function() return c end
end
local inc, get = counter()
inc() inc()
print(get())
local fs = {}
for i = 1, 3 do fs[i] = function() return i * 10 end end
print(fs[1](), fs[3]())
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print(fib(15))
local M = {}
M.__index = M
function M.new(v) return setmetatable({v = v}, M) end
function M:get() return self.v end
print(M.new(7):get())
local acc = {}
local function add(...)
    for _, v in ipairs({...}) do table.insert(acc, v) end
    return #acc
end
print(add(1, 2), add(3), table.concat(acc, ","))
''',
    "control": r'''
local function classify(n)
    if n < 0 then
        return "neg"
    elseif n == 0 then
        return "zero"
    elseif n < 10 then
        return "small"
    else
        return "big"
    end
end
for _, v in ipairs({-5, 0, 3, 50}) do print(v, classify(v)) end
local function both(a, b)
    if a and b then print("both") end
    if a or b then print("either") end
    if (a or b) and not (a and b) then print("exactly one") end
    if not a then print("not a") else print("a") end
    local v = a and "A" or "noA"
    local w = a or b
    local z = if a then 1 else 2
    print(v, w, z, a == b, a ~= b)
end
both(true, false) both(false, false) both(true, true) both(nil, 1)
local i = 0
while i < 5 do
    i += 1
    if i == 2 then continue end
    if i == 4 then break end
    print("w", i)
end
local j = 0
repeat
    j = j + 1
    print("r", j)
until j >= 3 or j == 10
for k = 10, 1, -3 do print("down", k) end
for k, v in pairs({a = 1}) do print(k, v) end
local sum = 0
for idx = 1, 4 do
    for jdx = 1, idx do
        if jdx == 3 then break end
        sum = sum + jdx
    end
end
print(sum)
local function loopret(t)
    for _, x in ipairs(t) do
        if x > 2 then return x end
    end
    return nil
end
print(loopret({1, 2, 3, 4}), loopret({1}))
''',
    "inline": r'''
local function kind(x)
    local t = type(x)
    if t == "table" then
        if x.a then return "A" end
        if x.b then return "B" end
    elseif t == "number" then
        return "num"
    elseif t == "string" then
        return x
    end
    return "other"
end
local function classify(list)
    local out = {}
    for i = 1, #list do
        local k = kind(list[i])
        out[#out + 1] = k .. "!"
    end
    return table.concat(out, " ")
end
print(classify({{a = 1}, {b = 1}, {}, 5, "s", true}))
local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end
local total = 0
for _, v in ipairs({-5, 3, 50}) do total += clamp(v, 0, 10) end
print(total)
local function isCallable(value)
    if type(value) == "function" then return true end
    if type(value) == "table" then
        local mt = getmetatable(value)
        if mt and type(rawget(mt, "__call")) == "function" then return true end
    end
    return false
end
print(isCallable(print), isCallable({}), isCallable(setmetatable({}, {__call = print})), isCallable(3))
''',
    "threading": r'''
local log = {}
local function note(...) table.insert(log, table.concat({...}, ",")) end
local function pick(a, b, c)
    if a then
        if b then
            note("ab")
        else
            note("a")
            if c then return "early" end
        end
    elseif b then
        note("b")
        if c then note("bc") end
    elseif c then
        note("c")
    else
        note("none")
    end
    return "done"
end
for _, a in ipairs({true, false}) do
    for _, b in ipairs({true, false}) do
        for _, c in ipairs({true, false}) do note(pick(a, b, c)) end
    end
end
local function find(t, want)
    for i, v in ipairs(t) do
        if v == want then
            if i > 2 then return i, "late" end
            return i, "early"
        end
    end
    return nil
end
note(find({1, 2, 3, 4}, 4)) note(find({1, 2}, 1)) note(tostring(find({}, 1)))
local function scan(n)
    local out = {}
    for i = 1, n do
        if i % 2 == 0 then continue end
        if i > 7 then break end
        local j = i
        while j > 0 do
            j -= 2
            if j == 3 then break end
        end
        table.insert(out, i .. ":" .. j)
    end
    return table.concat(out, " ")
end
note(scan(12))
local function guard(x)
    if not x then return "nil" end
    if type(x) ~= "table" then return "notable" end
    if x.a and x.b or x.c then return "abc" end
    return "plain"
end
note(guard(nil), guard(1), guard({a = 1, b = 2}), guard({c = 1}), guard({a = 1}))
local function nested(t)
    local total = 0
    for k, v in pairs(t) do
        if type(v) == "table" then
            for _, w in ipairs(v) do
                if w < 0 then
                    total -= 1
                elseif w > 10 then
                    total += 10
                    if w > 100 then break end
                else
                    total += w
                end
            end
        elseif v then
            total += 1
        end
    end
    return total
end
note(nested({x = {1, -2, 50, 500, 3}, y = true, z = false}))
print(table.concat(log, "\n"))
''',
    "values": r'''
local function check(a, b)
    local eq = a == b
    local lt = a < b
    local le = a <= b
    local gt = a > b
    local nl = a ~= nil
    print(eq, lt, le, gt, nl, not eq)
    local r = a > b and a or b
    print(r)
    local s = (a > 1 and b > 1) or (a < 0)
    print(s)
end
check(1, 2) check(3, 3) check(5, -1)
local t = {}
t.x, t.y = 1, 2
local a, b, c = 1, 2
print(t.x, t.y, a, b, c)
local u = {[1] = "one", [2] = "two", three = 3}
print(u[1], u[2], u.three)
local n = 0
for _ = 1, 5 do n += 2 end
n -= 1 n *= 3 n //= 2 n ..= "!"
print(n)
''',
}


# --- random programs ----------------------------------------------------------


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.depth = 0
        self.funcs = []
        self.n = 0

    def name(self, p="v"):
        self.n += 1
        return f"{p}{self.n}"

    def num(self):
        return str(self.r.choice([0, 1, 2, 3, 5, 7, 10, -1, -4]))

    def atom(self, vars_):
        c = self.r.random()
        if vars_ and c < 0.55:
            return self.r.choice(vars_)
        if c < 0.75:
            return self.num()
        if c < 0.82:
            return self.r.choice(["true", "false", "nil"])
        if c < 0.9:
            return '"' + self.r.choice(["a", "b", "xy", ""]) + '"'
        return "#{" + ", ".join(self.num() for _ in range(self.r.randint(0, 3))) + "}"

    def numexpr(self, vars_, d=0):
        nums = [v for v in vars_ if v.startswith(("n", "i", "p"))]
        if d > 2 or self.r.random() < 0.4:
            return self.r.choice(nums) if nums and self.r.random() < 0.6 else self.num()
        op = self.r.choice(["+", "-", "*", "%"])
        a, b = self.numexpr(vars_, d + 1), self.numexpr(vars_, d + 1)
        if op == "%":
            return f"({a} % 7)"
        return f"({a} {op} {b})"

    def cond(self, vars_, d=0):
        c = self.r.random()
        if d < 2 and c < 0.25:
            return f"({self.cond(vars_, d + 1)} {self.r.choice(['and', 'or'])} {self.cond(vars_, d + 1)})"
        if d < 2 and c < 0.32:
            return f"not ({self.cond(vars_, d + 1)})"
        if c < 0.45 and vars_:
            return self.r.choice(vars_)
        op = self.r.choice(["<", "<=", ">", ">=", "==", "~="])
        return f"{self.numexpr(vars_)} {op} {self.numexpr(vars_)}"

    def value(self, vars_):
        c = self.r.random()
        if c < 0.2:
            return f"({self.cond(vars_)}) and {self.atom(vars_)} or {self.atom(vars_)}"
        if c < 0.3:
            return f"if {self.cond(vars_)} then {self.atom(vars_)} else {self.atom(vars_)}"
        if c < 0.4 and self.funcs:
            f, nparams = self.r.choice(self.funcs)
            return f"{f}({', '.join(self.numexpr(vars_) for _ in range(nparams))})"
        if c < 0.5:
            return f"tostring({self.atom(vars_)}) .. \"|\""
        return self.numexpr(vars_)

    def block(self, vars_, ind, in_loop, in_func, budget):
        lines = []
        vars_ = list(vars_)
        for _ in range(self.r.randint(1, 4)):
            if budget[0] <= 0:
                break
            budget[0] -= 1
            lines += self.stmt(vars_, ind, in_loop, in_func, budget)
        return lines

    def stmt(self, vars_, ind, in_loop, in_func, budget):
        pad = "    " * ind
        c = self.r.random()
        nums = [v for v in vars_ if v.startswith("n")]
        if c < 0.18 or not vars_:
            v = self.name("n")
            vars_.append(v)
            return [f"{pad}local {v} = {self.numexpr(vars_[:-1])}"]
        if c < 0.28 and nums:
            v = self.r.choice(nums)
            return [f"{pad}{v} = {self.numexpr(vars_)}"]
        if c < 0.38:
            return [f"{pad}out({self.value(vars_)})"]
        if c < 0.55 and self.depth < 4:
            self.depth += 1
            out = [f"{pad}if {self.cond(vars_)} then"]
            out += self.block(vars_, ind + 1, in_loop, in_func, budget)
            for _ in range(self.r.choice([0, 0, 1, 2])):
                out.append(f"{pad}elseif {self.cond(vars_)} then")
                out += self.block(vars_, ind + 1, in_loop, in_func, budget)
            if self.r.random() < 0.5:
                out.append(f"{pad}else")
                out += self.block(vars_, ind + 1, in_loop, in_func, budget)
            out.append(f"{pad}end")
            self.depth -= 1
            return out
        if c < 0.65 and self.depth < 4:
            self.depth += 1
            i = self.name("i")
            lo, hi = self.r.randint(-2, 2), self.r.randint(0, 4)
            step = self.r.choice(["", "", ", 2", ", -1"])
            out = [f"{pad}for {i} = {lo}, {hi}{step} do"]
            out += self.block(vars_ + [i], ind + 1, True, in_func, budget)
            out.append(f"{pad}end")
            self.depth -= 1
            return out
        if c < 0.72 and self.depth < 4:
            self.depth += 1
            k, v = self.name("k"), self.name("e")
            src = self.r.choice(["ipairs({3, 1, 4})", "ipairs({})", "pairs({x = 1})", "ipairs({5, nil, 6})"])
            out = [f"{pad}for {k}, {v} in {src} do"]
            out += self.block(vars_ + [v], ind + 1, True, in_func, budget)
            out.append(f"{pad}end")
            self.depth -= 1
            return out
        if c < 0.78 and self.depth < 4:
            self.depth += 1
            w = self.name("n")
            out = [f"{pad}local {w} = 0", f"{pad}while {w} < {self.r.randint(1, 4)} {self.r.choice(['and', 'or'])} {self.cond(vars_)} and {w} < 6 do",
                   f"{pad}    {w} += 1"]
            out += self.block(vars_ + [w], ind + 1, True, in_func, budget)
            out.append(f"{pad}end")
            vars_.append(w)
            self.depth -= 1
            return out
        if c < 0.82 and self.depth < 4:
            self.depth += 1
            w = self.name("n")
            out = [f"{pad}local {w} = 0", f"{pad}repeat", f"{pad}    {w} += 1"]
            out += self.block(vars_ + [w], ind + 1, True, in_func, budget)
            out.append(f"{pad}until {w} >= {self.r.randint(1, 3)} or {self.cond(vars_)}")
            vars_.append(w)
            self.depth -= 1
            return out
        if c < 0.88 and in_loop:
            return [f"{pad}if {self.cond(vars_)} then {self.r.choice(['break', 'continue'])} end"]
        if c < 0.93 and in_func:
            return [f"{pad}if {self.cond(vars_)} then return {self.value(vars_)} end"]
        if c < 0.97:
            f = self.name("c")
            v = self.r.choice(nums) if nums else "0"
            return [f"{pad}local {f} = function(q) return q + {v} end", f"{pad}out({f}(1))"]
        t = self.name("t")
        return [f"{pad}local {t} = {{{self.atom(vars_)}, k = {self.atom(vars_)}}}", f"{pad}out({t}[1], {t}.k)"]

    def program(self):
        lines = ["local log = {}",
                 "local function out(...) local t = {} for i = 1, select('#', ...) do t[i] = tostring((select(i, ...))) end table.insert(log, table.concat(t, ',')) end"]
        for _ in range(self.r.randint(1, 3)):
            f = self.name("f")
            nparams = self.r.randint(0, 3)
            params = [f"p{self.name('')}" for _ in range(nparams)]
            body = self.block(params, 1, False, True, [12])
            lines.append(f"local function {f}({', '.join(params)})")
            lines += body
            lines.append(f"    return {self.value(params)}")
            lines.append("end")
            self.funcs.append((f, nparams))
        for f, nparams in self.funcs:
            for args in range(3):
                vals = [str(self.r.randint(-3, 6)) for _ in range(nparams)]
                lines.append(f"out({f}({', '.join(vals)}))")
        lines += self.block([], 0, False, False, [10])
        lines.append("print(table.concat(log, '\\n'))")
        return "\n".join(lines) + "\n"


def run(luau, path):
    try:
        r = subprocess.run([luau, path], capture_output=True, text=True, timeout=20)
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return "timeout"


def check(name, source, args, keep):
    """Both optimization levels of one program; the failures' descriptions."""
    luau = os.path.join(args.luau, "luau")
    compiler = os.path.join(args.luau, "luau-compile")
    src = os.path.join(keep, name + ".luau")
    with open(src, "w") as f:
        f.write(source)
    want = run(luau, src)
    if "error" in want or want == "timeout":
        return None  # a program that fails by itself says nothing
    failures = []
    for level in ("1", "2"):
        bc = os.path.join(keep, f"{name}.O{level}.bin")
        out = os.path.join(keep, f"{name}.O{level}.lua")
        with open(bc, "wb") as f:
            subprocess.run([compiler, "--binary", f"-O{level}", "-g1", src], stdout=f, check=True)
        dec = subprocess.run([sys.executable, os.path.join(HERE, "luau_decompile.py"), bc, "-o", out],
                             capture_output=True, text=True)
        got = run(luau, out) if dec.returncode == 0 else "decompile failed: " + dec.stderr
        if got != want:
            failures.append(f"{name} -O{level}: {out}")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--luau", required=True, help="the folder with luau and luau-compile")
    ap.add_argument("--fuzz", type=int, default=100, help="how many random programs")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="programs checked at once")
    ap.add_argument("--seed", type=int, default=1, help="the first random program's seed")
    ap.add_argument("--keep", help="where programs and outputs go (default a temporary folder)")
    args = ap.parse_args()
    keep = args.keep or tempfile.mkdtemp(prefix="luau_decompile_check.")
    os.makedirs(keep, exist_ok=True)
    programs = list(CASES.items())
    programs += [(f"fuzz{s}", Gen(s).program()) for s in range(args.seed, args.seed + args.fuzz)]
    failed = []
    ran = 0
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(args.jobs) as pool:
        for result in pool.map(lambda p: check(p[0], p[1], args, keep), programs):
            if result is None:
                continue
            ran += 1
            failed += result
            for f in result:
                print("FAIL", f, flush=True)
    print(f"{ran} programs, {2 * ran - len(failed)} of {2 * ran} round trips the same; kept in {keep}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
