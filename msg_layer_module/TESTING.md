# CXL Shared Memory Multi-Node Testing Guide

## Overview

This document provides comprehensive testing procedures for the refactored CXL shared memory module that supports up to 4 nodes with 16 ring buffers.

## Architecture Summary

### Ring Buffer Layout
- **Total Windows**: 16 (4 TX + 4 RX per node)
- **Node Configuration**: Each node has dedicated TX/RX windows
- **Memory Mapping**: All nodes map the same shared memory region but use different windows

### Window Mapping Logic
For node N (0-3):
- **TX Windows**: `win_tx[dest_nid]` - sends to destination node
- **RX Windows**: `win_rx[src_nid]` - receives from source node

## Testing Procedures

### 1. Single Node Testing

Test basic functionality with one node:

```bash
# Load module on node 0
sudo insmod cxl_shm.ko node_id=0 dax_name=dax0.0

# Check initialization
dmesg | tail -20
# Should show: "CXL node 0 initialized with 4 TX and 4 RX windows"

# Unload
sudo rmmod cxl_shm
```

### 2. Two Node Testing

Test communication between two nodes:

**Node 0:**
```bash
sudo insmod cxl_shm.ko node_id=0 dax_name=dax0.0
```

**Node 1:**
```bash
sudo insmod cxl_shm.ko node_id=1 dax_name=dax0.0
```

### 3. Four Node Testing

Test full 4-node communication:

**Node 0:**
```bash
sudo insmod cxl_shm.ko node_id=0 dax_name=dax0.0
```

**Node 1:**
```bash
sudo insmod cxl_shm.ko node_id=1 dax_name=dax0.0
```

**Node 2:**
```bash
sudo insmod cxl_shm.ko node_id=2 dax_name=dax0.0
```

**Node 3:**
```bash
sudo insmod cxl_shm.ko node_id=3 dax_name=dax0.0
```

## Key Functions for Testing

### 1. Sending Messages
```c
// Send to specific node
int cxl_kmsg_send_message(int dest_nid, struct cxl_kmsg_message *msg, size_t size);

// Broadcast to all other nodes
int cxl_kmsg_broadcast_message(struct cxl_kmsg_message *msg, size_t size);
```

### 2. Receiving Messages
```c
// Poll all RX windows for incoming messages
int cxl_kmsg_poll_all_rx(struct cxl_kmsg_message **msg, int *from_nid);
```

### 3. Message Allocation
```c
// Get message buffer
struct cxl_kmsg_message *cxl_kmsg_get(size_t size);

// Release message buffer
void cxl_kmsg_put(struct cxl_kmsg_message *msg);
```

## Expected Behavior

### Module Loading
Each node should log:
```
shm_cxl: Physical DAX range: 0x[address]
shm_cxl: CXL node [N] initialized with 4 TX and 4 RX windows
shm_cxl: Started receive handler thread
```

### Message Flow
- Node N sending to Node M should use `win_tx[M]`
- Node M should receive from Node N via `win_rx[N]`
- Broadcast messages should appear on all other nodes' RX windows

### Error Conditions
- Invalid node_id (not 0-3) should fail initialization
- Missing DAX device should fail with appropriate error
- Window mapping failures should be logged and handled gracefully

## Debugging

### Check Module Status
```bash
# Module information
modinfo cxl_shm.ko

# Loaded modules
lsmod | grep cxl_shm

# Kernel messages
dmesg | grep shm_cxl
```

### Memory Layout Verification
The module logs window mappings during initialization. Verify:
- All 8 windows per node are mapped successfully
- Physical addresses are within the DAX range
- No overlapping window addresses

### Performance Monitoring
- Monitor cache coherency behavior
- Check message latency between nodes
- Verify ring buffer utilization

## Common Issues

### 1. DAX Device Not Found
**Error**: `Failed to find DAX device`
**Solution**: Ensure DAX device exists and is accessible

### 2. Window Mapping Failure
**Error**: `Failed to map window`
**Solution**: Check memory permissions and DAX device configuration

### 3. Invalid Node ID
**Error**: `Invalid node_id`
**Solution**: Use node_id between 0-3

### 4. Thread Creation Failure
**Error**: `Failed to create receive thread`
**Solution**: Check system resources and thread limits

## Test Scenarios

### Scenario 1: Point-to-Point Communication
- Node 0 sends to Node 1
- Node 1 sends to Node 0
- Verify bidirectional communication

### Scenario 2: Multi-Node Broadcast
- Node 0 broadcasts to all nodes
- Verify all nodes (1, 2, 3) receive the message

### Scenario 3: Concurrent Communication
- Multiple nodes sending simultaneously
- Verify message ordering and delivery

### Scenario 4: High-Frequency Messaging
- Rapid message exchange between nodes
- Monitor for buffer overflow or loss

### Scenario 5: Node Addition/Removal
- Start with 2 nodes, add more dynamically
- Remove nodes and verify remaining functionality

## Success Criteria

1. ✅ Module compiles without errors
2. ✅ All nodes initialize successfully
3. ✅ Point-to-point messaging works
4. ✅ Broadcast messaging works
5. ✅ All 16 windows are properly utilized
6. ✅ No memory leaks during load/unload cycles
7. ✅ Proper error handling and recovery
8. ✅ Cache coherency maintained

## Next Steps

After basic testing:
1. Performance benchmarking
2. Stress testing with high message rates
3. Integration with higher-level applications
4. Power management testing
5. Fault tolerance validation
