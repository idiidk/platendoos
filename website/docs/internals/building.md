# Building from source

Only the sources and the scripts that build platendoos are in the repository: no rekordbox files and nothing built.
The models and the Windows address tables are build inputs that live in the
[`build-inputs`](https://github.com/idiidk/platendoos/releases/tag/build-inputs) release.

## macOS

You need a Mac with Apple Silicon, rekordbox 7 in `/Applications/rekordbox 7/`, and Xcode's command line tools
(`xcode-select --install`).

```sh
git clone https://github.com/idiidk/platendoos.git
cd platendoos
tools/build/fetch_inputs.sh     # the models (not in the repository)
./build.sh
```

That builds the mods and makes the patched copy at `~/Applications/platendoos.app`. Open it like any other app.

After changing a mod, `./install.sh` rebuilds and restarts it. It signs only the mods' library and leaves rekordbox's
own signature as it is: a newly signed rekordbox is a new app to macOS, which then asks again whether it may use the
microphone, and until that is answered rekordbox's start stands still. After `build.sh` or a patcher run macOS asks
once: click *Allow*.

For development, `RBM_DEBUGGABLE=1 ./install.sh` (or `build.sh`) also lets a debugger such as lldb attach; the next
plain `./install.sh` takes that permission away again.

## Windows

The Windows version is built on a Mac as well, cross-compiled with clang-cl:

```sh
brew install llvm lld xwin
xwin --accept-license splat --output ~/rbwin/xwin
tools/win/build.sh          # -> build/win/platendoos/{platendoos.exe, rbmods.dll, rbmods/}
```

Copy `build/win/platendoos/` to the PC (any folder) and start `platendoos.exe`, or run `platendoos patcher.exe` from
it to install it. It needs rekordbox 7.2.19 or 7.2.20 installed in `C:\Program Files\rekordbox`. `RBM_JOBS=<n> tools/win/build.sh`
compiles at most n files at a time.

## The patchers

```sh
tools/build/patcher_mac.sh      # -> build/platendoos-patcher-mac.zip
tools/win/build.sh              # -> build/win/platendoos-windows.zip, the patcher included
```

## The models

`tools/build/fetch_inputs.sh` puts the models in `models/` and the Windows tables and signatures in
`tools/win/{tables,sigs}/`. Or make them yourself with the scripts in `tools/models/` (see `models/README.md` in the
repository): for example `tools/models/htdemucs_coreml.sh` remakes the HTDemucs model (downloads about 1 GB of Python
packages).

## Releases

To publish a release, commit and push, then:

```sh
tools/build/release.sh 1.0.0    # --draft to check it on GitHub first; --inputs when a model or table changed
```

It builds both patchers at HEAD with that version in them and uploads them to the GitHub release `v1.0.0` with their
checksums. The release notes are that version's section of `CHANGELOG.md` (write it before releasing; without one,
the notes list the commits since the previous release).

## This website

The site is in `website/`; how to run, build and host it is in `website/README.md`.
