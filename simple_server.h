#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include "simple_common.h"

const char *host_ip = "0.0.0.0";
const uint16_t host_port = 1717;
const char *device_name = "rxe_0";
