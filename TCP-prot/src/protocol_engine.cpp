#include "protocol_engine.h"

ProtocolEngine::ProtocolEngine()
    : buffer(BUFFER_SIZE), sender(buffer), receiver(buffer) {
    for (auto& p : buffer) { p.is_acked = false; p.is_received = false; }
}
