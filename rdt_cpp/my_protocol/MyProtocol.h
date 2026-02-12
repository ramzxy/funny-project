/**
 * MyProtocol.h
 *
 *   Version: 2016-02-11
 *    Author: Jaco ter Braak & Frans van Dijk, University of Twente.
 * Copyright: University of Twente, 2015-2025
 *
 **************************************************************************
 *                          = Copyright notice =                          *
 *                                                                        *
 *            This file may  ONLY  be distributed UNMODIFIED              *
 * In particular, a correct solution to the challenge must  NOT be posted *
 * in public places, to preserve the learning effect for future students. *
 **************************************************************************
 */

#ifndef MyProtocol_H_
#define MyProtocol_H_

#include "../framework/IRDTProtocol.h"
#include "../framework/NetworkLayer.h"
#include "../framework/Utils.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>

namespace my_protocol {

class MyProtocol : public framework::IRDTProtocol {

public:
  MyProtocol();
  ~MyProtocol();
  void sender();
  std::vector<int32_t> receiver();
  void setNetworkLayer(framework::NetworkLayer *);
  void setFileID(std::string);
  void setStop();
  void TimeoutElapsed(int32_t);

private:
  std::string fileID;
  framework::NetworkLayer *networkLayer;
  bool stop = false;

  // --- Packet format constants ---
  enum : uint32_t {
    DATA_HEADER = 5, // type(1) + seq(2) + totalPkts(2)
    ACK_HEADER = 11, // type(1) + ackBase(2) + sackMask(8)
    DATASIZE = 100,  // payload bytes per data packet
    TYPE_DATA = 0,
    TYPE_ACK = 1
  };

  // --- Sender state ---
  std::vector<std::vector<int32_t>> packetBuffer; // all pre-built data packets
  std::vector<bool> acked;                        // per-packet ACK status
  std::vector<int64_t> sentTime; // send timestamp (ms) per packet
  uint32_t sendBase = 0;         // first unacked sequence number
  uint32_t nextSeq = 0;          // next sequence to send
  uint32_t totalPkts = 0;        // total number of data packets
  std::mutex senderMtx; // guards sender state accessed from TimeoutElapsed

  // Congestion control (CUBIC-inspired)
  double cwnd = 10.0;
  double ssthresh = 64.0;
  double wMax = 0.0;
  int64_t lastLossTime = 0;
  int64_t lastLossEventTime = 0; // for deduplicating loss events
  static constexpr double CUBIC_C = 0.4;
  static constexpr double CUBIC_BETA = 0.8;

  // Recovery mode (like TCP SACK recovery)
  bool inRecovery = false;
  uint32_t recoverySeq = 0; // highest seq when recovery started

  // RTT estimation (Jacobson/Karels)
  double estRtt = 200.0; // ms
  double devRtt = 50.0;  // ms

  // Retransmit tracking
  std::vector<bool> retransmitted; // has this packet been retransmitted?

  void ccOnAck(uint32_t ackedCount);
  void ccOnLoss();
  void ccOnTimeout();
  double getRTO();
  void updateRTT(double sampleMs);
  void sackRetransmit(uint32_t ackBase, uint64_t sackMask);

  // Build helpers
  std::vector<int32_t> buildDataPacket(uint32_t seq, uint32_t total,
                                       const std::vector<int32_t> &fileData,
                                       uint32_t offset, uint32_t len);
  std::vector<int32_t> buildAckPacket(uint32_t ackBase, uint64_t sackMask);

  // Parse helpers
  uint32_t parseSeq(const std::vector<int32_t> &pkt);
  uint32_t parseTotalPkts(const std::vector<int32_t> &pkt);
  uint32_t parseAckBase(const std::vector<int32_t> &pkt);
  uint64_t parseSackMask(const std::vector<int32_t> &pkt);

  int64_t nowMs();

  // --- Receiver state ---
  // (kept local in receiver() method)
};

} /* namespace my_protocol */

#endif /* MyProtocol_H_ */
