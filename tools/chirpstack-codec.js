// ChirpStack v4 payload codec for the Xiao dual-SX1262 bridge — v8.4 ABP encoder.
// ---------------------------------------------------------------------------
// Paste into a ChirpStack v4 Device Profile -> "Codec" -> JavaScript functions.
//
// The bridge emits the mesh/Custom payload as the LoRaWAN FRMPayload. Two layouts,
// matching the bridge's per-device "Prepend source tag" portal setting:
//
//   - UNTAGGED (default, M1 per-source): FRMPayload = [payload...]
//       The source identity IS the ABP device (its DevAddr), so no in-payload tag.
//   - TAGGED   (a multiplexed device):   FRMPayload = [proto:1][srcId:4 LE][payload...]
//       Lets one ChirpStack device carry many bridge sources; the codec splits them.
//
//   proto byte: 1 = Meshtastic, 2 = MeshCore, 3 = Reticulum, 4/other = Custom.
//   payload:    MT/MC bridged traffic is UTF-8 text (e.g. "Alice@MT: hi",
//               "pos 40.7,-74.0", "env 22.5C RH 45% 1013hPa"); a raw-LoRa Custom
//               weather station sends its proprietary bytes — decode in decodeStation().
//
// Set the two constants below to match how YOU configured the bridge + your station.
// ---------------------------------------------------------------------------

var HAS_SOURCE_TAG = false;   // true if this device's "Prepend source tag" is on
var STATION_FPORT  = 13;      // the FPort your raw-LoRa weather station's device uses

function decodeUplink(input) {
  var b = input.bytes || [];
  var fPort = input.fPort;
  var off = 0;
  var data = {};

  if (HAS_SOURCE_TAG && b.length >= 5) {
    var proto = b[0];
    var srcId = ((b[1]) | (b[2] << 8) | (b[3] << 16) | (b[4] << 24)) >>> 0;
    data.source = { proto: protoName(proto), srcId: hex8(srcId) };
    off = 5;
  }

  var payload = b.slice(off);
  data.raw_hex = toHex(payload);

  if (fPort === STATION_FPORT) {
    var station = decodeStation(payload);
    if (station) data.station = station;
  } else {
    var txt = toAscii(payload);
    if (txt !== null) data.text = txt;
  }

  return { data: data };
}

// EXAMPLE weather-station decoder — REPLACE with your station's real byte layout.
// This sample assumes: [tempC*100 int16 LE][rh*10 uint16 LE][presshPa*10 uint16 LE].
function decodeStation(p) {
  if (p.length < 6) return null;
  var t = (p[0] | (p[1] << 8)); if (t & 0x8000) t -= 0x10000;
  var rh = (p[2] | (p[3] << 8));
  var pr = (p[4] | (p[5] << 8));
  return { tempC: t / 100, humidityPct: rh / 10, pressureHpa: pr / 10 };
}

// The bridge is uplink-only (Class A, ADR off) and consumes no downlinks.
function encodeDownlink(input) { return { bytes: [] }; }

// --- helpers ---------------------------------------------------------------
function protoName(p) { return ({ 1: "meshtastic", 2: "meshcore", 3: "reticulum" })[p] || "custom"; }
function hex8(v) { return ("00000000" + (v >>> 0).toString(16)).slice(-8); }
function toHex(a) { var s = ""; for (var i = 0; i < a.length; i++) s += ("0" + a[i].toString(16)).slice(-2); return s; }
function toAscii(a) {
  if (!a.length) return null;
  var s = "";
  for (var i = 0; i < a.length; i++) { var c = a[i]; if (c < 0x20 || c > 0x7e) return null; s += String.fromCharCode(c); }
  return s;
}
