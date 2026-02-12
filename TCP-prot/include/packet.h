#ifndef PACKET_H
#define PACKET_H

#include <cstdint>

struct Packet {
    uint32_t seq;
    uint64_t sent_time; // Microseconds
    bool is_acked;      
    bool is_received;   
};

struct SackHeader {
    uint32_t ack_base;  // Cumulative ACK
    uint64_t sack_mask; // Bitmask of future packets received
};

#endif // PACKET_H
