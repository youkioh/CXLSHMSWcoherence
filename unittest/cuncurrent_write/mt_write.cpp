#include <iostream>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>

#define DIM 128
#define NUM_READERS 3
#define NUM_WRITERS 2

struct SharedData {
    float vector[DIM];
    std::atomic<bool> test_running;
};

void writer_thread(SharedData* shared_data, int writer_id) {
    int write_count = 0;
    
    std::cout << "Writer thread " << writer_id << " started" << std::endl;
    
    while (shared_data->test_running.load()) {
        // 각 writer는 고유한 패턴으로 write
        // Writer 0: [10000, 10001, 10002, ...]
        // Writer 1: [20000, 20001, 20002, ...]
        float write_value = static_cast<float>((writer_id + 1) * 10000 + write_count);
        
        // 벡터 전체를 동일한 값으로 채우기
        for (int i = 0; i < DIM; i++) {
            shared_data->vector[i] = write_value;
        }
        
        write_count++;
        
        // 1ms 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "Writer " << writer_id << " finished: " << write_count << " writes" << std::endl;
}

void reader_thread(SharedData* shared_data, int reader_id) {
    float previous_value = -1;
    int read_count = 0;
    int large_jumps = 0;
    int coherence_errors = 0;
    int pattern_changes = 0;
    
    std::cout << "Reader thread " << reader_id << " started" << std::endl;
    
    while (shared_data->test_running.load()) {
        // 벡터 읽기 (첫 번째 원소만 확인)
        float current_value = shared_data->vector[0];
        
        if (previous_value >= 0 && current_value != previous_value) {
            // 어느 writer가 쓴 값인지 판단
            int current_writer = static_cast<int>(current_value) / 10000 - 1;
            int previous_writer = static_cast<int>(previous_value) / 10000 - 1;
            
            if (current_writer == previous_writer) {
                // 같은 writer의 연속된 값
                float expected_diff = 1.0f;
                float actual_diff = current_value - previous_value;
                
                if (std::abs(actual_diff - expected_diff) > 0.001f) {
                    coherence_errors++;
                    std::cout << "Reader " << reader_id << ": Coherence error from writer " 
                              << current_writer << ": " << previous_value 
                              << " -> " << current_value << " (diff=" << actual_diff << ")" << std::endl;
                }
            } else {
                // Writer가 바뀜 - 정상적인 패턴 변화
                pattern_changes++;
                // std::cout << "Reader " << reader_id << ": Pattern change from writer " 
                //           << previous_writer << " to " << current_writer << std::endl;
            }
        }
        
        previous_value = current_value;
        read_count++;
        
        // 100us 대기 (writer보다 빠르게 읽기)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    std::cout << "Reader " << reader_id << " finished: " << read_count << " reads, " 
              << coherence_errors << " coherence errors, " 
              << pattern_changes << " pattern changes" << std::endl;
}

int main() {
    const char* filename = "simple_coherence_test.dat";
    size_t file_size = sizeof(SharedData);
    
    // 파일 생성 또는 열기
    int fd = open(filename, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("open");
        return 1;
    }
    
    // 파일 크기 설정
    if (ftruncate(fd, file_size) == -1) {
        perror("ftruncate");
        close(fd);
        return 1;
    }
    
    // mmap으로 파일 매핑
    SharedData* shared_data = static_cast<SharedData*>(
        mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (shared_data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    // 초기화
    std::cout << "Initializing shared data..." << std::endl;
    for (int i = 0; i < DIM; i++) {
        shared_data->vector[i] = 0.0f;
    }
    shared_data->test_running.store(true);
    
    std::cout << "File size: " << file_size << " bytes" << std::endl;
    std::cout << "Vector dimension: " << DIM << std::endl;
    std::cout << "Starting simple coherence test..." << std::endl;
    
    // 스레드 생성
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> writers;
    std::vector<std::thread> readers;
    
    // 여러 writer 스레드 생성
    for (int i = 0; i < NUM_WRITERS; i++) {
        writers.emplace_back(writer_thread, shared_data, i);
    }
    
    // 여러 reader 스레드 생성
    for (int i = 0; i < NUM_READERS; i++) {
        readers.emplace_back(reader_thread, shared_data, i);
    }
    
    // 10초간 테스트 실행
    std::this_thread::sleep_for(std::chrono::seconds(10));
    
    // 테스트 종료
    shared_data->test_running.store(false);
    
    // 모든 스레드 종료 대기
    for (auto& writer : writers) {
        writer.join();
    }
    for (auto& reader : readers) {
        reader.join();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Total runtime: " << duration.count() << " ms" << std::endl;
    std::cout << "Simple coherence test completed!" << std::endl;
    
    // 정리
    if (munmap(shared_data, file_size) == -1) {
        perror("munmap");
    }
    
    close(fd);
    
    return 0;
}
