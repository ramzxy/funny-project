#ifndef PROTOCOL_ENGINE_H
#define PROTOCOL_ENGINE_H

#include <vector>
#include "constants.h"
#include "packet.h"
#include "sender.h"
#include "receiver.h"

class ProtocolEngine {
private:
    std::vector<Packet> buffer;

public:
    Sender sender;
    Receiver receiver;

    ProtocolEngine();
};

#endif // PROTOCOL_ENGINE_H
