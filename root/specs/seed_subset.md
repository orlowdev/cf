# C! Seed Subset

The bootstrap language: the subset of C! that generation-0 (`cf0`) implements and
that the self-hosted compiler's own source is written in. It cuts down the frame
[[order_of_compilation.md]] lays out (`cf0` runs the same pipeline, degenerately)
and stands on the geometry story [[geometry_lowering.md]] §6 already tells (`page`
+ `arena`, bump-only). It assumes the grammar of [[ebnf.md]] and the semantics of
[[memory_model.md]].

Status: design settled. The bootstrap chain, the seed medium, and the
kept/dropped feature boundary below are ratified. The first seed's provenance is a
one-time authoring; everything after it is self-reproducing.

## 1. The one rule: a generous seed

Every bootstrap chooses how much language to seed by hand. The tempting choice is
**minimal** — the smallest `cf0` that can express a compiler at all — because it
ships soonest. C! rejects it.

> `cf0` is as **large** as it needs to be for `cf`'s source to be idiomatic,
> full-power C!. The seed subset is maximised, not minimised.

The reasoning is that the self-hosted compiler's source is written **once and read
forever**. A minimal `cf0` buys a few weeks of schedule and pays for it in a
compiler whose own source cannot use the language it compiles — no generics, no
closures, no sugar, a permanent second-class dialect at the heart of the project.
A generous `cf0` costs more up front and leaves `cf`'s source written in the same
C! its users enjoy, which is then **extended, never cleaned up**, in each
subsequent generation. The seed is the floor of the language's ergonomics, so it
is set high on purpose.

The single concession is **deep comptime** (§7) — recursive comptime
metaprogramming, which a compiler does not need to be beautiful. Everything else a
programmer reaches for is in.

Two invariants fall out and hold across every generation:

- **`cf`'s source never leaves the seed subset.** It may use the whole subset
  freely, but nothing outside it — so every generation can rebuild the next.
- **Implementing a feature is not using it.** `cf` (generation 1) *implements* the
  full language, including the parts `cf0` lacks; `cf`'s *source* uses only the
  seed subset. The two never contradict because a compiler that implements deep
  comptime need not itself be written with it.

## 2. The bootstrap ring and the evergreen line

C! versions with GenVer (see [[versioning.md]]): Glory.Enhancements.Nurture, where
a Glory of `0` is a pre-release **Glory hole** — precisely where a bootstrap
belongs.

- **`cf0` is `cf 0.x.x`** — the bootstrap ring. Pre-glory, not a real release: it
  compiles the seed subset and exists only to build the first evergreen compiler.
  Its source of truth is seed-subset C!; its committed seed is QBE IL (§3).
- **`cf` is `cf 1.x.x` onward** — the evergreen line. `cf 1.0.0` is the first
  Glory: the full language, its source disciplined to the seed subset, first
  compiled by `cf0`. From there the language is complete — `cf` implements the
  whole grammar, the full geometry catalog, table dispatch, deep comptime, and the
  observability invariant (§8) — and every later version bumps a GenVer axis per
  change (a breaking change raises Glory, a new feature Enhancements, a fix
  Nurture) while the source never leaves the seed subset (§1).

This is the same `cf` / `cf0` split the sibling specs already draw (see
[[order_of_compilation.md]] §9, [[geometry_lowering.md]] §6): `cf0` is `0.x`, `cf`
is `1.x+`.

**Self-hosting is a build-provenance property, orthogonal to the version.** To cut
a release of some version _V_ its source is compiled in stages:

- **stage 0** — `cf0`, built from the QBE IL seed by `qbe` + `cc`.
- **stage 1** — _V_'s source compiled by `cf0`: a full `cf`, but one whose own
  machine code was emitted by `cf0`'s backend (compare-chains inside the binary).
- **stage 2** — _V_'s source compiled by stage 1.
- **stage 3** — _V_'s source compiled by stage 2.

**The self-hosting proof is `stage 2 ≡ stage 3`, byte-identical** — both are _V_'s
source compiled by a *full* `cf`, so they emit identically. It is deliberately
**not** stage 1 ≡ stage 2: stage 1 was built by `cf0`, which emits compare-chains
where `cf` emits tables (§7), so the two diverge in the backend while agreeing in
behaviour. Every stage is the same *version* _V_ — the stages are build
provenance, not version increments — and the fixpoint is a whole-artifact compare,
needing none of the per-arc machinery `cf0` lacks (§8).

## 3. Seed medium, target, and linking

### The medium: QBE IL

The bootstrap seed is **QBE IL**, not C.

`cf` already emits QBE IL as its backend output, and the toolchain already vendors
`qbe` and shells out to `cc` to assemble and link (see [[order_of_compilation.md]],
Backend). Seeding in QBE IL therefore adds **no new trusted dependency**: the base
stays exactly `{qbe, cc}`, the same tools every later build already needs. Seeding
in C would drag a whole second language and its compiler into that base for one
generation and then discard it. QBE IL is the language `cf` targets, so
**the bootstrap dogfoods the real backend**.

The arrangement:

- **Source of truth is seed-subset C!.** `cf0` is a readable C! program. This is
  what makes the generous subset affordable — generics, closures, and sugar in
  `cf0` are *authored in C!*, never hand-written in raw IL.
- **The committed seed is generated QBE IL.** It is what `qbe` + `cc` build into
  the `cf0` binary.
- **The first copy is authored once.** With no `cf` in existence, the initial QBE
  IL is hand-authored (or hand-assisted) a single time and committed. Its
  provenance is that one act; it is not maintained by hand thereafter.
- **Maintenance is self-reproduction.** Editing `cf0`'s C! source and rebuilding
  re-emits the QBE IL, which is **byte-compared** against the committed seed — a
  plain textual `diff`, reusing the deterministic `qbe` → `cc` tail the pipeline
  already ends in. A drift is caught at the seed.

**The committed seed is the trust anchor.** Checking the QBE IL into the repo is
C!'s answer to Ken Thompson's *Reflections on Trusting Trust*: the only artifact
that compiles the compiler is built from **auditable, in-repo text** by tools
(`qbe`, `cc`) already trusted for every other build — never from an opaque,
previously-blessed compiler binary. Anyone can read the seed, rebuild it with
`qbe` + `cc`, and — because `cf0` re-emits it byte-for-byte from its C! source —
confirm the checked-in QBE IL is exactly what that source produces. There is no
un-inspectable step where a compiler could smuggle a backdoor into its successor;
the trust bottoms out in reviewable QBE IL plus the two vendored tools.

Because `cf0` drops the per-arc observability story (§8), it need **not** honour
the internal positional-numbering reproducibility that `cf` uses for arc
byte-compare (see [[order_of_compilation.md]], Names the compiler mints). The only
reproducibility `cf0` owes is the one that matters for the bootstrap: the final
committed QBE IL.

### The target: `darwin arm64` only

`cf0` targets exactly one triple — **`arm64-apple-darwin`**. This is not only where
the project runs; it is the *most fastidious* target on offer. Apple constrains
code signing, page permissions (no simultaneous write-and-execute), and what a
binary may do without a runtime far more tightly than Linux or bare metal, so a
bootstrap that satisfies darwin arm64 never has to be loosened later to reach it.
The single target is also licence to cut corners a portable compiler cannot: the
`asm` floor is one architecture's instructions — the arm64 `svc #0x80` syscalls
already shown in [[ebnf.md]], Assembly — and the comptime conditional imports that
pick a backing per target (§7) always resolve to the one branch. `cf0` still
*implements* `comptime_if`, because `cf`'s source uses it for portability; it just
only ever selects darwin/arm64. Multi-target codegen is an evergreen concern.

### Linking: freestanding by default

A C! program compiles to a **fully self-contained, freestanding binary** —
`-nostdlib`, no libc, no C runtime beneath it. It can afford to: the standard
library is written in C! and bottoms out at raw syscalls through `asm` functions
(see [[ebnf.md]], Assembly), so a finished binary needs nothing under it.
Freestanding is the **default**.

**The compiler is the one exception.** `cf0` and `cf` vendor `qbe`, which is C, so
the compiler binary itself **links the C runtime** — porting QBE to C! is not on
the bootstrap's critical path. The compiler is thus a *hosted* binary even though
every binary it emits is freestanding.

Freestanding and C interop are mutually exclusive, and this is the answer to the
obvious question: **a `-nostdlib` binary cannot link general C bindings.** C
libraries assume a C runtime — libc initialisation, TLS, `errno`, an allocator —
none of which a freestanding binary provides. To call **C externs** a program
therefore *omits* `-nostdlib` and links a C runtime, statically or dynamically,
paying the dependency for the interop. So there are two link modes: **freestanding**
(default, self-contained, no C) and **hosted** (opt-in, C externs available).

That two-mode policy, how externs are declared and linked, and the rest of the
compiler's command surface are the CLI's concern — the `--libc` flag is
[[cf_cli.md]] §4, the C-extern surface is deferred there (§8). They surface here
only because the bootstrap already straddles the line: the compiler is hosted, what
it emits is freestanding.

## 4. The feature boundary

`cf0` **accepts the full grammar** — it parses ordinary C! so that `cf`'s source
parses. The reduction is in what it *implements*, and it is small.

### Kept in full

The whole of [[ebnf.md]] is live in `cf0`, because `cf`'s source uses it:

- **Types** — pointers, references, bracket types (fixed array, tuple, dynamic
  array), named/generic types, type variables, function types.
- **Data modelling** — `data`, `type` (including named-tuple splat), `union` with
  payloads, record and aggregate literals, spreads, field puns, defaults.
- **Generics, with inference.** Full monomorphisation from both explicit
  (`f[Int](...)`) and inferred (`f(...)`) type arguments — `cf`'s source is not made
  to spell out every type argument.
- **Closures and higher-order functions.** Capturing function values lower to
  hidden parameters at specialization (the fake-closure model — see
  [[memory_model.md]], Closures); `map`/`fold` and friends work, so `cf`'s source
  reads functionally.
- **The full sugar suite.** String interpolation, `|>` pipe and partial
  application, compound assignment, record/aggregate spread, else-less `if`,
  `for`, `defer` in every form. All desugar in the `desugared` arc exactly as in
  `cf`.
- **Control flow** — `if`/`match`/`loop`/`for`, blocks-as-values, destructuring.
- **Modules** — imports, barrels/reexports, flatten-and-mangle, and **comptime
  conditional imports** for target selection (structurally required; §7).
- **The floor** — `intrinsic` declarations and `asm`-bodied functions, the
  bottom the standard library rests on.
- **The `!` allocation algebra**, checked both ways (§5).

### Degenerate

Two things `cf0` implements narrowly rather than fully — and both are already
pinned by the sibling specs:

| construct       | `cf`                                   | `cf0`                                 |
| --------------- | -------------------------------------- | ------------------------------------- |
| geometry        | full catalog (bump / free-list / rc / gc) | `page` + one `arena`, bump-only (§6) |
| `match` dispatch | synthesized function-pointer table    | compare-chain (§7)                    |

### Dropped

**Deep comptime** — user-level recursive comptime metaprogramming — is the one
feature `cf0` does not implement (§7). It is not a syntactic form to reject; it is
a class of comptime evaluation `cf0` simply does not perform beyond instantiation
and constant folding.

## 5. The pipeline, phase by phase

`cf0` runs the **same phase sequence** as `cf` (see [[order_of_compilation.md]],
The pipeline). What changes is narrow: the arcs no longer **stop** (§8), the memory
arc runs over one userland geometry (§6), and emit picks the compare-chain (§7).
Every gate runs in full — `cf0` is a real compiler and rejects ill-typed,
mis-effected, and lifetime-violating programs exactly as `cf` does.

| #   | phase              | `cf0` treatment                                            |
| --- | ------------------ | ---------------------------------------------------------- |
| 0–1 | Lex, Parse         | full — the whole grammar                                   |
| 2   | Resolve & flatten  | full; comptime conditional imports resolve to the single target (§3) |
| 3   | Typecheck & bind   | full gate                                                  |
| 4   | Desugar            | full — the entire sugar catalog                            |
| 5   | Effect & escape    | full gate — `!` both ways, escape classes                  |
| 6   | Specialize         | full monomorphisation; comptime = instantiation only (§7)  |
| 7   | Memory             | full placement, single geometry, trivial duplication (§6)  |
| 8   | Comptime fold      | full const-fold; no deep comptime (§7)                     |
| 9   | DCE                | full whole-program prune                                   |
| 10–12 | Emit, `qbe`, `cc` | full — `match` → compare-chain (§7); same vendored `qbe`   |

The **region-outlives gate** inside the memory arc still runs: a single userland
geometry does not mean a single node — nested `arena`s and rehomed pointers make
the lifetime check meaningful, so `cf0` performs it in full.

The one thing absent from the sequence is the **arc stop**: `cf0` does not emit
intermediate `.cf` (§8). The transformations still run in order; they just cannot
be observed.

## 6. The single manifold

`cf0`'s memory arc is the one [[geometry_lowering.md]] §6 already specifies in
full: the internal **`page`** geometry serving the root (`main` runs under it) and
exactly one **userland** geometry, **`arena`**. Both are bump family, and that is
what keeps the bootstrap lean. Read §6 for the detail; the consequences in brief:

- **Trivial duplication axis.** A `!` function never runs under `page` (the user
  cannot allocate in the root), and the userland set is a singleton, so every `!`
  function has exactly one memory instance — the `(fn × geometry)` product
  collapses.
- **No copying adoption.** `page` and `arena` are two geometries, but an arena is
  physically carved from its parent, so every `arena` → `page` claim is
  **copy-free wall-dissolution** — never the copy-and-register adoption a
  `gc`/`rc`/`heap` target would force.
- **The bump core only.** `on_free` and `on_store` are both no-ops in the bump
  family, so the finisher sweep leaves just `on_scope_enter`/`_exit`, `on_alloc`,
  `on_realloc`, `on_ret`/`on_alloc_ret`, and rehome-as-keep-alive.

What stays **identical** to `cf`: the nine-hook contract, the placement rules, the
in-flight / claim-once return protocol, and the `%node`/`%ret` calling convention.
`cf0` threads nodes, prunes node-free subtrees, and returns residue by pointer
exactly as `cf` does — it simply exercises `page` + one `arena` where `cf`
exercises the whole catalog.

## 7. `match` dispatch and comptime depth

### Compare-chain dispatch

`match` is held symbolic through every arc and lowered at **emit** (see
[[order_of_compilation.md]], The desugar catalog), so the choice of dispatch is
purely a backend decision — the arcs are byte-for-byte identical between `cf` and
`cf0`. `cf` synthesizes a **function-pointer table** (an array indexed by tag plus
an indirect call, which `cf` builds because QBE offers no `switch`, jump table, or
indirect jump). `cf0` takes the cheaper path QBE gives for free — a
**compare-chain**: a linear ladder of tag equality tests.

```
match n {
  Node.IntLit(v)     -> ...,
  Node.BinOp({ op }) -> ...,
  Nil                -> ...,
}
```

lowers, schematically, to

```
# cf0 emit — compare-chain (illustrative QBE shape)
  %t =w loadw %n.tag
  %is_intlit =w ceqw %t, 0
  jnz %is_intlit, @intlit, @next0
@next0:
  %is_binop =w ceqw %t, 1
  jnz %is_binop, @binop, @next1
@next1:
  jmp @nil            # exhaustive: last arm is the fall-through
```

Payload binding, exhaustiveness, and or-patterns are unchanged — only the
**dispatch** differs. The cost is `O(arms)` comparisons instead of `O(1)`, which is
immaterial for a bootstrap and vanishes at generation 1, where `cf` restores the
table.

### Comptime depth

`cf0`'s comptime does exactly three things, all of them structural to compilation:

- **Instantiation** (the `specialized` arc) — evaluate type arguments, comptime
  value parameters (`[n 'T]` sizes), and capture environments; drive the
  monomorphisation worklist. This is the comptime generics and fake closures rest
  on, so keeping it is what keeps those features in the subset.
- **Constant folding** (the `folded` arc) — reduce comptime-known expressions that
  survived into runtime shape, and inline non-deferred hook bodies to their literal
  operations.
- **Comptime conditional imports** — evaluate the module-level `if ... then ... else`
  against the target triple, so a name resolves to one backing per target. In
  `cf0` the triple is fixed (§3): every selection lands on darwin/arm64, though
  `cf`'s source still writes the portable form.

The **termination guard** (see [[order_of_compilation.md]], Specialize) is still
present — it bounds instantiation recursion and turns a runaway into a compile
error — but it is rarely stressed, because what is dropped is exactly the class
that would stress it:

**Deep comptime** — arbitrary recursive comptime functions that compute values or
types by general metaprogramming — is not implemented. `cf`'s source does not need
it to be idiomatic, so `cf0` need not carry it. `cf` (generation 1) implements it
for users; `cf`'s own source stays clear of it, per the §1 invariant.

## 8. No observability in `cf0`

The stop-and-emit story — every arc emitting valid `.cf` under a `cf-stage`
pragma, resumable by recompile-and-byte-compare (see [[order_of_compilation.md]],
Stopping, emitting, and resuming) — is a **generation-1 feature**. `cf0` is
**straight-through**: source → arcs (run, never checkpointed) → QBE IL → `qbe` →
`cc`. There is no `cf-stage` pragma, no resume, and — notably — **no formatter
dependency**, since nothing intermediate is emitted to canonicalise.

This is the one place `cf0` and `cf` **diverge architecturally** rather than merely
degenerately, and it is a deliberate trade: the observability invariant is the most
machinery-heavy part of the pipeline (the formatter, idempotent arcs, the
re-derive-from-source resume), and none of it is needed to *build* a working
compiler. It is needed to *explore* one, which is a user-facing luxury `cf`
restores at generation 1.

The bootstrap loses nothing by it. The reproducibility that the chain actually
depends on is the **whole-artifact** compare — the QBE IL seed (§3) and the
`gen₂ ≡ gen₃` fixpoint (§2) — neither of which uses the per-arc `.cf` machinery.

## 9. Graduation to `cf`

The evergreen line (`cf 1.x`) is where the language becomes whole. Everything
`cf0` runs degenerately or omits, `cf` restores — and the restoration is
**distributed across the sibling specs**, not owned here:

| restored in `cf` (1.x+)  | where it lives                                    |
| ------------------------ | ------------------------------------------------- |
| full geometry catalog    | [[geometry_lowering.md]] §5 (bump / free-list / rc / gc) |
| cross-geometry adoption, barriers, per-object reclaim | [[geometry_lowering.md]] §3–§5      |
| table `match` dispatch   | [[order_of_compilation.md]], Backend              |
| deep comptime            | the comptime spec (deferred)                      |
| observability & resume   | [[order_of_compilation.md]] §1, §6                |
| multi-target codegen     | [[cf_cli.md]] §5 — wasm + cross-linking deferred there |
| hosted C interop, link modes | [[cf_cli.md]] §4                              |

The pattern is the project's whole shape in miniature: each release **restores
capability while the source stays idiomatic**. `cf0` is the floor — generous by
design, so that `cf`'s source is written in real C! — and every release after it
extends the language its users write without ever having to go back and clean up
the compiler that compiles them.
