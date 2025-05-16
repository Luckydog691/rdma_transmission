/*
* BUILD COMMAND:
* gcc -Wall -O2 -o RDMA_RC_example RDMA_RC_example.c -libverbs
*/
/******************************************************************************
*
* RDMA Aware Networks Programming Example
*
* This code demonstrates how to perform the following operations using 
* the * VPI Verbs API:
* Send
* Receive
* RDMA Read
* RDMA Write
*
*****************************************************************************/
#include <stdio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/eventfd.h>

/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_TIMEOUT 100000
#define MSG "SEND operation "
#define RDMAMSGR "RDMA read operation "
#define RDMAMSGW "RDMA write operation"
#define MSG_SIZE 1024 * 128
#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x)
{
    return bswap_64(x);
}
static inline uint64_t ntohll(uint64_t x)
{
    return bswap_64(x);
}
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x)
{
    return x;
}
static inline uint64_t ntohll(uint64_t x)
{
    return x;
}
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

/* structure of test parameters */
struct config_t
{
    const char *dev_name; /* IB device name */
    char *server_name;    /* server host name */
    uint32_t tcp_port;    /* server TCP port */
    int ib_port;          /* local IB port to work with */
    int gid_idx;          /* gid index to use */
};

/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t
{
    uint64_t addr;        /* Buffer address */
    uint32_t rkey;        /* Remote key */
    uint32_t qp_num;      /* QP number */
    uint16_t lid;         /* LID of the IB port */
    uint8_t gid[16];      /* gid */
} __attribute__((packed));

/* structure of system resources */
struct resources
{
    struct ibv_device_attr device_attr; /* Device attributes */
    struct ibv_port_attr port_attr;     /* IB port attributes */
    struct cm_con_data_t remote_props;  /* values to connect to remote side */
    struct ibv_context *ib_ctx;         /* device handle */
    struct ibv_pd *pd;                  /* PD handle */
    struct ibv_cq *cq;                  /* CQ handle */
    struct ibv_qp *qp;                  /* QP handle */
    struct ibv_mr *mr;                  /* MR handle for buf */
    struct ibv_comp_channel *comp_channel; //完成事件通道
    char *buf;                          /* memory buffer pointer, used for RDMA and send ops */
};

int qp_init_sock = -1, qp_sync_sock = -1, qp_finish_sock = -1;

struct config_t config =
{
    NULL,  /* dev_name */
    NULL,  /* server_name */
    19875, /* tcp_port */
    1,     /* ib_port */
    -1     /* gid_idx */
};

/******************************************************************************
Socket operations:
For simplicity, the example program uses TCP sockets to exchange control
information. If a TCP/IP stack/connection is not available, connection manager
(CM) may be used to pass this information. Use of CM is beyond the scope of
this example
******************************************************************************/
/******************************************************************************
* Function: sock_connect
* Input:
* servername: URL of server to connect to (NULL for server mode)
* port: port of service
*
* Output:none
*
* Returns: socket (fd) on success, negative error code on failure
*
* Description:
* Connect a socket. If servername is specified a client connection will be
* initiated to the indicated server and port. Otherwise listen on the
* indicated port for an incoming connection.
*
******************************************************************************/
static int sock_connect(const char *servername, int port)
{
    struct addrinfo *resolved_addr = NULL;
    struct addrinfo *iterator;
    char service[6];
    int sockfd = -1;
    int listenfd = 0;
    int tmp;
    struct addrinfo hints =
    {
        .ai_flags    = AI_PASSIVE,
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM
    };

    if(sprintf(service, "%d", port) < 0)
    {
        goto sock_connect_exit;
    }

    /* Resolve DNS address, use sockfd as temp storage */
    sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
    if(sockfd < 0)
    {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(sockfd), servername, port);
        goto sock_connect_exit;
    }

    /* Search through results and find the one we want */
    for(iterator = resolved_addr; iterator ; iterator = iterator->ai_next)
    {
        sockfd = socket(iterator->ai_family, iterator->ai_socktype, iterator->ai_protocol);
        if(sockfd >= 0)
        {
            if(servername)
			{
                /* Client mode. Initiate connection to remote */
                if((tmp=connect(sockfd, iterator->ai_addr, iterator->ai_addrlen)))
                {
                    fprintf(stdout, "failed connect \n");
                    close(sockfd);
                    sockfd = -1;
                }
			}
            else
            {
                /* Server mode. Set up listening socket an accept a connection */
                listenfd = sockfd;
                sockfd = -1;
                if(bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
                {
                    goto sock_connect_exit;
                }
                listen(listenfd, 1);
                sockfd = accept(listenfd, NULL, 0);
            }
        }
    }

sock_connect_exit:
    if(listenfd)
    {
        close(listenfd);
    }

    if(resolved_addr)
    {
        freeaddrinfo(resolved_addr);
    }

    if(sockfd < 0)
    {
        if(servername)
        {
            fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        }
        else
        {
            perror("server accept");
            fprintf(stderr, "accept() failed\n");
        }
    }

    return sockfd;
}

/******************************************************************************
* Function: sock_sync_data
* Input:
* sock: socket to transfer data on
* xfer_size: size of data to transfer
* local_data: pointer to data to be sent to remote
*
* Output: remote_data pointer to buffer to receive remote data
*
* Returns: 0 on success, negative error code on failure
*
* Description:
* Sync data across a socket. The indicated local data will be sent to the
* remote. It will then wait for the remote to send its data back. It is
* assumed that the two sides are in sync and call this function in the proper
* order. Chaos will ensue if they are not. :)
*
* Also note this is a blocking function and will wait for the full data to be
* received from the remote.
*
******************************************************************************/
int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data)
{
    int rc;
    int read_bytes = 0;
    int total_read_bytes = 0;
    rc = write(sock, local_data, xfer_size);

    if(rc < xfer_size)
    {
        fprintf(stderr, "Failed writing data during sock_sync_data\n");
    }
    else
    {
        rc = 0;
    }

    while(!rc && total_read_bytes < xfer_size)
    {
        read_bytes = read(sock, remote_data, xfer_size);
        if(read_bytes > 0)
        {
            total_read_bytes += read_bytes;
        }
        else
        {
            rc = read_bytes;
        }
    }
    return rc;
}
/******************************************************************************
End of socket operations
******************************************************************************/

/* poll_completion */
/******************************************************************************
* Function: poll_completion
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description:
* Poll the completion queue for a single event. This function will continue to
* poll the queue until MAX_POLL_CQ_TIMEOUT milliseconds have passed.
*
******************************************************************************/
static inline int poll_completion(struct resources *res)
{
    struct ibv_wc wc;
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    // 等待CQ上的事件发生
    if (ibv_get_cq_event(res->comp_channel, &ev_cq, &ev_ctx)) {
        // 在生产环境中，可能需要更优雅的错误处理方式
        return 1;
    }
    // 确认收到的CQ事件，避免内存泄漏
    ibv_ack_cq_events(ev_cq, 1);

    // 轮询CQ以获取工作完成状态
    int poll_result = ibv_poll_cq(res->cq, 1, &wc);
    if (poll_result < 0) {
        // Poll CQ failed
        return 1;
    } else if (poll_result == 0) {
        // Theoretically shouldn't happen after getting a CQ event
        return 1;
    } else {
        // Check the completion status
        if (wc.status != IBV_WC_SUCCESS) {
            return 1;
        }
    }

    return 0; // Success
}

/******************************************************************************
* Function: post_send
*
* Input:
* res: pointer to resources structure
* opcode: IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: This function will create and post a send work request
******************************************************************************/
static int post_send(struct resources *res, int opcode)
{
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;

    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)res->buf;
    sge.length = MSG_SIZE;
    sge.lkey = res->mr->lkey;

    /* prepare the send work request */
    memset(&sr, 0, sizeof(sr));
    sr.next = NULL;
    sr.wr_id = 0;
    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = opcode;
    sr.send_flags = IBV_SEND_SIGNALED;
    if(opcode != IBV_WR_SEND)
    {
        sr.wr.rdma.remote_addr = res->remote_props.addr;
        sr.wr.rdma.rkey = res->remote_props.rkey;
    }

    /* there is a Receive Request in the responder side, so we won't get any into RNR flow */
    rc = ibv_post_send(res->qp, &sr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post SR\n");
    }
    else
    {
        switch(opcode)
        {
        case IBV_WR_SEND:
            fprintf(stdout, "Send Request was posted\n");
            break;
        case IBV_WR_RDMA_READ:
            //fprintf(stdout, "RDMA Read Request was posted\n");
            break;
        case IBV_WR_RDMA_WRITE:
            fprintf(stdout, "RDMA Write Request was posted\n");
            break;
        default:
            fprintf(stdout, "Unknown Request was posted\n");
            break;
        }
    }
    return rc;
}

/******************************************************************************
* Function: post_receive
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: post RR to be prepared for incoming messages
*
******************************************************************************/
static int post_receive(struct resources *res)
{
    struct ibv_recv_wr rr;
    struct ibv_sge sge;
    struct ibv_recv_wr *bad_wr;
    int rc;

    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)res->buf;
    sge.length = MSG_SIZE;
    sge.lkey = res->mr->lkey;

    /* prepare the receive work request */
    memset(&rr, 0, sizeof(rr));
    rr.next = NULL;
    rr.wr_id = 0;
    rr.sg_list = &sge;
    rr.num_sge = 1;

    /* post the Receive Request to the RQ */
    rc = ibv_post_recv(res->qp, &rr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post RR\n");
    }
    else
    {
        //fprintf(stdout, "Receive Request was posted\n");
    }
    return rc;
}

/******************************************************************************
* Function: resources_init
*
* Input:
* res: pointer to resources structure
*
* Output: res is initialized
*
* Returns: none
*
* Description: res is initialized to default values
******************************************************************************/
static void resources_init(struct resources *res)
{
    memset(res, 0, sizeof(struct resources));
}

static int init_sock(){
    int rc = 0;
    
    /* if client side */
    if(config.server_name)
    {
        qp_init_sock = sock_connect(config.server_name, config.tcp_port);
        if(qp_init_sock < 0)
        {
            fprintf(stderr, "failed to establish TCP connection to server %s, port %d\n",
                    config.server_name, config.tcp_port);
        }
        sleep(1);
        qp_sync_sock = sock_connect(config.server_name, config.tcp_port + 1);
        if(qp_sync_sock < 0)
        {
            fprintf(stderr, "failed to establish TCP connection to server %s, port %d\n",
                    config.server_name, config.tcp_port + 1);
        }
        sleep(1);
        qp_finish_sock = sock_connect(config.server_name, config.tcp_port + 2);
        if(qp_finish_sock < 0)
        {
            fprintf(stderr, "failed to establish TCP connection to server %s, port %d\n",
                    config.server_name, config.tcp_port + 2);
        }
    }

    return rc;
}

/******************************************************************************
* Function: resources_create
*
* Input: res pointer to resources structure to be filled in
*
* Output: res filled in with resources
*
* Returns: 0 on success, 1 on failure
*
* Description:
* This function creates and allocates all necessary system resources. These
* are stored in res.
*****************************************************************************/
static int resources_create(struct resources *res)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    size_t size;
    int i;
    int mr_flags = 0;
    int cq_size = 0;
    int num_devices;
    int rc = 0;

    /* get device names in the system */
    dev_list = ibv_get_device_list(&num_devices);
    if(!dev_list)
    {
        fprintf(stderr, "failed to get IB devices list\n");
        rc = 1;
        goto resources_create_exit;
    }

    /* if there isn't any IB device in host */
    if(!num_devices)
    {
        fprintf(stderr, "found %d device(s)\n", num_devices);
        rc = 1;
        goto resources_create_exit;
    }
    //fprintf(stdout, "found %d device(s)\n", num_devices);

    /* search for the specific device we want to work with */
    for(i = 0; i < num_devices; i ++)
    {
        if(!config.dev_name)
        {
            config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
            fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name);
        }
		/* find the specific device */
        if(!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name))
        {
            ib_dev = dev_list[i];
            break;
        }
    }

    /* if the device wasn't found in host */
    if(!ib_dev)
    {
        fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }

    /* get device handle */
    res->ib_ctx = ibv_open_device(ib_dev);
    if(!res->ib_ctx)
    {
        fprintf(stderr, "failed to open device %s\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }

    /* We are now done with device list, free it */
    ibv_free_device_list(dev_list);
    dev_list = NULL;
    ib_dev = NULL;

    /* query port properties */
    if(ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr))
    {
        fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
        rc = 1;
        goto resources_create_exit;
    }

    /* allocate Protection Domain */
    res->pd = ibv_alloc_pd(res->ib_ctx);
    if(!res->pd)
    {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        rc = 1;
        goto resources_create_exit;
    }

    // 创建完成事件通道
    res->comp_channel = ibv_create_comp_channel(res->ib_ctx);
    if (!res->comp_channel) {
        fprintf(stderr, "Failed to create completion event channel\n");
        // 错误处理
        return -1;
    }

    /* each side will send only one WR, so Completion Queue with 1 entry is enough */
    cq_size = 1;
    res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, res->comp_channel, 0);
    if(!res->cq)
    {
        fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
        rc = 1;
        goto resources_create_exit;
    }

    /* allocate the memory buffer that will hold the data */
    size = MSG_SIZE;
    res->buf = (char *) malloc(size);
    if(!res->buf)
    {
        fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
        rc = 1;
        goto resources_create_exit;
    }
    memset(res->buf, 0 , size);

    /* only in the server side put the message in the memory buffer */
    if(!config.server_name)
    {
        strcpy(res->buf, MSG);
        fprintf(stdout, "going to send the message: '%s'\n", res->buf);
    }
    else
    {
        memset(res->buf, 0, size);
    }

    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;
    res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
    if(!res->mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
    // fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
    //         res->buf, res->mr->lkey, res->mr->rkey, mr_flags);

    /* create the Queue Pair */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.cap.max_send_wr = 1;
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if(!res->qp)
    {
        fprintf(stderr, "failed to create QP\n");
        rc = 1;
        goto resources_create_exit;
    }
    //fprintf(stdout, "QP was created, QP number=0x%x\n", res->qp->qp_num);

resources_create_exit:
    if(rc)
    {
        /* Error encountered, cleanup */
        if(res->qp)
        {
            ibv_destroy_qp(res->qp);
            res->qp = NULL;
        }
        if(res->mr)
        {
            ibv_dereg_mr(res->mr);
            res->mr = NULL;
        }
        if(res->buf)
        {
            free(res->buf);
            res->buf = NULL;
        }
        if(res->cq)
        {
            ibv_destroy_cq(res->cq);
            res->cq = NULL;
        }
        if(res->pd)
        {
            ibv_dealloc_pd(res->pd);
            res->pd = NULL;
        }
        if(res->ib_ctx)
        {
            ibv_close_device(res->ib_ctx);
            res->ib_ctx = NULL;
        }
        if(dev_list)
        {
            ibv_free_device_list(dev_list);
            dev_list = NULL;
        }
    }
    return rc;
}


/******************************************************************************
* Function: modify_qp_to_init
*
* Input:
* qp: QP to transition
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: Transition a QP from the RESET to INIT state
******************************************************************************/
static int modify_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = config.ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to INIT\n");
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_rtr
*
* Input:
* qp: QP to transition
* remote_qpn: remote QP number
* dlid: destination LID
* dgid: destination GID (mandatory for RoCEE)
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: 
* Transition a QP from the INIT to RTR state, using the specified QP number
******************************************************************************/
static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 0x12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dlid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = config.ib_port;
    if(config.gid_idx >= 0)
    {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = config.gid_idx;
        attr.ah_attr.grh.traffic_class = 0;
    }

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_rts
*
* Input:
* qp: QP to transition
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: Transition a QP from the RTR to RTS state
******************************************************************************/
static int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x12;
    attr.retry_cnt = 6;
    attr.rnr_retry = 0;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
    }
    return rc;
}

/******************************************************************************
* Function: connect_qp
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: 
* Connect the QP. Transition the server side to RTR, sender side to RTS
******************************************************************************/
static int connect_qp(struct resources *res)
{
    struct cm_con_data_t local_con_data;
    struct cm_con_data_t remote_con_data;
    struct cm_con_data_t tmp_con_data;
    int rc = 0;
    char temp_char;
    union ibv_gid my_gid;
    if(config.gid_idx >= 0)
    {
        rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
        if(rc)
        {
            fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
            return rc;
        }
    }
    else
    {
        memset(&my_gid, 0, sizeof my_gid);
    }

    /* exchange using TCP sockets info required to connect QPs */
    local_con_data.addr = htonll((uintptr_t)res->buf);
    local_con_data.rkey = htonl(res->mr->rkey);
    local_con_data.qp_num = htonl(res->qp->qp_num);
    local_con_data.lid = htons(res->port_attr.lid);
    memcpy(local_con_data.gid, &my_gid, 16);
    //fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);
    if(sock_sync_data(qp_init_sock, sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0)
    {
        fprintf(stderr, "failed to exchange connection data between sides\n");
        rc = 1;
        goto connect_qp_exit;
    }

    remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey);
    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid);
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

    /* save the remote side attributes, we will need it for the post SR */
    res->remote_props = remote_con_data;
    // fprintf(stdout, "Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
    // fprintf(stdout, "Remote rkey = 0x%x\n", remote_con_data.rkey);
    // fprintf(stdout, "Remote QP number = 0x%x\n", remote_con_data.qp_num);
    // fprintf(stdout, "Remote LID = 0x%x\n", remote_con_data.lid);
    if(config.gid_idx >= 0)
    {
        //uint8_t *p = remote_con_data.gid;
        // fprintf(stdout, "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		// 		p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }

    /* modify the QP to init */
    rc = modify_qp_to_init(res->qp);
    if(rc)
    {
        fprintf(stderr, "change QP state to INIT failed\n");
        goto connect_qp_exit;
    }

    /* let the client post RR to be prepared for incoming messages */

    rc = post_receive(res);
    if(rc)
    {
        fprintf(stderr, "failed to post RR\n");
        goto connect_qp_exit;
    }
    

    /* modify the QP to RTR */
    rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit;
    }

    /* modify the QP to RTS */
    rc = modify_qp_to_rts(res->qp);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
        goto connect_qp_exit;
    }
    //fprintf(stdout, "QP state was change to RTS\n");

    /* sync to make sure that both sides are in states that they can connect to prevent packet loose */
    if(sock_sync_data(qp_sync_sock, 1, "Q", &temp_char))  /* just send a dummy char back and forth */
    {
        fprintf(stderr, "sync error after QPs are were moved to RTS\n");
        rc = 1;
    }

connect_qp_exit:
    return rc;
}

/******************************************************************************
* Function: resources_destroy
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description: Cleanup and deallocate all resources used
******************************************************************************/
static int resources_destroy(struct resources *res)
{
    int rc = 0;
    if(res->qp)
	{
                if(ibv_destroy_qp(res->qp))
        {
            fprintf(stderr, "failed to destroy QP\n");
            rc = 1;
        }
	}

    if(res->mr)
	{
        if(ibv_dereg_mr(res->mr))
        {
            fprintf(stderr, "failed to deregister MR\n");
            rc = 1;
        }
	}

    if(res->buf)
    {
        free(res->buf);
    }

    if(res->cq)
	{
        if(ibv_destroy_cq(res->cq))
        {
            fprintf(stderr, "failed to destroy CQ\n");
            rc = 1;
        }
	}

    if(res->pd)
	{
        if(ibv_dealloc_pd(res->pd))
        {
            fprintf(stderr, "failed to deallocate PD\n");
            rc = 1;
        }
	}

    if(res->ib_ctx)
	{
        if(ibv_close_device(res->ib_ctx))
        {
            fprintf(stderr, "failed to close device context\n");
            rc = 1;
        }
	}
    free(res);
    return rc;
}

//关闭总sock
static int close_sock(){
    int rc = 0;
    
    if(close(qp_init_sock))
	{
        fprintf(stderr, "failed to close socket\n");
        rc = 1;
    }

    if(close(qp_sync_sock))
        {
            fprintf(stderr, "failed to close socket\n");
            rc = 1;
        }

    if(close(qp_finish_sock))
    {
        fprintf(stderr, "failed to close socket\n");
        rc = 1;
	}

    return rc;
}
struct ibv_qp_attr attr;
struct ibv_qp_init_attr init_attr;

#define QP_COUNT 3 // 假设有3个QP conut

int event_fds[QP_COUNT];

void init_event_fds() {
    for(int i = 0; i < QP_COUNT; ++i) {
        event_fds[i] = eventfd(0, EFD_NONBLOCK); // 初始化为非阻塞模式
        if (event_fds[i] == -1) {
            perror("eventfd");
            exit(EXIT_FAILURE);
        }
    }
}

int check_qp_connection(struct resources *res) {
    // 查询 QP 状态
    int ret = ibv_query_qp(res->qp, &attr, IBV_QP_STATE, &init_attr);
    if (ret != 0) {
        perror("ibv_query_qp failed");
        return -1;
    }

    // 检查 QP 状态
    if (attr.qp_state == IBV_QPS_ERR) {
        printf("QP is in error state.\n");
        return -1; // 表示连接中断或有错误发生
    } else {
        printf("QP is operational and not in error state.\n");
        return 0; // 表示连接正常
    }
}

/******************************************************************************
* Function: print_config
*
* Input: none
*
* Output: none
*
* Returns: none
*
* Description: Print out config information
******************************************************************************/
static void print_config(void)
{
    fprintf(stdout, " ------------------------------------------------\n");
    fprintf(stdout, " Device name : \"%s\"\n", config.dev_name);
    fprintf(stdout, " IB port : %u\n", config.ib_port);
    if(config.server_name)
    {
        fprintf(stdout, " IP : %s\n", config.server_name);
    }
    fprintf(stdout, " TCP port : %u\n", config.tcp_port);
    if(config.gid_idx >= 0)
    {
        fprintf(stdout, " GID index : %u\n", config.gid_idx);
    }
    fprintf(stdout, " ------------------------------------------------\n\n");
}

/******************************************************************************
* Function: usage
*
* Input:
* argv0: command line arguments
*
* Output: none
*
* Returns: none
*
* Description: print a description of command line syntax
******************************************************************************/
static void usage(const char *argv0)
{
    fprintf(stdout, "Usage:\n");
    fprintf(stdout, " %s start a server and wait for connection\n", argv0);
    fprintf(stdout, " %s <host> connect to server at <host>\n", argv0);
    fprintf(stdout, "\n");
    fprintf(stdout, "Options:\n");
    fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
    fprintf(stdout, " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n");
    fprintf(stdout, " -i, --ib-port <port> use port <port> of IB device (default 1)\n");
    fprintf(stdout, " -g, --gid_idx <git index> gid index to be used in GRH (default not used)\n");
}

void writeCharToFile(const char* filename, const char* content) {
    // 打开或创建文件，采用追加模式
    FILE *file = fopen(filename, "a");
    if (file == NULL) {
        printf("无法打开或创建文件。\n");
        return;
    }

    // 写入内容
    fwrite(content, sizeof(char), strlen(content), file);

    // 关闭文件
    fclose(file);
}

void set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("获取文件状态标志失败");
        return;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("设置非阻塞模式失败");
    }
}

//-1: 出错 0: 没有新数据 1: 有新数据
int poll_socket(int sockfd, char* char_ret) {
    int bytesRead = read(sockfd, char_ret, 1); // 尝试读取 1 字节的数据
    if (bytesRead == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        perror("读取数据时出错");
        return -1;
    }else if (bytesRead == 0) {
        return 0;
    }
    return 1;
}

int send_char_to_socket(int sockfd, char ch) {
    
    // 发送单个字符
    int bytesSent = send(sockfd, &ch, sizeof(ch), 0);
    
    if (bytesSent < 0) {
        // 处理错误
        perror("send error");
    }
    
    return bytesSent;
}


/******************************************************************************
* Function: main
*
* Input:
* argc: number of items in argv
* argv: command line parameters
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description: Main program code
******************************************************************************/



//QP池
struct resources *res_pool[10];

//线程锁
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int should_wait[10]; // 标记每个线程是否需要等待
} ControlBlock;

ControlBlock control;


pthread_t threads[10];
int thread_ids[10];


void wait_n_nanoseconds(long nanoseconds) {
    struct timespec ts;
    ts.tv_sec = 0; // 秒部分
    ts.tv_nsec = nanoseconds; // 转换为纳秒
    nanosleep(&ts, NULL); // 如果被信号中断，第二个参数可用于剩余时间
}

void* handle_qp_read(void* arg){
    int qp_index = *((int*) arg);
    printf("qp %d线程准备好\n", qp_index);

    uint64_t u;
    int sleep_ti = 0;

    while(1){
        ssize_t s = read(event_fds[qp_index], &u, sizeof(uint64_t)); // 非阻塞读取
        if (s == sizeof(uint64_t)) {
            sleep_ti = 0;
            // 执行RDMA读取操作
            if (ibv_req_notify_cq(res_pool[qp_index]->cq, 0)) {
                fprintf(stderr, "Couldn't request CQ notification\n");
                return NULL;
            }
            if(post_send(res_pool[qp_index], IBV_WR_RDMA_READ)){
                fprintf(stderr, "failed to post SR 2\n");
            }
            if(poll_completion(res_pool[qp_index])){
                fprintf(stderr, "poll completion failed 2, id: %d\n", qp_index);
            }

            send_char_to_socket(qp_finish_sock, qp_index + '0');
        } else if (s == -1 && errno != EAGAIN) { // 处理错误
            perror("read error");
            return NULL;
        }else{
            // 等待一段时间再进行下一次轮询
            if(sleep_ti==0){
                usleep(200);
                sleep_ti = 1;
            }else{
                usleep(20);
            }
        }
         // 每隔500毫秒检查一次
    }
    return NULL;
}


void init_threads(){
    printf("init_threads\n"); // 打印当前进程号
    for(int i = 0; i < 3; ++i) {
        thread_ids[i] = i;
        if(pthread_create(&threads[i], NULL, handle_qp_read, (void*)&thread_ids[i]) != 0) {
            perror("Failed to create thread");
            exit(EXIT_FAILURE);
        }
    }
     printf("init_threads done\n"); // 打印当前进程号
}

void release_thread(int qp_index) {
    uint64_t u = 1;
    ssize_t s = write(event_fds[qp_index], &u, sizeof(uint64_t));
    if (s != sizeof(uint64_t)) {
        perror("write");
    }
}

int main(int argc, char *argv[])
{
    pid_t pid = getpid(); // 获取当前进程号
    printf("Current process ID: %d\n", pid); // 打印当前进程号
    int rc = 1;
    /* parse the command line parameters */
    while(1)
    {
        int c;
		/* Designated Initializer */
        static struct option long_options[] =
        {
            {.name = "port", .has_arg = 1, .val = 'p' },
            {.name = "ib-dev", .has_arg = 1, .val = 'd' },
            {.name = "ib-port", .has_arg = 1, .val = 'i' },
            {.name = "gid-idx", .has_arg = 1, .val = 'g' },
            {.name = NULL, .has_arg = 0, .val = '\0'}
        };

        c = getopt_long(argc, argv, "p:d:i:g:", long_options, NULL);
        if(c == -1)
        {
            break;
        }
        switch(c)
        {
        case 'p':
            config.tcp_port = strtoul(optarg, NULL, 0);
            break;
        case 'd':
            config.dev_name = strdup(optarg);
            break;
        case 'i':
            config.ib_port = strtoul(optarg, NULL, 0);
            if(config.ib_port < 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'g':
            config.gid_idx = strtoul(optarg, NULL, 0);
            if(config.gid_idx < 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* parse the last parameter (if exists) as the server name */
	/* 
	 * server_name is null means this node is a server,
	 * otherwise this node is a client which need to connect to 
	 * the specific server
	 */
    if(optind == argc - 1)
    {
        config.server_name = argv[optind];
    }
    else if(optind < argc)
    {
        usage(argv[0]);
        return 1;
    }

    /* print the used parameters for info*/
    print_config();
    if(init_sock()){
        fprintf(stderr, "init sock failed\n");
        goto main_exit;
    }
    
    //初始化qp
    for(int i=0;i<3;i++){
        res_pool[i] = (struct resources*) malloc(sizeof(struct resources));
        resources_init(res_pool[i]);
        if(resources_create(res_pool[i]))
        {
            fprintf(stderr, "failed to create resources\n");
            goto main_exit;
        }
         /* connect the QPs */
        if(connect_qp(res_pool[i]))
        {
            fprintf(stderr, "failed to connect QPs\n");
            goto main_exit;
        }
    }

    //socket设置为非阻塞
    //set_nonblocking(qp_finish_sock);
    //初始化线程池
    init_event_fds();
    init_threads();
    

    char temp_char;

    while(1){
        poll_socket(qp_finish_sock, &temp_char);
        if(temp_char == 'F'){
            break;
            //发送结束
        }
        //
        int qp_index = temp_char - '0';
        //这时:释放线程，进行传输
        // if(qp_index==0){
        //     printf("get char: %c\n", temp_char);
        // }
        release_thread(qp_index);
        //流切片等待
        wait_n_nanoseconds(100);
    }
    //等待数据全部传输完
    sleep(2);
    rc = 0;

main_exit:
    if(config.dev_name)
    {
        free((char *) config.dev_name);
    }
    if(close_sock()){
        fprintf(stderr, "failed to close global sock\n");
    }
    fprintf(stdout, "\ntest result is %d\n", rc);
    return rc;
}
