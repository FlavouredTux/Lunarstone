// Luau bytecode back into Luau: tools/luau_decompile.py in C++, for the
// console's decompile. It follows the tool function for function, with the
// tool's names, so the tool's comments explain this too and a change to one
// belongs in the other; tools/luau_decompile_check.py is what proves them.

#include "Decompiler.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lunar::lua {

namespace {

struct DecompileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// --- opcodes (Bytecode.h, in order) -----------------------------------------

enum class Op : std::uint8_t {
    NOP, BREAK, LOADNIL, LOADB, LOADN, LOADK, MOVE, GETGLOBAL, SETGLOBAL, GETUPVAL, SETUPVAL,
    CLOSEUPVALS, GETIMPORT, GETTABLE, SETTABLE, GETTABLEKS, SETTABLEKS, GETTABLEN, SETTABLEN,
    NEWCLOSURE, NAMECALL, CALL, RETURN, JUMP, JUMPBACK, JUMPIF, JUMPIFNOT, JUMPIFEQ, JUMPIFLE,
    JUMPIFLT, JUMPIFNOTEQ, JUMPIFNOTLE, JUMPIFNOTLT, ADD, SUB, MUL, DIV, MOD, POW, ADDK, SUBK, MULK,
    DIVK, MODK, POWK, AND, OR, ANDK, ORK, CONCAT, NOT, MINUS, LENGTH, NEWTABLE, DUPTABLE, SETLIST,
    FORNPREP, FORNLOOP, FORGLOOP, FORGPREP_INEXT, FASTCALL3, FORGPREP_NEXT, NATIVECALL,
    GETVARARGS, DUPCLOSURE, PREPVARARGS, LOADKX, JUMPX, FASTCALL, COVERAGE, CAPTURE, SUBRK, DIVRK,
    FASTCALL1, FASTCALL2, FASTCALL2K, FORGPREP, JUMPXEQKNIL, JUMPXEQKB, JUMPXEQKN, JUMPXEQKS,
    IDIV, IDIVK, GETUDATAKS, SETUDATAKS, NAMECALLUDATA, NEWCLASSMEMBER, CALLFB, CMPPROTO,
    FASTPCALL, NEWCLASS,
};
constexpr int opCount = static_cast<int>(Op::NEWCLASS) + 1;
using enum Op;

constexpr bool hasAux(Op op)
{
    switch (op) {
    case GETGLOBAL: case SETGLOBAL: case GETIMPORT: case GETTABLEKS: case SETTABLEKS: case NAMECALL:
    case JUMPIFEQ: case JUMPIFLE: case JUMPIFLT: case JUMPIFNOTEQ: case JUMPIFNOTLE: case JUMPIFNOTLT:
    case NEWTABLE: case SETLIST: case FORGLOOP: case LOADKX: case FASTCALL2: case FASTCALL2K: case FASTCALL3:
    case JUMPXEQKNIL: case JUMPXEQKB: case JUMPXEQKN: case JUMPXEQKS: case GETUDATAKS: case SETUDATAKS:
    case NAMECALLUDATA: case NEWCLASSMEMBER: case CALLFB: case CMPPROTO: case NEWCLASS:
        return true;
    default:
        return false;
    }
}

constexpr bool isCondJump(Op op)
{
    switch (op) {
    case JUMPIF: case JUMPIFNOT: case JUMPIFEQ: case JUMPIFLE: case JUMPIFLT: case JUMPIFNOTEQ: case JUMPIFNOTLE:
    case JUMPIFNOTLT: case JUMPXEQKNIL: case JUMPXEQKB: case JUMPXEQKN: case JUMPXEQKS:
        return true;
    default:
        return false;
    }
}

constexpr bool isUncondJump(Op op) { return op == JUMP || op == JUMPBACK || op == JUMPX; }
constexpr bool isForPrep(Op op) { return op == FORGPREP || op == FORGPREP_INEXT || op == FORGPREP_NEXT; }

constexpr bool isSkipped(Op op)
{
    switch (op) {
    case NOP: case BREAK: case CLOSEUPVALS: case PREPVARARGS: case COVERAGE: case NATIVECALL: case FASTCALL:
    case FASTCALL1: case FASTCALL2: case FASTCALL2K: case FASTCALL3: case FASTPCALL: case CMPPROTO:
        return true;
    default:
        return false;
    }
}

// What ends a run of instructions condition() folds into one jump.
constexpr bool endsSegment(Op op)
{
    return isCondJump(op) || isUncondJump(op) || op == RETURN || op == FORNPREP || op == FORNLOOP || op == FORGLOOP
           || isForPrep(op);
}

constexpr std::array<std::string_view, 22> keywords{
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "continue",
};

bool isKeyword(std::string_view s)
{
    return std::ranges::find(keywords, s) != keywords.end();
}

bool isIdent(std::string_view s)
{
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    return std::ranges::all_of(s, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// The decompiler counts pcs, registers and places in ints, as the tool does: one as an index.
constexpr std::size_t ix(int i)
{
    return static_cast<std::size_t>(i);
}

// --- reading the bytecode ---------------------------------------------------

struct Reader {
    std::string_view data;
    std::size_t offset = 0;

    std::uint8_t u8()
    {
        if (offset >= data.size())
            throw DecompileError("bytecode ends early");
        return static_cast<std::uint8_t>(data[offset++]);
    }

    std::uint64_t var()
    {
        std::uint64_t result = 0;
        for (int shift = 0;; shift += 7) {
            const std::uint8_t byte = u8();
            if (shift < 64)
                result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (!(byte & 0x80))
                return result;
        }
    }

    std::string_view bytes(std::size_t n)
    {
        if (offset > data.size() || n > data.size() - offset)
            throw DecompileError("bytecode ends early");
        const std::string_view out = data.substr(offset, n);
        offset += n;
        return out;
    }

    template <typename T>
    T get()
    {
        const std::string_view raw = bytes(sizeof(T));
        T value;
        std::memcpy(&value, raw.data(), sizeof(T));
        return value;
    }
};

struct Constant {
    enum class Kind : std::uint8_t { Nil, Bool, Num, Str, Import, Table, Closure, Vector, TableK, Int, Class };
    Kind kind = Kind::Nil;
    bool boolean = false;
    double number = 0;
    std::int64_t integer = 0;
    std::string text;
    std::vector<std::string> import;
    std::vector<std::uint64_t> keys; // a Table's
    std::uint64_t closure = 0;
    std::array<double, 4> vector{};
    std::vector<std::pair<std::uint64_t, std::int32_t>> entries; // a TableK's

    bool operator==(const Constant&) const = default;
};

std::string_view kindName(Constant::Kind kind)
{
    static constexpr std::array<std::string_view, 11> names{"nil",     "bool",   "num",    "str",  "import", "table",
                                                            "closure", "vector", "tablek", "int",  "class"};
    return names[static_cast<std::size_t>(kind)];
}

struct LocVar {
    std::optional<std::string> name;
    std::uint64_t start = 0, end = 0;
    int reg = 0;
};

struct Proto {
    int index = 0;
    int maxstack = 0, numparams = 0, nups = 0;
    bool vararg = false;
    int flags = 0;
    std::vector<std::uint32_t> code;
    std::vector<Constant> k;
    std::vector<std::uint64_t> protos;
    std::uint64_t linedefined = 0;
    std::optional<std::string> debugname;
    std::optional<std::vector<int>> lines;
    std::vector<LocVar> locvars;
    std::vector<std::optional<std::string>> upvalnames;
};

struct Chunk {
    int version = 0;
    int typesversion = 0;
    std::vector<std::string> strings;
    std::vector<Proto> protos;
    std::uint64_t main = 0;

    explicit Chunk(std::string_view data)
    {
        Reader r{data};
        version = r.u8();
        if (version == 0)
            throw DecompileError(std::format("not bytecode: a compile error ({})", data.substr(1)));
        if (version < 3 || version > 14)
            throw DecompileError(std::format("bytecode version {} isn't one this reads (3 to 14)", version));
        typesversion = version >= 4 ? r.u8() : 0;
        for (std::uint64_t n = r.var(), i = 0; i < n; ++i)
            strings.emplace_back(r.bytes(r.var()));
        if (typesversion == 3)
            for (std::uint8_t index = r.u8(); index; index = r.u8())
                r.var();
        for (std::uint64_t n = r.var(), i = 0; i < n; ++i)
            protos.push_back(proto(r, static_cast<int>(i)));
        main = r.var();
    }

    std::optional<std::string> string(std::uint64_t i) const
    {
        if (i == 0)
            return std::nullopt;
        if (i > strings.size())
            throw DecompileError(std::format("string {} isn't in the bytecode", i));
        return strings[i - 1];
    }

    Proto proto(Reader& r, int index) const
    {
        Proto p;
        p.index = index;
        const std::optional<std::uint64_t> size = version >= 12 ? std::optional(r.var()) : std::nullopt;
        const std::size_t start = r.offset;
        p.maxstack = r.u8();
        p.numparams = r.u8();
        p.nups = r.u8();
        p.vararg = r.u8() != 0;
        if (version >= 4) {
            p.flags = r.u8();
            if (typesversion >= 1 && typesversion <= 3)
                r.offset += r.var();
        }
        const std::uint64_t n = r.var();
        for (std::uint64_t i = 0; i < n; ++i)
            p.code.push_back(r.get<std::uint32_t>());
        for (std::uint64_t count = r.var(), i = 0; i < count; ++i) {
            Constant c;
            switch (const std::uint8_t t = r.u8()) {
            case 0:
                break;
            case 1:
                c.kind = Constant::Kind::Bool;
                c.boolean = r.u8() != 0;
                break;
            case 2:
                c.kind = Constant::Kind::Num;
                c.number = r.get<double>();
                break;
            case 3:
                c.kind = Constant::Kind::Str;
                c.text = string(r.var()).value_or("");
                break;
            case 4: {
                c.kind = Constant::Kind::Import;
                const std::uint32_t iid = r.get<std::uint32_t>();
                for (std::uint32_t j = 0; j < iid >> 30; ++j) {
                    const std::uint32_t at = (iid >> (20 - 10 * j)) & 1023;
                    if (at >= p.k.size())
                        throw DecompileError(std::format("import names constant {} before it", at));
                    c.import.push_back(p.k[at].text);
                }
                break;
            }
            case 5:
                c.kind = Constant::Kind::Table;
                for (std::uint64_t keys = r.var(), j = 0; j < keys; ++j)
                    c.keys.push_back(r.var());
                break;
            case 6:
                c.kind = Constant::Kind::Closure;
                c.closure = r.var();
                break;
            case 7:
                c.kind = Constant::Kind::Vector;
                for (double& x : c.vector)
                    x = static_cast<double>(r.get<float>());
                break;
            case 8:
                c.kind = Constant::Kind::TableK;
                for (std::uint64_t entries = r.var(), j = 0; j < entries; ++j) {
                    const std::uint64_t key = r.var();
                    c.entries.emplace_back(key, r.get<std::int32_t>());
                }
                break;
            case 9: {
                c.kind = Constant::Kind::Int;
                const bool negative = r.u8() != 0;
                const auto v = static_cast<std::int64_t>(r.var());
                c.integer = negative ? -v : v;
                break;
            }
            case 10: {
                c.kind = Constant::Kind::Class;
                r.var();
                const std::uint64_t props = r.var(), methods = r.var();
                for (std::uint64_t j = 0; j < props + methods; ++j)
                    r.var();
                break;
            }
            case 11:
                c.kind = Constant::Kind::Vector;
                for (double& x : c.vector)
                    x = r.get<double>();
                break;
            default:
                throw DecompileError(std::format("constant kind {} isn't known", t));
            }
            p.k.push_back(std::move(c));
        }
        for (std::uint64_t count = r.var(), i = 0; i < count; ++i)
            p.protos.push_back(r.var());
        p.linedefined = r.var();
        if (auto name = string(r.var()); name && !name->empty())
            p.debugname = std::move(name);
        if (r.u8()) {
            const int gap = r.u8();
            const long intervals = gap < 63 ? ((static_cast<long>(n) - 1) >> gap) + 1 : 1;
            std::vector<int> small;
            std::uint8_t last = 0;
            for (std::uint64_t i = 0; i < n; ++i) {
                last = static_cast<std::uint8_t>(last + r.u8());
                small.push_back(last);
            }
            std::vector<int> absolute;
            int line = 0;
            for (long i = 0; i < intervals; ++i) {
                line += r.get<std::int32_t>();
                absolute.push_back(line);
            }
            std::vector<int> lines;
            for (std::uint64_t j = 0; j < n; ++j) {
                const std::uint64_t at = gap < 63 ? j >> gap : 0;
                if (at >= absolute.size())
                    throw DecompileError("line info ends early");
                lines.push_back(absolute[at] + small[j]);
            }
            p.lines = std::move(lines);
        }
        if (r.u8()) {
            for (std::uint64_t count = r.var(), i = 0; i < count; ++i) {
                LocVar v;
                if (auto name = string(r.var()); name && !name->empty())
                    v.name = std::move(name);
                v.start = r.var();
                v.end = r.var();
                v.reg = r.u8();
                p.locvars.push_back(std::move(v));
            }
            for (std::uint64_t count = r.var(), i = 0; i < count; ++i) {
                auto name = string(r.var());
                p.upvalnames.push_back(name && !name->empty() ? std::move(name) : std::nullopt);
            }
        }
        if (size)
            r.offset = start + *size;
        else if (version >= 11)
            for (std::uint64_t count = r.var(), i = 0; i < count; ++i) {
                r.u8();
                r.var();
            }
        return p;
    }
};

struct Insn {
    int pc = 0;
    Op op = NOP;
    int a = 0, b = 0, c = 0, d = 0, e = 0;
    std::uint32_t aux = 0;
    int size = 1;
};

std::vector<Insn> decode(const Proto& p, bool scrambled)
{
    std::vector<Insn> out;
    for (std::size_t pc = 0; pc < p.code.size();) {
        const std::uint32_t word = p.code[pc];
        const int op = scrambled ? ((word & 0xFF) * 203) & 0xFF : word & 0xFF;
        if (op >= opCount)
            throw DecompileError(std::format("opcode {} at {} isn't known", op, pc));
        Insn i;
        i.pc = static_cast<int>(pc);
        i.op = static_cast<Op>(op);
        i.a = (word >> 8) & 0xFF;
        i.b = (word >> 16) & 0xFF;
        i.c = static_cast<int>((word >> 24) & 0xFF);
        i.d = static_cast<std::int16_t>(word >> 16);
        const int e = static_cast<int>(word >> 8);
        i.e = e >= 0x800000 ? e - 0x1000000 : e;
        i.size = hasAux(i.op) ? 2 : 1;
        if (hasAux(i.op) && pc + 1 < p.code.size())
            i.aux = p.code[pc + 1];
        out.push_back(i);
        pc += static_cast<std::size_t>(i.size);
    }
    return out;
}

bool looksScrambled(const Chunk& chunk)
{
    if (chunk.main >= chunk.protos.size())
        return false;
    const Proto& main = chunk.protos[chunk.main];
    const int last = main.code.empty() ? 0 : static_cast<int>(main.code.back() & 0xFF);
    return last != static_cast<int>(RETURN) && ((last * 203) & 0xFF) == static_cast<int>(RETURN);
}

// --- expressions ------------------------------------------------------------

constexpr int unaryPrec = 7;

int precOf(std::string_view op)
{
    if (op == "or")
        return 1;
    if (op == "and")
        return 2;
    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "~=" || op == "==")
        return 3;
    if (op == "..")
        return 4;
    if (op == "+" || op == "-")
        return 5;
    if (op == "^")
        return 8;
    return 6; // * / // %
}

bool isCompare(std::string_view op)
{
    return op == "<" || op == ">" || op == "<=" || op == ">=" || op == "~=" || op == "==";
}

enum class E : std::uint8_t { Const, Var, Upval, Global, Index, MethodRef, Call, Varargs, Binop, Unop, IfExpr, Flag,
                              Closure, Table };

struct Capture {
    bool upvalue = false; // the enclosing function's upvalue `index`, or its register
    int index = 0;
    bool operator==(const Capture&) const = default;
};

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
    E kind = E::Const;
    bool multi = false; // stands for all the values a call or ... gives
    // A Const: its kind and value (a table, tablek or class constant's is `constant`).
    Constant::Kind ck = Constant::Kind::Nil;
    bool boolean = false;
    double number = 0;
    std::int64_t integer = 0;
    std::array<double, 4> vec{};
    const Constant* constant = nullptr;
    // A Var's register, an Upval's index, a Flag's ident, a Closure's function.
    int n = 0;
    // A Const's string, a Global's or MethodRef's name, a Binop's or Unop's operator.
    std::string text;
    // Index: a[b]; MethodRef: a:text; Call: a(list); Binop: a text b; Unop: text a; IfExpr: if c then a else b.
    ExprPtr a, b, c;
    std::vector<ExprPtr> list; // a Call's arguments, a Table's array part
    std::vector<std::pair<ExprPtr, ExprPtr>> hash; // a Table's
    std::vector<Capture> captures; // a Closure's
};

ExprPtr make(E kind)
{
    auto e = std::make_shared<Expr>();
    e->kind = kind;
    return e;
}

ExprPtr varE(int reg)
{
    ExprPtr e = make(E::Var);
    e->n = reg;
    return e;
}

ExprPtr nilE()
{
    return make(E::Const);
}

ExprPtr boolE(bool value)
{
    ExprPtr e = make(E::Const);
    e->ck = Constant::Kind::Bool;
    e->boolean = value;
    return e;
}

ExprPtr numE(double value)
{
    ExprPtr e = make(E::Const);
    e->ck = Constant::Kind::Num;
    e->number = value;
    return e;
}

ExprPtr strE(std::string value)
{
    ExprPtr e = make(E::Const);
    e->ck = Constant::Kind::Str;
    e->text = std::move(value);
    return e;
}

ExprPtr named(E kind, std::string name)
{
    ExprPtr e = make(kind);
    e->text = std::move(name);
    return e;
}

ExprPtr indexE(ExprPtr obj, ExprPtr key)
{
    ExprPtr e = make(E::Index);
    e->a = std::move(obj);
    e->b = std::move(key);
    return e;
}

ExprPtr methodE(ExprPtr obj, std::string name)
{
    ExprPtr e = named(E::MethodRef, std::move(name));
    e->a = std::move(obj);
    return e;
}

ExprPtr callE(ExprPtr func, std::vector<ExprPtr> args, bool multi)
{
    ExprPtr e = make(E::Call);
    e->a = std::move(func);
    e->list = std::move(args);
    e->multi = multi;
    return e;
}

ExprPtr varargsE(bool multi)
{
    ExprPtr e = make(E::Varargs);
    e->multi = multi;
    return e;
}

ExprPtr binop(std::string op, ExprPtr a, ExprPtr b)
{
    ExprPtr e = named(E::Binop, std::move(op));
    e->a = std::move(a);
    e->b = std::move(b);
    return e;
}

ExprPtr unop(std::string op, ExprPtr a)
{
    ExprPtr e = named(E::Unop, std::move(op));
    e->a = std::move(a);
    return e;
}

ExprPtr ifE(ExprPtr cond, ExprPtr a, ExprPtr b)
{
    ExprPtr e = make(E::IfExpr);
    e->c = std::move(cond);
    e->a = std::move(a);
    e->b = std::move(b);
    return e;
}

ExprPtr flagE(int ident)
{
    ExprPtr e = make(E::Flag);
    e->n = ident;
    return e;
}

ExprPtr closureE(int proto, std::vector<Capture> captures)
{
    ExprPtr e = make(E::Closure);
    e->n = proto;
    e->captures = std::move(captures);
    return e;
}

bool isBool(const ExprPtr& e)
{
    return e->kind == E::Const && e->ck == Constant::Kind::Bool;
}

bool isVar(const ExprPtr& e, int reg)
{
    return e->kind == E::Var && e->n == reg;
}

// A read of a register: a Var (multi when it stands for all of a call's values) or a closure's capture.
struct RegUse {
    int reg;
    bool multi;
};

void regs(const ExprPtr& e, std::vector<RegUse>& out)
{
    switch (e->kind) {
    case E::Var:
        out.push_back({e->n, e->multi});
        break;
    case E::Index:
    case E::Binop:
        regs(e->a, out);
        regs(e->b, out);
        break;
    case E::MethodRef:
    case E::Unop:
        regs(e->a, out);
        break;
    case E::Call:
        regs(e->a, out);
        for (const ExprPtr& x : e->list)
            regs(x, out);
        break;
    case E::IfExpr:
        regs(e->c, out);
        regs(e->a, out);
        regs(e->b, out);
        break;
    case E::Closure:
        for (const Capture& cap : e->captures)
            if (!cap.upvalue)
                out.push_back({cap.index, false});
        break;
    case E::Table:
        for (const ExprPtr& x : e->list)
            regs(x, out);
        for (const auto& [k, v] : e->hash) {
            regs(k, out);
            regs(v, out);
        }
        break;
    default:
        break;
    }
}

std::vector<RegUse> regs(const ExprPtr& e)
{
    std::vector<RegUse> out;
    regs(e, out);
    return out;
}

bool reads(const ExprPtr& e, int reg)
{
    return std::ranges::any_of(regs(e), [reg](const RegUse& u) { return u.reg == reg; });
}

bool isBoolean(const ExprPtr& e)
{
    if (e->kind == E::Binop)
        return isCompare(e->text) || ((e->text == "and" || e->text == "or") && isBoolean(e->a) && isBoolean(e->b));
    if (e->kind == E::Unop)
        return e->text == "not";
    return isBool(e);
}

// The condition that holds when `e` doesn't (truthiness kept, so De Morgan holds).
ExprPtr neg(const ExprPtr& e)
{
    if (e->kind == E::Unop && e->text == "not")
        return e->a;
    if (e->kind == E::Binop) {
        if (e->text == "==")
            return binop("~=", e->a, e->b);
        if (e->text == "~=")
            return binop("==", e->a, e->b);
        if (e->text == "and")
            return binop("or", neg(e->a), neg(e->b));
        if (e->text == "or")
            return binop("and", neg(e->a), neg(e->b));
    }
    if (isBool(e))
        return boolE(!e->boolean);
    return unop("not", e);
}

bool sameConst(const Expr& a, const Expr& b)
{
    if (a.ck != b.ck)
        return false;
    switch (a.ck) {
    case Constant::Kind::Nil:
        return true;
    case Constant::Kind::Bool:
        return a.boolean == b.boolean;
    case Constant::Kind::Num:
        return a.number == b.number;
    case Constant::Kind::Int:
        return a.integer == b.integer;
    case Constant::Kind::Str:
        return a.text == b.text;
    case Constant::Kind::Vector:
        return a.vec == b.vec;
    default:
        return a.constant == b.constant || (a.constant && b.constant && *a.constant == *b.constant);
    }
}

bool same(const ExprPtr& a, const ExprPtr& b)
{
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
    case E::Var:
        return a->n == b->n;
    case E::Const:
        return sameConst(*a, *b);
    case E::Global:
        return a->text == b->text;
    case E::Upval:
        return a->n == b->n;
    case E::Unop:
        return a->text == b->text && same(a->a, b->a);
    case E::Binop:
        return a->text == b->text && same(a->a, b->a) && same(a->b, b->b);
    case E::Index:
        return same(a->a, b->a) && same(a->b, b->b);
    default:
        return a == b;
    }
}

ExprPtr condOr(const ExprPtr& a, const ExprPtr& b)
{
    if (isBool(b))
        return b->boolean ? boolE(true) : a; // truthiness: x or false is x
    return binop("or", a, b);
}

ExprPtr condAnd(const ExprPtr& a, const ExprPtr& b)
{
    if (isBool(b))
        return b->boolean ? a : boolE(false);
    return binop("and", a, b);
}

// if c then k else other, as a condition.
ExprPtr condIf(const ExprPtr& c, const ExprPtr& k, const ExprPtr& other)
{
    if (other->kind == E::Binop && other->text == "or" && same(other->b, k))
        return binop("or", condAnd(neg(c), other->a), k);
    if (other->kind == E::Binop && other->text == "and" && same(other->b, k))
        return binop("and", condOr(c, other->a), k);
    if (same(k, other))
        return k;
    if (isBool(k))
        return k->boolean ? condOr(c, other) : condAnd(neg(c), other);
    return ifE(c, k, other);
}

// --- statements -------------------------------------------------------------

enum class S : std::uint8_t { Assign, CallStmt, Return, SetList, If, While, Repeat, Until, NumFor, GenFor, Break,
                              Continue, Comment };

struct Stmt;
using StmtPtr = std::shared_ptr<Stmt>;
using Block = std::vector<StmtPtr>;

struct Stmt {
    S kind = S::Comment;
    int pc = -1;
    int usePc = -1;
    std::optional<int> line;
    std::vector<ExprPtr> targets; // an Assign's
    std::vector<ExprPtr> values;  // an Assign's, Return's or SetList's values; a GenFor's expressions
    ExprPtr e;                    // a CallStmt's call, a SetList's table; an If's, While's, Repeat's or Until's condition
    ExprPtr start, limit, step;   // a NumFor's
    Block then;                   // an If's then-part
    std::optional<Block> other;   // an If's else-part
    Block body;                   // a loop's
    int base = 0, nvars = 0;      // a NumFor's or GenFor's registers
    std::uint32_t first = 0;      // a SetList's first index
    std::string text;             // a Comment's
    int gotoTarget = -1;          // a Comment's for a jump it didn't place
    bool declaresFlag = false;    // an Assign that starts an inline wrapper's flag
    bool inlined = false;         // a Repeat that's an inlined function's returns
};

StmtPtr makeStmt(S kind)
{
    auto s = std::make_shared<Stmt>();
    s->kind = kind;
    return s;
}

StmtPtr assignS(std::vector<ExprPtr> targets, std::vector<ExprPtr> values)
{
    StmtPtr s = makeStmt(S::Assign);
    s->targets = std::move(targets);
    s->values = std::move(values);
    return s;
}

StmtPtr ifS(ExprPtr cond, Block then, std::optional<Block> other = std::nullopt)
{
    StmtPtr s = makeStmt(S::If);
    s->e = std::move(cond);
    s->then = std::move(then);
    s->other = std::move(other);
    return s;
}

StmtPtr commentS(std::string text, int gotoTarget = -1)
{
    StmtPtr s = makeStmt(S::Comment);
    s->text = std::move(text);
    s->gotoTarget = gotoTarget;
    return s;
}

// The expressions a statement evaluates itself (not its blocks').
std::vector<ExprPtr> hostExprs(const Stmt& s)
{
    switch (s.kind) {
    case S::Assign: {
        std::vector<ExprPtr> out = s.values;
        for (const ExprPtr& t : s.targets)
            if (t->kind == E::Index) {
                out.push_back(t->a);
                out.push_back(t->b);
            }
        return out;
    }
    case S::CallStmt:
    case S::If:
    case S::Until:
        return {s.e};
    case S::Return:
    case S::GenFor:
        return s.values;
    case S::SetList: {
        std::vector<ExprPtr> out{s.e};
        out.insert(out.end(), s.values.begin(), s.values.end());
        return out;
    }
    case S::NumFor:
        return {s.start, s.limit, s.step};
    default:
        return {};
    }
}

bool isLocalTarget(const StmtPtr& s)
{
    return s->kind == S::Assign && s->targets.size() == 1 && s->targets[0]->kind == E::Var && s->values.size() == 1;
}

// A deep copy, sharing inside it what the original shares (as Python's deepcopy does).
struct Copier {
    std::unordered_map<const Expr*, ExprPtr> exprs;

    ExprPtr operator()(const ExprPtr& e)
    {
        if (!e)
            return nullptr;
        if (auto it = exprs.find(e.get()); it != exprs.end())
            return it->second;
        auto out = std::make_shared<Expr>(*e);
        exprs[e.get()] = out;
        out->a = (*this)(e->a);
        out->b = (*this)(e->b);
        out->c = (*this)(e->c);
        for (ExprPtr& x : out->list)
            x = (*this)(x);
        for (auto& [k, v] : out->hash) {
            k = (*this)(k);
            v = (*this)(v);
        }
        return out;
    }

    StmtPtr operator()(const StmtPtr& s)
    {
        auto out = std::make_shared<Stmt>(*s);
        for (ExprPtr& x : out->targets)
            x = (*this)(x);
        for (ExprPtr& x : out->values)
            x = (*this)(x);
        out->e = (*this)(s->e);
        out->start = (*this)(s->start);
        out->limit = (*this)(s->limit);
        out->step = (*this)(s->step);
        out->then = block(s->then);
        if (s->other)
            out->other = block(*s->other);
        out->body = block(s->body);
        return out;
    }

    Block block(const Block& b)
    {
        Block out;
        for (const StmtPtr& s : b)
            out.push_back((*this)(s));
        return out;
    }
};

StmtPtr copyStmt(const StmtPtr& s)
{
    return Copier{}(s);
}

Block copyBlock(const Block& b)
{
    return Copier{}.block(b);
}

void walkExprs(const ExprPtr& e, const std::function<void(const ExprPtr&)>& fn)
{
    fn(e);
    switch (e->kind) {
    case E::Index:
    case E::Binop:
        walkExprs(e->a, fn);
        walkExprs(e->b, fn);
        break;
    case E::MethodRef:
    case E::Unop:
        walkExprs(e->a, fn);
        break;
    case E::Call:
        walkExprs(e->a, fn);
        for (const ExprPtr& x : e->list)
            walkExprs(x, fn);
        break;
    case E::IfExpr:
        walkExprs(e->c, fn);
        walkExprs(e->a, fn);
        walkExprs(e->b, fn);
        break;
    case E::Table:
        for (const ExprPtr& x : e->list)
            walkExprs(x, fn);
        for (const auto& [k, v] : e->hash) {
            walkExprs(k, fn);
            walkExprs(v, fn);
        }
        break;
    default:
        break;
    }
}

// `e` with its one read of register `reg` replaced by `value`.
ExprPtr replaceVar(const ExprPtr& e, int reg, const ExprPtr& value)
{
    bool done = false;
    const std::function<ExprPtr(const ExprPtr&)> go = [&](const ExprPtr& x) -> ExprPtr {
        if (done)
            return x;
        switch (x->kind) {
        case E::Var:
            if (x->n == reg) {
                done = true;
                return value;
            }
            return x;
        case E::Index:
        case E::Binop:
            x->a = go(x->a);
            x->b = go(x->b);
            break;
        case E::MethodRef:
        case E::Unop:
            x->a = go(x->a);
            break;
        case E::Call:
            x->a = go(x->a);
            for (ExprPtr& arg : x->list)
                arg = go(arg);
            break;
        case E::IfExpr:
            x->c = go(x->c);
            x->a = go(x->a);
            x->b = go(x->b);
            break;
        case E::Table:
            for (ExprPtr& v : x->list)
                v = go(v);
            for (auto& [k, v] : x->hash) {
                k = go(k);
                v = go(v);
            }
            break;
        default:
            break;
        }
        return x;
    };
    return go(e);
}

// --- one function -----------------------------------------------------------

using Regs = std::bitset<256>;

void addReg(Regs& set, long reg)
{
    if (reg >= 0 && reg < 256)
        set.set(static_cast<std::size_t>(reg));
}

void addRange(Regs& set, long lo, long hi)
{
    for (long r = std::max(lo, 0L); r < std::min(hi, 256L); ++r)
        set.set(static_cast<std::size_t>(r));
}

// What break and continue jump to in one loop (or an inline wrapper's repeat).
struct LoopCtx {
    std::set<int> brk, cont;
    bool wrapper = false;
    ExprPtr flag;
    bool flagUsed = false;
    ExprPtr raise;
};
using Loops = std::vector<LoopCtx*>;
using Tails = std::map<int, Block>;

struct Loop {
    bool repeat = false;
    int head = 0;
    const Insn* last = nullptr;
    int exit = 0;
};

class Function {
public:
    Function(const Proto& proto, bool scrambled);

    const Block& decompile()
    {
        if (!body_)
            body_ = block(0, static_cast<int>(p.code.size()), {}, -1, {});
        return *body_;
    }

    const Proto& p;
    std::set<int> capturedRef;

private:
    const Insn* at(int pc) const
    {
        if (pc < 0 || pc >= static_cast<int>(index_.size()) || index_[ix(pc)] < 0)
            return nullptr;
        return &insns_[ix(index_[ix(pc)])];
    }

    const Insn& insnAt(int pc) const
    {
        const Insn* i = at(pc);
        if (!i)
            throw DecompileError(std::format("no instruction at {}", pc));
        return *i;
    }

    int posOf(int pc) const { return at(pc) ? index_[ix(pc)] : -1; }

    int nextOf(int pc) const
    {
        const int k = posOf(pc);
        if (k < 0)
            throw DecompileError(std::format("no instruction at {}", pc));
        return k + 1 < static_cast<int>(insns_.size()) ? insns_[ix(k + 1)].pc : static_cast<int>(p.code.size());
    }

    bool isReachable(int pc) const { return pc >= 0 && pc < static_cast<int>(reachable_.size()) && reachable_[ix(pc)]; }

    std::optional<int> line(int pc) const
    {
        if (p.lines && pc >= 0 && pc < static_cast<int>(p.lines->size()))
            return (*p.lines)[ix(pc)];
        return std::nullopt;
    }

    // -- constants

    ExprPtr k(std::uint64_t i) const
    {
        if (i >= p.k.size())
            throw DecompileError(std::format("constant {} isn't in the function", i));
        const Constant& c = p.k[i];
        if (c.kind == Constant::Kind::Import) {
            if (c.import.empty())
                throw DecompileError("an import of nothing");
            ExprPtr e = named(E::Global, c.import[0]);
            for (std::size_t j = 1; j < c.import.size(); ++j)
                e = indexE(e, strE(c.import[j]));
            return e;
        }
        if (c.kind == Constant::Kind::Closure)
            return closureE(static_cast<int>(c.closure), {});
        ExprPtr e = make(E::Const);
        e->ck = c.kind;
        e->boolean = c.boolean;
        e->number = c.number;
        e->integer = c.integer;
        e->text = c.text;
        e->vec = c.vector;
        e->constant = &c;
        return e;
    }

    std::string kstr(std::uint64_t i) const
    {
        if (i >= p.k.size())
            throw DecompileError(std::format("constant {} isn't in the function", i));
        const Constant& c = p.k[i];
        if (c.kind == Constant::Kind::Str)
            return c.text;
        if (c.kind == Constant::Kind::Num)
            return std::format("{}", c.number);
        return std::string(kindName(c.kind));
    }

    int target(const Insn& i) const
    {
        switch (i.op) {
        case JUMPX:
            return i.pc + 1 + i.e;
        case LOADB:
        case FASTCALL:
        case FASTCALL1:
        case FASTCALL2:
        case FASTCALL2K:
        case FASTCALL3:
        case FASTPCALL:
            return i.pc + 1 + i.c;
        default:
            return i.pc + 1 + i.d;
        }
    }

    // -- register reads and writes, for liveness

    // The register a multiple-value producer before `pc` starts at.
    std::optional<int> multretBase(int pc) const
    {
        const int j = std::max(posOf(pc), 0);
        for (int back = j - 1; back > std::max(j - 8, -1); --back) {
            const Insn& i = insns_[ix(back)];
            if ((i.op == CALL || i.op == CALLFB) && i.c == 0)
                return i.a;
            if (i.op == GETVARARGS && i.b == 0)
                return i.a;
        }
        return std::nullopt;
    }

    std::pair<Regs, Regs> rw(const Insn& i) const
    {
        Regs R, W;
        const int a = i.a, b = i.b, c = i.c;
        switch (i.op) {
        case LOADNIL: case LOADB: case LOADN: case LOADK: case LOADKX: case GETGLOBAL: case GETUPVAL: case GETIMPORT:
        case NEWTABLE: case DUPTABLE: case NEWCLOSURE: case DUPCLOSURE:
            addReg(W, a);
            break;
        case MOVE:
            addReg(W, a);
            addReg(R, b);
            break;
        case SETGLOBAL: case SETUPVAL:
            addReg(R, a);
            break;
        case GETTABLE:
            addReg(W, a);
            addReg(R, b);
            addReg(R, c);
            break;
        case SETTABLE:
            addReg(R, a);
            addReg(R, b);
            addReg(R, c);
            break;
        case GETTABLEKS: case GETTABLEN: case GETUDATAKS:
            addReg(W, a);
            addReg(R, b);
            break;
        case SETTABLEKS: case SETTABLEN: case SETUDATAKS:
            addReg(R, a);
            addReg(R, b);
            break;
        case NAMECALL: case NAMECALLUDATA:
            addReg(W, a);
            addReg(W, a + 1);
            addReg(R, b);
            break;
        case CALL: case CALLFB:
            if (b == 0)
                addRange(R, a, multretBase(i.pc).value_or(a) + 1);
            else
                addRange(R, a, a + b);
            if (c == 0)
                addReg(W, a);
            else
                addRange(W, a, a + c - 1);
            break;
        case RETURN:
            if (b == 0)
                addRange(R, a, multretBase(i.pc).value_or(a) + 1);
            else
                addRange(R, a, a + b - 1);
            break;
        case JUMPIF: case JUMPIFNOT: case JUMPXEQKNIL: case JUMPXEQKB: case JUMPXEQKN: case JUMPXEQKS: case CMPPROTO:
            addReg(R, a);
            break;
        case JUMPIFEQ: case JUMPIFLE: case JUMPIFLT: case JUMPIFNOTEQ: case JUMPIFNOTLE: case JUMPIFNOTLT:
            addReg(R, a);
            addReg(R, i.aux);
            break;
        case ADD: case SUB: case MUL: case DIV: case MOD: case POW: case AND: case OR: case IDIV:
            addReg(W, a);
            addReg(R, b);
            addReg(R, c);
            break;
        case ADDK: case SUBK: case MULK: case DIVK: case MODK: case POWK: case ANDK: case ORK: case IDIVK: case NOT:
        case MINUS: case LENGTH:
            addReg(W, a);
            addReg(R, b);
            break;
        case SUBRK: case DIVRK:
            addReg(W, a);
            addReg(R, c);
            break;
        case CONCAT:
            addReg(W, a);
            addRange(R, b, c + 1);
            break;
        case SETLIST:
            addReg(R, a);
            if (c == 0)
                addRange(R, b, multretBase(i.pc).value_or(b) + 1);
            else
                addRange(R, b, b + c - 1);
            break;
        case FORNPREP: case FORGPREP: case FORGPREP_INEXT: case FORGPREP_NEXT:
            addRange(R, a, a + 3);
            break;
        case FORNLOOP:
            addRange(R, a, a + 3);
            addReg(W, a + 2);
            break;
        case FORGLOOP:
            addRange(R, a, a + 3);
            addRange(W, a + 2, a + 3 + static_cast<long>(i.aux & 0xFF));
            break;
        case GETVARARGS:
            if (b == 0)
                addReg(W, a);
            else
                addRange(W, a, a + b - 1);
            break;
        case CAPTURE:
            if (a == 0 || a == 1)
                addReg(R, b);
            break;
        case NEWCLASS:
            addReg(W, a);
            if (b != 0xFF)
                addReg(R, b);
            break;
        case NEWCLASSMEMBER:
            addReg(R, a);
            addReg(R, c);
            break;
        default:
            break;
        }
        return {R, W};
    }

    std::vector<int> successors(const Insn& i) const
    {
        const int nxt = nextOf(i.pc);
        if (i.op == RETURN)
            return {};
        if (isUncondJump(i.op))
            return {target(i)};
        if (i.op == LOADB && i.c)
            return {target(i)};
        if (isCondJump(i.op) || i.op == FORNPREP || i.op == CMPPROTO || i.op == FORNLOOP || i.op == FORGLOOP)
            return {nxt, target(i)};
        if (isForPrep(i.op))
            return {target(i)};
        return {nxt};
    }

    // liveOut_[k]: the registers some instruction after the k-th reads before writing.
    void liveness()
    {
        const std::size_t n = insns_.size();
        std::vector<Regs> readsOf(n), writesOf(n);
        std::vector<std::vector<int>> succ(n), preds(n);
        for (std::size_t k = 0; k < n; ++k) {
            std::tie(readsOf[k], writesOf[k]) = rw(insns_[k]);
            for (const int s : successors(insns_[k]))
                if (const int to = posOf(s); to >= 0) {
                    succ[k].push_back(to);
                    preds[ix(to)].push_back(static_cast<int>(k));
                }
        }
        std::vector<Regs> liveIn(n);
        liveOut_.assign(n, Regs{});
        std::vector<int> work(n);
        std::vector<bool> queued(n, true);
        for (std::size_t k = 0; k < n; ++k)
            work[k] = static_cast<int>(k);
        while (!work.empty()) {
            const int k = work.back();
            work.pop_back();
            queued[ix(k)] = false;
            Regs out;
            for (const int s : succ[ix(k)])
                out |= liveIn[ix(s)];
            liveOut_[ix(k)] = out;
            const Regs in = readsOf[ix(k)] | (out & ~writesOf[ix(k)]);
            if (in != liveIn[ix(k)]) {
                liveIn[ix(k)] = in;
                for (const int q : preds[ix(k)])
                    if (!queued[ix(q)]) {
                        queued[ix(q)] = true;
                        work.push_back(q);
                    }
            }
        }
    }

    bool deadAfter(int reg, int pc) const
    {
        const int k = posOf(pc);
        if (k < 0)
            return false;
        return reg < 0 || reg >= 256 || !liveOut_[ix(k)].test(static_cast<std::size_t>(reg));
    }

    // -- loops

    void findLoops()
    {
        // numeric fors by their FORNLOOP (FORNPREP's own jump may be threaded on past the loop)
        for (const Insn& i : insns_)
            if (i.op == FORNLOOP) {
                const int k = std::max(posOf(target(i)), 0) - 1;
                if (k >= 0 && insns_[ix(k)].op == FORNPREP)
                    fornloop_[insns_[ix(k)].pc] = i.pc;
            }
        std::map<int, const Insn*> back; // a loop's head -> the last jump back to it
        for (const Insn& i : insns_)
            if (isUncondJump(i.op) || isCondJump(i.op)) {
                const int t = target(i);
                if (t <= i.pc)
                    back[t] = &i;
            }
        for (const auto& [head, last] : back)
            loops_[head] = Loop{isCondJump(last->op), head, last, nextOf(last->pc)};
    }

    // -- lifting one instruction

    // The instruction's statements, fresh each time (folding changes them; code a structure
    // covers twice, or builds again, must start from the instruction's own).
    Block lift(const Insn& i)
    {
        auto it = lifted_.find(i.pc);
        if (it == lifted_.end()) {
            Block out = liftNew(i);
            for (const StmtPtr& s : out) {
                s->pc = s->usePc = i.pc;
                s->line = line(i.pc);
            }
            it = lifted_.emplace(i.pc, std::move(out)).first;
        }
        Block copies;
        for (const StmtPtr& s : it->second)
            copies.push_back(copyStmt(s));
        return copies;
    }

    // Registers from..to-1 as Vars, then `last` as all of the values before it when it's set.
    static std::vector<ExprPtr> varRange(int from, int to, std::optional<int> last = std::nullopt)
    {
        std::vector<ExprPtr> out;
        for (int r = from; r < to; ++r)
            out.push_back(varE(r));
        if (last) {
            ExprPtr v = varE(*last);
            v->multi = true;
            out.push_back(v);
        }
        return out;
    }

    Block liftNew(const Insn& i)
    {
        const int a = i.a, b = i.b, c = i.c, d = i.d;
        const auto assign = [a](ExprPtr e) { return Block{assignS({varE(a)}, {std::move(e)})}; };
        const auto arith = [](Op op) -> std::string {
            switch (op) {
            case ADD: case ADDK: return "+";
            case SUB: case SUBK: return "-";
            case MUL: case MULK: return "*";
            case DIV: case DIVK: return "/";
            case MOD: case MODK: return "%";
            case POW: case POWK: return "^";
            default: return "//";
            }
        };
        switch (i.op) {
        case LOADNIL:
            return assign(nilE());
        case LOADB:
            return assign(boolE(b != 0));
        case LOADN:
            return assign(numE(d));
        case LOADK:
            return assign(k(static_cast<std::uint64_t>(d)));
        case LOADKX:
            return assign(k(i.aux));
        case MOVE:
            return assign(varE(b));
        case GETGLOBAL:
            return assign(named(E::Global, kstr(i.aux)));
        case SETGLOBAL:
            return {assignS({named(E::Global, kstr(i.aux))}, {varE(a)})};
        case GETUPVAL: {
            ExprPtr up = make(E::Upval);
            up->n = b;
            return assign(up);
        }
        case SETUPVAL: {
            ExprPtr up = make(E::Upval);
            up->n = b;
            return {assignS({up}, {varE(a)})};
        }
        case GETIMPORT:
            return assign(k(static_cast<std::uint64_t>(d)));
        case GETTABLE:
            return assign(indexE(varE(b), varE(c)));
        case SETTABLE:
            return {assignS({indexE(varE(b), varE(c))}, {varE(a)})};
        case GETTABLEKS:
        case GETUDATAKS:
            return assign(indexE(varE(b), k(i.op == GETUDATAKS ? i.aux & 0xFFFF : i.aux)));
        case SETTABLEKS:
        case SETUDATAKS:
            return {assignS({indexE(varE(b), k(i.op == SETUDATAKS ? i.aux & 0xFFFF : i.aux))}, {varE(a)})};
        case GETTABLEN:
            return assign(indexE(varE(b), numE(c + 1)));
        case SETTABLEN:
            return {assignS({indexE(varE(b), numE(c + 1))}, {varE(a)})};
        case NEWCLOSURE:
        case DUPCLOSURE: {
            std::uint64_t proto = 0;
            if (i.op == NEWCLOSURE) {
                if (d < 0 || static_cast<std::size_t>(d) >= p.protos.size())
                    throw DecompileError(std::format("closure {} isn't in the function", d));
                proto = p.protos[ix(d)];
            } else {
                if (d < 0 || static_cast<std::size_t>(d) >= p.k.size())
                    throw DecompileError(std::format("constant {} isn't in the function", d));
                proto = p.k[ix(d)].closure;
            }
            std::vector<Capture> captures;
            for (std::size_t j = ix(posOf(i.pc) + 1); j < insns_.size() && insns_[j].op == CAPTURE; ++j) {
                const Insn& cap = insns_[j];
                if (cap.a == 2)
                    captures.push_back({true, cap.b});
                else {
                    captures.push_back({false, cap.b});
                    if (cap.a == 1)
                        capturedRef.insert(cap.b);
                }
            }
            return assign(closureE(static_cast<int>(proto), std::move(captures)));
        }
        case NAMECALL:
        case NAMECALLUDATA:
            return assign(methodE(varE(b), kstr(i.op == NAMECALLUDATA ? i.aux & 0xFFFF : i.aux)));
        case CALL:
        case CALLFB: {
            const int first = isMethodCall(i) ? a + 2 : a + 1;
            std::vector<ExprPtr> args;
            if (b == 0) {
                const int base = multretBase(i.pc).value_or(first);
                args = varRange(first, base, base);
            } else
                args = varRange(first, a + b);
            ExprPtr call = callE(varE(a), std::move(args), c == 0);
            if (c == 1) {
                StmtPtr s = makeStmt(S::CallStmt);
                s->e = call;
                return {s};
            }
            if (c == 0 || c == 2)
                return {assignS({varE(a)}, {call})};
            return {assignS(varRange(a, a + c - 1), {call})};
        }
        case RETURN: {
            StmtPtr s = makeStmt(S::Return);
            if (b == 0) {
                const int base = multretBase(i.pc).value_or(a);
                s->values = varRange(a, base, base);
            } else
                s->values = varRange(a, a + b - 1);
            return {s};
        }
        case ADD: case SUB: case MUL: case DIV: case MOD: case POW: case IDIV:
            return assign(binop(arith(i.op), varE(b), varE(c)));
        case ADDK: case SUBK: case MULK: case DIVK: case MODK: case POWK: case IDIVK:
            return assign(binop(arith(i.op), varE(b), k(static_cast<std::uint64_t>(c))));
        case SUBRK:
            return assign(binop("-", k(static_cast<std::uint64_t>(b)), varE(c)));
        case DIVRK:
            return assign(binop("/", k(static_cast<std::uint64_t>(b)), varE(c)));
        case AND:
        case OR:
            return assign(binop(i.op == AND ? "and" : "or", varE(b), varE(c)));
        case ANDK:
        case ORK:
            return assign(binop(i.op == ANDK ? "and" : "or", varE(b), k(static_cast<std::uint64_t>(c))));
        case CONCAT: {
            ExprPtr e = varE(c);
            for (int r = c - 1; r >= b; --r)
                e = binop("..", varE(r), e);
            return assign(e);
        }
        case NOT:
            return assign(unop("not", varE(b)));
        case MINUS:
            return assign(unop("-", varE(b)));
        case LENGTH:
            return assign(unop("#", varE(b)));
        case NEWTABLE:
            return assign(make(E::Table));
        case DUPTABLE: {
            ExprPtr t = make(E::Table);
            if (d < 0 || static_cast<std::size_t>(d) >= p.k.size())
                throw DecompileError(std::format("constant {} isn't in the function", d));
            if (p.k[ix(d)].kind == Constant::Kind::TableK) // a template with values already in it
                for (const auto& [key, value] : p.k[ix(d)].entries)
                    if (value >= 0)
                        t->hash.emplace_back(k(key), k(static_cast<std::uint64_t>(value)));
            return assign(t);
        }
        case SETLIST: {
            StmtPtr s = makeStmt(S::SetList);
            s->e = varE(a);
            if (c == 0) {
                const int base = multretBase(i.pc).value_or(b);
                s->values = varRange(b, base, base);
            } else
                s->values = varRange(b, b + c - 1);
            s->first = i.aux;
            return {s};
        }
        case GETVARARGS:
            if (b == 0)
                return assign(varargsE(true));
            if (b == 1)
                return {};
            if (b == 2)
                return assign(varargsE(false));
            return {assignS(varRange(a, a + b - 1), {varargsE(true)})};
        case NEWCLASS:
            return {commentS("decompiler: NEWCLASS isn't decompiled")};
        case NEWCLASSMEMBER:
            return {commentS("decompiler: NEWCLASSMEMBER isn't decompiled")};
        default:
            return {};
        }
    }

    bool isMethodCall(const Insn& call) const
    {
        for (int j = posOf(call.pc) - 1; j >= 0; --j) {
            const Insn& i = insns_[ix(j)];
            if (rw(i).second.test(static_cast<std::size_t>(call.a)))
                return i.op == NAMECALL || i.op == NAMECALLUDATA;
            if (isUncondJump(i.op) || isCondJump(i.op) || i.op == RETURN)
                return false;
        }
        return false;
    }

    // The condition under which the conditional jump is taken.
    ExprPtr jumpCond(const Insn& i) const
    {
        ExprPtr A = varE(i.a);
        switch (i.op) {
        case JUMPIF:
            return A;
        case JUMPIFNOT:
            return unop("not", A);
        case JUMPIFEQ:
            return binop("==", A, varE(static_cast<int>(i.aux)));
        case JUMPIFNOTEQ:
            return binop("~=", A, varE(static_cast<int>(i.aux)));
        case JUMPIFLE:
            return binop("<=", A, varE(static_cast<int>(i.aux)));
        case JUMPIFLT:
            return binop("<", A, varE(static_cast<int>(i.aux)));
        case JUMPIFNOTLE:
            return unop("not", binop("<=", A, varE(static_cast<int>(i.aux))));
        case JUMPIFNOTLT:
            return unop("not", binop("<", A, varE(static_cast<int>(i.aux))));
        default:
            break;
        }
        const bool flip = (i.aux >> 31) != 0;
        ExprPtr kv;
        if (i.op == JUMPXEQKNIL)
            kv = nilE();
        else if (i.op == JUMPXEQKB)
            kv = boolE((i.aux & 1) != 0);
        else
            kv = k(i.aux & 0xFFFFFF);
        return binop(flip ? "~=" : "==", A, kv);
    }

    // -- structure (see the tool's notes on how it's read off pc ranges)

    std::optional<Block> tailReturn(int t, int lo, int hi)
    {
        const Insn* i = at(t);
        if (!i || i->op != RETURN || (lo <= t && t < hi) || i->b == 0)
            return std::nullopt;
        Block out;
        for (const StmtPtr& s : lift(*i))
            out.push_back(copyStmt(s));
        return out;
    }

    // `t` and where unconditional jumps from it lead: one place, as far as control goes.
    std::set<int> threads(int t) const
    {
        std::set<int> out{t};
        for (int steps = 0; steps < 64 && out.size() < 64; ++steps) {
            const Insn* i = at(t);
            if (!i || (i->op != JUMP && i->op != JUMPX))
                break;
            t = target(*i);
            out.insert(t);
        }
        return out;
    }

    StmtPtr mark(StmtPtr s, int pc) const
    {
        s->pc = s->usePc = pc;
        s->line = line(pc);
        return s;
    }

    // break or continue for a jump to `t`, if it's one of the innermost loop's; out of loops to
    // an inline wrapper (an inlined function returning from a loop): set its flag and break.
    std::optional<Block> loopJump(int t, const Loops& loops, int pc) const
    {
        LoopCtx* ctx = loops.empty() ? nullptr : loops.back();
        if (ctx && ctx->brk.contains(t))
            return Block{mark(makeStmt(S::Break), pc)};
        if (ctx && ctx->cont.contains(t))
            return Block{mark(makeStmt(S::Continue), pc)};
        for (int k = static_cast<int>(loops.size()) - 2; k >= 0; --k) {
            LoopCtx* outer = loops[ix(k)];
            if (outer->wrapper && outer->brk.contains(t)) {
                for (std::size_t inner = ix(k + 1); inner < loops.size(); ++inner)
                    loops[inner]->raise = outer->flag;
                outer->flagUsed = true;
                return Block{mark(assignS({outer->flag}, {boolE(true)}), pc), mark(makeStmt(S::Break), pc)};
            }
            if (outer->wrapper)
                break;
        }
        return std::nullopt;
    }

    // Every jump target loopJump turns into a break or continue.
    static std::set<int> loopTargets(const Loops& loops)
    {
        if (loops.empty())
            return {};
        std::set<int> out = loops.back()->brk;
        out.insert(loops.back()->cont.begin(), loops.back()->cont.end());
        for (int k = static_cast<int>(loops.size()) - 2; k >= 0; --k)
            if (loops[ix(k)]->wrapper) {
                out.insert(loops[ix(k)]->brk.begin(), loops[ix(k)]->brk.end());
                break;
            }
        return out;
    }

    // After a loop an inlined function's return left from: on out of the next loop too.
    void afterLoop(Block& stmts, const LoopCtx& ctx) const
    {
        if (ctx.raise) {
            const int pc = stmts.back()->pc;
            stmts.push_back(mark(ifS(ctx.raise, {mark(makeStmt(S::Break), pc)}), pc));
        }
    }

    static Loops with(const Loops& loops, LoopCtx* ctx)
    {
        Loops out = loops;
        out.push_back(ctx);
        return out;
    }

    Block block(int lo, int hi, const Loops& loops, int skipLoop, const std::set<int>& ends, const Tails& tails = {})
    {
        Block stmts;
        int pc = lo;
        while (pc < hi) {
            const Insn* ip = at(pc);
            if (!ip) {
                ++pc;
                continue;
            }
            if (!isReachable(pc)) {
                pc = nextOf(pc);
                continue;
            }
            const Insn& i = *ip;
            if (auto loop = loops_.find(pc); loop != loops_.end() && pc != skipLoop && loop->second.last->pc < hi) {
                LoopCtx ctx;
                stmts.push_back(this->loop(loop->second, loops, ctx));
                afterLoop(stmts, ctx);
                pc = loop->second.exit;
                continue;
            }
            if (auto fl = fornloop_.find(pc); i.op == FORNPREP && fl != fornloop_.end() && fl->second < hi) {
                const int loopPc = fl->second;
                const int exitPc = nextOf(loopPc);
                LoopCtx ctx;
                ctx.brk = threads(exitPc);
                ctx.brk.merge(threads(target(i)));
                ctx.cont = {loopPc};
                StmtPtr s = makeStmt(S::NumFor);
                s->body = block(nextOf(pc), loopPc, with(loops, &ctx), -1, {});
                s->base = i.a;
                s->start = varE(i.a + 2);
                s->limit = varE(i.a);
                s->step = varE(i.a + 1);
                stmts.push_back(mark(s, pc));
                afterLoop(stmts, ctx);
                pc = exitPc;
                continue;
            }
            if (isForPrep(i.op) && at(target(i)) && nextOf(target(i)) <= hi) {
                const int loopPc = target(i);
                const Insn& li = insnAt(loopPc);
                if (li.op != FORGLOOP) {
                    stmts.push_back(mark(commentS(std::format("decompiler: {} at {} without its FORGLOOP",
                                                              opName(i.op), pc)),
                                         pc));
                    pc = nextOf(pc);
                    continue;
                }
                const int exitPc = nextOf(loopPc);
                LoopCtx ctx;
                ctx.brk = threads(exitPc);
                ctx.cont = {loopPc};
                StmtPtr s = makeStmt(S::GenFor);
                s->body = block(nextOf(pc), loopPc, with(loops, &ctx), -1, {});
                s->base = i.a;
                s->nvars = static_cast<int>(li.aux & 0xFF);
                s->values = {varE(i.a), varE(i.a + 1), varE(i.a + 2)};
                stmts.push_back(mark(s, pc));
                afterLoop(stmts, ctx);
                pc = exitPc;
                continue;
            }
            if (isCondJump(i.op)) {
                pc = condition(i, lo, hi, loops, stmts, ends, tails);
                continue;
            }
            if (isUncondJump(i.op) || (i.op == LOADB && i.c)) {
                if (i.op == LOADB)
                    append(stmts, lift(i));
                const int t = target(i);
                if (t == hi || ends.contains(t) || t == nextOf(pc)) {
                } else if (auto lj = loopJump(t, loops, pc))
                    append(stmts, std::move(*lj));
                else if (auto ret = tailReturn(t, lo, hi))
                    append(stmts, std::move(*ret));
                else if (auto tail = tails.find(t); tail != tails.end())
                    append(stmts, copyBlock(tail->second));
                else
                    stmts.push_back(mark(commentS(std::format("decompiler: goto {}", t), t), pc));
                pc = nextOf(pc);
                continue;
            }
            if (i.op == FORNLOOP || i.op == FORGLOOP) {
                stmts.push_back(mark(commentS(std::format("decompiler: stray {} at {}", opName(i.op), pc)), pc));
                pc = nextOf(pc);
                continue;
            }
            if (i.op == CAPTURE || isSkipped(i.op)) {
                pc = nextOf(pc);
                continue;
            }
            append(stmts, lift(i));
            pc = nextOf(pc);
        }
        stmts = wrapExits(std::move(stmts), lo, hi, loops);
        return tidy(std::move(stmts));
    }

    static void append(Block& to, Block from)
    {
        to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
    }

    static std::string_view opName(Op op)
    {
        static constexpr std::array<std::string_view, opCount> names{
            "NOP", "BREAK", "LOADNIL", "LOADB", "LOADN", "LOADK", "MOVE", "GETGLOBAL", "SETGLOBAL", "GETUPVAL",
            "SETUPVAL", "CLOSEUPVALS", "GETIMPORT", "GETTABLE", "SETTABLE", "GETTABLEKS", "SETTABLEKS",
            "GETTABLEN", "SETTABLEN", "NEWCLOSURE", "NAMECALL", "CALL", "RETURN", "JUMP", "JUMPBACK", "JUMPIF",
            "JUMPIFNOT", "JUMPIFEQ", "JUMPIFLE", "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT", "ADD",
            "SUB", "MUL", "DIV", "MOD", "POW", "ADDK", "SUBK", "MULK", "DIVK", "MODK", "POWK", "AND", "OR", "ANDK",
            "ORK", "CONCAT", "NOT", "MINUS", "LENGTH", "NEWTABLE", "DUPTABLE", "SETLIST", "FORNPREP", "FORNLOOP",
            "FORGLOOP", "FORGPREP_INEXT", "FASTCALL3", "FORGPREP_NEXT", "NATIVECALL", "GETVARARGS", "DUPCLOSURE",
            "PREPVARARGS", "LOADKX", "JUMPX", "FASTCALL", "COVERAGE", "CAPTURE", "SUBRK", "DIVRK", "FASTCALL1",
            "FASTCALL2", "FASTCALL2K", "FORGPREP", "JUMPXEQKNIL", "JUMPXEQKB", "JUMPXEQKN", "JUMPXEQKS", "IDIV",
            "IDIVK", "GETUDATAKS", "SETUDATAKS", "NAMECALLUDATA", "NEWCLASSMEMBER", "CALLFB", "CMPPROTO",
            "FASTPCALL", "NEWCLASS",
        };
        return names[static_cast<std::size_t>(op)];
    }

    // The targets of the gotos left in `stmts` (not inside loops: a break there means the loop).
    static void unresolved(const Block& stmts, std::set<int>& out)
    {
        for (const StmtPtr& s : stmts) {
            if (s->kind == S::Comment && s->gotoTarget >= 0)
                out.insert(s->gotoTarget);
            else if (s->kind == S::If) {
                unresolved(s->then, out);
                if (s->other)
                    unresolved(*s->other, out);
            } else if (s->kind == S::While || s->kind == S::Repeat || s->kind == S::NumFor || s->kind == S::GenFor)
                unresolved(s->body, out);
        }
    }

    // An inlined function's returns jump past the rest of it: repeat ... until true, break for them.
    Block wrapExits(Block stmts, int lo, int hi, const Loops& loops)
    {
        for (int round = 0; round < 8; ++round) {
            bool changed = false;
            for (std::size_t k = 0; k < stmts.size() && !changed; ++k) {
                const StmtPtr s = stmts[k];
                std::set<int> targets;
                unresolved({s}, targets);
                for (const int T : targets) {
                    if (!(s->pc < T && T <= hi))
                        continue;
                    std::size_t j = k + 1;
                    while (j < stmts.size() && stmts[j]->pc < T)
                        ++j;
                    if (T != hi && j == stmts.size())
                        continue;
                    const int start = s->pc;
                    if (start < lo || leavesLoop(start, T, loops))
                        continue;
                    LoopCtx wrapper;
                    wrapper.brk = threads(T);
                    wrapper.wrapper = true;
                    wrapper.flag = flagE(flags_);
                    StmtPtr rep = makeStmt(S::Repeat);
                    rep->body = block(start, T, with(loops, &wrapper), -1, {});
                    rep->e = boolE(true);
                    rep->inlined = true;
                    Block added{mark(rep, start)};
                    if (wrapper.flagUsed) {
                        ++flags_;
                        StmtPtr init = assignS({wrapper.flag}, {boolE(false)});
                        init->declaresFlag = true;
                        added.insert(added.begin(), mark(init, start));
                    }
                    Block out(stmts.begin(), stmts.begin() + static_cast<long>(k));
                    append(out, std::move(added));
                    out.insert(out.end(), stmts.begin() + static_cast<long>(j), stmts.end());
                    stmts = std::move(out);
                    changed = true;
                    break;
                }
            }
            if (!changed)
                break;
        }
        return stmts;
    }

    // Whether some jump in [lo, hi) is the enclosing loop's break or continue (a wrapper would take it).
    bool leavesLoop(int lo, int hi, const Loops& loops) const
    {
        if (loops.empty())
            return false;
        const LoopCtx& ctx = *loops.back();
        for (std::size_t k = ix(std::max(posOf(lo), 0)); k < insns_.size() && insns_[k].pc < hi; ++k) {
            const Insn& i = insns_[k];
            if ((isUncondJump(i.op) || isCondJump(i.op) || (i.op == LOADB && i.c))
                && (ctx.brk.contains(target(i)) || ctx.cont.contains(target(i))))
                return true;
        }
        return false;
    }

    StmtPtr loop(const Loop& loop, const Loops& loops, LoopCtx& ctx)
    {
        const int h = loop.head;
        const Insn& last = *loop.last;
        StmtPtr s;
        if (!loop.repeat) {
            ctx.brk = threads(loop.exit);
            ctx.cont = {h, last.pc};
            Block body = block(h, last.pc, with(loops, &ctx), h, {});
            ExprPtr cond = boolE(true);
            if (!body.empty() && body[0]->kind == S::If && !body[0]->other && body[0]->then.size() == 1
                && body[0]->then[0]->kind == S::Break) {
                cond = neg(body[0]->e);
                body.erase(body.begin());
            }
            s = makeStmt(S::While);
            s->e = cond;
            s->body = std::move(body);
        } else {
            ctx.brk = threads(loop.exit);
            ctx.cont = {last.pc};
            Block body = block(h, last.pc, with(loops, &ctx), h, {});
            StmtPtr until = makeStmt(S::Until);
            until->e = neg(jumpCond(last));
            until->pc = until->usePc = last.pc;
            body.push_back(until);
            body = tidy(std::move(body));
            ExprPtr cond = boolE(true);
            if (!body.empty() && body.back()->kind == S::Until) {
                cond = body.back()->e;
                body.pop_back();
            }
            s = makeStmt(S::Repeat);
            s->e = cond;
            s->body = std::move(body);
        }
        return mark(s, h);
    }

    // The statements from lo up to the conditional jump, folded into its condition; null if some stay.
    ExprPtr segment(int lo, const Insn& jump)
    {
        Block stmts;
        for (int pc = lo; pc < jump.pc; pc = nextOf(pc)) {
            const Insn& i = insnAt(pc);
            if (endsSegment(i.op) || (i.op == LOADB && i.c) || loops_.contains(pc))
                return nullptr;
            if (i.op != CAPTURE && !isSkipped(i.op))
                for (const StmtPtr& s : lift(i))
                    stmts.push_back(copyStmt(s));
        }
        StmtPtr host = makeStmt(S::Until);
        host->e = jumpCond(jump);
        host->pc = host->usePc = jump.pc;
        stmts.push_back(host);
        const Block folded = fold(std::move(stmts));
        if (folded.size() != 1)
            return nullptr;
        return folded[0]->e;
    }

    // A chain of conditional jumps from `first` (and/or): an If; returns where to go on.
    int condition(const Insn& first, int lo, int hi, const Loops& loops, Block& stmts, const std::set<int>& ends,
                  const Tails& tails)
    {
        struct Seg {
            int start;
            ExprPtr cond;
            int target;
        };
        std::vector<Seg> segs{{first.pc, jumpCond(first), target(first)}};
        std::vector<int> endsAt{nextOf(first.pc)};
        std::vector<ExprPtr> folded{nullptr};
        int pc = endsAt.back();
        while (pc < hi) {
            // the next conditional jump, with only computations to fold into it before
            int j = pc;
            while (j < hi && at(j) && !loops_.contains(j) && !endsSegment(at(j)->op))
                j = nextOf(j);
            if (j >= hi || !at(j) || !isCondJump(at(j)->op) || loops_.contains(j))
                break;
            const Insn& jump = *at(j);
            if (target(jump) <= jump.pc)
                break;
            ExprPtr cond = segment(pc, jump);
            if (!cond)
                break;
            segs.push_back({pc, nullptr, target(jump)});
            folded.push_back(cond);
            endsAt.push_back(nextOf(j));
            pc = endsAt.back();
        }
        const std::set<int> loopT = loopTargets(loops);
        const auto canon = [&](int t) { return ends.contains(t) ? hi : t; };

        for (int n = static_cast<int>(segs.size()); n > 0; --n) {
            const int N = endsAt[ix(n - 1)];
            std::vector<int> starts;
            for (int idx = 0; idx < n; ++idx)
                starts.push_back(segs[ix(idx)].start);
            std::set<int> external;
            for (int idx = 0; idx < n; ++idx) {
                const int t = canon(segs[ix(idx)].target);
                if (t == N || std::find(starts.begin() + idx + 1, starts.end(), t) != starts.end())
                    continue;
                external.insert(t);
            }
            if (external.empty())
                external = {N};
            if (external.size() != 1)
                continue;
            const int F = *external.begin();
            const Insn* fi = at(F);
            const bool usable = (starts[0] < F && F <= hi) || loopT.contains(F) || tails.contains(F)
                                || (fi && fi->op == RETURN && !(lo <= F && F < hi));
            if (!usable)
                continue;

            std::map<int, ExprPtr> memo;
            std::function<ExprPtr(int)> G;
            const auto value = [&](int t) -> ExprPtr {
                t = canon(t);
                if (t == F)
                    return boolE(true);
                if (t == N)
                    return boolE(false);
                const auto it = std::ranges::find(starts, t);
                if (it == starts.end())
                    throw DecompileError(std::format("a condition jumps to {}, inside none of its parts", t));
                return G(static_cast<int>(it - starts.begin()));
            };
            G = [&](int idx) -> ExprPtr {
                if (auto it = memo.find(idx); it != memo.end())
                    return it->second;
                const ExprPtr& cond = idx == 0 ? segs[0].cond : folded[ix(idx)];
                const ExprPtr nxt = idx + 1 < n ? value(starts[ix(idx + 1)]) : boolE(false);
                ExprPtr out = condIf(cond, value(segs[ix(idx)].target), nxt);
                memo[idx] = out;
                return out;
            };
            return makeIf(G(0), F, N, lo, hi, loops, stmts, first.pc, ends, tails);
        }
        stmts.push_back(mark(commentS(std::format("decompiler: unstructured jump to {}", target(first)), target(first)),
                             first.pc));
        return nextOf(first.pc);
    }

    int makeIf(const ExprPtr& E_, int F, int N, int lo, int hi, const Loops& loops, Block& stmts, int pc0,
               const std::set<int>& ends, const Tails& tails)
    {
        const std::set<int> loopT = loopTargets(loops);
        const auto canon = [&](int t) { return ends.contains(t) ? hi : t; };
        const auto subEnds = [&](int end, std::optional<int> raw) {
            std::set<int> out = threads(end);
            if (raw)
                out.merge(threads(*raw));
            if (end == hi)
                out.insert(ends.begin(), ends.end());
            return out;
        };
        const auto subTails = [&](int end) { return end == hi ? tails : Tails{}; };

        if (tails.contains(F) && !(N < F && F <= hi)) {
            // a jump into the else-part of the if this block is the then-part of: that else-part,
            // and the rest of this block otherwise
            Block rest = block(N, hi, loops, -1, ends, tails);
            stmts.push_back(mark(ifS(E_, copyBlock(tails.at(F)), std::move(rest)), pc0));
            return hi;
        }
        if (!(N < F && F <= hi)) {
            if (F == N)
                return N;
            if (auto lj = loopJump(F, loops, pc0)) {
                stmts.push_back(mark(ifS(E_, std::move(*lj)), pc0));
                return N;
            }
            if (auto ret = tailReturn(F, lo, hi)) {
                stmts.push_back(mark(ifS(E_, tidy(std::move(*ret))), pc0));
                return N;
            }
            stmts.push_back(mark(commentS(std::format("decompiler: unstructured jump to {}", F), F), pc0));
            return N;
        }
        // if not E then [N, F) [else [F, E2)] end
        const std::optional<int> lastPc = lastInsnBefore(F, N);
        if (lastPc && isCondJump(insnAt(*lastPc).op)) {
            // c and a or b: the then-part ends testing what it set and jumps over the else-part
            const Insn& li = insnAt(*lastPc);
            const int raw = target(li);
            const int E2 = canon(raw);
            if (F < E2 && E2 <= hi && !loopT.contains(E2)) {
                Block then = block(N, *lastPc, loops, -1, {});
                Block other = block(F, E2, loops, -1, subEnds(E2, raw));
                const ExprPtr jc = jumpCond(li);
                const auto t = singleAssign(then);
                const auto o = singleAssign(other);
                if (t && o && t->first == o->first && isVar(jc, t->first)) {
                    StmtPtr a = assignS({varE(t->first)}, {binop("or", binop("and", neg(E_), t->second), o->second)});
                    a->pc = pc0;
                    a->usePc = *lastPc;
                    a->line = line(pc0);
                    stmts.push_back(a);
                    return E2;
                }
                // in general: the else-part runs on both ways it's reached
                StmtPtr inner = ifS(neg(jc), copyBlock(other));
                inner->pc = inner->usePc = *lastPc;
                inner->line = line(*lastPc);
                then.push_back(inner);
                then = tidy(std::move(then));
                stmts.push_back(mark(ifS(neg(E_), std::move(then), std::move(other)), pc0));
                return E2;
            }
        }
        if (lastPc) {
            const Insn& li = insnAt(*lastPc);
            if (li.op == JUMP || li.op == JUMPX || (li.op == LOADB && li.c)) {
                const int raw = target(li);
                const int E2 = canon(raw);
                if (F < E2 && E2 <= hi && (E2 == hi || !loopT.contains(E2))) {
                    Block other = block(F, E2, loops, -1, subEnds(E2, raw), subTails(E2));
                    std::set<int> thenEnds = subEnds(E2, raw);
                    thenEnds.insert(*lastPc);
                    Tails thenTails = subTails(E2);
                    for (const int t : threads(F))
                        thenTails[t] = other;
                    Block then = block(N, *lastPc, loops, -1, thenEnds, thenTails);
                    if (li.op == LOADB) {
                        append(then, lift(li));
                        then = tidy(std::move(then));
                    }
                    stmts.push_back(mark(ifS(neg(E_), std::move(then), std::move(other)), pc0));
                    return E2;
                }
            }
        }
        Block then = block(N, F, loops, -1, subEnds(F, std::nullopt), subTails(F));
        stmts.push_back(mark(ifS(neg(E_), std::move(then)), pc0));
        return F;
    }

    std::optional<int> lastInsnBefore(int end, int lo) const
    {
        int k = static_cast<int>(std::ranges::lower_bound(insns_, end, {}, &Insn::pc) - insns_.begin()) - 1;
        // dead code the compiler left behind ends nothing, but a dead jump over an else-part
        // still says where the then-part ends
        while (k >= 0 && insns_[ix(k)].pc >= lo && !isReachable(insns_[ix(k)].pc) && insns_[ix(k)].op != JUMP
               && insns_[ix(k)].op != JUMPX)
            --k;
        if (k >= 0 && insns_[ix(k)].pc >= lo)
            return insns_[ix(k)].pc;
        return std::nullopt;
    }

    // -- folding temporaries into what reads them, and other tidying

    Block tidy(Block stmts) { return fold(valueIfs(std::move(stmts))); }

    // if c then r = a else r = b end -> r = if c then a else b; r = a; if r then r = b end -> r = a and b.
    Block valueIfs(Block stmts) const
    {
        Block out;
        for (StmtPtr& s : stmts) {
            if (s->kind == S::If) {
                const auto t = singleAssign(s->then);
                const auto o = s->other ? singleAssign(*s->other) : std::nullopt;
                if (t && o && t->first == o->first) {
                    const ExprPtr& a = t->second;
                    const ExprPtr& b = o->second;
                    ExprPtr value;
                    if (isBool(a) && isBool(b)) {
                        if (a->boolean && !b->boolean)
                            value = isBoolean(s->e)        ? s->e
                                    : isBoolean(neg(s->e)) ? unop("not", neg(s->e))
                                                           : ifE(s->e, a, b);
                        else if (b->boolean && !a->boolean)
                            value = isBoolean(neg(s->e)) ? neg(s->e) : unop("not", s->e);
                        else
                            value = ifE(s->e, a, b);
                    } else
                        value = ifE(s->e, a, b);
                    StmtPtr assigned = assignS({varE(t->first)}, {value});
                    assigned->pc = s->pc;
                    assigned->usePc = s->usePc;
                    assigned->line = s->line;
                    out.push_back(assigned);
                    continue;
                }
                if (t && !s->other && !out.empty() && out.back()->kind == S::Assign) {
                    Stmt& prev = *out.back();
                    if (prev.targets.size() == 1 && isVar(prev.targets[0], t->first) && prev.values.size() == 1) {
                        const ExprPtr& c = s->e;
                        if (reads(t->second, t->first)) {
                            out.push_back(s);
                            continue;
                        }
                        if (isVar(c, t->first)) {
                            prev.values = {binop("and", prev.values[0], t->second)};
                            prev.usePc = s->usePc;
                            continue;
                        }
                        if (c->kind == E::Unop && c->text == "not" && isVar(c->a, t->first)) {
                            prev.values = {binop("or", prev.values[0], t->second)};
                            prev.usePc = s->usePc;
                            continue;
                        }
                    }
                }
            }
            out.push_back(s);
        }
        return out;
    }

    static std::optional<std::pair<int, ExprPtr>> singleAssign(const Block& block)
    {
        if (block.size() != 1)
            return std::nullopt;
        if (isLocalTarget(block[0]))
            return std::pair{block[0]->targets[0]->n, block[0]->values[0]};
        return std::nullopt;
    }

    Block fold(Block stmts)
    {
        Block out;
        for (StmtPtr& s : stmts) {
            popInto(s, out);
            if (intoTable(s, out))
                continue;
            out.push_back(s);
        }
        return out;
    }

    static int readsOf(const Stmt& s, int reg)
    {
        int n = 0;
        for (const ExprPtr& e : hostExprs(s))
            for (const RegUse& u : regs(e))
                n += u.reg == reg;
        return n;
    }

    void popInto(const StmtPtr& s, Block& out)
    {
        std::set<int> forced;
        if (s->kind == S::NumFor)
            forced = {s->base, s->base + 1, s->base + 2};
        else if (s->kind == S::GenFor) {
            forced = {s->base, s->base + 1, s->base + 2};
            if (!out.empty() && out.back()->kind == S::Assign && out.back()->values.size() == 1
                && out.back()->targets.size() == 3) {
                const Stmt& last = *out.back();
                bool matches = true;
                for (int k = 0; k < 3; ++k)
                    matches = matches && isVar(last.targets[ix(k)], s->base + k);
                if (matches) {
                    ExprPtr call = last.values[0];
                    out.pop_back();
                    call->multi = true;
                    s->values = {call};
                    return;
                }
            }
        }
        while (!out.empty()) {
            const StmtPtr last = out.back();
            if (!isLocalTarget(last))
                return;
            const int reg = last->targets[0]->n;
            const ExprPtr value = last->values[0];
            for (const ExprPtr& e : hostExprs(*s))
                if (captures(e, reg))
                    return;
            if (captures(value, reg))
                return;
            if (value->kind == E::Table && fills(*s, reg))
                return;
            if (readsOf(*s, reg) != 1)
                return;
            if (!forced.contains(reg) && !deadAfter(reg, s->usePc) && !redefined(*s, reg))
                return;
            // where the read sits wants one value or all of them
            if (!place(*s, reg, value))
                return;
            out.pop_back();
        }
    }

    // Whether `s` sets a field or the array of the table in `reg` (intoTable's to fold).
    static bool fills(const Stmt& s, int reg)
    {
        if (s.kind == S::SetList)
            return isVar(s.e, reg);
        return s.kind == S::Assign && s.targets.size() == 1 && s.targets[0]->kind == E::Index
               && isVar(s.targets[0]->a, reg);
    }

    // Whether a closure in `e` captures register `reg`.
    static bool captures(const ExprPtr& e, int reg)
    {
        bool found = false;
        walkExprs(e, [&](const ExprPtr& x) {
            if (x->kind == E::Closure
                && std::ranges::find(x->captures, Capture{false, reg}) != x->captures.end())
                found = true;
        });
        return found;
    }

    static bool redefined(const Stmt& s, int reg)
    {
        return s.kind == S::Assign && std::ranges::any_of(s.targets, [reg](const ExprPtr& t) { return isVar(t, reg); });
    }

    // Puts `value` where `s` reads `reg`; false if it can't go there.
    static bool place(Stmt& s, int reg, ExprPtr value)
    {
        bool multiOk = false;
        for (const ExprPtr& e : hostExprs(s))
            for (const RegUse& u : regs(e))
                if (u.reg == reg && u.multi)
                    multiOk = true;
        if (value->multi && !multiOk) {
            if (value->kind == E::Call)
                value = callE(value->a, value->list, false);
            else if (value->kind == E::Varargs)
                value = varargsE(false);
        }
        if (multiOk && !value->multi)
            return false;
        const auto sub = [&](const ExprPtr& e) { return isVar(e, reg) ? value : replaceVar(e, reg, value); };
        switch (s.kind) {
        case S::Assign:
            for (ExprPtr& e : s.values)
                e = sub(e);
            for (const ExprPtr& t : s.targets)
                if (t->kind == E::Index) {
                    t->a = sub(t->a);
                    t->b = sub(t->b);
                }
            return true;
        case S::CallStmt:
        case S::If:
        case S::Until:
            s.e = sub(s.e);
            return true;
        case S::Return:
        case S::GenFor:
            for (ExprPtr& e : s.values)
                e = sub(e);
            return true;
        case S::SetList:
            s.e = sub(s.e);
            for (ExprPtr& e : s.values)
                e = sub(e);
            return true;
        case S::NumFor:
            s.start = sub(s.start);
            s.limit = sub(s.limit);
            s.step = sub(s.step);
            return true;
        default:
            return false;
        }
    }

    // t = {}; t.k = v -> t = {k = v} (and SETLIST into its array).
    static bool intoTable(const StmtPtr& s, Block& out)
    {
        if (out.empty())
            return false;
        const Stmt& last = *out.back();
        if (!isLocalTarget(out.back()) || last.values[0]->kind != E::Table)
            return false;
        const int reg = last.targets[0]->n;
        Expr& table = *last.values[0];
        if (s->kind == S::Assign && s->targets.size() == 1 && s->targets[0]->kind == E::Index) {
            const ExprPtr& t = s->targets[0];
            if (isVar(t->a, reg) && !reads(t->b, reg) && !reads(s->values[0], reg) && !captures(s->values[0], reg)) {
                for (auto& [key, v] : table.hash)
                    if (same(key, t->b) && key->kind == E::Const) {
                        v = s->values[0];
                        return true;
                    }
                table.hash.emplace_back(t->b, s->values[0]);
                return true;
            }
        }
        if (s->kind == S::SetList && isVar(s->e, reg) && s->first == table.list.size() + 1
            && std::ranges::none_of(s->values, [reg](const ExprPtr& e) { return reads(e, reg); })) {
            table.list.insert(table.list.end(), s->values.begin(), s->values.end());
            return true;
        }
        return false;
    }

    std::vector<Insn> insns_;
    std::vector<int> index_; // pc -> its instruction in insns_ (-1 for an aux word)
    std::vector<Regs> liveOut_;
    std::map<int, int> fornloop_; // FORNPREP pc -> its FORNLOOP's
    std::map<int, Loop> loops_;   // head pc -> loop
    std::vector<bool> reachable_;
    std::map<int, Block> lifted_;
    int flags_ = 0; // inline wrappers' flags so far
    std::optional<Block> body_;
};

// --- printing ---------------------------------------------------------------

std::string pad(int indent)
{
    return std::string(static_cast<std::size_t>(std::max(indent, 0)) * 4, ' ');
}

// Whether `s` is valid UTF-8 whose characters all print (Python's str.isprintable, near enough).
bool printableUtf8(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();) {
        const auto c0 = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if (c0 < 0x80) {
            cp = c0;
            len = 1;
        } else if (c0 >= 0xC2 && c0 < 0xE0) {
            cp = c0 & 0x1F;
            len = 2;
        } else if (c0 >= 0xE0 && c0 < 0xF0) {
            cp = c0 & 0x0F;
            len = 3;
        } else if (c0 >= 0xF0 && c0 < 0xF5) {
            cp = c0 & 0x07;
            len = 4;
        } else
            return false;
        if (i + len > s.size())
            return false;
        for (std::size_t j = 1; j < len; ++j) {
            const auto cj = static_cast<unsigned char>(s[i + j]);
            if ((cj & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cj & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000))
            return false;
        i += len;
        if (cp == '\n' || cp == '\t' || cp == '\r')
            continue;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0xA0) || cp == 0xAD || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200F)
            || (cp >= 0x2028 && cp <= 0x202F) || (cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF
            || (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xFFF9 && cp <= 0xFFFB) || cp >= 0xF0000)
            return false;
    }
    return true;
}

std::string luaString(std::string_view b)
{
    const bool wide = std::ranges::any_of(b, [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
    const bool asText = wide && printableUtf8(b); // printable UTF-8 reads better as itself
    std::string out = "\"";
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto ch = static_cast<unsigned char>(b[i]);
        if (ch == '"')
            out += "\\\"";
        else if (ch == '\\')
            out += "\\\\";
        else if (ch == '\n')
            out += "\\n";
        else if (ch == '\r')
            out += "\\r";
        else if (ch == '\t')
            out += "\\t";
        else if ((ch >= 32 && ch < 127) || (asText && ch >= 0x80))
            out += static_cast<char>(ch);
        else if (i + 1 < b.size() && std::isdigit(static_cast<unsigned char>(b[i + 1])))
            out += std::format("\\{:03}", static_cast<int>(ch)); // a digit after it would read as more of it
        else
            out += std::format("\\{}", static_cast<int>(ch));
    }
    return out + "\"";
}

std::string luaNumber(double v)
{
    if (std::isnan(v))
        return "0/0";
    if (std::isinf(v))
        return v > 0 ? "math.huge" : "-math.huge";
    if (v == std::trunc(v) && std::fabs(v) < 9007199254740992.0)
        return v == 0 && std::signbit(v) ? "-0" : std::format("{}", static_cast<std::int64_t>(v));
    // the shortest digits that read back as `v`, laid out as Python's repr does: plainly for
    // exponents -4 to 15, and as d.ddde+XX otherwise
    std::array<char, 64> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::fabs(v),
                                         std::chars_format::scientific);
    const std::string_view sci(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    const std::size_t e = sci.find('e');
    std::string digits(sci.substr(0, e));
    std::erase(digits, '.');
    const int exp = std::stoi(std::string(sci.substr(e + 1)));
    std::string out = v < 0 ? "-" : "";
    if (exp >= 16 || exp < -4) {
        out += digits.substr(0, 1);
        if (digits.size() > 1)
            out += "." + digits.substr(1);
        return out + std::format("e{}{:02}", exp < 0 ? '-' : '+', std::abs(exp));
    }
    if (exp < 0)
        return out + "0." + std::string(static_cast<std::size_t>(-exp - 1), '0') + digits;
    const auto whole = static_cast<std::size_t>(exp) + 1;
    if (digits.size() <= whole)
        return out + digits + std::string(whole - digits.size(), '0') + ".0";
    return out + digits.substr(0, whole) + "." + digits.substr(whole);
}

struct Scope {
    Scope* parent = nullptr;
    std::unordered_map<int, std::string> names;
    int declared = 0; // locals this block declared (redeclarations included)

    int active() const
    {
        int n = 0;
        for (const Scope* s = this; s; s = s->parent)
            n += s->declared;
        return n;
    }

    const std::string* find(int reg) const
    {
        for (const Scope* s = this; s; s = s->parent)
            if (auto it = s->names.find(reg); it != s->names.end())
                return &it->second;
        return nullptr;
    }
};

class Decompiler {
public:
    Decompiler(std::string_view data, bool lines);

    Function& function(std::uint64_t index)
    {
        if (index >= chunk.protos.size())
            throw DecompileError(std::format("function {} isn't in the bytecode", index));
        auto& slot = functions_[index];
        if (!slot)
            slot = std::make_unique<Function>(chunk.protos[index], scrambled);
        return *slot;
    }

    std::string text();

    Chunk chunk;
    bool scrambled = false;
    bool showLines = true;
    std::unordered_set<std::string> reserved;

private:
    std::map<std::uint64_t, std::unique_ptr<Function>> functions_;
};

Function::Function(const Proto& proto, bool scrambled) : p(proto)
{
    insns_ = decode(proto, scrambled);
    index_.assign(proto.code.size(), -1);
    for (std::size_t k = 0; k < insns_.size(); ++k)
        index_[ix(insns_[k].pc)] = static_cast<int>(k);
    liveness();
    findLoops();
    // what the entry can reach: the compiler leaves dead jumps behind when it threads others
    reachable_.assign(proto.code.size(), false);
    std::vector<int> work;
    if (!insns_.empty())
        work.push_back(insns_[0].pc);
    while (!work.empty()) {
        const int pc = work.back();
        work.pop_back();
        const Insn* i = at(pc);
        if (!i || reachable_[ix(pc)])
            continue;
        reachable_[ix(pc)] = true;
        for (const int s : successors(*i))
            work.push_back(s);
    }
}

class FuncCtx {
public:
    FuncCtx(Decompiler& dec, Function& fn, std::vector<std::string> upnames)
        : dec_(dec), fn_(fn), upnames_(std::move(upnames)), used_(dec.reserved)
    {
        used_.insert(upnames_.begin(), upnames_.end());
    }

    std::pair<std::vector<std::string>, std::vector<std::string>> render(const Block& body, int indent)
    {
        const Proto& p = fn_.p;
        Scope scope;
        std::vector<std::string> params;
        for (int r = 0; r < p.numparams; ++r) {
            std::string name = fresh(locvarName(r, 0).value_or(std::format("a{}", r + 1)));
            declare(scope, r, name);
            params.push_back(std::move(name));
        }
        if (p.vararg)
            params.emplace_back("...");
        plan(body, p.numparams);
        std::vector<std::string> lines;
        block(body, scope, indent + 1, lines);
        return {std::move(params), std::move(lines)};
    }

private:
    using Spot = std::pair<const Block*, int>;
    using Path = std::vector<Spot>;

    std::string fresh(std::string base)
    {
        if (!isIdent(base) || isKeyword(base))
            base = "v";
        std::string name = base;
        for (int k = 2; used_.contains(name) || isKeyword(name); ++k)
            name = std::format("{}_{}", base, k);
        used_.insert(name);
        return name;
    }

    static std::string clean(std::string_view s)
    {
        std::string out;
        for (const char c : s)
            out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
        if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
            out = "_" + out;
        return out.substr(0, 40);
    }

    static bool isStr(const ExprPtr& e) { return e->kind == E::Const && e->ck == Constant::Kind::Str; }

    // A name for a local from what it's first given.
    static std::optional<std::string> suggest(const ExprPtr& e)
    {
        if (e->kind == E::Call && e->a->kind == E::MethodRef) {
            static constexpr std::array<std::string_view, 8> finders{
                "GetService",           "WaitForChild",           "FindFirstChild",    "FindFirstChildOfClass",
                "FindFirstChildWhichIsA", "FindFirstAncestor", "FindFirstAncestorOfClass", "GetAttribute"};
            if (std::ranges::find(finders, e->a->text) != finders.end() && !e->list.empty() && isStr(e->list[0]))
                return clean(e->list[0]->text);
        }
        if (e->kind == E::Call && e->a->kind == E::Global && e->a->text == "require" && !e->list.empty()) {
            const ExprPtr& a = e->list[0];
            if (a->kind == E::Index && isStr(a->b))
                return clean(a->b->text);
            if (a->kind == E::Call && a->a->kind == E::MethodRef && !a->list.empty() && isStr(a->list[0]))
                return clean(a->list[0]->text);
        }
        if (e->kind == E::Index && isStr(e->b))
            return clean(e->b->text);
        if (e->kind == E::Call && e->a->kind == E::Index && e->a->a->kind == E::Global && e->a->a->text == "Instance"
            && !e->list.empty() && isStr(e->list[0]))
            return clean(e->list[0]->text);
        return std::nullopt;
    }

    static std::string defaultName(int reg) { return std::format("v{}", reg); }

    // -- where locals are declared

    // Per block: which statements declare which registers (the deepest one all their references are in).
    void plan(const Block& body, int numParams)
    {
        std::vector<int> order;
        std::unordered_map<int, std::vector<Path>> refs;
        const auto note = [&](int reg, const Path& path) {
            auto [it, added] = refs.try_emplace(reg);
            if (added)
                order.push_back(reg);
            it->second.push_back(path);
        };
        const auto noteExpr = [&](const ExprPtr& e, const Path& path, const std::set<int>& bound) {
            for (const RegUse& u : regs(e))
                if (!bound.contains(u.reg))
                    note(u.reg, path);
        };
        std::function<void(const Block&, const Path&, const std::set<int>&)> visit;
        visit = [&](const Block& block, const Path& path, const std::set<int>& bound) {
            for (int idx = 0; idx < static_cast<int>(block.size()); ++idx) {
                const Stmt& s = *block[ix(idx)];
                Path here = path;
                here.emplace_back(&block, idx);
                for (const ExprPtr& e : hostExprs(s))
                    noteExpr(e, here, bound);
                if (s.kind == S::Assign)
                    for (const ExprPtr& t : s.targets)
                        if (t->kind == E::Var && !bound.contains(t->n))
                            note(t->n, here);
                if (s.kind == S::If) {
                    visit(s.then, here, bound);
                    if (s.other)
                        visit(*s.other, here, bound);
                } else if (s.kind == S::While || s.kind == S::Repeat) {
                    if (s.kind == S::While)
                        noteExpr(s.e, here, bound);
                    visit(s.body, here, bound);
                    if (s.kind == S::Repeat) {
                        Path end = here;
                        end.emplace_back(&s.body, static_cast<int>(s.body.size()));
                        noteExpr(s.e, end, bound);
                    }
                } else if (s.kind == S::NumFor) {
                    std::set<int> inner = bound;
                    inner.insert(s.base + 2);
                    visit(s.body, here, inner);
                } else if (s.kind == S::GenFor) {
                    std::set<int> inner = bound;
                    for (int r = s.base + 3; r < s.base + 3 + s.nvars; ++r)
                        inner.insert(r);
                    visit(s.body, here, inner);
                }
            }
        };
        std::set<int> params;
        for (int r = 0; r < numParams; ++r)
            params.insert(r);
        visit(body, {}, params);
        decl_.clear();
        for (const int reg : order) {
            if (reg < numParams)
                continue;
            const std::vector<Path>& paths = refs[reg];
            std::size_t d = 0;
            while (std::ranges::all_of(paths, [&](const Path& p) { return p.size() > d + 1; })
                   && std::ranges::all_of(paths, [&](const Path& p) { return p[d] == paths[0][d]; }))
                ++d;
            Spot where;
            if (std::ranges::all_of(paths, [&](const Path& p) { return p[d].first == paths[0][d].first; })) {
                int lowest = paths[0][d].second;
                for (const Path& p : paths)
                    lowest = std::min(lowest, p[d].second);
                where = {paths[0][d].first, lowest};
            } else // then and else of one if: declared before it
                where = d > 0 ? paths[0][d - 1] : paths[0].back();
            decl_[where].push_back(reg);
        }
    }

    void declare(Scope& scope, int reg, const std::string& name)
    {
        scope.names[reg] = name;
        ++scope.declared;
    }

    std::optional<std::string> locvarName(int reg, int pc) const
    {
        for (const LocVar& v : fn_.p.locvars)
            if (v.reg == reg && v.start <= static_cast<std::uint64_t>(pc + 1)
                && static_cast<std::uint64_t>(pc + 1) <= v.end && v.name)
                return v.name;
        return std::nullopt;
    }

    void block(const Block& block, Scope& parent, int indent, std::vector<std::string>& lines)
    {
        Scope scope{&parent};
        statements(block, scope, indent, lines);
    }

    void statements(const Block& block, Scope& scope, int indent, std::vector<std::string>& lines)
    {
        for (int idx = 0; idx < static_cast<int>(block.size()); ++idx) {
            const Stmt& s = *block[ix(idx)];
            const auto it = decl_.find({&block, idx});
            static const std::vector<int> none;
            // return, break and continue end a block in Luau: one with more after it goes in a do-end
            const bool wrap = idx + 1 < static_cast<int>(block.size())
                              && (s.kind == S::Return || s.kind == S::Break || s.kind == S::Continue);
            stmt(s, scope, indent, lines, it != decl_.end() ? it->second : none, idx == 0, wrap);
        }
    }

    std::string name(Scope& scope, int reg)
    {
        if (const std::string* n = scope.find(reg))
            return *n;
        std::string n = fresh(defaultName(reg));
        // a read of something never declared: declare it at the top of the function
        Scope* root = &scope;
        while (root->parent)
            root = root->parent;
        root->names[reg] = n;
        return n;
    }

    void stmt(const Stmt& s, Scope& scope, int indent, std::vector<std::string>& lines, const std::vector<int>& declare,
              bool first, bool wrap)
    {
        const std::string pd = pad(indent);
        const std::string tail = dec_.showLines && s.line ? std::format(" --@{}", *s.line) : "";
        if (s.kind == S::Assign && s.declaresFlag) {
            const std::string flag = fresh("returned");
            flagNames_[s.targets[0]->n] = flag;
            lines.push_back(std::format("{}local {} = false{}", pd, flag, tail));
            return;
        }
        if (s.kind == S::Assign) {
            std::vector<int> direct;
            for (const ExprPtr& t : s.targets)
                if (t->kind == E::Var)
                    direct.push_back(t->n);
            const auto inDirect = [&](int r) { return std::ranges::find(direct, r) != direct.end(); };
            std::vector<int> newRegs;
            for (const int r : declare)
                if (inDirect(r))
                    newRegs.push_back(r);
            // the compiler reuses a dead local's register for the next one: a write that doesn't
            // read the old value, to a local of this block nothing captures by reference, starts a new one
            if (newRegs.empty() && !direct.empty() && direct.size() == s.targets.size() && scope.active() < 150
                && std::ranges::all_of(direct,
                                       [&](int r) { return scope.names.contains(r) && !fn_.capturedRef.contains(r); })) {
                bool readsDirect = false;
                for (const ExprPtr& e : hostExprs(s))
                    for (const RegUse& u : regs(e))
                        readsDirect = readsDirect || inDirect(u.reg);
                if (!readsDirect)
                    newRegs = direct;
            }
            for (const int r : declare)
                if (!inDirect(r)) {
                    const std::string nm = fresh(defaultName(r));
                    this->declare(scope, r, nm);
                    lines.push_back(std::format("{}local {}{}", pd, nm, tail));
                }
            // a plain write to a local declared in this block may start a new one (fresh name)
            std::vector<std::string> values;
            for (const ExprPtr& v : s.values)
                values.push_back(expr(v, scope, indent));
            if (!newRegs.empty() && newRegs.size() == s.targets.size()) {
                std::vector<std::string> names;
                for (const ExprPtr& t : s.targets) {
                    const std::optional<std::string> hint
                        = s.targets.size() == 1 ? suggest(s.values[0]) : std::nullopt;
                    names.push_back(fresh(locvarName(t->n, s.pc).value_or(hint.value_or(defaultName(t->n)))));
                }
                for (std::size_t k = 0; k < s.targets.size(); ++k)
                    this->declare(scope, s.targets[k]->n, names[k]);
                if (s.values.size() == 1 && s.values[0]->kind == E::Closure && names.size() == 1) {
                    auto [params, body] = closureBody(*s.values[0], scope, indent);
                    lines.push_back(std::format("{}local function {}({}){}", pd, names[0], join(params, ", "), tail));
                    lines.insert(lines.end(), body.begin(), body.end());
                    lines.push_back(pd + "end");
                    return;
                }
                lines.push_back(std::format("{}local {} = {}{}", pd, join(names, ", "), join(values, ", "), tail));
                return;
            }
            for (const int r : newRegs) {
                const std::string nm = fresh(defaultName(r));
                this->declare(scope, r, nm);
                lines.push_back(std::format("{}local {}{}", pd, nm, tail));
            }
            std::vector<std::string> targets;
            for (const ExprPtr& t : s.targets)
                targets.push_back(t->kind == E::Var ? name(scope, t->n) : expr(t, scope, indent));
            if (s.targets.size() == 1 && s.values.size() == 1 && s.values[0]->kind == E::Closure
                && (s.targets[0]->kind == E::Global || (s.targets[0]->kind == E::Index && dotted(s.targets[0])))) {
                auto [params, body] = closureBody(*s.values[0], scope, indent);
                lines.push_back(std::format("{}function {}({}){}", pd, targets[0], join(params, ", "), tail));
                lines.insert(lines.end(), body.begin(), body.end());
                lines.push_back(pd + "end");
                return;
            }
            lines.push_back(std::format("{}{} = {}{}", pd, join(targets, ", "), join(values, ", "), tail));
            return;
        }
        for (const int r : declare) {
            const std::string nm = fresh(defaultName(r));
            this->declare(scope, r, nm);
            lines.push_back(std::format("{}local {}", pd, nm));
        }
        switch (s.kind) {
        case S::CallStmt: {
            const std::string text = expr(s.e, scope, indent);
            lines.push_back(std::format("{}{}{}{}", pd, text.starts_with('(') && !first ? ";" : "", text, tail));
            break;
        }
        case S::Return: {
            const std::string vals = exprList(s.values, scope, indent);
            const std::string text = vals.empty() ? "return" : "return " + vals;
            lines.push_back(std::format("{}{}{}", pd, wrap ? std::format("do {} end", text) : text, tail));
            break;
        }
        case S::SetList: {
            const std::string t = expr(s.e, scope, indent);
            for (std::size_t k = 0; k < s.values.size(); ++k)
                lines.push_back(
                    std::format("{}{}[{}] = {}{}", pd, t, s.first + k, expr(s.values[k], scope, indent), tail));
            break;
        }
        case S::If:
            ifChain(s, scope, indent, lines, tail);
            break;
        case S::While:
            lines.push_back(std::format("{}while {} do{}", pd, expr(s.e, scope, indent), tail));
            block(s.body, scope, indent + 1, lines);
            lines.push_back(pd + "end");
            break;
        case S::Repeat: {
            lines.push_back(std::format("{}repeat{}{}", pd, s.inlined ? " -- inlined: break returns from it" : "", tail));
            Scope inner{&scope};
            statements(s.body, inner, indent + 1, lines);
            lines.push_back(std::format("{}until {}", pd, expr(s.e, inner, indent)));
            break;
        }
        case S::NumFor: {
            Scope inner{&scope};
            const std::string start = expr(s.start, scope, indent);
            const std::string limit = expr(s.limit, scope, indent);
            const std::string step = expr(s.step, scope, indent);
            const std::string var = fresh(indent < 3 ? "i" : std::format("i{}", indent));
            this->declare(inner, s.base + 2, var);
            lines.push_back(std::format("{}for {} = {}, {}{} do{}", pd, var, start, limit,
                                        step == "1" ? "" : ", " + step, tail));
            block(s.body, inner, indent + 1, lines);
            lines.push_back(pd + "end");
            break;
        }
        case S::GenFor: {
            Scope inner{&scope};
            std::vector<ExprPtr> exprs = s.values;
            while (exprs.size() > 1 && exprs.back()->kind == E::Const && exprs.back()->ck == Constant::Kind::Nil)
                exprs.pop_back();
            const std::string src = exprList(exprs, scope, indent);
            std::vector<std::string> names;
            for (int k = 0; k < s.nvars; ++k) {
                const std::string nm
                    = fresh(s.nvars <= 2 ? std::string(k == 0 ? "k" : "v") : std::format("v{}", k + 1));
                this->declare(inner, s.base + 3 + k, nm);
                names.push_back(nm);
            }
            lines.push_back(std::format("{}for {} in {} do{}", pd, join(names, ", "), src, tail));
            block(s.body, inner, indent + 1, lines);
            lines.push_back(pd + "end");
            break;
        }
        case S::Break:
            lines.push_back(std::format("{}{}{}", pd, wrap ? "do break end" : "break", tail));
            break;
        case S::Continue:
            lines.push_back(std::format("{}{}{}", pd, wrap ? "do continue end" : "continue", tail));
            break;
        case S::Comment:
            lines.push_back(std::format("{}--[[{}]]{}", pd, s.text, tail));
            break;
        case S::Until:
            lines.push_back(std::format("{}--[[decompiler: until {}]]", pd, expr(s.e, scope, indent)));
            break;
        default:
            break;
        }
    }

    void ifChain(const Stmt& s, Scope& scope, int indent, std::vector<std::string>& lines, const std::string& tail)
    {
        const std::string pd = pad(indent);
        lines.push_back(std::format("{}if {} then{}", pd, expr(s.e, scope, indent), tail));
        block(s.then, scope, indent + 1, lines);
        const Block* other = s.other ? &*s.other : nullptr;
        while (other) {
            if (other->size() == 1 && (*other)[0]->kind == S::If && !decl_.contains({other, 0})) {
                const Stmt& o = *(*other)[0];
                lines.push_back(std::format("{}elseif {} then", pd, expr(o.e, scope, indent)));
                block(o.then, scope, indent + 1, lines);
                other = o.other ? &*o.other : nullptr;
                continue;
            }
            lines.push_back(pd + "else");
            block(*other, scope, indent + 1, lines);
            break;
        }
        lines.push_back(pd + "end");
    }

    static bool dotted(const ExprPtr& t)
    {
        const Expr* e = t.get();
        while (e->kind == E::Index) {
            if (!isStr(e->b) || !isIdent(e->b->text) || isKeyword(e->b->text))
                return false;
            e = e->a.get();
        }
        return e->kind == E::Global || e->kind == E::Var || e->kind == E::Upval;
    }

    static std::string join(const std::vector<std::string>& parts, std::string_view sep)
    {
        std::string out;
        for (std::size_t k = 0; k < parts.size(); ++k) {
            if (k)
                out += sep;
            out += parts[k];
        }
        return out;
    }

    std::string exprList(const std::vector<ExprPtr>& exprs, Scope& scope, int indent)
    {
        std::vector<std::string> parts;
        for (std::size_t k = 0; k < exprs.size(); ++k) {
            std::string text = expr(exprs[k], scope, indent);
            if (k + 1 == exprs.size() && (exprs[k]->kind == E::Call || exprs[k]->kind == E::Varargs)
                && !exprs[k]->multi)
                text = "(" + text + ")";
            parts.push_back(std::move(text));
        }
        return join(parts, ", ");
    }

    std::pair<std::vector<std::string>, std::vector<std::string>> closureBody(const Expr& c, Scope& scope, int indent)
    {
        std::vector<std::string> upnames;
        for (const Capture& cap : c.captures) {
            if (!cap.upvalue)
                upnames.push_back(name(scope, cap.index));
            else
                upnames.push_back(cap.index < static_cast<int>(upnames_.size()) ? upnames_[ix(cap.index)]
                                                                                : std::format("up{}", cap.index));
        }
        Function& child = dec_.function(static_cast<std::uint64_t>(c.n));
        FuncCtx ctx(dec_, child, std::move(upnames));
        return ctx.render(child.decompile(), indent);
    }

    std::string expr(const ExprPtr& e, Scope& scope, int indent, int prec = 0)
    {
        auto [text, p] = exprP(e, scope, indent);
        return p < prec ? "(" + text + ")" : text;
    }

    std::string prefix(const ExprPtr& e, Scope& scope, int indent)
    {
        std::string text = exprP(e, scope, indent).first;
        switch (e->kind) {
        case E::Var:
        case E::Upval:
        case E::Global:
        case E::Index:
        case E::Call:
        case E::MethodRef:
            return text;
        default:
            return "(" + text + ")";
        }
    }

    std::pair<std::string, int> exprP(const ExprPtr& e, Scope& scope, int indent)
    {
        switch (e->kind) {
        case E::Var:
            return {name(scope, e->n), 10};
        case E::Const:
            switch (e->ck) {
            case Constant::Kind::Nil:
                return {"nil", 10};
            case Constant::Kind::Bool:
                return {e->boolean ? "true" : "false", 10};
            case Constant::Kind::Num:
            case Constant::Kind::Int: {
                std::string text
                    = e->ck == Constant::Kind::Int ? std::format("{}", e->integer) : luaNumber(e->number);
                const int p = text.starts_with('-') ? unaryPrec : 10;
                return {std::move(text), p};
            }
            case Constant::Kind::Str:
                return {luaString(e->text), 10};
            case Constant::Kind::Vector:
                return {std::format("vector.create({}, {}, {})", luaNumber(e->vec[0]), luaNumber(e->vec[1]),
                                    luaNumber(e->vec[2])),
                        10};
            case Constant::Kind::Table:
                return {"{}", 10};
            default:
                return {std::format("--[[{}]] nil", kindName(e->ck)), 10};
            }
        case E::Upval:
            return {e->n < static_cast<int>(upnames_.size()) ? upnames_[ix(e->n)] : std::format("up{}", e->n), 10};
        case E::Flag: {
            const auto it = flagNames_.find(e->n);
            return {it != flagNames_.end() ? it->second : std::format("returned{}", e->n), 10};
        }
        case E::Global:
            return {e->text, 10};
        case E::Index: {
            const std::string obj = prefix(e->a, scope, indent);
            if (isStr(e->b) && isIdent(e->b->text) && !isKeyword(e->b->text))
                return {std::format("{}.{}", obj, e->b->text), 10};
            return {std::format("{}[{}]", obj, expr(e->b, scope, indent)), 10};
        }
        case E::MethodRef:
            return {std::format("{}:{}", prefix(e->a, scope, indent), e->text), 10};
        case E::Call: {
            const std::string args = exprList(e->list, scope, indent);
            if (e->a->kind == E::MethodRef)
                return {std::format("{}:{}({})", prefix(e->a->a, scope, indent), e->a->text, args), 10};
            return {std::format("{}({})", prefix(e->a, scope, indent), args), 10};
        }
        case E::Varargs:
            return {"...", 10};
        case E::Binop: {
            const int p = precOf(e->text);
            const bool right = e->text == ".." || e->text == "^";
            const std::string a = expr(e->a, scope, indent, right ? p + 1 : p);
            const std::string b = expr(e->b, scope, indent, right ? p : p + 1);
            return {std::format("{} {} {}", a, e->text, b), p};
        }
        case E::Unop: {
            const std::string a = expr(e->a, scope, indent, unaryPrec);
            if (e->text == "not")
                return {"not " + a, unaryPrec};
            if (e->text == "-" && a.starts_with('-'))
                return {"-(" + a + ")", unaryPrec};
            return {e->text + a, unaryPrec};
        }
        case E::IfExpr: {
            const std::string c = expr(e->c, scope, indent);
            const std::string a = expr(e->a, scope, indent);
            const std::string b = expr(e->b, scope, indent);
            return {std::format("if {} then {} else {}", c, a, b), 0};
        }
        case E::Closure: {
            auto [params, lines] = closureBody(*e, scope, indent);
            return {std::format("function({})\n{}\n{}end", join(params, ", "), join(lines, "\n"), pad(indent)), 10};
        }
        case E::Table: {
            std::vector<std::string> parts;
            for (const ExprPtr& v : e->list)
                parts.push_back(expr(v, scope, indent + 1));
            if (!e->list.empty() && (e->list.back()->kind == E::Call || e->list.back()->kind == E::Varargs)
                && !e->list.back()->multi)
                parts.back() = "(" + parts.back() + ")";
            for (const auto& [k, v] : e->hash) {
                if (isStr(k) && isIdent(k->text) && !isKeyword(k->text))
                    parts.push_back(std::format("{} = {}", k->text, expr(v, scope, indent + 1)));
                else {
                    const std::string key = expr(k, scope, indent + 1);
                    parts.push_back(std::format("[{}] = {}", key, expr(v, scope, indent + 1)));
                }
            }
            if (parts.empty())
                return {"{}", 10};
            const std::string one = "{" + join(parts, ", ") + "}";
            if (one.size() <= 100 && !one.contains('\n'))
                return {one, 10};
            std::string out = "{\n";
            for (std::size_t k = 0; k < parts.size(); ++k)
                out += (k ? ",\n" : "") + pad(indent + 1) + parts[k];
            return {out + "\n" + pad(indent) + "}", 10};
        }
        }
        return {"--[[?]] nil", 10};
    }

    Decompiler& dec_;
    Function& fn_;
    std::vector<std::string> upnames_;
    std::unordered_set<std::string> used_;
    std::map<Spot, std::vector<int>> decl_;
    std::unordered_map<int, std::string> flagNames_;
};

Decompiler::Decompiler(std::string_view data, bool lines)
    : chunk(data), scrambled(looksScrambled(chunk)), showLines(lines)
{
    reserved = {keywords.begin(), keywords.end()};
    for (const char* name : {"game", "workspace", "script", "Enum", "Instance", "Vector3", "CFrame", "math", "string",
                             "table", "task", "print", "warn", "error", "pairs", "ipairs", "next", "require", "self",
                             "type", "typeof", "tostring", "tonumber", "select", "unpack", "_G", "shared", "wait",
                             "spawn", "delay", "tick", "time", "os", "coroutine", "debug", "utf8", "bit32", "buffer"})
        reserved.insert(name);
    for (const Proto& p : chunk.protos)
        for (const Constant& c : p.k)
            if (c.kind == Constant::Kind::Import && !c.import.empty())
                reserved.insert(c.import[0]);
    // globals read or written by name
    for (const Proto& p : chunk.protos) {
        try {
            for (const Insn& i : decode(p, scrambled))
                if ((i.op == GETGLOBAL || i.op == SETGLOBAL) && i.aux < p.k.size()
                    && p.k[i.aux].kind == Constant::Kind::Str)
                    reserved.insert(p.k[i.aux].text);
        } catch (const DecompileError&) { // the function fails again (and says why) when it's decompiled
        }
    }
}

std::string Decompiler::text()
{
    Function& main = function(chunk.main);
    FuncCtx ctx(*this, main, {});
    const auto [params, lines] = ctx.render(main.decompile(), -1);
    std::string out;
    for (const std::string& line : lines)
        out += line + "\n";
    return lines.empty() ? "\n" : out;
}

} // namespace

std::expected<std::string, std::string> decompileBytecode(std::string_view bytecode, bool lines)
{
    try {
        return Decompiler(bytecode, lines).text();
    } catch (const std::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
}

} // namespace lunar::lua
