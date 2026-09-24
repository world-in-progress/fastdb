#!/usr/bin/env bash

set -euo pipefail

readonly REAL_MV="${FASTDB_TEST_REAL_MV:?}"
readonly FAULT_MODE="${FASTDB_TEST_MV_MODE:-none}"
readonly FAULT_STATE="${FASTDB_TEST_MV_STATE:?}"

source_path="${1:-}"

case "${FAULT_MODE}" in
    install-failure|rollback-term|restore-failure|active-remove-failure)
        if [[ "${source_path}" == */.portable-payload-vendor.*/new/picosha2 && \
            ! -e "${FAULT_STATE}.install-failed" ]]; then
            : > "${FAULT_STATE}.install-failed"
            printf 'injected install failure for picosha2\n' >&2
            exit 71
        fi
        ;;
esac

if [[ "${FAULT_MODE}" == "rollback-term" && \
    "${source_path}" == */.portable-payload-vendor.*/old/double-conversion && \
    ! -e "${FAULT_STATE}.rollback-term-sent" ]]; then
    : > "${FAULT_STATE}.rollback-term-sent"
    printf 'injected TERM during rollback restore\n' >&2
    kill -TERM "${PPID}"
    sleep 0.1
fi

if [[ "${FAULT_MODE}" == "restore-failure" && \
    "${source_path}" == */.portable-payload-vendor.*/old/double-conversion ]]; then
    printf 'injected restore mv failure for double-conversion\n' >&2
    exit 74
fi

case "${FAULT_MODE}" in
    signal-int|signal-term|signal-hup)
        if [[ "${source_path}" == */.portable-payload-vendor.*/new/double-conversion && \
            ! -e "${FAULT_STATE}.signal-sent" ]]; then
            : > "${FAULT_STATE}.signal-sent"
            signal_name="${FAULT_MODE#signal-}"
            signal_name="$(printf '%s' "${signal_name}" | tr '[:lower:]' '[:upper:]')"
            printf 'injected %s during active replacement\n' \
                "${signal_name}" >&2
            kill -"${signal_name}" "${PPID}"
            sleep 0.1
        fi
        ;;
esac

exec "${REAL_MV}" "$@"
