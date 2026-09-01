# C! Context

Scope-provided data: how a scope makes typed values available to its dynamic
extent without threading parameters, and how consumers reach them by naming a
type. The mechanism is a generalization of the convention the language already
runs on — [[memory_model.md]]'s ambient node, the hidden first argument every
`!` function receives, is the primordial context entry, carrying exactly one
fact ("which node"). `std::mem::ctx` extends the same hidden-argument
convention to user data: each provided type becomes one more comptime-planned
hidden argument to exactly the functions that demand it. Nothing here is a
runtime registry; provision, discharge, and routing are all compile-time, and
the only runtime artifact is a pointer in a register.

Status: design **ratified, not implemented**. It is scheduled with the
concurrency arc — the demand analysis below (§4) shares its checker machinery
with the spawn gates, and context inheritance across tasks (§7) is that arc's
concern. The `uses` clause is a pending [[ebnf.md]] addition; `provide` and
`consume` are ordinary bodyless intrinsics in the style of the geometry
constructors ([[geometry_lowering.md]] §1) and need no grammar of their own.

## 1. The model

**Context is data a scope provides; consumers name its type.** A provide
places one aggregate; a consume anywhere in the provide's extent — any call
depth below it — binds that aggregate by naming its type. There are no keys,
no lookup, no registry: the compiler resolves every consume to a concrete
provide at comptime and threads a hidden pointer argument through the call
chain between them, exactly as it already threads the ambient node.

**Context carries data only — never behavior.** A provided type may not
contain a fn-typed field, transitively (§3). Behavior arrives by import
([[module_system.md]]) and is called directly; context supplies the data that
behavior reads. This is deliberate anti-symmetry with Odin's `context.logger`
and every DI container: a call through context would be dynamic dispatch
smuggled past the reader, and it would break the directness every demand-chain
call otherwise keeps.

Two effects, two colors, fully orthogonal:

| color    | means                          | needs                  |
| -------- | ------------------------------ | ---------------------- |
| `!`      | may reserve memory             | the ambient node       |
| `uses T` | reads the nearest provide of T | a hidden `T` pointer   |

All four combinations are legitimate. In particular a **non-`!` function may
consume — and may mutate a `let` provide** (§3): mutation is not allocation;
every write lands in storage the provider already reserved, in an arena that
strictly outlives the consumer. Providing, by contrast, *is* allocating — the
provide site places the aggregate — so provision lives on the `!` side of the
program. Providers allocate; consumers don't.

## 2. Providing

A provide is a stage in a geometry **construction pipe**:

```
std::mem::growing_arena::of(1024)
	|> std::mem::ctx::provide(config)
	|> defer std::mem::growing_arena::destroy
```

`provide` is a bodyless intrinsic, data-last per the stdlib rule: the user
supplies the value, the pipe supplies the freshly carved node. The aggregate
is placed survivor-side in that arena — when the value is constructed
literally in the pipe (`provide(AppConfig({ ... }))`), fold-lite builds it in
place at the reservation, zero copies. The provided type joins the geometry's
comptime shape, alongside fixed-vs-elastic and the chunk size.

**The anonymous pipe is the canonical form.** No arena binding is required:
the handle threads through the pipe as a temporary and `defer destroy`
captures it. Lowering synthesizes the binding the IR needs; the surface names
an arena only when it is also an allocation home for `in a` work. A
context-only arena reads as what it is — a scope-level declaration that this
scope provides `AppConfig`, reclaimed at exit.

**Extent is lexical**: from the provide statement's position to the end of the
enclosing scope. Discharge is *never* gated on `in a` — the `in` clause keeps
its single meaning, node selection for `!` calls ([[memory_model.md]]), and a
colorless demand chain never passes one. Storage rides the arena; visibility
rides the scope. An inner provide of the same type shadows the outer one for
its own extent, exactly as an inner binding shadows an outer name — a provide
*is* a binding, anonymous and keyed by type.

**Reclamation is `destroy`**, per the user-placed teardown contract
([[geometry_lowering.md]] §1). The idiomatic `|> defer destroy` makes the
storage's end coincide with the comptime extent's end. An explicit early
`destroy` followed by a consuming call is the pre-existing use-after-destroy
hazard, not a new one; because provides are comptime-visible, a later checker
can reject it outright.

A provide carries a **binding mode**, `const` or `let`, like any binding.
Mutability is transitive from the binding per [[type_system.md]] §9 — there
are no per-field markers — so a program that wants immutable wiring next to
mutable state writes two provides, not one mixed record:

```
std::mem::growing_arena::of(4096)
	|> std::mem::ctx::provide(flags)        # const: this run's invariants
	|> std::mem::ctx::provide(let state)    # let: shared mutable state
	|> defer std::mem::growing_arena::destroy
```

## 3. What may be provided

**Aggregates only, and transitively fn-free.** A provided type is a nominal
record ([[type_system.md]] §7); scalars are wrapped. No field anywhere in the
type, at any depth, may be fn-typed — checked by a comptime walk, rejected
cleanly: *context carries data; pass behavior explicitly*. This is the dual of
the consume-side restriction on fn values (§4); together they guarantee no
call in the program dispatches through context.

**`const` provides** are the run's invariants. The canonical root provide is
`main` parsing env and CLI argv once into typed records and providing them on
the root arena — `uses Flags` then replaces parameter-drilling configuration
through every layer below.

**`let` provides** are shared state, and their mutation is **in-place only**:
scalar fields and inline `[N Elem]` fields ([[type_system.md]] §6). Repointing
a ctx field at an aggregate freshly allocated by a consumer is rejected. The
store itself would be colorless, but the value was born in the consumer's
ambient — a descendant arena that dies before the provider's — so the record
would outlive what it points at. The consumer cannot repair this by placing
owner-side, because the owner's *node* is not threaded — only the context
pointer is (§4). Provider-outlives-consumer is exactly what makes reads safe,
and exactly what makes young-pointer-into-old-record writes always wrong;
inline storage is the language's own escape hatch, letting state live inside
the aggregate with no repointing at all.

## 4. Consuming and the demand set

`consume(T)` is a **comptime binder**: it names the hidden argument, costs
nothing at runtime, and copies nothing. Reads compile to loads through the
pointer; on a `let` provide, in-place writes compile to stores.

```
const log = (Str msg): () -> {
	const { sentry_host } = std::mem::ctx::consume(SentryConfig)
	# ... ship msg to sentry_host
}
```

A function's **demand set** is its own consumes unioned with its callees',
propagated bottom-up over the whole call tree by the same lattice that colors
`!` ([[memory_model.md]], the `!` algebra). Each demand records the type and
whether the consumer mutates: a mutating demand is discharged only by a `let`
provide; a read-only demand by either mode. At every call site the compiler
projects the lexically available provides onto the callee's demand set; a
demand no provide in scope can discharge is a comptime error naming the type
and the site. Because compilation is whole-program and fn values are
demand-free (below), the analysis is closed — there is no conservative
over-threading and no runtime failure mode. React-style "no provider" crashes
are unrepresentable.

**The demand set is the hidden signature.** Each demanded type is one hidden
pointer argument, appended after the ambient node (when present), in a
canonical order — sorted by mangled type name — so provide order, consume
order, and declaration order are all irrelevant to the ABI. Cost is strictly
pay-per-demand: a function on no demand chain carries nothing; one demanding
a single type under a scope providing five carries one pointer. A provide
that nothing in its extent demands is **dead** and skips placement entirely —
sound, because arena contents are unobservable except through consume.

Two obligations fall out of the ABI:

- **`pub` functions declare their demands** with a `uses` clause
  (`pub const f = (Str s): () uses AppConfig -> ...`; spelling pending in
  [[ebnf.md]]). Within a module demands are inferred freely; at a module
  boundary the hidden signature is part of the contract and must be written,
  exactly as `!` is worn on the name.
- **Fn values are demand-free.** A function with a non-empty demand set
  cannot become a value — its ABI differs by its demands, so an indirect call
  site cannot supply them. Rejected cleanly; a function meant to be passed
  around takes its data as an explicit parameter.

Escape of a consumed value through a return is not special: the context
pointer is an argument, so aliasing it into a return is the routing pass's
existing `ret:<param>` classification — survivor-side, node-bounded, never
dangling. Escape through a store into `let` context is governed by §3's
in-place rule.

## 5. Embedding: spread discharge

A provide can discharge a demand for a type its record **declares by
spread** ([[type_system.md]] §7):

```
data SentryConfig = { Str sentry_host }
data AppConfig = { ...SentryConfig, HashMap[Str, Bool] feature_flags }
```

`provide(app)` discharges `uses SentryConfig` because `AppConfig` declares
`...SentryConfig` — **the spread is the opt-in, and it is nominal**. A record
that merely happens to contain a `Str sentry_host` field does not match;
records are never anonymous and neither is provision. Embedding composes
transitively (`AppConfig` spreads `MidConfig` spreads `SentryConfig`), and
the embedded discharge inherits the provide's binding mode.

The mechanism is an **interior pointer**: both types are comptime shapes, so
the discharge site passes `ctx + splice_offset`, one comptime add. Multiple
embedded types yield multiple interior pointers into one provide. Offsets are
a layout obligation, not a runtime concern: a spread splices its source's
fields flat and in order, and record layout must align each splice point to
the source record's alignment so the embedded block's internal offsets equal
the standalone record's.

Ambiguity is rejected, never prioritized: if two provides in scope can both
discharge one demand — two records embedding the same type, or a direct
provide next to an embedding one — the program is in error, and the
diagnostic names both candidates. A record spreading the same type twice is
already a field-collision error before context enters the picture.

## 6. The library pattern

A library ships **behavior as functions and its data demand as a type** —
compile-time dependency injection with the whole graph visible:

```
# the library
data SentryConfig = { Str sentry_host }

pub const log = (Str msg): () uses SentryConfig -> {
	const { sentry_host } = std::mem::ctx::consume(SentryConfig)
	# ...
}
```

```
# the application
data AppConfig = { ...SentryConfig, HashMap[Str, Bool] feature_flags }

pub const main = () -> {
	const AppConfig config = { sentry_host: "ingest.example", feature_flags: flags }
	std::mem::growing_arena::of(1024)
		|> std::mem::ctx::provide(config)
		|> defer std::mem::growing_arena::destroy

	f()	# colorless; log's demand discharges through AppConfig's spread
}
```

Import wires the code; context wires the configuration; every call stays
direct and inlinable. What DI frameworks simulate with runtime reflection is
here a comptime add.

## 7. Context across tasks

Forward reference, owned by the concurrency arc: a task spawned under a scope
(`task::spawn in <geom>`) inherits the **`const`** demands of its body freely —
immutable, race-free by construction. A **`let`** demand crossing a spawn
boundary is rejected until atomics exist to earn it back. The binding mode of
a provide is thus also the concurrency safety line, with no additional rules.

## The rejections

Context adds no inference; it adds clean rejections. For reference, the
complete set: a fn-typed field anywhere in a provided type (§3); repointing a
`let` ctx field at a consumer-fresh aggregate (§3); a mutating demand against
a `const` provide (§4); an undischarged demand at a call site (§4); a
demand-carrying function used as a value (§4); an undeclared demand on a
`pub` function (§4); an ambiguous discharge (§5); a `let` demand crossing a
spawn boundary (§7).
