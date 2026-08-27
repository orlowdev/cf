# C! Parameter Order

**The data a function operates on is its _last_ parameter.** Configuration,
options, and behaviour come first; the subject — the thing being read, mutated,
or produced into — comes last.

```
const set_x = (Int v, *Point p) Void -> { p.x = v }   (* p is the subject → last *)
set_x(7, &pt)

const push  = ['T]('T x, *[T] xs) -> { ... }          (* xs is the subject → last *)
push(item, &list)
```

Not `set_x(&pt, 7)` — the mutated `Point` is the data, so it sits last.

**Why:** partial application fills arguments left to right (see
[[ebnf.md]]), so putting the subject last lets you bind the configuration and get
back a function still awaiting its data:

```
const set_to_7 = set_x(7)        (* config bound; awaits the Point *)
set_to_7(&pt)
```

It also gives every call a predictable shape — the subject is always in the same
place — and reads as "do _this_ (with these settings) _to that_".

**Exemption.** A raw primitive that mirrors an external ABI keeps that ABI's
order, not this one — e.g. `syscall6(num, a0, ..., a5)` follows the kernel's
register layout. The rule governs C!-domain functions, not the asm floor.
