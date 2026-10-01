#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__
#include <atomic>
#include <cstdint>
#define RING_MAX_SLOTS 16

typedef struct RingNode {
    uint32_t segment_id;
    uint32_t data_len;
    uint8_t  control_field;
}RingNode_t;

typedef struct RingBuffer{
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
    uint32_t capacity;
    RingNode_t slots[RING_MAX_SLOTS];

    bool push(const RingNode_t &node){
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

    bool pop(RingNode_t *node){
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
#endif