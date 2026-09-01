# Maintaining this fork

## Versioning

The engine reports the **upstream ReHLDS version this fork is built on**, not a
fork-inflated one. ReHLDS derives the build number from `git rev-list --count`, so
the commits this fork adds would otherwise inflate it past build numbers upstream
has not reached yet.

`rehlds/version/upstream_base` records the upstream commit currently rebased onto;
`appversion.sh` / `appversion.bat` count from there. Both fall back to counting from
`HEAD` if the file is missing or the SHA does not resolve.

## Rebasing onto a newer upstream

1. Replay this fork's commits onto the new `rehlds/ReHLDS` master.
2. Update `rehlds/version/upstream_base` to the new upstream commit **in the same
   commit that performs the rebase**, or the reported version will be wrong.
3. Confirm: `bash rehlds/version/appversion.sh .` should print the upstream version.

## Releases

Tags are `<upstream-version>-sven<n>`, e.g. `3.15.0.898-sven1`. The upstream version
comes first so the base is obvious and releases sort correctly; `-sven<n>` increments
for further releases on the same upstream base and resets after a rebase.

## Bundled plugin versions

metamod-fallguys and ReUnion are **pinned** in `.github/workflows/build.yml` (`MMFG_REF` at
the workflow level, `REUNION_VERSION` in the bundling step) rather than tracking "latest", so
a release is reproducible and only ships versions this stack has actually been run against.
Bump them deliberately.

`MMFG_REF` is one ref for **both** halves of metamod, and that is load-bearing. The Linux
`.so` is **built from source** in the `linux` job; the Windows `.dll` comes from that ref's
release asset. Pinning them separately would ship a `gamedir/` whose halves were built from
different source, and nothing downstream would notice.

⚠ The Linux binary cannot be downloaded, and upstream's README says otherwise. It claims the
makefile forces glibc 2.24 so the shipped `.so` is portable — true of the *make* path, false
of what ships, because metamod-fallguys' own CI ends on its **cmake** script and
`-DLINK_AGAINST_OLDER_GLIBC=TRUE` is a flag no `CMakeLists.txt` in that tree reads. Measured
on release `v20260730a`: **`GLIBC_2.38`**, against deployment targets that supply `GLIBC_2.31`.
So it is compiled in the `linux` job — the only bullseye environment here; `publish` is a bare
`ubuntu-24.04` runner where building would reproduce the bug exactly — and its glibc floor is
asserted before it is uploaded.

⚠ That assertion is **separate from `glibc_test.sh`**, on a deliberately different threshold.
The engine's script hardcodes `GLIBC 2.11`, which is upstream ReHLDS's portability target for
ancient distros; metamod-fallguys cannot meet it (its own release needs 2.38, and even the
glibc-forcing path its README describes targets 2.24). The number that matters for this fork
is the documented runtime, bullseye = **2.31**. Do not reconcile the two by lowering
`glibc_test.sh` — that weakens the *engine's* guarantee to accommodate a plugin.

The packaging step derives `reunion.cfg` from ReUnion's *own* shipped config for the pinned
version and changes only the authid policy, so it does not go stale against a bump. It then
guards its own output — both plugin binaries per platform, `cid_NoSteam47/48 == 3`, and the
salt sentinel matching the engine's `REUNION_SALT_SENTINEL` — and fails the build rather than
publishing a subtly broken archive.

Publishing a GitHub release triggers the build, and the `publish` job attaches
`rehlds-sven-bin-<version>.zip` and `rehlds-sven-dbg-<version>.7z`.

> [!NOTE]
> GitHub disables automatic workflow triggers on newly created forks. If a release or
> push does not start a run, open the **Actions** tab once and confirm the prompt to
> enable workflows. Until then a build can be started manually against the tag with
> `gh workflow run "C/C++ CI" --ref <tag>`, which still satisfies the publish job.

</details>
