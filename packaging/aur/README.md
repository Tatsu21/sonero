# The AUR packages

Two packages, so an Arch user can pick between a release and the branch:

| Package | Installs | Updates when |
|---|---|---|
| [`sonero`](https://aur.archlinux.org/packages/sonero) | the newest release tag | a release is published and the package is bumped |
| [`sonero-git`](https://aur.archlinux.org/packages/sonero-git) | `main` as it stands | every `yay -Sua`, by rebuilding |

Both build against the system Qt and PipeWire, so they follow the distribution's
own updates instead of freezing a copy of them the way the AppImage does.

## Neither is on the AUR yet

AUR registrations are closed at the time of writing, so the packages cannot be
uploaded. Until that changes, point your helper at the directory instead of at a
package name — it is the same `PKGBUILD`, and the package it produces is the same
one the AUR would serve:

```sh
yay -B packaging/aur/sonero        # paru -B works the same way
makepkg -si -D packaging/aur/sonero   # or no helper at all
```

The links in the table above will start working the day the packages are pushed;
`sync-to-aur.sh` is what pushes them, and the first push is what creates the
package page.

## Why the files live here

The AUR gives each package a git repository holding nothing but a `PKGBUILD`, a
`.SRCINFO` and any install scriptlet. Keeping those in this repository means
they are reviewed with the code that broke or fixed them — a new dependency
lands in the same commit as the `depends` line it needs. `sync-to-aur.sh` copies
them over.

`.SRCINFO` is generated, never edited: the AUR's search, dependency solving and
web interface all read it rather than the `PKGBUILD`, so a hand-edit that drifts
looks right in git and wrong to every user.

## Cutting a release

Publishing the GitHub release is the whole job. CI takes it from there, in two
steps that are deliberately not one:

1. **`aur-prepare`** points the package at the new tag, derives the tarball's
   checksum, regenerates `.SRCINFO`, and then *builds and tests the package from
   the tarball that was just published*. If the release does not build, nothing
   is pushed and the AUR keeps serving the previous version.
2. **`aur-publish`** commits the bump to `main` and pushes to the AUR — after it
   waits for you to approve it in the Actions run. You are approving something
   that has already been built, which is the only kind of approval worth asking
   for.

`sonero-git` is untouched by a release: it rebuilds from `main` on its own.
Push it only when its own `PKGBUILD` changes.

### What the automation needs, once

- **A secret** named `AUR_SSH_KEY` (Settings → Secrets and variables → Actions):
  the *private* key whose public half is registered on your AUR account. Use a
  key made for this and nothing else, so revoking it costs you nothing.
- **An environment** named `aur` (Settings → Environments) with yourself as a
  **required reviewer**. This is what makes the run stop and wait. Without the
  rule the environment still exists and the job runs straight through — the
  pause is the protection rule, not the name.

### Doing it by hand instead

Nothing above replaces the scripts; CI runs the same ones.

```sh
./packaging/aur/update-version.sh 0.1.3   # pkgver, checksum, .SRCINFO
git add packaging/aur && git commit -m "aur: 0.1.3"
./packaging/aur/sync-to-aur.sh sonero     # add --push when the diff looks right
```

## Changing a PKGBUILD

```sh
$EDITOR packaging/aur/sonero/PKGBUILD
./packaging/aur/gen-srcinfo.sh
makepkg -si --dir packaging/aur/sonero   # build it once before anyone else does
```

CI checks that both `.SRCINFO` files match their `PKGBUILD`, and builds the
package on release.

## First-time publishing

The AUR authenticates over SSH and nothing else. Once per machine:

1. Register the key at <https://aur.archlinux.org/account/> → *SSH Public Key*.
2. Tell ssh to use it:

   ```
   # ~/.ssh/config
   Host aur.archlinux.org
       User aur
       IdentityFile ~/.ssh/aur
   ```

3. `./packaging/aur/sync-to-aur.sh sonero --push`

The first push to an empty AUR repository is what creates the package page.
Whoever pushes it is its maintainer, and the `# Maintainer:` line at the top of
each `PKGBUILD` should be them.

## What the packages deliberately do not do

- **No `pipewire` in `depends`.** It is the server Sonero talks to, not a library
  it links, and installing a mixer should not drag in an audio server on a
  machine that runs something else. It is an `optdepends`, which is where the
  `.deb` puts it too.
- **No spdlog.** Optional at build time, so a build host that happens to have it
  would produce a differently linked package than one that does not. The
  built-in logger costs nothing.
- **No self-updating.** Sonero's in-app update check (Settings → Updates, off
  until switched on) recognises a pacman install and refuses to write anything:
  it shows `yay -Syu sonero` and stops there. A package's files belong to
  pacman.
- **No desktop-database or icon-cache scriptlets.** Arch already ships pacman
  hooks for both, and for reloading udev rules. The one thing no hook does is
  re-apply a new rule to hardware that is already plugged in, which is all
  `sonero.install` is for.
