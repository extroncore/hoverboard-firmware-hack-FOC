#include "preset_store.h"
#include "control.h"   // PROFILES (0=child, 1=race)
#include "config.h"    // CALIBRATION_DEFAULT + threshold seed values

// Bump when the Tunables layout changes so stale NVS blobs are discarded.
// v2: added brakeTorqueMax + reverse{MaxTorque,SpeedCeiling,RampMs}.
static const uint32_t NVS_VER = 2;
static const char    *NVS_NS  = "hovercar";

PresetStore g_presets;

static void setName(Tunables &t, const char *name) {
  strncpy(t.profileName, name, sizeof(t.profileName) - 1);
  t.profileName[sizeof(t.profileName) - 1] = '\0';
}

// Rescale an rpm speed so it represents the SAME real km/h on a different wheel.
// km/h = rpm * k * dia, so to hold km/h when the diameter changes: rpm scales by
// oldDia/newDia. Rounded, and clamped to the safe ceiling.
static int16_t rescaleSpeed(int16_t rpm, int16_t oldDia, int16_t newDia) {
  if (oldDia <= 0 || newDia <= 0 || oldDia == newDia) return rpm;
  long v = ((long)rpm * oldDia + newDia / 2) / newDia;
  if (v < 0) v = 0;
  if (v > SPEED_CEILING_MAX) v = SPEED_CEILING_MAX;
  return (int16_t)v;
}

void PresetStore::loadBuiltinsAndDefaults() {
  _p[0] = PROFILES[0]; setName(_p[0], "Kid");
  _p[1] = PROFILES[1]; setName(_p[1], "Race");
  for (int i = BUILTIN; i < COUNT; i++) {
    _p[i] = PROFILES[0];                       // safe default (child-like)
    char nm[16]; snprintf(nm, sizeof(nm), "Preset %d", i + 1);
    setName(_p[i], nm);
  }
}

void PresetStore::begin() {
  loadBuiltinsAndDefaults();
  Calibration calDefault = CALIBRATION_DEFAULT;   // config.h seed values
  _cal = calDefault;
  _prefs.begin(NVS_NS, false);

  if (_prefs.getUInt("ver", 0) != NVS_VER) {
    // Fresh flash or incompatible layout: reset to defaults and stamp version.
    // Leave "calok" unset -> not calibrated yet (forces the web setup page).
    _prefs.clear();
    _prefs.putUInt("ver", NVS_VER);
    for (int i = BUILTIN; i < COUNT; i++) persistPreset(i);
    _prefs.putInt("sel", 0);
    persistCalibration();
    _calibrated = false;
  } else {
    for (int i = BUILTIN; i < COUNT; i++) {
      char key[8]; snprintf(key, sizeof(key), "p%d", i);
      if (_prefs.getBytesLength(key) == sizeof(Tunables))
        _prefs.getBytes(key, &_p[i], sizeof(Tunables));
    }
    _sel = _prefs.getInt("sel", 0);
    if (_sel < 0 || _sel >= COUNT) _sel = 0;
    // Calibration is stored under its own key; a length mismatch (or a first
    // boot after adding it to an already-versioned install) keeps the defaults.
    if (_prefs.getBytesLength("calib") == sizeof(Calibration))
      _prefs.getBytes("calib", &_cal, sizeof(Calibration));
    // "calok" is only ever written by an explicit saveCalibration(), so it is
    // the authoritative "the user has calibrated this car" flag.
    _calibrated = (_prefs.getUChar("calok", 0) != 0);
  }

  // Built-in profiles are authored in config.h as rpm at the reference wheel
  // (WHEEL_DIA_MM). Rescale them to the actual wheel so their top/reverse speed
  // holds the same km/h on any tire. User presets (2..4) are already stored
  // scaled to the current wheel (rescaled on every wheel change), so load as-is.
  for (int i = 0; i < BUILTIN; i++) {
    _p[i].speedCeiling        = rescaleSpeed(_p[i].speedCeiling, WHEEL_DIA_MM, _cal.wheelDiaMm);
    _p[i].reverseSpeedCeiling = rescaleSpeed(_p[i].reverseSpeedCeiling, WHEEL_DIA_MM, _cal.wheelDiaMm);
  }

  g_state.setTunables(_p[_sel]);   // apply the remembered preset as live
  g_state.setCalibration(_cal);    // publish calibration to the control task
  g_state.setCalibrated(_calibrated); // gate driving until the car is calibrated
}

void PresetStore::select(int i) {
  if (i < 0 || i >= COUNT) return;
  _sel = i;
  g_state.setTunables(_p[i]);
  persistSel();
}

void PresetStore::saveUserPreset(int i, const Tunables &t) {
  if (!editable(i)) return;
  _p[i] = t;
  persistPreset(i);
}

void PresetStore::saveCalibration(const Calibration &c) {
  int16_t oldDia = _cal.wheelDiaMm;
  _cal = c;
  int16_t newDia = _cal.wheelDiaMm;
  // If the wheel size changed, rescale EVERY profile's top/reverse speed so each
  // keeps the same real km/h (the rpm adjusts for the new tire). Persist the
  // user presets so the change survives a reboot; the built-ins are re-derived
  // from config.h at boot (and boot-rescaled to the stored wheel).
  if (newDia != oldDia) {
    for (int i = 0; i < COUNT; i++) {
      _p[i].speedCeiling        = rescaleSpeed(_p[i].speedCeiling, oldDia, newDia);
      _p[i].reverseSpeedCeiling = rescaleSpeed(_p[i].reverseSpeedCeiling, oldDia, newDia);
    }
    for (int i = BUILTIN; i < COUNT; i++) persistPreset(i);
    g_state.setTunables(_p[_sel]); // apply the rescaled active profile live
  }
  persistCalibration();
  _calibrated = true;
  _prefs.putUChar("calok", 1);     // mark the car as calibrated (survives reboot)
  g_state.setCalibration(_cal);    // apply to the control task immediately
  g_state.setCalibrated(true);     // allow driving now that a calibration exists
}

void PresetStore::persistSel() { _prefs.putInt("sel", _sel); }

void PresetStore::persistPreset(int i) {
  char key[8]; snprintf(key, sizeof(key), "p%d", i);
  _prefs.putBytes(key, &_p[i], sizeof(Tunables));
}

void PresetStore::persistCalibration() {
  _prefs.putBytes("calib", &_cal, sizeof(Calibration));
}
