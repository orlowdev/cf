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
QBE/`as` backend, it is stripped from the emitted symbol — `alloc!` lowers to the
symbol `alloc` — so a program may not define both `foo` and `foo!`.)

## Types

A `type` is recursive — every form composes with every other (`*[Int]` is a
pointer to an array, `Map[Str, [Int]]` a generic over a nested array).

```ebnf
type = pointer | reference | bracket_type | named_type | type_var | func_type ;

named_type   = type_name , [ "[" , type , { "," , type } , "]" ] ;   (* Int  |  List[Int]  |  Map[Str, Int] *)
pointer      = "*" , type ;                                          (* *Int *)
reference    = "&" , type ;                                          (* &Int *)

func_type    = "(" , [ type , { "," , type } ] , ")" , type ;        (* (Int, Int) Int — param types then return; no names, no "->" *)

bracket_type = "[" , [ fixed_array | tuple | array ] , "]" ;         (* [] = unit (empty tuple) *)
array        = type ;                                                (* [Int]        — single type *)
fixed_array  = ( integer | var_name ) , type ;                       (* [4 Int], [n 'T]  — size then type *)
tuple        = type , "," , type , { "," , type } ;                  (* [Int, Bool]  — at least two types *)

type_var     = "'" , type_name ;                                     (* 'Value       — generic type variable *)
```

Disambiguation inside `[ ]` (all three share the brackets):

- **empty** `[]` → the **unit** type (a zero-element tuple; no values)
- leading **integer** or **lowercase value name** → `fixed_array` (its size; a
  type never starts with a digit, and every type form starts uppercase, `'`,
  `*`, `&`, or `[` — so a lowercase leading token is a comptime size like the
  `n` of `[n 'T]`)
- a single type → `array`
- two or more comma-separated types → `tuple`

A **generic** is told apart from a `bracket_type` by the `type_name` that
precedes the `[`; the bracket forms open _with_ `[`.

A **function type** is `(param types) return` — the parameter types in parens,
then the return type, with **no names and no `->`** (that arrow belongs to a
lambda _value_, not its type). A `(` at type position always starts a `func_type`;
parens have no grouping role in a type, so there is nothing else it could be. The
return type is **mandatory** — as with an `intrinsic`, nothing is implied; a
function returning nothing writes `Void`. Because the return is itself a `type`,
higher-order types nest naturally: `(Int) (Int) Int` is a function returning a
function, `((Int) Int, Int) Int` takes a function as its first parameter.

```
type Sum      = (Int, Int) Int          (* names a function type *)
type Predicate['T] = ('T) Bool          (* generic function type *)
type Thunk    = () Void                  (* no params, returns nothing *)
type Higher   = (Int) (Int) Int          (* returns a function *)
```

A `func_type` is an ordinary `type`, so it composes everywhere one is expected —
a `*(Int) Int` pointer-to-function, an `[(Int) Int]` array of them, a
`Map[Str, (Int) Int]` generic argument, or a `let`/`const`/`param`/field
annotation. Given `type Sum = (Int, Int) Int`, a value of that type is a lambda
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

Arrays, fixed arrays, and tuples share one literal — a bracketed, comma-separated
list of values. The concrete kind is a **type** decision resolved by inference or
annotation, not by the parser:

- all elements the same type → fixed array (`[1, 2, 3]` → `[3 Int]`)
- elements of differing types → tuple (`[1, true]` → `[Int, Bool]`)
- empty (`[]`) or an explicit `[Type]` annotation → dynamic array

The three kinds are genuinely distinct — the difference is **iterability and how
they may be indexed**. All use the same `[n]` bracket access syntax (tuples have
no separate `.n` form):

| kind                | length   | element types | `xs[n]` index                                   | iterable |
| ------------------- | -------- | ------------- | ----------------------------------------------- | -------- |
| fixed array `[N T]` | comptime | homogeneous   | runtime `n` (bounds-checked)                    | yes      |
| tuple `[T1..Tn]`    | comptime | per-position  | **comptime `n` only** (exact per-position type) | no       |
| dynamic array `[T]` | runtime  | homogeneous   | runtime `n` (bounds-checked)                    | yes      |

So `[2 Int]` (iterable homogeneous buffer) and `[Int, Int]` (positional record) are
**different types**. A same-typed literal infers to a fixed array; annotate
`[Int, Int]` to get the tuple instead. Restricting tuple indexing to comptime
integers keeps heterogeneous access statically typed with no union type required.
These access rules are semantics and will be formalized in the type-system spec;
the grammar here only defines the literal.

```ebnf
aggregate      = "[" , [ agg_element , { "," , agg_element } ] , "]" ;   (* [], [1, 2, 3], [1, true], [...x, 4, 5] *)
agg_element    = spread_element | expression ;                           (* elements are expressions: 1, x, a + 1, f(y) *)
spread_element = "..." , expression ;                                    (* ...x — splice another aggregate's elements *)
```

An element may be a **spread** `...expr`, which splices the elements of another
aggregate in place: `const y = [...x, 4, 5]`. It mirrors the record `spread`, but
one tier down — record spread splices a _type_'s fields (`...named_type`), an
aggregate spread splices a _value_'s elements (`...expression`). How it resolves
follows the kind (see the table above): for a **tuple** it is a comptime desugar
(`[...x, 4, 5]` → `[1, 2, 3, 4, 5]`), for a **fixed array** a runtime concat into
a fresh buffer.

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
(`[...xs, { x: 1, y: 2 }]`), or a constructed union member (`[...xs, U.Lit { v: 5 }]`).
That in-place-vs-copy split, and which element forms a given array accepts, are
semantics; the grammar only admits `...expr` as an element.

## Data Literals

A data literal is a brace-delimited set of `field: value` pairs (fields are
snake_case value names). The type name (e.g. `Point`) comes from the annotation
or context, not the literal — the literal itself is structural. Empty `{}` is
allowed; there is no trailing comma. It constructs an instance of a `data` type
(see Data & Type Declarations).

```ebnf
data_literal = "{" , [ field_init , { "," , field_init } ] , "}" ;
field_init   = var_name , ":" , expression       (* explicit: x: 0, x: a, x: f(y) *)
             | var_name ;                         (* pun: { value } ≡ { value: value } *)
```

A field may be written as a **pun** — a bare `var_name` with no `:`, which is
shorthand for `name: name` (the field is filled from the in-scope variable of the
same name). So with `const value = 1`, the literal `{ value }` means
`{ value: value }`. Parsing forks on a single-token peek after the name: a `:`
begins an explicit `field_init`, anything else (`,` or `}`) is a pun.

A `{ … }` opens either a **`data_literal`** or a **`block`** — blocks are values
too (see Blocks as Values under Expressions). In **statement** position (a
function body after `->`, a nested block) it is always a `block`. In **value**
position the two overlap, so a two-token peek past the `{` forks them:

- `}` immediately → the empty `data_literal` (`{}`).
- a `var_name` then `:`, `,`, or `}` → `data_literal` (an explicit field or a pun).
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
field_decl   = type , var_name , [ "=" , expression ] ;   (* type-first; default optional: Int x = 0 *)
spread       = "..." , named_type ;                       (* ...Identifiable — splice another record's fields *)
```

The type name is PascalCase and may be generic (`Box['Value]`); `generic_params`
is the same head as a lambda's (see Functions). The `=` is required. The body
forks on its first token: a `{` opens a `record_body` — fields written
**type-first** (`Int x`, the exact shape of a `param`) — and anything else is an
ordinary `type`: a named type (`Int`), a positional bracket type (`[Int, Bool]`,
`['Value]`), the unit `[]`, or a type variable (`'Value`).

Examples:

```
data Point = { Int x, Int y, }          (* record shape; trailing comma optional *)
data Box['Value] = { 'Value value }     (* generic record *)
data Some['Value] = 'Value              (* newtype over one value — holds exactly one *)
data None = []                          (* unit — no payload *)
data Meters = Uint32                    (* nominal alias of a built-in *)

data Point = { Int x = 0, Int y = 0 }   (* field defaults *)
data Identifiable = { Str id }
data User = { ...Identifiable, Str email }   (* desugars to { Str id, Str email } *)

type ServerOptions = { Int port, Str host }  (* comptime named tuple *)
type Id = Uint64                             (* comptime type alias *)
```

**`data` vs `type`.** Both name a type; they differ in when and how the name
lives:

- **`data`** is a **nominal runtime type** — the name is preserved at run time and
  is distinct from its body even when the two share a shape (`data Meters =
Uint32` is not itself an `Uint32`). This is the sense in which everything is
  data: `Int` is already a `data`-like runtime type, and `data` just names new
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
  const listen = (Int port, Str host, Handler handler) -> {}
  ```

  Ordering is significant (fields expand in order); the count is not fixed by any
  caller since the whole group splats. With any other body `type` is a plain
  comptime alias (`type Id = Uint64`). This erasure/expansion is a **semantic**
  rule — the grammar only defines the declaration.

Two amendments ride on `record_entry`, so they apply to a `record_body` under
either keyword (and are meaningless on a non-record body):

- **Field defaults** (`Int x = 0`) — a field may carry `= expression`, the value
  used when a construction omits it. The inner `=` is unambiguous: it sits after
  a `field_decl` inside the braces, distinct from the declaration's own `=`
  before the body. Whether the default must be comptime, and how a data literal
  omits a defaulted field, are semantic rules.
- **Spread** (`...Identifiable`) — a `record_entry` may be `...` followed by a
  named type, which splices that type's fields into this one **in place**. So
  `data User = { ...Identifiable, Str email }` desugars to `{ Str id, Str email }`.
  Position is preserved; field-name collisions and the spread source's own
  defaults are semantic concerns. The target is a `named_type`, so `...Box[Int]`
  parses.

Both are top-level declarations (`declaration`, see Visibility), so either may be
`pub`. A `record_body` is only a top-level `data`/`type` body — it is not yet a
`type`, so it cannot nest inside one (`[{ Int x }]`); that widening can come
later.

## Union Types

A `union` is an **overarching type** — a named set of **member types**. It is a
_type only_: there is no runtime "union value" distinct from its members. A value
always inhabits exactly one member (a concrete subtype) and carries a **tag** that
says which. A function may take the union as a parameter type — it then accepts
_any_ member and recovers which one with `match` — but you never instantiate the
union itself; you instantiate a member. There is no runtime `Node`, only an
`IntLit`-or-`BinOp`-or-… that a `Node`-typed slot can hold.

A union member takes one of two forms, along independent axes:

- **Bare `type`** — _resolve-or-create_: if the name already denotes a type in scope (a
  built-in, an imported type, a declared `data`/`union`) the union **composes over** it
  (`Uint8` in `PositiveInteger` below); if it denotes nothing, it is a fresh **nullary**
  member — a pure tag (`Nil`).
- **Named `Name = <rhs>`** — declare member `Name` and **assign** it, mirroring a `data`
  declaration's `=`. The right-hand side is a struct body (`IntLit = { Int value }`, a
  payload record), a type (`Wrap = Int`, the _named_ way to compose over a type), or a
  **literal** (`Semicolon = ";"`, a **singleton**).

These are orthogonal — a union freely mixes bare-resolved, bare-nullary, and named members.
So `union U = { A = { … }, B }` is exactly `data A = { … }`, a nullary `B`, and a union over
`{ A, B }`: the declaration is its members' definition site. A member is a first-class type
reached bare (`None`) or through its union (`Option.None`). The qualified path is not a
separate namespace — it is a path _to the same type_ — but it earns its keep two ways: it
disambiguates when short member names collide across unions (`NodeKind.Let` vs `Kw.Let`), and
it lets the module system export a whole union and reach its members through it. **You import
the union, not its members**, and `A.X` and `B.X` stay distinct when `X` sits in both.

**Representation.** A member value is `tag ++ payload`. **Nullary** and **literal** members
carry no payload — they are just their tag (a literal member's spelling is a compile-time
constant recoverable from the tag). A **typed** member (a struct body or an assigned type) is
`tag + payload`, sized to `tag + max(member payload)`. A union whose members are _all_ tag-only
lowers to a plain integer (this is how `NodeKind`, `Kw`, `TokKind` work today); a union with any
payload member is a tag+payload aggregate, arena-allocated and passed by pointer like a `data`
record. In value position a member (`Nil`, `Token.Semicolon`) is its **tag** — what `match`
dispatches on and what a union-typed slot holds; a literal member's literal is _additionally_
available as its constant.

```ebnf
union_decl    = "union" , type_name , [ generic_params ] , "=" , union_body ;
union_body    = "{" , union_member , { "," , union_member } , [ "," ] , "}" ;
union_member  = member_name , "=" , ( struct_body | type | literal )   (* named: payload record / typed payload / literal singleton *)
              | type ;                                                   (* bare: compose over an existing type, else a fresh nullary tag *)
struct_body   = "{" , field , { "," , field } , [ "," ] , "}" ;
member_access = type_name , "." , member_name ;   (* a path to the member type; its tag in value position *)
```

Examples:

```
union PositiveInteger = { Uint8, Uint16, Uint32, Uint64 }   (* bare → compose over built-ins *)

union Node = {
  IntLit = { Int value },
  BinOp  = { NodeRef left, NodeRef right, Int op },
  Nil,                                              (* fresh nullary tag *)
}

union Token = {
  Semicolon = ";",                                  (* literal singleton *)
  Plus      = "+",
  Ident     = { Int start, Int len },               (* payload record *)
}
```

`generic_params` on the union head (`Option['Value]`) flow into its members. Like the
other type declarations a union may be `pub` (see Visibility) and joins `declaration`.

**Instantiation** is per member, mirroring `data`'s two forms — annotated or
constructor-expression:

```
let Node.IntLit n = { value: 5 }
let n = Node.IntLit { value: 5 }
```

A nullary member is written bare or qualified and is just its tag (`Nil`, `Node.Nil`).
There is no `let Node n = { … }` — no bare-union value exists to build; a `Node`-typed
slot only ever holds a member produced this way.

**Narrowing** — recovering which member a value is — is done with `match` (see Pattern
Matching). An arm is a member `type_pattern`: the member name, then a parenthesized
sub-pattern for its payload (binding the whole payload, or a `{ … }` record pattern
destructuring its fields) — the same `Some(x)` / `Point({ x, y })` form every variant uses,
no union-only syntax:

```
match n {
  Node.IntLit(v)      -> v.value,
  Node.BinOp({ op })  -> op,
  Nil                 -> 0,
  _                   -> -1,
}
```

## Literals _(temporary)_

```ebnf
number = float | integer ;
value  = number | string | bool | aggregate | data_literal | function ;
```

## Expressions

The expression grammar is grown one form at a time. From loosest to tightest
binding: logical **or** (`||`), logical **and** (`&&`), **comparison**
(`== != < > <= >=`), bitwise **or** (`|`), **xor** (`^`), **and** (`&`),
**shift** (`<< >>`), **additive** (`+ -`), **multiplicative** (`* / %`), prefix
**unary** (`-` negate, `&` address-of, `~` bitwise-not, `!` logical-not — no
dereference), postfix **accessors** (`.field`, `[n]`), and **primary** (a
reference, a literal, or a parenthesized expression). Bitwise ops sit below
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
type_apply     = type_args ;                                                        (* f[Int] — partial type application, no call *)
call           = [ type_args ] , "(" , [ argument , { "," , argument } ] , ")" ;   (* f(x)  |  max[Int](a, b) *)
type_args      = "[" , type_arg , { "," , type_arg } , "]" ;                        (* explicit generics: [Int], [8, Int] *)
type_arg       = type | expression ;                                                (* Int (type)  |  8 (comptime value) *)
argument       = expression ;
primary        = var_name | value | block | "(" , expression , ")" ;   (* a block is a value too — see Blocks as Values; forks from a data_literal by a two-token peek *)
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
or a parenthesized lambda.

**`.field` auto-dereferences a pointer.** There is no `->` operator and no
dereference `*` — `.` is the only field selector, so on a `*T` it reaches through
to the pointee's field: `p.x` where `p : *Point` is the `x` of the pointed-to
`Point`, identical to `.x` on a bare `Point`. Because pointers point only to
aggregates and exactly one level deep (see [[memory_model.md]]), a single `.`
always lands on the field — there is no multi-level deref to spell out. `&p.x` is
`&(p.x)`, the address of the field, not of `p`.

A call may carry **explicit type arguments** in a `[…]` right before its `(`:
`max[Int](a, b)`, `zeros[8, Int]()`. A `[…]` in a postfix chain forks by a
single-token lookahead past the matching `]`, then — if needed — by the receiver:

- **`(` follows** the `]` → the `[…]` is a call's **`type_args`** (`max[Int](a, b)`).
- **no `(` follows**, contents **type-level** (a `type`, a `'T`, or several
  comma-separated `type_arg`s) → a standalone **`type_apply`** — an array is never
  indexed by a type, so this can only be partial type application
  (`my_complicated_fn[Int]`).
- **no `(` follows**, contents a **single `integer` or `var_name`** → either an
  `index_access` or a `type_apply`, resolved by the **receiver's type**: an array
  or tuple indexes (`xs[8]`), a function applies it as a comptime type argument
  (`f[8]`). Syntactically identical; the compiler knows which from the receiver.

So `xs[8]` indexes, `xs[8](y)` applies type arg `8` then calls, and
`my_complicated_fn[Int]` applies just the type. Each `type_arg` is a concrete
`type` (`Int`) or a comptime `expression` value (`8`); which slots are types vs.
values follows the callee's `generic_params`, a semantic check. Explicit args are
optional — `max(a, b)` infers them.

**Partial application** runs through both brackets and parens: applying fewer
arguments than a function takes yields a function of the rest, left to right.

```
const my_complicated_fn = ['Type]('Type a, Int b, Str c, Bool y) -> {}

const my_int_complicated_fn = my_complicated_fn[Int]         (* apply only the type *)
const applied_one           = my_complicated_fn(1)           (* one value arg *)
const applied_multiple      = my_complicated_fn(1, 2, "yes") (* three value args *)
const result                = applied_multiple(true)         (* fill the last, run it *)
```

The grammar already permits any argument count in a `call` and a bare
`type_apply`; that an under-full application yields a function rather than an
error is the semantic rule, and it is what lets `3 |> sum(2)` (see the pipe,
below) fill the last parameter — `sum(2)` is `(Int b) -> sum(2, b)`.

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
const sum = (Int a, Int b) -> a + b
const result = 3 |> sum(2)     (* sum(2) partially applies a = 2; the pipe fills b = 3 → 5 *)
```

`sum(2)` is `(Int b) -> sum(2, b)`, so `3 |> sum(2)` is `sum(2, 3)`. That the right
resolves to something callable is a semantic rule.

**`defer`** is a universal **tapping expression**. `defer f(x)` schedules the call
`f(x)` to run at the **end of the enclosing scope** and evaluates to the call's
**tapped argument** (`x`) — the value passes through, the cleanup is registered.
It is a prefix on a `postfix` (a `unary`), so it composes anywhere a value does:

```
const arena = defer mem.arena.destroy(mem.arena.of(256))
```

`arena` binds the arena (`mem.arena.of(256)`, the tapped argument) while
`mem.arena.destroy(arena)` is queued for scope exit. Written on its own line
(`defer mem.arena.destroy(arena)`) it is just an expression-statement whose value
is discarded. As a **pipe target** it needs no full call — the pipe partially
applies the function and the tapped value completes it, so

```
const arena = mem.arena.of(256) |> defer mem.arena.destroy
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
let Int x = 1
const Uint8 the_number = 4
let Int y              (* ok — let with type, value optional *)
let x = 1              (* ok — type inferred from value *)
const z = 4           (* ok — type inferred from value *)
let x                 (* invalid — no type and no value to infer from *)
const Int z           (* invalid — const must have a value *)
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
happens to be callable; `const add = (Int a, Int b) Int -> { return a + b }`
reuses `const_decl` wholesale. The lambda is
`[generics] (params) [return] -> body`. The `!` that may end the _name_ is the
allocation-effect marker (see Identifiers and the memory model), part of the
binding's name — not of the lambda.

```ebnf
function       = [ generic_params ] , param_list , [ type ] , "->" , ( block | expression | asm_block ) ;   (* asm_block: see Assembly *)

generic_params = "[" , generic_param , { "," , generic_param } , "]" ;   (* ['T] | ['K, 'V] | [Uint n, 'T] | [NonNegativeInteger 'V] *)
generic_param  = type_var                                          (* 'T                    — unbounded type variable *)
               | type , type_var                                   (* NonNegativeInteger 'V — type variable bounded by a union *)
               | type , var_name ;                                 (* Uint n                — comptime value param *)

param_list     = "(" , [ param , { "," , param } ] , ")" ;
param          = [ type ] , var_name | type , record_pattern ;   (* Int x  |  x (type inferred/annotated)  |  ServerOptions { port, host } *)

block          = "{" , { statement } , "}" ;
statement      = var_decl | return_stmt | yield_stmt | assign_stmt | expression ;   (* (temporary) — widened further with more control flow *)
return_stmt    = "return" , [ expression ] ;                   (* exits the whole function *)
yield_stmt     = "<-" , expression ;                           (* yields a block's value and ends the block (terminal, like return); inside a loop it breaks the loop with that value *)
```

Examples:

```
const sum = (Int a, Int b) -> a + b                  (* single-line body: implicit return *)
const sum2 = (Int a, Int b) -> {                     (* block body: explicit return *)
  return a + b
}
const add = (Int a, Int b) Int -> { return a + b }   (* explicit return type *)
const noop = () -> {}                                 (* no params, inferred/none *)
const alloc! = () -> {                                (* ! name: allocates residue *)
  const Point p = { x: 0, y: 0 }
  return p.x
}
const f = ['MyArg] ('MyArg a, 'MyArg b) 'MyArg -> {}  (* type var declared with ' *)
const zeros = [Uint n, 'T] () [n 'T] -> { }           (* value param n + type param 'T *)
const Sum sum = (a, b) -> a + b                       (* bare params: types come from the Sum annotation *)

alloc!() in arena                                     (* call with in clause *)
f[Int](1, 2)                                          (* explicit type arg: no ' on Int *)
zeros[8, Int]()                                       (* comptime value + type arg *)
f(1, 2)                                                (* same call, type arg inferred *)
```

Points:

- **A param's type may be omitted** when it can be supplied from elsewhere —
  chiefly a function-type annotation on the binding (`const Sum sum = (a, b) -> …`,
  where `Sum = (Int, Int) Int` types `a` and `b`). A bare param is a lone
  `var_name`; a typed one leads with a `type` (`Int x`) — cased apart the usual
  way, since a type starts uppercase, `'`, `*`, `&`, or `[` and a value name
  lowercase. A `record_pattern` param still requires its type (`ServerOptions { … }`).
  That every bare param's type is actually derivable is a semantic check; the
  grammar only permits the omission.
- **Return type sits between the params and the arrow** (`(Int x) Int -> …`).
  Omit it (`(Int x) -> …`) to infer it from the body — or for a function that
  returns nothing. The `->` always immediately precedes the body.
- **The body is a `block` or a single `expression`.** `-> a + b` is a
  single-line body whose value is the implicit return; `-> { … }` is a braced
  block that returns via `return`. So `(Int a, Int b) -> a + b` and
  `(Int a, Int b) -> { return a + b }` are the same function.
- **A leading `[` opens `generic_params`, not an `aggregate`, when its contents
  are type-level** — a type variable (`'T`) or a `Type name` pair (`Uint n`).
  Neither is a valid `value`, so `['T]`, `[Uint n]`, and `[Uint n, 'T]` can only
  be a generic head, whereas `[1, 2]` (values) is an aggregate. A generic head
  is always followed by the lambda's `(param_list)`, which confirms it.
- **A `{` right after `->` always opens a `block`, never a data literal.**
  This settles the brace ambiguity flagged under Data Literals even though the
  body may now be a bare expression: a single-expression body that _is_ a data
  literal must be parenthesized — `-> ({ x: 0 })`. Inside a block, `{…}` in value
  position forks between a `data_literal` and a value `block` by the two-token
  peek (see Data Literals).
- **`(` opens a lambda vs. a group by lookahead to `->`.** In value position a
  `(` begins a `function`'s `param_list` iff the matching `)` (with an optional
  return `type`) is followed by `->`; otherwise it opens a parenthesized
  `(expression)`. The bodies also differ — a `param` is `type var_name` (two
  tokens, no operator), which is not a valid expression — but the `->` is the
  decisive signal. Empty `()` is never a group (a group needs an inner
  expression), so it is always a zero-arg `param_list`.
- **Generics.** A `generic_param` is a **type variable** `'T` (a bare
  `type_var`), a **bounded type variable** `Union 'V` (a `type` before the tick),
  or a **comptime value param** `Type name` (same shape as an ordinary `param`,
  e.g. `Uint n`) — a bare `'T` binds an unconstrained type, `Union 'V` binds a
  type constrained to a union's members, and a `Type name` binds a comptime
  value. The three never collide: a leading `'` is the bare `type_var`; after a
  leading `type`, a `'` starts a bounded `type_var` and a lowercase name a value
  param. Inside the body, types, and return, a type variable keeps its tick
  (`'T`, `'V`) and a value param reads as its `var_name` (`n`, usable as a
  `fixed_array` size — `[n 'T]`). The tick lives **only on the accepting side** —
  the definition, where it both introduces the variable and tells it apart from a
  concrete type. At the providing side (a call's explicit `type_args`) you pass a
  concrete type with no tick: `f[Int]`, never `f['Int]`.
- **Constraints are unions.** A bound like `[NonNegativeInteger 'Value]` requires
  the argument type to be a member of that union — there is no separate trait
  system; a `union` _is_ the constraint. That the bounding `type` must be a union
  and that the argument satisfies it are semantic checks. Explicit vs. inferred
  type arguments at the call site live in the expression grammar (see
  Expressions).

Calls (`f(x)`, `max[Int](a, b)`) and the `in` clause live in the expression
grammar (see Expressions), since a call is a postfix accessor and `in` a
low-binding operator; only the lambda and its body are defined here.

## Intrinsics

An **intrinsic** is a function the **compiler** supplies the body for — `sizeof`,
`popcount`, `mem.copy`, atomics, and the like. Because the standard library is
written in C!, these primitives must bottom out _somewhere_; an `intrinsic`
declaration is that floor. Users **see** them: they are ordinary top-level
bindings that read like any function, minus the body.

The shape reuses the binding skeleton one-for-one — swap `const` for
`intrinsic`, keep the lambda signature, drop the `-> body`. The compiler fills
what the `->` would have.

```ebnf
intrinsic_decl = "intrinsic" , var_name , "=" , intrinsic_sig ;
intrinsic_sig  = [ generic_params ] , param_list , type ;   (* lambda signature, no "->" body — the compiler supplies it *)
```

The **return type is mandatory** — there is no body to infer from, and nothing
is implied. A void intrinsic states it (`Void`); the type is never omitted.
Everything else mirrors a lambda: optional `generic_params`, a `param_list`
whose params carry their types, and the `!` allocation marker rides the name
(`copy!`) when the intrinsic allocates. `pub` exports it like any declaration,
and std intrinsics are `pub` so callers can import them.

An intrinsic is a **top-level** form only — it appears in `declaration`, never in
`statement`; there are no local intrinsics.

```
pub intrinsic sizeof   = ['T]() Uint                         (* generic, returns a count *)
pub intrinsic popcount = (Uint x) Uint
pub intrinsic copy!    = ['T](*'T dst, *'T src, Uint n) Void (* allocates; return stated *)
intrinsic fence        = () Void                             (* module-private *)

pub intrinsic bad      = (Uint x)                            (* invalid — return type required *)
pub intrinsic worse    = (Uint x) Uint -> x                  (* invalid — intrinsics take no body *)
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
  infer from. A void asm function writes `Void`.
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
pub const syscall6 = (Uint num, Uint a0, Uint a1, Uint a2,
                      Uint a3, Uint a4, Uint a5) Uint -> asm "
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

pub const write = (Int fd, *[Uint8] buf, Uint n) Uint ->    (* ordinary C! atop it *)
  syscall6(4, fd, buf, n, 0, 0, 0)
```

**No inline asm.** A mid-expression `asm(…)` would have to interoperate with
QBE's register allocation — naming operands, declaring clobbers — which QBE
cannot support, forcing the whole enclosing function to lower as assembly. The
whole-function form sidesteps this: the asm is a separate symbol at a clean ABI,
QBE-lowered code around it untouched. If a real need ever appears, inline asm can
be revisited; the floor does not require it.

## Control Flow

`if` is an **expression**, not a statement — it always yields a value, so it may
be bound (`let x = if …`) or stand alone as an expression-statement
(`if c then f() else g()`). It is a top-level alternative of `expression` (see
Expressions), so to nest it inside an operator you parenthesize it.

```ebnf
if_expr = "if" , expression , "then" , branch , [ "else" , branch ] ;
branch  = block | expression ;
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
  both branches the value is their common type; with only `then`, the value is an
  `Option` — `Some(v)` when the branch runs, `None` when the condition is false
  (`if false then 1` is `None`). This wrapping is a type-system rule.
- **`else if` needs no special rule.** A branch is an `expression` and an
  `expression` may itself be an `if_expr`, so `if a then 1 else if b then 2 else 3`
  chains for free.

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
              | literal                                      (* 1, "one", true *)
              | var_name                                     (* binding — matches anything, names it *)
              | type_pattern                                 (* Some(x), None, Point({ x, y }) *)
              | record_pattern                               (* { x, y } — by field, see Destructuring *)
              | array_pattern ;                              (* [a, b] — positional, see Destructuring *)
type_pattern  = named_type , [ "(" , match_pattern , { "," , match_pattern } , ")" ] ;
literal       = number | string | bool ;
```

Examples:

```
const num = match v {
  Some(1) -> 1 + 1,        (* variant with a literal payload; single-line body *)
  Some(x) -> { <- x },     (* variant binding its payload; block body *)
  None    -> 1,
}

const value_is = match v {
  1 -> "one",
  2 -> "two",
  _ -> "many",             (* wildcard catch-all *)
}

const area = match v {
  Point({ x, y }) -> x + y,     (* record payload destructured by field *)
  Circle(r)       -> r,         (* single payload bound *)
  Rect({ w, h })  -> w * h,
}
```

Points:

- **A pattern's first token picks its kind**, with no overlap: `_` is the
  wildcard; a number/string/`true`/`false` is a `literal`; a lowercase name is a
  **binding** (matches anything, names it); an uppercase name is a `type_pattern`;
  a `{` opens a `record_pattern` and a `[` an `array_pattern` (both from
  Destructuring). A `type_pattern` is a member type (`None`) optionally followed
  by a parenthesized sub-pattern for its payload. A member may be named **bare**
  (`Some`, `None`) or **qualified** through its union (`Option.Some`,
  `NodeKind.IntLit`) — the qualified form disambiguates when the same short name
  lives in more than one union (see Union Types).
- **Or-patterns** — `p0 | p1 | …` matches when the scrutinee matches _any_ of the
  alternatives, so one arm can cover several cases (`Sat | Sun -> …`,
  `NodeKind.IndexField | NodeKind.LocalIndexField -> …`). The separator is a single
  `|`, not `||`: a pattern is never an expression, so `|` in pattern position is
  unambiguously alternation, never bitwise-or (which only exists in expressions).
- **Payload sub-patterns recurse**, so any `match_pattern` may sit inside the
  parens: a literal (`Some(1)`), a binding (`Some(x)`, `Circle(r)`), or a
  destructuring pattern (`Point({ x, y })`, `Rect({ w, h })`) that pulls the
  payload's fields apart by name. The parenthesized list is positional; a
  `record_pattern` inside it is non-exhaustive as usual (name only the fields you
  want). Deeper nesting composes the same way.
- **Arms are ordered; the first match wins.** Put specific patterns before
  general ones and a `_` (or a bare binding) last. Whether a `match` must be
  **exhaustive** — cover every union member or carry a catch-all — is a semantic
  rule, as is rejecting arms made unreachable by an earlier catch-all. Guards (an
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

const [Int] xs = [1, 2, 3]
let   [Int] evens = []
for i in xs {
  if i % 2 == 0 then array.push(i, *evens)
}

for i in xs if i % 2 == 0 then array.push(i, *evens)   (* one-line: body is a single expression *)

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

pattern        = record_pattern | array_pattern ;
record_pattern = "{" , var_name , { "," , var_name } , [ "," ] , "}" ;   (* by field name *)
array_pattern  = "[" , pattern_elem , { "," , pattern_elem } , [ "," ] , "]" ;
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

**Array patterns** (positional tuples, fixed and dynamic arrays) match by
position. A `skip` drops elements without binding: bare `_` skips one, `_n` skips
`n`. Because `_` and `_2` begin with `_` (not a lowercase letter) they are not
`var_name`s, so there is no clash with a binding.

```
const tuple      = [1, true]
const farr       = [1, 2, 3]
const [Int] darr = [1, 2, 3]

const [n, b]      = tuple    (* n = tuple[0], b = tuple[1] *)
const [_, a, b]   = farr     (* skip farr[0]; a = farr[1], b = farr[2] *)
const [_2, three] = darr     (* skip two; three = darr[2] *)
```

Whether a match must cover every element (tuples and fixed arrays are
comptime-sized; a dynamic array is runtime, bounds-checked) is a semantic rule.
Nested patterns and a rest element are later widenings.

**Disambiguation in a decl.** After `let`/`const`, a leading `{` can only begin a
`record_pattern` — no `type` starts with `{`. A leading `[` forks against a
bracket `type` (`const [Int] darr = …`): scan past the matching `]` — a `=` next
means the `[…]` was an `array_pattern`; a `var_name` next means it was the decl's
`type` and the name follows. A pattern's `{…}`/`[…]` sits in LHS position, so it
is never read as a `data_literal` or `aggregate` (those are values, on the RHS).

**At the argument level**, a `param` may carry a `record_pattern` in place of its
name — `(ServerOptions { port, host }, Point p)`. This is legal **only for a
named tuple**, whose fields are already splatted into the parameter list as sugar
(see Data & Type Declarations); the pattern just names those splatted
slots. A `data` parameter (`Point p`) is a single real value and **cannot** be
destructured here — it is bound whole, its fields read with `.`. That the type
must be a named tuple is a semantic check; the grammar admits the pattern on any
`param`.

## Imports

An import names a module by a path string and binds what it brings in. The `as`
target sets both the binding and — by its casing — _what_ is pulled from the
module:

```ebnf
import_decl  = [ "pub" ] , "import" , string , "as" , import_alias ;   (* pub import = reexport *)
import_alias = var_name          (* lowercase namespace: values only — mem.alloc *)
             | type_name         (* PascalCase namespace: types/data only — Math.Vec *)
             | import_list ;     (* destructured: named members, values and types *)
import_list  = "{" , import_name , { "," , import_name } , [ "," ] , "}" ;
import_name  = var_name | type_name ;
```

Examples:

```
import "std/mem" as mem                    (* mem.* — values only *)
import "std/math" as Math                   (* Math.* — types/data only *)
import "std/functor" as { map, Functor }    (* map (value) and Functor (type), direct *)
pub import "std/mem" as { arena }           (* reexport: arena is importable from this module too *)
```

Three `as` targets:

- **`var_name`** (lowercase) binds a namespace exposing the module's **values**
  only, reached as `mem.alloc`.
- **`type_name`** (PascalCase) binds a namespace exposing its **types and data**
  only, reached as `Math.Vec`.
- **`import_list`** destructures named members straight into scope. It mirrors a
  `record_pattern` but also admits a `type_name` (`Functor`), since a module
  member may itself be a type — so it is the one form that pulls in **both**
  kinds at once.

A **`pub import`** is a **reexport**: the names it brings in become part of this
module's own exported surface, so a module that imports _this_ one may import them
from here. It reads the same as `pub` on a declaration (see Visibility) — the
imported binding is simply exported onward.

Which names a module actually exports, and the values-vs-types split keyed on the
alias's case, are semantic rules; the grammar fixes only the surface. The path is
an ordinary `string`. How imports and declarations compose into a file is the
`module` rule (see Modules).

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
pub data Point = { Int x, Int y }
pub type ServerOptions = { Int port, Str host }
```

`pub` lives only on this top-level `declaration`; a `var_decl` used as a
`statement` inside a block is local and takes no `pub`. An `import` may also take
`pub` — that is a reexport, not a declaration (see Imports). How the two sit
together in a file is the `module` rule below.

## Modules

A **module is a `.cf` file**. Its top level is a sequence of imports and
declarations in any order; there is no separate module keyword or wrapper.

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
compiler-supplied `"comptime"` module (`os`, `arch`, and the target). Exactly one
branch's items survive into the module; the losing branch and the `if` scaffolding
itself dissolve. It **surrounds** items — imports never branch internally — so it
is how a single name resolves to a different backing per target:

```
import "comptime" as { os, arch, Os, Arch }

if os.target == Os.Darwin then
  if arch.target == Arch.Arm64 then import "sys/darwin/arm64" as { read_file }
  else import "sys/darwin/amd64" as { read_file }
else import "sys/linux" as { read_file }
```

Exactly one `read_file` reaches the rest of the module and the selection leaves no
trace. `else if` chains for free (a `module_branch` may itself be a `comptime_if`),
and a branch may brace a group of items. That the condition must be
comptime-evaluable, and the `"comptime"` module's full surface, are semantic
concerns — the latter its own deferred spec; the grammar only admits the form.

Another module is named by a **path string with no `.cf` extension**
(`"std/mem"`), resolved to a file by the toolchain — a semantic concern. A
module's exported surface is its `pub` declarations plus its `pub import`
reexports; everything else is private to the file.

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
- **Signature.** The entry returns an `Int` exit code and takes the process
  arguments and environment. All three are **optional by arity** — the runtime
  supplies as many as the signature declares:

  ```
  pub const main = () -> 0                                       # no args
  pub const main = (Int argc, *[Str] argv) -> { return 0 }       # argc + argv
  pub const main = (Int argc, *[Str] argv, *[Str] envp) -> {     # + environment
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
