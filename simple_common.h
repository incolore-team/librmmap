#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include <stdio.h>

#define log_error(msg, args...) do {\
    fprintf(stderr, "[error] %s:%d : " msg "\n", __FILE__, __LINE__, ## args);\
} while(0);

#define log_info(msg, args...) do {\
    fprintf(stdout, "[info] %s:%d : " msg "\n", __FILE__, __LINE__, ## args);\
} while(0);

struct __attribute((packed)) meta_t {
    // address and length of index table
    uint64_t address;  // virtual address from mr
    uint32_t length;  // length of data
    uint32_t key; // local key of the creator
};

extern int wait_rdmacm(struct rdma_event_channel *echannel, 
                        enum rdma_cm_event_type expected_event,
                        struct rdma_cm_event **cm_event);

extern int wait_wc(struct ibv_comp_channel *comp_channel, 
                   struct ibv_wc *wc,
                   int max_wc);
