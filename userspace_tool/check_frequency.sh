#!/bin/bash

# 로그 파일 설정
LOG_DIR="./measurement_logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/measurement_$(date +%Y%m%d_%H%M%S).log"
CSV_FILE="$LOG_DIR/measurement_$(date +%Y%m%d_%H%M%S).csv"

# CSV 헤더 작성
echo "Timestamp,Elapsed_Sec,Fault_Count,Frequency,Avg_Coherence_Transaction_Time_ns,Avg_Page_Replication_Time_ns,Avg_Metadata_Update_Time_ns,Avg_Page_Fault_Handling_Time_ns" > "$CSV_FILE"

# 1. 시작 대기
echo "=========================================="
read -p "Press [Enter] to START measurement..."
echo "Resetting counters and starting..."

# 2. 카운터 리셋 및 시작 시간 기록
sudo sh -c "echo 1 > /sys/kernel/swmc/page_coherence/reset_counters"
start_time=$(date +%s%N)
start_timestamp=$(date '+%Y-%m-%d %H:%M:%S')

echo "Start time: $start_timestamp" | tee -a "$LOG_FILE"
echo "Measurement is running (collecting data every second)..."

# 3. 매초마다 측정 (Ctrl+C로 종료할 때까지)
trap 'stop_measurement=1' INT

stop_measurement=0
while [ $stop_measurement -eq 0 ]; do
    current_time=$(date +%s%N)
    elapsed=$((current_time - start_time))
    
    # 카운터 읽기
    fault_count=$(cat /sys/kernel/swmc/page_coherence/fault_count)
    total_coherence_transaction_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_coherence_transaction_time)
    total_page_replication_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_page_replication_time)
    total_metadata_update_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_metadata_update_time)
    total_page_fault_handling_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_page_fault_handling_time)
    
    elapsed_sec=$(echo "scale=2; $elapsed / 1000000000" | bc)
    
    if [ "$fault_count" -gt 0 ]; then
        frequency=$(echo "scale=6; $fault_count / ($elapsed / 1000000000)" | bc)
        avg_coherence_transaction_time_ns=$(echo "scale=2; $total_coherence_transaction_time_ns / $fault_count" | bc)
        avg_page_replication_time_ns=$(echo "scale=2; $total_page_replication_time_ns / $fault_count" | bc)
        avg_meta_update_time_ns=$(echo "scale=2; $total_metadata_update_time_ns / $fault_count" | bc)
        avg_page_fault_handling_time_ns=$(echo "scale=2; $total_page_fault_handling_time_ns / $fault_count" | bc)
    else
        frequency=0
        avg_coherence_transaction_time_ns=0
        avg_page_replication_time_ns=0
        avg_meta_update_time_ns=0
        avg_page_fault_handling_time_ns=0
    fi
    
    # CSV에 append
    echo "$(date '+%Y-%m-%d %H:%M:%S'),$elapsed_sec,$fault_count,$frequency,$avg_coherence_transaction_time_ns,$avg_page_replication_time_ns,$avg_meta_update_time_ns,$avg_page_fault_handling_time_ns" >> "$CSV_FILE"
    
    # 다음 측정을 위해 카운터 리셋
    sudo sh -c "echo 1 > /sys/kernel/swmc/page_coherence/reset_counters"
    
    sleep 1
done

# 4. 최종 시간 기록
end_time=$(date +%s%N)
end_timestamp=$(date '+%Y-%m-%d %H:%M:%S')

# 5. 최종 데이터 읽기
fault_count=$(cat /sys/kernel/swmc/page_coherence/fault_count)
total_coherence_transaction_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_coherence_transaction_time)
total_page_replication_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_page_replication_time)
total_metadata_update_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_metadata_update_time)
total_page_fault_handling_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_page_fault_handling_time)

# 6. 경과 시간 계산 (나노초 단위)
elapsed=$((end_time - start_time))

# 7. 결과 계산 및 출력
print_result() {
    local output_method=$1  # "echo" 또는 "log"
    
    if [ "$fault_count" -eq 0 ]; then
        echo "No page faults recorded during the measurement period." | tee -a "$LOG_FILE"
    else
        # elapsed(ns)를 초 단위로 변환하여 주파수 계산
        frequency=$(echo "scale=6; $fault_count / ($elapsed / 1000000000)" | bc)
        
        # 보기 좋게 경과 시간도 출력
        elapsed_sec=$(echo "scale=2; $elapsed / 1000000000" | bc)
        
        echo "------------------------------------------------" | tee -a "$LOG_FILE"
        echo "End time: $end_timestamp" | tee -a "$LOG_FILE"
        echo "Total Duration: $elapsed_sec seconds" | tee -a "$LOG_FILE"
        echo "Total Faults: $fault_count" | tee -a "$LOG_FILE"
        echo "Page Fault Frequency: $frequency faults/second" | tee -a "$LOG_FILE"
        echo "Last Coherence Transaction Time (Total): $total_coherence_transaction_time_ns ns" | tee -a "$LOG_FILE"
        echo "Last Page Replication Time (Total): $total_page_replication_time_ns ns" | tee -a "$LOG_FILE"
        echo "Last Metadata Update Time (Total): $total_metadata_update_time_ns ns" | tee -a "$LOG_FILE"
        echo "Last Page Fault Handling Time (Total): $total_page_fault_handling_time_ns ns" | tee -a "$LOG_FILE"
        echo "------------------------------------------------" | tee -a "$LOG_FILE"
    fi
}

print_result

echo "" | tee -a "$LOG_FILE"
echo "Log files saved:" | tee -a "$LOG_FILE"
echo "  - Text log: $LOG_FILE" | tee -a "$LOG_FILE"
echo "  - CSV log: $CSV_FILE" | tee -a "$LOG_FILE"