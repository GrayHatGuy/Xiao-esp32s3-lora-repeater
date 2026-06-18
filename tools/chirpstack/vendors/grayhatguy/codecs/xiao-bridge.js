// ChirpStack v4 payload codec for the Xiao dual-SX1262 bridge — v8.4 ABP encoder.
// ===========================================================================
// Paste into a ChirpStack v4 Device Profile -> "Codec" tab -> "JavaScript
// functions" (it applies to every device on that profile). This same file is
// shipped inside the importable device-profile template at
// tools/chirpstack/vendors/grayhatguy/codecs/xiao-bridge.js — keep the two in sync.
//
// ChirpStack decrypts the FRMPayload with the device's AppSKey BEFORE calling
// this codec, so `input.bytes` is the bridge's cleartext payload. The bridge
// builds it (src/main.cpp enqueueAbpUplink) as one of two layouts, matching the
// per-device "Prepend source tag" portal checkbox:
//
//   - UNTAGGED (default, M1 per-source): FRMPayload = [payload...]
//       The source identity IS the ABP device (its DevAddr), so no in-payload tag.
//   - TAGGED   (multiplexed device):     FRMPayload = [proto:1][srcId:4 LE][payload...]
//       Lets ONE ChirpStack device carry many bridge sources; the codec splits them.
//
//   proto byte : 1=Meshtastic 2=MeshCore 3=Reticulum 4=Custom 5=LoRaWAN
//                (the bridge stamps the BridgeConfig::Protocol enum).
//   payload    : MT/MC bridged traffic is UTF-8 text (e.g. "Alice@MT: hi",
//                "pos 40.7,-74.0"); a raw-LoRa Custom weather station sends its
//                own proprietary bytes -> decode in decodeStation().
//
// CONFIGURE per device WITHOUT editing this file by setting ChirpStack Device
// "Variables" (Device -> Variables):
//   source_tag    = "true"   if that device's "Prepend source tag" is enabled
//   station_fport = "13"     the FPort your raw-LoRa weather-station device uses
// If a variable is absent, the DEFAULT_* constants below are used.
// ===========================================================================

var DEFAULT_SOURCE_TAG    = false;  // fallback when the "source_tag" variable is unset
var DEFAULT_STATION_FPORT = 13;     // fallback when the "station_fport" variable is unset

function decodeUplink(input) {
  var bytes = input.bytes || [];
  var fPort = input.fPort;
  var vars  = input.variables || {};
  var warnings = [];
  var errors   = [];
  var data = { fPort: fPort };

  var hasTag = (vars.source_tag !== undefined)
      ? (String(vars.source_tag).toLowerCase() === "true" || String(vars.source_tag) === "1")
      : DEFAULT_SOURCE_TAG;
  var stationFPort = (vars.station_fport !== undefined)
      ? parseInt(vars.station_fport, 10)
      : DEFAULT_STATION_FPORT;

  var off = 0;
  if (hasTag) {
    if (bytes.length < 5) {
      errors.push("source_tag is on but the payload is shorter than the 5-byte tag");
      return { data: data, errors: errors };
    }
    var proto = bytes[0];
    var srcId = ((bytes[1]) | (bytes[2] << 8) | (bytes[3] << 16) | (bytes[4] << 24)) >>> 0;
    data.source = { proto: protoName(proto), protoId: proto, srcId: hex8(srcId) };
    if (proto < 1 || proto > 5) warnings.push("unrecognised source proto byte " + proto);
    off = 5;
  }

  var payload = bytes.slice(off);
  data.raw_hex = toHex(payload);

  if (payload.length === 0) {
    warnings.push("empty payload" + (hasTag ? " after the source tag" : ""));
    return wrap(data, warnings, errors);
  }

  if (fPort === stationFPort) {
    var station = decodeStation(payload, warnings);
    if (station) data.station = station;
  } else {
    data.text = toUtf8(payload);
  }

  return wrap(data, warnings, errors);
}

// ===== Weather-station decoder — EDIT to your station's real byte layout. =====
// The bridge forwards a Custom raw-LoRa station's bytes VERBATIM (it is a
// transparent byte pipe and knows nothing of the format), so only the station's
// own datasheet defines this. The example below is a PLACEHOLDER assuming
// [tempC*100 int16 LE][rh*10 uint16 LE][pressHpa*10 uint16 LE]. Replace it with
// your station's field order / widths / endianness / scaling, then update
// test_decode_xiao-bridge.json to match.
function decodeStation(p, warnings) {
  if (p.length < 6) { warnings.push("station payload < 6 bytes (placeholder layout)"); return null; }
  var t = (p[0] | (p[1] << 8)); if (t & 0x8000) t -= 0x10000;
  var rh = (p[2] | (p[3] << 8));
  var pr = (p[4] | (p[5] << 8));
  return { tempC: t / 100, humidityPct: rh / 10, pressureHpa: pr / 10 };
}

// The bridge is uplink-only (Class A, ADR off) and consumes no downlinks.
function encodeDownlink(input) { return { bytes: [] }; }

// --- helpers ---------------------------------------------------------------
function wrap(data, warnings, errors) {
  var out = { data: data };
  if (warnings.length) out.warnings = warnings;
  if (errors.length)   out.errors   = errors;
  return out;
}
function protoName(p) { return ({ 1: "meshtastic", 2: "meshcore", 3: "reticulum", 4: "custom", 5: "lorawan" })[p] || ("proto" + p); }
function hex8(v) { return ("00000000" + (v >>> 0).toString(16)).slice(-8); }
function toHex(a) { var s = ""; for (var i = 0; i < a.length; i++) s += ("0" + a[i].toString(16)).slice(-2); return s; }
function toUtf8(a) {
  try { return new TextDecoder("utf-8", { fatal: false }).decode(new Uint8Array(a)); }
  catch (e) {  // fallback if TextDecoder is unavailable in the runtime
    var s = ""; for (var i = 0; i < a.length; i++) s += String.fromCharCode(a[i]); return s;
  }
}
