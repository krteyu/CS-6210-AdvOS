#ifndef __REGION_H__
#define __REGION_H__
#include <sys/types.h>
#include <cstdint>
#include "ringbuffer.h"
#define MAX_CLIENTS 8
#define MAX_SEGMENTS 5
enum segstatus : uint8_t{
    SEG_FREE=0,
    SEG_GRANTED=1,
    SEG_FILLED=2,
};
typedef segstatus segst_t;

typedef struct client{
    bool active;    
    pid_t pid;
    uint16_t max_inflight;
    uint16_t seg_held;
    uint16_t seg_req;
    int s2c_semid;
}client_t;

typedef struct segment_control{
    segst_t status;     //SEG_FREE, SEG_GRANTED or SEG_FILLED
    uint16_t owner;     //index of owner client
    uint32_t size;      //size of data (from base) that is valid
}segctl_t;

typedef struct shared_region{
    uint32_t n_sms;         //number of shared segments, set upon server spin up
    uint32_t sms_size;      //capacity of each segment, set upon server spin up
    int reg_semid;          //semaphore to lock registration to client table during init
    int doorbell_semid;     //semaphore to notify server of pending request, all clients can post to it

    client_t clients[MAX_CLIENTS];          //list of clients in their assigned indices (aka client table)
    segctl_t segment_control[MAX_SEGMENTS]; //control/status blocks for each segment
    ringbuffer_t c2sbuffer[MAX_CLIENTS];    //client->server ringbuffers per client
    ringbuffer_t s2cbuffer[MAX_CLIENTS];    //server->client ringbuffers per client

    uint8_t seg_pool[];     //pool of actual segments for file transfer (n_sms * sms_size)

}shared_region_t;
#endif