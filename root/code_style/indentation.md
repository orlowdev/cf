# Indentation conventions

This document outlines how C! source code is indented and laid out on the line.

## Rules

- Indentation uses **hard tabs**, never spaces. One level of nesting is one tab.
- A tab is **2 columns** wide. Configure your editor to render tabs at width 2
  (see `usr/` for editor extensions that set this for you).
- **One statement per line.** Inside a multiline block, every statement sits on
  its own line — statements are newline-terminated and are never juxtaposed on a
  shared line. See the grammar conventions in `root/specs/ebnf.md`.
