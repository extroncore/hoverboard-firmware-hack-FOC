# ESP32 UART Torque Controller + Phone Web Interface — Spec

> **Status: implemented & bench-validated (2026-07).** Serial link verified both ways, torque
> mode + brake + direction working, presets persist in flash. Remaining: on-vehicle tuning and
> the "secure before real use" hardening. This doc reflects the **as-built** system plus a few
> noted future options.

## 1. Context & Goal

Make an **ESP32** the brain of the hoverboard-powered vehicle. The hoverboard mainboard
(STM32) runs this FOC firmware as a "dumb" current amplifier — **FOC torque mode** with the
hardware current/speed limits acting as fixed safety ceilings. All *dynamic* behaviour (throttle
shaping, speed governing, launch caps, driving profiles, braking, direction) lives on the ESP32,
which:

- reads a **throttle pedal, a brake pedal, and a direction switch**,
- runs the control loop + torque/brake/direction state machine,
- streams commands to the mainboard over UART and reads telemetry back,
- hosts a **phone web interface** over its own WiFi for presets, live tuning, an emergency stop,
  and a live dashboard.

### Locked-in design decisions
1. **Limits:** compile-time caps in the board's `config.h` are hard safety ceilings; the ESP32 does
   **all** dynamic limiting. No runtime firmware-parameter protocol. (First flash keeps **stock**
   caps 15 A / 17 A / 1000 rpm — see §3.3.)
2. **Fail-safe:** a firmware patch zeroes the torque request on serial timeout, **plus** an
   ESP32-side link watchdog (defense in depth).
3. **Connectivity:** ESP32 runs as a **SoftAP**; the phone connects directly. Currently **open**
   (no password/token) for bring-up — see §6.
4. **Code home:** `esp32-controller/` in this repo, built via the root `platformio.ini` as the
   `esp32dev` environment.

> Macro/struct names below are the **real identifiers in this codebase**, not the placeholders
> from the initial notes (no `TORQUE_MODE`, `CONTROL_SERIAL_USART`, `TOTAL_CURRENT_LIMIT_MAX`,
> or `PHASE_CURRENT_LIMIT_MAX`).

---

## 2. Architecture

```
[Throttle ADC] [Brake ADC] [Dir switch] ─▶ ESP32 (control task @20ms, core 1)
                                              │  Serial2 @115200 8N1  (8-byte SerialCommand)
                                              │◀ (18-byte SerialFeedback: telemetry)
                                              ▼
                                     Hoverboard STM32 (VARIANT_USART, FOC + TRQ_MODE)
                                              │
                                        [L / R BLDC motors]  (mirror-mounted)

  Phone ──WiFi (ESP32 SoftAP, open)──▶ ESP32 web server (loop task) — presets, tuning, E-STOP, dashboard
```

| Component | Role |
|---|---|
| **STM32 mainboard** | Interprets `speed` as a torque/current request, applies only the fixed hardware caps + the on-board slew limiter, streams telemetry. No dynamic logic. |
| **ESP32** | Pedal/switch sensing, torque shaping, brake & direction state machine, PI speed limiter, presets (NVS), emergency stop, link watchdog, web server. Real-time control runs in a FreeRTOS task pinned to core 1; the web server runs in `loop()`. |

---

## 3. Firmware Changes (STM32 — `Inc/config.h`, `Src/main.c`, root `platformio.ini`)

### 3.1 USART host control — `VARIANT_USART` (done)
`VARIANT_USART` wires the **left sensor cable** (USART2 = `PA2` TX / `PA3` RX) for bidirectional
host control: `CONTROL_SERIAL_USART2 0` + `FEEDBACK_SERIAL_USART2`, with
`PRI_INPUT1/2 = 3, -1000, 0, 1000, 0`. Selected via `default_envs = VARIANT_USART` in the root
`platformio.ini`.

### 3.2 Control mode → FOC Torque (done)
`CTRL_MOD_REQ` overridden to `TRQ_MODE` **inside the `VARIANT_USART` block** (so other variants
keep `VLT_MODE`). `CTRL_TYP_SEL` stays `FOC_CTRL`. Torque dispatch is gated on `z_ctrlModReq == 3`
in `BLDC_controller.c`.

### 3.3 Hardware safety caps (kept stock for first flash)
`config.h` — `I_MOT_MAX 15 A` (per-motor phase), `I_DC_MAX 17 A` (DC-link chopping; keep ≈
`I_MOT_MAX + 2`), `N_MOT_MAX 1000 rpm`. **Left at stock** for the first flash (the ESP32 governs
below them and there was no dynamic limiter yet at bring-up). Raise later, conservatively, to the
vehicle's motor/battery/wiring limits. `FIELD_WEAK_ENA` off.

### 3.4 Serial dead-man fail-safe (done)
Gap: on link loss the stock firmware only beeps; `commandL/R` are only overwritten on a valid
frame, so the last torque command would persist forever. Patch (in the main loop, right after
`readCommand()`, guarded by `CONTROL_SERIAL_USART2/3`): when `timeoutFlgSerial` is set
(`SERIAL_TIMEOUT 160` ≈ 0.8 s) zero `input1/2[].cmd`. The on-board rate limiter then ramps torque
smoothly to 0.

- **Auto-recovers:** the patch only zeroes the *command*, never `enable`, so on the first valid
  frame after the ESP32 returns, `usart_process_command()` clears the flag and normal control
  resumes with no re-arm handshake.

### 3.5 Smoothing split — board = safety slew floor, ESP32 = feel (kept stock)
On-board `rateLimiter16` + `filtLowPass32` run every `DELAY_IN_MAIN_LOOP` (5 ms) tick. `RATE 480`
→ 30 units/tick → ~167 ms full-scale (0→1000) — a hard slew ceiling no ESP32 command can exceed.
`FILTER 0.1` just rounds corners. Both **kept at stock**: the board is the anti-spike safety floor;
each ESP32 preset ramps at or below it (`rampMsFullScale`, Race ~200 ms ≥ board 167 ms so it isn't
clipped; Child ~700 ms). Arming rule respected: the board only enables motors when the initial
command is `< 50`, so the ESP32 boots commanding ~0.

---

## 4. Serial Protocol

Full-duplex binary, **115200 8N1**. ESP32 sends every ~20 ms as a heartbeat. Start frame
`0xABCD`. Implemented on the ESP32 in `protocol.h` (packed structs + XOR checksums, with
`static_assert` on the 8/18-byte sizes) and `HoverboardLink.{h,cpp}` (send + incremental,
start-frame-synced, checksum-checked RX parser with valid/error frame counters).

### 4.1 Command (ESP32 → STM32) — 8 bytes
`start(0xABCD)`, `steer` (−1000..1000 → input1; sent 0, steering unused), `speed` (−1000..1000 →
input2 = **torque request**), `checksum = start ^ steer ^ speed`.

### 4.2 Feedback (STM32 → ESP32) — 18 bytes (odometry OFF)
`start`, `cmd1`, `cmd2`, `speedR_meas`, `speedL_meas`, `batVoltage` (centivolts), `boardTemp`
(**tenths of °C** — e.g. 330 = 33.0 °C; it's the STM32 chip sensor, the ESP32 divides by 10 for
display), `cmdLed`, `checksum` (XOR of all preceding). The ESP32 parser must match the board's
`ENABLE_ODOMETRY` flag (18 vs 22 bytes); **v1 keeps it OFF**.

---

## 5. ESP32 Software (`esp32-controller/src/`)

Files: `config.h` (all pins/tunables), `protocol.h`, `HoverboardLink.*`, `control.*` (pure
helpers), `shared_state.h` (mutex-guarded state between tasks), `preset_store.*` (NVS presets),
`WebInterface.*`, `main.cpp` (control task + `loop()`).

### 5.1 I/O pin map (`config.h`)
| Signal | ESP32 pin | Notes |
|---|---|---|
| Serial to board | RX `GPIO16`, TX `GPIO17` | to left cable `PA2`/`PA3`, cross TX↔RX, common GND, 3.3 V |
| Throttle pedal | `GPIO34` (ADC1) | calibrated `THROTTLE_RAW_MIN/MAX` (≈900/3040), deadband 60 |
| Brake pedal | `GPIO35` (ADC1) | `BRAKE_RAW_MIN/MAX` (≈900/3050); `BRAKE_TORQUE_MAX 1000` |
| Direction switch | `GPIO27` | `INPUT_PULLUP`; open (NO) = forward, closed = reverse |

### 5.2 Control loop (~20 ms, `CONTROL_PERIOD_MS`)
Read feedback + link watchdog → snapshot live tunables → read throttle/brake/switch → run the
state machine → shape torque → ramp → send command → publish telemetry.

**Priority order each tick:**
1. **Link lost** (`LINK_TIMEOUT_MS 500`) → torque 0.
2. **E-stop latched** (§6) → ignore inputs, brake to standstill, hold 0.
3. **Brake pedal** → overrules throttle & switch: torque opposite motion, **tapered to 0 near
   standstill** and applied **immediately** (no soft ramp) — see §5.4.
4. **Reversal requested while moving** → ignore throttle, gentle `DIR_CHANGE_BRAKE_TORQUE` (150,
   tapered) decel until near-stop, then adopt new direction.
5. **Normal throttle** → base torque → PI speed limiter → launch cap → ramp → drive in the active
   direction.

### 5.3 Mirror-mounted wheels — speed sign normalization
The two hub motors report **opposite** hall-speed signs for the same direction. Combined motion
speed = `SPEED_SIGN · (SPEED_L_SIGN·speedL + SPEED_R_SIGN·speedR) / 2`, with `SPEED_L_SIGN +1`,
`SPEED_R_SIGN −1` (from the bench log). Without this the naive average canceled to ~0 and broke
braking/limiting/near-stop. Bench calibration: roll forward → both contributions same sign; if
braking accelerates flip `SPEED_SIGN`; if F/R swapped flip `FORWARD_SIGN`.

### 5.4 Braking / no reverse-overshoot
Braking (pedal, e-stop, and the pre-reversal decel) uses an opposing torque that **tapers linearly
to zero below `BRAKE_BLEND_SPEED` (80 rpm)** and is applied **immediately** (bypasses the ESP32
soft ramp; the board's ~167 ms slew still smooths it). This fixed the observed "hard brake kicks
into reverse at standstill" — the old fixed brake torque released too slowly and drove past zero.

### 5.5 Profiles / PI limiter
A preset is a full `Tunables` set: `maxTorque`, `launchTorqueCap`, `speedCeiling`, `pGain`,
`iGain`, `iErrorMax/Min`, `hillPedalThresh`, `rampMsFullScale`, `limitingEnabled`. The PI limiter
proportionally bleeds torque at the ceiling, and an integral term *adds* torque (to max) when the
pedal is pinned but speed is below ceiling (hill/obstacle), with anti-windup.

### 5.6 Presets in NVS flash (`preset_store.*`)
5 slots via `Preferences`. Slots **1–2 read-only built-ins** (Kid=`PROFILE_CHILD`,
Race=`PROFILE_RACE`, regenerated from `config.h` each boot). Slots **3–5 user-editable**, saved to
flash with a name. The **selected slot is persisted**, so the car reboots into its last preset
(fresh flash → Kid). A `NVS_VER` stamp discards stale blobs if the `Tunables` layout changes
(self-healing; no code to remove after first flash). NVS survives app reflashes; only a full erase
resets it.

### 5.7 Debug logging (USB serial, independent of the board link)
`DEBUG_LOG` prints a status line every `DEBUG_LOG_PERIOD_MS` (500) over UART0: link OK/frames/errs,
battery, temp, `spd L/R` + combined `mspd`, raw `thrADC`/`brkADC` (+%), direction, braking, torque.
View with `pio device monitor -e esp32dev`.

### 5.8 Concurrency
Real-time control is a FreeRTOS task pinned to core 1 at higher priority than `loop()`; the web
server + logging run in `loop()`; WiFi stack is on core 0. Cross-task data goes through the
mutex-guarded `SharedState`; presets are touched only from the web/loop task.

---

## 6. Phone Web Interface (`WebInterface.*`)

- **SoftAP** `hovercar`, **currently open** (`AP_PASSWORD ""`) for easy access; ≥8 chars enables
  WPA2. Default page `http://192.168.4.1`.
- **Dashboard (polled every 200 ms, self-scheduling):** preset name, link status, direction
  (active + requested, e.g. `FWD→REV` mid-switch), braking indicator, battery V, board temp,
  `speed L/R`, torque out, throttle %, brake %.
- **Emergency STOP** (big red button at the top): latches an e-stop — the controller ignores all
  inputs and brakes to standstill (tapered). The page shows **"Braking to a stop…"** until
  standstill, then an **Engage** button re-enables input.
- **Presets:** dropdown to select (built-ins marked 🔒). An **Edit / save** panel below shows the
  selected preset's name + limits; for user slots (3–5) you can edit and **Apply (live)** or
  **Save preset** (persist to flash). Built-in slots are read-only. The edit form loads only when
  the selected preset changes, so in-progress edits aren't clobbered by the poll. Fields are in a
  `<form>` with Enter/Next moving to the next field (mobile keyboard navigation).
- **Endpoints:** `GET /api/state`; `POST /api/select`, `/api/limits` (live), `/api/savePreset`,
  `/api/estop`.
- **Captive portal:** wildcard DNS + redirect of the OS connectivity probes auto-opens the page on
  connect (see §6.2 for the trade-off).

### 6.1 Security (OWASP-aligned — this commands a physical vehicle)
- **Server-side clamping of every value to the safe ranges is ALWAYS on**, regardless of auth.
  Preset names are sanitized (alphanumeric/space/`-`/`_`, ≤15 chars) to prevent JSON/HTML injection.
- WPA2 + an `API_TOKEN` for state-changing `POST`s are **supported but off by default for
  bring-up** (open AP, empty token). **Re-enable both before real-world use** (`AP_PASSWORD` ≥8
  chars, random `API_TOKEN`; the token machinery + browser storage are still in place).
- Safe boot defaults (Kid preset, torque 0); no dynamic code eval; static assets in flash.

### 6.2 Captive portal vs. WiFi drift — current choice + future option
The captive portal auto-opens the page by **deliberately failing iOS/Android's internet probe**.
Side effect: iOS flags the AP "No Internet" and may **drift to another known (internet-having)
WiFi** over time. This is inherent to an offline AP — removing the captive portal would *not* fix
the drift (the probe still fails) and would also lose the auto-open.

- **Current (kept as-is):** captive portal on → auto-opens, but the phone may drift. Mitigate on
  the phone: tap "Use Without Internet", or disable Auto-Join for home WiFi while driving.
- **Future option — "report online" (recommended once we want stay-connected):** keep the DNS
  interception but reply to the OS probe URLs (`/hotspot-detect.html`, `/generate_204`,
  `/ncsi.txt`, …) with the expected **"Success"** response so the OS treats the AP as connected and
  **won't drift**. Trade-off: loses auto-open — open `http://192.168.4.1` manually (bookmark / Add
  to Home Screen). A best-effort hybrid (auto-open once, then report success) is possible but
  unreliable across iOS versions.

---

## 7. Bench Testing Notes

- Bare board idle ~0.1–0.3 A; the boot chime spikes current and trips low CC limits.
- Bench supply: **36–40 V**, CC ≥ **1.0–1.5 A** bare / ≥ **3 A** for free-spinning motors in the air.
- ⚠️ **Never** load-test or ride on a bench supply — torque spikes exceed 10 A and trip protection.
- With no motors/hall sensors connected, the board raises a motor-fault (`z_errCode`) and beeps
  (1 low beep, repeating) with `enable = 0` — expected; clears once motors + halls are wired.

---

## 8. Build & Flash

Both firmwares live in the root `platformio.ini` (`src_dir = .`, per-env `build_src_filter`).

- **STM32:** `pio run -e VARIANT_USART -t upload` (ST-Link).
- **ESP32:** `~/.platformio/penv/bin/pio run -e esp32dev -t upload` — **must** use the penv `pio`;
  the pyenv `pio` shim hits a stray `fatfs` package that breaks the espressif32 builder. Monitor:
  `... device monitor -e esp32dev`.

Files touched: `Inc/config.h`, `Src/main.c`, root `platformio.ini` (STM32); the whole
`esp32-controller/src/` tree (ESP32).

---

## 9. Verification status

1. ✅ Both firmwares build; STM32 unchanged in size after the src_dir refactor.
2. ✅ **Comms bring-up:** `link=OK`, rising `rx`, plausible battery/temp — STM32→ESP32 verified;
   no serial-timeout beep → ESP32→STM32 verified.
3. ⏳ **Fail-safe:** kill ESP32 TX → motors stop within ~1 s (needs motor-in-air confirmation).
4. ⏳ **Limiter / launch cap** — on-vehicle tuning pending.
5. ⏳ **Slew ceiling** 0→1000 step clamp — pending.
6. ✅ **Web UI:** presets/select/save, e-stop, live tuning, server-side clamping working; dashboard
   at 200 ms.
7. ✅ **Brake overshoot** into reverse fixed (§5.4); ✅ mirror-wheel sign fixed (§5.3); ✅ board-temp
   units fixed.

---

## 10. Open Items / Future

- **Raise the hardware caps** (`I_MOT_MAX`/`I_DC_MAX`/`N_MOT_MAX`) from stock to the vehicle's real
  limits, conservatively, once dynamic limiting is trusted on-vehicle.
- **Tune** per-preset ramps, speed ceilings, PI gains, `BRAKE_BLEND_SPEED`, launch cap on-vehicle.
- **Secure** before real use: set `AP_PASSWORD` (WPA2) and a random `API_TOKEN`.
- **WiFi drift:** consider the "report online" captive option (§6.2) for stay-connected driving.
- Confirm `SPEED_SIGN`/`FORWARD_SIGN` polarity under throttle; single vs dual-motor wiring.
