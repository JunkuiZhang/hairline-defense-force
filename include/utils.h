#pragma once

#include <iostream>
#include <pthread.h>

namespace hdf {

static inline int pin_to_core(int core_id) {
    auto pid = pthread_self();
    std::cout << "Pinning thread " << pid << " to core " << core_id << ".\n";
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pid, sizeof(cpuset), &cpuset);
}

static inline uint64_t rdtsc_lfence() {
    uint64_t lo, hi;
    asm volatile("lfence\n\t"
                 "rdtsc"
                 : "=a"(lo), "=d"(hi));
    return (hi << 32) | lo;
}

static inline uint64_t rdtscp_lfence() {
    uint64_t lo, hi;
    uint32_t aux;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    asm volatile("lfence" ::: "memory");
    return (hi << 32) | lo;
}

static inline double calibrate_tsc_ghz() {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t tsc0 = rdtsc_lfence();

    struct timespec req = {0, 100000000}; // 100ms
    nanosleep(&req, nullptr);

    uint64_t tsc1 = rdtscp_lfence();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ns =
        (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    return static_cast<double>(tsc1 - tsc0) / elapsed_ns;
}

} // namespace hdf