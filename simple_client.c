#include "simple_client.h"

static struct sockaddr_in server_sockaddr;
static struct meta_t meta;
static char *data;
static uint8_t *recv_buffer = NULL;

static struct rdma_event_channel *cm_event_channel = NULL;
static struct ibv_comp_channel *comp_channel = NULL;
static struct ibv_pd *pd = NULL;
static struct ibv_cq *cq = NULL;
static struct ibv_mr *meta_mr = NULL, *data_mr = NULL;
static struct ibv_sge server_recv_sge, client_send_sge;
static struct ibv_recv_wr server_recv_wr, *err_server_recv_wr = NULL;
static struct ibv_send_wr client_send_wr, *err_client_send_wr = NULL;

static struct rdma_cm_id *client_cmid = NULL;

static int setup_resources() {
    struct rdma_cm_event *event = NULL;
    int ret;
   
    // create event channel
    cm_event_channel = rdma_create_event_channel();
    
    if (cm_event_channel == NULL) {
        log_error("creating cm event channel failed, errno: %d", -errno);
        return -errno;
    }

    log_info("cm event channel created");

    // create client cmid
    ret = rdma_create_id(cm_event_channel, &client_cmid, 
        NULL, RDMA_PS_TCP);

    if (ret != 0) {
        log_error("creating cm id failed with errno: %d", -errno); 
        return -errno;
    }

    // resolve ip addr to ib addr
    ret = rdma_resolve_addr(client_cmid, NULL, (struct sockaddr *) &server_sockaddr, 2000);

    if (ret != 0) {
        log_error("Failed to resolve address, errno: %d", -errno);
        return -errno;
    }

    ret  = wait_rdmacm(cm_event_channel, RDMA_CM_EVENT_ADDR_RESOLVED, &event);

    if (ret != 0) {
        log_error("failed to receive a valid event, ret = %d", ret);
        return ret;
    }

    ret = rdma_ack_cm_event(event);

    if (ret != 0) {
        log_error("failed to acknowledge the cm event, errno: %d", -errno);
        return -errno;
    }

    log_info("rdma address is resolved");

    // resolve rdma route
    ret = rdma_resolve_route(client_cmid, 2000);
    
    if (ret != 0) {
        log_error("failed to resolve route, errno: %d", -errno);
        return ret;
    }

    ret = wait_rdmacm(cm_event_channel, RDMA_CM_EVENT_ROUTE_RESOLVED, &event);
    
    if (ret != 0) {
        log_error("failed to receive a valid event, ret = %d", ret);
        return ret;
    }

    ret = rdma_ack_cm_event(event);

    if (ret != 0) {
        log_error("failed to acknowledge the cm event, errno: %d", -errno);
        return -errno;
    }

    log_info("rdma route is resolved");

    // create pd
    pd = ibv_alloc_pd(client_cmid->verbs);

    if (pd == NULL) {
        log_error("failed to alloc pd, errno: %d", -errno);
        return -errno;
    }

    log_info("pd created");

    // create completion channel
    comp_channel = ibv_create_comp_channel(client_cmid->verbs);

    if (comp_channel == NULL) {
        log_error("failed to create io completion event channel, errno: %d", -errno);
        return -errno;
    }

    log_info("completion channel created");

    // create cq
    cq = ibv_create_cq(client_cmid->verbs, 16, NULL, comp_channel, 0);

    if (cq == NULL) {
        log_error("failed to create cq, errno: %d", -errno);
        return -errno;
    }

    log_info("cq created");

    // receive all types of notification
    ret = ibv_req_notify_cq(cq, 0);

    if (ret != 0) {
        log_error("failed to request notifications, errno: %d", -errno);
        return -errno;
    }
 
    // setup qp init helper struct
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));

    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.cap.max_send_wr = 1;
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    // create qp
    ret = rdma_create_qp(client_cmid, pd, &qp_init_attr);

    if (ret != 0) {
        log_error("failed to create qp due to errno: %d", -errno);
        return -errno;
    }

    log_info("qp created: qpn=0x%x", client_cmid->qp->qp_num);
    return 0;
}

int pre_post_meta_buf() {
    // prepare and register mr for server metadata
    meta_mr = ibv_reg_mr(pd, &meta, sizeof(meta), IBV_ACCESS_LOCAL_WRITE);
    if (meta_mr == NULL) {
        log_error("failed to create mr on buffer, errno: %d", -errno);
        return -errno;
    }
    log_info("mr for server metadata created");

    server_recv_sge.addr = (uint64_t) meta_mr->addr;
    server_recv_sge.length = (uint32_t) meta_mr->length;
    server_recv_sge.lkey = (uint32_t) meta_mr->lkey;

    memset(&server_recv_wr, 0, sizeof(server_recv_wr));
    server_recv_wr.sg_list = &server_recv_sge;
    server_recv_wr.num_sge = 1;

    int ret = ibv_post_recv(client_cmid->qp, &server_recv_wr, &err_server_recv_wr);

    if (ret != 0) {
        log_error("failed to pre-post the receive buffer, errno: %d", ret);
        return ret;
    }

    log_info("metadata recv buffer pre-posted");
    return 0;
}

int connect_to_server() {
    struct rdma_conn_param conn_param;
    struct rdma_cm_event *event = NULL;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;
    conn_param.retry_count = 3;

    int ret = rdma_connect(client_cmid, &conn_param);

    if (ret != 0) {
        log_error("failed to connect to remote host , errno: %d", -errno);
        return -errno;
    }

    ret = wait_rdmacm(cm_event_channel, RDMA_CM_EVENT_ESTABLISHED, &event);

    if (ret != 0) {
        log_error("failed to get cm event, ret = %d", ret);
        return ret;
    }

    ret = rdma_ack_cm_event(event);

    if (ret != 0) {
        log_error("failed to acknowledge cm event, errno: %d", -errno);
        return -errno;
    }

    log_info("connected successfully");
    return 0;
}

int read_meta() {
    struct ibv_wc wc;
    int ret;
    ret = wait_wc(comp_channel, &wc, 1);
    
    if (ret != 1) {
        log_error("failed to wait for work completion");
        return ret;
    }

    log_info("meta message length: %d", meta.length);
    return 0;
}

int read_data() {
    int ret;

    // prepare data & mr
    int mr_flags = IBV_ACCESS_LOCAL_WRITE |
                   IBV_ACCESS_REMOTE_READ |
                   IBV_ACCESS_REMOTE_WRITE;
    data = (char *) malloc(meta.length);
    data_mr = ibv_reg_mr(pd, data, meta.length, mr_flags);

    if (data_mr == NULL) {
        log_error("failed to create data mr, errno: %d", -errno);
        return -errno;
    }

    client_send_sge.addr = (uint64_t) data_mr->addr;
    client_send_sge.length = meta.length;
    client_send_sge.lkey = data_mr->lkey;

    memset(&client_send_wr, 0, sizeof(client_send_wr));
    client_send_wr.sg_list = &client_send_sge;
    client_send_wr.num_sge = 1;
    client_send_wr.opcode = IBV_WR_RDMA_READ;
    client_send_wr.send_flags = IBV_SEND_SIGNALED;

    client_send_wr.wr.rdma.rkey = meta.key;
    client_send_wr.wr.rdma.remote_addr = meta.address;

    ret = ibv_post_send(client_cmid->qp, &client_send_wr, &err_client_send_wr);

    if (ret != 0) {
        log_error("failed to post send wr, errno: %d", -errno);
        return -errno;
    }

    struct ibv_wc wc;
    ret = wait_wc(comp_channel, &wc, 1);

    printf("data '%s'\n", data);
    
    log_info("data received");
    
    return 0;
}

int main() {
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_addr.s_addr = inet_addr(host_ip);
    server_sockaddr.sin_port = htons(host_port);

    int ret = setup_resources();
    
    if (ret != 0) {
        log_error("failed to setup resources");
        exit(-1);
    }

    ret = pre_post_meta_buf();

    if (ret != 0) {
        log_error("failed to pre-post metadata recv buffer");
        exit(-1);
    }

    ret = connect_to_server();

    if (ret != 0) {
        log_error("failed to connect to server");
        exit(-1);
    }

    ret = read_meta();

    if (ret != 0) {
        log_error("failed to fetch meta");
        exit(-1);
    }

    ret = read_data();

    return 0;
}
