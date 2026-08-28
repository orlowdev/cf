# boot/vendor — vendored third-party source

Third-party source committed directly into the tree (as git subtrees), so the build is
hermetic and offline. Nothing here is fetched at build time.

## qbe

The QBE backend (<https://c9x.me/qbe/>), MIT-licensed (see `qbe/LICENSE`). cf uses QBE two ways:

1. **Build-time:** the standalone `qbe` assembles the committed IL seed (`boot/seed_<os>/cf.qbe`)
   into the cf binary — the pinned-qbe link of the trust chain (`public seed → pinned qbe → cc`).
2. **Use-time:** QBE's objects are linked *into* the shipped `cf` (minus `main.o`, plus
   `cf_embed.c`) so cf translates IL→asm in-process — users need no separate qbe install.

Both come from this one vendored source.

### Pin

- upstream: `git://c9x.me/qbe.git`
- tag: `v1.3`
- commit: `c0818978acec60ebb6167fade60fb7012cbf20ca`

Vendored via `git subtree` (squashed). QBE's own `.gitignore` rides in, so an in-tree build
(`make -C boot/vendor/qbe`) leaves `*.o`, `qbe`, and the generated `config.h` untracked.

### Updating the pin

Run `boot/fetch-qbe.sh` (the update tool) — it `git subtree pull`s a new pinned tag into
`boot/vendor/qbe`. It touches the network (`git://`) only at update time, never at build time.
