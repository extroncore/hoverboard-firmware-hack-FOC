// ============================================================================
//  WebInterface - SoftAP + HTTP dashboard/tuning UI. Runs in the Arduino loop()
//  task; the control task keeps running regardless of client activity.
//  Security (OWASP-aligned): WPA2 AP, shared-token check on every state change,
//  and server-side clamping of every value to the config.h safety ranges.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

class WebInterface {
public:
  void begin();
  void handle();   // call frequently from loop()

private:
  WebServer _server{80};
  DNSServer _dns;            // captive-portal DNS (all names -> AP IP)
  IPAddress _apIP;

  void handleRoot();
  void handlePortal();              // minimal, field-free launcher shown in the iOS captive sheet
  void handleGetState();            // lean live telemetry (polled at REFRESH_MS)
  void handleGetConfig();           // presets + calibration (fetched on load / after saves)
  void handleSelect();             // choose the active (running) preset
  void handleSavePreset();         // persist edits into a user slot (+ apply live if it's active)
  void handleSaveCalibration();    // persist global pedal/motion calibration (+ apply live)
  void handleConfigMode();         // enter/leave config mode (disables drive output)
  void handleEstop();              // emergency-stop engage/disengage
  bool authorized();               // true if request carries the valid token
};

extern WebInterface g_web;
