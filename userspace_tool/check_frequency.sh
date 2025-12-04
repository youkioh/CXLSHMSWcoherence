#!/bin/bash

# 1. 시작 대기
echo "=========================================="
read -p "Press [Enter] to START measurement..."
echo "Resetting counters and starting..."

# 2. 카운터 리셋 및 시작 시간 기록
sudo sh -c "echo 1 > /sys/kernel/swmc/page_coherence/reset_counters"
start_time=$(date +%s%N)

# 3. 종료 대기 (측정 중)
echo "Measurement is running..."
read -p "Press [Enter] to STOP and calculate..."

# 4. 종료 시간 기록 (엔터 치자마자 즉시 기록)
end_time=$(date +%s%N)

# 5. 데이터 읽기
fault_count=$(cat /sys/kernel/swmc/page_coherence/fault_count)
total_handling_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_handling_time)

# 6. 경과 시간 계산 (나노초 단위)
elapsed=$((end_time - start_time))

# 7. 결과 계산 및 출력
if [ "$fault_count" -eq 0 ]; then
    echo "No page faults recorded during the measurement period."
else
    # elapsed(ns)를 초 단위로 변환하여 주파수 계산
    frequency=$(echo "scale=6; $fault_count / ($elapsed / 1000000000)" | bc)
    avg_handling_time_ns=$(echo "scale=2; $total_handling_time_ns / $fault_count" | bc)
    
    # 보기 좋게 경과 시간도 출력
    elapsed_sec=$(echo "scale=2; $elapsed / 1000000000" | bc)
    
    echo "------------------------------------------------"
    echo "Total Duration: $elapsed_sec seconds"
    echo "Total Faults: $fault_count"
    echo "Page Fault Frequency: $frequency faults/second"
    echo "Average Page Fault Handling Time: $avg_handling_time_ns ns"
    echo "------------------------------------------------"
fi