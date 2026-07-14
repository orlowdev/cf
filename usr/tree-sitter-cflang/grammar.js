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
    // `() T` — a lambda's param_list + return type, or a func type.
    [$.func_type, $.param_list],
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
  ],

  rules: {
    module: $ => repeat(choice($.import_declaration, $._declaration)),

    comment: _ => token(seq('#', /.*/)),

    // ---- identifiers -------------------------------------------------------
    // `_word` is the keyword-extraction token: the snake_case value-name shape.
    _word: _ => /[a-z][a-z0-9_]*!?/,
    var_name: $ => $._word,
    type_name: _ => /[A-Z][A-Za-z0-9]*/,
    type_var: $ => seq("'", $.type_name),

    // ---- types -------------------------------------------------------------
    _type: $ => choice(
      $.pointer_type,
      $.reference_type,
      $.bracket_type,
      $.named_type,
      $.type_var,
      $.func_type,
    ),

    named_type: $ => prec.left(seq(
      $.type_name,
      optional(seq('[', commaSep1($._type_arg), ']')),
    )),

    pointer_type: $ => prec.right(seq('*', $._type)),
    reference_type: $ => prec.right(seq('&', $._type)),

    func_type: $ => prec.right(seq(
      '(', optional(commaSep1($._type)), ')', $._type,
    )),

    bracket_type: $ => seq('[', optional(choice(
      $.fixed_array_type,
      $.tuple_type,
      $._type, // array
    )), ']'),

    fixed_array_type: $ => seq(choice($.integer, $.var_name), $._type),
    tuple_type: $ => seq($._type, ',', commaSep1($._type)),

    // A type argument is a type or a comptime value (`[Int]`, `[8, Int]`).
    // Narrowed to types + simple comptime literals/names rather than the full
    // expression grammar, which keeps type-position `[…]` free of the aggregate
    // and data-literal ambiguities.
    _type_arg: $ => choice($._type, $.integer, $.float, $.var_name),

    // ---- literals ----------------------------------------------------------
    _literal: $ => choice(
      $.float,
      $.integer,
      $.string,
      $.boolean,
      $.aggregate,
      $.data_literal,
    ),

    integer: _ => token(choice(
      /[0-9][0-9_]*/,
      /0b[01][01_]*/,
      /0o[0-7][0-7_]*/,
      /0x[0-9a-fA-F][0-9a-fA-F_]*/,
    )),
    float: _ => token(prec(1, /[0-9][0-9_]*\.[0-9][0-9_]*/)),

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

    aggregate: $ => seq('[', optional(commaSep1(choice($.spread_element, $._expression))), ']'),
    spread_element: $ => seq('...', $._expression),

    data_literal: $ => seq('{', optional(commaSep1($.field_init)), '}'),
    field_init: $ => choice(
      seq($.var_name, ':', $._expression),
      $.var_name, // pun: { value }
    ),

    // ---- declarations ------------------------------------------------------
    _declaration: $ => seq(optional('pub'), choice(
      $.data_declaration,
      $.type_declaration,
      $.union_declaration,
      $.let_declaration,
      $.const_declaration,
      $.destructure_declaration,
      $.intrinsic_declaration,
    )),

    data_declaration: $ => seq('data', $.type_name, optional($.generic_params), '=', $._data_body),
    type_declaration: $ => seq('type', $.type_name, optional($.generic_params), '=', $._data_body),
    _data_body: $ => choice($.record_body, $._type),

    record_body: $ => seq('{', optional(seq(commaSep1($.record_entry), optional(','))), '}'),
    record_entry: $ => choice($.field_declaration, $.spread),
    field_declaration: $ => seq($._type, $.var_name, optional(seq('=', $._expression))),
    spread: $ => seq('...', $.named_type),

    union_declaration: $ => seq('union', $.type_name, optional($.generic_params),
      '=', seq('{', commaSep1($._type), optional(','), '}')),

    let_declaration: $ => seq('let', choice(
      seq($._type, $.var_name, optional(seq('=', $._expression))),
      seq($.var_name, '=', $._expression),
    )),
    const_declaration: $ => seq('const', optional($._type), $.var_name, '=', $._expression),

    destructure_declaration: $ => seq(choice('let', 'const'), $._pattern, '=', $._expression),

    intrinsic_declaration: $ => seq('intrinsic', $.var_name, '=', $.intrinsic_signature),
    intrinsic_signature: $ => seq(optional($.generic_params), $.param_list, $._type),

    // ---- functions ---------------------------------------------------------
    function: $ => prec.right(seq(
      optional($.generic_params),
      $.param_list,
      optional($._type),
      '->',
      choice($.block, $.asm_block, $._expression),
    )),

    generic_params: $ => seq('[', commaSep1($.generic_param), ']'),
    generic_param: $ => choice(
      $.type_var,
      seq($._type, $.type_var),
      seq($._type, $.var_name),
    ),

    param_list: $ => seq('(', optional(commaSep1($.param)), ')'),
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
    defer_expression: $ => prec.right(PREC.unary, seq('defer', $._postfix)),

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
    type_args: $ => seq('[', commaSep1($._type_arg), ']'),
    arguments: $ => seq('(', optional(commaSep1($._expression)), ')'),

    _primary: $ => choice(
      $.var_name,
      $._literal,
      $.function,
      $.parenthesized_expression,
    ),
    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // ---- control flow ------------------------------------------------------
    if_expression: $ => prec.right(seq(
      'if', $._expression, 'then', $._branch, optional(seq('else', $._branch)),
    )),
    _branch: $ => choice($.block, $._expression),

    match_expression: $ => seq('match', $._expression, '{',
      commaSep1($.match_arm), optional(','), '}'),
    match_arm: $ => seq($._match_pattern, '->', $._branch),
    _match_pattern: $ => choice(
      $.wildcard_pattern,
      $._literal_pattern,
      $.var_name,
      $.type_pattern,
      $.record_pattern,
      $.array_pattern,
    ),
    _literal_pattern: $ => choice($.integer, $.float, $.string, $.boolean),
    type_pattern: $ => seq($.named_type, optional(seq('(', commaSep1($._match_pattern), ')'))),

    loop_expression: $ => seq('loop', optional($.var_name), $.block),
    for_expression: $ => seq('for', $.var_name, 'in', $._expression, $._branch),
    // The EBNF gives `break`/`continue` an optional loop label. Because this
    // grammar treats newlines as insignificant, an optional trailing label would
    // greedily swallow the next statement's leading name (`break` <nl> `i = …`),
    // so the label is dropped here — a `break outer` still highlights fine, with
    // `outer` read as the following name. Labels are a compiler concern anyway.
    break_expression: _ => 'break',
    continue_expression: _ => 'continue',

    // ---- patterns (destructuring) -----------------------------------------
    _pattern: $ => choice($.record_pattern, $.array_pattern),
    wildcard_pattern: _ => token(prec(1, /_[0-9]*/)),
    record_pattern: $ => seq('{', commaSep1($.var_name), optional(','), '}'),
    array_pattern: $ => seq('[', commaSep1(choice($.var_name, $.wildcard_pattern)), optional(','), ']'),

    // ---- imports -----------------------------------------------------------
    import_declaration: $ => seq(optional('pub'), 'import', $.string, 'as', $._import_alias),
    _import_alias: $ => choice($.var_name, $.type_name, $.import_list),
    import_list: $ => seq('{', commaSep1(choice($.var_name, $.type_name)), optional(','), '}'),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
