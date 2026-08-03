# C! (cflang) — Zed extension

Syntax highlighting, bracket matching, indentation, folding, and document
outline for [C!](../../root/specs/ebnf.md) in the [Zed](https://zed.dev) editor.

## What's here

```
extension.toml                   Zed manifest — registers the grammar + language
tree-sitter-cflang/grammar.js    the C! grammar, translated from root/specs/ebnf.md
languages/cflang/config.toml     file suffix (.cf), comments (#), brackets, tabs
languages/cflang/*.scm           tree-sitter queries: highlights, brackets,
                                 indents, folds, outline
```

Zed language support is built entirely on **Tree-sitter**: every query file runs
against a parse tree, so the grammar in `tree-sitter-cflang/` is the foundation.
There is no separate TextMate/regex grammar.

## Install (development)

1. Open Zed → command palette → **`zed: install dev extension`**.
2. Pick this directory (`usr/zed`). Open any `.cf` file to see it applied.

Zed loads the language config and queries **live** from this directory, but
fetches the _grammar_ from a git commit — see `extension.toml`:

```toml
[grammars.cflang]
repository = "file:///Users/orlowdev/Code/cflang"  # this repo, local — no push
rev = "…"                                            # a commit containing the grammar
path = "usr/zed/tree-sitter-cflang"                     # its subdirectory
```

`file://` means no GitHub push is needed, but `git clone` only sees committed
state, so the grammar must live at a real commit and `rev` pins it.

## Iterating

- **Queries / `config.toml`** — edit freely, then **`zed: reload extensions`**.
  No commit needed; Zed reads them live.
- **`grammar.js`** — after editing, regenerate, commit, and bump `rev`:

  ```sh
  cd tree-sitter-cflang
  npx tree-sitter-cli generate      # regenerate src/
  npx tree-sitter-cli parse FILE.cf # sanity-check (look for ERROR nodes)
  cd - && git commit -am "…"        # grammar must be committed for Zed to fetch it
  git rev-parse HEAD                 # put this SHA in extension.toml `rev`
  ```

  Then **`zed: reload extensions`**.

The grammar parses the entire `boot/` corpus with zero error nodes.

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

## LSP

Deferred: C! has no language server yet. When one exists, register it under
`[language_servers.cflang]` in `extension.toml`; nothing else here changes.
