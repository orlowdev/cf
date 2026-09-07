; Runnables: each test case (`case!("…", () -> {…})` / `todo!`, testing.md) gets
; a run button in the gutter. The button fires whatever task carries the
; matching tag — define one in tasks.json (user or project `.zed/tasks.json`):
;
;   {
;     "label": "cf test $ZED_RELATIVE_FILE",
;     "command": "cf",
;     "args": ["test", "$ZED_RELATIVE_FILE"],
;     "tags": ["cflang-case"]
;   }
;
; `cf test` runs a whole file's cases (there is no per-case filter).
(
  (call_expression
    (var_name) @_fn
    (arguments (string) @_desc)) @run
  (#match? @_fn "^(case|todo)!$")
  (#set! tag cflang-case)
)
