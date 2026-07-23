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
  uint16_t rampMsFullScale;   // ESP32 software ramp: ms for a 0..1000 sweep (forward)
  bool     limitingEnabled;   // master switch for the PI speed limiter
  int16_t  brakeTorqueMax;    // brake pedal authority (torque applied opposite motion)
  int16_t  reverseMaxTorque;  // drive torque cap while reversing (usually < maxTorque)
  int16_t  reverseSpeedCeiling; // rpm ceiling for the limiter while reversing
  uint16_t reverseRampMs;     // ESP32 software ramp: ms for a 0..1000 sweep (reverse)
};

// Global vehicle calibration: pedal-sensor mapping + motion thresholds. Unlike
// Tunables these are a property of the physical car, not the driving style, so
// there is ONE shared set (not per-profile). Persisted in NVS; the config.h
// #defines are the defaults used when flash is empty. Read live by the control
// task (mutex-guarded, like Tunables) so the web UI can adjust them on the fly.
struct Calibration {
  // Pedal sensor mapping (raw 12-bit ADC counts).
  int16_t throttleRawMin;      // raw analogRead released
  int16_t throttleRawMax;      // raw analogRead fully pressed
  int16_t throttleDeadband;    // low-end raw counts ignored
  int16_t brakeRawMin;
  int16_t brakeRawMax;
  int16_t brakeDeadband;
  // Motion / direction behaviour (abs wheel rpm, and one torque).
  int16_t launchSpeedThresh;   // below = launching (launch cap) & safe to change dir
  int16_t nearStopThresh;      // <= = "stopped": adopt the switch direction
  int16_t brakeBlendSpeed;     // brake/decel torque tapers to zero below this rpm
  int16_t dirChangeBrakeTorque;// auto pre-reversal slow-down torque
  // Drive-wheel diameter (mm). Only used to convert rpm <-> km/h in the web UI;
  // the control loop stays entirely in rpm.
  int16_t wheelDiaMm;
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

  Calibration getCalibration() {
    Calibration c;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    c = _calibration;
    xSemaphoreGive(_mutex);
    return c;
  }
  void setCalibration(const Calibration &c) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _calibration = c;
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

  // Config mode: while set, the control task commands zero drive torque (the web
  // Config page is open). Pedals/switch are still read and telemetry still flows,
  // so calibration works, but nothing can make the cart drive.
  bool getConfigMode() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool c = _configMode;
    xSemaphoreGive(_mutex);
    return c;
  }
  void setConfigMode(bool c) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _configMode = c;
    xSemaphoreGive(_mutex);
  }

  // Whether the car has a valid saved calibration. Defaults to false so the
  // control task refuses to drive until PresetStore publishes the real state at
  // boot — an uncalibrated car never drives, even with no client connected.
  bool getCalibrated() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool c = _calibrated;
    xSemaphoreGive(_mutex);
    return c;
  }
  void setCalibrated(bool c) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _calibrated = c;
    xSemaphoreGive(_mutex);
  }

private:
  SemaphoreHandle_t _mutex = nullptr;
  Tunables    _tunables{};
  Calibration _calibration{};
  Telemetry   _telemetry{};
  bool        _estop = false;
  bool        _configMode = false;
  bool        _calibrated = false;
};

extern SharedState g_state;
