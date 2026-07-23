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
<title>Hovercar BigBoy</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: system-ui, sans-serif; margin: 0; background:#111; color:#eee; }
  header { padding: 12px 16px; background:#1c1c1c; font-weight:600; font-size:18px;
    display:flex; align-items:center; justify-content:space-between; gap:12px; }
  .nav { display:flex; gap:8px; }
  .nav button { background:#333; color:#fff; border:0; border-radius:8px; font-size:14px;
    font-weight:600; padding:8px 14px; cursor:pointer; }
  .nav button.sel { background:#2b6cff; }
  .wrap { padding: 16px; max-width: 560px; margin: 0 auto; }
  .page { display:none; } .page.active { display:block; }
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
    color:#eee; border:1px solid #333; border-radius:6px; font-size:16px; }
  .hint { font-size:12px; opacity:.6; margin-top:4px; }
  .row { display:flex; gap:8px; }
  button { padding:10px 14px; border:0; border-radius:8px; background:#2b6cff; color:#fff;
    font-size:15px; font-weight:600; cursor:pointer; }
  button.secondary { background:#333; }
  input:disabled, button:disabled { opacity:.5; cursor:not-allowed; }
  input.companion { text-align:center; color:#9ec5ff; opacity:.85; }
  #msg { min-height:18px; font-size:13px; margin-top:8px; }
  .estop-bar { margin:14px 0 18px; }
  button.estop { width:100%; background:#c62828; font-size:22px; padding:20px; letter-spacing:.03em; }
  button.estop:active { background:#8e1c1c; }
  button.engage { width:100%; background:#1a7f37; font-size:19px; padding:18px; }
  .estop-status { width:100%; box-sizing:border-box; text-align:center; padding:20px;
    border-radius:8px; background:#3d1212; color:#ff8080; font-size:19px; font-weight:700; }
  .active-sel select { font-size:17px; font-weight:600; padding:10px; }
  .cfg-banner { background:#4a3800; color:#ffd466; border:1px solid #6b5200; border-radius:8px;
    padding:12px 14px; margin-bottom:16px; font-weight:600; }
  .firstrun { background:#123d1a; color:#7CFFA0; border:1px solid #1f6b34; border-radius:8px;
    padding:12px 14px; margin-bottom:16px; font-weight:600; }
  .autocal { background:#12233d; border:1px solid #24406b; border-radius:8px; padding:12px 14px; margin:12px 0; }
  #autoCalMsg { min-height:18px; font-size:14px; margin-top:8px; color:#9ec5ff; font-weight:600; }
</style></head>
<body>
<header>
  <span>&#127950; Hovercar BigBoy</span>
  <div class="nav">
    <button id="navDrive" class="sel" onclick="showPage('drive')">Drive</button>
    <button id="navConfig" onclick="showPage('config')">&#9881; Config</button>
  </div>
</header>
<div class="wrap">
  <!-- ================= DRIVE PAGE ================= -->
  <div id="pageDrive" class="page active">
    <div class="estop-bar">
      <button id="stopBtn" class="estop" onclick="doStop()">&#9632; EMERGENCY STOP</button>
      <div id="stopStatus" class="estop-status" style="display:none">Braking to a stop...</div>
      <button id="engageBtn" class="engage" onclick="doEngage()" style="display:none">&#9654; Engage</button>
    </div>
    <fieldset class="active-sel"><legend>Active profile</legend>
      <select id="activeSel" onchange="selectActive()"></select>
      <div class="hint">Switches the running profile <b>immediately</b>.</div>
    </fieldset>
    <div class="grid">
      <div class="card"><div class="k">Driving now</div><div class="v" id="profile">-</div></div>
      <div class="card"><div class="k">Link</div><div class="v"><span id="link" class="pill">-</span></div></div>
      <div class="card"><div class="k">Direction</div><div class="v" id="dir">-</div></div>
      <div class="card"><div class="k">Brake</div><div class="v" id="brake">-</div></div>
      <div class="card"><div class="k">Battery</div><div class="v" id="batt">-</div></div>
      <div class="card"><div class="k">Board temp</div><div class="v" id="temp">-</div></div>
      <div class="card"><div class="k">Speed</div><div class="v" id="speedKmh">-</div></div>
      <div class="card"><div class="k">Speed L / R (rpm)</div><div class="v" id="speed">-</div></div>
      <div class="card"><div class="k">Torque out</div><div class="v" id="torque">-</div></div>
      <div class="card"><div class="k">Throttle</div><div class="v" id="throttle">-</div></div>
    </div>
    <fieldset><legend>Edit / save preset</legend>
      <label>Preset to edit</label>
      <select id="editSel" onchange="loadEditPreset()"></select>
      <div class="hint">Loads a preset into the form below to review or change it. Saving the
        <b>active</b> profile also applies it to the car right away.</div>
      <div id="editNote" style="font-size:12px;opacity:.7;margin:10px 0 6px"></div>
      <form id="editForm" onsubmit="return false;" autocomplete="off">
      <label>Preset name</label>
      <input id="presetName" type="text" maxlength="15" enterkeyhint="next">
      <div class="hint">Shown in the profile lists above.</div>
      <label>Max power (forward)</label>
      <input id="maxTorque" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
      <div class="hint">How hard it can accelerate forward. 0-1000 (higher = stronger pull).</div>
      <label>Top speed (forward, km/h)</label>
      <div class="row">
        <input id="speedCeiling" type="number" inputmode="decimal" step="0.1" min="0" max="60" enterkeyhint="next" style="flex:3" oninput="updateSpeedCompanions()">
        <input id="speedCeiling_rpm" class="companion" type="text" disabled style="flex:1" title="rpm equivalent">
      </div>
      <div class="hint">km/h - needs the speed limiter ON (below). Right box is the rpm it maps to (uses the wheel size from Config).</div>
      <label>Launch limit</label>
      <input id="launchTorqueCap" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
      <div class="hint">Power cap from a standstill - softer take-off, less wheelspin. 0-1000.</div>
      <label>Brake strength</label>
      <input id="brakeTorqueMax" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
      <div class="hint">How hard the brake pedal stops it. Lower = gentler. 0-1000.</div>
      <label>Limiter strength (P)</label>
      <input id="pGain" type="number" inputmode="decimal" step="0.01" min="0" max="5" enterkeyhint="next">
      <div class="hint">Decimal, range 0.0 - 5.0. Instant reaction over the top speed. Examples: Race 1.5, Kid 2.0.</div>
      <label>Limiter follow-through (I)</label>
      <input id="iGain" type="number" inputmode="decimal" step="0.01" min="0" max="1" enterkeyhint="next">
      <div class="hint">Decimal, range 0.0 - 1.0. Slow trim that builds over ~1 s to hold the exact top speed.</div>
      <label>Throttle smoothing (forward)</label>
      <input id="rampMsFullScale" type="number" inputmode="numeric" min="150" max="3000" enterkeyhint="next">
      <div class="hint">How gradually power comes on - ms to reach full pull. Higher = softer, gentler.</div>
      <label>Reverse power</label>
      <input id="reverseMaxTorque" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="next">
      <div class="hint">Max power when driving backwards. 0-1000 (usually lower than forward).</div>
      <label>Reverse top speed (km/h)</label>
      <div class="row">
        <input id="reverseSpeedCeiling" type="number" inputmode="decimal" step="0.1" min="0" max="60" enterkeyhint="next" style="flex:3" oninput="updateSpeedCompanions()">
        <input id="reverseSpeedCeiling_rpm" class="companion" type="text" disabled style="flex:1" title="rpm equivalent">
      </div>
      <div class="hint">km/h - speed limit while reversing. Right box is the rpm it maps to (uses the wheel size from Config).</div>
      <label>Reverse smoothing</label>
      <input id="reverseRampMs" type="number" inputmode="numeric" min="150" max="3000" enterkeyhint="done">
      <div class="hint">Throttle smoothing when reversing - ms to reach full pull. Higher = softer.</div>
      <label>Speed limiter</label>
      <select id="limitingEnabled"><option value="1">On</option><option value="0">Off</option></select>
      <div class="hint">Master switch for the top-speed governor (uses the P / I values above).</div>
      <div class="row" style="margin-top:14px">
        <button id="saveBtn" type="button" style="flex:1" onclick="savePreset()">Save preset</button>
      </div>
      </form>
    </fieldset>
  </div>

  <!-- ================= CONFIG PAGE ================= -->
  <div id="pageConfig" class="page">
    <div class="cfg-banner">&#9881; Config mode - drive output is <b>DISABLED</b>. Pedals &amp; the
      direction switch are read for calibration only; the cart will not move.</div>
    <div id="firstRun" class="firstrun" style="display:none">First-time setup: this car isn't
      calibrated yet. Set the pedals and wheel size, Save, then head to Drive.</div>

    <fieldset><legend>Wheel &amp; live readings</legend>
      <label>Wheel diameter (mm)</label>
      <input id="wheelDiaMm" type="number" inputmode="numeric" min="50" max="1000" enterkeyhint="next" oninput="refresh()">
      <div class="hint">Sets the rpm to km/h conversion for the whole app. 6.5" hub ~165 mm,
        8.5" ~216 mm, 10" ~254 mm.</div>
      <div class="grid" style="margin:12px 0">
        <div class="card"><div class="k">Speed now</div><div class="v" id="cfgKmh">-</div></div>
        <div class="card"><div class="k">Speed L / R (rpm)</div><div class="v" id="cfgRpm">-</div></div>
        <div class="card"><div class="k">Throttle raw now</div><div class="v" id="calThrRaw">-</div></div>
        <div class="card"><div class="k">Brake raw now</div><div class="v" id="calBrkRaw">-</div></div>
      </div>
    </fieldset>

    <fieldset><legend>Pedal calibration</legend>
      <div class="autocal">
        <div class="row">
          <button type="button" class="secondary" style="flex:1" onclick="startAutoCal('throttle')">Auto-calibrate throttle</button>
          <button type="button" class="secondary" style="flex:1" onclick="startAutoCal('brake')">Auto-calibrate brake</button>
        </div>
        <div id="autoCalMsg"></div>
        <div class="hint">Follow the prompt: hold the pedal fully, then release. It captures min/max
          (with a small margin) and fills the fields below to review.</div>
      </div>
      <form id="calForm" onsubmit="return false;" autocomplete="off">
      <label>Throttle raw - released (min)</label>
      <input id="throttleRawMin" type="number" inputmode="numeric" min="0" max="4095" enterkeyhint="next">
      <label>Throttle raw - fully pressed (max)</label>
      <input id="throttleRawMax" type="number" inputmode="numeric" min="0" max="4095" enterkeyhint="next">
      <label>Throttle deadband (raw)</label>
      <input id="throttleDeadband" type="number" inputmode="numeric" min="0" max="500" enterkeyhint="next">
      <div class="hint">Low-end raw counts ignored so a resting pedal reads 0 %.</div>
      <label>Brake raw - released (min)</label>
      <input id="brakeRawMin" type="number" inputmode="numeric" min="0" max="4095" enterkeyhint="next">
      <label>Brake raw - fully pressed (max)</label>
      <input id="brakeRawMax" type="number" inputmode="numeric" min="0" max="4095" enterkeyhint="next">
      <label>Brake deadband (raw)</label>
      <input id="brakeDeadband" type="number" inputmode="numeric" min="0" max="500" enterkeyhint="next">
      </form>
    </fieldset>

    <fieldset><legend>Motion thresholds</legend>
      <div class="hint" style="margin-bottom:8px">These stay in <b>rpm</b> (they're only a few rpm,
        too small to set precisely in km/h). The greyed box beside each shows the equivalent speed.</div>
      <form id="thrForm" onsubmit="return false;" autocomplete="off">
      <label>Launch speed threshold (rpm)</label>
      <div class="row">
        <input id="launchSpeedThresh" type="number" inputmode="numeric" min="1" max="500" enterkeyhint="next" style="flex:3" oninput="updateSpeedCompanions()">
        <input id="launchSpeedThresh_kmh" class="companion" type="text" disabled style="flex:1" title="km/h equivalent">
      </div>
      <div class="hint">Below this the car is "launching" (launch cap applies) and it's safe to change direction.</div>
      <label>Near-stop threshold (rpm)</label>
      <div class="row">
        <input id="nearStopThresh" type="number" inputmode="numeric" min="1" max="500" enterkeyhint="next" style="flex:3" oninput="updateSpeedCompanions()">
        <input id="nearStopThresh_kmh" class="companion" type="text" disabled style="flex:1" title="km/h equivalent">
      </div>
      <div class="hint">At/below this the car counts as stopped and adopts the direction switch position.</div>
      <label>Brake blend speed (rpm)</label>
      <div class="row">
        <input id="brakeBlendSpeed" type="number" inputmode="numeric" min="1" max="500" enterkeyhint="next" style="flex:3" oninput="updateSpeedCompanions()">
        <input id="brakeBlendSpeed_kmh" class="companion" type="text" disabled style="flex:1" title="km/h equivalent">
      </div>
      <div class="hint">Braking torque tapers to zero below this so the car eases to a stop. Higher = softer stop.</div>
      <label>Pre-reversal brake torque</label>
      <input id="dirChangeBrakeTorque" type="number" inputmode="numeric" min="0" max="1000" enterkeyhint="done">
      <div class="hint">Gentle automatic slow-down applied before a requested reversal is allowed. Small = softer.</div>
      </form>
      <div class="row" style="margin-top:14px">
        <button id="saveCalBtn" type="button" style="flex:1" onclick="saveConfig()">Save config</button>
      </div>
    </fieldset>
  </div>
  <div id="msg"></div>
</div>
<script>
const $ = id => document.getElementById(id);
// Preset-editor fields. speedCeiling / reverseSpeedCeiling are stored in rpm but
// shown/entered in km/h (converted at the UI boundary; companion id + "_rpm").
const FIELDS = ['maxTorque','speedCeiling','launchTorqueCap','brakeTorqueMax','pGain','iGain',
                'rampMsFullScale','reverseMaxTorque','reverseSpeedCeiling','reverseRampMs'];
const SPEED_KMH_INPUTS = ['speedCeiling','reverseSpeedCeiling'];
// Global-config fields: all stored/POSTed as-is (raw / rpm / torque / mm).
const CAL_FIELDS = ['throttleRawMin','throttleRawMax','throttleDeadband','brakeRawMin','brakeRawMax',
                    'brakeDeadband','dirChangeBrakeTorque','wheelDiaMm',
                    'launchSpeedThresh','nearStopThresh','brakeBlendSpeed'];
// Config thresholds: rpm is the real input, with a disabled km/h readout (id + "_kmh").
const SPEED_RPM_INPUTS = ['launchSpeedThresh','nearStopThresh','brakeBlendSpeed'];
let presetSig = '';
let lastPresets = [];
let editIdx = 0;
let editInit = false;
let calInit = false;
let currentPage = 'drive';
let autoCal = null;
let lastCalib = {};

// rpm <-> km/h. Wheel size from the live input if present, else last saved calib.
function wheelDiaMm(){ const v = parseFloat($('wheelDiaMm').value); return v > 0 ? v : (lastCalib.wheelDiaMm || 165); }
function rpmToKmh(rpm){ return rpm * Math.PI * (wheelDiaMm()/1000) * 0.06; }
function kmhToRpm(kmh){ const f = Math.PI * (wheelDiaMm()/1000) * 0.06; return f > 0 ? kmh/f : 0; }
const round1 = x => Math.round(x*10)/10;

function updateSpeedCompanions(){
  for (const k of SPEED_KMH_INPUTS){ const el = $(k+'_rpm'); if (el) el.value = Math.round(kmhToRpm(parseFloat($(k).value)||0)) + ' rpm'; }
  for (const k of SPEED_RPM_INPUTS){ const el = $(k+'_kmh'); if (el) el.value = rpmToKmh(parseFloat($(k).value)||0).toFixed(1) + ' km/h'; }
}
// Calibration counts as present only if the flag is set AND the stored data is
// actually valid (pressed > released for both pedals) - belt & braces.
function calibValid(s){
  return !!(s && s.calibrated && s.calib &&
    s.calib.throttleRawMax > s.calib.throttleRawMin &&
    s.calib.brakeRawMax > s.calib.brakeRawMin);
}

// Page navigation. Entering Config puts the firmware in config mode (drive
// output disabled); leaving it re-enables driving.
function showPage(name){
  currentPage = name;
  $('pageDrive').classList.toggle('active', name === 'drive');
  $('pageConfig').classList.toggle('active', name === 'config');
  $('navDrive').classList.toggle('sel', name === 'drive');
  $('navConfig').classList.toggle('sel', name === 'config');
  postQuiet('/api/configMode', { on: name === 'config' ? 1 : 0 });
  window.scrollTo(0, 0);
}

function fillForm(p){
  $('presetName').value = p.name;
  for (const k of FIELDS) $(k).value = SPEED_KMH_INPUTS.includes(k) ? round1(rpmToKmh(p[k])) : p[k];
  $('limitingEnabled').value = p.limitingEnabled ? '1' : '0';
  updateSpeedCompanions();
}

async function refresh(){
  try{
    const r = await fetch('/api/state'); const s = await r.json();
    lastPresets = s.presets;
    lastCalib = s.calib || {};
    // Calibration gate: until valid calibration exists, keep the user on Config
    // and disable the Drive page entirely.
    const calOk = calibValid(s);
    $('navDrive').disabled = !calOk;
    $('firstRun').style.display = calOk ? 'none' : '';
    if (!calOk && currentPage !== 'config') showPage('config');
    // Drive dashboard
    $('profile').textContent = s.activeName;
    $('link').textContent = s.linkOk ? 'OK' : 'LOST';
    $('link').className = 'pill ' + (s.linkOk ? 'ok' : 'bad');
    $('batt').textContent = (s.batVoltage_cV/100).toFixed(2) + ' V';
    $('temp').textContent = (s.boardTemp/10).toFixed(1) + ' C';
    $('speed').textContent = s.speedL + ' / ' + s.speedR;
    const kmhNow = rpmToKmh((Math.abs(s.speedL)+Math.abs(s.speedR))/2);
    $('speedKmh').textContent = kmhNow.toFixed(1) + ' km/h';
    $('torque').textContent = s.torqueSent;
    const act = s.activeForward ? 'FWD' : 'REV';
    const req = s.reqForward ? 'FWD' : 'REV';
    $('dir').textContent = (act === req) ? act : (act + '->' + req);
    $('brake').innerHTML = s.braking ? '<span class="pill bad">ON</span>' : '<span class="pill ok">off</span>';
    $('throttle').textContent = s.throttlePct + ' %';
    if (!s.estop) { hide('engageBtn'); hide('stopStatus'); show('stopBtn'); }
    else          { hide('stopBtn'); show('engageBtn'); s.stopped ? hide('stopStatus') : show('stopStatus'); }
    const sig = s.presets.map(p=>p.i+':'+p.name).join('|');
    if (sig !== presetSig){
      presetSig = sig;
      const opts = s.presets.map(p=>`<option value="${p.i}">${p.name}${p.editable?'':' (locked)'}</option>`).join('');
      $('activeSel').innerHTML = opts;
      $('editSel').innerHTML = opts;
    }
    if (document.activeElement !== $('activeSel')) $('activeSel').value = s.selected;
    if (!editInit){ editInit = true; editIdx = s.selected; $('editSel').value = editIdx; fillForm(s.presets[editIdx]); }
    if (document.activeElement !== $('editSel')) $('editSel').value = editIdx;
    const ep = s.presets[editIdx] || {};
    const ed = !!ep.editable;
    for (const k of ['presetName', ...FIELDS, 'limitingEnabled', 'saveBtn']) $(k).disabled = !ed;
    const isActive = (editIdx === s.selected);
    $('editNote').textContent = ed
      ? (isActive
          ? 'Editing "'+ep.name+'" - the ACTIVE profile. Save writes it to flash and applies it to the car now.'
          : 'Editing "'+ep.name+'". Save writes it to flash; switch it to Active above to drive with it.')
      : 'Built-in preset - read-only. Pick preset 3-5 to customise & save.';
    // Config page live readings
    $('cfgRpm').textContent = s.speedL + ' / ' + s.speedR;
    $('cfgKmh').textContent = kmhNow.toFixed(1) + ' km/h';
    $('calThrRaw').textContent = s.throttleRaw;
    $('calBrkRaw').textContent = s.brakeRaw;
    if (!calInit && s.calib){ calInit = true; for (const k of CAL_FIELDS) if ($(k)) $(k).value = s.calib[k]; }
    updateSpeedCompanions();
    tickAutoCal(s);
  }catch(e){ $('link').textContent='NO ESP'; $('link').className='pill bad'; }
}
function show(id){ $(id).style.display=''; }
function hide(id){ $(id).style.display='none'; }
async function post(url, extra){
  const p = new URLSearchParams();
  for (const k in extra) p.set(k, extra[k]);
  const r = await fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:p});
  const t = await r.text();
  $('msg').textContent = (r.ok ? 'OK ' : 'X ') + t;
  $('msg').style.color = r.ok ? '#7CFFA0' : '#ff8080';
  refresh();
}
async function postQuiet(url, extra){
  const p = new URLSearchParams();
  for (const k in extra) p.set(k, extra[k]);
  try { await fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:p}); } catch(e){}
}
function doStop(){ post('/api/estop', {stop:1}); }
function doEngage(){ post('/api/estop', {stop:0}); }
function selectActive(){ post('/api/select', {index:$('activeSel').value}); }
function loadEditPreset(){
  editIdx = parseInt($('editSel').value, 10);
  const p = lastPresets[editIdx];
  if (p) fillForm(p);
  refresh();
}
function limitFields(){
  const o = {};
  for (const k of FIELDS) o[k] = SPEED_KMH_INPUTS.includes(k) ? Math.round(kmhToRpm(parseFloat($(k).value)||0)) : $(k).value;
  o.limitingEnabled = $('limitingEnabled').value;
  return o;
}
function savePreset(){ post('/api/savePreset', Object.assign({index:editIdx, name:$('presetName').value}, limitFields())); }
// Save global config. Every field is sent as-is (rpm / raw counts / mm); the
// firmware clamps and stores in rpm - no km/h ever crosses the wire.
function saveConfig(){
  const o = {};
  for (const k of CAL_FIELDS) o[k] = $(k).value;
  post('/api/saveCalibration', o);
}

// Auto-calibrate: two-phase guided capture driven by the poll loop. Phase 'press'
// (hold full) then 'release'. Min/max tracked across the whole window.
const AUTOCAL_PHASE_MS = 5000;
const AUTOCAL_MIN_SPAN = 500; // min raw counts required between released & pressed
function startAutoCal(kind){
  autoCal = { kind, phase:'press', tEnd: Date.now()+AUTOCAL_PHASE_MS, min:4095, max:0 };
  renderAutoCal();
}
function tickAutoCal(s){
  if (!autoCal) return;
  const raw = autoCal.kind === 'throttle' ? s.throttleRaw : s.brakeRaw;
  autoCal.min = Math.min(autoCal.min, raw);
  autoCal.max = Math.max(autoCal.max, raw);
  if (Date.now() >= autoCal.tEnd){
    if (autoCal.phase === 'press'){ autoCal.phase = 'release'; autoCal.tEnd = Date.now()+AUTOCAL_PHASE_MS; }
    else { finishAutoCal(); return; }
  }
  renderAutoCal();
}
function renderAutoCal(){
  if (!autoCal){ $('autoCalMsg').textContent = ''; return; }
  const secs = Math.max(0, Math.ceil((autoCal.tEnd - Date.now())/1000));
  const name = autoCal.kind === 'throttle' ? 'THROTTLE' : 'BRAKE';
  $('autoCalMsg').textContent = autoCal.phase === 'press'
    ? `Press & HOLD the ${name} fully... ${secs}s`
    : `Now RELEASE the ${name} completely... ${secs}s`;
}
function finishAutoCal(){
  const kind = autoCal.kind, lo0 = autoCal.min, hi0 = autoCal.max;
  autoCal = null;
  const span = hi0 - lo0;
  if (hi0 <= lo0 || span < AUTOCAL_MIN_SPAN){
    $('autoCalMsg').textContent = `X ${kind} calibration failed: need min < max with at least ${AUTOCAL_MIN_SPAN} counts between them (got min ${lo0}, max ${hi0}). Try again - hold the pedal fully, then release.`;
    return;
  }
  const margin = Math.max(20, Math.round(span*0.05));
  const lo = lo0 + margin, hi = hi0 - margin;
  if (kind === 'throttle'){ $('throttleRawMin').value = lo; $('throttleRawMax').value = hi; }
  else { $('brakeRawMin').value = lo; $('brakeRawMax').value = hi; }
  $('autoCalMsg').textContent = `OK ${kind} captured: min ${lo}, max ${hi}. Review & Save config.`;
}

(function(){
  const els = Array.from(document.querySelectorAll('#editForm input:not(.companion), #calForm input, #thrForm input:not(.companion)'));
  els.forEach((el, i) => el.addEventListener('keydown', e => {
    if (e.key === 'Enter'){ e.preventDefault(); const n = els[i+1]; n ? n.focus() : el.blur(); }
  }));
})();
// iOS: the numeric keypad has no "Done" key and tapping empty space doesn't blur
// an input, so the keyboard can get stuck. Tapping anything that isn't itself a
// field dismisses it (buttons still fire their click after the blur).
document.addEventListener('pointerdown', e => {
  const a = document.activeElement;
  if (a && (a.tagName === 'INPUT' || a.tagName === 'SELECT' || a.tagName === 'TEXTAREA')
      && !e.target.closest('input, select, textarea, label')) a.blur();
});
const REFRESH_MS = 200;
(async function poll(){ await refresh(); setTimeout(poll, REFRESH_MS); })();
</script>
</body></html>)HTML";

// ---------------------------- helpers ---------------------------------------
static int16_t clampI(long v, long lo, long hi)
{
  if (v < lo)
    v = lo;
  if (v > hi)
    v = hi;
  return (int16_t)v;
}
static float clampF(float v, float lo, float hi)
{
  if (v < lo)
    v = lo;
  if (v > hi)
    v = hi;
  return v;
}

void WebInterface::begin()
{
  WiFi.mode(WIFI_AP);
  // Empty (or too-short) password -> open network. >=8 chars enables WPA2.
  const char *pw = (strlen(AP_PASSWORD) >= 8) ? AP_PASSWORD : nullptr;
  WiFi.softAP(AP_SSID, pw, AP_CHANNEL, false /*hidden*/, AP_MAX_CLIENTS);
  _apIP = WiFi.softAPIP();

  // Captive portal: resolve every hostname to us so the phone's connectivity
  // probe hits our server and the OS auto-opens the page.
  _dns.start(53, "*", _apIP);

  _server.on("/", HTTP_GET, [this]()
             { handleRoot(); });
  _server.on("/api/state", HTTP_GET, [this]()
             { handleGetState(); });
  _server.on("/api/select", HTTP_POST, [this]()
             { handleSelect(); });
  _server.on("/api/savePreset", HTTP_POST, [this]()
             { handleSavePreset(); });
  _server.on("/api/saveCalibration", HTTP_POST, [this]()
             { handleSaveCalibration(); });
  _server.on("/api/configMode", HTTP_POST, [this]()
             { handleConfigMode(); });
  _server.on("/api/estop", HTTP_POST, [this]()
             { handleEstop(); });
  // Any other path (incl. the OS captive-detection probes such as
  // /hotspot-detect.html, /generate_204, /ncsi.txt) -> redirect to our page,
  // which triggers the captive-portal sheet to open it.
  _server.onNotFound([this]()
                     {
    _server.sendHeader("Location", String("http://") + _apIP.toString() + "/", true);
    _server.send(302, "text/plain", ""); });
  _server.begin();
}

void WebInterface::handle()
{
  _dns.processNextRequest();
  _server.handleClient();
}

bool WebInterface::authorized()
{
  // Open control if no token is configured (convenient for bring-up).
  if (API_TOKEN[0] == '\0')
    return true;
  // Otherwise length-checked, constant-ish-time compare of the shared token.
  String tok = _server.arg("token");
  const size_t expLen = strlen(API_TOKEN);
  if (tok.length() != expLen)
    return false;
  // constant-ish time compare (avoid early-out on first mismatch)
  uint8_t diff = 0;
  for (size_t i = 0; i < expLen; i++)
    diff |= (uint8_t)(tok[i] ^ API_TOKEN[i]);
  return diff == 0;
}

void WebInterface::handleRoot()
{
  _server.send_P(200, "text/html", INDEX_HTML);
}

// Format one preset (name + full tunables) as a JSON object. Names are
// sanitised on save, so they're safe to embed. Returns snprintf's count.
static int fmtPreset(char *out, size_t n, int i, const Tunables &t, bool editable)
{
  return snprintf(out, n,
                  "{\"i\":%d,\"name\":\"%s\",\"editable\":%s,"
                  "\"maxTorque\":%d,\"speedCeiling\":%d,\"launchTorqueCap\":%d,\"brakeTorqueMax\":%d,"
                  "\"pGain\":%.3f,\"iGain\":%.3f,\"rampMsFullScale\":%u,"
                  "\"reverseMaxTorque\":%d,\"reverseSpeedCeiling\":%d,\"reverseRampMs\":%u,"
                  "\"limitingEnabled\":%s}",
                  i, t.profileName, editable ? "true" : "false",
                  t.maxTorque, t.speedCeiling, t.launchTorqueCap, t.brakeTorqueMax,
                  t.pGain, t.iGain, t.rampMsFullScale,
                  t.reverseMaxTorque, t.reverseSpeedCeiling, t.reverseRampMs,
                  t.limitingEnabled ? "true" : "false");
}

void WebInterface::handleGetState()
{
  Tunables t = g_state.getTunables(); // live / active profile
  Telemetry tm = g_state.getTelemetry();

  // All presets with their full tunables, so the editor can load any of them
  // (not just the active one) without an extra round-trip. static: this handler
  // only runs in the single-threaded loop() context, and it keeps these ~1.5 KB
  // of scratch off the stack.
  static char plist[1400];
  int pn = 0;
  pn += snprintf(plist + pn, sizeof(plist) - pn, "[");
  for (int i = 0; i < PresetStore::COUNT; i++)
  {
    if (i)
      pn += snprintf(plist + pn, sizeof(plist) - pn, ",");
    pn += fmtPreset(plist + pn, sizeof(plist) - pn, i, g_presets.preset(i), g_presets.editable(i));
  }
  snprintf(plist + pn, sizeof(plist) - pn, "]");

  // Global calibration (pedal mapping + motion thresholds). Nested object so the
  // editor can bind directly to it. throttleRaw/brakeRaw are the LIVE raw ADC
  // readings, exposed here so the user can calibrate by watching them move.
  const Calibration &c = g_presets.calibration();
  char calib[360];
  snprintf(calib, sizeof(calib),
           "{\"throttleRawMin\":%d,\"throttleRawMax\":%d,\"throttleDeadband\":%d,"
           "\"brakeRawMin\":%d,\"brakeRawMax\":%d,\"brakeDeadband\":%d,"
           "\"launchSpeedThresh\":%d,\"nearStopThresh\":%d,\"brakeBlendSpeed\":%d,"
           "\"dirChangeBrakeTorque\":%d,\"wheelDiaMm\":%d}",
           c.throttleRawMin, c.throttleRawMax, c.throttleDeadband,
           c.brakeRawMin, c.brakeRawMax, c.brakeDeadband,
           c.launchSpeedThresh, c.nearStopThresh, c.brakeBlendSpeed,
           c.dirChangeBrakeTorque, c.wheelDiaMm);

  static char buf[2200];
  snprintf(buf, sizeof(buf),
           "{\"activeName\":\"%s\",\"selected\":%d,\"presets\":%s,\"calib\":%s,"
           "\"calibrated\":%s,\"configMode\":%s,"
           "\"linkOk\":%s,\"batVoltage_cV\":%d,\"boardTemp\":%d,"
           "\"speedL\":%d,\"speedR\":%d,\"torqueSent\":%d,\"braking\":%s,"
           "\"activeForward\":%s,\"reqForward\":%s,\"throttlePct\":%d,\"brakePct\":%d,"
           "\"throttleRaw\":%d,\"brakeRaw\":%d,"
           "\"estop\":%s,\"stopped\":%s}",
           t.profileName, g_presets.selected(), plist, calib,
           g_presets.calibrated() ? "true" : "false", g_state.getConfigMode() ? "true" : "false",
           tm.linkOk ? "true" : "false", tm.batVoltage_cV, tm.boardTemp,
           tm.speedL, tm.speedR, tm.torqueSent, tm.braking ? "true" : "false",
           tm.activeForward ? "true" : "false", tm.reqForward ? "true" : "false",
           tm.throttlePct, tm.brakePct, tm.throttleRaw, tm.brakeRaw,
           g_state.getEstop() ? "true" : "false", tm.stopped ? "true" : "false");
  _server.send(200, "application/json", buf);
}

// Overwrite the provided, clamped limit fields of `t` from the request args.
static void applyLimitArgs(WebServer &s, Tunables &t)
{
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
static void applyNameArg(WebServer &s, Tunables &t)
{
  if (!s.hasArg("name"))
    return;
  String in = s.arg("name");
  char out[sizeof(t.profileName)];
  size_t o = 0;
  for (size_t i = 0; i < in.length() && o < sizeof(out) - 1; i++)
  {
    char c = in[i];
    if (isalnum((int)c) || c == ' ' || c == '-' || c == '_')
      out[o++] = c;
  }
  out[o] = '\0';
  if (o > 0)
  {
    strncpy(t.profileName, out, sizeof(t.profileName) - 1);
    t.profileName[sizeof(t.profileName) - 1] = '\0';
  }
}

void WebInterface::handleSelect()
{
  if (!authorized())
  {
    _server.send(401, "text/plain", "bad token");
    return;
  }
  int idx = _server.arg("index").toInt();
  if (idx < 0 || idx >= PresetStore::COUNT)
  {
    _server.send(400, "text/plain", "bad index");
    return;
  }
  g_presets.select(idx);
  _server.send(200, "text/plain", "preset selected");
}

void WebInterface::handleSavePreset()
{
  if (!authorized())
  {
    _server.send(401, "text/plain", "bad token");
    return;
  }
  // The edit target is an explicit slot index (the editor can point at a preset
  // other than the active one).
  int idx = _server.arg("index").toInt();
  if (idx < 0 || idx >= PresetStore::COUNT)
  {
    _server.send(400, "text/plain", "bad index");
    return;
  }
  if (!g_presets.editable(idx))
  {
    _server.send(400, "text/plain", "preset is read-only");
    return;
  }
  // Start from the stored preset, overlay the edited fields + name, persist.
  Tunables t = g_presets.preset(idx);
  applyLimitArgs(_server, t);
  applyNameArg(_server, t);
  g_presets.saveUserPreset(idx, t);
  // If we just saved the profile the car is running, apply it live too.
  if (idx == g_presets.selected())
    g_state.setTunables(t);
  _server.send(200, "text/plain", "preset saved");
}

// Overwrite the provided, clamped fields of the global calibration `c` from the
// request args. Same defence-in-depth clamping as the preset limits: every value
// is bounded to a safe range server-side regardless of what a client sends.
static void applyCalibArgs(WebServer &s, Calibration &c)
{
  if (s.hasArg("throttleRawMin"))
    c.throttleRawMin = clampI(s.arg("throttleRawMin").toInt(), 0, ADC_RAW_MAX);
  if (s.hasArg("throttleRawMax"))
    c.throttleRawMax = clampI(s.arg("throttleRawMax").toInt(), 0, ADC_RAW_MAX);
  if (s.hasArg("throttleDeadband"))
    c.throttleDeadband = clampI(s.arg("throttleDeadband").toInt(), 0, DEADBAND_RAW_MAX);
  if (s.hasArg("brakeRawMin"))
    c.brakeRawMin = clampI(s.arg("brakeRawMin").toInt(), 0, ADC_RAW_MAX);
  if (s.hasArg("brakeRawMax"))
    c.brakeRawMax = clampI(s.arg("brakeRawMax").toInt(), 0, ADC_RAW_MAX);
  if (s.hasArg("brakeDeadband"))
    c.brakeDeadband = clampI(s.arg("brakeDeadband").toInt(), 0, DEADBAND_RAW_MAX);
  // Motion thresholds: kept >= 1 (a zero rpm threshold would disable the
  // near-stop / blend logic the direction latch and brake taper rely on).
  if (s.hasArg("launchSpeedThresh"))
    c.launchSpeedThresh = clampI(s.arg("launchSpeedThresh").toInt(), 1, SPEED_THRESH_MAX);
  if (s.hasArg("nearStopThresh"))
    c.nearStopThresh = clampI(s.arg("nearStopThresh").toInt(), 1, SPEED_THRESH_MAX);
  if (s.hasArg("brakeBlendSpeed"))
    c.brakeBlendSpeed = clampI(s.arg("brakeBlendSpeed").toInt(), 1, SPEED_THRESH_MAX);
  if (s.hasArg("dirChangeBrakeTorque"))
    c.dirChangeBrakeTorque = clampI(s.arg("dirChangeBrakeTorque").toInt(), 0, TORQUE_ABS_MAX);
  if (s.hasArg("wheelDiaMm"))
    c.wheelDiaMm = clampI(s.arg("wheelDiaMm").toInt(), WHEEL_DIA_MM_MIN, WHEEL_DIA_MM_MAX);
}

void WebInterface::handleSaveCalibration()
{
  if (!authorized())
  {
    _server.send(401, "text/plain", "bad token");
    return;
  }
  // Start from the stored calibration, overlay the edited fields, persist +
  // apply live (saveCalibration publishes it to the control task).
  Calibration c = g_presets.calibration();
  applyCalibArgs(_server, c);
  g_presets.saveCalibration(c);
  _server.send(200, "text/plain", "calibration saved");
}

void WebInterface::handleConfigMode()
{
  if (!authorized())
  {
    _server.send(401, "text/plain", "bad token");
    return;
  }
  bool on = (_server.arg("on").toInt() != 0);
  g_state.setConfigMode(on);
  _server.send(200, "text/plain", on ? "config mode on (drive disabled)" : "config mode off");
}

void WebInterface::handleEstop()
{
  if (!authorized())
  {
    _server.send(401, "text/plain", "bad token");
    return;
  }
  bool stop = (_server.arg("stop").toInt() != 0);
  g_state.setEstop(stop);
  _server.send(200, "text/plain", stop ? "emergency stop engaged" : "engaged - input enabled");
}
