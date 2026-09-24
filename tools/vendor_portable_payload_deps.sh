#!/usr/bin/env bash

set -euo pipefail

readonly YYJSON_URL="https://github.com/ibireme/yyjson.git"
readonly YYJSON_TAG="0.12.0"
readonly YYJSON_COMMIT="8b4a38dc994a110abaec8a400615567bd996105f"

readonly DOUBLE_CONVERSION_URL="https://github.com/google/double-conversion.git"
readonly DOUBLE_CONVERSION_TAG="v3.4.0"
readonly DOUBLE_CONVERSION_COMMIT="9dd6227ee3e29807183e56877f0282aeb40b8b1e"

readonly PICOSHA2_URL="https://github.com/okdshin/PicoSHA2.git"
readonly PICOSHA2_TAG="v1.0.1"
readonly PICOSHA2_COMMIT="161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29"

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
readonly VENDOR_ROOT="${REPOSITORY_ROOT}/fastcarto/lib"

TEMP_ROOT=""
TRANSACTION_ROOT=""
TRANSACTION_STATE="inactive"
MOVED_OLD_NAMES=()
INSTALLED_NAMES=()

usage() {
    printf 'Usage: %s write|--check\n' "$0" >&2
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    local exit_status=$?
    local cleanup_failed=0

    trap - EXIT
    trap '' INT TERM HUP
    set +e
    if [[ -n "${TRANSACTION_ROOT}" && -d "${TRANSACTION_ROOT}" ]]; then
        case "${TRANSACTION_STATE}" in
            active)
                if rollback_vendor_write; then
                    if rm -rf "${TRANSACTION_ROOT}"; then
                        TRANSACTION_ROOT=""
                        TRANSACTION_STATE="inactive"
                    else
                        printf 'error: restored vendor snapshots but could not remove transaction workspace: %s\n' \
                            "${TRANSACTION_ROOT}" >&2
                        cleanup_failed=1
                    fi
                else
                    cleanup_failed=1
                fi
                ;;
            committed)
                if rm -rf "${TRANSACTION_ROOT}"; then
                    TRANSACTION_ROOT=""
                    TRANSACTION_STATE="inactive"
                else
                    printf 'error: committed snapshots remain installed; remove stale transaction workspace manually: %s\n' \
                        "${TRANSACTION_ROOT}" >&2
                    cleanup_failed=1
                fi
                ;;
            rolling_back|recovery_required)
                printf 'error: vendor recovery is incomplete; transaction workspace retained at: %s\n' \
                    "${TRANSACTION_ROOT}" >&2
                cleanup_failed=1
                ;;
            rolled_back|inactive)
                if rm -rf "${TRANSACTION_ROOT}"; then
                    TRANSACTION_ROOT=""
                    TRANSACTION_STATE="inactive"
                else
                    printf 'error: could not remove transaction workspace: %s\n' \
                        "${TRANSACTION_ROOT}" >&2
                    cleanup_failed=1
                fi
                ;;
            *)
                printf 'error: unknown vendor transaction state %s; workspace retained at: %s\n' \
                    "${TRANSACTION_STATE}" "${TRANSACTION_ROOT}" >&2
                cleanup_failed=1
                ;;
        esac
    fi
    if [[ -n "${TEMP_ROOT}" && -d "${TEMP_ROOT}" ]]; then
        rm -rf "${TEMP_ROOT}"
    fi
    if [[ ${exit_status} -eq 0 && ${cleanup_failed} -ne 0 ]]; then
        exit_status=1
    fi
    exit "${exit_status}"
}

signal_exit() {
    local exit_status="$1"
    trap '' INT TERM HUP
    exit "${exit_status}"
}

trap cleanup EXIT
trap 'signal_exit 130' INT
trap 'signal_exit 143' TERM
trap 'signal_exit 129' HUP

if [[ $# -ne 1 ]]; then
    usage
    exit 2
fi

readonly MODE="$1"
if [[ "${MODE}" != "write" && "${MODE}" != "--check" ]]; then
    usage
    exit 2
fi

tmp_parent="${TMPDIR:-/tmp}"
tmp_parent="${tmp_parent%/}"
TEMP_ROOT="$(mktemp -d "${tmp_parent}/fastdb-portable-payload-deps.XXXXXX")"
readonly CLONE_ROOT="${TEMP_ROOT}/clones"
readonly SNAPSHOT_ROOT="${TEMP_ROOT}/snapshots"
mkdir -p "${CLONE_ROOT}" "${SNAPSHOT_ROOT}"

clone_pinned() {
    local name="$1"
    local url="$2"
    local tag="$3"
    local expected_commit="$4"
    local checkout="${CLONE_ROOT}/${name}"

    git init --quiet "${checkout}"
    git -C "${checkout}" remote add origin "${url}"
    git -C "${checkout}" fetch --quiet --depth 1 --no-tags origin \
        "refs/tags/${tag}:refs/tags/${tag}"

    local tagged_commit
    tagged_commit="$(
        git -C "${checkout}" rev-parse --verify \
            "refs/tags/${tag}^{commit}"
    )"
    if [[ "${tagged_commit}" != "${expected_commit}" ]]; then
        fail "${name} ${tag} resolved to ${tagged_commit}, expected ${expected_commit}"
    fi

    git -C "${checkout}" -c advice.detachedHead=false checkout --quiet \
        --detach "${tagged_commit}"

    local actual_commit
    actual_commit="$(git -C "${checkout}" rev-parse HEAD)"
    if [[ "${actual_commit}" != "${expected_commit}" ]]; then
        fail "${name} ${tag} resolved to ${actual_commit}, expected ${expected_commit}"
    fi
}

copy_regular_file() {
    local source_root="$1"
    local destination_root="$2"
    local relative_path="$3"
    local current_path="${source_root}"
    local path_component
    local -a path_components

    IFS='/' read -r -a path_components <<< "${relative_path}"
    for path_component in "${path_components[@]}"; do
        current_path="${current_path}/${path_component}"
        if [[ -L "${current_path}" ]]; then
            fail "refusing symlink in retained path: ${relative_path}"
        fi
    done

    if [[ ! -f "${current_path}" ]]; then
        fail "retained upstream file is missing: ${relative_path}"
    fi

    mkdir -p "$(dirname "${destination_root}/${relative_path}")"
    cp "${current_path}" "${destination_root}/${relative_path}"
}

copy_allowlist() {
    local source_root="$1"
    local destination_root="$2"
    shift 2

    local relative_path
    mkdir -p "${destination_root}"
    for relative_path in "$@"; do
        copy_regular_file "${source_root}" "${destination_root}" "${relative_path}"
    done
}

write_upstream_metadata() {
    local output_path="$1"
    local project_name="$2"
    local url="$3"
    local tag="$4"
    local commit="$5"
    local license_identifier="$6"
    shift 6

    {
        printf '# %s upstream snapshot\n\n' "${project_name}"
        printf -- '- URL: <%s>\n' "${url}"
        printf -- '- Tag: `%s`\n' "${tag}"
        printf -- '- Commit: `%s`\n' "${commit}"
        printf -- '- License: `%s`\n\n' "${license_identifier}"
        printf '## Retained files\n\n'
        local retained_file
        for retained_file in "$@"; do
            printf -- '- `%s`\n' "${retained_file}"
        done
        printf '\n## Verification\n\n'
        printf '%s\n' '```sh'
        printf '%s\n' 'tools/vendor_portable_payload_deps.sh --check'
        printf '%s\n' '```'
    } > "${output_path}"
}

readonly -a YYJSON_FILES=(
    "src/yyjson.c"
    "src/yyjson.h"
    "LICENSE"
)

readonly -a DOUBLE_CONVERSION_FILES=(
    "double-conversion/bignum.cc"
    "double-conversion/bignum-dtoa.cc"
    "double-conversion/cached-powers.cc"
    "double-conversion/double-to-string.cc"
    "double-conversion/fast-dtoa.cc"
    "double-conversion/fixed-dtoa.cc"
    "double-conversion/string-to-double.cc"
    "double-conversion/strtod.cc"
    "double-conversion/bignum.h"
    "double-conversion/bignum-dtoa.h"
    "double-conversion/cached-powers.h"
    "double-conversion/diy-fp.h"
    "double-conversion/double-conversion.h"
    "double-conversion/double-to-string.h"
    "double-conversion/fast-dtoa.h"
    "double-conversion/fixed-dtoa.h"
    "double-conversion/ieee.h"
    "double-conversion/string-to-double.h"
    "double-conversion/strtod.h"
    "double-conversion/utils.h"
    "LICENSE"
)

readonly -a PICOSHA2_FILES=(
    "picosha2.h"
    "LICENSE"
)

build_snapshots() {
    clone_pinned "yyjson" "${YYJSON_URL}" "${YYJSON_TAG}" "${YYJSON_COMMIT}"
    copy_allowlist "${CLONE_ROOT}/yyjson" "${SNAPSHOT_ROOT}/yyjson" \
        "${YYJSON_FILES[@]}"
    write_upstream_metadata \
        "${SNAPSHOT_ROOT}/yyjson/UPSTREAM.md" \
        "yyjson" "${YYJSON_URL}" "${YYJSON_TAG}" "${YYJSON_COMMIT}" "MIT" \
        "${YYJSON_FILES[@]}"

    clone_pinned \
        "double-conversion" \
        "${DOUBLE_CONVERSION_URL}" \
        "${DOUBLE_CONVERSION_TAG}" \
        "${DOUBLE_CONVERSION_COMMIT}"
    copy_allowlist \
        "${CLONE_ROOT}/double-conversion" \
        "${SNAPSHOT_ROOT}/double-conversion" \
        "${DOUBLE_CONVERSION_FILES[@]}"
    write_upstream_metadata \
        "${SNAPSHOT_ROOT}/double-conversion/UPSTREAM.md" \
        "double-conversion" \
        "${DOUBLE_CONVERSION_URL}" \
        "${DOUBLE_CONVERSION_TAG}" \
        "${DOUBLE_CONVERSION_COMMIT}" \
        "BSD-3-Clause" \
        "${DOUBLE_CONVERSION_FILES[@]}"

    clone_pinned "picosha2" "${PICOSHA2_URL}" "${PICOSHA2_TAG}" \
        "${PICOSHA2_COMMIT}"
    copy_allowlist "${CLONE_ROOT}/picosha2" "${SNAPSHOT_ROOT}/picosha2" \
        "${PICOSHA2_FILES[@]}"
    write_upstream_metadata \
        "${SNAPSHOT_ROOT}/picosha2/UPSTREAM.md" \
        "PicoSHA2" "${PICOSHA2_URL}" "${PICOSHA2_TAG}" "${PICOSHA2_COMMIT}" \
        "MIT" "${PICOSHA2_FILES[@]}"

    local unexpected_symlink
    unexpected_symlink="$(find "${SNAPSHOT_ROOT}" -type l -print -quit)"
    if [[ -n "${unexpected_symlink}" ]]; then
        fail "generated snapshot contains a symlink: ${unexpected_symlink}"
    fi
}

rollback_vendor_write() {
    if [[ "${TRANSACTION_STATE}" != "active" ]]; then
        return
    fi

    TRANSACTION_STATE="rolling_back"

    local rollback_failed=0
    local name
    local destination
    local backup
    local -a failed_removals=()

    if [[ ${#INSTALLED_NAMES[@]} -ne 0 ]]; then
        for name in "${INSTALLED_NAMES[@]}"; do
            destination="${VENDOR_ROOT}/${name}"
            if [[ ! -e "${destination}" && ! -L "${destination}" ]]; then
                continue
            fi
            if ! rm -rf "${destination}" || \
                [[ -e "${destination}" || -L "${destination}" ]]; then
                printf 'error: failed to remove installed %s snapshot during rollback: %s\n' \
                    "${name}" "${destination}" >&2
                failed_removals+=("${name}")
                rollback_failed=1
            fi
        done
    fi

    if [[ ${#MOVED_OLD_NAMES[@]} -ne 0 ]]; then
        for name in "${MOVED_OLD_NAMES[@]}"; do
            destination="${VENDOR_ROOT}/${name}"
            backup="${TRANSACTION_ROOT}/old/${name}"

            local removal_failed=0
            if [[ ${#failed_removals[@]} -ne 0 ]]; then
                local failed_name
                for failed_name in "${failed_removals[@]}"; do
                    if [[ "${failed_name}" == "${name}" ]]; then
                        removal_failed=1
                        break
                    fi
                done
            fi
            if [[ ${removal_failed} -ne 0 ]]; then
                printf 'error: retained %s backup because its replacement could not be removed: %s\n' \
                    "${name}" "${backup}" >&2
                continue
            fi

            if [[ ! -e "${backup}" && ! -L "${backup}" ]]; then
                printf 'error: expected rollback backup is missing for %s: %s\n' \
                    "${name}" "${backup}" >&2
                rollback_failed=1
                continue
            fi
            if [[ -e "${destination}" || -L "${destination}" ]]; then
                printf 'error: cannot restore %s while destination exists; backup retained at: %s\n' \
                    "${name}" "${backup}" >&2
                rollback_failed=1
                continue
            fi
            if ! mv "${backup}" "${destination}" || \
                [[ -e "${backup}" || -L "${backup}" || \
                    (! -e "${destination}" && ! -L "${destination}") ]]; then
                printf 'error: failed to restore %s snapshot; backup retained at: %s\n' \
                    "${name}" "${backup}" >&2
                rollback_failed=1
            fi
        done
    fi

    if [[ ${rollback_failed} -ne 0 ]]; then
        TRANSACTION_STATE="recovery_required"
        printf 'error: vendor rollback incomplete; recover remaining backups from: %s/old\n' \
            "${TRANSACTION_ROOT}" >&2
        printf 'error: transaction workspace retained at: %s\n' \
            "${TRANSACTION_ROOT}" >&2
        return 1
    fi

    TRANSACTION_STATE="rolled_back"
    MOVED_OLD_NAMES=()
    INSTALLED_NAMES=()
    return 0
}

write_snapshots() {
    TRANSACTION_ROOT="$(mktemp -d "${VENDOR_ROOT}/.portable-payload-vendor.XXXXXX")"
    mkdir -p "${TRANSACTION_ROOT}/new" "${TRANSACTION_ROOT}/old"
    TRANSACTION_STATE="active"

    local name
    for name in yyjson double-conversion picosha2; do
        cp -R "${SNAPSHOT_ROOT}/${name}" "${TRANSACTION_ROOT}/new/${name}"
    done

    for name in yyjson double-conversion picosha2; do
        if [[ -e "${VENDOR_ROOT}/${name}" || -L "${VENDOR_ROOT}/${name}" ]]; then
            MOVED_OLD_NAMES+=("${name}")
            if ! mv "${VENDOR_ROOT}/${name}" \
                "${TRANSACTION_ROOT}/old/${name}"; then
                fail "failed to stage existing ${name} snapshot"
            fi
        fi
        INSTALLED_NAMES+=("${name}")
        if ! mv "${TRANSACTION_ROOT}/new/${name}" "${VENDOR_ROOT}/${name}"; then
            fail "failed to atomically install ${name} snapshot"
        fi
    done

    # This state assignment is the irrevocable commit boundary. Cleanup must
    # never remove installed snapshots after observing the committed state,
    # even if backup deletion fails or the process receives INT/TERM.
    TRANSACTION_STATE="committed"
    MOVED_OLD_NAMES=()
    INSTALLED_NAMES=()
    local cleanup_status=0
    rm -rf "${TRANSACTION_ROOT}" || cleanup_status=$?
    if [[ ${cleanup_status} -ne 0 ]]; then
        printf 'error: snapshots are committed, but backup cleanup failed; new snapshots remain installed and the workspace may require removal: %s\n' \
            "${TRANSACTION_ROOT}" >&2
        return "${cleanup_status}"
    fi
    TRANSACTION_ROOT=""
    TRANSACTION_STATE="inactive"
    printf 'Updated portable payload dependency snapshots.\n'
}

check_snapshots() {
    local comparison_failed=0
    local name
    for name in yyjson double-conversion picosha2; do
        if [[ ! -d "${VENDOR_ROOT}/${name}" ]]; then
            printf 'error: vendored snapshot is missing: fastcarto/lib/%s\n' \
                "${name}" >&2
            comparison_failed=1
            continue
        fi

        if ! git diff --no-index --no-renames -- \
            "${VENDOR_ROOT}/${name}" "${SNAPSHOT_ROOT}/${name}"; then
            comparison_failed=1
        fi
    done

    if [[ ${comparison_failed} -ne 0 ]]; then
        fail "portable payload dependency snapshots differ from their pinned upstreams"
    fi

    printf 'Portable payload dependency snapshots match pinned upstreams.\n'
}

build_snapshots

if [[ "${MODE}" == "write" ]]; then
    write_snapshots
else
    check_snapshots
fi
