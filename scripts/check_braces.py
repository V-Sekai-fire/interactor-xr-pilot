# SPDX-License-Identifier: Apache-2.0 OR MIT
"""Refuses C, C++ and Objective-C bodies without curly braces, reading the code with Clang's parser.

libclang parses each file into its syntax tree. The body of every if, for, range for, while and do
must be a compound statement ({ ... }), and an else must be one too or another if (else if). Code from
macro calls (Clang's record of each expansion) and included headers is skipped. Each file is parsed once per platform (Windows,
Linux, macOS), so the #ifdef branches of all three are read. Missing headers are tolerated: Clang
recovers and still builds the statements of each function. Exits 1 and names each body when any
brace is missing.

    check_braces.py <file>...
    check_braces.py --self-test
"""
import os
import sys
import tempfile

from clang import cindex

K = cindex.CursorKind
LOOPS = {K.FOR_STMT: "for", K.CXX_FOR_RANGE_STMT: "for", K.WHILE_STMT: "while", K.DO_STMT: "do"}
PLATFORMS = [["-D_WIN32", "-D_MSC_VER=1940"], ["-D__linux__"], ["-D__APPLE__", "-D__MACH__"]]
OBJC = {".m": "objective-c", ".mm": "objective-c++"}


def language(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in OBJC:
        return OBJC[ext]
    return "c" if ext == ".c" else "c++"


def args_for(path, platform):
    lang = language(path)
    std = ["-std=c17"] if lang in ("c", "objective-c") else ["-std=c++20"]
    here = os.path.dirname(os.path.abspath(path))
    return ["-x", lang, *std, *platform, "-fsyntax-only", "-ferror-limit=0", f"-I{here}"]


def braced(cursor):
    return cursor is not None and cursor.kind == K.COMPOUND_STMT


def body_of(cursor):
    """The statement a loop runs: the first child of a do, the last child of the others (the init,
    condition, increment and range parts come first). A body like g(); is an expression node."""
    children = list(cursor.get_children())
    if not children:
        return None
    return children[0] if cursor.kind == K.DO_STMT else children[-1]


def if_parts(cursor):
    """(then, else) of an if. Its children end with the then branch, and with the else branch when the
    token else stands between the last two children."""
    children = list(cursor.get_children())
    if len(children) < 2:
        return None, None
    if len(children) >= 3:
        between = cindex.SourceRange.from_locations(children[-2].extent.end, children[-1].extent.start)
        if any(t.spelling == "else" for t in cursor.translation_unit.get_tokens(extent=between)):
            return children[-2], children[-1]
    return children[-1], None


def in_this_file(cursor, path):
    loc = cursor.location
    return loc.file is not None and os.path.normcase(os.path.abspath(loc.file.name)) == path


def missing(path, source=None):
    """The bodies of path that lack braces, as (line, statement kind) pairs, over every platform."""
    index = cindex.Index.create()
    target = os.path.normcase(os.path.abspath(path))
    found = set()
    for platform in PLATFORMS:
        unsaved = [(path, source)] if source is not None else None
        tu = index.parse(path, args=args_for(path, platform), unsaved_files=unsaved,
                         options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        cursors = list(tu.cursor.walk_preorder())
        # The spans of the file that macro calls cover: a statement starting in one is the macro's.
        expansions = [(c.extent.start.offset, c.extent.end.offset) for c in cursors
                      if c.kind == K.MACRO_INSTANTIATION and in_this_file(c, target)]
        for cursor in cursors:
            if cursor.kind not in LOOPS and cursor.kind != K.IF_STMT:
                continue
            if not in_this_file(cursor, target):
                continue
            at = cursor.extent.start.offset
            if any(a <= at < b for a, b in expansions):
                continue
            line = cursor.location.line
            if cursor.kind == K.IF_STMT:
                then, other = if_parts(cursor)
                if then is not None and not braced(then):
                    found.add((line, "if"))
                if other is not None and not braced(other) and other.kind != K.IF_STMT:
                    found.add((other.extent.start.line, "else"))
            else:
                body = body_of(cursor)
                if body is not None and not braced(body):
                    found.add((line, LOOPS[cursor.kind]))
    return sorted(found)


def check(paths):
    bad = 0
    for path in paths:
        for line, kind in missing(path):
            print(f"{path}:{line}: the body of this {kind} has no curly braces")
            bad += 1
    if bad:
        print(f"\n{bad} bodies need curly braces.")
    return 1 if bad else 0


def self_test():
    cases = [
        ("braced if, else, for, range for, while and do",
         "void g(); void h(int);\nvoid f(int a, int (&v)[3]) {\n if (a) { g(); } else { g(); }\n for (;;) { g(); }\n"
         " for (int x : v) { h(x); }\n while (a) { --a; }\n do { --a; } while (a);\n}\n", []),
        ("else if chains with every body braced",
         "void g();\nvoid f(int a) {\n if (a) { g(); } else if (a > 1) { g(); } else { g(); }\n}\n", []),
        ("a switch and a lambda are left alone",
         "void g();\nvoid f(int a) {\n switch (a) { case 1: g(); break; default: break; }\n auto l = [] { g(); };\n}\n", []),
        ("control: an if without braces",
         "void g();\nvoid f(int a) {\n if (a)\n  g();\n}\n", [(3, "if")]),
        ("control: an else without braces",
         "void g();\nvoid f(int a) {\n if (a) { g(); }\n else\n  g();\n}\n", [(5, "else")]),
        ("control: a one-line for, a range for, a while and a do",
         "void g(int);\nvoid f(int a, int (&v)[3]) {\n for (int i = 0; i < 3; ++i) g(i);\n for (int x : v) g(x);\n"
         " while (a) --a;\n do --a; while (a);\n}\n", [(3, "for"), (4, "for"), (5, "while"), (6, "do")]),
        ("control: an unbraced if in code using undeclared types and a missing header",
         "#include \"nowhere.h\"\nUnknown f(Thing& t) {\n if (t.ok())\n  return t.value();\n return {};\n}\n", [(3, "if")]),
        ("control: an unbraced if inside #ifdef _WIN32",
         "void g();\nvoid f(int a) {\n#ifdef _WIN32\n if (a) g();\n#endif\n}\n", [(4, "if")]),
        ("a body written by a macro is the macro's business",
         "#define EACH(n) for (int i = 0; i < n; ++i) g(i);\nvoid g(int);\nvoid f() {\n EACH(3)\n}\n", []),
    ]
    held = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, source, want in cases:
            path = os.path.join(tmp, "case.cpp")
            with open(path, "w", encoding="utf-8") as f:
                f.write(source)
            got = missing(path)
            ok = got == want
            held += ok
            print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f": want {want}, got {got}"))
    print(f"  {held} of {len(cases)} held.")
    return 0 if held == len(cases) else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    sys.exit(check(sys.argv[1:]))
