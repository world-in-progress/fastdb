#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd -P)"
readonly REAL_GIT="$(command -v git)"
readonly REAL_MV="$(command -v mv)"
readonly REAL_RM="$(command -v rm)"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/fastdb-vendor-test.XXXXXX")"
TEST_ROOT="$(cd "${TEST_ROOT}" && pwd -P)"
readonly TEST_ROOT
readonly REMOTE_ROOT="${TEST_ROOT}/remotes"
readonly FIXTURE_ROOT="${TEST_ROOT}/fixtures"
readonly FAKE_BIN="${TEST_ROOT}/bin"
readonly ISOLATED_HOME="${TEST_ROOT}/home"
readonly ISOLATED_XDG_CONFIG_HOME="${TEST_ROOT}/xdg-config"

CURRENT_LABEL=""
CURRENT_SCENARIO_ROOT=""
CURRENT_SANDBOX=""
CURRENT_LOG=""
CURRENT_FAILED=0
LAST_STATUS=0

cleanup() {
    "${REAL_RM}" -rf "${TEST_ROOT}"
}
trap cleanup EXIT

isolate_git_environment() {
    local variable
    while IFS='=' read -r variable _; do
        case "${variable}" in
            GIT_CONFIG_KEY_*|GIT_CONFIG_VALUE_*)
                unset "${variable}"
                ;;
        esac
    done < <(env)

    export HOME="${ISOLATED_HOME}"
    export XDG_CONFIG_HOME="${ISOLATED_XDG_CONFIG_HOME}"
    export GIT_CONFIG_NOSYSTEM=1
    export GIT_CONFIG_GLOBAL=/dev/null
    export GIT_CONFIG_COUNT=0
}

mkdir -p "${REMOTE_ROOT}" "${FIXTURE_ROOT}" "${FAKE_BIN}" \
    "${ISOLATED_HOME}" "${ISOLATED_XDG_CONFIG_HOME}"
isolate_git_environment
cp "${SCRIPT_DIR}/fixtures/vendor_test_git.sh" "${FAKE_BIN}/git"
cp "${SCRIPT_DIR}/fixtures/vendor_test_mv.sh" "${FAKE_BIN}/mv"
cp "${SCRIPT_DIR}/fixtures/vendor_test_rm.sh" "${FAKE_BIN}/rm"
chmod +x "${FAKE_BIN}/git" "${FAKE_BIN}/mv" "${FAKE_BIN}/rm"

create_remote() {
    local name="$1"
    local tag="$2"
    local worktree="${FIXTURE_ROOT}/${name}"

    mkdir -p "${worktree}" || return
    cp -R "${REPOSITORY_ROOT}/fastcarto/lib/${name}/." "${worktree}/" || \
        return
    "${REAL_GIT}" -C "${worktree}" init --quiet || return
    "${REAL_GIT}" -C "${worktree}" add . || return
    "${REAL_GIT}" -C "${worktree}" \
        -c user.name=fixture \
        -c user.email=fixture@example.invalid \
        commit --quiet -m fixture || return
    "${REAL_GIT}" -C "${worktree}" \
        -c user.name=fixture \
        -c user.email=fixture@example.invalid \
        tag -a "${tag}" -m fixture || return
    "${REAL_GIT}" clone --quiet --bare \
        "${worktree}" "${REMOTE_ROOT}/${name}.git" || return
    "${REAL_GIT}" -C "${worktree}" rev-parse HEAD
}

YYJSON_FIXTURE_COMMIT="$(create_remote yyjson 0.12.0)"
readonly YYJSON_FIXTURE_COMMIT
DOUBLE_CONVERSION_FIXTURE_COMMIT="$(
    create_remote double-conversion v3.4.0
)"
readonly DOUBLE_CONVERSION_FIXTURE_COMMIT
PICOSHA2_FIXTURE_COMMIT="$(create_remote picosha2 v1.0.1)"
readonly PICOSHA2_FIXTURE_COMMIT

snapshot_probe() {
    local name="$1"
    case "${name}" in
        yyjson)
            printf 'src/yyjson.h\n'
            ;;
        double-conversion)
            printf 'double-conversion/double-conversion.h\n'
            ;;
        picosha2)
            printf 'picosha2.h\n'
            ;;
    esac
}

prepare_sandbox() {
    local scenario="$1"
    local name

    CURRENT_LABEL="${scenario}"
    CURRENT_SCENARIO_ROOT="${TEST_ROOT}/scenarios/${scenario}"
    CURRENT_SANDBOX="${CURRENT_SCENARIO_ROOT}/repository"
    CURRENT_LOG="${CURRENT_SCENARIO_ROOT}/vendor.log"
    CURRENT_FAILED=0

    mkdir -p "${CURRENT_SANDBOX}/tools" \
        "${CURRENT_SANDBOX}/fastcarto/lib"
    cp "${REPOSITORY_ROOT}/tools/vendor_portable_payload_deps.sh" \
        "${CURRENT_SANDBOX}/tools/vendor_portable_payload_deps.sh"
    chmod +x "${CURRENT_SANDBOX}/tools/vendor_portable_payload_deps.sh"

    for name in yyjson double-conversion picosha2; do
        mkdir -p "${CURRENT_SANDBOX}/fastcarto/lib/${name}"
        printf 'old snapshot\n' > \
            "${CURRENT_SANDBOX}/fastcarto/lib/${name}/OLD_SNAPSHOT"
    done
}

run_vendor() {
    local mv_mode="$1"
    local rm_mode="$2"
    local pin_mismatch_name="${3:-}"
    local fault_state="${CURRENT_SCENARIO_ROOT}/fault"

    set +e
    env \
        LC_ALL=C \
        PATH="${FAKE_BIN}:${PATH}" \
        HOME="${ISOLATED_HOME}" \
        XDG_CONFIG_HOME="${ISOLATED_XDG_CONFIG_HOME}" \
        GIT_CONFIG_NOSYSTEM=1 \
        GIT_CONFIG_GLOBAL=/dev/null \
        GIT_CONFIG_COUNT=0 \
        FASTDB_TEST_REAL_GIT="${REAL_GIT}" \
        FASTDB_TEST_REAL_MV="${REAL_MV}" \
        FASTDB_TEST_REAL_RM="${REAL_RM}" \
        FASTDB_TEST_REMOTE_ROOT="${REMOTE_ROOT}" \
        FASTDB_TEST_YYJSON_COMMIT="${YYJSON_FIXTURE_COMMIT}" \
        FASTDB_TEST_DOUBLE_CONVERSION_COMMIT="${DOUBLE_CONVERSION_FIXTURE_COMMIT}" \
        FASTDB_TEST_PICOSHA2_COMMIT="${PICOSHA2_FIXTURE_COMMIT}" \
        FASTDB_TEST_MV_MODE="${mv_mode}" \
        FASTDB_TEST_MV_STATE="${fault_state}.mv" \
        FASTDB_TEST_RM_MODE="${rm_mode}" \
        FASTDB_TEST_RM_STATE="${fault_state}.rm" \
        FASTDB_TEST_PIN_MISMATCH_NAME="${pin_mismatch_name}" \
        bash "${CURRENT_SANDBOX}/tools/vendor_portable_payload_deps.sh" write \
        > "${CURRENT_LOG}" 2>&1
    LAST_STATUS=$?
    set -e
}

case_failure() {
    printf 'FAIL [%s]: %s\n' "${CURRENT_LABEL}" "$*" >&2
    CURRENT_FAILED=1
}

assert_status() {
    local expected_status="$1"
    if [[ ${LAST_STATUS} -ne ${expected_status} ]]; then
        case_failure "status ${LAST_STATUS}, expected ${expected_status}"
    fi
}

assert_old_snapshots_installed() {
    local name
    for name in yyjson double-conversion picosha2; do
        if [[ ! -f \
            "${CURRENT_SANDBOX}/fastcarto/lib/${name}/OLD_SNAPSHOT" ]]; then
            case_failure "old ${name} snapshot was not restored"
        fi
    done
}

assert_new_snapshots_installed() {
    local name
    local probe
    for name in yyjson double-conversion picosha2; do
        probe="$(snapshot_probe "${name}")"
        if [[ ! -f \
            "${CURRENT_SANDBOX}/fastcarto/lib/${name}/${probe}" ]]; then
            case_failure "new ${name} snapshot is not installed"
        fi
        if [[ -e \
            "${CURRENT_SANDBOX}/fastcarto/lib/${name}/OLD_SNAPSHOT" ]]; then
            case_failure "old ${name} snapshot remained installed"
        fi
    done
}

find_transaction_root() {
    find "${CURRENT_SANDBOX}/fastcarto/lib" -maxdepth 1 \
        -name '.portable-payload-vendor.*' -print -quit
}

assert_no_transaction_residue() {
    local transaction_root
    transaction_root="$(find_transaction_root)"
    if [[ -n "${transaction_root}" ]]; then
        case_failure "transaction residue remained at ${transaction_root}"
    fi
}

assert_no_annotated_tag_warning() {
    if grep -F 'is not a commit!' "${CURRENT_LOG}" >/dev/null; then
        case_failure "annotated-tag clone warning was emitted"
    fi
}

assert_recovery_backup() {
    local name="$1"
    local clear_destination="${2:-0}"
    local transaction_root
    local backup_path
    local destination

    transaction_root="$(find_transaction_root)"
    if [[ -z "${transaction_root}" ]]; then
        case_failure "rollback failure discarded the transaction workspace"
        return
    fi

    backup_path="${transaction_root}/old/${name}"
    destination="${CURRENT_SANDBOX}/fastcarto/lib/${name}"
    if [[ ! -f "${backup_path}/OLD_SNAPSHOT" ]]; then
        case_failure "recoverable ${name} backup is missing at ${backup_path}"
        return
    fi
    if ! grep -F "${transaction_root}" "${CURRENT_LOG}" >/dev/null; then
        case_failure "diagnostic did not print recovery path ${transaction_root}"
    fi
    if [[ -e "${destination}" ]]; then
        if [[ ${clear_destination} -eq 1 ]]; then
            "${REAL_RM}" -rf "${destination}"
        else
            case_failure "recovery destination still exists for ${name}"
            return
        fi
    fi

    "${REAL_MV}" "${backup_path}" "${destination}"
    if [[ ! -f "${destination}/OLD_SNAPSHOT" ]]; then
        case_failure "backup at ${backup_path} was not manually recoverable"
    fi
}

finish_case() {
    assert_no_annotated_tag_warning
    if [[ ${CURRENT_FAILED} -ne 0 ]]; then
        printf '%s\n' "--- ${CURRENT_LABEL} captured output ---" >&2
        sed -n '1,200p' "${CURRENT_LOG}" >&2
        return 1
    fi
    printf 'PASS [%s]\n' "${CURRENT_LABEL}"
}

test_successful_write() {
    prepare_sandbox successful-write
    run_vendor none none
    assert_status 0
    assert_new_snapshots_installed
    assert_no_transaction_residue
    finish_case
}

test_committed_cleanup_fault() {
    local mode="$1"
    local expected_status="$2"
    prepare_sandbox "${mode}"
    run_vendor none "${mode}"
    assert_status "${expected_status}"
    assert_new_snapshots_installed
    assert_no_transaction_residue
    finish_case
}

test_successful_rollback() {
    prepare_sandbox successful-rollback
    run_vendor install-failure none
    assert_status 1
    assert_old_snapshots_installed
    assert_no_transaction_residue
    finish_case
}

test_rollback_term_reentry() {
    prepare_sandbox rollback-term-reentry
    run_vendor rollback-term none
    assert_status 1
    assert_old_snapshots_installed
    assert_no_transaction_residue
    finish_case
}

test_restore_failure_retains_backup() {
    prepare_sandbox restore-failure
    run_vendor restore-failure none
    assert_status 1
    assert_recovery_backup double-conversion
    finish_case
}

test_remove_failure_retains_backup() {
    prepare_sandbox remove-failure
    run_vendor active-remove-failure active-remove-failure
    assert_status 1
    assert_recovery_backup double-conversion 1
    finish_case
}

test_signal_exit() {
    local signal_name="$1"
    local expected_status="$2"
    prepare_sandbox "signal-${signal_name}"
    run_vendor "signal-${signal_name}" none
    assert_status "${expected_status}"
    assert_old_snapshots_installed
    assert_no_transaction_residue
    finish_case
}

test_pin_mismatch_preserves_snapshots() {
    prepare_sandbox pin-mismatch
    run_vendor none none yyjson
    assert_status 1
    assert_old_snapshots_installed
    assert_no_transaction_residue
    if ! grep -F 'expected 8b4a38dc994a110abaec8a400615567bd996105f' \
        "${CURRENT_LOG}" >/dev/null; then
        case_failure "pin mismatch diagnostic is missing the expected commit"
    fi
    finish_case
}

failures=0
if ! test_successful_write; then
    failures=1
fi
if ! test_committed_cleanup_fault committed-failure 73; then
    failures=1
fi
if ! test_committed_cleanup_fault committed-term 143; then
    failures=1
fi
if ! test_successful_rollback; then
    failures=1
fi
if ! test_rollback_term_reentry; then
    failures=1
fi
if ! test_restore_failure_retains_backup; then
    failures=1
fi
if ! test_remove_failure_retains_backup; then
    failures=1
fi
if ! test_signal_exit int 130; then
    failures=1
fi
if ! test_signal_exit term 143; then
    failures=1
fi
if ! test_signal_exit hup 129; then
    failures=1
fi
if ! test_pin_mismatch_preserves_snapshots; then
    failures=1
fi

if [[ ${failures} -ne 0 ]]; then
    exit 1
fi

printf 'PASS: vendor transaction and Git isolation regressions\n'
