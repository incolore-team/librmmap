#include "simple_common.h"

int wait_rdmacm(struct rdma_event_channel *echannel, 
                enum rdma_cm_event_type expected_event,
                struct rdma_cm_event **cm_event) {
    int ret = 1;
    ret = rdma_get_cm_event(echannel, cm_event);
    if (ret) {
        log_error("failed to retrieve a cm event, errno: %d", -errno);
        return -errno;
    }
    if((*cm_event)->status != 0){
        log_error("cm event has non zero status: %d", (*cm_event)->status);
        ret = -((*cm_event)->status);
        rdma_ack_cm_event(*cm_event);
        return ret;
    }  
    if ((*cm_event)->event != expected_event) {
        log_error("unexpected event received: %s [ expecting: %s ]", 
                   rdma_event_str((*cm_event)->event),
                   rdma_event_str(expected_event));
        rdma_ack_cm_event(*cm_event);
        return -1;
    }
    log_info("a new %s type event is received", rdma_event_str((*cm_event)->event));
    return ret;
}

int wait_wc(struct ibv_comp_channel *comp_channel, 
            struct ibv_wc *wc,
            int max_wc) {
	struct ibv_cq *cq_ptr = NULL;
	void *context = NULL;
	int ret = -1, i, total_wc = 0;
	ret = ibv_get_cq_event(comp_channel,
		                   &cq_ptr,
		                   &context);
    if (ret) {
	    log_error("Failed to get next CQ event due to %d", -errno);
	    return -errno;
    }
    ret = ibv_req_notify_cq(cq_ptr, 0);
    if (ret){
	    log_error("Failed to request further notifications %d", -errno);
	    return -errno;
    }
    total_wc = 0;
    do {
	    ret = ibv_poll_cq(cq_ptr, 
	                      max_wc - total_wc,
	                      wc + total_wc);
	    if (ret < 0) {
	        log_error("Failed to poll cq for wc due to %d", ret);
	        return ret;
	    }
	    total_wc += ret;
    } while (total_wc < max_wc); 
    log_info("%d WC are completed", total_wc);
    for( i = 0 ; i < total_wc ; i++) {
	    if (wc[i].status != IBV_WC_SUCCESS) {
	        log_error("Work completion (WC) has error status: %s at index %d", 
	 		            ibv_wc_status_str(wc[i].status), i);
            switch (wc[i].status) {
                case IBV_WC_LOC_QP_OP_ERR:
                    break;
                default:
                    break;
            }
	        return -(wc[i].status);
	    }
    }
    ibv_ack_cq_events(cq_ptr, 1);
    return total_wc; 
}
