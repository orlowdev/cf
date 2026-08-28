/* qbe_embed.c — cf's in-process entry into the vendored QBE (boot/vendor/qbe).
 *
 * The shipped cf links QBE's objects (see boot/build.sh) MINUS qbe's own main.o —
 * its `main` symbol collides with cf's entry `_main`. Excluding main.o drops QBE's
 * definitions of the globals `T` and `debug[]` (used across the passes) and the
 * driver loop, so this file supplies them and exposes ONE entry:
 *
 *     int cf_qbe_run(int target_id, const char *in_path, const char *out_path)
 *
 * which translates a QBE IL file to a target asm file, in-process. It reuses QBE's
 * own file I/O (fopen/fclose) — cf hands it temp paths, so no memory-stream patching.
 * The callbacks below are copied verbatim from qbe's main.c (dbg is always 0 here).
 *
 * This file lives OUTSIDE the subtree on purpose: the vendored tree stays pristine so
 * `git subtree pull` never conflicts with cf's code. Built with -I boot/vendor/qbe. */
#include "all.h"

/* Normally defined in qbe's main.c (which we exclude from cf's link). */
Target T;
char debug['Z'+1];

static FILE *outf;
static int dbg;

extern Target T_amd64_sysv;
extern Target T_amd64_apple;
extern Target T_amd64_win;
extern Target T_arm64;
extern Target T_arm64_apple;
extern Target T_rv64;

/* --- callbacks: verbatim from qbe main.c:41-128 --- */

static void
data(Dat *d)
{
	if (dbg)
		return;
	emitdat(d, outf);
	if (d->type == DEnd) {
		fputs("/* end data */\n\n", outf);
		freeall();
	}
}

static void
func(Fn *fn)
{
	uint n;

	if (dbg)
		fprintf(stderr, "**** Function %s ****", fn->name);
	if (debug['P']) {
		fprintf(stderr, "\n> After parsing:\n");
		printfn(fn, stderr);
	}
	T.abi0(fn);
	fillcfg(fn);
	filluse(fn);
	promote(fn);
	filluse(fn);
	ssa(fn);
	filluse(fn);
	ssacheck(fn);
	fillalias(fn);
	loadopt(fn);
	filluse(fn);
	fillalias(fn);
	coalesce(fn);
	filluse(fn);
	filldom(fn);
	ssacheck(fn);
	gvn(fn);
	fillcfg(fn);
	simplcfg(fn);
	filluse(fn);
	filldom(fn);
	gcm(fn);
	filluse(fn);
	ssacheck(fn);
	if (T.cansel) {
		ifconvert(fn);
		fillcfg(fn);
		filluse(fn);
		filldom(fn);
		ssacheck(fn);
	}
	T.abi1(fn);
	simpl(fn);
	fillcfg(fn);
	filluse(fn);
	T.isel(fn);
	fillcfg(fn);
	filllive(fn);
	fillloop(fn);
	fillcost(fn);
	spill(fn);
	rega(fn);
	fillcfg(fn);
	simpljmp(fn);
	fillcfg(fn);
	assert(fn->rpo[0] == fn->start);
	for (n=0;; n++)
		if (n == fn->nblk-1) {
			fn->rpo[n]->link = 0;
			break;
		} else
			fn->rpo[n]->link = fn->rpo[n+1];
	if (!dbg) {
		T.emitfn(fn, outf);
		fprintf(outf, "/* end function %s */\n\n", fn->name);
	} else
		fprintf(stderr, "\n");
	freeall();
}

static void
dbgfile(char *fn)
{
	emitdbgfile(fn, outf);
}

/* --- cf's entry --- */

/* target_id mirrors the driver's target selection:
 *   0 arm64_apple (darwin)   1 arm64 (bare ELF)
 *   2 amd64_apple            3 amd64_sysv           4 rv64
 * Returns 0 on success; 2 bad target, 3 cannot open input, 4 cannot open output. */
int
cf_qbe_run(int target_id, const char *in_path, const char *out_path)
{
	Target *sel;
	FILE *inf;

	switch (target_id) {
	case 0: sel = &T_arm64_apple; break;
	case 1: sel = &T_arm64;       break;
	case 2: sel = &T_amd64_apple; break;
	case 3: sel = &T_amd64_sysv;  break;
	case 4: sel = &T_rv64;        break;
	default: return 2;
	}
	T = *sel;
	memset(debug, 0, sizeof debug);
	dbg = 0;

	inf = fopen(in_path, "r");
	if (!inf)
		return 3;
	outf = fopen(out_path, "w");
	if (!outf) {
		fclose(inf);
		return 4;
	}

	parse(inf, (char *)in_path, dbgfile, data, func);
	fclose(inf);
	T.emitfin(outf);
	fclose(outf);
	return 0;
}

#ifdef __linux__
/* cf's asm FFI (boot/src/qbe.cf) calls `_cf_qbe_run` — the darwin Mach-O name, where C symbols get a
 * leading underscore. Linux ELF has no such prefix, so the same `bl _cf_qbe_run` would be undefined.
 * Export `_cf_qbe_run` as an explicit alias (ELF permits a leading underscore in a symbol name) so the
 * one FFI call links unchanged on both platforms — no per-OS asm in the compiler, no reseed. */
extern int _cf_qbe_run(int, const char *, const char *) __attribute__((alias("cf_qbe_run")));
#endif
