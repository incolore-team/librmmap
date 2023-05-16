#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>

#include "simple_server.h"

static struct meta_t meta;

static struct ibv_pd *pd = NULL;
static struct ibv_mr *data_mr, *meta_mr = NULL;
static struct rdma_event_channel *cm_event_channel = NULL;
static struct ibv_context *device_context = NULL;
static struct ibv_comp_channel *comp_channel = NULL;
static struct ibv_sge server_send_sge;
static struct ibv_send_wr server_send_wr, *err_server_send_wr = NULL;

static const char *data = "hello world!";

static int on_connect_request(struct rdma_cm_event *cm_event);
static int on_established(struct rdma_cm_event *cm_event);
static int on_disconnected(struct rdma_cm_event *cm_event);

static int setup_resources() {
    // get device list
    struct ibv_device **device_list;
    int device_num;
    device_list = ibv_get_device_list(&device_num);

    if (device_num == 0) {
        log_error("no ib device found");
        return -1;
    }

    // search for wanted device
    struct ibv_device *device = NULL;
    for (int i = 0; i < device_num; i++) {
        struct ibv_device *dev = device_list[i];
	    if (strcmp(ibv_get_device_name(dev), device_name) == 0) {
	        device = dev;
	        break;
	    }
    }

    if (device == NULL) {
        // wanted device not found
        log_error("no device named %s found", device_name);
	    return -1;
    }

    // get device context
    device_context = ibv_open_device(device);

    log_info("device %s opened", device_name);

    if (device_context == NULL) {
        log_error("cannot open device");
        return -1;
    }

    // create pd
    pd = ibv_alloc_pd(device_context);
    
    if (pd == NULL) {
        log_error("Failed to allocate a protection domain errno: %d", -errno);
        return -errno;
    }

    log_info("pd created");

    int mr_flags = IBV_ACCESS_REMOTE_READ;

    // create & register mr
    data_mr = ibv_reg_mr(pd, (void *) data, strlen(data) + 1, mr_flags);

    if (data_mr == NULL) {
        log_error("failed to create server data mr");
        return -errno;
    }

    log_info("data mr registered: addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x",
        data, data_mr->lkey, data_mr->rkey, mr_flags);

    // create server meta
    meta.address = (uint64_t) data_mr->addr;
    meta.length = data_mr->length;
    meta.key = data_mr->lkey;

    // register meta_mr
    mr_flags = IBV_ACCESS_LOCAL_WRITE;

    meta_mr = ibv_reg_mr(pd, &meta, sizeof(meta), mr_flags);

    if (meta_mr == NULL) {
        log_error("failed to create server meta mr");
        return -errno;
    }

    log_info("server meta mr registered: addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x",
           &meta, meta_mr->lkey, meta_mr->rkey, mr_flags);

    // create completion channel
    comp_channel = ibv_create_comp_channel(device_context);

    if (comp_channel == NULL) {
        log_error("Failed to create an I/O completion event channel, %d", -errno);
        return -errno;
    }

    log_info("completion channel created");

    // setup static meta message
    server_send_sge.addr = (uint64_t) &meta;
    server_send_sge.length = sizeof(meta);
    server_send_sge.lkey = meta_mr->lkey;

    memset(&server_send_wr, 0, sizeof(server_send_wr));
    server_send_wr.sg_list = &server_send_sge;
    server_send_wr.num_sge = 1;
    server_send_wr.opcode = IBV_WR_SEND;
    // server_send_wr.send_flags = IBV_SEND_SIGNALED;

    return 0;
}

static int start_server() {
    // prepare server-side resources
    // setup server
    struct sockaddr_in server_sockaddr;
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_addr.s_addr = inet_addr(host_ip);
    server_sockaddr.sin_port = htons(host_port);

    int ret;

    cm_event_channel = rdma_create_event_channel();

    if (cm_event_channel == NULL) {
        log_error("creating cm event channel failed with errno : (%d)", -errno);
        return -errno;
    }

    log_info("cm event channel created");

    struct rdma_cm_id *server_cmid = NULL;

    ret = rdma_create_id(cm_event_channel, &server_cmid, device_context, RDMA_PS_TCP);

    if (ret != 0) {
        log_error("creating server cm id failed with errno: %d", -errno);
        return -errno;
    }

    log_info("server cmid created");

    ret = rdma_bind_addr(server_cmid, (struct sockaddr *) &server_sockaddr);

    if (ret != 0) {
        log_error("failed to bind server address, errno: %d", -errno);
        return -errno;
    }

    log_info("server address binded");

    // listen for client
    rdma_listen(server_cmid, 1);

    if (ret != 0) {
        log_error("rdma_listen failed to listen on server address, errno: %d ", -errno);
        return -errno;
    }

    log_info("server is listening at: %s , port: %d",
        inet_ntoa(server_sockaddr.sin_addr),
        ntohs(server_sockaddr.sin_port));

    return 0;
}

static void run_event_loop() {
    log_info("event loop started");
    while (1) {
        struct rdma_cm_event *cm_event = NULL;
        rdma_get_cm_event(cm_event_channel, &cm_event);

        int ret = 0;
        const char *event_name;

        switch (cm_event->event) {
            case RDMA_CM_EVENT_CONNECT_REQUEST:
                event_name = "RDMA_CM_EVENT_CONNECT_REQUEST";
                ret = on_connect_request(cm_event);
                break;
            case RDMA_CM_EVENT_ESTABLISHED:
                event_name = "RDMA_CM_EVENT_ESTABLISHED";
                ret = on_established(cm_event);
                break;
            case RDMA_CM_EVENT_DISCONNECTED:
                event_name = "RDMA_CM_EVENT_DISCONNECTED";
                ret = on_disconnected(cm_event);
                break;
            default:
	            log_info("event %d (unhandled) occurred", cm_event->event);
                break;
        }

        if (ret != 0) {
            log_error("event %s callback returned non-zero value, ret: %d", event_name, ret);
        }

        ret = rdma_ack_cm_event(cm_event);

        if (ret != 0) {
            log_error("failed to acknowledge the cm event %d, errno: %d",
                        cm_event->event, -errno);
        }
    }
}

static int on_connect_request(struct rdma_cm_event *cm_event) {
    struct rdma_cm_id *cm_client_id = cm_event->id;

    // use opened context for shared resources
    cm_client_id->verbs = device_context;
    
    log_info("the client rdma connection request is acknowledged");

    // TODO readonly, seems saving is not required
    // setup client resources
    // create cq
    struct ibv_cq *cq = ibv_create_cq(device_context, 16, NULL, comp_channel, 0);

    if (cq == NULL) {
        log_error("failed to create cq, errno: %d", -errno);
        return -errno;
    }

    // setup qp init helper struct
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));

    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.cap.max_send_wr = 4;
    qp_init_attr.cap.max_recv_wr = 4;
    qp_init_attr.cap.max_send_sge = 4;
    qp_init_attr.cap.max_recv_sge = 4;

    // create qp
    int ret = rdma_create_qp(cm_client_id, pd, &qp_init_attr);

    if (ret != 0) {
        log_error("failed to create qp due to errno: %d", -errno);
        return -errno;
    }

    log_info("qp created: qpn=0x%x", cm_client_id->qp->qp_num);

    // accept the connection
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;

    ret = rdma_accept(cm_client_id, &conn_param);

    if (ret != 0) {
        log_error("failed to accept the connection, errno: %d", -errno);
        return -errno;
    }

    return 0;
}

static int on_established(struct rdma_cm_event *cm_event) {
    // send server meta
    struct rdma_cm_id *cm_client_id = cm_event->id;

    int ret = ibv_post_send(cm_client_id->qp, &server_send_wr, &err_server_send_wr);
    
    if (ret != 0) {
        log_error("posting of server meta data failed, errno: %d", -errno);
        return -errno;
    }

    log_info("server meta posted");

    return 0;
}

static int on_disconnected(struct rdma_cm_event *cm_event) {
    // destroy resources
    return 0;
}

int main() {
    int ret;
    ret = setup_resources();

    if (ret != 0) {
        log_error("fail to setup resources");
        exit(-1);
    }

    ret = start_server();

    if (ret != 0) {
        log_error("fail to start server");
        exit(-1);
    }
    
    run_event_loop();

    return 0;
}
