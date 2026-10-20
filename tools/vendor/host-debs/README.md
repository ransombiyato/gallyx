# Vendored host build tools

The development container does **not** persist `/usr` across sessions, and the
base image does not ship `cmake`, `ninja` or `shellcheck`. When the container is
reset those tools disappear and the project can no longer be configured or
built. These `.deb` files are committed so the build can be restored without a
package mirror.

Restore them with:

```bash
scripts/toolcache.sh install-host
```

`install-host` tries `apt-get install --no-download` first, so it works offline
when the dependencies are already present, and falls back to the network for any
unmet dependency. Verify afterwards with:

```bash
scripts/toolcache.sh status
```

## What is here, and why only this

| Package | Why it is vendored | Size |
| --- | --- | --- |
| `cmake` + `cmake-data` | configures and drives the build | 14 MB |
| `ninja-build` | the generator the presets use | 0.2 MB |
| `shellcheck` | lints `scripts/*.sh`; CI runs it | 4.7 MB |

Total: ~19 MB.

## What is deliberately *not* here

- **`clang` / `clang-format` / `libLLVM`.** `clang-format-19` is a 100 KB
  binary, but it links `libLLVM.so.19.1`, which is **124 MB** — larger than
  GitHub's 100 MB per-file limit, so it cannot be committed without Git LFS.
  More importantly it is not needed to reproduce the failure it was introduced
  for: `-Wsign-conversion` is now enabled for gcc as well (see `TESTING.md`
  §7), so `scripts/dev.sh build` catches the same bug class. A host with a
  package mirror can install clang the normal way via `scripts/bootstrap.sh`;
  a host without one can still build and test everything except the optional
  `clang-build` target.
- **The AArch64 cross toolchain.** It is ~150 MB, so it lives in the
  git-ignored `tools/cache/` and is restored with
  `scripts/toolcache.sh restore`. A host without a mirror can still run
  `scripts/dev.sh build` and `scripts/dev.sh test`; only `cross-test` needs it.

## Refreshing the debs

If a newer tool version is wanted, re-download and replace the files, then
update this table:

```bash
cd tools/vendor/host-debs
apt-get download cmake cmake-data ninja-build shellcheck
```

Keep the total well under 100 MB per file and prefer packages that do not drag
in LLVM.
