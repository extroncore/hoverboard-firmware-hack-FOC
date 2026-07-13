# ESP32 UART Torque Controller + Phone Web Interface — Spec

## 1. Context & Goal

Make an **ESP32** the brain of the hoverboard-powered vehicle. The hoverboard mainboard
(STM32) runs this FOC firmware as a "dumb" current amplifier — **FOC torque mode** with all
hardware limits set **high** (maximum safe headroom). All *dynamic* limiting (speed governing,
launch-torque caps, Child vs Race driving profiles) moves up to the ESP32, which:

- reads the pedal sensor,
- runs the control loop and torque-shaping logic,
- streams commands to the mainboard over UART (and reads telemetry back),
- hosts a **phone-accessible web interface** over its own WiFi for live tuning of every limit.

### Locked-in design decisions
1. **Limits:** high compile-time caps in `config.h` act as hard *safety ceilings*; the ESP32 does
   **all** dynamic limiting. No runtime firmware-parameter protocol.
2. **Fail-safe:** a small firmware patch makes a serial timeout force torque → 0 / motors
   disabled, **plus** an ESP32-side watchdog (defense in depth).
3. **Connectivity:** ESP32 runs as a **SoftAP** (its own WiFi); the phone connects directly.
4. **Code home:** a new `esp32-controller/` PlatformIO subfolder in this repo.

> The macro/struct names below are the **real identifiers verified in this codebase**, not the
> approximate placeholders from the initial notes (there is no `TORQUE_MODE`,
> `CONTROL_SERIAL_USART`, `TOTAL_CURRENT_LIMIT_MAX`, or `PHASE_CURRENT_LIMIT_MAX` in the code).

---

## 2. Architecture

```
[Pedal / sensors] ─▶ ESP32 (control loop, profiles, PI speed limiter, watchdog)
                        │  Serial2 @115200 8N1, ~20 ms  (8-byte SerialCommand)
                        │◀ (18-byte SerialFeedback: telemetry)
                        ▼
                   Hoverboard STM32  (VARIANT_USART, FOC + TRQ_MODE, limits high)
                        │
                   [L / R BLDC motors]

  Phone ──WiFi (ESP32 SoftAP, WPA2)──▶ ESP32 web UI  (live limit/profile tuning + dashboard)
```

| Component | Role |
|---|---|
| **STM32 mainboard** | Interprets the `speed` command as a torque/current request, applies only the fixed hardware safety caps, streams telemetry back. No dynamic logic. |
| **ESP32** | Pedal sensing, driving state machine (Child vs Race), torque shaping, PI-based smart speed limiting, link watchdog, and the web server. |

---

## 3. Firmware Changes (STM32 — `Inc/config.h` + `Src/main.c`)

### 3.1 Enable USART host control — build as `VARIANT_USART`
The `VARIANT_USART` block already wires the **left sensor cable** for bidirectional host control
(`Inc/config.h:324-337`):

- `CONTROL_SERIAL_USART2 0` — RX commands, priority 0 = primary (`config.h:327`)
- `FEEDBACK_SERIAL_USART2` — TX telemetry (`config.h:328`)
- `PRI_INPUT1/2 = 3, -1000, 0, 1000, 0` — auto-detect, ±1000, no deadband (`config.h:335-336`)

Select the variant via the existing `platformio.ini` / Makefile build flag. USART2 = left cable;
USART3 stays disabled (optional second input).

### 3.2 Control mode → FOC Torque
Currently `CTRL_MOD_REQ = VLT_MODE` (`config.h:158`). Change to:

```c
#define CTRL_TYP_SEL   FOC_CTRL   // already the default (config.h:157)
#define CTRL_MOD_REQ   TRQ_MODE   // TRQ_MODE = 3; TORQUE is only valid for FOC
```

Torque dispatch is gated on `z_ctrlModReq == 3` at `Src/BLDC_controller.c:1790`.

*Option (default off):* `ELECTRIC_BRAKE_ENABLE` (`config.h:175-177`) makes releasing the pedal
brake instead of freewheel — worth evaluating for a kids' car.

### 3.3 Raise the hard safety caps (headroom; ESP32 governs below these)
`config.h:161-164` — these become the physical protection ceiling:

| Macro | Meaning (notes' name) | Default |
|---|---|---|
| `I_MOT_MAX` | per-motor **phase** current ("PHASE_CURRENT_LIMIT_MAX") | 15 A |
| `I_DC_MAX` | **DC-link/total** current chopping protection ("TOTAL_CURRENT_LIMIT_MAX") | 17 A |
| `N_MOT_MAX` | max motor speed | 1000 rpm |

Keep the rule `I_DC_MAX ≈ I_MOT_MAX + 2 A`. Pick "high but safe" values per the specific motors,
battery, and wire gauge.

> ⚠️ **These protect the hardware.** Do not set them beyond what the wiring, battery BMS, and
> inverter MOSFETs can survive. The ESP32 keeps normal operation well below them.

*Option (default off):* `FIELD_WEAK_ENA 1` (`config.h:167`) raises top speed at the cost of
efficiency/heat.

### 3.4 Fail-safe patch (safety-critical, **new**)
**Verified gap:** on serial link loss the firmware only beeps (`Src/main.c:581-582`).
`commandL/R` are only overwritten on a *valid* frame, so the **last torque command persists
indefinitely** if the ESP32 stops sending. The `enable = 0` cases (`main.c:405-429`) are
power-button/poweroff paths — **not** a serial dead-man.

**Patch:** when `timeoutFlgSerial` is set (produced by `handleTimeout()` in `Src/util.c:943-1004`,
after `SERIAL_TIMEOUT 160` ≈ 0.8 s, `config.h:654`), force the input command / torque request to
zero (and/or `enable = 0`) in the main loop *before* it reaches the mixer (`main.c` ~328-374).
Keep it minimal and guarded behind the serial-control build.

### 3.5 Smoothing split — board = safety slew floor, ESP32 = profile feel
This is how we get "profiles on the ESP32" **and** "no sudden dangerous torque spikes."

Verified mechanics (`main.c:328-334`, runs once per `DELAY_IN_MAIN_LOOP` = 5 ms tick, gated at
`main.c:263`):

- **`rateLimiter16`** (`util.c:1682`) — a **hard slew-rate cap**. Max change per tick = `RATE/16`
  real command units. Full-scale (0→1000) ramp ≈ `(1000 / (RATE/16)) × 5 ms`. `RATE = 480`
  → 30 units/tick → **~167 ms**. An instantaneous ESP32 step is physically clamped to this slope.
- **`filtLowPass32`** (`util.c:1658`) — a light 1st-order low-pass (`FILTER = 0.1` ≈ 50 ms τ)
  that only rounds corners.

**Strategy:**
- **Board rate limiter = absolute safety ceiling.** Set `RATE` to the *fastest torque slew we'd
  ever allow* (the most aggressive Race profile + a small margin), ~140–170 ms full-scale
  (`RATE` ≈ 480–560). The board then guarantees no command — however buggy — produces a faster
  torque rise. This is the anti-spike guard.
- **Board `FILTER` stays light** (default `0.1`, or slightly lower). It removes harshness; it does
  not define the ride.
- **ESP32 owns per-profile feel, always at or below the board ceiling.** Child = slow ramp + more
  smoothing; Race = fast ramp approaching (never exceeding) the board slope. Profiles differentiate
  *downward* from the hard ceiling.
- **Double-ramping is intended:** in normal driving the slower ESP32 ramp dominates; the board
  limiter only bites on pathological steps. Effective ramp = the slower of the two.

| Layer | Full-scale torque ramp | Set via |
|---|---|---|
| Board safety ceiling | ~140–170 ms (fastest allowed) | `RATE` in `config.h` |
| ESP32 "Race" profile | ~170–250 ms | ESP32 software ramp |
| ESP32 "Child" profile | ~400–800 ms + heavier smoothing | ESP32 software ramp |

**Arming rule to respect:** the board only enables motors when the initial command is `< 50`
(`main.c:270-271`) and resets its filters on enable (`main.c:274`). The ESP32 must therefore
**boot and re-arm commanding ~0 torque**.

---

## 4. Serial Protocol (ESP32 side)

Full-duplex binary, **115200 8N1** (`Src/setup.c:72,99`). The ESP32 sends every ~20 ms as a
heartbeat. Start frame `SERIAL_START_FRAME = 0xABCD` (`config.h:652`).

### 4.1 Command (ESP32 → STM32) — **8 bytes** (`Inc/util.h:38-43`)
```c
typedef struct {
  uint16_t start;     // 0xABCD
  int16_t  steer;     // -1000..1000  → input1 (steering; set 0 if single-motor / tank unused)
  int16_t  speed;     // -1000..1000  → input2 (TORQUE request in TRQ_MODE)
  uint16_t checksum;  // start ^ steer ^ speed        (verified Src/util.c:1296)
} SerialCommand;
```
`speed` maps to `r_inpTgt`; ±1000 ≈ ±`I_MOT_MAX` torque. Board smoothing per §3.5 still applies.

### 4.2 Feedback (STM32 → ESP32) — **18 bytes** default (`Src/main.c:125-139`)
```c
typedef struct {
  uint16_t start;        // 0xABCD
  int16_t  cmd1;         // processed input1
  int16_t  cmd2;         // processed input2
  int16_t  speedR_meas;  // right rpm (rtY_Right.n_mot)
  int16_t  speedL_meas;  // left rpm
// int16_t wheelR_cnt, wheelL_cnt;  // ONLY if ENABLE_ODOMETRY (+4 bytes → 22 total)
  int16_t  batVoltage;   // calibrated battery voltage
  int16_t  boardTemp;    // board temperature (°C)
  uint16_t cmdLed;
  uint16_t checksum;     // XOR of all preceding fields (Src/main.c:538-542)
} SerialFeedback;
```

> The ESP32 parser **must** match the board's `ENABLE_ODOMETRY` build flag (18 vs 22 bytes) or the
> frame length and checksum won't line up. **v1 convention: odometry OFF (18 bytes).**

TX is DMA at ~10 ms cadence (`main.c:520-556`); RX is DMA + UART IDLE-line (`util.c:1091,1159`).

---

## 5. ESP32 Software (`esp32-controller/`)

- **Control loop (~20 ms):** read pedal (`analogRead`) → map to base torque → apply active
  profile → run smart speed limiter → clamp → send `SerialCommand`. Parse `SerialFeedback` for
  the dashboard.
- **Driving profiles / state machine (Child vs Race):** a profile is a set of tunables — max
  torque, launch-torque cap, speed ceiling, PI gains, ramp time. **Boot into the most restrictive
  profile at torque 0.**
- **Smart speed limiter (PI on the ESP32):** proportional cap at the speed ceiling, plus an
  integral term that only *adds* torque (up to max) when the pedal is pinned but speed is below
  ceiling (hill/obstacle), with anti-windup so it can't overshoot. (The initial notes' snippet is
  the starting point — reworked with clear naming and a guarded integral.)
- **Link watchdog:** if feedback stops or the ESP32's own loop stalls, command torque 0. This is
  the host half of the defense-in-depth pair with the §3.4 firmware patch.
- **Wiring:** `Serial2` on chosen GPIOs to the left sensor cable (TX/RX/GND). Common ground;
  both sides are 3.3 V logic.

---

## 6. Phone Web Interface (SoftAP)

- ESP32 **SoftAP** with a **WPA2 password** (not open). Dashboard served locally.
- **Endpoints:** `GET` telemetry/state (JSON); `POST` profile switch; `POST` per-limit updates.
- **Dashboard:** `batVoltage`, `boardTemp`, speed in km/h (from `speedL_meas` + wheel size),
  active profile, and link/watchdog status.

### 6.1 Security (OWASP-aligned — this commands a physical vehicle)
- **Validate & clamp every limit server-side** on the ESP32 to the hardware-safe range; never
  trust client values. Reject out-of-range input with an explicit error.
- WPA2 on the AP; a simple auth token for state-changing `POST`s; same-origin only.
- **Safe defaults on boot/reset:** restrictive profile, torque 0. The web UI is a *tuning*
  surface — never the sole safety mechanism (physical caps + fail-safe still apply).
- No dynamic code eval; static assets embedded in flash; rate-limit the control endpoints.

---

## 7. Bench Testing Notes

- Bare board idle draw ~0.1–0.3 A; the boot chime causes a brief current spike that trips low
  current limits.
- Bench supply: **36–40 V**, CC limit **≥ 1.0–1.5 A** for a bare board / **≥ 3 A** for
  free-spinning motors in the air.
- ⚠️ **Never** load-test or ride while on a bench supply — torque spikes exceed 10 A and trip the
  supply's protection.

---

## 8. Files Touched (implementation phases, after this spec)

- `Inc/config.h` — §3.1–3.3, §3.5 (`VARIANT_USART`, `TRQ_MODE`, raised caps, `RATE`/`FILTER`).
- `Src/main.c` — §3.4 fail-safe patch.
- `platformio.ini` — variant/build flag.
- `esp32-controller/` — new PlatformIO project (§5, §6).

---

## 9. Verification

1. **Firmware build:** compile `VARIANT_USART` + `TRQ_MODE`; confirm the binary builds.
2. **Protocol bring-up:** ESP32 streams a fixed small `speed`; scope/log confirms the board
   replies with valid 18-byte feedback (checksum OK) and motors respond (wheels in the air,
   CC-limited supply).
3. **Fail-safe (§3.4):** with motors spinning in the air, kill ESP32 TX → motors must stop within
   ~1 s and the 3-beep serial-timeout pattern sounds.
4. **Limiter (§5):** verify the speed ceiling holds and the integral term boosts torque when
   loaded-but-pinned; confirm anti-windup doesn't overshoot.
5. **Slew ceiling (§3.5):** command an instantaneous 0→1000 step from the ESP32; log
   `cmd2`/`speedL_meas` and confirm the torque rise is clamped to the board `RATE` slope (no
   instantaneous spike). Then confirm Child vs Race profiles produce visibly different (slower)
   ramps below that ceiling.
6. **Web UI (§6):** connect the phone to the SoftAP; change a limit; confirm server-side clamping
   and that an out-of-range value is rejected; confirm telemetry updates live.

---

## 10. Open Risks / To Confirm During Implementation

- Exact "high but safe" values for `I_MOT_MAX` / `I_DC_MAX` / `N_MOT_MAX` depend on the specific
  motors, battery, and wiring — pick conservatively and validate on the bench.
- Final `RATE` safety-ceiling value and per-profile ESP32 ramp times (§3.5) — tune on the bench.
- ESP32 ADC pedal noise/calibration; single vs dual motor (`TANK_STEERING`) wiring.
