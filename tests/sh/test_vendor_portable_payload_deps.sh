#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd -P)"
readonly REAL_GIT="$(command -v git)"
readonly REAL_RM="$(command -v rm)"
readonly TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/fastdb-vendor-test.XXXXXX")"
readonly REMOTE_ROOT="${TEST_ROOT}/remotes"
readonly FIXTURE_ROOT="${TEST_ROOT}/fixtures"
readonly FAKE_BIN="${TEST_ROOT}/bin"

cleanup() {
    "${REAL_RM}" -rf "${TEST_ROOT}"
}
trap cleanup EXIT

mkdir -p "${REMOTE_ROOT}" "${FIXTURE_ROOT}" "${FAKE_BIN}"
cp "${SCRIPT_DIR}/fixtures/vendor_test_git.sh" "${FAKE_BIN}/git"
cp "${SCRIPT_DIR}/fixtures/vendor_test_rm.sh" "${FAKE_BIN}/rm"
chmod +x "${FAKE_BIN}/git" "${FAKE_BIN}/rm"

create_remote() {
    local name="$1"
    local tag="$2"
    local worktree="${FIXTURE_ROOT}/${name}"

    mkdir -p "${worktree}"
    cp -R "${REPOSITORY_ROOT}/fastcarto/lib/${name}/." "${worktree}/"
    "${REAL_GIT}" -C "${worktree}" init --quiet
    "${REAL_GIT}" -C "${worktree}" add .
    "${REAL_GIT}" -C "${worktree}" \
        -c user.name=fixture \
        -c user.email=fixture@example.invalid \
        commit --quiet -m fixture
    "${REAL_GIT}" -C "${worktree}" \
        -c user.name=fixture \
        -c user.email=fixture@example.invalid \
        tag -a "${tag}" -m fixture
    "${REAL_GIT}" clone --quiet --bare \
        "${worktree}" "${REMOTE_ROOT}/${name}.git"
    "${REAL_GIT}" -C "${worktree}" rev-parse HEAD
}

readonly YYJSON_FIXTURE_COMMIT="$(create_remote yyjson 0.12.0)"
readonly DOUBLE_CONVERSION_FIXTURE_COMMIT="$(
    create_remote double-conversion v3.4.0
)"
readonly PICOSHA2_FIXTURE_COMMIT="$(create_remote picosha2 v1.0.1)"

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

run_cleanup_fault() {
    local fault_mode="$1"
    local expected_status="$2"
    local scenario_root="${TEST_ROOT}/${fault_mode}"
    local sandbox="${scenario_root}/repository"
    local log="${scenario_root}/vendor.log"
    local fault_state="${scenario_root}/rm-fault-fired"
    local status
    local failed=0
    local name

    mkdir -p "${sandbox}/tools" "${sandbox}/fastcarto/lib"
    cp "${REPOSITORY_ROOT}/tools/vendor_portable_payload_deps.sh" \
        "${sandbox}/tools/vendor_portable_payload_deps.sh"
    chmod +x "${sandbox}/tools/vendor_portable_payload_deps.sh"

    for name in yyjson double-conversion picosha2; do
        mkdir -p "${sandbox}/fastcarto/lib/${name}"
        printf 'old snapshot\n' > \
            "${sandbox}/fastcarto/lib/${name}/OLD_SNAPSHOT"
    done

    set +e
    env \
        LC_ALL=C \
        PATH="${FAKE_BIN}:${PATH}" \
        FASTDB_TEST_REAL_GIT="${REAL_GIT}" \
        FASTDB_TEST_REAL_RM="${REAL_RM}" \
        FASTDB_TEST_REMOTE_ROOT="${REMOTE_ROOT}" \
        FASTDB_TEST_YYJSON_COMMIT="${YYJSON_FIXTURE_COMMIT}" \
        FASTDB_TEST_DOUBLE_CONVERSION_COMMIT="${DOUBLE_CONVERSION_FIXTURE_COMMIT}" \
        FASTDB_TEST_PICOSHA2_COMMIT="${PICOSHA2_FIXTURE_COMMIT}" \
        FASTDB_TEST_RM_MODE="${fault_mode}" \
        FASTDB_TEST_RM_STATE="${fault_state}" \
        bash "${sandbox}/tools/vendor_portable_payload_deps.sh" write \
        > "${log}" 2>&1
    status=$?
    set -e

    if [[ ${status} -ne ${expected_status} ]]; then
        printf 'FAIL [%s]: status %s, expected %s\n' \
            "${fault_mode}" "${status}" "${expected_status}" >&2
        failed=1
    fi

    if [[ ! -e "${fault_state}" ]]; then
        printf 'FAIL [%s]: cleanup fault was not injected\n' \
            "${fault_mode}" >&2
        failed=1
    fi

    for name in yyjson double-conversion picosha2; do
        if [[ ! -f "${sandbox}/fastcarto/lib/${name}/$(snapshot_probe "${name}")" ]]; then
            printf 'FAIL [%s]: committed %s snapshot was rolled back\n' \
                "${fault_mode}" "${name}" >&2
            failed=1
        fi
        if [[ -e "${sandbox}/fastcarto/lib/${name}/OLD_SNAPSHOT" ]]; then
            printf 'FAIL [%s]: old %s snapshot remained installed\n' \
                "${fault_mode}" "${name}" >&2
            failed=1
        fi
    done

    if grep -F 'is not a commit!' "${log}" >/dev/null; then
        printf 'FAIL [%s]: annotated-tag clone warning was emitted\n' \
            "${fault_mode}" >&2
        failed=1
    fi

    if [[ ${failed} -ne 0 ]]; then
        printf '%s\n' "--- ${fault_mode} captured output ---" >&2
        sed -n '1,160p' "${log}" >&2
        return 1
    fi

    printf 'PASS [%s]: cleanup fault preserved committed snapshots\n' \
        "${fault_mode}"
}

failures=0
if ! run_cleanup_fault failure 73; then
    failures=1
fi
if ! run_cleanup_fault term 143; then
    failures=1
fi

if [[ ${failures} -ne 0 ]]; then
    exit 1
fi

printf 'PASS: explicit tag fetch emitted no annotated-tag warning\n'
