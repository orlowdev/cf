# C! CLI

The command surface of the `cf` binary — the subcommands, their flags, and how
they map onto the compile pipeline, the memory model's link modes, and the
`comptime` target. This is the API of the **full** compiler — `cf`, the `1.x`
evergreen line (see [[versioning.md]]); the bootstrap `cf0` (`0.x`) implements only
the slice it needs to build `cf` (§9). It surfaces the pipeline of
[[order_of_compilation.md]], the freestanding/hosted boundary of
[[seed_subset.md]] §3, and the target selection the `comptime` module exposes
(see [[ebnf.md]], Modules).

Status: design in progress. The surface below is settled in shape; several
subcommands and modes are explicitly deferred (§8) and widen in later rounds.

## 1. Shape

`cf` is a subcommanded CLI. Every subcommand has a one-letter alias except the two
that are deferred and the two whole-word ones; global flags work anywhere.

```
cf <command> [options]

Commands
  c, compile <file>      Build the executable rooted at <file>'s pub main
  run <file>             Compile <file> then run it   (= compile --run)
  t, test <file|dir>     Run tests
  f, format <file|dir>   Format in place
  l, lint <file|dir>     Lint
  s, lsp                 Start the LSP server (stdio)
  repl                   Read-eval-print loop                   [deferred §8]
  man                    Package manager                        [deferred §8]

Global
  -V, --version          Print cf's OWN GenVer (see [[versioning.md]]); to stamp
                         the compiled program's version use compile --set-version
  -h, --help             Help — cf <command> --help for a command's options
  -q, --quiet            No stdout; errors still on stderr   (--silent alias)
```

`cf --help` prints the brief above — overall blurb, version, and the command list;
`cf <command> --help` prints that command's own description and options. There is
no `-v`/verbose: the compiler's verbosity is its *intermediates* (§3, Emitting),
not a chattier log.

## 2. `compile` — the core

```
cf compile <file> [options]
  -o, --output <path>    Where to write the artifact
      --lib              Build a C-ABI library (no pub main required)
      --libc <mode>      none | static | dynamic          (§4)
  -T, --target <os-arch> darwin-arm64 | darwin-amd64 |
                         linux-arm64 | linux-amd64 | linux-riscv64 | wasm  (§5)
      --stop-at <stage>  where the pipeline halts          (§3; default binary)
  -c                     = --stop-at obj
  -S                     = --stop-at asm
      --emit-<arc>-cf    dump an arc's .cf and keep going   (§3)
      --emit-qbe|-asm|-obj   dump a backend artifact and keep going
      --watch            rebuild on change of <file> + its non-std imports
      --run              run after building (re-run under --watch)  (§6)
      --set-version <v>  stamp the ARTIFACT's std::comptime::version::current
                         (default 0.0.0-dev; ::compiler carries cf's own version)
```

`compile` takes **one file** — the compilation entry. It must be a `.cf` file, and
(unless `--lib`) must define a `pub const main` (see [[ebnf.md]], Entry Point).
Every path is resolved relative to that file's location, **except** standard
library paths (`std::...`), which the toolchain resolves. This single-entry,
relative-path rule is the whole module-resolution story; there is no ambient
search path.

### Output and kind

`--output`/`-o` names where the artifact lands; the default output directory is **`./out/`**, the
artifact named after the entry file's stem (so `cf compile src/main.cf` yields
`./out/main`), and `--emit` / `--stop-at` intermediates land there too — `-o` may
name a directory or a full path. `--lib` switches
the **output kind** from an executable to the **C-ABI library** artifact
[[order_of_compilation.md]] §3 defines — rooted at the file's `pub` functions under
a pinned node-free geometry, its symbols dropping the node parameter and the `!`
marker. A library has no `main`; an executable requires one. These are the only two
output kinds.

## 3. The pipeline: emitting and stopping

`compile` runs the pipeline of [[order_of_compilation.md]]: the six observable arcs
(`resolved → desugared → specialized → memory → folded → pruned`) then the backend
(`qbe → asm → obj → binary`). Two orthogonal controls expose it.

- **`--stop-at <stage>` halts the pipeline** and makes that stage's output the
  artifact. The enum is every stop point:

  ```
  resolved desugared specialized memory folded pruned   # the arcs — emit .cf
  qbe asm obj                                            # backend artifacts
  binary                                                 # default: the whole build
  ```

  Halting at an **arc** yields that arc's readable `.cf`, carrying the `cf-stage`
  pragma — the exploration checkpoint [[order_of_compilation.md]] §6 describes, and
  the file you feed back to **resume**. `-c` and `-S` are the C-native shorthands
  for the two most-typed stops: `-c` = `--stop-at obj` (compile to object, no
  link), `-S` = `--stop-at asm`.

- **`--emit-<arc>-cf` and `--emit-qbe`/`-asm`/`-obj` dump an intermediate as a side
  artifact and keep building.** Unlike `--stop-at`, they do not halt — you can dump
  the `desugared` `.cf` *and* the final binary in one run, and you may pass several.
  The `-cf` suffix marks the readable arc dumps apart from the backend ones.

**Validation.** Asking to `--emit` a stage the run will never reach — any emit past
the `--stop-at` point — is an **input error**, rejected at argument parsing, not a
silent no-op.

The arc emits, the resume story, and the formatter that canonicalises every
emitted `.cf` are the observability invariant, which is a `cf` feature: **`cf0`
implements none of it** (see [[seed_subset.md]] §8), so `--emit-*` and the arc
values of `--stop-at` exist only on the full compiler.

## 4. Linking: `--libc`

`--libc` picks the **link mode** — the freestanding/hosted boundary of
[[seed_subset.md]] §3 — and lowers to the underlying `cc` link flags:

| `--libc`  | meaning                          | lowers to     | C externs |
| --------- | -------------------------------- | ------------- | --------- |
| `none`    | freestanding, no C runtime       | `-nostdlib`   | no        |
| `static`  | hosted, C runtime linked static  | `-static`     | yes       |
| `dynamic` | hosted, C runtime linked dynamic | (default link) | yes      |

- **Default is `none`** — a C! binary is self-contained, its std bottoming out at
  raw syscalls (see [[ebnf.md]], Assembly). Given valuelessly (`--libc`), it means
  `dynamic`.
- **`none` cannot use C bindings.** A freestanding binary has no libc
  initialisation, TLS, `errno`, or allocator, so C externs are available **only**
  in a hosted mode (`static`/`dynamic`), linked accordingly.
- **`static` errors on darwin.** Apple does not support statically linking
  libSystem, so `--libc static` with a `darwin-*` target is a compile error, not a
  silent fallback — it is only meaningful for `linux-*`.
- **On darwin, `none` still links libSystem.** Apple's linker refuses a dynamic
  Mach-O with no libSystem load command, and arm64 has no static-executable path
  (a static binary is killed on exec), so on a `darwin-*` target `none` lowers to
  `-nostdlib -lSystem`, not `-nostdlib` alone. libSystem is present only to satisfy
  the loader — no C-runtime symbol is referenced, the binary stays freestanding in
  every other sense and still reaches the kernel through raw `svc` (see
  [[ebnf.md]], Assembly). The bare `-nostdlib` in the table is the non-darwin form.

`--libc` has no short flag on purpose: every candidate letter (`-C`, `-c`, `-l`,
`-L`) is a `cc` landmine. The full C-extern surface — declaring externs, naming C
libraries and search paths — is beyond this mode switch and is its own later
subject; `--libc` sets only whether a C runtime is present at all.

## 5. Targets: `--target`

`--target`/`-T` selects an **os-arch pair** — not a triple. A GNU/LLVM triple is
`arch-vendor-os` (plus a fourth ABI slot); C! needs neither extra slot — the vendor
is noise, and the ABI/runtime dimension is already `--libc` (§4). So the token is
just operating system and architecture, os first:

```
darwin-arm64  darwin-amd64  linux-arm64  linux-amd64  linux-riscv64  wasm
```

The pair splits into the two values the `std::comptime` module exposes for conditional
imports (see [[ebnf.md]], Modules; [[module_system.md]] §7): `os::target` ∈
`{darwin, linux}` and `arch::target` ∈ `{arm64, amd64, riscv64, wasm}`. `wasm`
is the one token that does not split on `-`: **Wasm is not an OS**, so it sets the
**arch** to `wasm` and leaves `os` at its default. Default is the **host** pair — the os
and arch `cf` itself runs on (so a bare `wasm` target is the Wasm arch on the host os).

Two targets carry known asterisks, both deferred (§8):

- **Cross-targets need a cross-linker.** Building `linux-*` on a darwin host cannot
  use Apple's Mach-O `ld`/`cc`; it needs the target's linker and C runtime. The
  arch backends are `qbe`'s (amd64, arm64, riscv64), but linking is a toolchain
  concern the cross story must supply.
- **`wasm` needs its own backend.** `qbe` emits none of it, and wasm has no `svc`
  syscall floor and a linear-memory model — so it is a self-hosted backend to
  write later, not a `qbe` target flag.

## 6. `run`, `watch`, and program arguments

- **`--run`** builds the artifact and executes it; under `--watch` it re-runs on
  every successful rebuild. **`cf run <file>`** is the shorthand for
  `cf compile <file> --run`.
- **`--watch`** recompiles when `<file>` or any of its non-std imports change,
  walking the transitive import graph.
- **Program arguments pass through `--`**: `cf run <file> -- <args...>` (equivalently
  `cf compile <file> --run -- <args...>`) forwards `<args...>` to the built binary's
  `main(argc, argv, envp)` (see [[ebnf.md]], Entry Point). Without the `--` the
  program receives no arguments.

`run` is AOT today: compile, then exec the result, and on `--watch` **restart** the
process. In-process hot reload — swapping functions in a live process without a
restart — is a different execution model (a JIT backend, not the `qbe` AOT path),
and is deferred (§8). When it lands it extends `run`; it does not change `--run`'s
current restart semantics.

## 7. `test`, `format`, `lint`, `lsp`

```
cf test   <file|dir>   --watch  --bail
cf format <file|dir>   --check
cf lint   <file|dir>   --check
cf lsp                 [--port <n>]
```

- **`test`** runs the tests in a `.cf` file, or walks a directory recursively and
  runs the tests in every `.cf` it finds. `--watch` re-runs on change; `--bail`
  exits the whole process on the first failing test. What *is* a test, and the
  assertion surface, are a small **dedicated testing spec** (deferred).
- **`format`** formats a file or a directory tree in place. It is the same
  formatter that runs as the pipeline's cross-cutting canonicaliser
  (see [[order_of_compilation.md]] §6), exposed as a command — so what `cf format`
  writes and what an arc emit is byte-compared against are one and the same.
  `--check` makes no writes and exits non-zero if any file is not already
  formatted (the CI gate).
- **`lint`** lints a file or a directory tree; `--check` exits non-zero on any
  finding. The lint set is a later subject.
- **`lsp`** starts the Language Server over **stdio** — an editor spawns it and
  talks stdin/stdout, so no port is needed in the normal case. `--port` is an
  optional TCP mode for debugging or remote use, likely rarely used.

## 8. Deferred surface

Named here so the tensions are on record; each widens later, some into their own
specs.

| deferred                     | why / where it lands                                       |
| ---------------------------- | ---------------------------------------------------------- |
| `run` JIT / hot reload       | a JIT backend + state-preservation semantics; on darwin needs `MAP_JIT` / W^X under the hardened runtime — its own spec |
| `repl`                       | C! is whole-program with explicit geometry; a REPL needs an implicit ambient geometry and incremental re-specialization, and leans on the JIT — a far-later subject |
| `man` (package manager)      | defines how modules are installed — git subtree into `./vendor/*` vs. direct source pinning — and owns dependency management wholesale |
| `wasm` target + backend      | a self-hosted backend (`qbe` cannot emit wasm); no syscall floor, linear memory — a dogfooding exercise later |
| cross-linking                | a cross-linker + target C runtime for `linux-*` from a darwin host |
| C-extern surface             | declaring externs, C libraries, search paths — beyond the `--libc` mode switch |
| testing surface              | what a test is, assertions — a small dedicated spec |

## 9. What `cf0` borrows

This is `cf`'s API. The bootstrap `cf0` (`0.x` — see [[seed_subset.md]]) takes only
the slice it needs to build `cf`, and no more:

- **`compile` only** — no `run`, `test`, `format`, `lint`, `lsp`, `repl`, or `man`.
- **`--output`**, to place the artifact.
- **A fixed target** — `darwin-arm64` alone (see [[seed_subset.md]] §3), so no
  `--target`.
- **`--libc none` and `--libc dynamic`** — freestanding for user binaries, hosted
  for building `cf` itself (which links the C runtime because it vendors `qbe`);
  `static` is absent (it errors on darwin anyway).
- **No `--emit-*` and no arc `--stop-at`** — `cf0` drops the observability
  invariant entirely (see [[seed_subset.md]] §8), and needs no formatter with it.

Everything else on this page is restored as the evergreen line grows — the same
pattern the rest of the suite follows: `cf0` is the floor, and each release widens
the surface without ever narrowing the language its users write.
