; C! syntax highlighting — queried against tree-sitter-cflang.
; Ordered general -> specific, since a later match overrides an earlier one.

; ---- comments ----------------------------------------------------------------
(comment) @comment

; ---- literals ----------------------------------------------------------------
(integer) @number
(float) @number
; a char literal is an integer in disguise (`'A'` = 65)
(char) @number
(boolean) @boolean

(string) @string
(escape_sequence) @string.escape

; `${ … }` interpolation: the delimiters are punctuation, the inner expression
; keeps its own highlighting.
(interpolation "${" @punctuation.special "}" @punctuation.special)

; ---- types -------------------------------------------------------------------
(type_name) @type
(type_var) @type
(pointer_type "*" @operator)

; a qualified member `Union.Member` — the member reads as a constructor/variant
(member_access "." (type_name) @constructor)

; ---- identifiers -------------------------------------------------------------
(var_name) @variable

; ---- namespaces ---------------------------------------------------------------
; `::`-path segments — an import's module path, and the namespace prefix of a
; qualified reference (`str::eq`, `growing_arena::of`) or type (`console::Key`).
(module_path (var_name) @namespace)
(module_path (type_name) @namespace)
(qualified_name (var_name) @namespace . "::")
(qualified_name (type_name) @namespace . "::")
(named_type (var_name) @namespace . "::")

; parameters
(param (var_name) @variable.parameter)
(generic_param (var_name) @variable.parameter)

; record / data-literal field names are properties
(field_declaration (var_name) @property)
(field_init (var_name) @property)

; field access `a.b` — the accessed member is a property
(field_expression (var_name) @property)

; union member names in a declaration are constructors/variants
(union_member (member_name (type_name) @constructor))

; ---- functions ---------------------------------------------------------------
; a call's callee, whether a bare name, a `.`-path method, or a `::`-path member
(call_expression
  (var_name) @function)
(call_expression
  (field_expression (var_name) @function .))
(call_expression
  (qualified_name (var_name) @function .))

; a PascalCase callee is a construction/cast (`Point(1, 2)`, `Uarch(fd)`)
(call_expression
  (type_name) @constructor)

; a `const`/`let` bound to a lambda is a function definition
(const_declaration
  (var_name) @function
  (function))
(let_declaration
  (var_name) @function
  (function))
(intrinsic_declaration
  (var_name) @function)

; ---- keywords ----------------------------------------------------------------
[
  "const"
  "let"
  "data"
  "type"
  "union"
  "intrinsic"
  "static"
  "import"
  "as"
  "pub"
] @keyword

[
  "if"
  "then"
  "else"
  "match"
  "loop"
  "for"
  "in"
  "return"
  "defer"
] @keyword.control

; `break`/`continue` are whole rules (bare keywords), captured as nodes.
(break_expression) @keyword.control
(continue_expression) @keyword.control

(asm_block "asm" @keyword)

; ---- operators & punctuation -------------------------------------------------
(binary_expression operator: _ @operator)
(assignment_statement operator: _ @operator)
(unary_expression operator: _ @operator)

[
  "->"
  "<-"
  "|>"
  "..."
] @operator

[ "(" ")" "[" "]" "{" "}" ] @punctuation.bracket
[ "," ":" "::" ] @punctuation.delimiter

; `import … as *` — the whole-surface wildcard
(import_declaration "*" @operator)

; ---- pattern matching --------------------------------------------------------
(wildcard_pattern) @variable.special
(type_pattern (named_type (type_name) @type))
; a negated number pattern (`-5`) — the sign is an operator
(negative_pattern "-" @operator)
