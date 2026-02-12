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
#include <cstdio>
#include <iostream>
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

  // ── Packet format (with XOR checksum) ──
  enum : uint32_t {
    DATA_HEADER = 6,  // type(1) + seq(2) + totalPkts(2) + xor(1)
    ACK_HEADER = 6,   // type(1) + ackBase(2) + sackMask(2) + xor(1)
    DATASIZE = 122,   // 128 - 6 header
    TYPE_DATA = 0,
    TYPE_ACK = 1
  };

  static const uint32_t WINDOW = 16;
  static const int64_t TIMEOUT_MS = 700;

  // ── Sender state ──
  std::vector<std::vector<int32_t>> packetBuffer;
  std::vector<bool> acked;
  std::vector<int64_t> sentTime;
  uint32_t sendBase = 0;
  uint32_t nextSeq = 0;
  uint32_t totalPkts = 0;

  // ── Methods ──
  std::vector<int32_t> buildDataPacket(uint32_t seq, uint32_t total,
                                       const std::vector<int32_t> &fileData,
                                       uint32_t offset, uint32_t len);
  std::vector<int32_t> buildAckPacket(uint32_t ackBase, uint16_t sackMask);

  uint32_t parseSeq(const std::vector<int32_t> &pkt);
  uint32_t parseTotalPkts(const std::vector<int32_t> &pkt);
  bool verifyDataChecksum(const std::vector<int32_t> &pkt);
  bool verifyAckChecksum(const std::vector<int32_t> &pkt);

  int64_t nowMs();
};

} /* namespace my_protocol */

#endif /* MyProtocol_H_ */
