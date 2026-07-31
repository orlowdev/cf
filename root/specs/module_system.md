# C! Module System

This document defines the semantics of C!'s modules: how files import one another,
what a module exports, how barrels (`pub import` reexports) chain, how build-time
conditional compilation selects a backing per target, and the surface of the
compiler-supplied **`"comptime"`** module (`os`/`arch`, `Os`/`Arch`, `target`/`current`).

It is the semantics behind the module *grammar* of [[ebnf.md]] (§ Imports, § Visibility,
§ Modules), which shapes only the surface and repeatedly defers the semantic rules —
"which names a module exports", "the values-vs-types split keyed on casing", "how paths
resolve to files", and, explicitly, "the `"comptime"` module's full surface … its own
deferred spec" — to here. It sits against the pipeline of [[order_of_compilation.md]],
where it is **phase 2, Resolve & flatten** (`cf-stage: resolved`): the resolver runs the
comptime conditional imports, resolves paths, jumps barrels, flattens every module into
one file, mangles module-qualified names, and prunes what no longer applies — turning a
tree of `.cf` files into a single import-free `.cf` for the typecheck gate ([[type_system.md]])
to run on.

Status: **ratified**. A three-reviewer gate ran against the sibling specs, cfcc's
implementation, and internal/decision fidelity; all findings and owner rulings are folded,
and the owner has signed off. Grounded in the ratified
grammar/pipeline and in the behavior of the `cfcc` genesis tool, which already implements
the resolver (destructured + namespace imports, transitive loads, cycles, `pub import`
barrels, comptime conditional imports, `std/` root, flatten-and-mangle, DCE). Where this
spec **leads cfcc** — `os.current`/`arch.current`, the closed `Os`/`Arch` variant sets, and
**namespace reexport** (`pub import … as ns`) — it states the target semantics and the
cfcc-lag notes are collected in § Reconciliation. The target surface (§7) is coordinated
with [[cf_cli.md]] §5 (the `--target` os-arch pair) — one coordinated edit landed there:
`wasm` sets the `Arch` only, since Wasm is not an OS.

## 1. What the module system owns

Per [[order_of_compilation.md]] §Resolve the module system owns, and this spec defines:

- **module identity and path resolution** (§2) — a module is a `.cf` file; a path
  string names another module; the toolchain maps path → file.
- **visibility** (§3) — `pub` exports a declaration; everything else is file-private.
- **imports and namespaces** (§4) — the three `as` forms and the values-vs-types split
  keyed on the alias's casing.
- **barrels / reexport** (§5) — `pub import` reexports what it brings in, and barrels
  chain transitively.
- **comptime conditional compilation** (§6) — the module-level `if … then … else`
  that selects exactly one backing per target and dissolves the rest.
- **the `"comptime"` module surface** (§7) — the compiler-supplied `os`/`arch`
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

Another module is named by a **path string with no `.cf` extension** — `import "std/mem"`,
`import "lexer"`, `import "sys/darwin/arm64"`. The toolchain resolves the string to a file
([[cf_cli.md]] §2, which calls this "the whole module-resolution story"):

- A **bare or relative path** (`"lexer"`, `"sys/darwin/arm64"`) resolves **relative to the
  importing file's directory** (each module's imports against that module's own location).
- A **`std/`-prefixed path** (`"std/mem"`) is the standard library, resolved under the
  toolchain's fixed library root regardless of the importer's location — `std/mem` →
  `<lib>/std/mem.cf`. `std/` is the reserved root prefix; there is **no ambient search
  path** ([[cf_cli.md]] §2).

The resolved path is canonicalized (`..`/`.`/symlinks collapsed), so the same file reached
by two different import strings — a diamond — loads once. Path resolution is lexical and
target-independent: it does not depend on which comptime branch is live (a losing branch's
path is never resolved at all, §6/§8). The `.cf` extension is always implicit and never
written in a path string.

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
pub const lex = (…) -> …       (* exported *)
const scan_ident = (…) -> …    (* file-private *)
pub data Token = { … }         (* exported *)
pub union TokKind = { … }      (* exported *)
```

`pub` lives only on a top-level `declaration`. A `var_decl` used as a block statement is a
local and takes no `pub`. An `import` may also carry `pub`, but that is a **reexport**
(§5), not a declaration.

**A module's exported surface** is exactly: its `pub` declarations, plus the names its
`pub import`s reexport. Everything else — private declarations, non-`pub` imports — is
invisible to importers. An import can only bind a name the target module actually exports;
importing a private name is an error (§9).

## 4. Imports and namespaces

An import (`[ "pub" ] "import" <path> "as" <alias>`, [[ebnf.md]] § Imports) binds what a
module brings in. The `as` target sets both the binding **and — by its casing — which half
of the module it pulls**:

- **`as <var_name>`** (lowercase) binds a **value namespace**: the module's exported
  **values** only, reached qualified — `import "std/mem" as mem` then `mem.alloc`.
- **`as <Type_name>`** (PascalCase) binds a **type namespace**: the module's exported
  **types and data** only, reached qualified — `import "std/math" as Math` then `Math.Vec`.
- **`as { … }`** (destructured) pulls **named members straight into scope**, and is the one
  form that admits **both** kinds at once — `import "std/functor" as { map, Functor }`
  binds the value `map` and the type `Functor` directly (no qualifier). A destructured
  member's own casing says which it is (`var_name` → value, `type_name` → type), and it must
  name a member the module exports.

The **values-vs-types split** is a namespace discipline: a lowercase alias is a window onto
values, a PascalCase alias a window onto types. It mirrors C!'s pervasive casing rule
(lowercase values, PascalCase types) and keeps the two planes from colliding under one
qualifier. Destructuring sidesteps the split by naming members individually.

Qualified access through a namespace (`mem.alloc`, `Math.Vec`) is a **path to the member**,
not a distinct entity: `Math.Vec` is the very type the module exports as `Vec`, reachable
also by destructuring it. A namespace member is resolved at compile time; there is no
runtime namespace object.

**Name collisions.** Destructuring two different members to the same name, or a destructured
name colliding with a local declaration, is an error (§9). Two namespace imports may share
no alias. A member and a same-named member reached through *different* namespaces do not
collide (they are distinct qualified paths).

## 5. Barrels and reexport

A module's **exported surface** is a set of named members: each `pub` declaration
contributes its name, and each **`pub import`** contributes what it reexports. A `pub import`
reads exactly like `pub` on a declaration — the imported binding is exported onward — so a
module importing *this* one may import those names **from here**. This is how a **barrel** (a
module that gathers and re-publishes a curated surface) is built.

**Reexport preserves the imported form.** A member reexported by a `pub import` enters the
barrel's surface with the same shape the import gave it:

- A **destructured** reexport contributes **flat members** — `pub import "impl" as { x }`
  adds `x` to the surface directly.
- A **namespace** reexport contributes a **nested namespace** — `pub import "impl" as io`
  adds `io` to the surface as a namespace, whose own members are reached through it (`io.…`).

**A namespace import windows over the whole surface.** Binding a module `M` as a namespace —
`import "M" as y` (value plane) or `as Y` (type plane, §4) — exposes **all of `M`'s exported
surface** (its `pub` declarations *and* its reexports) as qualified members `y.<name>`. A
flat member is reached `y.x`; a reexported namespace is reached and traversed `y.io.open`.
Two barrel shapes, both settled by the owner's ruling:

```
(* --- destructured reexport → flat member --- *)
(* file "barrel.cf"   *)  pub import "impl" as { x }    (* x joins barrel's surface *)
(* file "consumer.cf" *)  import "barrel" as y          (* y windows over the surface *)
                          … y.x …                       (* the flat member *)

(* --- namespace reexport → nested namespace --- *)
(* file "blah_barrel.cf" *)  pub import "blah" as blah  (* blah joins the surface, nested *)
(* file "consumer.cf"    *)  import "blah_barrel" as foo
                             … foo.blah …               (* and foo.blah.<member> *)
```

Barrels **chain transitively** with no depth limit; the resolver "jumps through barrels"
([[order_of_compilation.md]] §Resolve) — a `pub import` is an ordinary import wearing a
nicer name, followed to the original declaration during flatten — and a reexport cycle is
resolved by the flatten like any other cycle (§8). A reexport that is itself `pub` extends
the chain onward.

Both barrel forms use `pub import … as <namespace>` / `as { … }`; the **namespace-alias
reexport** (`pub import "blah" as blah`) is the one cfcc rejects today
(§ Reconciliation) — the settled semantics lead cfcc here. `cf0.cf`'s own imports are all
destructured, so self-hosting is unaffected.

A **non-`pub` import** binds names for use inside the module only and adds nothing to its
exported surface — an importer of this module cannot reach them.

## 6. Comptime conditional compilation

A module-level **`comptime_if`** — `if <cond> then <branch> [else <branch>]` at the top
level ([[ebnf.md]] § Modules) — brings **build-time conditional compilation**. Position
distinguishes it from the value-level `if_expr` (a module item is never an expression); it
is evaluated at **comptime, during resolve** (§8), not at runtime.

Semantics:

- The **condition reads the `"comptime"` module** (§7): a target member compared against an
  `Os`/`Arch` variant with `==`/`!=` — `os.target == Os.Darwin`, `arch.target == Arch.Arm64`.
  (`Os`/`Arch` are tag-only unions, so this is ordinary union tag equality, [[type_system.md]]
  §5.6.) This restricted shape is the blessed genesis form; whether the keeper widens it to
  any comptime-known `Bool` over the surface is a possible relaxation, **not yet ruled** — a
  non-comptime condition is an error either way (§9).
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
import "comptime" as { os, arch, Os, Arch }

if os.target == Os.Darwin then
  if arch.target == Arch.Arm64 then import "sys/darwin/arm64" as { read_file }
  else import "sys/darwin/amd64" as { read_file }
else import "sys/linux" as { read_file }
```

Exactly one `read_file` reaches the module; the selection leaves no residue in the
flattened output.

## 7. The `"comptime"` module

`"comptime"` is a **compiler-supplied intrinsic module**, not a `.cf` file. The resolver
special-cases the path `"comptime"` (as it does `"runtime"`, out of scope here) and binds
its members without touching the filesystem. It carries **no runtime names** — nothing it
exposes emits code; it exists only to be read at comptime, chiefly by a `comptime_if`
condition (§6). Its members are all **comptime-known** ([[type_system.md]] §9.1).

Its surface must agree with [[cf_cli.md]] §5, which already ratifies the `--target`
os-arch pair and the values `os.target`/`arch.target` take.

### 7.1 The target enums

```
Os   = { Darwin, Linux }
Arch = { Arm64, Amd64, Riscv64, Wasm }
```

`Os` and `Arch` are ordinary **tag-only unions** (like `Bool`): each variant is a bare tag,
compared with `==`/`!=` ([[type_system.md]] §5.6, §8.4). They are the type half of the
module — imported PascalCase (`as { … Os, Arch }` or a type namespace) and written
qualified (`Os.Darwin`, `Arch.Arm64`). The variant sets are **closed** — a target is one of
these — in coordination with [[cf_cli.md]] §5's `--target` token list.

**Wasm is an `Arch`, not an `Os`** — it is not an operating system, so it is a variant of
`Arch` only; the `wasm` `--target` token sets `arch.target` to `Arch.Wasm` and leaves
`os.target` at the host/default `Os` ([[cf_cli.md]] §5). `Os` therefore has no `Wasm`
variant.

### 7.2 The target namespaces

```
os   : { target: Os,   current: Os   }        (* comptime members, not a runtime record *)
arch : { target: Arch, current: Arch }
```

`os` and `arch` are the value half — imported lowercase (per §4; the conventional form is
the destructured `import "comptime" as { os, arch, Os, Arch }`) and read qualified. The
`{ … }` above names each namespace's comptime members, not a runtime record value (§4: there
is no runtime namespace object). Each exposes two comptime-known members:

- **`target`** — the platform the program is being **compiled for** (the output binary's
  platform). This is what conditional compilation keys on: `os.target`, `arch.target`.
- **`current`** — the platform the **compiler process runs on** (the build host). Equal to
  `target` for an ordinary (non-cross) build; the two differ only when cross-compiling.

Both are values of the corresponding enum (`os.target : Os`, `arch.current : Arch`).
`target` **defaults to `current`** — the host pair — and is overridden by `--target`
([[cf_cli.md]] §5, "Default is the host pair"). The `target`/`current` split exists for
build-tools that must reason about the host they run on versus the platform they emit for:
a compiler that shells out to a host `qbe`/`cc` reads `current`, while an ordinary program
reads `target` (where it will run *is* its target).

### 7.3 Comptime-known-ness

Every `"comptime"` member is comptime-known ([[type_system.md]] §9.1), so an expression over
them (`os.target == Os.Darwin`) folds at comptime. The resolver evaluates `comptime_if`
conditions using this surface with the target fixed for the compilation. Whether such an
expression may also appear outside a `comptime_if` — in a `const`, a comptime function
argument — follows the general comptime-known-ness rule of §6 (the genesis form restricts
the condition to the `os.target == Variant` shape; a wider use is the not-yet-ruled
relaxation, not a keeper guarantee).

## 8. Resolve and flatten

The resolver (phase 2, [[order_of_compilation.md]] §Resolve) turns the entry module and its
import graph into one import-free `.cf`:

1. **Evaluate comptime conditional imports** (§6) against the `"comptime"` surface (§7),
   with the target fixed for this compilation. Exactly one branch of each `comptime_if`
   survives; the scaffolding and losing branches dissolve. Only surviving imports are
   resolved further.
2. **Resolve module paths** (§2) — surviving paths → files — and **load transitively**,
   following each imported module's own imports.
3. **Jump through barrels** (§5) — a `pub import` reexport is followed to the original
   declaration.
4. **Cycles are legal.** A module import cycle (`a` imports `b`, `b` imports `a`) is
   resolved by the flatten itself — every module becomes top-level declarations in one
   file, so a cycle is just mutual reference, which functions already permit ([[ebnf.md]]
   § Modules reference-resolution). (A *value-binding* cycle `const a = b; const b = a`
   remains an error — but that is a later gate's concern, not the module system's.)
5. **Flatten and mangle.** Every module's declarations move to the single output file;
   module-qualified names are **mangled path-relative to the entry module** so two modules'
   private `helper`s become distinct collision-free top-level names
   (`std/mem/arena` → `std_mem_arena_*`, [[order_of_compilation.md]] §5 "Names the compiler
   mints").
6. **Prune** unused imports and the dissolved conditional-import branches.

Output: a single `.cf` file, no imports, every reference resolved to a mangled top-level
name — the input to the typecheck gate.

## 9. Errors (the module system's rejection set)

The resolver rejects, before any typecheck:

- **Unresolvable path** — a surviving import names a module the toolchain cannot map to a
  file. (A *losing* comptime branch's path is never resolved, §6, so it is never an error.)
- **Importing the entry module** — a module that imports the compilation root (the file
  carrying `main`), directly or transitively (§2).
- **Importing a private name** — `import "m" as { x }` where `m` does not export `x`
  (no `pub` declaration and no `pub import` reexport of `x`).
- **Name collision** — two destructured imports binding the same name, or a destructured
  name colliding with a local top-level declaration, or two namespace imports sharing an
  alias.
- **A non-comptime `comptime_if` condition** — a top-level `if` whose condition is not
  comptime-known (does not reduce over the `"comptime"` surface / other comptime-known
  values, §6/§7).
- **A wrong-plane qualified access** — reaching a value through a type namespace or vice
  versa (§4's casing split).

Type errors in imported names (a mismatched signature, an undeclared field) are the
typecheck gate's ([[type_system.md]]); the module system only guarantees the names resolve.

## Reconciliation status

The module *grammar* ([[ebnf.md]] § Imports/Visibility/Modules) and the resolve *pipeline*
([[order_of_compilation.md]] §Resolve, §5 "Names the compiler mints") already carry this
spec's surface and mechanics; this document supplies the semantics they defer here — chiefly
§5 (barrel chaining), §6 (comptime_if evaluation), and §7 (the `"comptime"` surface, which
the ebnf names as "its own deferred spec"). One sibling-spec point needs an owner ruling:
the `Os.Wasm` conflict with [[cf_cli.md]] §5 (§7.1). Otherwise no sibling-spec edits are
required; the ebnf's `import_alias` already admits a namespace reexport, and order_of
already lists the comptime-import evaluation and barrel-jumping steps.

**Genesis-tool (`cfcc`) lag — this spec leads cfcc.** The `cfcc` genesis tool implements the
resolver but predates several of this spec's points. They are keeper semantics and `cfcc`
catch-up bricks (each with a corpus test) if we want them exercised there; none blocks
self-hosting, since `cf0.cf`'s own imports are all destructured and it hardcodes the
darwin/arm64 floor (using no `os`/`arch`/`current`):

- **`current` is unimplemented in cfcc** — `parse_comptime_cond` accepts only the field
  `.target` and hardcodes `os → Darwin`, `arch → Arm64` (its fixed target). `.current` is
  new here.
- **cfcc validates no variant membership** — it string-matches the single live value
  (`"Arm64"`/`"Darwin"`) and silently treats **any** other PascalCase variant (`Arch.Wasm`,
  even `Arch.Nonsense`) as a non-match, with no error. There is no `Os`/`Arch` table in
  cfcc at all. This spec's closed variant sets (§7.1) are new, and cfcc's silent-accept is a
  laxity to tighten (opposite direction to the other lags).
- **Namespace reexport is rejected by cfcc** — `pub import "m" as ns` errors
  (`parse_import`, and corpus `807_error_namespace_reexport`), though the ebnf grammar
  admits it and §5 settles both barrel forms (destructured → flat `y.x`; namespace →
  nested `foo.blah`). Flipping 807 to a positive is the cfcc catch-up.
- **The `"comptime"` import is decorative in cfcc** — its destructured list is never bound
  or validated (the module binds no runtime names), the condition reads the bare words
  `os`/`arch` *literally* (so a namespace alias `import "comptime" as cm` → `cm.os.target`
  is unusable), and a variant may be referenced without importing it. This spec treats
  `"comptime"` as a proper intrinsic module (§7); cfcc's decorative handling is the
  narrowing.
- **Condition shape** — cfcc restricts a `comptime_if` condition to exactly
  `os.target`/`arch.target` `OP` variant (`OP ∈ { ==, != }`). §6 blesses that as the genesis
  form and leaves any widening not-yet-ruled — so here cfcc and the keeper agree today.

A coordinated edit landed in the ratified [[cf_cli.md]] §5 to resolve the one cross-spec
conflict: `wasm` sets the `Arch` only (not the `Os`), so `os.target ∈ {darwin, linux}`
matches §7.1 — Wasm is not an OS.
