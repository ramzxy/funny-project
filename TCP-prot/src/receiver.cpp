#include "receiver.h"

Receiver::Receiver(std::vector<Packet>& buf) : buffer(buf) {}

SackHeader Receiver::receive_packet(uint32_t seq_num) {
    uint32_t idx = seq_num & BUFFER_MASK;
    buffer[idx].is_received = true;

    while (buffer[recv_expected & BUFFER_MASK].is_received) {
        buffer[recv_expected & BUFFER_MASK].is_received = false;
        recv_expected++;
    }

    SackHeader header;
    header.ack_base = recv_expected;
    header.sack_mask = 0;
    for (int i = 0; i < 64; ++i) {
        uint32_t check_seq = recv_expected + 1 + i;
        if (buffer[check_seq & BUFFER_MASK].is_received) {
            header.sack_mask |= (1ULL << i);
        }
    }

    return header;
}
