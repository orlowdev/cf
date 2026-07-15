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

/* The whole program the M0 slice can express: `main` returns this Int. */
typedef struct {
	long exit_code;
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
		die(t->line, "unexpected token (M0 accepts only `pub const main = () -> <int>`)");
	return advance(p);
}

/* module = "pub" "const" "main" "=" "(" ")" "->" body
 * body   = INT | "{" "return" INT "}"                     (M0 slice) */
static void parse(Parser *p, Program *prog) {
	skip_newlines(p);
	expect_ident(p, "pub");
	expect_ident(p, "const");
	expect_ident(p, "main");
	expect(p, TK_EQ, "expected `=`");
	expect(p, TK_LPAREN, "expected `(`");
	expect(p, TK_RPAREN, "expected `)` (M0 main takes no parameters yet)");
	expect(p, TK_ARROW, "expected `->`");

	Token *t = peek(p);
	Token *code = NULL;
	if (t->kind == TK_INT) {
		code = advance(p);
	} else if (t->kind == TK_LBRACE) {
		advance(p);
		skip_newlines(p);
		expect_ident(p, "return");
		code = expect(p, TK_INT, "expected an integer exit code");
		skip_newlines(p);
		expect(p, TK_RBRACE, "expected `}`");
	} else {
		die(t->line, "expected an integer body (M0 slice)");
	}

	/* `main` lowers to a QBE `w` (32-bit word) return; reject a literal that
	 * would not survive the word so cfcc never emits a silently-truncated exit
	 * code. Literals are unsigned (ebnf Numbers), so only the upper bound bites. */
	if (code->ival > INT32_MAX)
		die(code->line, "exit code out of range (M0 supports 0..2147483647)");
	prog->exit_code = code->ival;

	skip_newlines(p);
	if (peek(p)->kind != TK_EOF)
		die(peek(p)->line, "trailing input (M0 accepts only a single `main`)");
}

/* ------------------------------------------------------------- emit QBE - */

static void emit_qbe(FILE *out, const Program *prog) {
	/* `main` returns a word (Int exit code). No page node is threaded yet —
	 * a parameterless, non-allocating main carries none (M0). */
	fprintf(out, "export function w $main() {\n");
	fprintf(out, "@start\n");
	fprintf(out, "\tret %ld\n", prog->exit_code);
	fprintf(out, "}\n");
}

/* The freestanding runtime, emitted verbatim beside the QBE-lowered code (asm
 * bypasses QBE; see ebnf Assembly). `_start` is the -nostdlib entry: it calls
 * `main` and exit-syscalls the Int return. Darwin arm64 BSD syscall: number in
 * x16, `svc #0x80`; SYS_exit = 1. QBE's arm64_apple target underscore-prefixes
 * `$main`, so the symbol is `_main`.
 *
 * argc/argv/envp wiring and the root page-node mint come in the next M0 step;
 * this slice only needs main -> exit(code). */
static void emit_runtime(FILE *out) {
	fprintf(out, ".text\n");
	fprintf(out, ".globl _start\n");
	fprintf(out, ".p2align 2\n");
	fprintf(out, "_start:\n");
	fprintf(out, "\tbl _main\n");     /* w0 = main() return, zero-extended into x0 */
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
