/**
 * Tree-sitter grammar for C! (cflang), translated from root/specs/ebnf.md.
 *
 * Scope: this grammar drives editor tooling (highlighting, brackets, indent,
 * outline), so it is deliberately *permissive* about statement separation —
 * newlines are insignificant whitespace here. The language rule is one
 * statement per line in a multiline block (see the EBNF conventions); that is
 * enforced by the compiler, not by this grammar, which stays lenient so it does
 * not flag the compact cf0 test files.
 */

// Precedence ladder — loosest (low) to tightest (high), mirroring Expressions.
const PREC = {
  in: 1,
  pipe: 2,
  or: 3,
  and: 4,
  comparison: 5,
  bit_or: 6,
  bit_xor: 7,
  bit_and: 8,
  shift: 9,
  additive: 10,
  multiplicative: 11,
  unary: 12,
  postfix: 13,
  primary: 14,
};

module.exports = grammar({
  name: 'cflang',

  word: $ => $._word,

  extras: $ => [/[ \t\r\n\f]/, $.comment],

  supertypes: $ => [$._expression, $._statement, $._type, $._literal],

  conflicts: $ => [
    // `(A, B) …` — a lambda's param_list, a tuple type, or a func type; all open
    // with `(` and fork only at the `)` (a following `->` → func).
    [$.func_type, $.param_list],
    [$.func_type, $.tuple_type],
    // `()` / `(a, b)` in value position — a tuple value or (at type position) a
    // tuple type, forked by whether a type or a value follows the `(`.
    [$.tuple, $.tuple_type],
    // `(x)` — a bare param of a lambda, or a parenthesized value (→ `->` lookahead).
    [$.param, $._primary],
    // `[n …]` fixed-array-type size vs a bracketed literal/index element.
    [$.bracket_type, $.aggregate],
    [$.fixed_array_type, $._primary],
    [$.fixed_array_type, $._literal],
    // `['T]` — a lambda's generic head, or a bracketed type (array of type var).
    [$.generic_param, $._type],
    // `{` after `->`/`then`/`else` is always a block, never a data literal.
    [$.block, $.data_literal],
    // `{ x }` — a block yielding `x`, or a data-literal pun field.
    [$.field_init, $._primary],
    // `xs[8]` (index) vs `f[8]` (type-apply): the EBNF resolves this by the
    // receiver's type, unknown here — bias toward indexing (the common case).
    [$._type_arg, $._literal],
    [$._type_arg, $._primary],
    // `Foo` / `Foo[T]` / `Foo.Bar` — a named type or a bare type_name
    // construction callee, forked by a trailing `.`/`[`/`(`.
    [$.named_type, $._primary],
    // A PascalCase token inside `[ … ]` is a type (array/type-arg) or a
    // construction callee (aggregate element) — kept live for both readings.
    [$._type, $._primary],
    // `Name['V] …` — a member carrying its own generics vs a bare generic type.
    [$.named_type, $.member_name],
    // `then x …` in a branch — a bare expression, or the target of an assignment
    // statement branch (`if c then x = 1`), forked by a following `assign_op`.
    [$.assignment_statement, $._expression],
    // `name :: …` — a qualified_name path segment vs a named_type's namespace
    // prefix; both read `var_name ::` and fork only at the final segment's case.
    [$._path_seg, $.named_type],
    // `import a::b :: …` — the path keeps absorbing segments, or the `::` opens
    // the destructure list (`::{ x, Y }`); forked by the token after the `::`.
    [$.module_path],
  ],

  rules: {
    module: $ => repeat($._module_item),
    _module_item: $ => choice($.import_declaration, $._declaration, $.comptime_if),

    // Build-time conditional compilation at the top level: it reuses
    // `if … then … else`, but a module item is never an expression, so a
    // top-level `if` is always this comptime form. A branch is one item or a
    // braced group; `else if` chains as a branch may itself be a comptime_if.
    comptime_if: $ => prec.right(seq(
      'if', $._expression, 'then', $._module_branch, optional(seq('else', $._module_branch)),
    )),
    _module_branch: $ => choice($._module_item, seq('{', repeat($._module_item), '}')),

    comment: _ => token(seq('#', /.*/)),

    // ---- identifiers -------------------------------------------------------
    // `_word` is the keyword-extraction token: the snake_case value-name shape.
    // A leading `$` is the stack-storage marker and a trailing `!` the
    // allocation marker — both are part of the one identifier token. A leading
    // `_` is admitted for the privileged floor names (`_start`); a bare `_`/`_n`
    // stays the higher-prec wildcard/skip token. The tail admits uppercase too
    // (`rIdx`) — the compiler's lexer does, even though the EBNF spells
    // snake_case; an editor grammar should not flag what compiles.
    _word: _ => /\$?[a-z_][A-Za-z0-9_]*!?/,
    var_name: $ => $._word,
    type_name: _ => /[A-Z][A-Za-z0-9]*/,
    type_var: $ => seq("'", $.type_name),

    // A `::`-path to a module member (`std::io::print`, `growing_arena::of`,
    // `str::eq`) — resolved through the module graph. The last segment may be a
    // value or a type; earlier segments are namespaces.
    qualified_name: $ => prec.right(seq(
      $._path_seg, repeat1(seq('::', $._path_seg)),
    )),
    _path_seg: $ => choice($.var_name, $.type_name),

    // ---- types -------------------------------------------------------------
    // No `&T` type — `&` is only the address-of operator on a value (see EBNF).
    _type: $ => choice(
      $.pointer_type,
      $.bracket_type,
      $.named_type,
      $.member_access,
      $.type_var,
      $.tuple_type,
      $.func_type,
    ),

    // A type may be reached through a `::` namespace path (`console::Key`,
    // `either::Either`) — the leading segments are module namespaces.
    named_type: $ => prec.left(seq(
      repeat(seq($.var_name, '::')),
      $.type_name,
      optional(seq('[', commaSepT($._type_arg), ']')),
    )),

    // A qualified member type/callee: `Maybe.Just`, `Maybe[Int32].Just`,
    // `Tree.Node[Int32]` — usable as a type, a construction callee, and a
    // `type_pattern` head.
    member_access: $ => prec.left(seq(
      $.named_type, '.', $.type_name,
      optional(seq('[', commaSepT($._type_arg), ']')),
    )),

    pointer_type: $ => prec.right(seq('*', $._type)),

    // `(A, B) -> R` — parameter types, `->`, return type (a tuple type + arrow).
    func_type: $ => prec.right(seq(
      '(', optional(commaSep1($._type)), ')', '->', $._type,
    )),

    // `(A, B)` — a positional product; `()` is unit, `(T)` is just `T`.
    tuple_type: $ => seq('(', optional(commaSep1($._type)), ')'),

    bracket_type: $ => seq('[', optional(choice(
      $.fixed_array_type,
      $._type, // array
    )), ']'),

    fixed_array_type: $ => seq(choice($.integer, $.var_name), $._type),

    // A type argument is a type or a comptime value (`[Int]`, `[8, Int]`).
    // Narrowed to types + simple comptime literals/names rather than the full
    // expression grammar, which keeps type-position `[…]` free of the aggregate
    // and data-literal ambiguities.
    _type_arg: $ => choice($._type, $.integer, $.float, $.var_name),

    // ---- literals ----------------------------------------------------------
    _literal: $ => choice(
      $.float,
      $.integer,
      $.char,
      $.string,
      $.boolean,
      $.aggregate,
      $.tuple,
      $.data_literal,
    ),

    // `_` digit separators sit BETWEEN digits only; base prefixes are lowercase.
    integer: _ => token(choice(
      /[0-9](_?[0-9])*/,
      /0b[01](_?[01])*/,
      /0o[0-7](_?[0-7])*/,
      /0x[0-9a-fA-F](_?[0-9a-fA-F])*/,
    )),
    float: _ => token(prec(1, /[0-9](_?[0-9])*\.[0-9](_?[0-9])*/)),

    // A character literal is one byte between apostrophes — an integer in
    // disguise (`'A'` = 65). The closing apostrophe tells it from a `'T` type
    // var; the escape set is its own (`\'` here, `\"`/`\$` in strings).
    char: _ => token(seq("'", choice(/[^\\']/, /\\[ntr0\\']/), "'")),

    boolean: _ => choice('true', 'false'),

    string: $ => seq(
      '"',
      repeat(choice(
        $.interpolation,
        $.escape_sequence,
        $._string_text,
        $._dollar,
      )),
      '"',
    ),
    _string_text: _ => token.immediate(prec(1, /[^"\\$]+/)),
    _dollar: _ => token.immediate('$'),
    escape_sequence: _ => token.immediate(/\\(u\{[0-9a-fA-F]+\}|.)/),
    interpolation: $ => seq(token.immediate('${'), $._expression, '}'),

    aggregate: $ => seq('[', optional(commaSepT(choice($.spread_element, $._expression))), ']'),
    spread_element: $ => seq('...', $._expression),

    // A `tuple` value shares the parens of a group/arg list: `()` is unit, and
    // a genuine product needs a comma (`(1, true)`, `(...t, 4)`); the one-element
    // `(e)` is `parenthesized_expression` (a one-tuple ≅ its element).
    // A comma (or a leading spread) is what marks a tuple apart from a `(e)`
    // grouping. The EBNF product has no trailing comma, but the one-tuple `(a,)`
    // and a trailing comma are admitted here (leniency, as records/arrays allow).
    tuple: $ => choice(
      seq('(', ')'),                                                                    // unit
      seq('(', $.spread_element, repeat(seq(',', $._tuple_element)), optional(','), ')'), // (...t), (...t, 4)
      seq('(', $._expression, repeat1(seq(',', $._tuple_element)), optional(','), ')'),   // (a, b), (a, ...b)
      seq('(', $._expression, ',', ')'),                                                // (a,) one-tuple
    ),
    _tuple_element: $ => choice($.spread_element, $._expression),

    data_literal: $ => seq('{', optional(commaSepT($._field_entry)), '}'),
    _field_entry: $ => choice($.field_init, $.field_spread),
    field_init: $ => choice(
      seq($.var_name, ':', $._expression),
      $.var_name, // pun: { value }
    ),
    field_spread: $ => seq('...', $._expression),   // ...other — splice a record value's fields

    // ---- declarations ------------------------------------------------------
    _declaration: $ => seq(optional('pub'), choice(
      $.data_declaration,
      $.type_declaration,
      $.union_declaration,
      $.let_declaration,
      $.const_declaration,
      $.destructure_declaration,
      $.intrinsic_declaration,
      $.static_declaration,
    )),

    // `static [4096 Uint8] pool` — a module-level BSS byte buffer,
    // program-lifetime, off every geometry. Uint8-only and comptime-sized are
    // semantic rules; the bracket reuses the fixed-array type spelling.
    static_declaration: $ => seq('static', $.bracket_type, $.var_name),

    data_declaration: $ => seq('data', $.type_name, optional($.generic_params), '=', $._data_body),
    type_declaration: $ => seq('type', $.type_name, optional($.generic_params), '=', $._data_body),
    _data_body: $ => choice($.record_body, $._type),

    record_body: $ => seq('{', optional(seq(commaSep1($.record_entry), optional(','))), '}'),
    record_entry: $ => choice($.field_declaration, $.spread),
    field_declaration: $ => seq($._type, $.var_name, optional(seq('=', $._expression))),
    spread: $ => seq('...', $.named_type),

    union_declaration: $ => seq('union', $.type_name, optional($.generic_params),
      '=', $.union_body),
    union_body: $ => seq('{', commaSep1($.union_member), optional(','), '}'),
    union_member: $ => choice(
      // named/payload member; may carry its own generics
      seq($.member_name, optional($.generic_params), $.member_payload),
      $.member_spread,
      $._type,   // bare: compose over an existing type, else a fresh nullary tag
    ),
    member_name: $ => $.type_name,   // a member is PascalCase, like any type
    member_payload: $ => choice(
      seq('(', optional(commaSep1($._type)), ')'),          // positional tuple payload
      seq('=', choice($.struct_body, $._type, $._singleton)), // struct / typed / literal
    ),
    member_spread: $ => seq('...', $.named_type),
    struct_body: $ => seq('{', commaSep1($.field_declaration), optional(','), '}'),
    // a literal singleton member (`Semicolon = ";"`): number | string | bool
    _singleton: $ => choice($.integer, $.float, $.string, $.boolean),

    let_declaration: $ => seq('let', choice(
      seq($._type, $.var_name, optional(seq('=', $._expression))),
      seq($.var_name, '=', $._expression),
    )),
    const_declaration: $ => seq('const', optional($._type), $.var_name, '=', $._expression),

    destructure_declaration: $ => seq(choice('let', 'const'), $._pattern, '=', $._expression),

    intrinsic_declaration: $ => choice(
      seq('intrinsic', $.var_name, '=', $.intrinsic_signature),
      // `intrinsic target: Os` — a bodyless comptime VALUE (no params, no `=`)
      seq('intrinsic', $.var_name, ':', $._type),
      // type-valued intrinsic + its constructor; nullary form omits the `=`
      seq('intrinsic', 'type', $.type_name, optional(seq('=', $.constructor_signature))),
    ),
    // the signature stops at the lambda's `:` return — no `->`, no body
    intrinsic_signature: $ => seq(optional($.generic_params), $.param_list, ':', $._type),
    constructor_signature: $ => seq($.param_list, ':', $._type),

    // ---- functions ---------------------------------------------------------
    // A stated return type is set off with `:` between the params and the `->`.
    function: $ => prec.right(seq(
      optional($.generic_params),
      $.param_list,
      optional(seq(':', $._type)),
      '->',
      choice($.block, $.asm_block, $._expression),
    )),

    generic_params: $ => seq('[', commaSepT($.generic_param), ']'),
    generic_param: $ => choice(
      $.type_var,
      seq($._type, $.type_var),
      seq($._type, $.var_name),
    ),

    param_list: $ => seq('(', optional(commaSepT($.param)), ')'),
    param: $ => choice(
      seq($._type, $.record_pattern),
      seq($._type, $.var_name),
      $.var_name,
    ),

    asm_block: $ => seq('asm', $.string),

    // A `{` right after `->`/`then`/`else`/loop opens a block; the dynamic
    // precedence resolves the empty-`{}` tie against a data literal in favour of
    // the block (per the EBNF).
    block: $ => prec.dynamic(1, seq('{', repeat($._statement), '}')),

    _statement: $ => choice(
      $.let_declaration,
      $.const_declaration,
      $.destructure_declaration,
      $.return_statement,
      $.yield_statement,
      $.assignment_statement,
      $._expression,
    ),

    return_statement: $ => prec.right(seq('return', optional($._expression))),
    yield_statement: $ => seq('<-', $._expression),

    assignment_statement: $ => prec.right(seq(
      field('left', $._unary),
      field('operator', choice('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=')),
      field('right', $._expression),
    )),

    // ---- expressions -------------------------------------------------------
    _expression: $ => choice(
      $.if_expression,
      $.match_expression,
      $.loop_expression,
      $.for_expression,
      $.break_expression,
      $.continue_expression,
      $.in_expression,
      $.pipe_expression,
      $.binary_expression,
      $._unary,
    ),

    in_expression: $ => prec.left(PREC.in, seq($._expression, 'in', $._expression)),

    pipe_expression: $ => prec.left(PREC.pipe, seq(
      $._expression, '|>', choice($._postfix, $.defer_expression),
    )),

    binary_expression: $ => {
      const table = [
        ['||', PREC.or], ['&&', PREC.and],
        ['|', PREC.bit_or], ['^', PREC.bit_xor], ['&', PREC.bit_and],
        ['<<', PREC.shift], ['>>', PREC.shift],
        ['+', PREC.additive], ['-', PREC.additive],
        ['*', PREC.multiplicative], ['/', PREC.multiplicative], ['%', PREC.multiplicative],
      ];
      const binary = table.map(([op, p]) => prec.left(p, seq(
        field('left', $._expression),
        field('operator', op),
        field('right', $._expression),
      )));
      // comparison is non-associative (no a < b < c)
      const cmp = ['==', '!=', '<', '>', '<=', '>='].map(op => prec.left(PREC.comparison, seq(
        field('left', $._expression),
        field('operator', op),
        field('right', $._expression),
      )));
      return choice(...binary, ...cmp);
    },

    _unary: $ => choice($.unary_expression, $.defer_expression, $._postfix),
    unary_expression: $ => prec.right(PREC.unary, seq(
      field('operator', choice('-', '&', '~', '!')),
      $._postfix,
    )),
    // `defer f(x)` taps a call; `defer { … }` schedules a whole block at scope exit.
    defer_expression: $ => prec.right(PREC.unary, seq('defer', choice($._postfix, $.block))),

    _postfix: $ => choice(
      $.field_expression,
      $.index_expression,
      $.type_apply_expression,
      $.call_expression,
      $._primary,
    ),

    field_expression: $ => prec.left(PREC.postfix, seq($._postfix, '.', $.var_name)),
    // The EBNF restricts an index to `integer | var_name`, but real code indexes
    // with full expressions (`src[start + k]`, `toks[nt - 1]`); the highlighter
    // accepts any expression so it does not flag them.
    index_expression: $ => prec.dynamic(1, prec.left(PREC.postfix, seq($._postfix, '[', $._expression, ']'))),
    type_apply_expression: $ => prec.left(PREC.postfix, seq($._postfix, $.type_args)),
    call_expression: $ => prec.left(PREC.postfix, seq(
      $._postfix, optional($.type_args), $.arguments,
    )),
    type_args: $ => seq('[', commaSepT($._type_arg), ']'),
    arguments: $ => seq('(', optional(commaSepT($._expression)), ')'),

    // A `type_name`/`member_access` in value position is a construction callee
    // (`Point(1, 2)`, `Uarch(fd)`, `Maybe.Just(1)`) — the PascalCase casing tells
    // a construction apart from an ordinary snake_case call. A `qualified_name`
    // is a `::`-path reference (`str::eq`, `growing_arena::of`).
    _primary: $ => choice(
      $.qualified_name,
      $.var_name,
      $.type_name,
      $.member_access,
      $._literal,
      $.function,
      $.parenthesized_expression,
    ),
    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // ---- control flow ------------------------------------------------------
    // `if` is primarily an expression, but its statement form drives control
    // flow — a branch may `return`, `<-`, or assign (`if err then return None`,
    // `if done then break`). break/continue are already expressions; the extra
    // statement forms are admitted here so both `if` shapes share one rule. The
    // grammar stays permissive; the compiler enforces the value/statement split.
    if_expression: $ => prec.right(seq(
      'if', $._expression, 'then', $._branch, optional(seq('else', $._branch)),
    )),
    _branch: $ => choice(
      $.block,
      $.return_statement,
      $.yield_statement,
      $.assignment_statement,
      $._expression,
    ),

    match_expression: $ => seq('match', $._expression, '{',
      commaSep1($.match_arm), optional(','), '}'),
    match_arm: $ => seq($.or_pattern, '->', $._branch),
    // `p0 | p1 | …` matches any one alternative; a single `|`, never `||` (a
    // pattern is never an expression, so `|` here is unambiguously alternation).
    or_pattern: $ => seq($._match_pattern, repeat(seq('|', $._match_pattern))),
    _match_pattern: $ => choice(
      $.wildcard_pattern,
      $._literal_pattern,
      $.var_name,
      $.type_pattern,
      $.record_pattern,
      $.array_pattern,
    ),
    // A leading `-` negates a NUMBER pattern (`-5`); string/bool take no sign.
    _literal_pattern: $ => choice($.integer, $.float, $.string, $.boolean, $.negative_pattern),
    negative_pattern: $ => seq('-', choice($.integer, $.float)),
    // The head may be a bare named type or a qualified member (`Node.IntLit(v)`).
    type_pattern: $ => seq(
      choice($.named_type, $.member_access),
      optional(seq('(', commaSepT($._match_pattern), ')')),
    ),

    loop_expression: $ => seq('loop', optional($.var_name), $.block),
    // one binder (`for x in`), or element + zero-based index (`for (x, i) in`).
    for_expression: $ => seq(
      'for',
      choice($.var_name, seq('(', $.var_name, ',', $.var_name, ')')),
      'in', $._expression, $._branch,
    ),
    // The EBNF gives `break`/`continue` an optional loop label. Because this
    // grammar treats newlines as insignificant, an optional trailing label would
    // greedily swallow the next statement's leading name (`break` <nl> `i = …`),
    // so the label is dropped here — a `break outer` still highlights fine, with
    // `outer` read as the following name. Labels are a compiler concern anyway.
    break_expression: _ => 'break',
    continue_expression: _ => 'continue',

    // ---- patterns (destructuring) -----------------------------------------
    // Arrays match by position in `[…]`, tuples/positional records in `(…)`.
    _pattern: $ => choice($.record_pattern, $.array_pattern, $.tuple_pattern),
    // `_` skips one element, `_3` skips three (skip = "_" , { dec_digit }).
    wildcard_pattern: _ => token(prec(1, /_[0-9]*/)),
    _pattern_elem: $ => choice($.var_name, $.wildcard_pattern),
    record_pattern: $ => seq('{', commaSep1($.var_name), optional(','), '}'),
    array_pattern: $ => seq('[', commaSep1($._pattern_elem), optional(','), ']'),
    tuple_pattern: $ => seq('(', commaSep1($._pattern_elem), optional(','), ')'),

    // ---- imports -----------------------------------------------------------
    // `import a::b::c` binds the last segment; `::{ x, Y }` destructures
    // members; `as n` renames the namespace; `as *` splices the whole exported
    // surface flat. `pub import` is a reexport.
    import_declaration: $ => seq(
      optional('pub'), 'import', $.module_path,
      optional(choice(
        seq('::', $.import_list),
        seq('as', choice($.var_name, '*')),
      )),
    ),
    module_path: $ => seq($._path_seg, repeat(seq('::', $._path_seg))),
    import_list: $ => seq('{', commaSepT(choice($.var_name, $.type_name)), '}'),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}

// A comma-separated list with the optional trailing comma every cf list allows.
function commaSepT(rule) {
  return seq(rule, repeat(seq(',', rule)), optional(','));
}
