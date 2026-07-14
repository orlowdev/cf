; Indentation: each of these nodes indents the lines it contains one level; the
; closing delimiter dedents its own line.
[
  (block)
  (record_body)
  (data_literal)
  (aggregate)
  (match_expression)
  (param_list)
  (arguments)
  (generic_params)
  (type_args)
  (import_list)
] @indent

["}" "]" ")"] @outdent
