#!/usr/bin/env python3
"""msg-filter: replace the subject line of mapped commits, keep everything else."""
import os
import sys

# short-hash (original) -> new subject
MAP = {
    # this session's colon essays
    "1719f8f9": "Enhance cf lsp with prepareRename and documentHighlight",
    "7193ed4e": "Enhance cf lsp references and rename with same-file type names",
    "661826d2": "Enhance cf lsp references and rename with cross-file pub names",
    "edb1b5ba": "Enhance cf lsp references and rename with union members",
    "f8a217a2": "Nurture cf lsp by chunking the bind-event log and batching the module tree's uses walk",
    "e4027113": "Enhance cf lsp rename with workspace-wide reach",
    "c2af76d9": "Enhance cf lsp with workspace symbols and full document-highlight coverage",
    "c792e81f": "Enhance cf format by wrapping over-wide comment lines",
    # earlier colon tails
    "a04c7ee2": "Enhance cf lsp mid-edit recovery with declaration-chunk isolation",
    "a4d175e1": "Enhance cf lsp references and rename with same-file top-level names",
    "22ad925d": "Enhance cf lsp with mid-edit salvage of the dying parse's bind-event log",
    "883745d3": "Enhance cf lsp completion with the locals in scope",
    "23446386": "Enhance cf lsp with a refs sub-verb over the bind log's use events",
    "2be8963c": "Enhance cf lsp with a def sub-verb replaying the parser's bind-event log",
    "fbee166a": "Enhance cf lsp with record-field goto and hover",
    "a868c783": "Enhance cf lsp goto-definition and hover with local bindings",
    "7b22438c": "Enhance diagnostics with expression spans",
    "c40cedc9": "Nurture cf lsp hover by cutting a function's body at its arrow",
    "dc7e47ec": "Enhance cf lsp with goto-definition across the module tree",
    "238c5ddf": "Nurture the host arch default by reading the per-arch floor's host_arch_tag",
    "9627dad6": "Enhance resolve diagnostics by stamping import statements",
    "ac1e5d42": "Nurture cf with a stdio language server and check-in-a-child diagnostics on save",
    "0518eb10": "Nurture cf check by treating the input as a module tree",
    # missing/foreign connectives
    "18004fff": "Nurture json floats by adding as_float on the exact fast path",
    "5627e40f": "Nurture the json names by adopting the std conventions JsonRrr and JsonEntry",
    "058b061e": "Nurture the match merge slot by scanning scalars past arms hidden behind payload bindings",
    "81ca6535": "Nurture match arms by rejecting a nonexistent member before it indexes the table",
    "a41002fb": "Nurture the Json union members by dropping the redundant J prefixes",
    "68ebba6c": "Enhance diagnostics by naming the offender and collapsing the duplicate type-variable report",
    "b4b05f5d": "Enhance monomorph by collecting failed instantiations as located values",
    "64929068": "Enhance the alloc and root gates by collecting every mis-colored declaration in one run",
    "086fb696": "Enhance diagnostics coverage with pruned declarations, monomorph stamps and std fd descriptors",
    "5a73f490": "Enhance typecheck by collecting located diagnostics as values",
    "f2f0cdae": "Enhance binding annotations by enforcing the written type with exact float widths",
    "4fbbb0f9": "Nurture the root-gate message and comments by saying geometry instead of arena",
}

commit = os.environ.get("GIT_COMMIT", "")
msg = sys.stdin.read()
new = None
for short, subject in MAP.items():
    if commit.startswith(short):
        new = subject
        break
if new is None:
    sys.stdout.write(msg)
else:
    lines = msg.split("\n")
    # the original subject may spill nothing — subjects are line 1; keep body/trailers
    rest = "\n".join(lines[1:])
    sys.stdout.write(new + "\n" + rest)
