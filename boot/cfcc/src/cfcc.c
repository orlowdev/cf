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
	TK_STR,   /* "..." string literal (decoded bytes in Token.sval/slen) */
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

/* A string breaks into segments: literal byte-runs and `${name}` interpolations,
 * in source order (ebnf § Strings). A plain string is a single SEG_LIT. cfcc
 * restricts interpolation content to a bare identifier — enough for asm bodies'
 * `${param}`/`${CONST}` — a disclaimed narrowing of the grammar's `${ expression }`. */
typedef enum { SEG_LIT, SEG_INTERP } StrSegKind;
typedef struct {
	StrSegKind kind;
	char *lit;   /* SEG_LIT: decoded bytes (heap-owned; may embed NULs) */
	int litlen;
	char name[64]; /* SEG_INTERP: the interpolated bare name */
} StrSeg;

typedef struct {
	TokKind kind;
	int line;
	const char *text; /* into the source buffer; not NUL-terminated */
	int len;
	long ival; /* for TK_INT */
	char *sval; /* for TK_STR w/o interpolation: the full decoded bytes, heap-owned */
	int slen;   /* for TK_STR w/o interpolation: the decoded byte count (may embed NULs) */
	StrSeg *segs;   /* for TK_STR: the segment list (literal runs + interpolations) */
	int nsegs;
	int has_interp; /* for TK_STR: 1 if any segment is a `${…}` interpolation */
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

/* Append a string segment (growing the array), returning it for the caller to
 * fill. Used only while lexing a string literal into its literal/interp pieces. */
static StrSeg *push_seg(StrSeg **segs, int *nsegs, int *cap) {
	if (*nsegs == *cap) {
		*cap = *cap ? *cap * 2 : 4;
		*segs = realloc(*segs, (size_t)*cap * sizeof **segs);
		if (!*segs)
			die(0, "out of memory");
	}
	StrSeg *sg = &(*segs)[(*nsegs)++];
	memset(sg, 0, sizeof *sg);
	return sg;
}

/* Flush the pending literal run `buf[0..n]` into a SEG_LIT (copying it, since buf
 * is reused for the next run). An empty run still produces a segment so segments
 * strictly alternate lit/interp/lit — simplifying later substitution. */
static void flush_lit(StrSeg **segs, int *nsegs, int *cap, const char *buf, int n) {
	StrSeg *sg = push_seg(segs, nsegs, cap);
	sg->kind = SEG_LIT;
	sg->lit = xmalloc((size_t)n + 1);
	memcpy(sg->lit, buf, (size_t)n);
	sg->lit[n] = '\0';
	sg->litlen = n;
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
		if (c == '"') {
			/* "..." — a string literal, lexed into segments: literal byte-runs and
			 * `${name}` interpolations (ebnf § Strings). Per the grammar a literal MAY
			 * span multiple lines — a raw newline is content — so only EOF terminates
			 * an unclosed string. `${` opens an interpolation; a lone `$` (not before
			 * `{`) is literal, and `\$` forces a literal `$`.
			 *
			 * cfcc narrowings (throwaway; cf0.cf must not inherit — it takes the full
			 * grammar): interpolation content is a bare identifier only (the grammar
			 * allows any `${ expression }`); the escape set is the provisional
			 * `\n \t \r \0 \\ \" \$`; and the {bytes*,len} header / `.len` are
			 * provisional, re-pinned at the M6/M9 representation gate. Whether an
			 * interpolated string is usable (asm body) or a deferred error (an ordinary
			 * value) is decided later, by the parser. */
			size_t start = lx->pos;
			int startline = lx->line;
			lx->pos++; /* opening quote */
			/* The decoded bytes of any one literal run are never longer than the
			 * remaining source, so one malloc of that length is always enough. */
			char *buf = xmalloc(strlen(s + lx->pos) + 1);
			int n = 0;
			StrSeg *segs = NULL;
			int nsegs = 0, capsegs = 0, has_interp = 0;
			for (;;) {
				int ch = (unsigned char)s[lx->pos];
				if (ch == '\0')
					die(startline, "unterminated string literal");
				if (ch == '"') {
					lx->pos++; /* closing quote */
					break;
				}
				if (ch == '\n') { /* a raw newline is literal content; keep line count */
					buf[n++] = '\n';
					lx->pos++;
					lx->line++;
					continue;
				}
				if (ch == '\\') {
					int e = (unsigned char)s[lx->pos + 1];
					char d;
					switch (e) {
					case 'n':  d = '\n'; break;
					case 't':  d = '\t'; break;
					case 'r':  d = '\r'; break;
					case '0':  d = '\0'; break;
					case '\\': d = '\\'; break;
					case '"':  d = '"';  break;
					case '$':  d = '$';  break;
					default: die(lx->line, "unknown string escape (M0 allows \\n \\t \\r \\0 \\\\ \\\" \\$)");
					}
					buf[n++] = d;
					lx->pos += 2;
					continue;
				}
				if (ch == '$' && s[lx->pos + 1] == '{') {
					/* `${name}` — flush the pending literal run, then read a bare name. */
					flush_lit(&segs, &nsegs, &capsegs, buf, n);
					n = 0;
					has_interp = 1;
					lx->pos += 2; /* past `${` */
					StrSeg *sg = push_seg(&segs, &nsegs, &capsegs);
					sg->kind = SEG_INTERP;
					int nl = 0;
					while (ident_char((unsigned char)s[lx->pos])) {
						if (nl >= (int)sizeof sg->name - 1)
							die(lx->line, "interpolation name too long");
						sg->name[nl++] = s[lx->pos++];
					}
					sg->name[nl] = '\0';
					if (nl == 0)
						die(lx->line, "empty `${}` interpolation");
					if (s[lx->pos] != '}')
						die(lx->line, "M0 interpolation must be a bare name: `${name}`");
					lx->pos++; /* past `}` */
					continue;
				}
				/* a lone `$` (not `${`) is a literal byte, like any other char */
				buf[n++] = (char)ch;
				lx->pos++;
			}
			flush_lit(&segs, &nsegs, &capsegs, buf, n); /* final literal run */
			push_tok(lx, TK_STR, s + start, (int)(lx->pos - start), 0);
			Token *tk = &lx->toks[lx->ntoks - 1];
			tk->segs = segs;
			tk->nsegs = nsegs;
			tk->has_interp = has_interp;
			if (!has_interp) { /* a plain string is one SEG_LIT — the whole content */
				tk->sval = segs[0].lit;
				tk->slen = segs[0].litlen;
			}
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
		if (c == '\'') {
			/* A type variable: `'T` — an apostrophe followed by an identifier. Lexed as
			 * one TK_IDENT whose text starts with `'` (ebnf: `type_var = "'" ident`). */
			size_t start = lx->pos;
			lx->pos++; /* the apostrophe */
			if (!(isalpha((unsigned char)s[lx->pos]) || s[lx->pos] == '_'))
				die(lx->line, "expected an identifier after `'` (a type variable, e.g. `'T`)");
			while (ident_char((unsigned char)s[lx->pos]))
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
	int loop_depth; /* how many loops enclose the statement being parsed */
} Parser;

/* The type of a value. M0 has three: `Int` (a word), an opaque pointer (`*T` —
 * argv/envp, never dereferenced), and a named record (`data`) type. A record
 * carries a pointer to its declaration, which fixes its fields and layout; the
 * pointer is filled in by the typecheck pass (parse leaves it NULL). */
typedef struct DataDecl DataDecl;
typedef struct UnionDecl UnionDecl;
struct Func; /* forward: an EX_CALL caches its resolved callee for emit */

typedef enum {
	TY_INT,    /* Int — a word */
	TY_PTR,    /* *T  — an opaque pointer (a long; not usable in Int expressions) */
	TY_RECORD, /* a `data` record type (see rec) */
	TY_STR,    /* Str — an `l` pointer to a {bytes*, len} header; not an Int */
	TY_UARCH,  /* Uarch — a register-width (pointer-width) unsigned integer, an `l`.
	            * The concrete pointer-width unsigned leaf (type_system §2): the syscall
	            * floor's register/count type. cfcc has it only as a parameter/return/
	            * argument value (no Uarch locals or arithmetic yet); the union `Uint` is
	            * not modeled (no unions in cfcc). */
	TY_BUF,    /* a `[N Uint8]` fixed byte buffer — an `l` pointer to N arena bytes, the
	            * writable buffer `read` fills. Named by a local; decays to a `*[Uint8]`
	            * where a pointer/Uarch is expected. Throwaway: no indexing/bounds, no
	            * length value (cf0's `[N Uint8]` is a real array). */
	TY_UNION,  /* a `union` type. M1.1 handles ALL-nullary (tag-only) unions only, which
	            * lower to a plain integer tag (type_system §8.4) — so a union value is a
	            * `w`, stored/read like an Int; the union identity (for match) is `uni`.
	            * Payload unions (tag+aggregate) are a later brick. */
} TypeKind;

typedef struct {
	TypeKind kind;
	DataDecl *rec;  /* TY_RECORD: the record's declaration */
	UnionDecl *uni; /* TY_UNION: the union's declaration */
} Type;

/* The entry ABI kinds an M0 `main` parameter can take: a word (Int, e.g. argc)
 * or a long (a pointer, e.g. *[Str] argv/envp). darwin hands these to _start in
 * x0/x1/x2, which forwards them unchanged, so QBE reads them as ordinary args. */
typedef enum {
	PK_WORD,   /* Int   -> w (argc) */
	PK_LONG,   /* *T    -> l (argv, envp, `*[Uint8]` buffers) — an opaque pointer */
	PK_RECORD, /* a record type -> l (a pointer to the caller's arena record) */
	PK_UARCH,  /* Uarch -> l (a register-width unsigned integer; see TY_UARCH) */
	PK_UNION,  /* a tag-only union -> w (the tag). Parsed as PK_RECORD, reclassified in
	            * resolve_signatures once the name is known to be a union. */
	PK_VAR,    /* a generic type-variable param (`'T x`) in a template. Never emitted —
	            * the monomorphization pass substitutes it with a concrete kind per
	            * instantiation. `type_name` holds the type-variable name (`'T`). */
} ParamKind;

typedef struct {
	char name[64];
	ParamKind kind;
	int line;           /* source line of the type (for diagnostics) */
	char type_name[64]; /* PK_RECORD/PK_UNION: the nominal type name (resolved later) */
	DataDecl *rec;      /* PK_RECORD: the resolved declaration */
	UnionDecl *uni;     /* PK_UNION: the resolved declaration */
} Param;

/* Expression AST. M0 expressions are word-valued: literals, references to a
 * bound Int name (a parameter or a local), unary (negate / bitwise-not /
 * logical-not), and the binary arithmetic, bitwise, shift, and comparison ops,
 * all at the ebnf precedence (comparison > bit-or/xor/and > shift > additive >
 * multiplicative). */
typedef enum {
	EX_INT,   /* integer literal (ival) */
	EX_STR,   /* string literal (sval/slen; strid names its module data) */
	EX_VAR,   /* reference to a bound name (param or local; any type) */
	EX_FIELD, /* record field access: base.name (lhs=base record expr) */
	EX_RECORD,/* record construction: a data literal { f: v, ... } of type `name` */
	EX_CALL,  /* function call: name(args) */
	EX_UMEMBER,/* union member value: Union.Member — a tag-only member's tag (uni, ival=tag) */
	EX_MATCH, /* match scrut { arms } — compare-chain tag dispatch (lhs=scrut, arms/narms) */
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

#define MAX_ARM_ALTS 16
#define MAX_TYPARAMS 8

/* One arm of a `match`. Either the `_` wildcard (`is_wild`) or an or-pattern of one
 * or more member alternatives (`A | B | …`), all qualified by the scrutinee's union
 * (`qual`). Each alternative names a member; its tag is resolved in typecheck. */
typedef struct {
	char qual[64];   /* the written union qualifier (shared by all alternatives) */
	char members[MAX_ARM_ALTS][64];
	int tags[MAX_ARM_ALTS]; /* per-alternative member tag, resolved in typecheck */
	int nalts;
	int is_wild;
	/* Payload binding (single-member arms only): `Node.IntLit(v)` binds `v` to payload
	 * field 0. `binds[i]` is the name (or "_" to ignore field i); `bind_ids[i]` the
	 * per-function storage id (`%pb<id>`), assigned in typecheck; `nbinds` = 0 for a
	 * bare/or-pattern arm, else the member's arity. */
	char binds[MAX_ARM_ALTS][64];
	int bind_ids[MAX_ARM_ALTS];
	int bind_word[MAX_ARM_ALTS]; /* 1 = the bound payload field is word-repr (Int/tag-only union),
	                              * 0 = pointer-repr (record/boxed union); set in typecheck */
	int nbinds;
	Expr *body;
	int line;
} MatchArm;

struct Expr {
	ExprKind kind;
	int line;         /* source line (for EX_CALL diagnostics) */
	long ival;        /* EX_INT */
	char *sval;       /* EX_STR: decoded bytes (heap-owned; may embed NULs) */
	int slen;         /* EX_STR: decoded byte count */
	int strid;        /* EX_STR: index into the module's string table (emit) */
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
	/* The expression's resolved type, filled by the typecheck pass. Emit reads it to
	 * tell a record value (an `l` arena pointer — passed/returned by pointer) from a
	 * word. */
	Type rtype;
	/* EX_IF/EX_AND/EX_OR: id of the 4-byte stack slot that merges the branch values.
	 * Assigned per function before emit (assign_stmt_slots) and allocated once in the
	 * entry block, so an if/logical inside a loop does not `alloc4` each iteration. */
	int slot;
	/* EX_CALL: the resolved callee, cached by typecheck so emit can read each
	 * parameter's kind (to pick the argument register width and widen an Int→Uarch). */
	struct Func *callee;
	/* EX_UMEMBER: `Union.Member`. `name` holds the union type name, `mem` the member
	 * name (parse); typecheck resolves `uni` and sets `ival` to the member's tag.
	 * EX_MATCH: `lhs` is the scrutinee; `uni` its union; `arms`/`narms` the arms. */
	char mem[64];
	UnionDecl *uni;
	MatchArm *arms;
	int narms;
	/* EX_VAR: set by typecheck when the name resolves to a match-arm payload binding
	 * (not a param/local) — emit reads its value from the `%pb<bind_id>` storage temp. */
	int is_bind;
	int bind_id;
	/* EX_CALL: explicit type arguments `f[Int, Point](…)`. The monomorphization pass
	 * uses them to select the instantiation, then rewrites `name` to the mangled clone. */
	char typeargs[MAX_TYPARAMS][64];
	int ntypeargs;
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
	ST_LOCAL,       /* const/let binding: declare + initialize */
	ST_ASSIGN,      /* reassign an existing `let` word local */
	ST_FIELD_ASSIGN,/* mutate a field of a `let` record local: name.field = expr */
	ST_RETURN,
	ST_LOOP,        /* loop { body } — infinite loop statement */
	ST_BREAK,       /* break (bare) or `if cond then break` (guard in expr) */
	ST_CONTINUE,    /* continue (bare) or `if cond then continue` (guard in expr) */
	ST_EXPR,        /* an expression evaluated for effect (a call), result discarded */
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
	StmtKind kind;
	int line;           /* source line of the statement (for diagnostics) */
	char name[64];      /* ST_LOCAL/ST_ASSIGN/ST_FIELD_ASSIGN: the target name */
	char field[64];     /* ST_FIELD_ASSIGN: the mutated field */
	char type_name[64]; /* ST_LOCAL: the record type annotation (empty = a word local) */
	int foff;           /* ST_FIELD_ASSIGN: the field's byte offset (filled by typecheck) */
	int bufsize;        /* ST_LOCAL: >0 ⇒ a `[N Uint8]` byte-buffer local of N bytes
	                     * (arena-allocated, no initializer); 0 otherwise */
	Expr *expr;         /* ST_LOCAL/ST_ASSIGN/ST_FIELD_ASSIGN: value (NULL for a buffer
	                     * local); ST_RETURN: returned; ST_BREAK/ST_CONTINUE: guard or NULL */
	Stmt *body;         /* ST_LOOP: the loop body statement list */
	Stmt *next;
};

/* A statement diverges (ends its block unconditionally): a `return`, or a bare
 * `break`/`continue`. A guarded break/continue and a loop fall through. */
static int stmt_is_terminal(const Stmt *s) {
	return s->kind == ST_RETURN ||
	       ((s->kind == ST_BREAK || s->kind == ST_CONTINUE) && !s->expr);
}

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
	char type_name[64]; /* the declared nominal type name (record/union/Str); empty for a
	                     * word (Int) local. Available pre-typecheck for generic inference. */
} Binding;

#define MAX_PARAMS 32
#define MAX_FIELDS 64
#define MAX_LOOP_DEPTH 64 /* cap on statically-nested loops (bounds Emit.loops[]) */

/* A `data` record declaration: `data Name = { Int f0, Point f1, ... }`. A field is
 * an `Int` or an aggregate (a `data` record / a `union`) type (G3a). Fields are laid
 * out in declaration order in UNIFORM 8-byte slots: field i sits at byte offset i*8
 * and the record occupies nfields*8 bytes. An Int field stores a word in the low 4
 * bytes of its slot; an aggregate field stores an 8-byte arena pointer (a record or
 * boxed-union VALUE is represented as a pointer in cfcc, so an aggregate field is an
 * implicit pointer — this incidentally permits pointer-recursive shapes like a linked
 * list; the explicit `*T` pointer-field syntax and its escape/identity semantics stay
 * deferred). ⚠ cf0 must NOT inherit this bare-aggregate implicit pointer: type_system §8.4
 * spells a recursive self-link as an EXPLICIT `*Node` (inline-by-value `T` and pointer `*T`
 * are distinct field forms there); cfcc collapses them into one implicit pointer as a
 * genesis shortcut. ⚠ THROWAWAY layout: the uniform 8-byte slot wastes space and the real
 * inline-vs-boxed field decision belongs to the M6/M9 representation gate. */
struct DataDecl {
	char name[64];        /* the concrete (possibly mangled, e.g. `Box.1.Int`) type name */
	char base_name[64];   /* the un-mangled template name (== name for a non-generic decl) */
	char fields[MAX_FIELDS][64];
	char field_types[MAX_FIELDS][64]; /* per-field type name: "Int", an aggregate name, a `'T`,
	                                   * or a mangled application (concretized in monomorphize) */
	int nfields;
	/* Generic type parameters (`data Pair['A,'B]`): ntyparams > 0 = a TEMPLATE, never laid
	 * out or emitted — cloned + substituted per concrete type-argument tuple (G3b). */
	char typarams[MAX_TYPARAMS][64];
	char bounds[MAX_TYPARAMS][64]; /* per-typaram generic bound (a union name), or "" = unbounded */
	int ntyparams;
};

#define MAX_UNION_MEMBERS 64

/* A `union Name = { A, B, C }` declaration. M1.1 handles ALL-nullary members — a pure
 * enum: each member is a fresh tag, and the union lowers to a plain integer tag
 * (type_system §8.4; seed_subset §7). The member names are stored qualified-only
 * (reached as `Name.Member`). A member may carry a positional payload of Int and/or
 * aggregate (record/union) fields (G3a); struct-body `M = { … }`/literal members and
 * compose-over/spread members are later bricks. A payload member whose field is its own
 * union is an implicit-pointer recursive union (§8.4 sanctions recursive unions), same
 * throwaway implicit-pointer treatment as a record field (see DataDecl).
 * ⚠ THROWAWAY layout (like the record/Str layouts): member i gets tag i by declaration
 * order, and a boxed union is `[tag @0][field @8+i*8]` in UNIFORM 8-byte slots (a word
 * field in the low half of its slot, an aggregate field an 8-byte pointer), size
 * 8+maxarity*8. type_system §8.4 fixes only the *shape* (all-tag→int, else tag+payload
 * aggregate) and hands the actual tag assignment, tag width, field offsets/packing, and
 * cross-sub/superset consistency to the M6/M9 representation gate — cf0 must not inherit
 * these specific bytes, the uniform-8 slotting, or the 0..n-by-order tags. */
struct UnionDecl {
	char name[64];        /* the concrete (possibly mangled, e.g. `Maybe.1.Int`) type name */
	char base_name[64];   /* the un-mangled template name (== name for a non-generic decl);
	                       * a match arm qualifies members by this (`Maybe.Just`, not the mangle) */
	char typarams[MAX_TYPARAMS][64]; /* generic type parameters; ntyparams > 0 = a TEMPLATE */
	char bounds[MAX_TYPARAMS][64];   /* per-typaram generic bound (a union name), or "" = unbounded */
	int ntyparams;
	char members[MAX_UNION_MEMBERS][64];
	int arity[MAX_UNION_MEMBERS]; /* positional payload field count per member (0 = nullary) */
	/* Per-member, per-field payload type name: "Int" or an aggregate (record/union) type
	 * (G3a — a member may carry aggregate payloads, incl. its own union → a boxed tree). */
	char payload_types[MAX_UNION_MEMBERS][MAX_ARM_ALTS][64];
	int nmembers;
	int has_payload; /* 1 if any member carries a payload → a boxed tag+payload union */
	int size;        /* boxed-union aggregate size, UNIFORM 8-byte slots: 8 (tag) + max arity * 8 */
};

/* Tag (0-based declaration index) of a member by name, or -1 if the union has none. */
static int union_member_tag(const UnionDecl *u, const char *name) {
	for (int i = 0; i < u->nmembers; i++)
		if (strcmp(u->members[i], name) == 0)
			return i;
	return -1;
}

/* True if a value of type `t` is word-sized (a QBE `w`): an Int, or a TAG-ONLY union
 * (a plain tag). A boxed (payload) union is a pointer to an arena aggregate — an `l`,
 * handled like a `data` record — as are records/pointers/Uarch/Str/buffers. */
static int type_is_word(Type t) {
	if (t.kind == TY_INT)
		return 1;
	if (t.kind == TY_UNION)
		return !t.uni->has_payload;
	return 0;
}

/* True if a parameter is passed in a word register: a word Int, or a tag-only union.
 * A boxed (payload) union is passed by pointer, like a record. */
static int param_is_word(const Param *p) {
	return p->kind == PK_WORD || (p->kind == PK_UNION && !p->uni->has_payload);
}

/* Record layout — uniform 8-byte slots (G3a): field i at byte offset i*8, size nfields*8.
 * A boxed union puts its tag in the first slot, so payload field i sits at 8 + i*8. */
static int data_size(const DataDecl *d) { return d->nfields * 8; }
static int data_field_offset(int idx) { return idx * 8; }
static int union_payload_offset(int fieldidx) { return 8 + fieldidx * 8; }

/* Index of a field by name, or -1 if the record has no such field. */
static int data_field_index(const DataDecl *d, const char *name) {
	for (int i = 0; i < d->nfields; i++)
		if (strcmp(d->fields[i], name) == 0)
			return i;
	return -1;
}

/* A top-level function: `[pub] const name = (params) [ret] -> body`. A function
 * returns Int (word) unless `ret_type_name` names a record type, in which case it
 * returns that record by pointer (`ret_rec`, resolved in typecheck). `locals`
 * accrues during parsing for name resolution within this function. */
typedef struct Func {
	char name[64];
	int is_pub;
	/* Generic type parameters (`'T`, `'U`): a function with ntyparams > 0 is a generic
	 * TEMPLATE — not emitted directly, but cloned + specialized per concrete type-argument
	 * tuple by the monomorphization pass. A `'T` appears as a type-name string ("'T") in a
	 * param/return/local annotation; specialization substitutes it with a concrete type. */
	char typarams[MAX_TYPARAMS][64];
	char bounds[MAX_TYPARAMS][64]; /* per-typaram generic bound (a union name), or "" = unbounded */
	int ntyparams;
	Param params[MAX_PARAMS];
	int nparams;
	char ret_type_name[64]; /* empty = Int; "Uarch" = Uarch; else a record/union/'T return */
	int ret_line;           /* source line of the return type (for diagnostics) */
	DataDecl *ret_rec;      /* resolved record return type (typecheck) */
	UnionDecl *ret_uni;     /* resolved union return type (typecheck) */
	Binding *locals;
	int nlocals, cap_locals;
	/* Match-arm payload bindings currently in scope (a stack, pushed/popped around each
	 * binding arm's body during typecheck; nested matches nest). `next_bind_id` mints a
	 * per-function unique storage id (`%pb<id>`). */
	struct { char name[64]; int id; Type type; } abinds[64];
	int nabinds;
	int next_bind_id;
	Stmt *body;             /* the statement/expression body — NULL for an asm function */
	int is_asm;             /* 1 if the body is an `asm "..."` block (see asm_body) */
	Token *asm_body;        /* is_asm: the asm string token, whose segments are emitted
	                         * verbatim (with `${param}` → arg register) into the .s */
} Func;

/* The whole program: a set of `data` declarations and a set of functions, one of
 * which is `pub const main`. */
typedef struct {
	DataDecl **datas;
	int ndatas, cap_datas;
	UnionDecl **unions;
	int nunions, cap_unions;
	Func **funcs;
	int nfuncs, cap_funcs;
} Program;

/* Index of a type variable among a function's generic parameters, or -1. */
static int func_typaram_index(const Func *fn, const char *name) {
	for (int i = 0; i < fn->ntyparams; i++)
		if (strcmp(fn->typarams[i], name) == 0)
			return i;
	return -1;
}

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

static UnionDecl *prog_find_union(Program *prog, const char *name) {
	for (int i = 0; i < prog->nunions; i++)
		if (strcmp(prog->unions[i]->name, name) == 0)
			return prog->unions[i];
	return NULL;
}

static void prog_add_union(Program *prog, UnionDecl *u) {
	if (prog->nunions == prog->cap_unions) {
		prog->cap_unions = prog->cap_unions ? prog->cap_unions * 2 : 8;
		prog->unions = realloc(prog->unions, prog->cap_unions * sizeof *prog->unions);
		if (!prog->unions)
			die(0, "out of memory");
	}
	prog->unions[prog->nunions++] = u;
}

/* Resolve a concrete field/payload type name to a Type (G3a): "Int" or an aggregate
 * (a `data` record or a `union`). A record and a boxed union are pointer-repr; an Int
 * and a tag-only union are word-repr (see type_is_word). Dies if the name is unknown. */
static Type resolve_member_type(Program *prog, const char *name, int line) {
	if (strcmp(name, "Int") == 0)
		return (Type){TY_INT, NULL, NULL};
	UnionDecl *u = prog_find_union(prog, name);
	if (u)
		return (Type){TY_UNION, NULL, u};
	DataDecl *d = prog_find_data(prog, name);
	if (d)
		return (Type){TY_RECORD, d, NULL};
	char msg[128];
	snprintf(msg, sizeof msg, "unknown field/payload type `%s`", name);
	die(line, msg);
}

/* The Type of record field `idx` / union member `tag` payload field `fieldidx`. */
static Type data_field_type(Program *prog, const DataDecl *d, int idx) {
	return resolve_member_type(prog, d->field_types[idx], 0);
}
static Type union_payload_type(Program *prog, const UnionDecl *u, int tag, int fieldidx) {
	return resolve_member_type(prog, u->payload_types[tag][fieldidx], 0);
}

/* How a name resolves in a function's scope. */
typedef enum {
	R_NONE,  /* undefined */
	R_PARAM, /* a parameter (immutable) */
	R_CONST, /* a `const` local (immutable) */
	R_LET,   /* a `let` local (reassignable) */
} Resolution;

/* Resolve a name (params first, then locals) and set *ty to its type. A word param
 * is Int; a record param is TY_RECORD (its decl, resolved in typecheck); a pointer
 * param is TY_PTR (argv/envp — not usable as an Int). */
static Resolution resolve_name(Func *fn, const char *name, Type *ty) {
	for (int i = 0; i < fn->nparams; i++)
		if (strcmp(fn->params[i].name, name) == 0) {
			ty->uni = NULL;
			switch (fn->params[i].kind) {
			case PK_WORD:   ty->kind = TY_INT;    ty->rec = NULL; break;
			case PK_RECORD: ty->kind = TY_RECORD; ty->rec = fn->params[i].rec; break;
			case PK_LONG:   ty->kind = TY_PTR;    ty->rec = NULL; break;
			case PK_UARCH:  ty->kind = TY_UARCH;  ty->rec = NULL; break;
			case PK_UNION:  ty->kind = TY_UNION;  ty->rec = NULL; ty->uni = fn->params[i].uni; break;
			case PK_VAR:    ty->kind = TY_INT;    ty->rec = NULL; break; /* template body parse only; type is ignored (re-typed per instantiation) */
			}
			return R_PARAM;
		}
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			*ty = fn->locals[i].type;
			return fn->locals[i].mutable ? R_LET : R_CONST;
		}
	return R_NONE;
}

/* If `name` is a match-arm payload binding currently in scope, return its storage id
 * (`%pb<id>`); else -1. Innermost binding wins, so a binding shadows an outer name. */
static int find_active_bind(Func *fn, const char *name) {
	for (int i = fn->nabinds - 1; i >= 0; i--)
		if (strcmp(fn->abinds[i].name, name) == 0)
			return fn->abinds[i].id;
	return -1;
}

/* Like find_active_bind, but also yields the binding's type (set at typecheck time,
 * when the payload field type is known). Used by typeof_expr to type a bound name. */
static int find_active_bind_type(Func *fn, const char *name, Type *ty) {
	for (int i = fn->nabinds - 1; i >= 0; i--)
		if (strcmp(fn->abinds[i].name, name) == 0) {
			*ty = fn->abinds[i].type;
			return fn->abinds[i].id;
		}
	return -1;
}

/* Record a new local with the given type. Caller has checked for a clash. For a
 * record local parse passes {TY_RECORD, NULL, NULL}; the typecheck pass backfills rec
 * (see resolve_record_binding). */
static void func_add_local(Func *fn, const char *name, int mutable, Type ty, const char *type_name) {
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
	snprintf(b->type_name, sizeof b->type_name, "%s", type_name ? type_name : "");
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

/* True if a token is a type variable — `'T` (lexed as an ident whose text starts with
 * an apostrophe). Type variables name a function's generic parameters. */
static int is_tyvar(Token *t) {
	return t->kind == TK_IDENT && t->len > 0 && t->text[0] == '\'';
}

/* True if `name` is a built-in type keyword cfcc knows. A bare union member of this
 * name would be a *compose-over* (the union carrying that type's value), which M1.1
 * does not implement — so it is rejected rather than minted as a fresh nullary tag. */
static int is_builtin_type_name(const char *name) {
	return strcmp(name, "Int") == 0 || strcmp(name, "Uarch") == 0 ||
	       strcmp(name, "Str") == 0 || strcmp(name, "Uint8") == 0;
}

/* Type-reference helpers (G3b) — defined further down, forward-declared here as the type
 * positions that use them (params, union values, locals, returns) come first. */
static void parse_type_arg(Parser *p, char *out, size_t cap);
static void check_tyvars_declared(const char *mangled, char typarams[][64], int ntp, int line);

/* Consume a parameter's type and classify it. M0 param types are `Int` (a word),
 * a record type (a long — a pointer to the caller's arena record; the type name is
 * stashed and resolved to a decl in typecheck), or a pointer type like `*[Str]` (a
 * long — argv/envp); a pointer's pointee is skipped wholesale since only the
 * top-level shape sets the register width. Fills `out->kind` (and `out->type_name`
 * for a record). */
static void parse_param_type(Parser *p, Param *out) {
	Token *t = peek(p);
	out->line = t->line;
	if (is_ident(t, "Int")) {
		advance(p);
		out->kind = PK_WORD;
		return;
	}
	if (is_ident(t, "Uarch")) {
		advance(p);
		out->kind = PK_UARCH;
		return;
	}
	if (is_ident(t, "Str")) /* Str params await a later brick (Str is a local-only type in M0) */
		die(t->line, "M0 has no Str parameters yet (pass `*[Uint8]` + a `Uarch` length)");
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
		out->kind = PK_LONG;
		return;
	}
	if (is_tyvar(t)) { /* a generic type variable (`'T`) — resolved at specialization */
		tok_copy(t, out->type_name, sizeof out->type_name);
		out->kind = PK_VAR;
		advance(p);
		return;
	}
	if (is_type_ident(t)) { /* a record/union type, or a generic application `Box[Int]` (G3b) */
		parse_type_arg(p, out->type_name, sizeof out->type_name);
		out->kind = PK_RECORD; /* resolve_signatures reclassifies a union to PK_UNION */
		return;
	}
	die(t->line, "a parameter type must be `Int`, `Uarch`, a record/union type, a type variable (`'T`), or a pointer type (e.g. `*[Str]`)");
}

/* param = type var_name  (typed; M0 params always carry their type). */
static void parse_param(Parser *p, Param *out) {
	parse_param_type(p, out);
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
	if (t->kind == TK_STR) {
		/* A `${…}` interpolation builds a value at runtime — deferred in M0 (the
		 * `"${s}"` string-copy path lands with runtime interpolation). Interpolation is
		 * currently meaningful only inside an `asm` body, handled where that is parsed. */
		if (t->has_interp)
			die(t->line, "runtime string interpolation is not supported yet");
		advance(p);
		Expr *e = new_expr(EX_STR);
		e->line = t->line;
		e->sval = t->sval;
		e->slen = t->slen;
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
		if (is_ident(t, "match"))
			die(t->line, "a `match` expression must stand alone or be parenthesized "
			             "(e.g. `1 + (match x { ... })`)");
		if (t->text[t->len - 1] == '!')
			die(t->line, "M0 does not support `!` in a name here");
		int line = t->line;
		advance(p); /* `t` still points at the name token (stable in the array) */
		if (peek(p)->kind == TK_LBRACKET || peek(p)->kind == TK_LPAREN) {
			Expr *e = new_expr(EX_CALL);
			e->line = line;
			tok_copy(t, e->name, sizeof e->name);
			/* Optional explicit type arguments `f[Int, Point](…)`. A type argument is a
			 * type name (Int/Uarch/Str/record/union) or, when the caller is itself
			 * generic, one of its type variables (`'T`, substituted at specialization). */
			if (peek(p)->kind == TK_LBRACKET) {
				advance(p); /* [ */
				for (;;) {
					Token *ta = peek(p);
					if (!is_tyvar(ta) && !is_type_ident(ta))
						die(ta->line, "expected a type argument (a type name or `'T`)");
					if (ta->text[ta->len - 1] == '!')
						die(ta->line, "M0 does not support `!` in a type name");
					if (e->ntypeargs == MAX_TYPARAMS)
						die(ta->line, "too many type arguments");
					tok_copy(ta, e->typeargs[e->ntypeargs++], sizeof e->typeargs[0]);
					advance(p);
					if (peek(p)->kind == TK_COMMA) {
						advance(p);
						continue;
					}
					break;
				}
				expect(p, TK_RBRACKET, "expected `]` to close the type arguments");
			}
			expect(p, TK_LPAREN, "expected `(` for the call arguments");
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
		/* A match-arm payload binding (in scope during the arm body's parse) resolves
		 * too — its type and storage are settled in typecheck. */
		if (resolve_name(fn, e->name, &ty) == R_NONE && find_active_bind(fn, e->name) < 0)
			die(line, "unknown name (M0 expressions use integers, parameters, locals, and calls)");
		/* A record value is legal here only as the base of a field access; a
		 * pointer never. Both are caught in the typecheck pass, which knows the
		 * surrounding context, so parse just records the reference. */
		return e;
	}
	if (t->kind == TK_IDENT && is_type_ident(t)) {
		/* A PascalCase name in value position is a union member value `Union.Member` or
		 * `Union[Args].Member` (G3b: a generic union is applied before the member is
		 * selected). A bare type name is not itself a value. */
		if (t->text[t->len - 1] == '!')
			die(t->line, "M0 does not support `!` in a type name");
		Expr *e = new_expr(EX_UMEMBER);
		e->line = t->line;
		parse_type_arg(p, e->name, sizeof e->name); /* union type name (mangled if generic) */
		if (peek(p)->kind != TK_DOT)
			die(t->line, "a bare type name is not a value (a union value is written `Union.Member`)");
		advance(p); /* . */
		Token *mt = peek(p);
		if (!is_type_ident(mt))
			die(mt->line, "expected a PascalCase member name after `.`");
		tok_copy(mt, e->mem, sizeof e->mem);
		advance(p);
		/* An optional payload: `Union.Member(arg, …)` — construction is application. */
		if (peek(p)->kind == TK_LPAREN) {
			advance(p); /* ( */
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
			expect(p, TK_RPAREN, "expected `)` to close the payload");
		}
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

/* match_expr = "match" expr "{" match_arm { "," match_arm } [ "," ] "}"
 * match_arm  = ( "_" | or_pattern ) "->" expr
 * or_pattern = member { "|" member }      member = Union "." Member
 * An expression, like `if` (arms yield values that unify). M1 arms are tag-only: a `_`
 * wildcard, or an or-pattern of one-or-more members of the scrutinee's union, each
 * qualified (`Color.Red`) — no payload sub-pattern, no literal/binding patterns yet.
 * Interior newlines allowed so arms may span lines. Exhaustiveness + arm typing are
 * checked in the typecheck pass. */
static Expr *parse_match(Parser *p, Func *fn) {
	Token *kw = peek(p);
	advance(p); /* `match` */
	Expr *e = new_expr(EX_MATCH);
	e->line = kw->line;
	e->lhs = parse_expr(p, fn); /* scrutinee */
	expect(p, TK_LBRACE, "expected `{` to open the match arms");
	skip_newlines(p);
	int cap = 0;
	while (peek(p)->kind != TK_RBRACE) {
		MatchArm arm;
		memset(&arm, 0, sizeof arm);
		Token *pt = peek(p);
		arm.line = pt->line;
		if (is_ident(pt, "_")) {
			arm.is_wild = 1;
			advance(p);
			if (peek(p)->kind == TK_PIPE)
				die(peek(p)->line, "`_` is a standalone wildcard, not an or-pattern alternative");
		} else if (is_type_ident(pt)) {
			/* A member arm: `Union.Member`, then either a payload sub-pattern `(binds)`
			 * (a single-member binding arm) or an or-pattern `| Union.Member …` (no
			 * binds), all alternatives qualified by the scrutinee's union. */
			for (;;) {
				Token *qt = peek(p);
				if (!is_type_ident(qt))
					die(qt->line, "expected a `Union.Member` pattern");
				char q[64];
				tok_copy(qt, q, sizeof q);
				advance(p);
				expect(p, TK_DOT, "a match arm names a member qualified by its union (`Union.Member`)");
				Token *mt = peek(p);
				if (!is_type_ident(mt))
					die(mt->line, "expected a PascalCase member name after `.`");
				if (arm.nalts == 0)
					snprintf(arm.qual, sizeof arm.qual, "%s", q);
				else if (strcmp(q, arm.qual) != 0)
					die(qt->line, "or-pattern alternatives must all be qualified by the same union");
				if (arm.nalts == MAX_ARM_ALTS)
					die(mt->line, "too many or-pattern alternatives");
				tok_copy(mt, arm.members[arm.nalts], sizeof arm.members[0]);
				arm.nalts++;
				advance(p);
				if (peek(p)->kind == TK_LPAREN) {
					/* Payload sub-pattern: bind lowercase names (or `_`) positionally.
					 * Only on a single member — an or-pattern arm cannot bind. */
					if (arm.nalts > 1)
						die(peek(p)->line, "a payload sub-pattern binds only on a single member, not an or-pattern");
					advance(p); /* ( */
					if (peek(p)->kind == TK_RPAREN)
						die(peek(p)->line, "an empty payload pattern `()` — omit the parens to match the tag only");
					for (;;) {
						Token *bt = peek(p);
						if (!is_ident(bt, "_") && (bt->kind != TK_IDENT || is_type_ident(bt)))
							die(bt->line, "a payload pattern binds lowercase names or `_` (literal/nested patterns are a later brick)");
						if (bt->text[bt->len - 1] == '!')
							die(bt->line, "M0 does not support `!` in a binding name");
						if (arm.nbinds == MAX_ARM_ALTS)
							die(bt->line, "too many payload bindings");
						tok_copy(bt, arm.binds[arm.nbinds++], sizeof arm.binds[0]);
						advance(p);
						if (peek(p)->kind == TK_COMMA) {
							advance(p);
							continue;
						}
						break;
					}
					expect(p, TK_RPAREN, "expected `)` to close the payload pattern");
					break; /* binding arm — no or-pattern alternatives */
				}
				if (peek(p)->kind == TK_PIPE) {
					advance(p);
					continue;
				}
				break;
			}
		} else {
			die(pt->line, "a match arm is `Union.Member` (or `A | B`) or `_` (M1: no literal/binding patterns yet)");
		}
		expect(p, TK_ARROW, "expected `->`");
		/* Put the arm's payload bindings in scope for the body's parse (parse resolves
		 * variable names eagerly); ids are assigned in typecheck. Pop after the body. */
		int saved_pb = fn->nabinds;
		for (int b = 0; b < arm.nbinds; b++) {
			if (strcmp(arm.binds[b], "_") == 0)
				continue;
			if (fn->nabinds == (int)(sizeof fn->abinds / sizeof fn->abinds[0]))
				die(arm.line, "too many active payload bindings");
			snprintf(fn->abinds[fn->nabinds].name, sizeof fn->abinds[0].name, "%s", arm.binds[b]);
			fn->abinds[fn->nabinds].id = 0; /* placeholder; typecheck assigns the real id */
			fn->nabinds++;
		}
		arm.body = parse_expr(p, fn);
		fn->nabinds = saved_pb;
		if (e->narms == cap) {
			cap = cap ? cap * 2 : 4;
			e->arms = realloc(e->arms, cap * sizeof *e->arms);
			if (!e->arms)
				die(0, "out of memory");
		}
		e->arms[e->narms++] = arm;
		skip_newlines(p);
		if (peek(p)->kind == TK_COMMA) {
			advance(p);
			skip_newlines(p);
			continue;
		}
		break;
	}
	expect(p, TK_RBRACE, "expected `}` to close the match arms");
	if (e->narms == 0)
		die(e->line, "a match needs at least one arm");
	return e;
}

static Expr *parse_expr(Parser *p, Func *fn) {
	if (is_ident(peek(p), "if"))
		return parse_if(p, fn);
	if (is_ident(peek(p), "match"))
		return parse_match(p, fn);
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

static Stmt *parse_stmt_seq(Parser *p, Func *fn, int require_return, int open_line);

/* statement  = local_decl | assign | return_stmt | loop | break | continue
 *            | "if" expr "then" ("break" | "continue")
 * local_decl = ("const" | "let") [ type ] var_name "=" (expr | data_literal)
 * assign     = var_name "=" expr | var_name "." var_name "=" expr
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
		int is_record = 0, is_str = 0, is_buf = 0, bufsize = 0;
		char rectype[64] = {0};
		Token *tt = peek(p);
		if (tt->kind == TK_STAR)
			die(tt->line, "M0 locals are `Int`, `Str`, a `[N Uint8]` buffer, or a record type, not a pointer");
		if (tt->kind == TK_LBRACKET) {
			/* `[N Uint8]` — a fixed byte buffer: N arena bytes, no initializer, the
			 * writable target `read` fills. Throwaway (no indexing/bounds); cf0's
			 * `[N Uint8]` is a real fixed array. */
			advance(p); /* [ */
			Token *nt = peek(p);
			if (nt->kind != TK_INT || nt->ival <= 0)
				die(nt->line, "a byte buffer needs a positive comptime length, e.g. `[16 Uint8]`");
			if (nt->ival > INT32_MAX)
				die(nt->line, "byte buffer too large");
			bufsize = (int)nt->ival;
			advance(p);
			if (!is_ident(peek(p), "Uint8"))
				die(peek(p)->line, "M0 byte buffers hold `Uint8` (e.g. `[16 Uint8]`)");
			advance(p);
			expect(p, TK_RBRACKET, "expected `]` to close the `[N Uint8]` buffer type");
			is_buf = 1;
		} else if (is_type_ident(tt)) {
			if (is_ident(tt, "Int")) {
				advance(p);
			} else if (is_ident(tt, "Str")) {
				is_str = 1;
				advance(p);
			} else if (is_ident(tt, "Uarch")) {
				die(tt->line, "M0 has no `Uarch` locals (Uarch is a parameter/return type)");
			} else { /* a record/union type, or a generic application `Box[Int]` (G3b) */
				parse_type_arg(p, rectype, sizeof rectype);
				is_record = 1;
			}
		}
		Token *name = peek(p);
		if (name->kind != TK_IDENT || is_type_ident(name))
			die(name->line, "expected a variable name");
		if (name->text[name->len - 1] == '!')
			die(name->line, "M0 does not support `!` in a variable name");
		Stmt *s = new_stmt(ST_LOCAL);
		s->line = name->line;
		tok_copy(name, s->name, sizeof s->name);
		advance(p);
		Type ty;
		if (resolve_name(fn, s->name, &ty) != R_NONE)
			die(name->line, "name already defined (no shadowing in M0)");
		if (is_buf) {
			/* A byte buffer has no initializer: `let [N Uint8] name`. Its N arena bytes
			 * are allocated at emit; the local names the base `*[Uint8]` pointer. */
			if (peek(p)->kind == TK_EQ)
				die(peek(p)->line, "a `[N Uint8]` buffer has no initializer (`read` fills it)");
			s->bufsize = bufsize;
			Type bt = {TY_BUF, NULL, NULL};
			func_add_local(fn, s->name, mutable, bt, "");
			return s;
		}
		expect(p, TK_EQ, "expected `=`");
		if (is_record) {
			/* A record local's initializer is a data literal (`{ … }`) or a
			 * record-valued expression such as a call that returns this record
			 * (`mk(…)`). Binding from another record variable is not allowed — that is
			 * an aggregate copy, which needs an explicit `copy` (memory_model §6). A
			 * `let` record's fields may later be mutated. Its rec is bound in
			 * typecheck; the annotation is kept in type_name to check the initializer. */
			snprintf(s->type_name, sizeof s->type_name, "%s", rectype);
			if (peek(p)->kind == TK_LBRACE)
				s->expr = parse_data_literal(p, fn, rectype, name->line);
			else
				s->expr = parse_expr(p, fn);
			Type rt = {TY_RECORD, NULL, NULL};
			func_add_local(fn, s->name, mutable, rt, rectype);
		} else if (is_str) {
			/* `const Str name = "literal"` — a Str local binds a string literal only.
			 * No `let Str` (M0 has no Str reassignment) and no `const Str t = other`
			 * (an aggregate copy: memory_model §6 wants an explicit copy — and the owner's
			 * rule is that a string copy is `const Str t = "${s}"`, deferred with runtime
			 * interpolation). Marked with type_name "Str" for typecheck/emit. */
			if (mutable)
				die(name->line, "a Str local must be `const` (M0 has no Str reassignment)");
			s->expr = parse_expr(p, fn);
			if (s->expr->kind != EX_STR)
				die(name->line, "a Str local binds a string literal only (M0 has no Str copy or interpolation yet)");
			snprintf(s->type_name, sizeof s->type_name, "Str");
			Type st = {TY_STR, NULL, NULL};
			func_add_local(fn, s->name, mutable, st, "Str");
		} else {
			/* A `{` initializer with no type annotation is an attempted record
			 * literal (M0 requires the annotation to know the record's type). */
			if (peek(p)->kind == TK_LBRACE)
				die(peek(p)->line,
				    "a record binding needs a type annotation, e.g. `const Point p = { x: 1 }`");
			s->expr = parse_expr(p, fn); /* initializer: name not yet in scope */
			Type it = {TY_INT, NULL, NULL};
			func_add_local(fn, s->name, mutable, it, "");
		}
		return s;
	}
	if (is_ident(t, "return")) {
		advance(p);
		Stmt *s = new_stmt(ST_RETURN);
		s->line = t->line;
		/* Returning a bare record literal is not lowered yet — bind it to a local
		 * first, then return the local. */
		if (peek(p)->kind == TK_LBRACE)
			die(peek(p)->line,
			    "M0 cannot return a record literal directly (bind it to a local, then return it)");
		s->expr = parse_expr(p, fn);
		*saw_return = 1;
		return s;
	}
	if (is_ident(t, "loop")) {
		/* loop { body } — an infinite loop statement (M0 loops don't yield a value;
		 * that needs `<-`). No label. break/continue steer it. */
		advance(p);
		if (p->loop_depth >= MAX_LOOP_DEPTH)
			die(t->line, "loops nested too deep");
		expect(p, TK_LBRACE, "expected `{` (a loop body is a block)");
		Stmt *s = new_stmt(ST_LOOP);
		s->line = t->line;
		p->loop_depth++;
		s->body = parse_stmt_seq(p, fn, 0, t->line); /* no required `return` */
		p->loop_depth--;
		return s;
	}
	if (is_ident(t, "break") || is_ident(t, "continue")) {
		int is_break = is_ident(t, "break");
		advance(p);
		if (p->loop_depth == 0)
			die(t->line, is_break ? "`break` is only valid inside a loop"
			                      : "`continue` is only valid inside a loop");
		Stmt *s = new_stmt(is_break ? ST_BREAK : ST_CONTINUE);
		s->line = t->line; /* bare — no guard, no label in M0 */
		return s;
	}
	if (is_ident(t, "if")) {
		/* In statement position `if` guards a loop control: `if <cond> then break`
		 * or `if <cond> then continue` (the value-`if` is an expression — it appears
		 * on a binding/return right-hand side, never as a bare statement). */
		advance(p);
		Expr *cond = parse_expr(p, fn);
		if (!is_ident(peek(p), "then"))
			die(peek(p)->line, "expected `then`");
		advance(p);
		Token *ctl = peek(p);
		int is_break = is_ident(ctl, "break");
		if (!is_break && !is_ident(ctl, "continue"))
			die(ctl->line, "a statement-position `if` guards a `break` or `continue`");
		advance(p);
		if (p->loop_depth == 0)
			die(ctl->line, "`break`/`continue` is only valid inside a loop");
		Stmt *s = new_stmt(is_break ? ST_BREAK : ST_CONTINUE);
		s->line = t->line;
		s->expr = cond; /* guarded */
		return s;
	}
	/* Otherwise a bare name leads an assignment: `name = expr` (reassign a `let`
	 * word local) or `name.field = expr` (mutate a `let` record's field). */
	if (t->kind == TK_IDENT && !is_type_ident(t)) {
		if (t->text[t->len - 1] == '!')
			die(t->line, "M0 does not support `!` in a name here");
		/* A name immediately followed by `(` — or `[` for a generic call `f[Int](…)` —
		 * is a call statement, invoked for its effect (e.g. `write(...)`), its result
		 * discarded. Parsed as a full expression (which builds the EX_CALL). */
		if (p->toks[p->pos + 1].kind == TK_LPAREN || p->toks[p->pos + 1].kind == TK_LBRACKET) {
			Stmt *s = new_stmt(ST_EXPR);
			s->line = t->line;
			s->expr = parse_expr(p, fn);
			return s;
		}
		char target[64];
		tok_copy(t, target, sizeof target);
		advance(p);
		if (peek(p)->kind == TK_DOT) {
			/* Field mutation: `name.field = expr`. The target must be a mutable
			 * record; the field and offset are resolved in typecheck. */
			advance(p);
			Token *f = peek(p);
			if (f->kind != TK_IDENT || is_type_ident(f))
				die(f->line, "expected a field name after `.`");
			if (f->text[f->len - 1] == '!')
				die(f->line, "M0 does not support `!` in a field name");
			Stmt *s = new_stmt(ST_FIELD_ASSIGN);
			s->line = t->line;
			snprintf(s->name, sizeof s->name, "%s", target);
			tok_copy(f, s->field, sizeof s->field);
			advance(p);
			expect(p, TK_EQ, "expected `=`");
			s->expr = parse_expr(p, fn);
			return s;
		}
		Stmt *s = new_stmt(ST_ASSIGN);
		snprintf(s->name, sizeof s->name, "%s", target);
		expect(p, TK_EQ, "expected `=` (M0 statements are const/let, a reassignment, or return)");
		Type ty;
		switch (resolve_name(fn, s->name, &ty)) {
		case R_NONE: die(t->line, "unknown name (assign to a declared `let` local)");
		case R_PARAM: die(t->line, "cannot reassign a parameter");
		case R_CONST: die(t->line, "cannot reassign a `const` binding (declare it with `let`)");
		case R_LET: break; /* ok */
		}
		/* A whole-record `let` cannot be reassigned as a unit (only a word can);
		 * mutate its fields with `.`. (Aggregate copy-binding is a later concern —
		 * memory_model §6 requires an explicit copy.) */
		if (ty.kind != TY_INT)
			die(t->line, "cannot assign to a whole record (mutate a field with `.`)");
		s->expr = parse_expr(p, fn);
		return s;
	}
	die(t->line, "expected `const`, `let`, `return`, or an assignment");
	return NULL; /* unreachable; die() exits */
}

/* Parse a brace-delimited statement sequence — the `{` already consumed — up to
 * and consuming the matching `}`. One statement per line; no statement may follow
 * a diverging one (`return`, or a bare `break`/`continue`). A function body must
 * end with `return` (require_return); a loop body has no such requirement. */
static Stmt *parse_stmt_seq(Parser *p, Func *fn, int require_return, int open_line) {
	skip_newlines(p);
	Stmt *head = NULL, *tail = NULL;
	int saw_return = 0;
	while (peek(p)->kind != TK_RBRACE) {
		if (peek(p)->kind == TK_EOF)
			die(peek(p)->line, "unterminated block (expected `}`)");
		if (tail && stmt_is_terminal(tail))
			die(peek(p)->line, "unreachable statement after a terminating statement");
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
	if (require_return && !saw_return)
		die(open_line, "a function's block must end with `return`");
	return head;
}

/* body = expr | "{" stmt_seq "}"   (a function body must return). */
static Stmt *parse_body(Parser *p, Func *fn) {
	Token *b = peek(p);
	if (b->kind != TK_LBRACE) {
		/* A single-expression body is exactly `return <expr>`. */
		Stmt *s = new_stmt(ST_RETURN);
		s->expr = parse_expr(p, fn);
		return s;
	}
	advance(p); /* consume `{` */
	return parse_stmt_seq(p, fn, 1, b->line);
}

/* Parse an optional generic bound preceding a type variable in a `generic_param`:
 * `Union 'T` (ebnf `generic_param = type_var | type , type_var`). A leading PascalCase
 * name is the bound; a leading tick means unbounded. Writes the bound name into `bound`
 * ("" when unbounded). The bound must name a *union* (type_system §8.5), checked later
 * against the whole program by check_bound_satisfied — here we only lex the name.
 * ⚠ cfcc restricts the bound `type` to a bare union NAME; cf0 admits any `type` (incl. a
 *   generic union application) before the tick. */
static void parse_optional_bound(Parser *p, char *bound, size_t cap) {
	bound[0] = '\0';
	if (is_type_ident(peek(p))) { /* a PascalCase name before the tick — a bound */
		tok_copy(peek(p), bound, cap);
		advance(p);
	}
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
	/* Functions emit as their bare name ($name → darwin _name), so a user name may
	 * not collide with a runtime symbol: `start` (the `.globl _start` entry) or the
	 * arena runtime's `cf_alloc`/`cf_top`/`cf_limit`/`cf_oom`/`cf_mmap_fail`. (A user
	 * function sharing a libSystem name is harmless while the binary is freestanding
	 * and references no libc symbol; the full compiler's name-mangling arc removes
	 * the whole hazard.) */
	if (strcmp(fn->name, "start") == 0 || strcmp(fn->name, "cf_alloc") == 0 ||
	    strcmp(fn->name, "cf_top") == 0 || strcmp(fn->name, "cf_limit") == 0 ||
	    strcmp(fn->name, "cf_oom") == 0 || strcmp(fn->name, "cf_mmap_fail") == 0)
		die(name->line, "that name is reserved for the runtime");
	if (strcmp(fn->name, "asm") == 0)
		die(name->line, "`asm` is a reserved keyword");
	if (prog_find_func(prog, fn->name))
		die(name->line, "function already defined");
	advance(p);
	expect(p, TK_EQ, "expected `=`");

	/* Optional generic type parameters: `['T, 'U]` before the value parameters. A
	 * function with type parameters is a generic template (monomorphized per use). */
	if (peek(p)->kind == TK_LBRACKET) {
		advance(p); /* [ */
		for (;;) {
			char bound[64];
			parse_optional_bound(p, bound, sizeof bound); /* optional `Union 'T` bound */
			Token *tv = peek(p);
			if (!is_tyvar(tv))
				die(tv->line, bound[0] ? "expected a type variable (e.g. `'T`) after the bound"
				                       : "expected a type variable (e.g. `'T`) in the generic parameter list");
			if (fn->ntyparams == MAX_TYPARAMS)
				die(tv->line, "too many type parameters");
			tok_copy(tv, fn->typarams[fn->ntyparams], sizeof fn->typarams[0]);
			snprintf(fn->bounds[fn->ntyparams], sizeof fn->bounds[0], "%s", bound);
			for (int j = 0; j < fn->ntyparams; j++)
				if (strcmp(fn->typarams[j], fn->typarams[fn->ntyparams]) == 0)
					die(tv->line, "duplicate type parameter");
			fn->ntyparams++;
			advance(p);
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
		expect(p, TK_RBRACKET, "expected `]` to close the type parameters");
	}

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

	/* Optional return type: `Int` (a word) or a record type (returned by pointer;
	 * resolved to a decl in typecheck). A pointer return type has no M0 use. */
	Token *rt = peek(p);
	int has_ret = 0; /* whether a return type was written (mandatory for an asm fn) */
	if (rt->kind == TK_STAR)
		die(rt->line, "M0 functions return `Int` or a record, not a pointer");
	if (is_tyvar(rt)) { /* a generic return type `'T` — resolved at specialization */
		has_ret = 1;
		tok_copy(rt, fn->ret_type_name, sizeof fn->ret_type_name);
		fn->ret_line = rt->line;
		advance(p);
	} else if (is_type_ident(rt)) {
		has_ret = 1;
		if (is_ident(rt, "Int")) {
			advance(p);
		} else { /* Uarch, a record/union type, or a generic application `Box[Int]` (G3b) */
			fn->ret_line = rt->line;
			parse_type_arg(p, fn->ret_type_name, sizeof fn->ret_type_name);
		}
	}
	expect(p, TK_ARROW, "expected `->`");
	/* `asm` right after `->` opens an asm-bodied function (ebnf § Assembly): a naked
	 * function whose verbatim assembly is the floor beneath the compiler (syscalls
	 * etc.). The body is an ordinary string (multiline, `${param}` interpolation);
	 * it is emitted straight to the .s, never lowered through QBE.
	 *
	 * cfcc narrowings (throwaway; cf0.cf must not inherit — it takes the full § Assembly
	 * surface): only `${param}` interpolates (the spec's `${CONST}` comptime-constant
	 * form is unsupported — no comptime constants yet); the `${param}`→register mapping
	 * is arm64-only and always names the 64-bit `x<i>` (cf0 needs per-target selection
	 * and per-type width); there is no `Void` return type (an effect-only asm fn must
	 * borrow `Int`); and `asm` is reserved only as a function name / after `->`, not
	 * globally. The return type IS required, matching the spec (no C! body to infer). */
	if (is_ident(peek(p), "asm")) {
		advance(p);
		Token *body = peek(p);
		if (body->kind != TK_STR)
			die(body->line, "expected a string after `asm`");
		if (!has_ret)
			die(name->line, "an asm function must declare its return type");
		if (fn->nparams > 8)
			die(name->line, "an asm function takes at most 8 parameters (the arm64 arg registers)");
		if (fn->ntyparams > 0)
			die(name->line, "an asm function cannot be generic");
		fn->is_asm = 1;
		fn->asm_body = body;
		advance(p);
		return fn;
	}
	/* Every type variable used in a signature — a bare `'T` param/return or one nested in a
	 * generic application like `Box['T]` — must be a declared type parameter. */
	for (int i = 0; i < fn->nparams; i++)
		check_tyvars_declared(fn->params[i].type_name, fn->typarams, fn->ntyparams, fn->params[i].line);
	check_tyvars_declared(fn->ret_type_name, fn->typarams, fn->ntyparams, fn->ret_line);
	fn->body = parse_body(p, fn);
	return fn;
}

/* True if `s` is a non-empty run of decimal digits — a mangled-name arity marker. */
static int is_all_digits(const char *s) {
	if (!*s)
		return 0;
	for (const char *c = s; *c; c++)
		if (!isdigit((unsigned char)*c))
			return 0;
	return 1;
}

/* Parse one type reference (a field/payload/param/return/local aggregate type, or a
 * type argument inside `[…]`) into its canonical mangled name in `out` (G3b):
 *   - a leaf — `Int`, a record/union type name, or a type variable `'T` — verbatim;
 *   - a generic application `Name[a, b, …]` mangled arity-prefixed as
 *     `Name "." nargs { "." <mangled arg> }`, e.g. `Box[Int]`→`Box.1.Int`,
 *     `Pair[Box[Int], 'B]`→`Pair.2.Box.1.Int.'B`.
 * The scheme is unambiguous — type names are PascalCase or `'T`, arity markers are digits
 * — so instantiate_type decodes it. `'T` variables are validated against the enclosing
 * declaration's type parameters once the whole declaration is parsed. */
static void parse_type_arg(Parser *p, char *out, size_t cap) {
	Token *t = peek(p);
	if (is_tyvar(t)) { /* a type variable `'T` */
		tok_copy(t, out, cap);
		advance(p);
		return;
	}
	if (!is_type_ident(t))
		die(t->line, "expected a type (a type name or `'T`)");
	if (t->text[t->len - 1] == '!')
		die(t->line, "M0 does not support `!` in a type name");
	char base[64];
	tok_copy(t, base, sizeof base);
	advance(p);
	if (peek(p)->kind != TK_LBRACKET) { /* a leaf type */
		if ((size_t)snprintf(out, cap, "%s", base) >= cap)
			die(t->line, "type name too long");
		return;
	}
	advance(p); /* [ */
	char args[MAX_TYPARAMS][256];
	int n = 0;
	for (;;) {
		if (n == MAX_TYPARAMS)
			die(peek(p)->line, "too many type arguments");
		parse_type_arg(p, args[n++], sizeof args[0]);
		if (peek(p)->kind == TK_COMMA) {
			advance(p);
			continue;
		}
		break;
	}
	expect(p, TK_RBRACKET, "expected `]` to close the type arguments");
	int off = snprintf(out, cap, "%s.%d", base, n);
	if (off < 0 || (size_t)off >= cap)
		die(t->line, "type name too long");
	for (int k = 0; k < n; k++) {
		int w = snprintf(out + off, cap - (size_t)off, ".%s", args[k]);
		if (w < 0 || (size_t)(off + w) >= cap)
			die(t->line, "type name too long");
		off += w;
	}
}

/* Parse a field/payload type into `out`: `Int`, an aggregate (record/union) type, a type
 * variable `'T`, or a generic application `Box[Int]` (all via parse_type_arg). Pointers,
 * buffers, Uarch and Str are not field/payload types. The name is resolved (and any `'T`
 * validated) later, so forward and mutually-recursive references work. */
static void parse_member_type(Parser *p, char *out, size_t cap) {
	Token *t = peek(p);
	if (t->kind == TK_STAR)
		die(t->line, "a field/payload type is `Int`, an aggregate, or `'T`, not a pointer");
	if (t->kind == TK_LBRACKET)
		die(t->line, "a field/payload type is `Int`, an aggregate, or `'T`, not an array/buffer");
	parse_type_arg(p, out, cap);
	if (strcmp(out, "Uarch") == 0 || strcmp(out, "Str") == 0)
		die(t->line, "a field/payload type is `Int`, an aggregate, or `'T` (not Uarch/Str)");
}

/* Parse an optional generic type-parameter list `['A, Union 'B, …]` into
 * typarams/bounds/ntyparams. Shared by `data` and `union` declarations (functions inline
 * their own copy). A `generic_param` is a bare `'T` (unbounded) or `Union 'T` (bounded).
 * ⚠ cf0 must NOT inherit two narrowings here (same family as the function-generics ones):
 * (1) a bound is a bare union NAME only — ebnf's `generic_param` admits any `type` before
 *     the tick (incl. a generic union application), which cf0 restores (type_system §8.5);
 * (2) `MAX_TYPARAMS` is a throwaway fixed cap, not a language limit. */
static void parse_typaram_list(Parser *p, char typarams[][64], char bounds[][64], int *ntyparams) {
	*ntyparams = 0;
	if (peek(p)->kind != TK_LBRACKET)
		return;
	advance(p); /* [ */
	for (;;) {
		char bound[64];
		parse_optional_bound(p, bound, sizeof bound); /* optional `Union 'T` bound */
		Token *tv = peek(p);
		if (!is_tyvar(tv))
			die(tv->line, bound[0] ? "expected a type variable (e.g. `'T`) after the bound"
			                       : "expected a type variable (e.g. `'T`) in the generic parameter list");
		if (*ntyparams == MAX_TYPARAMS)
			die(tv->line, "too many type parameters");
		tok_copy(tv, typarams[*ntyparams], 64);
		snprintf(bounds[*ntyparams], 64, "%s", bound);
		for (int j = 0; j < *ntyparams; j++)
			if (strcmp(typarams[j], typarams[*ntyparams]) == 0)
				die(tv->line, "duplicate type parameter");
		(*ntyparams)++;
		advance(p);
		if (peek(p)->kind == TK_COMMA) {
			advance(p);
			continue;
		}
		break;
	}
	expect(p, TK_RBRACKET, "expected `]` to close the type parameters");
}

/* Validate that every type variable (a `'T` element) in a mangled type string is declared
 * among `typarams` — catches a stray `'T` in a non-generic declaration or an undeclared one. */
static void check_tyvars_declared(const char *mangled, char typarams[][64], int ntp, int line) {
	char buf[512];
	if ((size_t)snprintf(buf, sizeof buf, "%s", mangled) >= sizeof buf)
		die(line, "type name too long");
	for (char *tok = strtok(buf, "."); tok; tok = strtok(NULL, ".")) {
		if (tok[0] != '\'')
			continue;
		int found = 0;
		for (int i = 0; i < ntp; i++)
			if (strcmp(typarams[i], tok) == 0) {
				found = 1;
				break;
			}
		if (!found)
			die(line, "unknown type variable (not in the declaration's type parameters)");
	}
}

/* data_decl   = "data" type_name "=" record_body
 * record_body = "{" [ field_decl { "," field_decl } [ "," ] ] "}"
 * field_decl  = member_type var_name
 * A record is a flat set of fields on one line (no generics, spread, defaults, or
 * interior newlines — all later increments), with at least one field so the record is
 * never zero-sized. A field is an `Int` or an aggregate (record/union) type (G3a). */
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
	snprintf(d->base_name, sizeof d->base_name, "%s", d->name);
	advance(p);
	if (prog_find_data(prog, d->name) || prog_find_union(prog, d->name))
		die(nm->line, "type already defined");
	parse_typaram_list(p, d->typarams, d->bounds, &d->ntyparams); /* optional `['A, Union 'B]` (generic data, G3b) */
	expect(p, TK_EQ, "expected `=`");
	if (peek(p)->kind != TK_LBRACE)
		die(peek(p)->line, "M0 `data` must have a record body `{ Int field, ... }`");
	advance(p); /* `{` */
	if (peek(p)->kind != TK_RBRACE)
		for (;;) {
			Token *ty = peek(p);
			if (ty->kind == TK_DOT)
				die(ty->line, "M0 records do not support `...` spread");
			char ftype[64];
			parse_member_type(p, ftype, sizeof ftype);
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
			snprintf(d->field_types[d->nfields], sizeof d->field_types[0], "%s", ftype);
			snprintf(d->fields[d->nfields], sizeof d->fields[0], "%s", fname);
			d->nfields++;
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
	for (int i = 0; i < d->nfields; i++) /* every `'T` field must name a declared type parameter */
		check_tyvars_declared(d->field_types[i], d->typarams, d->ntyparams, nm->line);
	return d;
}

/* union_decl = "union" type_name "=" "{" member { "," member } [ "," ] "}"
 * M1.1 unions are TAG-ONLY: every member is a fresh PascalCase nullary tag, numbered
 * by declaration order (member i → tag i), and the union lowers to a plain integer tag
 * (ebnf Union Types; type_system §8.4; seed_subset §7). Interior newlines are allowed
 * so a union may span lines (the idiomatic AST-node form). Payload members (`M(T)`,
 * `M = { … }`, `M = literal`), compose-over/spread members, and generics are later
 * bricks — each rejected with a clear message. */
static UnionDecl *parse_union_decl(Parser *p, Program *prog) {
	advance(p); /* `union` */
	Token *nm = peek(p);
	if (!is_type_ident(nm))
		die(nm->line, "expected a PascalCase type name after `union`");
	if (nm->text[nm->len - 1] == '!')
		die(nm->line, "M0 does not support `!` in a type name");
	UnionDecl *u = xmalloc(sizeof *u);
	memset(u, 0, sizeof *u);
	tok_copy(nm, u->name, sizeof u->name);
	snprintf(u->base_name, sizeof u->base_name, "%s", u->name);
	advance(p);
	if (prog_find_union(prog, u->name) || prog_find_data(prog, u->name))
		die(nm->line, "type already defined");
	parse_typaram_list(p, u->typarams, u->bounds, &u->ntyparams); /* optional `['V, Union 'W]` (generic union, G3b) */
	expect(p, TK_EQ, "expected `=`");
	expect(p, TK_LBRACE, "expected `{` (a union body is `{ Member, ... }`)");
	skip_newlines(p);
	if (peek(p)->kind != TK_RBRACE)
		for (;;) {
			Token *m = peek(p);
			if (m->kind == TK_DOT)
				die(m->line, "M1.1 unions do not support `...` member spread");
			if (!is_type_ident(m))
				die(m->line, "a union member is a PascalCase name");
			if (m->text[m->len - 1] == '!')
				die(m->line, "M0 does not support `!` in a member name");
			char mname[64];
			tok_copy(m, mname, sizeof mname);
			advance(p);
			/* Cap the member count BEFORE the payload loop writes payload_types[nmembers][…]
			 * (and arity[nmembers] below), so a 65th member cannot overrun the row array. */
			if (u->nmembers == MAX_UNION_MEMBERS)
				die(m->line, "too many union members");
			/* Optional positional payload: `Member(Int, Point, …)`. No parens = a nullary
			 * tag. Each payload field is an `Int` or an aggregate (record/union) type (G3a).
			 * (Struct-body `= { … }`/literal payloads are later bricks.) */
			int arity = 0;
			if (peek(p)->kind == TK_LPAREN) {
				advance(p); /* ( */
				if (peek(p)->kind == TK_RPAREN)
					die(peek(p)->line, "an empty payload `()` — write a nullary member as just its name");
				for (;;) {
					/* Capped at MAX_ARM_ALTS so any declarable member is bindable in a
					 * match arm (which holds up to MAX_ARM_ALTS binding names). */
					if (arity == MAX_ARM_ALTS)
						die(peek(p)->line, "a union member has at most 16 payload fields (M1)");
					char pt[64];
					parse_member_type(p, pt, sizeof pt);
					snprintf(u->payload_types[u->nmembers][arity], sizeof u->payload_types[0][0], "%s", pt);
					arity++;
					if (peek(p)->kind == TK_COMMA) {
						advance(p);
						continue;
					}
					break;
				}
				expect(p, TK_RPAREN, "expected `)` to close the payload");
			}
			if (peek(p)->kind == TK_EQ)
				die(peek(p)->line, "M1.2a union members are positional-payload or nullary (a `= struct`/literal member is a later brick)");
			/* Compose-over applies only to a BARE member (no payload); a payload member
			 * `Foo(Int)` is a named case, not a compose-over of type `Foo`. */
			if (arity == 0 && (is_builtin_type_name(mname) || prog_find_data(prog, mname) || prog_find_union(prog, mname)))
				die(m->line, "M1.2a does not support compose-over members (a bare member naming an existing type)");
			if (union_member_tag(u, mname) >= 0)
				die(m->line, "duplicate union member");
			u->arity[u->nmembers] = arity;
			snprintf(u->members[u->nmembers++], sizeof u->members[0], "%s", mname);
			skip_newlines(p);
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				skip_newlines(p);
				if (peek(p)->kind == TK_RBRACE) /* trailing comma */
					break;
				continue;
			}
			break;
		}
	expect(p, TK_RBRACE, "expected `}`");
	if (u->nmembers == 0)
		die(nm->line, "a union needs at least one member");
	/* A union with any payload member is a boxed tag+payload aggregate (type_system
	 * §8.4): tag (4) + max member payload. An all-nullary union stays a plain tag. */
	int maxarity = 0;
	for (int i = 0; i < u->nmembers; i++) {
		if (u->arity[i] > 0)
			u->has_payload = 1;
		if (u->arity[i] > maxarity)
			maxarity = u->arity[i];
	}
	u->size = 8 + maxarity * 8;
	for (int i = 0; i < u->nmembers; i++) /* every `'T` payload must name a declared type parameter */
		for (int j = 0; j < u->arity[i]; j++)
			check_tyvars_declared(u->payload_types[i][j], u->typarams, u->ntyparams, nm->line);
	return u;
}

/* module = { declaration } — one per line; exactly one is `pub const main`. A
 * declaration is a `data` record, a `union`, or a `[pub] const` function. */
static void parse(Parser *p, Program *prog) {
	skip_newlines(p);
	while (peek(p)->kind != TK_EOF) {
		if (is_ident(peek(p), "data"))
			prog_add_data(prog, parse_data_decl(p, prog));
		else if (is_ident(peek(p), "union"))
			prog_add_union(prog, parse_union_decl(p, prog));
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
	if (m->ret_type_name[0])
		die(0, "`main` must return Int (its value is the exit code)");
	if (m->is_asm)
		die(0, "`main` cannot be an asm function");
	if (m->ntyparams > 0)
		die(0, "`main` cannot be generic");
}

/* ---------------------------------------------------------- monomorphize - */

/* Generic functions are specialized by whole-program monomorphization (the spec's
 * `specialized` arc). ⚠ TWO throwaway degeneracies cf0.cf must NOT inherit:
 * (1) cfcc has **no generic bounds** and typechecks each specialized clone's body
 *     **per instantiation** — an invalid operation on `'T` surfaces at the *use*
 *     (`inc[Box]`), not the definition. The ratified type system (type_system §8.5/§8.4)
 *     makes a bound a `union` membership (`[U 'T]`), leaves an unbounded `'T`
 *     non-operable (narrow by `match` first), and checks the body **once at the
 *     definition site** — no deferred per-instantiation errors. cf0 does that.
 * (2) The mangled instantiation name is an ad-hoc `name.Arg…` scheme (below), not the
 *     flatten-and-mangle arc; cf0 owns real specialization names. */

/* Deep-copy an expression tree so a generic instantiation gets its own nodes (fresh
 * typecheck/emit annotations). Resolved fields (rtype, callee, slot, …) are NULL/0 in
 * the un-typechecked template, so a plain struct copy carries the right initial state. */
static Expr *clone_expr(Expr *e) {
	if (!e)
		return NULL;
	Expr *c = xmalloc(sizeof *c);
	*c = *e;
	c->lhs = clone_expr(e->lhs);
	c->rhs = clone_expr(e->rhs);
	c->els = clone_expr(e->els);
	if (e->nargs) {
		c->args = xmalloc((size_t)e->nargs * sizeof *c->args);
		for (int i = 0; i < e->nargs; i++)
			c->args[i] = clone_expr(e->args[i]);
	}
	if (e->nfields) { /* EX_RECORD */
		c->fnames = xmalloc((size_t)e->nfields * sizeof *c->fnames);
		memcpy(c->fnames, e->fnames, (size_t)e->nfields * sizeof *c->fnames);
		c->fvals = xmalloc((size_t)e->nfields * sizeof *c->fvals);
		for (int i = 0; i < e->nfields; i++)
			c->fvals[i] = clone_expr(e->fvals[i]);
	}
	c->ford = NULL; /* rebuilt by typecheck */
	if (e->narms) { /* EX_MATCH */
		c->arms = xmalloc((size_t)e->narms * sizeof *c->arms);
		memcpy(c->arms, e->arms, (size_t)e->narms * sizeof *c->arms);
		for (int i = 0; i < e->narms; i++)
			c->arms[i].body = clone_expr(e->arms[i].body);
	}
	if (e->kind == EX_STR && e->sval) {
		c->sval = xmalloc((size_t)e->slen + 1);
		memcpy(c->sval, e->sval, (size_t)e->slen);
		c->sval[e->slen] = '\0';
	}
	return c;
}

static Stmt *clone_stmt(Stmt *s) {
	if (!s)
		return NULL;
	Stmt *c = xmalloc(sizeof *c);
	*c = *s;
	c->expr = clone_expr(s->expr);
	c->body = clone_stmt(s->body);
	c->next = clone_stmt(s->next);
	return c;
}

/* ---- mangled type names (G3b) ------------------------------------------------
 * A generic type application `Box[Int]` is carried as an arity-prefixed mangled name
 * `Box.1.Int` (parse_type_arg). These helpers split, span, and join such names, and
 * substitute a template's type params inside one. */

typedef struct { char *el[128]; int n; } Mangle;

/* Split a mangled name into its '.'-separated elements (copied into `buf`). */
static void mangle_split(const char *name, Mangle *m, char *buf, size_t bufcap) {
	if ((size_t)snprintf(buf, bufcap, "%s", name) >= bufcap)
		die(0, "type name too long");
	m->n = 0;
	for (char *tok = strtok(buf, "."); tok; tok = strtok(NULL, ".")) {
		if (m->n == (int)(sizeof m->el / sizeof m->el[0]))
			die(0, "type name too deeply nested");
		m->el[m->n++] = tok;
	}
}

/* Index just past the one complete mangled type starting at element `start` (a base name,
 * optionally an arity digit and that many nested types). */
static int mangle_span(const Mangle *m, int start) {
	int i = start + 1; /* base name */
	if (i < m->n && is_all_digits(m->el[i])) {
		int arity = atoi(m->el[i]);
		i++;
		for (int k = 0; k < arity; k++)
			i = mangle_span(m, i);
	}
	return i;
}

/* Join elements [a, b) with '.' into `out`. */
static void mangle_join(const Mangle *m, int a, int b, char *out, size_t cap) {
	int off = 0;
	for (int i = a; i < b; i++) {
		int w = snprintf(out + off, cap - (size_t)off, "%s%s", i > a ? "." : "", m->el[i]);
		if (w < 0 || (size_t)(off + w) >= cap)
			die(0, "type name too long");
		off += w;
	}
}

/* Substitute type params with concrete arg names in a mangled type string: an element
 * equal to a type param `'T` becomes the matching `args[k]` (itself possibly a mangled
 * name); other elements (base names, arity digits) pass through unchanged. */
static void subst_mangled(char *dst, size_t cap, const char *src,
                          char typarams[][64], char args[][256], int ntp) {
	char buf[512];
	Mangle m;
	mangle_split(src, &m, buf, sizeof buf);
	int off = 0;
	for (int i = 0; i < m.n; i++) {
		const char *piece = m.el[i];
		for (int k = 0; k < ntp; k++)
			if (strcmp(m.el[i], typarams[k]) == 0) {
				piece = args[k];
				break;
			}
		int w = snprintf(dst + off, cap - (size_t)off, "%s%s", i > 0 ? "." : "", piece);
		if (w < 0 || (size_t)(off + w) >= cap)
			die(0, "type name too long");
		off += w;
	}
}

/* Substitute a template's type variables with the instantiation's concrete type names in a
 * cloned body. A `'T` survives into an expression as an EX_CALL's explicit type arguments
 * (`id['T](x)`) and as the type name of a generic record literal / union member value
 * (`Box['T]` / `Maybe['T].Just`) — all rewritten to the concrete instantiation. */
static void subst_expr(Expr *e, Func *tmpl, char targs[][256]) {
	if (!e)
		return;
	if (e->kind == EX_CALL)
		for (int i = 0; i < e->ntypeargs; i++) {
			int idx = func_typaram_index(tmpl, e->typeargs[i]);
			if (idx >= 0) {
				if (strlen(targs[idx]) >= sizeof e->typeargs[0])
					die(e->line, "type argument name too long");
				snprintf(e->typeargs[i], sizeof e->typeargs[0], "%s", targs[idx]);
			}
		}
	if ((e->kind == EX_RECORD || e->kind == EX_UMEMBER) && strchr(e->name, '\'')) {
		char sub[256];
		subst_mangled(sub, sizeof sub, e->name, tmpl->typarams, targs, tmpl->ntyparams);
		if (strlen(sub) >= sizeof e->name)
			die(e->line, "type name too long");
		snprintf(e->name, sizeof e->name, "%s", sub);
	}
	subst_expr(e->lhs, tmpl, targs);
	subst_expr(e->rhs, tmpl, targs);
	subst_expr(e->els, tmpl, targs);
	for (int i = 0; i < e->nargs; i++)
		subst_expr(e->args[i], tmpl, targs);
	for (int i = 0; i < e->nfields; i++)
		subst_expr(e->fvals[i], tmpl, targs);
	for (int i = 0; i < e->narms; i++)
		subst_expr(e->arms[i].body, tmpl, targs);
}

static void subst_stmts(Stmt *s, Func *tmpl, char targs[][256]) {
	for (; s; s = s->next) {
		if (s->kind == ST_LOCAL && s->type_name[0] && strchr(s->type_name, '\'')) {
			char sub[256];
			subst_mangled(sub, sizeof sub, s->type_name, tmpl->typarams, targs, tmpl->ntyparams);
			if (strlen(sub) >= sizeof s->type_name)
				die(s->line, "type name too long");
			snprintf(s->type_name, sizeof s->type_name, "%s", sub);
		}
		subst_expr(s->expr, tmpl, targs);
		subst_stmts(s->body, tmpl, targs);
	}
}

/* Reclassify a `'T` parameter to the concrete kind of its type argument. A record or
 * union name stays PK_RECORD (resolve_signatures reclassifies a union to PK_UNION). */
static void reclassify_param(Param *p, const char *concrete, int line) {
	if (strcmp(concrete, "Int") == 0) {
		p->kind = PK_WORD;
	} else if (strcmp(concrete, "Uarch") == 0) {
		p->kind = PK_UARCH;
	} else if (strcmp(concrete, "Str") == 0) {
		die(line, "a type argument of `Str` is not supported (M0 has no Str parameters)");
	} else if (concrete[0] == '\'') {
		die(line, "internal: unsubstituted type variable in a type argument");
	} else {
		p->kind = PK_RECORD; /* a record or union name; resolved in resolve_signatures */
		snprintf(p->type_name, sizeof p->type_name, "%s", concrete);
	}
}

/* ---- generic TYPE instantiation (G3b) ----------------------------------------
 * A generic `data`/`union` template (ntyparams > 0) is monomorphized like a generic
 * function: each use `Box[Int]` names a concrete instantiation whose mangled name is the
 * arity-prefixed string parse_type_arg produced (`Box.1.Int`). concretize_name ensures the
 * instantiation exists; instantiate_type clones the template, substitutes its type params
 * in every field/payload type, registers it under the mangled name BEFORE concretizing
 * those fields (so a self-referential generic union — a pointer-recursive `List['T]` — finds
 * itself and terminates), then concretizes each field/payload in turn. ⚠ THROWAWAY, like the
 * function monomorphizer: cf0 owns real specialization names and definition-site checking. The
 * arity-prefixed mangle and the fixed 64-char instance-name cap (which bounds generic nesting
 * depth — an over-long name is a clean error, never an overflow) are genesis-only limits. */
static void concretize_name(Program *prog, char *name, int line); /* forward (mutual) */

/* Enforce a generic bound (type_system §8.5) at instantiation. `bound` is "" (unbounded)
 * or the union named in `[Union 'T]`; `arg` is the concrete type-argument name. A bound
 * must name a declared *union* ("constraints are unions") and — since cfcc has no
 * compose-over unions (no member types) — the only type that reflexively satisfies it is
 * the union itself, so bound satisfaction is `arg == bound`. Consistent with cfcc's
 * per-instantiation checking: an unused template's bound is not checked (its body is not
 * typechecked either). ⚠ cf0 does real union-membership + sub-union subsumption (§8.2);
 * this reflexive-only rule is the disclaimed genesis narrowing.
 * ⚠ cf0 must NOT inherit a second narrowing: cfcc has no std numeric unions (§8.6) —
 *   `Int`/`Uarch`/`Str`/`Uint8` are builtins and `Uint`/`Float`/`Number` are absent — so the
 *   spec's canonical bounds `[Int 'T]`/`[Number 'T]` are NOT valid bounds here (they error
 *   "not a union"). cf0 restores Int/Uint/Float/Number as unions and accepts those bounds. */
static void check_bound_satisfied(Program *prog, const char *bound, const char *arg, int line) {
	if (bound[0] == '\0')
		return; /* unbounded `'T` — any type argument */
	char msg[320];
	if (!prog_find_union(prog, bound)) {
		snprintf(msg, sizeof msg, "a generic bound must name a union type (`%s` is not a union)", bound);
		die(line, msg);
	}
	if (strcmp(arg, bound) != 0) {
		snprintf(msg, sizeof msg, "type argument `%s` does not satisfy the bound `%s`", arg, bound);
		die(line, msg);
	}
}

static void instantiate_type(Program *prog, const char *mangled, int line) {
	if (prog_find_data(prog, mangled) || prog_find_union(prog, mangled))
		return; /* already instantiated (dedup / register-before-concretize recursion guard) */
	char buf[512];
	Mangle m;
	mangle_split(mangled, &m, buf, sizeof buf);
	if (m.n < 2 || !is_all_digits(m.el[1]))
		die(line, "malformed generic type application");
	const char *base = m.el[0];
	int arity = atoi(m.el[1]);
	if (arity > MAX_TYPARAMS)
		die(line, "too many type arguments");
	/* Extract each argument's mangled substring and ensure nested applications exist first. */
	char args[MAX_TYPARAMS][256];
	int idx = 2;
	for (int k = 0; k < arity; k++) {
		int end = mangle_span(&m, idx);
		mangle_join(&m, idx, end, args[k], sizeof args[0]);
		idx = end;
		concretize_name(prog, args[k], line);
	}
	if (idx != m.n) /* the arity's spans must consume the whole name (defensive) */
		die(line, "malformed generic type application");
	DataDecl *dt = prog_find_data(prog, base);
	UnionDecl *ut = prog_find_union(prog, base);
	if (dt && dt->ntyparams > 0) {
		if (arity != dt->ntyparams)
			die(line, "wrong number of type arguments for this generic data type");
		for (int k = 0; k < arity; k++)
			check_bound_satisfied(prog, dt->bounds[k], args[k], line);
		if (strlen(mangled) >= sizeof dt->name)
			die(line, "instantiated type name too long");
		DataDecl *c = xmalloc(sizeof *c);
		*c = *dt; /* copies fields/field_types/base_name by value */
		snprintf(c->name, sizeof c->name, "%s", mangled);
		c->ntyparams = 0;
		prog_add_data(prog, c); /* register BEFORE concretizing fields (recursion guard) */
		for (int i = 0; i < c->nfields; i++) {
			char sub[256];
			subst_mangled(sub, sizeof sub, dt->field_types[i], dt->typarams, args, arity);
			if (strlen(sub) >= sizeof c->field_types[0])
				die(line, "field type name too long");
			snprintf(c->field_types[i], sizeof c->field_types[0], "%s", sub);
			concretize_name(prog, c->field_types[i], line);
		}
	} else if (ut && ut->ntyparams > 0) {
		if (arity != ut->ntyparams)
			die(line, "wrong number of type arguments for this generic union type");
		for (int k = 0; k < arity; k++)
			check_bound_satisfied(prog, ut->bounds[k], args[k], line);
		if (strlen(mangled) >= sizeof ut->name)
			die(line, "instantiated type name too long");
		UnionDecl *c = xmalloc(sizeof *c);
		*c = *ut;
		snprintf(c->name, sizeof c->name, "%s", mangled);
		c->ntyparams = 0;
		prog_add_union(prog, c); /* register BEFORE concretizing payloads (recursion guard) */
		for (int mi = 0; mi < c->nmembers; mi++)
			for (int j = 0; j < c->arity[mi]; j++) {
				char sub[256];
				subst_mangled(sub, sizeof sub, ut->payload_types[mi][j], ut->typarams, args, arity);
				if (strlen(sub) >= sizeof c->payload_types[0][0])
					die(line, "payload type name too long");
				snprintf(c->payload_types[mi][j], sizeof c->payload_types[0][0], "%s", sub);
				concretize_name(prog, c->payload_types[mi][j], line);
			}
	} else if (dt || ut) {
		die(line, "type arguments given to a non-generic type");
	} else {
		die(line, "unknown generic type");
	}
}

/* Ensure a concrete (post-substitution) type name is usable: instantiate a generic
 * application, error on a generic template used without arguments or a stray `'T`; a plain
 * concrete/leaf name is left for resolve_member_type/resolve_signatures to validate. */
static void concretize_name(Program *prog, char *name, int line) {
	if (name[0] == '\0' || strcmp(name, "Int") == 0 ||
	    strcmp(name, "Uarch") == 0 || strcmp(name, "Str") == 0)
		return;
	if (name[0] == '\'')
		die(line, "internal: unsubstituted type variable in a concrete context");
	if (strchr(name, '.')) { /* a generic application */
		instantiate_type(prog, name, line);
		return;
	}
	/* ⚠ cfcc requires EXPLICIT type arguments at every generic-type use, INCLUDING a value
	 * construction (`Maybe[Int].Just(5)`, not `Maybe.Just(5)`) — constructor-argument
	 * inference is deferred. cf0 must NOT inherit this: type_system §8.1/§5.1 infer the member
	 * at the construction site (`Maybe.Just(1) : Maybe[Arch].Just`), and seed_subset §4 keeps
	 * generics-with-inference in full. cfcc infers args for a function CALL (shallow path) but
	 * demands them for a construction — a genesis narrowing that rejects, never miscompiles. */
	DataDecl *d = prog_find_data(prog, name);
	if (d && d->ntyparams > 0)
		die(line, "generic data type used without type arguments (write `Name[Int]`)");
	UnionDecl *u = prog_find_union(prog, name);
	if (u && u->ntyparams > 0)
		die(line, "generic union type used without type arguments (write `Name[Int]`)");
}

/* Concretize a concrete function's parameter and return type applications. */
static void concretize_signature(Program *prog, Func *fn) {
	for (int i = 0; i < fn->nparams; i++)
		if (fn->params[i].kind == PK_RECORD)
			concretize_name(prog, fn->params[i].type_name, fn->params[i].line);
	if (fn->ret_type_name[0])
		concretize_name(prog, fn->ret_type_name, fn->ret_line);
}

/* Create (or reuse) the specialization of a generic template for a concrete type-arg
 * tuple: clone it, substitute `'T`, mangle the name (`id.Int`), and add it to the
 * program. The clone is a concrete function typechecked/emitted like any other. */
static Func *instantiate(Program *prog, Func *tmpl, char typeargs[][64], int nargs, int line) {
	if (nargs != tmpl->ntyparams)
		die(line, "wrong number of type arguments for this generic function");
	for (int i = 0; i < nargs; i++)
		check_bound_satisfied(prog, tmpl->bounds[i], typeargs[i], line);
	/* Mangle `name.Arg.Arg…` — a `.` can't occur in a cfcc identifier, so a clone name
	 * can never collide with a user function (throwaway scheme; see the section note). */
	char mangled[256];
	int n = snprintf(mangled, sizeof mangled, "%s", tmpl->name);
	for (int i = 0; i < nargs; i++) {
		if (n < 0 || (size_t)n >= sizeof mangled) /* guard BEFORE the next write */
			die(line, "instantiation name too long");
		n += snprintf(mangled + n, sizeof mangled - (size_t)n, ".%s", typeargs[i]);
	}
	if (n < 0 || (size_t)n >= sizeof ((Func *)0)->name) /* the mangled name must fit Func.name */
		die(line, "instantiation name too long");
	Func *ex = prog_find_func(prog, mangled);
	if (ex)
		return ex;
	Func *c = new_func();
	*c = *tmpl; /* params array (by value), ret fields, flags */
	snprintf(c->name, sizeof c->name, "%s", mangled);
	c->ntyparams = 0;
	c->is_pub = 0;
	c->nabinds = 0;
	c->next_bind_id = 0;
	c->ret_rec = NULL;
	c->ret_uni = NULL;
	if (tmpl->nlocals) {
		c->locals = xmalloc((size_t)tmpl->cap_locals * sizeof *c->locals);
		memcpy(c->locals, tmpl->locals, (size_t)tmpl->nlocals * sizeof *c->locals);
	} else {
		c->locals = NULL;
		c->cap_locals = 0;
	}
	c->body = clone_stmt(tmpl->body);
	/* Widen the type args to the mangled-name buffer width for substitution. */
	char targs[MAX_TYPARAMS][256];
	for (int i = 0; i < nargs; i++)
		snprintf(targs[i], sizeof targs[0], "%s", typeargs[i]);
	for (int i = 0; i < c->nparams; i++) {
		if (c->params[i].kind == PK_VAR) {
			int idx = func_typaram_index(tmpl, c->params[i].type_name);
			reclassify_param(&c->params[i], typeargs[idx], line);
		} else if (c->params[i].kind == PK_RECORD && strchr(c->params[i].type_name, '\'')) {
			/* a generic-application param like `Box['T]` → substitute its type args */
			char sub[256];
			subst_mangled(sub, sizeof sub, c->params[i].type_name, tmpl->typarams, targs, tmpl->ntyparams);
			if (strlen(sub) >= sizeof c->params[i].type_name)
				die(line, "parameter type name too long");
			snprintf(c->params[i].type_name, sizeof c->params[i].type_name, "%s", sub);
		}
	}
	if (c->ret_type_name[0] && strchr(c->ret_type_name, '\'')) {
		char sub[256];
		subst_mangled(sub, sizeof sub, c->ret_type_name, tmpl->typarams, targs, tmpl->ntyparams);
		if (strcmp(sub, "Int") == 0) /* a bare `'T` return resolving to Int → empty (cfcc convention) */
			sub[0] = '\0';
		if (strlen(sub) >= sizeof c->ret_type_name)
			die(line, "return type name too long");
		snprintf(c->ret_type_name, sizeof c->ret_type_name, "%s", sub);
	}
	subst_stmts(c->body, tmpl, targs);
	prog_add_func(prog, c);
	return c;
}

/* A best-effort type NAME for an argument expression, used to infer a generic's type
 * arguments before typecheck. Returns "Int"/"Uarch"/a record-or-union name, or "" when
 * cfcc can't determine it cheaply (the caller then asks for an explicit type argument).
 * (This is a pre-typecheck shortcut, not the full type system — a wrong guess is caught
 * as a per-instantiation typecheck error.) */
static const char *shallow_type_name(Program *prog, Func *fn, Expr *e) {
	switch (e->kind) {
	case EX_INT:
		return "Int";
	case EX_UMEMBER:
		return e->name; /* the union type name */
	case EX_VAR:
		for (int i = 0; i < fn->nparams; i++)
			if (strcmp(fn->params[i].name, e->name) == 0) {
				switch (fn->params[i].kind) {
				case PK_WORD:   return "Int";
				case PK_UARCH:  return "Uarch";
				case PK_RECORD:
				case PK_UNION:
				case PK_VAR:    return fn->params[i].type_name;
				case PK_LONG:   return ""; /* a pointer — no simple type name */
				}
			}
		for (int i = 0; i < fn->nlocals; i++)
			if (strcmp(fn->locals[i].name, e->name) == 0)
				return fn->locals[i].type.kind == TY_INT ? "Int" : fn->locals[i].type_name;
		return "";
	case EX_CALL: {
		Func *c = prog_find_func(prog, e->name);
		if (!c || c->ntyparams > 0) /* unknown, or an unspecialized generic */
			return "";
		if (strcmp(c->ret_type_name, "Uarch") == 0)
			return "Uarch";
		return c->ret_type_name[0] ? c->ret_type_name : "Int";
	}
	default:
		/* arithmetic/comparison/logical/field — Int in the common case; a genuine
		 * mismatch surfaces later as a per-instantiation error. */
		return "Int";
	}
}

/* Walk a concrete function's body and specialize every call to a generic template —
 * children first, so a nested generic call is resolved (its return type known) before an
 * enclosing call infers from it. Type arguments come from the explicit `[…]` list or are
 * inferred from the argument types. The call is rewritten to the mangled concrete name;
 * appended instantiations are revisited by the driver loop → a fixpoint. */
static void monomorph_expr(Program *prog, Func *fn, Expr *e) {
	if (!e)
		return;
	monomorph_expr(prog, fn, e->lhs);
	monomorph_expr(prog, fn, e->rhs);
	monomorph_expr(prog, fn, e->els);
	for (int i = 0; i < e->nargs; i++)
		monomorph_expr(prog, fn, e->args[i]);
	for (int i = 0; i < e->nfields; i++)
		monomorph_expr(prog, fn, e->fvals[i]);
	for (int i = 0; i < e->narms; i++)
		monomorph_expr(prog, fn, e->arms[i].body);
	/* A generic record literal (`Box[Int]{…}`) or union member value (`Maybe[Int].Just`)
	 * names a type application — instantiate it. */
	if (e->kind == EX_RECORD || e->kind == EX_UMEMBER)
		concretize_name(prog, e->name, e->line);
	if (e->kind != EX_CALL)
		return;
	Func *callee = prog_find_func(prog, e->name);
	if (!callee || callee->ntyparams == 0) {
		if (e->ntypeargs > 0)
			die(e->line, "type arguments given to a non-generic function");
		return;
	}
	char args[MAX_TYPARAMS][64];
	int ntargs = e->ntypeargs;
	if (ntargs > 0) {
		for (int i = 0; i < ntargs; i++)
			snprintf(args[i], sizeof args[0], "%s", e->typeargs[i]);
	} else {
		/* Infer each type parameter from the argument in a `'T` position. */
		int found[MAX_TYPARAMS] = {0};
		for (int pi = 0; pi < callee->nparams; pi++) {
			if (callee->params[pi].kind != PK_VAR || pi >= e->nargs)
				continue;
			int tp = func_typaram_index(callee, callee->params[pi].type_name);
			if (tp < 0)
				continue;
			const char *at = shallow_type_name(prog, fn, e->args[pi]);
			if (!at[0])
				die(e->line, "cannot infer the type argument — write it explicitly, e.g. `f[Int](...)`");
			if (found[tp] && strcmp(args[tp], at) != 0)
				die(e->line, "conflicting inferred type arguments");
			snprintf(args[tp], sizeof args[0], "%s", at);
			found[tp] = 1;
		}
		for (int t = 0; t < callee->ntyparams; t++)
			if (!found[t])
				die(e->line, "cannot infer a type argument (not fixed by any parameter) — write it explicitly");
		ntargs = callee->ntyparams;
	}
	Func *inst = instantiate(prog, callee, args, ntargs, e->line);
	snprintf(e->name, sizeof e->name, "%s", inst->name);
	e->ntypeargs = 0; /* now a concrete call */
}

static void monomorph_stmts(Program *prog, Func *fn, Stmt *s) {
	for (; s; s = s->next) {
		if (s->kind == ST_LOCAL && s->type_name[0]) /* a local's generic-type annotation */
			concretize_name(prog, s->type_name, s->line);
		monomorph_expr(prog, fn, s->expr);
		monomorph_stmts(prog, fn, s->body);
	}
}

/* Specialize the whole program from its concrete functions and types. prog->nfuncs and
 * prog->ndatas/nunions grow as instantiations are appended; each loop revisits them, so
 * transitive generic uses are reached (a type instance is fully concretized on creation, so
 * revisiting it is idempotent). Templates (ntyparams > 0) are skipped here and by every
 * later pass. */
static void monomorphize(Program *prog) {
	for (int i = 0; i < prog->nfuncs; i++) {
		Func *fn = prog->funcs[i];
		if (fn->ntyparams > 0)
			continue;
		concretize_signature(prog, fn);          /* param/return type applications */
		if (!fn->is_asm)
			monomorph_stmts(prog, fn, fn->body); /* generic calls + local/literal type apps */
	}
	for (int i = 0; i < prog->ndatas; i++) {
		if (prog->datas[i]->ntyparams > 0)
			continue;
		DataDecl *d = prog->datas[i];
		for (int j = 0; j < d->nfields; j++)
			concretize_name(prog, d->field_types[j], 0);
	}
	for (int i = 0; i < prog->nunions; i++) {
		if (prog->unions[i]->ntyparams > 0)
			continue;
		UnionDecl *u = prog->unions[i];
		for (int m = 0; m < u->nmembers; m++)
			for (int j = 0; j < u->arity[m]; j++)
				concretize_name(prog, u->payload_types[m][j], 0);
	}
}

/* ------------------------------------------------------------- typecheck - */

static Type typeof_expr(Program *prog, Func *fn, Expr *e);
static Type func_ret_type(const Func *fn);

/* Require that `e` yields an Int. A record is legal only as a field-access base
 * and a pointer never, so wherever an Int is expected this rejects them both. */
static void expect_int(Program *prog, Func *fn, Expr *e) {
	if (typeof_expr(prog, fn, e).kind != TY_INT)
		die(e->line, "expected an Int value (a record is used only via field access, "
		             "and a string only via `.len`, in M0)");
}

/* Check that value `val` matches an expected field/payload type `want` (G3a). An Int
 * field wants an Int; a record field wants that record — and NOT a bare record variable,
 * whose arena pointer the field would alias (an aggregate copy needs an explicit `copy`,
 * memory_model §6; a `union` value is immutable so aliasing it is sound). */
static void check_member_value(Program *prog, Func *fn, Expr *val, Type want, int line) {
	Type at = typeof_expr(prog, fn, val);
	if (want.kind == TY_INT) {
		if (at.kind != TY_INT)
			die(line, "expected an Int value for this field/payload");
	} else if (want.kind == TY_RECORD) {
		if (at.kind != TY_RECORD || at.rec != want.rec)
			die(line, "field/payload type mismatch (record type differs)");
		/* A record field/payload must be a FRESH value. Only a record-returning call (or a
		 * data literal) allocates one; a bare variable, a field access (`rec.p`), a match/if
		 * result, etc. would alias existing MUTABLE record storage — memory_model §6 forbids
		 * a second binding without an explicit `copy`. Allowlist the fresh producers rather
		 * than blocklisting one form, so newly-reachable alias shapes stay closed. */
		if (val->kind != EX_CALL && val->kind != EX_RECORD)
			die(line, "a record field/payload must be a fresh value (a record-returning call), "
			          "not an alias of existing storage (an aggregate copy needs an explicit copy — not in M0)");
	} else if (want.kind == TY_UNION) {
		if (at.kind != TY_UNION || at.uni != want.uni)
			die(line, "field/payload type mismatch (union type differs)");
		/* A union value may be a bare variable here — cfcc unions are IMMUTABLE (no union
		 * field-mutation path), so aliasing one is unobservable and sound. ⚠ THROWAWAY: cf0
		 * must NOT inherit this — it rehomes/copies an aggregate payload like any aggregate
		 * (memory_model §6); the bare-union-var alias is a genesis shortcut leaning on M0
		 * union immutability. */
	} else {
		die(line, "unsupported field/payload type");
	}
}

/* Compute and validate the type of an expression, resolving field accesses and
 * calls against the whole program (so forward references and recursion work).
 * Field accesses are annotated in place (rec, foff) for emit. The public entry is
 * the `typeof_expr` wrapper below, which caches the result in `e->rtype`. */
static Type typeof_expr_compute(Program *prog, Func *fn, Expr *e) {
	switch (e->kind) {
	case EX_INT:
		return (Type){TY_INT, NULL, NULL};
	case EX_STR:
		return (Type){TY_STR, NULL, NULL};
	case EX_VAR: {
		/* A match-arm payload binding shadows params/locals; its value is an Int read
		 * from the `%pb<id>` storage temp (loaded at the arm block). */
		Type bt;
		int bid = find_active_bind_type(fn, e->name, &bt);
		if (bid >= 0) {
			e->is_bind = 1;
			e->bind_id = bid;
			return bt; /* Int for a word payload; a record/union type for an aggregate payload */
		}
		Type ty;
		if (resolve_name(fn, e->name, &ty) == R_NONE)
			die(e->line, "unknown name");
		return ty;
	}
	case EX_FIELD: {
		Type base = typeof_expr(prog, fn, e->lhs);
		if (base.kind == TY_STR) {
			/* A string exposes two fields: `.len`, its byte count (an Int), and
			 * `.bytes`, the pointer to its UTF-8 bytes (`*[Uint8]`, an opaque pointer)
			 * — enough to hand a buffer + length to `write`. (Both are provisional
			 * cfcc surface over the throwaway {bytes*,len} header; cf0's Str API differs.) */
			if (strcmp(e->name, "len") == 0)
				return (Type){TY_INT, NULL, NULL};
			if (strcmp(e->name, "bytes") == 0)
				return (Type){TY_PTR, NULL, NULL};
			die(e->line, "a string has only the `.len` and `.bytes` fields");
		}
		if (base.kind != TY_RECORD)
			die(e->line, "field access `.` needs a record value on the left");
		int idx = data_field_index(base.rec, e->name);
		if (idx < 0)
			die(e->line, "this data type has no such field");
		e->rec = base.rec;
		e->foff = data_field_offset(idx);
		return data_field_type(prog, base.rec, idx); /* Int or an aggregate field type */
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
		/* Each argument's type must match the parameter. A `Uarch` parameter is the
		 * register-width syscall type: it accepts a `Uarch`; an `Int`, which cfcc widens
		 * to register width here (a throwaway coercion — cf0 spells an explicit `Uarch(x)`
		 * cast, type_system §4); or a pointer, which already occupies a full register and
		 * rides it bare (no conversion — cf0 passes `*[Uint8]` into the syscall register
		 * directly too, type_system §6.4; there is no pointer↔integer cast). */
		for (int i = 0; i < callee->nparams; i++) {
			Param *pm = &callee->params[i];
			Type at = typeof_expr(prog, fn, e->args[i]);
			switch (pm->kind) {
			case PK_WORD:
				if (at.kind != TY_INT)
					die(e->line, "argument type mismatch (a word parameter expects an Int)");
				break;
			case PK_RECORD:
				if (at.kind != TY_RECORD)
					die(e->line, "argument type mismatch (a record parameter expects a record)");
				if (at.rec != pm->rec)
					die(e->line, "argument type mismatch (record type differs)");
				break;
			case PK_UARCH:
				if (at.kind != TY_UARCH && at.kind != TY_INT && at.kind != TY_PTR && at.kind != TY_BUF)
					die(e->line, "argument type mismatch (a Uarch parameter expects a Uarch, Int, pointer, or buffer)");
				break;
			case PK_LONG:
				if (at.kind != TY_PTR && at.kind != TY_BUF)
					die(e->line, "argument type mismatch (a pointer parameter expects a pointer or `[N Uint8]` buffer, e.g. `s.bytes`)");
				break;
			case PK_UNION:
				if (at.kind != TY_UNION || at.uni != pm->uni)
					die(e->line, "argument type mismatch (union type differs)");
				break;
			case PK_VAR:
				die(e->line, "internal: call to an unspecialized generic function");
				break;
			}
		}
		e->callee = callee; /* cached for emit (per-arg register width, Int→Uarch widen) */
		return func_ret_type(callee); /* Int, Uarch, or a record returned by pointer */
	}
	case EX_UMEMBER: {
		/* `Union.Member(payload…)` — construct a member. A tag-only member's value is
		 * its tag; a payload member's is a boxed tag+payload aggregate. */
		UnionDecl *u = prog_find_union(prog, e->name);
		if (!u)
			die(e->line, "unknown union type");
		int tag = union_member_tag(u, e->mem);
		if (tag < 0)
			die(e->line, "this union has no such member");
		if (e->nargs != u->arity[tag])
			die(e->line, "union member payload arity mismatch");
		for (int i = 0; i < e->nargs; i++)
			check_member_value(prog, fn, e->args[i], union_payload_type(prog, u, tag, i), e->line);
		e->uni = u;
		e->ival = tag; /* the member's tag */
		return (Type){TY_UNION, NULL, u};
	}
	case EX_MATCH: {
		/* Compare-chain over a union scrutinee's tag (seed_subset §7). Each arm names a
		 * member of the scrutinee's union (qualified) or is `_`; arm bodies are Int
		 * (M1.1) and unify to the match's type. Exhaustiveness: cover every member or
		 * carry a `_`. */
		Type st = typeof_expr(prog, fn, e->lhs);
		if (st.kind != TY_UNION)
			die(e->line, "M1.1 `match` requires a union scrutinee");
		UnionDecl *u = st.uni;
		e->uni = u;
		int covered[MAX_UNION_MEMBERS] = {0};
		int has_wild = 0, have_rt = 0;
		Type rt = {TY_INT, NULL, NULL}; /* the arms' common (result) type */
		for (int i = 0; i < e->narms; i++) {
			MatchArm *a = &e->arms[i];
			if (has_wild)
				die(a->line, "unreachable match arm after `_`");
			int saved_abinds = fn->nabinds; /* arm-scoped bindings pop after the body */
			if (a->is_wild) {
				has_wild = 1;
			} else {
				/* Arms qualify members by the union's base (template) name — `Maybe.Just`,
				 * not the mangled instance name `Maybe.1.Int` (type_system §8.3). */
				if (strcmp(a->qual, u->base_name) != 0)
					die(a->line, "a match arm must name a member of the scrutinee's union, qualified by it");
				for (int k = 0; k < a->nalts; k++) {
					int tag = union_member_tag(u, a->members[k]);
					if (tag < 0)
						die(a->line, "this union has no such member");
					if (covered[tag])
						die(a->line, "duplicate match arm for this member");
					covered[tag] = 1;
					a->tags[k] = tag;
				}
				/* A payload sub-pattern (single-member arm) binds each field to an Int
				 * scoped to this arm's body; `_` ignores a field. */
				if (a->nbinds > 0) {
					int tag = a->tags[0];
					if (a->nbinds != u->arity[tag])
						die(a->line, "payload pattern arity does not match the member");
					for (int b = 0; b < a->nbinds; b++) {
						/* A payload field's type (Int or an aggregate) sets the bound name's
						 * type and repr — a word (Int/tag-only union) or a pointer. */
						Type pt = union_payload_type(prog, u, tag, b);
						a->bind_word[b] = type_is_word(pt);
						if (strcmp(a->binds[b], "_") == 0) {
							a->bind_ids[b] = -1;
							continue;
						}
						for (int j = 0; j < b; j++)
							if (strcmp(a->binds[j], a->binds[b]) == 0)
								die(a->line, "duplicate payload binding name");
						if (fn->nabinds == (int)(sizeof fn->abinds / sizeof fn->abinds[0]))
							die(a->line, "too many active payload bindings");
						int id = fn->next_bind_id++;
						a->bind_ids[b] = id;
						snprintf(fn->abinds[fn->nabinds].name, sizeof fn->abinds[0].name, "%s", a->binds[b]);
						fn->abinds[fn->nabinds].id = id;
						fn->abinds[fn->nabinds].type = pt;
						fn->nabinds++;
					}
				}
			}
			/* Arm bodies unify to one type — an Int or a tag-only union (both a word);
			 * that type is the match's value type, so a `match` can yield either. The
			 * scrutinee may still be a boxed (payload) union — a match yielding a boxed
			 * union (which the word merge slot can't hold) is a later brick. */
			Type bt = typeof_expr(prog, fn, a->body);
			if (!type_is_word(bt))
				die(a->line, "a match arm yields an Int or a tag-only union (a boxed-union result is a later brick)");
			if (!have_rt) {
				rt = bt;
				have_rt = 1;
			} else if (bt.kind != rt.kind || (rt.kind == TY_UNION && bt.uni != rt.uni)) {
				die(a->line, "match arms must all yield the same type");
			}
			fn->nabinds = saved_abinds; /* pop this arm's bindings */
		}
		if (!has_wild)
			for (int i = 0; i < u->nmembers; i++)
				if (!covered[i])
					die(e->line, "non-exhaustive match (cover every union member or add a `_` arm)");
		return rt;
	}
	case EX_IF:
		expect_int(prog, fn, e->lhs); /* condition */
		expect_int(prog, fn, e->rhs); /* then — M0 if-branches are Int */
		expect_int(prog, fn, e->els); /* else */
		return (Type){TY_INT, NULL, NULL};
	case EX_NEG:
	case EX_BNOT:
	case EX_LNOT:
		expect_int(prog, fn, e->lhs);
		return (Type){TY_INT, NULL, NULL};
	case EX_ADD: case EX_SUB: case EX_MUL: case EX_DIV: case EX_REM:
	case EX_BOR: case EX_BXOR: case EX_BAND: case EX_SHL: case EX_SHR:
	case EX_EQ: case EX_NE: case EX_LT: case EX_GT: case EX_LE: case EX_GE:
	case EX_AND: case EX_OR:
		expect_int(prog, fn, e->lhs);
		expect_int(prog, fn, e->rhs);
		return (Type){TY_INT, NULL, NULL};
	}
	die(e->line, "internal: unhandled expression kind in typecheck");
}

/* Type an expression and cache the result in `e->rtype` so emit can dispatch on it
 * (record → an `l` arena pointer; word → a `w`). */
static Type typeof_expr(Program *prog, Func *fn, Expr *e) {
	Type t = typeof_expr_compute(prog, fn, e);
	e->rtype = t;
	return t;
}

/* A function's declared return type: a record (returned by pointer) or Int. */
static Type func_ret_type(const Func *fn) {
	if (strcmp(fn->ret_type_name, "Uarch") == 0)
		return (Type){TY_UARCH, NULL, NULL};
	if (fn->ret_uni)
		return (Type){TY_UNION, NULL, fn->ret_uni};
	if (fn->ret_type_name[0])
		return (Type){TY_RECORD, fn->ret_rec, NULL};
	return (Type){TY_INT, NULL, NULL};
}

/* Backfill a record local's declaration so later field accesses resolve. */
static void set_local_rec(Func *fn, const char *name, DataDecl *d) {
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			fn->locals[i].type.rec = d;
			return;
		}
}

/* Retype a local (parsed provisionally as a nominal `TY_RECORD`) to the union it was
 * annotated with — a tag-only union value is a word, so this only carries the union
 * identity for `match`. */
static void set_local_union(Func *fn, const char *name, UnionDecl *u) {
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			fn->locals[i].type.kind = TY_UNION;
			fn->locals[i].type.rec = NULL;
			fn->locals[i].type.uni = u;
			return;
		}
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
		check_member_value(prog, fn, e->fvals[k], data_field_type(prog, d, idx), e->line);
		e->ford[idx] = e->fvals[k];
	}
	for (int i = 0; i < d->nfields; i++)
		if (!e->ford[i])
			die(e->line, "data literal is missing a field");
	set_local_rec(fn, s->name, d);
}

/* Resolve a record local whose initializer is a record-valued expression (not a
 * literal) — in M0, a call that returns this record. Binding from another record
 * variable is rejected (an aggregate copy needs an explicit `copy`, memory_model
 * §6). Sets the local's record type from the annotation. */
static void resolve_record_expr_binding(Program *prog, Func *fn, Stmt *s) {
	DataDecl *d = prog_find_data(prog, s->type_name);
	if (!d)
		die(s->line, "unknown data type");
	/* Only a record-returning call produces a fresh record in expression position; a bare
	 * variable OR a field access (`rec.p`, now that a field may be a record) would alias
	 * existing storage — an aggregate copy needs an explicit copy (memory_model §6). (A data
	 * literal takes the resolve_record_binding path.) */
	if (s->expr->kind != EX_CALL)
		die(s->line, "a record binding's initializer must be a fresh record (a record-returning call); "
		             "aliasing existing record storage needs an explicit copy — not in M0");
	Type it = typeof_expr(prog, fn, s->expr);
	if (it.kind != TY_RECORD || it.rec != d)
		die(s->line, "initializer type does not match the record binding");
	set_local_rec(fn, s->name, d);
}

/* Type-check a statement list in source order, so a record binding's type is
 * resolved before any later statement reads its fields. Recurses into loop bodies. */
static void check_stmts(Program *prog, Func *fn, Stmt *list) {
	for (Stmt *s = list; s; s = s->next) {
		switch (s->kind) {
		case ST_LOCAL:
			if (s->bufsize)                                   /* a `[N Uint8]` buffer: no initializer */
				break;
			if (strcmp(s->type_name, "Str") == 0) {           /* a `const Str` local */
				if (typeof_expr(prog, fn, s->expr).kind != TY_STR)
					die(s->line, "internal: Str local initializer is not a string");
			} else if (prog_find_union(prog, s->type_name)) { /* a union binding */
				/* `const Color c = Color.Red` — the annotation names a union; the
				 * initializer must be a value of that union (a member, or another
				 * union var — a tag-only union is a word, so a copy is harmless). */
				UnionDecl *u = prog_find_union(prog, s->type_name);
				Type it = typeof_expr(prog, fn, s->expr);
				if (it.kind != TY_UNION || it.uni != u)
					die(s->line, "initializer type does not match the union binding");
				set_local_union(fn, s->name, u);
			} else if (s->type_name[0]) { /* a record binding (annotated with a record type) */
				if (s->expr->kind == EX_RECORD)
					resolve_record_binding(prog, fn, s);      /* { … } literal */
				else
					resolve_record_expr_binding(prog, fn, s); /* a record-valued call */
			} else {
				expect_int(prog, fn, s->expr);                /* a word local */
			}
			break;
		case ST_FIELD_ASSIGN: {
			/* `name.field = expr` — target must be a mutable (`let`) record local,
			 * the field must exist, and the value is Int. */
			Type ty;
			Resolution r = resolve_name(fn, s->name, &ty);
			if (r == R_NONE)
				die(s->line, "unknown name (mutate a declared record local)");
			if (ty.kind != TY_RECORD)
				die(s->line, "field assignment `.` needs a record on the left");
			if (r == R_PARAM)
				die(s->line, "cannot mutate a parameter's field");
			if (r == R_CONST)
				die(s->line, "cannot mutate a `const` record's field (declare it with `let`)");
			int idx = data_field_index(ty.rec, s->field);
			if (idx < 0)
				die(s->line, "this data type has no such field");
			s->foff = data_field_offset(idx);
			check_member_value(prog, fn, s->expr, data_field_type(prog, ty.rec, idx), s->line);
			break;
		}
		case ST_ASSIGN: /* target is a `let` word local; value is Int */
			expect_int(prog, fn, s->expr);
			break;
		case ST_RETURN: {
			/* The returned value must match the function's return type. A record
			 * return may not be a bare parameter — that would hand the caller an alias
			 * of its own argument; a returned record must be freshly built (a local or
			 * another call's result), which the arena keeps alive past the frame. */
			Type rt = func_ret_type(fn);
			if (rt.kind == TY_RECORD) {
				if (s->expr->kind == EX_VAR) {
					Type vt;
					if (resolve_name(fn, s->expr->name, &vt) == R_PARAM && vt.kind == TY_RECORD)
						die(s->expr->line, "cannot return a parameter record (build a fresh one)");
				}
				Type et = typeof_expr(prog, fn, s->expr);
				if (et.kind != TY_RECORD || et.rec != rt.rec)
					die(s->expr->line, "returned value does not match the record return type");
			} else if (rt.kind == TY_UARCH) {
				/* A Uarch return accepts a Uarch, or an Int/pointer widened to register
				 * width (the same call-site coercion as a Uarch parameter). */
				Type et = typeof_expr(prog, fn, s->expr);
				if (et.kind != TY_UARCH && et.kind != TY_INT && et.kind != TY_PTR)
					die(s->expr->line, "a Uarch function returns a Uarch, Int, or pointer value");
			} else if (rt.kind == TY_UNION) {
				Type et = typeof_expr(prog, fn, s->expr);
				if (et.kind != TY_UNION || et.uni != rt.uni)
					die(s->expr->line, "returned value does not match the union return type");
			} else {
				expect_int(prog, fn, s->expr);
			}
			break;
		}
		case ST_LOOP:
			check_stmts(prog, fn, s->body);
			break;
		case ST_BREAK:
		case ST_CONTINUE:
			if (s->expr) /* guarded: `if <cond> then break/continue` */
				expect_int(prog, fn, s->expr);
			break;
		case ST_EXPR:
			/* A call evaluated for effect: type it (validates the callee/args); the
			 * result, whatever its type, is discarded. */
			if (s->expr->kind != EX_CALL)
				die(s->line, "an expression statement must be a call");
			typeof_expr(prog, fn, s->expr);
			break;
		}
	}
}

static void check_func(Program *prog, Func *fn) {
	check_stmts(prog, fn, fn->body);
}

/* Resolve every function's record parameter and return types (name → declaration)
 * so both bodies and call sites see the resolved decls. Done for all functions
 * before any body is checked, since a call may reference a callee defined later. */
static void resolve_signatures(Program *prog) {
	for (int i = 0; i < prog->nfuncs; i++) {
		Func *fn = prog->funcs[i];
		if (fn->ntyparams > 0) /* a generic template — only its instantiations resolve */
			continue;
		for (int j = 0; j < fn->nparams; j++)
			if (fn->params[j].kind == PK_RECORD) {
				/* A nominal-typed param naming a union is reclassified to PK_UNION
				 * (tag-only → a word); otherwise it must name a `data` record. */
				UnionDecl *u = prog_find_union(prog, fn->params[j].type_name);
				if (u) {
					fn->params[j].kind = PK_UNION;
					fn->params[j].uni = u;
					continue;
				}
				DataDecl *d = prog_find_data(prog, fn->params[j].type_name);
				if (!d)
					die(fn->params[j].line, "a parameter names an unknown data type");
				fn->params[j].rec = d;
			}
		if (fn->ret_type_name[0] && strcmp(fn->ret_type_name, "Uarch") != 0) {
			UnionDecl *u = prog_find_union(prog, fn->ret_type_name);
			if (u) {
				fn->ret_uni = u;
			} else {
				DataDecl *d = prog_find_data(prog, fn->ret_type_name);
				if (!d)
					die(fn->ret_line, "a return type names an unknown data type");
				fn->ret_rec = d;
			}
		}
	}
}

static void typecheck(Program *prog) {
	resolve_signatures(prog);
	/* Every field/payload type must name Int or a declared aggregate (G3a). Validate up
	 * front so an unused bad field type is still an error, and forward/mutual refs resolve. */
	for (int i = 0; i < prog->ndatas; i++) {
		DataDecl *d = prog->datas[i];
		if (d->ntyparams > 0) /* a generic template — only its instantiations are laid out */
			continue;
		for (int j = 0; j < d->nfields; j++)
			resolve_member_type(prog, d->field_types[j], 0);
	}
	for (int i = 0; i < prog->nunions; i++) {
		UnionDecl *u = prog->unions[i];
		if (u->ntyparams > 0)
			continue;
		for (int m = 0; m < u->nmembers; m++)
			for (int j = 0; j < u->arity[m]; j++)
				resolve_member_type(prog, u->payload_types[m][j], 0);
	}
	for (int i = 0; i < prog->nfuncs; i++)
		if (prog->funcs[i]->ntyparams == 0) /* skip generic templates (only clones checked) */
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
 * (both function-scoped in QBE), plus a stack of enclosing loops' ids so `break`
 * and `continue` reach the nearest loop's end/top labels. */
typedef struct {
	int tmp;
	int lbl;
	int loops[MAX_LOOP_DEPTH]; /* label ids of enclosing loops (innermost last) */
	int loop_depth;
	int ret_uarch; /* 1 if the current function returns Uarch (an `l`) — a returned
	                * Int value is widened to `l` before `ret`. */
} Emit;

/* The module's string table: every EX_STR literal, assigned a stable index
 * (`Expr.strid`) by a pre-emit walk. Each becomes two module data defs — the raw
 * NUL-terminated bytes and a `{bytes*, len}` header — emitted by emit_string_data
 * ahead of the functions; a Str value is a pointer to its header. (Provisional
 * throwaway layout, like the record layout — not the cf0 string representation.) */
static Expr **g_strlits;
static int g_nstrlits, g_cap_strlits;

static void register_strlit(Expr *e) {
	if (g_nstrlits == g_cap_strlits) {
		g_cap_strlits = g_cap_strlits ? g_cap_strlits * 2 : 16;
		g_strlits = realloc(g_strlits, g_cap_strlits * sizeof *g_strlits);
		if (!g_strlits)
			die(0, "out of memory");
	}
	e->strid = g_nstrlits;
	g_strlits[g_nstrlits++] = e;
}

/* Walk an expression (and its sub-expressions) registering every string literal,
 * so each gets a module-unique data slot before emit. Mirrors assign_expr_slots. */
static void collect_strlits_expr(Expr *e) {
	if (!e)
		return;
	if (e->kind == EX_STR)
		register_strlit(e);
	collect_strlits_expr(e->lhs);
	collect_strlits_expr(e->rhs);
	collect_strlits_expr(e->els);
	for (int i = 0; i < e->nargs; i++)
		collect_strlits_expr(e->args[i]);
	for (int i = 0; i < e->nfields; i++)
		collect_strlits_expr(e->fvals[i]);
	for (int i = 0; i < e->narms; i++) /* EX_MATCH arm bodies */
		collect_strlits_expr(e->arms[i].body);
}

static void collect_strlits_stmt(Stmt *list) {
	for (Stmt *s = list; s; s = s->next) {
		collect_strlits_expr(s->expr);
		if (s->kind == ST_LOOP)
			collect_strlits_stmt(s->body);
	}
}

/* Emit each string literal as two module data defs: the bytes (emitted one
 * numeric byte at a time so any content — quotes, NULs, newlines — is safe in the
 * IL) with a trailing NUL, and a `{bytes*, len}` header the Str value points at.
 * `len` reads the length word at offset 8 of the header. */
static void emit_string_data(FILE *out) {
	for (int i = 0; i < g_nstrlits; i++) {
		Expr *e = g_strlits[i];
		fprintf(out, "data $cfstr_bytes_%d = { ", i);
		for (int j = 0; j < e->slen; j++)
			fprintf(out, "b %d, ", (unsigned char)e->sval[j]);
		fprintf(out, "b 0 }\n");
		fprintf(out, "data $cfstr_%d = { l $cfstr_bytes_%d, w %d }\n", i, i, e->slen);
	}
}

/* Emit the code that computes `e` into a fresh word temp, writing the operand
 * that names its value (a literal or a `%tN` temp) into `dst`. Constants are
 * inlined — QBE accepts them as instruction operands.
 *
 * Names live in stack slots (`%s_<name>`), so a variable reference is a `loadw`.
 * A record local's storage is an arena pointer held in `%r_<name>` (a field read
 * is a `loadw` at the field's offset off that pointer). The distinct `%s_` / `%r_`
 * / `%u_` / `%tN` / `%mN` prefixes (word slot / record pointer / incoming param /
 * temp / if-and-logical merge slot) mean a user name can never collide with a
 * compiler value. */
static void emit_expr(FILE *out, Expr *e, Emit *ex, char *dst, size_t cap) {
	switch (e->kind) {
	case EX_INT:
		snprintf(dst, cap, "%ld", e->ival);
		return;
	case EX_STR: {
		/* A Str value is a pointer to its header; materialize the address into a temp
		 * so it is a plain `l` operand wherever used (a call argument, `len`, …). */
		int t = ex->tmp++;
		fprintf(out, "\t%%t%d =l copy $cfstr_%d\n", t, e->strid);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_VAR: {
		/* A match-arm payload binding is an Int value held in its `%pb<id>` temp
		 * (loaded at the arm block). */
		if (e->is_bind) {
			snprintf(dst, cap, "%%pb%d", e->bind_id);
			return;
		}
		/* A record, byte-buffer, or boxed (payload) union name is an arena pointer
		 * (`%r_<name>`), used directly as an operand; a word name (Int or tag-only
		 * union) is a `loadw` from its slot. */
		if (e->rtype.kind == TY_RECORD || e->rtype.kind == TY_BUF ||
		    (e->rtype.kind == TY_UNION && e->rtype.uni->has_payload)) {
			snprintf(dst, cap, "%%r_%s", e->name);
			return;
		}
		if (e->rtype.kind == TY_PTR || e->rtype.kind == TY_UARCH) {
			/* A pointer (`*[Uint8]` buffer) or Uarch value is, in M0, always an
			 * immutable `l` parameter — reference its incoming temp directly (no slot). */
			snprintf(dst, cap, "%%u_%s", e->name);
			return;
		}
		if (e->rtype.kind == TY_STR) {
			/* A Str local: load its header pointer from its `l` slot. */
			int t = ex->tmp++;
			fprintf(out, "\t%%t%d =l loadl %%s_%s\n", t, e->name);
			snprintf(dst, cap, "%%t%d", t);
			return;
		}
		int t = ex->tmp++;
		fprintf(out, "\t%%t%d =w loadw %%s_%s\n", t, e->name);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_FIELD: {
		/* `s.len` — the length word sits at offset 8 of the string header. The base is
		 * any Str expression (a literal in this brick), so emit it and load. */
		if (e->lhs->rtype.kind == TY_STR) {
			char s[96];
			emit_expr(out, e->lhs, ex, s, sizeof s);
			if (strcmp(e->name, "bytes") == 0) {
				/* `.bytes` — the byte pointer at offset 0 of the header (an `l`). */
				int r = ex->tmp++;
				fprintf(out, "\t%%t%d =l loadl %s\n", r, s);
				snprintf(dst, cap, "%%t%d", r);
				return;
			}
			/* `.len` — the length word sits at offset 8 of the string header. */
			int a = ex->tmp++;
			fprintf(out, "\t%%t%d =l add %s, 8\n", a, s);
			int r = ex->tmp++;
			fprintf(out, "\t%%t%d =w loadw %%t%d\n", r, a);
			snprintf(dst, cap, "%%t%d", r);
			return;
		}
		/* Read a record field. Emit the base to its pointer operand (a record VAR is
		 * `%r_<name>`, a bound aggregate payload is `%pb<id>`, a record-returning call or a
		 * chained field access is a temp), add the field offset, then load a word (an Int
		 * or tag-only union field) or an 8-byte pointer (an aggregate field — itself a
		 * record/boxed-union value). This handles any base, so nested `a.b.c` works. */
		char base[96];
		emit_expr(out, e->lhs, ex, base, sizeof base);
		char addr[96];
		if (e->foff == 0) {
			snprintf(addr, sizeof addr, "%s", base);
		} else {
			int a = ex->tmp++;
			fprintf(out, "\t%%t%d =l add %s, %d\n", a, base, e->foff);
			snprintf(addr, sizeof addr, "%%t%d", a);
		}
		int t = ex->tmp++;
		if (type_is_word(e->rtype))
			fprintf(out, "\t%%t%d =w loadw %s\n", t, addr);
		else
			fprintf(out, "\t%%t%d =l loadl %s\n", t, addr);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_RECORD:
		die(e->line, "internal: record literal in expression position");
	case EX_CALL: {
		/* Evaluate each argument into its own temp, then call. Each argument's register
		 * width follows the *parameter* kind (word param → `w`; record/pointer/Uarch → `l`),
		 * and an Int passed to a Uarch parameter is widened `w`→`l`. The callee symbol is
		 * the bare function name ($<name>); the result width follows the return type. */
		int argt[MAX_PARAMS];
		char argw[MAX_PARAMS];
		for (int i = 0; i < e->nargs; i++) {
			char op[96];
			emit_expr(out, e->args[i], ex, op, sizeof op);
			ParamKind pk = e->callee->params[i].kind;
			if (pk == PK_UARCH && e->args[i]->rtype.kind == TY_INT) {
				/* Widen an Int to register width for a Uarch parameter (throwaway cfcc
				 * coercion; cf0 spells an explicit `Uarch(x)`). Sign-extend to match
				 * cfcc's signed Int — the syscall args here are small and non-negative. */
				int w = ex->tmp++;
				fprintf(out, "\t%%t%d =w copy %s\n", w, op);
				argt[i] = ex->tmp++;
				fprintf(out, "\t%%t%d =l extsw %%t%d\n", argt[i], w);
				argw[i] = 'l';
			} else {
				argw[i] = param_is_word(&e->callee->params[i]) ? 'w' : 'l';
				argt[i] = ex->tmp++;
				fprintf(out, "\t%%t%d =%c copy %s\n", argt[i], argw[i], op);
			}
		}
		int r = ex->tmp++;
		/* An Int or tag-only union result is a word; record/ptr/Uarch/boxed-union are `l`. */
		const char *rty = type_is_word(e->rtype) ? "w" : "l";
		fprintf(out, "\t%%t%d =%s call $%s(", r, rty, e->name);
		for (int i = 0; i < e->nargs; i++)
			fprintf(out, "%s%c %%t%d", i ? ", " : "", argw[i], argt[i]);
		fprintf(out, ")\n");
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_UMEMBER: {
		/* A tag-only union member value IS its tag — a word constant. */
		if (!e->uni->has_payload) {
			snprintf(dst, cap, "%ld", e->ival);
			return;
		}
		/* A boxed union member: bump-allocate the tag+payload aggregate, store the tag
		 * at offset 0 and each payload field at 8 + i*8 (word or pointer). Yields the arena pointer. */
		int p = ex->tmp++;
		fprintf(out, "\t%%t%d =l call $cf_alloc(w %d)\n", p, e->uni->size);
		fprintf(out, "\tstorew %ld, %%t%d\n", e->ival, p); /* tag in the first slot */
		for (int i = 0; i < e->nargs; i++) {
			char a[96];
			emit_expr(out, e->args[i], ex, a, sizeof a);
			int fa = ex->tmp++;
			fprintf(out, "\t%%t%d =l add %%t%d, %d\n", fa, p, union_payload_offset(i));
			/* An Int/tag-only-union payload is a word; a record/boxed-union payload is a pointer. */
			if (type_is_word(e->args[i]->rtype))
				fprintf(out, "\tstorew %s, %%t%d\n", a, fa);
			else
				fprintf(out, "\tstorel %s, %%t%d\n", a, fa);
		}
		snprintf(dst, cap, "%%t%d", p);
		return;
	}
	case EX_MATCH: {
		/* Compare-chain over the scrutinee's tag (seed_subset §7): a linear ladder of
		 * `ceqw tag, <k>` tests. Each member arm's block stores its value into the
		 * entry-block merge slot `%m<slot>` and jumps to @mend; the `_` arm (or, for an
		 * exhaustive match, an unreachable fallback) is the ladder's fall-through. Arm
		 * results merge through the slot exactly like `if`. */
		int id = ex->lbl++;
		char sc[96];
		emit_expr(out, e->lhs, ex, sc, sizeof sc);
		int tagt = ex->tmp++;
		/* The tag: a tag-only union value IS the tag; a boxed union is a pointer whose
		 * tag sits at offset 0. */
		if (e->uni->has_payload)
			fprintf(out, "\t%%t%d =w loadw %s\n", tagt, sc);
		else
			fprintf(out, "\t%%t%d =w copy %s\n", tagt, sc);
		int wild = -1;
		for (int i = 0; i < e->narms; i++)
			if (e->arms[i].is_wild) { wild = i; break; }
		for (int i = 0; i < e->narms; i++) {
			MatchArm *a = &e->arms[i];
			if (a->is_wild)
				continue;
			/* An or-pattern (`A | B | …`) tests each alternative tag in turn; any hit
			 * jumps to the shared body block, and only when none matches does control
			 * fall through to the next arm. */
			int hit = ex->lbl++;
			for (int k = 0; k < a->nalts; k++) {
				int c = ex->tmp++;
				fprintf(out, "\t%%t%d =w ceqw %%t%d, %d\n", c, tagt, a->tags[k]);
				if (k + 1 < a->nalts) {
					int altx = ex->lbl++;
					fprintf(out, "\tjnz %%t%d, @marm%d, @malt%d\n", c, hit, altx);
					fprintf(out, "@malt%d\n", altx); /* next alternative's test */
				} else {
					int nxt = ex->lbl++;
					fprintf(out, "\tjnz %%t%d, @marm%d, @mnext%d\n", c, hit, nxt);
					fprintf(out, "@marm%d\n", hit);
					/* Load each bound payload field into its `%pb<id>` temp (the scrutinee
					 * `sc` is the boxed union's pointer; field b sits at 8 + b*8). */
					for (int bi = 0; bi < a->nbinds; bi++) {
						if (a->bind_ids[bi] < 0) /* `_` */
							continue;
						int addr = ex->tmp++;
						fprintf(out, "\t%%t%d =l add %s, %d\n", addr, sc, union_payload_offset(bi));
						/* Load a word (Int/tag-only union) or an 8-byte pointer (aggregate). */
						if (a->bind_word[bi])
							fprintf(out, "\t%%pb%d =w loadw %%t%d\n", a->bind_ids[bi], addr);
						else
							fprintf(out, "\t%%pb%d =l loadl %%t%d\n", a->bind_ids[bi], addr);
					}
					char b[96];
					emit_expr(out, a->body, ex, b, sizeof b);
					fprintf(out, "\tstorew %s, %%m%d\n", b, e->slot);
					fprintf(out, "\tjmp @mend%d\n", id);
					fprintf(out, "@mnext%d\n", nxt); /* falls into the next arm, or the default */
				}
			}
		}
		if (wild >= 0) {
			char b[96];
			emit_expr(out, e->arms[wild].body, ex, b, sizeof b);
			fprintf(out, "\tstorew %s, %%m%d\n", b, e->slot);
		} else {
			/* Exhaustive: the fall-through is unreachable for a valid value; store a
			 * defined 0 so the block has a terminator either way. */
			fprintf(out, "\tstorew 0, %%m%d\n", e->slot);
		}
		fprintf(out, "\tjmp @mend%d\n", id);
		fprintf(out, "@mend%d\n", id);
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =w loadw %%m%d\n", r, e->slot);
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_IF: {
		/* if cond then A else B — merge the two branch values through the entry-block
		 * slot `%m<slot>` (a `phi` would need each value's predecessor block, which
		 * nesting makes awkward; a slot store/load is robust and reuses the local
		 * model). The condition is truthy when nonzero (`jnz`). */
		int id = ex->lbl++;
		char c[96];
		emit_expr(out, e->lhs, ex, c, sizeof c);
		fprintf(out, "\tjnz %s, @then%d, @else%d\n", c, id, id);
		fprintf(out, "@then%d\n", id);
		char tb[96];
		emit_expr(out, e->rhs, ex, tb, sizeof tb);
		fprintf(out, "\tstorew %s, %%m%d\n", tb, e->slot);
		fprintf(out, "\tjmp @end%d\n", id);
		fprintf(out, "@else%d\n", id);
		char eb[96];
		emit_expr(out, e->els, ex, eb, sizeof eb);
		fprintf(out, "\tstorew %s, %%m%d\n", eb, e->slot);
		fprintf(out, "\tjmp @end%d\n", id);
		fprintf(out, "@end%d\n", id);
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =w loadw %%m%d\n", r, e->slot);
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_AND:
	case EX_OR: {
		/* Short-circuit, reusing the entry-block merge slot `%m<slot>`. Evaluate lhs;
		 * if it settles the result (false for &&, true for ||) store the constant and
		 * skip rhs, else evaluate rhs and store its truthiness (`cnew b, 0` → 0/1). */
		int id = ex->lbl++;
		int is_and = e->kind == EX_AND;
		char a[96];
		emit_expr(out, e->lhs, ex, a, sizeof a);
		if (is_and)
			fprintf(out, "\tjnz %s, @rhs%d, @sc%d\n", a, id, id);
		else
			fprintf(out, "\tjnz %s, @sc%d, @rhs%d\n", a, id, id);
		fprintf(out, "@sc%d\n", id); /* short-circuit: 0 for &&, 1 for || */
		fprintf(out, "\tstorew %d, %%m%d\n", is_and ? 0 : 1, e->slot);
		fprintf(out, "\tjmp @lend%d\n", id);
		fprintf(out, "@rhs%d\n", id);
		char b[96];
		emit_expr(out, e->rhs, ex, b, sizeof b);
		int bt = ex->tmp++;
		fprintf(out, "\t%%t%d =w cnew %s, 0\n", bt, b); /* b != 0 → 0/1 */
		fprintf(out, "\tstorew %%t%d, %%m%d\n", bt, e->slot);
		fprintf(out, "\tjmp @lend%d\n", id);
		fprintf(out, "@lend%d\n", id);
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =w loadw %%m%d\n", r, e->slot);
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

/* Emit a statement list. Records live in the arena (`%r_<name>`); word locals in
 * stack slots (`%s_<name>`). A `loop` becomes a header block with a back-edge; a
 * `break`/`continue` (bare, or guarded by `if <cond> then …`) jumps to the nearest
 * loop's end/top label. */
static void emit_stmts(FILE *out, Stmt *list, Emit *ex) {
	for (Stmt *s = list; s; s = s->next) {
		char v[96];
		switch (s->kind) {
		case ST_LOCAL:
			if (s->bufsize) {
				/* A `[N Uint8]` byte buffer: bump-allocate N arena bytes; the local
				 * (`%r_<name>`) names the base `*[Uint8]` pointer, uninitialized. */
				fprintf(out, "\t%%r_%s =l call $cf_alloc(w %d)\n", s->name, s->bufsize);
				break;
			}
			if (s->expr->kind == EX_RECORD) {
				/* A record local lives in the arena: bump-allocate its storage
				 * (`%r_<name>` = the returned pointer) and store each field's value at
				 * its offset, in declaration order (ford). */
				DataDecl *d = s->expr->rec;
				fprintf(out, "\t%%r_%s =l call $cf_alloc(w %d)\n", s->name, data_size(d));
				for (int fi = 0; fi < d->nfields; fi++) {
					char fv[96];
					emit_expr(out, s->expr->ford[fi], ex, fv, sizeof fv);
					/* An Int/tag-only-union field stores a word; an aggregate field a pointer. */
					const char *st = type_is_word(s->expr->ford[fi]->rtype) ? "storew" : "storel";
					if (fi == 0) {
						fprintf(out, "\t%s %s, %%r_%s\n", st, fv, s->name);
					} else {
						int a = ex->tmp++;
						fprintf(out, "\t%%t%d =l add %%r_%s, %d\n", a, s->name, data_field_offset(fi));
						fprintf(out, "\t%s %s, %%t%d\n", st, fv, a);
					}
				}
			} else if (s->expr->rtype.kind == TY_RECORD ||
			           (s->expr->rtype.kind == TY_UNION && s->expr->rtype.uni->has_payload)) {
				/* A record or boxed-union local: adopt the initializer's fresh arena
				 * pointer as this local's storage (a move, no copy). */
				emit_expr(out, s->expr, ex, v, sizeof v);
				fprintf(out, "\t%%r_%s =l copy %s\n", s->name, v);
			} else if (s->expr->rtype.kind == TY_STR) {
				/* A Str local holds its header pointer in an `l` slot (reserved in the
				 * entry block); store the literal's static header address into it. */
				emit_expr(out, s->expr, ex, v, sizeof v);
				fprintf(out, "\tstorel %s, %%s_%s\n", v, s->name);
			} else {
				/* A word local: its slot was reserved in the entry block (see
				 * emit_func); the binding just stores the initial value. */
				emit_expr(out, s->expr, ex, v, sizeof v);
				fprintf(out, "\tstorew %s, %%s_%s\n", v, s->name);
			}
			break;
		case ST_ASSIGN:
			emit_expr(out, s->expr, ex, v, sizeof v);
			fprintf(out, "\tstorew %s, %%s_%s\n", v, s->name);
			break;
		case ST_FIELD_ASSIGN:
			/* Mutate a record field: store through the record's arena pointer
			 * (`%r_<name>`) at the field's offset. */
			emit_expr(out, s->expr, ex, v, sizeof v);
			if (s->foff == 0) {
				fprintf(out, "\t%s %s, %%r_%s\n", type_is_word(s->expr->rtype) ? "storew" : "storel", v, s->name);
			} else {
				int a = ex->tmp++;
				fprintf(out, "\t%%t%d =l add %%r_%s, %d\n", a, s->name, s->foff);
				fprintf(out, "\t%s %s, %%t%d\n", type_is_word(s->expr->rtype) ? "storew" : "storel", v, a);
			}
			break;
		case ST_RETURN:
			emit_expr(out, s->expr, ex, v, sizeof v);
			if (ex->ret_uarch && s->expr->rtype.kind == TY_INT) {
				/* Uarch function returning an Int: widen `w`→`l` so the `ret` operand
				 * matches the declared `l` return (a bare `w` temp would be ill-typed). */
				int w = ex->tmp++;
				fprintf(out, "\t%%t%d =w copy %s\n", w, v);
				int l = ex->tmp++;
				fprintf(out, "\t%%t%d =l extsw %%t%d\n", l, w);
				fprintf(out, "\tret %%t%d\n", l);
			} else {
				fprintf(out, "\tret %s\n", v);
			}
			break;
		case ST_LOOP: {
			/* @ltop<id>: body ; jmp @ltop<id> (back-edge) ; @lend<id>: fall-through.
			 * The back-edge is the body's fall-through, so it is only emitted when the
			 * body does not itself end in a divergence (a bare break/continue/return,
			 * which already closed the block); otherwise it would be orphaned. */
			int id = ex->lbl++;
			ex->loops[ex->loop_depth++] = id;
			fprintf(out, "@ltop%d\n", id);
			emit_stmts(out, s->body, ex);
			ex->loop_depth--;
			Stmt *tail = s->body;
			while (tail && tail->next)
				tail = tail->next;
			if (!tail || !stmt_is_terminal(tail)) /* body can fall through → loop back */
				fprintf(out, "\tjmp @ltop%d\n", id);
			fprintf(out, "@lend%d\n", id);
			break;
		}
		case ST_BREAK:
		case ST_CONTINUE: {
			/* Jump to the nearest loop's end (break) or top (continue). A guard
			 * (`if <cond> then …`) branches over the jump; a bare one just jumps. */
			int id = ex->loops[ex->loop_depth - 1];
			const char *dstlbl = s->kind == ST_BREAK ? "lend" : "ltop";
			if (s->expr) {
				char c[96];
				emit_expr(out, s->expr, ex, c, sizeof c);
				int g = ex->lbl++;
				fprintf(out, "\tjnz %s, @ldo%d, @lskip%d\n", c, g, g);
				fprintf(out, "@ldo%d\n", g);
				fprintf(out, "\tjmp @%s%d\n", dstlbl, id);
				fprintf(out, "@lskip%d\n", g);
			} else {
				fprintf(out, "\tjmp @%s%d\n", dstlbl, id);
			}
			break;
		}
		case ST_EXPR:
			/* Evaluate the call for effect; its result temp is simply not used. */
			emit_expr(out, s->expr, ex, v, sizeof v);
			break;
		}
	}
}

/* Give each if/logical expression a distinct merge-slot id, counting them in *n,
 * so emit_func can reserve all merge slots once in the entry block (see Expr.slot).
 * Walks the whole body, including loop bodies and nested sub-expressions. */
static void assign_expr_slots(Expr *e, int *n) {
	if (!e)
		return;
	if (e->kind == EX_IF || e->kind == EX_AND || e->kind == EX_OR || e->kind == EX_MATCH)
		e->slot = (*n)++;
	assign_expr_slots(e->lhs, n);
	assign_expr_slots(e->rhs, n);
	assign_expr_slots(e->els, n);
	for (int i = 0; i < e->nargs; i++)
		assign_expr_slots(e->args[i], n);
	for (int i = 0; i < e->nfields; i++) /* EX_RECORD field-init values */
		assign_expr_slots(e->fvals[i], n);
	for (int i = 0; i < e->narms; i++) /* EX_MATCH arm bodies */
		assign_expr_slots(e->arms[i].body, n);
}

static void assign_stmt_slots(Stmt *list, int *n) {
	for (Stmt *s = list; s; s = s->next) {
		assign_expr_slots(s->expr, n);
		if (s->kind == ST_LOOP)
			assign_stmt_slots(s->body, n);
	}
}

static void emit_func(FILE *out, const Func *fn) {
	/* `main` is the exported entry (returns the Int exit code; darwin hands it
	 * argc/argv/envp in x0/x1/x2 via _start). Other functions are internal. A record
	 * return type lowers to `l` (a pointer to the arena record); Int is `w`. No page
	 * node is threaded yet: a non-allocating body carries none (M0). */
	int is_main = strcmp(fn->name, "main") == 0;
	Type frt = func_ret_type(fn);
	const char *retty = type_is_word(frt) ? "w" : "l"; /* Int/tag-only union → w; else l */
	fprintf(out, "%sfunction %s $%s(", is_main ? "export " : "", retty, fn->name);
	for (int i = 0; i < fn->nparams; i++)
		fprintf(out, "%s%s %%u_%s", i ? ", " : "",
		        param_is_word(&fn->params[i]) ? "w" : "l", fn->params[i].name);
	fprintf(out, ") {\n");
	fprintf(out, "@start\n");

	/* Every Int name lives in a stack slot so `let` reassignment is just a store:
	 * spill each word parameter from its incoming temp into its slot. A record
	 * parameter arrives as a pointer; copy it into the `%r_<name>` form field access
	 * uses (read-only — a param's fields cannot be mutated). Pointer params (main's
	 * argv/envp) are never referenced, so they get no slot. */
	for (int i = 0; i < fn->nparams; i++) {
		const char *n = fn->params[i].name;
		/* A word param (Int or tag-only union tag) spills to a word slot; a record or
		 * boxed-union param arrives as an arena pointer, copied into its `%r_` form. */
		if (param_is_word(&fn->params[i])) {
			fprintf(out, "\t%%s_%s =l alloc4 4\n", n);
			fprintf(out, "\tstorew %%u_%s, %%s_%s\n", n, n);
		} else if (fn->params[i].kind == PK_RECORD ||
		           (fn->params[i].kind == PK_UNION && fn->params[i].uni->has_payload)) {
			fprintf(out, "\t%%r_%s =l copy %%u_%s\n", n, n);
		}
	}

	/* Reserve every word local's stack slot once, here in the entry block — NOT at
	 * its `let` (which may sit inside a loop; a QBE `alloc4` per iteration is an
	 * `alloca` that would overflow the stack). The `let` then only stores. Flat
	 * scoping makes this sound: each name has exactly one slot for the whole body. */
	for (int i = 0; i < fn->nlocals; i++) {
		/* An Int or tag-only union shares the word-slot path; a boxed union (like a
		 * record) needs no slot — it lives in the arena via its `%r_` pointer. */
		if (type_is_word(fn->locals[i].type))
			fprintf(out, "\t%%s_%s =l alloc4 4\n", fn->locals[i].name);
		else if (fn->locals[i].type.kind == TY_STR) /* holds an `l` header pointer */
			fprintf(out, "\t%%s_%s =l alloc8 8\n", fn->locals[i].name);
	}

	/* Likewise reserve every if/logical merge slot once here, not at each
	 * evaluation (which may recur inside a loop). */
	int nslots = 0;
	assign_stmt_slots(fn->body, &nslots);
	for (int i = 0; i < nslots; i++)
		fprintf(out, "\t%%m%d =l alloc4 4\n", i);

	Emit ex = {0};
	ex.ret_uarch = func_ret_type(fn).kind == TY_UARCH;
	emit_stmts(out, fn->body, &ex);
	fprintf(out, "}\n");
}

/* The QBE-level runtime: the root page/arena bump allocator, emitted into the
 * same IL module as the user functions. This is cf0's degenerate two-geometry
 * manifold (page + one arena) collapsed to its bump core (geometry_lowering §5–6):
 * a single global cursor `{cf_top, cf_limit}` over one OS mapping. `cf_alloc`
 * bumps the cursor by an 8-aligned size — the degenerate `on_alloc` — and traps to
 * `cf_oom` when it would pass the limit; there is no per-object free and no
 * teardown (the mapping lives for the whole process). Aggregates (records) home
 * here; scalars stay in stack slots. `cf_top`/`cf_limit` are `export`ed so the asm
 * `_start` can initialize them after the mmap. (cfcc's own codegen — not the cf0
 * `%node`/`%ret` ABI, which cf0.cf will emit; this is a provisional throwaway
 * runtime, like the record layout it serves.) */
static void emit_runtime_qbe(FILE *out) {
	fprintf(out, "export data $cf_top = { l 0 }\n");
	fprintf(out, "export data $cf_limit = { l 0 }\n");
	fprintf(out, "function l $cf_alloc(w %%n) {\n");
	fprintf(out, "@start\n");
	/* Lazily mint the root page on the first allocation: if the cursor is unset
	 * (cf_limit == 0) call the asm `cf_arena_init` (which mmaps + seeds it). This
	 * makes the arena work in BOTH link modes — the freestanding `_start` and the
	 * hosted C runtime both reach `main` without pre-mapping anything. */
	fprintf(out, "\t%%lim0 =l loadl $cf_limit\n");
	fprintf(out, "\t%%uninit =w ceql %%lim0, 0\n");
	fprintf(out, "\tjnz %%uninit, @init, @ready\n");
	fprintf(out, "@init\n");
	fprintf(out, "\tcall $cf_arena_init()\n");
	fprintf(out, "\tjmp @ready\n");
	fprintf(out, "@ready\n");
	fprintf(out, "\t%%r7 =w add %%n, 7\n");
	fprintf(out, "\t%%ra =w and %%r7, -8\n");      /* round the request up to 8 bytes */
	fprintf(out, "\t%%sz =l extuw %%ra\n");
	fprintf(out, "\t%%p =l loadl $cf_top\n");
	fprintf(out, "\t%%nt =l add %%p, %%sz\n");
	fprintf(out, "\t%%lim =l loadl $cf_limit\n");
	fprintf(out, "\t%%ok =w culel %%nt, %%lim\n");  /* new top <= limit ? */
	fprintf(out, "\tjnz %%ok, @ok, @oom\n");
	fprintf(out, "@oom\n");
	fprintf(out, "\tcall $cf_oom()\n");             /* traps (exits); never returns */
	fprintf(out, "\tret %%p\n");
	fprintf(out, "@ok\n");
	fprintf(out, "\tstorel %%nt, $cf_top\n");
	fprintf(out, "\tret %%p\n");
	fprintf(out, "}\n");
}

/* The freestanding asm runtime, emitted beside the QBE-lowered code (asm bypasses
 * QBE; see ebnf Assembly). `_start` is the -nostdlib entry. Darwin arm64 BSD
 * syscall: number in x16, `svc #0x80`, error flagged by the carry bit; SYS_exit=1,
 * SYS_mmap=197. QBE's arm64_apple target underscore-prefixes symbols, so `$main`
 * is `_main` and the exported `$cf_top`/`$cf_limit` are `_cf_top`/`_cf_limit`.
 *
 * `_start` mints the root page — one 64 MiB anonymous mapping (degenerate vs the
 * spec's remap-on-limit page: overflow just traps) — and seeds the bump cursor
 * (cf_top=base, cf_limit=base+size) before running main. argc/argv/envp arrive in
 * x0/x1/x2 under LC_MAIN; the mmap clobbers the arg registers, so they are parked
 * in x19-x21 (which the kernel preserves across `svc`) and restored before the
 * call. main then receives them by arity, exactly as before. */
/* The mode-independent asm floor: `cf_arena_init` mints the root page — one 64 MiB
 * anonymous mmap (degenerate vs the spec's remap-on-limit page: overflow just traps)
 * — and seeds the bump cursor (cf_top=base, cf_limit=base+size). It is a leaf function
 * (no `bl`), called lazily from `cf_alloc` on the first allocation, so the arena comes
 * up the same way whether the freestanding `_start` or the hosted C runtime reaches
 * `main`. `cf_mmap_fail`/`cf_oom` exit with distinct nonzero codes via raw `svc` (an
 * error path — a raw exit is fine in either link mode). SYS_mmap=197, SYS_exit=1. */
static void emit_runtime_common(FILE *out) {
	fprintf(out, ".text\n");
	fprintf(out, ".globl _cf_arena_init\n");
	fprintf(out, ".globl _cf_oom\n");
	fprintf(out, ".p2align 2\n");
	fprintf(out, "_cf_arena_init:\n");
	/* mmap(NULL, 64 MiB, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0) */
	fprintf(out, "\tmov x0, #0\n");
	fprintf(out, "\tmov x1, #0x4000000\n"); /* 64 MiB */
	fprintf(out, "\tmov x2, #3\n");         /* PROT_READ|PROT_WRITE */
	fprintf(out, "\tmov x3, #0x1002\n");    /* MAP_ANON|MAP_PRIVATE (darwin) */
	fprintf(out, "\tmov x4, #-1\n");        /* fd */
	fprintf(out, "\tmov x5, #0\n");         /* offset */
	fprintf(out, "\tmov x16, #197\n");      /* SYS_mmap */
	fprintf(out, "\tsvc #0x80\n");
	fprintf(out, "\tb.cs _cf_mmap_fail\n"); /* carry set on error */
	/* seed the cursor: cf_top = base (x0), cf_limit = base + 64 MiB */
	fprintf(out, "\tadrp x9, _cf_top@PAGE\n");
	fprintf(out, "\tadd x9, x9, _cf_top@PAGEOFF\n");
	fprintf(out, "\tstr x0, [x9]\n");
	fprintf(out, "\tmov x1, #0x4000000\n");
	fprintf(out, "\tadd x11, x0, x1\n");
	fprintf(out, "\tadrp x10, _cf_limit@PAGE\n");
	fprintf(out, "\tadd x10, x10, _cf_limit@PAGEOFF\n");
	fprintf(out, "\tstr x11, [x10]\n");
	fprintf(out, "\tret\n");
	/* mmap failure and arena overflow: exit with distinct nonzero codes. */
	fprintf(out, "_cf_mmap_fail:\n");
	fprintf(out, "\tmov x0, #71\n");
	fprintf(out, "\tmov x16, #1\n");
	fprintf(out, "\tsvc #0x80\n");
	fprintf(out, "_cf_oom:\n");
	fprintf(out, "\tmov x0, #70\n");
	fprintf(out, "\tmov x16, #1\n");
	fprintf(out, "\tsvc #0x80\n");
}

/* The freestanding entry (`--libc none` only). arm64 darwin hands argc/argv/envp in
 * x0/x1/x2 at LC_MAIN entry; with the arena now lazy there is nothing to set up, so
 * `_start` forwards those registers straight into `main` and exits with its Int
 * return. In `--libc dynamic` this is omitted — the C runtime's own `_start` calls
 * `_main` with the same argc/argv/envp ABI. */
static void emit_start(FILE *out) {
	fprintf(out, ".globl _start\n");
	fprintf(out, ".p2align 2\n");
	fprintf(out, "_start:\n");
	fprintf(out, "\tbl _main\n");          /* argc/argv/envp already in x0/x1/x2 */
	fprintf(out, "\tmov x16, #1\n");        /* SYS_exit with main's Int return (x0) */
	fprintf(out, "\tsvc #0x80\n");
}

/* Emit one asm-bodied function verbatim into the assembly output (ebnf § Assembly):
 * `.globl` so the QBE-lowered callers can `bl` it, then the body's segments with
 * each `${param}` replaced by the register that parameter occupies. arm64's C ABI
 * puts argument i in x<i> (parse capped nparams at 8), and the function is naked —
 * no prologue — so at the first instruction the arg registers already hold the
 * args and the body supplies its own `ret`. A `${name}` that is not a parameter is
 * rejected (cfcc has no comptime constants for `${CONST}` yet). */
static void emit_asm_func(FILE *out, const Func *fn) {
	Token *b = fn->asm_body;
	fprintf(out, ".globl _%s\n", fn->name);
	fprintf(out, ".p2align 2\n");
	fprintf(out, "_%s:", fn->name);
	for (int i = 0; i < b->nsegs; i++) {
		StrSeg *sg = &b->segs[i];
		if (sg->kind == SEG_LIT) {
			fwrite(sg->lit, 1, (size_t)sg->litlen, out);
		} else { /* SEG_INTERP: ${param} → its argument register x<index> */
			int idx = -1;
			for (int j = 0; j < fn->nparams; j++)
				if (strcmp(fn->params[j].name, sg->name) == 0) { idx = j; break; }
			if (idx < 0)
				die(b->line, "asm `${...}` must name a parameter "
				             "(M0 has no comptime constants in asm bodies yet)");
			fprintf(out, "x%d", idx);
		}
	}
	fprintf(out, "\n"); /* separate the body from the next symbol */
}

/* Emit every asm function into the runtime .s, beside `_start` and the arena
 * trampolines — not through QBE, which has no `svc`/asm path. */
static void emit_asm_funcs(FILE *out, Program *prog) {
	for (int i = 0; i < prog->nfuncs; i++)
		if (prog->funcs[i]->is_asm)
			emit_asm_func(out, prog->funcs[i]);
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

/* Link mode (`--libc`, cf_cli §4). `static` is a compile error on darwin (Apple has
 * no static libSystem), so cfcc models only the two darwin-valid modes. */
typedef enum { LIBC_NONE, LIBC_DYNAMIC } LinkMode;

static void usage(void) {
	fprintf(stderr, "usage: cfcc c [-o <path>] [--libc none|dynamic] <file.cf>\n");
	exit(2);
}

int main(int argc, char **argv) {
	const char *cmd = NULL;
	const char *input = NULL;
	const char *output = NULL;
	LinkMode libc = LIBC_NONE; /* default: freestanding (cf_cli §4) */

	/* cfcc mirrors the slice of the `cf` CLI that cf0 borrows: the `compile`
	 * command (alias `c`), `-o`, and `--libc` (cf_cli §9). A subcommand is required —
	 * cfcc is a subset of `cf`, so invocation reads the same: `cfcc c file.cf`. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
			if (++i >= argc)
				usage();
			output = argv[i];
		} else if (strcmp(argv[i], "--libc") == 0) {
			/* `--libc <mode>`; a valueless `--libc` (no following mode) means
			 * `dynamic` (cf_cli §4). `static` is a darwin compile error. */
			const char *mode = (i + 1 < argc) ? argv[i + 1] : NULL;
			if (mode && strcmp(mode, "none") == 0)            { libc = LIBC_NONE; i++; }
			else if (mode && strcmp(mode, "dynamic") == 0)    { libc = LIBC_DYNAMIC; i++; }
			else if (mode && strcmp(mode, "static") == 0)
				die(0, "`--libc static` is not supported on darwin (Apple has no static libSystem)");
			else /* valueless --libc */                       libc = LIBC_DYNAMIC;
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
	monomorphize(&prog); /* specialize generic calls before the concrete passes */
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
	emit_runtime_qbe(f); /* the arena allocator + bump cursor, ahead of the user code */
	for (int i = 0; i < prog.nfuncs; i++)
		if (prog.funcs[i]->ntyparams == 0) /* skip generic templates (only clones emit) */
			collect_strlits_stmt(prog.funcs[i]->body);
	emit_string_data(f); /* string-literal data defs, ahead of the functions that ref them */
	for (int i = 0; i < prog.nfuncs; i++)
		if (!prog.funcs[i]->is_asm && prog.funcs[i]->ntyparams == 0) /* asm/templates bypass QBE */
			emit_func(f, prog.funcs[i]);
	fclose(f);

	f = fopen(g_rt_s, "wb");
	if (!f)
		die(0, "cannot write runtime asm");
	emit_runtime_common(f);           /* arena init + error handlers (both link modes) */
	if (libc == LIBC_NONE)
		emit_start(f);            /* freestanding entry; hosted uses the C runtime's */
	emit_asm_funcs(f, &prog); /* user asm functions, verbatim, beside the runtime */
	fclose(f);

	/* qbe: IL -> arm64_apple assembly. */
	{
		char *av[] = {(char *)CF_QBE, "-t", "arm64_apple", "-o", g_main_s, g_qbe_il, NULL};
		run(av);
	}
	/* cc: assemble + link, by `--libc` mode (cf_cli §4).
	 *
	 * `none` (freestanding): `-nostdlib` drops the C startup and default libs — the
	 * binary uses no libc; our `_start` reaches the kernel through raw `svc`. But
	 * darwin's linker refuses a dynamic Mach-O with no libSystem load command ("must
	 * link with libSystem.dylib"), so `-lSystem` is added back solely to satisfy the
	 * loader. No libSystem symbol is referenced (seed_subset §3).
	 *
	 * `dynamic` (hosted): the default link — the C runtime is linked, its own `_start`
	 * calls our exported `main` (`_main`) with the same argc/argv/envp ABI, and C
	 * externs become available. We emit no `_start` of our own (it would clash). The
	 * arena still comes up via the lazy `cf_arena_init` on the first allocation. */
	{
		if (libc == LIBC_NONE) {
			char *av[] = {"cc", "-nostdlib", "-lSystem", "-Wl,-e,_start", "-o", outpath,
			              g_rt_s, g_main_s, NULL};
			run(av);
		} else { /* LIBC_DYNAMIC */
			char *av[] = {"cc", "-o", outpath, g_rt_s, g_main_s, NULL};
			run(av);
		}
	}
	/* No explicit codesign: darwin's `ld` ad-hoc-signs arm64 output by default
	 * (flags: adhoc,linker-signed) and the binary execs as-is, so the trusted
	 * base stays exactly {qbe, cc} (seed_subset §3). A dedicated signing step
	 * would return only if a future link mode emitted unsigned output. */

	fprintf(stderr, "cfcc: wrote %s\n", outpath);
	return 0;
}
