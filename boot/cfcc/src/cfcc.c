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

static void die(int line, const char *msg) {
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
		case '=': push_tok(lx, TK_EQ, s + lx->pos, 1, 0); lx->pos++; continue;
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

/* Expression AST. M0 bodies are word-valued integer expressions: literals, Int
 * parameter references (argc), unary negation, and the binary arithmetic ops
 * with the ebnf precedence (additive over multiplicative over unary). */
typedef enum {
	EX_INT,   /* integer literal (ival) */
	EX_PARAM, /* reference to an Int parameter; QBE temp is %<name> */
	EX_NEG,   /* unary minus (lhs) */
	EX_ADD,
	EX_SUB,
	EX_MUL,
	EX_DIV,
	EX_REM,
} ExprKind;

typedef struct Expr Expr;
struct Expr {
	ExprKind kind;
	long ival;       /* EX_INT */
	char name[64];   /* EX_PARAM */
	Expr *lhs, *rhs; /* operands (unary uses lhs) */
};

static Expr *new_expr(ExprKind kind) {
	Expr *e = xmalloc(sizeof *e);
	memset(e, 0, sizeof *e);
	e->kind = kind;
	return e;
}

/* The whole program the M0 slice can express: `main(params) -> body`, where the
 * body is a word-valued integer expression returned as the exit code. */
typedef struct {
	Param params[3];
	int nparams;
	Expr *body;
} Program;

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

static Expr *parse_expr(Parser *p, Program *prog); /* forward */

/* primary = INT | var_name | "(" expr ")"
 * A var_name must resolve to one of main's Int parameters (only word-typed
 * values flow through the integer expression grammar). */
static Expr *parse_primary(Parser *p, Program *prog) {
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
		Expr *e = parse_expr(p, prog);
		expect(p, TK_RPAREN, "expected `)`");
		return e;
	}
	if (t->kind == TK_IDENT && !is_type_ident(t)) {
		if (t->text[t->len - 1] == '!')
			die(t->line, "M0 does not support `!` in a name here");
		Expr *e = new_expr(EX_PARAM);
		tok_copy(t, e->name, sizeof e->name);
		advance(p);
		int found = -1;
		for (int i = 0; i < prog->nparams; i++)
			if (strcmp(prog->params[i].name, e->name) == 0) {
				found = i;
				break;
			}
		if (found < 0)
			die(t->line, "unknown name (M0 expressions use integers and Int parameters)");
		if (prog->params[found].kind != PK_WORD)
			die(t->line, "M0 expressions can only use Int parameters (e.g. argc)");
		return e;
	}
	die(t->line, "expected an integer, a parameter, or `(`");
	return NULL; /* unreachable; die() exits */
}

/* unary = "-" unary | primary   (right-associative negation) */
static Expr *parse_unary(Parser *p, Program *prog) {
	if (peek(p)->kind == TK_MINUS) {
		advance(p);
		Expr *e = new_expr(EX_NEG);
		e->lhs = parse_unary(p, prog);
		return e;
	}
	return parse_primary(p, prog);
}

/* Left-associative binary level: fold `left op right op ...` given a pair of
 * token→ExprKind mappings supplied by the two callers below. */
static Expr *fold_binary(Parser *p, Program *prog, Expr *(*next)(Parser *, Program *),
                         TokKind t0, ExprKind k0, TokKind t1, ExprKind k1,
                         TokKind t2, ExprKind k2) {
	Expr *e = next(p, prog);
	for (;;) {
		TokKind k = peek(p)->kind;
		ExprKind op;
		if (k == t0)
			op = k0;
		else if (k == t1)
			op = k1;
		else if (t2 != TK_EOF && k == t2)
			op = k2;
		else
			break;
		advance(p);
		Expr *bin = new_expr(op);
		bin->lhs = e;
		bin->rhs = next(p, prog);
		e = bin;
	}
	return e;
}

/* multiplicative = unary { ("*" | "/" | "%") unary } */
static Expr *parse_mul(Parser *p, Program *prog) {
	return fold_binary(p, prog, parse_unary, TK_STAR, EX_MUL, TK_SLASH, EX_DIV,
	                   TK_PERCENT, EX_REM);
}

/* additive = multiplicative { ("+" | "-") multiplicative } */
static Expr *parse_add(Parser *p, Program *prog) {
	return fold_binary(p, prog, parse_mul, TK_PLUS, EX_ADD, TK_MINUS, EX_SUB,
	                   TK_EOF, EX_ADD /* unused third slot */);
}

static Expr *parse_expr(Parser *p, Program *prog) {
	return parse_add(p, prog);
}

/* module     = "pub" "const" "main" "=" param_list "->" body
 * param_list = "(" [ param { "," param } ] ")"             (0..3 entry params)
 * body       = expr | "{" "return" expr "}"
 * expr       = additive over multiplicative over unary over primary  (M0 slice) */
static void parse(Parser *p, Program *prog) {
	skip_newlines(p);
	expect_ident(p, "pub");
	expect_ident(p, "const");
	expect_ident(p, "main");
	expect(p, TK_EQ, "expected `=`");

	expect(p, TK_LPAREN, "expected `(`");
	prog->nparams = 0;
	if (peek(p)->kind != TK_RPAREN) {
		for (;;) {
			if (prog->nparams == 3)
				die(peek(p)->line, "main takes at most 3 parameters (argc, argv, envp)");
			parse_param(p, &prog->params[prog->nparams]);
			for (int j = 0; j < prog->nparams; j++)
				if (strcmp(prog->params[j].name, prog->params[prog->nparams].name) == 0)
					die(p->toks[p->pos - 1].line, "duplicate parameter name");
			prog->nparams++;
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
	}
	expect(p, TK_RPAREN, "expected `)`");
	expect(p, TK_ARROW, "expected `->`");

	Token *b = peek(p);
	if (b->kind == TK_LBRACE) {
		advance(p);
		skip_newlines(p);
		expect_ident(p, "return");
		prog->body = parse_expr(p, prog);
		skip_newlines(p);
		expect(p, TK_RBRACE, "expected `}`");
	} else {
		prog->body = parse_expr(p, prog);
	}

	skip_newlines(p);
	if (peek(p)->kind != TK_EOF)
		die(peek(p)->line, "trailing input (M0 accepts only a single `main`)");
}

/* ------------------------------------------------------------- emit QBE - */

/* Emit the code that computes `e` into a fresh word temp, writing the operand
 * that names its value (a literal, a `%param`, or a `%tN` temp) into `dst`.
 * Constants are inlined — QBE accepts them directly as instruction operands. */
static void emit_expr(FILE *out, Expr *e, int *tmp, char *dst, size_t cap) {
	switch (e->kind) {
	case EX_INT:
		snprintf(dst, cap, "%ld", e->ival);
		return;
	case EX_PARAM:
		snprintf(dst, cap, "%%%s", e->name);
		return;
	case EX_NEG: {
		char a[96];
		emit_expr(out, e->lhs, tmp, a, sizeof a);
		int t = (*tmp)++;
		fprintf(out, "\t%%t%d =w neg %s\n", t, a);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_ADD:
	case EX_SUB:
	case EX_MUL:
	case EX_DIV:
	case EX_REM: {
		char a[96], b[96];
		emit_expr(out, e->lhs, tmp, a, sizeof a);
		emit_expr(out, e->rhs, tmp, b, sizeof b);
		const char *op = e->kind == EX_ADD ? "add"
		               : e->kind == EX_SUB ? "sub"
		               : e->kind == EX_MUL ? "mul"
		               : e->kind == EX_DIV ? "div"
		                                   : "rem"; /* EX_REM; div/rem are signed */
		int t = (*tmp)++;
		fprintf(out, "\t%%t%d =w %s %s, %s\n", t, op, a, b);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	}
	/* All ExprKinds are handled above (no default, so -Wswitch catches a new one
	 * before it can be miscompiled); this guards a corrupted node. */
	die(0, "internal: unhandled expression kind");
}

static void emit_qbe(FILE *out, const Program *prog) {
	/* `main` returns a word (Int exit code) and takes argc/argv/envp per arity —
	 * darwin hands these to _start in x0/x1/x2, which forwards them, so QBE reads
	 * them as ordinary args. No page node is threaded yet: a non-allocating main
	 * carries none (M0). */
	fprintf(out, "export function w $main(");
	for (int i = 0; i < prog->nparams; i++)
		fprintf(out, "%s%s %%%s", i ? ", " : "",
		        prog->params[i].kind == PK_WORD ? "w" : "l", prog->params[i].name);
	fprintf(out, ") {\n");
	fprintf(out, "@start\n");
	char result[96];
	int tmp = 0;
	emit_expr(out, prog->body, &tmp, result, sizeof result);
	fprintf(out, "\tret %s\n", result);
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
	emit_qbe(f, &prog);
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
