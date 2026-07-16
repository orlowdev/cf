/*
 * cfcc — the C! genesis compiler.
 *
 * A throwaway C tool that compiles seed-subset C! to a native darwin-arm64
 * binary through the same `qbe -> cc` tail the real backend uses. It is the
 * first-mover of the bootstrap (there is no cf/cf0 yet to compile cf0.cf) and
 * the development compiler for cf0.cf until cf0 can compile itself; it is
 * deleted once the S2 seed is committed. See root/specs/seed_subset.md §3.
 *
 * Correctness-only, darwin-arm64 only, freestanding by default.
 *
 * ---- M0: the tracer bullet ----------------------------------------------
 * This first slice compiles only the entry point returning a constant:
 *
 *     pub const main = () -> 0
 *     pub const main = () -> { return 42 }
 *
 * It exercises the whole spine end to end — read source, lex, parse, emit QBE
 * IL, drive `qbe` then `cc`, and mint a freestanding binary whose `_start`
 * calls `main` and exit-syscalls its Int return. The lexer and parser are real
 * (not a regex) so they grow into the full grammar in M1; they just reject
 * everything outside the slice above for now.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifndef CF_QBE
#define CF_QBE "qbe" /* overridden by build.sh with the vendored path */
#endif

/* ------------------------------------------------------------------ util - */

static const char *g_path = "<input>";

/* _Noreturn so the compiler knows `case X: die(...)` arms don't fall through and
 * the `die(...); return ...` stubs after a diagnostic are genuinely unreachable. */
static _Noreturn void die(int line, const char *msg) {
	if (line > 0)
		fprintf(stderr, "%s:%d: error: %s\n", g_path, line, msg);
	else
		fprintf(stderr, "cfcc: error: %s\n", msg);
	exit(1);
}

static void *xmalloc(size_t n) {
	void *p = malloc(n);
	if (!p)
		die(0, "out of memory");
	return p;
}

static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		die(0, "cannot open input file");
	struct stat st;
	if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode))
		die(0, "input is not a regular file");
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	if (n < 0)
		die(0, "cannot determine input size");
	fseek(f, 0, SEEK_SET);
	char *buf = xmalloc((size_t)n + 1);
	if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
		die(0, "cannot read input file");
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* ----------------------------------------------------------------- lexer - */

typedef enum {
	TK_EOF,
	TK_NEWLINE,
	TK_IDENT, /* includes keywords; disambiguated in the parser */
	TK_INT,
	TK_LPAREN,
	TK_RPAREN,
	TK_LBRACE,
	TK_RBRACE,
	TK_LBRACKET,
	TK_RBRACKET,
	TK_COMMA,
	TK_STAR,    /* * — multiply, and pointer types */
	TK_PLUS,
	TK_MINUS,
	TK_SLASH,
	TK_PERCENT,
	TK_AMP,     /* & bitwise-and */
	TK_PIPE,    /* | bitwise-or */
	TK_CARET,   /* ^ bitwise-xor */
	TK_TILDE,   /* ~ bitwise-not */
	TK_BANG,    /* ! logical-not */
	TK_SHL,     /* << */
	TK_SHR,     /* >> */
	TK_EQEQ,    /* == */
	TK_NE,      /* != */
	TK_LT,      /* < */
	TK_GT,      /* > */
	TK_LE,      /* <= */
	TK_GE,      /* >= */
	TK_ANDAND,  /* && (logical; deferred) */
	TK_OROR,    /* || (logical; deferred) */
	TK_EQ,
	TK_ARROW, /* -> */
} TokKind;

typedef struct {
	TokKind kind;
	int line;
	const char *text; /* into the source buffer; not NUL-terminated */
	int len;
	long ival; /* for TK_INT */
} Token;

typedef struct {
	const char *src;
	size_t pos;
	int line;
	Token *toks;
	size_t ntoks, cap;
} Lexer;

static void push_tok(Lexer *lx, TokKind kind, const char *text, int len, long ival) {
	if (lx->ntoks == lx->cap) {
		lx->cap = lx->cap ? lx->cap * 2 : 64;
		lx->toks = realloc(lx->toks, lx->cap * sizeof(Token));
		if (!lx->toks)
			die(0, "out of memory");
	}
	Token *t = &lx->toks[lx->ntoks++];
	t->kind = kind;
	t->line = lx->line;
	t->text = text;
	t->len = len;
	t->ival = ival;
}

static int ident_char(int c) {
	return isalnum(c) || c == '_';
}

/* A trailing '!' joins a var_name unless it opens '!=' (see ebnf Identifiers).
 * The tracer bullet has no '!' names yet, but the rule is cheap to honour so
 * the lexer already reads `alloc!` as one token. */
static void lex(Lexer *lx) {
	const char *s = lx->src;
	for (;;) {
		int c = (unsigned char)s[lx->pos];
		if (c == '\0') {
			push_tok(lx, TK_EOF, s + lx->pos, 0, 0);
			return;
		}
		if (c == '\n') {
			push_tok(lx, TK_NEWLINE, s + lx->pos, 1, 0);
			lx->pos++;
			lx->line++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r') {
			lx->pos++;
			continue;
		}
		if (c == '#') { /* comment to end of line */
			while (s[lx->pos] && s[lx->pos] != '\n')
				lx->pos++;
			continue;
		}
		if (isdigit(c)) {
			size_t start = lx->pos;
			long v = 0;
			while (isdigit((unsigned char)s[lx->pos])) {
				int d = s[lx->pos] - '0';
				if (v > (LONG_MAX - d) / 10)
					die(lx->line, "integer literal out of range");
				v = v * 10 + d;
				lx->pos++;
			}
			push_tok(lx, TK_INT, s + start, (int)(lx->pos - start), v);
			continue;
		}
		if (isalpha(c) || c == '_') {
			size_t start = lx->pos;
			while (ident_char((unsigned char)s[lx->pos]))
				lx->pos++;
			if (s[lx->pos] == '!' && s[lx->pos + 1] != '=')
				lx->pos++;
			push_tok(lx, TK_IDENT, s + start, (int)(lx->pos - start), 0);
			continue;
		}
		if (c == '-' && s[lx->pos + 1] == '>') {
			push_tok(lx, TK_ARROW, s + lx->pos, 2, 0);
			lx->pos += 2;
			continue;
		}
		int n = (unsigned char)s[lx->pos + 1]; /* lookahead for two-char operators */
		switch (c) {
		case '(': push_tok(lx, TK_LPAREN, s + lx->pos, 1, 0); lx->pos++; continue;
		case ')': push_tok(lx, TK_RPAREN, s + lx->pos, 1, 0); lx->pos++; continue;
		case '{': push_tok(lx, TK_LBRACE, s + lx->pos, 1, 0); lx->pos++; continue;
		case '}': push_tok(lx, TK_RBRACE, s + lx->pos, 1, 0); lx->pos++; continue;
		case '[': push_tok(lx, TK_LBRACKET, s + lx->pos, 1, 0); lx->pos++; continue;
		case ']': push_tok(lx, TK_RBRACKET, s + lx->pos, 1, 0); lx->pos++; continue;
		case ',': push_tok(lx, TK_COMMA, s + lx->pos, 1, 0); lx->pos++; continue;
		case '*': push_tok(lx, TK_STAR, s + lx->pos, 1, 0); lx->pos++; continue;
		case '+': push_tok(lx, TK_PLUS, s + lx->pos, 1, 0); lx->pos++; continue;
		case '-': push_tok(lx, TK_MINUS, s + lx->pos, 1, 0); lx->pos++; continue; /* -> handled above */
		case '/': push_tok(lx, TK_SLASH, s + lx->pos, 1, 0); lx->pos++; continue;
		case '%': push_tok(lx, TK_PERCENT, s + lx->pos, 1, 0); lx->pos++; continue;
		case '^': push_tok(lx, TK_CARET, s + lx->pos, 1, 0); lx->pos++; continue;
		case '~': push_tok(lx, TK_TILDE, s + lx->pos, 1, 0); lx->pos++; continue;
		case '&':
			if (n == '&') { push_tok(lx, TK_ANDAND, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_AMP, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '|':
			if (n == '|') { push_tok(lx, TK_OROR, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_PIPE, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '<':
			if (n == '<') { push_tok(lx, TK_SHL, s + lx->pos, 2, 0); lx->pos += 2; }
			else if (n == '=') { push_tok(lx, TK_LE, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_LT, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '>':
			if (n == '>') { push_tok(lx, TK_SHR, s + lx->pos, 2, 0); lx->pos += 2; }
			else if (n == '=') { push_tok(lx, TK_GE, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_GT, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '=':
			if (n == '=') { push_tok(lx, TK_EQEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_EQ, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '!': /* a lone `!`; a trailing `!` on a name is taken in the ident branch */
			if (n == '=') { push_tok(lx, TK_NE, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_BANG, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		}
		die(lx->line, "unexpected character");
	}
}

/* ---------------------------------------------------------------- parser - */

typedef struct {
	Token *toks;
	size_t pos;
} Parser;

/* The entry ABI kinds an M0 `main` parameter can take: a word (Int, e.g. argc)
 * or a long (a pointer, e.g. *[Str] argv/envp). darwin hands these to _start in
 * x0/x1/x2, which forwards them unchanged, so QBE reads them as ordinary args. */
typedef enum {
	PK_WORD, /* Int  -> w (argc) */
	PK_LONG, /* *T   -> l (argv, envp) */
} ParamKind;

typedef struct {
	char name[64];
	ParamKind kind;
} Param;

/* Expression AST. M0 expressions are word-valued: literals, references to a
 * bound Int name (a parameter or a local), unary (negate / bitwise-not /
 * logical-not), and the binary arithmetic, bitwise, shift, and comparison ops,
 * all at the ebnf precedence (comparison > bit-or/xor/and > shift > additive >
 * multiplicative). */
typedef enum {
	EX_INT,   /* integer literal (ival) */
	EX_VAR,   /* reference to a bound Int name (param or local) */
	EX_CALL,  /* function call: name(args) */
	/* unary (lhs) */
	EX_NEG,   /* - negate */
	EX_BNOT,  /* ~ bitwise not */
	EX_LNOT,  /* ! logical not (yields 0/1) */
	/* binary (lhs, rhs) */
	EX_ADD,
	EX_SUB,
	EX_MUL,
	EX_DIV,
	EX_REM,
	EX_BOR,   /* | */
	EX_BXOR,  /* ^ */
	EX_BAND,  /* & */
	EX_SHL,   /* << */
	EX_SHR,   /* >> (arithmetic; signed word) */
	EX_EQ,    /* == (yields 0/1) */
	EX_NE,    /* != */
	EX_LT,    /* < */
	EX_GT,    /* > */
	EX_LE,    /* <= */
	EX_GE,    /* >= */
} ExprKind;

typedef struct Expr Expr;
struct Expr {
	ExprKind kind;
	int line;         /* source line (for EX_CALL diagnostics) */
	long ival;        /* EX_INT */
	char name[64];    /* EX_VAR (bound name) / EX_CALL (callee) */
	Expr *lhs, *rhs;  /* operands (unary uses lhs) */
	Expr **args;      /* EX_CALL argument expressions */
	int nargs;
};

static Expr *new_expr(ExprKind kind) {
	Expr *e = xmalloc(sizeof *e);
	memset(e, 0, sizeof *e);
	e->kind = kind;
	return e;
}

/* A statement in a block body: a local binding (`const`/`let` name = expr) or
 * the terminal `return expr`. Statements form a linked list in source order. */
typedef enum {
	ST_LOCAL,  /* const/let binding: declare + initialize */
	ST_ASSIGN, /* reassign an existing `let` local */
	ST_RETURN,
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
	StmtKind kind;
	char name[64]; /* ST_LOCAL/ST_ASSIGN: the target name */
	Expr *expr;    /* ST_LOCAL/ST_ASSIGN: the value; ST_RETURN: returned value */
	Stmt *next;
};

static Stmt *new_stmt(StmtKind kind) {
	Stmt *s = xmalloc(sizeof *s);
	memset(s, 0, sizeof *s);
	s->kind = kind;
	return s;
}

/* A local binding. `word` is 1 for an Int (usable in the integer expression
 * grammar); M0 locals are always Int. `mutable` is 1 for `let` (reassignable),
 * 0 for `const`. */
typedef struct {
	char name[64];
	int word;
	int mutable;
} Binding;

#define MAX_PARAMS 32

/* A top-level function: `[pub] const name = (params) [Int] -> body`. All M0
 * functions return Int (word). `locals` accrues during parsing for name
 * resolution within this function. */
typedef struct {
	char name[64];
	int is_pub;
	Param params[MAX_PARAMS];
	int nparams;
	Binding *locals;
	int nlocals, cap_locals;
	Stmt *body;
} Func;

/* The whole program: a set of functions, one of which is `pub const main`. */
typedef struct {
	Func **funcs;
	int nfuncs, cap_funcs;
} Program;

static Func *new_func(void) {
	Func *f = xmalloc(sizeof *f);
	memset(f, 0, sizeof *f);
	return f;
}

static Func *prog_find_func(Program *prog, const char *name) {
	for (int i = 0; i < prog->nfuncs; i++)
		if (strcmp(prog->funcs[i]->name, name) == 0)
			return prog->funcs[i];
	return NULL;
}

static void prog_add_func(Program *prog, Func *f) {
	if (prog->nfuncs == prog->cap_funcs) {
		prog->cap_funcs = prog->cap_funcs ? prog->cap_funcs * 2 : 8;
		prog->funcs = realloc(prog->funcs, prog->cap_funcs * sizeof *prog->funcs);
		if (!prog->funcs)
			die(0, "out of memory");
	}
	prog->funcs[prog->nfuncs++] = f;
}

/* How a name resolves in a function's scope. */
typedef enum {
	R_NONE,  /* undefined */
	R_PARAM, /* a parameter (immutable) */
	R_CONST, /* a `const` local (immutable) */
	R_LET,   /* a `let` local (reassignable) */
} Resolution;

/* Resolve a name (params first, then locals) and set *word to whether it is an
 * Int (usable in the integer expression grammar). */
static Resolution resolve_name(Func *fn, const char *name, int *word) {
	for (int i = 0; i < fn->nparams; i++)
		if (strcmp(fn->params[i].name, name) == 0) {
			*word = fn->params[i].kind == PK_WORD;
			return R_PARAM;
		}
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			*word = fn->locals[i].word;
			return fn->locals[i].mutable ? R_LET : R_CONST;
		}
	return R_NONE;
}

/* Record a new local (always Int/word in M0). Caller has checked for a clash. */
static void func_add_local(Func *fn, const char *name, int mutable) {
	if (fn->nlocals == fn->cap_locals) {
		fn->cap_locals = fn->cap_locals ? fn->cap_locals * 2 : 16;
		fn->locals = realloc(fn->locals, fn->cap_locals * sizeof *fn->locals);
		if (!fn->locals)
			die(0, "out of memory");
	}
	Binding *b = &fn->locals[fn->nlocals++];
	snprintf(b->name, sizeof b->name, "%s", name);
	b->word = 1;
	b->mutable = mutable;
}

static Token *peek(Parser *p) {
	return &p->toks[p->pos];
}

static Token *advance(Parser *p) {
	Token *t = &p->toks[p->pos];
	if (t->kind != TK_EOF)
		p->pos++;
	return t;
}

static void skip_newlines(Parser *p) {
	while (peek(p)->kind == TK_NEWLINE)
		p->pos++;
}

static int is_ident(Token *t, const char *kw) {
	return t->kind == TK_IDENT && (int)strlen(kw) == t->len &&
	       strncmp(t->text, kw, t->len) == 0;
}

static Token *expect(Parser *p, TokKind kind, const char *what) {
	Token *t = peek(p);
	if (t->kind != kind)
		die(t->line, what);
	return advance(p);
}

static Token *expect_ident(Parser *p, const char *kw) {
	Token *t = peek(p);
	if (!is_ident(t, kw))
		die(t->line, "unexpected token (M0 accepts a single `pub const main`)");
	return advance(p);
}

/* Copy a token's text into a fixed buffer, or die if it does not fit. */
static void tok_copy(Token *t, char *buf, size_t cap) {
	if ((size_t)t->len >= cap)
		die(t->line, "identifier too long");
	memcpy(buf, t->text, t->len);
	buf[t->len] = '\0';
}

/* True if an identifier names a type (PascalCase — leading uppercase). */
static int is_type_ident(Token *t) {
	return t->kind == TK_IDENT && t->len > 0 && t->text[0] >= 'A' && t->text[0] <= 'Z';
}

/* Consume an entry parameter's type and classify it by ABI width. M0 entry types
 * are only `Int` (a word — argc) and pointer types like `*[Str]` (a long — argv,
 * envp); a pointer's pointee is skipped wholesale since only the top-level shape
 * sets the register width. */
static ParamKind parse_param_type(Parser *p) {
	Token *t = peek(p);
	if (is_ident(t, "Int")) {
		advance(p);
		return PK_WORD;
	}
	if (t->kind == TK_STAR) {
		advance(p);
		Token *u = peek(p);
		if (u->kind == TK_LBRACKET) { /* skip a balanced [ ... ] pointee */
			int depth = 0;
			do {
				Token *v = advance(p);
				if (v->kind == TK_LBRACKET)
					depth++;
				else if (v->kind == TK_RBRACKET)
					depth--;
				else if (v->kind == TK_EOF)
					die(v->line, "unterminated pointer type");
			} while (depth > 0);
		} else if (is_type_ident(u)) {
			advance(p);
		} else {
			die(u->line, "expected a type after `*`");
		}
		return PK_LONG;
	}
	die(t->line, "M0 entry parameters must be `Int` or a pointer type (e.g. `*[Str]`)");
	return PK_WORD; /* unreachable; die() exits */
}

/* param = type var_name  (typed; M0 entry params always carry their type). */
static void parse_param(Parser *p, Param *out) {
	out->kind = parse_param_type(p);
	Token *name = peek(p);
	if (name->kind != TK_IDENT || is_type_ident(name))
		die(name->line, "expected a parameter name");
	/* A trailing `!` is a legal var_name char (ebnf) but is not a QBE-legal temp
	 * char; M0 has no `!`-named entry params, so reject it with a source-level
	 * error instead of letting `%x!` reach qbe. */
	if (name->text[name->len - 1] == '!')
		die(name->line, "M0 does not support `!` in a parameter name");
	tok_copy(name, out->name, sizeof out->name);
	advance(p);
}

static Expr *parse_expr(Parser *p, Func *fn); /* forward */

/* primary = INT | call | var_name | "(" expr ")"
 * call    = var_name "(" [ expr { "," expr } ] ")"
 * A bare name resolves to an Int parameter or local; a name followed by `(` is a
 * function call (its callee is checked against the whole program after parsing). */
static Expr *parse_primary(Parser *p, Func *fn) {
	Token *t = peek(p);
	if (t->kind == TK_INT) {
		advance(p);
		/* Every integer is a word in M0, so a literal must fit one (literals are
		 * unsigned per ebnf Numbers, so only the upper bound bites). */
		if (t->ival > INT32_MAX)
			die(t->line, "integer literal out of range (M0 supports 0..2147483647)");
		Expr *e = new_expr(EX_INT);
		e->ival = t->ival;
		return e;
	}
	if (t->kind == TK_LPAREN) {
		advance(p);
		Expr *e = parse_expr(p, fn);
		expect(p, TK_RPAREN, "expected `)`");
		return e;
	}
	if (t->kind == TK_IDENT && !is_type_ident(t)) {
		if (t->text[t->len - 1] == '!')
			die(t->line, "M0 does not support `!` in a name here");
		int line = t->line;
		advance(p); /* `t` still points at the name token (stable in the array) */
		if (peek(p)->kind == TK_LPAREN) {
			advance(p);
			Expr *e = new_expr(EX_CALL);
			e->line = line;
			tok_copy(t, e->name, sizeof e->name);
			int cap = 0;
			if (peek(p)->kind != TK_RPAREN)
				for (;;) {
					if (e->nargs == cap) {
						cap = cap ? cap * 2 : 4;
						e->args = realloc(e->args, cap * sizeof *e->args);
						if (!e->args)
							die(0, "out of memory");
					}
					e->args[e->nargs++] = parse_expr(p, fn);
					if (peek(p)->kind == TK_COMMA) {
						advance(p);
						continue;
					}
					break;
				}
			expect(p, TK_RPAREN, "expected `)`");
			return e;
		}
		Expr *e = new_expr(EX_VAR);
		tok_copy(t, e->name, sizeof e->name);
		int word;
		if (resolve_name(fn, e->name, &word) == R_NONE)
			die(line, "unknown name (M0 expressions use integers, parameters, locals, and calls)");
		if (!word)
			die(line, "M0 expressions can only use Int values (e.g. argc)");
		return e;
	}
	die(t->line, "expected an integer, a name, or `(`");
	return NULL; /* unreachable; die() exits */
}

/* unary = ("-" | "~" | "!") unary | primary   (right-associative prefix ops) */
static Expr *parse_unary(Parser *p, Func *fn) {
	ExprKind op;
	switch (peek(p)->kind) {
	case TK_MINUS: op = EX_NEG; break;
	case TK_TILDE: op = EX_BNOT; break;
	case TK_BANG: op = EX_LNOT; break;
	default: return parse_primary(p, fn);
	}
	advance(p);
	Expr *e = new_expr(op);
	e->lhs = parse_unary(p, fn);
	return e;
}

/* Left-associative binary level: fold `left op right op ...` for up to three
 * token→ExprKind operator mappings. A level with fewer operators passes TK_EOF
 * in the unused t1/t2 slots (guarded below); t0 is always a real operator. */
static Expr *fold_binary(Parser *p, Func *fn, Expr *(*next)(Parser *, Func *),
                         TokKind t0, ExprKind k0, TokKind t1, ExprKind k1,
                         TokKind t2, ExprKind k2) {
	Expr *e = next(p, fn);
	for (;;) {
		TokKind k = peek(p)->kind;
		ExprKind op;
		if (k == t0)
			op = k0;
		else if (t1 != TK_EOF && k == t1)
			op = k1;
		else if (t2 != TK_EOF && k == t2)
			op = k2;
		else
			break;
		advance(p);
		Expr *bin = new_expr(op);
		bin->lhs = e;
		bin->rhs = next(p, fn);
		e = bin;
	}
	return e;
}

/* multiplicative = unary { ("*" | "/" | "%") unary } */
static Expr *parse_mul(Parser *p, Func *fn) {
	return fold_binary(p, fn, parse_unary, TK_STAR, EX_MUL, TK_SLASH, EX_DIV,
	                   TK_PERCENT, EX_REM);
}

/* additive = multiplicative { ("+" | "-") multiplicative } */
static Expr *parse_add(Parser *p, Func *fn) {
	return fold_binary(p, fn, parse_mul, TK_PLUS, EX_ADD, TK_MINUS, EX_SUB,
	                   TK_EOF, EX_ADD /* unused */);
}

/* The ebnf precedence ladder, tightest to loosest, above additive: shift, then
 * bitwise and/xor/or, then comparison. Each is left-associative except
 * comparison, which is non-associative. (TK_EOF fills an unused operator slot.) */
static Expr *parse_shift(Parser *p, Func *fn) {
	return fold_binary(p, fn, parse_add, TK_SHL, EX_SHL, TK_SHR, EX_SHR,
	                   TK_EOF, EX_ADD /* unused */);
}

static Expr *parse_bit_and(Parser *p, Func *fn) {
	return fold_binary(p, fn, parse_shift, TK_AMP, EX_BAND, TK_EOF, EX_ADD,
	                   TK_EOF, EX_ADD /* unused */);
}

static Expr *parse_bit_xor(Parser *p, Func *fn) {
	return fold_binary(p, fn, parse_bit_and, TK_CARET, EX_BXOR, TK_EOF, EX_ADD,
	                   TK_EOF, EX_ADD /* unused */);
}

static Expr *parse_bit_or(Parser *p, Func *fn) {
	return fold_binary(p, fn, parse_bit_xor, TK_PIPE, EX_BOR, TK_EOF, EX_ADD,
	                   TK_EOF, EX_ADD /* unused */);
}

/* comparison = bit_or [ comparison_op bit_or ]   (non-associative — at most one) */
static Expr *parse_comparison(Parser *p, Func *fn) {
	Expr *e = parse_bit_or(p, fn);
	ExprKind op;
	switch (peek(p)->kind) {
	case TK_EQEQ: op = EX_EQ; break;
	case TK_NE: op = EX_NE; break;
	case TK_LT: op = EX_LT; break;
	case TK_GT: op = EX_GT; break;
	case TK_LE: op = EX_LE; break;
	case TK_GE: op = EX_GE; break;
	default: return e;
	}
	advance(p);
	Expr *bin = new_expr(op);
	bin->lhs = e;
	bin->rhs = parse_bit_or(p, fn);
	switch (peek(p)->kind) {
	case TK_EQEQ: case TK_NE: case TK_LT: case TK_GT: case TK_LE: case TK_GE:
		die(peek(p)->line, "comparison is non-associative (parenthesize, e.g. `(a < b) < c`)");
	default:
		break;
	}
	return bin;
}

static Expr *parse_expr(Parser *p, Func *fn) {
	Expr *e = parse_comparison(p, fn);
	if (peek(p)->kind == TK_ANDAND || peek(p)->kind == TK_OROR)
		die(peek(p)->line, "logical `&&`/`||` are not supported yet (M0)");
	return e;
}

/* statement  = local_decl | assign | return_stmt
 * local_decl = ("const" | "let") [ "Int" ] var_name "=" expr
 * assign     = var_name "=" expr           (target must be a `let` local)
 * return_stmt = "return" expr
 * A local is not in scope during its own initializer, and cannot shadow a
 * parameter or an earlier local. `let` binds a reassignable value; `const` and
 * parameters cannot be reassigned. */
static Stmt *parse_stmt(Parser *p, Func *fn, int *saw_return) {
	Token *t = peek(p);
	if (is_ident(t, "const") || is_ident(t, "let")) {
		int mutable = is_ident(t, "let");
		advance(p);
		Token *tt = peek(p); /* optional `Int` type annotation */
		if (is_type_ident(tt) || tt->kind == TK_STAR) {
			if (is_ident(tt, "Int"))
				advance(p);
			else
				die(tt->line, "M0 locals must be Int");
		}
		Token *name = peek(p);
		if (name->kind != TK_IDENT || is_type_ident(name))
			die(name->line, "expected a variable name");
		if (name->text[name->len - 1] == '!')
			die(name->line, "M0 does not support `!` in a variable name");
		Stmt *s = new_stmt(ST_LOCAL);
		tok_copy(name, s->name, sizeof s->name);
		advance(p);
		int word;
		if (resolve_name(fn, s->name, &word) != R_NONE)
			die(name->line, "name already defined (no shadowing in M0)");
		expect(p, TK_EQ, "expected `=`");
		s->expr = parse_expr(p, fn); /* initializer: name not yet in scope */
		func_add_local(fn, s->name, mutable);
		return s;
	}
	if (is_ident(t, "return")) {
		advance(p);
		Stmt *s = new_stmt(ST_RETURN);
		s->expr = parse_expr(p, fn);
		*saw_return = 1;
		return s;
	}
	/* Otherwise a bare name leads an assignment: `name = expr`. */
	if (t->kind == TK_IDENT && !is_type_ident(t)) {
		if (t->text[t->len - 1] == '!')
			die(t->line, "M0 does not support `!` in a name here");
		Stmt *s = new_stmt(ST_ASSIGN);
		tok_copy(t, s->name, sizeof s->name);
		advance(p);
		expect(p, TK_EQ, "expected `=` (M0 statements are const/let, a reassignment, or return)");
		int word;
		switch (resolve_name(fn, s->name, &word)) {
		case R_NONE: die(t->line, "unknown name (assign to a declared `let` local)");
		case R_PARAM: die(t->line, "cannot reassign a parameter");
		case R_CONST: die(t->line, "cannot reassign a `const` binding (declare it with `let`)");
		case R_LET: break; /* ok */
		}
		s->expr = parse_expr(p, fn);
		return s;
	}
	die(t->line, "expected `const`, `let`, `return`, or an assignment");
	return NULL; /* unreachable; die() exits */
}

/* body = expr | "{" { statement (newline) } "return" expr "}"   (must return). */
static Stmt *parse_body(Parser *p, Func *fn) {
	Token *b = peek(p);
	if (b->kind != TK_LBRACE) {
		/* A single-expression body is exactly `return <expr>`. */
		Stmt *s = new_stmt(ST_RETURN);
		s->expr = parse_expr(p, fn);
		return s;
	}
	advance(p);
	skip_newlines(p);
	Stmt *head = NULL, *tail = NULL;
	int saw_return = 0;
	while (peek(p)->kind != TK_RBRACE) {
		if (peek(p)->kind == TK_EOF)
			die(peek(p)->line, "unterminated block (expected `}`)");
		if (saw_return)
			die(peek(p)->line, "unreachable statement after `return`");
		Stmt *s = parse_stmt(p, fn, &saw_return);
		if (tail)
			tail->next = s;
		else
			head = s;
		tail = s;
		if (peek(p)->kind != TK_RBRACE) {
			if (peek(p)->kind == TK_EOF)
				die(peek(p)->line, "unterminated block (expected `}`)");
			expect(p, TK_NEWLINE, "expected a newline (one statement per line)");
			skip_newlines(p);
		}
	}
	advance(p); /* consume `}` */
	if (!saw_return)
		die(b->line, "a function's block must end with `return`");
	return head;
}

/* declaration = [ "pub" ] "const" var_name "=" "(" [ param { "," param } ] ")"
 *               [ "Int" ] "->" body
 * All M0 functions return Int. Params resolve within the function only. */
static Func *parse_func(Parser *p, Program *prog) {
	Func *fn = new_func();
	if (is_ident(peek(p), "pub")) {
		fn->is_pub = 1;
		advance(p);
	}
	expect_ident(p, "const");
	Token *name = peek(p);
	if (name->kind != TK_IDENT || is_type_ident(name))
		die(name->line, "expected a function name");
	if (name->text[name->len - 1] == '!')
		die(name->line, "M0 does not support `!` in a function name");
	tok_copy(name, fn->name, sizeof fn->name);
	/* Functions emit as their bare name ($name → darwin _name). Only `start`
	 * clashes today — it is the runtime's `.globl _start`. (A user function
	 * sharing a libSystem name is harmless while the binary is freestanding and
	 * references no libc symbol; reserving those would become necessary if a
	 * future `--libc` mode links libc calls. The full compiler's name-mangling
	 * arc removes the whole hazard.) */
	if (strcmp(fn->name, "start") == 0)
		die(name->line, "`start` is reserved for the runtime entry symbol");
	if (prog_find_func(prog, fn->name))
		die(name->line, "function already defined");
	advance(p);
	expect(p, TK_EQ, "expected `=`");

	expect(p, TK_LPAREN, "expected `(`");
	if (peek(p)->kind != TK_RPAREN)
		for (;;) {
			if (fn->nparams == MAX_PARAMS)
				die(peek(p)->line, "too many parameters");
			parse_param(p, &fn->params[fn->nparams]);
			for (int j = 0; j < fn->nparams; j++)
				if (strcmp(fn->params[j].name, fn->params[fn->nparams].name) == 0)
					die(p->toks[p->pos - 1].line, "duplicate parameter name");
			fn->nparams++;
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
	expect(p, TK_RPAREN, "expected `)`");

	Token *rt = peek(p); /* optional Int return type */
	if (is_type_ident(rt) || rt->kind == TK_STAR) {
		if (is_ident(rt, "Int"))
			advance(p);
		else
			die(rt->line, "M0 functions return Int");
	}
	expect(p, TK_ARROW, "expected `->`");
	fn->body = parse_body(p, fn);
	return fn;
}

/* module = { declaration } — one per line; exactly one is `pub const main`. */
static void parse(Parser *p, Program *prog) {
	skip_newlines(p);
	while (peek(p)->kind != TK_EOF) {
		prog_add_func(prog, parse_func(p, prog));
		if (peek(p)->kind != TK_EOF) {
			expect(p, TK_NEWLINE, "expected a newline between declarations");
			skip_newlines(p);
		}
	}
	Func *m = prog_find_func(prog, "main");
	if (!m)
		die(0, "no entry point: define `pub const main`");
	if (!m->is_pub)
		die(0, "`main` must be `pub`");
	if (m->nparams > 3)
		die(0, "main takes at most 3 parameters (argc, argv, envp)");
}

/* After parsing, check every call: the callee exists, the arity matches, and its
 * parameters are Int (M0 passes only Int arguments). Runs over the whole program
 * so forward references and recursion resolve. */
static void check_expr(Program *prog, Expr *e) {
	if (!e)
		return;
	if (e->kind == EX_CALL) {
		Func *callee = prog_find_func(prog, e->name);
		if (!callee)
			die(e->line, "call to an unknown function");
		/* Over-application is a genuine error — an M0 function returns Int, which
		 * is not callable. Under-application is, per ebnf (Partial Application),
		 * NOT an error: it yields a function of the remaining parameters. cfcc has
		 * no first-class functions yet, so it is a TEMPORARY STUB error here.
		 * REVISIT when partial application lands — allow it, and revise test 064. */
		if (e->nargs > callee->nparams)
			die(e->line, "too many arguments");
		if (e->nargs < callee->nparams)
			die(e->line, "too few arguments (partial application is not supported yet)");
		for (int i = 0; i < callee->nparams; i++)
			if (callee->params[i].kind != PK_WORD)
				die(e->line, "M0 calls pass only Int arguments");
	}
	for (int i = 0; i < e->nargs; i++)
		check_expr(prog, e->args[i]);
	check_expr(prog, e->lhs);
	check_expr(prog, e->rhs);
}

static void validate_calls(Program *prog) {
	for (int i = 0; i < prog->nfuncs; i++)
		for (Stmt *s = prog->funcs[i]->body; s; s = s->next)
			check_expr(prog, s->expr);
}

/* ------------------------------------------------------------- emit QBE - */

/* The QBE word instruction for a binary op. Comparisons are signed and yield a
 * 0/1 word; `>>` is arithmetic (sar). div/rem are signed. */
static const char *binop(ExprKind k) {
	switch (k) {
	case EX_ADD: return "add";
	case EX_SUB: return "sub";
	case EX_MUL: return "mul";
	case EX_DIV: return "div";
	case EX_REM: return "rem";
	case EX_BOR: return "or";
	case EX_BXOR: return "xor";
	case EX_BAND: return "and";
	case EX_SHL: return "shl";
	case EX_SHR: return "sar";
	case EX_EQ: return "ceqw";
	case EX_NE: return "cnew";
	case EX_LT: return "csltw";
	case EX_GT: return "csgtw";
	case EX_LE: return "cslew";
	case EX_GE: return "csgew";
	default: die(0, "internal: not a binary op"); return NULL;
	}
}

/* Emit the code that computes `e` into a fresh word temp, writing the operand
 * that names its value (a literal or a `%tN` temp) into `dst`. Constants are
 * inlined — QBE accepts them as instruction operands.
 *
 * Names live in stack slots (`%s_<name>`), so a variable reference is a `loadw`.
 * The distinct `%s_` / `%u_` / `%tN` prefixes (slot / incoming param / temp)
 * mean a user name can never collide with a compiler temporary. */
static void emit_expr(FILE *out, Expr *e, int *tmp, char *dst, size_t cap) {
	switch (e->kind) {
	case EX_INT:
		snprintf(dst, cap, "%ld", e->ival);
		return;
	case EX_VAR: {
		int t = (*tmp)++;
		fprintf(out, "\t%%t%d =w loadw %%s_%s\n", t, e->name);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_CALL: {
		/* Evaluate each argument into its own temp, then call. All M0 values are
		 * word; the callee symbol is the bare function name ($<name>). */
		int argt[MAX_PARAMS];
		for (int i = 0; i < e->nargs; i++) {
			char op[96];
			emit_expr(out, e->args[i], tmp, op, sizeof op);
			argt[i] = (*tmp)++;
			fprintf(out, "\t%%t%d =w copy %s\n", argt[i], op);
		}
		int r = (*tmp)++;
		fprintf(out, "\t%%t%d =w call $%s(", r, e->name);
		for (int i = 0; i < e->nargs; i++)
			fprintf(out, "%sw %%t%d", i ? ", " : "", argt[i]);
		fprintf(out, ")\n");
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_NEG:
	case EX_BNOT:
	case EX_LNOT: {
		char a[96];
		emit_expr(out, e->lhs, tmp, a, sizeof a);
		int t = (*tmp)++;
		/* neg; ~x is `xor x, -1`; !x is `x == 0` (0/1). */
		if (e->kind == EX_NEG)
			fprintf(out, "\t%%t%d =w neg %s\n", t, a);
		else if (e->kind == EX_BNOT)
			fprintf(out, "\t%%t%d =w xor %s, -1\n", t, a);
		else
			fprintf(out, "\t%%t%d =w ceqw %s, 0\n", t, a);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_ADD:
	case EX_SUB:
	case EX_MUL:
	case EX_DIV:
	case EX_REM:
	case EX_BOR:
	case EX_BXOR:
	case EX_BAND:
	case EX_SHL:
	case EX_SHR:
	case EX_EQ:
	case EX_NE:
	case EX_LT:
	case EX_GT:
	case EX_LE:
	case EX_GE: {
		char a[96], b[96];
		emit_expr(out, e->lhs, tmp, a, sizeof a);
		emit_expr(out, e->rhs, tmp, b, sizeof b);
		int t = (*tmp)++;
		fprintf(out, "\t%%t%d =w %s %s, %s\n", t, binop(e->kind), a, b);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	}
	/* All ExprKinds are handled above (no default, so -Wswitch catches a new one
	 * before it can be miscompiled); this guards a corrupted node. */
	die(0, "internal: unhandled expression kind");
}

static void emit_func(FILE *out, const Func *fn) {
	/* `main` is the exported entry (returns the Int exit code; darwin hands it
	 * argc/argv/envp in x0/x1/x2 via _start). Other functions are internal. No
	 * page node is threaded yet: a non-allocating body carries none (M0). */
	int is_main = strcmp(fn->name, "main") == 0;
	fprintf(out, "%sfunction w $%s(", is_main ? "export " : "", fn->name);
	for (int i = 0; i < fn->nparams; i++)
		fprintf(out, "%s%s %%u_%s", i ? ", " : "",
		        fn->params[i].kind == PK_WORD ? "w" : "l", fn->params[i].name);
	fprintf(out, ") {\n");
	fprintf(out, "@start\n");

	/* Every Int name lives in a stack slot so `let` reassignment is just a store.
	 * Spill each word parameter from its incoming temp into its slot; pointer
	 * params (e.g. main's argv/envp) are never referenced, so they get no slot. */
	for (int i = 0; i < fn->nparams; i++)
		if (fn->params[i].kind == PK_WORD) {
			const char *n = fn->params[i].name;
			fprintf(out, "\t%%s_%s =l alloc4 4\n", n);
			fprintf(out, "\tstorew %%u_%s, %%s_%s\n", n, n);
		}

	int tmp = 0;
	for (Stmt *s = fn->body; s; s = s->next) {
		char v[96];
		switch (s->kind) {
		case ST_LOCAL:
			fprintf(out, "\t%%s_%s =l alloc4 4\n", s->name);
			emit_expr(out, s->expr, &tmp, v, sizeof v);
			fprintf(out, "\tstorew %s, %%s_%s\n", v, s->name);
			break;
		case ST_ASSIGN:
			emit_expr(out, s->expr, &tmp, v, sizeof v);
			fprintf(out, "\tstorew %s, %%s_%s\n", v, s->name);
			break;
		case ST_RETURN:
			emit_expr(out, s->expr, &tmp, v, sizeof v);
			fprintf(out, "\tret %s\n", v);
			break;
		}
	}
	fprintf(out, "}\n");
}

/* The freestanding runtime, emitted verbatim beside the QBE-lowered code (asm
 * bypasses QBE; see ebnf Assembly). `_start` is the -nostdlib entry: it calls
 * `main` and exit-syscalls the Int return. Darwin arm64 BSD syscall: number in
 * x16, `svc #0x80`; SYS_exit = 1. QBE's arm64_apple target underscore-prefixes
 * `$main`, so the symbol is `_main`.
 *
 * argc/argv/envp wiring: under LC_MAIN darwin invokes `_start` exactly like
 * `main(argc, argv, envp, apple)` — argc in x0, argv in x1, envp in x2. `_start`
 * touches none of x0..x2 before the call, so `main` receives them by arity for
 * free. The root page-node mint arrives once a `main` actually allocates. */
static void emit_runtime(FILE *out) {
	fprintf(out, ".text\n");
	fprintf(out, ".globl _start\n");
	fprintf(out, ".p2align 2\n");
	fprintf(out, "_start:\n");
	fprintf(out, "\tbl _main\n");     /* x0..x2 pass through: argc/argv/envp -> main */
	fprintf(out, "\tmov x16, #1\n");  /* SYS_exit */
	fprintf(out, "\tsvc #0x80\n");
}

/* --------------------------------------------------------------- driver - */

static void run(char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0)
		die(0, "fork failed");
	if (pid == 0) {
		execvp(argv[0], argv);
		perror(argv[0]);
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		die(0, "waitpid failed");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "cfcc: subprocess `%s` failed\n", argv[0]);
		exit(1);
	}
}

/* Intermediates live in a temp dir removed on exit — success or failure — so a
 * failed qbe/cc run (which exits through run()) leaves no litter. */
static char g_tmpdir[4096];
static char g_qbe_il[4096];
static char g_main_s[4096];
static char g_rt_s[4096];

static void cleanup_tmp(void) {
	if (g_qbe_il[0])
		unlink(g_qbe_il);
	if (g_main_s[0])
		unlink(g_main_s);
	if (g_rt_s[0])
		unlink(g_rt_s);
	if (g_tmpdir[0])
		rmdir(g_tmpdir);
}

/* Return the stem of a path — its basename without a trailing .cf. Used for the
 * default ./out/<stem> artifact name. */
static char *stem_of(const char *path) {
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	size_t n = strlen(base);
	if (n > 3 && strcmp(base + n - 3, ".cf") == 0)
		n -= 3;
	char *s = xmalloc(n + 1);
	memcpy(s, base, n);
	s[n] = '\0';
	return s;
}

static void usage(void) {
	fprintf(stderr, "usage: cfcc c [-o <path>] <file.cf>\n");
	exit(2);
}

int main(int argc, char **argv) {
	const char *cmd = NULL;
	const char *input = NULL;
	const char *output = NULL;

	/* cfcc mirrors the slice of the `cf` CLI that cf0 borrows: the `compile`
	 * command (alias `c`) and `-o` only (cf_cli §9). A subcommand is required —
	 * cfcc is a subset of `cf`, so invocation reads the same: `cfcc c file.cf`. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
			if (++i >= argc)
				usage();
			output = argv[i];
		} else if (argv[i][0] == '-' && argv[i][1] != '\0') {
			fprintf(stderr, "cfcc: unknown flag `%s`\n", argv[i]);
			usage();
		} else if (!cmd) {
			cmd = argv[i];
		} else if (!input) {
			input = argv[i];
		} else {
			usage();
		}
	}
	if (!cmd || (strcmp(cmd, "c") != 0 && strcmp(cmd, "compile") != 0)) {
		if (cmd)
			fprintf(stderr, "cfcc: unknown command `%s` (expected `c` or `compile`)\n", cmd);
		usage();
	}
	if (!input)
		usage();
	g_path = input;

	/* Front end. */
	char *src = read_file(input);
	Lexer lx = {0};
	lx.src = src;
	lx.line = 1;
	lex(&lx);

	Parser ps = {0};
	ps.toks = lx.toks;
	Program prog = {0};
	parse(&ps, &prog);
	validate_calls(&prog);

	/* Default artifact: ./out/<stem>. */
	char *stem = stem_of(input);
	if (stem[0] == '\0')
		die(0, "cannot derive an output name from the input path");
	char outpath[4096];
	int on;
	if (output)
		on = snprintf(outpath, sizeof outpath, "%s", output);
	else {
		if (mkdir("out", 0755) != 0 && errno != EEXIST)
			die(0, "cannot create the ./out directory");
		on = snprintf(outpath, sizeof outpath, "out/%s", stem);
	}
	if (on < 0 || (size_t)on >= sizeof outpath)
		die(0, "output path too long");

	/* Intermediates in a temp dir under $TMPDIR (darwin's per-user scratch),
	 * falling back to /tmp. Removed on exit via cleanup_tmp. */
	const char *tmproot = getenv("TMPDIR");
	if (!tmproot || !tmproot[0])
		tmproot = "/tmp";
	size_t tlen = strlen(tmproot);
	int tn = snprintf(g_tmpdir, sizeof g_tmpdir, "%s%scfcc.XXXXXX",
	                  tmproot, (tlen && tmproot[tlen - 1] == '/') ? "" : "/");
	if (tn < 0 || (size_t)tn >= sizeof g_tmpdir)
		die(0, "temp path too long");
	if (!mkdtemp(g_tmpdir))
		die(0, "cannot create temp dir");
	atexit(cleanup_tmp);
	if (snprintf(g_qbe_il, sizeof g_qbe_il, "%s/main.qbe", g_tmpdir) >= (int)sizeof g_qbe_il ||
	    snprintf(g_main_s, sizeof g_main_s, "%s/main.s", g_tmpdir) >= (int)sizeof g_main_s ||
	    snprintf(g_rt_s, sizeof g_rt_s, "%s/rt.s", g_tmpdir) >= (int)sizeof g_rt_s)
		die(0, "temp path too long");

	FILE *f = fopen(g_qbe_il, "wb");
	if (!f)
		die(0, "cannot write QBE IL");
	for (int i = 0; i < prog.nfuncs; i++)
		emit_func(f, prog.funcs[i]);
	fclose(f);

	f = fopen(g_rt_s, "wb");
	if (!f)
		die(0, "cannot write runtime asm");
	emit_runtime(f);
	fclose(f);

	/* qbe: IL -> arm64_apple assembly. */
	{
		char *av[] = {(char *)CF_QBE, "-t", "arm64_apple", "-o", g_main_s, g_qbe_il, NULL};
		run(av);
	}
	/* cc: assemble + link freestanding, entry at _start.
	 *
	 * `-nostdlib` drops the C startup and default libs — the binary uses no
	 * libc: `_start` reaches the kernel through raw `svc`. But darwin's linker
	 * refuses a dynamic Mach-O with no libSystem load command ("must link with
	 * libSystem.dylib"), so `-lSystem` is added back solely to satisfy the
	 * loader. No libSystem symbol is referenced; this is the darwin cost of
	 * `--libc none` that seed_subset §3 anticipates. */
	{
		char *av[] = {"cc", "-nostdlib", "-lSystem", "-Wl,-e,_start", "-o", outpath,
		              g_rt_s, g_main_s, NULL};
		run(av);
	}
	/* No explicit codesign: darwin's `ld` ad-hoc-signs arm64 output by default
	 * (flags: adhoc,linker-signed) and the binary execs as-is, so the trusted
	 * base stays exactly {qbe, cc} (seed_subset §3). A dedicated signing step
	 * would return only if a future link mode emitted unsigned output. */

	fprintf(stderr, "cfcc: wrote %s\n", outpath);
	return 0;
}
