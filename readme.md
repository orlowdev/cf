# C!

**C!** (pronounced "C-factorial") is a statically-typed systems programming
language. It compiles to native code through [QBE](https://c9x.me/qbe/) and a
tiny hand-written assembly floor to avoid libc where it is possible.
Programs are freestanding: a C! binary talks to the OS directly through
syscalls.

> **Status.** C! is at the bootstrapping stage. The compiler, `cf`, is
> self-hosting and passes its corpus, but it still accepts only a reduced subset
> of the full language, and the target is currently **arm64 macOS** only. The
> language specs live in [`root/specs/`](root/specs/).

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

- **Tofte & Talpin** (_Region-Based Memory Management_, 1997; _...using a Stack of
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

## Algebras and geometries

C!'s memory model separates two things most languages weld together: an
**algebra** that decides _whether and where_ a value is allocated, and a
**geometry** that decides _how that storage is laid out and reclaimed_. Splitting
them is what buys deterministic memory with no `malloc`, no `free`, and no garbage
collector.

**The allocation algebra** is the static discipline. A function carries the `!`
effect (`collect!`) when it holds an aggregate that lives _and dies within its own
frame_ — _residue_ that never escapes — or when it calls another `!` function. A
value it hands back is not charged to it (it becomes the destination's cost), so a
function whose every allocation escapes stays colorless — `!` marks residue, not
allocation as such. An allocating call names the region it runs `in`, and from
those facts the compiler proves one property — _a value must not outlive the
region that will reclaim it_ — the surface reading of Tofte–Talpin's region
typing. It is pure comptime: the shape of every lifetime is settled before a byte
moves.

**A geometry** is the policy the algebra hands the bytes to — and it is not a
keyword. It is an ordinary module exposing a fixed hook set (`on_alloc`,
`on_ret`, `on_scope_exit`, `on_free`, etc.) plus a constructor. The compiler never
knows what a bump allocator _is_: it only **places** hook calls — an `on_ret` at
an escape, an `on_scope_exit` bracket at a frame — and the module supplies the
bodies. `in arena` names the module; the same placement runs over any geometry,
standard or user-written. So a program's _lifetimes_ are fixed by the algebra
while its _representation_ stays swappable — `arena` / `page` / `fixed_buffer` are
bump allocators (allocation is a cursor advance, reclamation is bulk); an `rc` or
`gc` geometry on the same contract does per-object work for the values that need
it.

```c!
import std::mem::arena::fixed_arena
data Sum = { Iarch total }

const collect! = (Iarch n): Iarch -> {
	const s = Sum({ total: n + n * 2 })   # a scratch record, placed by the ambient geometry
	return s.total                        # only a scalar escapes
}

pub const main = () -> {
	const b = fixed_arena::of(4096)       # a scratch region, carved from the page
	defer fixed_arena::destroy(b)         # torn down at scope exit — no malloc, no free
	return collect!(3) in b               # collect! allocates in b; b is reclaimed on return
}
```

### Geometries are RAINI

RAII — _Resource Acquisition Is Initialization_ — fuses three events into an
object's construction: it **acquires** a resource, **initializes** it, and **binds
its release** to a scope. C!'s bump geometries deliberately **unbundle** them, and
the middle letter adds up:

- **Acquisition is amortized.** Real memory is acquired _once_, at the root of the
  manifold (one `mmap`), then grown lazily by watermark. No value — not even an
  arena — acquires memory at birth: a carved region is a _sub-reservation_ of
  space the manifold already owns.
- **Initialization is placement.** Constructing a value is a cursor advance into
  that pre-acquired space. It acquires _nothing_ — which is exactly why a bump
  allocator is free.
- **Release is bulk.** A value is never freed on its own; it dies when its
  region's cursor rewinds. Lifetime lives on a separate axis (region scope +
  escape), not welded to the constructor.

So for the region geometries, **Resource Acquisition Is _Not_ Initialization**:
the manifold is the resource, acquired once; values are just offsets into it. RAII
is not gone — it is _relocated_. It survives at the root (one acquire = the
program's start) and wherever a resource has a genuine paired release
(`defer close(open(path))` is textbook RAII). A per-object `rc` geometry would
re-bundle the triple. The hook framework spans the whole spectrum — the arenas are
simply its RAINI end.

## Install

Grab a released binary for your platform (darwin arm64/amd64, linux arm64/amd64/riscv64):

```sh
curl -fsSL https://raw.githubusercontent.com/orlowdev/cf/margarita/install.sh | sh
```

It downloads one static binary from this repo's GitHub releases, **verifies it against the
published sha256** before writing it, and drops `cf` in `~/.local/bin` — no `sudo`, no daemons.
(Yes, it's a `curl | sh`; the script is right there in the repo, and you're welcome to read it
first.) Knobs: `CF_CHANNEL=nightly|rc|latest|stable`, `CF_VERSION=<tag>`, `CF_INSTALL_DIR=<dir>`.

Pin a **channel** to always get the newest build on a release ring:

```sh
curl -fsSL https://raw.githubusercontent.com/orlowdev/cf/margarita/install.sh | CF_CHANNEL=stable sh
```

Each ring also publishes a rolling release under a bare tag, so the download URL is stable — you can
skip the script entirely and grab the binary directly (there's a `.sha256` next to each):

```
https://github.com/orlowdev/cf/releases/download/<ring>/cf-<ring>-<platform>
# e.g. …/releases/download/stable/cf-stable-linux-amd64   (ring ∈ nightly|rc|latest|stable)
```

For an immutable pin, use a versioned tag: `CF_VERSION=1.23.61-stable` (or that release's asset URL,
`…/releases/download/1.23.61-stable/cf-1.23.61-stable-<platform>`). To build from source instead, see
[Building the compiler](#building-the-compiler).

## Hello, world

```c!
import std::io::console::{ print }

pub const main = () -> {
	print("Hello, world!\n")
	return 0
}
```

```sh
var/cf hello.cf -o hello   # compile + embedded QBE + link (cf finds cc itself)  ->  ./hello
./hello                    # Hello, world!
```

## `$`-stack values

The manifold is where data lives by default. A binding whose name begins with `$`
is the one deliberate exception: it places a value on the **hardware stack**, freed
automatically when the function returns — no geometry, no `in`, no teardown.

```c!
data Point = { Iarch x, Iarch y }

const dist = (Iarch a, Iarch b): Iarch -> {
	let $p = Point({ x: a, y: b })   # $p lives in a frame slot, gone when dist returns
	return $p.x + $p.y               # only a scalar escapes; $p never does
}
```

For the effect system a `$` value behaves **like a scalar**: its storage is a frame
slot, not manifold residue, so it does not color its function `!`. A routine whose
only working memory is `$`-stack touches the manifold not at all and stays colorless.

That freedom is fenced, all at comptime:

- **Comptime-sized and capped** — 64 KiB per binding, 256 KiB per frame. Sizes are
  known, so both caps are compile errors, not runtime faults. This is exactly the
  guarantee that user _data_ can never overflow the hardware stack (control still
  spends a small, bounded slice per call).
- **No `$` inside a loop** — a stack slot is function-lifetime, so a per-iteration
  one would accumulate; hoist it out, or use a geometry whose per-iteration reset
  reclaims.
- **No escape, no re-point** — a `$` value dies at frame exit, so it may not be
  returned or stored where it would outlive the frame; and it is a fixed slot, so
  re-_binding_ it is rejected (mutate its fields instead).

`$`-stack pairs naturally with `fixed_buffer` below: `fixed_buffer::of($buf)` lays a
whole bump geometry over a stack buffer, so an entire scratch region can live and
die on the frame.

## Trust

C! takes reproducibility and provenance seriously. The trust chain has exactly
two external dependencies, and both are pinned or reproducible.

**The seed is public** The permanent root of the build is a **per-platform** seed:
[`boot/seed_mac/`](boot/seed_mac/) and [`boot/seed_linux/`](boot/seed_linux/), each `cf.qbe` (the
QBE IL of the compiler) plus its assembly `floor` (per arch). The seed is per-OS because the
compiler's own I/O bakes that OS's syscall numbers into the IL through comptime module selection —
not only into the floor; the two linux arches SHARE one IL (generic ABI) and differ only in the
floor. Each is cf compiling its own source: a **self-reproducing fixpoint** — a cf built from the
seed recompiles the source to a byte-identical seed. `seed_mac` (the darwin root) is checked with
[Diverse Double-Compilation](https://dwheeler.com/trusting-trust/) across two independent C compilers
(clang and gcc); `seed_linux` is the SAME compiler's deterministic cross-emit (`--target linux`), so
it inherits that trust and its native fixpoint is verified in CI.

**QBE is vendored by exact commit.** The backend lives *in* the tree as a squashed git subtree at
[`boot/vendor/qbe`](boot/vendor/qbe) (pinned to `v1.3`, commit `c081897…`), so the build is
hermetic and offline — there is no fetch step. cf uses that one source two ways: the standalone
`qbe` assembles the committed seed IL (the trust chain's qbe step), and QBE's objects are **linked
into `cf` itself** (via [`boot/qbe_embed.c`](boot/qbe_embed.c)) so the shipped compiler translates
IL→asm in-process — a user needs only `cf` plus a C toolchain, never a separate qbe.
[`boot/fetch-qbe.sh`](boot/fetch-qbe.sh) is now a maintainer tool that `git subtree pull`s a new
pin; updating the vendored source is the only time the network is touched.

So the entire tail from source to binary is unchanged: **public seed** (DDC-verified fixpoint) →
**pinned QBE** → **system `cc`**. Nothing else is trusted; embedding links the *same* pinned QBE
in rather than forking it out.

**cf hosts its own standard library** — and does it *in the language*, not with another linked C
blob. `std::comptime` exposes two comptime intrinsics: `embed("path")` bakes a file's bytes into the
binary as a static `Str`, and `embed_dir("dir")` walks a directory and bakes every file into a
`[EmbeddedFile]` — general tools any program can use for a template, a shader, a lookup table. The
compiler uses `embed_dir("lib/std")` in its own module loader: a shipped `var/cf` resolves `std::…`
imports with **nothing on disk** (disk first for development, the baked copy as fallback). The bytes
ride in a `data` section, emitted by cf itself — no `cc`, no separate object.

**The committed seed stays std-LESS.** Baking the stdlib into the seed `cf.qbe` — the trust-root —
would bloat it and force a reseed on *every* stdlib edit. So the embed is skipped for the seed: cf
takes a `--skip-embeds` flag that drops all `embed`/`embed_dir` data when emitting the bootstrap
intermediates. `reseed.sh` passes it, so the seed is a plain std-less compiler that reads `lib/std`
from disk; a stdlib edit that leaves the compiler's own code untouched needs no reseed at all.

## Building the compiler

```sh
sh boot/build.sh         # two-stage: std-less seed -> cf0 -> std-hosting var/cf  ->  var/cf
sh boot/test.sh          # run the corpus regression suite
```

`boot/build.sh [target]` never needs a C! compiler you don't already have (target defaults to the
host; darwin-arm64 and linux-arm64 build natively from their seed, linux-riscv64 cross-builds on a
linux-arm64 host). It runs in **two stages**: stage 1 assembles the host's committed (std-less) seed
(`boot/seed_<os>/cf.qbe` + `floor.<arch>.s`) straight through `qbe → cc` into a bootstrap compiler
`cf0`; stage 2 has `cf0` recompile the compiler *with* embedding on —
baking `lib/std` (read from disk) into the self-contained, std-hosting `var/cf`. So a stdlib edit
reflows into `cf` through stage 2 alone. The compiler's own source is the C! modules under
[`lib/std/compiler/`](lib/std/compiler/) — the whole pipeline, exposed as
`std::compiler::compile!` — driven by the thin CLI entry
[`boot/src/cf.cf`](boot/src/cf.cf) and its app-layer siblings (`cli`, `fmt`,
`docs`).

Once built, `cf` compiles a program end to end by itself:

```sh
var/cf hello.cf -o hello   # compile + embedded-QBE + link (finds cc on PATH)  ->  ./hello
```

### Changing the compiler

Editing the **compiler's own** C! source (`lib/std/compiler/*.cf` or
`boot/src/*.cf`) leaves the seed stale, since the seed *is* the compiler.
Regenerate and re-verify it:

```sh
sh boot/reseed.sh
```

`reseed.sh` builds cf from the _old_ seed, has it recompile the _new_ source
(with `--skip-embeds`, so the seed stays std-less), then confirms the result is
a true fixpoint (a cf built from the new seed reproduces it exactly). Only then
is the committed seed replaced; a non-fixpoint aborts without touching it.
Commit the regenerated seed alongside the source change.

Editing the rest of **`lib/std`** (anything the compiler doesn't itself import —
`std::math`, a new module, a doc fix) needs **no reseed**: the seed carries no
stdlib, so it is unaffected. Just `sh boot/build.sh` — stage 2 rebakes the
current `lib/std` into `var/cf`.

> One bootstrap constraint: the old cf must already accept the new source. A
> change to the language subset cf itself uses needs a transitional two-step
> reseed.

## Freestanding and embedded

C! emits **freestanding** binaries by default — no libc, no C runtime, just
syscalls and the tiny assembly floor. That reaches down to bare metal:
`cf app.cf --target bare-arm64 -o app` produces a freestanding ELF (SysV relocations,
no loader) that boots under `qemu-system-aarch64` — cf cross-assembles and links it
itself (`clang` + `ld.lld`), embedding the linker script.

"Freestanding" is a guarantee about the code cf **emits** from pure C! source, not
about the cf binary itself: the compiler links QBE (which is C, and uses libc) so it
can translate IL in-process. Cross the FFI boundary into C and you take on C's runtime;
programs cf compiles from C!-only source stay freestanding.

The memory story follows. `fixed_buffer` is the **zero-dependency geometry**: it
wraps a caller-provided `[Uint8]` buffer — `$`-stack, `static`, or heap — as a full
bump region, allocating nothing itself (its 96-byte header lays over the buffer's
own front; it differs from `fixed_arena` only in where the buffer comes from). Back
it with a `static` buffer and add `--root-size 0`, which rejects _any_ dynamic page
use, and the compiler proves the binary never touches an allocator at runtime: the
whole program's memory is one fixed, statically-sized buffer. That is the embedded
target — no OS, no heap, no allocation the compiler cannot see.

> **Status.** The bare-metal path is early: cf emits the freestanding ELF and it
> boots under qemu, arm64 only, with a standard library scoped to the primitives a
> freestanding program needs.

## Repository layout

```
boot/            build system + compiler (the trust root)
  fetch-qbe.sh   update the vendored QBE subtree to a new pin (maintainer tool)
  build.sh       build cf from the committed seed (builds + embeds the vendored QBE)
  reseed.sh      regenerate + fixpoint-verify the seed after a source edit
  test.sh        corpus regression suite
  seed/          cf.qbe + floor.s — the DDC-verified self-hosting seed
  src/           the compiler entry + app layer (cf, cli, fmt, docs), written in C!
  qbe_embed.c    cf's C bridge to the embedded QBE (cf_qbe_run)
  vendor/qbe/    the QBE backend, vendored as a pinned git subtree
root/specs/      the language specification
opt/             FHS third-party area (gitignored; QBE now lives in boot/vendor)
var/             build outputs (gitignored)
```

## License

Apache License 2.0 — see [`license`](license).
