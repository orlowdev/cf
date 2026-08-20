# C! Geometry Lowering

The interior of the Memory arc: the closed hook set a geometry implements, how
the compiler **places** those hooks into function bodies, and how the readable
`.cf` calls eventually become the emit-level `%node`/`%ret` calling convention.
It fills in the frame [[order_of_compilation.md]] leaves open (that arc "only
places the calls") and lands the semantics [[memory_model.md]] describes (the
manifold, the `!` algebra, zero-cost return) into actual code.

Status: design settled. The hook set, its placement, the return protocol, and
the calling convention below are ratified and **pinned** — read this before
touching the Memory arc rather than re-deriving it. Concrete geometry bodies
(§5) are reference implementations; the collection *algorithms* inside `rc`/`gc`
are their own subject.

This spec spans two altitudes. §1–§3 (the hook contract, placement, and the
return protocol) are the **`.cf`-level** layer: what the Memory arc emits, still
readable C!, still `node_0`. §4 (the `%node`/`%ret` calling convention) is the
**emit-level** ABI the backend lowers that placement to, where `node_0` becomes
QBE `%node`. The
seam between them is the last `.cf` arc; everything before it is observable,
everything after is one-way.

## 1. The hook contract

A **geometry is a module** — there is no `geometry` keyword. It is any module
that exposes the closed hook set below plus a constructor (`of`, `with_capacity`,
…) that mints a node (see [[memory_model.md]], The `in` clause). Selection is
structural: `in arena` names the module, and the Memory arc splices its hooks
into every governed body. Because the contract is duck-typed, a user geometry and
a standard one are indistinguishable to the compiler — the same placement runs
over both.

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
| `on_ret`         | `['T] (MemoryNode, 'T) -> 'T`          | mark residue as escaping (in-flight)  |
| `on_alloc_ret`   | `['T] (MemoryNode, 'T) -> 'T`          | claim an in-flight return into node   |
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
  it. This mirrors the escape analysis that computes `!` (see [[memory_model.md]],
  Zero-cost return) and is what the drop-set is defined against.
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
| _escapes-via-return_ | `return <agg>`            | `on_ret(node, …)` in callee; `on_alloc_ret(node, …)` wrapping the call in the caller |
| _rehomed_          | store `<agg>` into a foreign aggregate | wrap the stored value in `on_rehome(node_target, target, …)` |

The node handle threaded into each hook follows the node-locality law: `on_alloc`
takes the ambient `node`, but `on_realloc`/`on_rehome` take the *target's* node,
which the decide substep already resolved to a concrete `node_<i>`.

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

Escape is **keep-alive**. A value that must outlive the frame that built it is
**in-flight** — owned by nobody until a destination **claims** it. C! has exactly
two escape shapes, and they are the *same* mechanism aimed at two destinations:

- **Return** — claimed by the caller's binding: `on_ret` (callee) / `on_alloc_ret`
  (caller).
- **Rehome** — claimed by an ancestor aggregate: `on_rehome`.

Both say "hand this up, and don't let my cleanup take it."

### In-flight and claim-once

`on_ret(node, v)` runs in the **callee** on the **callee's** node. It marks `v`
and its whole reachable closure (the closure law) as in-flight by **removing it
from the frame's drop-set** — so whatever `on_scope_exit` / `on_free` reclaim,
they never touch it.

`on_alloc_ret(node, v)` runs in the **caller** on the **caller's** node, wrapping
the call, and **claims** the in-flight value into the caller.

The **claim-once law** holds structurally: a return has exactly one destination,
so every in-flight value is claimed by exactly one `on_alloc_ret` — never
double-claimed, never leaked. And because collection triggers only inside
`on_alloc` (no safepoints — §1, Reserved for concurrency), the window between an `on_ret` and its
`on_alloc_ret` contains **no allocation**: the in-flight value cannot be collected
mid-flight, so it needs no pinning to stay alive across the boundary. Dropping
`on_safepoint` bought this for free.

### Compact-on-claim, and why the boundary is copy-free

What the claim *costs* is the geometry's business; the boundary itself never
charges a copy (see [[memory_model.md]], Zero-cost return). The claim site is the
one place that knows the value's final home, so it is where a relocating policy
acts:

- **Bump family** (`page`, `arena`, `fixed_buffer`) — **copy-free**. `on_alloc_ret`
  either does nothing (the value already sits in a shared node that outlives the
  call — memory_model's event 1) or, when the callee's node tears down, advances
  the parent's bump to **adopt the returned closure in place** — walls dissolve,
  same address, returned pointers stay valid (event 2). Neither copies.
- **Compacting `gc`** — **compact-on-claim**: the collector relocates the value
  into compact storage *at the claim*, rather than deferring to a later poll. A
  copy, but the collector's own policy, not a boundary tax.

So `.cf` reads `on_alloc_ret(node_0, f(...))` uniformly, and each geometry folds
it to its own claim — a bump adjust, a compaction, an RC adopt.

### Rehome is return aimed at an ancestor

`on_rehome(node_target, target, v)` is the same keep-alive, claimed by `target`
instead of a return slot — `on_ret`'s drop-set removal and rehome's are the same
operation. The destination is a specific **comptime-known ancestor**: the pointer
to `target` carried its node identity down the call chain, and the call site
threads the matching handle in (memory_model, Escaping) — never a runtime walk.
Because the relationship between the ambient node and the target node is therefore
settled at comptime, rehome resolves to one of three cases:

| condition (all comptime-known)               | `on_rehome` becomes                                            |
| -------------------------------------------- | ------------------------------------------------------------- |
| `v`'s node already outlives `target`'s node  | **no-op** — `v` survives regardless; just wire the reference  |
| same geometry lineage, `v` below `target`    | **keep-alive** — drop-set removal + compaction toward the target frame (the `on_ret` machinery, driven by the *ambient* node); wall dissolution carries it the rest of the way up. The target handle only names *how far*, and is unused at runtime |
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

The handle is a **pointer to shared, mutable node state** — a bump `{top, limit}`
for the arena family, a heap/collector descriptor otherwise. It is threaded *by
pointer* precisely so every frame and every callee running under the same node
observes the same state: this is what makes two `in arena` calls share one node
(see [[memory_model.md]], The `in` clause). To the calling convention the handle
is **opaque** — only the inlined hooks know the state's layout; the ABI just moves
a word.

Locally minted nodes (`node_1`, `node_2` from a nested `mem.arena.of`) are
ordinary local handles. A call fills the callee's slot 0 with whichever node
identity the decide substep assigned it — the ambient node by default, or the
*target's* node for a `!` call that grows or rehomes into a foreign aggregate
(the node-locality law). Which handle fills slot 0 is thus a comptime fact at
every call site.

### Node-free pruning: no argument zero at all

A call proven **node-free** carries no `%node`. A function is node-free iff it is
**colorless, takes no `!` argument, and returns only scalars** (see
[[memory_model.md]], Allocation algebra) — its whole subtree is pure compute. Its
emitted signature is just its user parameters, and callers pass no node.

The escapes that re-attach a node even to a colorless function — an aggregate
escaping *through* it, or a `!` value riding *through* it — are all visible at the
call site, so the presence or absence of `%node` is a **per-call-site comptime
fact**, never a runtime decision. The consequence is the one [[memory_model.md]]
promises: a pure-compute subtree emits the *exact same ABI it would have without
C!* — no hidden parameter, no threading, nothing to pay.

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

A geometry is a module exposing the nine hooks plus a constructor. These sit at
the **floor of the allocation algebra**: a hook body takes the `MemoryNode` state
by pointer and works in raw memory (`mem.raw.*` — page mapping, pointer bumps,
byte copies), so a hook is itself **neither `!`-colored nor node-threaded**.
Otherwise `on_alloc` would need a geometry to allocate, and the regress never
bottoms out. Each geometry reads the opaque `MemoryNode` as its own private state
layout.

The bodies below are **reference implementations** — the essential operation of
each hook, not a line-complete allocator. The collection *algorithms* inside `rc`
and `gc` (mark-sweep vs copying vs generational) are their own subject; the hooks
only expose the trigger points.

### The bump family: `page`, `arena`, `fixed_buffer`

One shared core: the node is a **cursor and a ceiling**, allocation is a pointer
bump, reclamation is a cursor reset, and `Mark` is a saved cursor.

```
type BumpNode = { top: RawPtr, limit: RawPtr }   # how the family reads MemoryNode

const on_scope_enter = (MemoryNode node) -> node.top          # Mark = the cursor now
const on_scope_exit  = (MemoryNode node, Mark mark) -> {
  node.top = mark                                              # rewind — frees the frame at once
}

const on_alloc = ['T] (MemoryNode node, 'T v) -> {
  let p = node.top
  node.top = p + size_of(v)
  assert(node.top <= node.limit)                               # bounded families trap here
  mem.raw.place(p, v)                                          # v now homed at p
  return p                                                     # same 'T, new address (identity law)
}
```

The rest of the family's hooks are uniform:

| hook           | bump-family body                                                      |
| -------------- | -------------------------------------------------------------------- |
| `on_realloc`   | extend in place if `v` is the top-most block, else bump a fresh copy |
| `on_ret`       | keep-alive — exclude `v` from the coming rewind (§3)                  |
| `on_alloc_ret` | copy-free claim — nothing, or wall-dissolve re-attribution (§3)       |
| `on_free`      | **no-op** — a bump never frees per object (swept — §2)                |
| `on_rehome`    | degenerate per the §3 rehome table (no-op / keep-alive)              |
| `on_store`     | **no-op** — folds to a plain store (no barrier)                      |

The three members differ only in **where the buffer comes from** and **what
"full" means**:

- **`page`** — the root-adjacent geometry `main` runs under. Backed directly by OS
  pages; `on_alloc` maps more pages (`mem.raw.map`) when the cursor meets the
  limit, so it is the one **unbounded** bump. `on_scope_exit` at program scope
  unmaps.
- **`arena`** — `of(n)` pulls one `n`-byte block from the parent node
  (`parent.on_alloc`), so it is **bounded** by `n`; frames rewind within it and
  the whole node dissolves into its parent at teardown (memory_model, Zero-cost
  return).
- **`fixed_buffer`** — `of(buf)` wraps a **caller-provided** array (stack or
  static); creating the node allocates nothing at all. Bounded by the buffer;
  overflow is a hard trap. The zero-dependency embedded allocator — it differs
  from `arena` only in the buffer's provenance.

### The free-list family is subsumed

An earlier draft catalogued a free-list family — `pool`, `slab`, `heap` — whose
node owns a **free-list**: `on_alloc` pops a block, `on_free` pushes it back,
reclamation per-object. That family does **not survive** the `on_ret` /
`on_alloc_ret` design, and is dropped.

A free-list's only edge over a bump is freeing an object *before* its scope ends
and reusing the slot — which matters exactly when a scope both keeps some value
and drops others (the mixed case). But that is precisely what `on_alloc_ret` /
`on_ret` hand to the **arena**: the returned aggregate is pre-allocated on the
**caller** (`on_alloc_ret`), so the returning frame holds only dead scratch
(`on_ret`), and `on_scope_exit` rewinds the whole frame — the mixed
return-plus-residue case an older draft leaked now reclaims in bulk. For **every
statically-placeable lifetime**, the smart arena already reclaims it, at frame
exit, with no free-list. Same-size churn (`pool`) and size-classed churn (`slab`)
are just frame residue; they buy nothing over the arena.

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
  raw_store(dst, new)
  dec(old)                                                     # release the outgoing one
}
const on_free = ['T] (MemoryNode node, 'T v) -> {
  if dec(v) == 0 then {                                        # last reference
    drop_referents(v)                                          # recursively dec what v points at
    free_list_push(node, addr_of(v), size_of(v))
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
  let p = gc_bump(node, size_of(v))
  mem.raw.place(p, v)
  return p
}
const on_store = ['T, 'U] (MemoryNode node, *'U dst, 'T new) -> {
  remember(node, dst)                                          # card-mark / SATB before the write
  raw_store(dst, new)
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
**calling convention** (§4) are unchanged — `cf0` threads nodes, prunes node-free
subtrees, and returns residue by pointer exactly as `cf` does. It simply exercises
`page` and one `arena` where `cf` exercises the whole catalog.

This is what makes the bootstrap tractable: `cf0` implements `page` + `arena`, and
`cf` (generation 1) restores the full userland set — the barriered and per-object
geometries that make adoption and cross-geometry duplication real. Which phases
`cf0` runs in full and which it runs degenerately — this section among them — is
[[seed_subset.md]].
