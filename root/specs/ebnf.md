# C! Grammar (EBNF)

The grammar of C! in [ISO/IEC 14977](https://www.iso.org/standard/26153.html) EBNF.

This is a living document, built up one construct at a time. Rules marked
_(temporary)_ are placeholders that will be widened in later rounds.

## Conventions

- **Whitespace** between tokens is insignificant and handled at the lexer layer;
  it is not written into every rule.
- **Statements are newline-terminated.** C! has no semicolons. A newline ends a
  statement, so inside a multiline block **every statement sits on its own line**
  — two statements may never share a line. (A newline inside an unclosed `(`,
  `[`, or `{` is not a terminator: a parameter list, aggregate, or data literal
  may still span lines.) Statement termination is a lexer concern and is likewise
  omitted from the rules below.
- **Multi-line blocks set their braces alone.** A statement block `{ … }` that
  spans more than one line puts its opening `{` and closing `}` each on their own
  line — neither shares a line with a statement. A block that fits entirely on one
  line (`… -> { one_stmt }`, a short value block `{ <- v }`) stays inline. This is
  enforced by the parser, not merely a style convention.
- **Comments** start with `#` and run to the end of the line; there are no block
  comments. Like whitespace, a comment is a lexer concern and is stripped before
  the rules below apply.
- Identifier casing is part of the grammar: **types are PascalCase**, **values
  (variable names) are snake_case**.

## Character Classes

```ebnf
uppercase = "A" | "B" | "C" | "D" | "E" | "F" | "G" | "H" | "I" | "J" | "K" | "L" | "M"
          | "N" | "O" | "P" | "Q" | "R" | "S" | "T" | "U" | "V" | "W" | "X" | "Y" | "Z" ;
lowercase = "a" | "b" | "c" | "d" | "e" | "f" | "g" | "h" | "i" | "j" | "k" | "l" | "m"
          | "n" | "o" | "p" | "q" | "r" | "s" | "t" | "u" | "v" | "w" | "x" | "y" | "z" ;

(* digit ladder — each base is a superset of the one above it *)
bin_digit = "0" | "1" ;
oct_digit = bin_digit | "2" | "3" | "4" | "5" | "6" | "7" ;
dec_digit = oct_digit | "8" | "9" ;
hex_digit = dec_digit | "a" | "b" | "c" | "d" | "e" | "f"
                      | "A" | "B" | "C" | "D" | "E" | "F" ;
```

## Identifiers

```ebnf
type_name = uppercase , { uppercase | lowercase | dec_digit } ;         (* PascalCase: Int, Uint8 *)
var_name  = lowercase , { lowercase | dec_digit | "_" } , [ "!" ] ;     (* snake_case: x, the_number, alloc! *)
```

A `var_name` may end in a single `!`. Lexically the `!` is part of the
identifier (`alloc!` is one token). It is the **allocation-effect marker** and
is meaningful only on function names — a `!` function allocates; see the memory
model. Elsewhere a trailing `!` is grammatically permitted but semantically
inert.

A trailing `!` joins the name only when it does **not** open the `!=` operator —
the lexer takes the `!` into the identifier iff the next character is not `=`. So
`alloc!` and `alloc!()` are one identifier, but `n != 0` and `n!= 0` both lex as
`n`, `!=`, `0`. Write `alloc! = …` with a space so the `!` binds to the name
rather than to the operator. (Because `!` is not a legal symbol character for the
QBE/`as` backend, it is mangled off the emitted symbol — internally to a `_b`
suffix (`alloc!` → `alloc_b`), and stripped bare only for a C export
(`alloc!` → `alloc`, which is why `foo` and `foo!` cannot both be exported to C).
See the mangling scheme in [[order_of_compilation.md]].)

## Types

A `type` is recursive — every form composes with every other (`*[Int32]` is a
pointer to an array, `Map[Str, [Int32]]` a generic over a nested array).

```ebnf
type = pointer | bracket_type | named_type | member_access | type_var | tuple_type | func_type ;
                                                                     (* member_access as a type = a qualified member type: Maybe[Int32].Just (§ Union Types) *)

named_type    = type_name , [ "[" , type , { "," , type } , "]" ] ;  (* Int32  |  List[Int32]  |  Map[Str, Int32] *)
pointer       = "*" , type ;                                         (* *Point — points only to aggregates, never a scalar *)

tuple_type   = "(" , [ type , { "," , type } ] , ")" ;               (* () unit  |  (Int32, Bool) — paren product, no "->" return *)
func_type    = "(" , [ type , { "," , type } ] , ")" , "->" , type ; (* (Int32, Int32) -> Int32 — domain, "->", return; no names *)

bracket_type = "[" , [ fixed_array | array ] , "]" ;                 (* [] = empty dynamic array *)
array        = type ;                                                (* [Int32]      — single type *)
fixed_array  = ( integer | var_name ) , type ;                       (* [4 Int32], [n 'T]  — size then type *)

type_var     = "'" , type_name ;                                     (* 'Value       — generic type variable *)
```

Disambiguation inside `[ ]` (both bracket forms share the brackets):

- **empty** `[]` → the **empty dynamic array** value (a zero-element array; the
  **unit** type is `()`, below)
- leading **integer** or **lowercase value name** → `fixed_array` (its size; a
  type never starts with a digit, and every type form starts uppercase, `'`,
  `*`, `[`, or `(` — so a lowercase leading token is a comptime size like the
  `n` of `[n 'T]`)
- a single type → `array`

A **generic** is told apart from a `bracket_type` by the `type_name` that
precedes the `[`; the bracket forms open _with_ `[`.

A **tuple type** is a parenthesized, comma-separated list of types — `(Int32,
Bool)`, the positional product — sharing the parentheses of an argument list and a
function domain. The empty `()` is the **unit** type; a one-element `(T)` is just
`T` (a one-tuple is its element), so parens double as grouping with nothing to
disambiguate.

A **function type** is `(param types) -> return` — the parameter types in parens,
then `->`, then the return type, with **no parameter names**. The `->` is the same
arrow a lambda value uses (Functions): it reads "maps to / yields", and a *type*
yields its **return type** where a *value* yields its **body** — one meaning, two
levels. A `(` at type position opens a **tuple type** unless **`->` follows** the
matching `)`, which makes it a `func_type` — one token of lookahead after the `)`
decides (`(A, B)` is a tuple, `(A, B) -> R` a function). The return type is
**mandatory** in a function type — as with an `intrinsic`, nothing is implied; a
function returning nothing writes `Unit` (or `()`). Because the return is itself a
`type`, higher-order types nest — a function that **returns a function**
parenthesizes the inner one (so the reader never has to guess how a chain of `->`
associates): `(Int32) -> ((Int32) -> Int32)`; `((Int32) -> Int32, Int32) -> Int32`
takes a function as its first parameter.

> **Note.** There is no `&T` *type*. `&` is the **address-of operator** on a `let`
> aggregate value (Expressions, `unary`), producing a `*T` to pass to a `*T`
> parameter (see [[memory_model.md]]); the only pointer *type* is `*T`.

```
type Sum      = (Int32, Int32) -> Int32     (* names a function type *)
type Predicate['T] = ('T) -> Bool           (* generic function type *)
type Thunk    = () -> Unit                   (* no params, returns nothing *)
type Higher   = (Int32) -> ((Int32) -> Int32)  (* returns a function — inner parenthesized *)
```

A `func_type` is an ordinary `type`, so it composes everywhere one is expected —
a `*((Int32) -> Int32)` pointer-to-function, an `[(Int32) -> Int32]` array of them, a
`Map[Str, (Int32) -> Int32]` generic argument, or a `let`/`const`/`param`/field
annotation. Given `type Sum = (Int32, Int32) -> Int32`, a value of that type is a lambda
whose parameter types come **from the annotation**, so they may be left off:

```
const Sum sum = (a, b) -> a + b          (* a, b typed by Sum; body still uses -> *)
```

See Functions for the param rule that lets `(a, b)` drop its types.

## Numbers

Integers come in four bases; floats are decimal only. Any digit run may use `_`
as a separator, but only **between** digits — never leading, trailing, or
doubled (`1_000` is valid, `1_` and `1__0` are not). Base prefixes are
lowercase only. A number literal is **unsigned**; negativity is a unary
operator added with expressions.

The digit sets (`bin_digit`, `oct_digit`, `dec_digit`, `hex_digit`) are defined
as a ladder in [Character Classes](#character-classes).

```ebnf
dec_run = dec_digit , { [ "_" ] , dec_digit } ;
bin_run = bin_digit , { [ "_" ] , bin_digit } ;
oct_run = oct_digit , { [ "_" ] , oct_digit } ;
hex_run = hex_digit , { [ "_" ] , hex_digit } ;

integer = dec_run                (* 42, 1_000_000 *)
        | "0b" , bin_run         (* 0b1010 *)
        | "0o" , oct_run         (* 0o755 *)
        | "0x" , hex_run ;       (* 0xff, 0xDE_AD_BE_EF *)

float = dec_run , "." , dec_run ;   (* 1.0, 3_000.5 — digits on both sides *)
```

## Strings

A string is delimited by `"` and may span multiple lines (raw newlines are
part of the literal). `${ expression }` interpolates any expression;
`\` escapes the next character. Only three escapes are structurally required —
those that would otherwise clash with the string's own delimiters. The full set
of recognized escapes (`\n`, `\t`, `\u{…}`, …) is a lexical detail and will be
pinned down in the lexer spec.

```ebnf
string         = '"' , { string_element } , '"' ;
string_element = interpolation | escape | string_char ;

interpolation  = "${" , expression , "}" ;       (* expression: see Expressions *)

escape         = "\" , escape_char ;
escape_char    = '"' | "\" | "$" ;                (* structural escapes; lexer spec adds \n, \t, \u{…}, … *)

string_char    = ? any source character except an unescaped '"', a '\', or a '$' that starts '${' ? ;
```

A lone `$` not followed by `{` is a literal `$`; `\$` forces a literal `$` even
before `{`.

## Booleans

```ebnf
bool = "true" | "false" ;
```

## Aggregate Literals

Arrays and fixed arrays share one **bracket** literal — a `[…]` comma-separated
list of values; **tuples** are the **paren** literal `(…)` (see Types). The array's
concrete kind (fixed vs dynamic) is a **type** decision resolved by inference or
annotation, not by the parser:

- all elements the same type, or no annotation → fixed array (`[1, 2, 3]` → `[3 Iarch]`)
- empty (`[]`) or an explicit `[Type]` annotation → dynamic array

A **tuple** is written with parentheses and is heterogeneous — each position keeps
its own type (`(1, true)` → `(Iarch, Bool)`).

The three kinds are genuinely distinct — the difference is **iterability and how
they may be indexed**. Arrays use the `[n]` bracket access; a tuple uses the same
bracket but only at a comptime position (tuples have no separate `.n` form):

| kind                | length   | element types | `xs[n]` index                                   | iterable |
| ------------------- | -------- | ------------- | ----------------------------------------------- | -------- |
| fixed array `[N T]` | comptime | homogeneous   | runtime `n` (bounds-checked)                    | yes      |
| tuple `(T1..Tn)`    | comptime | per-position  | **comptime `n` only** (exact per-position type) | no       |
| dynamic array `[T]` | runtime  | homogeneous   | runtime `n` (bounds-checked)                    | yes      |

So `[2 Int32]` (iterable homogeneous buffer) and `(Int32, Int32)` (positional
product) are **different types** with **different syntax** — brackets for the
array, parentheses for the tuple. Restricting tuple indexing to comptime integers
keeps heterogeneous access statically typed with no union type required.
These access rules are semantics and will be formalized in the type-system spec;
the grammar here only defines the literals.

```ebnf
aggregate      = "[" , [ agg_element , { "," , agg_element } ] , "]" ;   (* [], [1, 2, 3], [...x, 4, 5] — arrays only *)
agg_element    = spread_element | expression ;                           (* elements are expressions: 1, x, a + 1, f(y) *)
spread_element = "..." , expression ;                                    (* ...x — splice another aggregate's elements *)

tuple          = "(" , [ tuple_element , { "," , tuple_element } ] , ")" ;   (* () unit, (e) grouping, (1, true), (...t, 4) *)
tuple_element  = spread_element | expression ;                               (* an element or a spread *)
```

An element may be a **spread** `...expr`, which splices the elements of another
aggregate in place: `const y = [...x, 4, 5]` (array) or `const t = (...x, 4, 5)`
(tuple). It mirrors the record `spread`, but one tier down — record spread splices
a _type_'s fields (`...named_type`), an aggregate spread splices a _value_'s
elements (`...expression`). How it resolves follows the kind (see the table
above): for a **tuple** it is a comptime desugar (`(...x, 4, 5)` → `(1, 2, 3, 4,
5)`), for a **fixed array** a runtime concat into a fresh buffer.

For a **dynamic array** the _binding_ decides between growing in place and
copying — the one spread form covers both:

- **Self-spread reassign** — `xs = [...xs, e]`, where the spread source is the
  very name being assigned, grows `xs` **in place**: the elements already there
  stay, `e` is appended, and the backing is reallocated only when it is full
  (amortized doubling). This is the append `array.push` is built on. A bare
  `xs = [...xs]` (nothing added) is a no-op.
- **Spread into another binding** — `let ys = [...xs, e]` is a genuine **copy**:
  `ys` gets a fresh backing holding `xs`'s elements plus the extras, and mutating
  `ys` never touches `xs`. The source may be spread bare (`let ys = [...xs]`, a
  plain clone) or with trailing elements.

`e` here is any element the array's type admits — a scalar, a `data` literal
(`[...xs, { x: 1, y: 2 }]`), or a constructed union member (`[...xs, U.Lit({ v: 5 })]`).
That in-place-vs-copy split, and which element forms a given array accepts, are
semantics; the grammar only admits `...expr` as an element.

## Data Literals

A data literal is a brace-delimited set of `field: value` pairs (fields are
snake_case value names). The type name (e.g. `Point`) comes from the annotation
or context, not the literal — the literal itself is structural. Empty `{}` is
allowed, and a trailing comma is optional (as in every comma-separated list —
data/type/union shapes, array literals, call and constructor arguments, and
parameter lists — so a formatter may explode a long list one item per line). It
constructs an instance of a `data` type (see Data & Type Declarations).

```ebnf
data_literal = "{" , [ field_entry , { "," , field_entry } ] , "}" ;
field_entry  = field_init | field_spread ;
field_init   = var_name , ":" , expression       (* explicit: x: 0, x: a, x: f(y) *)
             | var_name ;                         (* pun: { value } ≡ { value: value } *)
field_spread = "..." , expression ;              (* ...other — splice another record VALUE's fields; later entries win *)
```

A field may be written as a **pun** — a bare `var_name` with no `:`, which is
shorthand for `name: name` (the field is filled from the in-scope variable of the
same name). So with `const value = 1`, the literal `{ value }` means
`{ value: value }`. Parsing forks on a single-token peek after the name: a `:`
begins an explicit `field_init`, anything else (`,` or `}`) is a pun.

A field entry may also be a **value-level spread** `...expr`, which splices another
record _value_'s fields into this literal in place (`{ ...other, z: 3 }`); on a
name collision, later entries win. This is the value-tier echo of the record
_declaration_ spread `...named_type` (Data & Type Declarations, which splices a
_type_'s fields) and of the aggregate spread `...expression` — same `...`, one tier
per plane.

A `{ … }` opens either a **`data_literal`** or a **`block`** — blocks are values
too (see Blocks as Values under Expressions). In **statement** position (a
function body after `->`, a nested block) it is always a `block`. In **value**
position the two overlap, so a two-token peek past the `{` forks them:

- `}` immediately → the empty `data_literal` (`{}`).
- a `var_name` then `:`, `,`, or `}` → `data_literal` (an explicit field or a pun).
- `...` → `data_literal` (a value-level `field_spread`).
- anything else → `block` — a statement keyword (`const`, `let`, `return`, `<-`),
  or a `var_name` that continues as a statement rather than a field (`x = …`,
  `x.f = …`, `x + 1`, `f(x)`), or any other expression-leading token.

So `{ x }` stays the pun and a value block over `x` is `{ <- x }` (a value block
must yield with `<-`, per Control Flow). Whether a data literal may be written
without a type annotation (nominal vs structural typing) is a type-system
decision, not grammar.

## Data & Type Declarations

Two keywords give a name to a type. Both take the same body — a record shape or
**any other `type`** — because a declaration's body _is_ a type; the only
difference is what the name means at run time:

```ebnf
data_decl    = "data" , type_name , [ generic_params ] , "=" , data_body ;
type_decl    = "type" , type_name , [ generic_params ] , "=" , data_body ;
data_body    = record_body | type ;                       (* a record shape, or any other type *)

record_body  = "{" , [ record_entry , { "," , record_entry } , [ "," ] ] , "}" ;
record_entry = field_decl | spread ;
field_decl   = type , var_name , [ "=" , expression ] ;   (* type-first; default optional: Int32 x = 0 *)
spread       = "..." , named_type ;                       (* ...Identifiable — splice another record's fields *)
```

The type name is PascalCase and may be generic (`Box['Value]`); `generic_params`
is the same head as a lambda's (see Functions). The `=` is required. The body
forks on its first token: a `{` opens a `record_body` — fields written
**type-first** (`Int32 x`, the exact shape of a `param`) — and anything else is an
ordinary `type`: a named type (`Int32`), a positional tuple type (`(Int32, Bool)`),
the unit `()`, or a type variable (`'Value`). A leading `(` opens a tuple (or a
function type, if `->` follows the `)`), by the same one-token lookahead
as at type position.

Examples:

```
data Point = { Int32 x, Int32 y, }      (* record shape; trailing comma optional *)
data Box['Value] = { 'Value value }     (* generic record *)
data Wrap['Value] = 'Value              (* newtype over one value — holds exactly one *)
data Empty = ()                         (* unit — no payload *)
data Meters = Uint32                    (* nominal alias of a built-in *)

data Point = { Int32 x = 0, Int32 y = 0 }    (* field defaults *)
data Identifiable = { Str id }
data User = { ...Identifiable, Str email }   (* desugars to { Str id, Str email } *)

type ServerOptions = { Int32 port, Str host }  (* comptime named tuple *)
type Id = Uint64                             (* comptime type alias *)
```

**`data` vs `type`.** Both name a type; they differ in when and how the name
lives:

- **`data`** is a **nominal runtime type** — the name is preserved at run time and
  is distinct from its body even when the two share a shape (`data Meters =
Uint32` is not itself an `Uint32`). This is the sense in which everything is
  data: `Int32` is already a `data`-like runtime type, and `data` just names new
  ones. It carries only shape, no methods or behaviour — the reason the keyword
  is `data`, not `struct`. A record-bodied `data` is built with a data literal
  (`{ x: 0, y: 0 }`); a bracket-bodied one with an aggregate, and so on per its
  body's type.
- **`type`** is a **comptime** name — no runtime identity, a structural alias
  resolved and erased before run time. When its body is a `record_body` it is the
  **named tuple**, which **desugars positionally**, each field expanding to a
  separate variable in place:

  ```
  const listen = (ServerOptions options, Handler handler) -> {}
  ```

  is exactly

  ```
  const listen = (Int32 port, Str host, Handler handler) -> {}
  ```

  Ordering is significant (fields expand in order); the count is not fixed by any
  caller since the whole group splats. With any other body `type` is a plain
  comptime alias (`type Id = Uint64`). This erasure/expansion is a **semantic**
  rule — the grammar only defines the declaration.

Two amendments ride on `record_entry`, so they apply to a `record_body` under
either keyword (and are meaningless on a non-record body):

- **Field defaults** (`Int32 x = 0`) — a field may carry `= expression`, the value
  used when a construction omits it. The inner `=` is unambiguous: it sits after
  a `field_decl` inside the braces, distinct from the declaration's own `=`
  before the body. Whether the default must be comptime, and how a data literal
  omits a defaulted field, are semantic rules.
- **Spread** (`...Identifiable`) — a `record_entry` may be `...` followed by a
  named type, which splices that type's fields into this one **in place**. So
  `data User = { ...Identifiable, Str email }` desugars to `{ Str id, Str email }`.
  Position is preserved; field-name collisions and the spread source's own
  defaults are semantic concerns. The target is a `named_type`, so `...Box[Int32]`
  parses.

Both are top-level declarations (`declaration`, see Visibility), so either may be
`pub`. A `record_body` is only a top-level `data`/`type` body — it is not yet a
`type`, so it cannot nest inside one (`[{ Int32 x }]`); that widening can come
later.

## Union Types

A `union` is an **overarching type** — a named set of **member types**. It is a
_type only_: there is no runtime "union value" distinct from its members. A value
always inhabits exactly one member (a concrete subtype) and carries a **tag** that
says which. A function may take the union as a parameter type — it then accepts
_any_ member and recovers which one with `match` — but you never instantiate the
union itself; you instantiate a member. There is no runtime `Node`, only an
`IntLit`-or-`BinOp`-or-… that a `Node`-typed slot can hold.

A union member takes one of a few forms, along independent axes:

- **Bare `type`** — _resolve-or-create_: if the name already denotes a type in scope (a
  built-in, an imported type, a declared `data`/`union`) the union **composes over** it
  (`Uint8` in `PositiveInteger` below); if it denotes nothing, it is a fresh **nullary**
  member — a pure tag (`Nil`).
- **Named `Name = <rhs>`** — declare member `Name` and **assign** it, mirroring a `data`
  declaration's `=`. The right-hand side is a struct body (`IntLit = { Int32 value }`, a
  payload record), a type (`Wrap = Int32`, the _named_ way to compose over a type), or a
  **literal** (`Semicolon = ";"`, a **singleton**).
- **Positional payload `Name(T1, …, Tn)`** — a member carrying a positional tuple payload
  (`Just('Value)`, `IntLit(Int32)`), the union counterpart of a positional record;
  constructed by application (`Maybe.Just(1)`).
- **Member spread `...Other`** — splice another union's members in place (`...Int` in
  `Number`), the value-union echo of a record's `...named_type` field spread.

A member may also carry its **own `generic_params`**, placing the type parameter on the
member rather than the union head — `Node['Value]('Value)` in an otherwise-concrete union.

These are orthogonal — a union freely mixes bare-resolved, bare-nullary, named, and positional
members. So `union U = { A = { … }, B }` is exactly `data A = { … }`, a nullary `B`, and a
union over `{ A, B }`: the declaration is its members' definition site.

A member is a first-class type, but **how you reach it depends on how it was declared** — you
**import the union, not its members**:

- A **compose-over** member (a standalone type the union merely references, like `Uint8`) is
  reached **on its own** (`Uint8` — you are naming the independent type, which happens to
  also be a member), and redundantly qualified (`PositiveInteger.Uint8`).
- An **inline-declared** member — a nullary tag or a `Name = …` / `Name(…)` payload
  (`Node.Nil`, `Maybe.Nothing`, `Node.IntLit`) — exists **only within its union** and is reached
  **only qualified** (`Maybe.Nothing`, `NodeKind.IntLit`). There is no bare inline member.

The qualified path is not a separate namespace — it is a path _to the same type_ — but it
earns its keep two ways: it disambiguates when short member names collide across unions
(`NodeKind.Let` vs `Kw.Let`), and it lets the module system export a whole union and reach its
members through it. `A.X` and `B.X` stay distinct when `X` sits in both.

**Representation.** A member value is `tag ++ payload`. **Nullary** and **literal** members
carry no payload — they are just their tag (a literal member's spelling is a compile-time
constant recoverable from the tag). A **typed** member (a struct body, a positional payload,
or an assigned type) is `tag + payload`, sized to `tag + max(member payload)`. A union whose
members are _all_ tag-only lowers to a plain integer (this is how `NodeKind`, `Kw`, `TokKind`
work today); a union with any payload member is a tag+payload aggregate, **placed under the
ambient geometry** and passed by pointer like a `data` record. In value position a member
(`Node.Nil`, `Token.Semicolon`) is its **tag** — what `match` dispatches on and what a union-typed
slot holds; a literal member's literal is _additionally_ available as its constant.

```ebnf
union_decl     = "union" , type_name , [ generic_params ] , "=" , union_body ;
union_body     = "{" , union_member , { "," , union_member } , [ "," ] , "}" ;
union_member   = member_name , [ generic_params ] , member_payload   (* named/payload member; may carry its own generics *)
               | member_spread                                        (* ...Other — splice another union's members *)
               | type ;                                               (* bare: compose over an existing type, else a fresh nullary tag *)
member_payload = "(" , [ type , { "," , type } ] , ")"               (* positional tuple payload: Just('Value), IntLit(Int32) *)
               | "=" , ( struct_body | type | literal ) ;            (* = struct body / typed payload / literal singleton *)
member_spread  = "..." , named_type ;                               (* member-level spread; mirrors a record's field spread *)
struct_body    = "{" , field_decl , { "," , field_decl } , [ "," ] , "}" ;
member_name    = type_name ;                                        (* a member is PascalCase, like any type *)
member_access  = named_type , "." , member_name , [ "[" , type , { "," , type } , "]" ] ;
                 (* a qualified member: Maybe.Just (value callee/pattern), Maybe[Int32].Just (a member TYPE),
                    Tree.Node[Int32] (member-level generic — args go where the parameter was declared).
                    Usable as a type (in `type`), as a construction callee (`primary`), and as a `type_pattern` head. *)
```

Examples:

```
union PositiveInteger = { Uint8, Uint16, Uint32, Uint64 }   (* bare → compose over built-ins *)

union Maybe['Value]  = { Just('Value), Nothing }            (* positional payload + inline nullary *)
union Either['L, 'R] = { Left('L), Right('R) }
union Number = { ...Int, ...Uint, ...Float }                (* member spread: splice other unions' members *)

union Node = {
  IntLit = { Int32 value },
  BinOp  = { NodeRef left, NodeRef right, Int32 op },
  Nil,                                              (* fresh nullary tag *)
}

union Token = {
  Semicolon = ";",                                  (* literal singleton *)
  Plus      = "+",
  Ident     = { Int32 start, Int32 len },           (* payload record *)
}
```

`generic_params` on the union head (`Maybe['Value]`) flow into every member that uses them;
alternatively a **member** may carry its own `generic_params` (`Node['Value]('Value)`), leaving
the union un-parameterized. Like the other type declarations a union may be `pub` (see
Visibility) and joins `declaration`.

**Instantiation** is per member, by **construction** — construction is application (see
Expressions): apply the member to its payload. A member's qualified name is **both a
construction callee and a first-class type** — usable anywhere a type is (param, field,
return, binding annotation, type argument), written qualified (`Maybe[Int32].Just`,
`Node.IntLit`):

```
let n = Node.IntLit({ value: 5 })          (* construct: apply the member to its payload; n : Node.IntLit *)
let Node n = Node.IntLit({ value: 5 })     (* annotating with the union WIDENS the precise member to Node *)
```

A member-constructor synthesizes the **precise member type** (`n : Node.IntLit`), which
**widens to the union `Node`** by member→union subtyping only where a union is expected — an
annotation, param, return, or unification against a sibling member (see the type-system spec,
§8). A nullary member is written **qualified** and is just its tag (`Node.Nil`,
`Maybe.Nothing`). There is no bare-union literal `Node { … }` — a `Node`-typed slot is always
filled by a member (constructed, or narrowed via `match`).

**Narrowing** — recovering which member a union *value* is — is done with `match` (see Pattern
Matching). An arm is a member `type_pattern`: the member name **qualified by the scrutinee's
union**, then a parenthesized sub-pattern for its payload (binding the whole payload, or a
`{ … }` record pattern destructuring its fields) — `Maybe.Just(x)`, `Shape.Point({ x, y })`:

```
match n {
  Node.IntLit(v)      -> v.value,
  Node.BinOp({ op })  -> op,
  Node.Nil            -> 0,
  _                   -> -1,
}
```

## Literals _(temporary)_

```ebnf
number = float | integer ;
value  = number | string | bool | aggregate | tuple | data_literal | function ;
```

## Expressions

The expression grammar is grown one form at a time. From loosest to tightest
binding: logical **or** (`||`), logical **and** (`&&`), **comparison**
(`== != < > <= >=`), bitwise **or** (`|`), **xor** (`^`), **and** (`&`),
**shift** (`<< >>`), **additive** (`+ -`), **multiplicative** (`* / %`), prefix
**unary** (`-` negate, `&` address-of, `~` bitwise-not, `!` logical-not — no
dereference), postfix **accessors** (`.field`, `[n]`), and **primary** (a
reference, a construction callee (`Point`, `Maybe.Just`), a literal, or a
parenthesized/tuple expression). Bitwise ops sit below
arithmetic (Rust's ordering), so `(a + b) & mask` reads as intended without
parentheses; comparison sits below bitwise, so `a & b == c` is `(a & b) == c` — no
C footgun; and logical operators sit below comparison, so `a == b && c > d` is
`(a == b) && (c > d)`.

```ebnf
expression     = if_expr | match_expr | loop_expr | for_expr
               | break_expr | continue_expr
               | pipe , [ "in" , logical_or ] ;   (* control-flow forms: see Control Flow / Pattern Matching / Loops; pipe + in clause: below *)
pipe           = logical_or , { "|>" , pipe_target } ;   (* x |> f ≡ f(x); left-associative *)
pipe_target    = postfix | defer_expr ;
defer_expr     = "defer" , ( postfix | block ) ;         (* tap a call (yield its arg) — or schedule a whole block at scope exit *)
logical_or     = logical_and , { "||" , logical_and } ;
logical_and    = comparison , { "&&" , comparison } ;
comparison     = bit_or , [ comparison_op , bit_or ] ;   (* non-associative: no a < b < c *)
comparison_op  = "==" | "!=" | "<" | ">" | "<=" | ">=" ;
bit_or         = bit_xor , { "|" , bit_xor } ;
bit_xor        = bit_and , { "^" , bit_and } ;
bit_and        = shift , { "&" , shift } ;
shift          = additive , { ( "<<" | ">>" ) , additive } ;
additive       = multiplicative , { ( "+" | "-" ) , multiplicative } ;
multiplicative = unary , { ( "*" | "/" | "%" ) , unary } ;
unary          = defer_expr | { "-" | "&" | "~" | "!" } , postfix ;   (* defer tap; - negate, & address-of, ~ bitwise-not, ! logical-not — no deref, see Memory Model *)
postfix        = primary , { field_access | index_access | type_apply | call } ;
field_access   = "." , var_name ;
index_access   = "[" , ( integer | var_name ) , "]" ;                               (* xs[8], xs[i] — array/tuple index *)
type_apply     = type_args ;                                                        (* f[Int32] — partial type application, no call *)
call           = [ type_args ] , "(" , [ argument , { "," , argument } ] , ")" ;   (* f(x)  |  max[Int32](a, b) *)
type_args      = "[" , type_arg , { "," , type_arg } , "]" ;                        (* explicit generics: [Int32], [8, Int32] *)
type_arg       = type | expression ;                                                (* Int32 (type)  |  8 (comptime value) *)
argument       = expression ;
qualified_path = path_seg , "::" , path_seg , { "::" , path_seg } ;   (* a ::-path to a module member: std::comptime::os::target — resolved through the module graph, module_system §4 *)
primary        = qualified_path | var_name | type_name | member_access | value | block ;   (* qualified_path = a namespace-qualified reference (mem::alloc, std::io::print); type_name/member_access = a construction callee (Point, Maybe.Just); a block is a value too; a grouped/tuple ( … ) is the `tuple` value *)
```

Each tier binds tighter than the one above and is left-associative, so
`1 + 2 * 3` is `1 + (2 * 3)`, `a - b - c` is `(a - b) - c`, and `a & b | c` is
`(a & b) | c`. Comparison is the exception — it is **non-associative**: `a < b < c`
is a parse error, write `(a < b) < c` if that is truly meant. A comparison yields
a `Bool`. Logical `&&` and `||` take `Bool` operands, yield a `Bool`, and
short-circuit (semantics). Unary binds tighter than multiplicative
(`-a * b` = `(-a) * b`) and
postfix tighter still (`&p.x` = `&(p.x)`, the address of the field). Parentheses
override everything — `(&p)`, `(1 + 2) * 3`. `&` is both prefix and binary
(address-of / bitwise-and); position disambiguates — a binary operator sits
between two operands, a prefix one leads its operand (`a & &b` = `a & (&b)`).
`*` is **only** binary (multiply): there is **no dereference operator** — an
aggregate pointer is reached through `.field` (which auto-dereferences) and
`[i]`, and there are no pointers to scalars, so a whole-value `*p` is never
needed (see [[memory_model.md]]). Arithmetic operands must be numbers and bitwise
operands integers — type rules, not grammar. Braces are never grouping; they
belong to data literals and to blocks.

**Blocks are values, placed narrowly.** A `block` (defined under Functions) is
grammatically a `primary`, but as a value it is well-formed in only two spots:
the **right-hand side of a binding or assignment**, and as a **standalone
statement** (run for its scope and effects). It is deliberately kept out of
argument, operand, index, aggregate-element, and pipe positions — a block buried
inside a call or expression would clutter the reading. That restriction is a
semantic check, exactly as with lvalue-ness and the `in` clause: the grammar
admits the block and the checker rejects the misplaced ones.

```
let x = {
  const a = compute()
  <- a.total
}
```

runs the block as its own scope and binds its `<-` yield — the same block that
heads a lambda body or an `if`/`match` branch, so it keeps the same rule: **a
block used as a value must yield with `<-`** (no `<-` → no value, rejected where a
value is wanted). Against a `data_literal` the fork is the two-token peek in Data
Literals: `{ x }` is the pun, `{ <- x }` the value block. Because a block is an
ordinary nested scope, an aggregate it yields escapes through the `<-` exactly as
a returned value does, and its own residue dies at the block's scope exit
(`on_ret`; see [[memory_model.md]]).

The index is grammatically an integer literal or a variable; which is _legal_
depends on the receiver's type (a semantic rule — see Aggregate Literals). Arrays,
fixed or dynamic, accept a runtime index (`xs[i]`); a **tuple** accepts only a
comptime **integer literal** (`t[0]`).

A **call** `(…)` is a postfix accessor like `.field` and `[n]`, so it chains and
binds as tightly: `f()[0].x` is `((f())[0]).x`, and `&p.read()` is `&((p.read()))`.
The callee is whatever the postfix chain resolves to — a name, an accessed field,
a parenthesized lambda, or a **type** (a construction callee).

**Construction is application.** A value of a named or scalar type is produced by
**applying that type's constructor** — a `type_name` (or qualified `member_access`)
in callee position: `Point(1, 2)` builds a positional record, `Person({ name: n })`
a named one, `Maybe.Just(1)` a union member, and `Uint8(0)` a scalar. Because the
callee is a **type** (PascalCase) rather than a value (snake_case), the casing alone
tells a construction apart from an ordinary call. A **cast** is the same form over a
value — `Uint16(Uint8(1))`. Type application stays in brackets (`Vector[Uint8]` the
_type_), value construction in parens (`Vector(xs)` the _value_), and the two
compose (`Tree.Node[Int32](x)`). Whether the payload is a tuple `T(a, b)` or a data
literal `T({ f: v })` follows the type's declaration; these are type-system rules,
the grammar only admits the application. An **annotation is the same construction** —
`const Point p = { x: 0, y: 0 }` is `const p = Point({ x: 0, y: 0 })`, so a bare
`{ … }` in value position is a _payload_, typed by the enclosing `T(…)` or the
annotation, never a standalone typed record.

**`.field` auto-dereferences a pointer.** There is no `->` operator and no
dereference `*` — `.` is the only field selector, so on a `*T` it reaches through
to the pointee's field: `p.x` where `p : *Point` is the `x` of the pointed-to
`Point`, identical to `.x` on a bare `Point`. Because pointers point only to
aggregates and exactly one level deep (see [[memory_model.md]]), a single `.`
always lands on the field — there is no multi-level deref to spell out. `&p.x` is
`&(p.x)`, the address of the field, not of `p`.

A call may carry **explicit type arguments** in a `[…]` right before its `(`:
`max[Int32](a, b)`, `zeros[8, Int32]()`. A `[…]` in a postfix chain forks by a
single-token lookahead past the matching `]`, then — if needed — by the receiver:

- **`(` follows** the `]` → the `[…]` is a call's **`type_args`** (`max[Int32](a, b)`).
- **no `(` follows**, contents **type-level** (a `type`, a `'T`, or several
  comma-separated `type_arg`s) → a standalone **`type_apply`** — an array is never
  indexed by a type, so this can only be partial type application
  (`my_complicated_fn[Int32]`).
- **no `(` follows**, contents a **single `integer` or `var_name`** → either an
  `index_access` or a `type_apply`, resolved by the **receiver's type**: an array
  or tuple indexes (`xs[8]`), a function applies it as a comptime type argument
  (`f[8]`). Syntactically identical; the compiler knows which from the receiver.

So `xs[8]` indexes, `xs[8](y)` applies type arg `8` then calls, and
`my_complicated_fn[Int32]` applies just the type. Each `type_arg` is a concrete
`type` (`Int32`) or a comptime `expression` value (`8`); which slots are types vs.
values follows the callee's `generic_params`, a semantic check. Explicit args are
optional — `max(a, b)` infers them.

**Partial application** runs through both brackets and parens: applying fewer
arguments than a function takes yields a function of the rest, left to right.

```
const my_complicated_fn = ['Type]('Type a, Int32 b, Str c, Bool y) -> {}

const my_int_complicated_fn = my_complicated_fn[Int32]       (* apply only the type *)
const applied_one           = my_complicated_fn(1)           (* one value arg *)
const applied_multiple      = my_complicated_fn(1, 2, "yes") (* three value args *)
const result                = applied_multiple(true)         (* fill the last, run it *)
```

The grammar already permits any argument count in a `call` and a bare
`type_apply`; that an under-full application yields a function rather than an
error is the semantic rule, and it is what lets `3 |> sum(2)` (see the pipe,
below) fill the last parameter — `sum(2)` is `(Int32 b) -> sum(2, b)`.

The **pipe** `|>` threads a value into a function: `x |> f` is exactly `f(x)`. It
is left-associative and binds looser than every arithmetic and logical operator
but tighter than the `in` clause, so a chain reads top-to-bottom —

```
let name = " santa  "
  |> str.trim_start
  |> str.trim_end
  |> str.cases.to_pascal
```

is `str.cases.to_pascal(str.trim_end(str.trim_start(" santa  ")))`. The left
operand is any expression (`logical_or`); the right is a `pipe_target` — usually a
`postfix` that resolves to a function: a bare reference (`str.trim_start`), a
field path to one (`str.cases.to_pascal`), or an **under-applied** call (`sum(2)`)
— or a `defer` tap (below). `x |> g` applies
`g` to `x` as its next positional argument. For a bare reference that is the sole
argument (`x |> str.trim_start` = `str.trim_start(x)`); for an under-applied call
it fills the parameter still missing after **partial application** — given

```
const sum = (Int32 a, Int32 b) -> a + b
const result = 3 |> sum(2)     (* sum(2) partially applies a = 2; the pipe fills b = 3 → 5 *)
```

`sum(2)` is `(Int32 b) -> sum(2, b)`, so `3 |> sum(2)` is `sum(2, 3)`. That the right
resolves to something callable is a semantic rule.

**`defer`** is a universal **tapping expression**. `defer f(x)` schedules the call
`f(x)` to run at the **end of the enclosing scope** and evaluates to the call's
**tapped argument** (`x`) — the value passes through, the cleanup is registered.
It is a prefix on a `postfix` (a `unary`), so it composes anywhere a value does:

```
const fd = defer close(open(path))
```

`fd` binds the handle (`open(path)`, the tapped argument) while `close(fd)` is
queued for scope exit. Written on its own line (`defer close(fd)`) it is just an
expression-statement whose value is discarded. As a **pipe target** it needs no
full call — the pipe partially applies the function and the tapped value completes
it, so

```
const fd = open(path) |> defer close
```

is the same thing: `x |> defer f` ≡ `defer f(x)`. `defer` itself does not care
which route supplied the argument. Because the tap forwards its value it may sit
mid-chain (`x |> f |> defer log |> g`). Which argument is the tapped one when a
call takes several, when deferred cleanups fire, and their order (LIFO), are
semantic rules.

**`defer { … }`** defers a whole **block** instead of a single call. The block is
an ordinary nested scope — its own `on_scope_enter`/`on_scope_exit` (a nested mark
on the ambient node, never a new node) — scheduled to run at scope exit. Unlike
the call form it taps no argument and yields nothing, so it is a statement:

```
defer {
  log(a)
  free(b)
}
```

and is **not** a pipe target: a block accepts no piped value. It is the deferred
counterpart of the block-as-value form above; both reuse the one `block`
production.

The **`in` clause** is the single loosest-binding form, below even the pipe:
`f() in arena` is `(f()) in arena`. It wires a call and its whole subtree to a
**geometry** (a memory-placement policy — see the memory model); `arena` is an
ordinary expression naming that geometry. Grammatically the left of `in` is any
expression, but only a **call to a `!` function** may carry it — a geometry
places memory, so it is meaningless on a call that allocates none (a colorless
function) or on a non-call. That the left is a `!` call is a semantic rule, not a
grammatical one; the grammar leaves `in` open on any expression and the type/effect
checker rejects the rest.

## Variable Declaration

The type is optional where it can be **inferred** from the value, and
`const` always carries a value:

- `const` — value required, so the type is always optional.
- `let` — the type may be omitted **only if** a value is present; with no
  value there is nothing to infer from, so the type is required.

This is enforced at the grammar level.

```ebnf
let_decl   = "let" , ( type , var_name , [ "=" , expression ]   (* type given: init optional *)
                     | var_name , "=" , expression ) ;          (* no type: init required *)
const_decl = "const" , [ type ] , var_name , "=" , expression ;
var_decl   = let_decl | const_decl | destructure_decl ;   (* destructure_decl: see Destructuring *)
```

Examples:

```
let Int32 x = 1
const Uint8 the_number = 4
let Int32 y            (* ok — let with type, value optional *)
let x = 1              (* ok — type inferred from value *)
const z = 4           (* ok — type inferred from value *)
let x                 (* invalid — no type and no value to infer from *)
const Int32 z         (* invalid — const must have a value *)
```

## Assignment

A declaration binds a name; **assignment** reassigns an existing one. Only `let`
bindings are reassignable (`const` is not — a semantic rule; see the memory
model). Plain `=` overwrites; the **compound** forms are sugar — `x op= y` is
`x = x op y`, evaluated once for the target.

The target is grammatically a `unary` expression, so `x`, `x.field`, `xs[i]`, and
chains thereof all parse. Whether that expression denotes a real **lvalue** (a
reassignable place, not a temporary like `a + b`) is a semantic check, not a
grammatical one — the parser commits to an assignment the moment it sees an
`assign_op` after the target.

```ebnf
assign_stmt   = assign_target , assign_op , expression ;
assign_target = unary ;   (* lvalue — name, x.field, xs[i], chains; lvalue-ness is semantic *)
assign_op     = "=" | "+=" | "-=" | "*=" | "/=" | "%="
              | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
```

The compound set mirrors the binary operators one-for-one: arithmetic
(`+= -= *= /= %=`) and bitwise (`&= |= ^= <<= >>=`). There is no `&&=` or `||=` —
short-circuit assign only pays off with truthiness coercion, and C!'s `&&`/`||`
take strict `Bool`, so `x = x || y` says it plainly.

Examples:

```
let x = 1
x = 2                 (* plain reassign *)
x += 3                (* x = x + 3 → 5 *)
x <<= 1               (* x = x << 1 → 10 *)

let Point p = { x: 0, y: 0 }
p.x = 4               (* field target *)
xs[i] *= 2            (* index target *)

const c = 1
c = 2                 (* invalid — const is not reassignable (semantic) *)
a + b = 3             (* invalid — target is not an lvalue (semantic) *)
```

## Functions

A function is **not its own declaration form** — it is a `function` value (a
lambda) bound by an ordinary `let`/`const`. A function is thus just a value that
happens to be callable; `const add = (Int32 a, Int32 b): Int32 -> { return a + b }`
reuses `const_decl` wholesale. The lambda is
`[generics] (params) [":" return] -> body`. The `->` **yields the body** (a value),
exactly as the function *type*'s `->` yields its return type; when a lambda states
its return type explicitly, that type is set off with a **colon** before the arrow
(`(a): Int32 -> …`), so the single `->` never has to mean two things at once. The
return type is usually inferred and omitted; state it (via `:`) where inference
can't reach or where you want it documented. The `!` that may end the _name_ is the
allocation-effect marker (see Identifiers and the memory model), part of the
binding's name — not of the lambda.

```ebnf
function       = [ generic_params ] , param_list , [ ":" , type ] , "->" , ( block | expression | asm_block ) ;   (* ":" return set off from the "->" body; asm_block: see Assembly *)

generic_params = "[" , generic_param , { "," , generic_param } , "]" ;   (* ['T] | ['K, 'V] | [Uarch n, 'T] | [NonNegativeInteger 'V] *)
generic_param  = type_var                                          (* 'T                    — unbounded type variable *)
               | type , type_var                                   (* NonNegativeInteger 'V — type variable bounded by a union *)
               | type , var_name ;                                 (* Uarch n               — comptime value param *)

param_list     = "(" , [ param , { "," , param } ] , ")" ;
param          = [ type ] , var_name | type , record_pattern ;   (* Int32 x  |  x (type inferred/annotated)  |  ServerOptions { port, host } *)

block          = "{" , { statement } , "}" ;
statement      = var_decl | return_stmt | yield_stmt | assign_stmt | if_stmt | expression ;   (* (temporary) — widened further with more control flow *)
return_stmt    = "return" , [ expression ] ;                   (* exits the whole function *)
yield_stmt     = "<-" , expression ;                           (* yields a block's value and ends the block (terminal, like return); inside a loop it breaks the loop with that value *)
```

Examples:

```
const sum = (Int32 a, Int32 b) -> a + b              (* single-line body: implicit return *)
const sum2 = (Int32 a, Int32 b) -> {                 (* block body: explicit return *)
  return a + b
}
const add = (Int32 a, Int32 b): Int32 -> { return a + b }  (* explicit return type, set off with ":" *)
const noop = () -> {}                                 (* no params, inferred/none *)
const alloc! = () -> {                                (* ! name: allocates residue *)
  const Point p = { x: 0, y: 0 }
  return p.x
}
const f = ['MyArg] ('MyArg a, 'MyArg b): 'MyArg -> {} (* type var declared with ' *)
const zeros = [Uarch n, 'T] (): [n 'T] -> { }         (* value param n + type param 'T *)
const Sum sum = (a, b) -> a + b                       (* bare params: types come from the Sum annotation *)

alloc!() in arena                                     (* call with in clause *)
f[Int32](1, 2)                                        (* explicit type arg: no ' on Int32 *)
zeros[8, Int32]()                                     (* comptime value + type arg *)
f(1, 2)                                                (* same call, type arg inferred *)
```

Points:

- **A param's type may be omitted** when it can be supplied from elsewhere —
  chiefly a function-type annotation on the binding (`const Sum sum = (a, b) -> …`,
  where `Sum = (Int32, Int32) -> Int32` types `a` and `b`). A bare param is a lone
  `var_name`; a typed one leads with a `type` (`Int32 x`) — cased apart the usual
  way, since a type starts uppercase, `'`, `*`, or `[` and a value name
  lowercase. A `record_pattern` param still requires its type (`ServerOptions { … }`).
  That every bare param's type is actually derivable is a semantic check; the
  grammar only permits the omission.
- **A stated return type is set off with `:` between the params and the arrow**
  (`(Int32 x): Int32 -> …`). Omit it (`(Int32 x) -> …`) to infer it from the body —
  or for a function that returns nothing. The `->` always immediately precedes the
  body; the `:` return keeps it from colliding with the function *type*'s `->`.
- **The body is a `block` or a single `expression`.** `-> a + b` is a
  single-line body whose value is the implicit return; `-> { … }` is a braced
  block that returns via `return`. So `(Int32 a, Int32 b) -> a + b` and
  `(Int32 a, Int32 b) -> { return a + b }` are the same function.
- **A leading `[` opens `generic_params`, not an `aggregate`, when its contents
  are type-level** — a type variable (`'T`) or a `Type name` pair (`Uarch n`).
  Neither is a valid `value`, so `['T]`, `[Uarch n]`, and `[Uarch n, 'T]` can only
  be a generic head, whereas `[1, 2]` (values) is an aggregate. A generic head
  is always followed by the lambda's `(param_list)`, which confirms it.
- **A `{` right after `->` always opens a `block`, never a data literal.**
  This settles the brace ambiguity flagged under Data Literals even though the
  body may now be a bare expression: a single-expression body that _is_ a data
  literal must be parenthesized — `-> ({ x: 0 })`. Inside a block, `{…}` in value
  position forks between a `data_literal` and a value `block` by the two-token
  peek (see Data Literals).
- **`(` opens a lambda vs. a tuple by lookahead to `->`.** In value position a
  `(` begins a `function`'s `param_list` iff the matching `)` (with an optional
  return `type`) is followed by `->`; otherwise it opens a `tuple` value — the
  grouped `(expression)` (a one-tuple ≅ its element) or a genuine product
  `(a, b)`. The bodies also differ — a `param` is `type var_name` (two tokens, no
  operator), which is not a valid expression — but the `->` is the decisive signal.
  Empty `()` followed by `->` is a zero-arg `param_list`; standing alone it is the
  unit value.
- **Generics.** A `generic_param` is a **type variable** `'T` (a bare
  `type_var`), a **bounded type variable** `Union 'V` (a `type` before the tick),
  or a **comptime value param** `Type name` (same shape as an ordinary `param`,
  e.g. `Uarch n`) — a bare `'T` binds an unconstrained type, `Union 'V` binds a
  type constrained to a union's members, and a `Type name` binds a comptime
  value. The three never collide: a leading `'` is the bare `type_var`; after a
  leading `type`, a `'` starts a bounded `type_var` and a lowercase name a value
  param. Inside the body, types, and return, a type variable keeps its tick
  (`'T`, `'V`) and a value param reads as its `var_name` (`n`, usable as a
  `fixed_array` size — `[n 'T]`). The tick lives **only on the accepting side** —
  the definition, where it both introduces the variable and tells it apart from a
  concrete type. At the providing side (a call's explicit `type_args`) you pass a
  concrete type with no tick: `f[Int32]`, never `f['Int32]`.
- **Constraints are unions.** A bound like `[NonNegativeInteger 'Value]` requires
  the argument type to be a member of that union — there is no separate trait
  system; a `union` _is_ the constraint. That the bounding `type` must be a union
  and that the argument satisfies it are semantic checks. Explicit vs. inferred
  type arguments at the call site live in the expression grammar (see
  Expressions).

Calls (`f(x)`, `max[Int32](a, b)`) and the `in` clause live in the expression
grammar (see Expressions), since a call is a postfix accessor and `in` a
low-binding operator; only the lambda and its body are defined here.

## Intrinsics

An **intrinsic** is a function the **compiler** supplies the body for — `size_of`,
`popcount`, the `std::mem::raw` ops, atomics, and the like. Because the standard
library is written in C!, these primitives must bottom out _somewhere_; an
`intrinsic` declaration is that floor. Users **see** them: they are ordinary
top-level bindings that read like any function, minus the body.

The shape reuses the binding skeleton one-for-one — swap `const` for
`intrinsic` and give the **`:`-return signature** (`(params): return`) with no
`-> body` after it (the same `:` return a lambda uses, stopping where its `->`
body would begin). The compiler supplies the body.

```ebnf
intrinsic_decl  = "intrinsic" , var_name , "=" , intrinsic_sig                  (* value intrinsic *)
               | "intrinsic" , var_name , ":" , type                            (* bodyless comptime VALUE — no params *)
               | "intrinsic" , "type" , type_name , [ "=" , constructor_sig ] ;  (* type-valued intrinsic + its constructor; nullary form omits "=" *)
intrinsic_sig   = [ generic_params ] , param_list , ":" , type ;   (* the lambda's ":" return, no "->" body — the compiler supplies it *)
constructor_sig = param_list , ":" , type ;                        (* a type's constructor: (Number value): Uint8 *)
```

An `intrinsic` also names a **type the compiler supplies** — an `intrinsic type`
whose optional `= constructor_sig` gives the type its cast/construction signature
(`pub intrinsic type Uint8 = (Number value): Uint8`), and whose nullary form
(no `=`) is a bare compiler singleton (`pub intrinsic type Infinity`). The name is
PascalCase (a `type_name`); the constructor RHS is an ordinary `:`-return
signature (`(Number value): Uint8`), just like a value intrinsic's.

The **return type is mandatory** — there is no body to infer from, and nothing
is implied. A unit-returning intrinsic states it (`()`, or its alias `Unit`); the
type is never omitted.
Everything else mirrors a lambda: optional `generic_params`, a `param_list`
whose params carry their types, and the `!` allocation marker rides the name
(`copy!`) when the intrinsic allocates. `pub` exports it like any declaration,
and std intrinsics are `pub` so callers can import them.

An intrinsic is a **top-level** form only — it appears in `declaration`, never in
`statement`; there are no local intrinsics.

```
pub intrinsic size_of  = ['T]('T v): Uarch                    (* generic, returns a byte count *)
pub intrinsic popcount = (Uarch x): Uarch
pub intrinsic copy!    = ['T](*'T dst, *'T src, Uarch n): ()  (* allocates; return stated *)
intrinsic fence        = (): ()                               (* module-private *)

pub intrinsic type Uint8 = (Number value): Uint8            (* a width type carrying its cast constructor *)
pub intrinsic type Infinity                                 (* nullary compiler singleton — no constructor *)

pub intrinsic target: Os                                    (* a bodyless comptime VALUE — no params: read `target`, not `target()` *)

pub intrinsic bad      = (Uarch x)                           (* invalid — return type required (needs `: T`) *)
pub intrinsic worse    = (Uarch x): Uarch -> x              (* invalid — the trailing `-> x` is a body; intrinsics take none *)
```

## Assembly

The single escape hatch to raw machine code is an **`asm`-bodied function** — a
lambda whose body, in place of a `block` or expression, is an `asm` block holding
verbatim target assembly. It is the floor beneath the standard library:
syscalls, memory fences, and anything an instruction set exposes but C! (and the
QBE backend) cannot reach, are written here once and wrapped in ordinary C!
above. Nothing else emits raw asm, and there is deliberately **no inline**
(mid-expression) asm — see the end.

```ebnf
asm_block = "asm" , string ;   (* body is an ordinary string — multiline, with ${} *)
```

`asm` is a **reserved keyword**; right after a lambda's `->` it can only open an
`asm_block`, never a reference. The body is an **ordinary `string`** (already
multiline — raw newlines are part of a C! string), so no new literal form is
introduced. The one asm-specific twist is what `${…}` means inside it (below).
`$`, `%`, `#` pass through untouched — only `${` starts an interpolation, and a
lone `$` is literal (see Strings); `\` still escapes, rarely a concern in asm.

**`${…}` interpolation is operand-aware and resolved at comptime** — the same
syntax as any string, but in an asm body a name resolves to its _operand
location_, not a runtime value (asm has none):

- **`${param}`** → the register the parameter occupies (arm64: `${num}` → `x0`).
- **`${SOME_CONST}`** → a comptime value's literal (`mov x16, #${sys_write}` →
  `mov x16, #4`).

So the ABI mapping need not be hand-counted; name the parameter and the compiler
substitutes its register. Bare registers may still be written directly for
scratch.

Rules (semantic, the grammar only places the form):

- **Return type is mandatory**, as with an `intrinsic` — there is no C! body to
  infer from. A unit-returning asm function writes `Unit`.
- **Parameters occupy the target's C-ABI argument registers**, in order, reached
  by `${param}`; the **return value is left in the return register** (arm64:
  `x0..x7` in, `x0` out). The function is **naked** — no prologue/epilogue, so at
  the first instruction the arg registers already hold the arguments; the body
  supplies its own `ret`. This fixed ABI boundary is exactly what lets a
  QBE-lowered caller invoke it by an ordinary `call`; QBE never sees the assembly.
- **No generics.** Assembly is concrete, so an `asm` body takes no
  `generic_params`.
- **Per target.** The text is one architecture's instructions, so a portable
  primitive is one small `asm` function per target behind a single C! signature
  (selected per [[ebnf.md]]-external std convention).

The compiler emits the body **verbatim into the assembly output** (after `${}`
substitution), beside the QBE-lowered functions — not through QBE, which has no
`svc`/syscall instruction and no inline-asm path.

```
pub const syscall6 = (Uarch num, Uarch a0, Uarch a1, Uarch a2,
                      Uarch a3, Uarch a4, Uarch a5): Uarch -> asm "
    mov x16, ${num}
    mov x0,  ${a0}
    mov x1,  ${a1}
    mov x2,  ${a2}
    mov x3,  ${a3}
    mov x4,  ${a4}
    mov x5,  ${a5}
    svc #0x80
    ret
"

pub const write = (Iarch fd, *[Uint8] buf, Uarch n): Uarch ->  (* ordinary C! atop it *)
  syscall6(4, fd, buf, n, 0, 0, 0)
```

**No inline asm.** A mid-expression `asm(…)` would have to interoperate with
QBE's register allocation — naming operands, declaring clobbers — which QBE
cannot support, forcing the whole enclosing function to lower as assembly. The
whole-function form sidesteps this: the asm is a separate symbol at a clean ABI,
QBE-lowered code around it untouched. If a real need ever appears, inline asm can
be revisited; the floor does not require it.

## Control Flow

`if` is primarily an **expression** — it yields a value, so it may be bound
(`let x = if …`) or stand alone as an expression-statement
(`if c then f() else g()`). It is a top-level alternative of `expression` (see
Expressions), so to nest it inside an operator you parenthesize it. `if` also has
a **statement form** (`if_stmt`) for control flow: its branches are *statements*
(they may `return`, `break`, `continue`, `<-`, assign, or call), its `else` is
optional, and it yields no value. The two are distinguished by position and
branch content — a value-`if` supplies a value (its branches are expressions or
`<-`-yielding blocks, and `else` is mandatory so it always yields), whereas a
statement-`if` drives control flow (a branch diverges or acts for effect).

```ebnf
if_expr = "if" , expression , "then" , branch , [ "else" , branch ] ;
branch  = block | expression ;

if_stmt = "if" , expression , "then" , stmt_branch , [ "else" , stmt_branch ] ;
stmt_branch = block | statement ;                              (* a `return`/`break`/`continue`/assign statement, or a block of them *)
```

```
let x = if true then 1 else 2                (* 2 *)
if true then f() else g()                    (* unbound — evaluated for effect *)

const x = if true then {                     (* block branch *)
  const a = 1
  const b = 2
  <- a + b                                   (* yields the block's value *)
} else 4                                      (* branches mix block and single-line freely *)
```

Points:

- **A branch is a single `expression` or a `block`.** After `then`/`else`, a `{`
  opens a `block`; anything else is a single-expression branch. As with a lambda
  body, a branch that _is_ a data literal must be parenthesized — `then ({ x: 0 })`
  — since `then {` is always a block.
- **A block branch yields with `<-`, never `return`.** `<-` (`yield_stmt`)
  propagates a value out of the block to the enclosing block-expression; `return`
  forcefully exits the whole _function_. A block's value is therefore never
  "just the last line" — it is exactly what `<-` yields. **`<-` is terminal**,
  like `return`: it ends its block (or breaks its enclosing loop — see Loops)
  with that value, so a block holds at most one live `<-`, and any statements
  after it are unreachable and ignored — write `<-` last.
- **`if c then { x }` is invalid.** A block in value position must yield through
  `<-`: write the single-line `then x`, or `then { <- x }` if a block is truly
  wanted. A block whose body is a bare expression with no `<-` yields nothing, so
  it is rejected wherever a value is expected — in particular it **cannot be a
  binding or assignment right-hand side**. This is a semantic well-formedness
  rule — the grammar admits the block; the checker rejects it.
- **`else` is optional, and its absence changes the type, not the grammar.** With
  both branches the value is their common type; with only `then`, the value is
  **`Unit`** — the `then` branch runs for effect and the whole expression is `()`
  whether or not the condition holds (no wrapping; the `then` value is discarded).
  This is a type-system rule.
- **`else if` needs no special rule.** A branch is an `expression` and an
  `expression` may itself be an `if_expr`, so `if a then 1 else if b then 2 else 3`
  chains for free.
- **The statement form (`if_stmt`) drives control flow.** Its branches are
  *statements* — `if err then return None`, `if done then break`, `if x < 0 then { … }`
  — so a branch may `return` (an early return from the whole function), `break`/
  `continue` a loop, `<-` a value-loop, assign, or call. Its `else` is optional and
  it yields no value (it is not bound). When **both** branches diverge (each ends in
  `return`/`break`/`continue`/`<-`), the `if` is itself terminal and satisfies a
  function's or block's obligation to end by diverging. `else if` chains for free
  here too (a `stmt_branch` may be another `if_stmt`). The value form and the
  statement form share the `if … then … [else …]` surface; a use that supplies a
  value (bound, or an operand) is the expression, a use that acts for effect or
  diverges is the statement.

`<-` and a comparison `<` never collide: `<-` only ever leads a `yield_stmt`
(no left operand), while `<` is binary and always sits between two operands —
position tells them apart, exactly as it does for prefix-vs-binary `*`/`&`.

## Pattern Matching

`match` is an **expression**, like `if`: it tests a scrutinee against ordered
arms and yields the value of the first arm whose pattern matches. An arm's body
is a `branch` — a single-line expression or a `{ … }` block that yields with
`<-`, exactly as an `if` branch.

```ebnf
match_expr    = "match" , expression , "{" , match_arm , { "," , match_arm } , [ "," ] , "}" ;
match_arm     = or_pattern , "->" , branch ;
or_pattern    = match_pattern , { "|" , match_pattern } ;    (* alternatives — matches any one *)

match_pattern = "_"                                          (* wildcard — matches anything *)
              | literal_pattern                              (* 1, -5, "one", true *)
              | var_name                                     (* binding — matches anything, names it *)
              | type_pattern                                 (* Maybe.Just(x), Maybe.Nothing, Shape.Point({ x, y }) *)
              | record_pattern                               (* { x, y } — by field, see Destructuring *)
              | array_pattern ;                              (* [a, b] — positional, see Destructuring *)
type_pattern  = ( named_type | member_access ) , [ "(" , match_pattern , { "," , match_pattern } , ")" ] ;   (* head may be a qualified member *)
literal_pattern = [ "-" ] , number | string | bool ;         (* a leading "-" negates a NUMBER pattern (`-5`); string/bool take no sign *)
literal       = number | string | bool ;
```

Examples:

```
const num = match v {
  Maybe.Just(1) -> 1 + 1,        (* variant with a literal payload; single-line body *)
  Maybe.Just(x) -> { <- x },     (* variant binding its payload; block body *)
  Maybe.Nothing -> 1,
}

const value_is = match v {
  1 -> "one",
  2 -> "two",
  _ -> "many",             (* wildcard catch-all *)
}

const area = match v {          (* v : Shape — arms are qualified by that union *)
  Shape.Point({ x, y }) -> x + y,   (* record payload destructured by field *)
  Shape.Circle(r)       -> r,       (* single payload bound *)
  Shape.Rect({ w, h })  -> w * h,
}
```

Points:

- **A pattern's first token picks its kind**, with no overlap: `_` is the
  wildcard; a number/string/`true`/`false` is a `literal`; a lowercase name is a
  **binding** (matches anything, names it); an uppercase name is a `type_pattern`;
  a `{` opens a `record_pattern` and a `[` an `array_pattern` (both from
  Destructuring). A `type_pattern`'s head is a member type, optionally followed by
  a parenthesized sub-pattern for its payload. **In a match arm the head names a
  member of the scrutinee's own union, qualified by that union** — `Int.Int32`,
  `Shape.Circle`, `Maybe.Just`, `Number.Int8` — **never bare**, even for a compose-over
  member: the arm names the member *as a case of the matched union*, which frames
  exhaustiveness and keeps the checker from searching for which union a member belongs
  to. (Bare naming of a compose-over member — `Int8`, `Point` — is for type and value
  position; in a `type_pattern` the union qualifier is required. This is also what
  keeps a short name that lives in more than one union unambiguous — see Union Types.)
- **Or-patterns** — `p0 | p1 | …` matches when the scrutinee matches _any_ of the
  alternatives, so one arm can cover several cases (`Weekday.Sat | Weekday.Sun -> …`,
  `NodeKind.IndexField | NodeKind.LocalIndexField -> …`). The separator is a single
  `|`, not `||`: a pattern is never an expression, so `|` in pattern position is
  unambiguously alternation, never bitwise-or (which only exists in expressions).
- **Payload sub-patterns recurse**, so any `match_pattern` may sit inside the
  parens: a literal (`Maybe.Just(1)`), a binding (`Maybe.Just(x)`, `Shape.Circle(r)`), or a
  destructuring pattern (`Shape.Point({ x, y })`, `Shape.Rect({ w, h })`) that pulls the
  payload's fields apart by name. The parenthesized list is positional; a
  `record_pattern` inside it is non-exhaustive as usual (name only the fields you
  want). Deeper nesting composes the same way. A **nested** `type_pattern` head — a
  member matched inside a payload position (`Expr.Bin(Op.Add, l, r)`) — follows the
  qualifier rule of *its* field's type, not the outer scrutinee's: when the field names a
  **standalone union** declared separately and pulled into the payload, the nested member
  may be written **either bare (`Add`) or qualified by that union (`Op.Add`)**; when the
  variants are **declared inline within the enclosing union**, the nested head is **always
  qualified by that union**. (The outer arm head is always qualified — it is a case of the
  scrutinee's own union, above.)
- **Arms are ordered; the first match wins.** Put specific patterns before
  general ones and a `_` (or a bare binding) last. Whether a `match` must be
  **exhaustive** — cover every member of the **scrutinee's union** or carry a
  catch-all — is a semantic rule, as is rejecting arms made unreachable by an earlier
  catch-all and rejecting an arm that names a member outside the scrutinee's union. Guards (an
  `if` condition on an arm) are a later addition.
- **`match` yields a value**, so it composes like any expression and is a
  top-level alternative of `expression` (parenthesize to nest inside an
  operator), same as `if`.

## Loops

Two loop **expressions**: `loop`, the primitive that runs its block forever, and
`for`, which walks an iterable. Both yield a value; both are steered by `break`
and `continue`, which are themselves expressions (of the _never_ type, so they
sit anywhere an expression may — notably as an `if` branch).

```ebnf
loop_expr     = "loop" , [ var_name ] , block ;                 (* infinite loop; optional label *)
for_expr      = "for" , var_name , "in" , expression , branch ; (* iterate a value; nameless *)

break_expr    = "break" , [ var_name ] ;                        (* exit a loop, optionally by label *)
continue_expr = "continue" , [ var_name ] ;                     (* next iteration, optionally by label *)
```

```
const num = loop outer {
  <- loop inner {
    if true then break outer   (* exit the outer loop by name — never-typed, fits the then-branch *)
    <- 1                        (* break inner, yielding 1 *)
  }
}

const [Int32] xs = [1, 2, 3]
let   [Int32] evens = []
for i in xs {
  if i % 2 == 0 then array.push(i, &evens)
}

for i in xs if i % 2 == 0 then array.push(i, &evens)   (* one-line: body is a single expression *)

const two = for i in xs if i == 2 then i else 2        (* for is an expression *)
const two = for i in xs {
  if i != 2 then continue
  <- 2                          (* break the loop, yielding 2 *)
}
```

Points:

- **`loop` is the primitive**, an unconditional repeat with an optional lowercase
  **label** (`outer`). After `loop`, a `var_name` is the label and a `{` opens the
  body, so `loop { … }` and `loop outer { … }` never collide. Its body is a
  `block`.
- **`for var in iterable body`** binds each element to `var` and runs the body.
  `for` is **nameless** — no label. The body is a `branch` (a `block` or a single
  expression), which is what makes it one-lineable (`for i in xs if …`) and an
  expression (`… then i else 2`). The iterable is a full `expression`; because
  expressions never juxtapose (calls always use `()`), the boundary between it
  and the body is unambiguous — `xs { … }` and `xs if …` both split cleanly. The
  `in` here is the loop's own, distinct from the geometry `in` clause (the `for`
  keyword leads, so there is no clash).
- **`break` / `continue` are never-typed expressions.** Each takes an optional
  label naming an enclosing **`loop`** (a `for` has none), and with no label
  targets the nearest loop. Being expressions, they slot straight into an `if`
  branch (`if true then break outer`, `if i != 2 then continue`).
- **A loop's value comes from `<-`, which breaks with that value.** `<- v` inside
  a loop terminates it and yields `v`; a plain `break` terminates with no value.
  A one-line expression body (`for i in xs … then i else 2`) instead carries the
  value of its last evaluated iteration. Exactly what a loop yields on ordinary
  exhaustion or a valueless `break` is a semantic rule; the grammar only places
  the forms.
- **Semantic checks:** `break`/`continue` are legal only inside a loop, and a
  label must name an enclosing `loop`. Conditional `while` is unneeded (`loop` +
  `break` covers it); range/other iterator sugar over `for` can come later.

## Destructuring

Bind the parts of a composite in one step. A **binding** decl replaces the
`var_name` of a `let`/`const` with a **pattern**; each leaf desugars to an
ordinary single binding.

```ebnf
destructure_decl = ( "let" | "const" ) , pattern , "=" , expression ;

pattern        = record_pattern | array_pattern | tuple_pattern ;
record_pattern = "{" , var_name , { "," , var_name } , [ "," ] , "}" ;   (* by field name *)
array_pattern  = "[" , pattern_elem , { "," , pattern_elem } , [ "," ] , "]" ;   (* arrays — by position *)
tuple_pattern  = "(" , pattern_elem , { "," , pattern_elem } , [ "," ] , ")" ;   (* tuples / positional records — by position *)
pattern_elem   = var_name | skip ;                                       (* bind, or skip *)
skip           = "_" , { dec_digit } ;                                   (* _ skips one, _3 skips three *)
```

**Record patterns** (data and named tuples) match by field name and desugar to a
`.field` access each:

```
const Point p = { x: 0, y: 1 }
const { x, y } = p        (* → const x = p.x ; const y = p.y *)
let   { x }    = p        (* let too; non-exhaustive — y simply not bound *)
```

Extraction need not be exhaustive — a field left out of the pattern is just not
bound. Field names only; no renaming yet.

**Array patterns** (fixed and dynamic arrays) and **tuple patterns** (tuples and
positional records) match by position — arrays in `[…]`, tuples in `(…)`, matching
the value syntax. A `skip` drops elements without binding: bare `_` skips one, `_n`
skips `n`. Because `_` and `_2` begin with `_` (not a lowercase letter) they are not
`var_name`s, so there is no clash with a binding.

```
const tuple        = (1, true)
const farr         = [1, 2, 3]
const [Int32] darr = [1, 2, 3]

const (n, b)      = tuple    (* n = tuple[0], b = tuple[1] *)
const [_, a, b]   = farr     (* skip farr[0]; a = farr[1], b = farr[2] *)
const [_2, three] = darr     (* skip two; three = darr[2] *)
```

Whether a match must cover every element (tuples and fixed arrays are
comptime-sized; a dynamic array is runtime, bounds-checked) is a semantic rule.
Nested patterns and a rest element are later widenings.

**Disambiguation in a decl.** After `let`/`const`, a leading `{` can only begin a
`record_pattern` — no `type` starts with `{`. A leading `[` forks against a
bracket `type` (`const [Int32] darr = …`): scan past the matching `]` — a `=` next
means the `[…]` was an `array_pattern`; a `var_name` next means it was the decl's
`type` and the name follows. A leading `(` forks the same way against a tuple /
function `type`: a `=` after the matching `)` means the `(…)` was a `tuple_pattern`;
a `var_name` (or a return `type` then a `var_name`) means it was the decl's `type`
and the name follows. A pattern's `{…}`/`[…]`/`(…)` sits in LHS position, so it is
never read as a `data_literal`, `aggregate`, or `tuple` value (those are values, on
the RHS).

**At the argument level**, a `param` may carry a `record_pattern` in place of its
name — `(ServerOptions { port, host }, Point p)`. This is legal **only for a
named tuple**, whose fields are already splatted into the parameter list as sugar
(see Data & Type Declarations); the pattern just names those splatted
slots. A `data` parameter (`Point p`) is a single real value and **cannot** be
destructured here — it is bound whole, its fields read with `.`. That the type
must be a named tuple is a semantic check; the grammar admits the pattern on any
`param`.

## Imports

An import names a module by a `::`-separated **path** and binds an abbreviation for
it. There is no path string: the segments _are_ the module's location, and the binding
is the path's **last segment** — or an explicit `as` rename, or the destructured names.
Every name an import binds can equally be written as a full `::` path with no import at
all — an import is pure sugar for omitting a namespace prefix (see Modules;
[[module_system.md]] §4).

```ebnf
import_decl  = [ "pub" ] , "import" , module_path , ( [ "::" , import_list ] | [ "as" , ( var_name | "*" ) ] ) ;   (* pub import = reexport *)
module_path  = path_seg , { "::" , path_seg } ;
path_seg     = var_name | type_name ;
import_list  = "{" , import_name , { "," , import_name } , [ "," ] , "}" ;
import_name  = var_name | type_name ;
```

Examples:

```
import std::mem                       (* binds `mem` — mem::alloc, mem::Arena, … *)
import std::mem::{ alloc, Arena }      (* destructures alloc (value) and Arena (type) *)
import std::comptime::os as comptime_os  (* binds `comptime_os` — comptime_os::target *)
pub import std::mem::{ arena }         (* reexport: arena is importable from this module too *)
pub import std::io::console::arm64::darwin as *  (* reexport the WHOLE surface flat *)
```

Four forms:

- **A bare path** `import a::b::c` binds the **last segment** `c` as an abbreviation
  for the whole path: a use `c::member` means `a::b::c::member`. `c` is a namespace
  over the module's entire exported surface — **values and types alike**, told apart
  by the member's own casing (`mem::alloc`, `mem::Arena`). (There is no casing-keyed
  value/type split on the binding: a single `::` path reaches any member.)
- **A renamed path** `import a::b::c as n` binds `n` instead of the last segment `c`
  as the namespace abbreviation, so `n::member` means `a::b::c::member`. The rename is
  namespace-only (it never applies to `::{ … }`); it disambiguates a segment name that
  would collide, and — paired with `pub import` and a `comptime_if` — lets a barrel
  give every platform's implementation a single shared name (`pub import
  …::darwin::arm64 as console`).
- **A destructured path** `import a::b::c::{ x, Y }` pulls the named members straight
  into scope, so `x` means `a::b::c::x` and `Y` means `a::b::c::Y`. It mirrors a
  `record_pattern` but also admits a `type_name`, since a member may itself be a type.
- **A wildcard** `import a::b::c as *` splices `a::b::c`'s **whole exported surface**
  into the current module **flat** — every member at its own name, no nesting and no
  enumeration — so a `pub import … as *` **reexport** forwards the lot without re-listing
  them (and can never fall out of sync when the target gains a member). The natural
  barrel form: `pub import std::io::console::arm64::darwin as *` makes each of the
  platform module's members reachable through the barrel directly (`console::print`),
  not under an implementation-named nesting.

A **`pub import`** is a **reexport**: the names it brings in join this module's own
exported surface, so a module that imports _this_ one may reach them through it. It
reads the same as `pub` on a declaration (see Visibility).

Because an import only abbreviates, it is **never required**: a full path
`a::b::c::member` resolves the same whether or not `a::b::c` was imported (§ Modules;
[[module_system.md]] §4). `std::` is the standard-library root; any other leading
segment is relative to the importing file (§2). `::` is the **namespace/module path**
separator — distinct from `.`, which stays the runtime `field_access` and the
union-variant qualifier (`rec.field`, `Maybe.Just`, `Os.Darwin`). Which names a module
exports is a semantic rule; the grammar fixes only the surface.

## Visibility

A top-level declaration may be prefixed with **`pub`**, which exports it — a
`pub` name can be imported by other modules; without it the declaration is
module-private. There is no scoping argument, just the bare keyword.

```ebnf
declaration = [ "pub" ] , ( data_decl | type_decl | union_decl | var_decl | intrinsic_decl ) ;   (* top-level item *)
```

`pub` sits before the declaration keyword and applies to **data, types, and
values** (`let`/`const`, and thus functions, which are `const`-bound lambdas):

```
pub const x = 1        (* exported — importable *)
const y = 2            (* module-private *)
pub data Point = { Int32 x, Int32 y }
pub type ServerOptions = { Int32 port, Str host }
```

`pub` lives only on this top-level `declaration`; a `var_decl` used as a
`statement` inside a block is local and takes no `pub`. An `import` may also take
`pub` — that is a reexport, not a declaration (see Imports). How the two sit
together in a file is the `module` rule below.

## Modules

A **module is a `.cf` file**. Its top level is a sequence of imports and
declarations in any order; there is no separate module keyword or wrapper.
The "any order" here is about **layout** — imports and declarations may interleave freely,
with no forced grouping; it does not mean every *reference* resolves regardless of position.
**Reference-resolution caveat:** a binding to a **lambda** (a *function*, `const f = (…) -> …`)
may be referenced before its declaration — functions resolve whole-module, so mutual and
forward calls are fine. A binding to a **non-function value** (`const x = <value>`, `let x =
<value>`) must be **declared before it is referenced** — a deliberately orthodox rule that
keeps value-initialization order legible and rules out a class of confusing forward-reference
bugs. The axis is *function-ness*, not the keyword: `const`/`let` chooses mutability, while
being a lambda is what grants order-independence. A genuine **cycle** among value bindings
(`const a = b`, `const b = a`) is an error.

```ebnf
module        = { module_item } ;
module_item   = import_decl | declaration | comptime_if ;   (* comptime_if: build-time selection, below *)
comptime_if   = "if" , expression , "then" , module_branch , [ "else" , module_branch ] ;
module_branch = module_item | "{" , { module_item } , "}" ;   (* one item, or a braced group *)
```

A **`comptime_if`** brings **build-time conditional compilation** to the top
level. It reuses `if … then … else` — position tells it from the value-level
`if_expr`, since a module item is never an expression — but it is evaluated at
**comptime**, during import resolution, against comptime-known values, chiefly the
compiler-supplied `std::comptime` module (`os`, `arch`, and the target). Exactly one
branch's items survive into the module; the losing branch and the `if` scaffolding
itself dissolve. It **surrounds** items — imports never branch internally — so it
is how a single name resolves to a different backing per target:

```
import std::comptime::{ os, arch, Os, Arch }

if os::target == Os.Darwin then
  if arch::target == Arch.Arm64 then import sys::darwin::arm64::{ read_file }
  else import sys::darwin::amd64::{ read_file }
else import sys::linux::{ read_file }
```

Exactly one `read_file` reaches the rest of the module and the selection leaves no
trace. `else if` chains for free (a `module_branch` may itself be a `comptime_if`),
and a branch may brace a group of items. That the condition must be
comptime-evaluable, and the `std::comptime` module's full surface, are semantic
concerns — the latter its own deferred spec; the grammar only admits the form.
(`os::target` reaches the value member through the `::` namespace path; `Os.Darwin`
is a union-variant qualifier and stays on `.`.)

Another module is named by a **`::`-path with no `.cf` extension** (`std::mem`),
resolved to a file by the toolchain — a semantic concern. A module's exported surface
is its `pub` declarations plus its `pub import` reexports; everything else is private
to the file.

## Entry Point

A program's entry is a **`pub const` binding named `main`** — always `main`, never
`main!` — at the top level of the root module. No special syntax, just the
conventional name (see Visibility, Modules); it is an ordinary `declaration`, so
the grammar needs no new rule. The toolchain resolves the entry by name and runs
it without a call.

- **No `!` on the entry.** The allocation-effect marker (see Identifiers) is a
  _caller-facing_ signal — it tells a caller the function allocates. The entry has
  no caller, so the marker would inform nobody; it is therefore always omitted,
  whether or not the body allocates. The entry is `main`, full stop. (Allocation
  still happens inside, in the page geometry below — the name just carries no
  signal no one consumes.)
- **Signature.** The entry returns an `Iarch` exit code and takes the process
  arguments and environment. All three are **optional by arity** — the runtime
  supplies as many as the signature declares:

  ```
  pub const main = () -> 0                                       # no args
  pub const main = (Iarch argc, *[Str] argv) -> { return 0 }      # argc + argv
  pub const main = (Iarch argc, *[Str] argv, *[Str] envp) -> {    # + environment
    return 0
  }
  ```

  `argc` is the count, `argv` a pointer to the argument strings, `envp` a pointer
  to the environment strings. The environment is a **parameter, not ambient
  state**: it is data the runtime hands to `main`, and any other code that needs
  it receives it only by explicit provision from here — no module can reach it on
  its own.

- **Geometry.** The entry runs in the **page geometry**; user-defined geometries
  live inside its body (see the memory model). That the entry runs uncalled, the
  argc/argv/envp wiring, and the single-entry rule are semantic — the grammar
  already admits the declaration as an ordinary `pub const` function.
