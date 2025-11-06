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
#include <cstring>

volatile bool running = true;

void signal_handler(int sig) {
    if (sig == SIGINT) {
        running = false;
        std::cout << "\nReceived SIGINT, stopping...\n" << std::endl;
    }
}

struct CounterData {
    uint64_t counter;
    char padding[4096 - sizeof(uint64_t)]; // 페이지 크기로 패딩
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

void write_thread(CounterData* data_array, size_t array_size, SyncData* sync_data, int process_id, int thread_id, int stride) {
    std::cout << "Process " << process_id << ", Thread " << thread_id << " started with stride " << stride << std::endl;
    
    size_t index = 0;
    uint64_t write_count = 0;
    
    while (running) {
        // stride로 전체 배열을 순회하면서 write
        for (size_t i = 0; i < array_size && running; i += stride) {
            data_array[i].counter = write_count++;
        }
        
        if (write_count % 1000000 == 0) {
            std::cout << "Process " << process_id << ", Thread " << thread_id << " wrote " << write_count << " times" << std::endl;
        }
    }
    
    std::cout << "Process " << process_id << ", Thread " << thread_id << " finished with " << write_count << " writes" << std::endl;
}

void worker_process(const char* filename, int process_id, int threads_per_process, size_t array_size, int stride) {
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
    
    size_t datasize = sizeof(CounterData) * array_size;
    CounterData* data_array = static_cast<CounterData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (data_array == MAP_FAILED) {
        perror("mmap in worker process");
        close(fd);
        munmap(sync_data, sizeof(SyncData));
        exit(1);
    }
    
    std::cout << "Process " << process_id << " successfully mapped shared memory of size " << datasize << " bytes" << std::endl;
    
    // 프로세스 준비 완료 신호
    sync_data->ready_processes.fetch_add(1);
    
    // 모든 프로세스가 준비될 때까지 대기
    while (!sync_data->start_flag.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    
    // 스레드 생성 및 실행
    std::vector<std::thread> threads;
    for (int i = 0; i < threads_per_process; i++) {
        threads.emplace_back(write_thread, data_array, array_size, sync_data, process_id, i, stride);
    }
    
    // 모든 스레드 완료 대기
    for (auto& thread : threads) {
        thread.join();
    }
    
    std::cout << "Process " << process_id << " completed all threads" << std::endl;
    
    // 정리
    if (munmap(data_array, datasize) == -1) {
        perror("munmap in worker process");
    }
    if (munmap(sync_data, sizeof(SyncData)) == -1) {
        perror("munmap sync_data in worker process");
    }
    close(fd);
}

int main(int argc, char* argv[]) {
    // Signal handler 설정
    signal(SIGINT, signal_handler);
    
    // Command line argument 검증
    if (argc != 6) {
        std::cout << "Usage: " << argv[0] << " <file_path> <array_size> <stride> <threads_per_process> <num_processes>" << std::endl;
        std::cout << "Example: " << argv[0] << " test.dat 1000 10 2 4" << std::endl;
        return 1;
    }
    
    const char* filename = argv[1];
    size_t array_size = std::atoi(argv[2]);
    int stride = std::atoi(argv[3]);
    int threads_per_process = std::atoi(argv[4]);
    int num_processes = std::atoi(argv[5]);
    
    // 유효성 검사
    if (array_size <= 0) {
        std::cout << "Error: array_size must be > 0" << std::endl;
        return 1;
    }
    if (stride <= 0) {
        std::cout << "Error: stride must be > 0" << std::endl;
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
    
    size_t datasize = sizeof(CounterData) * array_size;
    
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

    // 파일 크기 설정 (배열 크기로)
    if (ftruncate(fd, datasize) == -1) {
        perror("ftruncate");
        close(fd);
        cleanup_sync_file(sync_data);
        return 1;
    }

    // open 한 file 크기 print
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        cleanup_sync_file(sync_data);
        return 1;
    }
    std::cout << "File size: " << st.st_size << " bytes" << std::endl;
    
    // mmap으로 파일 매핑 (부모 프로세스에서 초기화용)
    CounterData* data_array = static_cast<CounterData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (data_array == MAP_FAILED) {
        perror("mmap");
        close(fd);
        cleanup_sync_file(sync_data);
        return 1;
    }
    
    // 배열 초기화
    for (size_t i = 0; i < array_size; i++) {
        data_array[i].counter = 0;
        memset(data_array[i].padding, 0, sizeof(data_array[i].padding));
    }
    
    std::cout << "Starting multi-process write test (Press Ctrl+C to stop)..." << std::endl;
    std::cout << "File: " << filename << std::endl;
    std::cout << "Array size: " << array_size << std::endl;
    std::cout << "Stride: " << stride << std::endl;
    std::cout << "Processes: " << num_processes << std::endl;
    std::cout << "Threads per process: " << threads_per_process << std::endl;
    std::cout << "Total threads: " << (num_processes * threads_per_process) << std::endl;
    std::cout << "Total memory size: " << datasize << " bytes" << std::endl;
    
    // 프로세스 생성 및 실행
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 부모 프로세스의 mmap을 먼저 해제 (자식 프로세스들이 독립적으로 mmap하도록)
    if (munmap(data_array, datasize) == -1) {
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
            worker_process(filename, i, threads_per_process, array_size, stride);
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
    
    data_array = static_cast<CounterData*>(
        mmap(nullptr, datasize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    if (data_array == MAP_FAILED) {
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
    
    // 통계를 위한 주기적 모니터링
    std::thread monitor_thread([&]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (running) {
                uint64_t total_writes = 0;
                for (size_t i = 0; i < array_size; i += stride) {
                    total_writes += data_array[i].counter;
                }
                std::cout << "Total writes so far: " << total_writes << std::endl;
            }
        }
    });
    
    // 모든 자식 프로세스 완료 대기
    int status;
    for (pid_t pid : child_pids) {
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 0) {
            std::cout << "Process " << pid << " exited with error code " << WEXITSTATUS(status) << std::endl;
        }
    }
    
    running = false;
    monitor_thread.join();
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // 최종 결과 출력
    std::cout << "\n=== Final Results ===" << std::endl;
    std::cout << "Runtime: " << duration.count() << " ms" << std::endl;
    
    uint64_t total_writes = 0;
    for (size_t i = 0; i < array_size; i += stride) {
        total_writes += data_array[i].counter;
    }
    std::cout << "Total writes completed: " << total_writes << std::endl;
    
    // 정리
    if (munmap(data_array, datasize) == -1) {
        perror("munmap");
    }
    close(fd);
    cleanup_sync_file(sync_data);  // syncfile 정리 및 파일 삭제
    
    return 0;
}
