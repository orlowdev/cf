; Document outline / symbol list.
(data_declaration
  "data" @context
  (type_name) @name) @item

(type_declaration
  "type" @context
  (type_name) @name) @item

(union_declaration
  "union" @context
  (type_name) @name) @item

(intrinsic_declaration
  "intrinsic" @context
  (var_name) @name) @item

(static_declaration
  "static" @context
  (var_name) @name) @item

; a `const`/`let` bound to a lambda — a function definition
(const_declaration
  "const" @context
  (var_name) @name
  (function)) @item
(let_declaration
  "let" @context
  (var_name) @name
  (function)) @item

; record fields, shown nested under their type
(field_declaration
  (var_name) @name) @item
