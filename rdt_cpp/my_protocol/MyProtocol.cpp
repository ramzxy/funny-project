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
  // Cap to prevent queue overflow
  if (cwnd > 50.0)
    cwnd = 50.0;
}

// ccOnLoss: SACK-detected loss (entering recovery)
void MyProtocol::ccOnLoss() {
  int64_t now = nowMs();
  if (now - lastLossEventTime < (int64_t)estRtt) {
    return; // deduplicate
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

// ccOnTimeout: RTO expired — gentler multiplicative decrease + RTO backoff
void MyProtocol::ccOnTimeout() {
  int64_t now = nowMs();
  if (now - lastLossEventTime < (int64_t)estRtt) {
    rtoBackoff = std::min(rtoBackoff * 2.0, 4.0);
    return;
  }
  lastLossEventTime = now;

  wMax = cwnd;
  if (wMax < 4.0)
    wMax = 4.0;
  ssthresh = std::max(4.0, cwnd * CUBIC_BETA);
  cwnd = std::max(4.0, cwnd * 0.7);
  lastLossTime = now;
  inRecovery = false;

  // TCP standard: exponential backoff on timeout (capped at 4x)
  rtoBackoff = std::min(rtoBackoff * 2.0, 4.0);
}

double MyProtocol::getRTO() {
  double rto = (estRtt + 4.0 * devRtt) * rtoBackoff;
  if (rto < 300.0)
    rto = 300.0;
  if (rto > 5000.0)
    rto = 5000.0;
  return rto;
}

void MyProtocol::updateRTT(double sampleMs) {
  if (sampleMs < 0)
    return;
  double error = std::abs(sampleMs - estRtt);
  devRtt = 0.75 * devRtt + 0.25 * error;
  estRtt = 0.875 * estRtt + 0.125 * sampleMs;
  // Valid RTT sample received — reset backoff
  rtoBackoff = 1.0;
}

// ── SACK-based fast retransmit with recovery mode ──

void MyProtocol::sackRetransmit(uint32_t ackBase, uint64_t sackMask) {
  if (sackMask == 0)
    return;

  int highestBit = -1;
  for (int i = 63; i >= 0; i--) {
    if ((sackMask >> i) & 1) {
      highestBit = i;
      break;
    }
  }
  if (highestBit < 0)
    return;

  // Check for actual gaps
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

  // Enter recovery once
  if (!inRecovery) {
    inRecovery = true;
    recoverySeq = nextSeq > 0 ? nextSeq - 1 : 0;
    ccOnLoss();
  }

  // Retransmit gap packets (no further cwnd cuts during recovery)
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

  std::vector<int32_t> fileContents = framework::getFileContents(fileID);
  uint32_t fileSize = (uint32_t)fileContents.size();
  std::cout << "File length: " << fileSize << std::endl;

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
  lastSendTime = 0;
  inRecovery = false;
  rtoBackoff = 1.0;
  lastAckBase = 0;
  dupAckCount = 0;

  while (!stop) {
    // ── PACED SENDING ──
    // Send at most 2 packets per loop iteration to prevent queue overflow.
    // With 1ms loop period, max rate = 2000 pkts/sec, but window constraint
    // limits in-flight to cwnd. This spreads the window over multiple ms
    // instead of bursting all at once.
    {
      std::lock_guard<std::mutex> lock(senderMtx);
      uint32_t effectiveWindow = (uint32_t)cwnd;
      int64_t now = nowMs();
      double pacingMs = estRtt / std::max(cwnd, 1.0);
      if (pacingMs < 1.0)
        pacingMs = 1.0;

      int sent = 0;
      while (nextSeq < totalPkts && nextSeq < sendBase + effectiveWindow &&
             sent < 2) {
        if (sent == 0 && (now - lastSendTime) < (int64_t)pacingMs) {
          break; // wait for pacing interval
        }
        networkLayer->sendPacket(packetBuffer[nextSeq]);
        sentTime[nextSeq] = nowMs();
        framework::SetTimeout((long)getRTO(), this, (int32_t)nextSeq);
        nextSeq++;
        sent++;
      }
      if (sent > 0)
        lastSendTime = now;
    }

    // ── ACK PROCESSING ──
    std::vector<int32_t> ackPkt;
    while (networkLayer->receivePacket(&ackPkt)) {
      if (ackPkt.size() >= ACK_HEADER && (ackPkt[0] & 0xFF) == TYPE_ACK) {
        std::lock_guard<std::mutex> lock(senderMtx);

        uint32_t ackBase = parseAckBase(ackPkt);
        uint64_t sackMask = parseSackMask(ackPkt);
        uint32_t ackedCount = 0;

        // Advance send base (cumulative ACK)
        while (sendBase < ackBase && sendBase < totalPkts) {
          if (!acked[sendBase]) {
            // Karn's algorithm: only sample from non-retransmitted
            if (sentTime[sendBase] > 0 && !retransmitted[sendBase]) {
              double sample = (double)(nowMs() - sentTime[sendBase]);
              updateRTT(sample);
            }
            acked[sendBase] = true;
            ackedCount++;
          }
          sendBase++;
        }

        // Exit recovery when acked past recovery point
        if (inRecovery && sendBase > recoverySeq) {
          inRecovery = false;
        }

        // Process SACK bitmask
        for (int i = 0; i < 64; i++) {
          if ((sackMask >> i) & 1) {
            uint32_t sackSeq = ackBase + i;
            if (sackSeq < totalPkts && !acked[sackSeq]) {
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

        // SACK fast retransmit with recovery
        sackRetransmit(ackBase, sackMask);

        // Duplicate ACK detection: if sendBase didn't advance,
        // the head-of-line packet might be lost (SACK can't detect
        // loss of the LAST packet in a window). Retransmit after 2 dup ACKs.
        if (ackBase == lastAckBase && ackBase == sendBase) {
          dupAckCount++;
          if (dupAckCount >= 2 && sendBase < totalPkts &&
              !retransmitted[sendBase]) {
            networkLayer->sendPacket(packetBuffer[sendBase]);
            sentTime[sendBase] = nowMs();
            retransmitted[sendBase] = true;
            framework::SetTimeout((long)getRTO(), this, (int32_t)sendBase);
            if (!inRecovery) {
              inRecovery = true;
              recoverySeq = nextSeq > 0 ? nextSeq - 1 : 0;
              ccOnLoss();
            }
            dupAckCount = 0;
          }
        } else {
          lastAckBase = ackBase;
          dupAckCount = 0;
        }

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

        if (recvBuffer.find(seq) == recvBuffer.end()) {
          std::vector<int32_t> payload(packet.begin() + DATA_HEADER,
                                       packet.end());
          recvBuffer[seq] = payload;
        }

        while (recvBuffer.find(recvExpected) != recvBuffer.end()) {
          recvExpected++;
        }

        uint64_t sackMask = 0;
        for (int i = 0; i < 64; i++) {
          uint32_t checkSeq = recvExpected + i;
          if (recvBuffer.find(checkSeq) != recvBuffer.end()) {
            sackMask |= (1ULL << i);
          }
        }

        std::vector<int32_t> ack = buildAckPacket(recvExpected, sackMask);
        networkLayer->sendPacket(ack);

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
    // STALE TIMEOUT DETECTION: If this packet was already fast-retransmitted
    // and the retransmit was recent, this is the ORIGINAL timeout firing late.
    // Skip it — the fast retransmit has its own timer.
    if (retransmitted[seq]) {
      int64_t timeSinceSend = nowMs() - sentTime[seq];
      if (timeSinceSend < (int64_t)(getRTO() * 0.9)) {
        return; // stale timeout — fast retransmit is handling this packet
      }
    }

    // Genuine timeout — retransmit
    networkLayer->sendPacket(packetBuffer[seq]);
    sentTime[seq] = nowMs();
    retransmitted[seq] = true;
    framework::SetTimeout((long)getRTO(), this, tag);

    ccOnTimeout();
  }
}

// ─────────────────── Framework setters ───────────────────

void MyProtocol::setFileID(std::string id) { fileID = id; }

void MyProtocol::setNetworkLayer(framework::NetworkLayer *nLayer) {
  networkLayer = nLayer;
}

} /* namespace my_protocol */
