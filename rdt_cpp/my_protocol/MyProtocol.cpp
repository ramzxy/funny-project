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
#include <cstring>
#include <thread>

namespace my_protocol {

// ─────────────────── Helpers ───────────────────

int64_t MyProtocol::nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ── Packet builders ──

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
  for (uint32_t i = 0; i < len; i++) {
    pkt[DATA_HEADER + i] = fileData[offset + i] & 0xFF;
  }
  return pkt;
}

std::vector<int32_t> MyProtocol::buildAckPacket(uint32_t ackBase,
                                                uint64_t sackMask) {
  std::vector<int32_t> pkt(ACK_HEADER);
  pkt[0] = TYPE_ACK;
  pkt[1] = (ackBase >> 8) & 0xFF;
  pkt[2] = ackBase & 0xFF;
  for (int i = 0; i < 8; i++) {
    pkt[3 + i] = (sackMask >> (56 - 8 * i)) & 0xFF;
  }
  return pkt;
}

// ── Packet parsers ──

uint32_t MyProtocol::parseSeq(const std::vector<int32_t> &pkt) {
  return ((pkt[1] & 0xFF) << 8) | (pkt[2] & 0xFF);
}

uint32_t MyProtocol::parseTotalPkts(const std::vector<int32_t> &pkt) {
  return ((pkt[3] & 0xFF) << 8) | (pkt[4] & 0xFF);
}

uint32_t MyProtocol::parseAckBase(const std::vector<int32_t> &pkt) {
  return ((pkt[1] & 0xFF) << 8) | (pkt[2] & 0xFF);
}

uint64_t MyProtocol::parseSackMask(const std::vector<int32_t> &pkt) {
  uint64_t mask = 0;
  for (int i = 0; i < 8; i++) {
    mask |= ((uint64_t)(pkt[3 + i] & 0xFF)) << (56 - 8 * i);
  }
  return mask;
}

// ── Congestion control ──

void MyProtocol::ccOnAck(uint32_t ackedCount) {
  if (cwnd < ssthresh) {
    // Slow start: exponential growth
    cwnd += ackedCount;
  } else {
    // CUBIC congestion avoidance
    double now = (double)nowMs();
    double tSec = (now - (double)lastLossTime) / 1000.0;
    double K = std::cbrt(wMax * (1.0 - CUBIC_BETA) / CUBIC_C);
    double target = CUBIC_C * std::pow(tSec - K, 3.0) + wMax;
    double increment = (target - cwnd) / cwnd;
    cwnd += std::max(0.0, increment) * ackedCount;
  }
  if (cwnd > 200.0)
    cwnd = 200.0;
}

// ccOnLoss: called for SACK-detected loss (fast retransmit)
// Less severe — multiplicative decrease but stay in congestion avoidance
void MyProtocol::ccOnLoss() {
  // Deduplicate: only cut once per RTT
  int64_t now = nowMs();
  if (now - lastLossEventTime < (int64_t)estRtt) {
    return;
  }
  lastLossEventTime = now;

  wMax = cwnd;
  if (wMax < 4.0)
    wMax = 4.0;
  cwnd = cwnd * CUBIC_BETA;
  ssthresh = cwnd;
  lastLossTime = now;
  if (cwnd < 4.0)
    cwnd = 4.0;
}

// ccOnTimeout: called for RTO timeout — more severe
void MyProtocol::ccOnTimeout() {
  // Deduplicate: only cut once per RTT
  int64_t now = nowMs();
  if (now - lastLossEventTime < (int64_t)estRtt) {
    return;
  }
  lastLossEventTime = now;

  wMax = cwnd;
  if (wMax < 4.0)
    wMax = 4.0;
  ssthresh = cwnd * CUBIC_BETA;
  cwnd = 4.0; // reset to initial window on timeout (like TCP)
  lastLossTime = now;

  // Exit recovery on timeout
  inRecovery = false;
}

double MyProtocol::getRTO() {
  double rto = estRtt + 4.0 * devRtt;
  if (rto < 100.0)
    rto = 100.0;
  if (rto > 3000.0)
    rto = 3000.0;
  return rto;
}

void MyProtocol::updateRTT(double sampleMs) {
  if (sampleMs < 0)
    return;
  double error = std::abs(sampleMs - estRtt);
  devRtt = 0.75 * devRtt + 0.25 * error;
  estRtt = 0.875 * estRtt + 0.125 * sampleMs;
}

// ── SACK-based fast retransmit with recovery mode ──

void MyProtocol::sackRetransmit(uint32_t ackBase, uint64_t sackMask) {
  if (sackMask == 0)
    return;

  // Find highest SACK'd offset to know where gaps are
  int highestBit = -1;
  for (int i = 63; i >= 0; i--) {
    if ((sackMask >> i) & 1) {
      highestBit = i;
      break;
    }
  }
  if (highestBit < 0)
    return;

  // Check if there are actual gaps (missing packets below highest SACK)
  bool hasGaps = false;
  for (int i = 0; i < highestBit; i++) {
    uint32_t seq = ackBase + i;
    if (seq < totalPkts && !acked[seq] && !((sackMask >> i) & 1)) {
      hasGaps = true;
      break;
    }
  }
  if (!hasGaps)
    return;

  // Enter recovery if not already in it
  if (!inRecovery) {
    inRecovery = true;
    recoverySeq = nextSeq > 0 ? nextSeq - 1 : 0;
    ccOnLoss(); // cut cwnd ONCE when entering recovery
  }

  // Retransmit gap packets (no further cwnd cuts)
  for (int i = 0; i <= highestBit; i++) {
    uint32_t seq = ackBase + i;
    if (seq >= totalPkts)
      break;
    if (!acked[seq] && !((sackMask >> i) & 1) && !retransmitted[seq]) {
      networkLayer->sendPacket(packetBuffer[seq]);
      sentTime[seq] = nowMs();
      retransmitted[seq] = true;
      framework::SetTimeout((long)getRTO(), this, (int32_t)seq);
    }
  }
}

// ─────────────────── Constructor / Destructor ───────────────────

MyProtocol::MyProtocol() { this->networkLayer = NULL; }

MyProtocol::~MyProtocol() {}

void MyProtocol::setStop() { this->stop = true; }

// ─────────────────── SENDER ───────────────────

void MyProtocol::sender() {
  std::cout << "Sending..." << std::endl;

  // Read the input file
  std::vector<int32_t> fileContents = framework::getFileContents(fileID);
  uint32_t fileSize = (uint32_t)fileContents.size();
  std::cout << "File length: " << fileSize << std::endl;

  // Chop file into packets
  totalPkts = (fileSize + DATASIZE - 1) / DATASIZE;
  if (totalPkts == 0)
    totalPkts = 1;
  std::cout << "Total packets: " << totalPkts << std::endl;

  packetBuffer.resize(totalPkts);
  acked.resize(totalPkts, false);
  sentTime.resize(totalPkts, 0);
  retransmitted.resize(totalPkts, false);

  for (uint32_t i = 0; i < totalPkts; i++) {
    uint32_t offset = i * DATASIZE;
    uint32_t len = std::min((uint32_t)DATASIZE, fileSize - offset);
    packetBuffer[i] = buildDataPacket(i, totalPkts, fileContents, offset, len);
  }

  sendBase = 0;
  nextSeq = 0;
  lastLossTime = nowMs();
  lastLossEventTime = 0;
  inRecovery = false;

  // Main sender loop
  while (!stop) {
    // Send packets within the congestion window
    {
      std::lock_guard<std::mutex> lock(senderMtx);
      uint32_t effectiveWindow = (uint32_t)cwnd;
      while (nextSeq < totalPkts && nextSeq < sendBase + effectiveWindow) {
        networkLayer->sendPacket(packetBuffer[nextSeq]);
        sentTime[nextSeq] = nowMs();
        framework::SetTimeout((long)getRTO(), this, (int32_t)nextSeq);
        nextSeq++;
      }
    }

    // Check for incoming ACKs
    std::vector<int32_t> ackPkt;
    while (networkLayer->receivePacket(&ackPkt)) {
      if (ackPkt.size() >= ACK_HEADER && (ackPkt[0] & 0xFF) == TYPE_ACK) {
        std::lock_guard<std::mutex> lock(senderMtx);

        uint32_t ackBase = parseAckBase(ackPkt);
        uint64_t sackMask = parseSackMask(ackPkt);

        uint32_t ackedCount = 0;

        // Advance send base for cumulative ACK
        while (sendBase < ackBase && sendBase < totalPkts) {
          if (!acked[sendBase]) {
            // Karn's algorithm: only sample RTT from non-retransmitted packets
            if (sentTime[sendBase] > 0 && !retransmitted[sendBase]) {
              double sample = (double)(nowMs() - sentTime[sendBase]);
              updateRTT(sample);
            }
            acked[sendBase] = true;
            ackedCount++;
          }
          sendBase++;
        }

        // Exit recovery when we've acked past the recovery point
        if (inRecovery && sendBase > recoverySeq) {
          inRecovery = false;
        }

        // Process SACK bitmask
        for (int i = 0; i < 64; i++) {
          if ((sackMask >> i) & 1) {
            uint32_t sackSeq = ackBase + i;
            if (sackSeq < totalPkts && !acked[sackSeq]) {
              // Karn's: only sample RTT from non-retransmitted
              if (sentTime[sackSeq] > 0 && !retransmitted[sackSeq]) {
                double sample = (double)(nowMs() - sentTime[sackSeq]);
                updateRTT(sample);
              }
              acked[sackSeq] = true;
              ackedCount++;
            }
          }
        }

        if (ackedCount > 0) {
          ccOnAck(ackedCount);
        }

        // SACK-based fast retransmit (with recovery mode)
        sackRetransmit(ackBase, sackMask);

        // Check if all packets have been acked
        if (sendBase >= totalPkts) {
          std::cout << "All packets acknowledged! Transfer complete."
                    << std::endl;
          break;
        }
      }
    }

    if (sendBase >= totalPkts) {
      // Done — keep running until framework sets stop
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cout << "Sender finished. cwnd=" << cwnd << " rto=" << getRTO() << "ms"
            << std::endl;
}

// ─────────────────── RECEIVER ───────────────────

std::vector<int32_t> MyProtocol::receiver() {
  std::cout << "Receiving..." << std::endl;

  uint32_t expectedTotal = 0;
  uint32_t recvExpected = 0;
  std::map<uint32_t, std::vector<int32_t>> recvBuffer;

  while (true) {
    std::vector<int32_t> packet;

    if (networkLayer->receivePacket(&packet)) {
      if (packet.size() >= DATA_HEADER && (packet[0] & 0xFF) == TYPE_DATA) {
        uint32_t seq = parseSeq(packet);
        uint32_t total = parseTotalPkts(packet);

        if (expectedTotal == 0) {
          expectedTotal = total;
          std::cout << "Expecting " << expectedTotal << " packets."
                    << std::endl;
        }

        // Store payload if not already received
        if (recvBuffer.find(seq) == recvBuffer.end()) {
          std::vector<int32_t> payload(packet.begin() + DATA_HEADER,
                                       packet.end());
          recvBuffer[seq] = payload;
        }

        // Advance recvExpected
        while (recvBuffer.find(recvExpected) != recvBuffer.end()) {
          recvExpected++;
        }

        // Build SACK bitmask
        uint64_t sackMask = 0;
        for (int i = 0; i < 64; i++) {
          uint32_t checkSeq = recvExpected + i;
          if (recvBuffer.find(checkSeq) != recvBuffer.end()) {
            sackMask |= (1ULL << i);
          }
        }

        // Send ACK
        std::vector<int32_t> ack = buildAckPacket(recvExpected, sackMask);
        networkLayer->sendPacket(ack);

        // Check if we've received everything
        if (expectedTotal > 0 && recvExpected >= expectedTotal) {
          std::cout << "All " << expectedTotal << " packets received!"
                    << std::endl;
          break;
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Assemble the file
  std::vector<int32_t> fileContents;
  for (uint32_t i = 0; i < expectedTotal; i++) {
    auto &payload = recvBuffer[i];
    fileContents.insert(fileContents.end(), payload.begin(), payload.end());
  }

  std::cout << "Receiver returning " << fileContents.size() << " bytes."
            << std::endl;
  return fileContents;
}

// ─────────────────── TIMEOUT ───────────────────

void MyProtocol::TimeoutElapsed(int32_t tag) {
  std::lock_guard<std::mutex> lock(senderMtx);

  uint32_t seq = (uint32_t)tag;

  if (seq < totalPkts && !acked[seq] && seq >= sendBase) {
    networkLayer->sendPacket(packetBuffer[seq]);
    sentTime[seq] = nowMs();
    retransmitted[seq] = true;

    framework::SetTimeout((long)getRTO(), this, tag);

    // Timeouts are severe — reset cwnd
    ccOnTimeout();
  }
}

// ─────────────────── Framework setters ───────────────────

void MyProtocol::setFileID(std::string id) { fileID = id; }

void MyProtocol::setNetworkLayer(framework::NetworkLayer *nLayer) {
  networkLayer = nLayer;
}

} /* namespace my_protocol */
