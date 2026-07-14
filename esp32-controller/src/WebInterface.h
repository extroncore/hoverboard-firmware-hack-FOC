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
  void handleGetState();
  void handleSelect();             // choose active preset
  void handleSetLimits();          // apply edited limits live (no persist)
  void handleSavePreset();         // persist current live tunables into a user slot
  void handleEstop();              // emergency-stop engage/disengage
  bool authorized();               // true if request carries the valid token
};

extern WebInterface g_web;
