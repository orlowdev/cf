; C! syntax highlighting — queried against tree-sitter-cflang.
; Ordered general -> specific, since a later match overrides an earlier one.

; ---- comments ----------------------------------------------------------------
(comment) @comment

; ---- literals ----------------------------------------------------------------
(integer) @number
(float) @number
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
(reference_type "&" @operator)

; ---- identifiers -------------------------------------------------------------
(var_name) @variable

; parameters
(param (var_name) @variable.parameter)
(generic_param (var_name) @variable.parameter)

; record / data-literal field names are properties
(field_declaration (var_name) @property)
(field_init (var_name) @property)

; field access `a.b` — the accessed member is a property
(field_expression (var_name) @property)

; ---- functions ---------------------------------------------------------------
; a call's callee, whether a bare name or a `.`-path method
(call_expression
  (var_name) @function)
(call_expression
  (field_expression (var_name) @function .))

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
[ "," ":" ] @punctuation.delimiter

; ---- pattern matching --------------------------------------------------------
(wildcard_pattern) @variable.special
(type_pattern (named_type (type_name) @type))
