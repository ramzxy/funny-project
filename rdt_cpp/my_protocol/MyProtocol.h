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

  // ── Packet format ──
  enum : uint32_t {
    DATA_HEADER = 5, // type(1) + seq(2) + totalPkts(2)
    ACK_HEADER = 11, // type(1) + ackBase(2) + sackMask(8)
    DATASIZE = 200,  // payload bytes per data packet (sweet spot)
    TYPE_DATA = 0,
    TYPE_ACK = 1
  };

  // ── Sender state ──
  std::vector<std::vector<int32_t>> packetBuffer;
  std::vector<bool> acked;
  std::vector<int64_t> sentTime;
  uint32_t sendBase = 0;
  uint32_t nextSeq = 0;
  uint32_t totalPkts = 0;
  std::mutex senderMtx;

  // ── Congestion control — server-tuned AIMD ──
  double cwnd = 5.0;      // scaled for 2x bigger packets
  double ssthresh = 15.0;  // scaled for 2x bigger packets
  int64_t lastLossTime = 0;

  // ── RTT estimation (Jacobson/Karels) ──
  double estRtt = 650.0; // ms — matches challenge server RTT
  double devRtt = 50.0;  // ms — initial RTO = 650+200 = 850ms
  double rtoBackoff = 1.0;

  // ── Retransmit tracking ──
  std::vector<bool> retransmitted;

  // ── Duplicate ACK detection ──
  uint32_t lastAckBase = 0;
  uint32_t dupAckCount = 0;

  // ── Keepalive ──
  int64_t lastActivity = 0;
  int64_t lastSendTime = 0;

  // ── Methods ──
  void ccOnAck(uint32_t ackedCount);
  void ccOnLoss();
  void ccOnTimeout();
  double getRTO();
  void updateRTT(double sampleMs);
  void handleSackGaps(uint32_t ackBase, uint64_t sackMask);

  std::vector<int32_t> buildDataPacket(uint32_t seq, uint32_t total,
                                       const std::vector<int32_t> &fileData,
                                       uint32_t offset, uint32_t len);
  std::vector<int32_t> buildAckPacket(uint32_t ackBase, uint64_t sackMask);

  uint32_t parseSeq(const std::vector<int32_t> &pkt);
  uint32_t parseTotalPkts(const std::vector<int32_t> &pkt);
  uint32_t parseAckBase(const std::vector<int32_t> &pkt);
  uint64_t parseSackMask(const std::vector<int32_t> &pkt);

  int64_t nowMs();
};

} /* namespace my_protocol */

#endif /* MyProtocol_H_ */
