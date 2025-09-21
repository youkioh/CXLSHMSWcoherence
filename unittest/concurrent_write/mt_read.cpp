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
#include <sys/wait.h>
#include <signal.h>

struct CounterData {
    std::atomic<uint64_t> counter;
};

struct SyncData {
    std::atomic<int> ready_processes;  // 프로세스 동기화용
    std::atomic<bool> start_flag;      // 시작 신호
};

// syncfile을 생성하고 mmap하는 함수
SyncData* create_sync_file() {
    const char* sync_filename = "syncfile";
    
    // 파일 생성
    int sync_fd = open(sync_filename, O_CREAT | O_RDWR, 0666);
    if (sync_fd == -1) {
        perror("open syncfile");
        return nullptr;
    }
    
    // 파일 크기 설정
    if (ftruncate(sync_fd, sizeof(SyncData)) == -1) {
        perror("ftruncate syncfile");
        close(sync_fd);
        return nullptr;
    }
    
    // mmap
    SyncData* sync_data = static_cast<SyncData*>(
        mmap(nullptr, sizeof(SyncData), PROT_READ | PROT_WRITE, MAP_SHARED, sync_fd, 0)
    );
    
    if (sync_data == MAP_FAILED) {
        perror("mmap syncfile");
        close(sync_fd);
        return nullptr;
    }
    
    close(sync_fd);  // mmap 후에는 fd를 닫아도 됨
    
    // 초기화
    sync_data->ready_processes.store(0);
    sync_data->start_flag.store(false);
    
    return sync_data;
}

// syncfile을 mmap하는 함수 (자식 프로세스용)
SyncData* map_sync_file() {
    const char* sync_filename = "syncfile";
    
    int sync_fd = open(sync_filename, O_RDWR);
    if (sync_fd == -1) {
        perror("open syncfile in worker");
        return nullptr;
    }
    
    SyncData* sync_data = static_cast<SyncData*>(
        mmap(nullptr, sizeof(SyncData), PROT_READ | PROT_WRITE, MAP_SHARED, sync_fd, 0)
    );
    
    if (sync_data == MAP_FAILED) {
        perror("mmap syncfile in worker");
        close(sync_fd);
        return nullptr;
    }
    
    close(sync_fd);
    return sync_data;
}

// syncfile 정리 함수
void cleanup_sync_file(SyncData* sync_data) {
    if (sync_data) {
        munmap(sync_data, sizeof(SyncData));
    }
    unlink("syncfile");  // 파일 삭제
}

void increment_thread(CounterData* counter_data, SyncData* sync_data, int process_id, int thread_id, int iterations) {
    std::cout << "Process " << process_id << ", Thread " << thread_id << " started" << std::endl;
    
    for (int i = 0; i < iterations; i++) {
        counter_data->counter.load();
    }
    
    std::cout << "Process " << process_id << ", Thread " << thread_id << " finished " << iterations << " increments" << std::endl;
}

void worker_process(const char* filename, int process_id, int threads_per_process, int iterations_per_thread) {
    std::cout << "Process " << process_id << " starting with " << threads_per_process << " threads" << std::endl;
    
    // syncfile mmap
    SyncData* sync_data = map_sync_file();
    if (sync_data == nullptr) {
        std::cerr << "Process " << process_id << " failed to map sync file" << std::endl;
        exit(1);
    }
    
    // 각 프로세스가 독립적으로 파일을 열고 mmap (counter만)
    int fd = open(filename, O_RDWR);
    if (fd == -1) {
        perror("open in worker process");
        munmap(sync_data, sizeof(SyncData));
        exit(1);
    }
    
    size_t datasize = sizeof(CounterData);
    CounterData* counter_data = static_cast<CounterData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (counter_data == MAP_FAILED) {
        perror("mmap in worker process");
        close(fd);
        munmap(sync_data, sizeof(SyncData));
        exit(1);
    }
    
    std::cout << "Process " << process_id << " successfully mapped shared memory" << std::endl;
    
    // 프로세스 준비 완료 신호
    sync_data->ready_processes.fetch_add(1);
    
    // 모든 프로세스가 준비될 때까지 대기
    while (!sync_data->start_flag.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    
    // 스레드 생성 및 실행
    std::vector<std::thread> threads;
    for (int i = 0; i < threads_per_process; i++) {
        threads.emplace_back(increment_thread, counter_data, sync_data, process_id, i, iterations_per_thread);
    }
    
    // 모든 스레드 완료 대기
    for (auto& thread : threads) {
        thread.join();
    }
    
    std::cout << "Process " << process_id << " completed all threads" << std::endl;
    
    // 정리
    if (munmap(counter_data, datasize) == -1) {
        perror("munmap in worker process");
    }
    if (munmap(sync_data, sizeof(SyncData)) == -1) {
        perror("munmap sync_data in worker process");
    }
    close(fd);
}

int main(int argc, char* argv[]) {
    // Command line argument 검증
    if (argc != 5) {
        std::cout << "Usage: " << argv[0] << " <file_path> <iterations_per_thread> <threads_per_process> <num_processes>" << std::endl;
        std::cout << "Example: " << argv[0] << " test.dat 100000 2 4" << std::endl;
        return 1;
    }
    
    const char* filename = argv[1];
    int iterations_per_thread = std::atoi(argv[2]);
    int threads_per_process = std::atoi(argv[3]);
    int num_processes = std::atoi(argv[4]);
    
    // 유효성 검사
    if (iterations_per_thread <= 0) {
        std::cout << "Error: iterations_per_thread must be > 0" << std::endl;
        return 1;
    }
    if (threads_per_process <= 0) {
        std::cout << "Error: threads_per_process must be > 0" << std::endl;
        return 1;
    }
    if (num_processes <= 0) {
        std::cout << "Error: num_processes must be > 0" << std::endl;
        return 1;
    }
    
    size_t datasize = sizeof(CounterData);
    
    // SyncData를 파일 기반 mmap으로 생성
    SyncData* sync_data = create_sync_file();
    if (sync_data == nullptr) {
        std::cerr << "Failed to create sync file" << std::endl;
        return 1;
    }
    
    // 파일 생성 또는 열기
    int fd = open(filename, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("open");
        cleanup_sync_file(sync_data);
        return 1;
    }

    // // 파일 크기 설정 (SharedData 크기로)
    // if (ftruncate(fd, datasize) == -1) {
    //     perror("ftruncate");
    //     close(fd);
    //     return 1;
    // }

    // open 한 file 크기 print
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        cleanup_sync_file(sync_data);
        return 1;
    }
    std::cout << "File size: " << st.st_size << " bytes" << std::endl;
    
    // mmap으로 파일 매핑 (부모 프로세스에서 초기화용) - counter만
    CounterData* counter_data = static_cast<CounterData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (counter_data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        cleanup_sync_file(sync_data);
        return 1;
    }
    
    // 초기화 (sync_data는 이미 create_sync_file에서 초기화됨)
    // counter_data->counter.store(0);
    
    uint64_t expected_final_value = static_cast<uint64_t>(num_processes) * threads_per_process * iterations_per_thread;
    
    std::cout << "Starting multi-process atomic coherence test..." << std::endl;
    std::cout << "File: " << filename << std::endl;
    std::cout << "Processes: " << num_processes << std::endl;
    std::cout << "Threads per process: " << threads_per_process << std::endl;
    std::cout << "Iterations per thread: " << iterations_per_thread << std::endl;
    std::cout << "Total threads: " << (num_processes * threads_per_process) << std::endl;
    std::cout << "Expected final value: " << expected_final_value << std::endl;
    
    // 프로세스 생성 및 실행
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 부모 프로세스의 mmap을 먼저 해제 (자식 프로세스들이 독립적으로 mmap하도록)
    if (munmap(counter_data, datasize) == -1) {
        perror("munmap in parent");
        close(fd);
        cleanup_sync_file(sync_data);
        return 1;
    }
    close(fd);
    
    std::vector<pid_t> child_pids;
    for (int i = 0; i < num_processes; i++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // 자식 프로세스
            worker_process(filename, i, threads_per_process, iterations_per_thread);
            exit(0);
        } else if (pid > 0) {
            // 부모 프로세스
            child_pids.push_back(pid);
        } else {
            perror("fork");
            cleanup_sync_file(sync_data);
            return 1;
        }
    }
    
    // 부모 프로세스에서 모니터링을 위해 다시 mmap
    fd = open(filename, O_RDWR);
    if (fd == -1) {
        perror("reopen for monitoring");
        cleanup_sync_file(sync_data);
        return 1;
    }
    
    counter_data = static_cast<CounterData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (counter_data == MAP_FAILED) {
        perror("remmap for monitoring");
        close(fd);
        cleanup_sync_file(sync_data);
        return 1;
    }
    
    // 모든 프로세스가 준비될 때까지 대기
    std::cout << "Waiting for all processes to be ready..." << std::endl;
    while (sync_data->ready_processes.load() < num_processes) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "All processes ready! Starting test..." << std::endl;
    sync_data->start_flag.store(true);
    
    // 모든 자식 프로세스 완료 대기
    int status;
    for (pid_t pid : child_pids) {
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 0) {
            std::cout << "Process " << pid << " exited with error code " << WEXITSTATUS(status) << std::endl;
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // 결과 검증
    uint64_t actual_value = counter_data->counter.load();
    
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Runtime: " << duration.count() << " ms" << std::endl;
    std::cout << "Final counter value: " << actual_value << std::endl;
    std::cout << "Expected value: " << expected_final_value << std::endl;
    
    if (actual_value == expected_final_value) {
        std::cout << "✅ SUCCESS: Multi-process atomic coherence working perfectly!" << std::endl;
    } else {
        std::cout << "❌ FAILURE: Lost " << (expected_final_value - actual_value) << " operations!" << std::endl;
    }
    
    uint64_t total_ops = static_cast<uint64_t>(num_processes) * threads_per_process * iterations_per_thread;
    std::cout << "Operations per second: " << (total_ops * 1000.0 / duration.count()) << std::endl;
    
    // 정리
    if (munmap(counter_data, datasize) == -1) {
        perror("munmap");
    }
    close(fd);
    cleanup_sync_file(sync_data);  // syncfile 정리 및 파일 삭제
    
    return actual_value == expected_final_value ? 0 : 1;
}
