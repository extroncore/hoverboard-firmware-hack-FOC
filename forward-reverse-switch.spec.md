# Spec: External Forward/Reverse Switch for VARIANT_HOVERCAR

> **Implementer:** this spec is self-contained. All line numbers were verified against
> the current tree (branch `forward-reverse-switch`) at the time of writing, but **treat
> them as approximate anchors** — always confirm by reading the surrounding code before
> editing, since edits shift line numbers. Read the "Implementation gotchas" section
> before you start; it covers a variable-scope trap that will otherwise fail to compile.

## Summary

Replace the double-tap brake pedal reverse mechanism with a physical toggle/rocker
switch for direction control, while keeping 2 pedals (throttle + brake).

The chosen behaviour is **"throttle brakes, then reverses"**: if the switch is flipped
while the vehicle is moving, the direction does **not** snap over. Instead the brake
pedal keeps braking as normal, and the **throttle pedal is redirected into a braking
request that opposes the current motion**. Both pedals bring the vehicle to a stop; only
once it is at standstill does the latched direction flip and the throttle start driving
the other way. No violent torque reversal, and no ignored throttle input during the
transition.

## Current Behaviour

- `input1` = Brake pedal (ADC), `input2` = Throttle pedal (ADC). Both are normal pots
  producing a positive magnitude (0 → ~1000).
- Forward/Reverse is toggled by **double-tapping the brake pedal** at near-standstill
  (`speedAvgAbs < 60 rpm`).
- The double-tap detection (`multipleTapDet`) is intentionally gated to only work when
  the vehicle is nearly stopped — this is the existing safety guard.
- `MultipleTapBrake.b_multipleTap` is consumed in 3 places:
  1. **Direction logic** (`main.c:406-411`): chooses `speed = steer + speed` (fwd) vs
     `speed = steer - speed` (rev)
  2. **Electric brake** (`main.c:305`): `electricBrake(speedBlend, MultipleTapBrake.b_multipleTap)`
     — the `reverseDir` param flips the brake force direction
  3. **Backward beep** (`main.c:651`): beeps when `b_multipleTap` is set or motors spin
     backward
- Signals available in the loop that this design relies on:
  - `speedAvg` — signed average measured speed (sign = actual direction of travel)
  - `speedAvgAbs` — `|speedAvg|`
  - `speedBlend` (`main.c:283-284`) — `uint16_t` fixdt(0,16,15) value that ramps `0 → 1`
    over `[10 rpm, 60 rpm]`. It is `0` at/below 10 rpm and `1` (0x8000) at/above 60 rpm.
    It is used to fade braking authority out near standstill so the brake pedal cannot
    cause reverse creep. **It is declared as a plain local in the loop body and is in
    scope at the direction-selection block** — you can use it directly there.
  - `steer` / `speed` (`main.c:333-334`) — the brake-pedal and throttle-pedal commands
    after rate limiting + low-pass filtering (and, for `speed`, after the optional
    torque-boost / soft-limit shaping at `main.c:340-404`). Both are positive magnitudes
    at the direction-selection point. Note: the direction sign is applied to `speed`
    **after** the rate limiter, so a naive direction flip bypasses the ramp — this is
    exactly what we must avoid at speed.

## Hardware — how to wire the switch

The switch only needs to pull one MCU pin to GND, so **a simple SPST switch is enough**.
An SPDT toggle or rocker also works — just use the **common** terminal + **one** throw and
leave the other throw unconnected. No external resistor and no external voltage are needed:
the pin uses the STM32's **internal pull-up**.

**Signal used:** `BUTTON1` = **PB10**, exposed on the **right** sensor-board cable (the
sensor JST connector on the right side of the mainboard). The firmware maps it via
`SUPPORT_BUTTONS_RIGHT` (`defines.h:181-185`) and enables the internal pull-up in
`setup.c:389-397`, so PB10 idles HIGH (3.3 V) and reads LOW only when tied to GND.

The right sensor cable is free to use: the ADC pedals are on the **left** cable
(PA2/PA3), and `DEBUG_SERIAL_USART3` / `SIDEBOARD_SERIAL_USART3` are disabled for this
variant (the compile-time guard at `config.h:764-765` enforces that USART3 is off whenever
`SUPPORT_BUTTONS_RIGHT` is defined).

### Connections (two wires)

| Switch terminal        | Connects to                                                    |
|------------------------|----------------------------------------------------------------|
| Common (pole)          | **PB10** on the right sensor cable                             |
| One throw / other pole | **GND** (any board ground — e.g. the GND on the same cable)    |

```
   Switch                STM32 mainboard
  +------+
  |  o---+------------->  PB10  (BUTTON1, right sensor cable, internal pull-up)
  |  /   |
  |  o---+------------->  GND
  +------+
   open  = Forward
   closed= Reverse
```

### Logic

- Switch **open** → PB10 held HIGH by internal pull-up → `reverseSwitch = 0` → **Forward**.
- Switch **closed to GND** → PB10 pulled LOW → `reverseSwitch = 1` → **Reverse**.

### Finding the pins physically

Cable wire colors vary between board revisions — **do NOT trust color alone**. Trace the
pins:

1. Power the board OFF and unplug the battery before probing.
2. Locate the right-hand sensor connector on the mainboard. That cable carries the three
   right-hall signals plus power; PB10 (BUTTON1) and PB11 (BUTTON2) are the two non-hall
   logic lines on it.
3. Use a multimeter in continuity mode to find which cable wire lands on STM32 pin **PB10**
   and which lands on **GND**, cross-checking your board's schematic / the
   [firmware pinout wiki](https://github.com/EmanuelFeru/hoverboard-firmware-hack-FOC).
   Confirm before soldering.
4. After flashing, sanity-check with the meter: PB10 should read ~3.3 V with the switch
   open and ~0 V with it closed.

### Cautions

- **Only ever connect PB10 to GND.** Do **not** wire the switch to +15 V or +3.3 V —
  driving the pin to a supply rail can damage it.
- Keep the switch lead short and away from the motor phase wires to reduce noise. The
  standstill gate (and optional debounce) makes occasional bounce harmless, but clean
  routing is still preferred.

## Software

### 1. `Inc/config.h` — VARIANT_HOVERCAR block (`config.h:497-564`)

- Add `#define SUPPORT_BUTTONS_RIGHT` inside the `#ifdef VARIANT_HOVERCAR` block so
  `BUTTON1_PIN` (PB10) is configured as a pulled-up digital input.
  - There is a commented-out `SUPPORT_BUTTONS_RIGHT` line near the cruise-control notes
    (`config.h:236`); enable/add one that is active for this variant. Confirm the guard
    checks at `config.h:764-769` pass (they require `SUPPORT_BUTTONS_RIGHT` not coexist
    with USART3 / Nunchuk / PPM_RIGHT / PWM_RIGHT / I2C_LCD — none are enabled for
    HOVERCAR, so this is fine).
- The `MULTIPLE_TAP_*` defines (`config.h:559-563`) become unused and may be removed.

### 2. `Src/main.c` — direction as a latched source of truth

The physical switch is only ever **sampled**. A single latched variable, `reverseDir`,
is the one and only thing consumed by the direction logic, electric brake and beep. This
prevents the raw GPIO from ever driving the motors directly.

Declare two file-scope globals next to the existing `MultipleTapBrake` declaration
(`main.c:172`). Making `reverseSwitch` a global (like the existing `button1`/`button2`
globals) avoids the scope trap described in "Implementation gotchas".

```c
// main.c — globals (near main.c:172)
uint8_t reverseDir    = 0;   // 0 = Forward, 1 = Reverse. Latched; only changes near standstill.
uint8_t reverseSwitch = 0;   // Raw sampled switch state this loop: 1 = reverse requested.
```

Read + latch, placed in the **first** VARIANT_HOVERCAR ADC block, replacing the
`multipleTapDet` gate at `main.c:292-295`:

```c
#ifdef VARIANT_HOVERCAR
if (inIdx == CONTROL_ADC) {                                   // Only if pedals are in use (ADC input)
  reverseSwitch = !HAL_GPIO_ReadPin(BUTTON1_PORT, BUTTON1_PIN); // 1 = reverse requested (active-low)

  // Latch the new direction ONLY at (near) standstill. Above the threshold the switch is
  // remembered in reverseSwitch but not acted on as a direction — the transition logic
  // below handles it.
  if (speedAvgAbs < 60) {
    reverseDir = reverseSwitch;
  }

  if (input1[inIdx].cmd > 30) {                               // Brake pedal pressed → zero the throttle
    input2[inIdx].cmd = (int16_t)((input2[inIdx].cmd * speedBlend) >> 15);
    cruiseControl((uint8_t)rtP_Left.b_cruiseCtrlEna);
  }
}
#endif
```

`reverseSwitch != reverseDir` is therefore true exactly when the driver has requested a
direction change while still moving above 60 rpm — the "pending reversal" state.

### 3. `Src/main.c` — throttle-brakes-then-reverses transition

Replace the direction selection block (`main.c:406-411`, currently the
`if (!MultipleTapBrake.b_multipleTap) { … } else { … } steer = 0;`) with three cases:

```c
if (reverseSwitch != reverseDir) {
  // PENDING REVERSAL while still moving (>= 60 rpm) in the old direction.
  // Redirect the throttle pedal into a brake that opposes actual motion, faded out
  // near standstill via speedBlend (same treatment the brake pedal gets). This decelerates
  // the vehicle instead of accelerating it the wrong way.
  int16_t throttleBrake = (int16_t)(((int32_t)speed * speedBlend) >> 15);
  if (speedAvg > 0) {
    throttleBrake = -throttleBrake; // oppose forward motion
  }
  speed = steer + throttleBrake;    // steer (brake pedal) already opposes motion (see note)
} else if (!reverseDir) {
  speed = steer + speed;            // Forward driving
} else {
  speed = steer - speed;            // Reverse driving
}
steer = 0; // Do not apply steering to avoid side effects if STEER_COEFFICIENT is NOT 0
```

Note on "steer already opposes motion": the brake-pedal command `input1` is shaped at
`main.c:308-315` to be negative when `speedAvg > 0` (forward) and positive when in
reverse, and faded by `speedBlend`. After the filter this becomes `steer`. So when
rolling forward, both `steer` and `throttleBrake` are negative (both brake forward);
when rolling in reverse both are positive. The signs stay consistent automatically.

Behaviour walk-through (rolling forward, driver flips to Reverse):

1. `reverseSwitch` becomes 1, `reverseDir` stays 0 → pending reversal is active.
2. Throttle no longer accelerates forward; it is converted into reverse-opposing brake
   torque, scaled by `speedBlend` (full authority above 60 rpm, fading to 0 near
   standstill). The brake pedal continues to brake normally and adds to this.
3. Vehicle decelerates toward standstill. Because the command opposing motion is derived
   from the already rate-limited `speed`/`steer`, there is no rate-limiter-bypassing
   sign flip and no abrupt full-torque reversal.
4. When `speedAvgAbs` drops below 60 rpm, `reverseDir` latches to 1 (in the block from
   step 2). Pending reversal clears. At this low speed `speedBlend ≈ 0`, so the handoff
   is smooth, and the throttle now drives in reverse via `speed = steer - speed`.

The symmetric case (rolling in reverse, flip to Forward) works the same with the signs
mirrored.

### 4. `Src/main.c` — electric brake (do NOT pass the direction flag)

The `electricBrake()` call is at `main.c:305`, inside its own `#ifdef
ELECTRIC_BRAKE_ENABLE` block (**not** inside the ADC `if`-block). The function
(`util.c:701`) already uses `speedAvg` to brake against actual motion (`util.c:706-710`),
then **flips** it based on the `reverseDir` param (`util.c:712-715`). That flip is wrong
here — the brake must always oppose actual motion.

Change the call to:

```c
electricBrake(speedBlend, 0); // Always brake against actual motion, never flip on direction flag
```

(This also removes the last non-beep reference to `MultipleTapBrake`, which is required
for the code to compile once the global is deleted.)

Rationale: the `reverseDir` parameter existed because the double-tap toggle could be out
of sync with real motion during a transition. With the latched `reverseDir` + standstill
gate, the flag is always in sync with motion once latched, and during a pending reversal
the throttle-brake path in §3 (not `electricBrake`, which only acts when the throttle is
near zero) is what does the work. So the `speedAvg`-based logic alone is correct. You may
leave the `electricBrake` function signature unchanged (still takes `reverseDir`); only
the call site changes. The existing brake-pedal shaping (`main.c:308-315`) already uses
`speedAvg` directly and needs **no change**.

### 5. `Src/main.c` — backward beep (`main.c:651`)

Replace `MultipleTapBrake.b_multipleTap` with `reverseDir` in the beep condition:

```c
} else if (BEEPS_BACKWARD && (((cmdR < -50 || cmdL < -50) && speedAvg < 0) || reverseDir)) { // 1 beep fast (high pitch): Backward spinning motors
```

### 6. Remove double-tap detection

- Remove the `multipleTapDet(...)` call (was `main.c:294`; removed as part of §2).
- Remove the `MultipleTapBrake` global (`main.c:172`).
- After the changes in §2/§4/§5 there should be **zero** remaining references to
  `MultipleTapBrake` / `b_multipleTap` in `main.c`. Grep to confirm.
- The `MULTIPLE_TAP_*` config defines (`config.h:559-563`) become unused for this variant
  and can be removed.
- **Do not** delete the `multipleTapDet()` function definition or the `MultipleTap` type
  in `util.c`/`util.h` — other variants may reference them and this spec is
  HOVERCAR-scoped. Only remove the HOVERCAR usage.

## Implementation gotchas (read before coding)

1. **Two separate `CONTROL_ADC` blocks — scope trap.** There are *two* distinct
   `#ifdef VARIANT_HOVERCAR / if (inIdx == CONTROL_ADC) { … }` blocks in the loop: the
   switch is sampled in the first (~`main.c:291-302`) and the direction is decided in the
   second (~`main.c:337-413`). A `reverseSwitch` declared as a local inside the first
   block is **out of scope** in the second and will not compile. This is why §2 makes
   `reverseSwitch` a file-scope global. (Alternative: re-read the pin in the second block,
   but the global is cleaner and matches the existing `button1`/`button2` pattern.)
2. `reverseDir` must persist across loop iterations → it must be a global/static, not a
   loop local.
3. `speedBlend` is only defined under
   `#if defined(VARIANT_HOVERCAR) || defined(VARIANT_SKATEBOARD) || defined(ELECTRIC_BRAKE_ENABLE)`.
   For HOVERCAR it is always defined, so using it in the §3 block (also HOVERCAR-guarded)
   is safe.
4. Keep the `int16_t throttleBrake` computation in a `int32_t` intermediate (as written)
   to avoid overflow before the `>> 15`.

## Safety: switching direction at speed

The whole point of the latched `reverseDir` + pending-reversal logic is that flipping the
switch at speed **cannot** command an instantaneous reverse torque. Specifically it
prevents:

- **Rate-limiter bypass**: the direction sign is normally applied after the rate limiter,
  so a raw flip would jump the command from `+X` to `−X` in one cycle. Here the command
  during the transition is always derived from the rate-limited magnitude and only ever
  *opposes* motion (braking), never drives past zero into the new direction until latched.
- **Current spike / DC-bus overvoltage**: `I_MOT_MAX` (12 A) and `I_DC_MAX` (14 A) still
  clamp current, and braking authority is faded by `speedBlend`, so deceleration is
  controlled rather than a slam.
- **Mechanical shock / rider ejection**: no sudden torque reversal; the vehicle is
  brought to a stop first, then reverses.
- **Electric-brake inversion**: `electricBrake(speedBlend, 0)` always opposes actual
  motion, so it can never briefly push *with* the motion.

The `60 rpm` standstill threshold matches the original double-tap guard.

## Definition of done / acceptance criteria

1. **Builds clean** for the HOVERCAR variant. `default_envs` is already
   `VARIANT_HOVERCAR` in `platformio.ini`, so from the repo root:

   ```
   pio run -e VARIANT_HOVERCAR
   ```

   must complete with no errors or new warnings related to the changes.
2. `grep -rn "MultipleTapBrake\|b_multipleTap\|multipleTapDet" Src/main.c` returns
   **nothing** (all HOVERCAR usages removed).
3. `reverseDir` is the only thing consumed by the direction logic, electric-brake call,
   and backward-beep condition — the raw GPIO is never used directly to pick a driving
   direction.
4. The three direction cases in §3 are present and the `steer = 0;` line is preserved.
5. No changes to other variants' behaviour (changes are all inside `VARIANT_HOVERCAR` /
   HOVERCAR call sites; the `electricBrake` function body and `multipleTapDet` definition
   are untouched).

## Optional enhancements (NOT in scope — do not implement)

- **Debounce**: a short (e.g. 200-300 ms) hold before latching `reverseDir` to reject
  switch bounce. The standstill gate already blocks at-speed changes, so this is secondary
  and could delay the standstill handoff if made too long.
- **LED indicator**: drive an LED from `BUTTON2_PIN` (PB11 / green wire) to show the
  current `reverseDir` state.
- **Beep on direction change**: a short beep when `reverseDir` transitions, for audible
  confirmation.
- **Configurable threshold**: expose the `60 rpm` gate as a `#define` in the HOVERCAR
  config block.
