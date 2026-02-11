#include "sender.h"
#include <iostream>
#include <chrono>

uint64_t Sender::now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

Sender::Sender(std::vector<Packet>& buf) : buffer(buf) {}

void Sender::send_data() {
    uint32_t effective_window = (uint32_t)cc.cwnd;

    while (next_seq < send_base + effective_window) {
        uint32_t idx = next_seq & BUFFER_MASK;

        Packet& p = buffer[idx];
        p.seq = next_seq;
        p.sent_time = now();
        p.is_acked = false;

        next_seq++;
    }
}

void Sender::process_ack(SackHeader sack) {
    uint64_t current_time = now();
    int acked_count = 0;

    while (send_base < sack.ack_base) {
        uint32_t idx = send_base & BUFFER_MASK;
        if (!buffer[idx].is_acked) {
            rtt_mgr.update(buffer[idx].sent_time, current_time);
            acked_count++;
        }
        send_base++;
    }

    for (int i = 0; i < 64; ++i) {
        if ((sack.sack_mask >> i) & 1) {
            uint32_t sack_seq = sack.ack_base + 1 + i;
            uint32_t idx = sack_seq & BUFFER_MASK;

            if (!buffer[idx].is_acked) {
               buffer[idx].is_acked = true;
               acked_count++;
            }
        }
    }

    if (acked_count > 0) {
        cc.on_ack(acked_count, current_time);
    }
}

void Sender::check_timeouts() {
    uint64_t current_time = now();
    uint64_t rto = rtt_mgr.get_rto();
    bool loss_detected = false;

    for (uint32_t s = send_base; s < next_seq; ++s) {
        uint32_t idx = s & BUFFER_MASK;
        Packet& p = buffer[idx];

        if (!p.is_acked && (current_time - p.sent_time > rto)) {
            p.sent_time = current_time;
            loss_detected = true;
        }
    }

    if (loss_detected) {
        cc.on_loss(current_time);
    }
}

void Sender::print_stats() {
    std::cout << "CWND: " << cc.cwnd
              << " | RTT: " << rtt_mgr.get_rto() / 1000.0 << "ms"
              << " | Base: " << send_base
              << " | Next: " << next_seq << std::endl;
}
