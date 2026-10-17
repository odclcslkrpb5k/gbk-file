#!/usr/bin/env python3
"""Source rules the compiler can't state, run by ./build check.

  check-source.py            check src/ and tests/
  check-source.py self-test  prove each rule refuses its bad examples and passes its good ones

Rules:
  nodiscard   a function that reports failure through GError ** and returns a value is
              [[nodiscard]]: a declaration in a header, or a static function. (An exported
              function's definition is covered by its header's declaration.)
  unsequenced one argument of a call hands &x to a nested call while another argument reads
              x: the order the arguments are evaluated in is unspecified, so x may be read
              before the nested call has written it. No compiler warning or analyzer here
              catches it (GCC -Wsequence-point, Clang -Wunsequenced, clang-tidy, cppcheck).

The matching is textual (comments removed, then regular expressions), so it is written to
fail closed: a declaration it can't read is not silently passed, it is simply not seen.
That gap is what the self-test's examples pin down.
"""

import glob
import re
import sys

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# return type and qualifiers, function name, a parameter list holding GError **, ; or {
FUNCTION = re.compile(
    r"(?:^|[;}\n])\s*((?:\[\[nodiscard\]\]\s*)?(?:static\s+)?(?:inline\s+)?"
    r"[A-Za-z_][A-Za-z_0-9 \t\n*]*?)\b([a-z_][a-z_0-9]*)\s*\(([^;{)]*\bGError\s*\*\*[^;{)]*)\)\s*([;{])")


def nodiscard(path, text):
    problems = []
    header = path.endswith(".h")
    for m in FUNCTION.finditer(COMMENT.sub(" ", text)):
        ret = " ".join(m.group(1).split())
        name = m.group(2)
        if ret in ("", "return", "else") or ret.startswith("typedef") or ret.endswith(" void") \
                or ret == "void" or ret.startswith("extern"):
            continue
        if not header and not ret.startswith("[[nodiscard]] static") and "static" not in ret:
            continue
        if "[[nodiscard]]" not in ret:
            problems.append(f"{path}: {name} reports failure through GError ** but isn't "
                            "[[nodiscard]]")
    return problems


def top_level_args(text, start):
    """The arguments of the call whose '(' is at `start`, split at top-level commas, and the
    index just past its ')'; None if unbalanced."""
    depth, args, begin = 0, [], start + 1
    for i in range(start, len(text)):
        ch = text[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[begin:i])
                return args, i + 1
        elif ch == "," and depth == 1:
            args.append(text[begin:i])
            begin = i + 1
    return None


LVALUE = r"[A-Za-z_][A-Za-z_0-9]*(?:(?:->|\.)[A-Za-z_][A-Za-z_0-9]*)*"
STRING = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def unsequenced(path, text):
    problems = []
    code = STRING.sub('""', COMMENT.sub(" ", text))
    for m in re.finditer(r"\b[A-Za-z_][A-Za-z_0-9]*\s*\(", code):
        parsed = top_level_args(code, m.end() - 1)
        if parsed is None:
            continue
        args = parsed[0]
        for i, arg in enumerate(args):
            # &x inside a nested call in this argument (an out-parameter being written)
            for out in re.findall(r"\(\s*[^()]*&\s*(" + LVALUE + ")", arg):
                read = re.compile(r"(?<![&\w>.])" + re.escape(out) + r"(?![\w(])")
                for j, other in enumerate(args):
                    # sizeof's operand is not evaluated: not a read
                    other = re.sub(r"\bsizeof\s*\(?\s*" + LVALUE + r"\s*\)?", "sizeof ()", other)
                    if j != i and read.search(other):
                        line = code.count("\n", 0, m.start()) + 1
                        problems.append(f"{path}:{line}: {out} is written through &{out} in one "
                                        "argument and read in another; call first, then read")
    return sorted(set(problems))


RULES = [nodiscard, unsequenced]
# Declarations of GLib's own functions, for the analyzer: not ours to mark.
EXEMPT = {"src/analyzer-glib.h"}


def check():
    problems = []
    files = sorted(glob.glob("src/*.[ch]") + glob.glob("tests/*.[ch]") + glob.glob("tests/*/*.[ch]"))
    if len(files) < 20:
        sys.exit(f"check-source: found only {len(files)} files; run from the repository root")
    for path in files:
        if path in EXEMPT:
            continue
        text = open(path, encoding="utf-8").read()
        for rule in RULES:
            problems += rule(path, text)
    for p in problems:
        print("  " + p)
    if problems:
        sys.exit(f"check-source: {len(problems)} problems")
    print(f"check-source: {len(files)} files, {len(RULES)} rules, no problems")


# (rule, file name, source, expected number of problems)
EXAMPLES = [
    (nodiscard, "x.h", "bool f (const char *p, GError **error);", 1),
    (nodiscard, "x.h", "[[nodiscard]] bool f (const char *p, GError **error);", 0),
    (nodiscard, "x.h", "GbkThing *f (int a,\n           GError **error);", 1),
    (nodiscard, "x.h", "void f (GError **error);", 0),
    (nodiscard, "x.h", "/* bool f (GError **error); */", 0),
    (nodiscard, "x.h", "typedef bool (*Fn) (GError **error);", 0),
    (nodiscard, "x.c", "static bool\nf (GError **error)\n{\n  return true;\n}", 1),
    (nodiscard, "x.c", "[[nodiscard]] static bool\nf (GError **error)\n{\n  return true;\n}", 0),
    (nodiscard, "x.c", "bool\ngbk_f (GError **error)\n{\n  return true;\n}", 0),
    (nodiscard, "x.c", "  if (f (&error))\n    return g (x, &error);", 0),
    (unsequenced, "x.c", "agree (gbk_ops_run (a, &done->error), done->error);", 1),
    (unsequenced, "x.c", "agree (run (&e), e);", 1),
    (unsequenced, "x.c", "show (parse (s, &n), n + 1);", 1),
    (unsequenced, "x.c", "ok = run (&e);\nagree (ok, e);", 0),
    (unsequenced, "x.c", "if (!f (a, &error))\n  show (error->message);", 0),
    (unsequenced, "x.c", "g (f (&e), e2);", 0),
    (unsequenced, "x.c", "g (f (&d->e), d->error);", 0),
    (unsequenced, "x.c", "g (&e, f (e));", 0),
    (unsequenced, "x.c", "g (f (\"&e\"), e);", 0),
    (unsequenced, "x.c", "check (read (fd, &v, sizeof v), sizeof v);", 0),
    (unsequenced, "x.c", "check (read (fd, &v, sizeof (v)), v);", 1),
]


def self_test():
    failed = 0
    for rule, name, source, expected in EXAMPLES:
        got = len(rule(name, source))
        if got != expected:
            failed += 1
            print(f"  {rule.__name__}: expected {expected}, got {got}: {source!r}")
    if failed:
        sys.exit(f"check-source self-test: {failed} of {len(EXAMPLES)} examples wrong")
    print(f"check-source self-test: {len(EXAMPLES)} examples")


if __name__ == "__main__":
    {"self-test": self_test}.get(sys.argv[1] if len(sys.argv) > 1 else "", check)()
