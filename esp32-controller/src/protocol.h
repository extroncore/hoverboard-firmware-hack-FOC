// ============================================================================
//  protocol.h - binary UART protocol shared with the hoverboard firmware.
//  MUST match Inc/util.h (SerialCommand) and Src/main.c (SerialFeedback) of the
//  STM32 firmware. This assumes ENABLE_ODOMETRY is OFF -> 18-byte feedback.
// ============================================================================
#pragma once
#include <Arduino.h>

static const uint16_t HB_START_FRAME = 0xABCD;

#pragma pack(push, 1)

// ESP32 -> hoverboard, 8 bytes. `speed` is the TORQUE request in TRQ_MODE.
typedef struct {
  uint16_t start;
  int16_t  steer;
  int16_t  speed;
  uint16_t checksum;   // start ^ steer ^ speed
} SerialCommand;

// hoverboard -> ESP32, 18 bytes (odometry OFF).
typedef struct {
  uint16_t start;
  int16_t  cmd1;
  int16_t  cmd2;
  int16_t  speedR_meas;
  int16_t  speedL_meas;
  int16_t  batVoltage;   // hundredths of a volt (e.g. 3650 = 36.50 V)
  int16_t  boardTemp;    // TENTHS of a degree C (e.g. 330 = 33.0 C) - STM32 chip temp
  uint16_t cmdLed;
  uint16_t checksum;     // XOR of all preceding fields
} SerialFeedback;

#pragma pack(pop)

static_assert(sizeof(SerialCommand) == 8,  "SerialCommand must be 8 bytes");
static_assert(sizeof(SerialFeedback) == 18, "SerialFeedback must be 18 bytes (odometry OFF)");

inline uint16_t commandChecksum(const SerialCommand &c) {
  return (uint16_t)(c.start ^ c.steer ^ c.speed);
}

inline uint16_t feedbackChecksum(const SerialFeedback &f) {
  return (uint16_t)(f.start ^ f.cmd1 ^ f.cmd2 ^ f.speedR_meas ^ f.speedL_meas ^
                    f.batVoltage ^ f.boardTemp ^ f.cmdLed);
}
