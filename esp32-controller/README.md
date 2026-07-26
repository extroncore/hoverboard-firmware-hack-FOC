# ESP32 Hovercar Controller

ESP32 firmware that drives the hoverboard mainboard over UART in **FOC torque mode**
and hosts a **phone web interface** (its own WiFi) for live tuning. It is the "brain":
pedal sensing, Child/Race profiles, a PI smart-speed limiter, and a link watchdog.

See [`../esp32-torque-control.spec.md`](../esp32-torque-control.spec.md) for the full design.
The mainboard must be flashed with `VARIANT_USART` + `TRQ_MODE` (see repo root).

## Wiring

ESP32 UART2 ↔ hoverboard **left sensor cable** (both 3.3 V logic):

| ESP32 | Hoverboard left cable |
|-------|-----------------------|
| GPIO17 (TX) | RX |
| GPIO16 (RX) | TX |
| GND | GND |

Driver controls:

| Signal | ESP32 pin | Notes |
|--------|-----------|-------|
| Throttle pedal | `GPIO34` | analog, ADC1 (WiFi-safe, input-only) |
| Brake pedal | `GPIO35` | analog, ADC1 |
| Direction switch | `GPIO27` | digital, `INPUT_PULLUP`; **open = forward** (NO), closed to GND = reverse |

Pins/calibration are configurable in [`src/config.h`](src/config.h).

> Do **not** power the ESP32 from the hoverboard's logic rail without checking current
> capacity; a separate 5 V/3.3 V supply with common ground is safer.

## Configure before flashing

Edit [`src/config.h`](src/config.h):
- `AP_SSID` — network name. `AP_PASSWORD` is `""` (**open network**) by default for
  easy access; set ≥ 8 chars to enable WPA2.
- `API_TOKEN` is `""` by default (**no token**, open control) for bring-up; set a long
  random value to require it for settings changes. Server-side clamping is always on.
- `THROTTLE_*` / `BRAKE_*` — calibrate each pedal's raw ADC min/max/deadband.
- `SPEED_SIGN` / `FORWARD_SIGN` — bench sign calibration (see below).
- Profiles (`PROFILE_CHILD`, `PROFILE_RACE`) — starting points; all tunable live.

### Direction & brake behaviour

- The **direction switch** picks forward/reverse. Changing it **while moving**
  makes the car ignore the throttle and coast down gently (`DIR_CHANGE_BRAKE_TORQUE`)
  until near-stop, then the new direction engages and the throttle drives again.
- The **brake pedal always overrules** throttle and the switch: it applies torque
  opposite to motion to stop, easing to zero at standstill (never drives through).
- Sign calibrations, checked on the bench with wheels off the ground:
  - `SPEED_L_SIGN` / `SPEED_R_SIGN`: the hub motors are mirror-mounted and report
    **opposite** hall-speed signs for the same direction. Roll both wheels forward
    by hand and watch `spd L/R` in the log; set the two signs so both contributions
    have the **same** sign (e.g. L `+`, R `-` → `+1` / `-1`).
  - If pressing the brake **speeds the car up**, flip `SPEED_SIGN` to `-1`.
  - If forward/reverse feel **swapped**, flip `FORWARD_SIGN` to `-1`.

## Build & flash

The ESP32 env lives in the **root** `platformio.ini` as `esp32dev` (its sources are
here in `esp32-controller/src/`). Run from the repo root, or select the `esp32dev`
environment in the PlatformIO IDE switcher:

```bash
# from the repo root
pio run -e esp32dev                      # build
pio run -e esp32dev -t upload            # flash (set upload_port in platformio.ini if needed)
pio device monitor -e esp32dev           # serial log @115200
```

> Note: on this machine use `~/.platformio/penv/bin/pio` for the ESP32 env — the
> `pio` on PATH (pyenv shim) hits a stray `fatfs` package that breaks the espressif32
> builder. The STM32 variants build fine with either.

## Use

1. Power up. The car boots into the **child** profile at **zero torque**.
2. On your phone, join the open WiFi `hovercar` (the `AP_SSID`).
3. A **captive-portal sheet opens the control page automatically** (iOS/Android
   detect the portal and pop it up). If it doesn't, open `http://192.168.4.1`.
4. Pick a preset from the dropdown and (for user presets) adjust limits. All values are
   **clamped server-side** to the safe ranges in `config.h`.

> The captive sheet is a minimal browser. For the full page (and to keep it open),
> tap "Use Without Internet" / open `http://192.168.4.1` in Safari directly.

### Presets (saved in flash)

Five preset slots, selectable from the dropdown; the choice and all user presets are
**persisted in NVS flash**, so the car reboots into its last-used preset.

- Slots **1–2 are built-in and read-only**: **Kid** and **Race** (from `PROFILE_CHILD` /
  `PROFILE_RACE` in `config.h`).
- Slots **3–5 are user-editable**. Select one, edit the name + limits, then:
  - **Apply (live)** — try the values now without saving.
  - **Save preset** — write the name + values to that slot in flash.

Built-in slots show their values read-only. Names are limited to 15 characters
(alphanumeric, space, `-`, `_`). Bump `NVS_VER` in `preset_store.cpp` if you change the
`Tunables` layout (stale saved presets are then discarded and defaults restored).

### Emergency STOP

A big red **EMERGENCY STOP** button sits at the top of the page. Pressing it latches
an e-stop: the controller **ignores all inputs** and brakes to a standstill (tapered,
so it eases to zero without reversing). The page then shows **"Braking to a stop…"**
until standstill, after which an **Engage** button appears — press it to release the
e-stop and re-enable normal pedal/switch input.

## Safety notes

- The web UI is a *tuning* surface, never the sole safety mechanism. The hoverboard's
  compiled current/speed caps and the serial dead-man still apply.
- On link loss (no feedback within `LINK_TIMEOUT_MS`) the controller commands zero
  torque; the mainboard independently zeroes torque on its own serial timeout.
- The ESP32 boots and reconnects commanding ~0 torque and ramps up from there.
- Bench-test with the wheels off the ground and a current-limited supply first.
