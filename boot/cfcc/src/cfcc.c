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
	TK_DOT,     /* . — field access */
	TK_COLON,   /* : — data-literal field separator */
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
	TK_ANDAND,  /* && logical, short-circuit */
	TK_OROR,    /* || logical, short-circuit */
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
		case '.': push_tok(lx, TK_DOT, s + lx->pos, 1, 0); lx->pos++; continue;
		case ':': push_tok(lx, TK_COLON, s + lx->pos, 1, 0); lx->pos++; continue;
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

/* The type of a value. M0 has three: `Int` (a word), an opaque pointer (`*T` —
 * argv/envp, never dereferenced), and a named record (`data`) type. A record
 * carries a pointer to its declaration, which fixes its fields and layout; the
 * pointer is filled in by the typecheck pass (parse leaves it NULL). */
typedef struct DataDecl DataDecl;

typedef enum {
	TY_INT,    /* Int — a word */
	TY_PTR,    /* *T  — an opaque pointer (a long; not usable in Int expressions) */
	TY_RECORD, /* a `data` record type (see rec) */
} TypeKind;

typedef struct {
	TypeKind kind;
	DataDecl *rec; /* TY_RECORD: the record's declaration */
} Type;

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
	EX_VAR,   /* reference to a bound name (param or local; any type) */
	EX_FIELD, /* record field access: base.name (lhs=base record expr) */
	EX_RECORD,/* record construction: a data literal { f: v, ... } of type `name` */
	EX_CALL,  /* function call: name(args) */
	EX_IF,    /* if cond then a else b (lhs=cond, rhs=then, els=else) */
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
	/* short-circuit logical (lhs, rhs); yield 0/1 — NOT plain binary ops */
	EX_AND,   /* && */
	EX_OR,    /* || */
} ExprKind;

typedef struct Expr Expr;
struct Expr {
	ExprKind kind;
	int line;         /* source line (for EX_CALL diagnostics) */
	long ival;        /* EX_INT */
	char name[64];    /* EX_VAR (bound name) / EX_CALL (callee) */
	Expr *lhs, *rhs;  /* operands (unary uses lhs; EX_IF: lhs=cond, rhs=then; EX_FIELD: lhs=base) */
	Expr *els;        /* EX_IF: else branch */
	Expr **args;      /* EX_CALL argument expressions */
	int nargs;
	/* EX_FIELD: the record type and the field's byte offset, both resolved by the
	 * typecheck pass (parse leaves rec NULL). */
	DataDecl *rec;
	int foff;
	/* EX_RECORD: the field initializers as written (`fnames[i]: fvals[i]`), plus,
	 * after typecheck, `ford` — the value expressions reordered into declaration
	 * order (one per record field). `name` holds the record type name. */
	char (*fnames)[64];
	Expr **fvals;
	int nfields;
	Expr **ford;
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

/* A local binding. `type` is the bound value's type (Int, or a record for a
 * `data`-typed `const`). `mutable` is 1 for `let` (reassignable), 0 for `const`;
 * record bindings are always `const` in M0. For a record binding parse leaves
 * `type.rec` NULL — the typecheck pass fills it once the type name is resolved. */
typedef struct {
	char name[64];
	Type type;
	int mutable;
} Binding;

#define MAX_PARAMS 32
#define MAX_FIELDS 64

/* A `data` record declaration: `data Name = { Int f0, Int f1, ... }`. M0 fields
 * are all Int (4 bytes), laid out in declaration order, so field i sits at byte
 * offset i*4 and the record occupies nfields*4 bytes. */
struct DataDecl {
	char name[64];
	char fields[MAX_FIELDS][64];
	int nfields;
};

/* The byte offset / size for the all-Int M0 record layout. */
static int data_size(const DataDecl *d) { return d->nfields * 4; }

/* Index of a field by name, or -1 if the record has no such field. */
static int data_field_index(const DataDecl *d, const char *name) {
	for (int i = 0; i < d->nfields; i++)
		if (strcmp(d->fields[i], name) == 0)
			return i;
	return -1;
}

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

/* The whole program: a set of `data` declarations and a set of functions, one of
 * which is `pub const main`. */
typedef struct {
	DataDecl **datas;
	int ndatas, cap_datas;
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

static DataDecl *prog_find_data(Program *prog, const char *name) {
	for (int i = 0; i < prog->ndatas; i++)
		if (strcmp(prog->datas[i]->name, name) == 0)
			return prog->datas[i];
	return NULL;
}

static void prog_add_data(Program *prog, DataDecl *d) {
	if (prog->ndatas == prog->cap_datas) {
		prog->cap_datas = prog->cap_datas ? prog->cap_datas * 2 : 8;
		prog->datas = realloc(prog->datas, prog->cap_datas * sizeof *prog->datas);
		if (!prog->datas)
			die(0, "out of memory");
	}
	prog->datas[prog->ndatas++] = d;
}

/* How a name resolves in a function's scope. */
typedef enum {
	R_NONE,  /* undefined */
	R_PARAM, /* a parameter (immutable) */
	R_CONST, /* a `const` local (immutable) */
	R_LET,   /* a `let` local (reassignable) */
} Resolution;

/* Resolve a name (params first, then locals) and set *ty to its type. A word
 * param is Int; a pointer param is TY_PTR (argv/envp — not usable as an Int). */
static Resolution resolve_name(Func *fn, const char *name, Type *ty) {
	for (int i = 0; i < fn->nparams; i++)
		if (strcmp(fn->params[i].name, name) == 0) {
			ty->kind = fn->params[i].kind == PK_WORD ? TY_INT : TY_PTR;
			ty->rec = NULL;
			return R_PARAM;
		}
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			*ty = fn->locals[i].type;
			return fn->locals[i].mutable ? R_LET : R_CONST;
		}
	return R_NONE;
}

/* Record a new local with the given type. Caller has checked for a clash. For a
 * record local parse passes {TY_RECORD, NULL}; the typecheck pass backfills rec
 * (see resolve_record_binding). */
static void func_add_local(Func *fn, const char *name, int mutable, Type ty) {
	if (fn->nlocals == fn->cap_locals) {
		fn->cap_locals = fn->cap_locals ? fn->cap_locals * 2 : 16;
		fn->locals = realloc(fn->locals, fn->cap_locals * sizeof *fn->locals);
		if (!fn->locals)
			die(0, "out of memory");
	}
	Binding *b = &fn->locals[fn->nlocals++];
	snprintf(b->name, sizeof b->name, "%s", name);
	b->type = ty;
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
		if (is_ident(t, "if"))
			die(t->line, "an `if` expression must stand alone or be parenthesized "
			             "(e.g. `1 + (if c then a else b)`)");
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
		e->line = line;
		tok_copy(t, e->name, sizeof e->name);
		Type ty;
		if (resolve_name(fn, e->name, &ty) == R_NONE)
			die(line, "unknown name (M0 expressions use integers, parameters, locals, and calls)");
		/* A record value is legal here only as the base of a field access; a
		 * pointer never. Both are caught in the typecheck pass, which knows the
		 * surrounding context, so parse just records the reference. */
		return e;
	}
	die(t->line, "expected an integer, a name, or `(`");
	return NULL; /* unreachable; die() exits */
}

/* postfix = primary { "." field_name }
 * A `.field` reads a record field. The base is any primary (an M0 record base is
 * a record-typed name); the field's type and offset are resolved in typecheck. */
static Expr *parse_postfix(Parser *p, Func *fn) {
	Expr *e = parse_primary(p, fn);
	while (peek(p)->kind == TK_DOT) {
		advance(p);
		Token *f = peek(p);
		if (f->kind != TK_IDENT || is_type_ident(f))
			die(f->line, "expected a field name after `.`");
		if (f->text[f->len - 1] == '!')
			die(f->line, "M0 does not support `!` in a field name");
		Expr *fe = new_expr(EX_FIELD);
		fe->line = f->line;
		fe->lhs = e;
		tok_copy(f, fe->name, sizeof fe->name);
		advance(p);
		e = fe;
	}
	return e;
}

/* unary = ("-" | "~" | "!") unary | postfix   (right-associative prefix ops) */
static Expr *parse_unary(Parser *p, Func *fn) {
	ExprKind op;
	switch (peek(p)->kind) {
	case TK_MINUS: op = EX_NEG; break;
	case TK_TILDE: op = EX_BNOT; break;
	case TK_BANG: op = EX_LNOT; break;
	default: return parse_postfix(p, fn);
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

/* logical_and = comparison { "&&" comparison }   (short-circuit; yields 0/1) */
static Expr *parse_and(Parser *p, Func *fn) {
	Expr *e = parse_comparison(p, fn);
	while (peek(p)->kind == TK_ANDAND) {
		advance(p);
		Expr *n = new_expr(EX_AND);
		n->lhs = e;
		n->rhs = parse_comparison(p, fn);
		e = n;
	}
	return e;
}

/* logical_or = logical_and { "||" logical_and }   (short-circuit; yields 0/1) */
static Expr *parse_or(Parser *p, Func *fn) {
	Expr *e = parse_and(p, fn);
	while (peek(p)->kind == TK_OROR) {
		advance(p);
		Expr *n = new_expr(EX_OR);
		n->lhs = e;
		n->rhs = parse_and(p, fn);
		e = n;
	}
	return e;
}

/* if_expr = "if" expr "then" expr "else" expr
 * An expression yielding a word: the condition is truthy when nonzero; both
 * branches are required (an else-less `if` yields an Option, which M0 lacks).
 * Branches are expressions (a block branch with `<-` is deferred); `else if`
 * chains for free because the else branch is itself an expression. */
static Expr *parse_if(Parser *p, Func *fn) {
	Token *kw = peek(p);
	advance(p); /* `if` */
	Expr *e = new_expr(EX_IF);
	e->line = kw->line;
	e->lhs = parse_expr(p, fn); /* condition */
	if (!is_ident(peek(p), "then"))
		die(peek(p)->line, "expected `then`");
	advance(p);
	e->rhs = parse_expr(p, fn); /* then branch */
	if (!is_ident(peek(p), "else"))
		die(peek(p)->line,
		    "M0 requires an `else` branch (else-less `if` yields an Option, not supported yet)");
	advance(p);
	e->els = parse_expr(p, fn); /* else branch */
	return e;
}

static Expr *parse_expr(Parser *p, Func *fn) {
	if (is_ident(peek(p), "if"))
		return parse_if(p, fn);
	return parse_or(p, fn);
}

/* data_literal = "{" [ field_init { "," field_init } ] "}"
 * field_init   = field_name ":" expr       (M0: explicit only — no puns, no trailing comma)
 * Builds an EX_RECORD carrying the annotated type name in `name`; the typecheck
 * pass binds it to a `data` declaration and checks the fields cover it exactly.
 * The literal stays on one line in M0 (no interior newlines), like a data decl. */
static Expr *parse_data_literal(Parser *p, Func *fn, const char *typename, int line) {
	expect(p, TK_LBRACE, "expected `{` (a record is built with a data literal)");
	Expr *e = new_expr(EX_RECORD);
	e->line = line;
	snprintf(e->name, sizeof e->name, "%s", typename);
	int cap = 0;
	if (peek(p)->kind != TK_RBRACE)
		for (;;) {
			Token *f = peek(p);
			if (f->kind != TK_IDENT || is_type_ident(f))
				die(f->line, "expected a field name in the data literal");
			if (f->text[f->len - 1] == '!')
				die(f->line, "M0 does not support `!` in a field name");
			if (e->nfields == cap) {
				cap = cap ? cap * 2 : 4;
				e->fnames = realloc(e->fnames, cap * sizeof *e->fnames);
				e->fvals = realloc(e->fvals, cap * sizeof *e->fvals);
				if (!e->fnames || !e->fvals)
					die(0, "out of memory");
			}
			tok_copy(f, e->fnames[e->nfields], sizeof e->fnames[e->nfields]);
			advance(p);
			expect(p, TK_COLON, "expected `:` (M0 data literals are `field: value`)");
			e->fvals[e->nfields] = parse_expr(p, fn);
			e->nfields++;
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
	expect(p, TK_RBRACE, "expected `}`");
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
		/* Optional type annotation: `Int` (a word local) or a record type name (a
		 * `data`-typed local). A pointer annotation has no M0 local use. */
		int is_record = 0;
		char rectype[64] = {0};
		Token *tt = peek(p);
		if (tt->kind == TK_STAR)
			die(tt->line, "M0 locals are `Int` or a record type, not a pointer");
		if (is_type_ident(tt)) {
			if (is_ident(tt, "Int")) {
				advance(p);
			} else {
				tok_copy(tt, rectype, sizeof rectype);
				is_record = 1;
				advance(p);
			}
		}
		Token *name = peek(p);
		if (name->kind != TK_IDENT || is_type_ident(name))
			die(name->line, "expected a variable name");
		if (name->text[name->len - 1] == '!')
			die(name->line, "M0 does not support `!` in a variable name");
		Stmt *s = new_stmt(ST_LOCAL);
		tok_copy(name, s->name, sizeof s->name);
		advance(p);
		Type ty;
		if (resolve_name(fn, s->name, &ty) != R_NONE)
			die(name->line, "name already defined (no shadowing in M0)");
		expect(p, TK_EQ, "expected `=`");
		if (is_record) {
			/* A record local is built from a data literal and is immutable in M0
			 * (field mutation is a later increment). Its rec is bound in typecheck. */
			if (mutable)
				die(name->line, "M0 records are immutable (declare with `const`, not `let`)");
			s->expr = parse_data_literal(p, fn, rectype, name->line);
			Type rt = {TY_RECORD, NULL};
			func_add_local(fn, s->name, 0, rt);
		} else {
			/* A `{` initializer with no type annotation is an attempted record
			 * literal (M0 requires the annotation to know the record's type). */
			if (peek(p)->kind == TK_LBRACE)
				die(peek(p)->line,
				    "a record binding needs a type annotation, e.g. `const Point p = { x: 1 }`");
			s->expr = parse_expr(p, fn); /* initializer: name not yet in scope */
			Type it = {TY_INT, NULL};
			func_add_local(fn, s->name, mutable, it);
		}
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
		Type ty;
		switch (resolve_name(fn, s->name, &ty)) {
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

/* data_decl   = "data" type_name "=" record_body
 * record_body = "{" [ field_decl { "," field_decl } [ "," ] ] "}"
 * field_decl  = "Int" var_name
 * M0 records are a flat set of Int fields on one line (no generics, spread,
 * defaults, non-Int/nested fields, or interior newlines — all later increments),
 * with at least one field so the record is never zero-sized. */
static DataDecl *parse_data_decl(Parser *p, Program *prog) {
	advance(p); /* `data` */
	Token *nm = peek(p);
	if (!is_type_ident(nm))
		die(nm->line, "expected a PascalCase type name after `data`");
	if (nm->text[nm->len - 1] == '!')
		die(nm->line, "M0 does not support `!` in a type name");
	DataDecl *d = xmalloc(sizeof *d);
	memset(d, 0, sizeof *d);
	tok_copy(nm, d->name, sizeof d->name);
	advance(p);
	if (peek(p)->kind == TK_LBRACKET)
		die(peek(p)->line, "M0 data types are not generic");
	if (prog_find_data(prog, d->name))
		die(nm->line, "data type already defined");
	expect(p, TK_EQ, "expected `=`");
	if (peek(p)->kind != TK_LBRACE)
		die(peek(p)->line, "M0 `data` must have a record body `{ Int field, ... }`");
	advance(p); /* `{` */
	if (peek(p)->kind != TK_RBRACE)
		for (;;) {
			Token *ty = peek(p);
			if (ty->kind == TK_DOT)
				die(ty->line, "M0 records do not support `...` spread");
			if (!is_ident(ty, "Int"))
				die(ty->line, "M0 record fields must be `Int`");
			advance(p);
			Token *f = peek(p);
			if (f->kind != TK_IDENT || is_type_ident(f))
				die(f->line, "expected a field name");
			if (f->text[f->len - 1] == '!')
				die(f->line, "M0 does not support `!` in a field name");
			char fname[64];
			tok_copy(f, fname, sizeof fname);
			advance(p);
			if (peek(p)->kind == TK_EQ)
				die(peek(p)->line, "M0 record fields do not support defaults");
			if (data_field_index(d, fname) >= 0)
				die(f->line, "duplicate field name");
			if (d->nfields == MAX_FIELDS)
				die(f->line, "too many fields");
			snprintf(d->fields[d->nfields++], sizeof d->fields[0], "%s", fname);
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				if (peek(p)->kind == TK_RBRACE) /* trailing comma */
					break;
				continue;
			}
			break;
		}
	expect(p, TK_RBRACE, "expected `}`");
	if (d->nfields == 0)
		die(nm->line, "M0 records need at least one field");
	return d;
}

/* module = { declaration } — one per line; exactly one is `pub const main`. A
 * declaration is a `data` record type or a `[pub] const` function. */
static void parse(Parser *p, Program *prog) {
	skip_newlines(p);
	while (peek(p)->kind != TK_EOF) {
		if (is_ident(peek(p), "data"))
			prog_add_data(prog, parse_data_decl(p, prog));
		else
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

/* ------------------------------------------------------------- typecheck - */

static Type typeof_expr(Program *prog, Func *fn, Expr *e);

/* Require that `e` yields an Int. A record is legal only as a field-access base
 * and a pointer never, so wherever an Int is expected this rejects them both. */
static void expect_int(Program *prog, Func *fn, Expr *e) {
	if (typeof_expr(prog, fn, e).kind != TY_INT)
		die(e->line, "expected an Int value (a record is used only via field access in M0)");
}

/* Compute and validate the type of an expression, resolving field accesses and
 * calls against the whole program (so forward references and recursion work).
 * Field accesses are annotated in place (rec, foff) for emit. */
static Type typeof_expr(Program *prog, Func *fn, Expr *e) {
	switch (e->kind) {
	case EX_INT:
		return (Type){TY_INT, NULL};
	case EX_VAR: {
		Type ty;
		if (resolve_name(fn, e->name, &ty) == R_NONE)
			die(e->line, "unknown name");
		return ty;
	}
	case EX_FIELD: {
		Type base = typeof_expr(prog, fn, e->lhs);
		if (base.kind != TY_RECORD)
			die(e->line, "field access `.` needs a record value on the left");
		int idx = data_field_index(base.rec, e->name);
		if (idx < 0)
			die(e->line, "this data type has no such field");
		e->rec = base.rec;
		e->foff = idx * 4; /* all M0 fields are 4-byte Int */
		return (Type){TY_INT, NULL};
	}
	case EX_RECORD:
		die(e->line, "a record literal may only initialize a record-typed binding");
	case EX_CALL: {
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
		for (int i = 0; i < e->nargs; i++)
			expect_int(prog, fn, e->args[i]);
		return (Type){TY_INT, NULL};
	}
	case EX_IF:
		expect_int(prog, fn, e->lhs); /* condition */
		expect_int(prog, fn, e->rhs); /* then — M0 if-branches are Int */
		expect_int(prog, fn, e->els); /* else */
		return (Type){TY_INT, NULL};
	case EX_NEG:
	case EX_BNOT:
	case EX_LNOT:
		expect_int(prog, fn, e->lhs);
		return (Type){TY_INT, NULL};
	case EX_ADD: case EX_SUB: case EX_MUL: case EX_DIV: case EX_REM:
	case EX_BOR: case EX_BXOR: case EX_BAND: case EX_SHL: case EX_SHR:
	case EX_EQ: case EX_NE: case EX_LT: case EX_GT: case EX_LE: case EX_GE:
	case EX_AND: case EX_OR:
		expect_int(prog, fn, e->lhs);
		expect_int(prog, fn, e->rhs);
		return (Type){TY_INT, NULL};
	}
	die(e->line, "internal: unhandled expression kind in typecheck");
}

/* Bind an EX_RECORD data literal to its `data` declaration: check the fields
 * cover it exactly (each declared field set once, no unknowns, none missing),
 * type-check the values, reorder them into declaration order (`ford`), and
 * backfill the local's record type so later field accesses resolve. */
static void resolve_record_binding(Program *prog, Func *fn, Stmt *s) {
	Expr *e = s->expr; /* EX_RECORD; e->name is the annotated type name */
	DataDecl *d = prog_find_data(prog, e->name);
	if (!d)
		die(e->line, "unknown data type");
	e->rec = d;
	e->ford = xmalloc((size_t)d->nfields * sizeof *e->ford);
	for (int i = 0; i < d->nfields; i++)
		e->ford[i] = NULL;
	for (int k = 0; k < e->nfields; k++) {
		int idx = data_field_index(d, e->fnames[k]);
		if (idx < 0)
			die(e->line, "no such field on this data type");
		if (e->ford[idx])
			die(e->line, "field set more than once in the data literal");
		expect_int(prog, fn, e->fvals[k]);
		e->ford[idx] = e->fvals[k];
	}
	for (int i = 0; i < d->nfields; i++)
		if (!e->ford[i])
			die(e->line, "data literal is missing a field");
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, s->name) == 0) {
			fn->locals[i].type.rec = d;
			break;
		}
}

/* Type-check one function's body in source order, so a record binding's type is
 * resolved before any later statement reads its fields. */
static void check_func(Program *prog, Func *fn) {
	for (Stmt *s = fn->body; s; s = s->next) {
		switch (s->kind) {
		case ST_LOCAL:
			if (s->expr->kind == EX_RECORD)
				resolve_record_binding(prog, fn, s);
			else
				expect_int(prog, fn, s->expr);
			break;
		case ST_ASSIGN: /* target is a `let` word local; value is Int */
		case ST_RETURN: /* M0 functions return Int */
			expect_int(prog, fn, s->expr);
			break;
		}
	}
}

static void typecheck(Program *prog) {
	for (int i = 0; i < prog->nfuncs; i++)
		check_func(prog, prog->funcs[i]);
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

/* Per-function emit state: the next expression-temp and control-flow-label ids
 * (both function-scoped in QBE). */
typedef struct {
	int tmp;
	int lbl;
} Emit;

/* Emit the code that computes `e` into a fresh word temp, writing the operand
 * that names its value (a literal or a `%tN` temp) into `dst`. Constants are
 * inlined — QBE accepts them as instruction operands.
 *
 * Names live in stack slots (`%s_<name>`), so a variable reference is a `loadw`.
 * A record local's storage is the slot `%r_<name>` (a field read is a `loadw` at
 * the field's offset). The distinct `%s_` / `%r_` / `%u_` / `%tN` / `%ifN` /
 * `%lgN` prefixes (word slot / record storage / incoming param / temp / if-result
 * / logical-result) mean a user name can never collide with a compiler value. */
static void emit_expr(FILE *out, Expr *e, Emit *ex, char *dst, size_t cap) {
	switch (e->kind) {
	case EX_INT:
		snprintf(dst, cap, "%ld", e->ival);
		return;
	case EX_VAR: {
		int t = ex->tmp++;
		fprintf(out, "\t%%t%d =w loadw %%s_%s\n", t, e->name);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_FIELD: {
		/* Read a record field. The base is a record-typed local whose storage is
		 * the slot `%r_<name>`; the field sits at byte offset e->foff. (M0 field
		 * bases are always a name — record-returning exprs are a later increment.) */
		if (e->lhs->kind != EX_VAR)
			die(e->line, "internal: M0 field base is not a name");
		if (e->foff == 0) {
			int t = ex->tmp++;
			fprintf(out, "\t%%t%d =w loadw %%r_%s\n", t, e->lhs->name);
			snprintf(dst, cap, "%%t%d", t);
		} else {
			int a = ex->tmp++;
			fprintf(out, "\t%%t%d =l add %%r_%s, %d\n", a, e->lhs->name, e->foff);
			int t = ex->tmp++;
			fprintf(out, "\t%%t%d =w loadw %%t%d\n", t, a);
			snprintf(dst, cap, "%%t%d", t);
		}
		return;
	}
	case EX_RECORD:
		die(e->line, "internal: record literal in expression position");
	case EX_CALL: {
		/* Evaluate each argument into its own temp, then call. All M0 values are
		 * word; the callee symbol is the bare function name ($<name>). */
		int argt[MAX_PARAMS];
		for (int i = 0; i < e->nargs; i++) {
			char op[96];
			emit_expr(out, e->args[i], ex, op, sizeof op);
			argt[i] = ex->tmp++;
			fprintf(out, "\t%%t%d =w copy %s\n", argt[i], op);
		}
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =w call $%s(", r, e->name);
		for (int i = 0; i < e->nargs; i++)
			fprintf(out, "%sw %%t%d", i ? ", " : "", argt[i]);
		fprintf(out, ")\n");
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_IF: {
		/* if cond then A else B — merge the two branch values through a stack
		 * slot (a `phi` would need each value's predecessor block, which nesting
		 * makes awkward; a slot store/load is robust and reuses the local model).
		 * The condition is truthy when nonzero (`jnz`). */
		int id = ex->lbl++;
		char c[96];
		emit_expr(out, e->lhs, ex, c, sizeof c);
		fprintf(out, "\t%%if%d =l alloc4 4\n", id);
		fprintf(out, "\tjnz %s, @then%d, @else%d\n", c, id, id);
		fprintf(out, "@then%d\n", id);
		char tb[96];
		emit_expr(out, e->rhs, ex, tb, sizeof tb);
		fprintf(out, "\tstorew %s, %%if%d\n", tb, id);
		fprintf(out, "\tjmp @end%d\n", id);
		fprintf(out, "@else%d\n", id);
		char eb[96];
		emit_expr(out, e->els, ex, eb, sizeof eb);
		fprintf(out, "\tstorew %s, %%if%d\n", eb, id);
		fprintf(out, "\tjmp @end%d\n", id);
		fprintf(out, "@end%d\n", id);
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =w loadw %%if%d\n", r, id);
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_AND:
	case EX_OR: {
		/* Short-circuit, reusing the if slot-merge. Evaluate lhs; if it settles
		 * the result (false for &&, true for ||) store the constant and skip rhs,
		 * else evaluate rhs and store its truthiness (`cnew b, 0` → 0/1). */
		int id = ex->lbl++;
		int is_and = e->kind == EX_AND;
		char a[96];
		emit_expr(out, e->lhs, ex, a, sizeof a);
		fprintf(out, "\t%%lg%d =l alloc4 4\n", id);
		if (is_and)
			fprintf(out, "\tjnz %s, @rhs%d, @sc%d\n", a, id, id);
		else
			fprintf(out, "\tjnz %s, @sc%d, @rhs%d\n", a, id, id);
		fprintf(out, "@sc%d\n", id); /* short-circuit: 0 for &&, 1 for || */
		fprintf(out, "\tstorew %d, %%lg%d\n", is_and ? 0 : 1, id);
		fprintf(out, "\tjmp @lend%d\n", id);
		fprintf(out, "@rhs%d\n", id);
		char b[96];
		emit_expr(out, e->rhs, ex, b, sizeof b);
		int bt = ex->tmp++;
		fprintf(out, "\t%%t%d =w cnew %s, 0\n", bt, b); /* b != 0 → 0/1 */
		fprintf(out, "\tstorew %%t%d, %%lg%d\n", bt, id);
		fprintf(out, "\tjmp @lend%d\n", id);
		fprintf(out, "@lend%d\n", id);
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =w loadw %%lg%d\n", r, id);
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_NEG:
	case EX_BNOT:
	case EX_LNOT: {
		char a[96];
		emit_expr(out, e->lhs, ex, a, sizeof a);
		int t = ex->tmp++;
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
		emit_expr(out, e->lhs, ex, a, sizeof a);
		emit_expr(out, e->rhs, ex, b, sizeof b);
		int t = ex->tmp++;
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

	Emit ex = {0};
	for (Stmt *s = fn->body; s; s = s->next) {
		char v[96];
		switch (s->kind) {
		case ST_LOCAL:
			if (s->expr->kind == EX_RECORD) {
				/* A record local: reserve its storage (`%r_<name>`) and store each
				 * field's value at its offset, in declaration order (ford). */
				DataDecl *d = s->expr->rec;
				fprintf(out, "\t%%r_%s =l alloc4 %d\n", s->name, data_size(d));
				for (int fi = 0; fi < d->nfields; fi++) {
					char fv[96];
					emit_expr(out, s->expr->ford[fi], &ex, fv, sizeof fv);
					if (fi == 0) {
						fprintf(out, "\tstorew %s, %%r_%s\n", fv, s->name);
					} else {
						int a = ex.tmp++;
						fprintf(out, "\t%%t%d =l add %%r_%s, %d\n", a, s->name, fi * 4);
						fprintf(out, "\tstorew %s, %%t%d\n", fv, a);
					}
				}
			} else {
				fprintf(out, "\t%%s_%s =l alloc4 4\n", s->name);
				emit_expr(out, s->expr, &ex, v, sizeof v);
				fprintf(out, "\tstorew %s, %%s_%s\n", v, s->name);
			}
			break;
		case ST_ASSIGN:
			emit_expr(out, s->expr, &ex, v, sizeof v);
			fprintf(out, "\tstorew %s, %%s_%s\n", v, s->name);
			break;
		case ST_RETURN:
			emit_expr(out, s->expr, &ex, v, sizeof v);
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
	typecheck(&prog);

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
