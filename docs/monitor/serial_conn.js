'use strict';

// WebSerial/WebUSB transport for the LcdTap JSON protocol.
// Shared by the GitHub-Pages monitor and the WiFi setup page.

export class SerialConnection {
  constructor() {
    this._port = null;
    this._reader = null;
    this._writer = null;
    this._lineBuffer = '';
    this._lineResolvers = [];
    this._running = false;
    this._ready = false;
    // Kept so an unexpected drop (e.g. a self-reset after switching the
    // output interface) can re-find and re-open the same granted port.
    this._serialApi = null;
    this._portInfo = null;
    // Fired when the read loop ends while we still expected to be connected.
    this.onLost = null;
  }

  get connected() { return this._ready; }

  async connect(serialApi) {
    this._serialApi = serialApi;
    const port = await serialApi.requestPort();
    await this._open(port);
  }

  // Re-open the previously granted port after an unexpected drop. The device
  // re-enumerates with the same USB VID/PID, so the matching SerialPort can be
  // re-opened without prompting again. Throws while the device is still gone.
  async reopen() {
    if (!this._serialApi) throw new Error('Never connected');
    const ports = await this._serialApi.getPorts();
    let port = null;
    if (this._portInfo && this._portInfo.usbVendorId != null) {
      port = ports.find(p => {
        let info;
        try { info = p.getInfo(); } catch (_) { return false; }
        return info.usbVendorId === this._portInfo.usbVendorId &&
               info.usbProductId === this._portInfo.usbProductId;
      });
    }
    if (!port) port = ports[0];
    if (!port) throw new Error('Serial port not available yet');
    await this._open(port);
  }

  async _open(port) {
    await port.open({ baudRate: 115200 });
    this._port = port;
    try { this._portInfo = port.getInfo(); } catch (_) {}
    this._writer = port.writable.getWriter();
    this._lineBuffer = '';
    this._running = true;
    this._ready = true;
    this._readLoop();
  }

  async disconnect() {
    this._running = false;
    this._ready = false;
    // Reject any pending resolvers
    for (const { reject } of this._lineResolvers) {
      reject(new Error('Disconnected'));
    }
    this._lineResolvers = [];
    this._lineBuffer = '';
    try { if (this._reader) this._reader.cancel(); } catch (_) {}
    try { if (this._writer) this._writer.releaseLock(); } catch (_) {}
    try { if (this._port) await this._port.close(); } catch (_) {}
    this._port = null;
    this._reader = null;
    this._writer = null;
  }

  // Tear down state after an unexpected drop without touching the vanished
  // port (closing it may hang), then notify the UI so it can auto-reconnect.
  _handleLost() {
    this._running = false;
    this._ready = false;
    for (const { reject } of this._lineResolvers) {
      try { reject(new Error('Connection lost')); } catch (_) {}
    }
    this._lineResolvers = [];
    this._lineBuffer = '';
    try { if (this._writer) this._writer.releaseLock(); } catch (_) {}
    this._writer = null;
    this._reader = null;
    this._port = null;
    if (this.onLost) { try { this.onLost(); } catch (_) {} }
  }

  async _readLoop() {
    const decoder = new TextDecoder();
    let reader;
    try {
      reader = this._port.readable.getReader();
      this._reader = reader;
      while (this._running) {
        const { value, done } = await reader.read();
        if (done) break;
        this._lineBuffer += decoder.decode(value, { stream: true });
        let idx;
        while ((idx = this._lineBuffer.indexOf('\r\n')) !== -1) {
          const line = this._lineBuffer.substring(0, idx);
          this._lineBuffer = this._lineBuffer.substring(idx + 2);
          if (this._lineResolvers.length > 0) {
            const { resolve } = this._lineResolvers.shift();
            resolve(line);
          }
        }
      }
    } catch (e) {
      if (this._running) console.error('Read error:', e);
    } finally {
      try { reader.releaseLock(); } catch (_) {}
    }
    // Loop exited while still expected to run: the device dropped.
    if (this._running) this._handleLost();
  }

  async sendCommand(cmdObj, timeoutMs = 8000) {
    if (!this._ready || !this._writer) throw new Error('Not connected');
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const idx = this._lineResolvers.findIndex(r => r.resolve === resolve || r.reject === reject);
        if (idx !== -1) this._lineResolvers.splice(idx, 1);
        reject(new Error('Timeout'));
      }, timeoutMs);

      this._lineResolvers.push({
        resolve: (line) => { clearTimeout(timer); resolve(line); },
        reject:  (err)  => { clearTimeout(timer); reject(err); },
      });

      const encoder = new TextEncoder();
      const text = JSON.stringify(cmdObj) + '\r\n';
      this._writer.write(encoder.encode(text)).catch(reject);
    });
  }
}
