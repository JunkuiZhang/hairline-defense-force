#pragma once

#include <cstring>
#include <iostream>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace hdf {

class PerfCounter {
  private:
    int fd_;

  public:
    // type: PERF_TYPE_HARDWARE (硬件缓存等) 或 PERF_TYPE_SOFTWARE (缺页中断等)
    // config: 具体的事件标识，如 PERF_COUNT_HW_CACHE_MISSES
    PerfCounter(uint32_t type, uint64_t config) {
        struct perf_event_attr pe;
        std::memset(&pe, 0, sizeof(struct perf_event_attr));
        pe.type = type;
        pe.size = sizeof(struct perf_event_attr);
        pe.config = config;
        pe.disabled = 1;       // 初始处于禁用状态
        pe.exclude_kernel = 1; // 只统计用户态代码，排除内核态开销
        pe.exclude_hv = 1;     // 排除虚拟机 Hypervisor 开销

        // 调用 Linux 底层 API 打开计数器
        fd_ = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
        if (fd_ == -1) {
            std::cout << "[Warning] perf_event_open failed. Run as root or set "
                         "perf_event_paranoid < 2.\n";
        }
    }

    ~PerfCounter() {
        if (fd_ != -1) {
            close(fd_);
        }
    }

    void start() {
        if (fd_ == -1)
            return;
        ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
    }

    void stop() {
        if (fd_ == -1)
            return;
        ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
    }

    long long read_value() const {
        if (fd_ == -1)
            return 0;
        long long count = 0;
        if (read(fd_, &count, sizeof(long long)) == -1) {
            return 0;
        }
        return count;
    }
};

} // namespace hdf