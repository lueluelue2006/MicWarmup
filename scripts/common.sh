# Shared bundle and signature checks. The entry points run in macOS Bash.

readonly MW_APP_NAME=MicWarmup
readonly MW_BUNDLE_NAME=MicWarmup.app
readonly MW_APP_ID=local.doubaolab.MicWarmup
readonly MW_HELPER_NAME=mic-warmup-helper
readonly MW_HELPER_ID=local.doubaolab.MicWarmup.Helper
readonly MW_DEVELOPER_ID_REQUIREMENT='anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists'
readonly MW_AUDIO_ONLY_PATTERN='^[[:space:]]*\{[[:space:]]*"com\.apple\.security\.device\.audio-input"[[:space:]]*:[[:space:]]*true[[:space:]]*\}[[:space:]]*$'

mw_log() { printf '%s\n' "$*" >&2; }
mw_die() { mw_log "Error: $*"; exit 1; }

# Keep stdout for the artifact path and redact the home prefix in tool diagnostics.
mw_run() {
    local mw_line
    "$@" 2>&1 | while IFS= read -r mw_line || [[ -n "$mw_line" ]]; do
        if [[ -n "${HOME:-}" ]]; then mw_line=${mw_line//"$HOME"/\~}; fi
        printf '%s\n' "$mw_line" >&2
    done
}

mw_enter_repo() {
    cd -- "$mw_scripts_dir/.." 2>/dev/null || mw_die 'Cannot enter the project directory.'
}

mw_make_output() {
    mkdir -p build || mw_die 'Cannot create build/.'
    /usr/bin/mktemp -d "build/$1.XXXXXX" || mw_die 'Cannot create a new output directory under build/.'
}

mw_resolve_bundle() {
    local mw_given=$1 mw_parent
    while [[ "$mw_given" == */ && "$mw_given" != / ]]; do mw_given=${mw_given%/}; done
    [[ "${mw_given##*/}" == "$MW_BUNDLE_NAME" ]] || mw_die "Give the exact path to $MW_BUNDLE_NAME."
    mw_parent=$(/usr/bin/dirname -- "$mw_given")
    if [[ "$mw_parent" != /* ]]; then mw_parent="$PWD/$mw_parent"; fi
    mw_parent=$(cd -- "$mw_parent" 2>/dev/null && pwd -P) || mw_die 'Cannot find the supplied app directory.'
    mw_input_bundle="$mw_parent/$MW_BUNDLE_NAME"
}

mw_plist_string() {
    /usr/bin/plutil -extract "$2" raw -expect string -o - -- "$1" 2>/dev/null
}

# Populates mw_version, mw_bundle_version and mw_minimum_macos.
mw_inspect_info() {
    local mw_info=$1 mw_usage_description
    [[ -f "$mw_info" && ! -L "$mw_info" ]] &&
        /usr/bin/plutil -lint -s -- "$mw_info" >/dev/null 2>&1 || mw_die 'Missing or invalid Info.plist.'
    [[ "$(mw_plist_string "$mw_info" CFBundleIdentifier)" == "$MW_APP_ID" &&
       "$(mw_plist_string "$mw_info" CFBundleExecutable)" == "$MW_APP_NAME" &&
       "$(mw_plist_string "$mw_info" CFBundlePackageType)" == 'APPL' ]] ||
        mw_die 'Info.plist does not describe a MicWarmup app.'
    mw_version=$(mw_plist_string "$mw_info" CFBundleShortVersionString) &&
        [[ "$mw_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || mw_die 'Invalid CFBundleShortVersionString.'
    mw_bundle_version=$(mw_plist_string "$mw_info" CFBundleVersion) &&
        [[ "$mw_bundle_version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || mw_die 'Invalid CFBundleVersion.'
    mw_minimum_macos=$(mw_plist_string "$mw_info" LSMinimumSystemVersion) &&
        [[ "$mw_minimum_macos" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || mw_die 'Invalid LSMinimumSystemVersion.'
    mw_usage_description=$(mw_plist_string "$mw_info" NSMicrophoneUsageDescription) &&
        [[ "$mw_usage_description" =~ [^[:space:]] ]] || mw_die 'Missing NSMicrophoneUsageDescription.'
}

mw_architecture() {
    local mw_arches
    mw_arches=$(/usr/bin/xcrun lipo -archs "$1" 2>/dev/null) || mw_die 'Cannot read a bundle executable architecture.'
    case "$mw_arches" in
        arm64|x86_64) printf '%s\n' "$mw_arches" ;;
        'x86_64 arm64'|'arm64 x86_64') printf 'universal\n' ;;
        *) mw_die 'A bundle executable has unsupported architectures.' ;;
    esac
}

# Compare the complete entitlement dictionary, including the value's Boolean type.
mw_has_audio_only() {
    local mw_ents_json
    mw_ents_json=$(/usr/bin/plutil -convert json -o - -- - 2>/dev/null) || return 1
    [[ "$mw_ents_json" =~ $MW_AUDIO_ONLY_PATTERN ]]
}

mw_code_entitlements() {
    /usr/bin/codesign --display --architecture "$1" --entitlements - --xml "$2" 2>/dev/null ||
        /usr/bin/codesign --display --architecture "$1" --entitlements :- "$2" 2>/dev/null
}

mw_check_code() {
    local mw_target=$1 mw_identifier=$2 mw_slice=$3 mw_label=$4
    local mw_details mw_flags mw_req mw_prefix mw_leaf mw_mode mw_signer mw_stamp
    local mw_code_flags_pattern='^CodeDirectory .*flags=0x[[:xdigit:]]+\(([^)]*)\)'
    mw_details=$(/usr/bin/codesign --display --verbose=4 --architecture "$mw_slice" "$mw_target" 2>&1) ||
        mw_die "Cannot inspect the $mw_label signature ($mw_slice)."
    mw_details=$'\n'$mw_details$'\n'
    [[ "$mw_details" == *$'\nIdentifier='"$mw_identifier"$'\n'* ]] ||
        mw_die "Incorrect signing identifier on the $mw_label ($mw_slice)."
    mw_flags=$(printf '%s\n' "$mw_details" | /usr/bin/awk '/^CodeDirectory / { print; exit }')
    [[ "$mw_flags" =~ $mw_code_flags_pattern && ",${BASH_REMATCH[1]}," == *,runtime,* ]] ||
        mw_die "Hardened Runtime is missing from the $mw_label ($mw_slice)."
    if ! mw_code_entitlements "$mw_slice" "$mw_target" | mw_has_audio_only; then
        mw_die "The $mw_label ($mw_slice) must have only the audio input entitlement."
    fi

    if [[ "$mw_details" == *$'\nSignature=adhoc\n'* ]]; then
        mw_mode=adhoc
        mw_signer=''
    elif /usr/bin/codesign --verify --architecture "$mw_slice" --test-requirement "=$MW_DEVELOPER_ID_REQUIREMENT" \
            "$mw_target" >/dev/null 2>&1; then
        mw_mode=developer-id
        mw_signer=$(printf '%s\n' "$mw_details" | /usr/bin/awk -F= '/^TeamIdentifier=/ { print $2; exit }')
        mw_stamp=$(printf '%s\n' "$mw_details" | /usr/bin/awk '/^Timestamp=/ { sub(/^Timestamp=/, ""); print; exit }')
        [[ "$mw_signer" =~ ^[A-Z0-9]{10}$ && -n "$mw_stamp" ]] ||
            mw_die "The Developer ID $mw_label ($mw_slice) needs a team identifier and secure timestamp."
    else
        mw_mode=local
        mw_req=$(/usr/bin/codesign --display --architecture "$mw_slice" -r- "$mw_target" 2>/dev/null) ||
            mw_die "Cannot inspect the $mw_label designated requirement ($mw_slice)."
        mw_prefix="designated => identifier \"$mw_identifier\" and certificate leaf = H\""
        [[ "$mw_req" == "$mw_prefix"*'"' ]] ||
            mw_die "The local $mw_label ($mw_slice) must pin its signing certificate."
        mw_leaf=${mw_req#"$mw_prefix"}
        mw_leaf=${mw_leaf%\"}
        [[ "$mw_leaf" =~ ^[[:xdigit:]]{40}$ ]] ||
            mw_die "The local $mw_label ($mw_slice) has an invalid certificate requirement."
        /usr/bin/codesign --verify --architecture "$mw_slice" \
            --test-requirement "=certificate leaf = H\"$mw_leaf\"" "$mw_target" >/dev/null 2>&1 ||
            mw_die "The local $mw_label ($mw_slice) does not match its certificate requirement."
        mw_signer=$(printf '%s' "$mw_leaf" | /usr/bin/tr '[:lower:]' '[:upper:]')
    fi

    if [[ -z "$mw_actual_mode" ]]; then
        mw_actual_mode=$mw_mode
        mw_actual_signer=$mw_signer
    else
        [[ "$mw_actual_mode" == "$mw_mode" && "$mw_actual_signer" == "$mw_signer" ]] ||
            mw_die 'The app and helper architecture slices must use the same signing mode and signer.'
    fi
}

# Do not package signed extras (including diagnostics or unsealed metadata).
# The only optional entry is the file written by Apple's stapler.
mw_bundle_entry_type() {
    case "$1" in
        ''|.|Contents|Contents/MacOS|Contents/Helpers|Contents/Resources|Contents/_CodeSignature)
            mw_entry_type=directory ;;
        Contents/Info.plist|Contents/MacOS/MicWarmup|Contents/Helpers/mic-warmup-helper|\
        Contents/Resources/cover.png|Contents/Resources/LICENSE|Contents/_CodeSignature/CodeResources)
            mw_entry_type=file ;;
        Contents/CodeResources)
            [[ "$2" == true ]] || return 1
            mw_entry_type=file ;;
        *) return 1 ;;
    esac
}

mw_check_layout() {
    local mw_layout_bundle=$1 mw_layout_allow_ticket=$2
    if ! (
        cd -- "$mw_layout_bundle" 2>/dev/null || exit 1
        /usr/bin/find . -print0 2>/dev/null | while IFS= read -r -d '' mw_entry; do
            mw_bundle_entry_type "${mw_entry#./}" "$mw_layout_allow_ticket" || exit 1
            [[ ! -L "$mw_entry" ]] || exit 1
            case "$mw_entry_type" in
                directory) [[ -d "$mw_entry" ]] || exit 1 ;;
                file) [[ -f "$mw_entry" ]] || exit 1 ;;
            esac
        done
    ); then
        mw_die 'The app bundle has unexpected files, directories, or links.'
    fi
}

# Also populates mw_bundle_arch, mw_actual_mode and mw_actual_signer.
mw_inspect_bundle() {
    local mw_bundle=$1 mw_allow_ticket=${2:-false} mw_main mw_helper mw_helper_arch mw_slice
    local mw_slices
    mw_main="$mw_bundle/Contents/MacOS/$MW_APP_NAME"
    mw_helper="$mw_bundle/Contents/Helpers/$MW_HELPER_NAME"
    [[ -d "$mw_bundle" && ! -L "$mw_bundle" ]] || mw_die 'The supplied path is not an app directory.'
    mw_check_layout "$mw_bundle" "$mw_allow_ticket"
    mw_inspect_info "$mw_bundle/Contents/Info.plist"
    [[ -f "$mw_main" && -x "$mw_main" && ! -L "$mw_main" &&
       -f "$mw_helper" && -x "$mw_helper" && ! -L "$mw_helper" ]] ||
        mw_die 'The bundle is missing its app or helper executable.'
    [[ -s "$mw_bundle/Contents/Resources/cover.png" && ! -L "$mw_bundle/Contents/Resources/cover.png" &&
       -s "$mw_bundle/Contents/Resources/LICENSE" && ! -L "$mw_bundle/Contents/Resources/LICENSE" ]] ||
        mw_die 'The bundle is missing its cover image or license.'

    mw_bundle_arch=$(mw_architecture "$mw_main") || exit 1
    mw_helper_arch=$(mw_architecture "$mw_helper") || exit 1
    [[ "$mw_helper_arch" == "$mw_bundle_arch" ]] ||
        mw_die 'The app and helper have different architectures.'
    /usr/bin/codesign --verify --strict --deep --all-architectures --verbose=1 "$mw_bundle" >/dev/null 2>&1 ||
        mw_die 'Strict signature verification failed for the app bundle.'
    /usr/bin/codesign --verify --strict --all-architectures --verbose=1 "$mw_helper" >/dev/null 2>&1 ||
        mw_die 'Strict signature verification failed for the helper.'

    if [[ "$mw_bundle_arch" == universal ]]; then mw_slices=(arm64 x86_64); else mw_slices=("$mw_bundle_arch"); fi
    mw_actual_mode=''
    mw_actual_signer=''
    for mw_slice in "${mw_slices[@]}"; do
        mw_check_code "$mw_bundle" "$MW_APP_ID" "$mw_slice" app
        mw_check_code "$mw_helper" "$MW_HELPER_ID" "$mw_slice" helper
    done
}

mw_make_zip() {
    mw_run /usr/bin/ditto -c -k --keepParent --norsrc --noextattr --noacl --noqtn "$1" "$2" ||
        mw_die 'Cannot create the app ZIP.'
}

mw_unpack_zip() {
    local mw_archive=$1 mw_unpack_to=$2 mw_unpack_allow_ticket=$3 mw_listing mw_member mw_inside
    mw_listing=$(/usr/bin/unzip -Z -1 "$mw_archive" 2>/dev/null) || mw_die 'Cannot list the app ZIP contents.'
    while IFS= read -r mw_member; do
        [[ "$mw_member" == "$MW_BUNDLE_NAME" || "$mw_member" == "$MW_BUNDLE_NAME/"* ]] ||
            mw_die 'The ZIP must contain only the MicWarmup app.'
        mw_inside=${mw_member#"$MW_BUNDLE_NAME"}
        mw_inside=${mw_inside#/}
        mw_bundle_entry_type "${mw_inside%/}" "$mw_unpack_allow_ticket" ||
            mw_die 'The ZIP contains an unexpected app bundle entry.'
    done <<< "$mw_listing"
    mkdir -p "$mw_unpack_to"
    mw_run /usr/bin/ditto -x -k --norsrc --noextattr --noacl --noqtn "$mw_archive" "$mw_unpack_to" ||
        mw_die 'Cannot unpack the app ZIP for verification.'
}

mw_check_staple() {
    /usr/bin/xcrun stapler validate -q "$1" >/dev/null 2>&1 || mw_die 'A notarized package needs a valid stapled ticket.'
}
