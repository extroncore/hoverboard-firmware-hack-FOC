// ============================================================================
//  main.cpp - wiring of the ESP32 hovercar controller.
//
//  Two concurrent contexts:
//   * controlTask  - pinned to core 1, fixed CONTROL_PERIOD_MS cadence. Reads
//                    the pedal, shapes torque, streams the command, parses
//                    feedback, runs the link watchdog. This is safety-critical
//                    and must not be blocked by the network.
//   * loop()       - runs the web server (WiFi stack lives on core 0).
//
//  Boots into the most restrictive profile (index 0) at zero torque.
// ============================================================================
#include <Arduino.h>
#include "config.h"
#include "protocol.h"
#include "shared_state.h"
#include "control.h"
#include "preset_store.h"
#include "HoverboardLink.h"
#include "WebInterface.h"

SharedState   g_state;
WebInterface  g_web;
HoverboardLink g_link;

static void controlTask(void *) {
  const TickType_t period = pdMS_TO_TICKS(CONTROL_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  pinMode(DIR_SWITCH_PIN, INPUT_PULLUP);

  float   iError       = 0.0f;         // speed-limiter integral accumulator
  int16_t torqueOut    = 0;            // ramped, signed torque being commanded
  int8_t  activeCmdDir = FORWARD_SIGN; // command sign the throttle drives now

  for (;;) {
    // 1) Drain feedback and evaluate the host-side link watchdog.
    g_link.poll();
    bool linkOk = (millis() - g_link.lastRxMs()) < LINK_TIMEOUT_MS;
    const SerialFeedback &fb = g_link.feedback();

    // 2) Snapshot live tunables + global calibration (both set by the web task).
    Tunables    t = g_state.getTunables();
    Calibration c = g_state.getCalibration();

    // 3) Read inputs. The wheels are mirror-mounted (opposite hall-speed signs for
    //    the same direction), so normalise each side before combining. Result is in
    //    the command frame: a POSITIVE command -> POSITIVE motionSpeed (see SPEED_SIGN).
    int32_t motionSpeed = SPEED_SIGN *
        ((int32_t)SPEED_L_SIGN * fb.speedL_meas + (int32_t)SPEED_R_SIGN * fb.speedR_meas) / 2;
    int16_t speedAbs    = (int16_t)abs(motionSpeed);
    int     motionDir   = (motionSpeed > 0) - (motionSpeed < 0);   // -1 / 0 / +1

    int  throttleRaw   = analogRead(THROTTLE_PIN);
    int  brakeRaw      = analogRead(BRAKE_PIN);
    bool switchForward = (digitalRead(DIR_SWITCH_PIN) == HIGH);     // NO/open = forward

    int16_t throttleTq = mapPedal(throttleRaw, c.throttleRawMin, c.throttleRawMax,
                                  c.throttleDeadband, t.maxTorque);
    int16_t brakeTq    = mapPedal(brakeRaw, c.brakeRawMin, c.brakeRawMax,
                                  c.brakeDeadband, t.brakeTorqueMax);

    int8_t requestedCmdDir = switchForward ? (int8_t)FORWARD_SIGN : (int8_t)(-FORWARD_SIGN);
    bool   estop      = g_state.getEstop();
    bool   configMode = g_state.getConfigMode();
    bool   calibrated = g_state.getCalibrated();

    // 4) Decide the (signed) torque target.
    int16_t target  = 0;
    bool    braking = false;
    bool    fast    = false;   // brake/decel: respond now, skip the soft ramp

    // Standstill latch (hysteresis): once braking has brought the car below
    // NEAR_STOP_THRESH, release the brake and keep it released until the wheels
    // genuinely move again (>= BRAKE_BLEND_SPEED). Near zero the hall speed jitters
    // in sign, so without this the opposing "-motionDir * mag" torque flips every
    // tick and rocks the car back and forth. Mirrors the STM32 reverse-beep latch.
    static bool brakeHeldAtStop = false;
    if      (speedAbs <= c.nearStopThresh)  brakeHeldAtStop = true;
    else if (speedAbs >= c.brakeBlendSpeed) brakeHeldAtStop = false;

    // Opposing torque that tapers to zero near standstill, so we ease to a stop
    // instead of being driven past zero into reverse (overshoot). Once at
    // standstill it releases entirely (brakeHeldAtStop) so it can't rock the car.
    auto brakeToward = [&](int16_t mag) -> int16_t {
      if (brakeHeldAtStop) return 0;
      if (speedAbs < c.brakeBlendSpeed)
        mag = (int16_t)((int32_t)mag * speedAbs / c.brakeBlendSpeed);
      return (int16_t)(-motionDir * mag);   // motionDir is 0 at rest -> 0
    };

    if (!linkOk) {
      // Link lost: fail safe. Zero target, forget accumulated state.
      target = 0;
      iError = 0.0f;
    } else if (estop) {
      // Emergency stop (web STOP button): ignore all inputs and brake to a stop,
      // tapered to zero at standstill, then hold. Cleared by the Engage button.
      braking = true;
      fast    = true;
      iError  = 0.0f;
      target  = brakeToward(BRAKE_TORQUE_MAX);
    } else if (configMode || !calibrated) {
      // Drive inhibited: command zero drive torque (coast). Two cases:
      //  * configMode: the web Config page is open.
      //  * !calibrated: the car has no saved calibration yet. This is enforced
      //    HERE, in the control task, so an uncalibrated car never drives even
      //    with nothing connected to the AP (the web gate is only a convenience).
      // Pedals and the switch are still read and published (for calibration),
      // but nothing can make the cart drive.
      target = 0;
      fast   = true;
      iError = 0.0f;
    } else {
      // Whenever essentially stopped it is safe to adopt the switch direction.
      if (speedAbs <= c.nearStopThresh) activeCmdDir = requestedCmdDir;

      if (brakeTq > 0) {
        // (a) Brake pedal overrules everything: torque opposite motion to stop,
        //     tapered to zero at standstill (never drive through into reverse).
        braking = true;
        fast    = true;
        iError  = 0.0f;
        target  = brakeToward(brakeTq);
      } else if (requestedCmdDir != activeCmdDir && speedAbs > c.nearStopThresh) {
        // (b) Reversal requested while moving: slow down gently first. Throttle
        //     is ignored until we reach near-stop (then branch (a)/(c) resumes).
        braking = true;
        fast    = true;
        iError  = 0.0f;
        target  = brakeToward(c.dirChangeBrakeTorque);
      } else {
        // (c) Normal throttle drive in the active direction. Forward and reverse
        //     have independent caps/ceilings: reverse is usually weaker & slower.
        bool    rev      = (activeCmdDir != (int8_t)FORWARD_SIGN);
        int16_t driveCap = rev ? t.reverseMaxTorque    : t.maxTorque;
        // Remap the pedal against the direction's own cap so full travel = full
        // (reverse) torque, then limit against that direction's ceiling.
        int16_t driveTq  = mapPedal(throttleRaw, c.throttleRawMin, c.throttleRawMax,
                                    c.throttleDeadband, driveCap);
        Tunables eff = t;
        if (rev) eff.speedCeiling = t.reverseSpeedCeiling;
        int16_t base = applySpeedLimiter(driveTq, speedAbs, throttleRaw, eff, iError);
        base = applyLaunchCap(base, speedAbs, t.launchTorqueCap, c.launchSpeedThresh);
        if (base < 0) base = 0;
        if (base > driveCap) base = driveCap;
        target = (int16_t)(activeCmdDir * base);
      }
    }

    // 5) Throttle uses the soft ramp (feel); braking/decel is applied at once so
    //    no stale reverse torque lingers. The board's own slew still smooths both.
    //    Forward and reverse each have their own ramp; braking (fast) skips it.
    bool driveRev = (activeCmdDir != (int8_t)FORWARD_SIGN);
    if (fast) torqueOut = target;
    else      torqueOut = applyRamp(torqueOut, target,
                                    driveRev ? t.reverseRampMs : t.rampMsFullScale,
                                    CONTROL_PERIOD_MS);

    // 6) Stream the command (steer unused -> 0).
    g_link.sendCommand(0, torqueOut);

    // 7) Publish telemetry for the dashboard.
    Telemetry tm{};
    tm.batVoltage_cV = fb.batVoltage;
    tm.boardTemp     = fb.boardTemp;
    tm.speedL        = fb.speedL_meas;
    tm.speedR        = fb.speedR_meas;
    tm.motionSpeed   = (int16_t)motionSpeed;
    tm.torqueSent    = torqueOut;
    tm.linkOk        = linkOk;
    tm.braking       = braking;
    tm.activeForward = (activeCmdDir == (int8_t)FORWARD_SIGN);
    tm.reqForward    = switchForward;
    tm.throttlePct   = (t.maxTorque > 0) ? (int16_t)((long)throttleTq * 100 / t.maxTorque) : 0;
    tm.brakePct      = (t.brakeTorqueMax > 0) ? (int16_t)((long)brakeTq * 100 / t.brakeTorqueMax) : 0;
    tm.throttleRaw   = (int16_t)throttleRaw;
    tm.brakeRaw      = (int16_t)brakeRaw;
    tm.stopped       = (speedAbs <= c.nearStopThresh);
    g_state.setTelemetry(tm);

    vTaskDelayUntil(&lastWake, period);
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[hovercar] booting");

  g_state.begin();
  g_presets.begin();                         // loads presets from NVS + applies last selected

  g_link.begin(Serial2, HB_SERIAL_BAUD, HB_RX_PIN, HB_TX_PIN);
  analogReadResolution(12);

  g_web.begin();
  Serial.printf("[hovercar] SoftAP '%s' up, open http://192.168.4.1\n", AP_SSID);

  // Control task on core 1, higher priority than loop()/web. Its per-tick work
  // is tiny, so it preempts only briefly and leaves core 1 free for the server.
  xTaskCreatePinnedToCore(controlTask, "control", 4096, nullptr, 3, nullptr, 1);
}

void loop() {
  g_web.handle();

#if DEBUG_LOG
  static uint32_t lastLog = 0;
  if (millis() - lastLog >= DEBUG_LOG_PERIOD_MS) {
    lastLog = millis();
    Telemetry tm = g_state.getTelemetry();
    Serial.printf(
      "[hovercar] link=%-4s rx=%lu err=%lu | batt=%.2fV temp=%.1fC spd L/R=%d/%d mspd=%d | "
      "thrADC=%d(%d%%) brkADC=%d(%d%%) dir=%s(req %s) brake=%s tq=%d\n",
      tm.linkOk ? "OK" : "LOST",
      (unsigned long)g_link.rxFrames(), (unsigned long)g_link.rxErrors(),
      tm.batVoltage_cV / 100.0, tm.boardTemp / 10.0, tm.speedL, tm.speedR, tm.motionSpeed,
      tm.throttleRaw, tm.throttlePct, tm.brakeRaw, tm.brakePct,
      tm.activeForward ? "FWD" : "REV", tm.reqForward ? "FWD" : "REV",
      tm.braking ? "ON" : "off", tm.torqueSent);
  }
#endif

  delay(2);
}
