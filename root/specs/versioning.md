# C! Versioning

C! adopts (and invents!) the Genetic Versioning also called GenVer. It extends from SemVer by adopting three versioning axis - glory (SemVer major), enhancements (SemVer minor), and nurture (SemVer patch). There is one main difference between GenVer and SemVer:

> Unlike SemVer, GenVer does not reset lower axis versions when a higher axis version is incremented (e.g., incrementing the enhancements axis version for `2.3.12` results in `2.4.12`, and incrementing the glory axis version for `2.4.12` results in `3.4.12`).

This allows for preserving visual maturity of the product and detach version binding between different release rings when bug fixes, breaking changes, enhancements, or security patches land - each version increment is independent of the others and provides a real number of each of them per ring. Being completely detached from the resetting policy of SemVer, GenVer enables provision of the fixes per commit, so that each landing on a release ring can be directly depicted on the new version: three nurturing commits and two new enhancements may turn `2.3.12` into `2.5.15` on one release ring, and `3.22.17` into `3.24.19` on the other.

## Meaning

- **Glory** (SemVer major): a functional breaking change. Something that was previously there, now works differently (hopefully, better), but breaks the old way. _Must be `1` or higher. `0.x.x` versions are neglected as **Glory** holes._
- **Enhancements** (SemVer minor): a functional non-breaking change - something new is added. _Must be `0` or higher._
- **Nurture** (SemVer patch): a non-functional change - a fix or improvement that does not change the functionality but improves the quality attributes. _Must be `0` or higher._

## Pre-Releasing and Build Metadata

Pre-release versions and build metadata follow SemVer conventions. Pre-release labels take precedence over G.E.N versions themselves - G.E.N is compared per pre-release label (or its absence) so the same changeset MAY produce different versions per ring, rings do not synchronize by design. Build metadata is unused, it can be appended for user convenience.
