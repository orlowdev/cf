# C! Module System

This document defines the semantics of C!'s modules: how files reach one another by `::`
path, how `import` abbreviates those paths, what a module exports, how barrels (`pub import`
reexports) chain, how build-time conditional compilation selects a backing per target, and
the surface of the compiler-supplied **`std::comptime`** module (`os`/`arch`, `Os`/`Arch`,
`target`/`current`).

It is the semantics behind the module *grammar* of [[ebnf.md]] (§ Imports, § Visibility,
§ Modules), which shapes only the surface and repeatedly defers the semantic rules —
"which names a module exports", "how `::` paths resolve to files", and, explicitly, "the
`std::comptime` module's full surface ... its own deferred spec" — to here. It sits against the pipeline of [[order_of_compilation.md]],
where it is **phase 2, Resolve & flatten** (`cf-stage: resolved`): the resolver runs the
comptime conditional imports, resolves paths, jumps barrels, flattens every module into
one file, mangles module-qualified names, and prunes what no longer applies — turning a
tree of `.cf` files into a single import-free `.cf` for the typecheck gate ([[type_system.md]])
to run on.

Status: **ratified and implemented, `::` revision**. The module surface is `::` paths with
imports as optional sugar (§4), fully implemented in the self-hosted `cf` (the legacy
`.`/string-import surface is migrated and removed); § Reconciliation records the arc. The target surface (§7)
is coordinated with [[cf_cli.md]] §5 (the `--target` os-arch pair): `wasm` sets the `Arch`
only, since Wasm is not an OS.

## 1. What the module system owns

Per [[order_of_compilation.md]] §Resolve the module system owns, and this spec defines:

- **module identity and path resolution** (§2) — a module is a `.cf` file; a `::` path
  names another module; the toolchain maps path → file.
- **visibility** (§3) — `pub` exports a declaration; everything else is file-private.
- **imports, namespaces, and paths** (§4) — full `::` paths reach any member; `import`
  binds an optional abbreviation (a last-segment or `as`-renamed namespace, destructured
  names, or an `as *` wildcard that splices the whole surface flat).
- **barrels / reexport** (§5) — `pub import` reexports what it brings in, and barrels
  chain transitively.
- **comptime conditional compilation** (§6) — the module-level `if ... then ... else`
  that selects exactly one backing per target and dissolves the rest.
- **the `std::comptime` module surface** (§7) — the compiler-supplied `os`/`arch`
  namespaces, the `Os`/`Arch` enums, and the `target`/`current` members that drive §6.
- **the resolve-and-flatten mechanics** (§8) — evaluation order, cycles, mangling, pruning.

It does **not** own the type discipline of imported names (that is the typecheck gate,
[[type_system.md]]), the runtime meaning of `!`/geometry (the memory arc,
[[memory_model.md]]), or the grammar (the surface is [[ebnf.md]]). The module system is a
**resolve-and-rewrite** phase, not a checking gate: it produces a single flattened `.cf`
and rejects only the structural errors of §9.

## 2. Modules and paths

A **module is a `.cf` file.** Its top level is a sequence of imports, declarations, and
comptime conditionals in any order (the [[ebnf.md]] `module` rule); there is no module
keyword, wrapper, or one-class-per-file rule. A file's *identity* is its resolved path;
its *exported surface* is its `pub` declarations plus its `pub import` reexports (§5).

Another module is named by a **`::`-separated path with no `.cf` extension** — `std::mem`,
`lexer`, `sys::darwin::arm64`. There is no path string: the path is a sequence of bare
identifier segments joined by `::`, and `::` is the module/namespace separator throughout
the language (distinct from `.`, which is runtime field access and union-variant
qualification). The toolchain maps a path's segments to a file, `::` playing the role a
directory separator plays in the filesystem ([[cf_cli.md]] §2, "the whole module-resolution
story"):

- A **bare or relative path** (`lexer`, `sys::darwin::arm64`) resolves **relative to the
  importing file's directory** — `sys::darwin::arm64` → `<dir>/sys/darwin/arm64.cf`.
- A **`std::`-prefixed path** (`std::mem`) is the standard library, resolved under the
  toolchain's fixed library root regardless of the importer's location — `std::mem` →
  `<lib>/std/mem.cf`. `std` is the reserved root segment; there is **no ambient search
  path** ([[cf_cli.md]] §2).

The resolved path is canonicalized (`..`/`.`/symlinks collapsed), so the same file reached
by two different paths — a diamond — loads once. Path resolution is lexical and
target-independent: it does not depend on which comptime branch is live (a losing branch's
path is never resolved at all, §6/§8). The `.cf` extension is always implicit and never
written in a path.

The **entry module may not be imported** by another module. The entry is the compilation
root (it carries `main`); a module that imports it — directly or transitively — is an
error. Every other module is imported freely, including in cycles (§8).

The **entry module** is the root `.cf` handed to the compiler; its `pub const main` is the
program entry ([[ebnf.md]] § Entry Point). Flattening and mangling are performed
**path-relative to the entry module** (§8), so the entry fixes the naming frame.

## 3. Visibility

A top-level declaration prefixed with **`pub`** is **exported** — importable by other
modules. Without `pub` a declaration is **file-private** and invisible outside its module.
`pub` is the bare keyword with no scoping argument, and it applies to every top-level
`declaration`: `data`, `type`, `union`, `intrinsic`, and value bindings (`const`/`let`,
hence functions, which are `const`-bound lambdas).

```
pub const lex = (...) -> ...       (* exported *)
const scan_ident = (...) -> ...    (* file-private *)
pub data Token = { ... }         (* exported *)
pub union TokKind = { ... }      (* exported *)
```

`pub` lives only on a top-level `declaration`. A `var_decl` used as a block statement is a
local and takes no `pub`. An `import` may also carry `pub`, but that is a **reexport**
(§5), not a declaration.

**A module's exported surface** is exactly: its `pub` declarations, plus the names its
`pub import`s reexport. Everything else — private declarations, non-`pub` imports — is
invisible to importers. An import can only bind a name the target module actually exports;
importing a private name is an error (§9).

## 4. Imports, namespaces, and paths

**Every module member is reachable by its full `::` path — imports are optional.** A path
`std::comptime::os::target` names the member `target` in module `std::comptime::os` and
resolves the same **whether or not** it was imported: the leading segments name a module (§2),
the trailing segment(s) traverse its exported surface to a member. An import merely binds a
short **abbreviation** for a path prefix so the prefix can be omitted; it introduces no new
capability and no runtime entity (a namespace is resolved at compile time — there is **no
runtime namespace object**).

```
const t = std::comptime::os::target        (* no import needed — the full path resolves *)
```

An import (`[ "pub" ] "import" <module_path> ( [ "::" "{" ... "}" ] | [ "as" ( <name> | "*" ) ] )`,
[[ebnf.md]] § Imports) has four forms:

- **A bare path** `import a::b::c` binds the path's **last segment** `c` as a namespace over
  module `a::b::c`'s **whole** exported surface. A use `c::member` expands to
  `a::b::c::member`. Both planes ride one namespace — `mem::alloc` (a value) and `mem::Arena`
  (a type) are both reached through `import std::mem`, told apart by the **member's** own
  casing, not the binding's. (This is the deliberate simplification over the older
  value-vs-type namespace split: a single `::` path reaches any member.)
- **A renamed path** `import a::b::c as n` binds `n` — not the last segment `c` — as the
  namespace over the same **whole** surface, so `n::member` expands to `a::b::c::member`. The
  rename applies to the namespace binding only (never to `::{ ... }` destructuring). It resolves
  a last-segment collision, and — paired with **`pub import`** and a `comptime_if` (§6) — lets
  a barrel expose every platform's implementation under one shared name:

  ```
  if os::target == Os.Darwin
    if arch::target == Arch.Arm64
      pub import std::io::console::darwin::arm64 as console
  ```
- **A destructured path** `import a::b::c::{ x, Y }` pulls **named members straight into
  scope**: `x` means `a::b::c::x`, `Y` means `a::b::c::Y`, written bare. A destructured
  member's own casing says which plane it is (`var_name` → value, `type_name` → type), and it
  must name a member the module exports.
- **A wildcard** `import a::b::c as *` splices `a::b::c`'s **whole exported surface** into the
  current module **flat** — every member at its own name, no nesting prefix and no enumeration.
  As a **`pub import ... as *`** it reexports the entire surface without re-listing it, so a
  barrel stays in sync automatically when the target gains a member (the enumeration a
  destructured `::{ ... }` reexport would otherwise have to spell out and keep updated). This is
  the barrel form for platform dispatch — `pub import ...::arm64::darwin as *` makes each of the
  chosen implementation's members reachable through the barrel directly (`console::print`), not
  under an implementation-named nesting. (A wildcard forwards the value plane through the rename
  table; types, which are never mangled, come into scope simply because the target module is
  loaded.)

So an import is **pure sugar**: resolution can be understood as first *expanding* every
abbreviation back to a full path (`c::member` / `n::member` → `a::b::c::member`, bare `x` →
`a::b::c::x`; a wildcard splices `a::b::c`'s members in flat), then resolving full paths against
the module graph. Two programs — one with `import std::mem` + `mem::alloc`, one writing
`std::mem::alloc` inline — flatten identically.

`::` is the **namespace/module traversal** operator and is resolved at compile time. It is
distinct from `.`, which is the runtime member operator — a record `field_access`
(`rec.field`) and a union-variant qualifier (`Maybe.Just`, `Os.Darwin`). A path may traverse
several namespace hops (`a::b::c::member`) when `b`/`c` are modules reexported as nested
namespaces by their parents (§5); each hop is a compile-time step through an exported surface,
and the terminal segment is the value or type reached.

**Name collisions.** Destructuring two different members to the same name, or a destructured
name (or an import's last-segment/`as`-renamed binding) colliding with a local declaration, is
an error (§9) — resolved by an `as` rename or a full `::` path, both always available. A member and a
same-named member reached through *different* paths do not collide (they are distinct `::`
paths).

## 5. Barrels and reexport

A module's **exported surface** is a set of named members: each `pub` declaration
contributes its name, and each **`pub import`** contributes what it reexports. A `pub import`
reads exactly like `pub` on a declaration — the imported binding is exported onward — so a
module importing *this* one may reach those names **through here**. This is how a **barrel** (a
module that gathers and re-publishes a curated surface) is built.

**Reexport preserves the imported form.** A member reexported by a `pub import` enters the
barrel's surface with the same shape the import gave it:

- A **destructured** reexport contributes **flat members** — `pub import impl::{ x }`
  adds `x` to the surface directly.
- A **wildcard** reexport contributes **every flat member at once** — `pub import impl as *`
  adds all of `impl`'s exported members to the surface directly, exactly as a destructured
  reexport that enumerated them would, but without the enumeration (so a new member in `impl`
  joins the barrel automatically).
- A **bare-path** reexport contributes a **nested namespace** — `pub import impl::io`
  adds `io` to the surface as a namespace, whose own members are reached through it (`io::...`).

**A namespace path windows over the whole surface.** Reaching a module `M` by path — via an
`import M` abbreviation or a full path — exposes **all of `M`'s exported surface** (its `pub`
declarations *and* its reexports) as members `M::<name>`. A flat member is reached `M::x`; a
reexported namespace is reached and traversed `M::io::open`. Two barrel shapes:

```
(* --- destructured reexport → flat member --- *)
(* file "barrel.cf"   *)  pub import impl::{ x }       (* x joins barrel's surface *)
(* file "consumer.cf" *)  import barrel                (* window over the surface (or write barrel::x inline) *)
                          ... barrel::x ...                (* the flat member *)

(* --- bare-path reexport → nested namespace --- *)
(* file "blah_barrel.cf" *)  pub import ext::blah      (* blah joins the surface, nested *)
(* file "consumer.cf"    *)  import blah_barrel
                             ... blah_barrel::blah::open ...   (* traversed through the nesting *)

(* --- wildcard reexport → all flat members --- *)
(* file "barrel.cf"   *)  pub import impl as *          (* every impl member joins barrel's surface *)
(* file "consumer.cf" *)  import barrel
                          ... barrel::x ... barrel::y ...      (* each flat member, no re-listing in barrel *)
```

Barrels **chain transitively** with no depth limit; the resolver "jumps through barrels"
([[order_of_compilation.md]] §Resolve) — a `pub import` is an ordinary import wearing a
nicer name, followed to the original declaration during flatten — and a reexport cycle is
resolved by the flatten like any other cycle (§8). A reexport that is itself `pub` extends
the chain onward.

The barrel forms are the bare-path reexport (`pub import ext::blah` → nested namespace), the
destructured reexport (`pub import impl::{ x }` → flat member), and the wildcard reexport
(`pub import impl as *` → all members flat). All are settled semantics the keeper implements
(§ Reconciliation).

A **non-`pub` import** binds names for use inside the module only and adds nothing to its
exported surface — an importer of this module cannot reach them. (A full `::` path can still
reach the target module directly, since paths never depend on imports; what a non-`pub`
import withholds is only the *abbreviation*, and the reexport of the target's surface through
*this* module.)

## 6. Comptime conditional compilation

A module-level **`comptime_if`** — `if <cond> then <branch> [else <branch>]` at the top
level ([[ebnf.md]] § Modules) — brings **build-time conditional compilation**. Position
distinguishes it from the value-level `if_expr` (a module item is never an expression); it
is evaluated at **comptime, during resolve** (§8), not at runtime.

Semantics:

- The **condition reads the `std::comptime` module** (§7): a target member compared against an
  `Os`/`Arch` variant with `==`/`!=` — `os::target == Os.Darwin`, `arch::target == Arch.Arm64`.
  The target member is reached by `::` (a namespace path — `os::target`, or fully
  `std::comptime::os::target`); the variant stays on `.` (`Os.Darwin` is a union-variant
  qualifier). (`Os`/`Arch` are tag-only unions, so this is ordinary union tag equality,
  [[type_system.md]] §5.6.) This restricted shape is the blessed genesis form; whether the
  keeper widens it to any comptime-known `Bool` over the surface is a possible relaxation,
  **not yet ruled** — a non-comptime condition is an error either way (§9).
- **Exactly one branch survives** into the module; the other branch and the `if`
  scaffolding itself **dissolve, leaving no trace**. `else if` chains for free (a branch may
  itself be a `comptime_if`), and a branch may brace a group of items.
- A conditional **surrounds** module items — imports never branch *internally*. This is how
  a single name resolves to a different backing per target: each branch imports the same
  member name from a different module, and exactly one reaches the rest of the file.
- **The losing branch is never resolved.** Its import paths are not mapped to files, so a
  dead branch may name a module that does not exist on this target (a `sys/linux` backing
  absent from a darwin checkout) without error. Path resolution happens only for the
  surviving branch (§2, §8).

```
import std::comptime::{ os, arch, Os, Arch }

if os::target == Os.Darwin then
  if arch::target == Arch.Arm64 then import sys::darwin::arm64::{ read_file }
  else import sys::darwin::amd64::{ read_file }
else import sys::linux::{ read_file }
```

Exactly one `read_file` reaches the module; the selection leaves no residue in the
flattened output.

## 7. The `std::comptime` module

`std::comptime` is a **standard-library module** reached by `::` path like any other (§2/§4),
with **no special path-casing** — `std::comptime::os::target` resolves the ordinary way, and
the abbreviations come from `import std::comptime::{ ... }`. What makes it special is not the
path but its **members**: the target descriptors `os`/`arch` are **compiler-supplied comptime
intrinsics** — the compiler provides `target`/`current` from the build's `--target`
([[type_system.md]] § Intrinsics) — so nothing the module exposes emits runtime code; it
exists to be read at comptime, chiefly by a `comptime_if` condition (§6). Its members are all
**comptime-known** ([[type_system.md]] §9.1).

Its surface must agree with [[cf_cli.md]] §5, which already ratifies the `--target`
os-arch pair and the values `os::target`/`arch::target` take.

### 7.1 The target enums

```
Os   = { Darwin, Linux }
Arch = { Arm64, Amd64, Riscv64, Wasm }
```

`Os` and `Arch` are ordinary **tag-only unions** (like `Bool`): each variant is a bare tag,
compared with `==`/`!=` ([[type_system.md]] §5.6, §8.4). They are the type half of the
module — reached `std::comptime::Os` (or destructured `import std::comptime::{ Os, Arch }`)
and their variants written on `.` (`Os.Darwin`, `Arch.Arm64` — a union-variant qualifier, not
a namespace path). The variant sets are **closed** — a target is one of these — in
coordination with [[cf_cli.md]] §5's `--target` token list.

**Wasm is an `Arch`, not an `Os`** — it is not an operating system, so it is a variant of
`Arch` only; the `wasm` `--target` token sets `arch::target` to `Arch.Wasm` and leaves
`os::target` at the host/default `Os` ([[cf_cli.md]] §5). `Os` therefore has no `Wasm`
variant.

### 7.2 The target namespaces

```
os   : { target: Os,   current: Os   }        (* comptime members, not a runtime record *)
arch : { target: Arch, current: Arch }
```

`os` and `arch` are the value half — namespaces exposing the two members below, reached by
`::` (`os::target`, or fully `std::comptime::os::target`). The conventional form imports the
abbreviations: `import std::comptime::{ os, arch, Os, Arch }`. The `{ ... }` above names each
namespace's comptime members, not a runtime record value (§4: there is no runtime namespace
object). Each exposes two comptime-known members:

- **`target`** — the platform the program is being **compiled for** (the output binary's
  platform). This is what conditional compilation keys on: `os::target`, `arch::target`.
- **`current`** — the platform the **compiler process runs on** (the build host). Equal to
  `target` for an ordinary (non-cross) build; the two differ only when cross-compiling.

Both are values of the corresponding enum (`os::target : Os`, `arch::current : Arch`).
`target` **defaults to `current`** — the host pair — and is overridden by `--target`
([[cf_cli.md]] §5, "Default is the host pair"). The `target`/`current` split exists for
build-tools that must reason about the host they run on versus the platform they emit for:
a compiler that shells out to a host `qbe`/`cc` reads `current`, while an ordinary program
reads `target` (where it will run *is* its target).

### 7.3 Comptime-known-ness

Every `std::comptime` member is comptime-known ([[type_system.md]] §9.1), so an expression
over them (`os::target == Os.Darwin`) folds at comptime. The resolver evaluates `comptime_if`
conditions using this surface with the target fixed for the compilation. Whether such an
expression may also appear outside a `comptime_if` — in a `const`, a comptime function
argument (`const t = std::comptime::os::target`) — follows the general comptime-known-ness
rule of §6 (the genesis form restricts the condition to the `os::target == Variant` shape; a
wider use is the not-yet-ruled relaxation, not a keeper guarantee).

## 8. Resolve and flatten

The resolver (phase 2, [[order_of_compilation.md]] §Resolve) turns the entry module and its
import graph into one import-free `.cf`:

1. **Evaluate comptime conditional imports** (§6) against the `std::comptime` surface (§7),
   with the target fixed for this compilation. Exactly one branch of each `comptime_if`
   survives; the scaffolding and losing branches dissolve. Only surviving imports/paths are
   resolved further.
2. **Expand abbreviations to full paths** (§4). Each `import` binds an abbreviation (a
   last-segment or `as`-renamed namespace, destructured names, or an `as *` wildcard that
   splices the target's whole surface flat); every use of one is rewritten to the full `::`
   path it stands for, so the rest of resolution works on full paths alone. A path
   written out inline needs no expansion — it is already canonical.
3. **Resolve module paths** (§2) — the module prefix of each full path → a file — and
   **load transitively**, following each loaded module's own paths. The set of modules loaded
   is exactly those named by a surviving full path (whether via an import abbreviation or
   written inline); an `import` adds no dependency an inline path would not.
4. **Traverse namespaces / jump through barrels** (§5) — a path's trailing `::` hops walk the
   target's exported surface, following each `pub import` reexport to the original declaration.
5. **Cycles are legal.** A module import cycle (`a` imports `b`, `b` imports `a`) is
   resolved by the flatten itself — every module becomes top-level declarations in one
   file, so a cycle is just mutual reference, which functions already permit ([[ebnf.md]]
   § Modules reference-resolution). (A *value-binding* cycle `const a = b; const b = a`
   remains an error — but that is a later gate's concern, not the module system's.)
6. **Flatten and mangle.** Every module's declarations move to the single output file;
   module-qualified names are **mangled path-relative to the entry module** so two modules'
   private `helper`s become distinct collision-free top-level names
   (`std::mem::arena` → `std_mem_arena_*`, [[order_of_compilation.md]] §5 "Names the compiler
   mints").
7. **Prune** unused imports and the dissolved conditional-import branches.

Output: a single `.cf` file, no imports, every reference resolved to a mangled top-level
name — the input to the typecheck gate.

## 9. Errors (the module system's rejection set)

The resolver rejects, before any typecheck:

- **Unresolvable path** — a surviving path (imported or inline) names a module the toolchain
  cannot map to a file, or traverses to a member the target does not export. (A *losing*
  comptime branch's path is never resolved, §6, so it is never an error.)
- **Importing the entry module** — a path that reaches the compilation root (the file
  carrying `main`), directly or transitively (§2).
- **Reaching a private name** — `import m::{ x }` (or the inline `m::x`) where `m` does not
  export `x` (no `pub` declaration and no `pub import` reexport of `x`).
- **Name collision** — two destructured imports binding the same name, or a destructured name
  or an import's last-segment binding colliding with a local top-level declaration. (Resolved
  by using a full `::` path, always available.)
- **A non-comptime `comptime_if` condition** — a top-level `if` whose condition is not
  comptime-known (does not reduce over the `std::comptime` surface / other comptime-known
  values, §6/§7).
- **A `.`/`::` mismatch** — using `::` (a namespace path) to reach a value's field or a union
  variant, or `.` to traverse a namespace/module. `::` is compile-time module traversal; `.`
  is a runtime field / union-variant qualifier (§4).

Type errors in reached names (a mismatched signature, an undeclared field) are the
typecheck gate's ([[type_system.md]]); the module system only guarantees the names resolve.

## Reconciliation status

This revision moves the module surface to **`::` paths** and makes **imports optional sugar**
(§4), a deliberate simplification over the earlier `.`-qualified, string-path,
`as`-aliased design. The change removes a class of ambiguity: `.` had to serve both namespace
traversal and value/field access, so `a.b.c` could not be told apart from a namespaced value's
field, and a namespaced call collided with union-variant construction. A distinct `::` for
module/namespace paths — with `.` left to runtime fields and union variants — lets the surface
carry the intent, so both fall out cleanly, and the value-vs-type namespace split is no longer
needed (a single `::` path reaches any member). This surface is now **fully implemented** in the
self-hosted compiler `cf`; the legacy `.`/string-import surface has been migrated and dropped:

- **Path & import surface** — DONE: `import a::b::c` (binds the last segment), `import a::b::c
  as n` (renames the namespace binding), `import a::b::c as *` (wildcard — splices the whole
  surface flat), and `import a::b::c::{ x, Y }` (destructure). No string path. The whole corpus
  and `cf`'s own source were migrated `.`→`::`, and the parser/strip legacy paths (string
  imports, `.`-namespace mangling) are removed.
- **Full-path access / imports optional** — DONE: any `pub` member is reachable by full `::`
  path with no import (`std::comptime::os::target`). The strip pass mangles a full path flat
  (`::`→`_`) to its path-prefix-mangled definition and records the module as a synthetic import
  so the loader pulls it in (dedup by path).
- **Namespace reexport → nested namespace** — DONE: `pub import ext::blah` reexported and
  traversed `blah_barrel::blah::open` (§5); corpus covers deep chains and types-through-namespaces.
- **`std::comptime`** — DONE: a real std path (§7), not a pseudo-module. `os`/`arch` are
  compiler-supplied comptime intrinsics reached by `::` (`os::target` a bodyless value intrinsic);
  `comptime_if` folds the resolved token. Only `runtime` remains a reserved pseudo-namespace.
- **Closed variant sets & `current`** — the `Os`/`Arch` variant sets (§7.1) and `arch::current`
  (§7.2) are keeper semantics.

The one cross-spec coordination stands: [[cf_cli.md]] §5's `wasm` sets the `Arch` only (not the
`Os`), so `os::target ∈ {darwin, linux}` matches §7.1 — Wasm is not an OS.
