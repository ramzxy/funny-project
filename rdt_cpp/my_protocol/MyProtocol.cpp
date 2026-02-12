/**
 * MyProtocol.cpp
 *
 *   Version: 2016-02-11
 *    Author: Jaco ter Braak & Frans van Dijk, University of Twente.
 * Copyright: University of Twente, 2015-2025
 *
 **************************************************************************
 *                          = Copyright notice =                          *
 *                                                                        *
 *             This file may ONLY be distributed  UNMODIFIED              *
 * In particular, a correct solution to the challenge must NOT  be posted *
 * in public places, to preserve the learning effect for future students. *
 **************************************************************************
 */

#include "MyProtocol.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace my_protocol {

// ─────────────────── Helpers ───────────────────

int64_t MyProtocol::nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ── Packet builders (with XOR checksum over header bytes 1..4) ──

std::vector<int32_t>
MyProtocol::buildDataPacket(uint32_t seq, uint32_t total,
                            const std::vector<int32_t> &fileData,
                            uint32_t offset, uint32_t len) {
  std::vector<int32_t> pkt(DATA_HEADER + len);
  pkt[0] = TYPE_DATA;
  pkt[1] = (seq >> 8) & 0xFF;
  pkt[2] = seq & 0xFF;
  pkt[3] = (total >> 8) & 0xFF;
  pkt[4] = total & 0xFF;
  pkt[5] = (pkt[1] ^ pkt[2] ^ pkt[3] ^ pkt[4]) & 0xFF;
  for (uint32_t i = 0; i < len; i++) {
    pkt[DATA_HEADER + i] = fileData[offset + i] & 0xFF;
  }
  return pkt;
}

std::vector<int32_t> MyProtocol::buildAckPacket(uint32_t ackBase,
                                                uint16_t sackMask) {
  std::vector<int32_t> pkt(ACK_HEADER);
  pkt[0] = TYPE_ACK;
  pkt[1] = (ackBase >> 8) & 0xFF;
  pkt[2] = ackBase & 0xFF;
  pkt[3] = (sackMask >> 8) & 0xFF;
  pkt[4] = sackMask & 0xFF;
  pkt[5] = (pkt[1] ^ pkt[2] ^ pkt[3] ^ pkt[4]) & 0xFF;
  return pkt;
}

// ── Parsers + checksum verification ──

uint32_t MyProtocol::parseSeq(const std::vector<int32_t> &pkt) {
  return ((pkt[1] & 0xFF) << 8) | (pkt[2] & 0xFF);
}

uint32_t MyProtocol::parseTotalPkts(const std::vector<int32_t> &pkt) {
  return ((pkt[3] & 0xFF) << 8) | (pkt[4] & 0xFF);
}

bool MyProtocol::verifyDataChecksum(const std::vector<int32_t> &pkt) {
  uint8_t expected = (pkt[1] ^ pkt[2] ^ pkt[3] ^ pkt[4]) & 0xFF;
  return (pkt[5] & 0xFF) == expected;
}

bool MyProtocol::verifyAckChecksum(const std::vector<int32_t> &pkt) {
  uint8_t expected = (pkt[1] ^ pkt[2] ^ pkt[3] ^ pkt[4]) & 0xFF;
  return (pkt[5] & 0xFF) == expected;
}

// ─────────────────── Constructor / Destructor ───────────────────

MyProtocol::MyProtocol() { this->networkLayer = NULL; }

MyProtocol::~MyProtocol() {}

void MyProtocol::setStop() { this->stop = true; }

// ─────────────────── SENDER ───────────────────
//
// Dead-simple sliding window. No framework timers. No threads. No mutex.
// One loop: receive ACKs → retransmit stale → send new → sleep 1ms.
// XOR checksum on ACKs prevents corrupted ACKs from causing data loss.

void MyProtocol::sender() {
  std::cout << "Sending..." << std::endl;

  std::vector<int32_t> fileContents = framework::getFileContents(fileID);
  uint32_t fileSize = (uint32_t)fileContents.size();

  totalPkts = (fileSize + DATASIZE - 1) / DATASIZE;
  if (totalPkts == 0)
    totalPkts = 1;
  std::cout << "Total packets: " << totalPkts << std::endl;

  packetBuffer.resize(totalPkts);
  acked.resize(totalPkts, false);
  sentTime.resize(totalPkts, 0);

  for (uint32_t i = 0; i < totalPkts; i++) {
    uint32_t off = i * DATASIZE;
    uint32_t len = std::min((uint32_t)DATASIZE, fileSize - off);
    packetBuffer[i] = buildDataPacket(i, totalPkts, fileContents, off, len);
  }

  sendBase = 0;
  nextSeq = 0;

  while (!stop && sendBase < totalPkts) {
    int64_t now = nowMs();

    // ── Phase 1: Process all pending ACKs ──
    std::vector<int32_t> pkt;
    while (networkLayer->receivePacket(&pkt)) {
      if (pkt.size() < ACK_HEADER || (pkt[0] & 0xFF) != TYPE_ACK)
        continue;
      if (!verifyAckChecksum(pkt))
        continue;

      uint32_t ab = ((pkt[1] & 0xFF) << 8) | (pkt[2] & 0xFF);
      uint16_t sm = ((pkt[3] & 0xFF) << 8) | (pkt[4] & 0xFF);

      // Validate range
      if (ab > nextSeq || ab > totalPkts)
        continue;

      // Cumulative ACK
      while (sendBase < ab) {
        acked[sendBase] = true;
        sendBase++;
      }

      // SACK
      for (int i = 0; i < 16; i++) {
        if ((sm >> i) & 1) {
          uint32_t s = ab + i;
          if (s < nextSeq && s < totalPkts)
            acked[s] = true;
        }
      }
    }

    if (sendBase >= totalPkts)
      break;

    // ── Phase 2: Count in-flight + retransmit timed-out ──
    uint32_t inFlight = 0;
    for (uint32_t i = sendBase; i < nextSeq && i < totalPkts; i++) {
      if (!acked[i]) {
        inFlight++;
        if (sentTime[i] > 0 && (now - sentTime[i]) > TIMEOUT_MS) {
          networkLayer->sendPacket(packetBuffer[i]);
          sentTime[i] = now;
        }
      }
    }

    // ── Phase 3: Send new packets to fill window ──
    while (nextSeq < totalPkts && inFlight < WINDOW) {
      networkLayer->sendPacket(packetBuffer[nextSeq]);
      sentTime[nextSeq] = now;
      nextSeq++;
      inFlight++;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::cout << "Sender finished." << std::endl;
}

// ─────────────────── RECEIVER ───────────────────

std::vector<int32_t> MyProtocol::receiver() {
  std::cout << "Receiving..." << std::endl;

  uint32_t expectedTotal = 0;
  uint32_t recvExpected = 0;
  std::vector<std::vector<int32_t>> recvBuffer;
  std::vector<bool> received;
  int64_t lastRecvTime = nowMs();
  std::vector<int32_t> lastAck;

  while (true) {
    std::vector<int32_t> packet;

    if (networkLayer->receivePacket(&packet)) {
      if (packet.size() < DATA_HEADER || (packet[0] & 0xFF) != TYPE_DATA)
        continue;
      if (!verifyDataChecksum(packet))
        continue;

      uint32_t seq = parseSeq(packet);
      uint32_t total = parseTotalPkts(packet);

      if (expectedTotal == 0) {
        expectedTotal = total;
        recvBuffer.resize(expectedTotal);
        received.resize(expectedTotal, false);
        std::cout << "Expecting " << expectedTotal << " packets." << std::endl;
      }

      if (total != expectedTotal)
        continue; // corrupted totalPkts

      if (seq < expectedTotal && !received[seq]) {
        std::vector<int32_t> payload(packet.begin() + DATA_HEADER,
                                     packet.end());
        recvBuffer[seq] = payload;
        received[seq] = true;
      }

      while (recvExpected < expectedTotal && received[recvExpected]) {
        recvExpected++;
      }

      uint16_t sackMask = 0;
      for (int i = 0; i < 16; i++) {
        uint32_t checkSeq = recvExpected + i;
        if (checkSeq < expectedTotal && received[checkSeq]) {
          sackMask |= (1 << i);
        }
      }

      lastAck = buildAckPacket(recvExpected, sackMask);
      networkLayer->sendPacket(lastAck);
      lastRecvTime = nowMs();

      if (recvExpected >= expectedTotal) {
        std::cout << "All " << expectedTotal << " packets received!"
                  << std::endl;
        break;
      }
    } else {
      int64_t now = nowMs();
      if (!lastAck.empty() && (now - lastRecvTime) > 150) {
        networkLayer->sendPacket(lastAck);
        lastRecvTime = now;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::vector<int32_t> fileContents;
  for (uint32_t i = 0; i < expectedTotal; i++) {
    fileContents.insert(fileContents.end(), recvBuffer[i].begin(),
                        recvBuffer[i].end());
  }

  std::cout << "Receiver returning " << fileContents.size() << " bytes."
            << std::endl;
  return fileContents;
}

// ─────────────────── TIMEOUT (no-op) ───────────────────

void MyProtocol::TimeoutElapsed(int32_t tag) {
  (void)tag; // unused — all retransmission in main sender loop
}

// ─────────────────── Framework setters ───────────────────

void MyProtocol::setFileID(std::string id) { fileID = id; }

void MyProtocol::setNetworkLayer(framework::NetworkLayer *nLayer) {
  networkLayer = nLayer;
}

} /* namespace my_protocol */
