# security

Thanks for helping keep C! safe.

C! is a compiler and a language, not a hosted service. Security-relevant bugs are
things like: the compiler reading or writing outside its inputs, generated code that
is memory-unsafe in a way the language promises against, or a crash that could be
weaponized inside a build pipeline.

## Reporting

Please report security issues **privately** — do not open a public Issue.

- Preferred: GitHub's **private vulnerability reporting** — the repository's
  **Security** tab → **Report a vulnerability**. (Maintainers: enable this under
  Settings → Code security.)

Include a minimal repro and what you expected to happen. We will acknowledge the
report, investigate, and credit you — humans and robots alike — when a fix ships.

## Scope

The project is pre-1.0 and moves fast; there are no supported-version guarantees yet.
Fixes land on the active branch and flow up the Bulgakov chain toward `master`.
