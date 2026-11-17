#!/bin/bash
set -euo pipefail

mw_scripts_dir=$(cd -- "$(/usr/bin/dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
source "$mw_scripts_dir/common.sh"
mw_enter_repo

mw_usage() {
    cat <<'USAGE'
Usage: scripts/build.sh [--arch arm64|x86_64|universal] [--sign adhoc|local|developer-id] [--identity SHA1]

Defaults: host architecture, and ad hoc signing when no mode is configured.
MW_ARCH, MW_SIGNING_MODE and MW_SIGNING_IDENTITY set defaults for the flags.
The ignored .local/signing.env may set the two signing variables as KEY=VALUE lines.
On success, prints the app path relative to the project root.
USAGE
}

mw_cli_arch=''
mw_cli_mode=''
mw_cli_identity=''
while (( $# )); do
    case "$1" in
        --arch|--sign|--identity)
            [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || mw_die "$1 requires a value."
            case "$1" in
                --arch) mw_cli_arch=$2 ;;
                --sign) mw_cli_mode=$2 ;;
                --identity) mw_cli_identity=$2 ;;
            esac
            shift 2
            ;;
        --help|-h) mw_usage; exit 0 ;;
        *) mw_usage >&2; exit 2 ;;
    esac
done

mw_host_arch=$(/usr/bin/uname -m)
case "$mw_host_arch" in arm64|x86_64) ;; *) mw_die 'An Apple Silicon or Intel Mac is required.' ;; esac
mw_build_architecture=${mw_cli_arch:-${MW_ARCH:-$mw_host_arch}}
case "$mw_build_architecture" in
    arm64|x86_64|universal) ;;
    *) mw_die 'Choose --arch arm64, x86_64, or universal.' ;;
esac

mw_config_mode=''
mw_config_identity=''
# Read the config as data rather than executing it as shell code. An explicit
# ad hoc request intentionally ignores any machine-local identity or config.
if [[ "${mw_cli_mode:-${MW_SIGNING_MODE:-}}" != adhoc && -e .local/signing.env ]]; then
    [[ -f .local/signing.env && -r .local/signing.env ]] || mw_die 'Cannot read .local/signing.env.'
    while IFS= read -r mw_line || [[ -n "$mw_line" ]]; do
        case "${mw_line%$'\r'}" in
            ''|'#'*) ;;
            MW_SIGNING_MODE=?*)
                [[ -z "$mw_config_mode" ]] || mw_die 'Duplicate signing mode in .local/signing.env.'
                mw_config_mode=${mw_line#*=}
                mw_config_mode=${mw_config_mode%$'\r'}
                ;;
            MW_SIGNING_IDENTITY=?*)
                [[ -z "$mw_config_identity" ]] || mw_die 'Duplicate signing identity in .local/signing.env.'
                mw_config_identity=${mw_line#*=}
                mw_config_identity=${mw_config_identity%$'\r'}
                ;;
            *) mw_die 'Use nonempty KEY=VALUE lines for the two signing variables in .local/signing.env.' ;;
        esac
    done < .local/signing.env
fi

mw_build_sign_mode=${mw_cli_mode:-${MW_SIGNING_MODE:-$mw_config_mode}}
mw_build_identity=${mw_cli_identity:-${MW_SIGNING_IDENTITY:-$mw_config_identity}}
if [[ -z "$mw_build_sign_mode" ]]; then
    [[ -z "$mw_build_identity" ]] || mw_die 'A signing identity requires an explicit signing mode.'
    mw_build_sign_mode=adhoc
    mw_log 'No signing mode configured; using ad hoc signing (unnotarized).'
fi
case "$mw_build_sign_mode" in
    adhoc)
        [[ -z "$mw_cli_identity" ]] || mw_die '--identity requires local or developer-id signing.'
        mw_build_identity=-
        ;;
    local|developer-id)
        [[ "$mw_build_identity" =~ ^[0-9a-fA-F]{40}$ ]] ||
            mw_die 'Set MW_SIGNING_IDENTITY to the 40-digit SHA-1 of a valid code signing identity.'
        mw_build_identity=$(printf '%s' "$mw_build_identity" | /usr/bin/tr '[:lower:]' '[:upper:]')
        mw_identity_kind=$(
            /usr/bin/security find-identity -v -p codesigning 2>/dev/null |
                /usr/bin/awk -v wanted="$mw_build_identity" '
                    $2 == wanted {
                        found = 1
                        if ($0 ~ /"Developer ID Application: /) developer = 1
                    }
                    END {
                        if (found) print developer ? "developer-id" : "other"
                    }'
        ) || mw_die 'Cannot look up code signing identities in the keychain.'
        [[ -n "$mw_identity_kind" ]] || mw_die 'The selected code signing identity is not valid or not available in the keychain.'
        if [[ "$mw_build_sign_mode" == developer-id ]]; then
            [[ "$mw_identity_kind" == developer-id ]] || mw_die 'Developer ID signing requires a Developer ID Application identity.'
        else
            [[ "$mw_identity_kind" != developer-id ]] || mw_die 'Use --sign developer-id for a Developer ID Application identity.'
        fi
        ;;
    *) mw_die 'Choose --sign adhoc, local, or developer-id.' ;;
esac

/usr/bin/xcrun --sdk macosx --find clang >/dev/null 2>&1 ||
    mw_die 'Install the Xcode Command Line Tools and select a macOS SDK.'
mw_inspect_info Support/Info.plist
[[ -s Resources/cover.png && -s LICENSE ]] || mw_die 'Missing Resources/cover.png or LICENSE.'
mw_has_audio_only < Support/Entitlements.plist || mw_die 'Support/Entitlements.plist must permit only audio input.'
mw_output=$(mw_make_output build)
mw_bundle="$mw_output/$MW_BUNDLE_NAME"
mw_main="$mw_bundle/Contents/MacOS/$MW_APP_NAME"
mw_helper="$mw_bundle/Contents/Helpers/$MW_HELPER_NAME"
mw_test_output="$mw_output/tests"
mkdir -p "$mw_test_output"

mw_flags=(-std=c11 -O2 -Wall -Wextra -Werror "-mmacosx-version-min=$mw_minimum_macos" -Isrc)
mw_monitor=(src/DoubaoMonitor.m src/warmup.c src/hal_probe.c)
mw_compile() { mw_run /usr/bin/xcrun --sdk macosx clang "${mw_flags[@]}" "$@"; }

mw_log "Running five mock test suites on $mw_host_arch."
mw_compile -arch "$mw_host_arch" tests/test_warmup.c src/warmup.c -o "$mw_test_output/test_warmup"
mw_run "$mw_test_output/test_warmup"
mw_compile -arch "$mw_host_arch" tests/test_control.c src/control.c src/warmup.c -o "$mw_test_output/test_control"
mw_run "$mw_test_output/test_control"
mw_compile -arch "$mw_host_arch" -Dmain=MWHelperEntry -c src/helper.c -o "$mw_test_output/helper_entry.o"
mw_compile -arch "$mw_host_arch" tests/test_helper.c src/control.c src/warmup.c \
    "$mw_test_output/helper_entry.o" -o "$mw_test_output/test_helper"
mw_run "$mw_test_output/test_helper"
mw_compile -arch "$mw_host_arch" tests/test_auto_warmup.c src/auto_warmup.c src/warmup.c -o "$mw_test_output/test_auto_warmup"
mw_run "$mw_test_output/test_auto_warmup"
mw_compile -arch "$mw_host_arch" -fobjc-arc tests/test_app_renewal.m src/auto_warmup.c "${mw_monitor[@]}" \
    -framework AppKit -framework CoreAudio -o "$mw_test_output/test_app_renewal"
mw_run "$mw_test_output/test_app_renewal"

mw_compile_production() {
    local mw_target_arch=$1 mw_target_helper=$2 mw_target_main=$3 mw_target_check=$4
    mw_log "Building MicWarmup for $mw_target_arch."
    mw_compile -arch "$mw_target_arch" src/helper.c src/warmup.c src/control.c src/hal_probe.c \
        -framework CoreAudio -o "$mw_target_helper"
    mw_compile -arch "$mw_target_arch" -fobjc-arc src/App.m src/auto_warmup.c "${mw_monitor[@]}" \
        -framework AppKit -framework CoreAudio -o "$mw_target_main"
    mw_compile -arch "$mw_target_arch" -fobjc-arc -DMW_CHECK_AUTO_STANDALONE "${mw_monitor[@]}" \
        -framework AppKit -framework CoreAudio -o "$mw_target_check"
}

mkdir -p "$mw_bundle/Contents/"{MacOS,Helpers,Resources}
cp -X Support/Info.plist "$mw_bundle/Contents/Info.plist"
cp -X Resources/cover.png LICENSE "$mw_bundle/Contents/Resources/"
if [[ "$mw_build_architecture" == universal ]]; then
    for mw_arch in arm64 x86_64; do
        mkdir -p "$mw_output/$mw_arch"
        mw_compile_production "$mw_arch" "$mw_output/$mw_arch/$MW_HELPER_NAME" \
            "$mw_output/$mw_arch/$MW_APP_NAME" "$mw_output/$mw_arch/check-auto"
    done
    for mw_arch_binary in "$mw_helper" "$mw_main" "$mw_output/check-auto"; do
        mw_binary_name=${mw_arch_binary##*/}
        mw_run /usr/bin/xcrun lipo -create "$mw_output/arm64/$mw_binary_name" \
            "$mw_output/x86_64/$mw_binary_name" -output "$mw_arch_binary"
    done
else
    mw_compile_production "$mw_build_architecture" "$mw_helper" "$mw_main" "$mw_output/check-auto"
fi

mw_sign() {
    local mw_target=$1 mw_identifier=$2 mw_label=$3
    local mw_sign_options=(--force --sign "$mw_build_identity" --identifier "$mw_identifier" \
        --options runtime --entitlements Support/Entitlements.plist)
    case "$mw_build_sign_mode" in
        adhoc) mw_sign_options+=(--timestamp=none) ;;
        local)
            mw_sign_options+=(--timestamp=none \
                --requirements "=designated => identifier \"$mw_identifier\" and certificate leaf = H\"$mw_build_identity\"")
            ;;
        developer-id) mw_sign_options+=(--timestamp) ;;
    esac
    /usr/bin/codesign "${mw_sign_options[@]}" "$mw_target" >/dev/null 2>&1 ||
        mw_die "Signing the $mw_label failed ($mw_build_sign_mode). Check keychain access and trust for certificate signing, and network access for a Developer ID timestamp."
}

mw_log "Signing the helper and app ($mw_build_sign_mode)."
mw_sign "$mw_helper" "$MW_HELPER_ID" helper
mw_sign "$mw_bundle" "$MW_APP_ID" app
mw_inspect_bundle "$mw_bundle"
[[ "$mw_bundle_arch" == "$mw_build_architecture" && "$mw_actual_mode" == "$mw_build_sign_mode" ]] ||
    mw_die 'The built app does not match the requested architecture and signing mode.'
if [[ "$mw_build_sign_mode" != adhoc ]]; then
    /usr/bin/codesign --verify --all-architectures \
        --test-requirement "=certificate leaf = H\"$mw_build_identity\"" "$mw_bundle" "$mw_helper" >/dev/null 2>&1 ||
        mw_die 'The app or helper does not match the selected signing certificate.'
fi

printf '{\n  "bundle": "%s",\n  "version": "%s",\n  "bundle_version": "%s",\n  "minimum_macos": "%s",\n  "architecture": "%s",\n  "signing_mode": "%s"\n}\n' \
    "$MW_BUNDLE_NAME" "$mw_version" "$mw_bundle_version" "$mw_minimum_macos" "$mw_bundle_arch" "$mw_actual_mode" \
    > "$mw_output/manifest.json"
mw_log 'Strict signature verification passed. Build output is not notarized.'
printf '%s\n' "$mw_bundle"
