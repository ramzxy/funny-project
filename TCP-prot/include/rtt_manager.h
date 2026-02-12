#ifndef RTT_MANAGER_H
#define RTT_MANAGER_H

#include <cstdint>
#include <cmath>

class RTTManager {
    double estimated_rtt_us = 100000.0; // Start 100ms
    double dev_rtt_us = 0.0;
public:
    void update(uint64_t send_time, uint64_t ack_time) {
        double sample = (double)(ack_time - send_time);
        if (sample < 0) return;

        double error = std::abs(sample - estimated_rtt_us);
        dev_rtt_us = (0.75 * dev_rtt_us) + (0.25 * error);
        estimated_rtt_us = (0.875 * estimated_rtt_us) + (0.125 * sample);
    }

    uint64_t get_rto() const {
        return (uint64_t)(estimated_rtt_us + (4 * dev_rtt_us));
    }
};

#endif // RTT_MANAGER_H
