# C! Git Branching

Check out [[versioning.md]] to get an overview of the versioning scheme.

- `master` is the LTS stable release ring branch (as far as stability is concerned). `koroviev` is merged here whenever the new "LTS" thing is ready
- `margarita` is the working branch, you branch your feature branches from here
- `behemoth` is the nightly release ring branch. `margarita` is merged here daily
- `azazello` is the alpha release ring branch. `behemoth` is merged here every now and then
- `koroviev` is the beta release ring branch. `azazello` is merged here every now and then

## Enhancement Branches

> Feature branches are branched from `margarita` and merged back into it when they are ready.

The hard sequence:

feature branch <-> `margarita` -> `behemoth` -> `azazello` -> `koroviev` -> `master`

## Nurturing Branches

> Nurturing branches are branched from the most stable branch the problem they intend to address has infected. E.g. if the problem already propagated to `azazello`, the nurturing branch is branched from `azazello`. The implemented nurturing code is merged back to the branch the nurturing branch was branched from, and the change is propagated in the opposite direction of branching up until margarita.

Example sequence (a bug in `koroviev`):

fix branch <-> `koroviev` -> `azazello` -> `behemoth` -> `margarita`

## Releasing from Release Ring Branches

Upon merging, each release ring branch is git tagged with the new version that comes from the comparison with the previous version under the same pre-release tag, and the changes in commits that happened since that time.

- `behemoth` pre-release tag: `G.E.N-nightly`
- `azazello` pre-release tag: `G.E.N-rc`
- `koroviev` pre-release tag: `G.E.N-latest`
- `master` pre-release tag: `G.E.N-stable`
