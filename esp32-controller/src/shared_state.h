// ============================================================================
//  shared_state.h - state shared between the real-time control task and the
//  web server task, guarded by a mutex. Keep critical sections to plain copies.
// ============================================================================
#pragma once
#include <Arduino.h>

// Live-editable control parameters (a profile's values copied in, then tuned).
struct Tunables {
  char     profileName[16];
  int16_t  maxTorque;         // cap on commanded torque magnitude [0..1000]
  int16_t  launchTorqueCap;   // torque cap while below LAUNCH_SPEED_THRESH
  int16_t  speedCeiling;      // rpm ceiling for the speed limiter
  float    pGain;
  float    iGain;
  float    iErrorMax;         // anti-windup upper clamp (overspeed bleed)
  float    iErrorMin;         // anti-windup lower clamp (enables hill boost)
  int16_t  hillPedalThresh;   // raw pedal above which we allow the hill boost
  uint16_t rampMsFullScale;   // ESP32 software ramp: ms for a 0..1000 sweep
  bool     limitingEnabled;   // master switch for the PI speed limiter
};

// Read-only snapshot for the dashboard.
struct Telemetry {
  int16_t batVoltage_cV;      // hundredths of a volt
  int16_t boardTemp;          // deg C
  int16_t speedL;             // measured rpm (left)
  int16_t speedR;             // measured rpm (right)
  int16_t motionSpeed;        // combined, command-frame speed (+ = forward)
  int16_t torqueSent;         // last (signed) torque command actually sent
  bool    linkOk;             // feedback seen within LINK_TIMEOUT_MS
  bool    braking;            // brake pedal or pre-reversal slow-down active
  bool    activeForward;      // throttle currently drives "forward"
  bool    reqForward;         // direction switch currently requests "forward"
  int16_t throttlePct;        // 0..100 of allowed torque
  int16_t brakePct;           // 0..100 of brake authority
  int16_t throttleRaw;        // raw throttle ADC (always physical, for calibration)
  int16_t brakeRaw;           // raw brake ADC
  bool    stopped;            // at/near standstill (abs speed <= NEAR_STOP_THRESH)
};

class SharedState {
public:
  void begin() { _mutex = xSemaphoreCreateMutex(); }

  Tunables getTunables() {
    Tunables t;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    t = _tunables;
    xSemaphoreGive(_mutex);
    return t;
  }
  void setTunables(const Tunables &t) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _tunables = t;
    xSemaphoreGive(_mutex);
  }

  Telemetry getTelemetry() {
    Telemetry t;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    t = _telemetry;
    xSemaphoreGive(_mutex);
    return t;
  }
  void setTelemetry(const Telemetry &t) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _telemetry = t;
    xSemaphoreGive(_mutex);
  }

  // Emergency-stop latch. A plain bool is atomic enough here, but keep it under
  // the same mutex for consistency.
  bool getEstop() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool e = _estop;
    xSemaphoreGive(_mutex);
    return e;
  }
  void setEstop(bool e) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _estop = e;
    xSemaphoreGive(_mutex);
  }

private:
  SemaphoreHandle_t _mutex = nullptr;
  Tunables  _tunables{};
  Telemetry _telemetry{};
  bool      _estop = false;
};

extern SharedState g_state;
