#!/usr/bin/env python3
"""Turns Luau bytecode back into readable Luau.

What a live game sends its client is compiled Luau (a Roblox client's copy
with its opcodes scrambled, as Lunarstone's --dump-scripts writes them); this
rebuilds source from it for reading: if/else and the loops, and/or
conditions, locals and their scopes, closures with what they capture, table
constructors and method calls. Names are gone from what Roblox sends, so
locals are named after what they hold where that's clear (a service, a
child, a module) and v<register> otherwise; each statement ends with the
source line it came from (--@<line>), which is what error messages name.

  tools/luau_decompile.py script.luauc               to stdout
  tools/luau_decompile.py script.luauc -o script.lua
  tools/luau_decompile.py --dir dumped/ decompiled/  every .luauc under a folder
  tools/luau_decompile.py --disasm script.luauc      the instructions instead

A recorded session's scripts, all of them:

  build/release/app/lunarstone --net-replay <session> --dump-scripts <dumped>
  tools/luau_decompile.py --dir <dumped> <decompiled>

tools/luau_decompile_check.py round-trips programs through it to prove it.

Reads bytecode versions 3 to 14 (Luau's own format, Bytecode.h), opcodes
scrambled (op * 227) or not (told apart by how the main function ends).
Where control flow doesn't fit a structure, the output says so in a
comment (--[[decompiler: ...]]) and goes on in order.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import struct
import sys

sys.setrecursionlimit(200000)

# --- opcodes (Bytecode.h, in order) -----------------------------------------

OPS = """NOP BREAK LOADNIL LOADB LOADN LOADK MOVE GETGLOBAL SETGLOBAL GETUPVAL SETUPVAL
CLOSEUPVALS GETIMPORT GETTABLE SETTABLE GETTABLEKS SETTABLEKS GETTABLEN SETTABLEN
NEWCLOSURE NAMECALL CALL RETURN JUMP JUMPBACK JUMPIF JUMPIFNOT JUMPIFEQ JUMPIFLE
JUMPIFLT JUMPIFNOTEQ JUMPIFNOTLE JUMPIFNOTLT ADD SUB MUL DIV MOD POW ADDK SUBK MULK
DIVK MODK POWK AND OR ANDK ORK CONCAT NOT MINUS LENGTH NEWTABLE DUPTABLE SETLIST
FORNPREP FORNLOOP FORGLOOP FORGPREP_INEXT FASTCALL3 FORGPREP_NEXT NATIVECALL
GETVARARGS DUPCLOSURE PREPVARARGS LOADKX JUMPX FASTCALL COVERAGE CAPTURE SUBRK DIVRK
FASTCALL1 FASTCALL2 FASTCALL2K FORGPREP JUMPXEQKNIL JUMPXEQKB JUMPXEQKN JUMPXEQKS
IDIV IDIVK GETUDATAKS SETUDATAKS NAMECALLUDATA NEWCLASSMEMBER CALLFB CMPPROTO
FASTPCALL NEWCLASS""".split()
OP = {name: i for i, name in enumerate(OPS)}
AUX = {
    "GETGLOBAL", "SETGLOBAL", "GETIMPORT", "GETTABLEKS", "SETTABLEKS", "NAMECALL", "JUMPIFEQ", "JUMPIFLE",
    "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT", "NEWTABLE", "SETLIST", "FORGLOOP", "LOADKX",
    "FASTCALL2", "FASTCALL2K", "FASTCALL3", "JUMPXEQKNIL", "JUMPXEQKB", "JUMPXEQKN", "JUMPXEQKS", "GETUDATAKS",
    "SETUDATAKS", "NAMECALLUDATA", "NEWCLASSMEMBER", "CALLFB", "CMPPROTO", "NEWCLASS",
}
COND_JUMPS = {
    "JUMPIF", "JUMPIFNOT", "JUMPIFEQ", "JUMPIFLE", "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT",
    "JUMPXEQKNIL", "JUMPXEQKB", "JUMPXEQKN", "JUMPXEQKS",
}
UNCOND_JUMPS = {"JUMP", "JUMPBACK", "JUMPX"}
FORPREPS = {"FORGPREP", "FORGPREP_INEXT", "FORGPREP_NEXT"}
SKIPPED = {"NOP", "BREAK", "CLOSEUPVALS", "PREPVARARGS", "COVERAGE", "NATIVECALL", "FASTCALL", "FASTCALL1",
           "FASTCALL2", "FASTCALL2K", "FASTCALL3", "FASTPCALL", "CMPPROTO"}

KEYWORDS = set("""and break do else elseif end false for function if in local nil not or repeat return then
true until while continue""".split())


class DecompileError(Exception):
    pass


# --- reading the bytecode ---------------------------------------------------


class Reader:
    def __init__(self, data: bytes):
        self.d = data
        self.o = 0

    def u8(self):
        if self.o >= len(self.d):
            raise DecompileError("bytecode ends early")
        v = self.d[self.o]
        self.o += 1
        return v

    def var(self):
        r = s = 0
        while True:
            b = self.u8()
            r |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                return r

    def fmt(self, f, n):
        v = struct.unpack_from(f, self.d, self.o)[0]
        self.o += n
        return v

    def bytes(self, n):
        v = self.d[self.o:self.o + n]
        self.o += n
        return v


class Proto:
    def __init__(self, index):
        self.index = index
        self.maxstack = self.numparams = self.nups = 0
        self.vararg = False
        self.flags = 0
        self.code: list[int] = []
        self.k: list = []
        self.protos: list[int] = []
        self.linedefined = 0
        self.debugname = None
        self.lines: list[int] | None = None
        self.locvars: list = []
        self.upvalnames: list = []


class Chunk:
    def __init__(self, data: bytes):
        r = Reader(data)
        self.version = r.u8()
        if self.version == 0:
            raise DecompileError("not bytecode: a compile error (" + data[1:].decode("latin1") + ")")
        if not 3 <= self.version <= 14:
            raise DecompileError(f"bytecode version {self.version} isn't one this reads (3 to 14)")
        self.typesversion = r.u8() if self.version >= 4 else 0
        self.strings = []
        for _ in range(r.var()):
            self.strings.append(r.bytes(r.var()))
        if self.typesversion == 3:
            index = r.u8()
            while index:
                r.var()
                index = r.u8()
        self.protos = []
        for i in range(r.var()):
            self.protos.append(self._proto(r, i))
        self.main = r.var()

    def string(self, i):
        return None if i == 0 else self.strings[i - 1]

    def _proto(self, r: Reader, index):
        p = Proto(index)
        size = r.var() if self.version >= 12 else None
        start = r.o
        p.maxstack, p.numparams, p.nups, vararg = r.u8(), r.u8(), r.u8(), r.u8()
        p.vararg = bool(vararg)
        if self.version >= 4:
            p.flags = r.u8()
            if self.typesversion in (1, 2, 3):
                typesize = r.var()
                r.o += typesize
        n = r.var()
        p.code = list(struct.unpack_from(f"<{n}I", r.d, r.o))
        r.o += 4 * n
        for _ in range(r.var()):
            t = r.u8()
            if t == 0:
                p.k.append(("nil", None))
            elif t == 1:
                p.k.append(("bool", bool(r.u8())))
            elif t == 2:
                p.k.append(("num", r.fmt("<d", 8)))
            elif t == 3:
                p.k.append(("str", self.string(r.var())))
            elif t == 4:
                iid = r.fmt("<I", 4)
                names = [p.k[(iid >> (20 - 10 * j)) & 1023][1] for j in range(iid >> 30)]
                p.k.append(("import", names))
            elif t == 5:
                p.k.append(("table", [r.var() for _ in range(r.var())]))
            elif t == 6:
                p.k.append(("closure", r.var()))
            elif t == 7:
                p.k.append(("vector", struct.unpack_from("<4f", r.d, r.o)))
                r.o += 16
            elif t == 8:
                entries = []
                for _ in range(r.var()):
                    key = r.var()
                    entries.append((key, r.fmt("<i", 4)))
                p.k.append(("tablek", entries))
            elif t == 9:
                negative = r.u8()
                v = 0
                s = 0
                while True:
                    b = r.u8()
                    v |= (b & 0x7F) << s
                    s += 7
                    if not b & 0x80:
                        break
                p.k.append(("int", -v if negative else v))
            elif t == 10:
                r.var()
                props, methods = r.var(), r.var()
                for _ in range(props + methods):
                    r.var()
                p.k.append(("class", None))
            elif t == 11:
                p.k.append(("vector", struct.unpack_from("<4d", r.d, r.o)))
                r.o += 32
            else:
                raise DecompileError(f"constant kind {t} isn't known")
        p.protos = [r.var() for _ in range(r.var())]
        p.linedefined = r.var()
        name = self.string(r.var())
        p.debugname = name.decode("latin1") if name else None
        if r.u8():
            gap = r.u8()
            intervals = ((n - 1) >> gap) + 1
            last = 0
            small = []
            for _ in range(n):
                last = (last + r.u8()) & 0xFF
                small.append(last)
            line = 0
            absolute = []
            for _ in range(intervals):
                line += r.fmt("<i", 4)
                absolute.append(line)
            p.lines = [absolute[j >> gap] + small[j] for j in range(n)]
        if r.u8():
            for _ in range(r.var()):
                name = self.string(r.var())
                p.locvars.append((name.decode("latin1") if name else None, r.var(), r.var(), r.u8()))
            for _ in range(r.var()):
                name = self.string(r.var())
                p.upvalnames.append(name.decode("latin1") if name else None)
        if size is not None:
            r.o = start + size
        elif self.version >= 11:
            for _ in range(r.var()):
                r.u8()
                r.var()
        return p


class Insn:
    __slots__ = ("pc", "op", "name", "a", "b", "c", "d", "e", "aux", "size")

    def __init__(self, pc, word, aux, opcode):
        self.pc = pc
        self.op = opcode
        self.name = OPS[opcode] if opcode < len(OPS) else f"OP{opcode}"
        self.a = (word >> 8) & 0xFF
        self.b = (word >> 16) & 0xFF
        self.c = (word >> 24) & 0xFF
        d = word >> 16
        self.d = d - 0x10000 if d >= 0x8000 else d
        e = word >> 8
        self.e = e - 0x1000000 if e >= 0x800000 else e
        self.aux = aux
        self.size = 2 if self.name in AUX else 1


def decode(p: Proto, scrambled: bool) -> list[Insn]:
    out = []
    pc = 0
    code = p.code
    while pc < len(code):
        word = code[pc]
        op = ((word & 0xFF) * 203) & 0xFF if scrambled else word & 0xFF
        if op >= len(OPS):
            raise DecompileError(f"opcode {op} at {pc} isn't known")
        name = OPS[op]
        aux = code[pc + 1] if name in AUX and pc + 1 < len(code) else None
        insn = Insn(pc, word, aux, op)
        out.append(insn)
        pc += insn.size
    return out


def looks_scrambled(chunk: Chunk) -> bool:
    main = chunk.protos[chunk.main]
    last = main.code[-1] & 0xFF if main.code else 0
    return last != OP["RETURN"] and (last * 203) & 0xFF == OP["RETURN"]


# --- expressions ------------------------------------------------------------

PREC = {"or": 1, "and": 2, "<": 3, ">": 3, "<=": 3, ">=": 3, "~=": 3, "==": 3, "..": 4, "+": 5, "-": 5,
        "*": 6, "/": 6, "//": 6, "%": 6, "^": 8}
RIGHT = {"..", "^"}
COMPARE = {"<", ">", "<=", ">=", "~=", "=="}
UNARY = 7


class Expr:
    multi = False  # stands for all the values a call or ... gives

    def regs(self):
        return []

    def impure(self):
        return True


class Const(Expr):
    def __init__(self, kind, value):
        self.kind, self.value = kind, value

    def impure(self):
        return False


class Var(Expr):
    def __init__(self, reg):
        self.reg = reg

    def regs(self):
        return [self]

    def impure(self):
        return False


class Upval(Expr):
    def __init__(self, index):
        self.index = index


class Global(Expr):
    def __init__(self, name):
        self.name = name


class Index(Expr):
    def __init__(self, obj, key):
        self.obj, self.key = obj, key

    def regs(self):
        return self.obj.regs() + self.key.regs()


class MethodRef(Expr):  # what NAMECALL leaves for the CALL after it
    def __init__(self, obj, name):
        self.obj, self.name = obj, name

    def regs(self):
        return self.obj.regs()


class Call(Expr):
    def __init__(self, func, args, nret):
        self.func, self.args = func, args
        self.multi = nret == "multi"

    def regs(self):
        out = self.func.regs()
        for a in self.args:
            out += a.regs()
        return out


class Varargs(Expr):
    def __init__(self, multi):
        self.multi = multi

    def impure(self):
        return False


class Binop(Expr):
    def __init__(self, op, a, b):
        self.op, self.a, self.b = op, a, b

    def regs(self):
        return self.a.regs() + self.b.regs()

    def impure(self):
        return True if self.op not in ("and", "or") else self.a.impure() or self.b.impure()


class Unop(Expr):
    def __init__(self, op, a):
        self.op, self.a = op, a

    def regs(self):
        return self.a.regs()

    def impure(self):
        return self.op != "not" or self.a.impure()


class IfExpr(Expr):
    def __init__(self, cond, a, b):
        self.cond, self.a, self.b = cond, a, b

    def regs(self):
        return self.cond.regs() + self.a.regs() + self.b.regs()

    def impure(self):
        return self.cond.impure() or self.a.impure() or self.b.impure()


class Flag(Expr):  # set when an inlined function returns from inside a loop
    def __init__(self, ident):
        self.ident = ident

    def impure(self):
        return False


class Closure(Expr):
    def __init__(self, proto, captures):
        self.proto, self.captures = proto, captures  # captures: ("reg", r) or ("up", i)

    def regs(self):
        return [Var(r) for kind, r in self.captures if kind == "reg"]

    def impure(self):
        return False


class Table(Expr):
    def __init__(self):
        self.array: list[Expr] = []
        self.hash: list[tuple[Expr, Expr]] = []
        self.closed = False

    def regs(self):
        out = []
        for v in self.array:
            out += v.regs()
        for k, v in self.hash:
            out += k.regs() + v.regs()
        return out

    def impure(self):
        return any(v.impure() for v in self.array) or any(k.impure() or v.impure() for k, v in self.hash)


def is_boolean(e):
    if isinstance(e, Binop):
        return e.op in COMPARE or (e.op in ("and", "or") and is_boolean(e.a) and is_boolean(e.b))
    if isinstance(e, Unop):
        return e.op == "not"
    return isinstance(e, Const) and e.kind == "bool"


def neg(e):
    """The condition that holds when `e` doesn't (truthiness kept, so De Morgan holds)."""
    if isinstance(e, Unop) and e.op == "not":
        return e.a
    if isinstance(e, Binop):
        if e.op == "==":
            return Binop("~=", e.a, e.b)
        if e.op == "~=":
            return Binop("==", e.a, e.b)
        if e.op == "and":
            return Binop("or", neg(e.a), neg(e.b))
        if e.op == "or":
            return Binop("and", neg(e.a), neg(e.b))
    if isinstance(e, Const) and e.kind == "bool":
        return Const("bool", not e.value)
    return Unop("not", e)


def same(a, b):
    if type(a) is not type(b):
        return False
    if isinstance(a, Var):
        return a.reg == b.reg
    if isinstance(a, Const):
        return a.kind == b.kind and a.value == b.value
    if isinstance(a, (Global,)):
        return a.name == b.name
    if isinstance(a, Upval):
        return a.index == b.index
    if isinstance(a, Unop):
        return a.op == b.op and same(a.a, b.a)
    if isinstance(a, Binop):
        return a.op == b.op and same(a.a, b.a) and same(a.b, b.b)
    if isinstance(a, Index):
        return same(a.obj, b.obj) and same(a.key, b.key)
    return a is b


def cond_or(a, b):
    if isinstance(b, Const) and b.kind == "bool":
        return Const("bool", True) if b.value else a  # truthiness: x or false is x
    return Binop("or", a, b)


def cond_and(a, b):
    if isinstance(b, Const) and b.kind == "bool":
        return a if b.value else Const("bool", False)
    return Binop("and", a, b)


def cond_if(c, k, other):
    """if c then k else other, as a condition."""
    if isinstance(other, Binop) and other.op == "or" and same(other.b, k):
        return Binop("or", cond_and(neg(c), other.a), k)
    if isinstance(other, Binop) and other.op == "and" and same(other.b, k):
        return Binop("and", cond_or(c, other.a), k)
    if same(k, other):
        return k
    if isinstance(k, Const) and k.kind == "bool":
        return cond_or(c, other) if k.value else cond_and(neg(c), other)
    return IfExpr(c, k, other)


# --- statements -------------------------------------------------------------


class Stmt:
    pc = -1
    line = None

    def host_exprs(self):
        """The expressions this statement evaluates itself (not its blocks')."""
        return []

    def defs(self):
        return []


class Assign(Stmt):
    def __init__(self, targets, values):
        self.targets, self.values = targets, values

    def host_exprs(self):
        out = list(self.values)
        for t in self.targets:
            if isinstance(t, Index):
                out += [t.obj, t.key]
        return out

    def defs(self):
        return [t.reg for t in self.targets if isinstance(t, Var)]


class CallStmt(Stmt):
    def __init__(self, call):
        self.call = call

    def host_exprs(self):
        return [self.call]


class Return(Stmt):
    def __init__(self, values):
        self.values = values

    def host_exprs(self):
        return list(self.values)


class SetList(Stmt):
    def __init__(self, table, values, start):
        self.table, self.values, self.start = table, values, start

    def host_exprs(self):
        return [self.table] + list(self.values)


class If(Stmt):
    def __init__(self, cond, then, other=None):
        self.cond, self.then, self.other = cond, then, other

    def host_exprs(self):
        return [self.cond]


class While(Stmt):
    def __init__(self, cond, body):
        self.cond, self.body = cond, body


class Repeat(Stmt):
    def __init__(self, body, cond):
        self.body, self.cond = body, cond


class Until(Stmt):  # the end of a repeat's body while it's being folded: hosts the condition
    def __init__(self, cond):
        self.cond = cond

    def host_exprs(self):
        return [self.cond]


class NumFor(Stmt):
    def __init__(self, base, body):
        self.base, self.body = base, body
        self.start = Var(base + 2)
        self.limit = Var(base)
        self.step = Var(base + 1)

    def host_exprs(self):
        return [self.start, self.limit, self.step]


class GenFor(Stmt):
    def __init__(self, base, nvars, body):
        self.base, self.nvars, self.body = base, nvars, body
        self.exprs = [Var(base), Var(base + 1), Var(base + 2)]

    def host_exprs(self):
        return list(self.exprs)


class Break(Stmt):
    pass


class Continue(Stmt):
    pass


class Comment(Stmt):
    def __init__(self, text):
        self.text = text


def walk_exprs(e, fn):
    fn(e)
    if isinstance(e, Index):
        walk_exprs(e.obj, fn)
        walk_exprs(e.key, fn)
    elif isinstance(e, MethodRef):
        walk_exprs(e.obj, fn)
    elif isinstance(e, Call):
        walk_exprs(e.func, fn)
        for a in e.args:
            walk_exprs(a, fn)
    elif isinstance(e, Binop):
        walk_exprs(e.a, fn)
        walk_exprs(e.b, fn)
    elif isinstance(e, Unop):
        walk_exprs(e.a, fn)
    elif isinstance(e, IfExpr):
        walk_exprs(e.cond, fn)
        walk_exprs(e.a, fn)
        walk_exprs(e.b, fn)
    elif isinstance(e, Table):
        for v in e.array:
            walk_exprs(v, fn)
        for k, v in e.hash:
            walk_exprs(k, fn)
            walk_exprs(v, fn)


def replace_var(e, reg, new):
    """`e` with its one read of register `reg` replaced by `new`; (expr, replaced)."""
    done = [False]

    def go(x):
        if done[0]:
            return x
        if isinstance(x, Var):
            if x.reg == reg:
                done[0] = True
                return new
            return x
        if isinstance(x, Index):
            x.obj = go(x.obj)
            x.key = go(x.key)
        elif isinstance(x, MethodRef):
            x.obj = go(x.obj)
        elif isinstance(x, Call):
            x.func = go(x.func)
            x.args = [go(a) for a in x.args]
        elif isinstance(x, Binop):
            x.a = go(x.a)
            x.b = go(x.b)
        elif isinstance(x, Unop):
            x.a = go(x.a)
        elif isinstance(x, IfExpr):
            x.cond = go(x.cond)
            x.a = go(x.a)
            x.b = go(x.b)
        elif isinstance(x, Table):
            x.array = [go(v) for v in x.array]
            x.hash = [(go(k), go(v)) for k, v in x.hash]
        return x

    return go(e), done[0]


# --- one function -----------------------------------------------------------


class Function:
    def __init__(self, dec: "Decompiler", proto: Proto):
        self.dec = dec
        self.p = proto
        self.insns = decode(proto, dec.scrambled)
        self.at = {i.pc: i for i in self.insns}
        self.order = [i.pc for i in self.insns]
        self.pos = {pc: k for k, pc in enumerate(self.order)}
        self.next = {}
        for a, b in zip(self.order, self.order[1:] + [len(proto.code)]):
            self.next[a] = b
        self.flags = []  # inline wrappers' flags
        self.captured_ref = set()
        self.lifted = {}
        self.liveness()
        self.find_loops()
        # what the entry can reach: the compiler leaves dead jumps behind when it threads others
        self.reachable = set()
        work = [self.order[0]] if self.order else []
        while work:
            pc = work.pop()
            if pc in self.reachable or pc not in self.at:
                continue
            self.reachable.add(pc)
            work += self.successors(self.at[pc])
        self.body = None

    # -- constants

    def k(self, i):
        kind, v = self.p.k[i]
        if kind == "import":
            e = Global(v[0].decode("latin1"))
            for name in v[1:]:
                e = Index(e, Const("str", name))
            return e
        if kind == "closure":
            return Closure(v, [])
        return Const(kind, v)

    def kstr(self, i):
        kind, v = self.p.k[i]
        return v.decode("latin1") if kind == "str" else str(v)

    def target(self, i: Insn):
        if i.name in ("JUMPX",):
            return i.pc + 1 + i.e
        if i.name == "LOADB":
            return i.pc + 1 + i.c
        if i.name in ("FASTCALL", "FASTCALL1", "FASTCALL2", "FASTCALL2K", "FASTCALL3", "FASTPCALL"):
            return i.pc + 1 + i.c
        return i.pc + 1 + i.d

    # -- register reads and writes, for liveness

    def multret_base(self, pc):
        """The register a multiple-value producer before `pc` starts at."""
        j = self.pos.get(pc, 0)
        for back in range(j - 1, max(j - 8, -1), -1):
            i = self.at[self.order[back]]
            if i.name in ("CALL", "CALLFB") and i.c == 0:
                return i.a
            if i.name == "GETVARARGS" and i.b == 0:
                return i.a
        return None

    def rw(self, i: Insn):
        n, a, b, c = i.name, i.a, i.b, i.c
        R = set()
        W = set()
        if n in ("LOADNIL", "LOADB", "LOADN", "LOADK", "LOADKX", "GETGLOBAL", "GETUPVAL", "GETIMPORT",
                 "NEWTABLE", "DUPTABLE", "NEWCLOSURE", "DUPCLOSURE"):
            W = {a}
        elif n == "MOVE":
            W, R = {a}, {b}
        elif n in ("SETGLOBAL", "SETUPVAL"):
            R = {a}
        elif n == "GETTABLE":
            W, R = {a}, {b, c}
        elif n == "SETTABLE":
            R = {a, b, c}
        elif n in ("GETTABLEKS", "GETTABLEN", "GETUDATAKS"):
            W, R = {a}, {b}
        elif n in ("SETTABLEKS", "SETTABLEN", "SETUDATAKS"):
            R = {a, b}
        elif n in ("NAMECALL", "NAMECALLUDATA"):
            W, R = {a, a + 1}, {b}
        elif n in ("CALL", "CALLFB"):
            if b == 0:
                base = self.multret_base(i.pc)
                R = set(range(a, (base if base is not None else a) + 1))
            else:
                R = set(range(a, a + b))
            W = {a} if c == 0 else set(range(a, a + c - 1))
        elif n == "RETURN":
            if b == 0:
                base = self.multret_base(i.pc)
                R = set(range(a, (base if base is not None else a) + 1))
            else:
                R = set(range(a, a + b - 1))
        elif n in ("JUMPIF", "JUMPIFNOT", "JUMPXEQKNIL", "JUMPXEQKB", "JUMPXEQKN", "JUMPXEQKS", "CMPPROTO"):
            R = {a}
        elif n in ("JUMPIFEQ", "JUMPIFLE", "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT"):
            R = {a, i.aux}
        elif n in ("ADD", "SUB", "MUL", "DIV", "MOD", "POW", "AND", "OR", "IDIV"):
            W, R = {a}, {b, c}
        elif n in ("ADDK", "SUBK", "MULK", "DIVK", "MODK", "POWK", "ANDK", "ORK", "IDIVK", "NOT", "MINUS",
                   "LENGTH"):
            W, R = {a}, {b}
        elif n in ("SUBRK", "DIVRK"):
            W, R = {a}, {c}
        elif n == "CONCAT":
            W, R = {a}, set(range(b, c + 1))
        elif n == "SETLIST":
            if c == 0:
                base = self.multret_base(i.pc)
                R = {a} | set(range(b, (base if base is not None else b) + 1))
            else:
                R = {a} | set(range(b, b + c - 1))
        elif n == "FORNPREP":
            R = {a, a + 1, a + 2}
        elif n == "FORNLOOP":
            R, W = {a, a + 1, a + 2}, {a + 2}
        elif n in FORPREPS:
            R = {a, a + 1, a + 2}
        elif n == "FORGLOOP":
            R = {a, a + 1, a + 2}
            W = set(range(a + 2, a + 3 + (i.aux & 0xFF)))
        elif n == "GETVARARGS":
            W = {a} if b == 0 else set(range(a, a + b - 1))
        elif n == "CAPTURE":
            if a in (0, 1):
                R = {b}
        elif n == "NEWCLASS":
            W = {a}
            if b != 0xFF:
                R = {b}
        elif n == "NEWCLASSMEMBER":
            R = {a, c}
        return R, W

    def successors(self, i: Insn):
        n = i.name
        nxt = self.next[i.pc]
        if n == "RETURN":
            return []
        if n in UNCOND_JUMPS:
            return [self.target(i)]
        if n == "LOADB" and i.c:
            return [self.target(i)]
        if n in COND_JUMPS or n in ("FORNPREP", "CMPPROTO"):
            return [nxt, self.target(i)]
        if n == "FORNLOOP":
            return [nxt, self.target(i)]
        if n in FORPREPS:
            return [self.target(i)]
        if n == "FORGLOOP":
            return [nxt, self.target(i)]
        return [nxt]

    def liveness(self):
        """live_out[pc]: the registers some later instruction reads before writing."""
        reads, writes, succ = {}, {}, {}
        for i in self.insns:
            R, W = self.rw(i)
            reads[i.pc] = sum(1 << r for r in R)
            writes[i.pc] = sum(1 << r for r in W)
            succ[i.pc] = [s for s in self.successors(i) if s in self.at]
        preds = {pc: [] for pc in self.order}
        for pc, ss in succ.items():
            for s in ss:
                preds[s].append(pc)
        live_in = {pc: 0 for pc in self.order}
        live_out = {pc: 0 for pc in self.order}
        work = list(self.order)
        queued = set(work)
        while work:
            pc = work.pop()
            queued.discard(pc)
            out = 0
            for s in succ[pc]:
                out |= live_in[s]
            live_out[pc] = out
            new_in = reads[pc] | (out & ~writes[pc])
            if new_in != live_in[pc]:
                live_in[pc] = new_in
                for q in preds[pc]:
                    if q not in queued:
                        queued.add(q)
                        work.append(q)
        self.live_out = live_out
        self.live_in = live_in

    def dead_after(self, reg, pc):
        if pc is None or pc not in self.live_out:
            return False
        return not (self.live_out[pc] >> reg) & 1

    # -- loops

    def find_loops(self):
        # numeric fors by their FORNLOOP (FORNPREP's own jump may be threaded on past the loop)
        self.fornloop = {}
        for i in self.insns:
            if i.name == "FORNLOOP":
                body = self.target(i)
                k = self.pos.get(body, 0) - 1
                if k >= 0 and self.at[self.order[k]].name == "FORNPREP":
                    self.fornloop[self.order[k]] = i.pc
        self.loops = {}  # header pc -> loop
        back = {}
        for i in self.insns:
            if i.name in UNCOND_JUMPS or i.name in COND_JUMPS:
                t = self.target(i)
                if t <= i.pc:
                    back.setdefault(t, []).append(i)
        for h, jumps in back.items():
            last = max(jumps, key=lambda i: i.pc)
            exit_pc = self.next[last.pc]
            kind = "repeat" if last.name in COND_JUMPS else "while"
            self.loops[h] = {"kind": kind, "head": h, "last": last, "exit": exit_pc,
                             "continue": h if kind == "while" else last.pc}

    # -- lifting one instruction

    def lift(self, i: Insn):
        """The instruction's statements, fresh each time (folding changes them; code a structure
        covers twice, or builds again, must start from the instruction's own)."""
        if i.pc not in self.lifted:
            out = self._lift(i)
            for s in out:
                s.pc = i.pc
                s.use_pc = i.pc
                s.line = self.p.lines[i.pc] if self.p.lines else None
            self.lifted[i.pc] = out
        return [self.copy_stmt(s) for s in self.lifted[i.pc]]

    def _lift(self, i: Insn):
        n, a, b, c, d = i.name, i.a, i.b, i.c, i.d
        V = Var

        def assign(e, reg=a):
            return [Assign([V(reg)], [e])]

        if n == "LOADNIL":
            return assign(Const("nil", None))
        if n == "LOADB":
            return assign(Const("bool", bool(b)))
        if n == "LOADN":
            return assign(Const("num", float(d)))
        if n == "LOADK":
            return assign(self.k(d))
        if n == "LOADKX":
            return assign(self.k(i.aux))
        if n == "MOVE":
            return assign(V(b))
        if n == "GETGLOBAL":
            return assign(Global(self.kstr(i.aux)))
        if n == "SETGLOBAL":
            return [Assign([Global(self.kstr(i.aux))], [V(a)])]
        if n == "GETUPVAL":
            return assign(Upval(b))
        if n == "SETUPVAL":
            return [Assign([Upval(b)], [V(a)])]
        if n == "GETIMPORT":
            return assign(self.k(d))
        if n == "GETTABLE":
            return assign(Index(V(b), V(c)))
        if n == "SETTABLE":
            return [Assign([Index(V(b), V(c))], [V(a)])]
        if n in ("GETTABLEKS", "GETUDATAKS"):
            key = i.aux & 0xFFFF if n == "GETUDATAKS" else i.aux
            return assign(Index(V(b), self.k(key)))
        if n in ("SETTABLEKS", "SETUDATAKS"):
            key = i.aux & 0xFFFF if n == "SETUDATAKS" else i.aux
            return [Assign([Index(V(b), self.k(key))], [V(a)])]
        if n == "GETTABLEN":
            return assign(Index(V(b), Const("num", float(c + 1))))
        if n == "SETTABLEN":
            return [Assign([Index(V(b), Const("num", float(c + 1)))], [V(a)])]
        if n in ("NEWCLOSURE", "DUPCLOSURE"):
            proto = self.p.protos[d] if n == "NEWCLOSURE" else self.p.k[d][1]
            captures = []
            j = self.pos[i.pc] + 1
            while j < len(self.order) and self.at[self.order[j]].name == "CAPTURE":
                cap = self.at[self.order[j]]
                if cap.a == 2:
                    captures.append(("up", cap.b))
                else:
                    captures.append(("reg", cap.b))
                    if cap.a == 1:
                        self.captured_ref.add(cap.b)
                j += 1
            return assign(Closure(proto, captures))
        if n in ("NAMECALL", "NAMECALLUDATA"):
            key = i.aux & 0xFFFF if n == "NAMECALLUDATA" else i.aux
            s = Assign([V(a)], [MethodRef(V(b), self.kstr(key))])
            s.extra_defs = [a + 1]
            return [s]
        if n in ("CALL", "CALLFB"):
            method = self.is_method_call(i)
            first = a + 2 if method else a + 1
            if b == 0:
                base = self.multret_base(i.pc)
                base = first if base is None else base
                args = [V(r) for r in range(first, base)]
                last = V(base)
                last.multi = True
                args.append(last)
            else:
                args = [V(r) for r in range(first, a + b)]
            nret = "multi" if c == 0 else c - 1
            call = Call(V(a), args, nret)
            if c == 1:
                return [CallStmt(call)]
            if c == 0 or c == 2:
                return [Assign([V(a)], [call])]
            return [Assign([V(r) for r in range(a, a + c - 1)], [call])]
        if n == "RETURN":
            if b == 0:
                base = self.multret_base(i.pc)
                base = a if base is None else base
                vals = [V(r) for r in range(a, base)]
                last = V(base)
                last.multi = True
                vals.append(last)
            else:
                vals = [V(r) for r in range(a, a + b - 1)]
            return [Return(vals)]
        if n in ("ADD", "SUB", "MUL", "DIV", "MOD", "POW", "IDIV"):
            op = {"ADD": "+", "SUB": "-", "MUL": "*", "DIV": "/", "MOD": "%", "POW": "^", "IDIV": "//"}[n]
            return assign(Binop(op, V(b), V(c)))
        if n in ("ADDK", "SUBK", "MULK", "DIVK", "MODK", "POWK", "IDIVK"):
            op = {"ADDK": "+", "SUBK": "-", "MULK": "*", "DIVK": "/", "MODK": "%", "POWK": "^", "IDIVK": "//"}[n]
            return assign(Binop(op, V(b), self.k(c)))
        if n == "SUBRK":
            return assign(Binop("-", self.k(b), V(c)))
        if n == "DIVRK":
            return assign(Binop("/", self.k(b), V(c)))
        if n in ("AND", "OR"):
            return assign(Binop(n.lower(), V(b), V(c)))
        if n in ("ANDK", "ORK"):
            return assign(Binop(n[:-1].lower(), V(b), self.k(c)))
        if n == "CONCAT":
            e = V(c)
            for r in range(c - 1, b - 1, -1):
                e = Binop("..", V(r), e)
            return assign(e)
        if n == "NOT":
            return assign(Unop("not", V(b)))
        if n == "MINUS":
            return assign(Unop("-", V(b)))
        if n == "LENGTH":
            return assign(Unop("#", V(b)))
        if n == "NEWTABLE":
            return assign(Table())
        if n == "DUPTABLE":
            t = Table()
            kind, v = self.p.k[d]
            if kind == "tablek":  # a template with values already in it
                for key, value in v:
                    if value >= 0:
                        t.hash.append((self.k(key), self.k(value)))
            return assign(t)
        if n == "SETLIST":
            if c == 0:
                base = self.multret_base(i.pc)
                base = b if base is None else base
                vals = [V(r) for r in range(b, base)]
                last = V(base)
                last.multi = True
                vals.append(last)
            else:
                vals = [V(r) for r in range(b, b + c - 1)]
            return [SetList(V(a), vals, i.aux)]
        if n == "GETVARARGS":
            if b == 0:
                return assign(Varargs(True))
            if b == 1:
                return []
            if b == 2:
                return assign(Varargs(False))
            return [Assign([V(r) for r in range(a, a + b - 1)], [Varargs(True)])]
        if n == "NEWCLASS":
            return [Comment("decompiler: NEWCLASS isn't decompiled")]
        if n == "NEWCLASSMEMBER":
            return [Comment("decompiler: NEWCLASSMEMBER isn't decompiled")]
        return []

    def is_method_call(self, call: Insn):
        j = self.pos[call.pc] - 1
        while j >= 0:
            i = self.at[self.order[j]]
            R, W = self.rw(i)
            if call.a in W:
                return i.name in ("NAMECALL", "NAMECALLUDATA")
            if i.name in UNCOND_JUMPS or i.name in COND_JUMPS or i.name == "RETURN":
                return False
            j -= 1
        return False

    def jump_cond(self, i: Insn):
        """The condition under which the conditional jump is taken."""
        n, a = i.name, i.a
        A = Var(a)
        if n == "JUMPIF":
            return A
        if n == "JUMPIFNOT":
            return Unop("not", A)
        if n in ("JUMPIFEQ", "JUMPIFLE", "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT"):
            B = Var(i.aux)
            if n == "JUMPIFEQ":
                return Binop("==", A, B)
            if n == "JUMPIFNOTEQ":
                return Binop("~=", A, B)
            if n == "JUMPIFLE":
                return Binop("<=", A, B)
            if n == "JUMPIFLT":
                return Binop("<", A, B)
            if n == "JUMPIFNOTLE":
                return Unop("not", Binop("<=", A, B))
            return Unop("not", Binop("<", A, B))
        flip = i.aux >> 31
        if n == "JUMPXEQKNIL":
            k = Const("nil", None)
        elif n == "JUMPXEQKB":
            k = Const("bool", bool(i.aux & 1))
        else:
            k = self.k(i.aux & 0xFFFFFF)
        return Binop("~=" if flip else "==", A, k)

    # -- structure
    #
    # The compiler lays code out in source order, so structure is read off pc
    # ranges: block(lo, hi) is the statements from lo up to hi. `ends` are the
    # other jump targets that mean "the end of this block": the compiler
    # threads a jump to the end of a then-part on to where the whole if ends,
    # so inside a then-part a jump there is just its end. Loop contexts carry
    # what break and continue jump to. A jump to a return outside the block
    # is that return again (the compiler copies returns into jumps' places
    # and the other way round).

    def decompile(self):
        if self.body is None:
            self.body = self.block(0, len(self.p.code), [], None, frozenset())
        return self.body

    def tail_return(self, t, lo, hi):
        i = self.at.get(t)
        if i is None or i.name != "RETURN" or lo <= t < hi or i.b == 0:
            return None
        return [self.copy_stmt(s) for s in self.lift(i)]

    def threads(self, t):
        """`t` and where unconditional jumps from it lead: one place, as far as control goes."""
        out = {t}
        while t in self.at and self.at[t].name in ("JUMP", "JUMPX") and len(out) < 64:
            t = self.target(self.at[t])
            out.add(t)
        return out

    def loop_jump(self, t, loops, pc):
        """break or continue for a jump to `t`, if it's one of the innermost loop's; out of loops to
        an inline wrapper (an inlined function returning from a loop): set its flag and break."""
        ctx = loops[-1] if loops else None
        if ctx and t in ctx["break"]:
            return [self.mark(Break(), pc)]
        if ctx and t in ctx["continue"]:
            return [self.mark(Continue(), pc)]
        for k in range(len(loops) - 2, -1, -1):
            outer = loops[k]
            if outer.get("wrapper") and t in outer["break"]:
                for inner in loops[k + 1:]:
                    inner["raise"] = outer["flag"]
                outer["flag_used"] = True
                setflag = Assign([outer["flag"]], [Const("bool", True)])
                return [self.mark(setflag, pc), self.mark(Break(), pc)]
            if outer.get("wrapper"):
                break
        return None

    def loop_targets(self, loops):
        """Every jump target loop_jump turns into a break or continue."""
        if not loops:
            return set()
        out = loops[-1]["break"] | loops[-1]["continue"]
        for k in range(len(loops) - 2, -1, -1):
            if loops[k].get("wrapper"):
                out |= loops[k]["break"]
                break
        return out

    def after_loop(self, stmts, ctx):
        """After a loop an inlined function's return left from: on out of the next loop too."""
        if ctx.get("raise") is not None:
            check = If(ctx["raise"], [self.mark(Break(), stmts[-1].pc)])
            stmts.append(self.mark_if(check, stmts[-1].pc))

    def block(self, lo, hi, loops, skip_loop, ends, tails=None):
        tails = tails or {}
        stmts = []
        pc = lo
        while pc < hi:
            if pc not in self.at:
                pc += 1
                continue
            if pc not in self.reachable:
                pc = self.next[pc]
                continue
            i = self.at[pc]
            loop = self.loops.get(pc)
            if loop and pc != skip_loop and loop["last"].pc < hi:
                ctx = {}
                stmts.append(self.loop(loop, loops, ctx))
                self.after_loop(stmts, ctx)
                pc = loop["exit"]
                continue
            n = i.name
            if n == "FORNPREP" and pc in self.fornloop and self.fornloop[pc] < hi:
                loop_pc = self.fornloop[pc]
                exit_pc = self.next[loop_pc]
                ctx = {"break": self.threads(exit_pc) | self.threads(self.target(i)), "continue": {loop_pc}}
                body = self.block(self.next[pc], loop_pc, loops + [ctx], None, frozenset())
                stmts.append(self.mark(NumFor(i.a, body), pc))
                self.after_loop(stmts, ctx)
                pc = exit_pc
                continue
            if n in FORPREPS and self.at.get(self.target(i)) is not None and self.next[self.target(i)] <= hi:
                loop_pc = self.target(i)
                li = self.at.get(loop_pc)
                if li is None or li.name != "FORGLOOP":
                    stmts.append(self.mark(Comment(f"decompiler: {n} at {pc} without its FORGLOOP"), pc))
                    pc = self.next[pc]
                    continue
                exit_pc = self.next[loop_pc]
                ctx = {"break": self.threads(exit_pc), "continue": {loop_pc}}
                body = self.block(self.next[pc], loop_pc, loops + [ctx], None, frozenset())
                stmts.append(self.mark(GenFor(i.a, li.aux & 0xFF, body), pc))
                self.after_loop(stmts, ctx)
                pc = exit_pc
                continue
            if n in COND_JUMPS:
                pc = self.condition(i, lo, hi, loops, stmts, ends, tails)
                continue
            if n in UNCOND_JUMPS or (n == "LOADB" and i.c):
                if n == "LOADB":
                    stmts += self.lift(i)
                t = self.target(i)
                if t == hi or t in ends or t == self.next[pc]:
                    pass
                elif (lj := self.loop_jump(t, loops, pc)) is not None:
                    stmts += lj
                elif (ret := self.tail_return(t, lo, hi)) is not None:
                    stmts += ret
                elif t in tails:
                    stmts += self.copy_stmt(tails[t])
                else:
                    stmts.append(self.mark(Comment(f"decompiler: goto {t}"), pc))
                pc = self.next[pc]
                continue
            if n in ("FORNLOOP", "FORGLOOP"):
                stmts.append(self.mark(Comment(f"decompiler: stray {n} at {pc}"), pc))
                pc = self.next[pc]
                continue
            if n == "CAPTURE" or n in SKIPPED:
                pc = self.next[pc]
                continue
            stmts += self.lift(i)
            pc = self.next[pc]
        stmts = self.wrap_exits(stmts, lo, hi, loops, ends, tails)
        return self.tidy(stmts)

    def unresolved(self, stmts, out):
        """The targets of the gotos left in `stmts` (not inside loops: a break there means the loop)."""
        for s in stmts:
            if isinstance(s, Comment):
                m = re.match(r"decompiler: (?:goto|unstructured jump to) (\d+)$", s.text)
                if m:
                    out.add(int(m.group(1)))
            elif isinstance(s, If):
                self.unresolved(s.then, out)
                if s.other is not None:
                    self.unresolved(s.other, out)
            elif isinstance(s, (While, Repeat, NumFor, GenFor)):
                self.unresolved(s.body, out)
        return out

    def wrap_exits(self, stmts, lo, hi, loops, ends, tails):
        """An inlined function's returns jump past the rest of it: repeat ... until true, break for them."""
        for _ in range(8):
            changed = False
            for k, s in enumerate(stmts):
                targets = self.unresolved([s], set())
                for T in sorted(targets):
                    if not s.pc < T <= hi:
                        continue
                    j = next((n for n in range(k + 1, len(stmts)) if stmts[n].pc >= T), len(stmts))
                    if T != hi and (j == len(stmts) or not any(x.pc >= T for x in stmts[k + 1:])):
                        continue
                    start = s.pc
                    if start < lo or self.leaves_loop(start, T, loops):
                        continue
                    wrapper = {"break": self.threads(T), "continue": set(), "wrapper": True,
                               "flag": Flag(len(self.flags))}
                    body = self.block(start, T, loops + [wrapper], None, frozenset())
                    rep = Repeat(body, Const("bool", True))
                    rep.inlined = True
                    added = [self.mark(rep, start)]
                    if wrapper.get("flag_used"):
                        self.flags.append(wrapper["flag"])
                        init = Assign([wrapper["flag"]], [Const("bool", False)])
                        init.declares_flag = True
                        added.insert(0, self.mark(init, start))
                    stmts = stmts[:k] + added + stmts[j:]
                    changed = True
                    break
                if changed:
                    break
            if not changed:
                break
        return stmts

    def leaves_loop(self, lo, hi, loops):
        """Whether some jump in [lo, hi) is the enclosing loop's break or continue (a wrapper would take it)."""
        ctx = loops[-1] if loops else None
        if not ctx:
            return False
        targets = ctx["break"] | ctx["continue"]
        k = self.pos.get(lo, 0)
        while k < len(self.order) and self.order[k] < hi:
            i = self.at[self.order[k]]
            if (i.name in UNCOND_JUMPS or i.name in COND_JUMPS or (i.name == "LOADB" and i.c)) \
                    and self.target(i) in targets:
                return True
            k += 1
        return False

    def mark(self, s, pc):
        s.pc = s.use_pc = pc
        s.line = self.line(pc)
        return s

    def line(self, pc):
        return self.p.lines[pc] if self.p.lines and pc < len(self.p.lines) else None

    def loop(self, loop, loops, ctx):
        h, last, exit_pc = loop["head"], loop["last"], loop["exit"]
        if loop["kind"] == "while":
            ctx.update({"break": self.threads(exit_pc), "continue": {h, last.pc}})
            body = self.block(h, last.pc, loops + [ctx], h, frozenset())
            cond = Const("bool", True)
            if body and isinstance(body[0], If) and body[0].other is None and len(body[0].then) == 1 \
                    and isinstance(body[0].then[0], Break):
                cond = neg(body[0].cond)
                body = body[1:]
            s = While(cond, body)
        else:
            ctx.update({"break": self.threads(exit_pc), "continue": {last.pc}})
            body = self.block(h, last.pc, loops + [ctx], h, frozenset())
            until = Until(neg(self.jump_cond(last)))
            until.pc = until.use_pc = last.pc
            body = self.tidy(body + [until])
            cond = Const("bool", True)
            if body and isinstance(body[-1], Until):
                cond = body.pop().cond
            s = Repeat(body, cond)
        return self.mark(s, h)

    def segment(self, lo, jump: Insn):
        """The statements from lo up to the conditional jump, folded into its condition; None if some stay."""
        stmts = []
        pc = lo
        while pc < jump.pc:
            i = self.at[pc]
            if i.name in COND_JUMPS or i.name in UNCOND_JUMPS or i.name in ("RETURN", "FORNPREP", "FORNLOOP",
                                                                             "FORGLOOP") or i.name in FORPREPS:
                return None
            if (i.name == "LOADB" and i.c) or pc in self.loops:
                return None
            if i.name != "CAPTURE" and i.name not in SKIPPED:
                stmts += [self.copy_stmt(s) for s in self.lift(i)]
            pc = self.next[pc]
        host = Until(self.jump_cond(jump))
        host.pc = host.use_pc = jump.pc
        folded = self.fold(stmts + [host])
        if len(folded) != 1:
            return None
        return folded[0].cond

    def copy_stmt(self, s):
        import copy
        return copy.deepcopy(s)

    def condition(self, first: Insn, lo, hi, loops, stmts, ends, tails):
        """A chain of conditional jumps from `first` (and/or): an If; returns where to go on."""
        segs = [(first.pc, self.jump_cond(first), self.target(first))]
        ends_at = [self.next[first.pc]]
        folded = [None]
        pc = ends_at[-1]
        while pc < hi:
            # the next conditional jump, with only computations to fold into it before
            j = pc
            while j < hi and j in self.at and j not in self.loops:
                name = self.at[j].name
                if name in COND_JUMPS or name in UNCOND_JUMPS or name in ("RETURN", "FORNPREP", "FORNLOOP",
                                                                         "FORGLOOP") or name in FORPREPS:
                    break
                j = self.next[j]
            if j >= hi or j not in self.at or self.at[j].name not in COND_JUMPS or j in self.loops:
                break
            jump = self.at[j]
            if self.target(jump) <= jump.pc:
                break
            cond = self.segment(pc, jump)
            if cond is None:
                break
            segs.append((pc, None, self.target(jump)))
            folded.append(cond)
            ends_at.append(self.next[j])
            pc = ends_at[-1]
        loop_targets = self.loop_targets(loops)

        def canon(t):
            return hi if t in ends else t

        for n in range(len(segs), 0, -1):
            N = ends_at[n - 1]
            starts = [s for s, _, _ in segs[:n]]
            external = set()
            for idx in range(n):
                t = canon(segs[idx][2])
                if t == N or t in starts[idx + 1:n]:
                    continue
                external.add(t)
            if not external:
                external = {N}
            if len(external) != 1:
                continue
            F = external.pop()
            usable = (starts[0] < F <= hi) or F in loop_targets or F in tails or self.at.get(F) is not None \
                and self.at[F].name == "RETURN" and not lo <= F < hi
            if not usable:
                continue

            memo = {}

            def value(t):
                t = canon(t)
                if t == F:
                    return Const("bool", True)
                if t == N:
                    return Const("bool", False)
                return G(starts.index(t))

            def G(idx):
                if idx not in memo:
                    cond = segs[idx][1] if idx == 0 else folded[idx]
                    nxt = value(starts[idx + 1]) if idx + 1 < n else Const("bool", False)
                    memo[idx] = cond_if(cond, value(segs[idx][2]), nxt)
                return memo[idx]

            return self.make_if(G(0), F, N, lo, hi, loops, stmts, first.pc, ends, tails)
        stmts.append(self.mark(Comment(f"decompiler: unstructured jump to {self.target(first)}"), first.pc))
        return self.next[first.pc]

    def make_if(self, E, F, N, lo, hi, loops, stmts, pc0, ends, tails):
        loop_t = self.loop_targets(loops)

        def canon(t):
            return hi if t in ends else t

        def sub_ends(end, raw=None):
            out = set(self.threads(end))
            if raw is not None:
                out |= self.threads(raw)
            if end == hi:
                out |= ends
            return frozenset(out)

        def sub_tails(end):
            return tails if end == hi else {}

        if F in tails and not N < F <= hi:
            # a jump into the else-part of the if this block is the then-part of: that else-part,
            # and the rest of this block otherwise
            rest = self.block(N, hi, loops, None, ends, tails)
            stmts.append(self.mark_if(If(E, self.copy_stmt(tails[F]), rest), pc0))
            return hi
        if not N < F <= hi:
            if F == N:
                return N
            if (lj := self.loop_jump(F, loops, pc0)) is not None:
                stmts.append(self.mark_if(If(E, lj), pc0))
                return N
            if (ret := self.tail_return(F, lo, hi)) is not None:
                stmts.append(self.mark_if(If(E, self.tidy(ret)), pc0))
                return N
            stmts.append(self.mark(Comment(f"decompiler: unstructured jump to {F}"), pc0))
            return N
        # if not E then [N, F) [else [F, E2)] end
        last_pc = self.last_insn_before(F, N)
        if last_pc is not None and self.at[last_pc].name in COND_JUMPS:
            # c and a or b: the then-part ends testing what it set and jumps over the else-part
            li = self.at[last_pc]
            raw = self.target(li)
            E2 = canon(raw)
            if F < E2 <= hi and E2 not in loop_t:
                then = self.block(N, last_pc, loops, None, frozenset())
                other = self.block(F, E2, loops, None, sub_ends(E2, raw))
                jc = self.jump_cond(li)
                t = self.single_assign(then)
                o = self.single_assign(other)
                if t and o and t[0] == o[0] and isinstance(jc, Var) and jc.reg == t[0]:
                    value = Binop("or", Binop("and", neg(E), t[1]), o[1])
                    a = Assign([Var(t[0])], [value])
                    a.pc, a.use_pc, a.line = pc0, last_pc, self.line(pc0)
                    stmts.append(a)
                    return E2
                # in general: the else-part runs on both ways it's reached
                import copy
                inner = If(neg(jc), copy.deepcopy(other), None)
                inner.pc = inner.use_pc = last_pc
                inner.line = self.line(last_pc)
                then = self.tidy(then + [inner])
                stmts.append(self.mark_if(If(neg(E), then, other), pc0))
                return E2
        if last_pc is not None:
            li = self.at[last_pc]
            if li.name in ("JUMP", "JUMPX") or (li.name == "LOADB" and li.c):
                raw = self.target(li)
                E2 = canon(raw)
                if F < E2 <= hi and (E2 == hi or E2 not in loop_t):
                    other = self.block(F, E2, loops, None, sub_ends(E2, raw), sub_tails(E2))
                    then = self.block(N, last_pc, loops, None, sub_ends(E2, raw) | {last_pc},
                                      {**sub_tails(E2), **{t: other for t in self.threads(F)}})
                    if li.name == "LOADB":
                        then = self.tidy(then + self.lift(li))
                    stmts.append(self.mark_if(If(neg(E), then, other), pc0))
                    return E2
        then = self.block(N, F, loops, None, sub_ends(F), sub_tails(F))
        stmts.append(self.mark_if(If(neg(E), then, None), pc0))
        return F

    def mark_if(self, s, pc0):
        s.pc = s.use_pc = pc0
        s.line = self.line(pc0)
        return s

    def last_insn_before(self, end, lo):
        import bisect
        k = bisect.bisect_left(self.order, end) - 1
        # dead code the compiler left behind ends nothing, but a dead jump over an else-part
        # still says where the then-part ends
        while k >= 0 and self.order[k] >= lo and self.order[k] not in self.reachable \
                and self.at[self.order[k]].name not in ("JUMP", "JUMPX"):
            k -= 1
        return self.order[k] if k >= 0 and self.order[k] >= lo else None

    # -- folding temporaries into what reads them, and other tidying

    def tidy(self, stmts):
        stmts = self.value_ifs(stmts)
        return self.fold(stmts)

    def value_ifs(self, stmts):
        """if c then r = a else r = b end -> r = if c then a else b; r = a; if r then r = b end -> r = a and b."""
        out = []
        for s in stmts:
            if isinstance(s, If):
                t = self.single_assign(s.then)
                o = self.single_assign(s.other) if s.other is not None else None
                if t and o and t[0] == o[0]:
                    a, b = t[1], o[1]
                    if isinstance(a, Const) and isinstance(b, Const) and a.kind == b.kind == "bool":
                        if a.value and not b.value:
                            value = s.cond if is_boolean(s.cond) else Unop("not", neg(s.cond)) \
                                if is_boolean(neg(s.cond)) else IfExpr(s.cond, a, b)
                        elif b.value and not a.value:
                            value = neg(s.cond) if is_boolean(neg(s.cond)) else Unop("not", s.cond)
                        else:
                            value = IfExpr(s.cond, a, b)
                    else:
                        value = IfExpr(s.cond, a, b)
                    new = Assign([Var(t[0])], [value])
                    new.pc, new.use_pc, new.line = s.pc, s.use_pc, s.line
                    out.append(new)
                    continue
                if t and s.other is None and out and isinstance(out[-1], Assign):
                    prev = out[-1]
                    if len(prev.targets) == 1 and isinstance(prev.targets[0], Var) and prev.targets[0].reg == t[0] \
                            and len(prev.values) == 1:
                        c = s.cond
                        if any(v.reg == t[0] for v in t[1].regs()):
                            out.append(s)
                            continue
                        if isinstance(c, Var) and c.reg == t[0]:
                            prev.values = [Binop("and", prev.values[0], t[1])]
                            prev.use_pc = s.use_pc
                            continue
                        if isinstance(c, Unop) and c.op == "not" and isinstance(c.a, Var) and c.a.reg == t[0]:
                            prev.values = [Binop("or", prev.values[0], t[1])]
                            prev.use_pc = s.use_pc
                            continue
            out.append(s)
        return out

    def single_assign(self, block):
        if not block or len(block) != 1:
            return None
        s = block[0]
        if isinstance(s, Assign) and len(s.targets) == 1 and isinstance(s.targets[0], Var) and len(s.values) == 1:
            return (s.targets[0].reg, s.values[0])
        return None

    def fold(self, stmts):
        out: list[Stmt] = []
        for s in stmts:
            self.pop_into(s, out)
            if self.into_table(s, out):
                continue
            out.append(s)
        return out

    def reads_of(self, s, reg):
        return sum(1 for e in s.host_exprs() for v in e.regs() if v.reg == reg)

    def pop_into(self, s, out):
        forced = set()
        if isinstance(s, NumFor):
            forced = {s.base, s.base + 1, s.base + 2}
        elif isinstance(s, GenFor):
            forced = {s.base, s.base + 1, s.base + 2}
            if out and isinstance(out[-1], Assign) and len(out[-1].values) == 1 and \
                    [t.reg for t in out[-1].targets if isinstance(t, Var)] == [s.base, s.base + 1, s.base + 2] \
                    and len(out[-1].targets) == 3:
                call = out.pop().values[0]
                call.multi = True
                s.exprs = [call]
                return
        while out:
            last = out[-1]
            if not isinstance(last, Assign) or len(last.targets) != 1 or not isinstance(last.targets[0], Var) \
                    or len(last.values) != 1:
                return
            reg = last.targets[0].reg
            value = last.values[0]
            if any(self.captures(e, reg) for e in s.host_exprs()) or self.captures(value, reg):
                return
            if isinstance(value, Table) and self.fills(s, reg):
                return
            if self.reads_of(s, reg) != 1:
                return
            if reg not in forced:
                if not self.dead_after(reg, s.use_pc) and not self.redefined(s, reg):
                    return
            # where the read sits wants one value or all of them
            if not self.place(s, reg, value):
                return
            out.pop()

    def fills(self, s, reg):
        """Whether `s` sets a field or the array of the table in `reg` (into_table's to fold)."""
        if isinstance(s, SetList):
            return isinstance(s.table, Var) and s.table.reg == reg
        return isinstance(s, Assign) and len(s.targets) == 1 and isinstance(s.targets[0], Index) and \
            isinstance(s.targets[0].obj, Var) and s.targets[0].obj.reg == reg

    def captures(self, e, reg):
        """Whether a closure in `e` captures register `reg`."""
        found = [False]

        def check(x):
            if isinstance(x, Closure) and ("reg", reg) in x.captures:
                found[0] = True

        walk_exprs(e, check)
        return found[0]

    def redefined(self, s, reg):
        return reg in s.defs()

    def place(self, s, reg, value):
        """Puts `value` where `s` reads `reg`; False if it can't go there."""
        multi_ok = False
        for e in s.host_exprs():
            for v in e.regs():
                if v.reg == reg and getattr(v, "multi", False):
                    multi_ok = True
        if value.multi and not multi_ok:
            if isinstance(value, Call):
                value = Call(value.func, value.args, 1)
            elif isinstance(value, Varargs):
                value = Varargs(False)
        if multi_ok and not value.multi:
            return False
        if isinstance(s, Assign):
            s.values = [self.sub(e, reg, value) for e in s.values]
            for t in s.targets:
                if isinstance(t, Index):
                    t.obj = self.sub(t.obj, reg, value)
                    t.key = self.sub(t.key, reg, value)
        elif isinstance(s, CallStmt):
            s.call = self.sub(s.call, reg, value)
        elif isinstance(s, Return):
            s.values = [self.sub(e, reg, value) for e in s.values]
        elif isinstance(s, SetList):
            s.table = self.sub(s.table, reg, value)
            s.values = [self.sub(e, reg, value) for e in s.values]
        elif isinstance(s, (If, Until)):
            s.cond = self.sub(s.cond, reg, value)
        elif isinstance(s, NumFor):
            s.start, s.limit, s.step = (self.sub(e, reg, value) for e in (s.start, s.limit, s.step))
        elif isinstance(s, GenFor):
            s.exprs = [self.sub(e, reg, value) for e in s.exprs]
        else:
            return False
        return True

    def sub(self, e, reg, value):
        if isinstance(e, Var) and e.reg == reg:
            return value
        new, _ = replace_var(e, reg, value)
        return new

    def into_table(self, s, out):
        """t = {}; t.k = v -> t = {k = v} (and SETLIST into its array)."""
        if not out:
            return False
        last = out[-1]
        if not isinstance(last, Assign) or len(last.targets) != 1 or not isinstance(last.targets[0], Var) \
                or len(last.values) != 1 or not isinstance(last.values[0], Table):
            return False
        reg = last.targets[0].reg
        table = last.values[0]
        if isinstance(s, Assign) and len(s.targets) == 1 and isinstance(s.targets[0], Index):
            t = s.targets[0]
            if isinstance(t.obj, Var) and t.obj.reg == reg and not any(v.reg == reg for v in t.key.regs()) \
                    and not any(v.reg == reg for v in s.values[0].regs()) and not self.captures(s.values[0], reg):
                for n, (k, _) in enumerate(table.hash):
                    if same(k, t.key) and isinstance(k, Const):
                        table.hash[n] = (k, s.values[0])
                        return True
                table.hash.append((t.key, s.values[0]))
                return True
        if isinstance(s, SetList) and isinstance(s.table, Var) and s.table.reg == reg \
                and s.start == len(table.array) + 1 and not any(v.reg == reg for e in s.values for v in e.regs()):
            table.array += s.values
            return True
        return False


# --- printing ---------------------------------------------------------------

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def lua_string(b: bytes):
    out = ['"']
    for ch in b:
        c = chr(ch)
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif c == "\n":
            out.append("\\n")
        elif c == "\r":
            out.append("\\r")
        elif c == "\t":
            out.append("\\t")
        elif 32 <= ch < 127:
            out.append(c)
        else:
            out.append(f"\\{ch}")
    out.append('"')
    s = "".join(out)
    try:  # printable UTF-8 reads better as itself
        text = b.decode("utf-8")
        if all(ch.isprintable() or ch in "\n\t\r" for ch in text) and any(ord(ch) > 127 for ch in text):
            s = '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r") \
                .replace("\t", "\\t") + '"'
    except UnicodeDecodeError:
        pass
    return s


def lua_number(v):
    if isinstance(v, int):
        return str(v)
    if math.isnan(v):
        return "0/0"
    if math.isinf(v):
        return "math.huge" if v > 0 else "-math.huge"
    if v == int(v) and abs(v) < 2 ** 53:
        return str(int(v)) if not (v == 0 and math.copysign(1, v) < 0) else "-0"
    r = repr(v)
    return r


class Scope:
    def __init__(self, parent):
        self.parent = parent
        self.names = {}
        self.declared = 0  # locals this block declared (redeclarations included)

    def active(self):
        n = 0
        s = self
        while s:
            n += s.declared
            s = s.parent
        return n

    def find(self, reg):
        s = self
        while s:
            if reg in s.names:
                return s.names[reg]
            s = s.parent
        return None


class Printer:
    def __init__(self, dec: "Decompiler", lines: bool):
        self.dec = dec
        self.show_lines = lines
        self.out = []



class FuncCtx:
    def __init__(self, printer: Printer, fn: Function, upnames):
        self.pr = printer
        self.fn = fn
        self.upnames = upnames
        self.used = set(upnames) | printer.dec.reserved
        self.lines = []
        self.flag_names = {}

    def fresh(self, base):
        base = base if base and IDENT.match(base) and base not in KEYWORDS else "v"
        name = base
        k = 2
        while name in self.used or name in KEYWORDS:
            name = f"{base}_{k}"
            k += 1
        self.used.add(name)
        return name

    def suggest(self, reg, value):
        """A name for a local from what it's first given."""
        e = value
        if isinstance(e, Call) and isinstance(e.func, MethodRef):
            m = e.func.name
            if m in ("GetService", "WaitForChild", "FindFirstChild", "FindFirstChildOfClass",
                     "FindFirstChildWhichIsA", "FindFirstAncestor", "FindFirstAncestorOfClass",
                     "GetAttribute") and e.args:
                a = e.args[0]
                if isinstance(a, Const) and a.kind == "str":
                    return self.clean(a.value.decode("latin1"))
        if isinstance(e, Call) and isinstance(e.func, Global) and e.func.name == "require" and e.args:
            a = e.args[0]
            if isinstance(a, Index) and isinstance(a.key, Const) and a.key.kind == "str":
                return self.clean(a.key.value.decode("latin1"))
            if isinstance(a, Call) and isinstance(a.func, MethodRef) and a.args and isinstance(a.args[0], Const) \
                    and a.args[0].kind == "str":
                return self.clean(a.args[0].value.decode("latin1"))
        if isinstance(e, Index) and isinstance(e.key, Const) and e.key.kind == "str":
            return self.clean(e.key.value.decode("latin1"))
        if isinstance(e, Call) and isinstance(e.func, Index) and isinstance(e.func.obj, Global) \
                and e.func.obj.name == "Instance" and e.args and isinstance(e.args[0], Const) \
                and e.args[0].kind == "str":
            return self.clean(e.args[0].value.decode("latin1"))
        return None

    def clean(self, s):
        s = re.sub(r"[^A-Za-z0-9_]", "_", s)
        if not s or s[0].isdigit():
            s = "_" + s
        return s[:40]

    def default_name(self, reg):
        return f"v{reg}"

    # -- where locals are declared

    def plan(self, body, scope_regs=()):
        """Per block: which statements declare which registers (LCA of their references)."""
        refs = {}  # reg -> list of paths (tuple of (block id, index))
        blocks = {}

        def note(reg, path):
            refs.setdefault(reg, []).append(path)

        def visit(block, path, bound):
            blocks[id(block)] = block
            for idx, s in enumerate(block):
                here = path + ((id(block), idx),)
                for e in s.host_exprs():
                    for v in e.regs():
                        if v.reg not in bound:
                            note(v.reg, here)
                if isinstance(s, Assign):
                    for t in s.targets:
                        if isinstance(t, Var) and t.reg not in bound:
                            note(t.reg, here)
                if isinstance(s, If):
                    visit(s.then, here, bound)
                    if s.other is not None:
                        visit(s.other, here, bound)
                elif isinstance(s, (While, Repeat)):
                    if isinstance(s, While):
                        for v in s.cond.regs():
                            if v.reg not in bound:
                                note(v.reg, here)
                    visit(s.body, here, bound)
                    if isinstance(s, Repeat):
                        for v in s.cond.regs():
                            if v.reg not in bound:
                                note(v.reg, here + ((id(s.body), len(s.body)),))
                elif isinstance(s, NumFor):
                    visit(s.body, here, bound | {s.base + 2})
                elif isinstance(s, GenFor):
                    visit(s.body, here, bound | set(range(s.base + 3, s.base + 3 + s.nvars)))

        visit(body, (), set(scope_regs))
        decl = {}  # (block id, index) -> registers declared at that statement
        for reg, paths in refs.items():
            if reg in scope_regs:
                continue
            # the deepest statement all references are at or inside
            d = 0
            while all(len(p) > d + 1 for p in paths) and all(p[d] == paths[0][d] for p in paths):
                d += 1
            if all(p[d][0] == paths[0][d][0] for p in paths):
                where = (paths[0][d][0], min(p[d][1] for p in paths))
            else:  # then and else of one if: declared before it
                where = paths[0][d - 1]
            decl.setdefault(where, []).append(reg)
        self.decl = decl

    def render(self, body, indent):
        fn = self.fn
        p = fn.p
        scope = Scope(None)
        params = []
        for r in range(p.numparams):
            name = self.fresh(self.locvar_name(r, 0) or f"a{r + 1}")
            self.declare(scope, r, name)
            params.append(name)
        if p.vararg:
            params.append("...")
        self.plan(body, scope_regs=range(p.numparams))
        lines = []
        self.block(body, scope, indent + 1, lines)
        return params, lines

    def declare(self, scope, reg, name):
        scope.names[reg] = name
        scope.declared += 1

    def locvar_name(self, reg, pc):
        for name, start, end, r in self.fn.p.locvars:
            if r == reg and start <= pc + 1 <= end and name:
                return name
        return None

    def block(self, block, parent, indent, lines):
        scope = Scope(parent)
        self.statements(block, scope, indent, lines)

    def statements(self, block, scope, indent, lines):
        for idx, s in enumerate(block):
            regs = self.decl.get((id(block), idx), [])
            self.first = idx == 0
            # return, break and continue end a block in Luau: one with more after it goes in a do-end
            self.wrap = idx < len(block) - 1 and isinstance(s, (Return, Break, Continue))
            self.stmt(s, scope, indent, lines, regs)

    def name(self, scope, reg):
        n = scope.find(reg)
        if n is None:
            n = self.fresh(self.default_name(reg))
            # a read of something never declared: declare it at the top of the function
            s = scope
            while s.parent:
                s = s.parent
            s.names[reg] = n
        return n

    def stmt(self, s, scope, indent, lines, declare):
        pad = "    " * indent
        tail = f" --@{s.line}" if self.pr.show_lines and s.line is not None else ""
        if isinstance(s, Assign) and getattr(s, "declares_flag", False):
            flag = s.targets[0]
            self.flag_names[flag.ident] = self.fresh("returned")
            lines.append(f"{pad}local {self.flag_names[flag.ident]} = false{tail}")
            return
        if isinstance(s, Assign):
            direct = [t.reg for t in s.targets if isinstance(t, Var)]
            new = [r for r in declare if r in direct]
            # the compiler reuses a dead local's register for the next one: a write that doesn't
            # read the old value, to a local of this block nothing captures by reference, starts a new one
            if not new and direct and len(direct) == len(s.targets) and scope.active() < 150 and all(
                    r in scope.names and r not in self.fn.captured_ref for r in direct) and not any(
                    v.reg in direct for e in s.host_exprs() for v in e.regs()):
                new = list(direct)
            pre = [r for r in declare if r not in direct]
            for r in pre:
                nm = self.fresh(self.default_name(r))
                self.declare(scope, r, nm)
                lines.append(f"{pad}local {nm}{tail}")
            # a plain write to a local declared in this block may start a new one (fresh name)
            values = [self.expr(v, scope, indent) for v in s.values]
            if new and len(new) == len(s.targets):
                names = []
                for t in s.targets:
                    hint = self.suggest(t.reg, s.values[0]) if len(s.targets) == 1 else None
                    nm = self.fresh(self.locvar_name(t.reg, s.pc) or hint or self.default_name(t.reg))
                    names.append(nm)
                for t, nm in zip(s.targets, names):
                    self.declare(scope, t.reg, nm)
                if len(s.values) == 1 and isinstance(s.values[0], Closure) and len(names) == 1:
                    params, body = self.closure_body(s.values[0], scope, indent)
                    lines.append(f"{pad}local function {names[0]}({', '.join(params)}){tail}")
                    lines += body
                    lines.append(f"{pad}end")
                    return
                lines.append(f"{pad}local {', '.join(names)} = {', '.join(values)}{tail}")
                return
            for r in new:
                nm = self.fresh(self.default_name(r))
                self.declare(scope, r, nm)
                lines.append(f"{pad}local {nm}{tail}")
            targets = [self.target(t, scope, indent) for t in s.targets]
            if len(s.targets) == 1 and len(s.values) == 1 and isinstance(s.values[0], Closure) and \
                    (isinstance(s.targets[0], Global) or (isinstance(s.targets[0], Index) and self.dotted(s.targets[0]))):
                params, body = self.closure_body(s.values[0], scope, indent)
                lines.append(f"{pad}function {targets[0]}({', '.join(params)}){tail}")
                lines += body
                lines.append(f"{pad}end")
                return
            lines.append(f"{pad}{', '.join(targets)} = {', '.join(values)}{tail}")
            return
        for r in declare:
            nm = self.fresh(self.default_name(r))
            self.declare(scope, r, nm)
            lines.append(f"{pad}local {nm}")
        if isinstance(s, CallStmt):
            text = self.expr(s.call, scope, indent)
            lines.append(f"{pad}{';' if text.startswith('(') and not self.first else ''}{text}{tail}")
        elif isinstance(s, Return):
            vals = self.exprlist(s.values, scope, indent)
            text = f"return{(' ' + vals) if vals else ''}"
            lines.append(f"{pad}{f'do {text} end' if self.wrap else text}{tail}")
        elif isinstance(s, SetList):
            t = self.expr(s.table, scope, indent)
            for k, v in enumerate(s.values):
                lines.append(f"{pad}{t}[{s.start + k}] = {self.expr(v, scope, indent)}{tail}")
        elif isinstance(s, If):
            self.if_chain(s, scope, indent, lines, tail)
        elif isinstance(s, While):
            lines.append(f"{pad}while {self.expr(s.cond, scope, indent)} do{tail}")
            self.block(s.body, scope, indent + 1, lines)
            lines.append(f"{pad}end")
        elif isinstance(s, Repeat):
            note = " -- inlined: break returns from it" if getattr(s, "inlined", False) else ""
            lines.append(f"{pad}repeat{note}{tail}")
            inner = Scope(scope)
            self.statements(s.body, inner, indent + 1, lines)
            lines.append(f"{pad}until {self.expr(s.cond, inner, indent)}")
        elif isinstance(s, NumFor):
            inner = Scope(scope)
            start = self.expr(s.start, scope, indent)
            limit = self.expr(s.limit, scope, indent)
            step = self.expr(s.step, scope, indent)
            var = self.fresh("i" if indent < 3 else f"i{indent}")
            self.declare(inner, s.base + 2, var)
            stepping = "" if step == "1" else f", {step}"
            lines.append(f"{pad}for {var} = {start}, {limit}{stepping} do{tail}")
            self.block(s.body, inner, indent + 1, lines)
            lines.append(f"{pad}end")
        elif isinstance(s, GenFor):
            inner = Scope(scope)
            exprs = list(s.exprs)
            while len(exprs) > 1 and isinstance(exprs[-1], Const) and exprs[-1].kind == "nil":
                exprs.pop()
            src = self.exprlist(exprs, scope, indent)
            names = []
            for k in range(s.nvars):
                base = ("k", "v")[k] if s.nvars <= 2 else f"v{k + 1}"
                nm = self.fresh(base)
                self.declare(inner, s.base + 3 + k, nm)
                names.append(nm)
            lines.append(f"{pad}for {', '.join(names)} in {src} do{tail}")
            self.block(s.body, inner, indent + 1, lines)
            lines.append(f"{pad}end")
        elif isinstance(s, Break):
            lines.append(f"{pad}{'do break end' if self.wrap else 'break'}{tail}")
        elif isinstance(s, Continue):
            lines.append(f"{pad}{'do continue end' if self.wrap else 'continue'}{tail}")
        elif isinstance(s, Comment):
            lines.append(f"{pad}--[[{s.text}]]{tail}")
        elif isinstance(s, Until):
            lines.append(f"{pad}--[[decompiler: until {self.expr(s.cond, scope, indent)}]]")

    def if_chain(self, s, scope, indent, lines, tail):
        pad = "    " * indent
        lines.append(f"{pad}if {self.expr(s.cond, scope, indent)} then{tail}")
        self.block(s.then, scope, indent + 1, lines)
        other = s.other
        while other is not None:
            if len(other) == 1 and isinstance(other[0], If) and not self.decl.get((id(other), 0)):
                o = other[0]
                lines.append(f"{pad}elseif {self.expr(o.cond, scope, indent)} then")
                self.block(o.then, scope, indent + 1, lines)
                other = o.other
                continue
            lines.append(f"{pad}else")
            self.block(other, scope, indent + 1, lines)
            break
        lines.append(f"{pad}end")

    def dotted(self, t):
        e = t
        while isinstance(e, Index):
            if not (isinstance(e.key, Const) and e.key.kind == "str" and IDENT.match(e.key.value.decode("latin1"))
                    and e.key.value.decode("latin1") not in KEYWORDS):
                return False
            e = e.obj
        return isinstance(e, (Global, Var, Upval))

    def target(self, t, scope, indent):
        if isinstance(t, Var):
            return self.name(scope, t.reg)
        return self.expr(t, scope, indent)

    def exprlist(self, exprs, scope, indent):
        parts = []
        for k, e in enumerate(exprs):
            text = self.expr(e, scope, indent)
            if k == len(exprs) - 1 and isinstance(e, (Call, Varargs)) and not e.multi:
                text = f"({text})"
            parts.append(text)
        return ", ".join(parts)

    def closure_body(self, c: Closure, scope, indent):
        upnames = []
        for kind, r in c.captures:
            if kind == "reg":
                upnames.append(self.name(scope, r))
            else:
                upnames.append(self.upnames[r] if r < len(self.upnames) else f"up{r}")
        child = self.pr.dec.function(c.proto)
        ctx = FuncCtx(self.pr, child, upnames)
        params, lines = ctx.render(child.decompile(), indent)
        return params, lines

    def expr(self, e, scope, indent, prec=0):
        text, p = self.expr_p(e, scope, indent)
        return f"({text})" if p < prec else text

    def prefix(self, e, scope, indent):
        text, p = self.expr_p(e, scope, indent)
        if isinstance(e, (Var, Upval, Global, Index, Call, MethodRef)):
            return text
        return f"({text})"

    def expr_p(self, e, scope, indent):
        if isinstance(e, Var):
            return self.name(scope, e.reg), 10
        if isinstance(e, Const):
            if e.kind == "nil":
                return "nil", 10
            if e.kind == "bool":
                return ("true" if e.value else "false"), 10
            if e.kind in ("num", "int"):
                text = lua_number(e.value)
                return text, (UNARY if text.startswith("-") else 10)
            if e.kind == "str":
                return lua_string(e.value), 10
            if e.kind == "vector":
                return "vector.create(" + ", ".join(lua_number(float(x)) for x in e.value[:3]) + ")", 10
            if e.kind == "table":
                return "{}", 10
            return f"--[[{e.kind}]] nil", 10
        if isinstance(e, Upval):
            return (self.upnames[e.index] if e.index < len(self.upnames) else f"up{e.index}"), 10
        if isinstance(e, Flag):
            return self.flag_names.get(e.ident, f"returned{e.ident}"), 10
        if isinstance(e, Global):
            return e.name, 10
        if isinstance(e, Index):
            obj = self.prefix(e.obj, scope, indent)
            k = e.key
            if isinstance(k, Const) and k.kind == "str":
                s = k.value.decode("latin1")
                if IDENT.match(s) and s not in KEYWORDS:
                    return f"{obj}.{s}", 10
            return f"{obj}[{self.expr(k, scope, indent)}]", 10
        if isinstance(e, MethodRef):
            return f"{self.prefix(e.obj, scope, indent)}:{e.name}", 10
        if isinstance(e, Call):
            args = self.exprlist(e.args, scope, indent)
            if isinstance(e.func, MethodRef):
                m = e.func
                return f"{self.prefix(m.obj, scope, indent)}:{m.name}({args})", 10
            return f"{self.prefix(e.func, scope, indent)}({args})", 10
        if isinstance(e, Varargs):
            return "...", 10
        if isinstance(e, Binop):
            p = PREC[e.op]
            if e.op in RIGHT:
                a = self.expr(e.a, scope, indent, p + 1)
                b = self.expr(e.b, scope, indent, p)
            else:
                a = self.expr(e.a, scope, indent, p)
                b = self.expr(e.b, scope, indent, p + 1)
            return f"{a} {e.op} {b}", p
        if isinstance(e, Unop):
            a = self.expr(e.a, scope, indent, UNARY)
            if e.op == "not":
                return f"not {a}", UNARY
            if e.op == "-" and a.startswith("-"):
                return f"-({a})", UNARY
            return f"{e.op}{a}", UNARY
        if isinstance(e, IfExpr):
            return (f"if {self.expr(e.cond, scope, indent)} then {self.expr(e.a, scope, indent)} "
                    f"else {self.expr(e.b, scope, indent)}"), 0
        if isinstance(e, Closure):
            params, lines = self.closure_body(e, scope, indent)
            pad = "    " * indent
            return "function(" + ", ".join(params) + ")\n" + "\n".join(lines) + f"\n{pad}end", 10
        if isinstance(e, Table):
            parts = []
            for v in e.array:
                parts.append(self.expr(v, scope, indent + 1))
            if e.array and isinstance(e.array[-1], (Call, Varargs)) and not e.array[-1].multi:
                parts[-1] = f"({parts[-1]})"
            for k, v in e.hash:
                if isinstance(k, Const) and k.kind == "str" and IDENT.match(k.value.decode("latin1")) \
                        and k.value.decode("latin1") not in KEYWORDS:
                    parts.append(f"{k.value.decode('latin1')} = {self.expr(v, scope, indent + 1)}")
                else:
                    parts.append(f"[{self.expr(k, scope, indent + 1)}] = {self.expr(v, scope, indent + 1)}")
            if not parts:
                return "{}", 10
            one = "{" + ", ".join(parts) + "}"
            if len(one) <= 100 and "\n" not in one:
                return one, 10
            pad = "    " * (indent + 1)
            return "{\n" + ",\n".join(pad + x for x in parts) + "\n" + "    " * indent + "}", 10
        return f"--[[{type(e).__name__}]] nil", 10


# --- the whole chunk --------------------------------------------------------


class Decompiler:
    def __init__(self, data: bytes, scrambled=None, lines=True):
        self.chunk = Chunk(data)
        self.scrambled = looks_scrambled(self.chunk) if scrambled is None else scrambled
        self.functions = {}
        self.lines = lines
        self.reserved = set(KEYWORDS) | {"game", "workspace", "script", "Enum", "Instance", "Vector3", "CFrame",
                                         "math", "string", "table", "task", "print", "warn", "error", "pairs",
                                         "ipairs", "next", "require", "self", "type", "typeof", "tostring",
                                         "tonumber", "select", "unpack", "_G", "shared", "wait", "spawn", "delay",
                                         "tick", "time", "os", "coroutine", "debug", "utf8", "bit32", "buffer"}
        for p in self.chunk.protos:
            for kind, v in p.k:
                if kind == "import" and v:
                    self.reserved.add(v[0].decode("latin1"))
        # globals read or written by name
        for p in self.chunk.protos:
            try:
                for i in decode(p, self.scrambled):
                    if i.name in ("GETGLOBAL", "SETGLOBAL") and i.aux is not None:
                        kind, v = p.k[i.aux]
                        if kind == "str":
                            self.reserved.add(v.decode("latin1"))
            except DecompileError:  # the function fails again (and says why) when it's decompiled
                continue

    def function(self, index):
        if index not in self.functions:
            self.functions[index] = Function(self, self.chunk.protos[index])
        return self.functions[index]

    def text(self):
        printer = Printer(self, self.lines)
        main = self.function(self.chunk.main)
        ctx = FuncCtx(printer, main, [])
        params, lines = ctx.render(main.decompile(), -1)
        return "\n".join(lines) + "\n"


def disasm(data: bytes, scrambled=None):
    chunk = Chunk(data)
    scr = looks_scrambled(chunk) if scrambled is None else scrambled
    out = [f"-- bytecode version {chunk.version}, {len(chunk.protos)} functions, main {chunk.main}"
           + (", opcodes scrambled" if scr else "")]
    for p in chunk.protos:
        out.append(f"\nfunction {p.index} ({p.debugname or '-'}, line {p.linedefined}): "
                   f"{p.numparams} params{', vararg' if p.vararg else ''}, {p.nups} upvalues, children {p.protos}")
        for i in decode(p, scr):
            line = p.lines[i.pc] if p.lines else "-"
            extra = ""
            if i.name in ("GETGLOBAL", "SETGLOBAL", "GETTABLEKS", "SETTABLEKS", "NAMECALL", "LOADKX"):
                extra = repr(p.k[i.aux][1])
            elif i.name in ("LOADK", "GETIMPORT", "DUPCLOSURE"):
                extra = repr(p.k[i.d][1])
            out.append(f"  {line:>6} {i.pc:5} {i.name:14} A={i.a} B={i.b} C={i.c} D={i.d}"
                       + (f" aux={i.aux}" if i.aux is not None else "") + (f"  {extra}" if extra else ""))
    return "\n".join(out) + "\n"


def decompile_file(src, dst, scrambled, lines):
    data = open(src, "rb").read()
    try:
        text = Decompiler(data, scrambled, lines).text()
    except Exception as exc:  # one script failing shouldn't stop the rest
        text = f"--[[decompiler: failed: {type(exc).__name__}: {exc}]]\n"
        ok = False
    else:
        ok = True
    if dst:
        with open(dst, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="a .luauc file, or with --dir a folder")
    ap.add_argument("output", nargs="?", help="with --dir: the folder to write .lua files to")
    ap.add_argument("-o", dest="out", help="the .lua file to write (default stdout)")
    ap.add_argument("--dir", action="store_true", help="every .luauc under the input folder")
    ap.add_argument("--disasm", action="store_true", help="print the instructions instead")
    ap.add_argument("--scrambled", choices=["auto", "yes", "no"], default="auto")
    ap.add_argument("--no-lines", action="store_true", help="leave out the --@<line> comments")
    args = ap.parse_args()
    scrambled = None if args.scrambled == "auto" else args.scrambled == "yes"
    if args.disasm:
        sys.stdout.write(disasm(open(args.input, "rb").read(), scrambled))
        return 0
    if not args.dir:
        return 0 if decompile_file(args.input, args.out, scrambled, not args.no_lines) else 1
    if not args.output:
        ap.error("--dir needs an output folder")
    failed = done = 0
    for root, _, files in os.walk(args.input):
        for name in sorted(files):
            if not name.endswith(".luauc"):
                continue
            src = os.path.join(root, name)
            rel = os.path.relpath(src, args.input)
            dst = os.path.join(args.output, rel[:-len(".luauc")] + ".lua")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            ok = decompile_file(src, dst, scrambled, not args.no_lines)
            done += 1
            failed += not ok
            if not ok:
                print(f"failed: {rel}", file=sys.stderr)
    print(f"{done} scripts, {failed} failed", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    # deep elseif chains nest deep: a thread with room for the recursion
    import threading
    threading.stack_size(1 << 29)
    result = []
    worker = threading.Thread(target=lambda: result.append(main()))
    worker.start()
    worker.join()
    sys.exit(result[0] if result else 1)
