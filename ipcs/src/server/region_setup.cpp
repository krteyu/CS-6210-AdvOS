#include "region_setup.h"
#include "shm_common.h"
#include "include.h"

static const char *INFO_FILE_PATH = "/tmp/tinyfile_server_info.txt";

tf_err create_and_init_region(uint32_t n_sms, uint32_t sms_size, int *out_shm_id, shared_region_t **out_region){
    if (n_sms > MAX_SEGMENTS){
        return tf_err::E_INVALID_ARG;
    }
    size_t total_size = region_total_size(n_sms, sms_size);

    int shm_id = shmget(IPC_PRIVATE, total_size, IPC_CREAT | 0600);
    if (shm_id == -1) {
        return tf_err::E_SHMGET_FAILED;
    }
    void *shm = shmat(shm_id, nullptr, 0);
    if(shm == (void *)-1){
        shmctl(shm_id, IPC_RMID, nullptr);
        return tf_err::E_SHMAT_FAILED;
    }
    shared_region_t *region = reinterpret_cast<shared_region_t*>(shm);
    memset(shm, 0, total_size);
    region->n_sms = n_sms;
    region->sms_size = sms_size;

    //CREATE ALL SEGMENT CONTROLS
    for(uint32_t i=0; i<n_sms; i++){
        region->segment_control[i].status = SEG_FREE;
        region->segment_control[i].owner  = 0;      
        region->segment_control[i].size   = 0;
    }

    //CREATE CLIENT TABLE AND INITIALIZE RINGBUFFERS
    for(int i=0; i<MAX_CLIENTS; i++){
        region->clients[i].active = false;
        region->clients[i].pid = 0;
        region->clients[i].max_inflight = 0;
        region->clients[i].seg_held = 0;
        region->clients[i].seg_req = 0;
        region->clients[i].s2c_semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
        if(region->clients[i].s2c_semid == -1){
            destroy_region(region, shm_id);
            return tf_err::E_SEMGET_FAILED;
        }
        semctl(region->clients[i].s2c_semid, 0, SETVAL, 0);   //counting semaphore, starts at 0
        region->c2sbuffer[i].capacity = RING_MAX_SLOTS;
        region->s2cbuffer[i].capacity = RING_MAX_SLOTS;
    }

    region->reg_semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (region->reg_semid == -1){
        destroy_region(region, shm_id);
        return tf_err::E_SEMGET_FAILED;
    }
    semctl(region->reg_semid, 0, SETVAL, 1);   // Initialize semaphore to 1, acts as a mutex

    region->doorbell_semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (region->doorbell_semid == -1){
        destroy_region(region, shm_id);
        return tf_err::E_SEMGET_FAILED;
    }
    semctl(region->doorbell_semid, 0, SETVAL, 0);   //counting semaphore, starts at 0

    ServerInfo info{ shm_id };
    tf_err write_result = write_server_info(INFO_FILE_PATH, info);
    if (write_result != tf_err::SUCCESS){
        destroy_region(region, shm_id);
        return write_result;
    } 
    *out_region = region;
    *out_shm_id = shm_id;
    return tf_err::SUCCESS;
}

void destroy_region(shared_region_t *region, int shm_id){
    if (region == nullptr) return;  
    // Free every per-client response semaphore
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (region->clients[i].s2c_semid > 0) {
            semctl(region->clients[i].s2c_semid, 0, IPC_RMID);
        }
    }

    if (region->reg_semid > 0) {
        semctl(region->reg_semid, 0, IPC_RMID);
    }
    if (region->doorbell_semid > 0) {
        semctl(region->doorbell_semid, 0, IPC_RMID);
    }

    shmdt(region);
    shmctl(shm_id, IPC_RMID, nullptr);

    remove(INFO_FILE_PATH);
}