#include <iostream>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>

struct SharedData {
    std::atomic<uint64_t> counter;
};

void increment_thread(SharedData* shared_data, int thread_id, int iterations) {
    std::cout << "Thread " << thread_id << " started" << std::endl;
    
    for (int i = 0; i < iterations; i++) {
        shared_data->counter.fetch_add(1);
    }
    
    std::cout << "Thread " << thread_id << " finished " << iterations << " increments" << std::endl;
}

int main(int argc, char* argv[]) {
    // Command line argument 검증
    if (argc != 4) {
        std::cout << "Usage: " << argv[0] << " <file_path> <iterations_per_thread> <num_threads>" << std::endl;
        std::cout << "Example: " << argv[0] << " test.dat 100000 2" << std::endl;
        return 1;
    }
    
    const char* filename = argv[1];
    int iterations_per_thread = std::atoi(argv[2]);
    int num_threads = std::atoi(argv[3]);
    
    // 유효성 검사
    if (iterations_per_thread <= 0) {
        std::cout << "Error: iterations_per_thread must be > 0" << std::endl;
        return 1;
    }
    if (num_threads <= 0) {
        std::cout << "Error: num_threads must be > 0" << std::endl;
        return 1;
    }
    
    size_t datasize = sizeof(SharedData);
    
    // 파일 생성 또는 열기
    int fd = open(filename, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // open 한 file 크기 print
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return 1;
    }
    std::cout << "File size: " << st.st_size << " bytes" << std::endl;
    
    // // 파일 크기 설정
    // if (ftruncate(fd, file_size) == -1) {
    //     perror("ftruncate");
    //     close(fd);
    //     return 1;
    // }
    
    // mmap으로 파일 매핑
    SharedData* shared_data = static_cast<SharedData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (shared_data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    // 초기화
    shared_data->counter.store(0);
    
    uint64_t expected_final_value = static_cast<uint64_t>(num_threads) * iterations_per_thread;
    
    std::cout << "Starting simple atomic coherence test..." << std::endl;
    std::cout << "File: " << filename << std::endl;
    std::cout << "Threads: " << num_threads << std::endl;
    std::cout << "Iterations per thread: " << iterations_per_thread << std::endl;
    std::cout << "Expected final value: " << expected_final_value << std::endl;
    
    // 스레드 생성 및 실행
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(increment_thread, shared_data, i, iterations_per_thread);
    }
    
    // 모든 스레드 완료 대기
    for (auto& thread : threads) {
        thread.join();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // 결과 검증
    uint64_t actual_value = shared_data->counter.load();
    
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Runtime: " << duration.count() << " ms" << std::endl;
    std::cout << "Final counter value: " << actual_value << std::endl;
    std::cout << "Expected value: " << expected_final_value << std::endl;
    
    if (actual_value == expected_final_value) {
        std::cout << "✅ SUCCESS: CPU coherence working perfectly!" << std::endl;
    } else {
        std::cout << "❌ FAILURE: Lost " << (expected_final_value - actual_value) << " operations!" << std::endl;
    }
    
    uint64_t total_ops = static_cast<uint64_t>(num_threads) * iterations_per_thread;
    std::cout << "Operations per second: " << (total_ops * 1000.0 / duration.count()) << std::endl;
    
    // 정리
    if (munmap(shared_data, datasize) == -1) {
        perror("munmap");
    }
    close(fd);
    
    return actual_value == expected_final_value ? 0 : 1;
}
