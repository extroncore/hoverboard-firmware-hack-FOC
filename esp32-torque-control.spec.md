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

### 2.1 Why this control split — latency rationale

The split is deliberate: it puts the only latency-critical loop on the board and the
low-bandwidth loops on the ESP32.

**Two loops at very different speeds:**
- **Inner FOC current/torque loop** runs entirely on the STM32 at **16 kHz** PWM, closed locally on
  phase-current ADC. The ESP32 only supplies a torque *setpoint* — so **ESP32 latency has zero
  effect on torque stability/quality**. It is already where it must be.
- **Outer loops** (throttle→torque map, PI speed governor, brake taper, direction state machine)
  run on the ESP32 and are inherently low-bandwidth.

**ESP32 outer-loop latency budget:** feedback data age ≤ ~10 ms (board TX cadence) + 20 ms control
tick quantization + <2 ms serial ≈ **20–50 ms round-trip**. Compare to what those loops control:
vehicle speed dynamics have time constants of **hundreds of ms–seconds**, and human throttle-lag
perception is **~100–150 ms**. The latency is ~5–10× smaller than the dynamics/perception it
serves → **not significant**. The only place it can show is speed-cap overshoot with aggressive PI
gains, which is a tuning knob, not a structural limit.

**Alternative considered — move control onto the board + wire pedals directly to the STM32:**
- *Upside:* removes ~20–30 ms pedal input lag (below perceptual threshold) and trims parts from the
  realtime path.
- *Downside:* the rich behaviour (PI hill-boost limiter, launch cap, brake taper, direction state
  machine, profiles) would have to be reimplemented in STM32 C and **reflashed to change** — losing
  the live web tuning + presets that motivated putting logic on the ESP32; or switching the board to
  its built-in speed mode loses the torque-mode feel + anti-stall behaviour. The board's built-in
  limiting is only compile-time `I_MOT_MAX`/`N_MOT_MAX` clamps, not the smooth governing.
- *Safety:* for a kids' car, today's "ESP32 dies → car coasts to a stop" is **safer** than "pedals
  hardwired → board keeps driving even if the governor/ESP32 is dead."

**Verdict:** keep the current split. If speed governing ever feels loose, cheap fixes that avoid
re-architecting: (1) drop the ESP32 control period to 10 ms (match feedback cadence); (2) raise the
board feedback TX to every 5 ms; (3) use the board's `N_MOT_MAX` as a local, zero-latency hard
speed backstop beneath the ESP32's smooth PI cap (belt-and-suspenders).

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

**Brake authority is per-profile (`brakeTorqueMax`).** The brake pedal maps to `[0..brakeTorqueMax]`,
not a global constant. This fixed a second issue: with a fixed 1000-unit brake, the Kid profile
(drive `maxTorque` 400) braked ~2.5× harder than it could drive, so at speed the opposing torque
overpowered traction and **spun the wheels backwards while the car still rolled forward**. Keeping
`brakeTorqueMax ≤ maxTorque` (Kid 300, Race 900) keeps braking within grip; smoothness comes from the
lower magnitude + the board slew, **not** from ramping the brake (which stays instant so no stale
reverse torque lingers at direction changes). The global `BRAKE_TORQUE_MAX` (1000) remains the web
clamp ceiling and the authority used by the **e-stop**, which intentionally brakes at full strength.

### 5.5 Profiles / PI limiter — direction-aware
A preset is a full `Tunables` set: `maxTorque`, `launchTorqueCap`, `speedCeiling`, `pGain`, `iGain`,
`iErrorMax/Min`, `hillPedalThresh`, `rampMsFullScale`, `limitingEnabled`, **`brakeTorqueMax`**, and a
**reverse triplet `reverseMaxTorque` / `reverseSpeedCeiling` / `reverseRampMs`**. The PI limiter
proportionally bleeds torque at the ceiling, and an integral term *adds* torque (to max) when the
pedal is pinned but speed is below ceiling (hill/obstacle), with anti-windup.

**Forward vs. reverse are tuned independently.** In the normal-drive branch the controller picks the
direction's own cap and ceiling (reverse remaps the pedal against `reverseMaxTorque` and limits
against `reverseSpeedCeiling`), and the soft ramp uses `reverseRampMs` when reversing. Reverse is
deliberately weaker/slower/gentler (Kid 250 tq / 150 rpm / 900 ms; Race 600 / 400 / 400). `launchCap`
and `brakeTorqueMax` stay shared across directions. Because a reversal only completes after the car
reaches near-stop (§5.2 step 4), `activeCmdDir` is already flipped by the time reverse drive runs, so
it's a clean selector; braking is `fast` (no ramp), so `reverseRampMs` only shapes reverse
*acceleration*.

### 5.6 Presets in NVS flash (`preset_store.*`)
5 slots via `Preferences`. Slots **1–2 read-only built-ins** (Kid=`PROFILE_CHILD`,
Race=`PROFILE_RACE`, regenerated from `config.h` each boot). Slots **3–5 user-editable**, saved to
flash with a name. The **selected slot is persisted**, so the car reboots into its last preset
(fresh flash → Kid). A `NVS_VER` stamp discards stale blobs if the `Tunables` layout changes
(self-healing; no code to remove after first flash) — **bumped to 2** when the brake/reverse fields
(§5.5) were added, so pre-existing user presets auto-reset to defaults on the first boot after that
flash. NVS survives app reflashes; only a full erase resets it.

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

- **STM32:** `pio run -e VARIANT_USART -t upload` (ST-Link). Flash base address is
  `0x08000000` (start of STM32F103 internal flash) — use this as the start address for any
  manual flashing (ST-Link Utility, `st-flash write firmware.bin 0x08000000`, etc.).
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
- **Per-wheel torque / differential / traction control** — currently one global torque to both
  wheels; §12 = how to split it, §13 = shared electronic-diff/TC/brake building blocks, §14 = RWD
  (single board) config, §15 = 4×4 (dual board) config.
- **Visual curve editor** for throttle/acceleration response — see §11.

---

## 11. Future feature — visual (draggable) response-curve editor

Idea: instead of tuning acceleration/response with raw numbers, show an interactive **curve** on
the web page whose control point(s) you **drag** to reshape it, with the resulting mapping drawn
live. (Not built — captured here for later.)

### Two parts
1. **The widget** (drawing + dragging).
2. **The model** — a parametric curve the ESP32 actually evaluates and persists. This is the bigger
   half: today throttle→torque is a straight linear map (`mapPedal`) and the ramp is linear, so
   there is no curve to edit yet.

### Widget choice — constrained by offline AP + flash budget
The SoftAP has no internet, so any library must be **embedded in flash and served locally** (no
CDN), and the app partition is already ~74% full. Weight therefore dominates the choice:

| Option | Size (min) | Draggable points | Fit |
|---|---|---|---|
| **Inline SVG + vanilla pointer events** | ~2–4 KB hand-written | yes (self-coded) | ✅ recommended — tiny, offline, touch-friendly, matches the hand-rolled UI |
| uPlot | ~40 KB | no (display only) | dragging still hand-coded |
| Chart.js + chartjs-plugin-dragdata | ~200 KB+ | yes | heavy to embed even gzipped |
| D3.js | ~270 KB | yes | overkill |
| Plotly | ~3 MB | — | no |

**Recommendation:** inline **SVG `<path>` + draggable `<circle>` handles** with pointer events and
`touch-action:none`. A charting library mainly buys axes/tooltips we don't need and costs
50–200 KB of flash.

### Model options
- **Throttle response curve** (most useful): pedal % → torque %.
  - **Single-knob expo/gamma** (`out = in^γ`, like RC radios) — one draggable point bends the whole
    curve; 1 float per preset; low risk. **Suggested starting point.**
  - **Piecewise, 2–3 draggable points** — more expressive; more storage + logic.
- **Acceleration ramp shape** (ease-in/out vs linear) — lower value since the board already slews;
  defer.

### Implementation notes when we build it
- Add the curve parameter(s) to `Tunables`; evaluate in the throttle path (`mapPedal` / control
  loop); extend preset save/load; **bump `NVS_VER`** (old presets auto-reset, per §5.6).
- Keep the existing server-side clamping; clamp curve params to sane ranges too.
- Start with the single-knob expo curve as a proof of concept, then graduate to multi-point if
  wanted.

---

## 12. Per-wheel torque, differential & slip control (exploration)

> Origin: "does the ESP32 send one torque or per-wheel?" It sends **one**. This section captures
> what that means dynamically and what it would take to control the wheels independently. Nothing
> here is built yet.

### 12.1 What actually reaches each motor today
Chain: ESP32 sends **one** signed torque in `speed` with `steer = 0` (§4.1) → firmware maps
`input1 = steer`, `input2 = speed` (`Src/util.c` `usart_process_command` path) → VARIANT_USART runs
the **mixer** (`mixerFcn`, `Src/util.c:1706`), *not* tank steering:

```
cmdR = SPEED_COEFFICIENT·speed − STEER_COEFFICIENT·steer
cmdL = SPEED_COEFFICIENT·speed + STEER_COEFFICIENT·steer     (fixed-point, >>14)
```

With `steer = 0` and the USART defaults (`SPEED_COEFFICIENT 1.0`, `STEER_COEFFICIENT 0.5`),
`cmdL == cmdR == speed`. **Both motors get the identical torque request.** There is exactly **one**
torque number on the wire; the per-wheel split is a mixer identity, not independent control.
(`Src/main.c:373`; `pwml`/`pwmr` then just apply the `INVERT_L/R_DIRECTION` sign.)

### 12.2 What that behaves like dynamically
Each wheel is its own FOC torque loop, so we command **equal torque, independent speed**:
- **Cornering already works** — the outer wheel simply spins faster on its own; nothing forces
  equal speed. No differential needed for geometry.
- Unlike a mechanical **open differential** (equal torque, but the slipping wheel *caps* the torque
  both wheels receive), here the gripping wheel keeps its full commanded torque even if the other
  lifts or slips. Closer to a **spool on the torque side, open on the speed side**.
- Failure mode: on a low-grip patch the slipping wheel **spins up freely** (its FOC keeps pushing
  the commanded current, up to `I_MOT_MAX`) with no feedback cut — there is **no traction control
  today**.

### 12.3 Getting genuinely independent per-wheel torque
Two paths, neither touches the inner loop (the board already runs two independent FOC loops):

1. **Steer-channel differential — no firmware change.** Send `speed` = common torque, `steer` =
   bias; the mixer already yields `cmdL/R = speed ± (STEER/SPEED)·steer`. A symmetric torque split
   around the common command — enough for traction biasing and steering assist. Bounded by the
   coefficient ratio and the ±1000 clamp.
2. **Tank-steering passthrough — one `#define`.** `TANK_STEERING` is *compilable* for USART (it's
   only gated out for HOVERCAR/SKATEBOARD, `Src/main.c:367`): the board then does
   `cmdL = steer; cmdR = speed` with **no mixing** → fully independent per-wheel torque. Reinterpret
   the protocol as `steer → torque_L`, `speed → torque_R` (rename in `protocol.h` + the ESP32
   sender). Cleanest path for real per-wheel control.

Either way, per-motor `I_MOT_MAX` and the on-board slew still apply independently per wheel.

### 12.4 Slip / traction control — the sensing is already free
The feedback frame already carries **per-wheel speed** (`speedL_meas`, `speedR_meas`, §4.2) every
~10 ms, so the sensing half of traction control costs nothing new. An ESP32 slip controller would:
- detect slip = wheel-speed split beyond what cornering geometry explains, or a wheel-accel spike
  (dω/dt) over a threshold;
- cut/limit the offending wheel's torque — **needs the per-wheel command from §12.3**; with a
  single global torque you can only cut *both* wheels;
- re-arm with hysteresis so it doesn't chatter at the grip limit.

Caveats:
- **Apply the §5.3 sign normalization per wheel first** — mirror mounting flips one wheel's hall
  sign, so raw `speedL/R` aren't directly comparable.
- Unequal torque produces yaw that interacts with steering; for a kids' car, start with a
  **symmetric torque cut (traction control)** before any yaw-seeking torque vectoring.
- Resolution/latency: the ±1000 scale and ~20 ms control tick bound how fine/fast the intervention
  is; slip control wants the faster ~10 ms tick noted in §2.1.
- Stays on the ESP32, consistent with the §2 split — the board remains a dumb per-wheel amplifier.

### 12.5 Suggested increments
1. **Baseline first:** log `speedL/R` split vs. throttle on-vehicle to characterize normal cornering
   spread before picking slip thresholds.
2. Switch to **TANK_STEERING** + a dual-torque protocol; verify equal-torque driving is unchanged.
3. Add **symmetric traction control** (cut both wheels) as a safety net, then graduate to per-wheel
   cut once dual-torque is trusted.
4. Optional **mild torque vectoring** for steering feel — last, and only after slip control is
   solid.

---

## 13. Advanced drivetrain — common design (not built)

> **Status: design / requirements only.** §13 captures the building blocks **shared** by both target
> drivetrains; **§14 = RWD (single board)** and **§15 = 4×4 (dual board)** then specialize them. All
> of it extends the single-board build in §1–§12 — control split (§2), TRQ_MODE (§3.2), per-wheel
> torque (§12), sign normalization (§5.3), brake taper (§5.4), serial dead-man (§3.4) carry over. No
> functional code yet.

### 13.1 Motor control method & commands (both drivetrains)
- Every hoverboard PCB runs **FOC Torque Mode (`TRQ_MODE`)** (per §3.2) for natural feel and
  regenerative braking; `CTRL_TYP_SEL` stays `FOC_CTRL`.
- The **ESP32 is the Vehicle Control Unit (VCU)**: each tick it computes and **sends a signed torque
  per driven wheel** and **reads back the live RPM of every driven motor** over bidirectional UART.
- **Per-wheel torque** is obtained with `VARIANT_USART` **+ `TANK_STEERING`** on every board, so the
  two command payload fields map **directly** to that board's two wheels with no mixing (§12.3,
  path 2): `steer → left wheel`, `speed → right wheel`. Each board reports `speedL_meas`/`speedR_meas`
  for its two wheels.
- **Per-board / per-axle sign normalization (§5.3)** is applied independently before any cross-wheel
  comparison: mirror-mounted hubs report opposite hall-speed signs and boards may differ.

### 13.2 Electronic differential — Ackermann speed targets (both)
The VCU holds the **fixed vehicle geometry** as compile-time parameters:
- `WHEELBASE_L` — front-to-rear axle distance **L**.
- `TRACK_WIDTH_W` — left-to-right wheel distance **W**.
- (All wheels share the same rolling diameter, so ground-speed ratios equal RPM ratios.)

From the live steer input mapped to an effective steer angle **δ**, the VCU computes the turn radius
to the rear-axle centreline and each wheel's radius from the instantaneous centre of rotation (ICR),
which lies on the extended rear axle:

```
R          = L / tan(δ)                 # turn radius to rear-axle centre (δ→0 ⇒ R→∞ ⇒ straight)
R_rear_in  = R − W/2                     # inner rear wheel
R_rear_out = R + W/2                     # outer rear wheel
R_front_in = sqrt(L² + (R − W/2)²)       # inner front wheel
R_front_out= sqrt(L² + (R + W/2)²)       # outer front wheel
```

Each wheel's **target RPM** is the driver's speed demand scaled by its radius ratio:

```
k_wheel         = R_wheel / R
targetRPM_wheel = demandRPM · k_wheel
```

| Wheel | Radius from ICR | Speed factor k |
|---|---|---|
| Rear inner | `R − W/2` | `< 1` (slowest) |
| Rear outer | `R + W/2` | `> 1` |
| Front inner | `sqrt(L² + (R−W/2)²)` | `> rear inner` |
| Front outer | `sqrt(L² + (R+W/2)²)` | `> 1` (fastest) |

Going straight, all `k → 1` and every target RPM is equal. **RWD (§14) uses only the two rear rows;
4×4 (§15) uses all four.**

### 13.3 Speed-and-torque hybrid control (both)
The boards are **torque**-controlled, not speed-controlled, so the VCU realizes the Ackermann speed
targets by **modulating torque per wheel**:
- Throttle sets the **overall torque/speed demand** (`demandRPM` + a base torque envelope).
- A **per-wheel outer speed loop** (PI, analogous to the §5.5 governor) trims each driven wheel's
  torque so its **measured RPM tracks its Ackermann `targetRPM`**. The relative trims between wheels
  *are* the electronic differential; the common component is the drive torque.
- Every per-wheel torque stays bounded by the preset `maxTorque`, the board's per-motor `I_MOT_MAX`,
  and the on-board slew (§3.5) — all now acting **per wheel, per board**.

### 13.4 Traction control (anti-slip) (both)
- The VCU **compares all driven-wheel RPMs every tick**.
- **Reference = the Ackermann target (§13.2)**, so legitimate cornering spread is *not* misread as
  slip — the payoff of computing the differential explicitly.
- **Slip detection:** a wheel is slipping when its measured RPM exceeds its expected value (its
  Ackermann target, or the average of the other driven wheels adjusted by their `k` factors) by more
  than a threshold **`TC_SLIP_PCT`** (e.g. > X % faster than expected), or when its wheel-accel
  `dω/dt` spikes above a threshold.
- **Action:** immediately **reduce torque to that specific wheel (or that axle)** until its RPM
  re-synchronizes, then restore with **hysteresis** so it doesn't chatter at the grip limit.
- TC and the electronic differential **share the same RPM comparison and reference**; build the
  differential first, then layer TC on top.

### 13.5 Braking & near-stop taper (both)
- The brake pedal produces a **total braking-torque demand** (opposing / regenerative torque),
  distributed to the driven wheels per the drivetrain's bias rule (§14.4 / §15.4).
- The **near-standstill brake taper (§5.4)** is reused **per wheel** so hard braking never drives a
  wheel past zero into reverse.

### 13.6 Communication rate & fail-safe philosophy (both)
- **Command cadence: 50–100 Hz (10–20 ms) per link.** The single-board build runs ~50 Hz (20 ms,
  §5.2); **100 Hz (10 ms) is recommended** for the bandwidth the electronic differential and TC want
  (matches the faster-tick option in §2.1).
- **Every board keeps its own serial dead-man (§3.4)** — on its own link loss it ramps its wheels to
  zero torque independently.
- **The VCU watches every link** and, on any timeout, drives the **whole vehicle** to a safe state
  (torque 0), not just the affected axle — see the per-drivetrain rule (§14.5 / §15.5).
- On fault, torque **ramps to 0 (coast)** per §3.5/§5.4 rather than a hard stop, so a link glitch
  doesn't induce a skid. **E-stop (§6)** zeroes/brakes all driven wheels.

### 13.7 Config / protocol deltas from the single-board build (both)
- **Per board:** `VARIANT_USART` + define `TANK_STEERING` (available for USART, §12.3).
- **Protocol:** reuse the 8-byte `SerialCommand` per board but **reinterpret both payload fields as
  two wheel torques** (`steer → torque_L`, `speed → torque_R`); rename the fields in `protocol.h`
  and the sender to drop the "steer/speed" misnomer. Feedback unchanged (two RPMs per board).
- **New VCU parameters:** `WHEELBASE_L`, `TRACK_WIDTH_W`, brake-bias value(s), `TC_SLIP_PCT`
  (+ accel threshold and re-arm hysteresis), and per-wheel speed-loop PI gains. Add to the tunable
  set / presets (bump `NVS_VER`, §5.6); keep the §6.1 server-side clamping.

---

## 14. RWD configuration — single board, rear axle (design)

Rear-wheel drive: **one** hoverboard PCB drives the two **rear** hub motors; the **front wheels are
steered but undriven** (free-rolling). This is the current single-board hardware (§1–§12) plus the
electronic differential and traction control of §13, restricted to the rear axle.

### 14.1 Topology
```
[Throttle] [Brake] [Steer] ─▶ ESP32 VCU (@50–100 Hz)
                                │  single UART (Serial2, as §4)
                                ▼
                        Rear hoverboard PCB (VARIANT_USART + TANK_STEERING, FOC/TRQ)
                             │        │
                            [RL]     [RR]         front wheels: steered, undriven
```
One link, one `HoverboardLink`. `steer → RL torque`, `speed → RR torque` (§13.1).

### 14.2 Electronic differential (rear only)
Only the **rear** rows of §13.2 apply: `targetRPM_RL = demandRPM·(R−W/2)/R`,
`targetRPM_RR = demandRPM·(R+W/2)/R`. The per-wheel speed loop (§13.3) trims RL/RR torque to hit
them. The undriven front wheels contribute no torque and need no target.

### 14.3 Traction control (rear pair)
TC (§13.4) compares **RL vs. RR** against their rear Ackermann targets and cuts the spinning rear
wheel's torque.
- **Reference caveat:** with no instrumented undriven wheel, the VCU has no clean ground-speed
  truth, so it leans on the **relative** RL/RR comparison plus the `dω/dt` accel spike. If *both*
  rear wheels break traction together (straight-line launch on a slick surface) the relative check
  is blind — the accel-spike detector is the backstop there.
- *Optional upgrade:* feed a front-wheel speed sensor into a spare board/ESP32 input for a true
  ground-speed reference; otherwise accept the weaker rear-only reference.

### 14.4 Braking
Regenerative braking is available on the **rear axle only**; there is no electronic front/rear bias
to set (the front wheels aren't driven — any front braking would be separate friction brakes, out of
scope). Consequence: the whole regen budget lands on the rear, so **cap total rear brake torque** and
keep the §5.4 taper to avoid **rear lockup / oversteer**. Split the brake demand **equally L/R**
across RL/RR (modulated by TC if a wheel locks).

### 14.5 Fail-safe (single link)
One link, so the existing model (§3.4 dead-man + the VCU `LINK_TIMEOUT` watchdog, §5.2) is
sufficient: on link loss, torque ramps to 0 and the car coasts to a stop.

### 14.6 Suggested build order
1. Confirm the existing single-board link + dead-man (§9) still pass with `TANK_STEERING` and the
   dual-torque protocol; verify equal-torque straight-line driving is unchanged.
2. Add the **rear electronic differential** (§14.2) + per-wheel speed loop; validate cornering RL/RR
   spread against a coast-down baseline.
3. Add **rear traction control** (§14.3).
4. Add the **rear brake cap + taper** (§14.4); tune on-vehicle.

---

## 15. 4×4 configuration — dual board, all wheels driven (design)

Four-wheel drive: **two** hoverboard PCBs (front axle, rear axle; two motors each) under **one**
ESP32 VCU, over **two** independent UART links.

### 15.1 Topology
```
[Throttle] [Brake] [Steer] ─▶ ESP32 VCU (@50–100 Hz)
                                │  UART-A (Serial1)        UART-B (Serial2)
                                ▼                          ▼
                    Front PCB (VARIANT_USART              Rear PCB (VARIANT_USART
                    + TANK_STEERING, FOC/TRQ)             + TANK_STEERING, FOC/TRQ)
                         │        │                            │        │
                        [FL]     [FR]                         [RL]     [RR]
```

| Component | Role |
|---|---|
| **Front PCB** | Two FOC torque loops (FL, FR). Dumb per-wheel amplifier + own dead-man. |
| **Rear PCB** | Two FOC torque loops (RL, RR). Identical firmware/config. |
| **ESP32 (VCU)** | Electronic differential, TC, brake bias, per-wheel torque shaping, **two independent link watchdogs**. |

Two hardware UARTs → two `HoverboardLink` instances; the control loop iterates over `{front, rear}`.
Front board `steer → FL`, `speed → FR`; rear board `steer → RL`, `speed → RR` (§13.1).

### 15.2 Electronic differential (all four wheels)
The full four-row §13.2 table applies: the VCU computes `targetRPM` for **FL, FR, RL, RR** — the
front wheels ride farther from the ICR, so their targets are higher than the rears' in a turn. The
per-wheel speed loop (§13.3) trims all four torques to hit them.

### 15.3 Traction control (four wheels)
TC (§13.4) compares **all four** RPMs against their Ackermann targets. With four driven wheels the
reference is stronger than RWD: the non-slipping wheels give a good expected-speed estimate, so a
single spinning wheel (or a whole axle) is cleanly isolated and its torque cut.

### 15.4 Electronic brake bias (front/rear)
- The total brake-torque demand (§13.5) is split **front vs. rear** by a bias — **default e.g.
  60 % front / 40 % rear**, fixed or dynamic — then **equally L/R within each axle** (modulated by
  TC/ABS). Forward bias keeps the rear axle from **locking and breaking loose (oversteer)** under
  braking.
- `BRAKE_BIAS_FRONT` is a tunable VCU parameter with a safe default and clamped range.
- *Future:* per-wheel ABS-style modulation using the four-RPM feed (detect a wheel decelerating
  toward lock, ease its brake torque) — defer until base bias + TC are trusted.

### 15.5 Fail-safe (dual link)
Each board keeps its own dead-man (§13.6). The VCU watches **both** links independently; if **either**
times out (`LINK_TIMEOUT`), it commands **zero torque to *both* boards**. Driving one axle while the
other is dead is unsafe (sudden yaw / instability), so one board's failure must stop the **whole
vehicle**, not just its axle. E-stop zeroes all four wheels.

### 15.6 Suggested build order
1. **Two dumb links first:** drive both boards with the *same* single torque (no differential, no TC)
   and confirm both axles pull together and the dual-link fail-safe (§15.5) works.
2. **Per-wheel torque:** both boards on `TANK_STEERING` + dual-torque protocol; verify equal-torque
   straight-line driving.
3. **Electronic differential:** add the four Ackermann targets + per-wheel speed loop (§15.2).
4. **Traction control:** layer §15.3 on the four-RPM comparison.
5. **Brake bias:** add the front/rear split (§15.4); tune on-vehicle.
6. *Optional later:* ABS brake modulation and steering-feel torque vectoring.
