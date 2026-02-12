#ifndef CONGESTION_CONTROL_H
#define CONGESTION_CONTROL_H

#include <cstdint>
#include <cmath>
#include <algorithm>
#include "constants.h"

class CongestionControl {
public:
    double cwnd = 10.0;          // Current Window
    double ssthresh = 1000.0;    // Slow Start Threshold
    double w_max = 0.0;          // Window size at last loss
    uint64_t last_congestion_time = 0; 

    void on_ack(uint32_t acked_count, uint64_t now_us) {
        if (cwnd < ssthresh) {
            cwnd += acked_count;
            return;
        }
        double t_sec = (now_us - last_congestion_time) / 1000000.0;
        
        double K = std::cbrt(w_max * (1.0 - CUBIC_BETA) / CUBIC_C);
        
        double target = (CUBIC_C * std::pow(t_sec - K, 3)) + w_max;
        
        double increment = (target - cwnd) / cwnd; 
        cwnd += std::max(0.0, increment);
    }

    void on_loss(uint64_t now_us) {
        w_max = cwnd;
        if (w_max < 2.0) w_max = 2.0;
        
        cwnd = cwnd * CUBIC_BETA;
        ssthresh = cwnd;
        last_congestion_time = now_us;
        
        if (cwnd < 2.0) cwnd = 2.0;
    }
};

#endif // CONGESTION_CONTROL_H
