#include "include.h"
#include "ringbuffer.h"
using namespace std;

int sem_id, shm_id;
RingBuffer_t *rb;
volatile sig_atomic_t running = 1;

void handle_signal(int sig) {
    running = 0;
}

int create_shared_ringbuffer(){
    size_t ring_bytes = sizeof(RingBuffer_t);
    shm_id = shmget(IPC_PRIVATE, ring_bytes, IPC_CREAT | 0600);
    if (shm_id == -1) { 
        perror("shmget"); 
        exit(1); 
    }
    void *shm = shmat(shm_id, nullptr, 0);
    if (shm == (void *)-1){
        perror("shmat"); 
        exit(1); 
    }
    rb = reinterpret_cast<RingBuffer_t *>(shm);
    memset(shm, 0, ring_bytes);
    rb->head = 0;
    rb->tail = 0;
    rb->capacity = 32;
    sem_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_id == -1) { 
        perror("semget"); 
        exit(1); 
    }
    semctl(sem_id, 0, SETVAL, 0);    //Notification semaphore initially set to zero, producer posts
    ofstream info("/tmp/ring_info.txt");
    info << shm_id << "|" << sem_id << "|" << ring_bytes << "\n";
    info.close();
    return 0;
}
int main(int argc, char* argv[]) {
    create_shared_ringbuffer();
    struct sembuf wait_op = { 0, -1, 0 };

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        perror("sigaction SIGINT");
        exit(1);
    }
    if (sigaction(SIGTERM, &sa, nullptr) == -1) {
        perror("sigaction SIGTERM");
        exit(1);
    }
    
    while(running){
        if(semop(sem_id, &wait_op, 1)==-1){
            if(errno == EINTR) continue;
            perror("semop");
            break;
        }
        RingNode_t node;
        while (rb->pop(&node)){
            cout<<node.segment_id<<"||"<<node.data_len<<"||"<<node.control_field<<endl;
        }
    }
    shmdt(rb);
    shmctl(shm_id, IPC_RMID, nullptr);
    semctl(sem_id, 0, IPC_RMID);
    remove("/tmp/ring_info.txt");
    return 0;
}