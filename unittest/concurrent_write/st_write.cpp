#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <atomic>

struct CounterData {
    std::atomic<uint64_t> counter;
};

void read_counter(CounterData* counter_data, int iterations) {
    uint64_t value;
    std::cout << "Reading counter " << iterations << " times..." << std::endl;
    
    for (int i = 0; i < iterations; i++) {
        std::cout << "Press Enter to read iteration " << i << "..." << std::endl;
        std::cin.get();
        value = counter_data->counter.fetch_add(1);
        std::cout << "Read iteration " << i << ", counter value: " << value << std::endl;
    }
    
    std::cout << "Finished " << iterations << " reads" << std::endl;
}

int main(int argc, char* argv[]) {
    // Command line argument 검증
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <file_path> <iterations>" << std::endl;
        std::cout << "Example: " << argv[0] << " test.dat 100000" << std::endl;
        return 1;
    }
    
    const char* filename = argv[1];
    int iterations = std::atoi(argv[2]);
    
    // 유효성 검사
    if (iterations <= 0) {
        std::cout << "Error: iterations must be > 0" << std::endl;
        return 1;
    }
    
    size_t datasize = sizeof(CounterData);
    
    // 파일 열기
    int fd = open(filename, O_RDWR);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // 파일 크기 확인
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return 1;
    }
    std::cout << "File size: " << st.st_size << " bytes" << std::endl;
    
    // 파일 크기가 CounterData 구조체 크기보다 작으면 에러
    if (st.st_size < static_cast<off_t>(datasize)) {
        std::cout << "Error: File too small. Expected at least " << datasize << " bytes" << std::endl;
        close(fd);
        return 1;
    }
    
    // mmap으로 파일 매핑
    CounterData* counter_data = static_cast<CounterData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (counter_data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    counter_data->counter.store(0); // 초기화
    
    std::cout << "Starting single-thread read test..." << std::endl;
    std::cout << "File: " << filename << std::endl;
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Initial counter value: " << counter_data->counter << std::endl;
    
    // 실행 시간 측정 시작
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 카운터 읽기 실행
    read_counter(counter_data, iterations);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // 결과 출력
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Runtime: " << duration.count() << " ms" << std::endl;
    std::cout << "Final counter value: " << counter_data->counter << std::endl;
    std::cout << "Total read operations: " << iterations << std::endl;
    
    if (duration.count() > 0) {
        std::cout << "Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
    }
    
    // 정리
    if (munmap(counter_data, datasize) == -1) {
        perror("munmap");
    }
    close(fd);
    
    std::cout << "✅ Test completed successfully!" << std::endl;
    return 0;
}
