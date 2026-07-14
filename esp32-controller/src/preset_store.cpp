#include "preset_store.h"
#include "control.h"   // PROFILES (0=child, 1=race)

// Bump when the Tunables layout changes so stale NVS blobs are discarded.
static const uint32_t NVS_VER = 1;
static const char    *NVS_NS  = "hovercar";

PresetStore g_presets;

static void setName(Tunables &t, const char *name) {
  strncpy(t.profileName, name, sizeof(t.profileName) - 1);
  t.profileName[sizeof(t.profileName) - 1] = '\0';
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
  _prefs.begin(NVS_NS, false);

  if (_prefs.getUInt("ver", 0) != NVS_VER) {
    // Fresh flash or incompatible layout: reset to defaults and stamp version.
    _prefs.clear();
    _prefs.putUInt("ver", NVS_VER);
    for (int i = BUILTIN; i < COUNT; i++) persistPreset(i);
    _prefs.putInt("sel", 0);
  } else {
    for (int i = BUILTIN; i < COUNT; i++) {
      char key[8]; snprintf(key, sizeof(key), "p%d", i);
      if (_prefs.getBytesLength(key) == sizeof(Tunables))
        _prefs.getBytes(key, &_p[i], sizeof(Tunables));
    }
    _sel = _prefs.getInt("sel", 0);
    if (_sel < 0 || _sel >= COUNT) _sel = 0;
  }

  g_state.setTunables(_p[_sel]);   // apply the remembered preset as live
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

void PresetStore::persistSel() { _prefs.putInt("sel", _sel); }

void PresetStore::persistPreset(int i) {
  char key[8]; snprintf(key, sizeof(key), "p%d", i);
  _prefs.putBytes(key, &_p[i], sizeof(Tunables));
}
