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

// ─────────────────── Congestion Control (Server-Tuned) ───────────────────
//
// This channel has ~10% random corruption (NOT congestion). Cutting cwnd on
// SACK-detected loss is counterproductive — it slows us down without
// reducing the loss rate. Only reduce cwnd on genuine timeouts.

void MyProtocol::ccOnAck(uint32_t ackedCount) {
  if (cwnd < ssthresh) {
    // Slow start — exponential growth
    cwnd += ackedCount;
  } else {
    // Additive increase: +2.0 per full window of ACKs (fast ramp)
    cwnd += 2.0 * ackedCount / cwnd;
  }
  if (cwnd > 15.0)
    cwnd = 15.0;
}

void MyProtocol::ccOnLoss() {
  // No-op: loss on this channel is random corruption, not congestion.
  // Retransmission is handled by SACK; cwnd stays unchanged.
}

void MyProtocol::ccOnTimeout() {
  int64_t now = nowMs();
  if (now - lastLossTime < (int64_t)estRtt) {
    rtoBackoff = std::min(rtoBackoff * 1.5, 2.0);
    return;
  }
  lastLossTime = now;

  // Gentle decrease — timeout is likely corruption, not congestion
  ssthresh = std::max(4.0, cwnd * 0.85);
  cwnd = std::max(4.0, cwnd * 0.85);
  rtoBackoff = std::min(rtoBackoff * 1.5, 2.0);
}

double MyProtocol::getRTO() {
  double rto = (estRtt + 4.0 * devRtt) * rtoBackoff;
  if (rto < 700.0)
    rto = 700.0; // floor: must be above ~650ms RTT
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
  rtoBackoff = 1.0; // valid RTT → reset backoff
}

// ─────────────────── SACK-Driven Fast Retransmit ───────────────────
//
// On every ACK with SACK info, immediately retransmit any gap packets.
// Unlike standard TCP, we allow re-retransmitting a packet if enough time
// has passed since its last send (handles cascading losses where the
// retransmission itself was lost).

void MyProtocol::handleSackGaps(uint32_t ackBase, uint64_t sackMask) {
  if (sackMask == 0)
    return;

  // Find highest SACK bit to know the gap range
  int highBit = -1;
  for (int i = 63; i >= 0; i--) {
    if ((sackMask >> i) & 1) {
      highBit = i;
      break;
    }
  }
  if (highBit < 0)
    return;

  // Check for actual gaps (not just contiguous SACKs)
  bool hasGaps = false;
  for (int i = 0; i < highBit; i++) {
    uint32_t seq = ackBase + i;
    if (seq < totalPkts && !acked[seq] && !((sackMask >> i) & 1)) {
      hasGaps = true;
      break;
    }
  }
  if (!hasGaps)
    return;

  int64_t now = nowMs();
  bool lossDetected = false;

  for (int i = 0; i <= highBit; i++) {
    uint32_t seq = ackBase + i;
    if (seq >= totalPkts)
      break;
    if (!acked[seq] && !((sackMask >> i) & 1)) {
      // This packet is in a gap — retransmit it
      bool shouldRetransmit = false;
      if (!retransmitted[seq]) {
        // First retransmit: immediate
        shouldRetransmit = true;
      } else if (now - sentTime[seq] >= (int64_t)(getRTO() * 0.7)) {
        // Re-retransmit: previous retransmit likely also lost
        shouldRetransmit = true;
      }

      if (shouldRetransmit) {
        networkLayer->sendPacket(packetBuffer[seq]);
        sentTime[seq] = now;
        retransmitted[seq] = true;
        framework::SetTimeout((long)getRTO(), this, (int32_t)seq);
        lossDetected = true;
      }
    }
  }

  if (lossDetected) {
    ccOnLoss(); // deduplicated internally by lastLossTime
  }
}

// ─────────────────── Constructor / Destructor ───────────────────

MyProtocol::MyProtocol() { this->networkLayer = NULL; }

MyProtocol::~MyProtocol() {}

void MyProtocol::setStop() { this->stop = true; }

// ─────────────────── SENDER ───────────────────
//
// Strategy: paced window-based sending with SACK recovery
//  1. Send new packets paced at RTT/cwnd intervals (max 2 per loop)
//  2. Process ACKs with SACK-driven fast retransmit
//  3. After initial send, proactively retransmit stale unacked packets
//  4. Keepalive: retransmit oldest unacked if idle >500ms

void MyProtocol::sender() {
  std::cout << "Sending..." << std::endl;

  std::vector<int32_t> fileContents = framework::getFileContents(fileID);
  uint32_t fileSize = (uint32_t)fileContents.size();
  std::cout << "File length: " << fileSize << std::endl;

  totalPkts = (fileSize + DATASIZE - 1) / DATASIZE;
  if (totalPkts == 0)
    totalPkts = 1;
  std::cout << "Total packets: " << totalPkts << std::endl;

  // Pre-build all packets
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
  lastActivity = nowMs();
  lastAckBase = 0;
  dupAckCount = 0;
  rtoBackoff = 1.0;

  lastSendTime = 0;

  while (!stop) {
    // ── PHASE 1: Send new data packets (paced) ──
    // The server has a small buffer — bursting causes CongestedDropped.
    // Send at most 2 packets per loop iteration, paced by RTT/cwnd.
    {
      std::lock_guard<std::mutex> lock(senderMtx);
      uint32_t effectiveWindow = (uint32_t)std::min(cwnd, 15.0);
      int64_t now = nowMs();
      double pacingMs = estRtt / std::max(cwnd, 1.0);
      if (pacingMs < 20.0)
        pacingMs = 20.0; // 2x for 2x bigger packets
      int sent = 0;

      while (nextSeq < totalPkts && nextSeq < sendBase + effectiveWindow &&
             sent < 2) {
        if (sent == 0 && (now - lastSendTime) < (int64_t)pacingMs) {
          break; // wait for pacing interval
        }
        networkLayer->sendPacket(packetBuffer[nextSeq]);
        sentTime[nextSeq] = now;
        retransmitted[nextSeq] = false;
        framework::SetTimeout((long)getRTO(), this, (int32_t)nextSeq);
        nextSeq++;
        sent++;
      }

      // ── PHASE 2: Proactive tail retransmit ──
      // Once all packets initially sent, retransmit 1 unacked packet per
      // loop if enough time has passed. Gentle — don't re-flood.
      if (nextSeq >= totalPkts && sendBase < totalPkts && sent == 0) {
        for (uint32_t i = sendBase; i < totalPkts; i++) {
          if (!acked[i] && (now - sentTime[i]) > (int64_t)(estRtt * 1.5)) {
            networkLayer->sendPacket(packetBuffer[i]);
            sentTime[i] = now;
            retransmitted[i] = true;
            framework::SetTimeout((long)getRTO(), this, (int32_t)i);
            sent++;
            break;
          }
        }
      }

      // ── PHASE 3: Keepalive ──
      // If we haven't sent anything for a while, retransmit the oldest
      // unacked packet as a probe. Prevents server inactivity kill.
      if (sent == 0 && sendBase < totalPkts && (now - lastActivity) > 500) {
        for (uint32_t i = sendBase; i < totalPkts; i++) {
          if (!acked[i]) {
            networkLayer->sendPacket(packetBuffer[i]);
            sentTime[i] = now;
            retransmitted[i] = true;
            framework::SetTimeout((long)getRTO(), this, (int32_t)i);
            lastActivity = now;
            break;
          }
        }
      }

      if (sent > 0) {
        lastActivity = now;
        lastSendTime = now;
      }
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
            // Karn's algorithm: only sample RTT from non-retransmitted pkts
            if (sentTime[sendBase] > 0 && !retransmitted[sendBase]) {
              double sample = (double)(nowMs() - sentTime[sendBase]);
              updateRTT(sample);
            }
            acked[sendBase] = true;
            ackedCount++;
          }
          sendBase++;
        }

        // Process SACK bitmask — mark individually acked packets
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

        // SACK-driven fast retransmit
        handleSackGaps(ackBase, sackMask);

        // Duplicate ACK detection for head-of-line packet.
        // SACK can't show loss of the *first* unacked packet (it's before
        // the SACK range). So we use dup ACK counting for that case.
        if (ackBase == lastAckBase && ackBase == sendBase) {
          dupAckCount++;
          if (dupAckCount >= 2 && sendBase < totalPkts) {
            // Fast retransmit head-of-line
            networkLayer->sendPacket(packetBuffer[sendBase]);
            sentTime[sendBase] = nowMs();
            retransmitted[sendBase] = true;
            framework::SetTimeout((long)getRTO(), this, (int32_t)sendBase);
            ccOnLoss();
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
      // Done — stay alive until framework sets stop
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cout << "Sender finished. cwnd=" << cwnd << " rto=" << getRTO() << "ms"
            << std::endl;
}

// ─────────────────── RECEIVER ───────────────────
//
// Strategy:
//  1. ACK every received data packet with full SACK bitmap
//  2. If idle for >200ms, re-send the last ACK (keepalive + SACK refresh)
//     This is CRITICAL: it prevents the server from killing the session
//     AND gives the sender fresh loss information.

std::vector<int32_t> MyProtocol::receiver() {
  std::cout << "Receiving..." << std::endl;

  uint32_t expectedTotal = 0;
  uint32_t recvExpected = 0; // cumulative ACK base
  std::vector<std::vector<int32_t>> recvBuffer;
  std::vector<bool> received;
  int64_t lastRecvTime = nowMs();
  std::vector<int32_t> lastAck; // cached for keepalive resend

  while (true) {
    std::vector<int32_t> packet;

    if (networkLayer->receivePacket(&packet)) {
      if (packet.size() >= DATA_HEADER && (packet[0] & 0xFF) == TYPE_DATA) {
        uint32_t seq = parseSeq(packet);
        uint32_t total = parseTotalPkts(packet);

        if (expectedTotal == 0) {
          expectedTotal = total;
          recvBuffer.resize(expectedTotal);
          received.resize(expectedTotal, false);
          std::cout << "Expecting " << expectedTotal << " packets."
                    << std::endl;
        }

        // Buffer out-of-order packets (O(1) access)
        if (seq < expectedTotal && !received[seq]) {
          std::vector<int32_t> payload(packet.begin() + DATA_HEADER,
                                       packet.end());
          recvBuffer[seq] = payload;
          received[seq] = true;
        }

        // Advance cumulative ACK
        while (recvExpected < expectedTotal && received[recvExpected]) {
          recvExpected++;
        }

        // Build SACK bitmap: bit i = 1 means recvExpected+i is received
        uint64_t sackMask = 0;
        for (int i = 0; i < 64; i++) {
          uint32_t checkSeq = recvExpected + i;
          if (checkSeq < expectedTotal && received[checkSeq]) {
            sackMask |= (1ULL << i);
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
      }
    } else {
      // No data packet — keepalive: re-send last ACK if idle
      int64_t now = nowMs();
      if (!lastAck.empty() && (now - lastRecvTime) > 200) {
        networkLayer->sendPacket(lastAck);
        lastRecvTime = now;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Reassemble file from buffered payloads
  std::vector<int32_t> fileContents;
  for (uint32_t i = 0; i < expectedTotal; i++) {
    fileContents.insert(fileContents.end(), recvBuffer[i].begin(),
                        recvBuffer[i].end());
  }

  std::cout << "Receiver returning " << fileContents.size() << " bytes."
            << std::endl;
  return fileContents;
}

// ─────────────────── TIMEOUT ───────────────────
//
// Simple: if the packet is still unacked and in-window, retransmit it.
// No stale-timeout heuristics — just retransmit and let dedup handle the rest.

void MyProtocol::TimeoutElapsed(int32_t tag) {
  std::lock_guard<std::mutex> lock(senderMtx);

  uint32_t seq = (uint32_t)tag;

  if (seq < totalPkts && !acked[seq] && seq >= sendBase) {
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
