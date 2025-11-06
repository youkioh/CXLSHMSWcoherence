#ifndef SWMC_KMSG_H
#define SWMC_KMSG_H

#ifdef CONFIG_PAGE_COHERENCE

#include <linux/types.h>

#define CL_SIZE 64                    /* Cache line size */
#define SWMC_KMSG_PAYLOAD_SIZE 28        

/* Window offset will be defined by the message layer module based on actual window size */
/* Each module should define SWMC_KMSG_WINDOW_OFFSET based on sizeof(cxl_kmsg_window) */

/* Error codes for messaging */
#define SWMC_KMSG_ERR_NOT_READY -ENODEV   /* Messaging subsystem not ready */
#define SWMC_KMSG_ERR_NO_IMPL   -ENOSYS   /* No implementation registered */


/* message types */
enum swmc_kmsg_type {
    SWMC_KMSG_TYPE_FETCH = 0,
    SWMC_KMSG_TYPE_FETCH_ACK,
    SWMC_KMSG_TYPE_FETCH_NACK,
    SWMC_KMSG_TYPE_INVALIDATE,
    SWMC_KMSG_TYPE_INVALIDATE_ACK,
    SWMC_KMSG_TYPE_INVALIDATE_NACK,
    SWMC_KMSG_TYPE_ERROR,
    SWMC_KMSG_TYPE_MAX
};

/* CXL message header = 32B */
struct swmc_kmsg_hdr {
    enum swmc_kmsg_type type; 
    int ws_id; // wait station ID of the sender
    int from_nid; // will be set to current node ID by message layer module
    int to_nid;
} __attribute__((packed));

struct payload_data {
    unsigned long cxl_hdm_offset;
    int page_order; // 0 for PAGE_SIZE, PMD_ORDER for PMD_SIZE
    long acked_fault_count; // number of ACKed faults at the sender when this message was sent
} __attribute__((packed));

/* CXL message structure */
struct swmc_kmsg_message {
    struct swmc_kmsg_hdr header;
    struct payload_data payload;
} __attribute__((packed, aligned(CL_SIZE)));


/* SETUP */

/* Function pointer to callback functions*/
typedef int (*swmc_kmsg_cbftn)(struct swmc_kmsg_message *);

/* Register a callback function to handle the message type*/
int swmc_kmsg_register_callback(enum swmc_kmsg_type type, swmc_kmsg_cbftn callback);

/* Unregister a callback function for the message type */
int swmc_kmsg_unregister_callback(enum swmc_kmsg_type type);



/* MESSAGING INTERFACE */

int swmc_kmsg_unicast(enum swmc_kmsg_type type, int ws_id, int dest_nid, struct payload_data *payload);

int swmc_kmsg_broadcast(enum swmc_kmsg_type type, int ws_id, struct payload_data *payload);

// int swmc_kmsg_multicast(enum swmc_kmsg_type type, int ws_id, int *dest_nids, int num_dests, struct payload_data *payload); // TODO: Implement later

void swmc_kmsg_done(struct swmc_kmsg_message *message);

int swmc_kmsg_node_count(void); // Get the number of nodes in the system

// will use unicast, done. maybe broadcast later
int swmc_kmsg_process_message(struct swmc_kmsg_message *message);


/* FUNCTION POINTER TYPES THAT WILL BE REGISTERED BY MESSAGE LAYER MODULE */
struct swmc_kmsg_ops {
    char *name; // Name of the messaging implementation

    int (*node_count)(void); // Function to get the number of nodes
    int (*unicast)(enum swmc_kmsg_type type, int ws_id, int dest_nid, struct payload_data *payload);
    int (*broadcast)(enum swmc_kmsg_type type, int ws_id, struct payload_data *payload);
    // int (*multicast)(enum swmc_kmsg_type type, int ws_id, int *dest_nids, int num_dests, struct payload_data *payload);
    void (*done)(struct swmc_kmsg_message *message);

};

int swmc_kmsg_register_ops(struct swmc_kmsg_ops *ops);
void swmc_kmsg_unregister_ops(void);

#endif /* CONFIG_PAGE_COHERENCE */

#endif /* SWMC_KMSG_H */
