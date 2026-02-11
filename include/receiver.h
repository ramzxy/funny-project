#ifndef RECEIVER_H
#define RECEIVER_H

#include <vector>
#include <cstdint>
#include "constants.h"
#include "packet.h"

class Receiver {
private:
    std::vector<Packet>& buffer;
    uint32_t recv_expected = 0;

public:
    Receiver(std::vector<Packet>& buf);

    SackHeader receive_packet(uint32_t seq_num);
};

#endif // RECEIVER_H
