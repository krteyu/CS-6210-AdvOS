#ifndef __PROTOCOLS_H__
#define __PROTOCOLS_H__
#include <cstdint> 
/*Control Field Structure
    Bits 0-1:   opcode    (0x00-SEG_REQ,  0x01-SEG_GRANT,   0x10-DATA_READY,  0x11-SEG_RELEASE)
    Bits 2-3:   status    (0x00-CONT,     0x01-INIT,      0x10-FIN,      0x11-SNGL)
    Bit 4:      mode      (0x0-SYNC,    0x1-ASYNC)
    Bit 5-8:    reserved   
*/
constexpr uint8_t OPCODE_MASK   = 0x03;  
constexpr uint8_t OPCODE_SHIFT  = 0;    // bits 0-1

constexpr uint8_t STATUS_MASK   = 0x03;
constexpr uint8_t STATUS_SHIFT  = 2;    // bits 2-3

constexpr uint8_t TFMODE_BIT    = 1<<4; //bit 5

enum class tf_err : uint8_t{
    SUCCESS = 0,
    E_FILE_NOT_FOUND,
    E_SHMGET_FAILED,
    E_SHMAT_FAILED,
    E_SEMGET_FAILED,
    E_RING_FULL,
    E_RING_EMPTY,
    E_NO_FREE_SLOT,
    E_NO_FREE_SEGMENT,
    E_SEMOP_FAILED,
    E_INVALID_ARG,
    E_CLIENT_TABLE_FULL,
    E_INITIALIZED,
    E_UNINITIALIZED,
    E_UNREGISTERED,
    E_REGISTERED,
};

inline const char *tf_err_str(tf_err e) {
    switch (e) {
        case tf_err::SUCCESS:               return "SUCCESS";
        case tf_err::E_FILE_NOT_FOUND:      return "FILE_NOT_FOUND";
        case tf_err::E_SHMGET_FAILED:       return "SHMGET_FAILED";
        case tf_err::E_SHMAT_FAILED:        return "SHMAT_FAILED";
        case tf_err::E_SEMGET_FAILED:       return "SEMGET_FAILED";
        case tf_err::E_RING_FULL:           return "RING_FULL";
        case tf_err::E_RING_EMPTY:          return "RING_EMPTY";
        case tf_err::E_NO_FREE_SLOT:        return "NO_FREE_SLOT";
        case tf_err::E_NO_FREE_SEGMENT:     return "NO_FREE_SEGMENT";
        case tf_err::E_SEMOP_FAILED:        return "SEMOP_FAILED";
        case tf_err::E_INVALID_ARG:         return "INVALID_ARG";
        case tf_err::E_CLIENT_TABLE_FULL:   return "CLIENT_TABLE_FULL";
        case tf_err::E_INITIALIZED:         return "INITIALIZED";
        case tf_err::E_UNINITIALIZED:       return "UNINITIALIZED";
        case tf_err::E_REGISTERED:          return "REGISTERED";
        case tf_err::E_UNREGISTERED:        return "UNREGISTERED";
        default:                            return "UNKNOWN";
    }
}

enum opcode : uint8_t{
    SEG_REQ = 0,    //Client->Server: requesting a segment for data to be written into
    SEG_GRANT = 1,  //Server->Client: segment granted
    DATA_READY = 2, //Client->Server: Data ready for consumption
    SEG_RELEASE = 3,//Client->Server: done reading segment, so the server is ready to send another segment
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
#endif