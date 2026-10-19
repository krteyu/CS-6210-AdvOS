#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__
#include <atomic>
#include <cstdint>
#include "protocols.h"
#include "include.h"
#define RING_MAX_SLOTS 16

typedef struct RingNode {
    uint32_t req_id;
    uint32_t segment_id;
    uint32_t data_len;
    uint8_t  control_field;
}ringnode_t;

typedef struct RingBuffer{
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
    uint32_t capacity;
    ringnode_t slots[RING_MAX_SLOTS];

    bool push(const ringnode_t &node){
        uint32_t h = head.load(std::memory_order_relaxed);
        uint32_t t = tail.load(std::memory_order_acquire);
        if (h-t == capacity){
            // Ring is full
            return false;
        }
        slots[h % capacity] = node;
        head.store(h+1, std::memory_order_release);
        return true;
    }

    bool pop(ringnode_t *node){
        uint32_t t = tail.load(std::memory_order_relaxed);
        uint32_t h = head.load(std::memory_order_acquire);
        if(h == t){
            //Ring is empty
            return false;
        }
        *node = slots[t % capacity];
        tail.store(t+1, std::memory_order_release);
        return true;
    }
}ringbuffer_t;

// ---------Inline Functions---------
inline ringnode_t createMsg(opcode op, status st, transfer_mode tfm, uint32_t req_id, uint32_t segment_id, uint32_t data_len){
    ringnode_t msg;
    uint8_t control_field = build_control(op,st,tfm);
    msg.req_id = req_id;
    msg.segment_id = segment_id;
    msg.data_len = data_len;
    msg.control_field = control_field;
    return msg;
}
inline bool notify_if_needed(int semid) {
    struct sembuf ops[2] = {
        {0, 0, IPC_NOWAIT},  // Require semaphore == 0
        {0, 1, IPC_NOWAIT}   // Set semaphore to 1
    };

    if (semop(semid, ops, 2) == 0) {
        return true; // Posted notification
    }

    if (errno == EAGAIN) {
        return true; // Notification already pending
    }

    perror("notify_if_needed");
    return false;
}
#endif