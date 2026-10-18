#include "tinyfile.h"
#include "include.h"
#include "regions.h"
#include "ringbuffer.h"
#include "shm_common.h"
#include "protocols.h"
#include "debug.h"

#include <unordered_map>
#include <vector>
#include <cstring>
#include <algorithm>

using namespace std;

static const char *INFO_FILE_PATH = "/tmp/tinyfile_server_info.txt";

// ---------Structures---------
typedef struct PendingRequest {
    uint32_t         req_id;
    bool             done;
    vector<uint8_t>  result_data;   // appended to as DATA_READY response chunks arrive
}pendingreq_t;

// ---------Global Variables---------
static shared_region_t *g_region = nullptr;
static int      g_shm_id       = -1;
static int      g_client_id    = -1;
static uint32_t g_sms_size     = 0;
static uint32_t g_max_inflight = 0;
static int      g_s2c_semid    = -1;
static ringbuffer_t *g_c2sbuffer = nullptr;
static ringbuffer_t *g_s2cbuffer = nullptr;

static uint32_t g_next_req_id = 1;   // 0 reserved as "invalid"
static uint32_t g_async_pending = 0; // number of async requests still waiting for their result
static bool g_init = false;
static bool g_registered = false;

static unordered_map<uint32_t, vector<ringnode_t>> g_pending_grants;  // req_id -> queued GRANTs
static unordered_map<uint32_t, pendingreq_t>      g_active_requests; // req_id -> accumulated response

static struct sembuf post_op = { 0, +1, 0 };
static struct sembuf wait_op = { 0, -1, 0 };


// ---------Helper Functions---------
static tf_err attach_to_region();
static tf_err register_client(uint32_t freqs);
static tf_err unregister_client();
static tf_err detach_from_region();
static void drain_and_route();
static tf_err send_file(const uint8_t *data, size_t len, transfer_mode mode, uint32_t req_id);
static tf_err poll_response_blocking(uint32_t req_id, uint8_t **out_data, size_t *out_len);
static tf_err poll_response_nonblocking();

// ---------Small inline helpers---------
static tf_err push_and_notify(const ringnode_t &msg) {
    if (!g_c2sbuffer->push(msg)) {
        return tf_err::E_RING_FULL;
    }
    if (!notify_if_needed(g_region->doorbell_semid)) {
        return tf_err::E_SEMOP_FAILED;
    }
    gt_debug(stderr,"[CLIENT %d]doorbell value: %d\n", g_client_id, semctl(g_region->doorbell_semid, 0, GETVAL));
    return tf_err::SUCCESS;
}

static tf_err send_seg_req(uint32_t req_id) {
    ringnode_t msg = createMsg(SEG_REQ, SNGL, SYNC, req_id, 0, 0);
    return push_and_notify(msg);
}

static tf_err send_data_ready(uint32_t req_id, uint32_t segment_id, uint32_t data_len, status st, transfer_mode tfm) {
    ringnode_t msg = createMsg(DATA_READY, st, tfm, req_id, segment_id, data_len);
    return push_and_notify(msg);
}

static tf_err send_seg_release(uint32_t req_id, uint32_t segment_id) {
    ringnode_t msg = createMsg(SEG_RELEASE, SNGL, SYNC, req_id, segment_id, 0);
    return push_and_notify(msg);
}

// ---------Definitions---------
static tf_err attach_to_region(){
    if (g_init) return tf_err::E_INITIALIZED;

    ServerInfo info;
    tf_err retcode = read_server_info(INFO_FILE_PATH, info);
    if (retcode != tf_err::SUCCESS) {
        return retcode;
    }
    g_shm_id = info.shm_id;
    void *shm = shmat(g_shm_id, nullptr, 0);
    if (shm == (void *)-1) {
        return tf_err::E_SHMAT_FAILED;
    }
    g_region = reinterpret_cast<shared_region_t*>(shm);
    g_init = true;
    return tf_err::SUCCESS;
}

static tf_err register_client(uint32_t weight){
    if (!g_init) return tf_err::E_UNINITIALIZED;
    if (g_registered) return tf_err::E_REGISTERED;

    if (semop(g_region->reg_semid, &wait_op, 1) == -1) {
        return tf_err::E_SEMOP_FAILED;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_region->clients[i].active) {
            continue;
        }
        g_region->clients[i].active   = true;
        g_region->clients[i].pid      = getpid();
        g_region->clients[i].seg_held = 0;
        g_region->clients[i].seg_req  = 0;
        g_region->clients[i].weight   = weight;

        g_client_id    = i;
        g_c2sbuffer    = &g_region->c2sbuffer[i];     
        g_s2cbuffer    = &g_region->s2cbuffer[i];
        g_max_inflight = g_region->clients[i].max_inflight;
        g_s2c_semid    = g_region->clients[i].s2c_semid;
        g_sms_size     = g_region->sms_size;
        g_registered   = true;
        break;
    }

    if (semop(g_region->reg_semid, &post_op, 1) == -1) {
        return tf_err::E_SEMOP_FAILED;
    }
    if (!g_registered) {
        return tf_err::E_CLIENT_TABLE_FULL;
    }
    return tf_err::SUCCESS;
}

static tf_err unregister_client(){
    if (!g_init) return tf_err::E_UNINITIALIZED;
    if (!g_registered) return tf_err::E_UNREGISTERED;

    if (semop(g_region->reg_semid, &wait_op, 1) == -1) {
        return tf_err::E_SEMOP_FAILED;
    }

    g_region->clients[g_client_id].pid          = -1;
    g_region->clients[g_client_id].seg_held     = 0;
    g_region->clients[g_client_id].seg_req      = 0;
    g_region->clients[g_client_id].active       = false;
    g_region->clients[g_client_id].weight       = 1;

    if (semop(g_region->reg_semid, &post_op, 1) == -1) {
        return tf_err::E_SEMOP_FAILED;
    }

    g_c2sbuffer    = nullptr;
    g_s2cbuffer    = nullptr;
    g_s2c_semid    = -1;
    g_client_id    = -1;
    g_sms_size     = 0;
    g_max_inflight = 0;
    g_registered   = false;
    return tf_err::SUCCESS;
}

static tf_err detach_from_region(){
    if (!g_init) return tf_err::E_UNINITIALIZED;
    if (g_registered) return tf_err::E_REGISTERED;

    shmdt(g_region);
    g_region = nullptr;
    g_shm_id = -1;
    g_init = false;
    return tf_err::SUCCESS;
}

static void drain_and_route(){
    // consume any additional already-posted semaphore counts without blocking,
    // so the semaphore count stays in sync with however many messages we pop here
    struct sembuf trywait = { 0, -1, IPC_NOWAIT };
    while (semop(g_s2c_semid, &trywait, 1) == 0) { /* draining extra counts */ }

    ringnode_t msg;
    while (g_s2cbuffer->pop(&msg)) {
        opcode op = get_opcode(msg.control_field);

        if (op == SEG_GRANT) {
            g_pending_grants[msg.req_id].push_back(msg);

        } else if (op == DATA_READY) {
            pendingreq_t &pr = g_active_requests[msg.req_id];
            pr.req_id = msg.req_id;

            uint8_t *src = segment_ptr(g_region, msg.segment_id);
            pr.result_data.insert(pr.result_data.end(), src, src + msg.data_len);

            status kind = get_status(msg.control_field);
            if (kind == FIN || kind == SNGL) {
                pr.done = true;
            }
            send_seg_release(msg.req_id, msg.segment_id);
        }
    }
}


static tf_err send_file(const uint8_t *data, size_t len, transfer_mode mode, uint32_t req_id){
    if(!g_init) return tf_err::E_UNINITIALIZED;
    if(!g_registered) return tf_err::E_UNREGISTERED;
    
    uint32_t n_chunks = static_cast<uint32_t>((len + g_sms_size - 1) / g_sms_size);
    if (n_chunks == 0) n_chunks = 1;

    uint32_t chunks_sent = 0;
    uint32_t outstanding = 0;
    size_t   offset = 0;

    // Prime the window
    while (outstanding < g_max_inflight && chunks_sent < n_chunks) {
        tf_err err = send_seg_req(req_id);
        if (err != tf_err::SUCCESS) return err;     //TODO: Shouldn't autofail when trying to send message
        outstanding++;
        chunks_sent++;
    }

    // Steady state: drain and route all incoming requests
    // Use up all available segments
    // Send more segment requests
    uint32_t chunks_granted = 0;
    while(chunks_granted<n_chunks){
        if (semop(g_s2c_semid, &wait_op, 1) == -1) {
            if (errno == EINTR) continue;
            return tf_err::E_SEMOP_FAILED;
        }

        drain_and_route();

        auto it = g_pending_grants.find(req_id);
        if (it == g_pending_grants.end()) continue; //No grants yet, try again

        while (!it->second.empty()){
            ringnode_t grant_msg = it->second.back();
            it->second.pop_back();

            uint32_t seg_id = grant_msg.segment_id;
            size_t chunk_len = min (len - offset, (size_t)g_sms_size);
            uint8_t *dst = segment_ptr(g_region, seg_id);
            memcpy(dst, data + offset, chunk_len);
            g_region->segment_control[seg_id].status = SEG_FILLED;
            offset += chunk_len;

            bool is_last = (offset >= len);
            status st =   (n_chunks == 1) ? SNGL
                        : (chunks_granted == 0) ? INIT
                        : (is_last ? FIN : CONT);

            tf_err retcode = send_data_ready(req_id, seg_id, (uint32_t)chunk_len, st, mode);
            if (retcode != tf_err::SUCCESS) return retcode; //TODO: Shouldn't autofail when trying to send message

            chunks_granted++;
            outstanding--;

            if (chunks_sent < n_chunks) {
                retcode = send_seg_req(req_id);
                if (retcode != tf_err::SUCCESS) return retcode;
                outstanding++;
                chunks_sent++;
            }
        }
    }
    return tf_err::SUCCESS;
}

static tf_err poll_response_blocking(uint32_t req_id, uint8_t **out_data, size_t *out_len){
    while (true) {
        auto it = g_active_requests.find(req_id);
        if (it != g_active_requests.end() && it->second.done) {
            *out_len  = it->second.result_data.size();
            *out_data = new uint8_t[*out_len];
            memcpy(*out_data, it->second.result_data.data(), *out_len);
            g_active_requests.erase(it);
            return tf_err::SUCCESS;
        }
        if (semop(g_s2c_semid, &wait_op, 1) == -1) {
            if (errno == EINTR) continue;
            return tf_err::E_SEMOP_FAILED;
        }
        drain_and_route();
    }
}

static tf_err poll_response_nonblocking(){
    drain_and_route();
    return tf_err::SUCCESS;
}

// ---------External APIs---------
tf_err TF_init(uint32_t weight){
    if (weight == 0)
        return tf_err::E_INVALID_ARG;
    tf_err retcode = attach_to_region();
    if(retcode != tf_err::SUCCESS){
        cerr<<tf_err_str(retcode)<<endl;
        return retcode;
    }

    retcode = register_client(weight);
    if(retcode != tf_err::SUCCESS){
        cerr<<tf_err_str(retcode)<<endl;
    }
    return retcode;
}
tf_err TF_compress_sync(const uint8_t *data, size_t len, uint8_t **out_compressed, size_t *out_len){
    uint32_t req_id = g_next_req_id++;
    tf_err retcode = send_file(data, len, SYNC, req_id);
    if(retcode != tf_err::SUCCESS){
        cerr<<tf_err_str(retcode)<<endl;
        return retcode;
    }
    retcode = poll_response_blocking(req_id, out_compressed, out_len);
    if(retcode != tf_err::SUCCESS){
        cerr<<tf_err_str(retcode)<<endl;
    }
    return retcode;
}
tf_err TF_compress_async(const uint8_t *data, size_t len, uint64_t *out_req_id){
    uint32_t req_id = g_next_req_id++;
    tf_err retcode = send_file(data, len, ASYNC, req_id);
    if(retcode != tf_err::SUCCESS){
        cerr<<tf_err_str(retcode)<<endl;
    } else {
        g_async_pending++;
    }
    *out_req_id = req_id;
    return retcode;
}
tf_err TF_check_async(TF_result **out_results, size_t *out_count){
    tf_err retcode = poll_response_nonblocking();
    if(retcode != tf_err::SUCCESS){
        cerr<<tf_err_str(retcode)<<endl;
    }

    size_t done_count = 0;
    for (auto &kv : g_active_requests) {
        if (kv.second.done) done_count++;
    }

    if (done_count == 0) {
        *out_results = nullptr;
        *out_count = 0;
        return tf_err::SUCCESS;   
    }
    TF_result *results = new TF_result[done_count];
    size_t idx = 0;

    for (auto it = g_active_requests.begin(); it != g_active_requests.end(); ) {
        if (it->second.done) {
            size_t data_len = it->second.result_data.size();
            results[idx].req_id = it->second.req_id;
            results[idx].len    = data_len;
            results[idx].data   = new uint8_t[data_len];
            memcpy(results[idx].data, it->second.result_data.data(), data_len);
            idx++;
            it = g_active_requests.erase(it);   // erase returns the next valid iterator
        } else {
            ++it;
        }
    }

    if(done_count > g_async_pending) g_async_pending = 0;
    else g_async_pending -= done_count;

    *out_results = results;
    *out_count = done_count;
    return tf_err::SUCCESS;
}

tf_err TF_wait_async(TF_result **out_results, size_t *out_count)
{
    while(true)
    {
        tf_err retcode = TF_check_async(out_results, out_count);
        if(retcode != tf_err::SUCCESS || *out_count > 0 || g_async_pending == 0) return retcode;

        if(semop(g_s2c_semid, &wait_op, 1) == -1)
        {
            if(errno == EINTR) continue;
            cerr<<tf_err_str(tf_err::E_SEMOP_FAILED)<<endl;
            return tf_err::E_SEMOP_FAILED;
        }
    }
}
tf_err TF_cleanup(){
    tf_err retcode = unregister_client();
    if (retcode != tf_err::SUCCESS && retcode != tf_err::E_UNREGISTERED) {
        cerr << tf_err_str(retcode) << endl;
        return retcode;
    }
    retcode = detach_from_region();
    if (retcode != tf_err::SUCCESS) {
        cerr << tf_err_str(retcode) << endl;
    }
    return retcode;
}
void TF_free(uint8_t *data) {
    delete[] data;
}

void TF_free_results(TF_result *results, size_t count) {
    for (size_t i = 0; i < count; i++) {
        delete[] results[i].data;
    }
    delete[] results;
}