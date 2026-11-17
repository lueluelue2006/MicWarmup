#!/bin/bash
set -euo pipefail

mw_scripts_dir=$(cd -- "$(/usr/bin/dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
source "$mw_scripts_dir/common.sh"

mw_usage() {
    cat <<'USAGE'
Usage: scripts/notarize.sh --profile KEYCHAIN_PROFILE PATH/TO/MicWarmup.app

Submits a Developer ID build to Apple using an existing notarytool profile.
After Apple accepts it, staples and validates a new copy under build/.
On success, prints the stapled app path relative to the project root.
USAGE
}

case "${1:-}" in --help|-h) mw_usage; exit 0 ;; esac
[[ $# -eq 3 && "$1" == --profile && -n "$2" && "$2" != -* ]] || { mw_usage >&2; exit 2; }
mw_notary_profile=$2
mw_resolve_bundle "$3"
mw_enter_repo
mw_inspect_bundle "$mw_input_bundle"
[[ "$mw_actual_mode" == developer-id ]] || mw_die 'Only an app signed with Developer ID can be notarized.'
/usr/bin/xcrun --find notarytool >/dev/null 2>&1 && /usr/bin/xcrun --find stapler >/dev/null 2>&1 ||
    mw_die 'Select Xcode or Command Line Tools with notarytool and stapler.'

mw_output=$(mw_make_output notarize)
mw_upload_archive="$mw_output/submission.zip"
mw_result="$mw_output/submission.plist"
mw_copy="$mw_output/$MW_BUNDLE_NAME"
mw_make_zip "$mw_input_bundle" "$mw_upload_archive"
mw_unpack_zip "$mw_upload_archive" "$mw_output" false
mw_inspect_bundle "$mw_copy"
[[ "$mw_actual_mode" == developer-id ]] || mw_die 'The submission ZIP does not contain a Developer ID app.'

mw_log 'Submitting the app to Apple for notarization.'
/usr/bin/xcrun notarytool submit "$mw_upload_archive" --keychain-profile "$mw_notary_profile" \
    --wait --timeout 1h --output-format plist > "$mw_result" 2>/dev/null ||
    mw_die "Apple submission or wait failed. Check $mw_result for the response if available."
mw_status=$(mw_plist_string "$mw_result" status) || mw_die "Cannot read the Apple response in $mw_result."
[[ "$mw_status" == Accepted ]] || mw_die "Apple did not accept the submission. Check $mw_result."

mw_log 'Apple accepted the app. Attaching and validating the ticket.'
mw_run /usr/bin/xcrun stapler staple -q "$mw_copy" || mw_die 'The ticket could not be stapled to the app.'
mw_inspect_bundle "$mw_copy" true
mw_check_staple "$mw_copy"
printf '%s\n' "$mw_copy"
