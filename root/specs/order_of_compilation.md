# C! Order of Compilation

How a C! program travels from source to a native binary — the phases, their
order, and the one rule that shapes all of them: **every language-level phase can
stop and hand its result back as a `.cf` file you can read.**

This is the pass pipeline the rest of the toolchain hangs off. It is the frame
[[geometry_lowering.md]] fills in (the late memory passes) and the frame
[[seed_subset.md]] cuts down (the generation-0 bootstrap runs a marked subset of
it). It assumes the grammar of [[ebnf.md]] and the semantics of
[[memory_model.md]].

Status: design settled. The phase list and the observability invariant below are
ratified; the interior of the memory passes is deferred to [[geometry_lowering.md]].

## The observability invariant

C! leans hard on two kinds of work the compiler does to user code — **desugaring
runs** and **optimization passes** — and both are normally invisible. You cannot
see the sugar dissolve, or the memory model land, or dead code vanish. C! makes
them visible:

> At every observable phase the compiler can **stop and emit its intermediate
> state as a valid `.cf` file**, tagged with a pragma naming the phase it stopped
> at. Feeding that file back resumes from there.

This is not incremental compilation — it is **exploration**. You ask "what does my
code look like once memory management has landed?" and the compiler shows you, in
the language you already read. Two consequences run through the whole design:

- **Every intermediate must be expressible as C!.** A phase may not produce
  something the language cannot spell. Where a lowering would escape the language
  (a hidden node parameter, a mangled name), it uses forms that are still legal
  C! (a `node_0` variable, an `alloc_b__arena` binding). Where a form is genuinely
  sub-language (QBE dispatch tables, placed epilogues), it is pushed past the last
  `.cf` phase into the backend.
- **A phase boundary is a _milestone_, not a peek.** An observable phase (an
  **arc**) is a whole transformation _fully landed_ — "sugar stripped," "memory
  model landed," "dead code gone" — never a half-applied internal state. Nobody
  wants to see `node_0` threaded but the hooks not yet placed. Fine-grained
  internal steps are grouped up to the nearest meaningful arc.

## Gates, arcs, and the backend

Every phase is one of three kinds:

- **Gate** — validates, may raise a compile error, and transforms nothing. A gate
  has no observable output (the `.cf` before and after is identical), so it is not
  a stop point. Lexing, parsing, type-checking, effect-checking, and the
  region-outlives check are gates.
- **Arc** — a real transformation. It is a stop point: it emits readable `.cf`
  with a `cf-stage` pragma. The six arcs are the backbone of the pipeline.
- **Backend** — one-way lowering to non-`.cf` artifacts (`.qbe`, `.s`, the
  binary). Once you cross into the backend the `.cf` story ends; the backend is
  not resumable (see Stopping, emitting, and resuming).

## The pipeline

| #   | Phase                                             | Kind    | Emits                   |
| --- | ------------------------------------------------- | ------- | ----------------------- |
| 0   | Lex                                               | gate    | —                       |
| 1   | Parse → AST                                       | gate    | —                       |
| 2   | **Resolve & flatten**                             | arc     | `cf-stage: resolved`    |
| 3   | **Typecheck & bind**                              | gate    | —                       |
| 4   | **Desugar**                                       | arc     | `cf-stage: desugared`   |
| 5   | **Effect & escape**                               | gate    | —                       |
| 6   | **Specialize**                                    | arc     | `cf-stage: specialized` |
| 7   | **Memory** (decide → [region gate] → materialize) | arc     | `cf-stage: memory`      |
| 8   | **Comptime fold**                                 | arc     | `cf-stage: folded`      |
| 9   | **DCE**                                           | arc     | `cf-stage: pruned`      |
| 10  | Emit QBE IL                                       | backend | `.qbe`                  |
| 11  | `qbe` → assembly                                  | backend | `.s`                    |
| 12  | `cc` (assemble + link)                            | backend | binary                  |

**Six arcs:** `resolved → desugared → specialized → memory → folded → pruned`.
They always run in this order. Lexing, parsing, and AST construction precede them
with nothing to stop on yet; the three gates (typecheck, effect, region) sit
between arcs and only ever error.

C! is **whole-program**: a build has a **root set** and specializes everything
reachable from it. Normally the root is `main` (see [[ebnf.md]], Entry Point) and
the output is an executable; another module is code-generated only by being
imported and specialized against _this_ `main`'s use. But C! code **may** also be
compiled as a **library** — rooted at its exported `pub` functions under a pinned
geometry (the node-free `libc` policy is the natural target), emitting a C-ABI
artifact whose functions drop the node parameter and the `!` marker. The pipeline
is identical either way; only the root set and the ambient geometry differ. The
C-ABI representation of C! types and the ownership contract are their own subject,
left to a dedicated spec. A module can also just be _type-checked_ alone (the
gates run) — and **checking is not building**.

## Phases in depth

### 0–1. Lex and Parse (gates)

Tokens, then an AST. Newline-termination, the `#` comment rule, and identifier
casing are lexer concerns (see [[ebnf.md]], Conventions). Both can raise errors;
neither has an intermediate worth emitting, so neither is an arc.

### Resolve & flatten → `resolved` (arc)

Turns a set of files into **one flat program**:

- **Evaluate comptime conditional imports.** The module-level `if … then … else`
  (see [[ebnf.md]], Modules) is run here, at comptime, against the target triple
  exposed by the compiler-supplied `"comptime"` module. Exactly one branch's
  items survive; the scaffolding dissolves, so a name like `read_file` resolves to
  a single backing.
- **Resolve module paths** (`"std/mem"` → a file) and **jump through barrels** —
  a `pub import` reexport is an ordinary import wearing a nicer name, so it is
  followed to its ultimate target.
- **Cycles are legal** and resolved by the flatten itself.
- **Flatten** every module into one file and **mangle module-qualified names**
  path-relative to the main file, so two modules' private `helper`s become
  distinct, collision-free top-level names (`std/mem/arena` → `std_mem_arena_*`).
  Names stay valid `var_name`/`type_name`s and thus readable.
- **Prune** unused imports and the dissolved conditional-import branches.

Output: a single `.cf` file, no imports, every reference resolved to a
flat, unique name.

### Typecheck & bind (gate)

Binds every name and checks types. This gate owns: type inference and checking;
the casing rule (PascalCase types, snake_case values); `const`/immutability and
lvalue checks; **aggregate-literal kind** resolution (fixed vs tuple vs dynamic —
see [[ebnf.md]], Aggregate Literals); and **`match` exhaustiveness**. It annotates
the tree and rejects the ill-typed; it changes nothing, so it is a gate, not an
arc. All _later_ desugars are type-directed, which is exactly why the type gate
runs before them — a desugar never has to guess what a form means.

### Desugar → `desugared` (arc)

Reduces every sugar to its core form, **type-directed**, in one arc. The full
catalog is below (The desugar catalog). Two constructs are deliberately **not**
desugared here — `match` and `defer` — because their lowerings are sub-language
(dispatch tables, placed epilogues) and would smear unreadable forms across every
later arc; they stay symbolic and lower at emit. Everything else — compound
assignment, pipes, puns, string interpolation, else-less `if`, `for`, spreads —
is gone by the end of this arc.

Note that some desugars **introduce allocation** (string interpolation becomes
builder calls). That is intended, and it is why the effect gate runs _after_ this
arc, not before.

### Effect & escape (gate)

Two analyses over the desugared form:

- **The `!` effect, checked both ways per declaration.** A missing `!` on an
  allocating function is an error; a redundant `!` on a colorless one is too (see
  [[memory_model.md]]). It is checked on the **desugared** form so the checker
  sees the allocation that sugar introduced, and it is checked **per declaration**
  — the marker then propagates to call sites mechanically through the name, so
  there is no second, call-site effect pass.
- **Escape/residue classification.** Each aggregate site is tagged with its fate —
  _local_ (dies in frame), _escapes-via-return_, _rehomed_ (stored into another's
  aggregate), or _grow-in-place_ (self-spread reassign). This is the same analysis
  as `!`, recorded per site, and it is what the memory arc reads to pick between
  `on_alloc`, `on_ret`, `on_rehome`, and `on_realloc`.

A gate: it annotates and rejects, it does not transform.

### Specialize → `specialized` (arc)

Monomorphizes the whole program from `main` as a comptime worklist. The
specialization key is **`(fn, type-args, value-args, capture-env)`** — but **not
geometry**, which is resolved later, in the memory arc. This arc:

- runs **comptime-for-instantiation**: evaluating type arguments, comptime value
  parameters (`[n 'T]` sizes), and the like — just enough to know which concrete
  instances exist (the _other_ comptime role, folding runtime-shaped expressions,
  is the later `folded` arc);
- lowers **closure capture to hidden parameters** (C! closures are comptime; see
  [[memory_model.md]], Closures) — the same specialization step, same key;
- mints one instance per distinct key.

Comptime evaluation here can diverge (a recursive comptime function). A
**termination guard** bounds it: exhausting the budget is a compile error naming
the runaway entity, not a hang.

### Memory → `memory` (arc)

The heaviest arc: the geometry lands on user code. It runs in **three substeps**,
and only the third's output is the observable `memory` `.cf`.

1. **Decide.** Propagate geometry down the specialized call graph (a call with no
   `in` inherits its caller's ambient geometry) and assign each node a **comptime
   identity**. Annotations only — no bodies are duplicated yet.
2. **Region-outlives check (gate).** With node identities now concrete, verify no
   pointer or return is rehomed into a node that outlives its target — the whole
   of C!'s lifetime checking, a comptime check over node identities (see
   [[memory_model.md]]). It runs _here_, before duplication, so errors point at the
   single pre-explosion body.
3. **Materialize.** Now render the decision:
   - **Duplicate per `(fn × geometry)`** — a function used under an arena and under
     a GC becomes two instances, because their inlined hooks differ. Geometry is a
     specialization axis; it is just resolved in this arc rather than the previous
     one.
   - **Mangle names** (see Names the compiler mints): `!` → `_b`, geometry →
     `__<geom>`, so `alloc!` under `arena` becomes `alloc_b__arena`.
   - **Thread node handles** — add the hidden node parameter **only where the
     subtree is not provably node-free** (a colorless callee taking no `!`
     argument and returning only scalars carries no node; see [[memory_model.md]]).
   - **Land geometry hooks** — wrap each residue site per its escape class with
     `on_alloc` / `on_realloc` / `on_ret` / `on_alloc_ret` / `on_rehome`, and open
     each scope with `on_scope_enter` plus a **`defer`'d** `on_scope_exit`.
   - **Sweep no-op hooks** — a per-step finisher DCE drops hooks a policy leaves
     empty (an arena's `on_free`, etc.). This is the local kind of DCE (see DCE
     below), not the whole-program pass.

The hook set and the exact bodies — the `%node`/`%ret` calling convention,
claim-once returns, compact-on-claim — are [[geometry_lowering.md]]. This arc only
places the calls.

### Comptime fold → `folded` (arc)

The second comptime role: evaluate everything comptime-known that survived into
runtime shape. Fold `identity(x)` to `x`, propagate constants, and **inline the
bodies of non-deferred hooks** to their literal operations (an `on_alloc` becomes
the bump it stands for). The one carve-out: **fold does not reach inside a
`defer`.** A deferred hook must stay a _call_ through this arc — inlining it would
produce `defer { …instructions… }` before placement, and while `defer { block }`
is legal, an _instruction_ block is not a thing to defer. Deferred hooks are
inlined _and_ placed together at emit.

### DCE → `pruned` (arc)

**DCE has two meanings in this pipeline.** The general pass is here: drop every
function and binding **no longer reachable from `main`**. There is no structural
comptime/runtime split, so the rule is blunt — if nothing calls it, it goes,
including functions that ran only at comptime and left no runtime residue. The
_other_ kind of DCE is the **per-step finisher** each arc may run to clean up after
itself (the no-op-hook sweep in the memory arc is the example); those are part of
their arc, and some arcs have none.

### Backend: emit QBE IL, `qbe`, `cc`

Past `pruned`, the `.cf` story ends. Emit lowers what was kept symbolic:

- **`match` → dispatch.** A **table** for the `cf` compiler, a **compare-chain**
  for the `cf0` bootstrap (QBE has no switch/jump-table/indirect jump, so a real
  table is a function-pointer array + indirect call that _we_ synthesize; see
  [[seed_subset.md]]).
- **`defer` → placement.** Every scheduled call (user `defer` and geometry
  teardown alike) is emitted into the exit paths, LIFO, as QBE **epilogue blocks**
  — where multi-exit sharing is free, so none of the C!-level duplication a
  `.cf`-level placement would have needed.
- **asm-bodied functions** are emitted **verbatim beside** the QBE output (they
  bypass QBE entirely; see [[ebnf.md]], Assembly), and the `!` in a name is
  stripped from the emitted symbol.

Then `qbe` turns the IL into assembly and `cc` assembles and links it. `qbe` is
**vendored** into the toolchain, so building a C! binary needs no external QBE.

## Names the compiler mints

Every generated name is a legal C! identifier, so the arcs stay valid `.cf`.

| minted                | scheme                            | example                                    |
| --------------------- | --------------------------------- | ------------------------------------------ |
| bang symbol           | `!` → `_b` suffix                 | `alloc!` → `alloc_b`                       |
| geometry instance     | `__<geom>` suffix                 | `alloc_b` under `arena` → `alloc_b__arena` |
| node handle           | `node_<index>`, per body          | `node_0`, `node_1`                         |
| scope mark            | `mark_<index>`, per body          | `mark_0`, `mark_1`                         |
| flattened module name | path-relative to main, `_`-joined | `std/mem/arena` → `std_mem_arena_*`        |

Indices are **per body** and **positional** — two arenas in one function are
`node_0` and `node_1`; the same node threaded into a callee keeps its handle. They
are indices, not hashes, deliberately: positional numbering re-emits **byte-for-
byte identically**, which the resume byte-compare depends on. QBE-level sigils
(`$`, `%`) are applied at emit and never appear in a `.cf` arc.

The `_b` bang mangling is the **internal** form. A **C export** instead strips the
bang bare (`alloc!` → `alloc`) and carries no geometry suffix, since its symbols
face the C world — which is why `foo` and `foo!` cannot both be exported. That
export path is a dedicated subject (see the library note under The pipeline).

## Stopping, emitting, and resuming

Any arc can be a stop point. The emitted `.cf` opens with a pragma block:

```
# cf-stage:       memory
# cf-source:      /abs/path/to/main.cf
# cf-source-hash: <hash of the original source bytes>
# cf-self-hash:   <hash of this file's body, excluding this line>
# cf-compiler:    <GenVer + target triple>
```

- **`cf-source`** is the path to the **main file** — the compilation entry — so
  resume knows what to recompile.
- **`cf-source-hash`** pins the original bytes: if the source drifts before you
  resume, the compiler says _that_ plainly instead of a confusing stage mismatch.
- **`cf-self-hash`** is the checkpoint's own integrity (it excludes its own line to
  avoid the chicken-and-egg).

**Resume is exploration, not trust.** The intermediate `.cf` is a **read-only**
artifact; the compiler never structurally re-parses it as input. To resume from a
stage-N file it:

1. reads `cf-source`, **recompiles from the original main file up to stage N**,
2. runs both the fresh output and the on-disk file through the **formatter**,
3. **byte-compares** them,
4. continues to N+1 iff they match, else errors _"checkpoint diverged."_

Because it re-derives from source, error locations always come from the real
compile — a hand-edited or stale checkpoint can never silently mislead the rest of
the run. Every arc is **idempotent** (re-running stage N on stage-N output is
identity), which is what makes the compare sound.

The **formatter** is a cross-cutting service, not an arc: every emitted `.cf` goes
through it, so the compiler emits _correct_ C! and never hand-fights tabs, and the
byte-compare is against a canonical form.

The backend artifacts (`.qbe`, `.s`) are **not** resumable — resume is a
`.cf`-only story, and QBE's internals are out of scope for the compiler to
reconstruct.

## The desugar catalog

Everything the `desugared` arc rewrites, and the two forms held back for emit.

| sugar                                      | desugars to                                                       |
| ------------------------------------------ | ----------------------------------------------------------------- |
| compound assignment `x op= y`              | `x = x op y`                                                      |
| pipe `x \|> f`                             | `f(x)` (and `x \|> sum(2)` → `sum(2, x)` via partial application) |
| pipe tap `x \|> defer f`                   | `defer f(x)`                                                      |
| field pun `{ value }`                      | `{ value: value }`                                                |
| string interpolation `"${e}"`              | builder calls (allocates → colors `!`)                            |
| else-less `if c then v`                    | `Option` — `Some(v)` / `None`                                     |
| `for x in xs body`                         | an indexed `loop`                                                 |
| record spread `...Identifiable`            | the spliced fields, in place                                      |
| named-tuple splat (`type X = { … }` param) | its fields, positionally                                          |
| self-spread reassign `xs = [...xs, e]`     | grow-in-place (→ `on_realloc` in memory)                          |
| self-spread copy `let ys = [...xs, e]`     | fresh copy (→ `on_alloc`/`on_ret` in memory)                      |

Held back, lowered at **emit** (kept readable through every arc):

| construct | lowered to                                       |
| --------- | ------------------------------------------------ |
| `match`   | dispatch — table (`cf`) or compare-chain (`cf0`) |
| `defer`   | LIFO placement into QBE epilogue blocks          |

## Worked example: `make_arr!` arc by arc

Source:

```
pub const main = () -> {
  const arena = mem.arena.of(4096)
  const ys = make_arr!() in arena
}

const make_arr! = () -> {
  let xs = [1, 2, 3]
  xs = [...xs, 4]
  let ys = [...xs, 5]
  return ys
}
```

- **`resolved`** — single file already; `mem.arena.of` flattens to its mangled
  module name (shown short below for readability).
- **`desugared`** — the self-spreads are classified: `xs = [...xs, 4]` is
  grow-in-place, `let ys = [...xs, 5]` is a copy that also escapes via `return`.
- **`specialized`** — no generics, so `make_arr!` is unchanged; geometry is _not_
  resolved here.
- **`memory`** — the geometry lands. `make_arr!` is duplicated for `arena` and
  mangled to `make_arr_b__arena`; the node is threaded; escape classes pick the
  hooks; scopes open with `on_scope_enter` and a `defer`'d `on_scope_exit`:

```
pub const main = (node_0) -> {
  const mark_0 = page.on_scope_enter(node_0)
  defer page.on_scope_exit(node_0, mark_0)

  const node_1 = page.on_alloc(node_0, mem.arena.of(node_0, 4096))
  const ys = page.on_alloc_ret(node_0, make_arr_b__arena(node_1))
}

const make_arr_b__arena = (node_0) -> {
  const mark_0 = arena.on_scope_enter(node_0)
  defer arena.on_scope_exit(node_0, mark_0)

  let xs = arena.on_alloc(node_0, [1, 2, 3])
  xs = arena.on_realloc(node_0, [...xs, 4])
  let ys = arena.on_ret(node_0, [...xs, 5])
  return ys
}
```

(The arena binding _is_ its node, so `const arena` becomes the handle `node_1`;
`page`/`arena` hook names are shown short — flatten would render them as mangled
module names.)

- **`folded`** — the non-deferred hooks (`on_scope_enter`, `on_alloc`,
  `on_realloc`, `on_ret`, `on_alloc_ret`) inline to their literal bump/copy
  operations; the `defer`'d `on_scope_exit` stays a call.
- **`pruned`** — `main`'s `ys` is unused, so it and anything only it reached are
  dropped; comptime-only helpers go too.
- **emit** — `defer`'d teardowns are placed into epilogue blocks; the result is
  QBE IL, then assembly, then a binary.

## `cf` vs `cf0`

The bootstrap compiler (`cf0`) runs this **same pipeline shape** but exercises a
narrower language and takes the cheaper choice where one exists — most visibly
**compare-chain `match` dispatch** instead of tables, and a single geometry rather
than the full set. Which phases `cf0` implements in full, and which it runs
degenerately, is [[seed_subset.md]].
