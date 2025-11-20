#!/bin/bash
MESUREMENT_TIME=60  # in seconds


# start time
start_time=$(date +%s%N)

sudo sh -c "echo 1 > /sys/kernel/swmc/page_coherence/reset_counters"

# wait for some time to accumulate faults
sleep $MESUREMENT_TIME

fault_count=$(cat /sys/kernel/swmc/page_coherence/fault_count)
total_handling_time_ns=$(cat /sys/kernel/swmc/page_coherence/total_handling_time)

# end time
end_time=$(date +%s%N)
elapsed=$((end_time - start_time))

# page fault frequency calculation
if [ $fault_count -eq 0 ]; then
    echo "No page faults recorded during the measurement period."
else
    frequency=$(echo "scale=6; $fault_count / ($elapsed / 1000000000)" | bc)
    avg_handling_time_ns=$(echo "scale=2; $total_handling_time_ns / $fault_count" | bc)
    echo "Page Fault Frequency: $frequency faults/second"
    echo "Average Page Fault Handling Time: $avg_handling_time_ns ns"
fi