# C! Geometry Lowering

The interior of the Memory arc: the closed hook set a geometry implements, how
the compiler **places** those hooks into function bodies, and how the readable
`.cf` calls eventually become the emit-level `%node`/`%ret` calling convention.
It fills in the frame [[order_of_compilation.md]] leaves open (that arc "only
places the calls") and lands the semantics [[memory_model.md]] describes (the
manifold, the `!` algebra, zero-cost return) into actual code.

Status: design settled and **implemented** — the duplex-node arc landed this
whole contract in cf's own emit (the classification lives in `routing.cf`, the
routing and brackets in `emit.cf`, the floor in the emitted QBE/asm). The hook
set, its placement, the return protocol, and the calling convention below are
ratified and **pinned** — read this before touching the Memory arc rather than
re-deriving it. Concrete geometry bodies (§5) are reference implementations; the
collection *algorithms* inside `rc`/`gc` are their own subject.

Classification itself is not the new part — §2 has always driven placement
from the comptime escape class, and [[memory_model.md]] §8 always promised an
escaping allocation "placed on the caller's node to begin with." What the
duplex arc changed is **where the class acts**. Before, it was enacted at the
ESCAPE site: `on_ret` wrapped the *return expression*, exempting an
already-placed value from cleanup by bookkeeping (a drop-set edit), and the
boundary claim repaired placement after the fact — machinery whose soundness
hung on a no-allocation window, and whose implementation (an adopt-or-slide
claim) violated this spec's own "neither copies" clause. Now the same class is
enacted at the ALLOCATION site — **classification at birth**: a value that
escapes its frame (returned, yielded, or rehomed into caller-visible memory) is
born a **survivor**; provably-trapped scratch is born **residue**. The
separation is spatial, not bookkept. The bump family realizes it as a **duplex
node** (§5): two subnodes in one 96-byte header, survivors on the handle side,
residue on a bracketed side that frames and loops reset — no reclamation path
can reach a survivor even in principle. The polarity is fail-safe — an unknown
or unprovable site defaults to survivor, so a classification miss leaks until
node death and can never dangle. The classification is geometry-agnostic (it is
the memory model's own residue/survivor separation); the duplex layout is
merely the bump family's way of consuming it.

This spec spans two altitudes. §1–§3 (the hook contract, placement, and the
return protocol) are the **`.cf`-level** layer: what the Memory arc emits, still
readable C!, still `node_0`. §4 (the `%node`/`%ret` calling convention) is the
**emit-level** ABI the backend lowers that placement to, where `node_0` becomes
QBE `%node`. The
seam between them is the last `.cf` arc; everything before it is observable,
everything after is one-way.

## 1. The hook contract

A **geometry is a module** — there is no `geometry` keyword. It is any module
that exposes the closed hook set below plus the **node-lifecycle pair** that
brackets it: a constructor (`of`, `with_capacity`, …) that carves a child node,
and **`destroy`** that dissolves it back into its parent (§3, Wall dissolution).
The constructor and `destroy` are the node's birth and teardown; the nine hooks
below place and reclaim the *values* that live between them. `destroy` is
**user-placed** — the programmer bounds a child's lifetime with
`<arena> |> defer destroy` (or `defer <geom>::destroy(b)` on its own line), exactly
like any other `defer`. The compiler does not guess where teardown goes; it only
**verifies** the `destroy` is sound — that nothing the child holds escapes the scope
being closed (the same escape analysis behind `!`) — and rejects one that would
strand an escaping value. An un-`destroy`ed child simply lives until its parent
node's own teardown (node-bounded, never dangling). See [[memory_model.md]], The
`in` clause. Selection is structural:
`in arena` names the module, and the Memory arc splices its hooks into every
governed body. Because the contract is duck-typed, a user geometry and a standard
one are indistinguishable to the compiler — the same placement runs over both.

### The two intrinsic types

A geometry is never a runtime value, but the model still leans on two
**intrinsic** (compiler-provided) types, and a third opaque one:

- **`Geometry`** — the *comptime* type of a policy. `arena`, `gc`, `rc` are
  comptime values of type `Geometry`; `in g` takes a `Geometry`; a function may be
  parameterized over one. It has no runtime representation — geometries are pure
  comptime (see [[memory_model.md]], Geometries).
- **`MemoryNode`** — the *runtime* type of a node handle. This is what the hidden
  first parameter (`node_0`) is: the region state a `Geometry` instantiates. It is
  a real runtime value, threaded per the node-locality rule.
- **`Mark`** — the opaque token `on_scope_enter` returns and `on_scope_exit`
  consumes. Its representation is **geometry-private**: a bump-pointer snapshot for
  the bump family, a shadow-stack frame handle for `gc`, unit for `rc`. Only the
  minting geometry interprets it; marks nest LIFO with their scopes.

`Geometry` and `MemoryNode` are two sides of one thing — the comptime shape and
the runtime space — exactly as [[memory_model.md]] frames the geometry/node
duality.

### The nine hooks

The first column is the exact name the module must export; the compiler generates
calls to precisely these. The value hooks are generic over the aggregate type
`'T` (see [[ebnf.md]] for the `'T` convention).

| hook             | signature                              | role                                  |
| ---------------- | -------------------------------------- | ------------------------------------- |
| `on_scope_enter` | `(MemoryNode) -> Mark`                 | open a scope; return an opaque mark   |
| `on_scope_exit`  | `(MemoryNode, Mark)`                   | close a scope; bulk reclaim           |
| `on_alloc`       | `['T] (MemoryNode, 'T) -> 'T`          | place local residue (dies in frame)   |
| `on_realloc`     | `['T] (MemoryNode, 'T) -> 'T`          | grow an aggregate in place            |
| `on_ret`         | `['T] (MemoryNode, 'T) -> 'T`          | place an escaping value, at birth     |
| `on_alloc_ret`   | `['T] (MemoryNode, 'T) -> 'T`          | claim a returned value into the node  |
| `on_free`        | `['T] (MemoryNode, 'T)`                | reclaim one dead object               |
| `on_rehome`      | `['T, 'U] (MemoryNode, 'U, 'T) -> 'T`  | relocate residue into a target's node |
| `on_store`       | `['T, 'U] (MemoryNode, *'U, 'T)`       | perform a reference-field write       |

`on_rehome`'s second argument is the target aggregate; `on_store`'s is the `*T`
destination field. Both receive the *target's* `MemoryNode`, not the ambient one.

Every geometry implements all nine. A hook a policy does not need is a **pure
no-op** that returns its value argument unchanged (`on_free` for an arena,
`on_scope_exit` for `rc`). No-ops are not a special case — they are ordinary
bodies the finisher DCE later erases (§2, The no-op sweep). This is why one
placement scheme serves every policy: the *placement* is uniform, the *policy*
decides which placed calls survive.

### The laws

Placement is only sound because every geometry obeys these. They are checked
structurally by the compiler where noted, and are contract obligations on
hand-written geometries otherwise.

- **Bracket law.** Every `on_scope_enter` is matched by exactly one
  `on_scope_exit` on every exit path, with the mark it returned. Guaranteed by
  emitting the exit as a `defer` at scope top (§2, Scope brackets).
- **Identity law.** The value-returning hooks (`on_alloc`, `on_realloc`,
  `on_ret`, `on_alloc_ret`, `on_rehome`) return the *same logical value* of the
  *same type*. They may relocate its bytes; they may never change what it is.
  This is what lets a hook transparently wrap an expression —
  `arena.on_alloc(node_0, [1, 2, 3])` is still a `[3 Iarch]`.
- **Closure law.** `on_ret`'s argument is the **entire reachable closure** of the
  return value, not just its head — anything the return points at escapes with
  it. Under classification at birth the law is **discharged statically**: a value
  feeding an escape is itself classified escaping, so the whole closure is born
  survivor-side and no runtime closure walk ever happens. This mirrors the escape
  analysis that computes `!` (see [[memory_model.md]], Zero-cost return) and is
  what the drop-set is defined against.
- **Claim-once law.** Every value that passes through `on_ret` is claimed by
  **exactly one** `on_alloc_ret`. This is structural — a return has one
  destination — and it is the invariant the return protocol (§3) rests on.
- **Node-locality law.** `on_realloc` and `on_rehome` receive the *target
  aggregate's* node, threaded at the call site — **not** necessarily the ambient
  node. Growing or rehoming a foreign aggregate lands in *its* region, per
  [[memory_model.md]], Escaping.

### Reserved for concurrency: `on_safepoint` and `on_load`

Two further hooks are **reserved, not implemented** — both are concurrency
machinery the sequential language cannot exercise:

- **`on_safepoint(MemoryNode)`** — a mutator poll point at loop back-edges and
  call returns, so a running thread yields to a collector promptly. In a
  *sequential* program it earns nothing: a stop-the-world collector need only run
  when it is **out of space**, which is exactly `on_alloc`, so `gc`/`rc` trigger
  collection *inside their own `on_alloc`* and no structural poll is placed. RC is
  deterministic through `on_free`/`on_store` for the same reason.
- **`on_load(MemoryNode, *'T)`** — a read barrier, needed only by concurrent
  relocating collectors (Baker-style, ZGC-style colored pointers).

The nine-hook set is closed *for the current language*; these two are the named
holes the concurrency work will fill. Geometries do not implement them and the
Memory arc never places them.

## 2. Placement

The Memory arc materialize substep (see [[order_of_compilation.md]], Memory)
walks each specialized body and splices hook calls. Placement is fully
determined by two things already computed upstream: the **escape class** each
residue site carries out of the effect gate, and the **node identity** the decide
substep assigned. Nothing here re-analyzes; it only renders.

### Escape class → hook

The effect gate tags every aggregate site with one of four fates. Each maps to a
fixed placement:

| escape class       | site                        | placement                                   |
| ------------------ | --------------------------- | ------------------------------------------- |
| _local_            | `let/const x = <agg>`       | wrap RHS in `on_alloc(node, …)`             |
| _grow-in-place_    | `xs = [...xs, e]`           | wrap RHS in `on_realloc(node_xs, …)`        |
| _escapes-via-return_ | `return <agg>`            | the CONSTRUCTION is `on_ret`-placed at birth (survivor side); `on_alloc_ret(node, …)` stays wrapping the call in the caller (the bump fold is the identity) |
| _rehomed_          | store `<agg>` into a foreign aggregate | the stored value is classified escaping and `on_ret`-placed at birth; `on_rehome(node_target, target, …)` stays placed at the store (bump fold: no-op) |

The node handle threaded into each hook follows the node-locality law: `on_alloc`
takes the ambient `node`, but `on_realloc`/`on_rehome` take the *target's* node,
which the decide substep already resolved to a concrete `node_<i>`.

Birth routing generalizes the third and fourth rows beyond named sites: an
**anonymous construction in argument position** is `on_alloc`-placed (residue)
when the callee's summary proves the argument is neither stored beyond the
callee's frame nor aliased by its return — it is statement scratch, dead once
the call returns. A `write_str(fd, "…${x}…")` interpolation is the canonical
shape. Everything the summaries cannot prove defaults to survivor.

### Scope brackets

Every scope that contains residue opens with a mark binding and schedules its
exit immediately, so the bracket law holds on all paths:

```
const mark_0 = arena.on_scope_enter(node_0)
defer arena.on_scope_exit(node_0, mark_0)
```

The `defer` is the ordinary language construct (see [[ebnf.md]]) — the Memory arc
does not invent placement; it leans on `defer`'s LIFO emit (see
[[order_of_compilation.md]], Backend) so nested scopes tear down inner-first for
free. A scope with no residue gets no bracket.

### The drop-set: `on_free` and `on_scope_exit` are two hooks

This is the subtle point, and the reason both hooks exist. The **drop-set** of a
scope is *its own residue minus everything reachable from a return* — the objects
that provably die when the scope closes. It is **comptime-known** (the closure law
gives its complement), so the Memory arc does not emit a runtime loop over it. It
**unrolls** the set into one deferred `on_free` per member, in reverse birth
order:

```
let a = arena.on_alloc(node_0, ...)   # birth
let b = arena.on_alloc(node_0, ...)
defer arena.on_free(node_0, b)        # LIFO: b freed before a
defer arena.on_free(node_0, a)
# ... on_scope_exit(node_0, mark_0) was deferred first, so it runs last
```

So `on_free` and `on_scope_exit` are **independent hooks with independent
semantics**, and a geometry splits the work between them however its policy
dictates. The two standard shapes are duals:

- **Bulk reclaimers** (arena, fixed_buffer, page) do everything in
  `on_scope_exit` — one reset-to-mark frees the whole scope at once — and make
  `on_free` a **no-op**. The unrolled `on_free` calls vanish in the sweep.
- **Per-object reclaimers** (`rc`) do everything in `on_free` — each dead object
  drops its count and, at zero, returns its slot — and make `on_scope_exit` a
  **no-op**. (This is the *runtime*-lifetime dual; every *static* lifetime, even
  the mixed return-plus-dead-residue case, is reclaimed in bulk by the arena — see
  The free-list family is subsumed, below.)

Same placement, opposite geometry. Neither hook is defined in terms of the other,
and the Memory arc need not know which policy it is splicing into: it always emits
both, and the sweep removes whichever the policy left empty.

### The write barrier: `on_store`

`on_store` is not tied to a residue site; it is placed by structural rule at
every write of a **reference field** through a `*T` into a `let` aggregate — only
reference writes, never scalar-field writes, can carry a barrier, and only a `let`
aggregate is writable at all. The hook **performs the write itself**:

```
xs.head = v            # source
xs = on_store(node_xs, &xs.head, v)   # placed (node is the target's, per node-locality)
```

Because the store of `new` happens *inside* the hook, at hook entry the
destination still holds the **old** reference — so no separate `old` argument is
needed. A counting/tracing policy reads that old value out of `dst` before
overwriting: `rc` increments `new` and decrements old; a generational `gc` marks
the card / records the SATB snapshot, then writes. A non-barrier geometry (arena,
pool, …) just does the write, so its `on_store` is **not a no-op** — it **folds to
a plain store** at the `folded` arc, exactly as `on_alloc` folds to a bump. Only
genuine no-ops are swept (below); `on_store` always carries the store. (The
non-barrier geometries are `arena`, `fixed_buffer`, `page`.)

Scalar-field writes need no barrier and no hook — they emit as plain stores.

Collection itself is not placed. A `gc`/`rc` policy triggers a cycle from *inside
its own `on_alloc`* (out-of-space / threshold), which is why the sequential
language needs no `on_safepoint` (§1, Reserved for concurrency).

### The no-op sweep

After placement, each arc may run a **finisher DCE** (see
[[order_of_compilation.md]], DCE — the per-step kind, not the whole-program pass).
Here it deletes every hook call whose body a policy left as a **pure no-op**: an
arena's `on_free`, an `rc`'s `on_scope_exit`. It does **not** touch hooks that fold
to real work — an arena's `on_store` folds to a plain store, not nothing, so it
stays (and folds later). This is a local sweep over the just-placed calls, and it
is what makes the observable `memory` `.cf` read cleanly — an arena body shows
resets, not a litter of dead `on_free`s.

## 3. The return protocol

Escape is **placement**. A value that must outlive the frame that built it is
born where its destination can keep it — classified at the site, placed at
birth, formally **in-flight** until its one destination **claims** it. C! has
exactly two escape shapes, and they are the *same* mechanism aimed at two
destinations:

- **Return** — claimed by the caller's binding: `on_ret` (callee) / `on_alloc_ret`
  (caller).
- **Rehome** — claimed by an ancestor aggregate: `on_rehome`.

Both say "hand this up, and don't let my cleanup take it."

### In-flight and claim-once

`on_ret(node, v)` runs in the **callee** on the **callee's** node. Its meaning is
**spatial exclusion at birth**: `v` and its whole reachable closure (the closure
law) are constructed on the node's **survivor side**, which no residue bracket
ever touches — so whatever `on_scope_exit` / `on_free` reclaim, they cannot reach
it. There is no cloaking bookkeeping and no drop-set edit at runtime; the
exclusion was decided when the bytes were placed.

`on_alloc_ret(node, v)` runs in the **caller** on the **caller's** node, wrapping
the call, and **claims** the returned value into the caller.

The **claim-once law** holds structurally: a return has exactly one destination,
so every in-flight value is claimed by exactly one `on_alloc_ret` — never
double-claimed, never leaked. Under birth placement the old no-allocation claim
**window dissolves**: the value already sits in storage that outlives the
callee's brackets, so nothing can free or move it between the callee's exit and
the caller's claim — the fragile "no alloc between reset and claim" discipline
an earlier stage leaned on is gone by construction. (Collection still triggers
only inside `on_alloc` — §1, Reserved for concurrency.)

### Compact-on-claim, and why the boundary is copy-free

What the claim *costs* is the geometry's business; the boundary itself never
charges a copy (see [[memory_model.md]], Zero-cost return). The claim site is the
one place that knows the value's final home, so it is where a relocating policy
acts:

- **Bump family** (`page`, `arena`, `fixed_buffer`) — **copy-free**. `on_alloc_ret`
  folds to the **identity** (and the sweep erases it): a returned value was born
  on the survivor side of a node that outlives the call, so there is nothing to
  claim — same address, returned pointers stay valid. When a carved child node
  tears down, its walls dissolve into the parent it was carved from (event 2).
  Neither copies — the clause an intermediate stage's slide-down claim violated
  is restored by construction. The hook nonetheless **stays placed** at every
  claiming site: the identity is the *bump family's* fold, and a compacting `gc`
  keeps its only compaction site here.
- **Compacting `gc`** — **compact-on-claim**: the collector relocates the value
  into compact storage *at the claim*, rather than deferring to a later poll. A
  copy, but the collector's own policy, not a boundary tax.

So `.cf` reads `on_alloc_ret(node_0, f(...))` uniformly, and each geometry folds
it to its own claim — a bump adjust, a compaction, an RC adopt.

### Wall dissolution: `destroy`

The dual of construction, and event 2 above. A carved child `b = <geom>::of(n)`
opens a wall inside the parent it is carved from; **`<geom>::destroy(b)`** dissolves
that wall — the explicit child-node teardown named in the arena surface
(`fixed_arena::destroy`, `growing_arena::destroy`). It is the node-level counterpart
of `on_scope_exit`: where `on_scope_exit` rewinds a *frame's* residue mark, `destroy`
rewinds the *child's whole extent* back out of the parent — header, blocks, and for an
elastic child every pulled chunk — reclaiming the subtree in one operation.

`destroy` **preserves the escaping closure while reclaiming the dead block** — that is
its whole responsibility, discharged by each geometry in its own coin. For the **bump
family** the escaping closure was born survivor-side (`on_ret` at birth), physically
above the reclaimed region, so preservation is free and `destroy` is a single cursor
rewind to the carve mark; a compacting geometry relocates or adjusts counts as its
policy requires. Either way the boundary stays copy-free for what survives — the same
guarantee wall dissolution has always carried, now named and placed.

`destroy` is **user-placed**, an explicit teardown the programmer writes — the Memory arc
does *not* infer it. `const b = <geom>::of(n) |> defer destroy` carves the child and
schedules its dissolution at the binding's scope exit, LIFO with the frame's other
deferred teardowns (order_of_compilation, Backend); `defer <geom>::destroy(b)` on its own
line is the same. What the compiler contributes is **verification, not placement**: a
`destroy` is sound only if nothing the child holds escapes the scope it closes (rewinding
the parent past the child would strand an escaping value), and that is exactly the
closure law that computes `!` — so the compiler **rejects** an unsound `destroy` and
otherwise trusts the programmer's scoping. A child left un-`destroy`ed is not reclaimed
early; it lives until its own parent node tears down (node-bounded, never dangling) — the
arena discipline is the programmer's to state, like `free` in C but scope-checked.

Because a child is reclaimed by its *own* `destroy` at the position the programmer chose,
never by whichever ambient bracket happens to enclose it, **children are carved uniformly
from the parent's *survivor* side**. This is the key simplification the earlier
residue-side carve worked around: the residue carve and its interlocks (the carve
interlock, the pull-vs-bracket interlock) existed only to let the compiler *auto-reclaim*
a scratch child by borrowing the ambient's *residue* bracket — and once teardown is the
programmer's explicit `destroy`, that inference and its borrowing are unnecessary. With
every parent draw on the survivor side, `carve`/`pull`'s draw from the parent lowers to
the parent's own static `reserve_ret__<parent>` — the parent geometry is a comptime fact
of the construction ambient — with no runtime kind dispatch and no `cf_alloc`.

### Rehome is return aimed at an ancestor

`on_rehome(node_target, target, v)` is the same escape, claimed by `target`
instead of a return slot — a rehomed value is classified escaping and born
survivor-side exactly as a returned one is; the two differ only in destination. The destination is a specific **comptime-known ancestor**: the pointer
to `target` carried its node identity down the call chain, and the call site
threads the matching handle in (memory_model, Escaping) — never a runtime walk.
Because the relationship between the ambient node and the target node is therefore
settled at comptime, rehome resolves to one of three cases:

| condition (all comptime-known)               | `on_rehome` becomes                                            |
| -------------------------------------------- | ------------------------------------------------------------- |
| `v`'s node already outlives `target`'s node  | **no-op** — `v` survives regardless; just wire the reference  |
| same geometry lineage, `v` below `target`    | **no-op** — `v` was classified escaping (a rehomed value is an escape) and born survivor-side, so it already outlives every bracket between it and the target; just wire the reference. The target handle names *how far* only for the classification, and is unused at runtime. (A future geometry that reclaims survivors early would reactivate this row as real work.) |
| target geometry ≠ ambient geometry           | **relocation** — the target adopts `v` (below)                |

So for the whole intra-geometry world, rehome is exactly the "clean-up-protect"
it looks like, and the target `MemoryNode` is dead weight in the emitted code. It
earns its keep only in the third row.

### Cross-geometry adoption

The third row is the one true *relocation*, and **return and rehome share it**.
When the claiming node's geometry differs from the value's ambient geometry, the
target **adopts** `v`: `target_geom.on_alloc(target_node, v)` produces a copy
owned and registered by the target's policy — a `gc` traces it, an `rc` gives it a
count, a `heap` makes it individually freeable — and the original ambient copy is
**left to die normally**, falling into the ambient drop-set like any local
residue. Adoption is precisely why wall dissolution cannot carry an arena value
into a GC heap: the collector must *register* what it will later trace, which only
its own `on_alloc` does. This is the same event whether the boundary was reached
by `return` (a cross-geometry `on_alloc_ret`) or by `push` (a cross-geometry
`on_rehome`).

### Rehome and store at one site

When the rehomed value is written into a **reference field** of a barriered
(`gc`/`rc`) target, both hooks fire and compose — rehome adopts the residence,
the store barriers the pointer write:

```
xs.head = on_store(node_xs, &xs.head, on_rehome(node_xs, xs, p))
```

Inner to outer: `on_rehome` lands `p` in `xs`'s node (adopt, or degenerate per the
table); `on_store` writes the reference and runs the target's barrier. For an
arena target both degenerate — `on_rehome` to keep-alive/no-op, `on_store` to a
plain store — and the whole line folds back to `xs.head = p`.

## 4. The `%node`/`%ret` calling convention

This is the emit-level seam — past the `pruned` arc, where QBE sigils are applied
and the `.cf` story ends (see [[order_of_compilation.md]], Backend). Everything
above was readable `.cf` with `node_0` bindings; here `node_0` becomes QBE
`%node_0`, and the region machinery settles into an actual ABI.

### `%node`: the ambient node is argument zero

Every node-carrying function takes the ambient `MemoryNode` as a **hidden
argument in position 0**, ahead of the user parameters. In `.cf` it is the first
parameter (`node_0`); at emit it is `%node_0`, a pointer-sized handle, and QBE's
platform ABI assigns it a register like any leading argument.

The handle is a **pointer to shared, mutable node state** — the duplex subnode
pair for the bump family, a heap/collector descriptor otherwise. It is threaded
*by pointer* precisely so every frame and every callee running under the same
node observes the same state: this is what makes two `in arena` calls share one
node (see [[memory_model.md]], The `in` clause). To the calling convention the
handle is **opaque** — only the inlined hooks know the state's layout; the ABI
just moves a word. In particular the duplex split is **hook-private**: the
handle IS the survivor subnode (offset 0 keeps its pre-duplex meaning), and the
residue subnode's `+48` derivation appears only inside floor helpers and folded
hook bodies (in the emitted code, a bracketing frame's single
`%res_0 = %node_0 + 48` prologue line — a folded `on_scope_enter`). No other
code may learn the offsets; a `gc`/`rc` node is free to have a completely
different interior.

Locally minted nodes (`node_1`, `node_2` from a nested `fixed_arena::of`) are
ordinary local handles. A call fills the callee's slot 0 with whichever node
identity the decide substep assigned it — the ambient node by default, or the
*target's* node for a `!` call that grows or rehomes into a foreign aggregate
(the node-locality law). Which handle fills slot 0 is thus a comptime fact at
every call site.

### Node-free pruning: no argument zero at all

A call proven **node-free** carries no `%node`. The criterion is **body-driven,
not color-driven** — an earlier draft said "colorless, no `!` argument, scalar
return", which is wrong in both directions: a colorless constructor
(`-> P({…})`) or a rehoming colorless function (`g.inner = P({…})` through a
`*T` param) still allocates, while a frame holding only `$`-stack aggregates
does not. A function keeps `%node_0` iff it can ever NEED a node:

- it is `!` (every bang frame brackets its residue subnode);
- its body **allocates** — a construction outside `$`-stack storage, a string
  literal or string-pattern match (both materialize at runtime), a `Str(…)`
  seal, or an arena/buffer constructor;
- it makes an **indirect** call (the target is unknown; the site passes a node
  uniformly), or is **address-taken** (an indirect site targets it);
- it makes an un-grafted direct call — including a value-const **auto-call** —
  to a function whose own signature carries a node (an `in b` call threads the
  geom binding instead and costs the caller nothing);
- it is `main` (the floor enters it with the page).

The bits close over the call graph like the `!` fixpoint itself, and everything
is a **comptime fact** — never a runtime decision. Everything else prunes: its
emitted signature is just its user parameters, and callers pass no node. The
consequence is the one [[memory_model.md]] promises: a pure-compute subtree
emits the *exact same ABI it would have without C!* — no hidden parameter, no
threading, nothing to pay.

### `%ret`: the claimed return

- **Scalars** return in the natural QBE return, colorless and untouched — a
  node-free callee's return is an ordinary value.
- **Residue** returns as a **pointer** — `%ret`, a QBE `l` — to the in-flight
  aggregate. Never a by-value aggregate copy and never an `sret` out-parameter:
  the value lives in a region, not on the caller's stack, so what crosses the
  boundary is its address.

The callee's `on_ret` kept that aggregate out of its drop-set (§3, In-flight and
claim-once); the caller's
`on_alloc_ret` — already inlined to its literal claim at the `folded` arc — reads
`%ret` and either adopts the pointer as-is (bump wall-dissolve: same address, no
copy) or copies through it (compaction, or cross-geometry adopt). Durability
across a node teardown is the geometry's guarantee, not the ABI's: `%ret` is only
a pointer, and the claim is what makes it outlive the boundary.

### The seam, and the C boundary

Sigils are applied here and nowhere earlier: `node_0` → `%node_0`, `mark_0` →
`%mark_0`. The numbering stays **positional**, so emit is byte-identical on
re-run, which the resume byte-compare depends on (see [[order_of_compilation.md]],
Names the compiler mints). By this point the non-deferred hooks are already
literal operations; the deferred ones (`on_scope_exit`, and any user `defer`)
are still calls, and are placed into QBE **epilogue blocks** on the exit paths.

At a **C boundary** the convention inverts: a `pub` function exported under the
C ABI **drops `%node`** and faces the C world node-free, rooted under a pinned
geometry (see [[order_of_compilation.md]], The pipeline, and the C-ABI note
there). The `%node`/`%ret` convention is entirely internal to C!-to-C! calls.

## 5. Reference geometries

A geometry is a module exposing the nine hooks plus the node-lifecycle pair
(a constructor and `destroy`). These sit at
the **floor of the allocation algebra**: a hook body takes the `MemoryNode` state
by pointer and works in raw memory (`raw::*`, the privileged `std::mem::raw`
intrinsics — page growth, pointer bumps, byte copies), so a hook is itself
**neither `!`-colored nor node-threaded**.
Otherwise `on_alloc` would need a geometry to allocate, and the regress never
bottoms out. Each geometry reads the opaque `MemoryNode` as its own private state
layout.

The bodies below are **reference implementations** — the essential operation of
each hook, not a line-complete allocator. The collection *algorithms* inside `rc`
and `gc` (mark-sweep vs copying vs generational) are their own subject; the hooks
only expose the trigger points.

### The bump family: `page`, `fixed_arena`, `growing_arena`, `fixed_buffer`

One shared core: the node is a **duplex** — two bump subnodes in one 96-byte
header. The **survivor** subnode is the handle itself (offset 0, the pre-duplex
layout unchanged — fail-safe polarity: an unrouted allocation lands here and
merely leaks). The **residue** subnode sits behind it; frames and loops bracket
it. Allocation is a pointer bump on the classified side; reclamation is a
bracket reset of the residue side; `Mark` is the residue subnode's saved
**triple** `{top, committed, limit}` — a top-only mark cannot survive an elastic
pull, which relocates all three fields.

```
type BumpSub  = { top: RawPtr, committed: RawPtr, limit: RawPtr,
                  parent: MemoryNode, kind: Iarch, chunk: Iarch }
type BumpNode = { survivor: BumpSub, residue: BumpSub }   # the handle points at `survivor`

const on_scope_enter = (MemoryNode node) -> triple_of(node.residue)   # Mark = the triple now
const on_scope_exit  = (MemoryNode node, Mark mark) -> {
  # limit unchanged since the mark → restore top alone (the common case; committed
  # stays advanced so the page never re-commits). Limit MOVED → an elastic pull
  # relocated the subnode's block since the mark: the reset is a NO-OP — restoring
  # into the old block would park the cursor at its wall and re-pull a fresh chunk
  # on every subsequent frame (pull churn). Skipping leaks the marked interval
  # once per pull, reclaimed at node teardown; it can never dangle.
  if node.residue.limit == mark.limit then node.residue.top = mark.top
}

const on_alloc = ['T] (MemoryNode node, 'T v) -> {         # residue side shown; the
  let p = bump(side_of(v), raw::size_of(v))                # survivor side is the same
  raw::place(p, v)                                         # bump on `node.survivor`
  return p                                                 # same 'T (identity law)
}
```

**Scope granularity.** Every `!` frame brackets its residue triple. Where the
classification proves **nothing escapes a scope at all** — a frame with no
writable `*T` parameter, no capture, and a scalar return; or a loop iteration
with no return, no outer store, no `&`, and no rehoming callee — the scope
brackets the **survivor triple too**, so dropped callee returns, carved child
nodes, and every survivor born inside die with it. These survivor-scope marks
are pure **optimization gates**: failing one leaks until node death, never
dangles. The residue-side loop gate is softer — only an aggregate stored into an
outer target disqualifies it (such a value is residue that outlives the
iteration); returns, `&` arguments and rehoming callees are survivor-side
matters and no longer block per-iteration reclamation.

**Two-ended fixed blocks.** A bounded node (`fixed_arena::of(n)`, `fixed_buffer::of(buf)`,
the bare root) lays both subnodes over **one block**: survivors bump up from the
base, residue bumps down from the tail, and the cursors meeting is the
capacity trap — `of(n)` still means `n` bytes **total**. The residue side
maintains the survivor side's `limit` (= the residue cursor) on each
allocation, so the survivor's hot path stays a plain load-and-compare. The
sharing is sound precisely because a fixed survivor can never leave the block.

**Elastic nodes share nothing.** An elastic arena's survivor owns the whole
initial block and pulls chunks from the parent on overflow; its residue side is
a **lazy private chunk chain** — an empty subnode whose first allocation pulls
its own chunk. There is deliberately no two-ended sharing here: the maintained
boundary link breaks the moment the survivor pulls away to a fresh block (a
still-linked residue would clobber the survivor's limit with stale addresses).
Residue-free workloads never pay for the lane.

**The pin.** Both subnodes' `parent` points at the parent node's **survivor**
side, and headers/blocks are carved from it — a child's storage must never sit
in a region a parent-frame bracket can reset. The one licensed exception is the
**carve interlock** (memory_model §8): a child whose handle provably dies with
the carving frame or iteration is carved from — and, for an elastic child, has
its pulls parented on — the frame's **residue** side, precisely because the
brackets that reset that region are the ones that outlive the child by
construction (direct-value carve; no handle use crossing a marked loop's
back-edge; elastic additionally geom-only uses ordered against every loop
bracket). Every interlock gate fails toward the survivor pin.

The rest of the family's hooks are uniform:

| hook           | bump-family body                                                      |
| -------------- | -------------------------------------------------------------------- |
| `on_realloc`   | extend in place if `v` is the top-most block, else bump a fresh copy |
| `on_ret`       | birth placement — the construction bumps the SURVIVOR side (§3)       |
| `on_alloc_ret` | **identity** (swept) — the return already lives where it must (§3)    |
| `on_free`      | **no-op** — a bump never frees per object (swept — §2)                |
| `on_rehome`    | **no-op** — a rehomed value was born survivor-side (§3)               |
| `on_store`     | **no-op** — folds to a plain store (no barrier)                      |

The three members differ only in **where the buffer comes from** and **what
"full" means**:

- **`page`** — the root-adjacent geometry `main` runs under. Backed directly by OS
  pages; `on_alloc` commits more pages (`raw::grow`) when the cursor meets the
  limit, so it is the one **unbounded** bump. It has no constructor — the root node
  is provided to `main` as argument zero (§4). `on_scope_exit` at program scope
  unmaps.
- **`fixed_arena`** — `of(n)` pulls one `n`-byte block from the parent node
  (`parent.on_alloc`), so it is **bounded** by `n` and laid two-ended (above);
  frames rewind within it and the whole node dissolves into its parent at teardown
  (memory_model, Zero-cost return).
- **`growing_arena`** — `of(n)` starts from one `n`-byte block but its survivor
  **pulls fresh chunks** from the parent on overflow (the elastic lane above), so
  it grows within its parent; same scope-rewind and teardown. Splitting the fixed
  and growing shapes into two modules is what keeps each geometry's reserves
  monomorphic — neither tests a runtime fixed-vs-elastic kind at an allocation.
- **`fixed_buffer`** — `of(buf)` wraps a **caller-provided** array (stack or
  static); creating the node allocates nothing at all — the duplex header lays
  over the buffer's own front and the remainder is one two-ended block, so
  survivors live **in the buffer itself** (a static-backed escape stays valid
  for the buffer's whole life). Bounded by the buffer; the two-ended meet is a
  hard trap. The zero-dependency embedded allocator — it differs from `fixed_arena`
  only in the buffer's provenance.

### The free-list family is subsumed

An earlier draft catalogued a free-list family — `pool`, `slab`, `heap` — whose
node owns a **free-list**: `on_alloc` pops a block, `on_free` pushes it back,
reclamation per-object. That family does **not survive** the `on_ret` /
`on_alloc_ret` design, and is dropped.

A free-list's only edge over a bump is freeing an object *before* its scope ends
and reusing the slot — which matters exactly when a scope both keeps some value
and drops others (the mixed case). But that is precisely what `on_ret` /
`on_alloc_ret` hand to the **arena**: the return's closure is cloaked out of the
drop-set (`on_ret`) and placed on the **caller's** node (`on_alloc_ret`), so the
frame holds only dead scratch, and `on_scope_exit` rewinds it **in place** — the
mixed keep-and-drop case an older draft leaked now reclaims in bulk, at the frame.
For **every statically-placeable lifetime**, the smart arena already reclaims it,
at frame exit, with no free-list. Same-size churn (`pool`) and size-classed churn
(`slab`) are just frame residue; they buy nothing over the arena.

What a free-list is genuinely for is the **complement**: lifetimes the compiler
**cannot** place statically — individual objects freed at points fixed only at
runtime (shared ownership, dynamic or cyclic graphs). Those are exactly `rc` and
`gc`, and the free-list survives **inside them** as their reclamation substrate
(`rc`'s slot-return when a count hits zero; a `gc` sweep list). It is not a
standalone static geometry.

- **`pool`, `slab`** — **dropped**. Subsumed by the smart arena.
- **`heap`** — heterogeneous, individually-freed, long-lived data is the
  *runtime*-lifetime case, i.e. the substrate under `rc` / `gc` (size-classed
  free-lists + coalescing as an implementation detail of those). Not a distinct
  static geometry.

The line is sharp: **static lifetime → arena** (bulk, `on_scope_exit`); **runtime
lifetime → `rc` / `gc`** (per-object / tracing, over a free-list). Nothing sits
between them.

### `rc` — reference counting

Per-object like the free-list family, but every object carries a **count header**,
and `on_store` is where the real work lives:

```
const on_store = ['T, 'U] (MemoryNode node, *'U dst, 'T new) -> {
  inc(new)                                                     # retain the incoming referent
  let old = deref(dst)                                         # dst still holds old
  raw::store(raw::addr(dst), new)
  dec(old)                                                     # release the outgoing one
}
const on_free = ['T] (MemoryNode node, 'T v) -> {
  if dec(v) == 0 then {                                        # last reference
    drop_referents(v)                                          # recursively dec what v points at
    free_list_push(node, raw::addr(v), raw::size_of(v))
  }
}
```

`on_alloc` allocates (as `heap`) with count `1`; `on_ret`/`on_alloc_ret` **move**
ownership without touching counts; `on_scope_exit` is a no-op. RC reclaims
deterministically, so it needs no `on_safepoint`. **Cycles leak, by contract**:
`rc` is for *acyclic* shared ownership. The three answers to a cycle are all
elsewhere — a cyclic graph with a common lifetime belongs in a **region** (free
the whole tangle at once, zero per-object cost); a general object graph belongs in
**`gc`**; and a cycle-collecting RC (synchronous Bacon–Rajan, which C!'s comptime
layout knowledge would make cheap to generate) is a possible geometry, left
unspecified here — the catalog is representative, not exhaustive.

### `gc` — tracing collection

The one family that uses `on_scope_enter`'s `Mark` for **root discovery** and
triggers collection from within `on_alloc`:

```
const on_scope_enter = (MemoryNode node) -> shadow_push(node)  # Mark = a root frame
const on_scope_exit  = (MemoryNode node, Mark mark) -> shadow_pop(node, mark)

const on_alloc = ['T] (MemoryNode node, 'T v) -> {
  if heap_pressure(node) then collect(node)                    # scan roots via shadow frames; no safepoint
  let p = gc_bump(node, raw::size_of(v))
  raw::place(p, v)
  return p
}
const on_store = ['T, 'U] (MemoryNode node, *'U dst, 'T new) -> {
  remember(node, dst)                                          # card-mark / SATB before the write
  raw::store(raw::addr(dst), new)
}
```

`on_free` is a no-op (the collector reclaims). `on_ret`/`on_alloc_ret` keep the
return closure as a root until claimed, and a copying collector may **compact it
at the claim** (§3). `on_rehome` from a foreign geometry **adopts** `v` into the
GC (register + trace), per the cross-geometry rule.

### Known extensions

The same nine-hook contract admits more, left to the std geometry catalog and to
user space: **`buddy`** (power-of-two splitting, a natural suballocator under
`page`), **`ring`** (circular reuse for bounded streaming), **`tlsf`** (two-level
segregated fit, O(1) real-time). None needs a new hook.

### Summary

Where each family does its work — the shape of the whole catalog in one view:

| geometry       | buffer from      | reclaim locus            | barrier (`on_store`) |
| -------------- | ---------------- | ------------------------ | -------------------- |
| `page`         | OS pages         | `on_scope_exit` (unmap)  | none                 |
| `arena`        | parent block     | `on_scope_exit` (rewind) | none                 |
| `fixed_buffer` | caller's array   | `on_scope_exit` (rewind) | none                 |
| `rc`           | parent block     | `on_free` (count → 0)    | inc/dec              |
| `gc`           | parent block     | `on_alloc` (collect)     | card / SATB          |

## 6. `cf0` degeneracy

The bootstrap compiler (`cf0`) runs this exact placement, but over a **minimal
two-geometry manifold** rather than the full set (see [[order_of_compilation.md]],
`cf` vs `cf0`): the internal **`page`** geometry that serves the root — `main`
runs under it, exactly as in `cf` — and one **userland** geometry, **`arena`**.
`cf` keeps the same `page` and adds the rest of the userland catalog on top.
(`page` is not a userland *choice* but the mandatory floor that maps the program's
memory from the OS; the "single geometry" of the order-of-compilation spec is this
single *userland* geometry.)

**What collapses.** Both `page` and `arena` are **bump family**, and that is what
keeps `cf0` lean:

- **A trivial duplication axis.** The Materialize substep duplicates per
  `(fn × geometry)` (see [[order_of_compilation.md]], Memory), but a `!` function
  never runs under `page` — the user cannot allocate in the root, so allocation
  always happens under `arena` (see [[memory_model.md]]) — and the userland set is
  a singleton. So every `!` function has exactly one memory instance; `page`
  governs only `main`'s own frame and the graft of the arena node.
- **No copying adoption.** `page` and `arena` are two geometries, so an
  `arena`→`page` return *is* a geometry boundary — but an arena is physically
  carved from its parent, so every such claim is **copy-free wall-dissolution**
  (§3, Compact-on-claim). The real adoption path — copy + register into a
  `gc`/`rc`/`heap` — needs a non-bump target, which `cf0` never has.
- **The bump core only.** Both no-op `on_free` and `on_store` (§5, The bump
  family), so the finisher sweep leaves just the bump hooks: `on_scope_enter`/
  `_exit`, `on_alloc`, `on_realloc`, `on_ret`/`on_alloc_ret`, and
  rehome-as-keep-alive. No barriers, no per-object reclaim, no collection trigger.

**What stays identical.** The nine-hook **contract** (§1), the **placement** rules
(§2), the in-flight / claim-once **return protocol** (§3), and the `%node`/`%ret`
**calling convention** (§4) are unchanged — the bootstrap compiler threads nodes
and returns residue by pointer exactly as the full design prescribes. It simply
exercises `page` and one `arena` where `cf` exercises the whole catalog. The
node-free pruning of §4 is live (roughly a fifth of the self-hosted compiler's
own functions emit bare signatures). The duplex substrate and classification-at-birth of §5 are
**live** in the self-hosted compiler: the classification is `routing.cf` (shadow
sets + per-function summaries, cross-checked against the `!`-oracle on every
compile), the birth routing and scope brackets are `emit.cf`, and the corpus
pins the reclamation behavior (frame, loop, survivor-scope, two-ended and meet
tests).

This is what makes the bootstrap tractable: `cf0` implements `page` + `arena`, and
`cf` (generation 1) restores the full userland set — the barriered and per-object
geometries that make adoption and cross-geometry duplication real. Which phases
`cf0` runs in full and which it runs degenerately — this section among them — is
[[seed_subset.md]].

**The geometry-modules arc: the degeneracy relocates from the floor to std source.**
The bump geometries now live as **user-visible std source** — `lib/std/mem/page.cf`,
`lib/std/mem/fixed_buffer.cf`, and the two arena shapes `lib/std/mem/arena/{fixed_arena,
growing_arena}.cf` — exporting the §1 hooks over the `raw::*` substrate (`std::mem::raw`),
rather than as hardcoded folds in the QBE floor. Each geometry's constructor is a bodyless
`pub intrinsic of` the user imports and calls (`fixed_arena::of(n)`, `fixed_buffer::of(buf)`);
`page` has no constructor (its node is argument zero). The Materialize substep mints
per-`(fn × geometry)` copies under a **fixed mangle tag** (`__pg`/`__fx`/`__el`/`__fb`) for
the functions reachable under each ambient, and emit places the geometry's own
`reserve_*`/claim/scope calls (fold-lite: a record literal builds directly in a
`reserve_ret`/`reserve_alloc` reservation, the identity-law-fused `on_alloc`/`on_ret`). The
compiler self-hosts through its own `growing_arena` hooks (it grafts one growing arena in
`main`). The **`page`** geometry is
materialized for a program whose `main` genuinely allocates on the root ambient — a
colorless (non-`!`) frame building a record, plus its bare-reachable colorless allocator
subtree (`build__pg`), which the root check permits on the page; a `!` frame is never `pg`
(it must run under a graft), so `page` only ever reserves survivor-side and never brackets.
An arena-grafting `main` that places nothing on the page (the compiler, and every program
whose allocating work runs `in` a graft) stays legacy and the auto-imported `page` module
is stripped back out, leaving such programs byte-identical. What Materialize cannot prove
(indirect calls, dynamic node handles, non-record aggregates) degrades to the shared bump
**floor** (`cf_alloc`/`cf_reset`), coherent because hooks and floor honor the one 96-byte
duplex node layout — so the floor narrows to the residue Materialize leaves it, never to
unsoundness. Retiring the dead floor paths where every ambient is specialized is the arc's
cleanup stage.
