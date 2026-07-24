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
	TK_FLOAT, /* decimal float literal `1.5` (decoded double in Token.fval) */
	TK_STR,   /* "..." string literal (decoded bytes in Token.sval/slen) */
	TK_LPAREN,
	TK_RPAREN,
	TK_LBRACE,
	TK_RBRACE,
	TK_LBRACKET,
	TK_RBRACKET,
	TK_COMMA,
	TK_DOT,     /* . — field access */
	TK_ELLIPSIS,/* ... — spread (record field / member spread) */
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
	/* compound-assignment operators (`x op= y` is sugar for `x = x op y`) — one per
	 * binary op: arithmetic then bitwise, mirroring ebnf § Assignment `assign_op`. */
	TK_PLUSEQ,    /* += */
	TK_MINUSEQ,   /* -= */
	TK_STAREQ,    /* *= */
	TK_SLASHEQ,   /* /= */
	TK_PERCENTEQ, /* %= */
	TK_AMPEQ,     /* &= */
	TK_PIPEEQ,    /* |= */
	TK_CARETEQ,   /* ^= */
	TK_SHLEQ,     /* <<= */
	TK_SHREQ,     /* >>= */
	TK_ARROW, /* -> */
	TK_PIPEGT, /* |> pipe: `x |> f` ≡ `f(x)` (ebnf § pipe) */
	TK_YIELD, /* <- yield: `<- v` yields a block/loop value (ebnf § Control Flow) */
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
	double fval; /* for TK_FLOAT: the decoded double */
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

/* The value of digit `c` in `base` (2/8/10/16), or -1 if it is not a digit of that
 * base. Hex accepts either case (`0xff`, `0xDEAD`). */
static int digit_val(int c, int base) {
	int d;
	if (c >= '0' && c <= '9')
		d = c - '0';
	else if (c >= 'a' && c <= 'f')
		d = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		d = c - 'A' + 10;
	else
		return -1;
	return d < base ? d : -1;
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
			/* A base prefix is lowercase only (`0x`/`0o`/`0b`, ebnf § Numbers); anything
			 * else is a decimal run. A leading `0` alone (or `07`) stays decimal — there is
			 * no C-style leading-zero octal. */
			int base = 10;
			if (s[lx->pos] == '0' && (s[lx->pos + 1] == 'x' || s[lx->pos + 1] == 'o' || s[lx->pos + 1] == 'b')) {
				base = s[lx->pos + 1] == 'x' ? 16 : s[lx->pos + 1] == 'o' ? 8 : 2;
				lx->pos += 2; /* past `0x`/`0o`/`0b` */
			}
			int any = 0, overflow = 0;
			for (;;) {
				int d = digit_val((unsigned char)s[lx->pos], base);
				if (d < 0)
					break;
				/* Defer the range error: this run may turn out to be a float's integer part,
				 * which is decoded by strtod (not `v`) and whose magnitude is unbounded. Keep
				 * `v` well-defined (no signed overflow UB) by not accumulating past the cap. */
				if (v > (LONG_MAX - d) / base)
					overflow = 1;
				else
					v = v * base + d;
				lx->pos++;
				any = 1;
				/* A `_` digit separator is consumed only BETWEEN two digits (ebnf § Numbers:
				 * `{ [ "_" ] , dec_digit }`), so a single `_` with a digit on each side is
				 * skipped; a leading/trailing/doubled `_` is left for the next token (a stray
				 * `_…` then lexes as an identifier → a downstream parse error, never a value). */
				if (s[lx->pos] == '_' && digit_val((unsigned char)s[lx->pos + 1], base) >= 0)
					lx->pos++;
			}
			if (!any) /* `0x`/`0o`/`0b` with no following digit */
				die(lx->line, "expected digits after the base prefix (e.g. `0xff`, `0o17`, `0b101`)");
			/* A decimal integer part followed by `.<digit>` is a FLOAT (`float = dec_run "."
			 * dec_run`, decimal only — no hex/oct/bin float). `1.` (no fractional digit) or
			 * `1.foo` is NOT a float: it stays the integer `1` and a following `.` token, so
			 * field access `rec.x` and integer `1` keep working. Separators apply in the
			 * fractional run too. */
			if (base == 10 && s[lx->pos] == '.' && isdigit((unsigned char)s[lx->pos + 1])) {
				lx->pos++; /* the `.` */
				for (;;) {
					if (!isdigit((unsigned char)s[lx->pos]))
						break;
					lx->pos++;
					if (s[lx->pos] == '_' && isdigit((unsigned char)s[lx->pos + 1]))
						lx->pos++;
				}
				/* Decode via strtod over a `_`-stripped copy of the literal text. */
				char fb[64];
				int fbn = 0;
				for (const char *q = s + start; q < s + lx->pos; q++)
					if (*q != '_' && fbn < (int)sizeof fb - 1)
						fb[fbn++] = *q;
				fb[fbn] = '\0';
				push_tok(lx, TK_FLOAT, s + start, (int)(lx->pos - start), 0);
				lx->toks[lx->ntoks - 1].fval = strtod(fb, NULL);
				continue;
			}
			if (overflow) /* an integer literal (not a float) that exceeded the accumulator */
				die(lx->line, "integer literal out of range");
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
		case '.':
			if (n == '.' && s[lx->pos + 2] == '.') { /* `...` spread; `s` is NUL-terminated so [+2] is safe */
				push_tok(lx, TK_ELLIPSIS, s + lx->pos, 3, 0);
				lx->pos += 3;
			} else {
				push_tok(lx, TK_DOT, s + lx->pos, 1, 0);
				lx->pos++;
			}
			continue;
		case ':': push_tok(lx, TK_COLON, s + lx->pos, 1, 0); lx->pos++; continue;
		case '*':
			if (n == '=') { push_tok(lx, TK_STAREQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_STAR, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '+':
			if (n == '=') { push_tok(lx, TK_PLUSEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_PLUS, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '-': /* -> handled above */
			if (n == '=') { push_tok(lx, TK_MINUSEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_MINUS, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '/':
			if (n == '=') { push_tok(lx, TK_SLASHEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_SLASH, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '%':
			if (n == '=') { push_tok(lx, TK_PERCENTEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_PERCENT, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '^':
			if (n == '=') { push_tok(lx, TK_CARETEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_CARET, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '~': push_tok(lx, TK_TILDE, s + lx->pos, 1, 0); lx->pos++; continue;
		case '&':
			if (n == '&') { push_tok(lx, TK_ANDAND, s + lx->pos, 2, 0); lx->pos += 2; }
			else if (n == '=') { push_tok(lx, TK_AMPEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_AMP, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '|':
			if (n == '|') { push_tok(lx, TK_OROR, s + lx->pos, 2, 0); lx->pos += 2; }
			else if (n == '=') { push_tok(lx, TK_PIPEEQ, s + lx->pos, 2, 0); lx->pos += 2; }
			else if (n == '>') { push_tok(lx, TK_PIPEGT, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_PIPE, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '<':
			if (n == '<' && s[lx->pos + 2] == '=') { push_tok(lx, TK_SHLEQ, s + lx->pos, 3, 0); lx->pos += 3; }
			else if (n == '<') { push_tok(lx, TK_SHL, s + lx->pos, 2, 0); lx->pos += 2; }
			else if (n == '=') { push_tok(lx, TK_LE, s + lx->pos, 2, 0); lx->pos += 2; }
			/* `<-` is the yield lead (ebnf § Control Flow): it only ever begins a yield
			 * statement, never a comparison, so a greedy two-char lex is unambiguous —
			 * `a < -b` is written with a space (or parens) and reads as `a`, `<`, `-b`. */
			else if (n == '-') { push_tok(lx, TK_YIELD, s + lx->pos, 2, 0); lx->pos += 2; }
			else { push_tok(lx, TK_LT, s + lx->pos, 1, 0); lx->pos++; }
			continue;
		case '>':
			if (n == '>' && s[lx->pos + 2] == '=') { push_tok(lx, TK_SHREQ, s + lx->pos, 3, 0); lx->pos += 3; }
			else if (n == '>') { push_tok(lx, TK_SHR, s + lx->pos, 2, 0); lx->pos += 2; }
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

typedef struct Program Program; /* forward: the Parser references the program it builds */

#define MAX_LOOP_DEPTH 64 /* cap on statically-nested loops (bounds Parser/Emit loop stacks) */

typedef struct {
	Token *toks;
	size_t pos;
	int loop_depth; /* how many loops enclose the statement being parsed */
	char loop_val[MAX_LOOP_DEPTH]; /* per-depth: 1 if the enclosing loop yields a value (a
	                                * `loop` in value position), so `<-` is legal and a bare
	                                * `break` is not; 0 for a statement `loop`/`for` */
	int in_defer;   /* 1 while parsing a `defer { … }` block body (no nested defer/return) */
	int in_closure; /* 1 while parsing a closure body (no nested closures in v1) */
	int for_id;     /* monotonic id minting each `for` loop's hidden counter-local name */
	int dest_id;    /* monotonic id minting each destructure's hidden tuple-temp name */
	Program *prog;  /* the program under construction — lets a closure binding append its
	                 * lifted top-level function (set by the top-level parse loop) */
} Parser;

/* The type of a value. M0 has three: `Int` (a word), an opaque pointer (`*T` —
 * argv/envp, never dereferenced), and a named record (`data`) type. A record
 * carries a pointer to its declaration, which fixes its fields and layout; the
 * pointer is filled in by the typecheck pass (parse leaves it NULL). */
typedef struct DataDecl DataDecl;
typedef struct UnionDecl UnionDecl;
typedef struct TupleDecl TupleDecl;
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
	TY_ARRAY,  /* a `[N Int]` FIXED ARRAY — an `l` pointer to N arena elements (an aggregate,
	            * like a record: passed by pointer, fresh-allocated, value semantics, const-only).
	            * The comptime length N rides on `alen`. Index `xs[i]` (runtime i, no bounds
	            * check) and length `xs.len` (comptime constant) read it; a future `for` brick
	            * will iterate it. ⚠ THROWAWAY — cf0 must NOT inherit (type_system §6.2/§7.2):
	            * elements are Int-only in uniform 8-byte slots (cf0's `[N T]` is any element
	            * type, tightly packed); the index is a signed `Int` and `.len` returns `Int`,
	            * whereas §6.2 makes BOTH `Uarch` (cfcc has no Uarch arithmetic, so it stays Int
	            * — an owner-flagged genesis narrowing); `.len` is a PROVISIONAL cfcc surface
	            * spelling (like Str's `.len` — the spec names no array-length accessor, so cf0's
	            * array API may differ); indexing is unchecked (cf0 bounds-checks); the length
	            * must be annotated (cf0 infers `[N T]` from the literal, §7.2); and an array
	            * cannot yet be a parameter, a return, or a record/union field. */
	TY_UNION,  /* a `union` type. M1.1 handles ALL-nullary (tag-only) unions only, which
	            * lower to a plain integer tag (type_system §8.4) — so a union value is a
	            * `w`, stored/read like an Int; the union identity (for match) is `uni`.
	            * Payload unions (tag+aggregate) are a later brick. */
	TY_FN,     /* a function VALUE: `(Int, …) Int` — a capture-free callable, lowered to an
	            * `l` code pointer (memory_model §7: capture-free values stay runtime-dispatched).
	            * Its Int-arity rides on the Expr (`fn_arity`), not here. cfcc restricts function
	            * types to all-`Int` params and an `Int` return; a CAPTURING closure has no
	            * runtime value and is a later (specialization) brick. */
	TY_TUPLE,  /* a tuple `(T0, …, Tn-1)` — a heterogeneous positional product (ebnf § Aggregate
	            * Literals). An `l` pointer to n arena elements in UNIFORM 8-byte slots (like a
	            * record: a word element in the low half of its slot, an aggregate element an
	            * 8-byte pointer). Structural, not nominal — its shape rides on `tup` (interned so
	            * equal shapes share one decl → identity is a pointer compare). Indexed ONLY at a
	            * comptime literal `t[k]` (per-position type; not iterable, no `.len`). ⚠ THROWAWAY
	            * layout, like the record layout: cf0's tuple packs by real element size. */
	TY_F64,    /* Float64 — an IEEE-754 double, QBE `d`. A raw scalar (NaN/±Inf are values,
	            * type_system §2); cfcc has it as a local/param/return/argument and in arithmetic,
	            * comparison, and Int/Uarch↔Float casts. NOT a word — its own load/store/register
	            * path. ⚠ THROWAWAY narrowings (cf0 takes the full tower; all are SUBSET-safe —
	            * cfcc rejects more, never mis-lowers): (1) a float LITERAL is Float64 — a Float32
	            * value comes from an explicit `Float32(…)` cast (no literal-adopts-Float32); (2) an
	            * INT literal does NOT adopt a float context (`const Float64 x = 5` is rejected, use
	            * `5.0` or `Float64(5)`) and an INFERRED float local `const x = 1.5` is rejected
	            * (defaults to Int) — §3 grants both, so like the Uarch precedent cf0 should
	            * WARN/adopt not reject; (3) float `%` is DEFERRED (§5.6 lists it as arithmetic —
	            * spec-legal on floats — cfcc just hasn't wired float `rem`); (4) floats are not yet
	            * aggregate fields/payloads/tuple elements/array elements. */
	TY_F32,    /* Float32 — an IEEE-754 single, QBE `s`. Same story as TY_F64 (its own `s`
	            * load/store/register path, 4-byte slot). A float LITERAL is Float64, so a
	            * Float32 value comes from an explicit `Float32(…)` cast (of a float or an int).
	            * Same THROWAWAY narrowings as TY_F64. */
	TY_UNIT,   /* `Unit` / `()` — the zero-element tuple, the terminal type `1` (type_system §2.3,
	            * §6.1). It carries no information, so cfcc lowers its sole value to a word `0`
	            * (type_is_word ⇒ it rides every word slot/store/return path); it stays a DISTINCT
	            * kind from Int so typecheck never confuses the two (`() + 1` is rejected — Int
	            * contexts test `.kind == TY_INT`, not `type_is_word`). Chiefly a function return
	            * type (`(A) -> Unit`) and a bindable value. */
} TypeKind;

typedef struct {
	TypeKind kind;
	DataDecl *rec;  /* TY_RECORD: the record's declaration */
	UnionDecl *uni; /* TY_UNION: the union's declaration */
	int alen;       /* TY_ARRAY: the comptime element count N (elements are Int) */
	TupleDecl *tup; /* TY_TUPLE: the (interned) tuple shape */
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
	PK_CAPTURE,/* a synthetic parameter of a lifted closure: an enclosing word (Int) local
	            * captured BY REFERENCE. It arrives as an `l` pointer to the caller's word
	            * slot; inside the closure a read is a `loadw` and a write a `storew` through
	            * it, so mutations are visible to the enclosing scope. See lift_closure. */
	PK_CAPTURE_REC,/* a captured enclosing RECORD, by reference. A record already lives in the
	            * arena behind a pointer, so this is that pointer itself (like a PK_RECORD
	            * param) — used as the `%r_<name>` base directly, but MUTABLE: the closure may
	            * assign its fields, and the enclosing scope sees the change. `rec`/`type_name`
	            * carry the record type, resolved in resolve_signatures. */
	PK_FN,     /* a function-VALUE parameter: `(Int, …) Int f` — a higher-order function's
	            * callable argument, an `l` code pointer. Called indirectly inside the body;
	            * the caller passes a capture-free function's address. `fn_arity` is its Int
	            * parameter count. */
	PK_TUPLE,  /* a tuple parameter `(T0, …) name` -> l (a pointer to the caller's arena tuple,
	            * like a record). Read-only in the body, indexed at comptime `name[k]`. Its shape
	            * rides on `tup` (resolved from `tuple_types`/`tuple_n` in resolve_signatures). */
	PK_UNIT,   /* a unit parameter `Unit name` (or `() name`) -> w. The zero-tuple carries no data,
	            * so it arrives as a word `0` and spills to a word slot like an Int (type TY_UNIT). */
	PK_F64,    /* Float64 -> d (an IEEE double passed in a float register; see TY_F64). */
	PK_F32,    /* Float32 -> s (an IEEE single passed in a float register; see TY_F32). */
} ParamKind;

typedef struct Param {
	char name[64];
	ParamKind kind;
	int line;           /* source line of the type (for diagnostics) */
	char type_name[64]; /* PK_RECORD/PK_UNION: the nominal type name (resolved later) */
	DataDecl *rec;      /* PK_RECORD: the resolved declaration */
	UnionDecl *uni;     /* PK_UNION: the resolved declaration */
	int is_ptr;         /* PK_RECORD/PK_UNION: this param is an EXPLICIT pointer `*Aggregate`
	                     * (its Type is TY_PTR to `rec`/`uni`), not the aggregate value itself.
	                     * cfcc represents both as the same arena pointer, so they interconvert.
	                     * ⚠ cf0 must NOT inherit: cf0 keeps a value and its `&`-reference distinct. */
	int fn_arity;       /* PK_FN: the function type's parameter count */
	/* PK_FN: the function type's parameter descriptors (fn_arity of them) and its single
	 * return descriptor — each is a scalar/pointer/nested-function component (aggregates are
	 * a later brick). Heap-allocated; shared read-only when a Param is copied onto a clone
	 * (like `tuple_types`), since a function-type signature is immutable metadata. */
	struct Param *fn_ptypes;
	struct Param *fn_ret;
	/* PK_TUPLE: the element type NAMES (heap array, shared read-only on a generic clone) and
	 * count, interned to `tup` in resolve_signatures. */
	char (*tuple_types)[64];
	int tuple_n;
	TupleDecl *tup;
} Param;

/* Expression AST. M0 expressions are word-valued: literals, references to a
 * bound Int name (a parameter or a local), unary (negate / bitwise-not /
 * logical-not), and the binary arithmetic, bitwise, shift, and comparison ops,
 * all at the ebnf precedence (comparison > bit-or/xor/and > shift > additive >
 * multiplicative). */
typedef enum {
	EX_INT,   /* integer literal (ival) */
	EX_FLOAT, /* float literal (fval) — type Float64 (TY_F64) */
	EX_STR,   /* string literal (sval/slen; strid names its module data) */
	EX_VAR,   /* reference to a bound name (param or local; any type) */
	EX_FIELD, /* record field access: base.name (lhs=base record expr) */
	EX_INDEX, /* array element access: base[index] (lhs=base array, rhs=index Int expr) */
	EX_ARRAY, /* fixed-array literal `[e0, e1, …]` (elements in args/nargs; type `[nargs Int]`) */
	EX_TUPLE, /* tuple literal `(e0, e1, …)` (elements in args/nargs; type `(T0, …)` — heterogeneous).
	           * Reuses the EX_ARRAY arg machinery (every tree walker already recurses `args[]`);
	           * `rtype.tup` carries the interned shape, filled by typecheck. */
	EX_SPREAD,/* a tuple-element spread `...src` (lhs=src, a tuple value). Appears ONLY as an
	           * element of an EX_TUPLE; at typecheck/emit its src tuple's elements are spliced
	           * in place (`(...t, x)` — a comptime desugar, ebnf § Aggregate Literals). */
	EX_UNIT,  /* the unit value `()` — the zero-element tuple (type `Unit`/`()`, TY_UNIT). It holds
	           * no data, so it carries no operands and emits the word constant `0`. */
	EX_RECORD,/* record construction: a data literal { f: v, ... } of type `name` */
	EX_CALL,  /* function call: name(args) */
	EX_CAST,  /* numeric cast Int(x)/Uarch(x) — a scalar conversion (lhs=operand, name=target type) */
	EX_UMEMBER,/* union member value: Union.Member — a tag-only member's tag (uni, ival=tag) */
	EX_MATCH, /* match scrut { arms } — compare-chain tag dispatch (lhs=scrut, arms/narms) */
	EX_IF,    /* if cond then a else b (lhs=cond, rhs=then, els=else) */
	EX_LOOP,  /* loop { body } in VALUE position — an infinite loop that yields a value via
	           * `<- v` (a break-with-value). `loop_body` is the body statement list; `slot`
	           * the entry-block merge slot the yields store into. A statement `loop` (for
	           * effect, no value) stays an ST_LOOP; this is the expression form. */
	EX_DEFER, /* defer <call> — a tapping expression: schedules lhs (an EX_CALL) at scope
	           * exit (LIFO) and evaluates to the call's tapped argument (its last positional
	           * arg). `x |> defer f` builds this too (the pipe fills the tapped slot). */
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
	double fval;      /* EX_FLOAT: the decoded double (type Float64) */
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
	/* EX_RECORD: an optional value-level spread source `{ ...src, ... }` — a bare
	 * record variable whose fields seed this literal (later explicit entries override).
	 * NULL when absent. resolve_record_binding fills each un-overridden `ford` slot with
	 * a synthesized `src.field` read, so emit is unchanged. */
	Expr *spread;
	/* The expression's resolved type, filled by the typecheck pass. Emit reads it to
	 * tell a record value (an `l` arena pointer — passed/returned by pointer) from a
	 * word. */
	Type rtype;
	/* EX_IF/EX_AND/EX_OR/EX_MATCH/EX_LOOP: id of the 4-byte stack slot that merges the
	 * branch/yield values. Assigned per function before emit (assign_stmt_slots) and
	 * allocated once in the entry block, so an if/logical/loop inside a loop does not
	 * `alloc4` each iteration. */
	int slot;
	/* EX_LOOP: the loop body statement list (an expression carrying statements — the only
	 * such shape in cfcc; its `<- v` yields break the loop with a value). */
	struct Stmt *loop_body;
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
	/* EX_CALL: set once this call to a local closure has been rewritten to target the
	 * lifted function (its captures prepended as arguments) — so typecheck does it once. */
	int closure_call;
	/* EX_VAR: a bare name that resolves to a function VALUE (a top-level function or a
	 * capture-free closure), not a local/param — legal only as a `(…) Int` argument.
	 * typecheck rewrites `name` to the emit symbol (a closure's lifted name). EX_CALL:
	 * `indirect` marks a call through a PK_FN parameter (an indirect call). */
	int is_fnref;
	int indirect;
	int fn_arity; /* EX_VAR of TY_FN: the function value's parameter count (for arg checks) */
	char fn_sig[256]; /* EX_VAR of TY_FN: the function value's signature string (see func_value_sig),
	                   * compared against a function-type parameter's expected signature */
	int hof_specialized; /* EX_CALL: this HOF call has been specialized for its capturing-closure args */
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
	ST_FOR,         /* for <var> in <array> { body } — iterate a fixed array (statement-only).
	                 * `name` = the loop variable (a const Int local), `field` = the hidden
	                 * counter local's name, `expr` = the iterable (a `[N Int]` variable),
	                 * `body` = the loop body. Desugared in emit to a counter loop. */
	ST_BREAK,       /* break (bare) or `if cond then break` (guard in expr) */
	ST_CONTINUE,    /* continue (bare) or `if cond then continue` (guard in expr) */
	ST_YIELD,       /* `<- v` (bare) or `if cond then <- v` (guard in expr) — inside a
	                 * value-yielding loop it breaks the loop with value `yval`. `expr`
	                 * holds the optional guard (as for break/continue); `yval` the value. */
	ST_EXPR,        /* an expression evaluated for effect (a call), result discarded */
	ST_DEFER,       /* defer <call> | defer { block } — schedule a cleanup to run at scope
	                 * exit, LIFO. `expr` holds the call (call form); `body` the block form. */
	ST_CLOSURE,     /* `const f = (…) -> …` — a closure declaration. Pure metadata: the
	                 * closure is lifted to a top-level function and recorded in the
	                 * enclosing Func's closure table; this statement emits nothing. */
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
	char bufsize_name[64]; /* ST_LOCAL: a `[n Uint8]` buffer whose length is a comptime
	                        * value parameter (`n`); resolved to `bufsize` at instantiation */
	Expr *expr;         /* ST_LOCAL/ST_ASSIGN/ST_FIELD_ASSIGN: value (NULL for a buffer
	                     * local); ST_RETURN: returned; ST_BREAK/ST_CONTINUE: guard or NULL;
	                     * ST_DEFER: the scheduled call (call form; NULL for the block form) */
	Stmt *body;         /* ST_LOOP: the loop body statement list; ST_DEFER: the block form's
	                     * body (NULL for the call form) */
	Expr *yval;         /* ST_YIELD: the yielded value (`<- yval`); `expr` carries the
	                     * optional guard, mirroring break/continue */
	int destructure_arity; /* ST_LOCAL: >0 marks a destructuring's hidden tuple temp
	                        * (`const (a,b) = e`); the value = the pattern's position count,
	                        * checked against the tuple's arity in typecheck (0 = ordinary) */
	Stmt *next;
};

/* A statement diverges (ends its block unconditionally): a `return`, or a bare
 * `break`/`continue`/`<-`. A guarded break/continue/yield and a loop fall through. */
static int stmt_is_terminal(const Stmt *s) {
	return s->kind == ST_RETURN ||
	       ((s->kind == ST_BREAK || s->kind == ST_CONTINUE || s->kind == ST_YIELD) && !s->expr);
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

/* A `type Name = { Type a, Type b }` grouped-params named tuple (ebnf § Data & Type
 * Declarations): a COMPTIME type hint with no runtime identity whose fields — which
 * are exactly params (`field_decl` = the shape of a `param`) — SPLAT positionally into
 * a parameter list. `(Name x)` desugars to `(Type a, Type b)`; the placeholder name is
 * discarded and each field becomes its own parameter. This is the record-body `type`
 * fork; the plain-alias fork is erased earlier by the token pre-pass. */
typedef struct {
	char name[64];
	Param fields[MAX_PARAMS];
	int nfields;
	int line;
} ParamGroup;
#define MAX_DEFERS 64     /* cap on `defer`s per function (bounds Emit.defers[]) */
#define MAX_CLOSURES 64   /* cap on closures declared in one function (bounds Func.closures[]) */

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

/* A tuple shape `(T0, …, Tn-1)` — a structural, anonymous positional product. Unlike a
 * nominal `data` record it has no name and no source declaration; it is synthesized from a
 * tuple literal's element types and INTERNED (prog_intern_tuple), so two literals of the
 * same shape share one decl and type identity is a `tup` pointer compare. Layout mirrors a
 * record's uniform 8-byte slots (data_field_offset), so tuple emit reuses the record path. */
struct TupleDecl {
	int nelem;
	Type elems[MAX_FIELDS]; /* per-position element type (cfcc brick 1: Int, Str, or a record) */
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
	if (t.kind == TY_UNIT) /* the unit value lowers to a word `0` — see TY_UNIT */
		return 1;
	if (t.kind == TY_UNION)
		return !t.uni->has_payload;
	return 0;
}

/* True if a parameter is passed in a word register: a word Int, or a tag-only union.
 * A boxed (payload) union is passed by pointer, like a record. */
static int param_is_word(const Param *p) {
	return p->kind == PK_WORD || p->kind == PK_UNIT || (p->kind == PK_UNION && !p->uni->has_payload);
}

/* True if a type is a floating-point scalar (Float32/Float64). */
static int is_float_type(Type t) { return t.kind == TY_F32 || t.kind == TY_F64; }

/* The QBE base type of a scalar value: `d`/`s` for a Float64/Float32, `w` for a word
 * (Int/tag-only union/unit), `l` for everything else (a pointer/Uarch/aggregate reference).
 * Used to pick a value's register, load/store (`load%c`/`store%c`), and constant syntax. */
static char qtype_of(Type t) {
	if (t.kind == TY_F64)
		return 'd';
	if (t.kind == TY_F32)
		return 's';
	return type_is_word(t) ? 'w' : 'l';
}
static char param_qtype(const Param *p) {
	if (p->kind == PK_F64)
		return 'd';
	if (p->kind == PK_F32)
		return 's';
	return param_is_word(p) ? 'w' : 'l';
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
	/* A generic slot is a *type* parameter (`'T`, valtype[i] == "") or a *comptime value*
	 * parameter (`Uarch n` / `Int n`, valtype[i] naming its scalar type — type_system §9.1).
	 * A value slot's name (`n`) sits in typarams[i] like a type-var name, so the positional
	 * type-argument mapping and dedup are shared; monomorphization substitutes it with the
	 * concrete integer (an `n` read folds to a literal, a `[n Uint8]` buffer size resolves).
	 * ⚠ THROWAWAY: cfcc value params are Int/Uarch-typed and used only as a value or a
	 * `[n Uint8]` buffer size; cf0 takes the full `[n 'T]` generic-fixed-array grammar.
	 * A value ARGUMENT must be a bare integer literal (`f[8]()`); ebnf `type_arg = expression`
	 * admits any comptime-known expression (a `const`, a comptime call) — cf0 restores that. */
	char valtype[MAX_TYPARAMS][64];
	int ntyparams;
	Param params[MAX_PARAMS];
	int nparams;
	char ret_type_name[64]; /* empty = Int; "Uarch" = Uarch; else a record/union/'T return */
	int ret_line;           /* source line of the return type (for diagnostics) */
	int ret_is_ptr;         /* the return type is an explicit pointer `*Aggregate` (TY_PTR to
	                         * ret_rec/ret_uni); see Param.is_ptr */
	DataDecl *ret_rec;      /* resolved record return type (typecheck) */
	UnionDecl *ret_uni;     /* resolved union return type (typecheck) */
	/* A tuple return type `(T0, …)` — structural, so it has no `ret_type_name`. Parse records
	 * the element type NAMES (like a record's field_types); resolve_signatures interns them
	 * into `ret_tup`. `ret_tuple_n > 0` is the "returns a tuple" flag. */
	char ret_tuple_types[MAX_FIELDS][64];
	int ret_tuple_n;
	TupleDecl *ret_tup;
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
	/* Lexical parent — set only while a closure's body is parsed/capture-analysed, so a
	 * reference to an enclosing name resolves (and is recognised as a capture). Cleared to
	 * NULL once the closure is lifted to a standalone top-level function (see lift_closure). */
	struct Func *parent;
	/* Closures declared as locals in this function's body (`const f = (…) -> …`). Each
	 * records the lifted top-level function it compiles to and the enclosing word variables
	 * it captures by reference, in the order the lifted function takes them as leading
	 * PK_CAPTURE parameters. A call `f(args)` prepends these captures' addresses. */
	struct { char name[64]; struct Func *lifted; char caps[MAX_PARAMS][64]; int ncaps; } closures[MAX_CLOSURES];
	int nclosures;
	/* Caller-facing parameter signature: one slot per WRITTEN parameter. A grouped-params
	 * `type` param is ONE slot (the caller passes a single `{…}` literal, desugared to the
	 * group's field values); an ordinary param is one slot with `callgroups[i] == NULL`.
	 * Used only by the pre-monomorphize group-call desugar; `nparams` stays the splatted
	 * count. `has_group` is 1 iff any slot is a group (else the desugar skips the callee). */
	ParamGroup *callgroups[MAX_PARAMS];
	int ncallslots;
	int has_group;
} Func;

/* The whole program: a set of `data` declarations and a set of functions, one of
 * which is `pub const main`. */
struct Program {
	DataDecl **datas;
	int ndatas, cap_datas;
	UnionDecl **unions;
	int nunions, cap_unions;
	Func **funcs;
	int nfuncs, cap_funcs;
	TupleDecl **tuples; /* interned tuple shapes (structural — one decl per distinct shape) */
	int ntuples, cap_tuples;
	ParamGroup **groups; /* `type { … }` grouped-params named tuples (comptime, splat at param sites) */
	int ngroups, cap_groups;
	int closure_counter; /* mints unique names for lifted closures (`__cf_closure_<n>`) */
};

/* Structural type equality. Scalars match on kind; the nominal aggregates match on their
 * decl pointer (records/unions) or comptime length (arrays); tuples match element-wise (used
 * to intern shapes, so afterwards `a.tup == b.tup` is the fast identity test). */
static int types_equal(Type a, Type b) {
	if (a.kind != b.kind)
		return 0;
	switch (a.kind) {
	case TY_RECORD: return a.rec == b.rec;
	case TY_UNION:  return a.uni == b.uni;
	case TY_ARRAY:  return a.alen == b.alen;
	case TY_TUPLE:
		if (a.tup == b.tup)
			return 1;
		if (a.tup->nelem != b.tup->nelem)
			return 0;
		for (int i = 0; i < a.tup->nelem; i++)
			if (!types_equal(a.tup->elems[i], b.tup->elems[i]))
				return 0;
		return 1;
	default: return 1; /* INT/PTR/STR/UARCH/BUF/FN — kind alone identifies the type */
	}
}

/* ---- Function-value signatures (brick: full function types) ------------------
 * A function VALUE is checked structurally: two functions match iff their parameter kinds
 * and return kind agree. We reduce a signature to a short string — one letter per parameter,
 * a `>`, then the return letter — and compare strings. Nested function types recurse as
 * `(params>ret)`. cfcc's function-type components are scalars, pointers, and nested functions
 * (record/union/tuple components are a later brick), so the letters distinguish exactly those;
 * a non-scalar letter (`R`/`U`/`T`) leaks in only from a passed function that could never
 * match a scalar/pointer slot, which is the mismatch we want. ⚠ cf0 must NOT inherit this
 * letter matching — it carries full nominal function types and matches them structurally
 * against the type-system (a `*Point` and a `*Str` are distinct there, both `p` here). */
static char type_sig_char(Type t) {
	switch (t.kind) {
	case TY_INT:    return 'i';
	case TY_UARCH:  return 'u';
	case TY_F64:    return 'd';
	case TY_F32:    return 's';
	case TY_UNIT:   return 'n';
	case TY_PTR: case TY_BUF: return 'p';
	case TY_FN:     return 'F';
	case TY_RECORD: return 'R';
	case TY_UNION:  return 'U';
	case TY_TUPLE:  return 'T';
	default:        return '?';
	}
}

static void sig_append(char *buf, size_t cap, char c) {
	size_t n = strlen(buf);
	if (n + 1 >= cap)
		/* Fail closed: a silently-truncated signature could compare EQUAL to a different
		 * truncated one and license an unsound width match. A function type this deeply
		 * nested is a genesis limit, not valid input, so reject it outright. */
		die(0, "function type too complex for cfcc (signature exceeds buffer)");
	buf[n] = c;
	buf[n + 1] = 0;
}

static void sig_append_param(const Param *pm, char *buf, size_t cap) {
	switch (pm->kind) {
	case PK_WORD:   sig_append(buf, cap, 'i'); return;
	case PK_UARCH:  sig_append(buf, cap, 'u'); return;
	case PK_F64:    sig_append(buf, cap, 'd'); return;
	case PK_F32:    sig_append(buf, cap, 's'); return;
	case PK_UNIT:   sig_append(buf, cap, 'n'); return;
	case PK_LONG:   sig_append(buf, cap, 'p'); return;
	case PK_RECORD: sig_append(buf, cap, 'R'); return;
	case PK_UNION:  sig_append(buf, cap, 'U'); return;
	case PK_TUPLE:  sig_append(buf, cap, 'T'); return;
	case PK_VAR:    sig_append(buf, cap, 'V'); return;
	case PK_FN:
		sig_append(buf, cap, '(');
		for (int i = 0; i < pm->fn_arity; i++)
			sig_append_param(&pm->fn_ptypes[i], buf, cap);
		sig_append(buf, cap, '>');
		sig_append_param(pm->fn_ret, buf, cap);
		sig_append(buf, cap, ')');
		return;
	default:        sig_append(buf, cap, '?'); return;
	}
}

/* The signature string a function-type PARAMETER expects of its argument. */
static void param_fn_sig(const Param *pm, char *buf, size_t cap) {
	buf[0] = 0;
	for (int i = 0; i < pm->fn_arity; i++)
		sig_append_param(&pm->fn_ptypes[i], buf, cap);
	sig_append(buf, cap, '>');
	sig_append_param(pm->fn_ret, buf, cap);
}

/* The signature string of a top-level function used as a VALUE (a bare function name). */
static Type func_ret_type(const Func *fn);
static void func_value_sig(const Func *g, char *buf, size_t cap) {
	buf[0] = 0;
	for (int i = 0; i < g->nparams; i++)
		sig_append_param(&g->params[i], buf, cap);
	sig_append(buf, cap, '>');
	sig_append(buf, cap, type_sig_char(func_ret_type(g)));
}

/* The record/union a type denotes whether it is the aggregate VALUE (TY_RECORD/TY_UNION) or
 * an explicit pointer to it (TY_PTR to that decl). cfcc represents an aggregate value as its
 * arena pointer, so `Point` and `*Point` share a representation and interconvert freely at
 * calls/returns. ⚠ cf0 must NOT inherit: cf0 keeps a value and its `&`-reference distinct. */
static DataDecl *aggregate_record(Type t) {
	return (t.kind == TY_RECORD || t.kind == TY_PTR) ? t.rec : NULL;
}
static UnionDecl *aggregate_union(Type t) {
	return (t.kind == TY_UNION || t.kind == TY_PTR) ? t.uni : NULL;
}

/* A function-type component descriptor (a scalar/pointer/nested-function type) as a Type —
 * used for the result type of an indirect call and for checking an indirect call's arguments.
 * Aggregates never reach here (rejected when the function type is parsed). */
static Type param_component_type(const Param *pm) {
	switch (pm->kind) {
	case PK_UARCH: return (Type){TY_UARCH, NULL, NULL, 0, NULL};
	case PK_F64:   return (Type){TY_F64, NULL, NULL, 0, NULL};
	case PK_F32:   return (Type){TY_F32, NULL, NULL, 0, NULL};
	case PK_UNIT:  return (Type){TY_UNIT, NULL, NULL, 0, NULL};
	case PK_LONG:  return (Type){TY_PTR, NULL, NULL, 0, NULL};
	case PK_FN:    return (Type){TY_FN, NULL, NULL, 0, NULL};
	default:       return (Type){TY_INT, NULL, NULL, 0, NULL}; /* PK_WORD */
	}
}

/* Intern a tuple shape: return the existing decl for this element-type list, or create and
 * register a fresh one. Interning makes structurally-equal tuples share identity. */
static TupleDecl *prog_intern_tuple(Program *prog, const Type *elems, int n) {
	for (int i = 0; i < prog->ntuples; i++) {
		TupleDecl *t = prog->tuples[i];
		if (t->nelem != n)
			continue;
		int same = 1;
		for (int k = 0; k < n; k++)
			if (!types_equal(t->elems[k], elems[k])) {
				same = 0;
				break;
			}
		if (same)
			return t;
	}
	TupleDecl *t = xmalloc(sizeof *t);
	memset(t, 0, sizeof *t);
	t->nelem = n;
	for (int k = 0; k < n; k++)
		t->elems[k] = elems[k];
	if (prog->ntuples == prog->cap_tuples) {
		prog->cap_tuples = prog->cap_tuples ? prog->cap_tuples * 2 : 8;
		prog->tuples = realloc(prog->tuples, prog->cap_tuples * sizeof *prog->tuples);
		if (!prog->tuples)
			die(0, "out of memory");
	}
	prog->tuples[prog->ntuples++] = t;
	return t;
}

/* Index of a type variable among a function's generic parameters, or -1. */
static int func_typaram_index(const Func *fn, const char *name) {
	for (int i = 0; i < fn->ntyparams; i++)
		if (strcmp(fn->typarams[i], name) == 0)
			return i;
	return -1;
}

/* Index of a comptime value parameter (`Uarch n`) by name among a function's generic
 * parameters, or -1. A value slot is one whose valtype is set. */
static int func_valparam_index(const Func *fn, const char *name) {
	for (int i = 0; i < fn->ntyparams; i++)
		if (fn->valtype[i][0] && strcmp(fn->typarams[i], name) == 0)
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

static ParamGroup *prog_find_group(Program *prog, const char *name) {
	for (int i = 0; i < prog->ngroups; i++)
		if (strcmp(prog->groups[i]->name, name) == 0)
			return prog->groups[i];
	return NULL;
}

static void prog_add_group(Program *prog, ParamGroup *g) {
	if (prog->ngroups == prog->cap_groups) {
		prog->cap_groups = prog->cap_groups ? prog->cap_groups * 2 : 8;
		prog->groups = realloc(prog->groups, prog->cap_groups * sizeof *prog->groups);
		if (!prog->groups)
			die(0, "out of memory");
	}
	prog->groups[prog->ngroups++] = g;
}

/* A grouped-params `type` name is only valid in parameter position (where it splats). If
 * `name` is a group, reject it here with a targeted message instead of a generic
 * unknown-type error — it reached a return/local/field position it may not occupy. */
static void reject_group_as_type(Program *prog, const char *name, int line) {
	if (prog_find_group(prog, name))
		die(line, "a grouped-params `type` may only group parameters, not name a return/local/field type");
}

/* Resolve a concrete field/payload type name to a Type (G3a): "Int" or an aggregate
 * (a `data` record or a `union`). A record and a boxed union are pointer-repr; an Int
 * and a tag-only union are word-repr (see type_is_word). Dies if the name is unknown. */
static TupleDecl *resolve_tuple_shape(Program *prog, char names[][64], int n, int line); /* forward */

static Type resolve_member_type(Program *prog, const char *name, int line) {
	if (name[0] == '*') {
		/* An explicit pointer type `*Aggregate` (ebnf § Types; type_system §6.4 — a pointer
		 * points ONLY to a record or union, never a scalar). cfcc represents it as a TY_PTR
		 * carrying the pointee decl — the very arena pointer an aggregate value already is, so
		 * `*Point` and `Point` share a representation and interconvert. ⚠ cf0 must NOT inherit:
		 * cf0 distinguishes an aggregate value from its `&`-reference. */
		const char *pointee = name + 1;
		UnionDecl *pu = prog_find_union(prog, pointee);
		if (pu) {
			/* Only a payload-bearing (boxed) union is a pointer's referent. An all-tag-only
			 * union lowers to a plain integer, so `*TagOnlyUnion` would be a `*Scalar` — which
			 * §6.4 forbids; type_system §8.4 makes this a PERMANENT type-gate rejection (cf0
			 * rejects it too), not a cfcc narrowing. */
			if (!pu->has_payload)
				die(line, "a pointer to an all-tag-only union is illegal (it would be `*Scalar`, §6.4/§8.4)");
			return (Type){TY_PTR, NULL, pu, 0, NULL};
		}
		DataDecl *pd = prog_find_data(prog, pointee);
		if (pd)
			return (Type){TY_PTR, pd, NULL, 0, NULL};
		die(line, "a pointer type `*T` points to a record or union, not a scalar or unknown type (§6.4)");
	}
	if (strcmp(name, "Int") == 0)
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	if (strcmp(name, "Unit") == 0) /* `Unit`/`()` — the zero-tuple, a word `0` field/payload */
		return (Type){TY_UNIT, NULL, NULL, 0, NULL};
	if (name[0] == '(') {
		/* A tuple field/payload type stored as `(T0,T1,…)` (parse_member_type): split the
		 * element names on TOP-LEVEL commas (a comma inside a nested `(…)` stays part of the
		 * element) and intern the shape. */
		char names[MAX_FIELDS][64];
		int n = 0, k = 0, depth = 0;
		for (const char *s = name + 1; *s; s++) {
			if (*s == '(') {
				depth++;
			} else if (*s == ')') {
				if (depth == 0)
					break; /* the outer close */
				depth--;
			} else if (*s == ',' && depth == 0) {
				names[n][k] = '\0';
				n++;
				k = 0;
				if (n == MAX_FIELDS)
					die(line, "tuple type has too many elements");
				continue;
			}
			if (k == 63)
				die(line, "tuple element type name too long");
			names[n][k++] = *s;
		}
		names[n][k] = '\0';
		n++;
		return (Type){TY_TUPLE, NULL, NULL, 0, resolve_tuple_shape(prog, names, n, line)};
	}
	UnionDecl *u = prog_find_union(prog, name);
	if (u)
		return (Type){TY_UNION, NULL, u, 0, NULL};
	DataDecl *d = prog_find_data(prog, name);
	if (d)
		return (Type){TY_RECORD, d, NULL, 0, NULL};
	reject_group_as_type(prog, name, line);
	char msg[128];
	snprintf(msg, sizeof msg, "unknown field/payload type `%s`", name);
	die(line, msg);
}

/* Resolve a tuple shape from its element type NAMES and intern it. Elements are Int, Str,
 * or a record type (cfcc brick 1 — matching a tuple value's elements). Shared by tuple
 * parameter and tuple return-type resolution. */
static TupleDecl *resolve_tuple_shape(Program *prog, char names[][64], int n, int line) {
	Type elems[MAX_FIELDS];
	for (int j = 0; j < n; j++) {
		const char *tn = names[j];
		if (strcmp(tn, "Int") == 0) {
			elems[j] = (Type){TY_INT, NULL, NULL, 0, NULL};
		} else if (strcmp(tn, "Str") == 0) {
			elems[j] = (Type){TY_STR, NULL, NULL, 0, NULL};
		} else if (strcmp(tn, "Unit") == 0) { /* the unit element `Unit`/`()` — a word `0` */
			elems[j] = (Type){TY_UNIT, NULL, NULL, 0, NULL};
		} else if (tn[0] == '(') { /* a nested tuple element `(A,B)` */
			elems[j] = resolve_member_type(prog, tn, line);
		} else {
			DataDecl *d = prog_find_data(prog, tn);
			if (!d)
				die(line, "a tuple element is Int, Str, Unit, a record, or a nested tuple");
			elems[j] = (Type){TY_RECORD, d, NULL, 0, NULL};
		}
	}
	return prog_intern_tuple(prog, elems, n);
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
			ty->tup = NULL;
			switch (fn->params[i].kind) {
			case PK_WORD:    ty->kind = TY_INT;    ty->rec = NULL; break;
			case PK_RECORD: /* is_ptr → an explicit `*Record` pointer (TY_PTR to the pointee) */
				if (fn->params[i].is_ptr) { ty->kind = TY_PTR; ty->rec = fn->params[i].rec; }
				else { ty->kind = TY_RECORD; ty->rec = fn->params[i].rec; }
				break;
			case PK_LONG:    ty->kind = TY_PTR;    ty->rec = NULL; break;
			case PK_UARCH:   ty->kind = TY_UARCH;  ty->rec = NULL; break;
			case PK_UNION: /* is_ptr → an explicit `*Union` pointer (TY_PTR to the pointee union) */
				ty->kind = fn->params[i].is_ptr ? TY_PTR : TY_UNION;
				ty->rec = NULL;
				ty->uni = fn->params[i].uni;
				break;
			case PK_VAR:     ty->kind = TY_INT;    ty->rec = NULL; break; /* template body parse only; type is ignored (re-typed per instantiation) */
			case PK_CAPTURE: ty->kind = TY_INT;    ty->rec = NULL; return R_LET; /* a by-ref word: readable AND writable */
			case PK_CAPTURE_REC: ty->kind = TY_RECORD; ty->rec = fn->params[i].rec; return R_LET; /* a by-ref record: fields mutable */
			case PK_FN:      ty->kind = TY_FN; ty->rec = NULL; break; /* a function value (arity on the Param) */
			case PK_TUPLE:   ty->kind = TY_TUPLE; ty->rec = NULL; ty->tup = fn->params[i].tup; break; /* by pointer, read-only */
			case PK_UNIT:    ty->kind = TY_UNIT;  ty->rec = NULL; break; /* the unit value, a word `0` */
			case PK_F64:     ty->kind = TY_F64;   ty->rec = NULL; break; /* Float64 -> d */
			case PK_F32:     ty->kind = TY_F32;   ty->rec = NULL; break; /* Float32 -> s */
			}
			return R_PARAM;
		}
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			*ty = fn->locals[i].type;
			return fn->locals[i].mutable ? R_LET : R_CONST;
		}
	/* A closure body resolves an enclosing name through its lexical parent (a capture);
	 * the parent's mutability carries, so assigning a captured `const`/param is still
	 * rejected. Only set while parsing/analysing the closure — NULL for a top-level fn. */
	if (fn->parent)
		return resolve_name(fn->parent, name, ty);
	return R_NONE;
}

/* If `name` binds a closure in this function's scope, return its index in the closure
 * table; else -1. */
static int func_find_closure(const Func *fn, const char *name) {
	for (int i = 0; i < fn->nclosures; i++)
		if (strcmp(fn->closures[i].name, name) == 0)
			return i;
	return -1;
}

/* True if `name` is a PK_CAPTURE parameter of `fn` — a word captured by reference,
 * read/written through its `%u_<name>` pointer rather than a `%s_<name>` word slot. */
static int is_capture_param(const Func *fn, const char *name) {
	if (!fn)
		return 0;
	for (int i = 0; i < fn->nparams; i++)
		if (fn->params[i].kind == PK_CAPTURE && strcmp(fn->params[i].name, name) == 0)
			return 1;
	return 0;
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
 * record local parse passes {TY_RECORD, NULL, NULL, 0}; the typecheck pass backfills rec
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

/* The scalar type names — a pointer must NOT point to one (type_system §6.4, no `*Scalar`). */
static int is_scalar_type_name(const char *name) {
	return is_builtin_type_name(name) || strcmp(name, "Float32") == 0 ||
	       strcmp(name, "Float64") == 0 || strcmp(name, "Unit") == 0;
}

/* Type-reference helpers (G3b) — defined further down, forward-declared here as the type
 * positions that use them (params, union values, locals, returns) come first. */
static void parse_type_arg(Parser *p, char *out, size_t cap);
static void parse_tuple_elem_type(Parser *p, char *out, size_t cap); /* forward: nested tuple types */
static void check_tyvars_declared(const char *mangled, char typarams[][64], int ntp, int line);

/* A function-type component (a parameter type or the return type) may be a scalar
 * (`Int`/`Uarch`/`Float64`/`Float32`/`Unit`), a pointer (`*T`), or a nested function type
 * — every form whose register width is fixed by its kind. Aggregate components (a record,
 * union, or tuple) and a bare type variable (`'T`) are a later brick: reject them cleanly.
 * ⚠ cf0 must NOT inherit this restriction — its function types admit any type. */
static void reject_nonscalar_fn_component(const Param *pm, int line) {
	switch (pm->kind) {
	case PK_WORD: case PK_UARCH: case PK_F64: case PK_F32: case PK_UNIT:
	case PK_LONG: case PK_FN:
		return;
	default:
		die(line, "a function-type parameter/return must be a scalar (`Int`/`Uarch`/`Float64`/"
		          "`Float32`/`Unit`), a pointer (`*T`), or a function type in M0 — record/union/"
		          "tuple/`'T` components are a later brick");
	}
}

/* In a TYPE position, consume an optional `.Member` qualifier after a just-parsed type name
 * — a "member type" (ebnf § Types: `Maybe[Int].Just` names one variant of a union). cfcc
 * COLLAPSES a member type to its union: a member-typed value is represented identically to a
 * union value (a tag + optional payload), so the qualifier is validated then ERASED — `base`
 * (already the union name, or its generic-application mangle like `Maybe.1.Int`) is left
 * unchanged and stands in for the whole type. Validation requires the union to be declared
 * textually-before and to own the member.
 * ⚠ cf0 must NOT inherit: (1) the collapse — cf0 tracks the PRECISE member type and its §8.1
 * subtyping (a `Maybe.Just`-typed slot rejects a `Nothing` value; cfcc, seeing only `Maybe`,
 * accepts it); (2) declared-before — cf0 resolves member types order-independently; (3) only
 * the head-generic spelling `Maybe[Int].Just` is accepted — a member-position generic suffix
 * (`Tree.Node[Int32]`, §8.1) is not parsed here (cfcc unions are head-generic-only anyway). */
static void consume_member_type_suffix(Parser *p, const char *base, int line) {
	if (peek(p)->kind != TK_DOT || !is_type_ident(&p->toks[p->pos + 1]))
		return; /* no `.Member` follows — leave any stray `.` for the caller to diagnose */
	advance(p); /* . */
	char mem[64];
	tok_copy(peek(p), mem, sizeof mem);
	advance(p); /* Member */
	if (!p->prog)
		return; /* a pre-`prog` pass only consumes the tokens; validation waits for `prog` */
	/* The base union name is `base` up to any generic-application mangle (`Maybe.1.Int`→`Maybe`). */
	char bname[64];
	size_t bl = 0;
	while (base[bl] && base[bl] != '.' && bl + 1 < sizeof bname) {
		bname[bl] = base[bl];
		bl++;
	}
	bname[bl] = 0;
	UnionDecl *u = prog_find_union(p->prog, bname);
	if (!u) {
		if (prog_find_data(p->prog, bname))
			die(line, "a member type (`Union.Member`) requires a union base; a record has no members");
		die(line, "a member type (`Union.Member`) needs its union declared before it (a genesis limit)");
	}
	if (union_member_tag(u, mem) < 0)
		die(line, "unknown member in a member-type annotation (`Union.Member`)");
}

/* Consume a parameter's type and classify it. M0 param types are `Int` (a word),
 * a record type (a long — a pointer to the caller's arena record; the type name is
 * stashed and resolved to a decl in typecheck), or a pointer type like `*[Str]` (a
 * long — argv/envp); a pointer's pointee is skipped wholesale since only the
 * top-level shape sets the register width. Fills `out->kind` (and `out->type_name`
 * for a record). */
static void parse_param_type(Parser *p, Param *out) {
	Token *t = peek(p);
	memset(out, 0, sizeof *out); /* a clean slate: `is_ptr` (and every other flag) defaults off
	                             * regardless of how the caller allocated the Param — grouped-
	                             * params fields reuse this via parse_param and would otherwise
	                             * read a garbage `is_ptr`. */
	out->line = t->line;
	if (t->kind == TK_LPAREN) {
		/* A leading `(` at type position opens EITHER a function type `(Int, …) -> Int` OR a
		 * tuple type `(T0, …)` — the `->` after the matching `)` decides (ebnf § Types). Scan
		 * the balanced parens and peek past the close for `->`. */
		int is_fn = 0;
		{
			int depth = 0;
			for (size_t i = p->pos;; i++) {
				TokKind k = p->toks[i].kind;
				if (k == TK_EOF)
					die(t->line, "unterminated `(` in a parameter type");
				if (k == TK_LPAREN) {
					depth++;
				} else if (k == TK_RPAREN && --depth == 0) {
					is_fn = p->toks[i + 1].kind == TK_ARROW;
					break;
				}
			}
		}
		if (!is_fn) {
			/* A tuple parameter type `(T0, …, Tn-1)` — a heterogeneous product passed by
			 * pointer (like a record). Elements are Int, Str, or a record type (brick 1);
			 * resolved + interned in resolve_signatures. An empty `()` is instead the unit
			 * parameter type — the zero-tuple, `Unit` spelled with the crab-claw (§6.1). */
			advance(p); /* ( */
			if (peek(p)->kind == TK_RPAREN) {
				advance(p); /* ) — `() name` is the unit parameter */
				out->kind = PK_UNIT;
				return;
			}
			int cap = 4;
			out->tuple_types = xmalloc(cap * sizeof *out->tuple_types);
			for (;;) {
				if (out->tuple_n == cap) {
					cap *= 2;
					out->tuple_types = realloc(out->tuple_types, cap * sizeof *out->tuple_types);
					if (!out->tuple_types)
						die(0, "out of memory");
				}
				parse_tuple_elem_type(p, out->tuple_types[out->tuple_n++], sizeof out->tuple_types[0]);
				if (peek(p)->kind == TK_COMMA) {
					advance(p);
					continue;
				}
				break;
			}
			expect(p, TK_RPAREN, "expected `)` to close the tuple parameter type");
			if (out->tuple_n < 2)
				die(t->line, "a tuple type needs at least two elements");
			out->kind = PK_TUPLE;
			return;
		}
		/* A function type `(P0, …) -> R` — a higher-order function's callable parameter.
		 * The `->` separates the return type (as in a bare function type). Each parameter
		 * type and the return type is parsed recursively by parse_param_type, so a component
		 * may be any scalar, a pointer, or a nested function type (aggregates rejected). */
		advance(p); /* ( */
		Param *fps = NULL;
		int arity = 0, fcap = 0;
		if (peek(p)->kind != TK_RPAREN)
			for (;;) {
				/* Cap the arity at MAX_PARAMS: a value of this type is called through the
				 * fixed `it[MAX_PARAMS]`/`iw[MAX_PARAMS]` emit arrays, and any function passed
				 * to it is itself parse-capped at MAX_PARAMS — so a larger arity could never be
				 * satisfied anyway, and would overflow those arrays at the indirect call. */
				if (arity == MAX_PARAMS)
					die(t->line, "a function type has too many parameters");
				if (arity == fcap) {
					fcap = fcap ? fcap * 2 : 4;
					fps = realloc(fps, (size_t)fcap * sizeof *fps);
					if (!fps)
						die(t->line, "out of memory");
				}
				memset(&fps[arity], 0, sizeof fps[arity]);
				parse_param_type(p, &fps[arity]);
				reject_nonscalar_fn_component(&fps[arity], t->line);
				arity++;
				if (peek(p)->kind == TK_COMMA) {
					advance(p);
					continue;
				}
				break;
			}
		expect(p, TK_RPAREN, "expected `)` to close the function-type parameters");
		expect(p, TK_ARROW, "a function type separates its return with `->` (e.g. `(Int) -> Int`)");
		Param *fret = xmalloc(sizeof *fret);
		memset(fret, 0, sizeof *fret);
		parse_param_type(p, fret);
		reject_nonscalar_fn_component(fret, t->line);
		out->kind = PK_FN;
		out->fn_arity = arity;
		out->fn_ptypes = fps;
		out->fn_ret = fret;
		return;
	}
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
	if (is_ident(t, "Float64")) {
		advance(p);
		out->kind = PK_F64;
		return;
	}
	if (is_ident(t, "Float32")) {
		advance(p);
		out->kind = PK_F32;
		return;
	}
	if (is_ident(t, "Unit")) { /* `Unit name` — the unit parameter (a word `0`); `()` below is the same */
		advance(p);
		out->kind = PK_UNIT;
		return;
	}
	if (is_ident(t, "Str")) /* Str params await a later brick (Str is a local-only type in M0) */
		die(t->line, "M0 has no Str parameters yet (pass `*[Uint8]` + a `Uarch` length)");
	if (t->kind == TK_STAR) {
		advance(p);
		Token *u = peek(p);
		if (u->kind == TK_LBRACKET) { /* `*[…]` — an OPAQUE buffer pointer (argv/envp, `*[Uint8]`) */
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
			out->kind = PK_LONG;
			return;
		}
		if (!is_type_ident(u))
			die(u->line, "expected a type after `*`");
		/* `*Aggregate` — an EXPLICIT typed pointer to a record/union (§6.4: never a scalar).
		 * Classified like a record param (PK_RECORD → reclassified to PK_UNION in
		 * resolve_signatures if the pointee is a union) but flagged `is_ptr`, so its resolved
		 * type is TY_PTR to the pointee. */
		char pointee[64];
		tok_copy(u, pointee, sizeof pointee);
		if (is_scalar_type_name(pointee))
			die(u->line, "a pointer type `*T` points to a record or union, not a scalar (§6.4)");
		parse_type_arg(p, out->type_name, sizeof out->type_name); /* the pointee (generic ok) */
		out->kind = PK_RECORD;
		out->is_ptr = 1;
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
		consume_member_type_suffix(p, out->type_name, t->line); /* `Maybe[Int].Just` → the union */
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

/* Parse a parenthesized parameter list `( [param {, param}] )` into fn->params — shared
 * by a top-level function and a closure lambda. */
static void parse_paren_params(Parser *p, Func *fn) {
	expect(p, TK_LPAREN, "expected `(`");
	if (peek(p)->kind != TK_RPAREN)
		for (;;) {
			/* A grouped-params `type` name in parameter position SPLATS its fields as
			 * separate parameters in place; the written placeholder name is discarded
			 * (ebnf § Data & Type Declarations). */
			Token *tt = peek(p);
			ParamGroup *g = NULL;
			if (is_type_ident(tt) && p->prog) {
				char tn[64];
				tok_copy(tt, tn, sizeof tn);
				g = prog_find_group(p->prog, tn);
			}
			if (g) {
				advance(p); /* the group type name */
				Token *gn = peek(p);
				if (gn->kind != TK_IDENT || is_type_ident(gn))
					die(gn->line, "expected a placeholder name after a grouped-params type");
				advance(p); /* discard the placeholder — the fields splat as their own params */
				if (fn->ncallslots == MAX_PARAMS)
					die(gn->line, "too many parameters");
				fn->callgroups[fn->ncallslots++] = g; /* one caller slot: a `{…}` literal */
				fn->has_group = 1;
				for (int gi = 0; gi < g->nfields; gi++) {
					if (fn->nparams == MAX_PARAMS)
						die(gn->line, "too many parameters");
					fn->params[fn->nparams] = g->fields[gi];
					for (int j = 0; j < fn->nparams; j++)
						if (strcmp(fn->params[j].name, fn->params[fn->nparams].name) == 0)
							die(gn->line, "a grouped-params field collides with another parameter name");
					fn->nparams++;
				}
			} else {
				if (fn->nparams == MAX_PARAMS)
					die(peek(p)->line, "too many parameters");
				parse_param(p, &fn->params[fn->nparams]);
				for (int j = 0; j < fn->nparams; j++)
					if (strcmp(fn->params[j].name, fn->params[fn->nparams].name) == 0)
						die(p->toks[p->pos - 1].line, "duplicate parameter name");
				fn->callgroups[fn->ncallslots++] = NULL; /* one caller slot: a single param */
				fn->nparams++;
			}
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
	expect(p, TK_RPAREN, "expected `)`");
}

static Expr *parse_expr(Parser *p, Func *fn); /* forward */
static Expr *parse_data_literal(Parser *p, Func *fn, const char *typename, int line); /* forward */
static Stmt *parse_stmt_seq(Parser *p, Func *fn, int require_return, int open_line); /* forward: EX_LOOP body */

/* Disambiguate `name[…]`: a generic call `f[Type](args)` vs an array index `xs[i]`.
 * Assumes the current token is `[`; returns 1 iff the matching `]` is immediately
 * followed by `(` (the call form). Scans the token stream tracking bracket depth;
 * the stream is TK_EOF-terminated so the [i+1] peek past the last `]` is safe. */
static int generic_call_ahead(Parser *p) {
	int depth = 0;
	for (size_t i = p->pos;; i++) {
		TokKind k = p->toks[i].kind;
		if (k == TK_EOF)
			return 0;
		if (k == TK_LBRACKET) {
			depth++;
		} else if (k == TK_RBRACKET) {
			if (--depth == 0)
				return p->toks[i + 1].kind == TK_LPAREN;
		}
	}
}

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
	if (t->kind == TK_FLOAT) {
		advance(p);
		Expr *e = new_expr(EX_FLOAT);
		e->fval = t->fval; /* type Float64 (typeof_expr) */
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
	if (t->kind == TK_LBRACKET) {
		/* Fixed-array literal `[e0, e1, …]` (ebnf aggregate; brackets = arrays only).
		 * Elements are Int expressions; the value's type is `[nargs Int]`. ⚠ deferred:
		 * empty `[]` (needs an annotation to know the element type), element spread
		 * `[...x, …]` (the aggregate-spread tier), and non-Int elements. */
		int line = t->line;
		advance(p); /* [ */
		if (peek(p)->kind == TK_ELLIPSIS)
			die(peek(p)->line, "aggregate spread `[...x]` is not supported yet");
		if (peek(p)->kind == TK_RBRACKET)
			die(peek(p)->line, "an empty array literal `[]` needs a type annotation (not supported yet)");
		Expr *e = new_expr(EX_ARRAY);
		e->line = line;
		int cap = 0;
		for (;;) {
			if (peek(p)->kind == TK_ELLIPSIS)
				die(peek(p)->line, "aggregate spread `[...x]` is not supported yet");
			if (e->nargs == cap) {
				cap = cap ? cap * 2 : 4;
				e->args = realloc(e->args, cap * sizeof *e->args);
				if (!e->args)
					die(0, "out of memory");
			}
			e->args[e->nargs++] = parse_expr(p, fn);
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				if (peek(p)->kind == TK_RBRACKET) /* trailing comma */
					break;
				continue;
			}
			break;
		}
		expect(p, TK_RBRACKET, "expected `]` to close the array literal");
		return e;
	}
	if (t->kind == TK_LBRACE) {
		/* A record literal in EXPRESSION position (`({ x: a, y: b })`), unannotated: its
		 * record type comes from context — the function's return type for a directly-
		 * returned literal `-> ({…})`. Field-checked + typed against that context in
		 * typecheck; a bare `{…}` with no record context is a typecheck error. */
		return parse_data_literal(p, fn, "", t->line);
	}
	if (t->kind == TK_LPAREN) {
		/* `(` opens either a grouped expression `(e)` or a tuple `(e0, e1, …)`. A comma after
		 * the first element — OR a leading `...spread` — forks to the tuple (ebnf § Aggregate
		 * Literals: a one-tuple `(e)` ≅ its element, so a lone plain element, even with a
		 * trailing comma, stays grouping). An element may be a `...src` spread, splicing a
		 * tuple's elements in place. An empty `()` is the unit value (§6.1 — the zero-tuple). */
		int line = t->line;
		advance(p);
		if (peek(p)->kind == TK_RPAREN) {
			advance(p); /* ) — the unit value `()` */
			Expr *u = new_expr(EX_UNIT);
			u->line = line;
			return u;
		}
		int first_spread = peek(p)->kind == TK_ELLIPSIS;
		Expr *first;
		if (first_spread) {
			int sl = peek(p)->line;
			advance(p); /* ... */
			first = new_expr(EX_SPREAD);
			first->line = sl;
			first->lhs = parse_expr(p, fn);
		} else {
			first = parse_expr(p, fn);
		}
		if (!first_spread && peek(p)->kind != TK_COMMA) {
			expect(p, TK_RPAREN, "expected `)`");
			return first; /* a grouped expression */
		}
		Expr *e = new_expr(EX_TUPLE);
		e->line = line;
		int cap = 4;
		e->args = xmalloc(cap * sizeof *e->args);
		e->args[e->nargs++] = first;
		while (peek(p)->kind == TK_COMMA) {
			advance(p); /* , */
			if (peek(p)->kind == TK_RPAREN) /* trailing comma */
				break;
			if (e->nargs == cap) {
				cap *= 2;
				e->args = realloc(e->args, cap * sizeof *e->args);
				if (!e->args)
					die(0, "out of memory");
			}
			if (peek(p)->kind == TK_ELLIPSIS) {
				int sl = peek(p)->line;
				advance(p); /* ... */
				Expr *sp = new_expr(EX_SPREAD);
				sp->line = sl;
				sp->lhs = parse_expr(p, fn);
				e->args[e->nargs++] = sp;
			} else {
				e->args[e->nargs++] = parse_expr(p, fn);
			}
		}
		expect(p, TK_RPAREN, "expected `)` to close the tuple");
		/* `(e,)` — a lone plain element is just its element (grouping); a lone `(...t)` stays
		 * a tuple (it spreads t's ≥2 elements). */
		if (e->nargs == 1 && e->args[0]->kind != EX_SPREAD)
			return e->args[0];
		return e;
	}
	if (is_ident(t, "loop")) {
		/* `loop { … }` in VALUE position (a binding/return right-hand side, or an operand):
		 * an infinite loop that yields a value via `<- v`. Unlike an `if`/`match` expression,
		 * a loop is brace-delimited, so it needs no parenthesization to disambiguate. The
		 * body is parsed like a statement loop but flagged value-yielding, so `<-` is legal
		 * and a bare `break` is not (a value loop exits by yielding). */
		advance(p); /* `loop` */
		if (p->loop_depth >= MAX_LOOP_DEPTH)
			die(t->line, "loops nested too deep");
		expect(p, TK_LBRACE, "expected `{` (a loop body is a block)");
		Expr *e = new_expr(EX_LOOP);
		e->line = t->line;
		p->loop_val[p->loop_depth] = 1;
		p->loop_depth++;
		e->loop_body = parse_stmt_seq(p, fn, 0, t->line); /* no required `return` */
		p->loop_depth--;
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
		/* `name(` is a direct call; `name[Type](` a generic call. A bare `name[i]` NOT
		 * followed by `(` is an ARRAY INDEX — leave `name` as an EX_VAR here and let
		 * parse_postfix consume the `[i]`. */
		if (peek(p)->kind == TK_LPAREN || (peek(p)->kind == TK_LBRACKET && generic_call_ahead(p))) {
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
					if (e->ntypeargs == MAX_TYPARAMS)
						die(ta->line, "too many generic arguments");
					if (ta->kind == TK_INT) { /* a comptime value argument `f[8](...)` */
						if (ta->ival < 0)
							die(ta->line, "a comptime value argument must be non-negative");
					} else if (is_tyvar(ta) || is_type_ident(ta)) {
						if (ta->text[ta->len - 1] == '!')
							die(ta->line, "M0 does not support `!` in a type name");
					} else {
						die(ta->line, "expected a generic argument (a type name, `'T`, or a comptime value)");
					}
					tok_copy(ta, e->typeargs[e->ntypeargs++], sizeof e->typeargs[0]);
					advance(p);
					if (peek(p)->kind == TK_COMMA) {
						advance(p);
						continue;
					}
					break;
				}
				expect(p, TK_RBRACKET, "expected `]` to close the generic arguments");
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
		 * too — its type and storage are settled in typecheck. A comptime value parameter
		 * (`n`) resolves here as well; it survives as an EX_VAR only inside the template
		 * body (never typechecked/emitted) and is folded to its literal in each clone. */
		if (resolve_name(fn, e->name, &ty) == R_NONE && find_active_bind(fn, e->name) < 0 &&
		    func_valparam_index(fn, e->name) < 0) {
			/* A bare name that is a closure or a top-level function is a function VALUE
			 * (a fnref) — legal only as a `(…) Int` argument; typecheck validates that. */
			if (func_find_closure(fn, e->name) >= 0 || prog_find_func(p->prog, e->name))
				e->is_fnref = 1;
			else
				die(line, "unknown name (M0 expressions use integers, parameters, locals, and calls)");
		}
		/* A record value is legal here only as the base of a field access; a
		 * pointer never. Both are caught in the typecheck pass, which knows the
		 * surrounding context, so parse just records the reference. */
		return e;
	}
	if (t->kind == TK_IDENT && is_type_ident(t)) {
		/* A scalar type name applied to an argument is a numeric cast `Int(x)`/`Uarch(x)`
		 * — "the cast is the constructor call `T(x)`" (type_system §4). cfcc has exactly
		 * two scalar integer types, so a widen `Uarch(Int)` (sign-extend, exact) and a
		 * narrow `Int(Uarch)` (truncate the low word) are the only casts. The written cast
		 * licenses the loss, so no diagnostic — unlike cfcc's disclaimed *implicit* Int→Uarch
		 * widening at call args, this is the spec-faithful explicit form cf0 keeps.
		 * ⚠ cf0 must NOT inherit: cfcc REJECTS a narrowing under annotation (`const Int w =
		 * <Uarch>`, via expect_int), forcing this explicit cast; type_system §4 instead treats
		 * `const T x = v` as sugar for `const x = T(v)` and WARNS rather than forbids — cf0
		 * should warn, not reject. */
		if ((is_ident(t, "Int") || is_ident(t, "Uarch") || is_ident(t, "Float64") ||
		     is_ident(t, "Float32")) &&
		    p->toks[p->pos + 1].kind == TK_LPAREN) {
			Expr *e = new_expr(EX_CAST);
			e->line = t->line;
			tok_copy(t, e->name, sizeof e->name); /* target: "Int"/"Uarch"/"Float64"/"Float32" */
			advance(p); /* type name */
			advance(p); /* ( */
			e->lhs = parse_expr(p, fn);
			expect(p, TK_RPAREN, "expected `)` to close the cast");
			return e;
		}
		if (t->text[t->len - 1] == '!')
			die(t->line, "M0 does not support `!` in a type name");
		/* A record type name applied to a `{ … }` payload is record CONSTRUCTION —
		 * `Point({ x: 1, y: 2 })` — construction-is-application (type_system §6.6/§7.3;
		 * the construction mirrors the named-field declaration). The bare context-typed
		 * `{ … }` literal is sugar for exactly this. A PascalCase name directly followed by
		 * `(` (no `.`, not a scalar cast) can only be this — a union value needs `Union.Member`,
		 * a cast is Int/Uarch. ⚠ cf0 must NOT inherit: cfcc has only named-field records, so
		 * the payload is a `{ … }` record literal — positional `Point(1, 2)` (from a tuple decl
		 * `data Point = (Int, Int)`) needs tuples, which cfcc lacks (a later brick). Generic
		 * `Box[Int]({ … })` is deferred too (use the sugar `const Box[Int] b = { … }`). */
		if (p->toks[p->pos + 1].kind == TK_LPAREN) {
			char rname[64];
			tok_copy(t, rname, sizeof rname); /* the record type name */
			advance(p); /* type name */
			advance(p); /* ( */
			if (peek(p)->kind != TK_LBRACE)
				die(peek(p)->line, "record construction takes a `{ … }` payload, e.g. `Point({ x: 1, y: 2 })`");
			Expr *e = parse_data_literal(p, fn, rname, t->line); /* EX_RECORD, name = rname */
			expect(p, TK_RPAREN, "expected `)` to close the record construction");
			return e;
		}
		/* Otherwise a PascalCase name in value position is a union member value
		 * `Union.Member` or `Union[Args].Member` (G3b: a generic union is applied before
		 * the member is selected). A bare type name is not itself a value. */
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
	for (;;) {
		if (peek(p)->kind == TK_DOT) {
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
		} else if (peek(p)->kind == TK_LBRACKET) {
			/* Array index `base[i]` — a `[` reaching postfix is never a generic call
			 * (parse_primary consumes `name[Type](…)`). The index is a full expression. */
			int line = peek(p)->line;
			advance(p); /* [ */
			Expr *ie = new_expr(EX_INDEX);
			ie->line = line;
			ie->lhs = e;
			ie->rhs = parse_expr(p, fn);
			expect(p, TK_RBRACKET, "expected `]` to close the index");
			e = ie;
		} else {
			break;
		}
	}
	return e;
}

static Expr *parse_defer_expr(Parser *p, Func *fn, int allow_bare); /* forward */

/* unary = defer_expr | ("-" | "~" | "!") unary | postfix   (ebnf § unary)
 * A `defer f(x)` tap is a prefix that composes anywhere a value does. */
static Expr *parse_unary(Parser *p, Func *fn) {
	if (is_ident(peek(p), "defer"))
		return parse_defer_expr(p, fn, 0); /* prefix form needs a full call */
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

/* Append `arg` as the final positional argument of an EX_CALL, growing its array
 * by one. Used by the pipe: `x |> f(a, b)` threads `x` on as the last argument. */
static void call_append_arg(Expr *call, Expr *arg) {
	call->args = realloc(call->args, (call->nargs + 1) * sizeof *call->args);
	if (!call->args)
		die(0, "out of memory");
	call->args[call->nargs++] = arg;
}

/* `defer` (in any position) is confined to a function's top level: the deferred
 * work is scheduled statically, so a per-iteration count (inside a loop) or a defer
 * nested in another `defer` block is rejected here. */
static void check_defer_position(Parser *p, int line) {
	if (p->loop_depth != 0)
		die(line, "M0 allows `defer` only at a function's top level, not inside a loop");
	if (p->in_defer)
		die(line, "M0 does not allow `defer` nested inside a `defer` block");
}

/* Parse a callable reference to an EX_CALL: a function name optionally with type
 * arguments and/or an under-applied argument list (`f`, `f(2)`, `f[Int](x)`).
 * `parse_postfix` builds the EX_CALL when a `(` or `[` follows; a bare name has
 * neither, so with `allow_bare` build a zero-argument EX_CALL the caller (a pipe, or
 * a `defer` tap) fills. A bare name can't go through parse_primary — that resolves it
 * as a value and dies on a function name — so this is the one spot that admits it. */
static Expr *parse_callable(Parser *p, Func *fn, int allow_bare) {
	Token *t = peek(p);
	if (t->kind != TK_IDENT || is_type_ident(t))
		die(t->line, "expected a function name (optionally an under-applied call, e.g. `sum(2)`)");
	if (t->text[t->len - 1] == '!')
		die(t->line, "M0 does not support `!` in a name here");
	/* A `(` or `[` after the name → a normal call/type-application; let parse_postfix
	 * build it (and reject a trailing `.field`, which is not a callable target). */
	if (p->toks[p->pos + 1].kind == TK_LPAREN || p->toks[p->pos + 1].kind == TK_LBRACKET) {
		Expr *e = parse_postfix(p, fn);
		if (e->kind != EX_CALL)
			die(t->line, "expected a function call");
		return e;
	}
	if (!allow_bare)
		die(t->line, "a `defer` schedules a function call (`defer f(x)`) or a block (`defer { … }`)");
	Expr *e = new_expr(EX_CALL);
	e->line = t->line;
	tok_copy(t, e->name, sizeof e->name);
	advance(p);
	return e;
}

/* defer_expr = "defer" postfix-call — the tapping expression (ebnf § defer). Builds an
 * EX_DEFER over an EX_CALL: it schedules the call at scope exit and evaluates to the
 * call's tapped argument. `allow_bare` is set only for a pipe target (`x |> defer f`),
 * where the function is named bare and the pipe supplies the tapped argument; a prefix
 * `defer f(x)` needs a full call. The block form `defer { … }` yields no value and is
 * NOT handled here (it is a statement — see parse_stmt). */
static Expr *parse_defer_expr(Parser *p, Func *fn, int allow_bare) {
	Token *t = peek(p); /* `defer` */
	check_defer_position(p, t->line);
	advance(p);
	if (peek(p)->kind == TK_LBRACE)
		die(t->line, "a `defer { … }` block is a statement, not a value — write it on its own line");
	Expr *e = new_expr(EX_DEFER);
	e->line = t->line;
	e->lhs = parse_callable(p, fn, allow_bare);
	return e;
}

/* pipe_target — the right operand of `|>`: a callable (the common case) or a `defer`
 * tap (`x |> defer f` ≡ `defer f(x)`; the pipe fills the tapped slot). A `defer` block
 * is not a pipe target — it taps no value. */
static Expr *parse_pipe_target(Parser *p, Func *fn) {
	if (is_ident(peek(p), "defer"))
		return parse_defer_expr(p, fn, 1); /* bare: the pipe supplies the tapped argument */
	return parse_callable(p, fn, 1);
}

/* pipe = logical_or { "|>" pipe_target }   (left-associative; ebnf § pipe)
 * `x |> f` ≡ `f(x)`: the left operand becomes the target call's final positional
 * argument, so `3 |> sum(2)` is `sum(2, 3)`. A chain may continue on the next line
 * with a leading `|>` — no statement begins with `|>`, so newlines followed by one
 * are the continuation, never a terminator (they are consumed only in that case). */
static Expr *parse_pipe(Parser *p, Func *fn) {
	Expr *e = parse_or(p, fn);
	for (;;) {
		int save = p->pos;
		skip_newlines(p);
		if (peek(p)->kind != TK_PIPEGT) {
			p->pos = save; /* the newlines terminate the statement, not a pipe */
			break;
		}
		advance(p); /* |> */
		skip_newlines(p); /* allow the target on the following line */
		Expr *target = parse_pipe_target(p, fn);
		/* `x |> defer f`: the pipe fills the tapped slot of the *deferred call*, and the
		 * EX_DEFER (which forwards the tapped value) becomes the value flowing on. */
		call_append_arg(target->kind == EX_DEFER ? target->lhs : target, e);
		e = target;
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
				/* The union qualifier is a base name `List` OR a concrete instance `List[Int]`
				 * (parse_type_arg mangles the latter to `List.1.Int`). Both name the scrutinee's
				 * union — typecheck accepts the qual against the union's base AND instance names,
				 * so `List.Cons` and `List[Int].Cons` are interchangeable, and a wrong instance
				 * (`List[Str].Cons` on a `List[Int]`) is rejected. */
				char q[64];
				parse_type_arg(p, q, sizeof q);
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
	return parse_pipe(p, fn);
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
			if (f->kind == TK_ELLIPSIS) {
				/* Value-level record spread `{ ...src, y: 5 }` — splice a record VALUE's
				 * fields, then let later explicit entries override (ebnf field_spread;
				 * type_system §7 "later entries win"). ⚠ THROWAWAY narrowings (cf0 must NOT
				 * inherit): the spread must be the FIRST and ONLY entry (full grammar admits
				 * spreads anywhere and several), and its source is any `...expression` there;
				 * cfcc also requires the source to be a bare same-type record variable with
				 * all-word fields (see resolve_record_binding). The same-type rule sidesteps a
				 * SPEC-SILENT question the ratified docs leave open: when the source is a
				 * DIFFERENT record ("splices another record's fields"), which fields splice —
				 * all, or only shared names? cf0 needs a ratified answer (type_system §7); cfcc
				 * dodges it, so this narrowing is not the eventual cf0 semantics. */
				if (e->spread || e->nfields > 0)
					die(f->line, "a record spread `...src` must be the first and only spread in the literal");
				advance(p); /* `...` */
				e->spread = parse_expr(p, fn);
				if (peek(p)->kind == TK_COMMA) {
					advance(p);
					continue;
				}
				break;
			}
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
			if (peek(p)->kind == TK_COLON) {
				advance(p);
				e->fvals[e->nfields] = parse_expr(p, fn);
			} else {
				/* Field PUN `{ x }` = `{ x: x }` — the field value is the in-scope
				 * variable of the same name (ebnf field_init pun). Synthesize the
				 * `EX_VAR`; it resolves like any name reference in typecheck. */
				Expr *pv = new_expr(EX_VAR);
				pv->line = f->line;
				tok_copy(f, pv->name, sizeof pv->name);
				e->fvals[e->nfields] = pv;
			}
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
static Stmt *parse_body(Parser *p, Func *fn); /* forward */

/* ---- closures (lambda-lifting) -----------------------------------------------
 * A local `const f = (params) [Ret] -> body` is a closure. cfcc lifts it to a
 * standalone top-level function whose leading parameters are the enclosing word
 * variables it captures BY REFERENCE (PK_CAPTURE, an `l` pointer to the caller's slot);
 * a call `f(args)` prepends those captures' addresses. This is a THROWAWAY lowering —
 * cf0 uses the fake-closure model of memory_model.md (captures become hidden params at
 * monomorphization). v1 restrictions: a closure is only *called* (never passed, returned,
 * or stored), captures only `Int` variables, and does not nest inside another closure. */

/* Does the initializer at the cursor open a lambda (`(params) ->`) rather than a
 * parenthesized expression? A lambda's `(` is followed by `)` (no params) or a param
 * `Type name` / `'T name` / `*T name`; `(Int(5))` (a cast) and `(a + b)` are not. */
static int looks_like_lambda(Parser *p) {
	if (peek(p)->kind != TK_LPAREN)
		return 0;
	Token *t1 = &p->toks[p->pos + 1];
	if (t1->kind == TK_RPAREN) {
		/* `()` is a nullary closure only if a body/return-type arrow follows (`() ->` or
		 * `() : Ret ->`); a bare `()` with neither is the unit value (§6.1), an expression. */
		Token *t2 = &p->toks[p->pos + 2];
		return t2->kind == TK_ARROW || t2->kind == TK_COLON;
	}
	if (is_tyvar(t1) || t1->kind == TK_STAR)
		return 1; /* `('T x) ->` / `(*[Str] x) ->` */
	/* `(Type name` — a param, distinct from a cast `(Int(…)` whose type is followed by `(`. */
	Token *t2 = &p->toks[p->pos + 2];
	return is_type_ident(t1) && t2->kind == TK_IDENT && !is_type_ident(t2);
}

/* One captured variable: its name and (for a record) its nominal type. `type_name` is
 * empty for a word capture (a by-reference `Int`) and the record type name otherwise. */
typedef struct { char name[64]; char type_name[64]; } Capture;

/* The nominal type name of an enclosing binding (a record's type, e.g. `Point`), searched
 * up the lexical-parent chain; "" if it is a word or not found. */
static const char *enclosing_type_name(Func *fn, const char *name) {
	for (Func *f = fn; f; f = f->parent) {
		for (int i = 0; i < f->nparams; i++)
			if (strcmp(f->params[i].name, name) == 0)
				return f->params[i].type_name;
		for (int i = 0; i < f->nlocals; i++)
			if (strcmp(f->locals[i].name, name) == 0)
				return f->locals[i].type_name;
	}
	return "";
}

/* Classify a name referenced in a closure body: a closure param/local is not a capture;
 * an enclosing `Int` (word) or record variable is captured by reference (added once, in
 * reference order); anything else (a string, pointer, union) is a v2 error. `is_write`
 * marks a mutating occurrence (an assignment / field mutation) — a captured `const` or
 * parameter cannot be mutated through a closure. */
static void note_capture(Func *cl, const char *name, int is_write, int line,
                         Capture *caps, int *ncaps) {
	for (int i = 0; i < cl->nparams; i++)
		if (strcmp(cl->params[i].name, name) == 0)
			return; /* the closure's own (explicit) parameter */
	for (int i = 0; i < cl->nlocals; i++)
		if (strcmp(cl->locals[i].name, name) == 0)
			return; /* the closure's own local */
	Type ty;
	Resolution r = resolve_name(cl->parent, name, &ty);
	if (r == R_NONE)
		die(line, "unknown name in closure body");
	if (ty.kind != TY_INT && ty.kind != TY_RECORD)
		die(line, "M0 closures capture only `Int` and record variables by reference (not strings, pointers, or unions)");
	/* A word write to a captured `const` is already rejected while parsing the closure body
	 * (resolve chains to the parent); a record FIELD write is not, so enforce it here. */
	if (is_write && r != R_LET)
		die(line, "a closure cannot mutate a captured `const` binding or parameter");
	for (int i = 0; i < *ncaps; i++)
		if (strcmp(caps[i].name, name) == 0)
			return; /* already captured */
	if (*ncaps >= MAX_PARAMS)
		die(line, "closure captures too many variables");
	Capture *c = &caps[(*ncaps)++];
	snprintf(c->name, sizeof c->name, "%s", name);
	snprintf(c->type_name, sizeof c->type_name, "%s",
	         ty.kind == TY_RECORD ? enclosing_type_name(cl->parent, name) : "");
}

static void collect_captures_stmt(Func *cl, Stmt *s, Capture *caps, int *ncaps);

static void collect_captures_expr(Func *cl, Expr *e, Capture *caps, int *ncaps) {
	if (!e)
		return;
	if (e->kind == EX_VAR) /* a value reference — a call's callee name is NOT a capture */
		note_capture(cl, e->name, 0, e->line, caps, ncaps);
	collect_captures_expr(cl, e->lhs, caps, ncaps);
	collect_captures_expr(cl, e->rhs, caps, ncaps);
	collect_captures_expr(cl, e->els, caps, ncaps);
	for (int i = 0; i < e->nargs; i++)
		collect_captures_expr(cl, e->args[i], caps, ncaps);
	for (int i = 0; i < e->nfields; i++)
		collect_captures_expr(cl, e->fvals[i], caps, ncaps);
	if (e->spread) /* value-level record spread source */
		collect_captures_expr(cl, e->spread, caps, ncaps);
	for (int i = 0; i < e->narms; i++)
		collect_captures_expr(cl, e->arms[i].body, caps, ncaps);
	collect_captures_stmt(cl, e->loop_body, caps, ncaps); /* EX_LOOP body (NULL otherwise) */
}

static void collect_captures_stmt(Func *cl, Stmt *s, Capture *caps, int *ncaps) {
	for (; s; s = s->next) {
		if (s->kind == ST_ASSIGN || s->kind == ST_FIELD_ASSIGN)
			note_capture(cl, s->name, 1, s->line, caps, ncaps); /* a write target is a capture too */
		collect_captures_expr(cl, s->expr, caps, ncaps);
		collect_captures_expr(cl, s->yval, caps, ncaps); /* ST_YIELD value (NULL otherwise) */
		collect_captures_stmt(cl, s->body, caps, ncaps); /* loop / defer-block bodies */
	}
}

/* Turn a parsed closure `cl` (its explicit params + body) into a standalone top-level
 * function: prepend a capture parameter per captured variable (PK_CAPTURE for a word,
 * PK_CAPTURE_REC for a record — both by reference), give it a unique name, detach its
 * lexical parent, and append it to the program. Record the binding (name → lifted
 * function + capture list) in the enclosing function for call sites. */
static void lift_closure(Parser *p, Func *encl, Func *cl, const char *localname, int line,
                         Capture *caps, int ncaps) {
	int nexp = cl->nparams;
	if (nexp + ncaps > MAX_PARAMS)
		die(line, "closure has too many parameters and captures combined");
	for (int i = nexp - 1; i >= 0; i--) /* shift explicit params right to make room */
		cl->params[i + ncaps] = cl->params[i];
	for (int i = 0; i < ncaps; i++) {
		Param *pm = &cl->params[i];
		memset(pm, 0, sizeof *pm);
		pm->line = line;
		snprintf(pm->name, sizeof pm->name, "%s", caps[i].name);
		if (caps[i].type_name[0]) { /* a record capture — resolved to a decl in resolve_signatures */
			pm->kind = PK_CAPTURE_REC;
			snprintf(pm->type_name, sizeof pm->type_name, "%s", caps[i].type_name);
		} else {
			pm->kind = PK_CAPTURE;
		}
	}
	cl->nparams = nexp + ncaps;
	snprintf(cl->name, sizeof cl->name, "__cf_closure_%d", p->prog->closure_counter++);
	cl->parent = NULL; /* now a standalone function; captures reach it as capture params */
	cl->is_pub = 0;
	prog_add_func(p->prog, cl);
	if (encl->nclosures >= MAX_CLOSURES)
		die(line, "too many closures in one function");
	int k = encl->nclosures++;
	snprintf(encl->closures[k].name, sizeof encl->closures[k].name, "%s", localname);
	encl->closures[k].lifted = cl;
	encl->closures[k].ncaps = ncaps;
	for (int i = 0; i < ncaps; i++)
		snprintf(encl->closures[k].caps[i], 64, "%s", caps[i].name);
}

/* Parse an optional return-type annotation `: <type>` that sits between a parameter list
 * and the `->` body arrow (ebnf § Functions). The arrow always introduces the body/value;
 * the return type, when stated, is set off with a colon (`(a): Int -> …`), so `->` keeps a
 * single meaning. Returns 1 if a return type was written. */
static int parse_return_type(Parser *p, Func *fn) {
	if (peek(p)->kind != TK_COLON)
		return 0;
	advance(p); /* : */
	Token *rt = peek(p);
	if (rt->kind == TK_LPAREN) {
		/* A tuple return type `(T0, …, Tn-1)` — a function returning a heterogeneous product
		 * (the multi-value return). Element types are Int, Str, or a record name (brick 1,
		 * matching a tuple value's elements); resolved + interned in resolve_signatures. An
		 * empty `()` is instead the unit return type — the zero-tuple, spelled `Unit` (§6.1). */
		fn->ret_line = rt->line;
		advance(p); /* ( */
		if (peek(p)->kind == TK_RPAREN) {
			advance(p); /* ) — `: ()` reads as the unit return type `Unit` */
			snprintf(fn->ret_type_name, sizeof fn->ret_type_name, "Unit");
			return 1;
		}
		for (;;) {
			if (fn->ret_tuple_n == MAX_FIELDS)
				die(peek(p)->line, "tuple return type has too many elements");
			parse_tuple_elem_type(p, fn->ret_tuple_types[fn->ret_tuple_n++], sizeof fn->ret_tuple_types[0]);
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
		expect(p, TK_RPAREN, "expected `)` to close the tuple return type");
		if (fn->ret_tuple_n < 2)
			die(rt->line, "a tuple return type needs at least two elements");
		return 1;
	}
	if (is_tyvar(rt)) { /* a generic return type `'T` — resolved at specialization */
		tok_copy(rt, fn->ret_type_name, sizeof fn->ret_type_name);
		fn->ret_line = rt->line;
		advance(p);
	} else if (rt->kind == TK_STAR) { /* an explicit pointer return `*Aggregate` (§6.4) */
		fn->ret_line = rt->line;
		advance(p); /* * */
		Token *pt = peek(p);
		if (!is_type_ident(pt))
			die(pt->line, "expected an aggregate type after `*` in a return type");
		char pointee[64];
		tok_copy(pt, pointee, sizeof pointee);
		if (is_scalar_type_name(pointee))
			die(pt->line, "a pointer type `*T` points to a record or union, not a scalar (§6.4)");
		parse_type_arg(p, fn->ret_type_name, sizeof fn->ret_type_name); /* the pointee (generic ok) */
		fn->ret_is_ptr = 1;
	} else if (is_type_ident(rt)) {
		fn->ret_line = rt->line;
		if (is_ident(rt, "Int"))
			advance(p);
		else { /* Uarch, a record/union type, or a generic application `Box[Int]` (G3b) */
			parse_type_arg(p, fn->ret_type_name, sizeof fn->ret_type_name);
			consume_member_type_suffix(p, fn->ret_type_name, rt->line); /* `Maybe[Int].Just` → the union */
		}
	} else {
		die(rt->line, "expected a return type after `:`");
	}
	return 1;
}

/* Parse a closure lambda (the `(params) [: Ret] -> body` after `=`) and lift it, recording
 * the binding under `localname` in the enclosing function `fn`. */
static void parse_closure(Parser *p, Func *fn, const char *localname, int line) {
	if (p->in_closure)
		die(line, "M0 does not support nested closures");
	Func *cl = new_func();
	cl->parent = fn;
	parse_paren_params(p, cl);
	parse_return_type(p, cl); /* optional `: Ret` before the body arrow */
	expect(p, TK_ARROW, "expected `->` (a closure body follows the parameters)");
	p->in_closure = 1;
	cl->body = parse_body(p, cl);
	p->in_closure = 0;
	Capture caps[MAX_PARAMS];
	int ncaps = 0;
	collect_captures_stmt(cl, cl->body, caps, &ncaps);
	lift_closure(p, fn, cl, localname, line, caps, ncaps);
}

/* If `k` is a compound-assignment token (`+=`, `<<=`, …), set `*op` to the binary
 * op it desugars to and return 1; else return 0. `x op= y` is sugar for
 * `x = x op y` (ebnf § Assignment). The compound set mirrors the binary ops
 * one-for-one — no `&&=`/`||=` (short-circuit assign has no `Bool` payoff). */
static int compound_assign_op(TokKind k, ExprKind *op) {
	switch (k) {
	case TK_PLUSEQ:    *op = EX_ADD;  return 1;
	case TK_MINUSEQ:   *op = EX_SUB;  return 1;
	case TK_STAREQ:    *op = EX_MUL;  return 1;
	case TK_SLASHEQ:   *op = EX_DIV;  return 1;
	case TK_PERCENTEQ: *op = EX_REM;  return 1;
	case TK_AMPEQ:     *op = EX_BAND; return 1;
	case TK_PIPEEQ:    *op = EX_BOR;  return 1;
	case TK_CARETEQ:   *op = EX_BXOR; return 1;
	case TK_SHLEQ:     *op = EX_SHL;  return 1;
	case TK_SHREQ:     *op = EX_SHR;  return 1;
	default: return 0;
	}
}

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
		if (peek(p)->kind == TK_LPAREN) {
			/* A tuple destructuring `const (a, _, b) = e` (ebnf § Destructuring). cfcc has no
			 * tuple-TYPE annotations, so a `(` right after `const`/`let` is unambiguously a
			 * tuple pattern. It desugars to a hidden tuple temp bound to `e` (evaluated once)
			 * plus one ordinary binding per named position (`a = .destN[k]`); a `_`/`_n` skip
			 * drops one/n positions. Brick 1 binds Int positions only (a value copy — sound for
			 * `let` too); a Str/record position is reached by indexing `.destN` after skipping. */
			int dline = peek(p)->line;
			advance(p); /* ( */
			int destid = p->dest_id++;
			char hidden[64];
			snprintf(hidden, sizeof hidden, "dtor.%d", destid); /* `.` = untypeable, no user clash */
			Stmt *names[MAX_FIELDS];
			int npos_of[MAX_FIELDS]; /* the tuple position each bound name reads */
			int nnames = 0, pos = 0;
			for (;;) {
				Token *pt = peek(p);
				if (pt->kind != TK_IDENT || is_type_ident(pt))
					die(pt->line, "a tuple pattern binds lowercase names or skips (`_`, `_n`)");
				if (pt->text[0] == '_') {
					/* A skip: bare `_` drops one position, `_n` drops n (only digits may follow). */
					int skip = 1;
					if (pt->len > 1) {
						skip = 0;
						for (int i = 1; i < pt->len; i++) {
							if (pt->text[i] < '0' || pt->text[i] > '9')
								die(pt->line, "a skip is `_` or `_n` (n a count); a name starts with a lowercase letter");
							skip = skip * 10 + (pt->text[i] - '0');
						}
						if (skip == 0)
							die(pt->line, "`_0` skips nothing — omit it");
					}
					pos += skip;
					advance(p);
				} else {
					if (pt->text[pt->len - 1] == '!')
						die(pt->line, "M0 does not support `!` in a name");
					if (nnames == MAX_FIELDS)
						die(pt->line, "too many pattern positions");
					char nm[64];
					tok_copy(pt, nm, sizeof nm);
					Type tmp;
					if (resolve_name(fn, nm, &tmp) != R_NONE || func_find_closure(fn, nm) >= 0)
						die(pt->line, "name already defined (no shadowing in M0)");
					for (int i = 0; i < nnames; i++)
						if (strcmp(names[i]->name, nm) == 0)
							die(pt->line, "duplicate name in the tuple pattern");
					Stmt *b = new_stmt(ST_LOCAL);
					b->line = pt->line;
					snprintf(b->name, sizeof b->name, "%s", nm);
					names[nnames] = b;
					npos_of[nnames] = pos;
					nnames++;
					pos++;
					advance(p);
				}
				if (peek(p)->kind == TK_COMMA) {
					advance(p);
					if (peek(p)->kind == TK_RPAREN) /* trailing comma */
						break;
					continue;
				}
				break;
			}
			expect(p, TK_RPAREN, "expected `)` to close the tuple pattern");
			if (pos < 1)
				die(dline, "a tuple pattern binds at least one position");
			expect(p, TK_EQ, "expected `=`");
			/* The hidden temp holds the whole tuple (const — cfcc tuples are immutable); its
			 * arity is checked against the pattern's position count (`pos`) in typecheck. */
			Stmt *h = new_stmt(ST_LOCAL);
			h->line = dline;
			snprintf(h->name, sizeof h->name, "%s", hidden);
			h->expr = parse_expr(p, fn);
			h->destructure_arity = pos;
			Type wt = {TY_INT, NULL, NULL, 0, NULL};
			func_add_local(fn, hidden, 0, wt, ""); /* provisional word; retyped to the tuple in typecheck */
			/* Chain the element bindings after the hidden temp: `a = .destN[k]`. Each is added
			 * as a provisional word local and validated (Int position) in typecheck. */
			Stmt *tail = h;
			for (int i = 0; i < nnames; i++) {
				Expr *base = new_expr(EX_VAR);
				base->line = dline;
				snprintf(base->name, sizeof base->name, "%s", hidden);
				Expr *idx = new_expr(EX_INT);
				idx->line = dline;
				idx->ival = npos_of[i];
				Expr *ix = new_expr(EX_INDEX);
				ix->line = dline;
				ix->lhs = base;
				ix->rhs = idx;
				names[i]->expr = ix;
				Type it = {TY_INT, NULL, NULL, 0, NULL};
				func_add_local(fn, names[i]->name, mutable, it, "");
				tail->next = names[i];
				tail = names[i];
			}
			return h;
		}
		/* Optional type annotation: `Int` (a word local) or a record type name (a
		 * `data`-typed local). A pointer annotation has no M0 local use. */
		int is_record = 0, is_str = 0, is_buf = 0, bufsize = 0, is_arr = 0, arrlen = 0, is_f64 = 0, is_f32 = 0;
		char bufsize_name[64] = {0}; /* a `[n Uint8]` buffer sized by a comptime value param */
		char rectype[64] = {0};
		Token *tt = peek(p);
		if (tt->kind == TK_STAR)
			die(tt->line, "M0 locals are `Int`, `Str`, a `[N Uint8]` buffer, or a record type, not a pointer");
		if (tt->kind == TK_LBRACKET) {
			/* `[N Uint8]` = a fixed byte buffer (no initializer, `read` fills it); `[N Int]`
			 * = a fixed array (bound to an array literal). Both take a comptime length N.
			 * The buffer is throwaway (no indexing/bounds); the array supports index/`.len`. */
			advance(p); /* [ */
			Token *nt = peek(p);
			int have_lit = 0;
			long litN = 0;
			if (nt->kind == TK_INT) {
				if (nt->ival <= 0)
					die(nt->line, "a `[N …]` length must be a positive comptime integer");
				if (nt->ival > INT32_MAX)
					die(nt->line, "`[N …]` length too large");
				litN = nt->ival;
				have_lit = 1;
				advance(p);
			} else { /* a comptime value parameter (`[n Uint8]`) — resolved at instantiation */
				char nm[64];
				if (nt->kind != TK_IDENT || is_type_ident(nt))
					die(nt->line, "a `[N …]` needs a comptime length: a literal (`[16 Uint8]`) "
					              "or a value parameter (`[n Uint8]`)");
				tok_copy(nt, nm, sizeof nm);
				if (func_valparam_index(fn, nm) < 0)
					die(nt->line, "a `[N …]` length must be a literal or a comptime value parameter");
				snprintf(bufsize_name, sizeof bufsize_name, "%s", nm);
				advance(p);
			}
			Token *et = peek(p);
			if (is_ident(et, "Uint8")) {
				advance(p);
				if (have_lit)
					bufsize = (int)litN;
				is_buf = 1;
			} else if (is_ident(et, "Int")) {
				/* A `[N Int]` fixed array. cfcc's array length must be a literal (a
				 * value-parameter length rides only the `[n Uint8]` buffer path). */
				if (!have_lit)
					die(et->line, "a `[N Int]` array length must be a literal (a value-parameter length is only for `[n Uint8]` buffers in M1)");
				advance(p);
				arrlen = (int)litN;
				is_arr = 1;
			} else {
				die(et->line, "M0 `[N …]` holds `Uint8` (a byte buffer) or `Int` (a fixed array)");
			}
			expect(p, TK_RBRACKET, "expected `]` to close the `[N …]` type");
		} else if (is_type_ident(tt)) {
			if (is_ident(tt, "Int")) {
				advance(p);
			} else if (is_ident(tt, "Str")) {
				is_str = 1;
				advance(p);
			} else if (is_ident(tt, "Uarch")) {
				die(tt->line, "M0 has no `Uarch` locals (Uarch is a parameter/return type)");
			} else if (is_ident(tt, "Float64")) {
				is_f64 = 1;
				advance(p);
			} else if (is_ident(tt, "Float32")) {
				is_f32 = 1;
				advance(p);
			} else { /* a record/union type, or a generic application `Box[Int]` (G3b) */
				parse_type_arg(p, rectype, sizeof rectype);
				consume_member_type_suffix(p, rectype, tt->line); /* `Maybe[Int].Just` → the union */
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
		if (resolve_name(fn, s->name, &ty) != R_NONE || func_find_closure(fn, s->name) >= 0)
			die(name->line, "name already defined (no shadowing in M0)");
		if (is_buf) {
			/* A byte buffer has no initializer: `let [N Uint8] name`. Its N arena bytes
			 * are allocated at emit; the local names the base `*[Uint8]` pointer. */
			if (peek(p)->kind == TK_EQ)
				die(peek(p)->line, "a `[N Uint8]` buffer has no initializer (`read` fills it)");
			s->bufsize = bufsize;
			snprintf(s->bufsize_name, sizeof s->bufsize_name, "%s", bufsize_name);
			Type bt = {TY_BUF, NULL, NULL, 0, NULL};
			func_add_local(fn, s->name, mutable, bt, "");
			return s;
		}
		expect(p, TK_EQ, "expected `=`");
		if (!is_record && !is_str && looks_like_lambda(p)) {
			/* `const f = (params) -> body` — a closure. It is lifted to a top-level
			 * function and recorded in this function's closure table; the ST_LOCAL becomes
			 * an ST_CLOSURE marker that emits nothing. (No annotation: a closure is not an
			 * `Int`/record/Str value.) */
			if (mutable)
				die(name->line, "a closure must be bound with `const` (M0 has no reassignable closures)");
			parse_closure(p, fn, s->name, name->line);
			s->kind = ST_CLOSURE;
			return s;
		}
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
			Type rt = {TY_RECORD, NULL, NULL, 0, NULL};
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
			Type st = {TY_STR, NULL, NULL, 0, NULL};
			func_add_local(fn, s->name, mutable, st, "Str");
		} else if (is_arr) {
			/* `const [N Int] xs = [e0, …]` — a fixed array bound to an array literal
			 * (only; an array-returning call is a later brick). The declared length N
			 * rides on the local's type; typecheck checks the literal's length matches.
			 * The array's N elements live in the arena (fresh alloc at emit), like a
			 * record; `%r_<name>` names the base pointer. ⚠ const-only: element assignment
			 * `xs[i] = v` (a side-effecting lvalue) and whole-array reassignment are later
			 * bricks — a `[N Int]` array is read-only after construction in M1. */
			if (mutable)
				die(name->line, "a `[N Int]` array must be `const` (mutable arrays / element assignment are a later brick)");
			s->expr = parse_expr(p, fn);
			if (s->expr->kind != EX_ARRAY)
				die(name->line, "a `[N Int]` array binds an array literal `[…]` (an array-returning call is not supported yet)");
			Type at = {TY_ARRAY, NULL, NULL, 0, NULL};
			at.alen = arrlen;
			func_add_local(fn, s->name, mutable, at, "");
		} else if (is_f64 || is_f32) {
			/* `const Float64 x = <float expr>` / `const Float32 x = Float32(…)` (or `let` —
			 * floats reassign like Int). Typecheck verifies the initializer matches the
			 * annotated float type. */
			s->expr = parse_expr(p, fn);
			const char *tn = is_f64 ? "Float64" : "Float32";
			snprintf(s->type_name, sizeof s->type_name, "%s", tn);
			Type ft = {is_f64 ? TY_F64 : TY_F32, NULL, NULL, 0, NULL};
			func_add_local(fn, s->name, mutable, ft, tn);
		} else {
			/* A `{` initializer with no type annotation is an attempted record
			 * literal (M0 requires the annotation to know the record's type). */
			if (peek(p)->kind == TK_LBRACE)
				die(peek(p)->line,
				    "a record binding needs a type annotation, e.g. `const Point p = { x: 1 }`");
			s->expr = parse_expr(p, fn); /* initializer: name not yet in scope */
			/* A tuple binds `const` only (like a `[N Int]` array): a mutable tuple, whole-
			 * tuple reassignment, and element assignment are later bricks. The local is added
			 * as a provisional word; typecheck retypes it to the interned tuple shape. */
			if (mutable && s->expr->kind == EX_TUPLE)
				die(name->line, "a tuple must be bound with `const` (a mutable tuple is a later brick)");
			Type it = {TY_INT, NULL, NULL, 0, NULL};
			func_add_local(fn, s->name, mutable, it, "");
		}
		return s;
	}
	if (is_ident(t, "return")) {
		if (p->in_defer)
			die(t->line, "a `defer` block cannot `return` (it runs at scope exit, after the return value is fixed)");
		advance(p);
		Stmt *s = new_stmt(ST_RETURN);
		s->line = t->line;
		/* A bare `return { … }` is ambiguous with a block; parenthesize it as a record
		 * literal expression — `return ({ … })` (typed by the return annotation). */
		if (peek(p)->kind == TK_LBRACE)
			die(peek(p)->line,
			    "parenthesize a returned record literal: `return ({ … })` (a bare `{` reads as a block)");
		/* A valueless `return` is sugar for `return ()` — it yields the unit value (§6.1), for
		 * a `Unit`-returning body. A returned value always rides on the `return`'s own line, so
		 * a newline or block-end `}` right after `return` means no value was written. */
		if (peek(p)->kind == TK_NEWLINE || peek(p)->kind == TK_RBRACE) {
			Expr *u = new_expr(EX_UNIT);
			u->line = t->line;
			s->expr = u;
			*saw_return = 1;
			return s;
		}
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
		p->loop_val[p->loop_depth] = 0; /* a statement loop yields no value */
		p->loop_depth++;
		s->body = parse_stmt_seq(p, fn, 0, t->line); /* no required `return` */
		p->loop_depth--;
		return s;
	}
	if (is_ident(t, "for")) {
		/* for <var> in <array> { body } — iterate a fixed array (ebnf § Loops). cfcc's
		 * `for` is STATEMENT-ONLY (like `loop`): no `<-` value yield, and the body is a
		 * block, not the one-line expression form. break/continue steer it (continue
		 * advances to the next element). ⚠ cf0 must NOT inherit: the iterable is a bare
		 * `[N Int]` variable (the full grammar iterates any expression / any iterable),
		 * the loop var is FUNCTION-scoped not loop-scoped (cfcc's flat scope — reusing a
		 * name across sibling `for`s collides, AND the var stays readable AFTER the loop,
		 * which cf0's loop-scoped binding rejects), and there is no value/one-line form. */
		advance(p);
		Token *vt = peek(p);
		if (vt->kind != TK_IDENT || is_type_ident(vt))
			die(vt->line, "expected a loop variable name after `for`");
		if (vt->text[vt->len - 1] == '!')
			die(vt->line, "M0 does not support `!` in a name");
		char varname[64];
		tok_copy(vt, varname, sizeof varname);
		advance(p);
		if (!is_ident(peek(p), "in"))
			die(peek(p)->line, "expected `in` after the `for` variable (`for x in xs`)");
		advance(p); /* in */
		Stmt *s = new_stmt(ST_FOR);
		s->line = t->line;
		snprintf(s->name, sizeof s->name, "%s", varname);
		s->expr = parse_expr(p, fn); /* the iterable — a bare array variable in cfcc */
		if (s->expr->kind != EX_VAR)
			die(s->line, "M1 `for` iterates a bare array variable (`for x in xs`)");
		/* The loop var is a const Int local; a hidden counter local carries the index.
		 * Both are word locals (their `%s_` slots are hoisted to the entry block). */
		Type ti = {TY_INT, NULL, NULL, 0, NULL};
		Type tmp;
		if (resolve_name(fn, varname, &tmp) != R_NONE || func_find_closure(fn, varname) >= 0)
			die(vt->line, "name already defined (no shadowing in M0)");
		func_add_local(fn, varname, 0, ti, "");        /* const Int loop variable */
		snprintf(s->field, sizeof s->field, "for.i%d", p->for_id++); /* hidden counter name (`.` = untypeable) */
		func_add_local(fn, s->field, 1, ti, "");       /* let Int hidden counter */
		if (p->loop_depth >= MAX_LOOP_DEPTH)
			die(t->line, "loops nested too deep");
		expect(p, TK_LBRACE, "expected `{` (a `for` body is a block)");
		p->loop_val[p->loop_depth] = 0; /* a statement `for` yields no value */
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
		if (is_break && p->loop_val[p->loop_depth - 1])
			die(t->line, "a value-yielding loop exits by yielding a value: `<- v`, not a bare `break`");
		Stmt *s = new_stmt(is_break ? ST_BREAK : ST_CONTINUE);
		s->line = t->line; /* bare — no guard, no label in M0 */
		return s;
	}
	if (peek(p)->kind == TK_YIELD) {
		/* `<- v` — yield a value from the enclosing value-loop (a break-with-value). Terminal,
		 * like `break`. Legal only when the nearest loop yields a value (a `loop` in value
		 * position); a statement `loop`/`for` has no slot to yield into. */
		advance(p); /* `<-` */
		if (p->loop_depth == 0 || !p->loop_val[p->loop_depth - 1])
			die(t->line, "`<- v` yields a loop's value, so it needs a value-yielding loop "
			             "(a `loop` in value position); a statement `loop`/`for` has no value");
		Stmt *s = new_stmt(ST_YIELD);
		s->line = t->line;
		s->yval = parse_expr(p, fn);
		return s;
	}
	if (is_ident(t, "if")) {
		/* In statement position `if` guards a loop control: `if <cond> then break`,
		 * `if <cond> then continue`, or `if <cond> then <- v` (a guarded value-yield).
		 * The value-`if` is an expression — it appears on a binding/return right-hand
		 * side, never as a bare statement. */
		advance(p);
		Expr *cond = parse_expr(p, fn);
		if (!is_ident(peek(p), "then"))
			die(peek(p)->line, "expected `then`");
		advance(p);
		if (peek(p)->kind == TK_YIELD) {
			/* `if <cond> then <- v` — a guarded yield: exit the value-loop with `v` when
			 * the guard holds, else fall through. Not terminal (the guard may be false). */
			int yline = peek(p)->line;
			advance(p); /* `<-` */
			if (p->loop_depth == 0 || !p->loop_val[p->loop_depth - 1])
				die(yline, "`<- v` yields a loop's value, so it needs a value-yielding loop "
				           "(a `loop` in value position); a statement `loop`/`for` has no value");
			Stmt *s = new_stmt(ST_YIELD);
			s->line = t->line;
			s->expr = cond;                /* guard */
			s->yval = parse_expr(p, fn);   /* yielded value */
			return s;
		}
		Token *ctl = peek(p);
		int is_break = is_ident(ctl, "break");
		if (!is_break && !is_ident(ctl, "continue"))
			die(ctl->line, "a statement-position `if` guards a `break`, `continue`, or `<- v`");
		advance(p);
		if (p->loop_depth == 0)
			die(ctl->line, "`break`/`continue` is only valid inside a loop");
		if (is_break && p->loop_val[p->loop_depth - 1])
			die(ctl->line, "a value-yielding loop exits by yielding a value: `<- v`, not a bare `break`");
		Stmt *s = new_stmt(is_break ? ST_BREAK : ST_CONTINUE);
		s->line = t->line;
		s->expr = cond; /* guarded */
		return s;
	}
	if (is_ident(t, "defer") && p->toks[p->pos + 1].kind == TK_LBRACE) {
		/* defer block — `defer { … }` schedules a whole block at scope exit (ebnf § defer).
		 * It taps no value and yields nothing, so it is a statement (never a pipe target
		 * or a value). The call/tap form `defer f(x)` is an expression (parse_defer_expr),
		 * handled below as an expression statement. cfcc confines defer to a function's
		 * top level: the deferred work fires at every `return`, LIFO, so a per-iteration
		 * count (inside a loop) or a defer nested in another defer is rejected. */
		check_defer_position(p, t->line);
		advance(p); /* consume `defer` */
		advance(p); /* consume `{` */
		Stmt *s = new_stmt(ST_DEFER);
		s->line = t->line;
		p->in_defer = 1;
		s->body = parse_stmt_seq(p, fn, 0, t->line); /* no required `return` */
		p->in_defer = 0;
		return s;
	}
	if (is_ident(t, "defer")) {
		/* `defer f(x)` (or a pipe tap `x |> defer f`) on its own line: an
		 * expression-statement whose tapped value is discarded. parse_expr reaches
		 * parse_defer_expr via parse_unary and builds the EX_DEFER. */
		Stmt *s = new_stmt(ST_EXPR);
		s->line = t->line;
		s->expr = parse_expr(p, fn);
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
			/* An index expression (`xs[i]`) at statement head followed by `=`/`op=` is an
			 * attempted element assignment — a side-effecting lvalue, a later brick. */
			ExprKind cop;
			if (peek(p)->kind == TK_EQ || compound_assign_op(peek(p)->kind, &cop)) {
				if (s->expr->kind == EX_INDEX)
					die(peek(p)->line, "array element assignment (`xs[i] = …`) is not supported yet (arrays are read-only in M1)");
				die(peek(p)->line, "cannot assign to this expression");
			}
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
			ExprKind cop;
			if (compound_assign_op(peek(p)->kind, &cop)) {
				/* `p.f op= y` desugars to `p.f = p.f op y`. The target has no
				 * side-effecting subexpression (a name + field), so re-reading it
				 * once for the `op` matches "evaluated once for the target".
				 * cf0 must NOT inherit this textual re-read: with a side-effecting
				 * lvalue (`xs[i] op= y`), cf0 must evaluate the target *place* once
				 * and reuse it. cfcc has only name/field targets, so it is sound here. */
				int opline = peek(p)->line;
				advance(p);
				Expr *base = new_expr(EX_VAR);
				base->line = t->line;
				snprintf(base->name, sizeof base->name, "%s", target);
				Expr *fld = new_expr(EX_FIELD);
				fld->line = t->line;
				fld->lhs = base;
				snprintf(fld->name, sizeof fld->name, "%s", s->field);
				Expr *bin = new_expr(cop);
				bin->line = opline;
				bin->lhs = fld;
				bin->rhs = parse_expr(p, fn);
				s->expr = bin;
			} else {
				expect(p, TK_EQ, "expected `=`");
				s->expr = parse_expr(p, fn);
			}
			return s;
		}
		Stmt *s = new_stmt(ST_ASSIGN);
		snprintf(s->name, sizeof s->name, "%s", target);
		Type ty;
		switch (resolve_name(fn, s->name, &ty)) {
		case R_NONE: die(t->line, "unknown name (assign to a declared `let` local)");
		case R_PARAM: die(t->line, "cannot reassign a parameter");
		case R_CONST: die(t->line, "cannot reassign a `const` binding (declare it with `let`)");
		case R_LET: break; /* ok */
		}
		/* Only a scalar `let` in a slot (an Int word or a Float64) can be reassigned as a
		 * unit; a whole record/aggregate cannot — mutate its fields with `.`. (Aggregate
		 * copy-binding is a later concern — memory_model §6 requires an explicit copy.) */
		if (ty.kind != TY_INT && !is_float_type(ty))
			die(t->line, "cannot assign to a whole record (mutate a field with `.`)");
		ExprKind cop;
		if (compound_assign_op(peek(p)->kind, &cop)) {
			/* `x op= y` desugars to `x = x op y` (x a plain `let` word local). */
			int opline = peek(p)->line;
			advance(p);
			Expr *base = new_expr(EX_VAR);
			base->line = t->line;
			snprintf(base->name, sizeof base->name, "%s", target);
			Expr *bin = new_expr(cop);
			bin->line = opline;
			bin->lhs = base;
			bin->rhs = parse_expr(p, fn);
			s->expr = bin;
		} else {
			expect(p, TK_EQ, "expected `=` (M0 statements are const/let, a reassignment, or return)");
			s->expr = parse_expr(p, fn);
		}
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
		while (tail->next) /* a destructuring desugars to a chain — advance past all of it */
			tail = tail->next;
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

	/* Optional generic parameters: `['T, 'U]` before the value parameters. A function
	 * with generic parameters is a template (monomorphized per use). A generic parameter
	 * is a type variable `'T`, a bounded type variable `Union 'T`, or a comptime *value*
	 * parameter `Type name` (`Uarch n`, type_system §9.1). A leading PascalCase name is a
	 * bound when a `'T` follows and the value param's type when a lowercase name follows. */
	if (peek(p)->kind == TK_LBRACKET) {
		advance(p); /* [ */
		for (;;) {
			char lead[64];
			parse_optional_bound(p, lead, sizeof lead); /* a type-var bound OR a value-param type */
			Token *nx = peek(p);
			if (fn->ntyparams == MAX_TYPARAMS)
				die(nx->line, "too many generic parameters");
			if (is_tyvar(nx)) { /* `'T` or `Union 'T` */
				tok_copy(nx, fn->typarams[fn->ntyparams], sizeof fn->typarams[0]);
				snprintf(fn->bounds[fn->ntyparams], sizeof fn->bounds[0], "%s", lead);
				fn->valtype[fn->ntyparams][0] = '\0';
				advance(p);
			} else if (lead[0] && nx->kind == TK_IDENT && !is_type_ident(nx)) {
				/* a comptime value parameter `Type name`; cfcc carries it as an integer
				 * literal, so its type must be `Int` or `Uarch`. */
				if (strcmp(lead, "Int") != 0 && strcmp(lead, "Uarch") != 0)
					die(nx->line, "a comptime value parameter must be typed `Int` or `Uarch`");
				if (nx->text[nx->len - 1] == '!')
					die(nx->line, "M0 does not support `!` in a value parameter name");
				tok_copy(nx, fn->typarams[fn->ntyparams], sizeof fn->typarams[0]);
				fn->bounds[fn->ntyparams][0] = '\0';
				snprintf(fn->valtype[fn->ntyparams], sizeof fn->valtype[0], "%s", lead);
				advance(p);
			} else {
				die(nx->line, lead[0]
				    ? "expected a type variable (`'T`) or a value parameter name after the type"
				    : "expected a type variable (`'T`) or a value parameter (`Uarch n`)");
			}
			for (int j = 0; j < fn->ntyparams; j++)
				if (strcmp(fn->typarams[j], fn->typarams[fn->ntyparams]) == 0)
					die(nx->line, "duplicate generic parameter");
			fn->ntyparams++;
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
		expect(p, TK_RBRACKET, "expected `]` to close the generic parameters");
	}

	parse_paren_params(p, fn);

	/* Optional return type `: <type>` — an `Int` (a word), `Uarch`, a record (returned by
	 * pointer), or a generic `'T`, set off with a colon before the `->` body arrow. */
	int has_ret = parse_return_type(p, fn); /* whether a return type was written (mandatory for an asm fn) */
	expect(p, TK_ARROW, "expected `->` (a return type is written `: Type` before the arrow, e.g. `(Int a): Int -> …`)");
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
	/* Every type variable used in a signature — a bare `'T` param/return, one nested in a
	 * generic application like `Box['T]`, or one in a tuple element like `('T, Int)` — must be
	 * a declared type parameter. A tuple param/return keeps its elements as separate strings. */
	for (int i = 0; i < fn->nparams; i++) {
		check_tyvars_declared(fn->params[i].type_name, fn->typarams, fn->ntyparams, fn->params[i].line);
		if (fn->params[i].kind == PK_TUPLE)
			for (int j = 0; j < fn->params[i].tuple_n; j++)
				check_tyvars_declared(fn->params[i].tuple_types[j], fn->typarams, fn->ntyparams, fn->params[i].line);
	}
	check_tyvars_declared(fn->ret_type_name, fn->typarams, fn->ntyparams, fn->ret_line);
	for (int j = 0; j < fn->ret_tuple_n; j++)
		check_tyvars_declared(fn->ret_tuple_types[j], fn->typarams, fn->ntyparams, fn->ret_line);
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
	expect(p, TK_RBRACKET, "expected `]` to close the generic arguments");
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

/* Parse ONE tuple element type into a canonical string: a simple leaf/aggregate name (via
 * parse_type_arg, e.g. `Int`, `Str`, `Point`), a type variable `'T` (validated against the
 * enclosing declaration's type parameters, substituted at instantiation), or a NESTED tuple
 * `(T0,T1,…)` — recursively, so `((Int,'T),Int)` round-trips. */
static void parse_tuple_elem_type(Parser *p, char *out, size_t cap) {
	Token *t = peek(p);
	if (t->kind == TK_LPAREN) {
		advance(p); /* ( */
		if ((size_t)1 >= cap)
			die(t->line, "tuple type too long");
		int off = 0, n = 0;
		out[off++] = '(';
		for (;;) {
			char el[128];
			parse_tuple_elem_type(p, el, sizeof el);
			int w = snprintf(out + off, cap - (size_t)off, "%s%s", n ? "," : "", el);
			if (w < 0 || (size_t)(off + w) + 1 >= cap)
				die(t->line, "tuple type too long");
			off += w;
			n++;
			if (peek(p)->kind == TK_COMMA) {
				advance(p);
				continue;
			}
			break;
		}
		expect(p, TK_RPAREN, "expected `)` to close the tuple type");
		if (n < 2)
			die(t->line, "a tuple type needs at least two elements");
		out[off++] = ')';
		out[off] = '\0';
		return;
	}
	if (!is_tyvar(t) && !is_type_ident(t))
		die(t->line, "a tuple type lists element types (`(Int, Str)`)");
	parse_type_arg(p, out, cap); /* a leaf/aggregate name, `'T`, or a generic application */
}

/* Parse a field/payload type into `out`: `Int`, an aggregate (record/union) type, a tuple
 * `(T0, …)`, a type variable `'T`, or a generic application `Box[Int]`. Pointers, buffers,
 * Uarch and Str are not field/payload types (but Str/tuples ARE valid tuple ELEMENTS). The
 * name is resolved (and any `'T` validated) later, so forward/mutual references work. */
static void parse_member_type(Parser *p, char *out, size_t cap) {
	Token *t = peek(p);
	if (t->kind == TK_STAR) {
		/* An explicit `*Aggregate` field/payload type (§6.4; the recursive `*List`/`*Node`
		 * form, type_system §8.4). Stored with the leading `*` — resolve_member_type turns
		 * `*List` into a TY_PTR to the pointee. §6.4: never a scalar. */
		advance(p); /* * */
		Token *u = peek(p);
		if (!is_type_ident(u))
			die(u->line, "expected an aggregate type after `*`");
		char pointee[64];
		tok_copy(u, pointee, sizeof pointee);
		if (is_scalar_type_name(pointee))
			die(u->line, "a pointer type `*T` points to a record or union, not a scalar (§6.4)");
		if (cap < 2)
			die(t->line, "type name too long");
		out[0] = '*';
		parse_type_arg(p, out + 1, cap - 1); /* the (possibly generic) pointee name */
		return;
	}
	if (t->kind == TK_LBRACKET)
		die(t->line, "a field/payload type is `Int`, an aggregate, or `'T`, not an array/buffer");
	if (t->kind == TK_LPAREN) {
		/* A tuple field/payload type `(T0, …)`, stored canonically as `(T0,T1,…)` (no spaces);
		 * resolve_member_type interns it. Elements may themselves be tuples (nested). The `(…)`
		 * form has no `.`, so it passes opaquely through the generic mangle/concretize machinery. */
		parse_tuple_elem_type(p, out, cap);
		return;
	}
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
	/* Split on `.` (mangle separator) AND `(`,`)`,`,` so a `'T` nested inside a tuple element
	 * type — `(Int,'T)` — is validated too, not just a top-level `'T`. */
	for (char *tok = strtok(buf, ".(),"); tok; tok = strtok(NULL, ".(),")) {
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
			if (ty->kind == TK_ELLIPSIS) {
				/* Declaration-level record spread `...Base` — splice another record's
				 * fields in place (ebnf: `data User = { ...Identifiable, Str email }`
				 * desugars to `{ Str id, Str email }`). A comptime desugar resolved
				 * here at parse time by copying the source's field name/type strings.
				 * ⚠ THROWAWAY narrowings (cf0 must NOT inherit): the source must be an
				 * already-declared CONCRETE record (no generic application `...Base[T]`,
				 * no un-applied template) — the full grammar admits any `...named_type`,
				 * incl. a generic application; the source must appear textually BEFORE
				 * this decl (parse-time `prog_find_data` resolution) — cf0 resolves type
				 * references order-independently in a later Resolve arc, so forward/
				 * mutually-recursive spread sources are legal there; a name collision is
				 * a hard error whereas the ratified rule leaves override ("later entries
				 * win" for value spread; decl collisions a semantic concern) to a later
				 * gate; and the source's own field DEFAULTS would be dropped by this copy
				 * (moot now — cfcc rejects all field defaults — but a narrowing if they land). */
				advance(p); /* `...` */
				Token *src = peek(p);
				if (!is_type_ident(src))
					die(src->line, "expected a record type name after `...`");
				char sname[64];
				tok_copy(src, sname, sizeof sname);
				advance(p);
				if (peek(p)->kind == TK_LBRACKET)
					die(peek(p)->line, "M1 cannot spread a generic application (`...Name[...]`) yet — spread a concrete record");
				if (prog_find_union(prog, sname))
					die(src->line, "spread source must be a record, not a union");
				DataDecl *sd = prog_find_data(prog, sname);
				if (!sd)
					die(src->line, "spread source record is not declared (declare it before this record)");
				if (sd->ntyparams > 0)
					die(src->line, "cannot spread a generic record template — spread a concrete record");
				for (int i = 0; i < sd->nfields; i++) {
					if (data_field_index(d, sd->fields[i]) >= 0)
						die(src->line, "duplicate field name from spread");
					if (d->nfields == MAX_FIELDS)
						die(src->line, "too many fields");
					snprintf(d->field_types[d->nfields], sizeof d->field_types[0], "%s", sd->field_types[i]);
					snprintf(d->fields[d->nfields], sizeof d->fields[0], "%s", sd->fields[i]);
					d->nfields++;
				}
			} else {
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
			}
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
 * so a union may span lines (the idiomatic AST-node form). Payload members (`M(T)`) and
 * generics are handled; member spread `...Other` splices another union's members (a
 * parse-time member-copy — see the loop). Struct-body `M = { … }`/literal members and
 * compose-over members (a bare member naming an existing type) stay later bricks — each
 * rejected with a clear message. */
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
			if (m->kind == TK_ELLIPSIS) {
				/* Union member spread `...Small` — splice another union's members in
				 * place (ebnf member_spread; the value-union echo of a record's field
				 * spread). A parse-time member-copy: append the source union's members
				 * (name + arity + payload types); tags follow declaration order.
				 * ⚠ THROWAWAY narrowings (cf0 must NOT inherit): (1) the source must be an
				 * already-declared CONCRETE union (no generic application `...Small[T]`, no
				 * un-applied template) appearing textually BEFORE this decl (parse-time
				 * `prog_find_union` — cf0 resolves order-independently in a later Resolve
				 * arc); (2) a member-name collision is a hard error; (3) cfcc splices member
				 * DECLARATIONS only — Big and Small stay DISTINCT union types with
				 * independent tags and NO subsumption (a Small value is not a Big value),
				 * whereas cf0's `...Small` participates in union-subset subtyping (§8.2) with
				 * cross-sub/superset tag consistency owned by the M6/M9 representation gate;
				 * (4) cfcc's source carries only inline tag/payload members — it has no
				 * compose-over members, so the fuller `...Int`-style numeric-union spread is
				 * out of reach. */
				advance(p); /* `...` */
				Token *src = peek(p);
				if (!is_type_ident(src))
					die(src->line, "expected a union type name after `...`");
				char sname[64];
				tok_copy(src, sname, sizeof sname);
				advance(p);
				if (peek(p)->kind == TK_LBRACKET)
					die(peek(p)->line, "M1 cannot spread a generic application (`...Name[...]`) yet — spread a concrete union");
				if (prog_find_data(prog, sname))
					die(src->line, "member spread source must be a union, not a record");
				UnionDecl *su = prog_find_union(prog, sname);
				if (!su)
					die(src->line, "member spread source union is not declared (declare it before this union)");
				if (su->ntyparams > 0)
					die(src->line, "cannot spread a generic union template — spread a concrete union");
				for (int i = 0; i < su->nmembers; i++) {
					if (union_member_tag(u, su->members[i]) >= 0)
						die(src->line, "duplicate union member from spread");
					if (u->nmembers == MAX_UNION_MEMBERS)
						die(src->line, "too many union members");
					u->arity[u->nmembers] = su->arity[i];
					for (int j = 0; j < su->arity[i]; j++)
						snprintf(u->payload_types[u->nmembers][j], sizeof u->payload_types[0][0], "%s", su->payload_types[i][j]);
					snprintf(u->members[u->nmembers], sizeof u->members[0], "%s", su->members[i]);
					u->nmembers++;
				}
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

/* type_decl (record-body fork) = "type" type_name "=" "{" field_decl { "," field_decl } "}"
 * A grouped-params named tuple (ebnf § Data & Type Declarations). Only the record-body
 * form reaches here — the plain-alias form (`type Id = Uarch`) is erased by the token
 * pre-pass. Each field is exactly a `param` (`field_decl` shares the param shape), so the
 * body is parsed with parse_param and stored as a ParamGroup that splats at param sites.
 *
 * A CALL passes the group as one record literal `f({ a: 1, b: 2 })` (named, order-free),
 * desugared to positional field values (`f(1, 2)`) by desugar_group_calls before
 * monomorphize; positional `f(1, 2)` is NOT accepted for a group slot.
 *
 * ⚠ cf0 must NOT inherit (a faithful SUBSET; disclaimed): the group name splats ONLY in a
 * parameter list (cf0's named tuple is a general type — usable as a return, local, or field,
 * "each field expanding to a separate variable in place"); no field DEFAULTS (`Int x = 0`),
 * no field SPREAD (`...Base`), no generic groups, no `pub`; a nested group-typed field is
 * rejected (no group nesting); the EMPTY group `{ }` is rejected though `record_body`'s entry
 * list is grammatically optional (a zero-field splat); the group must be declared textually
 * before use (cfcc is order-dependent; cf0 resolves order-independently). */
static ParamGroup *parse_type_decl(Parser *p, Program *prog) {
	expect_ident(p, "type");
	Token *nm = peek(p);
	if (!is_type_ident(nm))
		die(nm->line, "expected a type name after `type`");
	ParamGroup *g = xmalloc(sizeof *g);
	g->nfields = 0;
	tok_copy(nm, g->name, sizeof g->name);
	g->line = nm->line;
	advance(p);
	if (peek(p)->kind == TK_LBRACKET)
		die(peek(p)->line, "generic `type` declarations are not supported yet (a later brick)");
	if (prog_find_data(prog, g->name) || prog_find_union(prog, g->name) || prog_find_group(prog, g->name))
		die(nm->line, "a type of that name is already defined");
	expect(p, TK_EQ, "a `type` declaration needs `=`");
	expect(p, TK_LBRACE, "a grouped-params `type` body is a record shape `{ Type name, … }`");
	if (peek(p)->kind == TK_RBRACE)
		die(peek(p)->line, "a grouped-params `type` needs at least one field");
	for (;;) {
		if (g->nfields == MAX_PARAMS)
			die(peek(p)->line, "too many fields in a grouped-params `type`");
		parse_param(p, &g->fields[g->nfields]);
		Param *f = &g->fields[g->nfields];
		if (f->kind == PK_VAR)
			die(f->line, "a grouped-params `type` field cannot be generic (`'T`)");
		if (prog_find_group(prog, f->type_name))
			die(f->line, "a grouped-params `type` field cannot itself be a grouped-params type");
		if (peek(p)->kind == TK_EQ)
			die(peek(p)->line, "grouped-params `type` fields cannot carry defaults yet");
		for (int j = 0; j < g->nfields; j++)
			if (strcmp(g->fields[j].name, f->name) == 0)
				die(f->line, "duplicate field name in a grouped-params `type`");
		g->nfields++;
		if (peek(p)->kind == TK_COMMA) {
			advance(p);
			if (peek(p)->kind == TK_RBRACE) /* trailing comma */
				break;
			continue;
		}
		break;
	}
	expect(p, TK_RBRACE, "expected `}` to close the grouped-params `type`");
	return g;
}

/* module = { declaration } — one per line; exactly one is `pub const main`. A
 * declaration is a `data` record, a `union`, a `type` (grouped params), or a
 * `[pub] const` function. */
static void parse(Parser *p, Program *prog) {
	p->prog = prog; /* so a closure binding can append its lifted top-level function */
	skip_newlines(p);
	while (peek(p)->kind != TK_EOF) {
		if (is_ident(peek(p), "data"))
			prog_add_data(prog, parse_data_decl(p, prog));
		else if (is_ident(peek(p), "union"))
			prog_add_union(prog, parse_union_decl(p, prog));
		else if (is_ident(peek(p), "type"))
			prog_add_group(prog, parse_type_decl(p, prog));
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
	if (m->ret_type_name[0] || m->ret_tuple_n)
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
static Stmt *clone_stmt(Stmt *s); /* forward: EX_LOOP body */
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
	c->spread = clone_expr(e->spread); /* EX_RECORD value-level spread source */
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
	c->loop_body = clone_stmt(e->loop_body); /* EX_LOOP body (NULL otherwise) */
	return c;
}

static Stmt *clone_stmt(Stmt *s) {
	if (!s)
		return NULL;
	Stmt *c = xmalloc(sizeof *c);
	*c = *s;
	c->expr = clone_expr(s->expr);
	c->yval = clone_expr(s->yval); /* ST_YIELD value (NULL otherwise) */
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
	if (src[0] == '(') {
		/* A tuple type string `(T0,T1,…)` — substitute each element (top-level comma split,
		 * respecting nested parens) and rejoin, so a `'T` element picks up its concrete arg
		 * (`(Int,'T)` → `(Int,Int)`). Element names are themselves mangled/tuple strings, so
		 * each recurses through here. */
		int off = 0;
		if ((size_t)2 >= cap)
			die(0, "tuple type name too long");
		dst[off++] = '(';
		int depth = 0, start = 1, n = 0;
		for (int i = 1;; i++) {
			char ch = src[i];
			if (ch == '(') {
				depth++;
			} else if (ch == ')' && depth > 0) {
				depth--;
			} else if (ch == '\0' || ((ch == ',' || ch == ')') && depth == 0)) {
				char el[256], sub[256];
				int len = i - start;
				if (len < 0 || (size_t)len >= sizeof el)
					die(0, "tuple element type too long");
				memcpy(el, src + start, (size_t)len);
				el[len] = '\0';
				subst_mangled(sub, sizeof sub, el, typarams, args, ntp);
				int w = snprintf(dst + off, cap - (size_t)off, "%s%s", n ? "," : "", sub);
				if (w < 0 || (size_t)(off + w) + 1 >= cap)
					die(0, "tuple type name too long");
				off += w;
				n++;
				start = i + 1;
				if (ch == ')' || ch == '\0')
					break;
			}
		}
		dst[off++] = ')';
		dst[off] = '\0';
		return;
	}
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
static void subst_stmts(Stmt *s, Func *tmpl, char targs[][256]); /* forward: EX_LOOP body */
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
	if (e->spread)
		subst_expr(e->spread, tmpl, targs);
	for (int i = 0; i < e->narms; i++)
		subst_expr(e->arms[i].body, tmpl, targs);
	subst_stmts(e->loop_body, tmpl, targs); /* EX_LOOP body (NULL otherwise) */
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
		subst_expr(s->yval, tmpl, targs); /* ST_YIELD value (NULL otherwise) */
		subst_stmts(s->body, tmpl, targs);
	}
}

/* Substitute comptime value parameters (`[Uarch n]`) in a cloned body: every read of a
 * value-param name folds to its concrete integer literal, so the specialized body carries
 * no runtime type variable (type_system §9.1 — a value param is comptime-by-declaration).
 * `names[i]` is the value-param name (`n`), `vals[i]` its concrete value. */
static void subst_value_stmts(Stmt *s, char names[][64], long *vals, int nv); /* forward: EX_LOOP body */
static void subst_value_expr(Expr *e, char names[][64], long *vals, int nv) {
	if (!e)
		return;
	if (e->kind == EX_VAR)
		for (int i = 0; i < nv; i++)
			if (strcmp(e->name, names[i]) == 0) {
				e->kind = EX_INT; /* fold the read to its comptime literal */
				e->ival = vals[i];
				break;
			}
	subst_value_expr(e->lhs, names, vals, nv);
	subst_value_expr(e->rhs, names, vals, nv);
	subst_value_expr(e->els, names, vals, nv);
	for (int i = 0; i < e->nargs; i++)
		subst_value_expr(e->args[i], names, vals, nv);
	for (int i = 0; i < e->nfields; i++)
		subst_value_expr(e->fvals[i], names, vals, nv);
	if (e->spread)
		subst_value_expr(e->spread, names, vals, nv);
	for (int i = 0; i < e->narms; i++)
		subst_value_expr(e->arms[i].body, names, vals, nv);
	subst_value_stmts(e->loop_body, names, vals, nv); /* EX_LOOP body (NULL otherwise) */
}

static void subst_value_stmts(Stmt *s, char names[][64], long *vals, int nv) {
	for (; s; s = s->next) {
		/* A `[n Uint8]` buffer whose length is a value param resolves to a concrete size. */
		if (s->kind == ST_LOCAL && s->bufsize_name[0])
			for (int i = 0; i < nv; i++)
				if (strcmp(s->bufsize_name, names[i]) == 0) {
					if (vals[i] <= 0)
						die(s->line, "a byte-buffer length must be positive");
					if (vals[i] > INT32_MAX)
						die(s->line, "byte buffer too large");
					s->bufsize = (int)vals[i];
					s->bufsize_name[0] = '\0';
					break;
				}
		subst_value_expr(s->expr, names, vals, nv);
		subst_value_expr(s->yval, names, vals, nv); /* ST_YIELD value (NULL otherwise) */
		subst_value_stmts(s->body, names, vals, nv);
	}
}

/* Reclassify a `'T` parameter to the concrete kind of its type argument. A record or
 * union name stays PK_RECORD (resolve_signatures reclassifies a union to PK_UNION). */
static void reclassify_param(Param *p, const char *concrete, int line) {
	if (strcmp(concrete, "Int") == 0) {
		p->kind = PK_WORD;
	} else if (strcmp(concrete, "Uarch") == 0) {
		p->kind = PK_UARCH;
	} else if (strcmp(concrete, "Unit") == 0) {
		p->kind = PK_UNIT; /* the unit type argument — a word `0` parameter (see PK_UNIT) */
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
	if (name[0] == '*') /* an explicit `*Aggregate`: concretize the POINTEE — the `*` is not
	                     * part of the (possibly generic) type name (mirrors resolve_member_type).
	                     * The buffer keeps its `*`; resolve_member_type strips it later. */
		name++;
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
		} else if (c->params[i].kind == PK_TUPLE) {
			/* A tuple param `('T, Int)` — its element strings are a heap array SHARED with the
			 * template (copied by pointer above), so substitute into a FRESH copy, never in place
			 * (that would corrupt the template for its other instantiations). resolve_signatures
			 * interns the clone's own shape from these substituted strings. */
			int tn = c->params[i].tuple_n;
			char (*elems)[64] = xmalloc((size_t)tn * sizeof *elems);
			for (int j = 0; j < tn; j++) {
				char sub[256];
				subst_mangled(sub, sizeof sub, tmpl->params[i].tuple_types[j], tmpl->typarams, targs, tmpl->ntyparams);
				if (strlen(sub) >= sizeof elems[0])
					die(line, "tuple parameter element type name too long");
				snprintf(elems[j], sizeof elems[0], "%s", sub);
			}
			c->params[i].tuple_types = elems;
			c->params[i].tup = NULL; /* re-interned per this instantiation in resolve_signatures */
		}
	}
	for (int j = 0; j < c->ret_tuple_n; j++) {
		/* A tuple RETURN's element strings live in a by-value array (already the clone's own),
		 * so substitute each in place; resolve_signatures re-interns `ret_tup`. */
		char sub[256];
		subst_mangled(sub, sizeof sub, tmpl->ret_tuple_types[j], tmpl->typarams, targs, tmpl->ntyparams);
		if (strlen(sub) >= sizeof c->ret_tuple_types[0])
			die(line, "tuple return element type name too long");
		snprintf(c->ret_tuple_types[j], sizeof c->ret_tuple_types[0], "%s", sub);
		c->ret_tup = NULL;
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
	/* Fold each comptime value parameter to its concrete integer throughout the body. */
	char vnames[MAX_TYPARAMS][64];
	long vvals[MAX_TYPARAMS];
	int nv = 0;
	for (int i = 0; i < nargs; i++)
		if (tmpl->valtype[i][0]) {
			if (!is_all_digits(typeargs[i]))
				die(line, "a comptime value parameter needs an integer value, not a type");
			snprintf(vnames[nv], sizeof vnames[0], "%s", tmpl->typarams[i]);
			vvals[nv] = strtol(typeargs[i], NULL, 10);
			nv++;
		}
	if (nv)
		subst_value_stmts(c->body, vnames, vvals, nv);
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
				case PK_WORD:    return "Int";
				case PK_UARCH:   return "Uarch";
				case PK_RECORD:
				case PK_UNION:
				case PK_VAR:     return fn->params[i].type_name;
				case PK_LONG:    return ""; /* a pointer — no simple type name */
				case PK_CAPTURE: return "Int"; /* a captured word */
				case PK_CAPTURE_REC: return fn->params[i].type_name; /* a captured record */
				case PK_FN: return ""; /* a function value — no simple nominal type name */
				case PK_TUPLE: return ""; /* a structural tuple — no nominal name to infer from */
				case PK_UNIT: return "Unit"; /* the unit type */
				case PK_F64: return "Float64";
				case PK_F32: return "Float32";
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
static void monomorph_stmts(Program *prog, Func *fn, Stmt *s); /* forward: EX_LOOP body */
static void monomorph_expr(Program *prog, Func *fn, Expr *e) {
	if (!e)
		return;
	monomorph_stmts(prog, fn, e->loop_body); /* EX_LOOP body (NULL otherwise) */
	monomorph_expr(prog, fn, e->lhs);
	monomorph_expr(prog, fn, e->rhs);
	monomorph_expr(prog, fn, e->els);
	for (int i = 0; i < e->nargs; i++)
		monomorph_expr(prog, fn, e->args[i]);
	for (int i = 0; i < e->nfields; i++)
		monomorph_expr(prog, fn, e->fvals[i]);
	if (e->spread)
		monomorph_expr(prog, fn, e->spread);
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
	/* A comptime value parameter (`[Uarch n]`) can't be inferred from a runtime argument,
	 * so a callee with any value param must be given explicit arguments. */
	int has_valparam = 0;
	for (int i = 0; i < callee->ntyparams; i++)
		if (callee->valtype[i][0])
			has_valparam = 1;
	char args[MAX_TYPARAMS][64];
	int ntargs = e->ntypeargs;
	if (ntargs > 0) {
		for (int i = 0; i < ntargs; i++)
			snprintf(args[i], sizeof args[0], "%s", e->typeargs[i]);
		/* Each slot must be filled by the right kind: a value param takes an integer
		 * literal, a type param a type name (never a bare integer). */
		for (int i = 0; i < ntargs && i < callee->ntyparams; i++) {
			int slot_is_value = callee->valtype[i][0] != '\0';
			if (slot_is_value && !is_all_digits(args[i]))
				die(e->line, "this generic slot is a comptime value parameter; pass a value, not a type");
			if (!slot_is_value && is_all_digits(args[i]))
				die(e->line, "this generic slot is a type parameter; pass a type, not a value");
		}
	} else if (has_valparam) {
		die(e->line, "this generic function has a comptime value parameter; "
		             "call it with explicit arguments, e.g. `f[8, Int](...)`");
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
		monomorph_expr(prog, fn, s->yval); /* ST_YIELD value (NULL otherwise) */
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

/* ---- fake-closure specialization (HOF v2) ------------------------------------
 * A capturing closure has no runtime value (memory_model §7), so when one is passed
 * to a higher-order function it cannot ride a code pointer like a capture-free value
 * (HOF v1). Instead the callee is SPECIALIZED per closure: a clone of the HOF whose
 * function-value parameter is replaced by leading hidden parameters carrying the
 * closure's captures, and whose `f(…)` calls retarget to the closure's lifted body.
 * The call site drops the closure argument and threads the captures' addresses. This
 * runs as its own pass on pristine (un-typechecked) bodies, before typecheck. */

/* Specialize `callee` for the capturing-closure arguments of call `e` (made in `fn`).
 * Rewrites `e` in place (retargets to the clone, threads capture args). A no-op when no
 * argument is a capturing closure — those stay v1 function pointers. */
static void specialize_hof(Program *prog, Func *fn, Expr *e, Func *callee) {
	if (e->nargs != callee->nparams)
		return; /* arity error — let typecheck report it */
	int spec[MAX_PARAMS];       /* per param: fn->closures index of a capturing-closure arg, else -1 */
	int any = 0;
	for (int i = 0; i < callee->nparams; i++) {
		spec[i] = -1;
		if (callee->params[i].kind != PK_FN || e->args[i]->kind != EX_VAR)
			continue;
		int ci = func_find_closure(fn, e->args[i]->name);
		if (ci >= 0 && fn->closures[ci].ncaps > 0) { /* a CAPTURING closure — capture-free stays a pointer */
			/* Validate the closure's signature against the function-type parameter, here
			 * where the error can point at the call (the specialized clone would otherwise
			 * fail deeper, with a confusing message). */
			Func *lifted = fn->closures[ci].lifted;
			int nc = fn->closures[ci].ncaps;
			if (lifted->nparams - nc != callee->params[i].fn_arity)
				die(e->line, "the closure has the wrong number of parameters for this function-type argument");
			/* The closure's non-capture parameters + its return must match the function-type
			 * parameter's signature (same structural test as a bare function value). */
			char csig[256] = {0}, esig[256];
			for (int j = nc; j < lifted->nparams; j++)
				sig_append_param(&lifted->params[j], csig, sizeof csig);
			sig_append(csig, sizeof csig, '>');
			sig_append(csig, sizeof csig, type_sig_char(func_ret_type(lifted)));
			param_fn_sig(&callee->params[i], esig, sizeof esig);
			if (strcmp(csig, esig) != 0)
				die(e->line, "the closure's signature does not match this function-type argument");
			spec[i] = ci;
			any = 1;
		}
	}
	if (!any)
		return;

	/* Clone name: <callee>$<lifted-closure per specialized param>. */
	char key[256];
	int n = snprintf(key, sizeof key, "%s", callee->name);
	for (int i = 0; i < callee->nparams; i++)
		if (spec[i] >= 0) {
			if (n < 0 || (size_t)n >= sizeof key)
				die(e->line, "HOF specialization name too long");
			n += snprintf(key + n, sizeof key - (size_t)n, "$%s", fn->closures[spec[i]].lifted->name);
		}
	if (n < 0 || (size_t)n >= sizeof ((Func *)0)->name)
		die(e->line, "HOF specialization name too long");

	Func *clone = prog_find_func(prog, key);
	if (!clone) {
		clone = new_func();
		*clone = *callee; /* params/closures arrays (by value), ret fields, flags */
		snprintf(clone->name, sizeof clone->name, "%s", key);
		clone->is_pub = 0;
		clone->nabinds = 0;
		clone->next_bind_id = 0;
		clone->ret_rec = NULL;
		clone->ret_uni = NULL;
		clone->ret_tup = NULL; /* re-interned per instantiation from ret_tuple_types */
		if (callee->nlocals) {
			clone->locals = xmalloc((size_t)callee->cap_locals * sizeof *clone->locals);
			memcpy(clone->locals, callee->locals, (size_t)callee->nlocals * sizeof *clone->locals);
		} else {
			clone->locals = NULL;
			clone->cap_locals = 0;
		}
		clone->body = clone_stmt(callee->body);
		/* Rebuild params: [each specialized closure's captures, renamed uniquely] ++
		 * [callee params minus the specialized PK_FN ones]. Register the specialized
		 * function parameter as a closure binding so its `f(…)` calls retarget. */
		Param np[MAX_PARAMS];
		int nn = 0, capctr = 0;
		for (int i = 0; i < callee->nparams; i++) {
			if (spec[i] < 0)
				continue;
			Func *lifted = fn->closures[spec[i]].lifted;
			int ncaps = fn->closures[spec[i]].ncaps;
			if (clone->nclosures >= MAX_CLOSURES)
				die(e->line, "too many specialized closures");
			int k = clone->nclosures++;
			snprintf(clone->closures[k].name, sizeof clone->closures[k].name, "%s", callee->params[i].name);
			clone->closures[k].lifted = lifted;
			clone->closures[k].ncaps = ncaps;
			for (int j = 0; j < ncaps; j++) {
				if (nn >= MAX_PARAMS)
					die(e->line, "too many threaded captures after specialization");
				np[nn] = lifted->params[j]; /* PK_CAPTURE/PK_CAPTURE_REC, kind+type_name copied */
				snprintf(np[nn].name, sizeof np[nn].name, "__hcap%d", capctr);
				snprintf(clone->closures[k].caps[j], 64, "__hcap%d", capctr);
				capctr++;
				nn++;
			}
		}
		for (int i = 0; i < callee->nparams; i++)
			if (spec[i] < 0) {
				if (nn >= MAX_PARAMS)
					die(e->line, "too many parameters after specialization");
				np[nn++] = callee->params[i];
			}
		clone->nparams = nn;
		memcpy(clone->params, np, sizeof(Param) * (size_t)nn);
		prog_add_func(prog, clone);
	}

	/* Retarget the call: [each specialized closure's capture variables] ++ [kept args]. */
	int total = 0;
	for (int i = 0; i < callee->nparams; i++)
		total += spec[i] >= 0 ? fn->closures[spec[i]].ncaps : 1;
	Expr **na = xmalloc((size_t)total * sizeof *na);
	int idx = 0;
	for (int i = 0; i < callee->nparams; i++)
		if (spec[i] >= 0)
			for (int j = 0; j < fn->closures[spec[i]].ncaps; j++) {
				Expr *cv = new_expr(EX_VAR);
				cv->line = e->line;
				snprintf(cv->name, sizeof cv->name, "%s", fn->closures[spec[i]].caps[j]);
				na[idx++] = cv;
			}
	for (int i = 0; i < callee->nparams; i++)
		if (spec[i] < 0)
			na[idx++] = e->args[i];
	e->args = na;
	e->nargs = total;
	snprintf(e->name, sizeof e->name, "%s", clone->name);
	e->hof_specialized = 1;
}

static void specialize_calls_stmt(Program *prog, Func *fn, Stmt *s);

static void specialize_calls_expr(Program *prog, Func *fn, Expr *e) {
	if (!e)
		return;
	if (e->kind == EX_CALL && !e->hof_specialized) {
		Func *callee = prog_find_func(prog, e->name);
		if (callee && callee->ntyparams == 0)
			for (int i = 0; i < callee->nparams; i++)
				if (callee->params[i].kind == PK_FN) {
					specialize_hof(prog, fn, e, callee);
					break;
				}
	}
	specialize_calls_expr(prog, fn, e->lhs);
	specialize_calls_expr(prog, fn, e->rhs);
	specialize_calls_expr(prog, fn, e->els);
	for (int i = 0; i < e->nargs; i++)
		specialize_calls_expr(prog, fn, e->args[i]);
	for (int i = 0; i < e->nfields; i++)
		specialize_calls_expr(prog, fn, e->fvals[i]);
	if (e->spread)
		specialize_calls_expr(prog, fn, e->spread);
	for (int i = 0; i < e->narms; i++)
		specialize_calls_expr(prog, fn, e->arms[i].body);
	specialize_calls_stmt(prog, fn, e->loop_body); /* EX_LOOP body (NULL otherwise) */
}

static void specialize_calls_stmt(Program *prog, Func *fn, Stmt *s) {
	for (; s; s = s->next) {
		specialize_calls_expr(prog, fn, s->expr);
		specialize_calls_expr(prog, fn, s->yval); /* ST_YIELD value (NULL otherwise) */
		specialize_calls_stmt(prog, fn, s->body); /* loop / defer-block bodies */
	}
}

/* Specialize every higher-order call in the program. Clones append to prog->funcs and are
 * reached by the growing loop bound, so a specialized clone that itself passes a captured
 * closure onward (a HOF calling a HOF) is specialized in turn (the fake-closure fixpoint). */
static void specialize_hofs(Program *prog) {
	for (int i = 0; i < prog->nfuncs; i++) {
		Func *fn = prog->funcs[i];
		if (fn->ntyparams == 0 && !fn->is_asm)
			specialize_calls_stmt(prog, fn, fn->body);
	}
}

/* ------------------------------------------- grouped-params call desugar - *
 *
 * A call to a function with a grouped-params `type` parameter passes ONE `{…}` record
 * literal per group; this pass rewrites it to the group's field VALUES, positionally in
 * the group's declared field order (matched by field name), so every downstream pass sees
 * an ordinary flattened positional call. It runs after parse (all callees are known, so a
 * forward call resolves) and before monomorphize (clones then inherit the flattened calls,
 * so generics + groups compose for free). The bare-`{…}` call form is authorized by
 * type_system §7.3 (a structural `type` payload is the bare data literal, order-independent;
 * a nominal `data` would instead be `Point({…})`). ⚠ cf0 must NOT inherit: the group literal
 * must be a BARE `{ f: v, … }` (no spread, no annotation) that sets every field exactly once;
 * and ONLY a direct by-name call desugars — a grouped-params function used as a first-class
 * value (a HOF argument) would not splat (cfcc has no such callee; disclaimed). */
static void desugar_group_calls_expr(Program *prog, Expr *e);

static void expand_group_call(Program *prog, Expr *e, Func *callee) {
	(void)prog;
	if (e->nargs != callee->ncallslots)
		die(e->line, "wrong number of arguments (a grouped-params `type` is passed as one `{…}` literal)");
	Expr **out = xmalloc(sizeof(Expr *) * (size_t)callee->nparams);
	int no = 0;
	for (int s = 0; s < callee->ncallslots; s++) {
		Expr *arg = e->args[s];
		ParamGroup *g = callee->callgroups[s];
		if (!g) {
			out[no++] = arg;
			continue;
		}
		if (arg->kind != EX_RECORD || arg->name[0] || arg->spread)
			die(arg->line, "a grouped-params argument must be a bare `{ field: value, … }` literal");
		if (arg->nfields != g->nfields)
			die(arg->line, "the `{…}` group literal must set every field of the grouped-params type exactly once");
		for (int fi = 0; fi < g->nfields; fi++) {
			Expr *v = NULL;
			for (int k = 0; k < arg->nfields; k++)
				if (strcmp(arg->fnames[k], g->fields[fi].name) == 0) {
					if (v)
						die(arg->line, "a group literal sets a field more than once");
					v = arg->fvals[k];
				}
			if (!v)
				die(arg->line, "the `{…}` group literal is missing a field of the grouped-params type");
			out[no++] = v;
		}
	}
	e->args = out;
	e->nargs = no;
}

static void desugar_group_calls_stmt(Program *prog, Stmt *s) {
	for (; s; s = s->next) {
		desugar_group_calls_expr(prog, s->expr);
		desugar_group_calls_expr(prog, s->yval);
		desugar_group_calls_stmt(prog, s->body);
	}
}

static void desugar_group_calls_expr(Program *prog, Expr *e) {
	if (!e)
		return;
	if (e->kind == EX_CALL) {
		Func *callee = prog_find_func(prog, e->name);
		if (callee && callee->has_group)
			expand_group_call(prog, e, callee);
	}
	desugar_group_calls_expr(prog, e->lhs);
	desugar_group_calls_expr(prog, e->rhs);
	desugar_group_calls_expr(prog, e->els);
	for (int i = 0; i < e->nargs; i++)
		desugar_group_calls_expr(prog, e->args[i]);
	for (int i = 0; i < e->nfields; i++)
		desugar_group_calls_expr(prog, e->fvals[i]);
	if (e->spread)
		desugar_group_calls_expr(prog, e->spread);
	for (int i = 0; i < e->narms; i++)
		desugar_group_calls_expr(prog, e->arms[i].body);
	desugar_group_calls_stmt(prog, e->loop_body);
}

static void desugar_group_calls(Program *prog) {
	for (int i = 0; i < prog->nfuncs; i++)
		if (!prog->funcs[i]->is_asm)
			desugar_group_calls_stmt(prog, prog->funcs[i]->body);
}

/* ------------------------------------------------------------- typecheck - */

static Type typeof_expr(Program *prog, Func *fn, Expr *e);
static Type func_ret_type(const Func *fn);
static void resolve_record_literal(Program *prog, Func *fn, Expr *e, DataDecl *d);

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
	} else if (want.kind == TY_TUPLE) {
		/* A tuple field/payload of a matching shape. A tuple is immutable, so a bare tuple
		 * variable may alias here (like a union) — no freshness rule needed. */
		if (at.kind != TY_TUPLE || !types_equal(at, want))
			die(line, "field/payload type mismatch (tuple shape differs)");
	} else if (want.kind == TY_UNIT) {
		/* A `Unit` field/payload takes the unit value `()` — its only value (a word `0`). */
		if (at.kind != TY_UNIT)
			die(line, "expected the unit value `()` for this `Unit` field/payload");
	} else if (want.kind == TY_PTR && (want.rec || want.uni)) {
		/* An explicit `*Aggregate` field/payload — the recursive `*List`/`*Node` slot. An
		 * aggregate value (or another such pointer) satisfies it; cfcc stores the arena
		 * pointer. A `*T` is explicitly a reference, so (unlike a by-value record payload)
		 * no freshness rule applies — aliasing is the intent. */
		if (want.rec ? aggregate_record(at) != want.rec : aggregate_union(at) != want.uni)
			die(line, "field/payload type mismatch (`*Aggregate` differs)");
	} else {
		die(line, "unsupported field/payload type");
	}
}

static void check_stmts(Program *prog, Func *fn, Stmt *list); /* forward: EX_LOOP body */

/* A value loop must actually yield: scan its own body for a `<- v`. Yields target the
 * nearest loop, so a yield inside a NESTED loop belongs to that loop, not this one —
 * only the loop's own direct statements count (guarded yields are ST_YIELD nodes too). */
static int loop_body_yields(const Stmt *body) {
	for (const Stmt *s = body; s; s = s->next)
		if (s->kind == ST_YIELD)
			return 1;
	return 0;
}

/* Compute and validate the type of an expression, resolving field accesses and
 * calls against the whole program (so forward references and recursion work).
 * Field accesses are annotated in place (rec, foff) for emit. The public entry is
 * the `typeof_expr` wrapper below, which caches the result in `e->rtype`. */
static Type typeof_expr_compute(Program *prog, Func *fn, Expr *e) {
	switch (e->kind) {
	case EX_INT:
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	case EX_FLOAT: /* a float literal is Float64 (no literal-adopts-Float32 in cfcc) */
		return (Type){TY_F64, NULL, NULL, 0, NULL};
	case EX_STR:
		return (Type){TY_STR, NULL, NULL, 0, NULL};
	case EX_VAR: {
		/* A function reference (a bare top-level function or capture-free closure name):
		 * a function VALUE, valid only as a `(…) Int` argument. Resolve it to its emit
		 * symbol and Int-arity; a capturing closure has no runtime value (memory_model §7),
		 * and cfcc function values are all-`Int` → `Int`. */
		if (e->is_fnref) {
			int ci = func_find_closure(fn, e->name);
			Func *g;
			if (ci >= 0) {
				if (fn->closures[ci].ncaps != 0)
					die(e->line, "a capturing closure has no runtime value — it cannot be passed as a "
					             "function argument yet (only capture-free functions can)");
				g = fn->closures[ci].lifted;
			} else {
				g = prog_find_func(prog, e->name);
				if (!g)
					die(e->line, "unknown name");
				if (g->ntyparams > 0)
					die(e->line, "a generic function cannot be passed as a value (instantiate it first)");
			}
			/* No Int-only restriction: a function value carries its full signature, checked
			 * structurally where it is passed to a function-type parameter (a mismatch — say a
			 * record-taking function into a scalar slot — is reported there, at the call). */
			snprintf(e->name, sizeof e->name, "%s", g->name); /* the emit symbol ($<name>) */
			e->fn_arity = g->nparams;
			func_value_sig(g, e->fn_sig, sizeof e->fn_sig);
			return (Type){TY_FN, NULL, NULL, 0, NULL};
		}
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
		Resolution r = resolve_name(fn, e->name, &ty);
		if (r == R_NONE)
			die(e->line, "unknown name");
		if (ty.kind == TY_FN) /* a function-value parameter passed along — carry its signature */
			for (int i = 0; i < fn->nparams; i++)
				if (fn->params[i].kind == PK_FN && strcmp(fn->params[i].name, e->name) == 0) {
					e->fn_arity = fn->params[i].fn_arity;
					param_fn_sig(&fn->params[i], e->fn_sig, sizeof e->fn_sig);
				}
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
				return (Type){TY_INT, NULL, NULL, 0, NULL};
			if (strcmp(e->name, "bytes") == 0)
				return (Type){TY_PTR, NULL, NULL, 0, NULL};
			die(e->line, "a string has only the `.len` and `.bytes` fields");
		}
		if (base.kind == TY_ARRAY) {
			/* A fixed array exposes `.len`, its comptime element count (an Int). Emit
			 * reads the constant from the base's cached rtype.alen. */
			if (strcmp(e->name, "len") == 0)
				return (Type){TY_INT, NULL, NULL, 0, NULL};
			die(e->line, "a fixed array has only the `.len` field");
		}
		/* A record VALUE or an explicit `*Record` pointer both address the record (cfcc
		 * represents an aggregate value as its arena pointer), so a field reads the same way. */
		DataDecl *brec = base.kind == TY_RECORD ? base.rec
		               : (base.kind == TY_PTR ? base.rec : NULL);
		if (!brec)
			die(e->line, "field access `.` needs a record value on the left");
		int idx = data_field_index(brec, e->name);
		if (idx < 0)
			die(e->line, "this data type has no such field");
		e->rec = brec;
		e->foff = data_field_offset(idx);
		return data_field_type(prog, brec, idx); /* Int or an aggregate field type */
	}
	case EX_INDEX: {
		/* `base[i]` — a fixed array (runtime index → Int element) or a tuple (COMPTIME
		 * literal index → the exact per-position element type). No array bounds check
		 * (throwaway; cf0's `[N T]` is bounds-checked). ⚠ cf0 must NOT inherit the array
		 * index type: §6.2 makes it a `Uarch` — cfcc uses Int (see TY_ARRAY). */
		Type base = typeof_expr(prog, fn, e->lhs);
		if (base.kind == TY_TUPLE) {
			/* A tuple is heterogeneous, so the position must be statically known — only a
			 * literal `t[k]`, in range, yields a well-defined element type. */
			if (e->rhs->kind != EX_INT)
				die(e->line, "a tuple is indexed only at a comptime literal position (`t[0]`), "
				             "so each access has a known element type");
			long k = e->rhs->ival;
			if (k < 0 || k >= base.tup->nelem)
				die(e->line, "tuple index out of range");
			return base.tup->elems[k];
		}
		if (base.kind != TY_ARRAY)
			die(e->line, "index `[…]` needs a fixed-array or tuple value on the left");
		expect_int(prog, fn, e->rhs);
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	}
	case EX_ARRAY: {
		/* A fixed-array literal `[e0, …]`: every element is an Int (M1 element type);
		 * the value's type is `[nargs Int]`. Only valid as a `[N Int]` binding's
		 * initializer — resolve_array_binding checks the length matches the annotation. */
		for (int i = 0; i < e->nargs; i++)
			expect_int(prog, fn, e->args[i]);
		Type t = {TY_ARRAY, NULL, NULL, 0, NULL};
		t.alen = e->nargs;
		return t;
	}
	case EX_TUPLE: {
		/* A tuple literal `(e0, …, en-1)`: type each element and intern the shape. Elements are
		 * Int, Str, or a FRESH record (a record-returning call or literal) — an aggregate
		 * element takes over its arena storage, so aliasing an existing record is rejected (the
		 * record-field freshness rule, memory_model §6). A `...src` spread splices the elements
		 * of another tuple in place (a comptime desugar): its already-owned elements ride in as
		 * pointer/word copies, no freshness needed. Array/union/pointer elements are a later brick. */
		Type elems[MAX_FIELDS];
		int n = 0;
		for (int i = 0; i < e->nargs; i++) {
			if (e->args[i]->kind == EX_SPREAD) {
				Type st = typeof_expr(prog, fn, e->args[i]->lhs);
				if (st.kind != TY_TUPLE)
					die(e->args[i]->line, "a tuple spread `...x` splices a tuple value");
				e->args[i]->rtype = st; /* cache for emit (its element widths/offsets) */
				for (int j = 0; j < st.tup->nelem; j++) {
					if (n == MAX_FIELDS)
						die(e->line, "tuple has too many elements");
					elems[n++] = st.tup->elems[j];
				}
				continue;
			}
			Type et = typeof_expr(prog, fn, e->args[i]);
			if (type_is_word(et) || et.kind == TY_STR || et.kind == TY_TUPLE) {
				/* Int / a tag-only union (a word), an immutable Str, or a nested tuple (also
				 * immutable, so its pointer may ride in unchanged) — no alias/freshness risk. */
			} else if (et.kind == TY_RECORD) {
				if (e->args[i]->kind != EX_CALL && e->args[i]->kind != EX_RECORD)
					die(e->args[i]->line, "a record tuple element must be a fresh value (a record-"
					                      "returning call or literal), not an alias of existing storage");
			} else {
				die(e->args[i]->line, "a tuple element is an Int, Str, record, or tuple value in cfcc "
				                      "(array/union/pointer elements are a later brick)");
			}
			if (n == MAX_FIELDS)
				die(e->line, "tuple has too many elements");
			elems[n++] = et;
		}
		if (n < 2)
			die(e->line, "a tuple has at least two elements");
		Type t = {TY_TUPLE, NULL, NULL, 0, NULL};
		t.tup = prog_intern_tuple(prog, elems, n);
		return t;
	}
	case EX_UNIT:
		/* The unit value `()` — the sole value of the zero-tuple type `Unit`. */
		return (Type){TY_UNIT, NULL, NULL, 0, NULL};
	case EX_SPREAD:
		die(e->line, "a spread `...x` is only valid as a tuple element");
	case EX_RECORD:
		if (e->name[0]) {
			/* Explicit construction `Point({…})`: the literal names its own record type.
			 * Resolve once (idempotent — typeof may be called repeatedly) and yield it. */
			if (!e->rec) {
				DataDecl *d = prog_find_data(prog, e->name);
				if (!d)
					die(e->line, "unknown record type in construction");
				resolve_record_literal(prog, fn, e, d);
			}
			return e->rtype;
		}
		/* An unresolved bare `{…}` reached here with no type context to construct from —
		 * the literal is sugar for `Type({…})`, and there is no `Type`. Only a record
		 * binding annotation or a record return type supplies it. */
		die(e->line, "cannot infer the record type to construct here (annotate the binding, "
		             "or use it as a record return value)");
	case EX_CALL: {
		/* A call whose "callee" is a function-VALUE parameter (`f(x)` where `f` is a `(…) -> R`
		 * param) is an INDIRECT call through the code pointer. Each argument is checked against
		 * the corresponding function-type component, and the call's type is the return component
		 * (any scalar, pointer, or nested function type). */
		{
			Type pty;
			if (resolve_name(fn, e->name, &pty) == R_PARAM && pty.kind == TY_FN) {
				const Param *fp = NULL;
				for (int i = 0; i < fn->nparams; i++)
					if (fn->params[i].kind == PK_FN && strcmp(fn->params[i].name, e->name) == 0)
						fp = &fn->params[i];
				if (!fp) /* a TY_FN param is always a PK_FN param — belt and braces */
					die(e->line, "internal: function-value parameter not found");
				if (e->nargs != fp->fn_arity)
					die(e->line, "wrong number of arguments to a function value");
				for (int i = 0; i < e->nargs; i++) {
					const Param *pc = &fp->fn_ptypes[i];
					Type at = typeof_expr(prog, fn, e->args[i]);
					switch (pc->kind) {
					case PK_WORD:
						if (at.kind != TY_INT)
							die(e->line, "argument type mismatch (an `Int` function-type parameter expects an `Int`)");
						break;
					case PK_UARCH: /* an Int widens to Uarch (throwaway cfcc coercion) */
						if (at.kind != TY_UARCH && at.kind != TY_INT)
							die(e->line, "argument type mismatch (a `Uarch` function-type parameter expects a `Uarch`)");
						break;
					case PK_F64:
						if (at.kind != TY_F64)
							die(e->line, "argument type mismatch (a `Float64` function-type parameter expects a `Float64`)");
						break;
					case PK_F32:
						if (at.kind != TY_F32)
							die(e->line, "argument type mismatch (a `Float32` function-type parameter expects a `Float32`)");
						break;
					case PK_UNIT:
						if (at.kind != TY_UNIT)
							die(e->line, "argument type mismatch (a `Unit` function-type parameter expects the unit value `()`)");
						break;
					case PK_LONG:
						if (at.kind != TY_PTR && at.kind != TY_BUF)
							die(e->line, "argument type mismatch (a pointer function-type parameter expects a pointer)");
						break;
					case PK_FN: {
						if (at.kind != TY_FN)
							die(e->line, "argument type mismatch (a function-type parameter expects a function value)");
						char expect_sig[256];
						param_fn_sig(pc, expect_sig, sizeof expect_sig);
						if (strcmp(e->args[i]->fn_sig, expect_sig) != 0)
							die(e->line, "argument type mismatch (the function value's signature does not match)");
						break;
					}
					default:
						die(e->line, "internal: unsupported function-type component");
					}
				}
				e->indirect = 1;
				return param_component_type(fp->fn_ret);
			}
		}
		/* A call to a closure bound in this function rewrites to a call of its lifted
		 * top-level function, prepending each captured variable (by name) as a leading
		 * argument. Those match the lifted function's PK_CAPTURE parameters, whose emit
		 * passes the variable's address (capture by reference). Done once (guarded). */
		if (!e->closure_call) {
			int ci = func_find_closure(fn, e->name);
			if (ci >= 0) {
				int nc = fn->closures[ci].ncaps;
				Expr **na = xmalloc((size_t)(nc + e->nargs) * sizeof *na);
				for (int i = 0; i < nc; i++) {
					Expr *cv = new_expr(EX_VAR);
					cv->line = e->line;
					snprintf(cv->name, sizeof cv->name, "%s", fn->closures[ci].caps[i]);
					na[i] = cv;
				}
				for (int i = 0; i < e->nargs; i++)
					na[nc + i] = e->args[i];
				e->args = na;
				e->nargs += nc;
				snprintf(e->name, sizeof e->name, "%s", fn->closures[ci].lifted->name);
				e->closure_call = 1;
			}
		}
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
				/* A record value OR an explicit `*Record` pointer to the same decl (they share
				 * the arena-pointer representation, so either satisfies a `Point` or `*Point`
				 * parameter — pm->rec is the same decl for both param spellings). */
				if (aggregate_record(at) != pm->rec)
					die(e->line, "argument type mismatch (expected a record or `*Record` of the parameter's type)");
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
				/* A union value OR an explicit `*Union` pointer to the same decl. */
				if (aggregate_union(at) != pm->uni)
					die(e->line, "argument type mismatch (expected a union or `*Union` of the parameter's type)");
				break;
			case PK_VAR:
				die(e->line, "internal: call to an unspecialized generic function");
				break;
			case PK_CAPTURE:
				/* A prepended capture argument: the enclosing variable, which must be a
				 * word (captured by reference and read/written through a pointer). */
				if (at.kind != TY_INT)
					die(e->line, "internal: closure capture is not a word");
				break;
			case PK_CAPTURE_REC:
				/* A prepended record capture: the enclosing record, passed by pointer. */
				if (at.kind != TY_RECORD || at.rec != pm->rec)
					die(e->line, "internal: closure record capture type mismatch");
				break;
			case PK_FN: {
				/* A higher-order argument: a function value whose full signature matches. */
				if (at.kind != TY_FN)
					die(e->line, "argument type mismatch (a function-type parameter expects a function value, e.g. a bare function name)");
				char expect_sig[256];
				param_fn_sig(pm, expect_sig, sizeof expect_sig);
				if (strcmp(e->args[i]->fn_sig, expect_sig) != 0)
					die(e->line, "argument type mismatch (the function value's signature does not match the function-type parameter)");
				break;
			}
			case PK_TUPLE:
				/* A tuple argument: a tuple of the SAME shape, passed by pointer. cfcc tuples
				 * are immutable and the parameter is read-only, so aliasing the caller's tuple
				 * is sound (no freshness rule, unlike a mutable record field). */
				if (at.kind != TY_TUPLE)
					die(e->line, "argument type mismatch (a tuple parameter expects a tuple)");
				if (!types_equal(at, (Type){TY_TUPLE, NULL, NULL, 0, pm->tup}))
					die(e->line, "argument type mismatch (tuple shape differs)");
				break;
			case PK_UNIT:
				/* A unit argument: the unit value `()` — its only value (a word `0`). */
				if (at.kind != TY_UNIT)
					die(e->line, "argument type mismatch (a `Unit` parameter expects the unit value `()`)");
				break;
			case PK_F64:
				/* A Float64 parameter expects a Float64 value (no implicit Int→Float — cast). */
				if (at.kind != TY_F64)
					die(e->line, "argument type mismatch (a Float64 parameter expects a Float64 value)");
				break;
			case PK_F32:
				if (at.kind != TY_F32)
					die(e->line, "argument type mismatch (a Float32 parameter expects a Float32 value)");
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
		return (Type){TY_UNION, NULL, u, 0, NULL};
	}
	case EX_MATCH: {
		/* Compare-chain over a union scrutinee's tag (seed_subset §7). Each arm names a
		 * member of the scrutinee's union (qualified) or is `_`; arm bodies are Int
		 * (M1.1) and unify to the match's type. Exhaustiveness: cover every member or
		 * carry a `_`. */
		Type st = typeof_expr(prog, fn, e->lhs);
		/* A union value OR an explicit `*Union` pointer (a boxed union is already a pointer,
		 * so `match` on a `*List` binding drives the recursive walk). */
		UnionDecl *u = aggregate_union(st);
		if (!u)
			die(e->line, "M1.1 `match` requires a union scrutinee");
		e->uni = u;
		int covered[MAX_UNION_MEMBERS] = {0};
		int has_wild = 0, have_rt = 0;
		Type rt = {TY_INT, NULL, NULL, 0, NULL}; /* the arms' common (result) type */
		for (int i = 0; i < e->narms; i++) {
			MatchArm *a = &e->arms[i];
			if (has_wild)
				die(a->line, "unreachable match arm after `_`");
			int saved_abinds = fn->nabinds; /* arm-scoped bindings pop after the body */
			if (a->is_wild) {
				has_wild = 1;
			} else {
				/* Arms qualify members by the scrutinee's union: its base (template) name
				 * `Maybe.Just`, or the exact concrete instance `Maybe[Int].Just` (mangled
				 * `Maybe.1.Int`, == u->name). Both spell the same member (type_system §8.1/§8.3);
				 * a DIFFERENT instance (`Maybe[Str]` on a `Maybe[Int]`) matches neither and is
				 * rejected. For a non-generic union base_name == name, so this is one check. */
				if (strcmp(a->qual, u->base_name) != 0 && strcmp(a->qual, u->name) != 0)
					die(a->line, "a match arm must name a member of the scrutinee's union, qualified "
					             "by its name or its exact instance");
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
	case EX_CAST: {
		/* A numeric cast converts between scalar numbers (Int/Uarch/Float64/Float32); its
		 * operand must itself be a scalar number (no pointer↔number conversion, §4). */
		Type at = typeof_expr(prog, fn, e->lhs);
		if (at.kind != TY_INT && at.kind != TY_UARCH && !is_float_type(at))
			die(e->line, "a numeric cast `Int(x)`/`Uarch(x)`/`Float64(x)`/`Float32(x)` takes a scalar number");
		TypeKind tk = strcmp(e->name, "Uarch") == 0 ? TY_UARCH
		            : strcmp(e->name, "Float64") == 0 ? TY_F64
		            : strcmp(e->name, "Float32") == 0 ? TY_F32
		            : TY_INT;
		return (Type){tk, NULL, NULL, 0, NULL};
	}
	case EX_IF:
		expect_int(prog, fn, e->lhs); /* condition */
		expect_int(prog, fn, e->rhs); /* then — M0 if-branches are Int */
		expect_int(prog, fn, e->els); /* else */
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	case EX_LOOP:
		/* A value-yielding `loop`: check its body (each `<- v` yields an Int — validated
		 * per-statement in check_stmts), and require at least one yield so the loop has a
		 * value. M0 merges yields through a word slot, so a loop yields an Int (like an
		 * if-expression); a record/aggregate yield is out of reach, same limitation. */
		check_stmts(prog, fn, e->loop_body);
		if (!loop_body_yields(e->loop_body))
			die(e->line, "a value-yielding `loop` must reach a `<- v` (add a yield, or use a "
			             "statement `loop` if no value is needed)");
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	case EX_DEFER: {
		/* A `defer` tap: type its call (validates callee/args, caches the call's and
		 * each arg's rtype for emit), then yield the *tapped argument* — the call's last
		 * positional argument, the value that passes through. The call needs at least one
		 * argument to have a tapped value; a no-arg cleanup uses `defer { f() }`. */
		typeof_expr(prog, fn, e->lhs);
		if (e->lhs->nargs < 1)
			die(e->line, "a `defer` call needs at least one argument — its last is the tapped "
			             "value that passes through (use `defer { f() }` for a no-arg cleanup)");
		return e->lhs->args[e->lhs->nargs - 1]->rtype;
	}
	case EX_NEG: {
		/* Unary `-` negates an Int or a float (yielding the operand type); `~`/`!` are
		 * integer-only. */
		Type t = typeof_expr(prog, fn, e->lhs);
		if (is_float_type(t))
			return t;
		expect_int(prog, fn, e->lhs);
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	}
	case EX_BNOT:
	case EX_LNOT:
		expect_int(prog, fn, e->lhs);
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	case EX_ADD: case EX_SUB: case EX_MUL: case EX_DIV:
	case EX_EQ: case EX_NE: case EX_LT: case EX_GT: case EX_LE: case EX_GE: {
		/* Arithmetic `+ - * /` and comparison `== != < > <= >=` are NUMERIC: both operands
		 * are Int OR both the SAME float type (no mixed Int/float and no mixed Float32/Float64
		 * — cast explicitly, type_system §4). Arithmetic yields the operand type; a comparison
		 * yields Int (0/1). */
		Type lt = typeof_expr(prog, fn, e->lhs);
		Type rt = typeof_expr(prog, fn, e->rhs);
		if (is_float_type(lt) || is_float_type(rt)) {
			if (lt.kind != rt.kind || !is_float_type(lt))
				die(e->line, "a numeric operator needs two operands of the same type (no mixed Int/Float or Float32/Float64 — cast explicitly)");
			int is_cmp = e->kind == EX_EQ || e->kind == EX_NE || e->kind == EX_LT ||
			             e->kind == EX_GT || e->kind == EX_LE || e->kind == EX_GE;
			return (Type){is_cmp ? TY_INT : lt.kind, NULL, NULL, 0, NULL};
		}
		expect_int(prog, fn, e->lhs);
		expect_int(prog, fn, e->rhs);
		return (Type){TY_INT, NULL, NULL, 0, NULL};
	}
	case EX_REM:
	case EX_BOR: case EX_BXOR: case EX_BAND: case EX_SHL: case EX_SHR:
	case EX_AND: case EX_OR:
		/* `%`, bitwise, shift, and logical are integer-only. */
		expect_int(prog, fn, e->lhs);
		expect_int(prog, fn, e->rhs);
		return (Type){TY_INT, NULL, NULL, 0, NULL};
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

/* A function's declared return type: a tuple, record, or boxed union (all returned by
 * pointer), a Uarch, or Int. */
static Type func_ret_type(const Func *fn) {
	if (fn->ret_tup)
		return (Type){TY_TUPLE, NULL, NULL, 0, fn->ret_tup};
	if (strcmp(fn->ret_type_name, "Uarch") == 0)
		return (Type){TY_UARCH, NULL, NULL, 0, NULL};
	if (strcmp(fn->ret_type_name, "Float64") == 0)
		return (Type){TY_F64, NULL, NULL, 0, NULL};
	if (strcmp(fn->ret_type_name, "Float32") == 0)
		return (Type){TY_F32, NULL, NULL, 0, NULL};
	if (strcmp(fn->ret_type_name, "Unit") == 0) /* `Unit`/`()` — the zero-tuple, a word `0` */
		return (Type){TY_UNIT, NULL, NULL, 0, NULL};
	if (fn->ret_is_ptr) /* an explicit `*Aggregate` return — a TY_PTR to the pointee decl */
		return (Type){TY_PTR, fn->ret_rec, fn->ret_uni, 0, NULL};
	if (fn->ret_uni)
		return (Type){TY_UNION, NULL, fn->ret_uni, 0, NULL};
	if (fn->ret_type_name[0])
		return (Type){TY_RECORD, fn->ret_rec, NULL, 0, NULL};
	return (Type){TY_INT, NULL, NULL, 0, NULL};
}

/* Backfill a record local's declaration so later field accesses resolve. */
static void set_local_rec(Func *fn, const char *name, DataDecl *d) {
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			fn->locals[i].type.rec = d;
			return;
		}
}

/* Retype a local (parsed provisionally as a word `Int`) to a record — for an inferred
 * binding `const p = Point({…})` whose type comes from the constructor, not an annotation. */
static void set_local_record_type(Func *fn, const char *name, DataDecl *d) {
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			fn->locals[i].type = (Type){TY_RECORD, d, NULL, 0, NULL};
			snprintf(fn->locals[i].type_name, sizeof fn->locals[i].type_name, "%s", d->name);
			return;
		}
}

/* Retype a local (parsed provisionally as a word `Int`) to `Str` — for a `const s = t[k]`
 * that binds a Str tuple position (an immutable header pointer in the local's `l` slot). */
static void set_local_str(Func *fn, const char *name) {
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			fn->locals[i].type = (Type){TY_STR, NULL, NULL, 0, NULL};
			snprintf(fn->locals[i].type_name, sizeof fn->locals[i].type_name, "Str");
			return;
		}
}

/* Retype a local (parsed provisionally as a word `Int`) to a tuple — for an inferred
 * binding `const t = (1, "x")` whose shape comes from the literal, not an annotation. */
static void set_local_tuple(Func *fn, const char *name, TupleDecl *tup) {
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			fn->locals[i].type = (Type){TY_TUPLE, NULL, NULL, 0, tup};
			return;
		}
}

/* Retype a local (parsed provisionally as a word `Int`) to `Unit` — for `const u = ()` or a
 * binding of a `Unit`-returning call. Unit is word-repr (a `0`), so the provisional word slot
 * already fits; this only makes the local's type precise (a read yields `Unit`, not `Int`). */
static void set_local_unit(Func *fn, const char *name) {
	for (int i = 0; i < fn->nlocals; i++)
		if (strcmp(fn->locals[i].name, name) == 0) {
			fn->locals[i].type = (Type){TY_UNIT, NULL, NULL, 0, NULL};
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
/* Resolve an EX_RECORD data literal against a KNOWN record type `d`: check the fields
 * cover it exactly, type the values, reorder into declaration order (`ford`), resolve
 * any value-level spread, and cache the literal's rtype. Shared by the annotated binding
 * path (below) and a directly-returned literal `-> ({…})` (typed by the return type).
 * OWNER MODEL (2026-07): a context-typed bare literal is SUGAR for construction-by-
 * application — `-> ({ x, y })` ≡ `-> Point({ x: x, y: y })` — the value is built with the
 * type constructor named by context (the return type / binding annotation), so a wrong or
 * missing field here is a CONSTRUCTION error. This IS type_system §6.6/§7.3 "construction is
 * application", NOT a divergence: cfcc lowers the sugar directly. cfcc simply does not yet
 * expose the explicit `Type({…})` / `Type(1,2)` surface (cf0 does — and per seed_subset §4
 * cf0 must PARSE it). ⚠ The one genesis narrowing: cfcc context-types a bare literal only in
 * a binding/return position; §3/§5.2 propagate an expected type more generally (e.g. into a
 * call argument), which cf0 restores. */
static void resolve_record_literal(Program *prog, Func *fn, Expr *e, DataDecl *d) {
	e->rec = d;
	e->rtype = (Type){TY_RECORD, d, NULL, 0, NULL};
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
	if (e->spread) {
		/* Fill each un-overridden field from the spread source. The source must be a
		 * bare same-type record variable (re-read once per copied field — an EX_VAR read
		 * is a cheap pointer load, so no double-evaluation); a copied field must be
		 * word-sized, since a shallow copy of an aggregate field would alias its
		 * sub-record and reopen the value-semantics hole (an explicitly-overridden
		 * aggregate field is fine — it is not copied). The whole literal remains a FRESH
		 * arena allocation (emit), so this is a genuine copy, never an alias of `src`. */
		if (e->spread->kind != EX_VAR)
			die(e->line, "a record spread source must be a bare record variable");
		Type st = typeof_expr(prog, fn, e->spread);
		if (st.kind != TY_RECORD || st.rec != d)
			die(e->line, "a record spread source must be the same record type as the target");
		for (int i = 0; i < d->nfields; i++) {
			if (e->ford[i]) /* explicitly overridden — not copied */
				continue;
			if (!type_is_word(data_field_type(prog, d, i)))
				die(e->line, "a spread-copied field must be word-sized (a shallow copy of an aggregate field would alias its sub-record)");
			Expr *fe = new_expr(EX_FIELD);
			fe->line = e->line;
			fe->lhs = e->spread;
			snprintf(fe->name, sizeof fe->name, "%s", d->fields[i]);
			typeof_expr(prog, fn, fe); /* resolves rec/foff/rtype for emit */
			e->ford[i] = fe;
		}
	}
	for (int i = 0; i < d->nfields; i++)
		if (!e->ford[i])
			die(e->line, "data literal is missing a field");
}

/* The annotated binding path: `const Point p = { … }`. The literal's type is named by
 * the annotation (e->name); resolve it, then backfill the local's record type. */
static void resolve_record_binding(Program *prog, Func *fn, Stmt *s) {
	Expr *e = s->expr; /* EX_RECORD; e->name is the literal's own type name, or "" */
	/* A bare/parenthesized literal `const Point p = {…}` / `= ({…})` reaches here
	 * UNANNOTATED (name==""); fall back to the binding's annotation. An explicit
	 * construction `const Point p = Point({…})` carries its own name, which must then
	 * agree with the annotation. */
	if (e->name[0] && s->type_name[0] && strcmp(e->name, s->type_name) != 0)
		die(e->line, "constructed record type does not match the binding annotation");
	const char *tn = e->name[0] ? e->name : s->type_name;
	DataDecl *d = prog_find_data(prog, tn);
	if (!d) {
		reject_group_as_type(prog, tn, e->line);
		die(e->line, "unknown data type");
	}
	resolve_record_literal(prog, fn, e, d);
	set_local_rec(fn, s->name, d);
}

/* Resolve a record local whose initializer is a record-valued expression (not a
 * literal) — in M0, a call that returns this record. Binding from another record
 * variable is rejected (an aggregate copy needs an explicit `copy`, memory_model
 * §6). Sets the local's record type from the annotation. */
static void resolve_record_expr_binding(Program *prog, Func *fn, Stmt *s) {
	DataDecl *d = prog_find_data(prog, s->type_name);
	if (!d) {
		reject_group_as_type(prog, s->type_name, s->line);
		die(s->line, "unknown data type");
	}
	Type it = typeof_expr(prog, fn, s->expr); /* validates the call/defer, sets nargs, rtype */
	if (it.kind != TY_RECORD || it.rec != d)
		die(s->line, "initializer type does not match the record binding");
	/* Only a record-returning call produces a fresh record in expression position; a bare
	 * variable OR a field access (`rec.p`, now that a field may be a record) would alias
	 * existing storage — an aggregate copy needs an explicit copy (memory_model §6). A
	 * `defer` tap forwards its tapped argument unchanged, so it is fresh exactly when that
	 * argument is (`of(N) |> defer destroy` binds the fresh arena, schedules its teardown).
	 * (A data literal takes the resolve_record_binding path.) */
	Expr *fresh = s->expr->kind == EX_DEFER ? s->expr->lhs->args[s->expr->lhs->nargs - 1] : s->expr;
	if (fresh->kind != EX_CALL)
		die(s->line, "a record binding's initializer must be a fresh record (a record-returning call); "
		             "aliasing existing record storage needs an explicit copy — not in M0");
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
			if (s->destructure_arity > 0) {
				/* The hidden temp of a `const (…) = e` destructuring: `e` must be a tuple, and
				 * the pattern's position count must match its arity exactly (a positional
				 * pattern covers the whole tuple — comptime-sized, so this is checkable). The
				 * temp then homes the tuple; the chained `a = temp[k]` bindings validate each
				 * bound position (Int in brick 1) on their own. */
				Type it = typeof_expr(prog, fn, s->expr);
				if (it.kind != TY_TUPLE)
					die(s->line, "destructuring `const (…) = e` needs a tuple value on the right");
				if (it.tup->nelem != s->destructure_arity)
					die(s->line, "the tuple pattern covers a different number of positions than the tuple has");
				set_local_tuple(fn, s->name, it.tup);
				break;
			}
			if (strcmp(s->type_name, "Str") == 0) {           /* a `const Str` local */
				if (typeof_expr(prog, fn, s->expr).kind != TY_STR)
					die(s->line, "internal: Str local initializer is not a string");
			} else if (strcmp(s->type_name, "Float64") == 0) { /* a Float64 local */
				if (typeof_expr(prog, fn, s->expr).kind != TY_F64)
					die(s->line, "a Float64 local's initializer must be a Float64 value (cast with `Float64(x)` if needed)");
			} else if (strcmp(s->type_name, "Float32") == 0) { /* a Float32 local */
				if (typeof_expr(prog, fn, s->expr).kind != TY_F32)
					die(s->line, "a Float32 local's initializer must be a Float32 value (cast with `Float32(x)`)");
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
			} else if (s->expr->kind == EX_ARRAY) {           /* a `[N Int]` fixed-array binding */
				Type dt;
				resolve_name(fn, s->name, &dt);
				if (dt.kind != TY_ARRAY)
					/* ⚠ cf0 must NOT inherit: §7.2 infers `[N T]` from the literal with no
					 * annotation; cfcc requires the `[N Int]` annotation and infers nothing. */
					die(s->line, "an array literal needs a `[N Int]` annotation (e.g. `const [3 Int] xs = [1, 2, 3]`)");
				Type it = typeof_expr(prog, fn, s->expr); /* checks each element is Int; sets alen */
				if (it.alen != dt.alen)
					die(s->line, "array literal length does not match the `[N Int]` annotation");
			} else if (s->expr->kind == EX_RECORD && s->expr->name[0]) {
				/* `const p = Point({…})` — an explicit construction infers the record type
				 * from the constructor (no annotation needed; the local was parsed as a word). */
				Type it = typeof_expr(prog, fn, s->expr);
				set_local_record_type(fn, s->name, it.rec);
			} else if (s->expr->kind == EX_TUPLE) {
				/* `const t = (1, "x")` — the tuple's structural shape is inferred from the
				 * literal (no annotation); the local, parsed as a word, is retyped to the
				 * interned tuple and homes in the arena via `%r_<name>`. */
				Type it = typeof_expr(prog, fn, s->expr);
				set_local_tuple(fn, s->name, it.tup);
			} else if (s->expr->kind == EX_INDEX) {
				/* `const a = t[k]` (also a destructuring's desugared element binding): an Int
				 * position stays a word local; a Str position retypes to a Str local; a record
				 * position retypes to a record local that ALIASES the tuple's arena slot. That
				 * alias is sound only while BOTH sides stay read-only — the tuple is immutable
				 * and a `const` record cannot have its fields mutated, so `const` is required
				 * for Str and record positions (a `let` record could mutate the shared storage,
				 * a second mutable binding memory_model §6 forbids). */
				Type it = typeof_expr(prog, fn, s->expr);
				Type lt;
				int is_let = resolve_name(fn, s->name, &lt) == R_LET;
				if (it.kind == TY_INT) {
					/* a word local — its provisional Int type already fits (a value copy) */
				} else if (it.kind == TY_STR) {
					if (is_let)
						die(s->line, "a Str position binds `const` (a `let` Str is a later brick)");
					set_local_str(fn, s->name);
				} else if (it.kind == TY_RECORD) {
					if (is_let)
						die(s->line, "a record position binds `const` (a `let` would alias the "
						             "tuple's storage mutably — not allowed without a copy)");
					set_local_record_type(fn, s->name, it.rec);
				} else if (it.kind == TY_TUPLE) {
					/* a nested-tuple position — an immutable aggregate, so a `const` alias is
					 * sound (same argument as a record position). */
					if (is_let)
						die(s->line, "a nested-tuple position binds `const`");
					set_local_tuple(fn, s->name, it.tup);
				} else if (it.kind == TY_UNIT) {
					/* a unit position — a word `0`, so the provisional word local already fits;
					 * retype it to `Unit` so a read yields the precise type. */
					set_local_unit(fn, s->name);
				} else {
					die(s->line, "cfcc binds an Int, Str, Unit, record, or tuple position from an index "
					             "(array/union positions are a later brick)");
				}
			} else {
				/* Any other initializer: an Int (a word local), or a TUPLE-valued expression
				 * — a tuple-returning call (`const t = divmod(a, b)`, the multi-value return)
				 * or a tuple variable (an alias, sound since cfcc tuples are immutable). The
				 * destructuring desugar's hidden temp takes this path for a call RHS. */
				Type it = typeof_expr(prog, fn, s->expr);
				if (it.kind == TY_TUPLE)
					set_local_tuple(fn, s->name, it.tup);
				else if (it.kind == TY_UNIT) /* `const u = ()` or a `Unit`-returning call */
					set_local_unit(fn, s->name);
				else if (it.kind != TY_INT)
					die(s->expr->line, "expected an Int value (a record is used only via field "
					                   "access, and a string only via `.len`, in M0)");
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
		case ST_ASSIGN: { /* target is a scalar `let` local; the value must match its type */
			Type tt;
			resolve_name(fn, s->name, &tt);
			if (is_float_type(tt)) {
				if (typeof_expr(prog, fn, s->expr).kind != tt.kind)
					die(s->line, "a float `let` is reassigned a value of its own float type");
			} else {
				expect_int(prog, fn, s->expr);
			}
			break;
		}
		case ST_RETURN: {
			/* The returned value must match the function's return type. A record
			 * return may not be a bare parameter — that would hand the caller an alias
			 * of its own argument; a returned record must be freshly built (a local or
			 * another call's result), which the arena keeps alive past the frame. */
			Type rt = func_ret_type(fn);
			if (rt.kind == TY_PTR && (rt.rec || rt.uni)) {
				/* An explicit `*Aggregate` return. A record/union value — or another such
				 * pointer — satisfies it (same arena pointer, viewed as a reference); a
				 * directly-returned literal adopts the pointee record. Unlike a by-value
				 * record return, returning a bare parameter pointer is fine — a `*T` is
				 * explicitly an alias. */
				if (rt.rec && s->expr->kind == EX_RECORD && !s->expr->name[0]) {
					resolve_record_literal(prog, fn, s->expr, rt.rec);
					break;
				}
				Type et = typeof_expr(prog, fn, s->expr);
				if (rt.rec ? aggregate_record(et) != rt.rec : aggregate_union(et) != rt.uni)
					die(s->expr->line, "returned value does not match the `*Aggregate` return type");
			} else if (rt.kind == TY_RECORD) {
				if (s->expr->kind == EX_RECORD && !s->expr->name[0]) {
					/* An UNANNOTATED directly-returned literal (`-> ({…})`) adopts the
					 * return type (the sugar for `RetType({…})`). A fresh arena alloc that
					 * escapes via return — copy-free, sound. An explicit `Point({…})` (name
					 * set) instead types normally below and must match the return type. */
					resolve_record_literal(prog, fn, s->expr, rt.rec);
					break;
				}
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
			} else if (rt.kind == TY_TUPLE) {
				/* A tuple return: the returned value must be a tuple of the SAME shape (a
				 * tuple literal, a tuple local, or another tuple-returning call). The tuple
				 * lives in the arena, which outlives the frame, so its pointer escapes soundly
				 * — and cfcc has no tuple parameters yet, so it cannot alias a caller's
				 * argument (the record-return hazard). */
				Type et = typeof_expr(prog, fn, s->expr);
				if (et.kind != TY_TUPLE || !types_equal(et, rt))
					die(s->expr->line, "returned value does not match the tuple return type");
			} else if (rt.kind == TY_UNIT) {
				/* A `Unit`-returning function yields the unit value `()` — its only value. */
				Type et = typeof_expr(prog, fn, s->expr);
				if (et.kind != TY_UNIT)
					die(s->expr->line, "a `Unit` function returns the unit value `()`");
			} else if (is_float_type(rt)) {
				Type et = typeof_expr(prog, fn, s->expr);
				if (et.kind != rt.kind)
					die(s->expr->line, "a float function returns a value of its declared float type (cast if needed)");
			} else {
				expect_int(prog, fn, s->expr);
			}
			break;
		}
		case ST_LOOP:
			check_stmts(prog, fn, s->body);
			break;
		case ST_FOR: {
			/* The iterable must be a fixed array; the loop var is Int (already declared).
			 * Then check the body. */
			Type it = typeof_expr(prog, fn, s->expr);
			if (it.kind != TY_ARRAY)
				die(s->line, "`for … in` needs a fixed-array value (M1: a `[N Int]` variable)");
			check_stmts(prog, fn, s->body);
			break;
		}
		case ST_BREAK:
		case ST_CONTINUE:
			if (s->expr) /* guarded: `if <cond> then break/continue` */
				expect_int(prog, fn, s->expr);
			break;
		case ST_YIELD:
			/* `<- v` (or `if <cond> then <- v`): the guard, when present, and the yielded
			 * value are both Int (the loop merges yields through a word slot, § EX_LOOP). */
			if (s->expr) /* guard */
				expect_int(prog, fn, s->expr);
			expect_int(prog, fn, s->yval);
			break;
		case ST_EXPR:
			/* A call or a `defer` tap evaluated for effect: type it (validates the
			 * callee/args and, for a defer, schedules it); the tapped/result value,
			 * whatever its type, is discarded. */
			if (s->expr->kind != EX_CALL && s->expr->kind != EX_DEFER)
				die(s->line, "an expression statement must be a call or a `defer`");
			typeof_expr(prog, fn, s->expr);
			break;
		case ST_DEFER:
			/* A `defer { … }` block: validate its statements. Fires at scope exit. */
			check_stmts(prog, fn, s->body);
			break;
		case ST_CLOSURE:
			/* A closure declaration is pure metadata — the lifted function is checked on
			 * its own as a top-level function. Nothing to do here. */
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
					if (fn->params[j].is_ptr && !u->has_payload) /* `*TagOnlyUnion` = `*Scalar`, §6.4/§8.4 */
						die(fn->params[j].line, "a pointer to an all-tag-only union is illegal (it would be `*Scalar`, §6.4/§8.4)");
					fn->params[j].kind = PK_UNION;
					fn->params[j].uni = u;
					continue;
				}
				DataDecl *d = prog_find_data(prog, fn->params[j].type_name);
				if (!d) {
					reject_group_as_type(prog, fn->params[j].type_name, fn->params[j].line);
					die(fn->params[j].line, "a parameter names an unknown data type");
				}
				fn->params[j].rec = d;
			} else if (fn->params[j].kind == PK_CAPTURE_REC) {
				/* A captured record names a `data` type (unions are not captured). */
				DataDecl *d = prog_find_data(prog, fn->params[j].type_name);
				if (!d)
					die(fn->params[j].line, "a captured variable names an unknown data type");
				fn->params[j].rec = d;
			}
		if (fn->ret_type_name[0] && strcmp(fn->ret_type_name, "Uarch") != 0 &&
		    strcmp(fn->ret_type_name, "Unit") != 0 && strcmp(fn->ret_type_name, "Float64") != 0 &&
		    strcmp(fn->ret_type_name, "Float32") != 0) {
			UnionDecl *u = prog_find_union(prog, fn->ret_type_name);
			if (u) {
				if (fn->ret_is_ptr && !u->has_payload) /* `*TagOnlyUnion` return = `*Scalar`, §6.4/§8.4 */
					die(fn->ret_line, "a pointer to an all-tag-only union is illegal (it would be `*Scalar`, §6.4/§8.4)");
				fn->ret_uni = u;
			} else {
				DataDecl *d = prog_find_data(prog, fn->ret_type_name);
				if (!d) {
					reject_group_as_type(prog, fn->ret_type_name, fn->ret_line);
					die(fn->ret_line, "a return type names an unknown data type");
				}
				fn->ret_rec = d;
			}
		}
		if (fn->ret_tuple_n > 0) /* a tuple return type: resolve + intern its shape */
			fn->ret_tup = resolve_tuple_shape(prog, fn->ret_tuple_types, fn->ret_tuple_n, fn->ret_line);
		for (int j = 0; j < fn->nparams; j++) /* a tuple parameter: resolve + intern its shape */
			if (fn->params[j].kind == PK_TUPLE)
				fn->params[j].tup = resolve_tuple_shape(prog, fn->params[j].tuple_types,
				                                         fn->params[j].tuple_n, fn->params[j].line);
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
/* The QBE mnemonic for an arithmetic/bitwise/shift op. Type-agnostic — the result type
 * on the `=<t>` selects the operation (float `add` on a `d`, integer `add` on a `w`);
 * `div`/`rem` are the signed integer forms and float `div` on a `d`. */
static const char *arith_mnemonic(ExprKind k) {
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
	default: die(0, "internal: not an arithmetic op"); return NULL;
	}
}

/* A comparison mnemonic `c<cond><operand-type>` (result is always a word). Ordering uses
 * the SIGNED integer conditions (`cslt…`) for a word/long and the ORDERED float conditions
 * (`clt…`, no signedness) for a `s`/`d`; equality is `ceq`/`cne`. Float `ceq`/`clt` follow
 * IEEE (NaN compares unequal / unordered), which type_system §5 mandates. */
static void cmp_mnemonic(ExprKind k, char oq, char *buf, size_t cap) {
	int fp = oq == 's' || oq == 'd';
	const char *cond;
	switch (k) {
	case EX_EQ: cond = "eq"; break;
	case EX_NE: cond = "ne"; break;
	case EX_LT: cond = fp ? "lt" : "slt"; break;
	case EX_GT: cond = fp ? "gt" : "sgt"; break;
	case EX_LE: cond = fp ? "le" : "sle"; break;
	case EX_GE: cond = fp ? "ge" : "sge"; break;
	default: die(0, "internal: not a comparison op"); cond = NULL;
	}
	snprintf(buf, cap, "c%s%c", cond, oq);
}

/* One scheduled `defer`, fired at scope exit. Exactly one of `block`/`call` is set:
 * a `defer { … }` block re-emits `block` at exit (reading locals live then), while a
 * `defer f(x)` tap re-issues `call` with its arguments already snapshotted into the
 * `args` temps (their widths in `argw`) at the point the defer was reached. */
typedef struct {
	Stmt *block;                 /* block form: the body to re-emit; NULL for the call form */
	Expr *call;                  /* call form: the EX_CALL (callee name + return type) */
	int nargs;
	char args[MAX_PARAMS][64];   /* snapshotted argument operands (`%tN`) */
	char argw[MAX_PARAMS];       /* each argument's register width ('w' or 'l') */
} Defer;

/* Per-function emit state: the next expression-temp and control-flow-label ids
 * (both function-scoped in QBE), plus a stack of enclosing loops' ids so `break`
 * and `continue` reach the nearest loop's end/top labels. */
typedef struct {
	const Func *fn; /* the function being emitted — lets EX_VAR/ST_ASSIGN recognise a
	                 * PK_CAPTURE parameter (a by-reference word) and load/store through it */
	int tmp;
	int lbl;
	int loops[MAX_LOOP_DEPTH]; /* label ids of enclosing loops (innermost last) */
	int loop_slots[MAX_LOOP_DEPTH]; /* per enclosing loop: the EX_LOOP merge slot a `<- v`
	                                 * stores into, or -1 for a statement loop/for (no value) */
	int loop_depth;
	int ret_uarch; /* 1 if the current function returns Uarch (an `l`) — a returned
	                * Int value is widened to `l` before `ret`. */
	/* Pending `defer`s in the order they are reached, fired LIFO at each `return`. A
	 * block defer holds its body; a call/tap defer holds the callee and its arguments
	 * SNAPSHOTTED at the defer point (evaluated once, into temps), so the scheduled call
	 * sees the value that passed through, not a re-read at scope exit. */
	Defer defers[MAX_DEFERS];
	int ndefers;
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
static void collect_strlits_stmt(Stmt *list); /* forward: EX_LOOP body */
static void collect_strlits_expr(Expr *e) {
	if (!e)
		return;
	if (e->kind == EX_STR)
		register_strlit(e);
	collect_strlits_stmt(e->loop_body); /* EX_LOOP body (NULL otherwise) */
	collect_strlits_expr(e->lhs);
	collect_strlits_expr(e->rhs);
	collect_strlits_expr(e->els);
	for (int i = 0; i < e->nargs; i++)
		collect_strlits_expr(e->args[i]);
	for (int i = 0; i < e->nfields; i++)
		collect_strlits_expr(e->fvals[i]);
	if (e->spread)
		collect_strlits_expr(e->spread);
	for (int i = 0; i < e->narms; i++) /* EX_MATCH arm bodies */
		collect_strlits_expr(e->arms[i].body);
}

static void collect_strlits_stmt(Stmt *list) {
	for (Stmt *s = list; s; s = s->next) {
		collect_strlits_expr(s->expr);
		collect_strlits_expr(s->yval); /* ST_YIELD value (NULL otherwise) */
		if (s->kind == ST_LOOP || s->kind == ST_FOR || s->kind == ST_DEFER) /* bodies with statements */
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
static void emit_stmts(FILE *out, Stmt *list, Emit *ex); /* forward: EX_LOOP body */
static void emit_expr(FILE *out, Expr *e, Emit *ex, char *dst, size_t cap) {
	switch (e->kind) {
	case EX_INT:
		snprintf(dst, cap, "%ld", e->ival);
		return;
	case EX_FLOAT: {
		/* A Float64 constant: QBE spells a double literal `d_<value>`. Materialize it into a
		 * `d` temp so it is a plain operand everywhere (like EX_STR's pointer temp). `%.17g`
		 * round-trips a double exactly; force a `.0` when it prints as an integer so the QBE
		 * lexer reads it as a float, not an int. */
		char num[64];
		snprintf(num, sizeof num, "%.17g", e->fval);
		if (!strpbrk(num, ".eEnN")) /* no '.'/exponent/(nan|inf) → looks like an int */
			snprintf(num + strlen(num), sizeof num - strlen(num), ".0");
		int t = ex->tmp++;
		fprintf(out, "\t%%t%d =d copy d_%s\n", t, num);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_STR: {
		/* A Str value is a pointer to its header; materialize the address into a temp
		 * so it is a plain `l` operand wherever used (a call argument, `len`, …). */
		int t = ex->tmp++;
		fprintf(out, "\t%%t%d =l copy $cfstr_%d\n", t, e->strid);
		snprintf(dst, cap, "%%t%d", t);
		return;
	}
	case EX_VAR: {
		/* A function reference: materialize the callee's code address ($<symbol>) into an
		 * `l` temp (typecheck rewrote `name` to the emit symbol). A function-VALUE parameter
		 * (a PK_FN param, TY_FN) is its incoming `%u_<name>` pointer, used directly. */
		if (e->is_fnref) {
			int t = ex->tmp++;
			fprintf(out, "\t%%t%d =l copy $%s\n", t, e->name);
			snprintf(dst, cap, "%%t%d", t);
			return;
		}
		if (e->rtype.kind == TY_FN) {
			snprintf(dst, cap, "%%u_%s", e->name);
			return;
		}
		/* A match-arm payload binding is an Int value held in its `%pb<id>` temp
		 * (loaded at the arm block). */
		if (e->is_bind) {
			snprintf(dst, cap, "%%pb%d", e->bind_id);
			return;
		}
		/* A record, byte-buffer, fixed-array, tuple, or boxed (payload) union name is an
		 * arena pointer (`%r_<name>`), used directly as an operand; a word name (Int or
		 * tag-only union) is a `loadw` from its slot. */
		if (e->rtype.kind == TY_RECORD || e->rtype.kind == TY_BUF || e->rtype.kind == TY_ARRAY ||
		    e->rtype.kind == TY_TUPLE || (e->rtype.kind == TY_UNION && e->rtype.uni->has_payload)) {
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
		if (is_float_type(e->rtype)) {
			/* A float local/param lives in its own slot (params spill there like words), so a
			 * read is a `load<qt>` (`loads`/`loadd`). */
			char qt = qtype_of(e->rtype);
			int t = ex->tmp++;
			fprintf(out, "\t%%t%d =%c load%c %%s_%s\n", t, qt, qt, e->name);
			snprintf(dst, cap, "%%t%d", t);
			return;
		}
		int t = ex->tmp++;
		/* A captured word (PK_CAPTURE) lives in the caller's slot; its `%u_<name>` param
		 * holds that slot's address, so read it with a `loadw` through the pointer. A plain
		 * word local/param reads from its own `%s_<name>` slot. */
		if (is_capture_param(ex->fn, e->name))
			fprintf(out, "\t%%t%d =w loadw %%u_%s\n", t, e->name);
		else
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
		if (e->lhs->rtype.kind == TY_ARRAY) {
			/* `xs.len` — the comptime element count, emitted as a constant (the base is a
			 * side-effect-free array local, so it needs no evaluation). */
			snprintf(dst, cap, "%d", e->lhs->rtype.alen);
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
	case EX_INDEX: {
		/* A tuple index `t[k]` has a COMPTIME literal position (k*8 is a constant offset) and
		 * loads a word or an 8-byte pointer per the element's type; an array index `xs[i]`
		 * takes a runtime word index (widened to a long, *8) and always loads a word. Neither
		 * bounds-checks (throwaway). */
		char base[96];
		emit_expr(out, e->lhs, ex, base, sizeof base);
		char addr[96];
		if (e->lhs->rtype.kind == TY_TUPLE) {
			long off = e->rhs->ival * 8; /* rhs is a literal (checked in typeof) */
			if (off == 0) {
				snprintf(addr, sizeof addr, "%s", base);
			} else {
				int a = ex->tmp++;
				fprintf(out, "\t%%t%d =l add %s, %ld\n", a, base, off);
				snprintf(addr, sizeof addr, "%%t%d", a);
			}
		} else {
			char idx[96];
			emit_expr(out, e->rhs, ex, idx, sizeof idx);
			int iw = ex->tmp++;
			fprintf(out, "\t%%t%d =l extsw %s\n", iw, idx);
			int o = ex->tmp++;
			fprintf(out, "\t%%t%d =l mul %%t%d, 8\n", o, iw);
			int a = ex->tmp++;
			fprintf(out, "\t%%t%d =l add %s, %%t%d\n", a, base, o);
			snprintf(addr, sizeof addr, "%%t%d", a);
		}
		int r = ex->tmp++;
		if (type_is_word(e->rtype)) /* an Int/tag-only element is a word; an aggregate a pointer */
			fprintf(out, "\t%%t%d =w loadw %s\n", r, addr);
		else
			fprintf(out, "\t%%t%d =l loadl %s\n", r, addr);
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_ARRAY:
		die(e->line, "internal: array literal in expression position");
	case EX_TUPLE: {
		/* Construct a tuple in the arena: bump-allocate its (comptime-known) slots and fill
		 * them left to right. A plain element stores its value (word via storew, aggregate
		 * pointer via storel). A `...src` spread copies each of src's elements out of src's
		 * slots into the next output slots (a word/pointer copy per the src element's width).
		 * The total arity comes from the interned shape (rtype.tup). Yields the base pointer. */
		int N = e->rtype.tup->nelem;
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =l call $cf_alloc(w %d)\n", r, N * 8);
		int slot = 0;
		for (int i = 0; i < e->nargs; i++) {
			if (e->args[i]->kind == EX_SPREAD) {
				char sp[96];
				emit_expr(out, e->args[i]->lhs, ex, sp, sizeof sp); /* the src tuple pointer */
				TupleDecl *stup = e->args[i]->rtype.tup;
				for (int j = 0; j < stup->nelem; j++, slot++) {
					int word = type_is_word(stup->elems[j]);
					int ld = ex->tmp++;
					if (j == 0)
						fprintf(out, "\t%%t%d =%s %s %s\n", ld, word ? "w" : "l", word ? "loadw" : "loadl", sp);
					else {
						int sa = ex->tmp++;
						fprintf(out, "\t%%t%d =l add %s, %d\n", sa, sp, j * 8);
						fprintf(out, "\t%%t%d =%s %s %%t%d\n", ld, word ? "w" : "l", word ? "loadw" : "loadl", sa);
					}
					int da = ex->tmp++;
					fprintf(out, "\t%%t%d =l add %%t%d, %d\n", da, r, slot * 8);
					fprintf(out, "\t%s %%t%d, %%t%d\n", word ? "storew" : "storel", ld, da);
				}
				continue;
			}
			char ev[96];
			emit_expr(out, e->args[i], ex, ev, sizeof ev);
			const char *st = type_is_word(e->args[i]->rtype) ? "storew" : "storel";
			int da = ex->tmp++;
			fprintf(out, "\t%%t%d =l add %%t%d, %d\n", da, r, slot * 8);
			fprintf(out, "\t%s %s, %%t%d\n", st, ev, da);
			slot++;
		}
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_UNIT:
		/* The unit value carries no data — lower it to the word constant `0` (see TY_UNIT). */
		snprintf(dst, cap, "0");
		return;
	case EX_SPREAD:
		die(e->line, "internal: spread outside a tuple literal");
	case EX_RECORD: {
		/* Construct a record in expression position (a directly-returned literal
		 * `-> ({…})`): bump-allocate its storage, store each field at its offset (in
		 * declaration order via `ford`), and yield the fresh arena pointer. Mirrors the
		 * ST_LOCAL record construction, but into a temp rather than a named slot. */
		DataDecl *d = e->rec;
		int r = ex->tmp++;
		fprintf(out, "\t%%t%d =l call $cf_alloc(w %d)\n", r, data_size(d));
		for (int fi = 0; fi < d->nfields; fi++) {
			char fv[96];
			emit_expr(out, e->ford[fi], ex, fv, sizeof fv);
			const char *st = type_is_word(e->ford[fi]->rtype) ? "storew" : "storel";
			if (fi == 0) {
				fprintf(out, "\t%s %s, %%t%d\n", st, fv, r);
			} else {
				int a = ex->tmp++;
				fprintf(out, "\t%%t%d =l add %%t%d, %d\n", a, r, data_field_offset(fi));
				fprintf(out, "\t%s %s, %%t%d\n", st, fv, a);
			}
		}
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_CALL: {
		/* An INDIRECT call through a function-value parameter (`f(x)`): the callee is the
		 * `%u_<name>` code pointer. Each argument's register width follows the corresponding
		 * function-type component (an `Int` passed to a `Uarch` slot is sign-extended, as in a
		 * direct call), and the result width follows the return component. */
		if (e->indirect) {
			const Param *fp = NULL;
			for (int i = 0; i < ex->fn->nparams; i++)
				if (ex->fn->params[i].kind == PK_FN && strcmp(ex->fn->params[i].name, e->name) == 0)
					fp = &ex->fn->params[i];
			int it[MAX_PARAMS];
			char iw[MAX_PARAMS];
			for (int i = 0; i < e->nargs; i++) {
				char op[96];
				emit_expr(out, e->args[i], ex, op, sizeof op);
				char w = fp ? param_qtype(&fp->fn_ptypes[i]) : 'w';
				if (w == 'l' && fp && fp->fn_ptypes[i].kind == PK_UARCH &&
				    e->args[i]->rtype.kind == TY_INT) {
					/* Widen an Int argument to a Uarch slot (throwaway coercion; cf0 casts). */
					int tw = ex->tmp++;
					fprintf(out, "\t%%t%d =w copy %s\n", tw, op);
					it[i] = ex->tmp++;
					fprintf(out, "\t%%t%d =l extsw %%t%d\n", it[i], tw);
				} else {
					it[i] = ex->tmp++;
					fprintf(out, "\t%%t%d =%c copy %s\n", it[i], w, op);
				}
				iw[i] = w;
			}
			char rw = fp ? param_qtype(fp->fn_ret) : 'w';
			int r = ex->tmp++;
			fprintf(out, "\t%%t%d =%c call %%u_%s(", r, rw, e->name);
			for (int i = 0; i < e->nargs; i++)
				fprintf(out, "%s%c %%t%d", i ? ", " : "", iw[i], it[i]);
			fprintf(out, ")\n");
			snprintf(dst, cap, "%%t%d", r);
			return;
		}
		/* Evaluate each argument into its own temp, then call. Each argument's register
		 * width follows the *parameter* kind (word param → `w`; record/pointer/Uarch → `l`),
		 * and an Int passed to a Uarch parameter is widened `w`→`l`. The callee symbol is
		 * the bare function name ($<name>); the result width follows the return type. */
		int argt[MAX_PARAMS];
		char argw[MAX_PARAMS];
		for (int i = 0; i < e->nargs; i++) {
			ParamKind pk = e->callee->params[i].kind;
			if (pk == PK_CAPTURE) {
				/* Capture by reference: pass the ADDRESS of the enclosing word variable
				 * (its `%s_<name>` slot), so the closure reads/writes the live variable. */
				const char *cn = e->args[i]->name;
				argw[i] = 'l';
				argt[i] = ex->tmp++;
				fprintf(out, "\t%%t%d =l copy %s%s\n", argt[i],
				        is_capture_param(ex->fn, cn) ? "%u_" : "%s_", cn);
				continue;
			}
			char op[96];
			emit_expr(out, e->args[i], ex, op, sizeof op);
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
				argw[i] = param_qtype(&e->callee->params[i]);
				argt[i] = ex->tmp++;
				fprintf(out, "\t%%t%d =%c copy %s\n", argt[i], argw[i], op);
			}
		}
		int r = ex->tmp++;
		/* Word / Float64(`d`) / else `l` result — mirroring the callee's return type. */
		char rty = qtype_of(e->rtype);
		fprintf(out, "\t%%t%d =%c call $%s(", r, rty, e->name);
		for (int i = 0; i < e->nargs; i++)
			fprintf(out, "%s%c %%t%d", i ? ", " : "", argw[i], argt[i]);
		fprintf(out, ")\n");
		snprintf(dst, cap, "%%t%d", r);
		return;
	}
	case EX_DEFER: {
		/* A `defer f(x)` tap: SNAPSHOT the deferred call's arguments here (evaluate each
		 * once, into a temp — the same per-arg width/Uarch-widen rules as EX_CALL), record
		 * the call to fire at scope exit (emit_defers), and evaluate to the tapped argument
		 * — the call's last snapshot. The snapshots are top-level temps, so they dominate
		 * every later `return`; the tapped value passing through is exactly what the
		 * scheduled call will receive (e.g. `const arena = of(N) |> defer destroy`). */
		Expr *call = e->lhs;
		if (ex->ndefers >= MAX_DEFERS)
			die(e->line, "too many `defer`s in one function");
		Defer *d = &ex->defers[ex->ndefers++];
		d->block = NULL;
		d->call = call;
		d->nargs = call->nargs;
		for (int i = 0; i < call->nargs; i++) {
			ParamKind pk = call->callee->params[i].kind;
			if (pk == PK_CAPTURE) {
				/* Snapshot the captured variable's ADDRESS (a stable slot pointer); the
				 * deferred closure call reads/writes the live variable at scope exit. */
				const char *cn = call->args[i]->name;
				int t = ex->tmp++;
				fprintf(out, "\t%%t%d =l copy %s%s\n", t,
				        is_capture_param(ex->fn, cn) ? "%u_" : "%s_", cn);
				snprintf(d->args[i], sizeof d->args[i], "%%t%d", t);
				d->argw[i] = 'l';
				continue;
			}
			char op[96];
			emit_expr(out, call->args[i], ex, op, sizeof op);
			if (pk == PK_UARCH && call->args[i]->rtype.kind == TY_INT) {
				int w = ex->tmp++;
				fprintf(out, "\t%%t%d =w copy %s\n", w, op);
				int t = ex->tmp++;
				fprintf(out, "\t%%t%d =l extsw %%t%d\n", t, w);
				snprintf(d->args[i], sizeof d->args[i], "%%t%d", t);
				d->argw[i] = 'l';
			} else {
				d->argw[i] = param_qtype(&call->callee->params[i]);
				int t = ex->tmp++;
				fprintf(out, "\t%%t%d =%c copy %s\n", t, d->argw[i], op);
				snprintf(d->args[i], sizeof d->args[i], "%%t%d", t);
			}
		}
		/* The tapped value is the last positional argument's snapshot. */
		snprintf(dst, cap, "%s", d->args[call->nargs - 1]);
		return;
	}
	case EX_CAST: {
		/* Convert a scalar number between Int (`w`, signed), Uarch (`l`, unsigned), Float64
		 * (`d`), and Float32 (`s`). Int↔Uarch: `extsw` widen / `w copy` truncate. Int/Uarch →
		 * float: `swtof` (signed Int) / `ultof` (unsigned Uarch), result `s` or `d`. Float →
		 * Int/Uarch: `<src>tosi` (→ signed Int `w`) / `<src>toui` (→ unsigned Uarch `l`),
		 * `<src>` ∈ {s,d}. Float↔float: `truncd` (d→s) / `exts` (s→d). Same-type = copy. */
		char op[96];
		emit_expr(out, e->lhs, ex, op, sizeof op);
		TypeKind s = e->lhs->rtype.kind, d = e->rtype.kind;
		char sq = qtype_of(e->lhs->rtype), dq = qtype_of(e->rtype);
		int sf = is_float_type(e->lhs->rtype), df = is_float_type(e->rtype);
		int r = ex->tmp++;
		if (s == d)
			fprintf(out, "\t%%t%d =%c copy %s\n", r, dq, op);
		else if (!sf && !df) { /* Int↔Uarch */
			if (d == TY_INT)
				fprintf(out, "\t%%t%d =w copy %s\n", r, op);   /* → Int: truncate */
			else
				fprintf(out, "\t%%t%d =l extsw %s\n", r, op);  /* → Uarch: sign-extend */
		} else if (!sf && df) /* Int/Uarch → float */
			fprintf(out, "\t%%t%d =%c %s %s\n", r, dq, s == TY_INT ? "swtof" : "ultof", op);
		else if (sf && !df) /* float → Int/Uarch */
			fprintf(out, "\t%%t%d =%c %cto%s %s\n", r, dq, sq, d == TY_INT ? "si" : "ui", op);
		else /* float → float (different widths) */
			fprintf(out, "\t%%t%d =%c %s %s\n", r, dq, s == TY_F64 ? "truncd" : "exts", op);
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
	case EX_LOOP: {
		/* A value-yielding loop: like the statement loop (@ltop back-edge, @lend exit), but
		 * each `<- v` stores `v` into the merge slot `%m<slot>` and jumps to @lend; the value
		 * is then loaded there. @lend is reached ONLY through those yields (never fall-through),
		 * so the slot is always stored before the load. The loop is pushed on the label/slot
		 * stacks so break/continue/yield inside reach it. */
		int id = ex->lbl++;
		ex->loops[ex->loop_depth] = id;
		ex->loop_slots[ex->loop_depth] = e->slot;
		ex->loop_depth++;
		fprintf(out, "@ltop%d\n", id);
		emit_stmts(out, e->loop_body, ex);
		ex->loop_depth--;
		Stmt *tail = e->loop_body;
		while (tail && tail->next)
			tail = tail->next;
		if (!tail || !stmt_is_terminal(tail)) /* body can fall through → loop back */
			fprintf(out, "\tjmp @ltop%d\n", id);
		fprintf(out, "@lend%d\n", id);
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
		/* neg (word or float); ~x is `xor x, -1`; !x is `x == 0` (0/1). */
		if (e->kind == EX_NEG)
			fprintf(out, "\t%%t%d =%c neg %s\n", t, qtype_of(e->lhs->rtype), a);
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
		/* Both operands share a type (typecheck); pick the QBE operand type from the LHS. A
		 * comparison yields a word via a type-suffixed `c…` mnemonic; arithmetic yields the
		 * operand type. */
		char oq = qtype_of(e->lhs->rtype);
		int is_cmp = e->kind == EX_EQ || e->kind == EX_NE || e->kind == EX_LT ||
		             e->kind == EX_GT || e->kind == EX_LE || e->kind == EX_GE;
		if (is_cmp) {
			char m[16];
			cmp_mnemonic(e->kind, oq, m, sizeof m);
			fprintf(out, "\t%%t%d =w %s %s, %s\n", t, m, a, b);
		} else {
			fprintf(out, "\t%%t%d =%c %s %s, %s\n", t, oq, arith_mnemonic(e->kind), a, b);
		}
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
static void emit_defers(FILE *out, Emit *ex); /* forward: fired at each ST_RETURN */

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
			if (s->expr->kind == EX_ARRAY) {
				/* A fixed-array local lives in the arena: bump-allocate N*8 bytes
				 * (`%r_<name>` = the base pointer) and store each element word at
				 * offset i*8 (uniform 8-byte slots, like a record). */
				Expr *a = s->expr;
				fprintf(out, "\t%%r_%s =l call $cf_alloc(w %d)\n", s->name, a->nargs * 8);
				for (int i = 0; i < a->nargs; i++) {
					char ev[96];
					emit_expr(out, a->args[i], ex, ev, sizeof ev);
					if (i == 0) {
						fprintf(out, "\tstorew %s, %%r_%s\n", ev, s->name);
					} else {
						int off = ex->tmp++;
						fprintf(out, "\t%%t%d =l add %%r_%s, %d\n", off, s->name, i * 8);
						fprintf(out, "\tstorew %s, %%t%d\n", ev, off);
					}
				}
			} else if (s->expr->kind == EX_RECORD) {
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
			} else if (s->expr->rtype.kind == TY_RECORD || s->expr->rtype.kind == TY_TUPLE ||
			           (s->expr->rtype.kind == TY_UNION && s->expr->rtype.uni->has_payload)) {
				/* A record, tuple, or boxed-union local: adopt the initializer's fresh arena
				 * pointer as this local's storage (a move, no copy). The EX_TUPLE emit built
				 * the tuple in the arena and yielded its base pointer. */
				emit_expr(out, s->expr, ex, v, sizeof v);
				fprintf(out, "\t%%r_%s =l copy %s\n", s->name, v);
			} else if (s->expr->rtype.kind == TY_STR) {
				/* A Str local holds its header pointer in an `l` slot (reserved in the
				 * entry block); store the literal's static header address into it. */
				emit_expr(out, s->expr, ex, v, sizeof v);
				fprintf(out, "\tstorel %s, %%s_%s\n", v, s->name);
			} else if (is_float_type(s->expr->rtype)) {
				/* A float local: store the value into its slot with the matching width. */
				emit_expr(out, s->expr, ex, v, sizeof v);
				fprintf(out, "\tstore%c %s, %%s_%s\n", qtype_of(s->expr->rtype), v, s->name);
			} else {
				/* A word local: its slot was reserved in the entry block (see
				 * emit_func); the binding just stores the initial value. */
				emit_expr(out, s->expr, ex, v, sizeof v);
				fprintf(out, "\tstorew %s, %%s_%s\n", v, s->name);
			}
			break;
		case ST_ASSIGN: {
			emit_expr(out, s->expr, ex, v, sizeof v);
			/* Assigning a captured word writes through its `%u_<name>` pointer (mutating the
			 * enclosing scope's slot); a plain word/float local writes its own `%s_<name>`
			 * slot with the store matching its type (`stored` for a Float64, else `storew`). */
			Type tt;
			resolve_name((Func *)ex->fn, s->name, &tt);
			char st = is_float_type(tt) ? qtype_of(tt) : 'w'; /* store<st>: stores/stored/storew */
			if (is_capture_param(ex->fn, s->name))
				fprintf(out, "\tstore%c %s, %%u_%s\n", st, v, s->name);
			else
				fprintf(out, "\tstore%c %s, %%s_%s\n", st, v, s->name);
			break;
		}
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
			/* Capture the return value first, then fire pending `defer`s (LIFO) before
			 * the `ret`: the returned word is already snapshotted into a temp, so a defer
			 * cannot change it, while a returned arena pointer stays live so a defer's
			 * mutations through it are visible to the caller (geometry_lowering's
			 * scope-exit model). */
			emit_expr(out, s->expr, ex, v, sizeof v);
			if (ex->ret_uarch && s->expr->rtype.kind == TY_INT) {
				/* Uarch function returning an Int: widen `w`→`l` so the `ret` operand
				 * matches the declared `l` return (a bare `w` temp would be ill-typed). */
				int w = ex->tmp++;
				fprintf(out, "\t%%t%d =w copy %s\n", w, v);
				int l = ex->tmp++;
				fprintf(out, "\t%%t%d =l extsw %%t%d\n", l, w);
				emit_defers(out, ex);
				fprintf(out, "\tret %%t%d\n", l);
			} else {
				emit_defers(out, ex);
				fprintf(out, "\tret %s\n", v);
			}
			break;
		case ST_LOOP: {
			/* @ltop<id>: body ; jmp @ltop<id> (back-edge) ; @lend<id>: fall-through.
			 * The back-edge is the body's fall-through, so it is only emitted when the
			 * body does not itself end in a divergence (a bare break/continue/return,
			 * which already closed the block); otherwise it would be orphaned. */
			int id = ex->lbl++;
			ex->loop_slots[ex->loop_depth] = -1; /* a statement loop yields no value */
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
		case ST_FOR: {
			/* `for x in xs { body }` desugared to a counter loop. The counter starts at
			 * -1 and is incremented at @ltop, so `continue` (which jumps to @ltop via the
			 * shared loop-label stack) advances to the next element, and the first entry
			 * lands on element 0 — no separate increment label needed, so break/continue
			 * reuse the ordinary @ltop/@lend targets. The length N is the array's comptime
			 * `alen`; the base is the array pointer (`%r_xs`). No bounds check. */
			int id = ex->lbl++;
			int N = s->expr->rtype.alen;
			fprintf(out, "\tstorew -1, %%s_%s\n", s->field);       /* counter = -1 */
			ex->loop_slots[ex->loop_depth] = -1; /* a statement `for` yields no value */
			ex->loops[ex->loop_depth++] = id;
			fprintf(out, "@ltop%d\n", id);
			int c = ex->tmp++;
			fprintf(out, "\t%%t%d =w loadw %%s_%s\n", c, s->field);
			int c1 = ex->tmp++;
			fprintf(out, "\t%%t%d =w add %%t%d, 1\n", c1, c);      /* ++counter */
			fprintf(out, "\tstorew %%t%d, %%s_%s\n", c1, s->field);
			int done = ex->tmp++;
			fprintf(out, "\t%%t%d =w csgew %%t%d, %d\n", done, c1, N); /* counter >= N ? */
			fprintf(out, "\tjnz %%t%d, @lend%d, @lbody%d\n", done, id, id);
			fprintf(out, "@lbody%d\n", id);
			char base[96];
			emit_expr(out, s->expr, ex, base, sizeof base);        /* %r_xs */
			int ext = ex->tmp++;
			fprintf(out, "\t%%t%d =l extsw %%t%d\n", ext, c1);
			int off = ex->tmp++;
			fprintf(out, "\t%%t%d =l mul %%t%d, 8\n", off, ext);
			int addr = ex->tmp++;
			fprintf(out, "\t%%t%d =l add %s, %%t%d\n", addr, base, off);
			int elem = ex->tmp++;
			fprintf(out, "\t%%t%d =w loadw %%t%d\n", elem, addr);
			fprintf(out, "\tstorew %%t%d, %%s_%s\n", elem, s->name); /* bind the loop var */
			emit_stmts(out, s->body, ex);
			ex->loop_depth--;
			Stmt *ftail = s->body;
			while (ftail && ftail->next)
				ftail = ftail->next;
			if (!ftail || !stmt_is_terminal(ftail)) /* body falls through → next iteration */
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
		case ST_YIELD: {
			/* `<- v` — store `v` into the nearest value-loop's merge slot and jump to its
			 * @lend. A guard (`if <cond> then <- v`) branches over the store/jump; a bare
			 * yield just does it (and is terminal, so nothing follows in its block). */
			int id = ex->loops[ex->loop_depth - 1];
			int slot = ex->loop_slots[ex->loop_depth - 1];
			char vv[96];
			if (s->expr) { /* guarded */
				char c[96];
				emit_expr(out, s->expr, ex, c, sizeof c);
				int g = ex->lbl++;
				fprintf(out, "\tjnz %s, @ydo%d, @yskip%d\n", c, g, g);
				fprintf(out, "@ydo%d\n", g);
				emit_expr(out, s->yval, ex, vv, sizeof vv);
				fprintf(out, "\tstorew %s, %%m%d\n", vv, slot);
				fprintf(out, "\tjmp @lend%d\n", id);
				fprintf(out, "@yskip%d\n", g);
			} else {
				emit_expr(out, s->yval, ex, vv, sizeof vv);
				fprintf(out, "\tstorew %s, %%m%d\n", vv, slot);
				fprintf(out, "\tjmp @lend%d\n", id);
			}
			break;
		}
		case ST_EXPR:
			/* Evaluate the call for effect; its result temp is simply not used. */
			emit_expr(out, s->expr, ex, v, sizeof v);
			break;
		case ST_DEFER:
			/* Schedule a `defer { … }` block; nothing is emitted here. It fires at each
			 * later `return` (emit_defers). A defer only appears at the function's top
			 * level, walked in source order, so the pending list grows monotonically and
			 * every reachable `return` sees exactly the defers that precede it. (The
			 * call/tap form registers itself when its EX_DEFER is emitted, above.) */
			if (ex->ndefers >= MAX_DEFERS)
				die(s->line, "too many `defer`s in one function");
			ex->defers[ex->ndefers].block = s->body;
			ex->defers[ex->ndefers].call = NULL;
			ex->ndefers++;
			break;
		case ST_CLOSURE:
			break; /* pure metadata — the lifted function is emitted on its own */
		}
	}
}

/* Fire the pending `defer`s in LIFO order — last reached runs first (ebnf § defer).
 * A block re-emits its body (reading locals live); a call re-issues its call with the
 * arguments snapshotted when the defer was reached, its result discarded. */
static void emit_defers(FILE *out, Emit *ex) {
	for (int i = ex->ndefers - 1; i >= 0; i--) {
		Defer *d = &ex->defers[i];
		if (!d->call) {
			emit_stmts(out, d->block, ex); /* block form (NULL body = empty defer, a no-op) */
		} else {
			Expr *call = d->call;
			int r = ex->tmp++;
			const char *rty = type_is_word(call->rtype) ? "w" : "l";
			fprintf(out, "\t%%t%d =%s call $%s(", r, rty, call->name);
			for (int j = 0; j < d->nargs; j++)
				fprintf(out, "%s%c %s", j ? ", " : "", d->argw[j], d->args[j]);
			fprintf(out, ")\n");
		}
	}
}

/* Give each if/logical expression a distinct merge-slot id, counting them in *n,
 * so emit_func can reserve all merge slots once in the entry block (see Expr.slot).
 * Walks the whole body, including loop bodies and nested sub-expressions. */
static void assign_stmt_slots(Stmt *list, int *n); /* forward: EX_LOOP body */
static void assign_expr_slots(Expr *e, int *n) {
	if (!e)
		return;
	if (e->kind == EX_IF || e->kind == EX_AND || e->kind == EX_OR || e->kind == EX_MATCH ||
	    e->kind == EX_LOOP) /* EX_LOOP merges its `<- v` yields through a slot, like an if */
		e->slot = (*n)++;
	assign_expr_slots(e->lhs, n);
	assign_expr_slots(e->rhs, n);
	assign_expr_slots(e->els, n);
	for (int i = 0; i < e->nargs; i++)
		assign_expr_slots(e->args[i], n);
	for (int i = 0; i < e->nfields; i++) /* EX_RECORD field-init values */
		assign_expr_slots(e->fvals[i], n);
	if (e->spread)
		assign_expr_slots(e->spread, n);
	for (int i = 0; i < e->narms; i++) /* EX_MATCH arm bodies */
		assign_expr_slots(e->arms[i].body, n);
	assign_stmt_slots(e->loop_body, n); /* EX_LOOP body (NULL otherwise) */
}

static void assign_stmt_slots(Stmt *list, int *n) {
	for (Stmt *s = list; s; s = s->next) {
		assign_expr_slots(s->expr, n);
		assign_expr_slots(s->yval, n); /* ST_YIELD value (NULL otherwise) */
		if (s->kind == ST_LOOP || s->kind == ST_FOR || s->kind == ST_DEFER) /* bodies with statements */
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
	char retty = qtype_of(frt); /* Int/tag-only union → w; Float64 → d; else l */
	fprintf(out, "%sfunction %c $%s(", is_main ? "export " : "", retty, fn->name);
	for (int i = 0; i < fn->nparams; i++)
		fprintf(out, "%s%c %%u_%s", i ? ", " : "",
		        param_qtype(&fn->params[i]), fn->params[i].name);
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
		} else if (fn->params[i].kind == PK_F64 || fn->params[i].kind == PK_F32) {
			/* A float param spills to its slot (8 bytes for a `d`, 4 for an `s`). */
			char qt = param_qtype(&fn->params[i]);
			fprintf(out, "\t%%s_%s =l alloc%d %d\n", n, qt == 'd' ? 8 : 4, qt == 'd' ? 8 : 4);
			fprintf(out, "\tstore%c %%u_%s, %%s_%s\n", qt, n, n);
		} else if ((fn->params[i].kind == PK_RECORD && !fn->params[i].is_ptr) ||
		           fn->params[i].kind == PK_CAPTURE_REC || fn->params[i].kind == PK_TUPLE ||
		           (fn->params[i].kind == PK_UNION && fn->params[i].uni->has_payload &&
		            !fn->params[i].is_ptr)) {
			/* A record, captured-record, tuple, or boxed-union param arrives as an arena
			 * pointer, copied into the `%r_<name>` form field/index access uses. A captured
			 * record aliases the enclosing scope's storage, so field writes there are visible.
			 * An explicit `*Record`/`*Union` pointer param is instead read directly as its
			 * `%u_<name>` incoming temp (EX_VAR TY_PTR path), so it is excluded — no `%r_`. */
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
		else if (is_float_type(fn->locals[i].type)) /* a float slot: 8 bytes for `d`, 4 for `s` */
			fprintf(out, "\t%%s_%s =l alloc%d %d\n", fn->locals[i].name,
			        fn->locals[i].type.kind == TY_F64 ? 8 : 4, fn->locals[i].type.kind == TY_F64 ? 8 : 4);
	}

	/* Likewise reserve every if/logical merge slot once here, not at each
	 * evaluation (which may recur inside a loop). */
	int nslots = 0;
	assign_stmt_slots(fn->body, &nslots);
	for (int i = 0; i < nslots; i++)
		fprintf(out, "\t%%m%d =l alloc4 4\n", i);

	Emit ex = {0};
	ex.fn = fn;
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

/* ----------------------------------------------- comptime `type` aliases - *
 *
 * `type Name = <type>` (ebnf § Data & Type Declarations) is a COMPTIME structural
 * alias: the name has no runtime identity — it is "resolved and erased before run
 * time". cfcc implements the plain-alias form as a TOKEN-LEVEL erasure pass between
 * lex and parse: collect every top-level `type` decl, drop its line, then splice the
 * target token-sequence in wherever the alias name appears. Because a `type` is a
 * pure comptime substitution, this reproduces the semantics exactly and needs ZERO
 * changes to the parser / type gate / emit — an alias for a scalar (`type Id = Uarch`),
 * an aggregate (`type P = Point`), a tuple (`type Pair = (Int, Int)`), or a generic
 * application (`type IntBox = Box[Int]`) all splice uniformly.
 *
 * ⚠ cf0 must NOT inherit these genesis narrowings (a faithful SUBSET — rejects more,
 * never mis-accepts):
 *   (1) the RECORD-body form `type T = { Int x }` (the "named tuple" that splats
 *       positionally, ebnf § Data & Type Declarations) is rejected — a separate later
 *       brick; cf0 desugars it field-by-field;
 *   (2) generic aliases `type T['A] = ...` are rejected;
 *   (3) a `type` name may not shadow a builtin / `data` / `union` head / a `union`
 *       MEMBER / another alias (hard collision error; cf0 has scoped resolve-or-shadow
 *       rules). The member guard keeps the erasure strictly subtractive;
 *   (4) the body must fit on the decl's line (collection stops at the first NEWLINE),
 *       so a bracketed body wrapping across lines is not accepted (cfcc's own source
 *       never needs it; cf0 parses the full grammar);
 *   (5) `pub type` is erased wholesale — cfcc is a single-file module with no imports,
 *       so a type's cross-module visibility (M2) has nothing to preserve yet.
 * Resolution here is ORDER-INDEPENDENT (every decl is collected before any
 * substitution) — that is CLOSER to cf0's Resolve arc than cfcc's other
 * order-dependent features, so it is not a divergence to unwind. A member/field
 * selector (an ident right after `.`) is never expanded, so `Maybe.Just` keeps its
 * member even if `Just` were aliased. */
#define MAX_TYPE_ALIASES 64

typedef struct {
	char name[64];
	const Token *body; /* target tokens (into the lexer's array), no NEWLINE/EOF */
	int nbody;
	int line;
} TypeAlias;

typedef struct { Token *buf; size_t n, cap; } TokBuf;

static void tb_push(TokBuf *tb, Token t) {
	if (tb->n == tb->cap) {
		tb->cap = tb->cap ? tb->cap * 2 : 256;
		tb->buf = realloc(tb->buf, tb->cap * sizeof(Token));
		if (!tb->buf)
			die(0, "out of memory");
	}
	tb->buf[tb->n++] = t;
}

/* Index of the alias whose name equals PascalCase token `t`, or -1. */
static int find_alias(TypeAlias *al, int nal, Token *t) {
	if (!is_type_ident(t))
		return -1;
	for (int i = 0; i < nal; i++)
		if (is_ident(t, al[i].name))
			return i;
	return -1;
}

/* Append `toks[0..n)` to `tb`, expanding any alias name (except a selector right
 * after `.`) into its target sequence — recursively, so an alias-of-alias resolves
 * to a fixpoint; a depth cap breaks a cyclic alias. */
static void alias_emit_seq(TokBuf *tb, TypeAlias *al, int nal, const Token *toks, int n, int depth) {
	if (depth > 64)
		die(toks[0].line, "cyclic `type` alias");
	TokKind prev = TK_EOF;
	for (int i = 0; i < n; i++) {
		Token *t = (Token *)&toks[i];
		int a = (prev == TK_DOT) ? -1 : find_alias(al, nal, t);
		if (a >= 0)
			alias_emit_seq(tb, al, nal, al[a].body, al[a].nbody, depth + 1);
		else
			tb_push(tb, *t);
		prev = t->kind;
	}
}

/* Set `*kw` to the keyword-token index of the declaration starting at index `i`,
 * past an optional leading `pub`. */
static void decl_kw(Token *toks, size_t i, size_t *kw) {
	*kw = is_ident(&toks[i], "pub") ? i + 1 : i;
}

/* Rewrite the token stream, erasing top-level `type` aliases (see the block comment).
 * Updates the token array and count in place; a no-op when there is no `type`. */
static void expand_type_aliases(Token **ptoks, size_t *pntoks) {
	Token *toks = *ptoks;
	size_t n = *pntoks;

	TypeAlias al[MAX_TYPE_ALIASES];
	int nal = 0;

	/* PASS 1 — collect every top-level `type Name = <body>` decl. */
	int depth = 0, line_start = 1;
	for (size_t i = 0; i < n; i++) {
		Token *t = &toks[i];
		if (t->kind == TK_NEWLINE) {
			line_start = 1;
			continue;
		}
		int here = line_start && depth == 0;
		line_start = 0;
		if (t->kind == TK_LPAREN || t->kind == TK_LBRACE || t->kind == TK_LBRACKET)
			depth++;
		else if (t->kind == TK_RPAREN || t->kind == TK_RBRACE || t->kind == TK_RBRACKET)
			depth--;
		if (!here)
			continue;
		size_t kw;
		decl_kw(toks, i, &kw);
		if (!is_ident(&toks[kw], "type"))
			continue;
		size_t k = kw + 1; /* the type name */
		if (!is_type_ident(&toks[k]))
			die(toks[k].line, "expected a type name after `type`");
		Token *nmeT = &toks[k];
		k++;
		if (toks[k].kind == TK_LBRACKET)
			die(toks[k].line, "generic `type` declarations are not supported yet (a later brick)");
		if (toks[k].kind != TK_EQ)
			die(toks[k].line, "a `type` declaration needs `=` (e.g. `type Id = Uarch`)");
		k++; /* past `=` */
		if (toks[k].kind == TK_LBRACE)
			continue; /* a record-body named tuple (grouped params) — kept in the stream for the parser */
		size_t b0 = k;
		while (toks[k].kind != TK_NEWLINE && toks[k].kind != TK_EOF)
			k++;
		if (k == b0)
			die(nmeT->line, "a `type` declaration needs a body after `=`");
		if (nal == MAX_TYPE_ALIASES)
			die(nmeT->line, "too many type aliases");
		char nm[64];
		tok_copy(nmeT, nm, sizeof nm);
		if (is_builtin_type_name(nm) || strcmp(nm, "Unit") == 0)
			die(nmeT->line, "a `type` alias may not shadow a built-in type");
		for (int j = 0; j < nal; j++)
			if (strcmp(al[j].name, nm) == 0)
				die(nmeT->line, "duplicate `type` alias");
		snprintf(al[nal].name, sizeof al[nal].name, "%s", nm);
		al[nal].body = &toks[b0];
		al[nal].nbody = (int)(k - b0);
		al[nal].line = nmeT->line;
		nal++;
	}
	if (nal == 0)
		return;

	/* A `type` name may not collide with a top-level `data`/`union` HEAD, nor with an
	 * inline `union` MEMBER name — a member is the only place a bare (unqualified)
	 * PascalCase name that is NOT a type would be silently rewritten (a member USE is
	 * `.`-qualified, so the selector guard already spares it; only the DECLARATION is
	 * bare). Guarding it keeps the erasure strictly subtractive — never a mis-lowering. */
	depth = 0;
	line_start = 1;
	for (size_t i = 0; i < n; i++) {
		Token *t = &toks[i];
		if (t->kind == TK_NEWLINE) {
			line_start = 1;
			continue;
		}
		int here = line_start && depth == 0;
		line_start = 0;
		if (t->kind == TK_LPAREN || t->kind == TK_LBRACE || t->kind == TK_LBRACKET)
			depth++;
		else if (t->kind == TK_RPAREN || t->kind == TK_RBRACE || t->kind == TK_RBRACKET)
			depth--;
		if (!here || (!is_ident(t, "data") && !is_ident(t, "union")))
			continue;
		if (is_type_ident(&toks[i + 1]) && find_alias(al, nal, &toks[i + 1]) >= 0)
			die(toks[i + 1].line, "a `type` alias collides with a `data`/`union` of the same name");
		if (!is_ident(t, "union"))
			continue;
		/* Scan the union body for member names. The first `{` after the keyword is the
		 * body (only `['T]` and `=` can precede it). A member name sits at brace-depth 1,
		 * paren-depth 0, right after `{` or a top-level `,`; a `...` spreads a union HEAD
		 * (already guarded) so it starts no new member. */
		size_t j = i + 1;
		while (j < n && toks[j].kind != TK_LBRACE && toks[j].kind != TK_EOF)
			j++;
		int bd = 0, pd = 0, want_member = 0;
		for (; j < n && toks[j].kind != TK_EOF; j++) {
			TokKind k = toks[j].kind;
			if (k == TK_LBRACE) { if (++bd == 1) want_member = 1; continue; }
			if (k == TK_RBRACE) { if (--bd == 0) break; continue; }
			if (k == TK_LPAREN || k == TK_LBRACKET) { pd++; continue; }
			if (k == TK_RPAREN || k == TK_RBRACKET) { pd--; continue; }
			if (bd != 1 || pd != 0)
				continue;
			if (k == TK_COMMA) { want_member = 1; continue; }
			if (k == TK_ELLIPSIS) { want_member = 0; continue; }
			if (want_member && is_type_ident(&toks[j]) && find_alias(al, nal, &toks[j]) >= 0)
				die(toks[j].line, "a `type` alias collides with a union member of the same name");
			want_member = 0;
		}
	}

	/* PASS 2a — copy the stream, dropping each PLAIN-alias `type` decl line (with its
	 * NEWLINE). A record-body named-tuple `type` (its name is NOT in the alias table) is
	 * KEPT, so the parser sees it as a real declaration. */
	TokBuf stripped = {0};
	depth = 0;
	line_start = 1;
	for (size_t i = 0; i < n; i++) {
		Token *t = &toks[i];
		int here = line_start && depth == 0;
		if (t->kind == TK_NEWLINE) {
			line_start = 1;
			tb_push(&stripped, *t);
			continue;
		}
		line_start = 0;
		if (t->kind == TK_LPAREN || t->kind == TK_LBRACE || t->kind == TK_LBRACKET)
			depth++;
		else if (t->kind == TK_RPAREN || t->kind == TK_RBRACE || t->kind == TK_RBRACKET)
			depth--;
		size_t kw;
		if (here && (decl_kw(toks, i, &kw), is_ident(&toks[kw], "type")) &&
		    find_alias(al, nal, &toks[kw + 1]) >= 0) {
			while (i < n && toks[i].kind != TK_NEWLINE && toks[i].kind != TK_EOF)
				i++; /* skip the decl body */
			if (i < n && toks[i].kind == TK_NEWLINE) {
				line_start = 1; /* the dropped NEWLINE ends the line — next token starts a new one */
				continue;       /* drop the NEWLINE too (loop's ++ moves past) */
			}
			i--; /* EOF: let the outer loop see it */
			continue;
		}
		tb_push(&stripped, *t);
	}

	/* PASS 2b — expand alias names in the stripped stream. */
	TokBuf out = {0};
	alias_emit_seq(&out, al, nal, stripped.buf, (int)stripped.n, 0);
	free(stripped.buf);
	*ptoks = out.buf;
	*pntoks = out.n;
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
	expand_type_aliases(&ps.toks, &lx.ntoks); /* erase comptime `type` aliases before parse */
	Program prog = {0};
	parse(&ps, &prog);
	desugar_group_calls(&prog); /* expand `{…}` group-literal args to positional field values */
	monomorphize(&prog); /* specialize generic calls before the concrete passes */
	specialize_hofs(&prog); /* fake-closure specialization: capturing closures → hidden params */
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
