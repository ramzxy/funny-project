#include <iostream>
#include "protocol_engine.h"

int main() {
    ProtocolEngine endpoint_a;
    ProtocolEngine endpoint_b;

    for (int i = 0; i < 100; ++i) {

        endpoint_a.sender.send_data();

        if (i != 5) {
            SackHeader ack = endpoint_b.receiver.receive_packet(i);

            endpoint_a.sender.process_ack(ack);
        }

        endpoint_a.sender.check_timeouts();

        if (i % 10 == 0) endpoint_a.sender.print_stats();
    }

    return 0;
}
