"""Scripted LSP exchanges for cf lsp — prepareRename/documentHighlight, type-name refs,
cross-file pub rename."""
import os
import time as time_mod

FIX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fix", "main.cf")
FIX2 = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fix2", "main.cf")
FIX2LIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fix2", "lib.cf")


def pos_of(lines, line, token, occurrence=0):
    """(line, char) of the occurrence-th `token` on 0-based `line`."""
    at = -1
    for _ in range(occurrence + 1):
        at = lines[line].index(token, at + 1)
    return line, at


def run(s, check, tdp, uri_of):
    text = open(FIX).read()
    lines = text.split("\n")
    s.notify("textDocument/didOpen", {"textDocument": {"uri": uri_of(FIX), "text": text}})
    s.drain()

    # --- prepareRename ---
    l, c = pos_of(lines, 10, "total", 1)  # second `total` on `total = total + 1`
    r = s.request("textDocument/prepareRename", tdp(FIX, l, c))["result"]
    check("prepare on local answers", r is not None and r.get("placeholder") == "total", repr(r))
    check("prepare range covers token",
          r is not None and r["range"]["start"] == {"line": l, "character": c}
          and r["range"]["end"]["character"] == c + len("total"), repr(r))

    l, c = pos_of(lines, 15, "helper")  # non-pub top-level use
    r = s.request("textDocument/prepareRename", tdp(FIX, l, c))["result"]
    check("prepare on non-pub top-level answers", r is not None and r.get("placeholder") == "helper", repr(r))

    l, c = pos_of(lines, 17, "main")  # pub — must refuse
    r = s.request("textDocument/prepareRename", tdp(FIX, l, c))["result"]
    check("prepare on pub name refuses", r is None, repr(r))

    r = s.request("textDocument/prepareRename", tdp(FIX, 1, 0))["result"]
    check("prepare on empty line refuses", r is None, repr(r))

    # --- documentHighlight ---
    l, c = pos_of(lines, 10, "total", 1)
    r = s.request("textDocument/documentHighlight", tdp(FIX, l, c))["result"]
    check("highlight on local: 4 rows", r is not None and len(r) == 4, repr(r))
    check("highlight first is Write", r is not None and r[0]["kind"] == 3 and all(x["kind"] == 2 for x in r[1:]), repr(r))
    check("highlight binder line", r is not None and r[0]["range"]["start"]["line"] == 9, repr(r))

    l, c = pos_of(lines, 15, "helper")
    r = s.request("textDocument/documentHighlight", tdp(FIX, l, c))["result"]
    check("highlight on top-level: decl + use", r is not None and len(r) == 2, repr(r))

    l, c = pos_of(lines, 17, "main")
    r = s.request("textDocument/documentHighlight", tdp(FIX, l, c))["result"]
    check("highlight on pub name answers (decl row)", r is not None and len(r) == 1
          and r[0]["range"]["start"]["line"] == 17, repr(r))

    l, c = pos_of(lines, 18, "Point")  # a type: decl + alias target + 3 uses
    r = s.request("textDocument/documentHighlight", tdp(FIX, l, c))["result"]
    check("highlight on a type: 5 rows", r is not None and len(r) == 5, repr(r))

    l, c = pos_of(lines, 19, "Line")  # a union member
    r = s.request("textDocument/documentHighlight", tdp(FIX, l, c))["result"]
    check("highlight on a member: 3 rows", r is not None and len(r) == 3, repr(r))

    # --- regressions: references + rename on a local still answer ---
    l, c = pos_of(lines, 10, "total", 1)
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("references on local still 4", r is not None and len(r) == 4, repr(r))

    r = s.request("textDocument/rename", tdp(FIX, l, c, {"newName": "sum"}))["result"]
    check("rename local still works", r is not None and len(r["changes"][uri_of(FIX)]) == 4, repr(r))

    # --- type-name references ---
    l, c = pos_of(lines, 18, "Point")  # annotation use in main
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    got = sorted((x["range"]["start"]["line"], x["range"]["start"]["character"]) for x in (r or []))
    want = sorted([(2, 5),                     # data Point decl
                   (34, len("type Alias = ")), # alias target (erased pre-parse, line-scanned)
                   (15, lines[15].index("Point")),         # return annotation
                   (15, lines[15].index("Point", 25)),     # constructor
                   (18, lines[18].index("Point"))])        # binding annotation
    check("refs on record type: 5 rows", got == want, f"{got} != {want}")

    l, c = pos_of(lines, 22 + 1, "Shape")  # match arm pattern
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    got = sorted((x["range"]["start"]["line"], x["range"]["start"]["character"]) for x in (r or []))
    want = sorted([(4, 6),
                   (19, lines[19].index("Shape")),
                   (19, lines[19].index("Shape", 10)),
                   (23, lines[23].index("Shape"))])
    check("refs on union type: 4 rows", got == want, f"{got} != {want}")

    l, c = pos_of(lines, 6, "Pub")
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("refs on pub type: decl row via the pub path", r is not None and len(r) == 1
          and r[0]["range"]["start"] == {"line": 6, "character": 9}, repr(r))

    l, c = pos_of(lines, 9, "Iarch")
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("refs on builtin type refuses", r is None, repr(r))

    l, c = pos_of(lines, 29, "Alias")
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("refs on type alias refuses", r is None, repr(r))

    # --- type rename ---
    l, c = pos_of(lines, 18, "Point")
    r = s.request("textDocument/rename", tdp(FIX, l, c, {"newName": "point"}))["result"]
    check("type rename to lowercase refuses", r is None, repr(r))

    r = s.request("textDocument/prepareRename", tdp(FIX, l, c))["result"]
    check("prepare on type answers", r is not None and r.get("placeholder") == "Point", repr(r))

    r = s.request("textDocument/rename", tdp(FIX, l, c, {"newName": "Spot"}))["result"]
    edits = (r or {}).get("changes", {}).get(uri_of(FIX), [])
    check("type rename: 5 edits", len(edits) == 5, repr(r))

    # apply the edits and verify the renamed program still checks clean
    import subprocess, tempfile
    new = [list(x) for x in lines]
    for e in sorted(edits, key=lambda e: (e["range"]["start"]["line"], e["range"]["start"]["character"]), reverse=True):
        ln, s0, e0 = e["range"]["start"]["line"], e["range"]["start"]["character"], e["range"]["end"]["character"]
        new[ln][s0:e0] = list(e["newText"])
    renamed = "\n".join("".join(x) for x in new)
    with tempfile.NamedTemporaryFile("w", suffix=".cf", delete=False) as f:
        f.write(renamed)
    rc = subprocess.run(["/Users/orlowdev/Code/cf/var/cf", "check", f.name],
                        capture_output=True, cwd="/Users/orlowdev/Code/cf")
    check("renamed program checks clean", rc.returncode == 0 and "Point" not in renamed,
          rc.stderr.decode()[:200])
    os.unlink(f.name)

    # --- cross-file pub rename (fix2: main.cf + lib.cf) ---
    import shutil, subprocess as sp
    m2 = open(FIX2).read().split("\n")
    l2 = open(FIX2LIB).read().split("\n")
    s.notify("textDocument/didOpen", {"textDocument": {"uri": uri_of(FIX2), "text": "\n".join(m2)}})
    s.drain()

    def locs_of(r):
        out = []
        for x in (r or []):
            out.append((x["uri"].replace("file://", ""), x["range"]["start"]["line"], x["range"]["start"]["character"]))
        return sorted(out)

    # references on the ns::twice member from the entry
    l, c = pos_of(m2, 14, "twice")
    r = s.request("textDocument/references", tdp(FIX2, l, c, {"context": {"includeDeclaration": True}}))["result"]
    want = sorted([(FIX2LIB, 4, l2[4].index("twice")),
                   (FIX2LIB, 6, l2[6].index("twice", 20)),
                   (FIX2LIB, 8, l2[8].index("twice")),
                   (FIX2, 14, m2[14].index("twice")),
                   (FIX2, 14, m2[14].index("twice", m2[14].index("lib::")))])
    check("pub refs cross files: 5 rows, local shadow skipped", locs_of(r) == want, f"{locs_of(r)} != {want}")

    # references on Pair (a pub TYPE) from its annotation use
    l, c = pos_of(m2, 6, "Pair")
    r = s.request("textDocument/references", tdp(FIX2, l, c, {"context": {"includeDeclaration": True}}))["result"]
    want = sorted([(FIX2LIB, 2, l2[2].index("Pair")),
                   (FIX2LIB, 6, l2[6].index("Pair")),
                   (FIX2LIB, 6, l2[6].index("Pair", l2[6].index("Pair") + 1)),
                   (FIX2, 2, m2[2].index("Pair")),
                   (FIX2, 6, m2[6].index("Pair"))])
    check("pub type refs cross files: import member included", locs_of(r) == want, f"{locs_of(r)} != {want}")

    # prepareRename on the qualified member: range covers just the segment
    l, c = pos_of(m2, 14, "twice")
    r = s.request("textDocument/prepareRename", tdp(FIX2, l, c))["result"]
    check("prepare on pub qualified member", r is not None and r.get("placeholder") == "twice"
          and r["range"]["start"]["character"] == c and r["range"]["end"]["character"] == c + 5, repr(r))

    # rename twice -> dbl from the entry; apply to copies of both files; verify clean
    r = s.request("textDocument/rename", tdp(FIX2, l, c, {"newName": "dbl"}))["result"]
    ch = (r or {}).get("changes", {})
    check("pub rename touches 2 files", len(ch) == 2 and len(ch.get(uri_of(FIX2), [])) == 2
          and len(ch.get(uri_of(FIX2LIB), [])) == 3, repr(ch))

    import tempfile
    tmpd = tempfile.mkdtemp()
    texts = {uri_of(FIX2): [list(x) for x in m2], uri_of(FIX2LIB): [list(x) for x in l2]}
    for u, edits in ch.items():
        for e in sorted(edits, key=lambda e: (e["range"]["start"]["line"], e["range"]["start"]["character"]), reverse=True):
            ln, s0, e0 = e["range"]["start"]["line"], e["range"]["start"]["character"], e["range"]["end"]["character"]
            texts[u][ln][s0:e0] = list(e["newText"])
    open(os.path.join(tmpd, "main.cf"), "w").write("\n".join("".join(x) for x in texts[uri_of(FIX2)]))
    open(os.path.join(tmpd, "lib.cf"), "w").write("\n".join("".join(x) for x in texts[uri_of(FIX2LIB)]))
    rc = sp.run(["/Users/orlowdev/Code/cf/var/cf", "check", os.path.join(tmpd, "main.cf")],
                capture_output=True, cwd="/Users/orlowdev/Code/cf")
    renamed_main = "\n".join("".join(x) for x in texts[uri_of(FIX2)])
    check("cross-file renamed program checks clean",
          rc.returncode == 0 and "ns::dbl" in renamed_main and "lib::dbl" in renamed_main
          and "dbl = 9" not in renamed_main,  # the local `twice` stayed
          rc.stderr.decode()[:300])
    shutil.rmtree(tmpd)

    # negative: rename refuses `main` and a non-pub name from another file stays refused
    l, c = pos_of(m2, 11, "main")
    r = s.request("textDocument/rename", tdp(FIX2, l, c, {"newName": "entry"}))["result"]
    check("rename of main refuses", r is None, repr(r))

    # --- union-member references + rename (same-file, non-pub: fix Shape) ---
    l, c = pos_of(lines, 19, "Line")  # ctor member `Shape.Line(p.x)`
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    got = sorted((x["range"]["start"]["line"], x["range"]["start"]["character"]) for x in (r or []))
    want = sorted([(4, lines[4].index("Line")), (19, c), (23, lines[23].index("Line"))])
    check("member refs: decl + ctor + pattern", got == want, f"{got} != {want}")

    l, c = pos_of(lines, 4, "Line")  # cursor ON the member declaration
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    got = sorted((x["range"]["start"]["line"], x["range"]["start"]["character"]) for x in (r or []))
    check("member refs from the decl cursor", got == want, f"{got} != {want}")

    l, c = pos_of(lines, 4, "Dot")
    r = s.request("textDocument/references", tdp(FIX, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("unused member: decl row only", r is not None and len(r) == 1, repr(r))

    l, c = pos_of(lines, 19, "Line")
    r = s.request("textDocument/prepareRename", tdp(FIX, l, c))["result"]
    check("prepare on member", r is not None and r.get("placeholder") == "Line"
          and r["range"]["start"]["character"] == c, repr(r))

    r = s.request("textDocument/rename", tdp(FIX, l, c, {"newName": "Seg"}))["result"]
    edits = (r or {}).get("changes", {}).get(uri_of(FIX), [])
    check("member rename: 3 edits", len(edits) == 3, repr(r))
    new = [list(x) for x in lines]
    for e in sorted(edits, key=lambda e: (e["range"]["start"]["line"], e["range"]["start"]["character"]), reverse=True):
        ln, s0, e0 = e["range"]["start"]["line"], e["range"]["start"]["character"], e["range"]["end"]["character"]
        new[ln][s0:e0] = list(e["newText"])
    renamed = "\n".join("".join(x) for x in new)
    with tempfile.NamedTemporaryFile("w", suffix=".cf", delete=False) as f:
        f.write(renamed)
    rc = subprocess.run(["/Users/orlowdev/Code/cf/var/cf", "check", f.name],
                        capture_output=True, cwd="/Users/orlowdev/Code/cf")
    check("member-renamed program checks clean", rc.returncode == 0 and "Line" not in renamed,
          rc.stderr.decode()[:200])
    os.unlink(f.name)

    # --- union-member cross-file (fix2: pub union Op in lib.cf, used in main.cf) ---
    m2b = open(FIX2).read().split("\n")
    l2b = open(FIX2LIB).read().split("\n")
    l, c = pos_of(m2b, 14, "Add")  # `op_v(Op.Add)` in main's return
    r = s.request("textDocument/references", tdp(FIX2, l, c, {"context": {"includeDeclaration": True}}))["result"]
    lu = next(i for i, x in enumerate(l2b) if "union Op" in x)
    want = sorted([(FIX2LIB, lu, l2b[lu].index("Add")),
                   (FIX2, 14, c),
                   (FIX2, 18, m2b[18].index("Add"))])
    check("pub member refs cross files", locs_of(r) == want, f"{locs_of(r)} != {want}")

    r = s.request("textDocument/rename", tdp(FIX2, l, c, {"newName": "Plus"}))["result"]
    ch2 = (r or {}).get("changes", {})
    check("pub member rename: 2 files, 3 edits",
          len(ch2) == 2 and len(ch2.get(uri_of(FIX2), [])) == 2 and len(ch2.get(uri_of(FIX2LIB), [])) == 1, repr(ch2))
    texts2 = {uri_of(FIX2): [list(x) for x in m2b], uri_of(FIX2LIB): [list(x) for x in l2b]}
    for u, edits in ch2.items():
        for e in sorted(edits, key=lambda e: (e["range"]["start"]["line"], e["range"]["start"]["character"]), reverse=True):
            ln, s0, e0 = e["range"]["start"]["line"], e["range"]["start"]["character"], e["range"]["end"]["character"]
            texts2[u][ln][s0:e0] = list(e["newText"])
    tmpd2 = tempfile.mkdtemp()
    open(os.path.join(tmpd2, "main.cf"), "w").write("\n".join("".join(x) for x in texts2[uri_of(FIX2)]))
    open(os.path.join(tmpd2, "lib.cf"), "w").write("\n".join("".join(x) for x in texts2[uri_of(FIX2LIB)]))
    import lspdrv as _drv
    rc = sp.run([_drv.CF, "check", os.path.join(tmpd2, "main.cf")],
                capture_output=True, cwd=_drv.CWD)
    check("cross-file member rename checks clean", rc.returncode == 0, rc.stderr.decode()[:300])
    shutil.rmtree(tmpd2)

    # references from the DECL side (cursor on `pub const twice` in lib.cf) with the server
    # rooted at a workspace that does NOT contain the fixture: the workspace walk finds no
    # importer, so the answer is scoped to lib.cf's OWN module tree — only the decl + lib's
    # internal uses answer
    import lspdrv as _drv2
    s0 = _drv2.Server(pwd=tempfile.mkdtemp())
    s0.request("initialize", {"capabilities": {}})
    s0.notify("textDocument/didOpen", {"textDocument": {"uri": uri_of(FIX2LIB), "text": "\n".join(l2)}})
    time_mod.sleep(1); s0.drain()
    l, c = pos_of(l2, 4, "twice")
    r = s0.request("textDocument/references", tdp(FIX2LIB, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("pub refs from the decl file outside the workspace: tree-scoped", r is not None and len(r) == 3, repr(locs_of(r)))

    # --- WORKSPACE-wide rename: a server rooted AT the project sees importers of the decl
    # file that the decl file's own tree cannot reach ---
    import lspdrv
    fix2dir = os.path.dirname(FIX2)
    s2 = lspdrv.Server(pwd=fix2dir)
    s2.request("initialize", {"capabilities": {}})
    s2.notify("textDocument/didOpen", {"textDocument": {"uri": uri_of(FIX2LIB), "text": "\n".join(l2b)}})
    time_mod.sleep(1); s2.drain()

    l, c = pos_of(l2b, 4, "twice")
    r = s2.request("textDocument/references", tdp(FIX2LIB, l, c, {"context": {"includeDeclaration": True}}))["result"]
    want = sorted([(FIX2LIB, 4, l2b[4].index("twice")),
                   (FIX2LIB, 6, l2b[6].index("twice", 20)),
                   (FIX2LIB, 8, l2b[8].index("twice")),
                   (FIX2, 14, m2b[14].index("twice")),
                   (FIX2, 14, m2b[14].index("twice", m2b[14].index("lib::")))])
    check("workspace refs from the decl file find the importer", locs_of(r) == want, f"{locs_of(r)} != {want}")

    r = s2.request("textDocument/rename", tdp(FIX2LIB, l, c, {"newName": "dbl"}))["result"]
    chw = (r or {}).get("changes", {})
    check("workspace rename from the decl file: 2 files",
          len(chw) == 2 and len(chw.get(uri_of(FIX2), [])) == 2 and len(chw.get(uri_of(FIX2LIB), [])) == 3, repr(chw))

    # member from the decl side, workspace-wide
    lu2 = next(i for i, x in enumerate(l2b) if "union Op" in x)
    l, c = lu2, l2b[lu2].index("Add")
    r = s2.request("textDocument/references", tdp(FIX2LIB, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("workspace member refs from the union decl", r is not None and len(r) == 3, repr(locs_of(r)))

    # a same-named decl in a REACHING workspace file refuses (per-file shadow)
    shadow = os.path.join(fix2dir, "shadow.cf")
    open(shadow, "w").write("import lib::{ Pair }\n\nconst twice = (Iarch x): Iarch -> x\n\nconst use_it = (Iarch y): Iarch -> twice(y)\n")
    l, c = pos_of(l2b, 4, "twice")
    r = s2.request("textDocument/references", tdp(FIX2LIB, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("workspace shadow decl refuses", r is None, repr(locs_of(r)))
    os.unlink(shadow)

    # a BROKEN mentioning file is SKIPPED — what cannot resolve cannot consume the target
    # (deliberately-broken test fixtures must not block every rename)
    broken = os.path.join(fix2dir, "broken.cf")
    open(broken, "w").write("const oops = (Iarch x): Iarch -> twice((\n")
    r = s2.request("textDocument/references", tdp(FIX2LIB, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("broken mentioning file is skipped", locs_of(r) == want, f"{locs_of(r)} != {want}")
    os.unlink(broken)

    # gone again: the walk recovers once the clutter is removed
    r = s2.request("textDocument/references", tdp(FIX2LIB, l, c, {"context": {"includeDeclaration": True}}))["result"]
    check("workspace refs recover after cleanup", locs_of(r) == want, f"{locs_of(r)} != {want}")

    # highlight of an IMPORTED name: this document's occurrences only (no decl here)
    s2.notify("textDocument/didOpen", {"textDocument": {"uri": uri_of(FIX2), "text": "\n".join(m2b)}})
    s2.drain()
    l, c = pos_of(m2b, 14, "twice")  # the ns::twice member
    r = s2.request("textDocument/documentHighlight", tdp(FIX2, l, c))["result"]
    check("highlight on an imported name: this file's 2 uses", r is not None and len(r) == 2, repr(r))

    # workspace/symbol over the fix2 workspace
    r = s2.request("workspace/symbol", {"query": "twi"})["result"]
    check("workspace/symbol narrow query", r is not None and len(r) == 1
          and r[0]["name"] == "twice" and r[0]["location"]["uri"] == uri_of(FIX2LIB), repr(r))
    r = s2.request("workspace/symbol", {"query": ""})["result"]
    names = sorted(x["name"] for x in (r or []))
    check("workspace/symbol lists all decls",
          names == sorted(["use_pair", "main", "op_v", "Pair", "twice", "combine", "untouched", "Op"]), repr(names))
    s2.stop()

    # --- textDocument/formatting ---
    fmt_params = {"textDocument": {"uri": uri_of(FIX)}, "options": {"tabSize": 4, "insertSpaces": False}}
    r = s.request("textDocument/formatting", fmt_params)["result"]
    check("formatting a clean file: no edits", r == [], repr(r))

    # a dirty overlay (trailing blank lines) formats back to the canonical bytes in ONE edit
    perturbed = text + "\n\n\n"
    s.notify("textDocument/didChange", {"textDocument": {"uri": uri_of(FIX)},
                                        "contentChanges": [{"text": perturbed}]})
    s.drain()
    r = s.request("textDocument/formatting", fmt_params)["result"]
    check("formatting the dirty overlay: one whole-document edit", r is not None and len(r) == 1, repr(r))
    check("formatting newText restores the canonical bytes",
          r is not None and len(r) == 1 and r[0]["newText"] == text,
          repr(r[0]["newText"][-40:] if r else r))
    check("formatting range spans the whole overlay",
          r is not None and len(r) == 1 and r[0]["range"]["start"] == {"line": 0, "character": 0}
          and r[0]["range"]["end"] == {"line": perturbed.count("\n"), "character": 0},
          repr(r[0]["range"] if r else r))

    # a save drops the overlay — the clean disk file answers again
    s.notify("textDocument/didSave", {"textDocument": {"uri": uri_of(FIX)}})
    s.drain()
    r = s.request("textDocument/formatting", fmt_params)["result"]
    check("formatting after save falls back to the clean disk file", r == [], repr(r))

    # --- completion: barrel-following (the import graph as API structure) ---
    def complete(buf, line, char):
        s.notify("textDocument/didChange", {"textDocument": {"uri": uri_of(FIX)},
                                            "contentChanges": [{"text": buf}]})
        s.drain()
        r = s.request("textDocument/completion", tdp(FIX, line, char))["result"]
        return [(x["label"], x["kind"]) for x in (r or [])]

    # the std root: the 8 top-level barrels, all Modules, nothing buried
    got = complete("const x = std::\n" + text, 0, 15)
    check("completion std:: lists the top barrels",
          sorted(got) == sorted([("comptime", 9), ("compiler", 9), ("data", 9), ("io", 9),
                                 ("math", 9), ("process", 9), ("sys", 9), ("test", 9)]), repr(got))

    # an import path continues with MODULES only — io.cf's namespace reexports, no members
    got = complete("import std::io::\n" + text, 0, 16)
    check("completion import std::io:: follows the barrel",
          sorted(got) == sorted([("fd", 9), ("console", 9), ("file", 9), ("fs", 9), ("term", 9)]),
          repr(got))
    check("completion import std::io:: buries no member",
          all(k == 9 for _, k in got), repr(got))

    # a use through the file's import alias walks the same surface
    got = complete("import std::io\nconst x = io::\n" + text, 1, 14)
    check("completion io:: through the alias",
          sorted(n for n, _ in got) == sorted(["fd", "console", "file", "fs", "term"]), repr(got))

    # an `as`-renamed alias completes like its target
    got = complete("import std::comptime::os as myos\nconst x = myos::\n" + text, 1, 16)
    check("completion through an as-renamed alias",
          sorted(n for n, _ in got) == sorted(["target", "current"]), repr(got))

    # inside `::{` — the destructured module's VALUE surface with true kinds
    got = complete("import std::data::{ \n" + text, 0, 20)
    check("completion inside the destructure brace",
          sorted(n for n, _ in got) == sorted(["Either", "Maybe", "Json", "JsonEntry", "JsonRrr"]),
          repr(got))

    # a member surface two hops deep keeps declaration kinds (22 Struct, 3 Function, 21 Constant)
    got = complete("import std::io\nconst x = io::fd::\n" + text, 1, 18)
    check("completion io::fd:: surfaces the module's pubs",
          ("STDIN", 21) in got and ("Fd", 22) in got and ("entry_is_dir", 3) in got, repr(got))

    # the first segment of an import: the roots only
    got = complete("import \n" + text, 0, 7)
    check("completion import first segment offers std", got == [("std", 9)], repr(got))

    # a dot after a namespace path reads a field off no value — answer nothing, not the dump
    got = complete("const x = std::io.\n" + text, 0, 18)
    check("completion after std::io. is empty", got == [], repr(got))

    got = complete("import std::io\nconst x = io.\n" + text, 1, 13)
    check("completion after an alias dot is empty", got == [], repr(got))

    # a record variable's dot keeps the old bare answer (fields are not scanned, but the
    # namespace guard must not swallow it)
    got = complete("const x = std::io.\nconst y = notamodule.\n" + text, 1, 21)
    check("completion after a value dot still answers", len(got) > 0, repr(got))

    # an import-path member completes as its DESTRUCTURE: label plain, insertText braced
    def complete_raw(buf, line, char):
        s.notify("textDocument/didChange", {"textDocument": {"uri": uri_of(FIX)},
                                            "contentChanges": [{"text": buf}]})
        s.drain()
        return s.request("textDocument/completion", tdp(FIX, line, char))["result"] or []

    r = complete_raw("import std::data::\n" + text, 0, 18)
    ins = {x["label"]: x.get("insertText") for x in r}
    check("import member inserts the destructure", ins.get("Either") == "{ Either }", repr(ins))

    # a wildcard-dispatch barrel's surface reaches the import path too
    r = complete_raw("import std::io::console::\n" + text, 0, 25)
    labs = [x["label"] for x in r]
    check("import console:: offers the wildcard surface",
          "println" in labs and "print" in labs, repr(labs))
    check("import console:: members all insert braces",
          all(x.get("insertText") == "{ %s }" % x["label"] for x in r), repr(r[:3]))

    # a use-site member keeps the plain insert (no braces outside imports)
    r = complete_raw("import std::io\nconst x = io::fd::\n" + text, 1, 18)
    check("use-site members insert plainly",
          all("insertText" not in x for x in r), repr(r[:3]))

    # completion details styled like hovers: bare fn signatures, decl heads without `pub`/`=`
    r = complete_raw("import std::io\nconst x = io::fd::\n" + text, 1, 18)
    det = {x["label"]: x["detail"] for x in r}
    check("detail: bare function signature", det.get("entry_is_dir") == "(DirEntry e): Bool", repr(det))
    check("detail: data head without =", det.get("Fd") == "data Fd", repr(det))
    check("detail: union head without =", det.get("IoRrr") == "union IoRrr", repr(det))
    check("detail: value const head", det.get("STDIN") == "const STDIN", repr(det))

    # goto on EVERY import path segment jumps to that module's file (mid-path resolves
    # through the barrel machinery even when the tree never loaded the module)
    s.notify("textDocument/didChange", {"textDocument": {"uri": uri_of(FIX)},
                                        "contentChanges": [{"text": "import std::io::fd\n" + text}]})
    s.drain()

    def goto_file(line, char):
        r = s.request("textDocument/definition", tdp(FIX, line, char))["result"]
        if isinstance(r, list) and r:
            return r[0]["uri"].split("/cf/")[-1]
        return None

    check("goto std jumps to the root barrel", goto_file(0, 7) == "lib/std.cf", repr(goto_file(0, 7)))
    check("goto io mid-path jumps to the io barrel", goto_file(0, 12) == "lib/std/io.cf", repr(goto_file(0, 12)))
    check("goto fd still jumps to the module", goto_file(0, 16) == "lib/std/io/fd.cf", repr(goto_file(0, 16)))
