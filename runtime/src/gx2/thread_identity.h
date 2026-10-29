#pragma once
// A cheap identity of the calling thread, unique among live threads, without a thread_local (gx2's
// display-list recording slots look the recording thread up by it on every emit).
#include <cstdint>
#include <pthread.h>

namespace gx2 {
inline uintptr_t thread_identity() {
#if defined(__aarch64__) && defined(__linux__)  // Linux: TPIDR_EL0 is the thread pointer
    return (uintptr_t)__builtin_thread_pointer();
#else
    // macOS: pthread_self() reads TPIDRRO_EL0 / %gs; __builtin_thread_pointer (TPIDR_EL0) is not per
    // thread there (threads share values)
    return (uintptr_t)pthread_self();
#endif
}
}  // namespace gx2
