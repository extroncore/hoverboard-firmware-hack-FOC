// ============================================================================
//  HoverboardLink - streams SerialCommand to the board and parses SerialFeedback
//  off UART2 with an incremental, start-frame-synced, checksum-checked reader.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "protocol.h"

class HoverboardLink {
public:
  void begin(HardwareSerial &port, uint32_t baud, int rxPin, int txPin);

  // Send a command frame (steer/speed already in -1000..1000).
  void sendCommand(int16_t steer, int16_t speed);

  // Drain the RX buffer; returns true if a valid feedback frame was decoded.
  bool poll();

  const SerialFeedback &feedback() const { return _fb; }
  uint32_t lastRxMs() const { return _lastRxMs; }
  uint32_t rxFrames() const { return _rxFrames; }   // valid frames decoded (for diagnostics)
  uint32_t rxErrors() const { return _rxErrors; }   // frames dropped on bad checksum

private:
  HardwareSerial *_port = nullptr;
  SerialFeedback  _fb{};
  uint8_t         _buf[sizeof(SerialFeedback)];
  uint16_t        _idx = 0;
  uint32_t        _lastRxMs = 0;
  uint32_t        _rxFrames = 0;
  uint32_t        _rxErrors = 0;
};
