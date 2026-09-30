#include <atomic>
#include <cstdint>

/*Control Field Structure
    Bits 0-1:   opcode    (0x00-SEG_REQ,  0x01-SEG_GRANT,   0x10-DATA_READY)
    Bits 2-3:   status    (0x00-CONT,     0x01-INIT,      0x10-FIN,      0x11-SNGL)
    Bit 4:      mode      (0x0-SYNC,    0x1-ASYNC)
    Bit 5-8:    reserved   
*/
constexpr uint8_t OPCODE_MASK   = 0x03;  
constexpr uint8_t OPCODE_SHIFT  = 0;    // bits 0-1

constexpr uint8_t STATUS_MASK   = 0x03;
constexpr uint8_t STATUS_SHIFT  = 2;    // bits 2-3

constexpr uint8_t TFMODE_BIT    = 1<<4; //bit 5


enum opcode : uint8_t{
    SEG_REQ = 0,    //Client->Server: requesting a segment for data to be written into
    SEG_GRANT = 1,  //Server->Client: segment granted
    DATA_READY = 2, //Client->Server: Data ready for consumption
};

enum status: uint8_t{
    CONT = 0,   //Neither first nor last request
    INIT = 1,   //First message of a request
    FIN = 2,    //Last message of a request
    SNGL = 3,   //Only message (first and last) of a request
};

enum transfer_mode: uint8_t{
    SYNC=0,
    ASYNC=1,
};

inline uint8_t build_control(opcode op, status st, transfer_mode tfm){
    uint8_t ctl = 0;
    ctl|= (static_cast<uint8_t>(op) & OPCODE_MASK) << OPCODE_SHIFT;
    ctl|= (static_cast<uint8_t>(st) & STATUS_MASK) << STATUS_SHIFT;
    if(tfm) ctl|= TFMODE_BIT;
    return ctl;
}

inline opcode get_opcode(uint8_t ctl){
    return static_cast<opcode>((ctl >> OPCODE_SHIFT) & OPCODE_MASK);
}

inline status get_status(uint8_t ctl){
    return static_cast<status>((ctl >> STATUS_SHIFT) & STATUS_MASK);
}

inline transfer_mode get_transfer_mode(uint8_t ctl){
    return (ctl & TFMODE_BIT)? ASYNC: SYNC;
}

#define RING_MAX_SLOTS 128
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
}RingBuffer_t;
