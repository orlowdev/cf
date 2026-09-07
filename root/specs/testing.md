# C! Testing

How tests are written, built, and run in C!. A test lives beside the code it
checks, at the top level of a module, and is compiled by an **alternative route**
(`cf test`, see [[cf_cli.md]] §7) that swaps in a test root and, per case, a
provided verdict accumulator and a reporting scope exit — a route a shipped binary
never takes, so tests cost the artifact nothing. This is the small dedicated spec
`cf_cli.md` defers.

It builds directly on [[memory_model.md]] (the node, the `!` marker, the `in`
clause, scope-exit), [[geometry_lowering.md]] (geometries as std source with value
hooks and the `on_scope_exit` bracket), and [[context.md]] (scope-provided data
reached by type — the substrate the verdict accumulator rides). The surface is
[[module_system.md]]-resolved like any `std::` name.

Status: design ratified in shape (this round). The verdict accumulator rests on
[[context.md]]'s provide/consume, so the implementation lands after context does.
Timeout and coverage are named and deferred (§7); everything else below is v1.

## 1. The shape

A test is a top-level **call** — cf has no top-level statements, and a test is the
one thing that must sit beside a module's declarations without being one, so it
borrows the only top-level form there is (a call). Two forms. `case!` and `todo!`
are **real `std::test` functions** — ordinary HOF source, not compiler forms; the
only thing the compiler knows about them is positional: a top-level expression is
legal **iff** it is one of these two calls, and each one registers as an entry the
test runner (§6) drives. Under any other route the registered calls are simply
never reached.

```
std::test::case!("my_awesome_function works", () -> {
    my_awesome_function() |> std::test::equals(true)
})

std::test::todo!("handles the empty input", () -> {
    my_awesome_function(empty) |> std::test::equals(nothing)
})
```

- **`case!(description, body)`** is one test. `body` runs to completion under the
  test route (there is no early abort — see §4); it passes when every assertion in
  it held.
- **`todo!(description, body)`** is a pending test: `body` is parsed and
  type-checked exactly like a `case!`, but never run. The report marks it pending,
  outside the pass/fail tally. Flip `todo!` to `case!` when it is ready — the shapes
  are identical, so it is a one-word edit.

There is **no `suite!`** and no nesting. The **file is the group**: `cf test`
walks files and headers the report by module, and coverage (when it lands, §7)
attaches to the `case!` that exercised the code, never to a hand-drawn group. A
grouping primitive can return later if fixtures or serial-within-group scheduling
earn it; an ungrouped top-level `case!` and a future grouped one are the same node
with a different parent.

## 2. The body is native C!

A `case!` body is ordinary C! — not a restricted subset. `case!` throws the body
into a fresh **`test_arena`** (§3) and runs it with that arena as the ambient
node, so the body allocates freely with no `in` clause, and may still graft its
own geometry where it wants one:

```
std::test::case!("works in a fixed buffer", () -> {
    const fb = std::mem::fixed_buffer::of(1024)
    const result = my_func!() in fb
    result |> std::test::equals(true)
})
```

`my_func!() in fb` pins the fixed buffer for that call; everything else lands in
the `test_arena`. The whole `!` algebra ([[memory_model.md]] §5) and the `in`
clause apply unchanged inside the body.

## 3. `test_arena` — the case geometry

`case!` opens a **`test_arena`**: a **growing-arena geometry**
([[geometry_lowering.md]] §5) — bump allocation, elastic overflow, standard duplex
node — that differs from a plain arena in two orthogonal ways:

1. its construction **provides** a **`let` verdict accumulator** over it
   ([[context.md]] §2–§3) — the record an assertion appends `{ok, message}` to
   (§4). `case!` builds a fresh accumulator per case in **its own ambient node**
   ([[context.md]], Placement — manifold storage, one bracket above the case arena, so
   it strictly outlives the arena and the returned tally reads it after the
   teardown), then grafts the `test_arena` inside that provide's extent. A
   `let` provide of shared mutable state, so the appends are in-place writes
   into node storage `case!` already reserved. This is what gives the
   assertions their reach: they consume it by *type* ([[context.md]] §4), at
   any call depth, colorlessly (§4). (Fresh per case, never reset and reused:
   a case's `desc` is a `Str` — a pointer slot — and re-aiming a shared
   `let` provide's pointer slot is exactly what §3's in-place rule forbids.)
2. its **teardown reports**. Where a plain arena's `destroy` only rewinds,
   `test_arena::destroy` is a **real function** (not the bodyless rewind
   intrinsic) declared `uses Verdicts`: it consumes the accumulator, **dumps
   the case's verdict** (§5), *then* reclaims with the same survivor rewind.
   That is a different teardown, so `test_arena` **is a distinct geometry** —
   a growing arena whose reclamation bracket also reports.

Both reader and writers reach the accumulator the **same** way — the demand
chain. An assertion consumes it by type through the hidden context pointer; the
teardown sits in `case!`'s own source *inside the provide's extent*, so its
`consume` resolves to the case's provide like any other consumer's. One
mechanism, no side channel: context gives every reacher — writer or reader —
its reach. (Reporting from the geometry's `on_scope_exit` hook instead would
also fit the model: a hook body splices inline into the function whose scope
exits ([[geometry_lowering.md]] §1), so a consuming hook is not forbidden — its
demand would simply attribute to the spliced-into function like any inlined
consume. It is machinery the teardown form doesn't need, so v1 reports from
`destroy`; the requirement is only that reporting is **deferred to after the
case body has fully run**, which any bracket-exit placement satisfies.)

The case's ambient node is always its `test_arena` **by construction** — `case!`'s
own source runs the body `in` the arena it just carved, so no caller choice can
repoint it. An `in` clause on the `case!` call itself selects only the **parent**
the `test_arena` carves from — which is precisely how the runner places every case
in the run arena (§6) — and at the top level, where the user writes a `case!`,
there is no binding in scope to name, so no override even exists to write. The
provide's extent is the whole case body ([[context.md]] §2 — extent is lexical,
from the provide to scope end), so every assertion under it, **at any call depth**,
reaches it. (Inside the body a nested `... in fb` is fine — it grafts an allocation
*child*; it does not repoint the case node, and the provide still covers it.)

Because the verdict resolution happens at scope close, the body never returns a
result and never branches on an assertion — it is pure **effect composition**: each
assertion writes onto the provided accumulator, and the close reads what
accumulated. This is the same scope bracket the bump geometries already use to
reclaim their block; the case simply provides a richer value over it and reports
before it reclaims.

All of which is nothing but ordinary C! — `case!` **is std source**, the whole
per-case lifecycle in five lines ([[context.md]]'s provide idiom over a carve,
a callback pinned to it, the reporting teardown deferred):

```
pub const case! = (Str desc, () -> () cb): Uarch -> {
    let Verdicts vd = { desc: desc, count: 0, fails: 0 }
    const ta = test_arena::of(65536)
        |> ctx::provide(let vd)
        |> defer test_arena::destroy
    cb() in ta
    return vd.fails
}
```

`cb` may allocate or not — the `in ta` binds per specialization
([[memory_model.md]] §6, the node-side rule) — and its assertions' `uses
Verdicts` demand rides the value into the specialized direct call
([[context.md]] §4). `todo!` is the trivial sibling: report pending, never call
`cb`, return `0`.

## 4. Assertions

An assertion **consumes the verdict accumulator** ([[context.md]] §4 —
`consume(Verdicts)` binds the case's `let` provide by type) and **appends a
`{ok, message}` verdict** to it in place. The canonical one:

- **`equals(expected, actual)`** — written data-last in a pipe,
  `actual |> equals(expected)`. It compares **structurally**: primitives by value,
  `Str` by bytes, records field by field, arrays by length then element, unions by
  tag then payload. On a match it appends `{ok: true}`; on a mismatch,
  `{ok: false, message}` naming the first differing leaf and both its values
  (`expected 3, got 4 at .pos.x`). A **recursive (self-referential) type is
  rejected at compile time** in v1 — structural comparison of it would not
  terminate.

An assertion is **colorless — never `!`**. It does not allocate: it mutates a
`let` provide in place ([[context.md]] §1 — a non-`!` function may consume and
mutate a `let` provide), and its mismatch message lands in the accumulator's own
inline storage, reserved by the provider. Its demand is `uses Verdicts`; because
demands propagate like `!` does ([[context.md]] §4), an assertion nested inside a
helper the body calls still reaches the case's accumulator — a local helper needs
no annotation, a `pub` cross-module one declares `uses Verdicts` like any context
consumer.

The whole body runs regardless of earlier results: a failed `equals` does not stop
the case, it just leaves a `{ok: false}` on the accumulator. Later assertions still
run and append. At scope close the case fails if **any** verdict is not ok, and the
report lists every mismatch. (This is the deliberate consequence of §3: there is no
return to abort on, and "resolve later, on scope close" is the model.)

An assertion **takes no geometry**: `actual |> equals(expected) in <geom>` is a
compile error — a colorless call selects no node, and there is nothing to redirect.
The consume resolves to the case's provide at comptime ([[context.md]] §4), by
construction.

Further assertions (`not_equals`, `is_true`, …) join later under the same rules:
structural where they compare, colorless, and no geometry.

## 5. The report

Each verdict prints one line, **prefixed by the module of the file that declared
the case** — the file path with `/` rewritten to `::` and the `.cf` extension
dropped, exactly how a module name is formed ([[module_system.md]]):

```
std::something::somewhere -> ✓ my_awesome_function works
std::something::somewhere -> ✗ handles negative input
    expected 3, got 4 at .pos.x
std::something::somewhere -> ⋯ handles the empty input        (todo)
```

- **✓** the case passed (every verdict ok);
- **✗** the case failed (≥1 verdict not ok); each mismatch message follows,
  indented;
- **⋯** a `todo!` — pending, not run, not tallied.

The **test root** owns the tally across every case and the process **exit code**:
`0` when no case failed, non-`1` (`1`) when any did. A `todo!` never affects it.
Output is stdout; a failing build (a real compile error in a test file) is stderr
and a non-zero exit before any case runs, like any `cf` compile.

## 6. The build route

`cf test <file|dir>` (see [[cf_cli.md]] §7) compiles under the test route and runs
the result:

- the module's own `pub const main`, if any, **steps aside** — the test route's
  entry is the runner over the file's `case!`s, not the app;
- every other declaration stays (a `case!` calls the module's real functions);
- the synthesized runner is nothing but the registered calls and a tally — a
  run-level parent arena the per-case `test_arena`s carve from, and each
  registered `case!` invoked directly in it, its returned fail count summed:

  ```
  const main = () -> {
      const run = growing_arena::of(…)
      let Uarch fails = 0
      fails = fails + case!("adds numbers", () -> { … }) in run
      fails = fails + todo!("handles empty", () -> { … }) in run
      return if fails > 0 then 1 else 0
  }
  ```

  Everything per-case — the fresh accumulator, its provide, the `test_arena`,
  the reporting teardown — lives in `case!`'s own std source (§3), not in the
  runner and not in the compiler;
- a **directory** target walks recursively and runs the `case!`s in every `.cf`
  it finds; a **file** target runs that file's;
- `--bail` stops the whole process at the first failing case (see [[cf_cli.md]]
  §7); without it every case runs and the run reports them all.

Any other build — `cf compile`, `cf check`, `cf run` — takes the ordinary route:
the `case!`/`todo!` calls are **thrown away** before resolution, the module's real
`main` is the entry, and the tests contribute nothing to the artifact. (A
consequence: `cf check` does not type-check test bodies; `cf test` is where a test
file is validated. A `cf check --test` could close that later.)

## 7. Deferred

Named here so the surface is stable, but **not v1**:

- **Timeout.** A per-`case!` execution bound, so a hung test cannot hang the run.
  Honest isolation of a runaway case means a fork-per-case or a `SIGALRM`
  watchdog; it lands after the pass/fail core. The deadline rides the case the
  same way the accumulator does — a second value provided on the `test_arena`.
- **Coverage.** "Which code did this `case!` exercise" — evaluated along the way
  via the observer model ([[memory_model.md]]; the `on_reserve` instrumentation
  and `observe::node` query). The test route already carries the richer node the
  observer wants; coverage is a second increment over it.
- **`suite!` / fixtures / parallel scheduling.** §1 — reserved, unbuilt. Running
  cases concurrently is gated by [[context.md]] §7: the accumulator is a `let`
  provide, and a `let` demand crossing a spawn boundary is rejected until atomics
  earn it back — the same rule that keeps serial cases sound makes parallel ones
  wait.
- **More assertions.** §4 — `not_equals`, `is_true`, ordering, error assertions.
```
