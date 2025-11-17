// read.cpp (AVX-512, 16 threads, no prefetch/madvise)
#include <immintrin.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <thread>
#include <vector>

#ifndef __AVX512F__
# error "This file requires AVX-512F. Compile with -mavx512f on a CPU that supports it."
#endif

struct ThreadStats {
    std::uint64_t touches = 0;
    std::uint64_t bytes   = 0;
    unsigned long long sum = 0;
};

static inline void scan_range_avx512(const unsigned char* base,
                                     size_t begin, size_t end,
                                     size_t stride_bytes,
                                     ThreadStats& out)
{
    for (size_t off = begin; off < end; off += stride_bytes) {
        size_t chunk = std::min(stride_bytes, end - off);
        const unsigned char* p = base + off;

        size_t i = 0;
        // 64B blocks
        for (; i + 64 <= chunk; i += 64) {
            __m512i v = _mm512_loadu_si512((const void*)(p + i));
            // 하위 128비트 → 하위 64비트만 스칼라 합산 (연산 최소화)
            __m128i lo128 = _mm512_castsi512_si128(v);
            unsigned long long lo64 = (unsigned long long)_mm_cvtsi128_si64(lo128);
            out.sum += lo64;
        }
        // 8B tail
        for (; i + 8 <= chunk; i += 8) {
            unsigned long long v;
            std::memcpy(&v, p + i, 8); // 안전한 비정렬 접근
            out.sum += v;
        }
        // 1B tail
        for (; i < chunk; ++i) {
            out.sum += p[i];
        }

        out.touches += 1;
        out.bytes   += chunk;
    }
}

int main(int argc, char* argv[]) {
    // Usage: prog <file_path> <stride_bytes>
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <file_path> <stride_bytes>\n"
                  << "Example: " << argv[0] << " test.dat 4096\n";
        return 1;
    }

    const char* filename = argv[1];
    size_t stride_bytes  = 0;
    try { stride_bytes = std::stoull(argv[2]); } catch (...) {
        std::cerr << "Invalid stride_bytes\n"; return 1;
    }
    if (stride_bytes == 0) stride_bytes = 1;

    // open & size
    int fd = open(filename, O_RDONLY);
    if (fd == -1) { std::cerr << "open: " << std::strerror(errno) << "\n"; return 1; }

    struct stat st{};
    if (fstat(fd, &st) == -1) { std::cerr << "fstat: " << std::strerror(errno) << "\n"; close(fd); return 1; }
    if (st.st_size <= 0) { std::cerr << "File size is 0.\n"; close(fd); return 1; }
    const size_t len = static_cast<size_t>(st.st_size);
    std::cout << "File size: " << len << " bytes\n";

    // mmap
    void* map = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { std::cerr << "mmap: " << std::strerror(errno) << "\n"; close(fd); return 1; }
    const unsigned char* base = static_cast<const unsigned char*>(map);

    // 16 threads 고정
    const int THREADS = 16;
    std::cout << "Starting scan (stride=" << stride_bytes << " B, threads=" << THREADS
              << ", AVX-512)\n";

    auto t0 = std::chrono::steady_clock::now();

    // 영역 분할
    size_t chunk_size = (len + THREADS - 1) / THREADS;
    std::vector<std::thread> ths;
    std::vector<ThreadStats> stats(THREADS);

    ths.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        size_t begin = t * chunk_size;
        if (begin >= len) { stats.resize(t); break; }
        size_t end = std::min(len, begin + chunk_size);

        ths.emplace_back(scan_range_avx512, base, begin, end, stride_bytes,
                            std::ref(stats[t]));
    }

    for (auto& th : ths) th.join();

    // 집계 및 출력
    std::uint64_t touches = 0, bytes = 0;
    unsigned long long sum = 0;
    for (const auto& s : stats) {
        touches += s.touches;
        bytes   += s.bytes;
        sum     += s.sum;   // 의도적으로 overflow 허용(벤치 목적)
    }

    auto now = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(now - t0).count();
    double gib = bytes / (1024.0 * 1024.0 * 1024.0);
    double gibps = gib / (sec > 0 ? sec : 1.0);

    std::cout << "[+" << (int)sec << "s] touches=" << touches
                << ", read=" << bytes << " B"
                << "  (~" << gibps * 1024.0 << " MiB/s)"
                << ", sum=" << sum << "\n";
    // 다음 패스 반복

    // not reached
    munmap(map, len);
    close(fd);
    return 0;
}
