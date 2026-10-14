#include "include.h"
#include "regions.h"
#include "region_setup.h"
#include "ringbuffer.h"
#include "shm_common.h"
#include <map>
#include <deque>
#include <vector>
#include <utility>
#include <algorithm>
extern "C" {
#include "snappy.h"
}
using namespace std;

// ---------Structures---------
typedef struct IncomingRequest {
    int      client_id;
    uint32_t req_id;
    transfer_mode mode;
    vector<uint8_t> data;     // appended to as DATA_READY chunks arrive from the client
    bool     complete;        // true once FIN/SNGL chunk received
}in_req_t;

typedef struct OutgoingRequest {
    int      client_id;
    uint32_t req_id;
    transfer_mode mode;
    vector<uint8_t> data;      // the compressed bytes
    size_t   offset_sent;      // how much has been pushed as DATA_READY chunks so far
}out_req_t;

// ---------Global Variables---------
shared_region_t *g_region = nullptr;
int g_shm_id = -1;
volatile sig_atomic_t running = 1;

using req_key_t = pair<int, uint32_t>;
static map<req_key_t, in_req_t> g_incoming;     //maps (client_id,req_id) -> local data/metadata buffer
static deque<req_key_t> g_compression_queue;    //FIFO for compression
static map<req_key_t, out_req_t> g_outgoing;    //maps (client_id,req_id) -> local compressed output buffer
static deque<req_key_t> g_send_queue;           //FIFO for outgoing requests
static deque<uint32_t> g_pending_seg_req[MAX_CLIENTS];

static struct sembuf post_op = { 0, +1, 0 };
static struct sembuf wait_op = { 0, -1, 0 };

// ---------Helper Functions---------
static tf_err parse_rings();
static void grant_segments();
static void process_compression_queue(snappy_env& env);
static void process_send_queue();
static int32_t find_free_segment();
static bool has_free_segment();
static tf_err notify_client(int client_id);
static void handle_signal(int sig);

// ---------Small inline helpers---------

// ---------Definitions---------
void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static int32_t find_free_segment(){
    for(uint32_t i = 0; i<g_region->n_sms; i++){
        if(g_region->segment_control[i].status == SEG_FREE){
            g_region->segment_control[i].status = SEG_GRANTED;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// similar to find_free_segment, but just checks it doesn't take
static bool has_free_segment(){
    for(uint32_t i = 0; i<g_region->n_sms; i++){
        if(g_region->segment_control[i].status == SEG_FREE) return true;
    }
    return false;
}

static tf_err notify_client(int client_id){
    if(semop(g_region->clients[client_id].s2c_semid, &post_op, 1) == -1){
        return tf_err::E_SEMOP_FAILED;
    }
    return tf_err::SUCCESS;
}

static tf_err parse_rings(){
    for(int c = 0; c<MAX_CLIENTS; c++){
        if (!g_region->clients[c].active) continue;

        ringbuffer_t *ring = &g_region->c2sbuffer[c];
        ringnode_t msg;

        while(ring->pop(&msg)){
            opcode op = get_opcode(msg.control_field);
            req_key_t key = { c, msg.req_id };
            switch(op){
                case SEG_REQ: {
                    if(g_region->clients[c].seg_req >= g_region->clients[c].max_inflight) break;    //Reject SEG_REQ from clients that have max_inflight requests in queue
                    g_pending_seg_req[c].push_back(msg.req_id);
                    g_region->clients[c].seg_req++;
                    break;
                }
                case DATA_READY:{
                    status st = get_status(msg.control_field);
                    in_req_t &ir = g_incoming[key];
                    if(st == INIT || st == SNGL){
                        ir.client_id = c;
                        ir.req_id = msg.req_id;
                        ir.mode = get_transfer_mode(msg.control_field);
                    }
                    uint8_t *src = segment_ptr(g_region, msg.segment_id);
                    ir.data.insert(ir.data.end(), src, src + msg.data_len);

                    g_region->segment_control[msg.segment_id].status = SEG_FREE;
                    g_region->segment_control[msg.segment_id].owner  = 0;
                    g_region->clients[c].seg_held--;

                    if (st == FIN || st == SNGL) {
                        ir.complete = true;
                        g_compression_queue.push_back(key);
                    }
                    break;

                }
                case SEG_RELEASE:{
                    g_region->segment_control[msg.segment_id].status = SEG_FREE;
                    g_region->segment_control[msg.segment_id].owner  = 0;
                    break;
                }
                default:
                    break;
            }
        }
    }
    return tf_err::SUCCESS;
}

static void grant_segments(){
    for (int c = 0; c < MAX_CLIENTS; c++){
        if (!g_region->clients[c].active) continue;
        while(!g_pending_seg_req[c].empty() && g_region->clients[c].seg_held < g_region->clients[c].max_inflight){
            int32_t seg = find_free_segment();
            if (seg < 0) return;        //Segment pool exhausted, keep waiting for segment relinquishes
            uint32_t req_id = g_pending_seg_req[c].front();
            g_pending_seg_req[c].pop_front();
            g_region->clients[c].seg_req--;
            g_region->clients[c].seg_held++;
            g_region->segment_control[seg].owner  = static_cast<uint16_t>(c);
            ringnode_t grant_msg = createMsg(SEG_GRANT, SNGL, SYNC, req_id, static_cast<uint32_t>(seg), 0);
            g_region->s2cbuffer[c].push(grant_msg);
            notify_client(c);
        }
    }
}
//TODO: Add snappy integration, currently the "compressed" file is just 25% of the original data
static void process_compression_queue(snappy_env& env){
    if (g_compression_queue.empty()) return;

    req_key_t key = g_compression_queue.front();
    g_compression_queue.pop_front();

    auto it = g_incoming.find(key);
    if (it == g_incoming.end()) return;

    out_req_t out;
    out.client_id = it->second.client_id;
    out.req_id = it->second.req_id;
    out.mode = it->second.mode;
    out.offset_sent = 0;

    const vector<uint8_t>& input = it->second.data;
    size_t max_len = snappy_max_compressed_length(it->second.data.size());
    out.data.resize(max_len);
    size_t compressed_len = 0;
    int rc = snappy_compress(&env,
                            reinterpret_cast<const char*>(input.data()),
                            input.size(),
                            reinterpret_cast<char*>(out.data.data()),
                            &compressed_len);
    if(rc != 0){
        fprintf(stderr, "Snappy compression failed: %d\n", rc);
        running = 0;
        return;
    }
    out.data.resize(compressed_len);

    g_outgoing[key] = move(out);
    g_send_queue.push_back(key);
    g_incoming.erase(it);
}

static void process_send_queue(){
    while (!g_send_queue.empty()) {
        req_key_t key = g_send_queue.front();

        auto it = g_outgoing.find(key);
        if (it == g_outgoing.end()) {
            // nothing to send for this key anymore -- drop it and move on
            g_send_queue.pop_front();
            continue;
        }
        out_req_t &out = it->second;

        int32_t seg = find_free_segment();
        if (seg < 0) return;   // pool exhausted right now -- try again on a later pass

        size_t total_len   = out.data.size();
        size_t remaining    = total_len - out.offset_sent;
        size_t chunk_len    = std::min(remaining, (size_t)g_region->sms_size);

        uint8_t *dst = segment_ptr(g_region, static_cast<uint32_t>(seg));
        memcpy(dst, out.data.data() + out.offset_sent, chunk_len);
        g_region->segment_control[seg].status = SEG_FILLED;
        g_region->segment_control[seg].owner = static_cast<uint16_t>(out.client_id);

        bool is_first = (out.offset_sent == 0);
        out.offset_sent += chunk_len;
        bool is_last = (out.offset_sent >= total_len);

        status st;
        if (is_first && is_last) {
            st = SNGL;
        } else if (is_first) {
            st = INIT;
        } else if (is_last) {
            st = FIN;
        } else {
            st = CONT;
        }

        ringnode_t data_msg = createMsg(DATA_READY, st, out.mode, out.req_id, static_cast<uint32_t>(seg), static_cast<uint32_t>(chunk_len));
        g_region->s2cbuffer[out.client_id].push(data_msg);
        notify_client(out.client_id);

        if (is_last) {
            g_outgoing.erase(it);
            g_send_queue.pop_front();
        } else {
            // more chunks remain for this request -- stop here rather than writing
            // ahead into a second segment before the client has read/released this one
            break;
        }
    }
}

int main(int argc, char *argv[]){
    uint32_t n_sms = 0;
    uint32_t sms_size = 0;

    // Argument parsing
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--n_sms") == 0 && i + 1 < argc) {
            n_sms = static_cast<uint32_t>(atoi(argv[++i]));
        } 
        else if (strcmp(argv[i], "--sms_size") == 0 && i + 1 < argc) {
            sms_size = static_cast<uint32_t>(atoi(argv[++i]));
        } 
        else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            fprintf(stderr, "Usage: %s --n_sms <N> --sms_size <bytes>\n", argv[0]);
            return 1;
        }
    }

    if (n_sms == 0 || sms_size == 0) {
        fprintf(stderr, "Both --n_sms and --sms_size are required and must be > 0\n");
        return 1;
    }

    //Region setup
    tf_err retcode = create_and_init_region(n_sms, sms_size, &g_shm_id, &g_region);
    if(retcode!=tf_err::SUCCESS){
        fprintf(stderr, "create_and_init_region failed: %s\n", tf_err_str(retcode));
        return 1;
    }

    //signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        perror("sigaction SIGINT");
        destroy_region(g_region, g_shm_id);
        return 1;
    }
    if (sigaction(SIGTERM, &sa, nullptr) == -1) {
        perror("sigaction SIGTERM");
        destroy_region(g_region, g_shm_id);
        return 1;
    }
    snappy_env env{};

    if (snappy_init_env(&env) != 0) {
        fprintf(stderr, "Snappy initialization failed\n");
        destroy_region(g_region, g_shm_id);
        return 1;
    }

    printf("TINYFILE_READY\n");
    fflush(stdout);

    struct sembuf wait_op = { 0, -1, 0 };
    while (running) {
        bool did_work = false;

        if (!g_compression_queue.empty()) {
            process_compression_queue(env);
            did_work = true;
        }
        if (!g_send_queue.empty() && has_free_segment()) {
            process_send_queue();
            did_work = true;
        }

        if (!did_work) {
            if (semop(g_region->doorbell_semid, &wait_op, 1) == -1) {
                if (errno == EINTR) continue;
                perror("semop doorbell");
                break;
            }
        } else {
            // drain any accumulated notifications without blocking, so new
            // ring traffic gets routed before the next pass
            struct sembuf trywait = { 0, -1, IPC_NOWAIT };
            while (semop(g_region->doorbell_semid, &trywait, 1) == 0) { /* drain */ }
        }

        parse_rings();
        grant_segments();
    }

    //Cleanup
    snappy_free_env(&env);
    destroy_region(g_region, g_shm_id);
    return 0;
}