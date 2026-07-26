#include "control.h"
#include "config.h"

const Tunables PROFILES[] = {
  PROFILE_CHILD,   // index 0 - safe boot default
  PROFILE_RACE,
};
const size_t PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);

int16_t mapPedal(int raw, int rawMin, int rawMax, int deadband, int16_t outMax) {
  int lo = rawMin + deadband;
  int hi = rawMax;
  if (hi <= lo)       return 0;
  if (raw <= lo)      return 0;
  if (raw >= hi)      return outMax;
  long v = (long)(raw - lo) * outMax / (hi - lo);
  if (v < 0) v = 0;
  if (v > outMax) v = outMax;
  return (int16_t)v;
}

int16_t applySpeedLimiter(int16_t baseTorque, int16_t measuredSpeedAbs,
                          int rawPedal, const Tunables &t, float &iError) {
  if (!t.limitingEnabled) {
    iError = 0.0f;
    return baseTorque;
  }

  int32_t speedError = (int32_t)measuredSpeedAbs - t.speedCeiling;
  int16_t torque = baseTorque;

  if (speedError > 0) {
    // Overspeeding: accumulate and bleed torque off smoothly.
    iError += (float)speedError;
    if (iError > t.iErrorMax) iError = t.iErrorMax;
    float reduction = speedError * t.pGain + iError * t.iGain;
    long out = (long)baseTorque - (long)reduction;
    torque = (int16_t)(out < 0 ? 0 : out);
  } else if (rawPedal > t.hillPedalThresh && speedError < 0) {
    // Pedal pinned but below ceiling: likely a hill/obstacle. Let the integral
    // (negative) add torque up to the anti-windup floor.
    iError += (float)speedError;              // speedError is negative here
    if (iError < t.iErrorMin) iError = t.iErrorMin;
    long out = (long)baseTorque - (long)(iError * t.iGain);   // -neg => boost
    torque = (int16_t)(out < 0 ? 0 : out);
  } else {
    // Normal driving well within limits: reset the accumulator.
    iError = 0.0f;
  }
  return torque;
}

int16_t applyLaunchCap(int16_t torque, int16_t measuredSpeedAbs, int16_t launchCap,
                       int16_t launchSpeedThresh) {
  if (measuredSpeedAbs < launchSpeedThresh && torque > launchCap) {
    return launchCap;
  }
  return torque;
}

int16_t applyRamp(int16_t current, int16_t target, uint16_t rampMsFullScale, uint16_t loopMs) {
  if (rampMsFullScale == 0) return target;
  // Max torque units of change allowed this tick for a full-scale (1000) sweep.
  long maxStep = (long)TORQUE_ABS_MAX * loopMs / rampMsFullScale;
  if (maxStep < 1) maxStep = 1;
  long diff = (long)target - current;
  if (diff >  maxStep) diff =  maxStep;
  if (diff < -maxStep) diff = -maxStep;
  return (int16_t)(current + diff);
}
