#include "HoverboardLink.h"

void HoverboardLink::begin(HardwareSerial &port, uint32_t baud, int rxPin, int txPin) {
  _port = &port;
  _port->begin(baud, SERIAL_8N1, rxPin, txPin);
  _idx = 0;
}

void HoverboardLink::sendCommand(int16_t steer, int16_t speed) {
  if (!_port) return;
  SerialCommand c;
  c.start    = HB_START_FRAME;
  c.steer    = steer;
  c.speed    = speed;
  c.checksum = commandChecksum(c);
  _port->write(reinterpret_cast<const uint8_t *>(&c), sizeof(c));
}

bool HoverboardLink::poll() {
  bool got = false;
  if (!_port) return false;

  while (_port->available() > 0) {
    uint8_t b = (uint8_t)_port->read();

    // Sync on the little-endian start frame 0xABCD -> bytes 0xCD, 0xAB.
    if (_idx == 0) {
      if (b == 0xCD) { _buf[_idx++] = b; }
    } else if (_idx == 1) {
      if (b == 0xAB) { _buf[_idx++] = b; }
      else           { _idx = (b == 0xCD) ? 1 : 0; }   // resync
    } else {
      _buf[_idx++] = b;
      if (_idx >= sizeof(SerialFeedback)) {
        SerialFeedback f;
        memcpy(&f, _buf, sizeof(f));
        if (f.start == HB_START_FRAME && f.checksum == feedbackChecksum(f)) {
          _fb       = f;
          _lastRxMs = millis();
          _rxFrames++;
          got       = true;
        } else {
          _rxErrors++;
        }
        _idx = 0;   // start looking for the next frame
      }
    }
  }
  return got;
}
