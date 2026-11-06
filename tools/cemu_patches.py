"""Cemu graphic packs' code patches: rules.txt presets, the patch files and a PowerPC assembler.

A Cemu graphic pack changes a game's code with patch_*.asm files (or a Cemuhook patches.txt) next to its
rules.txt. This reads them the way Cemu does (src/Cafe/GraphicPack/GraphicPack2PatchesParser.cpp and
GraphicPack2PatchesApply.cpp, src/Cemu/ExpressionParser/ExpressionParser.h and
src/Cemu/PPCAssembler/ppcAssembler.cpp of https://github.com/cemu-project/Cemu), so that a pack made for
Cemu means the same here:

- A file holds groups ([name]); a group applies to the modules its `moduleMatches` names (the RPX's
  checksum, or `rpx`). Lines are `<address> = <instruction or data>`, labels (`name:`), variables
  (`name = expression`) and `.origin = codecave` / `.origin = <address>`, after which lines without an
  address are placed one after another. Each group has its own code cave: addresses below 8 MiB are
  offsets into it. Labels and variables are shared by all groups of a pack.
- Expressions are Cemu's: + - * / % ^ and comparisons, numbers (0x hex; a leading 0 without a decimal
  point anywhere after it is hex too), $preset variables, labels, variables, `import.<module>.<function>`,
  the functions ha() hi() lo() reloc(), and one @ha/@h/@hi/@l/@lo suffix, which applies to the whole
  expression's value. A label "sticks" to the next instruction or data, after its alignment.
- rules.txt: [Default] holds the variables, each [Preset] (name, category, condition, default) sets
  some of them; one preset per category is active. Values are numbers (expressions of the same
  section's earlier variables), conditions are integer expressions of the current values.

What Cemu resolves against the running game is kept apart here: an expression that reads a $preset
variable is compiled to a postfix program (RPN, see Rpn) for the runtime, so a preset can change while
the game runs. Everything else (labels, cave addresses, constants) is resolved when the pack is read.
"""
import math
import os
import re
import struct

CODECAVE_MAX_OFFSET = 8 * 1024 * 1024  # Cemu's PatchGroup::CODECAVE_MAX_OFFSET


class PatchError(ValueError):
    """Something in a pack Cemu would refuse, or this port can't do (the message says which)."""


class Unresolved(Exception):
    """An expression names a symbol that is not known (yet): resolve again later."""

    def __init__(self, name):
        super().__init__(name)
        self.name = name


# ---------------------------------------------------------------------------------------- expressions

def _digit(c):
    return "0" <= c <= "9"


def _alpha(c):
    return ("a" <= c <= "z") or ("A" <= c <= "Z")


def _xdigit(c):
    return _digit(c) or ("a" <= c <= "f") or ("A" <= c <= "F")


def convert_number(s):
    """ExpressionParser::ConvertString: (value, characters used) of the number at the start of s, which
    runs to the end of the whole expression (the hex rule below looks at all of it)."""
    used = 0
    if s[:1] in ("-", "+"):
        negative = s[0] == "-"
        s, used = s[1:], 1
    else:
        negative = False
    if len(s) >= 3 and s[0] == "0" and s[1] in "xX":
        j = 2
        while j < len(s) and _xdigit(s[j]):
            j += 1
        if j == 2:
            raise PatchError("can't parse number: %s" % s)
        value, used = float(int(s[2:j], 16)), used + j
    elif len(s) >= 2 and s[0] == "0" and _xdigit(s[1]) and "." not in s:
        # a number with a leading 0 and no decimal point (in the rest of the expression) is hex
        j = 1
        while j < len(s) and _xdigit(s[j]):
            j += 1
        value, used = float(int(s[1:j], 16)), used + j
    else:
        if s[:1] == "+":
            s, used = s[1:], used + 1
        j, point, exponent = 0, False, False
        while j < len(s):
            c = s[j]
            if _digit(c):
                pass
            elif c == "." and not point:
                point = True
            elif c in "eE" and not exponent:
                exponent = True
            else:
                break
            j += 1
        text = s[:j]
        # from_chars: the longest prefix that is a number ("1e" -> 1)
        m = re.match(r"(\d+\.?\d*|\.\d+)([eE]\d+)?", text)
        if not m:
            raise PatchError("can't parse number: %s" % s)
        value, used = float(m.group(0)), used + len(m.group(0))
    return (-value if negative else value), used


_OPERATORS = {"+": (2, False), "-": (2, False), "*": (3, False), "/": (3, False), "%": (3, False),
              "^": (4, True), "==": (1, False), "!=": (1, False), "<": (1, False), "<=": (1, False),
              ">": (1, False), ">=": (1, False)}
_MODIFIERS = {"ha": "ha", "hi": "hi", "lo": "lo", "h": "hi", "l": "lo"}


def _modifier(s, ignore_errors):
    """ParseRelocModifier: (modifier, length) of an @ha/@hi/@h/@lo/@l at the start of s."""
    if not s.startswith("@") or len(s) < 2:
        return None, 0
    two = s[1:3].lower()
    if len(s) >= 3 and two in ("ha", "hi", "lo"):
        return _MODIFIERS[two], 3
    if s[1].lower() in "hl":
        return _MODIFIERS[s[1].lower()], 2
    if ignore_errors:
        return None, 0
    raise PatchError("unknown relocation modifier (only @lo, @hi, @ha, @l, @h are supported) at: %s" % s)


class Rpn:
    """A compiled expression: tokens in postfix order and the @modifier applied to the result.

    Tokens: a number (float), a name (str: a $preset variable, a label or variable, import.x.y, or
    in an integer expression one of the constants given), ("fn", name) for a function of the value
    before it, ("op", op) for a binary operator. The runtime reads the same form (to_json)."""

    __slots__ = ("tokens", "modifier", "text")

    def __init__(self, tokens, modifier, text):
        self.tokens, self.modifier, self.text = tokens, modifier, text

    def names(self):
        return [t for t in self.tokens if isinstance(t, str)]

    def presets(self):
        return sorted(set(n for n in self.names() if n.startswith("$")))


def compile_expression(text, functions=True):
    """TExpressionParser::Evaluate's tokenizer and shunting-yard. `functions`: name( calls a function
    (Cemu's patches set a function callback; rules.txt and conditions do not)."""
    if not text:
        raise PatchError("empty expression is not allowed")
    output, operators = [], []
    modifier = [None]

    def set_modifier(m):
        if m is None or modifier[0] == m:
            return
        if modifier[0] is not None:
            raise PatchError("mismatching relocation modifiers (suffix @..) in expression: %s" % text)
        modifier[0] = m

    i, n, last_operator = 0, len(text), True
    while i < n:
        c = text[i]
        if c in " \t":
            i += 1
            continue
        if _digit(c) or (last_operator and c in "-+"):
            value, used = convert_number(text[i:])
            output.append(value)
            i += used
            last_operator = False
            m, length = _modifier(text[i:], False)
            set_modifier(m)
            i += length
            continue
        if _alpha(c) or c in "_$":
            j = i + 1
            while j < n and (_alpha(text[j]) or _digit(text[j]) or text[j] in "_@."):
                j += 1
            name = text[i:j]
            at = name.rfind("@")
            if at >= 0:
                m, length = _modifier(name[at:], True)
                set_modifier(m)
                if length:
                    name = name[:len(name) - length]
            if functions and j < n and text[j] == "(":
                operators.append(("fn", name))
                operators.append("(")
                i = j + 1
                last_operator = True
                continue
            output.append(name)
            i = j
            last_operator = False
            continue
        if c == "(":
            operators.append("(")
            i += 1
            last_operator = True
            continue
        if c == ")":
            match = False
            while operators:
                top = operators.pop()
                if top != "(":
                    output.append(top)
                    continue
                match = True
                break
            if not match:
                raise PatchError("parentheses mismatch: %s" % text)
            i += 1
            continue
        op = None
        if c in "+-*/^%":
            op = c
        elif c in "=!" and text[i + 1:i + 2] == "=":
            op, i = c + "=", i + 1
        elif c in "<>":
            if text[i + 1:i + 2] == "=":
                op, i = c + "=", i + 1
            else:
                op = c
        if op is None:
            raise PatchError("invalid operator found in expression: %s" % text)
        prec, right = _OPERATORS[op]
        while operators:
            top = operators[-1]
            if isinstance(top, tuple) and top[0] == "op":
                tprec = _OPERATORS[top[1]][0]
                if (not right and prec <= tprec) or (right and prec < tprec):
                    output.append(operators.pop())
                    continue
            break
        operators.append(("op", op))
        i += 1
        last_operator = True
    while operators:
        top = operators.pop()
        if top == "(":
            raise PatchError("parentheses mismatch in expression: %s" % text)
        output.append(top)
    return Rpn(output, modifier[0], text)


def cround(x):
    """std::round: halves away from zero."""
    return math.floor(x + 0.5) if x >= 0 else -math.floor(-x + 0.5)


def to_u32(x):
    """(uint32)double as Cemu's x86-64 builds convert it (through a 64-bit integer)."""
    if not math.isfinite(x) or abs(x) >= 2.0 ** 63:
        return 0
    return int(x) & 0xFFFFFFFF


def _int32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def func_ha(x):
    a = to_u32(x)
    return float(((a >> 16) + (1 if a & 0x8000 else 0)) & 0xFFFF)


def func_hi(x):
    return float((to_u32(x) >> 16) & 0xFFFF)


def func_lo(x):
    return float(to_u32(x) & 0xFFFF)


MODIFIER_FUNCS = {"ha": func_ha, "hi": func_hi, "lo": func_lo}


def apply_op(op, lhs, rhs, integer=False):
    """One binary operator of TExpressionParser<double> (or <int>: integer)."""
    if integer:
        lhs, rhs = int(lhs), int(rhs)
        if op == "+":
            return _int32(lhs + rhs)
        if op == "-":
            return _int32(lhs - rhs)
        if op == "*":
            return _int32(lhs * rhs)
        if op == "/":
            if rhs == 0:
                return 0
            q = abs(lhs) // abs(rhs)
            return _int32(-q if (lhs < 0) != (rhs < 0) else q)
        if op == "^":
            try:
                v = math.pow(lhs, rhs)
            except (OverflowError, ValueError):
                return 0
            return _int32(int(v)) if math.isfinite(v) else 0
        if op == "%":
            if rhs == 0:
                return 0
            r = abs(lhs) % abs(rhs)
            return -r if lhs < 0 else r
        return int({"==": lhs == rhs, "!=": lhs != rhs, "<": lhs < rhs, "<=": lhs <= rhs,
                    ">": lhs > rhs, ">=": lhs >= rhs}[op])
    if op == "+":
        return lhs + rhs
    if op == "-":
        return lhs - rhs
    if op == "*":
        return lhs * rhs
    if op == "/":
        return 0.0 if rhs == 0.0 else lhs / rhs
    if op == "^":
        try:
            return math.pow(lhs, rhs)
        except OverflowError:
            return math.inf
        except ValueError:
            return math.nan
    if op == "%":
        a, b = cround(rhs), cround(lhs)
        if a == 0 or not math.isfinite(lhs) or not math.isfinite(rhs):
            return 0.0
        r = abs(int(b)) % abs(int(a))
        return float(-r if b < 0 else r)
    if op == "==":
        return 1.0 if abs(lhs - rhs) <= 0.0000000001 else 0.0
    if op == "!=":
        return 1.0 if abs(lhs - rhs) > 0.0000000001 else 0.0
    return 1.0 if {"<": lhs < rhs, "<=": lhs <= rhs, ">": lhs > rhs, ">=": lhs >= rhs}[op] else 0.0


def evaluate(rpn, lookup, integer=False, call=None):
    """The value of a compiled expression. lookup(name) gives a symbol's value (raises Unresolved),
    call(name, value) a function's (None: ha/hi/lo and their 16 forms only)."""
    stack = []
    for t in rpn.tokens:
        if isinstance(t, float):
            stack.append(int(t) if integer else t)
        elif isinstance(t, str):
            v = lookup(t)
            stack.append(int(v) if integer else float(v))
        elif t[0] == "fn":
            if not stack:
                raise PatchError("not enough parameters for equation: %s" % rpn.text)
            p = stack.pop()
            name = t[1].lower()
            if call is not None:
                stack.append(call(name, p))
            elif name in ("ha", "ha16", "hi", "hi16", "lo", "lo16"):
                stack.append(MODIFIER_FUNCS[name[:2]](p))
            else:
                raise Unresolved(t[1] + "()")
        else:
            if len(stack) < 2:
                raise PatchError("not enough parameters for equation: %s" % rpn.text)
            rhs = stack.pop()
            lhs = stack.pop()
            stack.append(apply_op(t[1], lhs, rhs, integer))
    if not stack:
        raise PatchError("expression has no value: %s" % rpn.text)
    v = stack[-1]
    if rpn.modifier:
        v = MODIFIER_FUNCS[rpn.modifier](v)
        if integer:
            v = int(v)
    return v


def rpn_to_json(rpn):
    """The runtime's form: a list of numbers, "$name" variables and "+", "fn:ha", ... operators;
    the @modifier is applied last."""
    out = []
    for t in rpn.tokens:
        if isinstance(t, float):
            out.append(t)
        elif isinstance(t, str):
            out.append(t)
        elif t[0] == "fn":
            out.append("fn:" + t[1].lower())
        else:
            out.append(t[1])
    if rpn.modifier:
        out.append("fn:" + rpn.modifier)
    return out


# ------------------------------------------------------------------------------------------- rules.txt

class Preset:
    __slots__ = ("name", "category", "condition", "default", "values")

    def __init__(self, name, category, condition, default, values):
        self.name, self.category, self.condition, self.default, self.values = name, category, condition, default, values


class Rules:
    """A pack's rules.txt: its name and description, the [Default] variables and the presets."""

    def __init__(self):
        self.name = self.description = self.version = ""
        self.title_ids = []
        self.defaults = {}  # $name -> (type "int"/"double", value)
        self.presets = []   # in file order
        self.categories = []  # in order of first appearance
        self.sections = []  # lower-case names of every section
        self.texture_keys = set()  # the keys of its [TextureRedefine] rules (lower case)
        self.notes = []     # presets Cemu would skip, and why

    def possible_values(self, name):
        """Every value $name can take: its default and what the presets set it to."""
        out = set()
        if name in self.defaults:
            out.add(self.defaults[name][1])
        for p in self.presets:
            if name in p.values:
                out.add(p.values[name])
        return sorted(out)


def _strip_comment(line):
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c == "#" and not quoted:
            return line[:i]
    return line


def _unquote(v):
    v = v.strip()
    return v[1:-1] if len(v) >= 2 and v[0] == '"' and v[-1] == '"' else v


def _section_values(fields, where, notes):
    """ParsePresetVars: the $variables of one section, each value an expression of the earlier ones."""
    values, types, known = {}, {}, {}
    for key, value in fields:
        if not key.startswith("$"):
            continue
        name, kind = key, "double"
        if ":" in key:
            name, kind = key.split(":", 1)
            name, kind = name.strip(), kind.strip()
            kind = kind if kind in ("int", "double") else "double"
        rpn = compile_expression(value, functions=False)

        def lookup(n):
            if n in known:
                return known[n]
            raise PatchError("unknown constant found \"%s\" in expression: %s" % (n, value))
        v = evaluate(rpn, lookup)
        if name not in values:  # try_emplace: the first wins
            values[name], types[name] = v, kind
        known[name] = v
    return values, types


def parse_rules(text):
    rules = Rules()
    sections = []
    for raw in text.splitlines():
        line = _strip_comment(raw).strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            sections.append((line[1:-1].strip().lower(), []))
            continue
        if "=" not in line or not sections:
            continue
        key, value = line.split("=", 1)
        sections[-1][1].append((key.strip(), value.strip()))
    for name, fields in sections:
        rules.sections.append(name)
        lower = {}
        for k, v in fields:
            lower.setdefault(k.lower(), v)
        if name == "textureredefine":
            rules.texture_keys |= set(lower)
        if name == "definition":
            rules.name = _unquote(lower.get("name", ""))
            rules.description = _unquote(lower.get("description", "")).replace("|", "\n")
            rules.version = _unquote(lower.get("version", ""))
            rules.title_ids = [t.strip().lower() for t in _unquote(lower.get("titleids", "")).split(",") if t.strip()]
            if not rules.name:
                path = _unquote(lower.get("path", ""))
                rules.name = path.rsplit("/", 1)[-1]
        elif name == "default":
            values, types = _section_values(fields, "[Default]", rules.notes)
            for k, v in values.items():
                rules.defaults[k] = (types[k], v)
        elif name == "preset":
            pname = _unquote(lower.get("name", ""))
            if not pname:
                rules.notes.append("a preset without a name is skipped")
                continue
            category = _unquote(lower.get("category", ""))
            condition = lower.get("condition")
            try:
                values, _ = _section_values(fields, "preset " + pname, rules.notes)
                cond = compile_expression(_unquote(condition), functions=False) if condition else None
            except PatchError as e:
                rules.notes.append("preset \"%s\" is skipped: %s" % (pname, e))
                continue
            try:
                default = int(float(_unquote(lower.get("default", "0")))) != 0
            except ValueError:
                default = False
            rules.presets.append(Preset(pname, category, cond, default, values))
            if category not in rules.categories:
                rules.categories.append(category)
    try:
        categorized = int(rules.version) >= 5
    except ValueError:
        categorized = True
    if categorized:  # Cemu refuses such a pack
        for p in rules.presets:
            for k in p.values:
                if k not in rules.defaults:
                    raise PatchError("preset \"%s\" sets %s, which [Default] doesn't define" % (p.name, k))
    return rules


# ------------------------------------------------------------------------------------- the assembler

GPR, FPR = "r", "f"


class Reloc:
    """A field of an assembled instruction or datum that an expression fills in when the pack is
    resolved: kind "imm" (bits at shift of the word), "b16"/"b26" (a branch target), or a datum
    "float", "double", "u32", "u16", "u8" at byte offset `at`."""
    __slots__ = ("kind", "rpn", "at", "shift", "bits")

    def __init__(self, kind, rpn, at=0, shift=0, bits=0):
        self.kind, self.rpn, self.at, self.shift, self.bits = kind, rpn, at, shift, bits


class Assembled:
    __slots__ = ("data", "relocs", "align", "code")

    def __init__(self, data, relocs, align, code):
        self.data, self.relocs, self.align, self.code = data, relocs, align, code


def _split_operands(text):
    """ppcAssembler: operands are separated by commas outside "strings"."""
    out, cur, quoted, i = [], [], False, 0
    while i < len(text):
        c = text[i]
        if c == '"':
            backslashes = 0
            k = len(cur) - 1
            while k >= 0 and cur[k] == "\\":
                backslashes += 1
                k -= 1
            if backslashes % 2 == 0:
                quoted = not quoted
        if c == "," and not quoted:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append("".join(cur).strip())
    return out


def _reg(text, prefix, ea_zero=False):
    t = text.strip()
    if ea_zero and t == "0":
        return 0
    if t[:len(prefix)].lower() != prefix or not t[len(prefix):].isdigit():
        raise PatchError("\"%s\" is not a %s" % (text, {"r": "GPR (r0 - r31)", "f": "FPR (f0 - f31)",
                                                         "cr": "CR field (cr0 - cr7)"}[prefix]))
    n = int(t[len(prefix):])
    if n >= (8 if prefix == "cr" else 32):
        raise PatchError("\"%s\" is out of range" % text)
    return n


_CR_CONSTANTS = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}
_CR_CONSTANTS.update({"cr%d" % i: i for i in range(8)})


def _constant(text, names=None, integer=False):
    """A constant operand (no symbols except `names`), or None when it names something else."""
    rpn = compile_expression(text)

    def lookup(n):
        if names is not None and n in names:
            return names[n]
        raise Unresolved(n)
    try:
        return evaluate(rpn, lookup, integer=integer)
    except Unresolved:
        return None


def _crbit(text):
    """A CR bit: lt/gt/eq/so, or 4*crN+eq and the like (TExpressionParser<sint32> with those names)."""
    v = _constant(text, _CR_CONSTANTS, integer=True)
    if v is None or not 0 <= v < 32:
        raise PatchError("CR bit operand \"%s\" is not a constant from 0 to 31" % text)
    return int(v)


class _Builder:
    """Collects one instruction's word and relocations."""

    def __init__(self, word):
        self.word = word
        self.relocs = []

    def imm(self, text, shift, bits, signed, negate=False):
        expr = "0-(%s)" % text if negate else text
        rpn = compile_expression(expr)
        if not rpn.names() and not any(isinstance(t, tuple) and t[0] == "fn" for t in rpn.tokens):
            v = to_u32(evaluate(rpn, lambda n: 0.0))
            sv = _int32(v)
            lo, hi = (-(1 << (bits - 1)), (1 << bits) - 1) if signed else (0, (1 << bits) - 1)
            if signed and bits == 16:
                lo = -32768  # signed 16-bit fields also take 0x8000-0xFFFF (Cemu's extended range)
            if not (lo <= sv <= hi or lo <= v <= hi):
                raise PatchError("value of operand \"%s\" is out of range" % text)
            self.word |= (v & ((1 << bits) - 1)) << shift
        else:
            self.relocs.append(Reloc("imm", rpn, 0, shift, bits))

    def mem(self, text):
        """offset(rA): the 16-bit displacement and rA."""
        t = text.strip()
        if not t.endswith(")") or "(" not in t:
            raise PatchError("\"%s\" is not a memory operand of the form offset(gpr), e.g. 0x20(r3)" % text)
        open_at = t.rfind("(")
        reg = _reg(t[open_at + 1:-1], GPR, ea_zero=True)
        self.word |= reg << 16
        offset = t[:open_at].strip()
        if not offset:
            raise PatchError("memory operand \"%s\" has no offset" % text)
        self.imm(offset, 0, 16, True)

    def branch(self, text, kind):
        t = text.strip()
        if t.startswith("."):  # .+0x8: a constant distance
            v = _constant("0" + t[1:])
            if v is None:
                raise PatchError("branch target \"%s\" is not constant" % text)
            v = int(v)
            limit = 0x8000 if kind == "b16" else 0x2000000
            if not -limit <= v < limit:
                raise PatchError("branch target out of range")
            if v & 3:
                raise PatchError("branch target must be aligned to 4")
            self.word |= v & (0xFFFC if kind == "b16" else 0x3FFFFFC)
        else:
            self.relocs.append(Reloc(kind, compile_expression(t)))


def _ops(operands, n, name):
    if len(operands) != n:
        raise PatchError("%s takes %d operand%s" % (name, n, "s" if n != 1 else ""))


# operand layouts, each a function(builder, operands)
def _rD_rA_simm(op, negate=False, signed=True):
    def f(b, o):
        _ops(o, 3, "this instruction")
        b.word |= op << 26 | _reg(o[0], GPR) << 21 | _reg(o[1], GPR, ea_zero=True) << 16
        b.imm(o[2], 0, 16, signed, negate)
    return f


def _rD_simm(op, negate=False):  # li / lis
    def f(b, o):
        _ops(o, 2, "this instruction")
        b.word |= op << 26 | _reg(o[0], GPR) << 21
        b.imm(o[1], 0, 16, True, negate)
    return f


def _rA_rS_uimm(op):  # ori, andi. ...
    def f(b, o):
        _ops(o, 3, "this instruction")
        b.word |= op << 26 | _reg(o[1], GPR) << 21 | _reg(o[0], GPR) << 16
        b.imm(o[2], 0, 16, False)
    return f


def _cmp_imm(op, signed):
    def f(b, o):
        if len(o) == 3:
            cr, ra, v = _reg(o[0], "cr"), o[1], o[2]
        elif len(o) == 2:
            cr, ra, v = 0, o[0], o[1]
        else:
            raise PatchError("compare takes [crN,] rA, value")
        b.word |= op << 26 | cr << 23 | _reg(ra, GPR) << 16
        b.imm(v, 0, 16, signed)
    return f


def _cmp_reg(xo, fp=False):
    def f(b, o):
        if fp:  # fcmpu [crN,] fA, fB (cr0 when left out, as Cemu takes it)
            if len(o) not in (2, 3):
                raise PatchError("fcmpu/fcmpo takes [crN,] fA, fB")
            cr = _reg(o[0], "cr") if len(o) == 3 else 0
            b.word |= 63 << 26 | cr << 23 | _reg(o[-2], FPR) << 16 | _reg(o[-1], FPR) << 11 | xo << 1
            return
        if len(o) == 3:
            cr, ra, rb = _reg(o[0], "cr"), o[1], o[2]
        elif len(o) == 2:
            cr, ra, rb = 0, o[0], o[1]
        else:
            raise PatchError("compare takes [crN,] rA, rB")
        b.word |= 31 << 26 | cr << 23 | _reg(ra, GPR) << 16 | _reg(rb, GPR) << 11 | xo << 1
    return f


def _mem_form(op, reg):  # lwz rD, d(rA) ...
    def f(b, o):
        _ops(o, 2, "this load/store")
        b.word |= op << 26 | _reg(o[0], reg) << 21
        b.mem(o[1])
    return f


def _x_form(xo, reg=GPR, rc=0):  # lwzx rD, rA, rB ...
    def f(b, o):
        _ops(o, 3, "this load/store")
        b.word |= 31 << 26 | _reg(o[0], reg) << 21 | _reg(o[1], GPR, ea_zero=True) << 16 | _reg(o[2], GPR) << 11 | xo << 1 | rc
    return f


def _xo_form(xo, nregs, oe, rc, swap=False):  # add rD, rA, rB ...
    def f(b, o):
        _ops(o, nregs, "this instruction")
        regs = [_reg(x, GPR) for x in o]
        if swap:  # sub rD, rA, rB = subf rD, rB, rA
            regs[1], regs[2] = regs[2], regs[1]
        b.word |= 31 << 26 | regs[0] << 21 | regs[1] << 16 | (regs[2] << 11 if nregs == 3 else 0) | oe << 10 | xo << 1 | rc
    return f


def _logic(xo, rc, nregs=3, same=False):  # and rA, rS, rB; mr rA, rS = or rA, rS, rS
    def f(b, o):
        _ops(o, 2 if same or nregs == 2 else 3, "this instruction")
        ra, rs = _reg(o[0], GPR), _reg(o[1], GPR)
        rb = rs if same else (_reg(o[2], GPR) if nregs == 3 else 0)
        b.word |= 31 << 26 | rs << 21 | ra << 16 | rb << 11 | xo << 1 | rc
    return f


def _srawi(rc):
    def f(b, o):
        _ops(o, 3, "srawi")
        b.word |= 31 << 26 | _reg(o[1], GPR) << 21 | _reg(o[0], GPR) << 16 | 824 << 1 | rc
        b.imm(o[2], 11, 5, False)
    return f


def _const5(text):
    v = _constant(text)
    if v is None:
        raise PatchError("\"%s\" must be a constant" % text)
    v = int(v)
    if not 0 <= v <= 31:
        raise PatchError("\"%s\" is out of range (0-31)" % text)
    return v


def _rotate(op, rc, form=None):
    """rlwinm/rlwimi/rlwnm rA, rS, SH|rB, MB, ME and the shorthands (form: how to get SH, MB, ME)."""
    def f(b, o):
        ra, rs = _reg(o[0], GPR), _reg(o[1], GPR)
        if form is None:
            _ops(o, 5, "rlwinm/rlwimi/rlwnm")
            sh = _reg(o[2], GPR) if op == 23 else _const5(o[2])
            mb, me = _const5(o[3]), _const5(o[4])
        else:
            args = [_const5(x) for x in o[2:]]
            sh, mb, me = form(*args)
            if not all(0 <= x <= 31 for x in (sh, mb, me)):
                raise PatchError("shift or mask out of range")
        b.word |= op << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1 | rc
    return f


_ROTATE_SHORTHANDS = {
    "slwi": (21, lambda n: (n, 0, 31 - n)), "srwi": (21, lambda n: ((32 - n) & 31, n, 31)),
    "clrlwi": (21, lambda n: (0, n, 31)), "clrrwi": (21, lambda n: (0, 0, 31 - n)),
    "rotlwi": (21, lambda n: (n, 0, 31)), "rotrwi": (21, lambda n: ((32 - n) & 31, 0, 31)),
    "extlwi": (21, lambda n, b: (b, 0, n - 1)), "extrwi": (21, lambda n, b: ((b + n) & 31, 32 - n, 31)),
    "inslwi": (20, lambda n, b: ((32 - b) & 31, b, b + n - 1)),
    "insrwi": (20, lambda n, b: ((32 - (b + n)) & 31, b, b + n - 1)),
    "clrlslwi": (21, lambda b, n: (n, b - n, 31 - n)),
}

_SPR = {"xer": 1, "lr": 8, "ctr": 9}


def _spr_field(spr):
    return (spr & 0x1F) << 16 | ((spr >> 5) & 0x1F) << 11


def _mfspr(fixed=None):
    def f(b, o):
        if fixed is None:
            _ops(o, 2, "mfspr")
            rd, spr = _reg(o[0], GPR), _constant(o[1])
        else:
            _ops(o, 1, "this instruction")
            rd, spr = _reg(o[0], GPR), fixed
        if spr is None:
            raise PatchError("SPR number must be a constant")
        b.word |= 31 << 26 | rd << 21 | _spr_field(int(spr)) | 339 << 1
    return f


def _mtspr(fixed=None):
    def f(b, o):
        if fixed is None:
            _ops(o, 2, "mtspr")
            spr, rs = _constant(o[0]), _reg(o[1], GPR)
        else:
            _ops(o, 1, "this instruction")
            spr, rs = fixed, _reg(o[0], GPR)
        if spr is None:
            raise PatchError("SPR number must be a constant")
        b.word |= 31 << 26 | rs << 21 | _spr_field(int(spr)) | 467 << 1
    return f


def _fp(op, xo, regs, rc):
    """FP forms: regs names the fields in operand order: d (21), a (16), b (11), c (6)."""
    shifts = {"d": 21, "a": 16, "b": 11, "c": 6}

    def f(b, o):
        _ops(o, len(regs), "this instruction")
        b.word |= op << 26 | xo << 1 | rc
        for field, text in zip(regs, o):
            b.word |= _reg(text, FPR) << shifts[field]
    return f


def _fp_store_x(xo):
    return _x_form(xo, FPR)


def _crop(xo, form="dab"):
    def f(b, o):
        if form == "dab":
            _ops(o, 3, "this CR instruction")
            d, a, bb = (_crbit(x) for x in o)
        elif form == "ddd":  # crset/crclr bx
            _ops(o, 1, "this CR instruction")
            d = a = bb = _crbit(o[0])
        else:  # "dbb": crmove/crnot bx, by
            _ops(o, 2, "this CR instruction")
            d, a = _crbit(o[0]), _crbit(o[1])
            bb = a
        b.word |= 19 << 26 | d << 21 | a << 16 | bb << 11 | xo << 1
    return f


def _mcrf(b, o):
    _ops(o, 2, "mcrf")
    b.word |= 19 << 26 | _reg(o[0], "cr") << 23 | _reg(o[1], "cr") << 18


def _fixed(word):
    def f(b, o):
        if o:
            raise PatchError("this instruction takes no operands")
        b.word |= word
    return f


def _b(aa, lk):
    def f(b, o):
        _ops(o, 1, "b")
        b.word |= 18 << 26 | aa << 1 | lk
        b.branch(o[0], "b26")
    return f


def _bc_raw(aa, lk):  # bc BO, BI, target
    def f(b, o):
        _ops(o, 3, "bc")
        bo, bi = _constant(o[0]), _crbit(o[1])
        if bo is None:
            raise PatchError("BO must be a constant")
        b.word |= 16 << 26 | int(bo) << 21 | bi << 16 | aa << 1 | lk
        b.branch(o[2], "b16")
    return f


def _bclr_raw(xo, lk):  # bclr/bcctr BO, BI
    def f(b, o):
        _ops(o, 2, "bclr/bcctr")
        bo, bi = _constant(o[0]), _crbit(o[1])
        if bo is None:
            raise PatchError("BO must be a constant")
        b.word |= 19 << 26 | int(bo) << 21 | bi << 16 | xo << 1 | lk
    return f


def _bcond(bo, bit, kind, aa=0, lk=0, needs_bi=False):
    """b<cond> [crN,] target / b<cond>lr [crN] / b<cond>ctr [crN] (bit: the condition's bit of the CR
    field), bdnz target / bdnzlr (bit None), and bt/bf/bdnzt... BI, target (needs_bi). kind: "b16" for a
    branch with a target, else the XO of bclr (16) or bcctr (528)."""
    def f(b, o):
        target = kind == "b16"
        if needs_bi:
            _ops(o, 2 if target else 1, "this branch")
            bi, rest = _crbit(o[0]), o[1:]
        elif bit is None:
            _ops(o, 1 if target else 0, "this branch")
            bi, rest = 0, o
        else:
            if len(o) == (2 if target else 1):
                bi, rest = 4 * _reg(o[0], "cr") + bit, o[1:]
            elif len(o) == (1 if target else 0):
                bi, rest = bit, o
            else:
                raise PatchError("this branch takes %s" % ("[crN,] target" if target else "[crN]"))
        b.word |= bo << 21 | bi << 16 | lk
        if target:
            b.word |= 16 << 26 | aa << 1
            b.branch(rest[0], "b16")
        else:
            b.word |= 19 << 26 | kind << 1
    return f


def _build_table():
    t = {}
    # integer arithmetic with immediates
    t["addi"] = _rD_rA_simm(14)
    t["addis"] = _rD_rA_simm(15)
    t["addic"] = _rD_rA_simm(12)
    t["addic."] = _rD_rA_simm(13)
    t["subfic"] = _rD_rA_simm(8)
    t["mulli"] = _rD_rA_simm(7)
    t["subi"] = _rD_rA_simm(14, negate=True)
    t["subis"] = _rD_rA_simm(15, negate=True)
    t["subic"] = _rD_rA_simm(12, negate=True)
    t["subic."] = _rD_rA_simm(13, negate=True)
    t["li"] = _rD_simm(14)
    t["lis"] = _rD_simm(15)

    def la(b, o):  # la rD, d(rA) = addi rD, rA, d
        _ops(o, 2, "la")
        b.word |= 14 << 26 | _reg(o[0], GPR) << 21
        b.mem(o[1])
    t["la"] = la
    t["ori"] = _rA_rS_uimm(24)
    t["oris"] = _rA_rS_uimm(25)
    t["xori"] = _rA_rS_uimm(26)
    t["xoris"] = _rA_rS_uimm(27)
    t["andi."] = _rA_rS_uimm(28)
    t["andis."] = _rA_rS_uimm(29)
    t["nop"] = _fixed(0x60000000)
    t["cmpwi"] = _cmp_imm(11, True)
    t["cmplwi"] = _cmp_imm(10, False)
    t["cmpw"] = _cmp_reg(0)
    t["cmplw"] = _cmp_reg(32)
    t["fcmpu"] = _cmp_reg(0, fp=True)
    t["fcmpo"] = _cmp_reg(32, fp=True)

    def cmp_raw(op_or_xo, imm, signed):
        def f(b, o):
            _ops(o, 4, "cmp/cmpl/cmpi/cmpli")
            cr, l, ra = _reg(o[0], "cr"), _constant(o[1]), _reg(o[2], GPR)
            if l not in (0.0, 0):
                raise PatchError("only 32-bit compares (L = 0)")
            if imm:
                b.word |= op_or_xo << 26 | cr << 23 | ra << 16
                b.imm(o[3], 0, 16, signed)
            else:
                b.word |= 31 << 26 | cr << 23 | ra << 16 | _reg(o[3], GPR) << 11 | op_or_xo << 1
        return f
    t["cmpi"] = cmp_raw(11, True, True)
    t["cmpli"] = cmp_raw(10, True, False)
    t["cmp"] = cmp_raw(0, False, True)
    t["cmpl"] = cmp_raw(32, False, False)
    # loads and stores
    for name, op in (("lwz", 32), ("lwzu", 33), ("lbz", 34), ("lbzu", 35), ("stw", 36), ("stwu", 37), ("stb", 38),
                     ("stbu", 39), ("lhz", 40), ("lhzu", 41), ("lha", 42), ("lhau", 43), ("sth", 44), ("sthu", 45),
                     ("lmw", 46), ("stmw", 47)):
        t[name] = _mem_form(op, GPR)
    for name, op in (("lfs", 48), ("lfsu", 49), ("lfd", 50), ("lfdu", 51), ("stfs", 52), ("stfsu", 53), ("stfd", 54),
                     ("stfdu", 55)):
        t[name] = _mem_form(op, FPR)
    for name, xo in (("lwzx", 23), ("lwzux", 55), ("lbzx", 87), ("lbzux", 119), ("stwx", 151), ("stwux", 183),
                     ("stbx", 215), ("stbux", 247), ("lhzx", 279), ("lhzux", 311), ("lhax", 343), ("lhaux", 375),
                     ("sthx", 407), ("sthux", 439), ("lwbrx", 534), ("stwbrx", 662), ("lhbrx", 790), ("sthbrx", 918),
                     ("lwarx", 20)):
        t[name] = _x_form(xo)
    t["stwcx."] = _x_form(150, rc=1)
    for name, xo in (("lfsx", 535), ("lfsux", 567), ("lfdx", 599), ("lfdux", 631), ("stfsx", 663), ("stfsux", 695),
                     ("stfdx", 727), ("stfdux", 759), ("stfiwx", 983)):
        t[name] = _x_form(xo, FPR)
    # integer arithmetic, register forms
    for name, xo, n in (("add", 266, 3), ("addc", 10, 3), ("adde", 138, 3), ("addme", 234, 2), ("addze", 202, 2),
                        ("divw", 491, 3), ("divwu", 459, 3), ("mullw", 235, 3), ("neg", 104, 2), ("subf", 40, 3),
                        ("subfc", 8, 3), ("subfe", 136, 3), ("subfme", 232, 2), ("subfze", 200, 2)):
        for oe in (0, 1):
            for rc in (0, 1):
                t[name + ("o" if oe else "") + ("." if rc else "")] = _xo_form(xo, n, oe, rc)
    for name, xo in (("mulhw", 75), ("mulhwu", 11)):
        t[name] = _xo_form(xo, 3, 0, 0)
        t[name + "."] = _xo_form(xo, 3, 0, 1)
    for name, xo in (("sub", 40), ("subc", 8)):
        for oe in (0, 1):
            for rc in (0, 1):
                t[name + ("o" if oe else "") + ("." if rc else "")] = _xo_form(xo, 3, oe, rc, swap=True)
    for name, xo in (("and", 28), ("or", 444), ("xor", 316), ("andc", 60), ("orc", 412), ("nand", 476), ("nor", 124),
                     ("eqv", 284), ("slw", 24), ("srw", 536), ("sraw", 792)):
        t[name] = _logic(xo, 0)
        t[name + "."] = _logic(xo, 1)
    for name, xo in (("cntlzw", 26), ("extsb", 954), ("extsh", 922)):
        t[name] = _logic(xo, 0, nregs=2)
        t[name + "."] = _logic(xo, 1, nregs=2)
    t["mr"] = _logic(444, 0, same=True)
    t["mr."] = _logic(444, 1, same=True)
    t["not"] = _logic(124, 0, same=True)
    t["not."] = _logic(124, 1, same=True)
    t["srawi"] = _srawi(0)
    t["srawi."] = _srawi(1)
    for name, op in (("rlwinm", 21), ("rlwimi", 20), ("rlwnm", 23)):
        t[name] = _rotate(op, 0)
        t[name + "."] = _rotate(op, 1)
    for name, (op, form) in _ROTATE_SHORTHANDS.items():
        t[name] = _rotate(op, 0, form)
        t[name + "."] = _rotate(op, 1, form)

    def rotlw(rc):
        def f(b, o):
            _ops(o, 3, "rotlw")
            b.word |= 23 << 26 | _reg(o[1], GPR) << 21 | _reg(o[0], GPR) << 16 | _reg(o[2], GPR) << 11 | 31 << 1 | rc
        return f
    t["rotlw"] = rotlw(0)
    t["rotlw."] = rotlw(1)
    # special registers
    t["mfspr"] = _mfspr()
    t["mtspr"] = _mtspr()
    for name, spr in _SPR.items():
        t["mf" + name] = _mfspr(spr)
        t["mt" + name] = _mtspr(spr)

    def mfcr(b, o):
        _ops(o, 1, "mfcr")
        b.word |= 31 << 26 | _reg(o[0], GPR) << 21 | 19 << 1
    t["mfcr"] = mfcr

    def mtcrf(b, o):
        _ops(o, 2, "mtcrf")
        crm = _constant(o[0])
        if crm is None or not 0 <= crm <= 255:
            raise PatchError("mtcrf mask must be a constant from 0 to 255")
        b.word |= 31 << 26 | _reg(o[1], GPR) << 21 | int(crm) << 12 | 144 << 1
    t["mtcrf"] = mtcrf

    def mtcr(b, o):
        _ops(o, 1, "mtcr")
        b.word |= 31 << 26 | _reg(o[0], GPR) << 21 | 0xFF << 12 | 144 << 1
    t["mtcr"] = mtcr
    # floating point
    for name, xo, regs in (("fdivs", 18, "dab"), ("fsubs", 20, "dab"), ("fadds", 21, "dab"), ("fres", 24, "db"),
                           ("fmuls", 25, "dac"), ("fmsubs", 28, "dacb"), ("fmadds", 29, "dacb"), ("fnmsubs", 30, "dacb"),
                           ("fnmadds", 31, "dacb")):
        t[name] = _fp(59, xo, regs, 0)
        t[name + "."] = _fp(59, xo, regs, 1)
    for name, xo, regs in (("frsp", 12, "db"), ("fctiw", 14, "db"), ("fctiwz", 15, "db"), ("fdiv", 18, "dab"),
                           ("fsub", 20, "dab"), ("fadd", 21, "dab"), ("fsel", 23, "dacb"), ("fmul", 25, "dac"),
                           ("frsqrte", 26, "db"), ("fmsub", 28, "dacb"), ("fmadd", 29, "dacb"), ("fnmsub", 30, "dacb"),
                           ("fnmadd", 31, "dacb"), ("fneg", 40, "db"), ("fmr", 72, "db"), ("fnabs", 136, "db"),
                           ("fabs", 264, "db"), ("mffs", 583, "d")):
        t[name] = _fp(63, xo, regs, 0)
        t[name + "."] = _fp(63, xo, regs, 1)
    # condition register
    for name, xo in (("crand", 257), ("cror", 449), ("crxor", 193), ("crnand", 225), ("crnor", 33), ("creqv", 289),
                     ("crandc", 129), ("crorc", 417)):
        t[name] = _crop(xo)
    t["crset"] = _crop(289, "ddd")
    t["crclr"] = _crop(193, "ddd")
    t["crmove"] = _crop(449, "dbb")
    t["crnot"] = _crop(33, "dbb")
    t["mcrf"] = _mcrf
    # branches
    t["b"], t["ba"], t["bl"], t["bla"] = _b(0, 0), _b(1, 0), _b(0, 1), _b(1, 1)
    t["bc"], t["bca"], t["bcl"], t["bcla"] = _bc_raw(0, 0), _bc_raw(1, 0), _bc_raw(0, 1), _bc_raw(1, 1)
    t["bclr"], t["bclrl"] = _bclr_raw(16, 0), _bclr_raw(16, 1)
    t["bcctr"], t["bcctrl"] = _bclr_raw(528, 0), _bclr_raw(528, 1)
    t["blr"], t["blrl"] = _fixed(0x4E800020), _fixed(0x4E800021)
    t["bctr"], t["bctrl"] = _fixed(0x4E800420), _fixed(0x4E800421)
    conditions = {"lt": (12, 0), "le": (4, 1), "eq": (12, 2), "ge": (4, 0), "gt": (12, 1), "nl": (4, 0), "ne": (4, 2),
                  "ng": (4, 1), "so": (12, 3), "ns": (4, 3), "un": (12, 3), "nu": (4, 3)}
    for cond, (bo, bit) in conditions.items():
        for suffix, kind, aa, lk in (("", "b16", 0, 0), ("a", "b16", 1, 0), ("l", "b16", 0, 1), ("la", "b16", 1, 1),
                                     ("lr", 16, 0, 0), ("lrl", 16, 0, 1), ("ctr", 528, 0, 0), ("ctrl", 528, 0, 1)):
            t["b" + cond + suffix] = _bcond(bo, bit, kind, aa, lk)
    for name, bo, needs in (("dnz", 16, False), ("dz", 18, False), ("t", 12, True), ("f", 4, True), ("dnzt", 8, True),
                            ("dnzf", 0, True), ("dzt", 10, True), ("dzf", 2, True)):
        for suffix, kind, aa, lk in (("", "b16", 0, 0), ("a", "b16", 1, 0), ("l", "b16", 0, 1), ("la", "b16", 1, 1),
                                     ("lr", 16, 0, 0), ("lrl", 16, 0, 1)):
            t["b" + name + suffix] = _bcond(bo, None, kind, aa, lk, needs_bi=needs)
        if needs and name in ("t", "f"):
            t["b" + name + "ctr"] = _bcond(bo, None, 528, 0, 0, needs_bi=True)
            t["b" + name + "ctrl"] = _bcond(bo, None, 528, 0, 1, needs_bi=True)
    # synchronisation and traps
    t["sync"] = _fixed(0x7C0004AC)
    t["isync"] = _fixed(0x4C00012C)
    t["eieio"] = _fixed(0x7C0006AC)
    t["trap"] = _fixed(0x7FE00008)

    def tw(b, o):
        _ops(o, 3, "tw")
        to = _constant(o[0])
        if to is None:
            raise PatchError("TO must be a constant")
        b.word |= 31 << 26 | int(to) << 21 | _reg(o[1], GPR) << 16 | _reg(o[2], GPR) << 11 | 4 << 1
    t["tw"] = tw

    def twi(b, o):
        _ops(o, 3, "twi")
        to = _constant(o[0])
        if to is None:
            raise PatchError("TO must be a constant")
        b.word |= 3 << 26 | int(to) << 21 | _reg(o[1], GPR) << 16
        b.imm(o[2], 0, 16, True)
    t["twi"] = twi
    return t


INSTRUCTIONS = _build_table()
_ALIASES = {"bnl": "bgt", "bng": "ble"}  # as Cemu's _ppcAssembler_translateAlias (sic)
_DATA = {".float": ("float", 4), ".double": ("double", 8), ".int": ("u32", 4), ".uint": ("u32", 4), ".ptr": ("u32", 4),
         ".u32": ("u32", 4), ".long": ("u32", 4), ".word": ("u16", 2), ".u16": ("u16", 2), ".short": ("u16", 2),
         ".byte": ("u8", 1), ".string": ("u8", 1), ".u8": ("u8", 1), ".char": ("u8", 1)}


def _unescape(body):
    out, i = bytearray(), 0
    while i < len(body):
        c = body[i]
        i += 1
        if c != "\\":
            out += c.encode("utf-8")
            continue
        if i >= len(body):
            break
        c = body[i].lower()
        i += 1
        if c == "\\":
            out.append(0x5C)
        elif c == "n":
            out.append(10)
        elif c == "t":
            out.append(9)
        elif c == "r":
            out.append(13)
        elif c == '"':
            out.append(0x22)
        elif c == "'":
            out.append(0x27)
        elif c == "x":
            j = i
            while j < len(body) and _xdigit(body[j]):
                j += 1
            if j == i:
                raise PatchError("string contains an invalid hex escape sequence")
            out.append(int(body[i:j], 16) & 0xFF)
            i = j
            # Cemu stops reading the string after a \x escape
            break
        else:
            out.append(0x5C)
    return bytes(out)


def assemble(text, address, fixed_address=False):
    """ppcAssembler_assembleSingleInstruction: one instruction or data directive at `address`.
    Returns (aligned address, Assembled). fixed_address: a line with an <address> = prefix, which is
    never padded."""
    text = text.strip()
    m = re.match(r"(\S+)\s*(.*)$", text, re.S)
    name, rest = (m.group(1).lower(), m.group(2)) if m else ("", "")
    if not name:
        raise PatchError("instruction name is invalid")
    if name.endswith(("+", "-")) and len(name) > 1:  # branch hint: not part of the meaning
        name = name[:-1]
    name = _ALIASES.get(name, name)
    operands = _split_operands(rest) if rest.strip() else []

    def aligned(n):
        return address if fixed_address else (address + n - 1) & ~(n - 1)

    if name in _DATA:
        kind, size = _DATA[name]
        if not operands:
            raise PatchError("value expected after data directive %s" % name)
        at = aligned(size)
        data, relocs = bytearray(), []
        for op in operands:
            if op.startswith('"'):
                if kind != "u8":
                    raise PatchError("string constants are only allowed in .byte or .string data directives")
                if len(op) < 2 or not op.endswith('"'):
                    raise PatchError("string constants must end with a quotation mark")
                data += _unescape(op[1:-1]) + b"\0"
                continue
            rpn = compile_expression(op)
            relocs.append(Reloc(kind, rpn, len(data)))
            data += bytes(size)
        return at, Assembled(data, relocs, size, False)
    if name == ".align":
        if len(operands) != 1:
            raise PatchError(".align directive must have exactly one operand")
        v = _constant(operands[0])
        if v is None:
            raise PatchError(".align value \"%s\" is not a constant" % operands[0])
        n = int(v)
        if not 0 < n < 256:
            raise PatchError("alignment value \"%s\" is not within the allowed range (1-256)" % operands[0])
        pad = (-address) % n
        return address, Assembled(bytearray(pad), [], 1, False)
    f = INSTRUCTIONS.get(name)
    if f is None:
        raise PatchError("unknown instruction \"%s\"" % name)
    b = _Builder(0)
    f(b, operands if operands else [])
    return aligned(4), Assembled(bytearray(struct.pack(">I", b.word & 0xFFFFFFFF)), b.relocs, 4, True)


# ------------------------------------------------------------------------------------ the patch files

class Entry:
    """One line of a patch file: kind "item" (instruction or data at `address`), "label" (at `address`,
    or placed at the next item: address None until then), "var" (name = expression) or "symbol"
    (Cemuhook's name = address)."""
    __slots__ = ("kind", "line", "file", "address", "name", "rpn", "item")

    def __init__(self, kind, line, file, address=None, name=None, rpn=None, item=None):
        self.kind, self.line, self.file, self.address = kind, line, file, address
        self.name, self.rpn, self.item = name, rpn, item

    def where(self):
        return "%s line %d" % (self.file, self.line)


class Group:
    def __init__(self, name, file):
        self.name, self.file = name, file
        self.modules = []
        self.rpx = False
        self.entries = []
        self.cave_size = 0
        self.callbacks = []

    def matches(self, crc):
        return self.rpx or crc in self.modules


_SYMBOL = re.compile(r"[A-Za-z_.$][A-Za-z0-9_.$]*")


def _comment_free(line):
    """GetLengthWithoutComment: # and ; outside "strings" end the line."""
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c in "#;" and not quoted:
            return line[:i]
    return line


def _parse_u32(s):
    m = re.match(r"\s*(0[xX][0-9A-Fa-f]+|\d+)", s)
    if not m:
        return None, s
    v = int(m.group(1), 0) if m.group(1)[:2].lower() == "0x" else int(m.group(1))
    return v & 0xFFFFFFFF, s[m.end():]


def _module_matches(group, rest, where):
    if not rest.lstrip().startswith("="):
        raise PatchError("%s: expected '=' after moduleMatches" % where)
    rest = rest.lstrip()[1:]
    while True:
        if rest.strip().lower().startswith("rpx"):
            group.rpx = True
            return
        v, rest = _parse_u32(rest)
        if v is None:
            raise PatchError("%s: invalid value for moduleMatches" % where)
        group.modules.append(v)
        rest = rest.lstrip()
        if not rest.startswith(","):
            break
        rest = rest[1:]
    if rest.strip():
        raise PatchError("%s: unexpected character in moduleMatches" % where)


def parse_patch_file(text, filename):
    """ParseCemuPatchesTxtInternal (patch_*.asm): the groups of one file."""
    groups = []
    group = None
    origin = None   # None: no .origin yet
    cave = 0        # the group's code cave end so far
    scheduled = []  # labels waiting for the next item

    def flush(address):
        for label in scheduled:
            label.address = address
            group.entries.append(label)
        scheduled.clear()

    def finish():
        if group is None:
            return
        flush(origin if origin is not None else 0)
        group.cave_size = max([e.address + len(e.item.data) for e in group.entries
                               if e.kind == "item" and e.address < CODECAVE_MAX_OFFSET] or [0])
        if not group.modules and not group.rpx:
            raise PatchError("%s: group [%s] has no moduleMatches" % (filename, group.name))
        groups.append(group)

    for number, raw in enumerate(text.splitlines(), 1):
        where = "%s line %d" % (filename, number)
        line = _comment_free(raw).strip()
        if not line:
            continue
        if line.startswith("["):
            end = line.find("]")
            if end < 0:
                raise PatchError("%s: expected ']'" % where)
            if line[end + 1:].strip():
                raise PatchError("%s: unexpected characters after ']'" % where)
            finish()
            group = Group(line[1:end], filename)
            origin, cave = None, 0
            continue
        if line.lower().startswith("modulematches"):
            if group is None:
                raise PatchError("%s: moduleMatches outside of a group" % where)
            _module_matches(group, line[len("modulematches"):], where)
            continue
        if group is None:
            raise PatchError("%s: a patch line outside of a group" % where)
        fixed = None
        if line[:2].lower() == "0x":
            fixed, rest = _parse_u32(line)
            rest = rest.lstrip()
            if fixed is None or not rest.startswith("="):
                raise PatchError("%s: expected '=' after the address" % where)
            line = rest[1:].strip()
        low = line.lower()
        if low.startswith(".origin"):
            if fixed is not None:
                raise PatchError("%s: .origin must appear alone, without <address> =" % where)
            rest = line[len(".origin"):].strip()
            if not rest.startswith("="):
                raise PatchError("%s: missing '=' after .origin" % where)
            rest = rest[1:].strip()
            if rest.lower().startswith("codecave"):
                origin = cave
            else:
                v, tail = _parse_u32(rest)
                if v is None or tail.strip():
                    raise PatchError("%s: '.origin =' must be followed by codecave or an address" % where)
                origin = v
            continue
        if low.startswith(".callback"):
            raise PatchError("%s: .callback (code Cemu runs at startup) is not supported" % where)
        m = _SYMBOL.match(line)
        if m and line[m.end():].lstrip().startswith(":") and not line[m.end():].lstrip()[1:].strip():
            label = Entry("label", number, filename, name=m.group(0))
            if fixed is not None:
                label.address = fixed
                group.entries.append(label)
            else:
                if origin is None:
                    raise PatchError("%s: label %s has no address (no .origin and no <address> =)" % (where, m.group(0)))
                scheduled.append(label)
            continue
        if m and line[m.end():].lstrip().startswith("=") and not line[m.end():].lstrip().startswith("=="):
            expr = line[m.end():].lstrip()[1:].strip()
            group.entries.append(Entry("var", number, filename, name=m.group(0), rpn=compile_expression(expr)))
            continue
        if fixed is not None:
            address = fixed
        elif origin is not None:
            address = origin
        else:
            raise PatchError("%s: no address for this line (declare .origin or prefix it with <address> =)" % where)
        try:
            at, item = assemble(line, address, fixed_address=fixed is not None)
        except PatchError as e:
            raise PatchError("%s: %s (in \"%s\")" % (where, e, line)) from None
        if fixed is None:
            origin = at + len(item.data)
            if origin <= 32 * 1024 * 1024:
                cave = max(cave, origin)
        flush(at)
        group.entries.append(Entry("item", number, filename, address=at, item=item))
    finish()
    return groups


def parse_cemuhook_patches(text, filename):
    """ParseCemuhookPatchesTxtInternal (patches.txt): groups of <address> = <instruction> lines and
    name = address symbols, without code caves."""
    groups, group = [], None
    for number, raw in enumerate(text.splitlines(), 1):
        where = "%s line %d" % (filename, number)
        line = _comment_free(raw).strip()
        if not line:
            continue
        if line.startswith("["):
            end = line.find("]")
            if end < 0 or line[end + 1:].strip():
                raise PatchError("%s: malformed group name" % where)
            if group is not None:
                groups.append(group)
            group = Group(line[1:end], filename)
            continue
        if line[:2].lower() == "0x":
            address, rest = _parse_u32(line)
            rest = rest.lstrip()
            if address is None or not rest.startswith("="):
                raise PatchError("%s: expected '=' after the address" % where)
            try:
                at, item = assemble(rest[1:].strip(), address, fixed_address=True)
            except PatchError as e:
                raise PatchError("%s: %s" % (where, e)) from None
            group.entries.append(Entry("item", number, filename, address=at, item=item))
            continue
        if group is None:
            raise PatchError("%s: a patch line outside of a group" % where)
        if line.lower().startswith("modulematches"):
            _module_matches(group, line[len("modulematches"):], where)
            continue
        m = _SYMBOL.match(line)
        if m and line[m.end():].lstrip().startswith("="):
            group.entries.append(Entry("symbol", number, filename, name=m.group(0),
                                       rpn=compile_expression(line[m.end():].lstrip()[1:].strip())))
            continue
        raise PatchError("%s: invalid syntax" % where)
    if group is not None:
        groups.append(group)
    for g in groups:
        if not g.modules and not g.rpx:
            raise PatchError("%s: group [%s] has no moduleMatches" % (filename, g.name))
    return groups


def patch_files(folder):
    """The pack's patch files, as Cemu loads them: every patch_*.asm, or else patches.txt."""
    names = sorted(os.listdir(folder))
    asm = [n for n in names if n.lower().startswith("patch_") and n.lower().endswith(".asm")
           and os.path.isfile(os.path.join(folder, n))]
    if asm:
        return asm, False
    txt = [n for n in names if n.lower() == "patches.txt" and os.path.isfile(os.path.join(folder, n))]
    return txt, True


def read_text(path, limit=4 << 20):
    if os.path.getsize(path) > limit:
        raise PatchError("%s is too large" % os.path.basename(path))
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def load_groups(folder, crc):
    """The groups of a pack folder that apply to the module with checksum `crc`."""
    files, cemuhook = patch_files(folder)
    groups = []
    for name in files:
        text = read_text(os.path.join(folder, name))
        parsed = parse_cemuhook_patches(text, name) if cemuhook else parse_patch_file(text, name)
        groups += [g for g in parsed if g.matches(crc)]
    return groups


# ------------------------------------------------------------------------------------- resolving

class Item:
    """An instruction or datum placed at its final address. `values`: [(Reloc, Rpn)], relocations whose
    value comes from $preset variables at run time (the Rpn names nothing else); `import_call`:
    (module, function) for a bl to import.<module>.<function>; `refs`: the labels and variables its
    data or immediates name (not its branch targets)."""
    __slots__ = ("address", "data", "code", "values", "import_call", "entry", "refs")

    def __init__(self, address, data, code, entry):
        self.address, self.data, self.code, self.entry = address, data, code, entry
        self.values, self.import_call, self.refs = [], None, set()


class Resolved:
    def __init__(self):
        self.items = []    # every Item, in file order
        self.symbols = {}  # name -> value (int), or an Rpn for a variable of $presets (taken as uint32)
        self.labels = {}   # name -> address
        self.caves = []    # (base, size, group name)
        self.presets_used = set()


class _Dynamic(Exception):
    """An expression reads a $preset variable (or a variable that does)."""


class _Import(Exception):
    def __init__(self, name):
        super().__init__(name)
        self.name = name


def _known_preset(rules, name):
    if rules is None:
        return False
    return name in rules.defaults or any(name in p.values for p in rules.presets)


_RELOC_FUNCS = {"ha": "ha", "ha16": "ha", "hi": "hi", "hi16": "hi", "lo": "lo", "lo16": "lo"}


def resolve(groups, rules, cave_base, in_module, imports=None):
    """Places a pack's groups (load_groups) as Cemu's ApplyPatchGroups does. cave_base(size) gives each
    group with a code cave its address; in_module(address) says whether an address is in one of the
    module's sections; imports: the (module, function) pairs the game imports (None: any).
    Expressions that read $preset variables stay programs (Item.values, Resolved.symbols)."""
    out = Resolved()
    bases = {}
    for g in groups:
        if g.cave_size:
            bases[id(g)] = cave_base(g.cave_size)
            out.caves.append((bases[id(g)], g.cave_size, g.name))

    def relocate(g, address, entry):
        if address <= CODECAVE_MAX_OFFSET:
            if id(g) not in bases:
                raise PatchError("%s: address %08X is in the code cave of a group that has none" % (entry.where(), address))
            return bases[id(g)] + address
        if not in_module(address):
            raise PatchError("%s: address %08X is not in the code cave or the game's code or data" % (entry.where(), address))
        return address

    def lookup(name, refs=None):
        if name.startswith("$"):
            if not _known_preset(rules, name):
                raise PatchError("%s is not a variable of rules.txt" % name)
            out.presets_used.add(name)
            raise _Dynamic(name)
        if name.lower().startswith("import."):
            raise _Import(name)
        if name in out.symbols:
            v = out.symbols[name]
            if isinstance(v, Rpn):
                raise _Dynamic(name)
            if refs is not None:
                refs.add(name)
            return float(v)
        raise Unresolved(name)

    def functions(g, e):
        def call(name, value):
            if name in _RELOC_FUNCS:
                return MODIFIER_FUNCS[_RELOC_FUNCS[name]](value)
            if name == "reloc":
                return float(relocate(g, to_u32(value), e))
            raise PatchError("unknown function %s()" % name)
        return call

    def resolvable(rpn):  # every name but the $presets is known
        return all(n.startswith("$") or n in out.symbols for n in rpn.names())

    pending = []
    for g in groups:
        for e in g.entries:
            if e.kind == "label":  # labels need no expressions
                if e.name in out.symbols:
                    raise PatchError("%s: label %s is already defined" % (e.where(), e.name))
                out.symbols[e.name] = out.labels[e.name] = relocate(g, e.address, e)
            elif e.kind in ("var", "symbol"):
                pending.append((g, e))
    for _ in range(30):  # as Cemu: references may go forward and across groups
        progress = False
        for g, e in list(pending):
            try:
                v = evaluate(e.rpn, lookup, call=functions(g, e))
            except Unresolved:
                continue
            except _Dynamic:
                if not resolvable(e.rpn):
                    continue
                if e.kind == "symbol":
                    raise PatchError("%s: address %s can't depend on presets" % (e.where(), e.name))
                value = _inline(e.rpn, out.symbols, e)
            except _Import as i:
                raise PatchError("%s: %s can only be the target of a call" % (e.where(), i.name))
            except PatchError as err:
                raise PatchError("%s: %s" % (e.where(), err)) from None
            else:
                value = to_u32(v)
                if e.kind == "symbol":
                    value = relocate(g, value, e)
            if e.name in out.symbols:
                raise PatchError("%s: %s is already defined" % (e.where(), e.name))
            out.symbols[e.name] = value
            pending.remove((g, e))
            progress = True
        if not pending or not progress:
            break
    if pending:
        g, e = pending[0]
        missing = [n for n in e.rpn.names() if not n.startswith("$") and n not in out.symbols]
        raise PatchError("%s: unresolved symbol %s" % (e.where(), missing[0] if missing else e.name))

    for g in groups:
        for e in g.entries:
            if e.kind != "item":
                continue
            item = Item(relocate(g, e.address, e), bytearray(e.item.data), e.item.code, e)
            for r in e.item.relocs:
                try:
                    _apply_reloc(item, r, out, lookup, functions(g, e), imports)
                except PatchError as err:
                    raise PatchError("%s: %s (in \"%s\")" % (e.where(), err, r.rpn.text)) from None
            out.items.append(item)
    return out


def _inline(rpn, symbols, entry=None):
    """An Rpn with only $presets left for the runtime: labels and variables become their values, and
    variables of $presets are inlined with their conversion to uint32 ("fn:u32")."""
    tokens = []
    for t in rpn.tokens:
        if isinstance(t, str) and not t.startswith("$"):
            v = symbols.get(t)
            if v is None:
                raise PatchError("unresolved symbol %s" % t)
            if isinstance(v, Rpn):
                tokens += v.tokens + ([("fn", v.modifier)] if v.modifier else []) + [("fn", "u32")]
            else:
                tokens.append(float(v))
        elif isinstance(t, tuple) and t[0] == "fn":
            name = t[1].lower()
            if name == "u32":
                tokens.append(t)
            elif name in _RELOC_FUNCS:
                tokens.append(("fn", _RELOC_FUNCS[name]))
            else:
                raise PatchError("%s() can't be applied to a preset value" % t[1])
        else:
            tokens.append(t)
    return Rpn(tokens, rpn.modifier, rpn.text)


def evaluate_runtime(rpn, presets):
    """What the runtime computes for an inlined Rpn (_inline) from the $preset values (a dict)."""
    def call(name, value):
        if name == "u32":
            return float(to_u32(value))
        return MODIFIER_FUNCS[_RELOC_FUNCS[name]](value)
    return evaluate(rpn, lambda n: presets[n], call=call)


def _apply_reloc(item, r, out, lookup, call, imports):
    refs = set()
    try:
        v = evaluate(r.rpn, lambda n: lookup(n, refs), call=call)
    except _Dynamic:
        if r.kind in ("b16", "b26"):
            raise PatchError("a branch target can't depend on presets")
        item.values.append((r, _inline(r.rpn, out.symbols)))
        item.refs |= set(n for n in r.rpn.names() if not n.startswith("$"))
        return
    except _Import as i:
        if r.kind != "b26" or r.rpn.tokens != [i.name] or r.rpn.modifier:
            raise PatchError("%s can only be the target of a call" % i.name)
        parts = i.name.split(".")
        if len(parts) != 3 or not parts[1] or not parts[2]:
            raise PatchError("invalid import syntax %s (import.<module>.<function>)" % i.name)
        if imports is not None and (parts[1], parts[2]) not in imports:
            raise PatchError("the game doesn't import %s.%s" % (parts[1], parts[2]))
        if not struct.unpack(">I", bytes(item.data[0:4]))[0] & 1:
            raise PatchError("%s can only be called (bl), not branched to" % i.name)
        item.import_call = (parts[1], parts[2])
        return
    except Unresolved as u:
        raise PatchError("unresolved symbol %s" % u.name) from None
    if r.kind not in ("b16", "b26"):
        item.refs |= refs
    store_reloc(item, r, v)


def store_reloc(item, r, v):
    """Writes a relocation's value into the item, as Cemu's PatchEntryInstruction::resolveReloc."""
    data = item.data
    if r.kind == "float":
        data[r.at:r.at + 4] = struct.pack(">f", _to_float(v))
    elif r.kind == "double":
        data[r.at:r.at + 8] = struct.pack(">d", v)
    elif r.kind == "u32":
        data[r.at:r.at + 4] = struct.pack(">I", to_u32(v))
    elif r.kind == "u16":
        data[r.at:r.at + 2] = struct.pack(">H", to_u32(v) & 0xFFFF)
    elif r.kind == "u8":
        data[r.at] = to_u32(v) & 0xFF
    elif r.kind == "imm":
        word = struct.unpack(">I", bytes(data[0:4]))[0]
        mask = ((1 << r.bits) - 1) << r.shift
        word = (word & ~mask) | ((to_u32(v) << r.shift) & mask)
        data[0:4] = struct.pack(">I", word & 0xFFFFFFFF)
    else:
        word = struct.unpack(">I", bytes(data[0:4]))[0]
        target = to_u32(v)
        here = item.address
        if r.kind == "b26" and word & 2:  # absolute
            if target >= 0x3FFFFFC:
                raise PatchError("branch target %08X out of range for an absolute branch" % target)
            field = target
        else:  # relative (Cemu ignores AA for 16-bit branches)
            limit = (0x1FFFFFF, 0x1FFFFFF) if r.kind == "b26" else (0x8000, 0x8000)
            if target < here and here - target > limit[0] or target >= here and target - here >= limit[1]:
                raise PatchError("branch target %08X out of range (use mtctr + bctr for long distances)" % target)
            field = (target - here) & 0xFFFFFFFF
        m = 0x3FFFFFC if r.kind == "b26" else 0xFFFC
        data[0:4] = struct.pack(">I", (word & ~m) | (field & m))


def _to_float(v):
    """(float)double: out of float range becomes infinity, as in C."""
    try:
        struct.pack(">f", v)
        return v
    except OverflowError:
        return math.copysign(math.inf, v)
