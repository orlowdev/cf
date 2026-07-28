# C! Type System

This document defines the types of C! and the rules the **typecheck gate** applies.
It is the semantics behind the type *grammar* of [[ebnf.md]] (which only shapes the
syntax and repeatedly defers "this is a type-system decision" here) and it sits
against the memory semantics of [[memory_model.md]] (aggregate-vs-scalar, ownership,
the `!` allocation effect). In the pipeline of [[order_of_compilation.md]] it is
phase 3, `Typecheck & bind` — a **gate**: it annotates the tree and rejects the
ill-typed, and transforms nothing (the `.cf` before and after is identical).

Status: complete draft — **not yet ratified**. All sections (§1–§9) are written, and
their decisions have been propagated into the sibling specs ([[ebnf.md]],
[[order_of_compilation.md]], [[memory_model.md]]); a few sub-points remain open
(flagged inline). A final ratification pass is pending.

## 1. What the type gate owns

Per [[order_of_compilation.md]] §4 the type gate owns, and this spec defines:

- **type inference and checking** (§5) — including that an integer literal takes its
  type from context (§3);
- the **casing rule** — a `type_name` is PascalCase, a `var_name` snake_case
  ([[ebnf.md]]); types and values never collide because they are lexically split;
- **`const`/immutability and lvalue checks** — the type-level half of the ownership
  rules whose runtime meaning is in [[memory_model.md]] §6 (§9);
- **aggregate-literal** typing — the array fixed-vs-dynamic choice and
  record-literal typing (tuple-vs-array is now syntactic, §7);
- **`match` exhaustiveness** (§8.3).

It runs **before every desugar**, because all desugars are type-directed: a desugar
never guesses what a form means. Its annotations are read downstream by the effect
gate (which derives escape class from them), specialize (type arguments and `[n 'T]`
sizes), the memory arc (node-free-ness — "returns only scalars"), and emit (`match`
dispatch). Nothing it annotates may be a runtime transformation; it only decorates
and rejects.

The `!` allocation effect is **not** a type. It is part of a function's *name*
([[memory_model.md]]: "`!` is part of the function's name, not its type") — there
is no type infection, and a function type carries no color.

## 2. Primitive types

The primitives are the leaves of the type grammar — the types that are not built
out of other types. Everything else (§6) composes over these.

### 2.1 Integers

The concrete integer *types* are two mirrored families, signed and unsigned, each
with four fixed widths plus a pointer-width member:

| Signed | Unsigned | Width |
| --- | --- | --- |
| `Int8`  | `Uint8`  | 8 bits |
| `Int16` | `Uint16` | 16 bits |
| `Int32` | `Uint32` | 32 bits |
| `Int64` | `Uint64` | 64 bits |
| `Iarch`  | `Uarch`  | the target's pointer width |

- **`Iarch`/`Uarch` are pointer-width** — the natural word of the target (64-bit on
  cf0's sole target `arm64-apple-darwin`), the types for addresses, sizes, and
  counts. `Iarch` is a *distinct type* from `Int64` even where they share a
  representation (like Rust's `isize` vs `i64`): pointer-width is a semantic
  property, not "64", so a move between them is an explicit conversion (§4). `Iarch`
  is also the **default type of an unannotated integer literal** (§3).
- **`Int`/`Uint` are the *unions*, not concrete types.** `Int` is the std union of
  all signed widths, `Uint` of all unsigned, defined in §8:
  `Int = { Int8, Int16, Int32, Int64, Iarch }` and likewise `Uint`. They are the
  **generic bounds** ("any signed integer"), never a runtime value type — a
  `[Int 'T]` function is parametric over one width, monomorphized (§5.4, §8). There
  is no bare `Int` value; everyday code names a width (`Iarch`, `Int32`, …).
- **Each width is an intrinsic type carrying its constructor** —
  `pub intrinsic type Uint8 = (Number value) -> Uint8` — so `Uint8(x)` is the cast
  (§4), and the argument bound `Number` (§8) rejects `Uint8("hi")` cleanly.
- **`Uint8` is the byte.** A byte buffer is `*[Uint8]`; there is no `byte` type.
- **The ten concrete integer types are distinct** — none a subtype of another;
  crossing widths or signedness is always an explicit conversion (§4). (The only
  integer subtyping is union membership — `Int8 <: Int`, `Int8 <: Number` — and
  union-subset — `Int <: Number` — per §5.5.)

**Signedness and negation.** An integer *literal* is unsigned — negativity is the
unary `-` operator applied to a literal, not part of it ([[ebnf.md]] Numbers). The
unary `-` requires a signed operand type (a signed integer or a float); applying it
to an unsigned type is a type error (there is no wrap-to-unsigned; reach for an
explicit conversion if that is what you mean). Fixed-width **integer** overflow
**wraps** — two's-complement modular arithmetic, the hardware default, silent, and
the **same at comptime and runtime** (a comptime-known overflow folds to its wrapped
value; the compiler does not override the developer's arithmetic — §5.6). The std
offers `wrapping_*`/`saturating_*`/`checked_*` for a chosen or guaranteed result
instead. (A *literal* that does not fit its type is still a compile error, §3 — that
is a literal-range check, not arithmetic.)

### 2.2 Floats

Two **IEEE-754** floating-point types:

| Type | Representation |
| --- | --- |
| `Float32` | IEEE-754 binary32 (single) |
| `Float64` | IEEE-754 binary64 (double) |

- **Full IEEE semantics — `NaN` and `±Infinity` are ordinary values.** A float
  operation that in IEEE yields `NaN` (`0.0 / 0.0`, `sqrt(-1.0)`) or overflows to
  `±Infinity` produces that value; nothing traps. This is the float half of the same
  "a wrong value is the developer's responsibility, not a crash" rule that wraps
  integer overflow (§5.6). The consequences are the IEEE ones: **`==` is not
  reflexive** (`NaN != NaN`) and **`<` is a partial order** (comparisons with `NaN`
  are all false) — so a float, and any aggregate containing one, is not guaranteed
  self-equal. The std offers `checked_*` (→ `Either`) and finite-checking helpers for
  code that needs the guarantee.
- **Two distinct types**, and distinct from every integer type; a move between
  `Float32` and `Float64`, or between a float and any integer, is an explicit
  conversion (§4) — never implicit. Like the integers, **`Float` is the *union***
  `{ Float32, Float64 }` (§8) — the generic bound "any float", not a concrete type.
  `Float64` is the **default type of an unannotated float literal** (§3).
- **Signed.** Unary `-` applies.
- A **float literal** is decimal with a point (`3.14`) ([[ebnf.md]] Numbers) and,
  like an integer literal, takes its concrete float type from context (§3).
- **`Infinity` and `NaN` are intrinsic singleton types** (`pub intrinsic type
  Infinity`, `pub intrinsic type NaN`), *outside* the `Float` union — the IEEE bit
  patterns already exist inside `Float32`/`Float64`, so these give a *name* to write
  them, not a second representation. They are **context-typed like a literal**:
  `const Float32 x = Infinity` is `Float32(Infinity)`, an exact construction into
  that precision; `-Infinity` is unary `-`. (A bare `const x = Infinity` is legal —
  `x : Infinity` — unlike a bare numeric literal, since `Infinity` is a real type.)
  To *match* the exceptional cases, the std offers the opt-in
  `FpClass = { Finite = Float64, PosInfinity, NegInfinity, NaN }` union and a
  `classify` function — so no plain float op is forced to return a union.

### 2.3 The other primitives

- **`Str`** — a UTF-8 string, a record `{ *[Uint8] buf, Uarch char_count, Uarch len }`
  — the byte buffer, the codepoint count, and the byte length (`len ≥ char_count`,
  since a UTF-8 codepoint is 1–4 bytes). There is **no `Char` type**; a codepoint is
  a `Uint32` (a Unicode scalar value), and byte-level work goes through `buf`
  (`*[Uint8]`). A static string literal lives in read-only data and is not residue; a
  runtime-constructed string is an aggregate ([[memory_model.md]]). A string literal
  has type `Str` (§3). (Codepoint-vs-byte indexing/iteration is a std-API concern.)
- **`Bool`** — `true` or `false`. Not a built-in primitive but a std **union of two
  singleton constructors** `Bool = { False, True }` (§8); being all-tag-only it
  lowers to a one-byte integer, so it costs exactly what a primitive would. `true`/
  `false` are literal sugar for `Bool.True`/`Bool.False`. A comparison yields `Bool`;
  `&&`/`||`/`!` take and yield `Bool`; `if`/`match` on it are union dispatch (§5.6,
  §8). Listed here to place it; defined in §8.
- **`Unit`** — the name of the **unit type `()`**: the zero-element tuple, the sole
  value of which is also `()`. `Unit` and `()` are the same type — `Unit` is the
  readable spelling, used chiefly in return position so a unit-returning signature
  reads `() Unit` rather than the `() ()` crab-claw. A function that "yields nothing"
  returns `Unit`. (It replaces a `Void`: there is no C-style void here — the type has
  exactly one value, so it is named honestly.) `Maybe[Unit]` *is* `Maybe[()]`.
- **`Never`** — the **bottom type**: the type of an expression that never yields a
  value — `break`, `continue`, an unbounded loop, or a call that never returns (a
  panic/abort). `Never` **may be written** as a return type (a function that always
  diverges — `panic`, an event loop — returns `Never`), and a body with **no normal
  exit** *infers* `Never`. `Never` is a subtype of every type, so a `Never`-typed
  expression is well-typed in any position (notably as an `if` branch — `let x = if c
  then v else panic()`). Nothing has a `Never` value at runtime; it types control that
  leaves rather than returns. This is the real "produces no value" type; `Unit`/`()`
  by contrast produces the one trivial value and returns normally.

`Unit`/`()` — the zero-element tuple — is a first-class value type (bindable,
storable, an element or payload, a type argument); it falls out of the
parenthesis/tuple grammar (§6.1). It is the payload of a nullary union member
(`Maybe.Nothing`) and the return of a unit-returning function.

## 3. Literal typing

A literal is **checked against an expected type** supplied by its context; it does
not carry an intrinsic type of its own. The expected type is whatever the
surrounding form demands: the annotation on the binding, parameter, field, or
return it flows into, or an explicit type argument.

### 3.1 Numeric literals

An integer literal (`5`, `0xff`, `0b1010`, `0o17`) is **untyped** until a context
gives it a numeric expected type; it then *is* a value of that type — this is not
a conversion, so no cast is involved:

```
const Uint8 b = 5        # 5 is a Uint8
const Int32 x = -5       # 5 is an Int32; unary - negates it
five()                   # -> 5 in a function returning Int16 makes 5 an Int16
write(1, buf, n)         # a literal argument takes the parameter's type
```

- **Range check.** The literal must fit the expected type's range, checked at
  comptime; `256` where a `Uint8` is expected, or a negative into an unsigned, is
  a compile error. The check is **per literal against its adopted type** — comptime
  *arithmetic* over already-typed literals is not re-range-checked; it wraps like any
  other overflow (§2.1, §5.6), so `const Uint8 x = 200 + 100` is `44`, not an error.
- **Default type when none is supplied.** A bare literal with no annotation and no
  typed context takes a **default concrete type**: an integer literal is `Iarch` (the
  machine word — §2.1), a float literal is `Float64`. So `const x = 5` is `Iarch` and
  `const x = 3.14` is `Float64`. Every *other* width is user-specified — you annotate
  (`const Int32 x = 5`) or reach a typed context only when you want a non-default
  width. This is the Go model (word-size int, double float): the defaults exist so
  untyped arithmetic, counters, and indices need no ceremony, while a deliberate
  width is always one annotation away.
- **A numeric *union* expected type resolves to the same default member.** When the
  only expected type is a numeric union — `Int`, `Uint`, `Number`, `Float` — the
  literal cannot pick a width from the union (its members each have their own
  representation), so it takes the **default concrete member**, chosen so it is always
  a member of *that* union: a signed or `Number` integer → `Iarch`, an integer whose
  union is the unsigned family `Uint` → `Uarch`, a float → `Float64` (`Iarch ∈ Int,
  Number`; `Uarch ∈ Uint`; `Float64 ∈ Float`). `const Number n = 5` resolves the
  literal to its `Iarch` member and the binding's value **is** that plain `Iarch`;
  default resolution never fabricates a runtime union box out of thin air, so a literal
  is operable at its default width without a `match`. Where such a member *later*
  reaches a genuinely union-typed position — a `Number` parameter, a `Number`-annotated
  binding, a `match` scrutinee — it is subsumed into the union by the ordinary
  member→union rule (§5.5) and the **tag is attached at that boundary**, exactly as for
  an already-typed value (`let Number n = int8_val`). It only pays for a tag when it
  actually crosses into a union slot.
- **Propagation through operators.** An expected type propagates into the operands
  of arithmetic over untyped literals (`const Int8 y = 5 + 3` types both `5` and
  `3`, and the result, as `Int8`); a literal combined with an already-typed value
  takes that value's type; two untyped literals with no expected type each take the
  default (`Iarch`/`Float64`). The full propagation/unification mechanics are
  §5.2/§5.6; the rule here is only that a literal never invents a *non-default* type.

A **float literal** (`3.14` — decimal with a point) follows the same rules: it takes
`Float32` or `Float64` from context, or **`Float64`** by default (above), subject to
the range check. A non-representable decimal (`0.1`) **rounds to the nearest value**
of its float type — ordinary floating-point rounding, not an error; only an
out-of-*range* magnitude is rejected. An **integer literal (no point) may also take a
float expected type** — `const Float64 x = 5` is `5.0`, exact for a representable
integer — since a literal has no intrinsic width; a **float literal never takes an
integer type** (a point means a real).

### 3.2 Other scalar literals

- A **string literal** has type `Str`.
- A **`bool` literal** (`true`/`false`) has type `Bool`.

### 3.3 Aggregate and data literals

Aggregate and data literals type the same way — checked against an expected type —
but the details (array `[…]` vs tuple `(…)`, fixed vs dynamic, and nominal-vs-
structural record typing) are §7.

## 4. Conversions and casts

Conversions between distinct types are **explicit**. There is **no implicit**
widening, narrowing, or signedness change: an `Int32` does not silently become an
`Int64`, an `Int32` does not silently become a `Uint32` (nor `Iarch` an `Int64`), and
a value never changes type by flowing into a differently-typed context. Wherever two distinct types meet and
the language does not otherwise unify them, a written conversion is required.

Note this does **not** apply to literals (§3): a literal *adopts* its context type
directly rather than being converted, so `const Uint8 b = 5` needs no cast, while
converting an already-typed `Int32` value to a `Uint8` does.

- **Numeric conversions** — every cross-family (signed↔unsigned, integer↔float)
  and cross-width (widen/narrow) move is a written conversion. Truncation,
  sign-/zero-extension, integer↔float rounding, and reinterpretation are thereby
  always visible at the call site (this is what the throwaway cfcc's implicit
  `extsw` widening got wrong).
- **No pointer↔integer conversion, and no pointer arithmetic.** A `*T` is never
  turned into a `Uarch` and back — that would let a pointer wander off its aggregate
  and escape the node/region guarantees of [[memory_model.md]]. Pointers are used only
  through `.field`/`[i]` (auto-deref), created with `&`, and passed as `*T`; a raw
  address is visible only inside an `asm` function (the floor), where a syscall reads
  it straight from the register (§6.4).

**The cast is a constructor call `T(x)`** — the same "construction is application"
form defined in §6. Converting a value `x` to a type `T` is applying `T`'s
constructor to it: `Uint32(x)`, and nested for a chain of steps, `Uint16(Uint8(1))`.
A **widening** `T(x)` is exact; a **narrowing** or signedness-crossing `T(x)`
**truncates / reinterprets** the bits — silently, per §5.6's "a wrong value is the
developer's responsibility" rule. A *comptime-known* out-of-range narrowing **folds
to its truncated value** (like comptime overflow, §5.6) — not a compile error: the
written cast *is* the request to truncate. (This differs from a bare literal that
does not fit its declared type, `const Int8 x = 300` — that is the §3 literal-range
check, a value asserting a type it cannot have, with no cast to license the loss.)
Its guaranteed counterpart is an ordinary module function
returning `Either` (e.g. `numeric.to_u8(x) -> Either[RangeError, Uint8]`), never a
constructor; float↔integer conversion rounds/truncates likewise.

Because `const T x = v` is itself sugar for `const x = T(v)` (§6), a binding's
annotation *is* the conversion — `const Uint8 x = 5` types the literal directly (an
exact construction, §3), while `const Uint8 x = wide` is a narrowing `Uint8(wide)`
that truncates. That a narrowing hides under annotation-shaped syntax is a
legibility cost; the checker **warns** on a narrowing-under-annotation so it is at
least flagged (it does not forbid it — the developer may want the truncation).

## 5. Inference and unification

Inference in C! is **local and bidirectional**. Local: a function's signature is
its interface — inference never reaches across a call to solve one function from
another's use, so a whole-program view is never needed and every signature is
readable on its own. Bidirectional: types flow in two directions — **synthesis**
reads a type *out* of an expression (a literal's context, a call's return, a
binding's initializer), and **checking** pushes an *expected* type *into* an
expression (into a literal, an aggregate literal, an `if`/`match` branch, a lambda
body, a generic argument). §3's "a literal adopts its context type" is the checking
direction; this section fixes the rest.

### 5.1 Where annotations are required

A type annotation is required exactly where a type cannot be synthesized:

- **Function parameters** must be typed — by an explicit annotation, or derived
  from a surrounding typed context (a lambda checked against a known function type:
  `const Sum s = (a, b) -> a + b` types `a`, `b` from `Sum`). A bare top-level
  function whose params have neither is rejected; a param never becomes an implicit
  generic. Explicit generics are written with `'T` ([[ebnf.md]]).
- **Function return types** are inferred from the body and so may be omitted. (An
  `asm`/`intrinsic` has no body, so its return type is mandatory — [[ebnf.md]].)
- **`const`/`let` locals** may omit the type iff the initializer **synthesizes** a
  concrete type: `const p = make_point()` infers `Point`, and `const x = 5`
  synthesizes its default `Iarch` (§3). But `const xs = []` (an empty aggregate with
  no element type), or any initializer that only *checks against* an expected type
  without producing one, still requires an annotation. A member construction infers the
**precise member type** — `let m = Maybe.Just(1)` gives `m : Maybe[Iarch].Just` (§8.1);
to hold the whole union — e.g. so a `let` may be reassigned across members — annotate
it (`let Maybe[Iarch] m = Maybe.Just(1)`), which widens by subtyping.

### 5.2 Expected-type propagation (checking)

When a form supplies an expected type, it is pushed inward:

- into a **numeric literal**, which becomes a value of that type if it is in range
  (§3), and through arithmetic over untyped literals to their operands (`const Int8
  y = 5 + 3` types `5`, `3`, and the result as `Int8`);
- into an **aggregate/data literal**, resolving its kind and element/field types
  (§7);
- into both **branches of an `if`** and every **`match` arm** (§5.3);
- into a **lambda body** (the expected function type gives the params their types
  and the body its expected return);
- into a **call's generic arguments** (§5.4).

Where no expected type is supplied, a form must synthesize its own; a numeric
literal that reaches neither an annotation nor a typed neighbour synthesizes its
**default** — `Iarch` for an integer, `Float64` for a float (§3).

### 5.3 Unifying branches and match arms

`if c then A else B`, and the arms of a `match`, must combine to a **single**
result type. The combining rule is deliberately narrow — it never performs an
implicit conversion:

- Two branches of the **same type** combine to that type.
- A **`Never`** branch (a `break` or `continue` — the never-typed expressions of
  [[ebnf.md]] — or a diverging call) is subsumed: the result is the other
  branch's type. `if c then 1 else break` has whatever type the context gives `1`
  (say `Int32`).
- **Members of a single common union combine to that union.** Two branches whose
  types are members (or precise member types) of the **same one** union join to it —
  `Maybe[Iarch].Just` and `Maybe[Iarch].Nothing` combine to `Maybe[Iarch]` — with **no
  expected type needed and no search**, because a member's home union is unique enough
  to name the join (member→union subtyping, §8). A payload-free member carrying a free
  type parameter (`Maybe.Nothing` synthesizes `Maybe[?].Nothing`) takes that parameter
  from the constrained sibling in the same join — `Maybe[Iarch]` from a `Maybe[Iarch].Just`
  branch — which is unifying the one union's own argument, not a lattice search. When the members belong to **several**
  unions (a compose-over leaf like `Int8`, in both `Int` and `Number`) the join is
  ambiguous, so it happens only toward an **expected** union (annotation/param/return),
  never invented.
- Branches of **different, unrelated types** — including two different numeric
  types (`Int32` vs `Int64`) — do **not** combine; it is a type error, and the fix
  is an explicit conversion (§4). No least-upper-bound is searched for.

An **else-less `if`** has no second branch to unify, so it yields **`Unit`**: the
`then` branch runs for effect when the condition holds, and the whole expression is
`()` either way. The `then` branch **may have any type** — its value is discarded —
so `if c then compute()` is well-formed whatever `compute()` returns. (It does *not*
wrap the branch in a `Maybe` — that overhead bought nothing; a value you want to keep
uses a full `if`/`else` or a `match`.)

### 5.4 Generic type-argument inference

A call may give type arguments explicitly (`max[Int32](a, b)`) or leave them to be
**inferred** from the value arguments (`max(a, b)`) ([[ebnf.md]]). Inference unifies
each parameter's declared type (which mentions the callee's type variables `'T`)
against the corresponding argument's synthesized type, and binds each `'T` to the
type it matched. When several arguments bind the **same** `'T`, their synthesized
types must be **identical** — inference does not join them to a common union even
when one is a member of the other (`Int8` and `Int` do not merge to `Int`), so the
type stays uniform and unification never searches a lattice (§5.5). If the arguments
leave a type variable **unconstrained or contradictory** (a non-uniform binding is
contradictory), inference fails and the call must spell the argument out explicitly. A comptime value parameter (`[n 'T]` size) is likewise fixed here from
an explicit argument or the context, and carried to specialize
([[order_of_compilation.md]]). Whether an inferred type argument **satisfies its
bound** — a bound is a `union` membership check — is §8.

### 5.5 Subtyping is minimal

The subtyping relation has only these forms:

1. **`Never` is a subtype of every type** (the bottom type — §2.3).
2. **A union member is a subtype of its union** — `Int8 <: Int`, `Int8 <: Number`
   (the spread makes the twelve leaves members of `Number`), `Bool.True <: Bool` (§8).
3. **A union is a subtype of a union whose members are a superset** — `Int <: Number`
   (union-subset subtyping; its precise rules are §8).

There is **no `Top`/`Any`** supertype, and **no numeric or width subtyping among the
concrete types** — no `Int8 <: Int64`, no `signed <: unsigned`, no
`Float32 <: Float64`. Every type not related by the rules above is invariant, and any
move between two such types is an explicit conversion (§4). In particular **function
types are invariant** in both domain and codomain — `(A) B` relates to `(A') B'` only
when `A ≡ A'` and `B ≡ B'`, with no contravariant-domain/covariant-codomain rule. So
`() Never` is *not* a subtype of `() R`: `Never <: R` is a rule about **values**, not
one that flows through a function's return position, and a diverging function is not
silently usable where a returning one is expected. This is the closure of
"no implicit conversions among concretes": unification (§5.3, §5.4) never has to
*search* a lattice. Widening happens in only two forms, neither a search — toward an
**expected** union (annotation/param/return), and the join of branches that are all
members of a **single** common union, whose unique home union names the target directly
(§5.3); branches spanning several unions widen only toward an expected union.

### 5.6 Operators and their types

**Arithmetic** (`+ - * / %`) and **bitwise** (`& | ^ << >>`) binary operators
require **both operands to be the same type** — a numeric type for arithmetic, an
integer type for bitwise — and yield that type. There is no implicit conversion
between operands: `a + b` with `a: Int32` and `b: Int64` is an error. An untyped
literal operand adopts the other operand's type; two untyped literals stay untyped
and take the expression's expected type, or their default `Iarch`/`Float64` if none
(§3). Unary `-` requires a
signed operand (§2.1), `~` an integer, both yielding the operand's type.

**Numeric operations never trap.** A wrong value is the developer's responsibility,
not a crash (a crash is the program's fault, a wrong value the programmer's), so
every arithmetic operator is **total** and returns a defined result even at the
boundary:

- **integer overflow wraps** (two's complement, §2.1);
- **integer `/ 0` and `% 0` are `0`** — a defined result on every target (the
  compiler emits a guard where the hardware would fault, e.g. x86; on arm64 the
  instruction already yields 0). Baffling but total, and the *same* comptime and at
  runtime — a comptime-known `/0` folds to `0`, it is not a compile error.
- **float operations follow IEEE-754** — producing `NaN` / `±Infinity` as values
  (§2.2), never a crash.

These defined-but-possibly-wrong results are consistent at comptime and runtime; the
compiler does **not** substitute its judgement for the developer's. When the
developer wants a *chosen* or *guaranteed* result instead, the std supplies it,
never the core operators: `wrapping_*` / `saturating_*` for explicit integer intent,
and `checked_*` (`checked_add`, `checked_div`, …) returning an `Either` whose `Left`
names the condition (overflow, divide-by-zero, non-finite). The `checked_*`/
`wrapping_*` families are a standard-library surface, not new type rules.

**Comparison.** Equality `==`/`!=` is **structural**: scalars by value, aggregates
field-wise, unions by tag then payload — yielding `Bool`. It is *almost* universal;
the one caveat is **floats follow IEEE** (§2.2), so `NaN != NaN` and a value (or an
aggregate) containing `NaN` is not self-equal — `==` is not reflexive on floats.
Ordering `< <= > >=` is defined **only on the twelve numeric types** (other types,
including `Str`, support only `==`/`!=`); on floats it is the IEEE **partial** order
(every comparison with `NaN` is false). Both yield `Bool`.

For the cases that need a *reflexive, total* comparison over floats — hash keys,
sorted containers, dedup — the standard library provides **`total_eq`** and
**`total_cmp`**, which compare the underlying bits (NaN a fixed value, `-0.0` and
`+0.0` distinguished), mirroring Rust's `f64::total_cmp`. They work on any type
**without a function-typed field** (the function-`==` ban below is transitive),
recursing field-wise, so a float-bearing aggregate can still key a map. The rule is:
IEEE `==` for arithmetic-style comparison, `total_*` when a container needs a total
order. This is a std surface, not a type rule — the type gate only fixes that bare
`==` on floats is the IEEE one.

**`==` on a function-typed value is a compile error.** Function equality is
undecidable (two functions are equal only if they agree on every input), and cf
functions are comptime-first-class and erased at specialize, so there is no runtime
function value to compare. Runtime polymorphism is *union + `match`*, not
function-pointers; if a pointer ever names a callable, that is pointer identity
(§4/§6.4), a separate question, not function-value `==`. The ban is **transitive**:
`==` (and `total_eq`/`total_cmp`) on any aggregate that transitively contains a
function-typed field is rejected too, since structural recursion would reach a field
with no equality.

**Logical.** `&&`, `||`, and unary `!` take `Bool` operands and yield `Bool`
([[ebnf.md]]); `&&`/`||` short-circuit. There is no truthiness — a non-`Bool` is not
an implicit condition, in an `if` or a logical operator.

## 6. Type constructors and construction

The compound types are built from the primitives (§2) by a small set of type
constructors, all of which compose freely. This section defines them and the one
uniform way a value of any type comes into being — **construction is application**.

> **Note.** Several forms here revise the ratified grammar of [[ebnf.md]]
> (tuple/unit syntax, the cast, construction-as-application, dropping the prefix
> record literal); those revisions have been propagated into [[ebnf.md]] directly
> (see Reconciliation status). The semantics are fixed here.

### 6.1 Parentheses: tuples, unit, and the argument shape

A **tuple** is a positional, heterogeneous product, written with parentheses:
`(a, b)`, `(Int32, Str)`. Parentheses — not brackets — because a tuple is the same
shape as an argument list and a function domain (§6.3), and one glyph serves all
three.

- **Unit is `()`** — the empty tuple, the terminal type `1`, its sole value also
  written `()`. It is named `Unit` (§2.3), and is the payload of a nullary union
  member (`Nothing`), the return of a function that yields nothing, and the argument
  of a nullary call (`f()` applies `f` to `()`).
- **A one-tuple is its element**: `(e) ≅ e`. The unary product is the identity, so
  grouping-parentheses and a one-tuple are the *same thing* — there is nothing to
  disambiguate, and no trailing-comma rule. `(4)` is `4`.
- **Two or more** elements form a genuine product: `(a, b)`, `(a, b, c)`.

Parentheses play two roles by position, exactly as elsewhere: in **expression**
position `(…)` is grouping / a tuple value; in **binding/pattern** position `(…)` is
a positional destructuring pattern (`const (a, b) = pair`). A bare name binds the
whole value; a parenthesized pattern pulls elements out by position. Destructuring is
**sugar for positional element extraction** — `const (a, b) = pair` desugars to
`const a = pair[0]` and `const b = pair[1]` (the comptime positional index of §6.2/§7.3)
— so there is no separate arity rule: each name resolves to one comptime tuple index, an
index past the tuple's arity is the ordinary comptime out-of-range error, and extra
trailing elements are simply left unbound.

Brackets are now **arrays only** (§6.2), which removes the old overload where `[…]`
meant tuple, fixed array, or dynamic array depending on content.

### 6.2 Brackets: arrays

Brackets build the homogeneous, indexable sequence types:

- **`[T]`** — a **dynamic array** of `T`: runtime length, bounds-checked indexing,
  iterable. Its empty value is `[]`.
- **`[N T]`** — a **fixed array** of comptime length `N` (`[4 Int32]`, or `[n 'T]`
  with a comptime value parameter): comptime length, bounds-checked indexing,
  iterable. A fixed array is a homogeneous tuple, but a distinct type — its length
  is in the type, and it iterates, where a tuple is accessed only at comptime
  positions.

A **length and an index are `Uarch`** (a non-negative pointer-width count): `[T]`/`[N
T]` report their length as a `Uarch`, and `xs[i]` expects a `Uarch` index — so an
index computed from narrower/signed data needs an explicit conversion (§4), like Rust's
`usize`.

`[N T]`, `[T]`, and `(T1, …, Tn)` remain distinct types with distinct
representations ([[memory_model.md]] enumerates `struct`, `array`, `tuple`,
`string`); the syntactic split (brackets = homogeneous sequence, parentheses =
positional product) now matches that type distinction directly.

### 6.3 Function types

A function type is a **domain tuple followed by a return type**: `(A, B) R` — take
`(A, B)`, return `R`. The domain being a tuple is the whole point: a call `f(a, b)`
applies `f` to the tuple `(a, b)`, so the function type and the call site share the
parenthesized-tuple shape.

- The **return type is mandatory in a function *type*** and follows a `->` after the
  domain group; a following `->` ⇒ function, none ⇒ the parentheses were just a
  tuple (one token of lookahead after the `)`). A nullary function is `() -> R`
  (domain unit); a unit-returning function is `(A) -> ()` — or `(A) -> Unit`, the
  readable name (§2.3).
- A function **value** (lambda) still writes `(params) [: Ret] -> body`; its return
  type is inferred from the body and usually omitted (§5.1), and — when stated — set
  off with a colon so the `->` is unambiguously the body. Only `asm`/`intrinsic`
  functions, which have no body, must write the return type.
- Function types **compose in type position** — `Map[Str, (Int32) -> Int32]` names a
  comptime mapping whose value type is a function, and a generic bound `'F` may be a
  function type. But there is **no runtime function pointer** — no `*((Int32) -> Int32)`:
  functions are comptime-first-class and erased at specialize (§5.6), and §6.4 admits
  no pointer to a non-aggregate, so a function type is used at comptime (type argument,
  bound, comptime container), never as a runtime pointer referent.

### 6.4 Pointers and references

- **`*T`** — a pointer. Per [[memory_model.md]] §6, pointers point **only to
  aggregates**, **only into a `let`**, and **one level deep**; there is no
  `*Scalar` (no `*Int32`), and no whole-value dereference (`.field` and `[i]`
  auto-dereference). A `*T` is a pointer-width address at runtime, but there is **no
  `*T`↔integer conversion and no pointer arithmetic** (§4) — a pointer can only reach
  its aggregate's fields/elements, never a computed address, so it can never escape
  the node it belongs to. The raw address is exposed only inside the `asm` floor.
- **`&T`** — a reference: the operator that hands a `let` aggregate to a `*T`
  parameter. `&c` on a `const` is an error (no by-reference of an immutable).

Writability is inferred from a pointer's referent — a `*T` is writable only if its
referent lives in a `let` — and checked at the call site ([[memory_model.md]] §6);
that is a binding/effect check, not a distinct pointer type.

### 6.5 Named and generic types

A **named type** is a `type_name`, optionally applied to type arguments with
**brackets**: `Int32`, `Map[Str, Int32]`, `Maybe['Value]`. Type application uses `[…]`;
value construction uses `(…)` (§6.6) — so `Vector[Uint8]` is *the type* "vector of
`Uint8`", while `Vector(xs)` *constructs* a vector value. Bracket-vs-parenthesis is
what keeps the type plane and the value plane from colliding even when a type is
generic. A **type variable** is `'T` (§5, generics); it appears only on the
accepting side of a definition.

### 6.6 Construction is application

Every value of a named or scalar type is produced by **applying that type's
constructor** — a value of type `T` is made by calling `T`. This one form replaces
the several construction syntaxes the grammar carried:

```
Uint8(0)                 # a scalar from a literal (total; the literal is typed by T)
Uint16(Uint8(1))         # a scalar from a scalar — a cast (widen exact; narrow truncates)
Point(1, 2)              # a POSITIONAL record from its fields (data Point = (Int32, Int32))
Person({ name: n, age: a })  # a NAMED record from a data literal
Maybe.Just(1)            # a union member from its payload
Maybe.Nothing            # a nullary member — no application needed
```

The **payload shape follows the declaration** (§7.3): a positional record/member
takes a tuple `T(a, b)`, a named one takes a data literal `T({ f: v })`. So there is
one construction form per type, not two.

- **Ascription is construction.** `const T x = v` is sugar for `const x = T(v)`
  (§4). The annotation *is* the constructor application, so literal typing (§3),
  casting (§4), and construction are one operation — `const Uint8 x = 0` and
  `const x = Uint8(0)` are the same. A bare `{ … }` in value position is no longer a
  typed record — it is a *payload*, typed by an enclosing `T({ … })` (or the
  annotation, §7.3).
- **A constructor is a function** — the type's type-theoretic introduction form,
  with the same standing as any function (`Maybe.Just` may be passed where a
  function is expected). It is **capture-free**, so ([[memory_model.md]] §7) it
  travels as an ordinary value; in practice it resolves and monomorphizes at
  comptime (never carrying a runtime environment), which is why passing one costs
  nothing.
- **Constructors are colorless — never `!`.** A constructor returns the value it
  builds, so everything it allocates **escapes via the return** and is charged to
  the caller's owner, not the constructor ([[memory_model.md]] escape rule). By that
  rule a function whose every allocation escapes is colorless, so no constructor
  ever carries `!`, and `const Point p = Point(1, 2)` needs none either.
- **The payload is a tuple.** A record's fields, a union member's payload, and a
  call's arguments are all the one argument-tuple shape: `Maybe.Just(1)` is a member over
  the one-tuple `(1)` (≅ `1`), `Node(l, v, r)` over `(l, v, r)`. Union payload
  storage is therefore tuple storage — the tag plus the payload tuple's layout
  (§8) — with no per-member-shape special case. This is a *semantic/surface*
  uniformity; it does **not** merge the runtime representations of records, tuples,
  and arrays (§6.2), and it does **not** erase records' named fields or nominal
  identity (a record payload is a *named* tuple; §7).

The per-kind details — record field checking and nominal-vs-structural typing
(§7), union member resolution and representation (§8) — build on this one form.

## 7. Aggregate-literal and data-literal typing

The old grammar packed fixed array, tuple, and dynamic array into one bracket
literal whose *kind* the type gate had to resolve. The move to parentheses for
tuples (§6.1) makes most of that a **syntactic** decision now: `(…)` is a tuple,
`[…]` is an array. What remains for the type gate is the array's fixed-vs-dynamic
choice, tuple element typing, and record-literal typing.

### 7.1 Tuple literals

`(e1, …, en)` is a tuple value; each element is typed **independently** (a tuple is
heterogeneous), so `(1, "hi")` in a `(Int32, Str)` context is `(Int32, Str)`. `()`
is unit, `(e)` is `e` (§6.1). A tuple element adopts its position's expected type
(§3) — the context is per-position, from an annotation or the callee's domain.

### 7.2 Array literals — fixed vs dynamic

`[e1, …, en]` is an array literal; all elements must **unify to one element type**
(arrays are homogeneous, §6.2), taken from the elements if they are already typed,
else from the expected element type (§3). The **kind** follows the context:

- a **fixed** expected type `[N T]`, or **no annotation at all** → a **fixed array**
  `[n T]` of the literal's own comptime-known length (the length is right there in
  the literal); with `[N T]`, that length must equal `N`. The element type comes from
  the elements, or their default (`Iarch`/`Float64`, §3) when they are untyped
  literals — so `const xs = [1, 2, 3]` is now a well-formed `[3 Iarch]`;
- a **dynamic** expected type `[T]`, the empty literal `[]` **with no fixed expected
  type**, or a literal that flows into a growing binding → a **dynamic array** `[T]`.
  An explicit `[N T]` context always wins over the `[]`-is-dynamic default: `[]`
  checked against `[0 T]` is the fixed empty array, not a dynamic one.

So `[1, 2, 3]` is `[3 Iarch]` (fixed) unless a `[T]` context or growth makes it
dynamic.

### 7.3 Record construction and data-literal typing

**Construction mirrors the declaration**, reusing the same tuple/record blocks so
one record has exactly one construction form:

| Declaration | Kind | Construct | Access |
| --- | --- | --- | --- |
| `data Point = (Int32, Int32)` | positional | `Point(1, 2)` | `p[0]`, `p[1]` |
| `data Point = { Int32 x, Int32 y }` | named | `Point({ x: 1, y: 2 })` | `p.x`, `p.y` |

A **positional** record is a nominal tuple — its payload is the positional tuple
`(…)`, constructed by applying the type to that tuple (`Point(1, 2)`) and indexed by
position. A **named** record is a nominal named-tuple — its payload is the **data
literal** `{ field: value, … }` (colon, per [[ebnf.md]]; the `=` you may recall is
the field *default* in the declaration, `Int32 x = 0`, deliberately distinct). Either
way the annotation sugar `const Point p = <payload>` is `Point(<payload>)` (§6.6).

- **Nominal vs structural — the question [[ebnf.md]] defers here — resolves through
  the existing `data`/`type` split.** A payload literal (`(…)` or `{…}`) is
  structural *syntax* with no type of its own; the named type in context supplies it.
  If that type is a **`data`** declaration, the value is **nominal** — `Point` and
  `Vec2` with the same shape are different types, and `data Meters = Uint32` is not a
  `Uint32` ([[ebnf.md]], Data & Type Declarations). If it is a **`type`** (a
  named-tuple alias), the
  value is **structural** and comptime-erased. A payload literal with **no** type
  source is an error, like a bare numeric literal (§3) — a record is never anonymous
  (and a bare `(1, 2)` with no type is just the tuple `(Iarch, Iarch)` by the default
  rule of §3, not a `Point`).
- **Named-record fields** are order-independent; each value is checked against its
  field's declared type (a literal adopts it, §3). **Puns** (`{ x }` ≡ `{ x: x }`)
  and **defaults** (a field with a `= default` in its declaration may be omitted) are
  per [[ebnf.md]]. A **value-level spread** (`{ ...src, z: 3 }`) splices a record
  value's fields into a new record; **fields match by name and later entries win** on
  a collision. The result's **type is the destination's**: an annotation (or a target
  field's type) fixes it, and every spliced-or-explicit field must belong to that type
  — a source field it lacks, or a field it needs but nothing supplies, is an error.
  With **no annotation** the type is *inferred* — a single source, or several of
  identical structure, supplies it, and you may **override** its fields but not **add**
  one (an extra field is an error); **sources of differing structure** cannot be
  inferred, so an explicit type is **required** and each is matched against it by name.
  Per [[ebnf.md]]'s `data_literal` value spread, distinct from the record-*declaration*
  spread and the bracket-aggregate spread.
  **Positional-record** fields are filled by position, like any tuple.

## 8. Unions, subtyping, and constraints

A `union` is an overarching type — a named set of member types ([[ebnf.md]]). A
value always inhabits **exactly one** member (a concrete subtype) carrying a tag
that says which; you never instantiate the union, only a member. A union serves two
roles: as a **generic bound** (`[SomeUnion 'T]` — "`'T` is any member", resolved at
comptime, §5.4) and as a **value type** (a param/field of union type accepts any
member and recovers which one with `match`, at runtime via the tag). **Constraints
are unions** — a bound *is* a membership check; there is no separate trait system.

### 8.1 The member model — resolve-or-create

A union body lists members; each is one of ([[ebnf.md]]):

- **A bare type** — *resolve-or-create*: if the name already denotes a type in scope
  (a primitive, an imported type, a declared `data`/`union`), the union **composes
  over** it and that existing type *is* the member (`Uint8` in `Uint`, `Float32` in
  `Float`). If the name denotes nothing, it is a fresh **nullary** member — a pure tag
  with no payload (`Nothing`, `Nil`). This resolution is **deliberately
  scope-sensitive**: a bare member name that matches a type in scope composes over it,
  and importing such a name is an explicit act — the compose-over is intended, not a
  trap to warn about.
- **A payload member**, whose payload mirrors a record declaration (§7.3): a
  **positional** `Name(T1, …, Tn)` (`Just('Value)`, `IntLit(Int32)`) or a **named**
  `Name = { type field, … }` (`IntLit = { Int32 value }`). Constructed and matched by
  the matching shape (`Maybe.Just(1)` / `Node.IntLit({ value: 5 })`).
- **A named compose-over or singleton `Name = <rhs>`** — `<rhs>` a **type** (`Wrap =
  Int32`) or a **literal** (a **singleton**, `Semicolon = ";"`, a tag recoverable as
  that constant).
- **A member spread** `...Other` splices another union's members ([[ebnf.md]]) — how
  `Number = { ...Int, ...Uint, ...Float }` is built.

```
union Maybe['Value]   = { Just('Value), Nothing }
union Either['L, 'R]  = { Left('L), Right('R) }
```

Members are **first-class types**, but *how you reach one depends on how it was
declared* — you **import the union, not its members**:

- A **compose-over member** is a standalone type the union only references, so it is
  reached **on its own** (`Int8`, `Float32` — you are naming the independent type,
  which happens to also be a member), and redundantly as `Uint.Uint8`.
- An **inline-declared member** — a nullary tag or a `Name = …` payload/type/singleton
  (`Just`, `Nothing`, `Left`, `Right`, `IntLit`) — exists **only within its union** and
  is reached **only qualified**: `Maybe.Just`, `Either.Left`, `Node.IntLit`. There is no
  bare `Just` or `Nothing`, in value **or** type position. This keeps the namespace
  clean (`Just` alone is meaningless; `Maybe.Just` is not) and keeps `A.X` and `B.X`
  distinct when `X` sits in both.

Either kind is a **first-class type usable anywhere a type is** — a param, field,
return, binding annotation, or type argument — a compose-over member bare (`Int8`), an
inline member only qualified (`Maybe[Int32].Just`, `Node.IntLit`). Because an inline
member is **bound to its union**, `Maybe.Nothing` and a different union's `Nothing` are
**distinct types** and case names never collide across unions — the case-name space is
the union's, not the module's. (A member value is still obtained by construction or by
`match` narrowing, never by a bare downcast — §8.3.)

**Generic placement is composable** — put the type parameter where the intent needs
it. A parameter on the **union head** flows into every member that uses it —
`union Maybe['Value] = { Just('Value), Nothing }`, so `Maybe[Int32]` has
`Just(Int32)`. Alternatively a parameter can live on a **member alone**, leaving the
union un-parameterized — `union Tree = { *Tree, Node['Value]('Value) }`, where `Tree`
is a concrete type but its `Node` member is generic (and `*Tree` is the recursive
link, §8.4). The two are the same mechanism; a member may carry its own generics
exactly as a standalone type would. A member *can* be extracted into a standalone
type when a design wants it (`type Just['Value] = ('Value)` composed over by
`Maybe`) — but `Maybe`'s and `Either`'s members are deliberately **not** extracted:
they stay inline, reachable only as `Maybe.Just` / `Either.Left` (the qualified-only
rule above).

A member is **constructed by application** (§6.6): `Maybe.Just(1)`,
`Node.IntLit({ value: 5 })`, `Maybe.Nothing` (nullary — no application).

A member-constructor expression **synthesizes the precise member type** —
`Maybe.Just(1) : Maybe[Iarch].Just` (the `1` taking its default, §3) — because the
member is statically known at the construction site: the value is exactly that case,
carries no tag, and **widens to its union** (`Maybe[Iarch]`) by member→union subtyping
(§8.2) wherever a union is expected (an annotation, param, return, or unification
against a sibling member). This is what lets a signature state a *precise* result —
`f() Maybe[Int32].Just` returns **only** `Just`, never `Nothing` — and lets the
compiler carry that exact case through for codegen (no tag, no dispatch). It also
still lets sibling-member branches unify with no annotation:
`match x { A -> Maybe.Just(1), B -> Maybe.Nothing }` is `Maybe[Iarch]` — the two
precise member types `Maybe[Iarch].Just` and `Maybe[Iarch].Nothing` join to their
**single common union** with no lattice search, since each lives only in `Maybe`
(§5.3). The member-type spelling carries the container's instantiation:
**`Maybe[Int32].Just`** (the parameter sits on `Maybe`), or **`Tree.Node[Int32]`**
when it sits on the member instead — the type argument goes where the parameter was
declared, so `Maybe.Just[Int32]` is *not* the spelling.

### 8.2 Subtyping

The union relation is what §5.5 rules 2–3 name, made precise:

- **A member is a subtype of its union** — `Int8 <: Int`, `Bool.True <: Bool` — and
  directly of a union that splices it in by spread (`Int8 <: Number`, since `...Int`
  flattens `Int8` into `Number`'s members).
- **A union is a subtype of a union whose members are a superset** — `Int <: Number`
  (every member of `Int` is a member of `Number`), so an `Int`-typed value or bound
  satisfies a `Number` one.

There is no other coercion (no numeric/width subtyping among the concrete leaves,
§5.5). Using a member value *as* its union may attach the discriminant tag (§8.4);
for an all-tag-only union the value already *is* the tag. **Union→superunion
subsumption stays a no-op** (`Int <: Number` re-uses the value, no re-tag) because
the representation gate encodes a shared member's tag identically in both unions
(§8.4) — the type system requires that consistency and defers the encoding.

### 8.3 `match` — dispatch, narrowing, exhaustiveness

A `match` inspects which member the scrutinee is. Each arm is a member `type_pattern`
**qualified by the scrutinee's union** that **narrows** the scrutinee to that member
and binds its payload — `Node.IntLit(v) -> …` binds `v` to an `IntLit`'s payload. Two
rules make the frame explicit:

- **Arms may only name members of the scrutinee's own union.** Matching an `Int` offers
  exactly `Int`'s five members; a `Number`, all twelve. The scrutinee's static union
  *is* the frame, so the checker never searches for which union a member belongs to —
  the type already says. (This is why sibling-member unification needs no lattice
  search, §5.3.)
- **The container qualifier is required** — `Int.Int32(x)`, never bare `Int32(x)`,
  and when matching a `Number` it is `Number.Int32` — so the syntax names the member
  *as a case of that union*, not as any-int. Narrowing binds the **precise member
  type**: in the arm the scrutinee *is* that member. (This applies to the **arm head**.
  A member matched inside a *nested* payload sub-pattern follows its field's rule instead
  — bare-or-qualified for a standalone union, always-qualified for an inline one; see
  [[ebnf.md]] § match, "Payload sub-patterns recurse".)

For a **compose-over** member the payload *is the value itself* (`Int.Int8(v)` binds
`v` to the `Int8`, since a compose-over member is not a wrapper — §8.1); for an
inline payload member it is the declared payload (`Node.IntLit(v)` binds the payload).

- **Exhaustiveness** is checked by the type gate ([[order_of_compilation.md]]): a
  `match` on a union must cover **every** member, or carry a **catch-all — the
  wildcard `_`** ([[ebnf.md]]); a non-exhaustive match without one is a compile error,
  and an arm made unreachable by an earlier `_` is likewise rejected. (The `...` in
  the examples below is elision of the remaining member arms, not a catch-all.) Under
  the comptime type-switch a `_` is allowed and simply pruned when a concrete `'T`
  makes it unreachable.
- **Resolution is as early as the information allows.** The two modes share the
  syntax but not the mechanism. When the member is **known at comptime** — a
  monomorphized generic whose `'T` is now concrete — the match is a **comptime
  type-switch** on `'T`'s identity: only the matching arm survives, the rest are
  pruned, at **zero runtime cost** (the scrutinee is a plain value, no tag). When the
  scrutinee is a genuine **runtime** union value (tag + payload), it dispatches on the
  tag. Resolved at compile time where it can be:

```
pub const describe = [Int 'T]('T a, 'T b) 'T -> match a {   # 'T bound by the Int union
  Int.Int8(v)  -> v + b,
  Int.Int32(v) -> v - b,
  ...                          # at describe[Int32], only the Int32 arm survives
}
```

Because the scrutinee **is** `'T`, matching an arm refines `'T` *itself* to that arm's
member for the arm's body: in `Int.Int8` the gate reads `'T ≡ Int8`, so the other
`'T`-typed binding `b` is also `Int8` and `v + b` is `Int8 + Int8`. This is what lets
the body be checked **once, at the definition site** — the surviving arm in each
monomorphization is the one whose member equals `'T`, so no per-instantiation error is
deferred.

### 8.4 Representation

- **An all-tag-only union** — every member nullary or a literal singleton, no
  payload — lowers to a plain **integer** (the tag). `Bool`, and AST-kind unions like
  `NodeKind`/`TokKind`, cost exactly a small int.
- **A union with any payload member** is a **tag + payload aggregate**: a discriminant
  plus the payload, sized to `tag + max(member payload)`, placed under the ambient
  geometry and passed by pointer like a record ([[memory_model.md]] — placement is
  the geometry's, not necessarily an arena). The payload is a tuple or named record
  (§7); member→union subsumption writes the tag.
- **A pointer to an all-tag-only union is illegal.** Since such a union *is* a scalar
  integer (above), `*Bool` / `*NodeKind` would be a `*Scalar`, which §6.4 forbids —
  only a **payload-bearing** union (a tag+payload aggregate) can be a pointer's
  referent, as in the recursive `*Node` case below. The type gate rejects `*U` for an
  all-tag-only `U`.
- **Recursive unions are legal** — the AST case. Because a payload union is an
  aggregate (tag+payload), a member may hold a **pointer to its own union**
  (`BinOp = { *Node left, *Node right }` in a `union Node`). A *by-value* self-reference
  never has to be forbidden as a special rule: it simply **cannot be expressed** — a
  union value is a by-pointer aggregate, so a member's own-union field is always a
  `*Node`, never an inline `Node` of unbounded size.
- The **exact byte layout** (tag width, alignment, payload packing, niche
  optimizations) is the aggregate-representation gate (M6/M9); this spec fixes the
  *shape* (all-tag → int, else tag+payload) and the typing, not the bytes. That gate
  also assigns tags **consistently across sub/superset unions** — a shared member's
  discriminant has the same encoding in the sub- and super-union (`Number`'s tag for
  `Int8` equals `Int`'s) — so that union→superunion subsumption (`Int <: Number`,
  §8.2) is a no-op reinterpret rather than a re-tag. The type system fixes that
  *requirement*; the gate owns the bytes.

### 8.5 Constraints are unions

A generic bound `[U 'T]` requires the type argument to be a **member of the union
`U`** (§5.4) — bound satisfaction *is* union membership, checked at instantiation.
There is no separate trait, typeclass, or interface system: to say "any number, any
non-negative integer, any of these three shapes," you name the union of them.
`[Number 'T]`, `[NonNegativeInteger 'V]`, and a user's `[Drawable 'S]` are the one
mechanism. As a **bound** a union is comptime (it drives monomorphization); as a
**value type** it is runtime (tag dispatch) — the same union, used two ways.

**A union bound exists to be *narrowed by `match`*, and that is its only use inside
the body.** A `'T` bounded by a union is not directly operable — the union does not
define `+`, `.field`, etc. across its members — so the body **matches on `'T` to
narrow to a concrete member**, then operates on that member's own type:

```
pub const negate = [Int 'T]('T x) 'T -> match x {   # scrutinee's union is Int (signed)
  Int.Int8(v)  -> -v,   # here v is an Int8; -v is Int8 arithmetic
  Int.Int64(v) -> -v,   # here v is an Int64
  ...                   # qualified by Int — the arms name Int's members
}
```

The bound is `Int`, not `Number`, on purpose: unary `-` is undefined on the unsigned
leaves (§2.1), so a `Number` bound could not be negated in every arm. **A bound is
exactly the set of members whose own operations the body uses** — narrowing to a
member you cannot operate on is the mistake the bound prevents. Each arm is then
checked **once**, against the type it narrows to — so the body is checked at the
definition site (no "does every member support `-`" rule, and no deferred
per-instantiation errors: a function's signature stays its readable interface, §5).
Trying to write `x + y` directly on an `Int`-bounded `'T` is a type error — narrow
first.

### 8.6 The standard numeric and boolean unions

The union system offers two ways to give a union its members: **declare them inline**
(reachable only through the union — §8.1), or **declare each type separately and reuse
it** inside the union (compose-over). The **numeric** unions use the *latter* — every
width (`Int8`, `Int32`, `Iarch`, …) is a **standalone, directly-usable type**
(`const Int32 x = …`) the union merely composes over, so the widths are first-class on
their own and the union just adds the "any of them" bound. **`Bool` uses the former** —
`False` and `True` are **inline nullary singletons**, reached only qualified
(`Bool.False`, or the `true`/`false` sugar), not standalone types. Either way the
member set is:

```
pub union Int    = { Int8, Int16, Int32, Int64, Iarch }
pub union Uint   = { Uint8, Uint16, Uint32, Uint64, Uarch }
pub union Float  = { Float32, Float64 }
pub union Number = { ...Int, ...Uint, ...Float }        # union (∪) of all 12 leaves
pub union Bool   = { False, True }                      # two nullary singleton members
```

- **`Int`/`Uint`/`Float`/`Number`** give width-generic functions: `[Number 'T](…)`
  is "any numeric type", monomorphized per type (§2.1, §2.2). A value is a `Number`
  iff its type is one of the twelve leaves; the spread `...Int` splices the members.
- **`Bool`** is `{ False, True }` — both nullary, so all-tag-only, so it lowers to a
  one-byte integer: a boolean costs exactly what a primitive would while *being* an
  ordinary union. `true`/`false` are literal sugar for `Bool.True`/`Bool.False`; `if`
  / `&&` / `match` on a `Bool` are union dispatch (which is why there is no
  truthiness, §5.6).
- The float exceptional-value union `FpClass = { Finite = Float64, PosInfinity,
  NegInfinity, NaN }` (§2.2) is the opt-in matchable view over a raw-IEEE float, via
  `classify`. It composes over the `NaN` singleton type directly and splits the
  `Infinity` singleton by sign into its `PosInfinity`/`NegInfinity` arms.

## 9. comptime-known-ness and ownership interactions

Two of the gate's obligations reach past pure type-checking into the arcs
downstream, yet both are still **annotate-and-reject** duties — the gate transforms
nothing (§1). It classifies **comptime-known-ness** so that specialize has a
well-defined worklist (§9.1), and it enforces the **type-level half** of the
mutability/ownership rules whose runtime meaning is [[memory_model.md]] §6 (§9.2).
The region-level half — no-dangling — needs concrete node identities and is *not*
this gate's; it waits for the memory arc (§9.2, last paragraph).

### 9.1 comptime-known-ness

Some type-forming positions demand a value fixed before runtime. The gate
classifies every expression as **comptime-known or not**, rejects a runtime value
where a comptime one is required, and marks the comptime-known ones so specialize
knows which to evaluate ([[order_of_compilation.md]], Specialize —
"comptime-for-instantiation"). It **classifies; it does not evaluate** (evaluation
is specialize's, or fold's) — it only certifies that a value *will* be available. A
comptime computation that diverges or cycles is therefore not the gate's to catch;
specialize's termination guard is ([[order_of_compilation.md]]).

Comptime-known is a small closure over syntax and bindings:

- a **literal** — any scalar (§3), or an aggregate literal all of whose parts are
  comptime-known;
- a **`const` binding whose initializer is comptime-known**, transitively. `const`
  is not *automatically* comptime — `const x = read_line()` is runtime-immutable but
  not comptime-known; `const x = 4` is both. Comptime-known-ness follows the value,
  not the `const` keyword;
- a **comptime value parameter** in scope — the `n` of `[n 'T]` is comptime *by
  declaration*, a specialization axis, even though its concrete value is unknown
  until specialize mints the instance;
- a **type** (every type is comptime — §5.4, §8.5), and a **call whose callee and
  arguments are all comptime-known** — C! has no comptime/runtime function split
  ([[order_of_compilation.md]]), so a function applied to comptime-known arguments
  yields a comptime-known result, evaluated at specialize (under its termination
  guard), never here. There is no `comptime` marker on a function; comptime-known-ness
  is a property of the call, not a declared category.

A `let` binding, a runtime parameter, and anything derived from one are **not**
comptime-known. Three positions require the property:

1. **A fixed-array length `N` in `[N T]`** (§6.2). `[4 Int32]` and `[n 'T]` are
   *types* only because `4`/`n` are comptime-known; `[len xs]` with a `let len` is
   not a type — either the length is comptime, or the array is the dynamic `[T]`.
   This is exactly the fixed-vs-dynamic decision the gate owns (§1, §7.2).
2. **A type argument** — explicit `f[Int32](…)` or inferred `f(…)` (§5.4). A type is
   comptime by nature, so this is automatic; the gate's duty is only that an
   *inferred* `'T` resolve to a concrete type, never a runtime value.
3. **A comptime value argument** — where a callee declares `[n 'T]`, the caller's
   `n` argument must be comptime-known (§5.4), carried to specialize as part of the
   key `(fn, type-args, value-args, capture-env)` ([[order_of_compilation.md]]).

Where the gate cannot certify the property for such a slot, it is a **type error at
the definition or call site** — the same "spell it out or it fails" discipline as
inference (§5.4), never a deferred per-instantiation error (§8.5).

The classification also underwrites the **comptime type-switch** (§8.3, §8.5): a
`match` whose scrutinee's member is comptime-known — a monomorphized `'T` now
concrete — resolves at compile time to the one surviving arm at zero runtime cost.
The gate marks the scrutinee comptime-known; specialize does the pruning.

### 9.2 Mutability and ownership at the type level

[[memory_model.md]] §6 gives the runtime meaning of `let`/`const`, pointers, and
by-value views. Its **static half is this gate's** — the `const`/immutability and
lvalue checks of §1. The gate enforces four rules; every one is annotate-and-reject,
none rewrites the tree.

**Mutability is a property of the binding, not the type** ([[memory_model.md]]). The
gate carries a `let`/`const` attribute on each binding and lvalue, *separate from its
type* — `Point` is one type whether its home is `let` or `const`. The attribute is
**transitive through aggregates**: a `.field` or `[i]` reached from a `const`
aggregate is const, from a `let` is let, all the way down — `const` recursively
immutable, `let` recursively mutable, with no per-field `mut` markers to reconcile.

**Write-capability of a pointer is inferred, then checked at the call site.** There
are no `mut` annotations ([[memory_model.md]]). The gate infers from a `*T`
parameter's body whether it **writes** through the pointer, and checks each argument
against that verdict: a writing body demands the `&` of a `let` aggregate; passing a
`const`'s pointer is a type error. The write-verdict for each `*T` parameter is a
**derived part of the function's interface** — computed from its body, including the
verdicts of any callee it forwards the pointer to, and available at every call site.
It is a binding/effect-level property carried alongside the signature and computed
bottom-up over the call graph; §5's read-a-signature-locally guarantee is about
*type* inference and is untouched. Two structural restrictions fall out and are
enforced here:

- **No `*Scalar`** — `*Int32`, `*Uint8`, … are not types (§6.4). A pointer's
  referent is always an aggregate; a scalar is passed by value, or lives as a field
  of a pointed-to aggregate.
- **No `&` of a `const`** — `&c` on a `const` is an error, so a `*T` argument is
  always the address of a `let` aggregate: exactly the writable case.

**The no-second-bind rule.** Binding an **aggregate** to a second variable — `const
y = x` or `let y = x` where `x` is an aggregate — is a type error: that would be a
borrow, and C! has none ([[memory_model.md]]). A **scalar** rebinds freely (it is a
by-value copy). To get your own instance of an aggregate, `copy` it.

**By-value is a read-only view; `copy`/`copy_transitive` make it writable.** A by-value
parameter — and any by-value binding — is a **read-only view** regardless of the
caller's `let`/`const`; the gate treats it as const for lvalue purposes. To mutate,
take your own instance with **`copy`** or **`copy_transitive`**, which the gate types as a
**fresh, writable instance** of the same type (`copy x : T`, `copy_transitive x : T` for
`x : T` — both are identity on the type). The result is writable when its home is a
`let`, because **rehoming adopts the destination's rule** ([[memory_model.md]]): a
value moved into a `const` aggregate becomes const, into a `let` aggregate becomes
let. The gate re-derives the mutability attribute from the destination home, never
from the source.

**What `copy` versus `copy_transitive` actually duplicate is [[memory_model.md]]'s to
define, not the type gate's — and it is settled there (§6).** Both clone the entire
*owned* value; they differ only at **`*T` reference edges**, which `copy` reproduces as
**aliases** and `copy_transitive` follows and clones (so its result shares nothing with
the source). Neither creates a second owner — a `*T` referent is owned by a `let`
elsewhere, not *inside* the aggregate, so `copy`'s aliasing is the permitted
multi-pointer case, and an inline owned sub-aggregate is duplicated outright. `copy` is
therefore legal on any aggregate, pointer-bearing or not; the copying *behaviour* across
pointers is the memory model's, the fresh-instance *type* is the gate's.

Finally, the **region-level** half of ownership — the no-dangling / region-outlives
check — is **not** this gate's. It needs concrete node identities and runs in the
memory arc, before duplication ([[order_of_compilation.md]], Memory;
[[memory_model.md]] §6). The type gate settles **mutability and lvalue legality**;
the memory arc settles **lifetime**.

## Appendix. Reconciliation status

The decisions in this spec that revised the ratified [[ebnf.md]],
[[order_of_compilation.md]], and [[memory_model.md]] have been **propagated into those
documents in place** (2026-07); each now stands on its own with no dependency on this
appendix. The former Appendix A (24 grammar items) and Appendix B (3 pipeline items)
were a working catalogue for that one coordinated pass and are **retired** now that it
is applied — the itemized change record lives in git history.
