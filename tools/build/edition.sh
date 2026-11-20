# Sourced by the build scripts (zsh): which edition to build, see docs/editions.md.
#   RBM_EDITION=full (default) | public. A tree without private/ (an exported public tree) is always public.
# Sets $edition and exports RBM_EDITION for the Python generators; needs $root (or $here) to be the repository.
_repo=${root:-$here}
edition=${RBM_EDITION:-full}
[[ $edition == full || $edition == public ]] || { echo "RBM_EDITION must be full or public, not '$edition'"; exit 1; }
[[ -d $_repo/private ]] || edition=public
export RBM_EDITION=$edition
# Copies the windows' pages (ui/, plus private/ui/ for the full edition) to <dest>/ui, replacing what was there.
edition_ui() {
    rm -rf "$1/ui"
    cp -R "$_repo/ui" "$1/"
    if [[ $edition == full ]]; then cp "$_repo"/private/ui/*(N) "$1/ui/"; fi
}
