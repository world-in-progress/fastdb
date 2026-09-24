#!/usr/bin/env bash

set -euo pipefail

readonly REAL_RM="${FASTDB_TEST_REAL_RM:?}"
readonly FAULT_MODE="${FASTDB_TEST_RM_MODE:-none}"
readonly FAULT_STATE="${FASTDB_TEST_RM_STATE:?}"

transaction_root=""
for argument in "$@"; do
    case "${argument}" in
        */fastcarto/lib/.portable-payload-vendor.*)
            transaction_root="${argument}"
            ;;
    esac
done

if [[ "${FAULT_MODE}" == "active-remove-failure" ]]; then
    for argument in "$@"; do
        if [[ "${argument}" == */fastcarto/lib/double-conversion ]]; then
            printf 'injected rollback rm failure for double-conversion\n' >&2
            exit 75
        fi
    done
fi

if [[ -n "${transaction_root}" && \
    ("${FAULT_MODE}" == "committed-failure" || \
        "${FAULT_MODE}" == "committed-term") && \
    ! -e "${FAULT_STATE}" ]]; then
    : > "${FAULT_STATE}"
    "${REAL_RM}" -rf "${transaction_root}/old"
    printf 'injected %s after removing transaction backups\n' \
        "${FAULT_MODE}" >&2

    if [[ "${FAULT_MODE}" == "committed-term" ]]; then
        kill -TERM "${PPID}"
        exit 143
    fi
    exit 73
fi

exec "${REAL_RM}" "$@"
