#pragma once

// Minimal, allocation-free diagnostics shared by the failing Windows CTest
// binaries.  Only scalar state and unbuffered stderr writes are used so that a
// std::terminate, an explicit std::abort, or an abrupt process exit still
// leaves evidence behind.  Nothing in this header changes assertions, budgets,
// failure-injection counters/windows, thread counts, or cleanup behavior.

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace fastdb::test::diag {

inline std::atomic<const char*> active_test{"<none>"};
inline std::atomic<std::size_t> allocation_sequence{0U};
inline std::atomic<std::size_t> last_injected_sequence{0U};
inline std::atomic<std::size_t> last_injected_size{0U};

// Called at the top of every intercepted operator new.  Scalar increment only.
inline void note_allocation() noexcept {
    allocation_sequence.fetch_add(1U, std::memory_order_relaxed);
}

// Called immediately before an injected std::bad_alloc is thrown.
inline void note_injected(std::size_t size) noexcept {
    last_injected_sequence.store(
        allocation_sequence.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    last_injected_size.store(size, std::memory_order_relaxed);
}

// Flush both streams so evidence survives an abrupt exit.
inline void flush() noexcept {
    std::fflush(stdout);
    std::fflush(stderr);
}

inline void set_active_test(const char* name) noexcept {
    active_test.store(name, std::memory_order_relaxed);
}

// One marker per top-level test; never called per iteration.
inline void test_marker(const char* name) noexcept {
    set_active_test(name);
    std::fprintf(stderr, "[fastdb-diag] begin %s\n", name);
    flush();
}

// Label for the pre-existing std::abort() sites in the two backing binaries.
[[noreturn]] inline void abort_marker(const char* where) noexcept {
    std::fprintf(stderr, "[fastdb-diag] abort at %s (test=%s)\n", where,
                 active_test.load(std::memory_order_relaxed));
    flush();
    std::abort();
}

namespace detail {

inline void print_terminate_state() noexcept {
    std::fprintf(
        stderr,
        "[fastdb-diag] terminate test=%s last_injected_index=%zu "
        "last_injected_size=%zu\n",
        active_test.load(std::memory_order_relaxed),
        last_injected_sequence.load(std::memory_order_relaxed),
        last_injected_size.load(std::memory_order_relaxed));
    flush();
}

inline void print_current_exception() noexcept {
    try {
        const std::exception_ptr failure = std::current_exception();
        if (failure == nullptr) {
            std::fputs("[fastdb-diag] terminate: no active exception\n",
                       stderr);
        } else {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[fastdb-diag] terminate what(): %s\n",
                             error.what());
            } catch (...) {
                std::fputs("[fastdb-diag] terminate: non-std exception\n",
                           stderr);
            }
        }
    } catch (...) {
        std::fputs("[fastdb-diag] terminate: exception inspection failed\n",
                   stderr);
    }
    flush();
}

}  // namespace detail

// Diagnostic terminate handler: prints the active test and last injected
// allocation, then aborts exactly like the default handler.
[[noreturn]] inline void terminate_handler() noexcept {
    detail::print_terminate_state();
    detail::print_current_exception();
    std::abort();
}

inline void install_terminate_handler() noexcept {
    std::set_terminate(&terminate_handler);
}

}  // namespace fastdb::test::diag
