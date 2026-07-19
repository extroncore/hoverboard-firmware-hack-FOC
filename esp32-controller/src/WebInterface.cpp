#include "WebInterface.h"
#include "config.h"
#include "control.h"
#include "shared_state.h"
#include "preset_store.h"
#include <WiFi.h>

// ----------------------------------------------------------------------------
//  Dashboard page. Static (no server-side templating of user data -> no XSS).
//  Values are fetched as JSON and rendered client-side.
// ----------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hovercar mini</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: system-ui, sans-serif; margin: 0; background:#111; color:#eee; }
  header { padding: 12px 16px; background:#1c1c1c; font-weight:600; font-size:18px; }
  .wrap { padding: 16px; max-width: 560px; margin: 0 auto; }
  .grid { display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-bottom:16px; }
  .card { background:#1c1c1c; border-radius:10px; padding:12px 14px; }
  .card .v { font-size:22px; font-weight:700; }
  .card .k { font-size:12px; opacity:.6; text-transform:uppercase; letter-spacing:.05em; }
  .pill { display:inline-block; padding:2px 8px; border-radius:999px; font-size:12px; }
  .ok { background:#123d1a; color:#7CFFA0; } .bad { background:#3d1212; color:#ff8080; }
  fieldset { border:1px solid #333; border-radius:10px; margin-bottom:16px; }
  legend { padding:0 6px; opacity:.7; }
  label { display:block; margin:8px 0 2px; font-size:13px; }
  input, select { width:100%; box-sizing:border-box; padding:8px; background:#111;
    color:#eee; border:1px solid #333; border-radius:6px; font-size:15px; }
  .row { display:flex; gap:8px; }
  button { padding:10px 14px; border:0; border-radius:8px; background:#2b6cff; color:#fff;
    font-size:15px; font-weight:600; cursor:pointer; }
  button.secondary { background:#333; }
  #msg { min-height:18px; font-size:13px; margin-top:8px; }
  .estop-bar { margin:14px 0 18px; }
  button.estop { width:100%; background:#c62828; font-size:22px; padding:20px;
    letter-spacing:.03em; }
  button.estop:active { background:#8e1c1c; }
  button.engage { width:100%; background:#1a7f37; font-size:19px; padding:18px; }
  .estop-status { width:100%; box-sizing:border-box; text-align:center; padding:20px;
    border-radius:8px; background:#3d1212; color:#ff8080; font-size:19px; font-weight:700; }
</style></head>
<body>
<header>🏎️ Hovercar mini controller</header>
<div class="wrap">
  <div class="estop-bar">
    <button id="stopBtn" class="estop" onclick="doStop()">■ EMERGENCY STOP</button>
    <div id="stopStatus" class="estop-status" style="display:none">⏳ Braking to a stop…</div>
    <button id="engageBtn" class="engage" onclick="doEngage()" style="display:none">▶ Engage</button>
  </div>
  <div class="grid">
    <div class="card"><div class="k">Preset</div><div class="v" id="profile">-</div></div>
    <div class="card"><div class="k">Link</div><div class="v"><span id="link" class="pill">-</span></div></div>
    <div class="card"><div class="k">Direction</div><div class="v" id="dir">-</div></div>
    <div class="card"><div class="k">Brake</div><div class="v" id="brake">-</div></div>
    <div class="card"><div class="k">Battery</div><div class="v" id="batt">-</div></div>
    <div class="card"><div class="k">Board temp</div><div class="v" id="temp">-</div></div>
    <div class="card"><div class="k">Speed L / R</div><div class="v" id="speed">-</div></div>
    <div class="card"><div class="k">Torque out</div><div class="v" id="torque">-</div></div>
    <div class="card"><div class="k">Throttle</div><div class="v" id="throttle">-</div></div>
    <div class="card"><div class="k">Brake pedal</div><div class="v" id="brakepct">-</div></div>
  </div>

  <fieldset><legend>Preset</legend>
    <label>Active preset</label>
    <select id="presetSel" onchange="selectPreset()"></select>
  </fieldset>

  <fieldset><legend>Edit / save preset</legend>
    <div id="editNote" style="font-size:12px;opacity:.7;margin-bottom:6px"></div>
    <form id="editForm" onsubmit="return false;" autocomplete="off">
    <label>Name</label><input id="presetName" type="text" maxlength="15" enterkeyhint="next">
    <label>Max torque (0-1000)</label><input id="maxTorque" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
    <label>Speed ceiling (rpm)</label><input id="speedCeiling" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
    <label>Launch torque cap</label><input id="launchTorqueCap" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
    <label>Brake power (0-1000)</label><input id="brakeTorqueMax" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
    <label>P gain</label><input id="pGain" type="number" inputmode="decimal" step="0.01" min="0" max="20" enterkeyhint="next">
    <label>I gain</label><input id="iGain" type="number" inputmode="decimal" step="0.01" min="0" max="5" enterkeyhint="next">
    <label>Ramp full-scale (ms, forward)</label><input id="rampMsFullScale" type="number" inputmode="numeric" min="150" max="3000" enterkeyhint="next">
    <label>Reverse max torque (0-1000)</label><input id="reverseMaxTorque" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
    <label>Reverse speed ceiling (rpm)</label><input id="reverseSpeedCeiling" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
    <label>Reverse ramp (ms)</label><input id="reverseRampMs" type="number" inputmode="numeric" min="150" max="3000" enterkeyhint="done">
    <label>Speed limiter</label>
    <select id="limitingEnabled"><option value="1">On</option><option value="0">Off</option></select>
    <div class="row" style="margin-top:12px">
      <button id="applyBtn" type="button" class="secondary" onclick="applyLimits()">Apply (live)</button>
      <button id="saveBtn" type="button" onclick="savePreset()">💾 Save preset</button>
    </div>
    </form>
  </fieldset>
  <div id="msg"></div>
</div>
<script>
const $ = id => document.getElementById(id);
async function refresh(){
  try{
    const r = await fetch('/api/state'); const s = await r.json();
    $('profile').textContent = s.profileName;
    $('link').textContent = s.linkOk ? 'OK' : 'LOST';
    $('link').className = 'pill ' + (s.linkOk ? 'ok' : 'bad');
    $('batt').textContent = (s.batVoltage_cV/100).toFixed(2) + ' V';
    $('temp').textContent = (s.boardTemp/10).toFixed(1) + ' °C';
    $('speed').textContent = s.speedL + ' / ' + s.speedR;
    $('torque').textContent = s.torqueSent;
    const act = s.activeForward ? 'FWD' : 'REV';
    const req = s.reqForward ? 'FWD' : 'REV';
    $('dir').textContent = (act === req) ? act : (act + '→' + req);
    $('brake').innerHTML = s.braking ? '<span class="pill bad">ON</span>' : '<span class="pill ok">off</span>';
    $('throttle').textContent = s.throttlePct + ' %';
    $('brakepct').textContent = s.brakePct + ' %';
    // Emergency-stop UI: STOP when running; once engaged show Engage so it can be
    // cancelled at any time (even while still braking), plus a "Braking…" banner
    // until the car has actually stopped.
    if (!s.estop) { hide('engageBtn'); hide('stopStatus'); show('stopBtn'); }
    else          { hide('stopBtn'); show('engageBtn'); s.stopped ? hide('stopStatus') : show('stopStatus'); }
    // Preset dropdown: rebuild options only when the name list changes.
    const sig = s.presets.map(p=>p.i+':'+p.name).join('|');
    if (sig !== presetSig){
      presetSig = sig;
      $('presetSel').innerHTML = s.presets.map(p=>`<option value="${p.i}">${p.name}${p.editable?'':' 🔒'}</option>`).join('');
    }
    if (document.activeElement !== $('presetSel')) $('presetSel').value = s.selected;
    // Load the edit form ONLY when the selected preset changes, so we don't stomp
    // in-progress edits (moving to the next field must not revert what you typed).
    if (s.selected !== lastSel){
      lastSel = s.selected;
      $('presetName').value = s.profileName;
      $('maxTorque').value = s.maxTorque;
      $('speedCeiling').value = s.speedCeiling;
      $('launchTorqueCap').value = s.launchTorqueCap;
      $('brakeTorqueMax').value = s.brakeTorqueMax;
      $('pGain').value = s.pGain;
      $('iGain').value = s.iGain;
      $('rampMsFullScale').value = s.rampMsFullScale;
      $('reverseMaxTorque').value = s.reverseMaxTorque;
      $('reverseSpeedCeiling').value = s.reverseSpeedCeiling;
      $('reverseRampMs').value = s.reverseRampMs;
      $('limitingEnabled').value = s.limitingEnabled ? '1':'0';
    }
    // Enable editing only for user presets (built-ins are read-only).
    const ed = s.editable;
    for (const k of ['presetName','maxTorque','speedCeiling','launchTorqueCap','brakeTorqueMax','pGain','iGain','rampMsFullScale','reverseMaxTorque','reverseSpeedCeiling','reverseRampMs','limitingEnabled','applyBtn','saveBtn']){
      $(k).disabled = !ed;
    }
    $('editNote').textContent = ed
      ? 'Editing “'+s.profileName+'” — Apply tests it live, Save keeps it in flash.'
      : 'Built-in preset — read-only. Pick preset 3–5 to customise & save.';
  }catch(e){ $('link').textContent='NO ESP'; $('link').className='pill bad'; }
}
let presetSig = '';
let lastSel = -1;   // last preset index the edit form was loaded from
function show(id){ $(id).style.display=''; }
function hide(id){ $(id).style.display='none'; }
async function post(url, extra){
  const p = new URLSearchParams();
  for (const k in extra) p.set(k, extra[k]);
  const r = await fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:p});
  const t = await r.text();
  $('msg').textContent = (r.ok ? '✓ ' : '✗ ') + t;
  $('msg').style.color = r.ok ? '#7CFFA0' : '#ff8080';
  refresh();
}
function doStop(){ post('/api/estop', {stop:1}); }
function doEngage(){ post('/api/estop', {stop:0}); }
function selectPreset(){ post('/api/select', {index:$('presetSel').value}); }
function limitFields(){
  return { maxTorque:$('maxTorque').value, speedCeiling:$('speedCeiling').value,
    launchTorqueCap:$('launchTorqueCap').value, brakeTorqueMax:$('brakeTorqueMax').value,
    pGain:$('pGain').value, iGain:$('iGain').value, rampMsFullScale:$('rampMsFullScale').value,
    reverseMaxTorque:$('reverseMaxTorque').value, reverseSpeedCeiling:$('reverseSpeedCeiling').value,
    reverseRampMs:$('reverseRampMs').value, limitingEnabled:$('limitingEnabled').value };
}
function applyLimits(){ post('/api/limits', limitFields()); }
function savePreset(){ post('/api/savePreset', Object.assign({name:$('presetName').value}, limitFields())); }
// Enter/Next on the mobile keyboard advances to the next field (last one blurs).
(function(){
  const els = Array.from(document.querySelectorAll('#editForm input'));
  els.forEach((el, i) => el.addEventListener('keydown', e => {
    if (e.key === 'Enter'){ e.preventDefault(); const n = els[i+1]; n ? n.focus() : el.blur(); }
  }));
})();
// Self-scheduling poll: wait for each response before the next, so requests can
// never pile up on the single-client server if WiFi briefly stalls.
const REFRESH_MS = 200;
(async function poll(){ await refresh(); setTimeout(poll, REFRESH_MS); })();
</script>
</body></html>)HTML";

// ---------------------------- helpers ---------------------------------------
static int16_t clampI(long v, long lo, long hi) {
  if (v < lo) v = lo; if (v > hi) v = hi; return (int16_t)v;
}
static float clampF(float v, float lo, float hi) {
  if (v < lo) v = lo; if (v > hi) v = hi; return v;
}

void WebInterface::begin() {
  WiFi.mode(WIFI_AP);
  // Empty (or too-short) password -> open network. >=8 chars enables WPA2.
  const char *pw = (strlen(AP_PASSWORD) >= 8) ? AP_PASSWORD : nullptr;
  WiFi.softAP(AP_SSID, pw, AP_CHANNEL, false /*hidden*/, AP_MAX_CLIENTS);
  _apIP = WiFi.softAPIP();

  // Captive portal: resolve every hostname to us so the phone's connectivity
  // probe hits our server and the OS auto-opens the page.
  _dns.start(53, "*", _apIP);

  _server.on("/", HTTP_GET, [this]() { handleRoot(); });
  _server.on("/api/state", HTTP_GET, [this]() { handleGetState(); });
  _server.on("/api/select", HTTP_POST, [this]() { handleSelect(); });
  _server.on("/api/limits", HTTP_POST, [this]() { handleSetLimits(); });
  _server.on("/api/savePreset", HTTP_POST, [this]() { handleSavePreset(); });
  _server.on("/api/estop", HTTP_POST, [this]() { handleEstop(); });
  // Any other path (incl. the OS captive-detection probes such as
  // /hotspot-detect.html, /generate_204, /ncsi.txt) -> redirect to our page,
  // which triggers the captive-portal sheet to open it.
  _server.onNotFound([this]() {
    _server.sendHeader("Location", String("http://") + _apIP.toString() + "/", true);
    _server.send(302, "text/plain", "");
  });
  _server.begin();
}

void WebInterface::handle() {
  _dns.processNextRequest();
  _server.handleClient();
}

bool WebInterface::authorized() {
  // Open control if no token is configured (convenient for bring-up).
  if (API_TOKEN[0] == '\0') return true;
  // Otherwise length-checked, constant-ish-time compare of the shared token.
  String tok = _server.arg("token");
  const size_t expLen = strlen(API_TOKEN);
  if (tok.length() != expLen) return false;
  // constant-ish time compare (avoid early-out on first mismatch)
  uint8_t diff = 0;
  for (size_t i = 0; i < expLen; i++) diff |= (uint8_t)(tok[i] ^ API_TOKEN[i]);
  return diff == 0;
}

void WebInterface::handleRoot() {
  _server.send_P(200, "text/html", INDEX_HTML);
}

void WebInterface::handleGetState() {
  Tunables  t  = g_state.getTunables();
  Telemetry tm = g_state.getTelemetry();

  // Preset list for the dropdown. Names are sanitised on save, so safe to embed.
  char plist[288]; int pn = 0;
  pn += snprintf(plist + pn, sizeof(plist) - pn, "[");
  for (int i = 0; i < PresetStore::COUNT; i++) {
    pn += snprintf(plist + pn, sizeof(plist) - pn,
                   "%s{\"i\":%d,\"name\":\"%s\",\"editable\":%s}",
                   i ? "," : "", i, g_presets.preset(i).profileName,
                   g_presets.editable(i) ? "true" : "false");
  }
  snprintf(plist + pn, sizeof(plist) - pn, "]");

  char buf[1200];
  snprintf(buf, sizeof(buf),
    "{\"profileName\":\"%s\",\"selected\":%d,\"editable\":%s,\"presets\":%s,"
    "\"linkOk\":%s,\"batVoltage_cV\":%d,\"boardTemp\":%d,"
    "\"speedL\":%d,\"speedR\":%d,\"torqueSent\":%d,\"braking\":%s,"
    "\"activeForward\":%s,\"reqForward\":%s,\"throttlePct\":%d,\"brakePct\":%d,"
    "\"maxTorque\":%d,\"speedCeiling\":%d,\"launchTorqueCap\":%d,\"brakeTorqueMax\":%d,"
    "\"pGain\":%.3f,\"iGain\":%.3f,\"rampMsFullScale\":%u,\"reverseMaxTorque\":%d,"
    "\"reverseSpeedCeiling\":%d,\"reverseRampMs\":%u,\"limitingEnabled\":%s,"
    "\"estop\":%s,\"stopped\":%s}",
    t.profileName, g_presets.selected(),
    g_presets.editable(g_presets.selected()) ? "true" : "false", plist,
    tm.linkOk ? "true" : "false", tm.batVoltage_cV, tm.boardTemp,
    tm.speedL, tm.speedR, tm.torqueSent, tm.braking ? "true" : "false",
    tm.activeForward ? "true" : "false", tm.reqForward ? "true" : "false",
    tm.throttlePct, tm.brakePct, t.maxTorque, t.speedCeiling, t.launchTorqueCap,
    t.brakeTorqueMax, t.pGain, t.iGain, t.rampMsFullScale, t.reverseMaxTorque,
    t.reverseSpeedCeiling, t.reverseRampMs, t.limitingEnabled ? "true" : "false",
    g_state.getEstop() ? "true" : "false", tm.stopped ? "true" : "false");
  _server.send(200, "application/json", buf);
}

// Overwrite the provided, clamped limit fields of `t` from the request args.
static void applyLimitArgs(WebServer &s, Tunables &t) {
  if (s.hasArg("maxTorque"))
    t.maxTorque = clampI(s.arg("maxTorque").toInt(), 0, TORQUE_ABS_MAX);
  if (s.hasArg("launchTorqueCap"))
    t.launchTorqueCap = clampI(s.arg("launchTorqueCap").toInt(), 0, TORQUE_ABS_MAX);
  if (s.hasArg("speedCeiling"))
    t.speedCeiling = clampI(s.arg("speedCeiling").toInt(), 0, SPEED_CEILING_MAX);
  if (s.hasArg("brakeTorqueMax"))
    t.brakeTorqueMax = clampI(s.arg("brakeTorqueMax").toInt(), 0, BRAKE_TORQUE_MAX);
  if (s.hasArg("pGain"))
    t.pGain = clampF(s.arg("pGain").toFloat(), 0.0f, PGAIN_MAX);
  if (s.hasArg("iGain"))
    t.iGain = clampF(s.arg("iGain").toFloat(), 0.0f, IGAIN_MAX);
  if (s.hasArg("rampMsFullScale"))
    t.rampMsFullScale = (uint16_t)clampI(s.arg("rampMsFullScale").toInt(), RAMP_MS_MIN, RAMP_MS_MAX);
  if (s.hasArg("reverseMaxTorque"))
    t.reverseMaxTorque = clampI(s.arg("reverseMaxTorque").toInt(), 0, TORQUE_ABS_MAX);
  if (s.hasArg("reverseSpeedCeiling"))
    t.reverseSpeedCeiling = clampI(s.arg("reverseSpeedCeiling").toInt(), 0, SPEED_CEILING_MAX);
  if (s.hasArg("reverseRampMs"))
    t.reverseRampMs = (uint16_t)clampI(s.arg("reverseRampMs").toInt(), RAMP_MS_MIN, RAMP_MS_MAX);
  if (s.hasArg("limitingEnabled"))
    t.limitingEnabled = (s.arg("limitingEnabled").toInt() != 0);
}

// Copy a user-supplied name into t.profileName, keeping only safe characters
// (avoids JSON/HTML injection when the name is echoed back). Max 15 chars.
static void applyNameArg(WebServer &s, Tunables &t) {
  if (!s.hasArg("name")) return;
  String in = s.arg("name");
  char out[sizeof(t.profileName)]; size_t o = 0;
  for (size_t i = 0; i < in.length() && o < sizeof(out) - 1; i++) {
    char c = in[i];
    if (isalnum((int)c) || c == ' ' || c == '-' || c == '_') out[o++] = c;
  }
  out[o] = '\0';
  if (o > 0) { strncpy(t.profileName, out, sizeof(t.profileName) - 1);
               t.profileName[sizeof(t.profileName) - 1] = '\0'; }
}

void WebInterface::handleSelect() {
  if (!authorized()) { _server.send(401, "text/plain", "bad token"); return; }
  int idx = _server.arg("index").toInt();
  if (idx < 0 || idx >= PresetStore::COUNT) { _server.send(400, "text/plain", "bad index"); return; }
  g_presets.select(idx);
  _server.send(200, "text/plain", "preset selected");
}

void WebInterface::handleSetLimits() {
  if (!authorized()) { _server.send(401, "text/plain", "bad token"); return; }
  // Apply edited limits to the LIVE tunables only (not persisted).
  Tunables t = g_state.getTunables();
  applyLimitArgs(_server, t);
  g_state.setTunables(t);
  _server.send(200, "text/plain", "limits applied");
}

void WebInterface::handleSavePreset() {
  if (!authorized()) { _server.send(401, "text/plain", "bad token"); return; }
  int idx = g_presets.selected();
  if (!g_presets.editable(idx)) { _server.send(400, "text/plain", "preset is read-only"); return; }
  // Persist the edited fields + name into the selected user slot (and go live).
  Tunables t = g_state.getTunables();
  applyLimitArgs(_server, t);
  applyNameArg(_server, t);
  g_state.setTunables(t);
  g_presets.saveUserPreset(idx, t);
  _server.send(200, "text/plain", "preset saved");
}

void WebInterface::handleEstop() {
  if (!authorized()) { _server.send(401, "text/plain", "bad token"); return; }
  bool stop = (_server.arg("stop").toInt() != 0);
  g_state.setEstop(stop);
  _server.send(200, "text/plain", stop ? "emergency stop engaged" : "engaged - input enabled");
}
