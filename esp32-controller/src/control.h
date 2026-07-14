// ============================================================================
//  control.h - pure control helpers (no I/O). Torque shaping, PI speed limiter,
//  launch cap and software ramp. See spec sections 3.5 and 5.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "shared_state.h"

// Built-in profile presets (index 0 is the safe boot default).
extern const Tunables PROFILES[];
extern const size_t   PROFILE_COUNT;

// Map a raw pedal reading to [0..outMax] over the calibration window, applying
// a low-end deadband. Used for both throttle and brake.
int16_t mapPedal(int raw, int rawMin, int rawMax, int deadband, int16_t outMax);

// Smart speed limiter (PI on the ESP32). `iError` is persistent accumulator
// state owned by the caller. Returns the torque after limiting.
//   - Overspeed: bleed torque smoothly to hold the ceiling.
//   - Below ceiling but pedal pinned: integral adds torque (hill/obstacle).
//   - Otherwise: reset the accumulator.
int16_t applySpeedLimiter(int16_t baseTorque, int16_t measuredSpeedAbs,
                          int rawPedal, const Tunables &t, float &iError);

// Clamp torque to the launch cap while the car is essentially stationary.
int16_t applyLaunchCap(int16_t torque, int16_t measuredSpeedAbs, int16_t launchCap);

// Slew-limit `current` toward `target`. rampMsFullScale is the time for a full
// 0..1000 sweep; loopMs is the control period. Returns the new current value.
int16_t applyRamp(int16_t current, int16_t target, uint16_t rampMsFullScale, uint16_t loopMs);
