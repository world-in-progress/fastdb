#!/usr/bin/env bash

set -euo pipefail

readonly REAL_RM="${FASTDB_TEST_REAL_RM:?}"
readonly FAULT_MODE="${FASTDB_TEST_RM_MODE:?}"
readonly FAULT_STATE="${FASTDB_TEST_RM_STATE:?}"

transaction_root=""
for argument in "$@"; do
    case "${argument}" in
        */fastcarto/lib/.portable-payload-vendor.*)
            transaction_root="${argument}"
            ;;
    esac
done

if [[ -n "${transaction_root}" && ! -e "${FAULT_STATE}" ]]; then
    : > "${FAULT_STATE}"
    "${REAL_RM}" -rf "${transaction_root}/old"
    printf 'injected %s after removing transaction backups\n' \
        "${FAULT_MODE}" >&2

    if [[ "${FAULT_MODE}" == "term" ]]; then
        kill -TERM "${PPID}"
        exit 143
    fi
    exit 73
fi

exec "${REAL_RM}" "$@"
