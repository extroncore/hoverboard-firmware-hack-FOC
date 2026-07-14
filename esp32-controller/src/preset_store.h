// ============================================================================
//  preset_store.h - 5 tuning presets persisted in NVS flash (Preferences).
//   * slots 0,1 are read-only built-ins (Kid / Race), regenerated at boot.
//   * slots 2,3,4 are user-editable and saved to flash.
//  The selected slot is also persisted, so the car reboots into its last preset.
//  A preset IS a Tunables (its name lives in Tunables.profileName).
//
//  Touched only from the web/loop task, so no mutex here; it publishes the live
//  tunables via the mutex-guarded g_state.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "shared_state.h"

class PresetStore {
public:
  static const int COUNT   = 5;
  static const int BUILTIN = 2;   // slots [0..BUILTIN-1] are read-only

  void begin();                                   // load NVS, apply selected as live
  int  selected() const           { return _sel; }
  bool editable(int i) const      { return i >= BUILTIN && i < COUNT; }
  const Tunables &preset(int i) const { return _p[i]; }

  void select(int i);                             // make slot i the live/active preset
  void saveUserPreset(int i, const Tunables &t);  // persist into slot i (2..4 only)

private:
  Tunables    _p[COUNT];
  int         _sel = 0;
  Preferences _prefs;

  void loadBuiltinsAndDefaults();
  void persistSel();
  void persistPreset(int i);
};

extern PresetStore g_presets;
