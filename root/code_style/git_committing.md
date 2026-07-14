# C! Committing

Every commit should be a single, self-contained change definition. If the change is too large, it should be broken down into smaller, self-contained changes. If the change is affecting the version (see [[versioning.md]] for details), the message should naturally reflect the kind of change being made:

- **Glory** commits start with `GLORIOUSLY` verbatim:
  - `GLORIOUSLY remove the lexer from the codebase`
  - `GLORIOUSLY change /api/v1/users method from GET to POST`
- **Enhancement** commits are structured as follows: `Enhance <thing> by <change>`:
  - `Enhance the lexer by adding support for enums`
  - `Enhance the /api/v1/users response with an optional tags field`
- **Nurture** commits are structured as follows: `Nurture <thing> by <change>`:
  - `Nurture the lexer by skipping comments entirely`
  - `Nurture the /api/v1/users by simplifying DB request`

If the commit does not affect the version, the message MUST NOT use any of the above conventions, and instead should be a simple, descriptive message in "sentence case": - `Add readme to the codebase` - `Refactor lexer for readability` - `Split parser into 6 files`
