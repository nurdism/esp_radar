// ESP Radar web flasher.
//
// Builds a small "config" partition image from the form, then flashes the
// release firmware plus that config image to the board over Web Serial using
// esptool-js. No app install required (Chrome / Edge on desktop).

import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.5.4/bundle.js";

// --- Flash layout -----------------------------------------------------------
// Offsets must match partitions.csv / flasher_args.json in the firmware build.
const FIRMWARE = [
  { addr: 0x0,     url: "./firmware/bootloader.bin" },
  { addr: 0x8000,  url: "./firmware/partition-table.bin" },
  { addr: 0x10000, url: "./firmware/esp_radar.bin" },
];
const CONFIG_ADDR = 0x410000;

// --- Config blob layout -----------------------------------------------------
// Byte-for-byte identical to cfg_blob_t in components/app_config/app_config.c.
// Every field is a fixed-width, NUL-terminated ASCII string.
const MAGIC = "ESPRDR1"; // stored in an 8-byte field (with trailing NUL)
const FIELDS = [
  ["magic", 8],
  ["wifi_ssid", 33],
  ["wifi_pass", 64],
  ["zip", 16],
  ["lat", 16],
  ["lon", 16],
  ["tz", 48],
  ["ntp", 48],
  ["range_nm", 8],
  ["refresh_sec", 8],
  ["beep", 4],
  ["ui_clock", 4],
  ["ui_rings", 4],
  ["ui_battery", 4],
  ["ui_header", 4],
  ["ui_status", 4],
  ["ui_labels", 4],
  ["ui_leaders", 4],
];
const BLOB_SIZE = FIELDS.reduce((n, [, sz]) => n + sz, 0);

// --- DOM helpers ------------------------------------------------------------
const $ = (id) => document.getElementById(id);
const logEl = $("log");
const progEl = $("prog");

function log(msg) {
  logEl.textContent += "\n" + msg;
  logEl.scrollTop = logEl.scrollHeight;
}

const terminal = {
  clean() { logEl.textContent = ""; },
  writeLine(data) { log(data); },
  write(data) { logEl.textContent += data; logEl.scrollTop = logEl.scrollHeight; },
};

// --- Form defaults & persistence --------------------------------------------
// Keyed by element id. Mirrors the firmware's menuconfig defaults; checkbox
// fields use booleans, everything else strings. `addr`/`raw-toggle` are UX-only
// (not part of the config blob).
const DEFAULTS = {
  ssid: "myssid",
  pass: "mypassword",
  addr: "",
  zip: "10001",
  lat: "40.7128",
  lon: "-74.0060",
  range: "50",
  refresh: "15",
  tz: "EST5EDT,M3.2.0,M11.1.0",
  ntp: "pool.ntp.org",
  beep: true,
  ui_clock: true,
  ui_rings: true,
  ui_battery: true,
  ui_header: true,
  ui_status: true,
  ui_labels: true,
  ui_leaders: true,
  "raw-toggle": false,
};

const STORE_KEY = "esp_radar_flasher";

function loadSaved() {
  try { return JSON.parse(localStorage.getItem(STORE_KEY)) || {}; }
  catch { return {}; }
}

function saveForm() {
  const data = {};
  for (const id of Object.keys(DEFAULTS)) {
    const el = $(id);
    if (!el) continue;
    data[id] = el.type === "checkbox" ? el.checked : el.value;
  }
  try { localStorage.setItem(STORE_KEY, JSON.stringify(data)); } catch {}
}

// Populate the form from saved values, falling back to DEFAULTS per field.
function applyForm(values) {
  for (const id of Object.keys(DEFAULTS)) {
    const el = $(id);
    if (!el) continue;
    const val = id in values ? values[id] : DEFAULTS[id];
    if (el.type === "checkbox") el.checked = !!val;
    else el.value = val;
  }
  // Reflect the raw-lat/lon toggle and coordinate readout.
  const raw = $("raw-toggle").checked;
  $("raw-mode").hidden = !raw;
  $("addr-mode").hidden = raw;
  syncCoordsFromInputs();
}

// --- Build the config image -------------------------------------------------
function buildConfigBlob() {
  const buf = new Uint8Array(BLOB_SIZE);
  const enc = new TextEncoder();
  let off = 0;

  const values = {
    magic: MAGIC,
    wifi_ssid: $("ssid").value,
    wifi_pass: $("pass").value,
    zip: $("zip").value,
    lat: $("lat").value.trim(),
    lon: $("lon").value.trim(),
    tz: $("tz").value,
    ntp: $("ntp").value,
    range_nm: $("range").value.trim(),
    refresh_sec: $("refresh").value.trim(),
    beep: $("beep").checked ? "1" : "0",
    ui_clock: $("ui_clock").checked ? "1" : "0",
    ui_rings: $("ui_rings").checked ? "1" : "0",
    ui_battery: $("ui_battery").checked ? "1" : "0",
    ui_header: $("ui_header").checked ? "1" : "0",
    ui_status: $("ui_status").checked ? "1" : "0",
    ui_labels: $("ui_labels").checked ? "1" : "0",
    ui_leaders: $("ui_leaders").checked ? "1" : "0",
  };

  for (const [name, size] of FIELDS) {
    const bytes = enc.encode(values[name] ?? "");
    // Leave at least one byte for the NUL terminator; longer input is truncated.
    const n = Math.min(bytes.length, size - 1);
    buf.set(bytes.subarray(0, n), off);
    off += size; // remaining bytes stay 0 -> NUL padding
  }
  return buf;
}

// esptool-js takes file data as a binary ("latin1") string.
function toBinaryString(u8) {
  let s = "";
  const chunk = 0x8000;
  for (let i = 0; i < u8.length; i += chunk) {
    s += String.fromCharCode.apply(null, u8.subarray(i, i + chunk));
  }
  return s;
}

// --- Location resolution ----------------------------------------------------
// The lat/lon inputs are always the source of truth for the config blob;
// address search and geolocation just populate them.
function setCoordsText(t) { $("coords").textContent = t; }

function applyCoords(lat, lon) {
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
    setCoordsText("Could not determine coordinates.");
    return;
  }
  $("lat").value = lat.toFixed(5);
  $("lon").value = lon.toFixed(5);
  setCoordsText(`Coordinates: ${lat.toFixed(5)}, ${lon.toFixed(5)}`);
  saveForm();
}

function syncCoordsFromInputs() {
  const lat = $("lat").value.trim();
  const lon = $("lon").value.trim();
  setCoordsText(lat && lon ? `Coordinates: ${lat}, ${lon}` : "No coordinates set yet.");
}

async function geocode() {
  const q = $("addr").value.trim();
  if (!q) { setCoordsText("Enter an address first."); return; }
  setCoordsText("Looking up address...");
  try {
    // OpenStreetMap Nominatim: free, no API key, CORS-enabled.
    const url = "https://nominatim.openstreetmap.org/search?format=jsonv2"
      + "&addressdetails=1&limit=1&q=" + encodeURIComponent(q);
    const res = await fetch(url, { headers: { Accept: "application/json" } });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    if (!data.length) { setCoordsText(`No match for “${q}”.`); return; }
    const r = data[0];
    applyCoords(parseFloat(r.lat), parseFloat(r.lon));
    // Auto-fill the header label from the result if the user left it blank.
    if (!$("zip").value.trim() && r.address) {
      const a = r.address;
      $("zip").value = a.postcode || a.city || a.town || a.village || a.county || "";
    }
  } catch (e) {
    setCoordsText("Address lookup failed: " + (e.message || e));
  }
}

function useLocation() {
  if (!navigator.geolocation) { setCoordsText("Geolocation not supported here."); return; }
  setCoordsText("Requesting your location...");
  navigator.geolocation.getCurrentPosition(
    (pos) => applyCoords(pos.coords.latitude, pos.coords.longitude),
    (err) => setCoordsText("Location error: " + err.message),
    { enableHighAccuracy: true, timeout: 10000 },
  );
}

async function fetchBin(url) {
  const res = await fetch(url, { cache: "no-store" });
  if (!res.ok) throw new Error(`failed to fetch ${url} (${res.status})`);
  return new Uint8Array(await res.arrayBuffer());
}

// --- Flash flow -------------------------------------------------------------
async function flash() {
  const btn = $("flash");
  btn.disabled = true;
  progEl.style.width = "0";

  let transport;
  try {
    log("Loading firmware images...");
    const parts = [];
    for (const f of FIRMWARE) {
      const u8 = await fetchBin(f.url);
      parts.push({ address: f.addr, data: toBinaryString(u8) });
      log(`  ${f.url} (${u8.length} bytes) -> 0x${f.addr.toString(16)}`);
    }
    const blob = buildConfigBlob();
    parts.push({ address: CONFIG_ADDR, data: toBinaryString(blob) });
    log(`  config (${blob.length} bytes) -> 0x${CONFIG_ADDR.toString(16)}`);

    // Use the port chosen in the dropdown; if none, prompt for one now.
    let port = selectedPort();
    if (!port) {
      log("Select the board's serial port...");
      port = await navigator.serial.requestPort();
      await refreshPorts(port);
    }
    transport = new Transport(port, true);

    const esploader = new ESPLoader({ transport, baudrate: 921600, terminal });
    const chip = await esploader.main();
    log(`Connected: ${chip}`);

    log("Writing flash (do not unplug)...");
    await esploader.writeFlash({
      fileArray: parts,
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (idx, written, total) => {
        const pct = Math.round((written / total) * 100);
        progEl.style.width = `${pct}%`;
      },
    });

    log("Done. Resetting board...");
    await esploader.after();
    log("\n✓ Flashed successfully. The radar should boot now.");
  } catch (err) {
    log(`\n✗ Error: ${err.message || err}`);
  } finally {
    try { await transport?.disconnect(); } catch {}
    btn.disabled = false;
  }
}

// --- Serial port selection --------------------------------------------------
// Web Serial can only enumerate ports the user has already granted, and exposes
// just USB VID/PID (no friendly name), so labels are best-effort.
let knownPorts = [];

function portLabel(p, i) {
  const info = p.getInfo ? p.getInfo() : {};
  if (info.usbVendorId != null) {
    const vid = info.usbVendorId.toString(16).padStart(4, "0");
    const pid = (info.usbProductId ?? 0).toString(16).padStart(4, "0");
    return `Port ${i + 1} — USB ${vid}:${pid}`;
  }
  return `Port ${i + 1}`;
}

function selectedPort() {
  const sel = $("port");
  const i = sel ? parseInt(sel.value, 10) : NaN;
  return Number.isInteger(i) ? knownPorts[i] : null;
}

async function refreshPorts(preferred) {
  if (!("serial" in navigator)) return;
  knownPorts = await navigator.serial.getPorts();
  const sel = $("port");
  sel.innerHTML = "";
  if (!knownPorts.length) {
    const o = document.createElement("option");
    o.value = ""; o.textContent = "No ports authorized — click “Choose port…”";
    sel.appendChild(o);
    sel.disabled = true;
    return;
  }
  sel.disabled = false;
  knownPorts.forEach((p, i) => {
    const o = document.createElement("option");
    o.value = String(i);
    o.textContent = portLabel(p, i);
    sel.appendChild(o);
  });
  const idx = preferred ? knownPorts.indexOf(preferred) : -1;
  sel.value = String(idx >= 0 ? idx : knownPorts.length - 1);
}

async function choosePort() {
  try {
    const p = await navigator.serial.requestPort();
    await refreshPorts(p);
  } catch {
    /* user dismissed the picker */
  }
}

// --- Init -------------------------------------------------------------------
// Location controls work regardless of Web Serial support.
$("geocode").addEventListener("click", geocode);
$("addr").addEventListener("keydown", (e) => {
  if (e.key === "Enter") { e.preventDefault(); geocode(); }
});
$("geoloc").addEventListener("click", useLocation);
$("lat").addEventListener("input", syncCoordsFromInputs);
$("lon").addEventListener("input", syncCoordsFromInputs);
$("raw-toggle").addEventListener("change", (e) => {
  const raw = e.target.checked;
  $("raw-mode").hidden = !raw;
  $("addr-mode").hidden = raw;
});

// Persist every form field; restore saved values (or defaults) on load.
applyForm(loadSaved());
for (const id of Object.keys(DEFAULTS)) {
  const el = $(id);
  if (!el) continue;
  el.addEventListener(el.type === "checkbox" ? "change" : "input", saveForm);
}
$("reset").addEventListener("click", () => {
  try { localStorage.removeItem(STORE_KEY); } catch {}
  applyForm({});
});

if (!("serial" in navigator)) {
  $("nosupport").hidden = false;
  $("flash").disabled = true;
  $("addport").disabled = true;
  $("port").disabled = true;
} else {
  $("flash").addEventListener("click", flash);
  $("addport").addEventListener("click", choosePort);
  refreshPorts();
  navigator.serial.addEventListener("connect", () => refreshPorts(selectedPort()));
  navigator.serial.addEventListener("disconnect", () => refreshPorts(selectedPort()));
}
