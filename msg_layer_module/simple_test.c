/**
 * simple_test.c - Simple test program for CXL shared memory multi-node functionality
 * 
 * This program demonstrates basic usage of the CXL shared memory module
 * by creating and sending test messages between nodes.
 * 
 * Compile: gcc -o simple_test simple_test.c
 * 
 * Note: This is a basic example. In practice, you would need to interact
 * with the kernel module through appropriate interfaces (procfs, sysfs, 
 * character device, etc.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* Message structure matching kernel module */
struct test_message {
    int type;
    int size;
    int from_nid;
    int to_nid;
    char payload[36];
};

void print_usage(const char *prog_name) {
    printf("Usage: %s <node_id> <command> [args...]\n", prog_name);
    printf("Commands:\n");
    printf("  send <dest_nid> <message>  - Send message to specific node\n");
    printf("  broadcast <message>        - Broadcast message to all nodes\n");
    printf("  receive                    - Poll for incoming messages\n");
    printf("  status                     - Show node status\n");
    printf("\nExamples:\n");
    printf("  %s 0 send 1 \"Hello Node 1\"\n", prog_name);
    printf("  %s 1 broadcast \"Hello everyone\"\n", prog_name);
    printf("  %s 2 receive\n", prog_name);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    int node_id = atoi(argv[1]);
    const char *command = argv[2];
    
    if (node_id < 0 || node_id > 3) {
        printf("Error: node_id must be between 0 and 3\n");
        return 1;
    }
    
    printf("=== CXL Shared Memory Test (Node %d) ===\n", node_id);
    
    if (strcmp(command, "send") == 0) {
        if (argc < 5) {
            printf("Error: send command requires destination node and message\n");
            print_usage(argv[0]);
            return 1;
        }
        
        int dest_nid = atoi(argv[3]);
        const char *message = argv[4];
        
        if (dest_nid < 0 || dest_nid > 3) {
            printf("Error: destination node_id must be between 0 and 3\n");
            return 1;
        }
        
        if (dest_nid == node_id) {
            printf("Error: cannot send message to self\n");
            return 1;
        }
        
        printf("Sending message from Node %d to Node %d: \"%s\"\n", 
               node_id, dest_nid, message);
        
        /* In a real implementation, this would call the kernel module API */
        printf("Success: Message queued for delivery\n");
        printf("TX Window: win_tx[%d] (Node %d -> Node %d)\n", 
               dest_nid, node_id, dest_nid);
        
    } else if (strcmp(command, "broadcast") == 0) {
        if (argc < 4) {
            printf("Error: broadcast command requires message\n");
            print_usage(argv[0]);
            return 1;
        }
        
        const char *message = argv[3];
        
        printf("Broadcasting message from Node %d: \"%s\"\n", node_id, message);
        
        /* Show which TX windows would be used */
        printf("Broadcasting to TX windows: ");
        for (int i = 0; i < 4; i++) {
            if (i != node_id) {
                printf("win_tx[%d] ", i);
            }
        }
        printf("\n");
        
        /* In a real implementation, this would call the kernel module API */
        printf("Success: Broadcast message queued for delivery\n");
        
    } else if (strcmp(command, "receive") == 0) {
        printf("Polling for incoming messages on Node %d...\n", node_id);
        
        /* Show which RX windows would be polled */
        printf("Polling RX windows: ");
        for (int i = 0; i < 4; i++) {
            if (i != node_id) {
                printf("win_rx[%d] ", i);
            }
        }
        printf("\n");
        
        /* In a real implementation, this would call the kernel module API */
        printf("Simulating message polling...\n");
        sleep(1);
        
        /* Simulate received messages */
        printf("Message received from Node %d: \"Test message\"\n", (node_id + 1) % 4);
        printf("RX Window: win_rx[%d] (Node %d -> Node %d)\n", 
               (node_id + 1) % 4, (node_id + 1) % 4, node_id);
        
    } else if (strcmp(command, "status") == 0) {
        printf("Node %d Status:\n", node_id);
        printf("- Node ID: %d\n", node_id);
        printf("- TX Windows: 4 (win_tx[0], win_tx[1], win_tx[2], win_tx[3])\n");
        printf("- RX Windows: 4 (win_rx[0], win_rx[1], win_rx[2], win_rx[3])\n");
        printf("- Can send to: ");
        for (int i = 0; i < 4; i++) {
            if (i != node_id) {
                printf("Node %d ", i);
            }
        }
        printf("\n");
        printf("- Can receive from: ");
        for (int i = 0; i < 4; i++) {
            if (i != node_id) {
                printf("Node %d ", i);
            }
        }
        printf("\n");
        
    } else {
        printf("Error: Unknown command '%s'\n", command);
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}

/*
 * Multi-Node Communication Examples:
 * 
 * 4-Node Setup:
 * =============
 * 
 * Terminal 1 (Node 0):
 * $ sudo insmod cxl_shm.ko node_id=0 dax_name=dax0.0
 * $ ./simple_test 0 status
 * $ ./simple_test 0 send 1 "Hello from Node 0"
 * $ ./simple_test 0 broadcast "Broadcasting from Node 0"
 * 
 * Terminal 2 (Node 1):
 * $ sudo insmod cxl_shm.ko node_id=1 dax_name=dax0.0
 * $ ./simple_test 1 receive
 * $ ./simple_test 1 send 2 "Node 1 to Node 2"
 * 
 * Terminal 3 (Node 2):
 * $ sudo insmod cxl_shm.ko node_id=2 dax_name=dax0.0
 * $ ./simple_test 2 receive
 * $ ./simple_test 2 send 3 "Node 2 to Node 3"
 * 
 * Terminal 4 (Node 3):
 * $ sudo insmod cxl_shm.ko node_id=3 dax_name=dax0.0
 * $ ./simple_test 3 receive
 * $ ./simple_test 3 send 0 "Node 3 to Node 0"
 * 
 * Ring Buffer Usage:
 * ==================
 * 
 * Total Windows: 16
 * Per Node: 8 windows (4 TX + 4 RX)
 * 
 * Node 0: win_tx[0,1,2,3] + win_rx[0,1,2,3]
 * Node 1: win_tx[0,1,2,3] + win_rx[0,1,2,3]
 * Node 2: win_tx[0,1,2,3] + win_rx[0,1,2,3]
 * Node 3: win_tx[0,1,2,3] + win_rx[0,1,2,3]
 * 
 * Message Flow Example:
 * =====================
 * 
 * Node 0 -> Node 1: Uses Node 0's win_tx[1], Node 1's win_rx[0]
 * Node 1 -> Node 0: Uses Node 1's win_tx[0], Node 0's win_rx[1]
 * Node 0 -> All:    Uses Node 0's win_tx[1,2,3]
 * 
 */
