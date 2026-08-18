# C!

**C!** (pronounced "C-factorial") is a statically-typed systems programming
language. It compiles to native code through [QBE](https://c9x.me/qbe/) and a
tiny hand-written assembly floor to avoid libc where it is possible.
Programs are freestanding: a C! binary talks to the OS directly through
syscalls.

> **Status.** C! is at the bootstrapping stage. cf0 is self-hosting and passes
> its corpus, but the language is still a reduced subset (`cf0`) and the target
> is currently **arm64 macOS** only. The language specs live in
> [`root/specs/`](root/specs/).

## Why C!

C! deliberately pulls apart three things most languages fuse:

- **Behavior is detached from data.** There are no methods. Data is inert;
  behavior lives in modules.
- **Data is detached from memory layout.** You never `malloc` or `free`.
  Placement is decided by a _geometry_ — a comptime allocation strategy — not
  written by hand at each call site.
- **Memory is detached from the program's identity.** All memory lives in one
  tree, the _manifold_; code only ever names a _policy_ over it, never a raw
  pointer to freed-able storage.

```c!
data Point = { Iarch x, Iarch y }

const manhattan = (Point p): Iarch -> p.x + p.y

pub const main = () -> manhattan(Point({ x: 20, y: 22 }))   # 42
```

### The idea, and where it comes from

C!'s memory model is a deliberate descendant of
[region-based memory management](https://en.wikipedia.org/wiki/Region-based_memory_management).
The lineage:

- **Tofte & Talpin** (_Region-Based Memory Management_, 1997; _…using a Stack of
  Regions_, POPL 1994) showed you can reclaim memory safely with no garbage
  collector by partitioning the heap into _regions_: every allocation goes into
  some region, and an entire region is freed in one step when its lifetime ends.
  Crucially theirs is a [**type-and-effect system**](https://en.wikipedia.org/wiki/Effect_system)
  (in the line of Gifford & Lucassen, 1986) — a value's type records the region
  it lives in, a function's _effect_ records which regions it touches, and both
  are checked statically.
- **Cyclone** (Grossman et al., PLDI 2002) carried regions into a
  [safe dialect of C](https://en.wikipedia.org/wiki/Cyclone_(programming_language)),
  showing the discipline survives contact with real systems code.
- **Rust** recast a region as a _lifetime_ and paired it with affine ownership —
  the [substructural-types](https://en.wikipedia.org/wiki/Substructural_type_system)
  tradition (Girard's linear logic, 1987; Walker's _Substructural Type Systems_,
  2005) in which a value is used at most once.

C! sits squarely in this line and makes two moves of its own. Where Tofte–Talpin
arrange regions in a **stack**, C! generalizes them to a **tree** — the
_manifold_. And where they _infer_ regions invisibly, C! makes the region a
first-class, named thing: a call states `in` which region it allocates, a
function that allocates is marked with the `!` **allocation effect**, and an
escape check — _a value must not outlive its region_ — is the surface reading of
Tofte–Talpin's region typing. `malloc`/`free` disappear because a static type-and-effect
discipline assigns every value to a region and proves, at compile time, that
nothing escapes the region that will reclaim it.

The other axis — **behavior detached from data** — is the
[data-oriented](https://en.wikipedia.org/wiki/Data-oriented_design) tradition
(entity/component designs; Mike Acton's data-oriented design): inert data, no
methods, transformations written as plain functions over plain records.

### A functional streak

The value layer leans on ML and its descendants. Data is
[**algebraic**](https://en.wikipedia.org/wiki/Algebraic_data_type) — records are
products, `union`s are sums (`Maybe.Just(x)`, `Either.Right(v)`) — and `match` is
checked for **exhaustiveness**, so an unhandled case is a compile error. Types are
largely **inferred** in the
[Hindley–Milner](https://en.wikipedia.org/wiki/Hindley%E2%80%93Milner_type_system)
tradition, and code is expression-oriented: `if`, `match`, and blocks can all _yield_
values. Functions are first-class with closures and higher-order composition,
immutability is the default (`const`), and a left-to-right **pipe** (`x |> f |> g`)
reads dataflow the way F# and Elm do. The `!` allocation effect above is itself
in the type-and-effect vein that effect-typed functional languages (e.g. Koka)
have since made mainstream.

Under-applying any function — passing fewer arguments than it takes — yields a
function of the rest, left to right. There is no special `curry` and no
placeholder: `sum(2)` is just `(b) -> sum(2, b)`, and the same rule spans type
and value arguments alike (`f[Int32]`, `f(1)`, and `f(1, 2)` are all partial
applications of one `f`). It composes directly with the pipe — `3 |> sum(2)`
fills the missing argument, i.e. `sum(2, 3)` — so a pipeline reads as a chain of
progressively-specialized functions. C! leans on this hard enough that it
**deliberately omits** curried, function-returning-function definitions
(`f(a)(b)`) as redundant: partial application already expresses them, and more
directly.

Algebraic data and an exhaustive `match`:

```c!
union Maybe = { Just(Iarch), None }

const or_zero = (Maybe m): Iarch -> match m {
	Maybe.Just(x) -> x,   # payload bound as x
	Maybe.None    -> 0    # every case must be handled, or it won't compile
}
```

A pipeline of partially-applied functions, read top to bottom:

```c!
const inc = (Iarch x) -> x + 1
const sum = (Iarch a, Iarch b) -> a + b

pub const main = () -> 3 |> inc |> inc |> sum(10)   # sum(10) is partial → sum(10, 5) = 15
```

## Hello, world

```c!
import "std/io/console" as { print }

pub const main = () -> {
	print("Hello, world!\n")
	return 0
}
```

```sh
sh boot/driver.sh hello.cf hello   # cf0 -> qbe -> cc  ->  ./hello
./hello                            # Hello, world!
```

## Trust

C! takes reproducibility and provenance seriously. The trust chain has exactly
two external dependencies, and both are pinned or reproducible.

**The seed is public** The permanent root of the build is
[`boot/seed/`](boot/seed/): `cf0.qbe` (the QBE IL of the compiler) and `floor.s`
(its assembly floor). This is cf0 compiling its own source. It is a
**self-reproducing fixpoint** — a cf0 built from the seed recompiles the source
to a byte-identical seed. That fixpoint has been checked with
[Diverse Double-Compilation](https://dwheeler.com/trusting-trust/) across two
independent C compilers (clang and gcc).

**QBE is pinned by exact commit.** The backend has to be fetched at the first repo pull
(`opt/` is gitignored). [`boot/fetch-qbe.sh`](boot/fetch-qbe.sh) clones QBE from
its upstream, then **verifies the checkout against a hard-coded commit SHA**
before building. The transport is unauthenticated, but the pin is checked after
fetch, so in-transit tampering is caught before anything is built. Re-running is
idempotent and works offline once fetched.

So the entire tail from source to binary is: **public seed** (DDC-verified
fixpoint) → **pinned QBE** → **system `cc`**. Nothing else is trusted.

## Building the compiler

```sh
sh boot/fetch-qbe.sh     # 1. fetch + build the pinned QBE backend into opt/qbe
sh boot/build.sh         # 2. build cf0 from the committed seed  ->  var/cf0
sh boot/test.sh          # 3. run the corpus regression suite
```

`boot/build.sh` never needs a C! compiler you don't already have: it assembles
the committed seed (`boot/seed/cf0.qbe` + `floor.s`) straight through
`qbe → cc`. The compiler's own source is the C! modules in
[`boot/src/`](boot/src/) (`cf0.cf` and its imports).

### Changing the compiler

Because cf0 is built from the seed, editing its source leaves the seed stale.
After changing anything in `boot/src/*.cf`, regenerate and re-verify the seed:

```sh
sh boot/reseed.sh
```

`reseed.sh` builds cf0 from the _old_ seed, has it recompile the _new_ source,
then confirms the result is a true fixpoint (a cf0 built from the new seed
reproduces it exactly). Only then is the committed seed replaced; a
non-fixpoint aborts without touching it. Commit the regenerated seed alongside
the source change.

> One bootstrap constraint: the old cf0 must already accept the new source. A
> change to the language subset cf0 itself uses needs a transitional two-step
> reseed.

## Repository layout

```
boot/            build system + compiler (the trust root)
  fetch-qbe.sh   fetch/verify/build the pinned QBE backend
  build.sh       build cf0 from the committed seed
  driver.sh      compile+assemble+link one program (cf0 -> qbe -> cc)
  reseed.sh      regenerate + fixpoint-verify the seed after a source edit
  test.sh        corpus regression suite
  seed/          cf0.qbe + floor.s — the DDC-verified self-hosting seed
  src/           the compiler, written in C!
  tests/         corpus/ (the test programs) + manifest (the cf0 subset)
root/specs/      the language specification
opt/             vendored QBE (fetched, gitignored)
var/             build outputs (gitignored)
```

## License

Apache License 2.0 — see [`license`](license).
