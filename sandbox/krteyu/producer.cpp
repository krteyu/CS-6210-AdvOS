#include "include.h"
#include "ringbuffer.h"
using namespace std;

int main(int /*argc*/, char* /*argv*/[]) {
    ifstream info("/tmp/ring_info.txt");
    if (!info){
        perror("open ring_info.txt"); 
        exit(1);
    }
    int shm_id, sem_id;
    size_t ring_bytes;
    char delim;
    info>>shm_id>>delim>>sem_id>>delim>>ring_bytes;
    info.close();

    void *shm = shmat(shm_id, nullptr, 0);
    if (shm == (void *)-1) { 
        perror("shmat"); 
        exit(1); 
    }
    ringbuffer_t *rb = reinterpret_cast<ringbuffer_t*>(shm);
    struct sembuf post_op = { 0, +1, 0 };
    for(int i=0;i<10;i++){
        RingNode_t node;
        node.segment_id = i;
        node.data_len = 0;
        std::atomic_ref<uint8_t> ctl_ref(node.control_field);
        if(i%2==0){
            node.control_field = build_control(SEG_REQ, SNGL, SYNC);
        }
        else{
            if(i==1)
                node.control_field = build_control(DATA_READY, INIT, SYNC);
            else if(i==9)
                node.control_field = build_control(DATA_READY, FIN, SYNC);
            else
                node.control_field = build_control(DATA_READY, CONT, SYNC);
        }
        
        if(!rb->push(node)){
            cerr << "ring full\n";
            shmdt(rb);
            exit(1);
        }
        if (semop(sem_id, &post_op, 1) == -1) { 
            perror("semop"); 
            exit(1); 
        }
    }
    shmdt(rb);
    return 0;
}