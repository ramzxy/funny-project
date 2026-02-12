#ifndef SENDER_H
#define SENDER_H

#include <vector>
#include <cstdint>
#include "constants.h"
#include "packet.h"
#include "rtt_manager.h"
#include "congestion_control.h"

class Sender {
private:
    std::vector<Packet>& buffer;

    uint32_t send_base = 0;
    uint32_t next_seq = 0;

    RTTManager rtt_mgr;
    CongestionControl cc;

    uint64_t now();

public:
    Sender(std::vector<Packet>& buf);

    void send_data();
    void process_ack(SackHeader sack);
    void check_timeouts();
    void print_stats();
};

#endif // SENDER_H
