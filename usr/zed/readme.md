# C! (cflang) — Zed extension

Syntax highlighting, bracket matching, indentation, folding, document outline,
test **run buttons**, and **diagnostics** (via the `cf lsp` language server) for
[C!](../../root/specs/ebnf.md) in the [Zed](https://zed.dev) editor.

## What's here

```
extension.toml                   Zed manifest — grammar + language + language server
src/lib.rs                       the wasm shim handing Zed the `cf lsp` command
languages/cflang/config.toml     file suffix (.cf), comments (#), brackets, tabs
languages/cflang/*.scm           tree-sitter queries: highlights, brackets,
                                 indents, folds, outline, runnables
```

The grammar lives in its own repo,
[tree-sitter-cflang](https://github.com/orlowdev/tree-sitter-cflang), vendored
beside this one in the cf tree at `usr/tree-sitter-cflang`.

## Running tests

`languages/cflang/runnables.scm` marks every `case!`/`todo!` call, so Zed shows
a run button in the gutter next to each test case. The button fires whatever
task carries the `cflang-case` tag — define one in your project's
`.zed/tasks.json` (the cf repo ships one that uses `./var/cf`):

```json
[
  {
    "label": "cf test $ZED_RELATIVE_FILE",
    "command": "cf",
    "args": ["test", "$ZED_RELATIVE_FILE"],
    "tags": ["cflang-case"]
  }
]
```

`cf test` runs a whole file's cases — there is no per-case filter.

## Language server

The server is the compiler itself: `cf lsp` speaks LSP over stdio and publishes
located diagnostics on open and save (positions negotiated as `utf-8` byte
columns). The extension resolves `cf` from the worktree's PATH; point it
elsewhere with Zed settings:

```json
{ "lsp": { "cf-lsp": { "binary": { "path": "/path/to/cf" } } } }
```

Building the wasm shim needs the `wasm32-wasip1` Rust target
(`rustup target add wasm32-wasip1`) — Zed compiles it automatically when the
dev extension is (re)installed.

Zed language support is built entirely on **Tree-sitter**: every query file runs
against a parse tree, so the grammar in `tree-sitter-cflang/` is the foundation.
There is no separate TextMate/regex grammar.

## Install (development)

1. Open Zed → command palette → **`zed: install dev extension`**.
2. Pick this directory (`usr/zed`). Open any `.cf` file to see it applied.

Zed loads the language config and queries **live** from this directory, but
fetches the _grammar_ from the tree-sitter-cflang GitHub repo at the commit
pinned by `rev` in `extension.toml`.

## Iterating

- **Queries / `config.toml`** — edit freely, then **`zed: reload extensions`**.
  No commit needed; Zed reads them live.
- **`grammar.js`** — edit in `usr/tree-sitter-cflang`, regenerate, commit, push
  the subtree upstream, and pin the new rev:

  ```sh
  cd ../tree-sitter-cflang
  npx tree-sitter generate           # regenerate src/
  npx tree-sitter parse FILE.cf      # sanity-check (look for ERROR nodes)
  cd ../.. && git commit …           # the subtree split hash is what gets pushed
  git subtree split --prefix=usr/tree-sitter-cflang   # → SHA for `rev`
  git subtree push --prefix=usr/tree-sitter-cflang git@github.com:orlowdev/tree-sitter-cflang.git master
  ```

  Then **`zed: reload extensions`**.

The grammar parses `lib/`, `boot/src`, and the valid `boot/tests` corpus with
zero error nodes (sole corner: `n!= 0`, whose bang the lexer cannot split
without lookahead).

## Deliberate scope decisions

The grammar serves editor tooling, so a few choices favour resilient
highlighting over strict conformance:

- **Newlines are insignificant.** The language is newline-terminated (one
  statement per line — see `root/code_style/indentation.md`), but the compact
  cf0 test files put several statements on a line. The grammar stays permissive
  so it highlights both styles without flagging the existing files.
- **`break` / `continue` carry no label.** The EBNF gives them an optional loop
  label; with insignificant newlines an optional trailing name would swallow the
  next statement's leading identifier. A `break outer` still highlights fine.
- **Index / type-application overlap** (`xs[8]` vs `f[8]`) is resolved by the
  receiver's type in the real language — unknown to a syntax grammar — so the
  grammar biases toward indexing, the common case.
- **Types and UPPER_SNAKE consts share one token.** `STDOUT` and `Str` are
  lexically inseparable without lookahead, so `type_name` covers both and the
  highlight query recolors the all-caps shape as a constant via `#match?`.
