#!/bin/bash
set -euo pipefail

mw_scripts_dir=$(cd -- "$(/usr/bin/dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
source "$mw_scripts_dir/common.sh"

mw_usage() {
    cat <<'USAGE'
Usage: scripts/package.sh [--notarized] [--] PATH/TO/MicWarmup.app

Validates the exact app bundle and creates a ZIP with a separate SHA-256 file.
--notarized requires a Developer ID signature and a valid stapled ticket.
On success, prints the ZIP path relative to the project root.
USAGE
}

mw_notarized=false
case "${1:-}" in
    --help|-h) mw_usage; exit 0 ;;
    --notarized) mw_notarized=true; shift ;;
esac
if [[ "${1:-}" == -- ]]; then shift; fi
[[ $# -eq 1 ]] || { mw_usage >&2; exit 2; }

mw_resolve_bundle "$1"
mw_enter_repo
mw_inspect_bundle "$mw_input_bundle" "$mw_notarized"
if [[ "$mw_notarized" == true ]]; then
    [[ "$mw_actual_mode" == developer-id ]] || mw_die 'A notarized package requires a Developer ID signature.'
    mw_check_staple "$mw_input_bundle"
    mw_suffix=notarized
else
    mw_suffix="$mw_actual_mode-unnotarized"
fi

mw_output=$(mw_make_output package)
mw_zip_name="$MW_APP_NAME-$mw_version-macos-$mw_bundle_arch-$mw_suffix.zip"
mw_zip="$mw_output/$mw_zip_name"
mw_expected="$mw_version/$mw_bundle_version/$mw_bundle_arch/$mw_actual_mode/$mw_actual_signer"
mw_verify_dir="$mw_output/.verify"
mw_pending_zip="$mw_verify_dir/pending.zip"
mkdir -p "$mw_verify_dir"
mw_make_zip "$mw_input_bundle" "$mw_pending_zip"
mw_unpack_zip "$mw_pending_zip" "$mw_verify_dir/unpacked" "$mw_notarized"
mw_unpacked="$mw_verify_dir/unpacked/$MW_BUNDLE_NAME"
mw_inspect_bundle "$mw_unpacked" "$mw_notarized"
[[ "$mw_expected" == "$mw_version/$mw_bundle_version/$mw_bundle_arch/$mw_actual_mode/$mw_actual_signer" ]] ||
    mw_die 'The packaged bundle metadata or signer changed during packaging.'
if [[ "$mw_notarized" == true ]]; then mw_check_staple "$mw_unpacked"; fi
/bin/mv "$mw_pending_zip" "$mw_zip"
(
    cd -- "$mw_output"
    /usr/bin/shasum -a 256 -- "$mw_zip_name" > "$mw_zip_name.sha256"
)
mw_log "Packaged $MW_APP_NAME $mw_version ($mw_bundle_arch, $mw_suffix)."
printf '%s\n' "$mw_zip"
