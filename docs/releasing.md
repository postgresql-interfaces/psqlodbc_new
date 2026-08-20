# Releasing

This is a maintainer-facing guide for cutting a release. Releases are fully
automated by the GitHub Actions workflow in
[`.github/workflows/release.yml`](../.github/workflows/release.yml): pushing a
version tag builds every platform and publishes a GitHub Release with the
artifacts attached. You do not build or upload anything by hand.

## Cut a release

```bash
# 1. Be on the exact commit you want to ship, with CI already green.
git checkout main && git pull

# 2. Create an annotated version tag. It MUST be vX.Y.Z (see format rules below).
git tag -a v1.0.0 -m "psqlodbc2 1.0.0"

# 3. Push the tag. This — not the commit — is what triggers the release.
git push origin v1.0.0
```

That push starts the `Release` workflow. When it finishes, a public GitHub
Release for the tag exists with all of the artifacts below attached.

## What gets built

The workflow runs these build jobs in parallel, then a publish job:

| Job | Artifact |
|---|---|
| Build (linux-x64) | `psqlodbc2-<tag>-linux-x64.tar.gz` |
| Build (macos-arm64) | `psqlodbc2-<tag>-macos-arm64.tar.gz` |
| Build (windows-x64) | `psqlodbc2-<tag>-windows-x64.msi` |
| Build (windows-arm64) | `psqlodbc2-<tag>-windows-arm64.msi` |
| create-release | The GitHub Release, with all four artifacts attached |

The Unix jobs package the stripped shared object in a tarball. The Windows jobs
build the driver DLL with MSVC, bundle libpq's runtime DLLs (and the DSN setup
DLL) beside it, and wrap everything in a WiX MSI that registers the driver with
the Windows ODBC subsystem.

## Tag format rules

- **The tag must start with `v`.** The workflow's trigger is `tags: ['v*']`;
  a tag without the `v` prefix does nothing.
- **Use three numeric parts: `vX.Y.Z`.** The MSI version step matches
  `^v(\d+)\.(\d+)\.(\d+)$` and turns it into the 4-part `ProductVersion`
  `X.Y.Z.0` that Windows Installer requires. A tag that doesn't match (for
  example `v1.0` or `v1.0.0-rc1`) still triggers the workflow, but the MSI
  falls back to a `0.0.0.0` placeholder version — which breaks in-place MSI
  upgrades — so avoid it for real releases.
- **The Release is published only for `refs/tags/v*`.** The `create-release`
  job is gated on that ref and marked `makeLatest: true`, `draft: false`,
  `prerelease: false`, so a matching tag publishes immediately as the latest
  release. There is no manual "publish" step to click.

Remember to bump the **Version** line in [`README.md`](../README.md) to match
the tag in the same commit you tag, so the documented version tracks releases.

## Dry run (build without publishing)

To confirm all four platforms build before committing to a real tag, trigger
the workflow manually from the **Actions → Release → Run workflow** button
(the workflow declares `workflow_dispatch`). A manual run builds and uploads the
artifacts as workflow artifacts (5-day retention) but **skips** `create-release`
because the ref is a branch, not a `v*` tag — so nothing is published. Download
the artifacts from the run's summary page to sanity-check the MSIs and tarballs.

## If a release fails partway

The artifacts are only attached by the final `create-release` job, which runs
after every build job succeeds, so a partial/failed run does not produce a
half-published release. Fix the cause, then re-run:

- If the fix is a code change, delete the tag locally and remotely
  (`git tag -d v1.0.0 && git push origin :refs/tags/v1.0.0`), commit the fix,
  and re-tag.
- If only a transient CI step failed, re-run the failed jobs from the Actions
  run page — the tag does not need to move.
