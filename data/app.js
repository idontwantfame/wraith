"use strict";

// --- small helpers ----------------------------------------------------------
const $ = (id) => document.getElementById(id);

async function api(path, method = "GET", body) {
  // X-Requested-With is required by the server on every state-changing route; a
  // cross-site page cannot set it, so it (with the Host/Origin check) blocks CSRF
  // and DNS-rebinding against this unauthenticated LAN surface.
  const opt = { method, headers: { "X-Requested-With": "fogger" } };
  if (body !== undefined) {
    opt.headers["Content-Type"] = "application/json";
    opt.body = JSON.stringify(body);
  }
  try {
    const res = await fetch(path, opt);
    try {
      return await res.json();
    } catch (_) {
      return { ok: res.ok };
    }
  } catch (_) {
    // Device rebooting / dropped off Wi-Fi: surface as a normal rejection so
    // every caller can toast it, and no fetch leaks an unhandled rejection.
    return { ok: false, error: "network" };
  }
}

let toastTimer;
function toast(msg, isErr = false) {
  const t = $("toast");
  t.textContent = msg;
  t.classList.toggle("err", isErr);
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), isErr ? 6000 : 2600); // errors linger
}

// --- tabs --------------------------------------------------------------------
function activateTab(name) {
  document.querySelectorAll(".tab").forEach((b) =>
    b.classList.toggle("active", b.dataset.tab === name)
  );
  document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
  const panel = $("tab-" + name);
  if (panel) panel.classList.add("active");
  if (name === "settings") {
    // Settings first, then the lists: a Wi-Fi scan can stall the device's HTTP
    // replies for seconds, and the form must be filled before anything can be
    // saved from it. The machine list is a cheap read of the BLE scan cache.
    loadConfig().then(loadMachines).then(() => scanWifi());
  }
}
document.querySelectorAll(".tab").forEach((btn) =>
  btn.addEventListener("click", () => activateTab(btn.dataset.tab))
);

// --- live status (WebSocket + polling fallback) ------------------------------
let ws = null;
let wsOk = false;
let dragging = false;
// After a slider is released we POST the new value, but the device keeps
// reporting the OLD value for a frame or two until it applies. Hold the shown
// value until the device confirms it (or a grace timeout) so an in-flight status
// frame can't briefly snap the slider back to the previous value.
const sliderPending = {};

function connectWs() {
  try {
    ws = new WebSocket(`ws://${location.host}/ws`);
  } catch (_) {
    return;
  }
  ws.onopen = () => (wsOk = true);
  ws.onclose = () => {
    wsOk = false;
    setTimeout(connectWs, 2000);
  };
  ws.onmessage = (e) => {
    try {
      renderStatus(JSON.parse(e.data));
    } catch (_) {}
  };
}

setInterval(() => {
  if (!wsOk) api("/api/status").then(renderStatus).catch(() => {});
}, 2000);

function setStat(id, text, cls) {
  const el = $(id);
  el.textContent = text;
  el.className = "stat-val" + (cls ? " " + cls : "");
}

// Machine report bits (status bytes 10..11), decoded from the machine's firmware
// (docs/PROTOCOL.md). `fault`: the machine refuses or stops fog for it.
const REPORT_BITS = [
  { bit: 0x0001, text: "low battery", fault: true },
  { bit: 0x0002, text: "battery reading invalid", fault: true },
  { bit: 0x0004, text: "short circuit / over-current", fault: true },
  { bit: 0x0008, text: "oil tank missing", fault: true },
  { bit: 0x0020, text: "overheating", fault: true },
  { bit: 0x0080, text: "battery imbalance", fault: true },
  { bit: 0x0100, text: "dry burn (oil empty?)", fault: true },
  { bit: 0x0200, text: "oil tank connected", fault: false },
  { bit: 0x0400, text: "oil tank not recognised", fault: true },
];

// Describe a report word, e.g. "0x0208" -> { text, fault }.
function describeReport(hex) {
  const word = parseInt(hex, 16) || 0;
  const parts = [];
  let fault = false;
  let known = 0;
  for (const r of REPORT_BITS) {
    if (word & r.bit) {
      parts.push(r.text);
      fault = fault || r.fault;
      known |= r.bit;
    }
  }
  const unknown = word & ~known;
  if (unknown) parts.push("unknown event 0x" + unknown.toString(16).padStart(4, "0"));
  return { text: parts.join("; "), fault };
}

// Why the last program ended (program.last_stop), when worth saying.
const PROGRAM_ENDED = {
  completed: "last program completed",
  "machine stopped": "last program ended: the machine stopped fog",
  "machine did not start": "last program ended: the machine did not start a pulse",
  "link lost": "last program ended: machine link lost",
  "runtime cap": "last program ended: 2-hour limit",
};

function renderStatus(d) {
  if (!d || !d.ble) return;
  const b = d.ble;

  const pill = $("linkPill");
  pill.textContent = b.connected ? "BLE live" : "BLE " + b.link;
  pill.className = "pill " + (b.connected ? "pill-ok" : "pill-warn");

  setStat("sFog", b.valid ? (b.fogging ? "On" : "Off") : "…", b.fogging ? "on" : "");
  // Reflect running state on the START button (unless the user is mid-arm).
  fogRunning = b.valid && b.fogging;
  if (armed && fogRunning) {
    // Fog started elsewhere (a program or another client) while the user was
    // mid-arm: drop the arm UI so a stale "CONFIRM START" can't swallow the next
    // tap, and show RUNNING immediately.
    disarm();
  } else if (!armed) {
    const sb = $("btnStart");
    sb.textContent = fogRunning ? "RUNNING" : "START FOG";
    sb.classList.toggle("running", fogRunning);
  }
  // Battery: the lower cell, with a ⚡ when the charger is connected.
  const bolt = b.charger ? " ⚡" : "";
  const battCls = !b.valid ? "" : b.charger ? "on" : b.batt_min < 20 ? "bad" : "";
  // Two packs; show both (the lower one is the limiter), bolt while charging.
  setStat("sBatt", b.valid ? b.batt_a + " / " + b.batt_b + "%" + bolt : "…", battCls);
  // Machine report (status bytes 10..11): the machine latches its last events
  // there (not live state), and not every value is a fault. Amber for events
  // such as "oil tank connected", red when the machine refuses or stops fog.
  // Never hidden.
  const fb = $("faultBanner");
  if (b.valid && b.fault && b.fault !== "0x0000") {
    const r = describeReport(b.fault);
    fb.textContent = "Last machine report " + b.fault + ": " + r.text;
    fb.classList.toggle("alarm", r.fault);
    fb.hidden = false;
  } else {
    fb.hidden = true;
  }

  if (!dragging && b.valid) {
    if (b.density) applySlider("density", "densityOut", densToPct(b.density), (v) => v + "%");
    if (b.fan) applySlider("fan", "fanOut", b.fan, (v) => v + "%");
  }
  $("curTarget").textContent = b.target || "none";

  if (d.wifi) {
    const w = d.wifi;
    // With several access points on one SSID, show which one we joined.
    const ap = w.bssid ? ` · AP ${w.bssid} ch ${w.channel} · ${w.rssi} dBm` : "";
    $("wifiState").textContent = w.mode + (w.portal ? " (portal)" : "") + ap;
    $("wifiIp").textContent = d.wifi.ip || "…";
    if (d.wifi.host) $("curHost").textContent = "http://" + d.wifi.host + ".local";
  }

  if (d.program) {
    const p = d.program;
    $("progState").textContent = p.running
      ? `${p.phase} · pulse ${p.cycle}${p.repeats ? "/" + p.repeats : ""} · ${Math.ceil(
          p.phase_remaining_ms / 1000
        )}s`
      : "idle" + (PROGRAM_ENDED[p.last_stop] ? " · " + PROGRAM_ENDED[p.last_stop] : "");
  }

  if (d.mqtt) {
    const m = d.mqtt;
    $("mqttState").textContent = !m.enabled
      ? "disabled"
      : m.connected
        ? "connected"
        : "not connected" + (m.error ? " (" + m.error + ")" : "");
  }

  if (d.time) {
    $("clock").textContent = d.time.synced ? d.time.local : "no time";
    $("devTime").textContent = d.time.synced
      ? d.time.local + " (" + (d.time.tz || "") + ")"
      : "not synced yet";
  }
}

// --- fog control (two-tap arm for START) ------------------------------------
let armed = false;
let armTimer;
let fogRunning = false;
function disarm() {
  armed = false;
  clearTimeout(armTimer);
  const b = $("btnStart");
  b.classList.remove("armed");
  // Fall back to the live running/idle label rather than forcing "START FOG".
  b.textContent = fogRunning ? "RUNNING" : "START FOG";
  b.classList.toggle("running", fogRunning);
}
$("btnStart").addEventListener("click", async () => {
  if (fogRunning) return;  // already running; use STOP, don't re-arm start
  if (!armed) {
    armed = true;
    const b = $("btnStart");
    b.classList.add("armed");
    b.textContent = "CONFIRM START";
    armTimer = setTimeout(disarm, 4000);
    return;
  }
  disarm();
  // Server-enforced arm: arm, then start within the server's window.
  const a = await api("/api/fog/arm", "POST");
  if (!a.ok) {
    toast("Rejected: " + (a.error || "?"), true);
    return;
  }
  const r = await api("/api/fog/start", "POST");
  toast(r.ok ? "Fog starting" : "Rejected: " + (r.error || "?"), !r.ok);
});
$("btnStop").addEventListener("click", async () => {
  disarm();
  const r = await api("/api/fog/stop", "POST");
  toast(r.ok ? "Stop sent" : "Rejected: " + (r.error || "?"), !r.ok);
});

// --- sliders (commit on release) --------------------------------------------
// Update a slider from status, unless a just-committed value is still pending:
// hold the shown value until the device reports it (within tolerance) or the
// grace window elapses, so telemetry lag can't bounce the slider back.
function applySlider(id, outId, reportedVal, fmt) {
  const p = sliderPending[id];
  if (p) {
    if (Math.abs(reportedVal - p.v) <= 1 || Date.now() > p.until) {
      delete sliderPending[id];  // confirmed, or grace expired -> resume syncing
    } else {
      return;  // keep showing the committed value
    }
  }
  $(id).value = reportedVal;
  $(outId).textContent = fmt(reportedVal);
}

function wireSlider(id, outId, path, toValue = (v) => v, fmt = (v) => String(v)) {
  const el = $(id);
  const out = $(outId);
  // Serialize sends so the device can never be left on a stale value: hold only
  // the newest pending position and keep a single request in flight at a time.
  // (The firmware likewise keeps only the latest value, so coalescing drops
  // nothing.) This ordering — not a timer — is what makes the final value land,
  // because fetch() completion order across connections isn't guaranteed.
  let queued = null;   // newest value awaiting a live send, or null
  let sending = false;

  async function send(v, silent) {
    const r = await api(path, "POST", { value: toValue(v) });
    if (!r.ok && !silent) toast("Rejected: " + (r.error || "?"), true);
    return r;
  }
  // Drain queued live values newest-first, one request in flight at a time.
  async function flush() {
    if (sending) return;
    sending = true;
    try {
      while (queued !== null) {
        const v = queued;
        queued = null;
        await send(v, true);  // live (drag) sends are silent to avoid toast spam
      }
    } finally {
      sending = false;
    }
  }

  // `dragging` freezes status-sync for the sliders; clear it whenever the gesture
  // ends, including a tap with no value change or a cancelled/blurred gesture, so
  // a stuck flag can't silently stop both sliders from syncing. The real anti-
  // snap-back guard is sliderPending (set on change), not this flag.
  const endDrag = () => (dragging = false);
  el.addEventListener("pointerdown", () => (dragging = true));
  el.addEventListener("pointerup", endDrag);
  el.addEventListener("pointercancel", endDrag);
  el.addEventListener("blur", endDrag);

  el.addEventListener("input", () => {
    const v = Number(el.value);
    out.textContent = fmt(v);
    queued = v;  // coalesce to the newest position
    flush();
  });
  el.addEventListener("change", async () => {
    dragging = false;
    const shown = Number(el.value);
    sliderPending[id] = { v: shown, until: Date.now() + 3000 };  // hold until confirmed
    queued = null;  // supersede any queued drag value; the final send wins
    // Let an in-flight live send finish so the final POST is genuinely last.
    while (sending) await new Promise((r) => setTimeout(r, 0));
    const r = await send(shown, false);  // final, authoritative, reports rejection
    if (!r.ok) delete sliderPending[id];  // rejected: let status revert
  });
}
// Density is shown as 0-100% but the wire protocol wants 8000-40000.
const densToPct = (d) => Math.max(0, Math.min(100, Math.round((d - 8000) / 320)));
const pctToDens = (p) => Math.round(8000 + p * 320);
wireSlider("density", "densityOut", "/api/density", pctToDens, (p) => p + "%");
wireSlider("fan", "fanOut", "/api/fan", (v) => v, (v) => v + "%");

// --- modes / timer program ---------------------------------------------------
document.querySelectorAll(".chip").forEach((chip) => {
  chip.addEventListener("click", () => {
    $("progOn").value = chip.dataset.on;
    $("progOff").value = chip.dataset.off;
    $("progRep").value = chip.dataset.rep;
  });
});
let progArmed = false;
let progArmTimer;
function disarmProg() {
  progArmed = false;
  clearTimeout(progArmTimer);
  const b = $("btnProgStart");
  b.classList.remove("armed");
  b.textContent = "START PROGRAM";
}
// Repeats must be explicit: a blank field must not silently mean 0 (= run until
// stopped / the 2 h cap).
function readRepeats() {
  const raw = $("progRep").value.trim();
  const n = raw === "" ? NaN : Number(raw);
  return Number.isInteger(n) && n >= 0 ? n : null;
}
$("btnProgStart").addEventListener("click", async () => {
  if (readRepeats() === null) {
    disarmProg();
    return toast("Set Repeats (0 = until stopped)", true);
  }
  if (!progArmed) {
    progArmed = true;
    const b = $("btnProgStart");
    b.classList.add("armed");
    b.textContent = "CONFIRM START";
    progArmTimer = setTimeout(disarmProg, 4000);
    return;
  }
  disarmProg();
  // A program ignites fog, so it needs the server-side arm first.
  const a = await api("/api/fog/arm", "POST");
  if (!a.ok) {
    toast("Rejected: " + (a.error || "?"), true);
    return;
  }
  const body = {
    on_sec: Number($("progOn").value) || 5,
    off_sec: Number($("progOff").value) || 5,
    repeats: readRepeats(),
  };
  const r = await api("/api/program/start", "POST", body);
  toast(r.ok ? "Program running" : "Rejected: " + (r.error || "?"), !r.ok);
});
$("btnProgStop").addEventListener("click", async () => {
  disarmProg();
  const r = await api("/api/program/stop", "POST");
  toast(r.ok ? "Program stopped" : "Rejected: " + (r.error || "?"), !r.ok);
});

// --- machines ----------------------------------------------------------------
// Build a two-column <li> from untrusted text WITHOUT innerHTML (the machine
// name and SSID come from the radio and must never be parsed as HTML).
function listRow(primaryText, metaText, rightText) {
  const li = document.createElement("li");
  const left = document.createElement("span");
  left.textContent = primaryText;
  if (metaText) {
    const meta = document.createElement("div");
    meta.className = "meta";
    meta.textContent = metaText;
    left.appendChild(meta);
  }
  const right = document.createElement("span");
  right.className = "meta";
  right.textContent = rightText;
  li.appendChild(left);
  li.appendChild(right);
  return li;
}

function emptyRow(text) {
  const li = document.createElement("li");
  li.className = "meta";
  li.textContent = text;
  return li;
}

async function loadMachines() {
  const d = await api("/api/machines");
  const ul = $("machineList");
  ul.replaceChildren();
  (d.machines || []).forEach((m) => {
    const li = listRow(m.name || "(unnamed)", m.mac, (m.rssi ?? "") + " dBm");
    if (m.mac === d.target) li.classList.add("selected");
    li.addEventListener("click", async () => {
      const r = await api("/api/machine/select", "POST", { mac: m.mac });
      toast(r.ok ? "Machine selected" : "Rejected: " + (r.error || "?"), !r.ok);
      loadMachines();
    });
    ul.appendChild(li);
  });
  if (!ul.children.length) ul.appendChild(emptyRow("No machines found yet…"));
}
$("btnMachineScan").addEventListener("click", loadMachines);
$("btnMachineDisconnect").addEventListener("click", async () => {
  if (!confirm("Disconnect and clear the bound machine?")) return;
  const r = await api("/api/machine/disconnect", "POST");
  toast(r.ok ? "Disconnected" : "Failed: " + (r.error || "?"), !r.ok);
  loadMachines();
});

// --- wi-fi -------------------------------------------------------------------
// One bounded scan-poll chain at a time, and only while Settings is open: the
// device can report "scanning" for a while (e.g. mid-connect), and an unbounded
// poll per Settings visit would pile requests onto the device.
const kScanPollMs = 1500;
const kScanPollMax = 20; // ~30 s
let scanGen = 0;
async function scanWifi(force = false, gen = ++scanGen, polls = 0) {
  const d = await api("/api/wifi/scan" + (force ? "?force=1" : ""));
  if (gen !== scanGen) return; // superseded by a newer scan request
  const ul = $("wifiList");
  if (d.busy && !(d.networks || []).length) {
    // The device won't scan while fog runs (shared radio with the BLE link).
    ul.replaceChildren(emptyRow("Scanning paused while fog is running"));
    return;
  }
  if (d.scanning) {
    if (polls >= kScanPollMax || !$("tab-settings").classList.contains("active")) {
      ul.replaceChildren(emptyRow("Still scanning. Tap Scan to check again"));
      return;
    }
    ul.replaceChildren(emptyRow("Scanning… (may briefly drop the connection)"));
    setTimeout(() => scanWifi(false, gen, polls + 1), kScanPollMs);
    return;
  }
  ul.replaceChildren();
  (d.networks || []).forEach((n) => {
    const li = listRow((n.ssid || "") + (n.secure ? " 🔒" : ""), "",
                       (n.rssi ?? "") + " dBm");
    li.addEventListener("click", () => ($("wifiSsid").value = n.ssid));
    ul.appendChild(li);
  });
  if (!ul.children.length) {
    ul.appendChild(emptyRow("No networks. Tap Scan, or type your SSID below"));
  }
}
$("btnWifiScan").addEventListener("click", () => scanWifi(true));
$("btnWifiConnect").addEventListener("click", async () => {
  const ssid = $("wifiSsid").value.trim();
  if (!ssid) return toast("Enter an SSID", true);
  const r = await api("/api/wifi/connect", "POST", { ssid, pass: $("wifiPass").value });
  toast(r.ok ? "Connecting; the device may change IP" : "Rejected: " + (r.error || "?"), !r.ok);
});

// --- mqtt / home assistant ---------------------------------------------------
// Saves that send a whole form (MQTT, hostname, time) stay disabled until the
// stored settings have loaded into it: saving a half-filled form would wipe
// stored values (an empty username also drops the saved MQTT password).
const kConfigSaveButtons = ["btnMqttSave", "btnHostSave", "btnTimeSave"];
function setConfigSavesEnabled(on) {
  kConfigSaveButtons.forEach((id) => ($(id).disabled = !on));
}

// Show only the fields the chosen transport uses: the WebSocket path for ws /
// wss, the CA certificate + host-name option for the TLS ones.
const kTlsTransports = ["mqtts", "wss"];
const kWebSocketTransports = ["ws", "wss"];
function updateMqttTransportFields() {
  const t = $("mqttTransport").value;
  $("mqttPath").hidden = !kWebSocketTransports.includes(t);
  const tls = kTlsTransports.includes(t);
  ["mqttCa", "mqttClearCaRow", "mqttSkipCnRow"].forEach((id) => ($(id).hidden = !tls));
}
$("mqttTransport").addEventListener("change", () => {
  // Swap the well-known default port when moving between plain and TLS MQTT.
  const t = $("mqttTransport").value;
  const port = $("mqttPort");
  if (t === "mqtts" && port.value === "1883") port.value = "8883";
  if (t === "mqtt" && port.value === "8883") port.value = "1883";
  updateMqttTransportFields();
});

async function loadConfig() {
  setConfigSavesEnabled(false);
  const c = await api("/api/config");
  if (!c.mqtt) {
    toast("Couldn't load settings" + (c.error ? " (" + c.error + ")" : "") +
          "; reopen Settings to retry", true);
    return;
  }
  $("mqttEnabled").checked = c.mqtt.enabled;
  $("mqttHost").value = c.mqtt.host || "";
  $("mqttPort").value = c.mqtt.port || 1883;
  $("mqttUser").value = c.mqtt.user || "";
  $("mqttTopic").value = c.mqtt.base_topic || "wraith";
  $("mqttIgnite").checked = c.mqtt.allow_remote_ignition;
  if (!c.mqtt.tls_supported) {
    // This firmware build has no TLS (not enough RAM on this board): don't offer it.
    [...$("mqttTransport").options]
      .filter((o) => kTlsTransports.includes(o.value))
      .forEach((o) => o.remove());
  }
  $("mqttTransport").value = c.mqtt.transport || "mqtt";
  $("mqttPath").value = c.mqtt.path || "/mqtt";
  // The CA is never sent back either; say whether one is stored.
  $("mqttCa").value = "";
  $("mqttCa").placeholder = c.mqtt.has_ca
    ? "CA certificate saved (paste a new one to replace)"
    : "Broker CA certificate (PEM), required for TLS";
  $("mqttClearCa").checked = false;
  $("mqttSkipCn").checked = !!c.mqtt.skip_hostname_check;
  updateMqttTransportFields();
  // The password is never sent back; say plainly whether one is stored.
  $("mqttPass").value = "";
  $("mqttPass").placeholder = c.mqtt.has_password
    ? "Password saved (leave blank to keep)"
    : "Password (none saved)";
  if (c.timezone !== undefined) $("tz").value = c.timezone;
  if (c.ntp_server !== undefined) $("ntp").value = c.ntp_server;
  if (c.hostname !== undefined) $("hostname").value = c.hostname;
  setConfigSavesEnabled(true);
}
$("btnHostSave").addEventListener("click", async () => {
  const r = await api("/api/hostname", "POST", { hostname: $("hostname").value.trim() });
  if (!r.ok) return toast("Rejected: " + (r.error || "?"), true);
  // The device only accepts its CURRENT name as a Host, so a page opened via the
  // old <name>.local must move to the new one (after mDNS re-registers).
  const here = location.hostname.toLowerCase();
  const next = (r.hostname || "") + ".local";
  // Only follow a well-formed label (the server sanitises to this too), so a
  // tampered response can't navigate the page off-device.
  const validLabel = /^[a-z0-9-]{1,32}$/.test(r.hostname || "");
  if (validLabel && here.endsWith(".local") && here !== next) {
    toast("Hostname saved; moving to " + next);
    setTimeout(() => (location.href = "http://" + next + "/"), 1500);
    return;
  }
  toast("Hostname saved");
  setTimeout(() => api("/api/status").then(renderStatus), 500);
});
$("btnTimeSave").addEventListener("click", async () => {
  const body = {
    timezone: $("tz").value.trim() || "GMT0",
    ntp_server: $("ntp").value.trim() || "pool.ntp.org",
  };
  const r = await api("/api/time", "POST", body);
  toast(r.ok ? "Time settings saved" : "Rejected: " + (r.error || "?"), !r.ok);
});
$("btnTzGuess").addEventListener("click", async () => {
  const r = await api("/api/time/guess", "POST");
  toast(r.ok ? "Guessing timezone from IP…" : "Rejected: " + (r.error || "?"), !r.ok);
  if (r.ok) setTimeout(loadConfig, 2500);  // refresh the field once guessed
});
$("btnMqttSave").addEventListener("click", async () => {
  const body = {
    enabled: $("mqttEnabled").checked,
    host: $("mqttHost").value.trim(),
    port: Number($("mqttPort").value) || 1883,
    user: $("mqttUser").value,
    pass: $("mqttPass").value,
    base_topic: $("mqttTopic").value.trim() || "wraith",
    allow_remote_ignition: $("mqttIgnite").checked,
    transport: $("mqttTransport").value,
    path: $("mqttPath").value.trim() || "/mqtt",
    ca_pem: $("mqttCa").value.trim(), // blank = keep the saved CA
    clear_ca: $("mqttClearCa").checked,
    skip_hostname_check: $("mqttSkipCn").checked,
    // Only checked when allowing remote START (or changing broker while it is
    // allowed): the per-device setup password from the serial console.
    device_password: $("devicePass").value,
  };
  const r = await api("/api/mqtt", "POST", body);
  $("devicePass").value = "";
  if (r.ok && r.password_cleared) {
    // Broker identity (host/port/user) changed with the password left blank:
    // the old password was deliberately not kept for the new identity.
    toast("Saved, rebooting. The stored password was cleared (broker/user changed): re-enter it", true);
  } else if (r.ok) {
    toast("Saved, rebooting…"); // the device reboots itself to apply
  } else {
    toast("Rejected: " + (r.error || "?"), true);
  }
});

// --- OTA (with progress) -----------------------------------------------------
async function readText(file) {
  return file ? (await file.text()).replace(/\s+/g, "") : "";
}
$("otaForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const file = $("otaFile").files[0];
  if (!file) return toast("Choose a .bin file", true);
  const sig = await readText($("otaSig").files[0]);
  const url = "/api/ota" + (sig ? "?sig=" + encodeURIComponent(sig) : "");
  const fd = new FormData();
  fd.append("firmware", file);
  const prog = $("otaProgress");
  prog.hidden = false;
  const xhr = new XMLHttpRequest();
  xhr.open("POST", url);
  // Required by the server on every mutation (CSRF guard), uploads included.
  xhr.setRequestHeader("X-Requested-With", "fogger");
  xhr.upload.onprogress = (ev) => {
    if (ev.lengthComputable) prog.value = (ev.loaded / ev.total) * 100;
  };
  xhr.onload = () => {
    const ok = xhr.status === 200;
    let msg = ok ? "Flashed, rebooting…" : "Update failed";
    try {
      const r = JSON.parse(xhr.responseText);
      if (!r.ok && r.error) msg = "Update failed: " + r.error;
    } catch (_) {}
    toast(msg, !ok);
  };
  xhr.onerror = () => toast("Upload error", true);
  xhr.send(fd);
});

// --- maintenance -------------------------------------------------------------
$("btnReboot").addEventListener("click", async () => {
  if (!confirm("Reboot the controller?")) return;
  await api("/api/reboot", "POST");
  toast("Rebooting…");
});
$("btnFactory").addEventListener("click", async () => {
  if (!confirm("Erase all settings and reboot?")) return;
  await api("/api/factory_reset", "POST");
  toast("Factory reset, rebooting…");
});

// --- boot --------------------------------------------------------------------
connectWs();
api("/api/status")
  .then((d) => {
    renderStatus(d);
    // If Wi-Fi isn't set up yet (portal mode), land on Settings for onboarding.
    if (d && d.wifi && d.wifi.portal) activateTab("settings");
  })
  .catch(() => {});
