#include "include.h"
#include "regions.h"
#include "region_setup.h"

shared_region_t *region = nullptr;
int shm_id = -1;
volatile sig_atomic_t running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
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
    tf_err retcode = create_and_init_region(n_sms, sms_size, &shm_id, &region);
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
        destroy_region(region, shm_id);
        return 1;
    }
    if (sigaction(SIGTERM, &sa, nullptr) == -1) {
        perror("sigaction SIGTERM");
        destroy_region(region, shm_id);
        return 1;
    }

    printf("TINYFILE_READY\n");
    fflush(stdout);

    struct sembuf wait_op = { 0, -1, 0 };
    while (running) {
        if (semop(region->doorbell_semid, &wait_op, 1) == -1) {
            if (errno == EINTR) continue;
            perror("semop doorbell");
            break;
        }

        // TODO: scan client_table for active clients with pending c2sbuffer messages,
        // dispatch SEG_REQ / DATA_READY handling, run the scheduler, etc.
        // This is the next slice of work -- registration + message dispatch.
    }

    //Cleanup
    destroy_region(region, shm_id);
    return 0;
}