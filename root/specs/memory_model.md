# C! Memory Model

This document describes how memory works in C!. It is the semantic backbone the
grammar (see [[ebnf.md]]) is shaped around — in particular the `!` marker on
function names and the `in` clause on calls.

Status: design in progress. Items marked _(confirm)_ are agreed in spirit but
not yet finally ratified.

## 1. Three separations

C! deliberately pulls apart three things most languages fuse:

1. **Behavior is detached from data.** There are no methods. Behavior lives in
   modules; data is inert.
2. **Data is detached from memory layout.** The user never allocates or frees.
   Placement is decided by a **geometry** — a comptime allocation strategy.
3. **Memory is detached from the program's identity.** All memory lives in one
   structure, the **manifold**, and code only ever names a _policy_ over it.

## 2. The manifold

Memory is a **tree** of nodes called the manifold.

- The **root** node is unbounded, backed directly by the pages the OS hands the
  process. `main` is run under the root at app start; everything inside `main`
  is user land. The user **cannot** allocate in the root — a program must graft
  at least one geometry node before it has usable memory.
- Every non-root node is minted by **instantiating a geometry at runtime** —
  the `mem.arena.of(4096)` call itself creates the node (see
  §4). A child node **pulls its space from
  its parent node** — it is physically _inside_ the parent.
- A node lives exactly as long as the binding that names it, and bindings nest
  with the call structure, so **a node always outlives every node instantiated
  below it**, just as a caller always outlives its callees. This single fact
  removes the need for lifetime annotations or borrow checking (see
  §6).

This is a region system with **pluggable region policies** — the manifold is a
tree of Tofte–Talpin regions (Mads Tofte & Jean-Pierre Talpin, _Region-Based
Memory Management_, Information and Computation 132(2), 1997; implemented in the
[ML Kit](https://elsman.com/mlkit/)), generalized so each region carries a
_chosen_ geometry rather than the single stack-of-regions discipline of the
original.

Node capacities form a **comptime algebra**: a bounded child must fit within
its parent's bound, and an unbounded child (say, a GC heap) requires an
unbounded ancestor chain up to the root. Violations — an unbounded child
grafted into a bounded parent, or children whose total cap exceeds the
parent's — are compile errors. This is checkable precisely because geometries
are comptime entities and can never be assigned at runtime.

## 3. Geometries

A geometry is a memory-management policy — arena, GC, RC, and so on. Crucially it
is a **comptime** construct: it does not exist as a runtime object. Instead a
geometry is a set of **lifecycle hooks** that are inlined directly into the body
of every function it governs. Because the hooks are inlined at comptime, a
geometry assigned to a call **propagates down the entire call subtree** until a
nested call overrides it with its own geometry.

The full hook set is **nine** lifecycle hooks; the exact signatures, placement,
and bodies are [[geometry_lowering.md]]:

- `on_scope_enter` / `on_scope_exit` — open and close a scope
- `on_alloc` — place residue that dies in the frame
- `on_realloc` — grow an aggregate in place
- `on_ret` / `on_alloc_ret` — mark a value escaping via return, and claim it in
  the caller
- `on_free` — reclaim one dead object
- `on_rehome` — relocate residue into another aggregate's node
- `on_store` — the write barrier on a reference field

Different policies implement different hooks (an arena leans on the scope pair and
no-ops `on_free`/`on_store`; a GC and an RC lean on `on_store` and trigger
collection from within their own `on_alloc`). Hooks a policy does not need are
supplied as no-ops and removed by dead-code elimination. Two further hooks — a
poll point `on_safepoint` and a read barrier `on_load` — are **reserved for
concurrency** and go unused by the current sequential language (see
[[geometry_lowering.md]]).

Geometry and node are two sides of the same thing inside the manifold: the
geometry is the comptime side (the hook set — the _shape_), the node is the
runtime side (the region state — the _space_). Instantiating the one creates
the other; hence the geometry metaphor.

The node a geometry instantiates is passed to every governed function as a
**hidden first parameter**, so the hooks reach it in one step. It never appears
in source, and it is pruned entirely from provably node-free calls (see
§5). The call site threads the _correct_ node handle per comptime node
identity — a hook that grows a foreign aggregate (see `on_realloc` under
§5, Escaping) receives that aggregate's node, not the ambient one.

## 4. The `in` clause

A geometry is attached to a program **only at a call site**, with an `in` clause:

```
const arena = mem.arena.of(4096)

my_allocating_function!() in arena
```

(The `mem.arena.of(...)` part is modules, specified elsewhere.)

- **Instantiation creates the node.** The geometry-creating call
  (`mem.arena.of(4096)`) is **comptime-shaped, runtime-materialized**: the
  node's capacity, hook set, and identity are fixed and validated at comptime;
  the call's runtime step merely grabs the buffer (carves pages, or space from
  the parent) for that already-settled shape. The binding names the node. Every
  `in arena` wires its call to **that same node** — two calls sharing `in arena`
  share the node. (Harmless for arenas, essential for GC/RC, which manage one
  shared heap.)
- **Node lifetime is geometry-defined.** _For an arena_, node lifetime = the
  binding's lifetime: the node lives as long as the variable it is bound to, and
  is torn down by the arena's `on_scope_exit` of the scope that declared it
  (because the geometry binding is itself residue — see below). Other geometries
  define `on_scope_exit` differently: GC/RC no-op it and reclaim through their
  own hooks (collection driven from `on_alloc`, bookkeeping from `on_store`), so
  **their nodes ignore scope boundaries** and outlive the binding's scope by
  policy.
- **Propagation.** A call with no `in` inherits the ambient geometry from its
  caller. `in` is therefore _required_ only where a branch that needs memory has
  no ambient geometry yet — i.e. at the top of the branch — and optional
  everywhere below.

## 5. Allocation algebra: the `!` effect

Allocation is the **only effect** in C!. The effect system that tracks it is
called the _allocation algebra_, and its marker is `!` — which is why the
language is called `C!`.

A function is marked `!` when it needs memory: it holds at least one aggregate
that lives _and dies within its own frame_ (between `on_scope_enter` and
`on_scope_exit`, never escaping), **or** it calls another `!` function that is
not received as a parameter.

```
const my_allocating_function! = () -> {
  const Point p = { x: 0, y: 0 }   # own residue — never escapes
  return p.x                       # only a scalar leaves
}
```

Here `p` is trapped, so the function is `!`. Had it returned `p`, the residence
would escape and the function would be colorless.

`in` supplies a geometry to a `!` call; it does **not** bleach the caller. The
color therefore propagates up the call chain and terminates at `main`, which
the runtime itself runs under the root.

`main` is the **one exemption** from the marker and from the both-ways check
below. It is the terminus of the color — no caller ever glances at it — and the
runtime runs it directly under the root, so a `main` that allocates neither needs
nor carries a `!`. Every _other_ function is checked exactly.

The two constructs answer different questions. `!` _reports_ memory: a
colorless function's subtree provably touches none (beyond what escapes to the
caller or arrives as a visible `!` argument). `in` _places_ memory: it wires a
subtree's allocations to a node. Bleaching the caller at `in` would conflate
the two — a "discharged" function still carves its node's capacity out of the
ambient parent, so its caller still pays; hiding that behind a colorless name
would make the cost invisible. In C! the only colorless functions are the ones
that are genuinely free.

Not bleaching also buys a threading optimization. The hidden node parameter
(see §3) need not reach every function: a call whose
callee is colorless, takes no `!` argument, and returns only scalars is
provably node-free — its entire subtree is pure compute — so no node is
threaded through it at all. All three facts are visible at the call site from
signatures alone. (A colorless callee can still carry a node in the other two
cases: an aggregate escaping through it, or a `!` argument riding through it —
both visible at the call site.) A discharged `!` would destroy the guarantee:
a colorless name could hide a subtree that needed a node and satisfied it
internally, indistinguishable from a subtree that is just summing numbers.

### What counts as residue

- **Aggregates** (struct, array, tuple, string) are residue.
- A **string literal** is _not_ residue: it is a static constant living in
  read-only data, so binding one (`let s = "hi"`) allocates nothing and does not
  color its function. Only a string _constructed_ at runtime is residue. The same
  holds for any comptime-constant aggregate the compiler can place statically.
- **Fixed scalars** are exempt — they ride registers/immediates. Only
  comptime-sized residence may live on the stack-on-heap; this guarantees user
  **data** can never overflow the hardware stack. (Control still spends a small,
  comptime-bounded slice of hardware stack per call, so unbounded recursion
  remains a depth problem — never a data problem.)
- Allocation that **escapes** is _not_ charged to the allocator. It is charged to
  the destination's owner. So a function whose every allocation escapes is
  colorless.

### Escaping

A value escapes in one of two directions:

1. **Returning** — handed up to the caller.
2. **Rehoming** — handed to whoever owns the aggregate you store it into.

```
const push = ['Value] ('Value x, *['Value] xs) -> { ...put x into xs... }
```

`push` is **not** `!`: `x` is rehomed into `xs`, which belongs to someone else, so
`push` stays colorless even though it may grow `xs`. The growth is charged to
`xs`'s owner and executed by the `on_realloc` hook, which works solely with
aggregates: a pointer carries its node identity at comptime, the call site
threads the matching node handle in, and the growth lands in `xs`'s node, not
`push`'s ambient one.

### Higher-order functions

Coloring a HOF by its callback is the classic effect-system trap. C!'s rule:

- **Definition side.** A function is `!` from a call only when the callee is a
  concrete `!` function — **not** when it merely invokes a `!` value received as a
  parameter. So a `map` whose body only calls its callback stays colorless and
  stays reusable with `!` and non-`!` callbacks alike.
- **Call side.** A **call is effectful** if the callee is `!` **or** any argument
  is a `!` function value. An effectful call requires a geometry and colors _its_
  enclosing function `!`. So `map(xs, f!)` is effectful at the site — the effect
  rides the visibly-`!` argument, never hidden.

Because geometries are comptime, every HOF is **specialized per call site**, so
the effect resolves to a concrete answer wherever it matters. There is no runtime
effect polymorphism to reason about.

### Declared and checked

`!` is written by the user and **validated by the compiler both ways**: a missing
`!` on an allocating function is a compile error, and a redundant `!` on a
colorless one is also a compile error. The effect is exact. (`main` alone is
exempt, as above.)

`!` is part of the function's **name**, not its type — there is no type
infection. The compiler validates every function by its body, so the body is
the oracle for color: hide a `!` function behind a struct field and it is still
a `!`-function body, and every call site (resolved at comptime) sees it.

## 6. Mutability and ownership

C! has **no lifetime annotations and no borrow checker**. Four rules plus one
comptime region-outlives check replace them.

- `let` is reassignable and **recursively mutable**; `const` is non-reassignable
  and **recursively immutable**. Both are transitive through aggregates.
- A pointer is **writable only if its referent lives in a `let`**. A pointer into
  a `const` is read-only. There are no `mut` annotations — write-capability is
  inferred from a function body and checked against the argument at the call site
  (passing a `const`'s pointer where the body writes is an error).
- Passing **by value** yields a **read-only view**, regardless of `let`/`const`.
  To mutate a by-value argument you must explicitly `copy` (or `copy_deep`) it
  into your own writable instance.
- An aggregate cannot be bound to a second variable — `const y = x` on an
  aggregate is forbidden. That would be a borrow, and C! has none. Want your
  own — `copy` it.

**Pointers point only to aggregates, and only into a `let`.** Two restrictions
keep the pointer story minimal:

- **No pointers to scalars.** There is no `*Int`, `*Uint8`, etc. A scalar is
  always passed by value (a read-only view); to "mutate a scalar" you return the
  new value, or the scalar is a **field of an aggregate** you pass by `*T`. All
  mutable shared state therefore lives in aggregates.
- **No by-reference of a `const`.** `&c` on a `const` is an error — a `const` is
  only ever passed by value (read-only). A `*T` parameter thus only ever receives
  the address of a **`let` aggregate**, which is exactly the writable case.

A consequence worth stating: because the only pointer is `*Aggregate` and it is
reached through `.field` (which auto-dereferences, see [[ebnf.md]]) and `[i]`,
there is **no need for a whole-value dereference `*p`** — and the language does
not have one. `&` exists solely to hand a `let` aggregate to a `*T` parameter.

**Mutability is a property of the home, not the value.** Rehoming adopts the
destination's rule: a `let` value rehomed into a `const` aggregate becomes const;
a `const` value rehomed into a `let` aggregate becomes let.

Rehoming is a **copy into the destination's node** under the destination's
mutability; the by-value view is not consumed and the original is untouched.

**No dangling, structurally.** Only a caller can hand a callee a pointer — its own
or its parent's — and by the tree invariant that referent always outlives the
callee. The one static check: a pointer can never be rehomed into a node that
outlives its target — a comptime **region-outlives check** over node identities,
no annotations required. This is the entire residue of lifetime checking in C!.

(Aliasing of two writable pointers is _permitted_ — the model guarantees
use-after-free freedom via regions, not data-race or iterator-invalidation
freedom. C! has no iterators, and concurrency is out of scope for now, so this is
a deliberate, currently-cost-free tradeoff.)

## 7. Closures

C! closures are **fake**: capture is a comptime construct, not runtime
machinery. A **capturing** function value is specialized per call site — its
captures lower into **hidden parameters** at specialization, so there is no
closure object and no environment allocation. Specialization removes the
_closure_, not every indirect call: a **capture-free** function value carries no
environment and may stay runtime-dispatched (see below).

Consequences:

- Capturing allocates nothing and never colors a function `!` by itself.
- A capturing function value cannot escape — it cannot be returned or stored in
  a runtime aggregate, because it does not exist at runtime. A capture-free
  function value is just a name and travels freely.

Full capture rules to be specified; the direction above is ratified.

## 8. Zero-cost return

Returning a value across a geometry boundary is **free** — no copy. Two
distinct events hide behind "return", and both are copy-free:

1. **Scope exit inside a live node.** The function returns but its node is
   shared and outlives the call (an `in arena` used by several calls). The
   return value already sits in storage that outlives the call — there is
   nothing to do at all. `on_scope_exit` only runs the drop-set for the dead
   residue.
2. **Node teardown.** The binding that names the node dies with its scope, and
   a value is returned out of the dying node. Because a child node is
   physically carved from its parent, the boundary between them is only
   bookkeeping: the node's **walls dissolve** and the return's storage is
   **re-attributed to the parent, in place** — same address, same bytes, and
   any **returned pointer stays valid**.

For teardown to be sound the compiler promotes the **entire reachable closure**
of the return value, not just its top: anything the return points at is promoted
too, and the drop-set is _own residue minus everything reachable from the
return_. This reuses the same escape analysis that computes `!`.

Reclaiming the _dead_ residue is the geometry's own concern, orthogonal to the
copy-free return:

- **Arena** — literally free. Arenas never per-object free, so dissolving the
  walls merges the child bump (dead residue included) into the parent, reclaimed
  at the parent's reset.
- **RC / GC** — the return is still not copied, but dead residue runs its hooks; a
  compacting collector may relocate by its own policy.

The invariant that always holds: **crossing a return boundary never itself costs a
copy.** Any movement is a geometry's chosen policy, not a boundary tax.

---

**Lowering.** How all of the above is actually landed into function bodies — the hidden
`%node`/`%ret` calling convention, the closed nine-hook set (`on_scope_enter`/`_exit`,
`on_alloc`, `on_realloc`, `on_ret`, `on_alloc_ret`, `on_free`, `on_rehome`, `on_store`), the per-block mark/reset
discipline, the in-flight / claim-once return mechanics, and compact-on-claim — is the
**settled** subject of [geometry_lowering.md](./geometry_lowering.md). It is pinned there so
the design is not re-litigated; read it before touching Phase 3.
