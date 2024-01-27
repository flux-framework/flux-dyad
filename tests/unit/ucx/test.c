#include <archive.h>
#include <czmq_prelude.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucp/api/ucp.h>
#include <unistd.h>
#define MAX_TRANSPORT_PARTS (1)
#define TOTAL_WORKERS (MAX_TRANSPORT_PARTS + 1)
#define RECV_WORKER_IDX (MAX_TRANSPORT_PARTS)
#define MAX_SETUP_RKEY_SIZE (32)

#define CHECK_UCP_OK(status, msg)                          \
    do {                                                   \
        if (status != UCS_OK) {                            \
            fprintf(stderr, "%s %s (%d)\n", msg,           \
                    ucs_status_string(status),             \
                    (int) status);                         \
        }                                                  \
    } while (0);

struct part_worker_ctx_t {
    ucp_worker_h   ucp_worker;
    ucp_address_t *address;
    size_t         address_length;
    ucp_ep_h       *endpoints;
};
typedef struct part_worker_ctx_t part_worker_ctx_t;

struct ompi_mca_ucx_setup_t {
    void *raddr;
    char *rkey_buffer[MAX_SETUP_RKEY_SIZE];
    size_t rkey_size;
};

/*****************************************************************************
 * Global Params                                                             *
 *****************************************************************************/
int rank;

ucp_context_h ucp_context;

part_worker_ctx_t worker_ctx[TOTAL_WORKERS];

ucp_address_t **remote_address;
size_t         *remote_address_length;

/*****************************************************************************
 * Helper Functions                                                          *
 *****************************************************************************/
// Define a request struct to be used in handling
// async UCX operations
struct ucx_request {
    int completed;
};
typedef struct ucx_request dyad_ucx_request_t;
// Define a function that UCX will use to allocate and
// initialize our request struct
static void dyad_ucx_request_init (void* request)
{
    dyad_ucx_request_t* real_request = NULL;
    real_request = (dyad_ucx_request_t*)request;
    real_request->completed = 0;
}
static void cb_function(void *request, ucs_status_t status, void *user_data)
{
    printf("In cb_function %d pid %d\n", status, getpid());
    dyad_ucx_request_t* real_req = (dyad_ucx_request_t*)request;
    real_req->completed = 1;
    fflush(stdout);
}
static inline void
mca_part_ucx_init_ucp(void)
{
    ucp_config_t* ucp_config;
    ucs_status_t status;
    ucp_params_t ucp_params;
    ucp_worker_params_t ucp_worker_params;
    int world_size;

    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    status = ucp_config_read(NULL, NULL, &ucp_config);
    CHECK_UCP_OK(status, "ucp_config_read() failed");

    memset(&ucp_params, 0, sizeof(ucp_params_t));

    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES
                            | UCP_PARAM_FIELD_REQUEST_SIZE
                            | UCP_PARAM_FIELD_MT_WORKERS_SHARED
                            | UCP_PARAM_FIELD_ESTIMATED_NUM_EPS;

    ucp_params.features = UCP_FEATURE_RMA
                          | UCP_FEATURE_AMO32;

    ucp_params.mt_workers_shared = 1;
    ucp_params.estimated_num_eps = (world_size * TOTAL_WORKERS);

    ucp_params.request_size = sizeof (struct ucx_request);
    ucp_params.request_init = dyad_ucx_request_init;
    status = ucp_init(&ucp_params, ucp_config, &ucp_context);
    CHECK_UCP_OK(status, "ucp_init() failed\n");

    ucp_config_release(ucp_config);

    /* Create Send UCX Workers */
    memset(&ucp_worker_params, 0, sizeof(ucp_worker_params_t));

    ucp_worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    ucp_worker_params.thread_mode = UCS_THREAD_MODE_SERIALIZED;

    for (int i=0; i<TOTAL_WORKERS; i++) {
        status = ucp_worker_create(ucp_context, &ucp_worker_params,
                                   &worker_ctx[i].ucp_worker);
        CHECK_UCP_OK(status, "ucp_worker_create() failed\n");

        status = ucp_worker_get_address(worker_ctx[i].ucp_worker,
                                        &worker_ctx[i].address,
                                        &worker_ctx[i].address_length);
        CHECK_UCP_OK(status, "ucp_worker_create(send) failed\n");

        worker_ctx[i].endpoints = (ucp_ep_h*)  calloc(world_size, sizeof(ucp_ep_h));

        for (int k=0; k<world_size; k++) {
            worker_ctx[i].endpoints[k] = NULL;
        }
    }

    /* Allocate UCP Worker address information */
    /* This will be populated as we call MPI_Psend/recv_init */
    remote_address = (ucp_address_t**) calloc(world_size, sizeof(ucp_address_t**));
    remote_address_length = (size_t*)  calloc(world_size, sizeof(size_t*));

    for (int i=0; i<world_size; i++) {
        remote_address[i] = NULL;
        remote_address_length[i] = -1;
    }
}

static inline void
mca_part_ucx_exchange_worker_handles(void)
{
    if (0 == rank)
    {
        MPI_Recv(&remote_address_length[1], 1, MPI_LONG, 1, 123, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        remote_address[1] = calloc(1, remote_address_length[1]);

        MPI_Recv(remote_address[1], remote_address_length[1], MPI_BYTE, 1, 123, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
    }
    else if (1 == rank)
    {
        MPI_Send(&worker_ctx[RECV_WORKER_IDX].address_length, 1, MPI_LONG, 0, 123, MPI_COMM_WORLD);
        MPI_Send(worker_ctx[RECV_WORKER_IDX].address, worker_ctx[RECV_WORKER_IDX].address_length,
                 MPI_BYTE, 0, 123, MPI_COMM_WORLD);
    }
}

static inline void
mca_part_ucx_create_ep(void)
{
    ucs_status_t status;
    ucp_ep_params_t ucp_ep_params;

    memset(&ucp_ep_params, 0, sizeof(ucp_ep_params_t));

    ucp_ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ucp_ep_params.address = remote_address[1];

    for (int w=0; w<MAX_TRANSPORT_PARTS; w++)
    {
        status = ucp_ep_create(worker_ctx[w].ucp_worker, &ucp_ep_params,
                               &worker_ctx[w].endpoints[1]);
        CHECK_UCP_OK(status, "ucp_ep_create() failed");
    }
}

static inline void
mca_part_ucx_reg_mem(const void *buf, size_t bytes,
                     ucp_mem_h *memh, void **rkey_buffer, size_t *size)
{
    ucs_status_t status;
    ucp_mem_map_params_t mem_map_params;

    mem_map_params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS
                                | UCP_MEM_MAP_PARAM_FIELD_LENGTH
                                | UCP_MEM_MAP_PARAM_FIELD_FLAGS;
    mem_map_params.address = (void*) buf;
    mem_map_params.length = bytes;
    mem_map_params.flags = 0; //UCP_MEM_MAP_NONBLOCK;

    status = ucp_mem_map(ucp_context, &mem_map_params, memh);
    CHECK_UCP_OK(status, "ucp_mem_map() failed");

    status = ucp_rkey_pack(ucp_context, (*memh), rkey_buffer, size);
    CHECK_UCP_OK(status, "ucp_rkey_pack() failed");
}

int main()
{
    MPI_Init(NULL, NULL);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    mca_part_ucx_init_ucp();
    mca_part_ucx_exchange_worker_handles();

    if (0 == rank)
    {
        mca_part_ucx_create_ep();
    }
    printf("rank %d pid %d\n", rank, getpid());
    size_t bytes = sizeof(int32_t);
    int32_t *buf = calloc(1, bytes);
    int32_t orig = 2*(rank + 1);
    buf[0] = orig;

    struct ompi_mca_ucx_setup_t setup_info;
    ucp_mem_h memh;
    void *raddr;
    void *rkey_buffer;
    size_t rkey_size;
    ucp_rkey_h rkey;

    /********************************
     * Exchange Rkeys
     ********************************/
    if (1 == rank) {
        mca_part_ucx_reg_mem(buf, bytes, &memh, &rkey_buffer, &rkey_size);

        setup_info.raddr = (void*) buf;
        setup_info.rkey_size = rkey_size;

        if (MAX_SETUP_RKEY_SIZE < rkey_size)
        {
            printf("Increase MAX_SETUP_RKEY_SIZE\n");
            return -1;
        }

        memcpy(setup_info.rkey_buffer, rkey_buffer, rkey_size);

        MPI_Send(&setup_info, sizeof(setup_info), MPI_BYTE, 0, 123, MPI_COMM_WORLD);
    }
    else if (0 == rank)
    {
        MPI_Recv(&setup_info, sizeof(setup_info), MPI_BYTE, 1, 123, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        raddr = setup_info.raddr;
        rkey_size = setup_info.rkey_size;
        rkey_buffer = calloc(1, rkey_size);

        memcpy(rkey_buffer, setup_info.rkey_buffer, rkey_size);

        ucs_status_t status;

        for (int w=0; w<MAX_TRANSPORT_PARTS; w++)
        {
            status = ucp_ep_rkey_unpack(worker_ctx[w].endpoints[1], rkey_buffer, &rkey);
            CHECK_UCP_OK(status, "ucp_ep_rkey_unpack() failed");
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);

    /********************************
     * Atomic Put
     ********************************/
    if (0 == rank)
    {
        int progress = 1;
        ucs_status_ptr_t status_ptr;
        ucp_request_param_t params;

        params.op_attr_mask = UCP_OP_ATTR_FIELD_DATATYPE
                              | UCP_OP_ATTR_FIELD_CALLBACK;
        params.cb.send = cb_function;
        params.datatype = ucp_dt_make_contig(sizeof(uint32_t));

        status_ptr = ucp_put_nbx(worker_ctx[0].endpoints[1], buf,
                                       1, (uint64_t) raddr, rkey, &params);

        if (UCS_OK == status_ptr)
        {
            progress = 0;
            printf("// Finished immidiately\n");
            fflush(stdout);
        }
        else if (UCS_PTR_IS_ERR(status_ptr))
        {
            fprintf(stderr, "ucp_put_nbx() failed %s (%d)\n",
                    ucs_status_string(UCS_PTR_STATUS(status_ptr)),
                    UCS_PTR_STATUS(status_ptr));
            fflush(stdout);
            return -1;
        }
        ucs_status_t final_request_status = UCS_OK;
        if (UCS_PTR_IS_PTR (status_ptr)) {
            // Spin lock until the request is completed
            // The spin lock shouldn't be costly (performance-wise)
            // because the wait should always come directly after other UCX calls
            // that minimize the size of the worker's event queue.
            // In other words, prior UCX calls should mean that this loop only runs
            // a couple of times at most.
            do {
                ucp_worker_progress (worker_ctx[0].ucp_worker);
                // usleep(100);
                // Get the final status of the communication operation
                final_request_status = ucp_request_check_status (status_ptr);
            } while (final_request_status == UCS_INPROGRESS);
            // Free and deallocate the request object
            ucp_request_free (status_ptr);
        }
            // If 'request' is actually a UCX error, this means the communication
            // operation immediately failed. In that case, we simply grab the
            // 'ucs_status_t' object for the error.
        else if (UCS_PTR_IS_ERR (status_ptr)) {
            final_request_status = UCS_PTR_STATUS (status_ptr);
        }
        fprintf(stderr, "ucp_put_nbx() finished\n");
    }else
    {
        while (orig == buf[0])
        {
            ucp_worker_progress(worker_ctx[RECV_WORKER_IDX].ucp_worker);
            printf("%d\n", buf[0]);
            usleep(100);
        }
    }

    printf("[%d] Done with data %d orig %d\n", rank, buf[0], orig);
    MPI_Barrier(MPI_COMM_WORLD);
    ////MPI_Finalize();
    return 0;
}