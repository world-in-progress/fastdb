#!/usr/bin/env bash

set -euo pipefail

readonly REAL_GIT="${FASTDB_TEST_REAL_GIT:?}"
readonly REMOTE_ROOT="${FASTDB_TEST_REMOTE_ROOT:?}"

args=("$@")
checkout=""

for ((index = 0; index < ${#args[@]}; index += 1)); do
    case "${args[${index}]}" in
        https://github.com/ibireme/yyjson.git)
            args[${index}]="file://${REMOTE_ROOT}/yyjson.git"
            ;;
        https://github.com/google/double-conversion.git)
            args[${index}]="file://${REMOTE_ROOT}/double-conversion.git"
            ;;
        https://github.com/okdshin/PicoSHA2.git)
            args[${index}]="file://${REMOTE_ROOT}/picosha2.git"
            ;;
        -C)
            if ((index + 1 < ${#args[@]})); then
                checkout="${args[$((index + 1))]}"
            fi
            ;;
    esac
done

name=""
if [[ -n "${checkout}" ]]; then
    name="$(basename "${checkout}")"
fi

expected_commit=""
actual_commit=""
case "${name}" in
    yyjson)
        expected_commit="8b4a38dc994a110abaec8a400615567bd996105f"
        actual_commit="${FASTDB_TEST_YYJSON_COMMIT:?}"
        ;;
    double-conversion)
        expected_commit="9dd6227ee3e29807183e56877f0282aeb40b8b1e"
        actual_commit="${FASTDB_TEST_DOUBLE_CONVERSION_COMMIT:?}"
        ;;
    picosha2)
        expected_commit="161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29"
        actual_commit="${FASTDB_TEST_PICOSHA2_COMMIT:?}"
        ;;
esac

is_rev_parse=0
for argument in "${args[@]}"; do
    if [[ "${argument}" == "rev-parse" ]]; then
        is_rev_parse=1
        break
    fi
done

if [[ ${is_rev_parse} -eq 1 && -n "${actual_commit}" ]]; then
    resolved_commit="$("${REAL_GIT}" "${args[@]}")"
    if [[ "${resolved_commit}" == "${actual_commit}" ]]; then
        printf '%s\n' "${expected_commit}"
    else
        printf '%s\n' "${resolved_commit}"
    fi
    exit 0
fi

if [[ -n "${expected_commit}" ]]; then
    for ((index = 0; index < ${#args[@]}; index += 1)); do
        if [[ "${args[${index}]}" == "${expected_commit}" ]]; then
            args[${index}]="${actual_commit}"
        fi
    done
fi

exec "${REAL_GIT}" "${args[@]}"
