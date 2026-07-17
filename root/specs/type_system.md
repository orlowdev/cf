# C! Type System

This document defines the types of C! and the rules the **typecheck gate** applies.
It is the semantics behind the type *grammar* of [[ebnf.md]] (which only shapes the
syntax and repeatedly defers "this is a type-system decision" here) and it sits
against the memory semantics of [[memory_model.md]] (aggregate-vs-scalar, ownership,
the `!` allocation effect). In the pipeline of [[order_of_compilation.md]] it is
phase 3, `Typecheck & bind` — a **gate**: it annotates the tree and rejects the
ill-typed, and transforms nothing (the `.cf` before and after is identical).

Status: design in progress. Written in steps — sections marked **(TBD)** are
placeholders to be filled in later rounds and are not yet ratified.

## 1. What the type gate owns

Per [[order_of_compilation.md]] §4 the type gate owns, and this spec defines:

- **type inference and checking** (§5) — including that an integer literal takes its
  type from context (§3);
- the **casing rule** — a `type_name` is PascalCase, a `var_name` snake_case
  ([[ebnf.md]]); types and values never collide because they are lexically split;
- **`const`/immutability and lvalue checks** — the type-level half of the ownership
  rules whose runtime meaning is in [[memory_model.md]] §6 (§9 TBD);
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
| `Arch`  | `Uarch`  | the target's pointer width |

- **`Arch`/`Uarch` are pointer-width** — the natural word of the target (64-bit on
  cf0's sole target `arm64-apple-darwin`), the types for addresses, sizes, and
  counts. `Arch` is a *distinct type* from `Int64` even where they share a
  representation (like Rust's `isize` vs `i64`): pointer-width is a semantic
  property, not "64", so a move between them is an explicit conversion (§4).
- **`Int`/`Uint` are the *unions*, not concrete types.** `Int` is the std union of
  all signed widths, `Uint` of all unsigned, defined in §8:
  `Int = { Int8, Int16, Int32, Int64, Arch }` and likewise `Uint`. They are the
  **generic bounds** ("any signed integer"), never a runtime value type — a
  `[Int 'T]` function is parametric over one width, monomorphized (§5.4, §8). There
  is no bare `Int` value; everyday code names a width (`Arch`, `Int32`, …).
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
  a compile error.
- **No expected type is an error.** When nothing supplies a numeric expected type —
  an unannotated `const x = 5`, or a bare literal expression with no typed
  context — the literal is rejected: it must be annotated (`const Int32 x = 5`) or
  reached through a typed context. There is no default integer type.
- **Propagation through operators.** An expected type propagates into the operands
  of arithmetic over untyped literals (`const Int8 y = 5 + 3` types both `5` and
  `3`, and the result, as `Int8`); a literal combined with an already-typed value
  takes that value's type. The full propagation/unification mechanics are §5.2/§5.6;
  the rule here is only that a literal never invents its own type.

A **float literal** (`3.14` — decimal with a point) follows the same rule: untyped
until a context supplies `Float32` or `Float64`, then a value of that type, subject
to the same "no expected type is an error" and range checks. An **integer literal
(no point) may also take a float expected type** — `const Float64 x = 5` is `5.0`,
an exact float value — since a literal has no intrinsic width; a **float literal
never takes an integer type** (a point means a real).

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
`Int64`, an `Int32` does not silently become a `Uint32` (nor `Arch` an `Int64`), and
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
  concrete type: `const p = make_point()` infers `Point`; but `const x = 5` (a bare
  literal — §3), `const xs = []` (an empty aggregate), or any initializer that only
  *checks against* an expected type without producing one, requires an annotation.

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
literal that reaches neither an annotation nor a typed neighbour is an error (§3) —
there is no default numeric type.

### 5.3 Unifying branches and match arms

`if c then A else B`, and the arms of a `match`, must combine to a **single**
result type. The combining rule is deliberately narrow — it never performs an
implicit conversion:

- Two branches of the **same type** combine to that type.
- A **`Never`** branch (a `break` or `continue` — the never-typed expressions of
  [[ebnf.md]] — or a diverging call) is subsumed: the result is the other
  branch's type. `if c then 1 else break` has whatever type the context gives `1`
  (say `Int32`).
- When the context **expects a union**, branches whose types are **members** of
  that union combine to the union (member-to-union subtyping, §8) — this is the
  only widening, and it only happens toward an expected union, never invented.
- Branches of **different, unrelated types** — including two different numeric
  types (`Int32` vs `Int64`) — do **not** combine; it is a type error, and the fix
  is an explicit conversion (§4). No least-upper-bound is searched for.

An **else-less `if`** has no second branch to unify, so it yields **`Unit`**: the
`then` branch runs for effect when the condition holds, and the whole expression is
`()` either way. (It does *not* wrap the branch in a `Maybe` — that overhead bought
nothing; a value you want to keep uses a full `if`/`else` or a `match`.)

### 5.4 Generic type-argument inference

A call may give type arguments explicitly (`max[Int32](a, b)`) or leave them to be
**inferred** from the value arguments (`max(a, b)`) ([[ebnf.md]]). Inference unifies
each parameter's declared type (which mentions the callee's type variables `'T`)
against the corresponding argument's synthesized type, and binds each `'T` to the
type it matched. If the arguments leave a type variable **unconstrained or
contradictory**, inference fails and the call must spell the argument out
explicitly. A comptime value parameter (`[n 'T]` size) is likewise fixed here from
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
move between two such types is an explicit conversion (§4). This is the closure of
"no implicit conversions among concretes": unification (§5.3, §5.4) never has to
search a lattice — the only widening is toward an *expected* union.

### 5.6 Operators and their types

**Arithmetic** (`+ - * / %`) and **bitwise** (`& | ^ << >>`) binary operators
require **both operands to be the same type** — a numeric type for arithmetic, an
integer type for bitwise — and yield that type. There is no implicit conversion
between operands: `a + b` with `a: Int32` and `b: Int64` is an error. An untyped
literal operand adopts the other operand's type; two untyped literals stay untyped
and take the expression's expected type (error if none — §3). Unary `-` requires a
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
`+0.0` distinguished), mirroring Rust's `f64::total_cmp`. They work on any type,
recursing field-wise, so a float-bearing aggregate can still key a map. The rule is:
IEEE `==` for arithmetic-style comparison, `total_*` when a container needs a total
order. This is a std surface, not a type rule — the type gate only fixes that bare
`==` on floats is the IEEE one.

**`==` on a function-typed value is a compile error.** Function equality is
undecidable (two functions are equal only if they agree on every input), and cf
functions are comptime-first-class and erased at specialize, so there is no runtime
function value to compare. Runtime polymorphism is *union + `match`*, not
function-pointers; if a pointer ever names a callable, that is pointer identity
(§4/§6.4), a separate question, not function-value `==`.

**Logical.** `&&`, `||`, and unary `!` take `Bool` operands and yield `Bool`
([[ebnf.md]]); `&&`/`||` short-circuit. There is no truthiness — a non-`Bool` is not
an implicit condition, in an `if` or a logical operator.

## 6. Type constructors and construction

The compound types are built from the primitives (§2) by a small set of type
constructors, all of which compose freely. This section defines them and the one
uniform way a value of any type comes into being — **construction is application**.

> **Note.** Several forms here revise the ratified grammar of [[ebnf.md]]
> (tuple/unit syntax, the cast, construction-as-application, dropping the prefix
> record literal). Every such change is listed in the ebnf-reconciliation appendix
> (§A) for one coordinated grammar pass; the semantics are fixed here.

### 6.1 Parentheses: tuples, unit, and the argument shape

A **tuple** is a positional, heterogeneous product, written with parentheses:
`(a, b)`, `(Int, Str)`. Parentheses — not brackets — because a tuple is the same
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
whole value; a parenthesized pattern pulls elements out by position. (Whether a
pattern shorter than the tuple binds a prefix or must match the arity exactly is a
destructuring rule, deferred to the pattern section.)

Brackets are now **arrays only** (§6.2), which removes the old overload where `[…]`
meant tuple, fixed array, or dynamic array depending on content.

### 6.2 Brackets: arrays

Brackets build the homogeneous, indexable sequence types:

- **`[T]`** — a **dynamic array** of `T`: runtime length, bounds-checked indexing,
  iterable. Its empty value is `[]`.
- **`[N T]`** — a **fixed array** of comptime length `N` (`[4 Int]`, or `[n 'T]`
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

- The **return type is mandatory in a function *type*** and is whatever follows the
  domain group; a following type ⇒ function, none ⇒ the parentheses were just a
  tuple (one token of lookahead after the `)`). A nullary function is `() R`
  (domain unit); a unit-returning function is `(A) ()` — or `(A) Unit`, the
  readable name (§2.3), to avoid the `() ()` reading.
- A function **value** (lambda) still writes `(params) [Ret] -> body`; its return
  type is inferred from the body and usually omitted (§5.1). Only `asm`/`intrinsic`
  functions, which have no body, must write the return type.
- Higher order composes: `*(Int32) Int32` a pointer to a function, `[(Int32) Int32]`
  an array of them, `Map[Str, (Int32) Int32]` a generic over one.

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
  the literal); with `[N T]`, that length must equal `N`. The element type still
  needs a source (§3);
- a **dynamic** expected type `[T]`, or the empty literal `[]`, or a literal that
  flows into a growing binding → a **dynamic array** `[T]`.

So `[1, 2, 3]` is `[3 T]` (fixed) unless a `[T]` context or growth makes it dynamic.

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
  (and a bare `(1, 2)` with no type is just the tuple `(Int32, Int32)`, not a `Point`).
- **Named-record fields** are order-independent; each value is checked against its
  field's declared type (a literal adopts it, §3). **Puns** (`{ x }` ≡ `{ x: x }`)
  and **defaults** (a field with a `= default` in its declaration may be omitted) are
  per [[ebnf.md]]. A **value-level spread** (`{ ...other, z: 3 }` splices another
  record's fields; later entries win on a collision) is a grammar addition (§A) —
  ebnf currently has spread only in a record *declaration* and in bracket aggregates.
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
  with no payload (`Nothing`, `Nil`).
- **A payload member**, whose payload mirrors a record declaration (§7.3): a
  **positional** `Name(T1, …, Tn)` (`Just('Value)`, `IntLit(Int32)`) or a **named**
  `Name = { field: type, … }` (`IntLit = { Int32 value }`). Constructed and matched by
  the matching shape (`Maybe.Just(1)` / `Node.IntLit({ value: 5 })`).
- **A named compose-over or singleton `Name = <rhs>`** — `<rhs>` a **type** (`Wrap =
  Int32`) or a **literal** (a **singleton**, `Semicolon = ";"`, a tag recoverable as
  that constant).
- **A member spread** `...Other` splices another union's members (§A) — how
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
  bare `Just` or `Nothing`. This keeps the namespace clean (`Just` alone is meaningless;
  `Maybe.Just` is not) and keeps `A.X` and `B.X` distinct when `X` sits in both.

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
they stay inline, reachable only as `Maybe.Just` / `Either.Left` (§8.6).

A member is **constructed by application** (§6.6): `Maybe.Just(1)`,
`Node.IntLit({ value: 5 })`, `Maybe.Nothing` (nullary — no application).

A member-constructor expression **synthesizes its union type, not the member** —
`Maybe.Just(1) : Maybe[Int32]`, not `Maybe.Just[Int32]`. This is what lets branches
that build *different* members of one union unify with no annotation: `match x { A ->
Maybe.Just(1), B -> Maybe.Nothing }` has type `Maybe[Int32]` (each arm synthesizes
`Maybe[Int32]`, so §5.3's same-type rule combines them). A member type appears on its
own only in a `match` `type_pattern` (§8.3), where it narrows.

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
for an all-tag-only union the value already *is* the tag.

### 8.3 `match` — dispatch, narrowing, exhaustiveness

A `match` inspects which member the scrutinee is. Each arm is a member `type_pattern`
that **narrows** the scrutinee to that member and binds its payload — `Node.IntLit(v)
-> …` binds `v` to an `IntLit`'s payload. For a **compose-over** member the payload
*is the value itself* (`Int.Int8(v)` binds `v` to the `Int8`, since a compose-over
member is not a wrapper — §8.1).

- **Exhaustiveness** is checked by the type gate ([[order_of_compilation.md]]): a
  `match` on a union must cover **every** member, or carry a catch-all — a
  non-exhaustive match without one is a compile error.
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

### 8.4 Representation

- **An all-tag-only union** — every member nullary or a literal singleton, no
  payload — lowers to a plain **integer** (the tag). `Bool`, and AST-kind unions like
  `NodeKind`/`TokKind`, cost exactly a small int.
- **A union with any payload member** is a **tag + payload aggregate**: a discriminant
  plus the payload, sized to `tag + max(member payload)`, placed under the ambient
  geometry and passed by pointer like a record ([[memory_model.md]] — placement is
  the geometry's, not necessarily an arena). The payload is a tuple or named record
  (§7); member→union subsumption writes the tag.
- **Recursive unions are legal** — the AST case. Because a payload union is an
  aggregate (tag+payload), a member may hold a **pointer to its own union**
  (`BinOp = { *Node left, *Node right }` in a `union Node`). A *by-value* self-reference
  never has to be forbidden as a special rule: it simply **cannot be expressed** — a
  union value is a by-pointer aggregate, so a member's own-union field is always a
  `*Node`, never an inline `Node` of unbounded size.
- The **exact byte layout** (tag width, alignment, payload packing, niche
  optimizations) is the aggregate-representation gate (M6/M9); this spec fixes the
  *shape* (all-tag → int, else tag+payload) and the typing, not the bytes.

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
pub const negate = [Number 'T]('T x) 'T -> match x {
  Int.Int8(v)  -> -v,      # here v is an Int8; -v is Int8 arithmetic
  Float.Float32(v) -> -v,  # here v is a Float32
  ...
}
```

Each arm is checked **once**, against the type it narrows to — so the body is
checked at the definition site (no "does every member support `-`" rule, and no
deferred per-instantiation errors: a function's signature stays its readable
interface, §5). Trying to write `x + y` directly on a `Number`-bounded `'T` is a type
error — narrow first.

### 8.6 The standard numeric and boolean unions

The union system offers two ways to give a union its members: **declare them inline**
(reachable only through the union — §8.1), or **declare each type separately and reuse
it** inside the union (compose-over). The **numeric** unions use the *latter* — every
width (`Int8`, `Int32`, `Arch`, …) is a **standalone, directly-usable type**
(`const Int32 x = …`) the union merely composes over, so the widths are first-class on
their own and the union just adds the "any of them" bound. **`Bool` uses the former** —
`False` and `True` are **inline nullary singletons**, reached only qualified
(`Bool.False`, or the `true`/`false` sugar), not standalone types. Either way the
member set is:

```
pub union Int    = { Int8, Int16, Int32, Int64, Arch }
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

## 9. comptime-known-ness and ownership interactions (TBD)

How the type system surfaces comptime-known values (array sizes `[n 'T]`, comptime
parameters) for specialize, and the type-level half of the mutability/ownership
rules ([[memory_model.md]] §6): transitivity through aggregates, the no-second-bind
rule, `copy`/`copy_deep`.

## Appendix A. ebnf reconciliation

This spec settles several decisions that revise the ratified grammar of
[[ebnf.md]]. They are collected here so the grammar is amended in **one coordinated
pass** (piecemeal edits would leave the grammar transiently inconsistent), rather
than as this spec is drafted. Each item names the semantics (fixed here) and the
grammar surface it touches.

1. **Tuples move from brackets to parentheses.** `[T1, …, Tn]` → `(T1, …, Tn)`;
   tuple *values* likewise `(a, b)`. `bracket_type`/`aggregate` lose the `tuple`
   arm; `[…]` becomes arrays only (`array`, `fixed_array`). The `type` production
   **gains a paren-tuple arm**, and `func_type` is no longer "a `(` at type position
   always starts a func_type" (ebnf's current rule) — a paren group is a **tuple type**
   unless a return type follows (`(A, B)` tuple vs `(A, B) R` function); the same
   one-token lookahead governs `data_body`. The **pattern** grammar follows too: a
   `(…)` destructuring pattern (tuples/positional records) forks from the `[…]`
   `array_pattern` — `pattern`/`array_pattern`/`destructure_decl` gain the paren form.
   (§6.1, §6.2, §6.3, §7.1)
2. **Unit is `()`, not `[]`.** The empty tuple / terminal type is `()`; `[]` is now
   the empty *dynamic array* value. (§6.1)
3. **A one-tuple is its element** (`(e) ≅ e`) — grouping-parentheses and one-tuples
   coincide, so no separate one-tuple form and no trailing-comma rule. (§6.1)
4. **Unit `()` is named `Unit`** (replacing `Void`) — the "returns nothing"
   spelling; not a distinct type. Every `Void` in the grammar/examples becomes
   `Unit` / `()`. (§2.3, §6.3)
5. **Construction is application `T(...)`.** Add `type_name` as a callee (a
   construction expression); the parenthesized argument list is the payload tuple.
   Casing keeps it unambiguous (PascalCase callee = construction, snake_case =
   call); **type application stays `T[...]`** (brackets), value construction `T(...)`
   (parens). (§6.5, §6.6)
6. **A bare brace literal in value position no longer means "typed record."**
   Construction is `T(…)` (or the annotated sugar `const T x = …`), so the
   annotation-driven `const Point p = { … }` becomes `Point({ … })` and the
   union-member juxtaposition `Node.IntLit { … }` becomes `Node.IntLit({ … })`.
   (ebnf has no `Point { … }` prefix form to remove — plain records are annotation-
   driven today; the change is that a `{ … }` in value position is now only a
   *payload*, typed by an enclosing `T(…)`.) (§6.6, §7.3)
7. **Ascription is sugar for construction:** `const T x = v` ⟶ `const x = T(v)`. A
   `var_decl`'s leading type is a desugar, not an independent typing act. (§4, §6.6)
8. **The cast is `T(x)`** (no new `as`/intrinsic form); narrowing truncates
   (a comptime-known out-of-range narrowing **folds** to its truncated value, like
   comptime overflow — not a compile error), the `checked`-style guaranteed
   conversion is a module function returning `Either`. (§4)
9. **`Maybe`/`Either` for `Option`:** rename `Option`→`Maybe`, `Some`→`Just`,
   `None`→`Nothing`, and **add** `Either`/`Left`/`Right` (ebnf has no `Result` to
   replace). These are library unions; ebnf's `Option`/`Some`/`None` references are
   renamed — **and this ripples into [[order_of_compilation.md]]'s desugar catalog**,
   which also names `Option`/`Some`/`None`. **The else-less-`if` rule changes**: ebnf
   (and order_of) desugar `if c then v` → `Option`; this spec makes it yield **`Unit`**
   (§5.3) — the branch runs for effect, no wrapping. (§3/§5.3; details in §8)
10. **Bare-literal bindings** (`const z = 4`, unannotated `[1, 2, 3]`) that ebnf
    marks "ok — inferred from value": reconcile with §3/§5.1 (a literal needs an
    inference source; `const z = 4` with none requires an annotation). Either amend
    those examples or supply a source (e.g. `main`'s `Int`-return convention already
    types `() -> 0`). (§3, §5.1)
11. **`Int`/`Uint`/`Float` become unions; the pointer-width concretes are `Arch`/
    `Uarch`.** The old concrete `Int`/`Uint` (pointer-width) are renamed `Arch`/
    `Uarch`, and `Int`/`Uint`/`Float`/`Number` are the std constraint-unions (§2.1,
    §2.2, §8). **Every concrete `Int`/`Uint` in the grammar/examples** (`Int argc`,
    `data Point = { Int x }`, `Int fd`, the exit code, …) becomes a concrete width —
    `Arch` (or `Int32`/`Uint32`) — since a bare `Int`/`Uint` is now a *union*. This
    is the largest single ripple; a bare `Int` field/param would otherwise mean a
    runtime any-integer union. (§2.1)
12. **`Infinity`/`NaN`** are `pub intrinsic type` singletons (§2.2); the ebnf's
    `PositiveInteger`/`NonNegativeInteger` constraint-unions align with the §8
    numeric-union scheme (members are the concrete widths). (§2.1, §2.2, §8)
13. **Union bodies gain member spread.** `union Number = { ...Int, ...Uint, ...Float }`
    splices another union's members. `union_body`/`union_member` add a `...named_type`
    form (today `union_member` has no spread). (§8.1)
14. **Data literals gain a value-level spread.** `{ ...other, z: 3 }` in a `data_literal`
    (a value), distinct from the existing record-*declaration* spread `...named_type`
    and the bracket-`aggregate` spread. `field_init` (or `data_literal`) adds the
    `...expression` element. (§7.3)
15. **`intrinsic type` — a type-valued intrinsic carrying its constructor.**
    `pub intrinsic type Uint8 = (Number value) -> Uint8` (and the nullary
    `pub intrinsic type Infinity`). Today `intrinsic_decl` is function-only
    (snake_case name, no `type` keyword); it gains a `type`-keyword, `type_name`,
    constructor-signature-RHS form. (§2.1, §2.2)
16. **Inline union members are qualified-only.** ebnf's union section shows a member
    reached bare (`None`); §8.1 restricts bare access to **compose-over** members
    (standalone types like `Int8`) — an **inline-declared** member (`Just`, `Nothing`,
    `Left`, `Right`, a payload/singleton) is reachable only through its union
    (`Maybe.Just`). The bare-`None` example becomes `Maybe.Nothing`. (§8.1)
17. **No `*T`↔integer conversion / no pointer arithmetic** — not an ebnf *grammar*
    change (memory_model makes pointer arithmetic inexpressible already), but this
    spec drops the "explicit pointer/integer conversion" it had briefly implied; raw
    addresses live only in the `asm` floor. (§4, §6.4)
18. **A union member may have a positional payload `Name(T1, …, Tn)`.** ebnf's
    `union_member` allows a struct-body / type / literal payload but no positional
    tuple; `Just('Value)` / `IntLit(Int32)` add it (mirroring positional records,
    §7.3), constructed by application `Maybe.Just(1)`. (§8.1)
19. **Qualified member paths in value and pattern position.** `Maybe.Just(1)` (value)
    and `Int.Int8(v)` / `Node.IntLit(p)` (a `type_pattern`) require `member_access`
    (`Type.PascalMember`) to be wired into `primary`/`postfix` and into `type_pattern`
    — today `primary` admits no `type_name` callee and `type_pattern`'s head is an
    unqualified `named_type`. (§6.6, §8.1, §8.3)
20. **A union member may carry its own generic params.** `union Tree = { *Tree,
    Node['Value]('Value) }` puts the parameter on the member, not the head; ebnf's
    `union_member` has no `generic_params` and states only head-level params "flow
    into" members. `union_member` gains an optional `generic_params` before its
    payload, and construction gains member-level type application `Tree.Node[Int32](x)`
    (inferred where possible). (§8.1)
21. **Union payload placement is the ambient geometry's, not "arena."** ebnf's union
    Representation aside calls a tag+payload union "arena-allocated"; §8.4 defers
    placement to [[geometry_lowering.md]] (payload sits under whatever geometry owns
    the value). Drop the "arena" claim from ebnf's aside. (§8.4)
22. **`aggregate-literal kind` loses the tuple axis.** [[order_of_compilation.md]] §4
    lists the type gate resolving "fixed vs tuple vs dynamic"; with tuples now
    syntactic `(…)` (item 1) the gate resolves only array fixed-vs-dynamic and
    record-literal typing — "tuple" drops from that ownership sentence. (§1, §7)
