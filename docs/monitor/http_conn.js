'use strict';

// HTTP transport for the LcdTap JSON protocol, used by the device-served
// page (pico2w_remote). Same surface as SerialConnection: sendCommand()
// resolves to the raw response line (a JSON string without the CRLF).
//
// The device processes one API request at a time, so requests are queued
// client-side and POSTed sequentially to /api.

export class HttpConnection {
  constructor(baseUrl = '') {
    this._base = baseUrl;
    this._ready = false;
    this._queue = Promise.resolve();
    // Fired when a request fails while we believed we were connected
    // (device rebooting, WiFi drop). The app's reconnect loop takes over.
    this.onLost = null;
  }

  get connected() { return this._ready; }

  // There is no persistent link to open: the app's connect flow verifies
  // reachability with its own hello handshake right after this.
  async connect() { this._ready = true; }

  async reopen() { this._ready = true; }

  async disconnect() { this._ready = false; }

  _handleLost() {
    if (!this._ready) return;
    this._ready = false;
    if (this.onLost) { try { this.onLost(); } catch (_) {} }
  }

  sendCommand(cmdObj, timeoutMs = 8000) {
    const run = () => this._post(cmdObj, timeoutMs);
    const p = this._queue.then(run, run);
    // Keep the queue alive regardless of this request's outcome.
    this._queue = p.catch(() => {});
    return p;
  }

  async _post(cmdObj, timeoutMs) {
    if (!this._ready) throw new Error('Not connected');
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), timeoutMs);
    try {
      const resp = await fetch(this._base + 'api', {
        method: 'POST',
        body: JSON.stringify(cmdObj) + '\r\n',
        signal: ctrl.signal,
      });
      if (resp.status === 503) throw new Error('Device busy');
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
      const text = await resp.text();
      return text.endsWith('\r\n') ? text.slice(0, -2) : text;
    } catch (e) {
      if (e.name === 'AbortError') throw new Error('Timeout');
      // A network-level failure means the device went away (reboot after
      // setparams/setnetconfig, WiFi drop). 503 does not.
      if (e.message !== 'Device busy') this._handleLost();
      throw e;
    } finally {
      clearTimeout(timer);
    }
  }
}
