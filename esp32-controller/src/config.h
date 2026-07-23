// ============================================================================
//  config.h - all hardware pins, WiFi, and default tunables in one place.
//  Everything a specific vehicle build needs to change lives here.
//  See ../../esp32-torque-control.spec.md for the design rationale.
// ============================================================================
#pragma once

// -------------------------- Serial link to hoverboard -----------------------
// Wire ESP32 UART2 to the LEFT sensor cable (VARIANT_USART, CONTROL/FEEDBACK
// on USART2). Both sides are 3.3V logic. Cross TX<->RX and share GND.
#define HB_SERIAL_BAUD 115200
#define HB_RX_PIN 16 // ESP32 RX  <- hoverboard TX
#define HB_TX_PIN 17 // ESP32 TX  -> hoverboard RX

// How often we stream a command (heartbeat). The board times out at ~0.8 s.
#define CONTROL_PERIOD_MS 20

// If no valid feedback frame is seen for this long, treat the link as down and
// command zero torque (host-side watchdog; the firmware has its own dead-man).
#define LINK_TIMEOUT_MS 500

// -------------------------- Debug logging (USB / UART0) ---------------------
// Prints a live status line to the USB serial monitor (Serial / UART0), which
// is independent of the Serial2 link to the hoverboard. View it with:
//   pio device monitor -e esp32dev
// Set to 0 to silence once bring-up is done.
#define DEBUG_LOG 0
#define DEBUG_LOG_PERIOD_MS 500

// -------------------------- Pedals (throttle + brake) -----------------------
// Two analog pedals, each on its own ADC1 pin (ADC1 works while WiFi is on;
// GPIO34/35 are input-only). Calibrate MIN/MAX to each sensor.
#define THROTTLE_PIN 34
#define THROTTLE_RAW_MIN 900  // raw analogRead released
#define THROTTLE_RAW_MAX 2090 // raw analogRead fully pressed
#define THROTTLE_DEADBAND_RAW 60

#define BRAKE_PIN 35
#define BRAKE_RAW_MIN 900
#define BRAKE_RAW_MAX 2090
#define BRAKE_DEADBAND_RAW 60
// Absolute brake ceiling. Per-profile brakeTorqueMax (Tunables) is the live knob;
// this only bounds it (and is the authority used for the emergency stop, which
// intentionally brakes at full strength). Lower per-profile values give a gentler
// brake that won't overpower traction and spin the wheels backwards.
#define BRAKE_TORQUE_MAX 1000 // max brake authority (torque applied opposite motion)

// -------------------------- Direction switch --------------------------------
// A switch on a digital pin, read with INPUT_PULLUP. Normally-Open (NO) = the
// contact is OPEN in the "forward" position (reads HIGH) and CLOSED to GND for
// "reverse" (reads LOW). Use a plain GPIO (input-only 34-39 have no pull-ups).
#define DIR_SWITCH_PIN 27

// -------------------------- Motion / direction behaviour --------------------
// Below this measured wheel speed (abs hall rpm) the car is treated as
// launching (torque clamped to launchTorqueCap) AND as safe to change direction.
#define LAUNCH_SPEED_THRESH 40
#define NEAR_STOP_THRESH 30 // <= this (abs rpm) => "stopped": adopt the switch direction

// Braking/decel torque is tapered linearly to zero below this speed (abs rpm) so
// the car eases to a stop instead of being driven past zero into reverse. Higher
// = softer, earlier fade-out near standstill.
#define BRAKE_BLEND_SPEED 80

// Gentle deceleration used to slow the car before a requested reversal is
// allowed ("slow down first, not too hard"). Small = softer. The brake pedal
// can be firmer than this; this is only the automatic pre-reversal slow-down.
#define DIR_CHANGE_BRAKE_TORQUE 150

// Drive-wheel diameter (mm). Only used to convert rpm <-> km/h in the web UI;
// the control loop is entirely in rpm. 6.5" hub ~165, 8.5" ~216, 10" ~254.
#define WHEEL_DIA_MM 165

// -------------------------- Calibration defaults ----------------------------
// The pedal-mapping and motion-threshold values above are DEFAULTS only: they
// seed the global Calibration blob on first boot (empty flash). After that the
// live values come from NVS and are editable in the web UI. Field order must
// match struct Calibration in shared_state.h.
#define CALIBRATION_DEFAULT {                                  \
    THROTTLE_RAW_MIN, THROTTLE_RAW_MAX, THROTTLE_DEADBAND_RAW, \
    BRAKE_RAW_MIN, BRAKE_RAW_MAX, BRAKE_DEADBAND_RAW,          \
    LAUNCH_SPEED_THRESH, NEAR_STOP_THRESH, BRAKE_BLEND_SPEED,  \
    DIR_CHANGE_BRAKE_TORQUE, WHEEL_DIA_MM}

// Sign calibration (set on the bench).
//  * SPEED_L_SIGN / SPEED_R_SIGN: the two hub motors are mounted mirror-image, so
//    they report OPPOSITE hall-speed signs for the same physical direction. These
//    normalise each wheel so both are positive when the car rolls forward. From
//    the bench log (roll both wheels forward): L was +, R was - => +1 / -1.
//    Set them so both wheels' contributions have the SAME sign when rolling one way.
//  * SPEED_SIGN: overall polarity so measured speed is POSITIVE when a POSITIVE
//    command is sent. If pressing the brake ACCELERATES the car, flip this to -1.
//  * FORWARD_SIGN: which command sign the "forward" switch position drives.
//    If forward/reverse feel swapped, flip this to -1.
#define SPEED_L_SIGN (+1)
#define SPEED_R_SIGN (-1)
#define SPEED_SIGN (+1)
#define FORWARD_SIGN (+1)

// -------------------------- WiFi SoftAP + web security ----------------------
// The ESP32 hosts its own network; the phone connects directly (default URL
// http://192.168.4.1). AP_PASSWORD "" = OPEN network (no password) for easy
// access. To secure it, set a password of >= 8 characters (enables WPA2).
#define AP_SSID "Hovercar BigBoy"
#define AP_PASSWORD "" // "" = open AP; >=8 chars enables WPA2
#define AP_CHANNEL 1
#define AP_MAX_CLIENTS 4

// Shared secret required for every state-changing POST (sent as the "token"
// form field). "" = no token required (open control) - convenient for bring-up.
// Set a long random value to require it (defence-in-depth for altering limits).
// NOTE: server-side clamping of all values to the safe ranges above is ALWAYS
// enforced regardless of this token.
#define API_TOKEN ""

// -------------------------- Safety clamp ranges -----------------------------
// Hard bounds the web layer clamps every incoming value to, regardless of what
// a client sends. These bound the ESP32's authority; the hoverboard's config.h
// caps (I_MOT_MAX etc.) are the ultimate hardware ceiling below these.
#define TORQUE_ABS_MAX 1000    // matches the board's +/-1000 command range
#define SPEED_CEILING_MAX 1000 // rpm; keep <= board N_MOT_MAX
#define PGAIN_MAX 20.0f
#define IGAIN_MAX 5.0f
#define RAMP_MS_MIN 150 // never faster than the board slew ceiling (~167 ms)
#define RAMP_MS_MAX 3000

// Bounds the web layer clamps the global Calibration fields to. Pedal raw values
// are 12-bit ADC counts; the speed thresholds are abs wheel rpm.
#define ADC_RAW_MAX 4095     // 12-bit ADC full scale (analogReadResolution(12))
#define DEADBAND_RAW_MAX 500 // sane ceiling for a low-end pedal deadband
#define SPEED_THRESH_MAX 500 // rpm ceiling for launch / near-stop / blend thresholds
#define WHEEL_DIA_MM_MIN 50   // smallest sane drive-wheel diameter (mm)
#define WHEEL_DIA_MM_MAX 1000 // largest sane drive-wheel diameter (mm)

// -------------------------- Default drive profiles --------------------------
// The car BOOTS into the most restrictive profile (index 0 = "child") at zero
// torque. Profiles are starting points - all values are tunable live over the
// web UI. rampMsFullScale must stay >= RAMP_MS_MIN (kept below the board slew).
//
// Fields: name, maxTorque, launchTorqueCap, speedCeiling, pGain, iGain,
//         iErrorMax, iErrorMin, hillPedalThresh(raw), rampMsFullScale,
//         limitingEnabled, brakeTorqueMax, reverseMaxTorque,
//         reverseSpeedCeiling, reverseRampMs
//
// brakeTorqueMax is kept <= maxTorque so the car never brakes harder than it can
// drive (prevents the wheels being spun backwards past traction). Reverse is
// deliberately weaker/slower/gentler than forward for a kids' car.
//
// speedCeiling / reverseSpeedCeiling here are rpm AT THE REFERENCE WHEEL
// (WHEEL_DIA_MM). PresetStore rescales them to the actual wheel at boot so each
// profile's TOP SPEED stays constant in km/h regardless of the fitted tire
// (e.g. child ~10 km/h fwd / ~4.7 km/h rev at 165 mm). Change the tire, keep the
// speed.
#define PROFILE_CHILD {"child", 400, 300, 320, 2.0f, 0.1f, 1000.0f, -700.0f, 3000, 700, true, 300, 250, 150, 900}
#define PROFILE_RACE {"race", 1000, 1000, 900, 1.5f, 0.05f, 800.0f, -400.0f, 3500, 200, false, 900, 600, 400, 400}
