#ifdef MESHADDON
/*
 * MeshAddon implementation. See MeshAddon.h and ~/meshaddon/docs/PROTOCOL.md.
 */
#include "MeshAddon.h"
#include "MyMesh.h"
#include <target.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHTesp.h>
#include <ArduinoJson.h>

extern MyMesh the_mesh;
MeshAddon addon;

#define ADDON_CFG_MAGIC   0x4D41444Eu   // "MADN"
#define ADDON_CFG_VERSION 1
#define ADDON_CFG_FILE    "/addon.cfg"
#define POLL_MS           5000UL
#define PI_PING_MS        10000UL
#define PI_TIMEOUT_MS     30000UL
#define PI_SENSORS_MS     60000UL
#define PI_RUN_TIMEOUT_MS 40000UL

// Free header pins on the Heltec V4 (everything else is LoRa/PA/OLED/GPS/USB/PSRAM)
static const uint8_t ALLOWED_PINS[] = {3, 4, 6, 15, 16, 33, 40, 41, 45, 47, 48};
static bool isAdcPin(int p) { return p == 3 || p == 4 || p == 6 || p == 15 || p == 16; }

static const char* TYPE_NAMES[] = {"none", "ds18b20", "dht22", "analog", "digin", "counter", "relay", "pir", "i2c"};

// ---------- small helpers ----------
static void jesc(const char* in, char* out, size_t n) {       // JSON string escape
  size_t o = 0;
  for (; *in && o + 7 < n; in++) {
    unsigned char c = *in;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
    else if (c < 0x20) { o += snprintf(out + o, n - o, "\\u%04x", c); }
    else out[o++] = c;
  }
  out[o] = 0;
}
static void trim(char* s) {
  char* e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  char* b = s; while (*b && isspace((unsigned char)*b)) b++;
  if (b != s) memmove(s, b, strlen(b) + 1);
}
static void lower(char* s) { for (; *s; s++) *s = tolower((unsigned char)*s); }
static int hexval(char c) { return isdigit((unsigned char)c) ? c - '0' : (isxdigit((unsigned char)c) ? (tolower(c) - 'a' + 10) : -1); }

static void IRAM_ATTR counterIsr(void* arg) {
  AddonSlotState* s = (AddonSlotState*)arg;
  uint32_t now = micros();
  if (now - s->last_us > 5000) { s->count++; s->last_us = now; }   // 5 ms debounce
}

// ---------- lifecycle ----------
void MeshAddon::defaults() {
  memset(&_cfg, 0, sizeof(_cfg));
  _cfg.magic = ADDON_CFG_MAGIC; _cfg.version = ADDON_CFG_VERSION;
  strcpy(_cfg.chan_name, "#relay");
  strcpy(_cfg.prefix, "!");
  _cfg.pi_en = 1; _cfg.pi_tx = 47; _cfg.pi_rx = 48; _cfg.pi_baud = 115200;
  strcpy(_cfg.rules[0].cmd, "status"); strcpy(_cfg.rules[0].act, "status");
}

bool MeshAddon::load() {
  if (!_fs || !_fs->exists(ADDON_CFG_FILE)) return false;
  File f = _fs->open(ADDON_CFG_FILE, "r");
  if (!f) return false;
  AddonCfg tmp;
  bool ok = f.read((uint8_t*)&tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == ADDON_CFG_MAGIC && tmp.version == ADDON_CFG_VERSION;
  f.close();
  if (ok) memcpy(&_cfg, &tmp, sizeof(tmp));
  return ok;
}

bool MeshAddon::save() {
  if (!_fs) return false;
  File f = _fs->open(ADDON_CFG_FILE, "w");
  if (!f) return false;
  bool ok = f.write((const uint8_t*)&_cfg, sizeof(_cfg)) == sizeof(_cfg);
  f.close();
  return ok;
}

void MeshAddon::begin(fs::FS* fs) {
  _fs = fs;
  memset(_st, 0, sizeof(_st));
  memset(_pending, 0, sizeof(_pending));
  if (!load()) defaults();
  applyChannel();
  applyHardware();
  piBegin();
  Serial.printf("MeshAddon %s ready (chan %s, pi %s)\n", MESHADDON_VERSION, _cfg.chan_name, _cfg.pi_en ? "on" : "off");
}

void MeshAddon::loop() {
  uint32_t now = millis();
  pollDigital();
  if (now - _last_poll >= POLL_MS) { _last_poll = now; pollSensors(false); }
  for (int i = 0; i < ADDON_MAX_SLOTS; i++) {   // relay pulse end
    if (_cfg.slots[i].type == AST_RELAY && _st[i].pulse_until && (int32_t)(now - _st[i].pulse_until) >= 0) {
      _st[i].pulse_until = 0; _st[i].out = 0;
      digitalWrite(_cfg.slots[i].pin, _cfg.slots[i].invert ? HIGH : LOW);
    }
  }
  piLoop();
  flushReplies();
}

// ---------- channel ----------
void MeshAddon::applyChannel() {
  _chan_ok = false;
  if (!_cfg.chan_name[0]) return;
  memset(&_chan, 0, sizeof(_chan));
  if (_cfg.chan_key_set) {
    memcpy(_chan.secret, _cfg.chan_key, 16);
  } else {   // hashtag channel: key = first 16 bytes of sha256("#name")
    mesh::Utils::sha256(_chan.secret, 16, (const uint8_t*)_cfg.chan_name, strlen(_cfg.chan_name));
  }
  mesh::Utils::sha256(_chan.hash, sizeof(_chan.hash), _chan.secret, 16);
  _chan_ok = true;
}

int MeshAddon::searchChannel(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  if (!_chan_ok || max_matches < 1 || _chan.hash[0] != hash[0]) return 0;
  channels[0] = _chan;
  return 1;
}

void MeshAddon::onChannelText(const uint8_t* data, size_t len) {
  if (len < 6 || (data[4] >> 2) != 0) return;   // plain text only
  char msg[MAX_PACKET_PAYLOAD + 1];
  size_t n = len - 5; if (n > MAX_PACKET_PAYLOAD) n = MAX_PACKET_PAYLOAD;
  memcpy(msg, data + 5, n); msg[n] = 0;

  // "<sender>: <text>"
  char* sep = strstr(msg, ": ");
  const char* from = "?";
  char* text = msg;
  if (sep) { *sep = 0; from = msg; text = sep + 2; }
  if (strcmp(from, the_mesh.getNodeName()) == 0) return;   // our own reply

  char ej[200], ef[48];
  jesc(text, ej, sizeof(ej)); jesc(from, ef, sizeof(ef));
  char ec[48]; jesc(_cfg.chan_name, ec, sizeof(ec));
  char ev[320]; snprintf(ev, sizeof(ev), "{\"t\":\"chan\",\"ch\":\"%s\",\"from\":\"%s\",\"text\":\"%s\"}", ec, ef, ej);
  evt(ev);

  size_t pl = strlen(_cfg.prefix);
  if (pl == 0 || strncmp(text, _cfg.prefix, pl) != 0) return;

  // sender allow-list (comma separated names, empty or "*" = everyone). NOT cryptographic.
  if (_cfg.allow[0] && strcmp(_cfg.allow, "*") != 0) {
    char list[64]; strncpy(list, _cfg.allow, sizeof(list)); list[63] = 0;
    bool ok = false;
    for (char* tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) { trim(tok); if (strcasecmp(tok, from) == 0) { ok = true; break; } }
    if (!ok) return;
  }

  char cmd[160]; strncpy(cmd, text + pl, sizeof(cmd) - 1); cmd[sizeof(cmd) - 1] = 0; trim(cmd);
  char lc[160]; strcpy(lc, cmd); lower(lc);
  char out[ADDON_REPLY_MAX + 1];

  if (strcmp(lc, "scripts") == 0) {
    if (!_cfg.pi_en) { queueReply("no Pi link"); return; }
    if (!_nscripts) { queueReply(_pi_up ? "no Pi scripts" : "Pi link is down"); return; }
    size_t o = snprintf(out, sizeof(out), "scripts:");
    for (int i = 0; i < _nscripts && o < sizeof(out) - 2; i++) o += snprintf(out + o, sizeof(out) - o, "%s %s", i ? "," : "", _scripts[i].name);
    queueReply(out);
    return;
  }
  if (strncmp(lc, "run", 3) == 0 && (lc[3] == 0 || lc[3] == ' ')) {
    char err[80];
    if (!piRun(from, cmd + 3, false, err, sizeof(err))) queueReply(err);
    return;
  }
  for (int i = 0; i < ADDON_MAX_RULES; i++) {
    if (_cfg.rules[i].cmd[0] && strcasecmp(_cfg.rules[i].cmd, cmd) == 0) {
      doAction(_cfg.rules[i].act, out, sizeof(out));
      queueReply(out);
      return;
    }
  }
  // unknown command: stay silent (don't spam the channel)
}

void MeshAddon::queueReply(const char* text) {
  if (_rq_n >= 4) return;   // drop if flooded
  strncpy(_rq[_rq_n], text, ADDON_REPLY_MAX); _rq[_rq_n][ADDON_REPLY_MAX] = 0;
  _rq_n++;
}

void MeshAddon::flushReplies() {
  if (!_rq_n || !_chan_ok) return;
  uint32_t now = millis();
  if (_last_reply && now - _last_reply < ADDON_REPLY_GAP_MS) return;
  _last_reply = now ? now : 1;
  if (the_mesh.addonSendChannelText(_chan, _rq[0])) {
    char e[200]; jesc(_rq[0], e, sizeof(e));
    char ev[240]; snprintf(ev, sizeof(ev), "{\"t\":\"reply\",\"text\":\"%s\"}", e);
    evt(ev);
  }
  for (int i = 1; i < _rq_n; i++) memcpy(_rq[i - 1], _rq[i], sizeof(_rq[0]));
  _rq_n--;
}

// ---------- actions ----------
void MeshAddon::formatValue(int i, char* out, size_t n) const {
  const AddonSlotCfg& c = _cfg.slots[i];
  const AddonSlotState& s = _st[i];
  if (c.type == AST_RELAY) { snprintf(out, n, "%s", s.out ? "ON" : "OFF"); return; }
  if (c.type == AST_I2C)   { snprintf(out, n, "see telemetry"); return; }
  if (c.type == AST_COUNTER) { snprintf(out, n, "%.2f", s.count * (c.per ? c.per : 1)); return; }
  if (!s.valid) { snprintf(out, n, "n/a"); return; }
  switch (c.type) {
    case AST_DS18B20: snprintf(out, n, "%.1fC", s.v1); break;
    case AST_DHT22:   snprintf(out, n, "%.1fC %.0f%%", s.v1, s.v2); break;
    case AST_ANALOG:  snprintf(out, n, "%.2f", s.v1); break;
    case AST_DIGIN:   snprintf(out, n, "%s", s.v1 ? "closed" : "open"); break;
    case AST_PIR:     snprintf(out, n, "%s", s.v1 ? "motion" : "idle"); break;
    default:          snprintf(out, n, "?");
  }
}

void MeshAddon::doAction(const char* act, char* out, size_t n) {
  if (strcmp(act, "status") == 0) {
    uint32_t up = millis() / 1000;
    size_t o = snprintf(out, n, "bat %.2fV up %lud%luh", board.getBattMilliVolts() / 1000.0f,
                        (unsigned long)(up / 86400), (unsigned long)(up % 86400 / 3600));
    for (int i = 0; i < ADDON_MAX_SLOTS && o < n - 1; i++) {
      if (!_cfg.slots[i].type || _cfg.slots[i].type == AST_I2C) continue;
      char v[24]; formatValue(i, v, sizeof(v));
      o += snprintf(out + o, n - o, " | %s %s", _cfg.slots[i].label, v);
    }
    return;
  }
  int slot = 0;
  if (sscanf(act, "slot:%d", &slot) == 1) {
    if (slot < 1 || slot > ADDON_MAX_SLOTS || !_cfg.slots[slot - 1].type) { snprintf(out, n, "slot %d empty", slot); return; }
    char v[24]; formatValue(slot - 1, v, sizeof(v));
    snprintf(out, n, "%s: %s", _cfg.slots[slot - 1].label, v);
    return;
  }
  char op = 0;
  if (sscanf(act, "relay:%d=%c", &slot, &op) == 2) {
    int i = slot - 1;
    if (i < 0 || i >= ADDON_MAX_SLOTS || _cfg.slots[i].type != AST_RELAY) { snprintf(out, n, "slot %d is not a relay", slot); return; }
    AddonSlotCfg& c = _cfg.slots[i];
    uint8_t v = op == 't' ? !_st[i].out : (op == '1');
    _st[i].out = v;
    digitalWrite(c.pin, (v ^ c.invert) ? HIGH : LOW);
    _st[i].pulse_until = (v && c.pulse_ms) ? (millis() + c.pulse_ms) | 1 : 0;
    char ev[80]; snprintf(ev, sizeof(ev), "{\"t\":\"sensor\",\"slot\":%d,\"v\":%d}", slot, v);
    evt(ev);
    snprintf(out, n, "%s: %s%s", c.label, v ? "ON" : "OFF", (v && c.pulse_ms) ? " (pulse)" : "");
    return;
  }
  snprintf(out, n, "unknown action %s", act);
}

// ---------- sensors ----------
bool MeshAddon::pinAllowed(int pin) const {
  for (uint8_t p : ALLOWED_PINS) if (p == pin) return true;
  return false;
}
const char* MeshAddon::typeName(uint8_t t) const { return t < sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]) ? TYPE_NAMES[t] : "?"; }
uint8_t MeshAddon::typeFromName(const char* s) const {
  for (uint8_t i = 0; i < sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]); i++) if (strcasecmp(s, TYPE_NAMES[i]) == 0) return i;
  return 255;
}

void MeshAddon::releaseSlot(int i) {
  AddonSlotState& s = _st[i];
  const AddonSlotCfg& c = _cfg.slots[i];
  if (c.type == AST_COUNTER && c.pin) detachInterrupt(c.pin);
  if (c.type == AST_DS18B20) { delete (DallasTemperature*)s.drv2; delete (OneWire*)s.drv; }
  if (c.type == AST_DHT22) delete (DHTesp*)s.drv;
  memset(&s, 0, sizeof(s));
}

void MeshAddon::initSlot(int i) {
  AddonSlotCfg& c = _cfg.slots[i];
  AddonSlotState& s = _st[i];
  memset(&s, 0, sizeof(s));
  s.last_digital = -1;
  switch (c.type) {
    case AST_DS18B20: {
      OneWire* ow = new OneWire(c.pin);
      DallasTemperature* dt = new DallasTemperature(ow);
      dt->begin(); dt->setWaitForConversion(false); dt->requestTemperatures();
      s.drv = ow; s.drv2 = dt;
      break;
    }
    case AST_DHT22: { DHTesp* d = new DHTesp(); d->setup(c.pin, DHTesp::DHT22); s.drv = d; break; }
    case AST_ANALOG: pinMode(c.pin, INPUT); analogSetPinAttenuation(c.pin, ADC_11db); break;
    case AST_DIGIN: case AST_PIR: pinMode(c.pin, c.pull_up ? INPUT_PULLUP : INPUT); break;
    case AST_COUNTER:
      pinMode(c.pin, c.pull_up ? INPUT_PULLUP : INPUT);
      attachInterruptArg(c.pin, counterIsr, &s, FALLING);
      break;
    case AST_RELAY: pinMode(c.pin, OUTPUT); digitalWrite(c.pin, c.invert ? HIGH : LOW); break;
    default: break;   // i2c: handled by EnvironmentSensorManager on ENV_PIN_SDA/SCL
  }
}

void MeshAddon::applyHardware() {
  for (int i = 0; i < ADDON_MAX_SLOTS; i++) initSlot(i);
  pollSensors(true);
}

void MeshAddon::pollDigital() {
  for (int i = 0; i < ADDON_MAX_SLOTS; i++) {
    const AddonSlotCfg& c = _cfg.slots[i];
    if (c.type != AST_DIGIN && c.type != AST_PIR) continue;
    int raw = digitalRead(c.pin);
    int v = (c.type == AST_DIGIN && c.invert) ? !raw : raw;
    if (v != _st[i].last_digital) {
      _st[i].last_digital = v; _st[i].v1 = v; _st[i].valid = true;
      char ev[64]; snprintf(ev, sizeof(ev), "{\"t\":\"sensor\",\"slot\":%d,\"v\":%d}", i + 1, v);
      evt(ev);
    }
  }
}

void MeshAddon::pollSensors(bool force) {
  for (int i = 0; i < ADDON_MAX_SLOTS; i++) {
    const AddonSlotCfg& c = _cfg.slots[i];
    AddonSlotState& s = _st[i];
    switch (c.type) {
      case AST_DS18B20: {
        DallasTemperature* dt = (DallasTemperature*)s.drv2;
        if (!dt) break;
        float t = dt->getTempCByIndex(0);
        s.valid = t > -100 && t < 125 && t != 85.0f;   // -127 = disconnected, 85 = power-on value
        if (s.valid) s.v1 = t;
        dt->requestTemperatures();   // async; read on the next poll
        break;
      }
      case AST_DHT22: {
        DHTesp* d = (DHTesp*)s.drv;
        if (!d) break;
        TempAndHumidity th = d->getTempAndHumidity();
        s.valid = d->getStatus() == DHTesp::ERROR_NONE;
        if (s.valid) { s.v1 = th.temperature; s.v2 = th.humidity; }
        break;
      }
      case AST_ANALOG:
        s.v1 = analogReadMilliVolts(c.pin) / 1000.0f * (c.scale ? c.scale : 1) + c.offset;
        s.valid = true;
        break;
      case AST_COUNTER: s.v1 = s.count * (c.per ? c.per : 1); s.valid = true; break;
      case AST_RELAY:   s.v1 = s.out; s.valid = true; break;
      default: continue;
    }
    if (s.valid && c.type != AST_RELAY) {
      char ev[96];
      if (c.type == AST_DHT22) snprintf(ev, sizeof(ev), "{\"t\":\"sensor\",\"slot\":%d,\"v\":%.2f,\"v2\":%.1f}", i + 1, s.v1, s.v2);
      else snprintf(ev, sizeof(ev), "{\"t\":\"sensor\",\"slot\":%d,\"v\":%.3f}", i + 1, s.v1);
      evt(ev);
    }
  }
}

void MeshAddon::appendTelemetry(uint8_t perms, CayenneLPP& lpp) {
  if (!(perms & TELEM_PERM_ENVIRONMENT)) return;
  for (int i = 0; i < ADDON_MAX_SLOTS; i++) {
    const AddonSlotCfg& c = _cfg.slots[i];
    const AddonSlotState& s = _st[i];
    uint8_t ch = c.ch ? c.ch : 10 + i;
    switch (c.type) {
      case AST_DS18B20: if (s.valid) lpp.addTemperature(ch, s.v1); break;
      case AST_DHT22:   if (s.valid) { lpp.addTemperature(ch, s.v1); lpp.addRelativeHumidity(ch, s.v2); } break;
      case AST_ANALOG:  lpp.addAnalogInput(ch, s.v1); break;
      case AST_DIGIN:   if (s.valid) lpp.addDigitalInput(ch, (uint8_t)s.v1); break;
      case AST_PIR:     if (s.valid) lpp.addPresence(ch, (uint8_t)s.v1); break;
      case AST_COUNTER: lpp.addGenericSensor(ch, s.count * (c.per ? c.per : 1)); break;
      case AST_RELAY:   lpp.addDigitalOutput(ch, s.out); break;
      default: break;
    }
  }
}

void MeshAddon::onRx(uint8_t payload_type, float snr, float rssi) {
  if (!_events) return;
  static const char* names[] = {"REQ", "RESPONSE", "TXT", "ACK", "ADVERT", "GRP_TXT", "GRP_DATA", "ANON_REQ", "PATH", "TRACE", "MULTIPART", "CONTROL"};
  const char* tn = payload_type < sizeof(names) / sizeof(names[0]) ? names[payload_type] : "RAW";
  char ev[96]; snprintf(ev, sizeof(ev), "{\"t\":\"rx\",\"type\":\"%s\",\"snr\":%.1f,\"rssi\":%d}", tn, snr, (int)rssi);
  evt(ev);
}

// ---------- Pi link ----------
void MeshAddon::piBegin() {
  if (_pi) { _pi->end(); }
  _pi_up = false; _nscripts = 0; _pilen = 0;
  if (!_cfg.pi_en) return;
  if (!_pi) _pi = new HardwareSerial(2);   // UART0 = debug pins, UART1 = GPS
  _pi->begin(_cfg.pi_baud ? _cfg.pi_baud : 115200, SERIAL_8N1, _cfg.pi_rx, _cfg.pi_tx);
  char name[48]; jesc(the_mesh.getNodeName(), name, sizeof(name));
  char j[160]; snprintf(j, sizeof(j), "{\"t\":\"hello\",\"fw\":\"%s\",\"node\":\"%s\"}", the_mesh.getFirmwareVer(), name);
  piSend(j);
  _pi_last_ping = millis();
}

void MeshAddon::piSend(const char* json) {
  if (_pi && _cfg.pi_en) { _pi->print(json); _pi->print('\n'); }
}

void MeshAddon::piSendSensors() {
  char j[300]; size_t o = snprintf(j, sizeof(j), "{\"t\":\"sensors\",\"v\":{");
  bool first = true;
  for (int i = 0; i < ADDON_MAX_SLOTS && o < sizeof(j) - 30; i++) {
    const AddonSlotCfg& c = _cfg.slots[i];
    if (!c.type || c.type == AST_I2C || !_st[i].valid) continue;
    o += snprintf(j + o, sizeof(j) - o, "%s\"%d\":%.3f", first ? "" : ",", i + 1, c.type == AST_RELAY ? (float)_st[i].out : _st[i].v1);
    first = false;
  }
  snprintf(j + o, sizeof(j) - o, "}}");
  piSend(j);
}

void MeshAddon::piLoop() {
  if (!_pi || !_cfg.pi_en) return;
  uint32_t now = millis();
  while (_pi->available()) {
    char c = _pi->read();
    if (c == '\n') { _pibuf[_pilen] = 0; if (_pilen) piHandleLine(_pibuf); _pilen = 0; }
    else if (c != '\r' && _pilen < (int)sizeof(_pibuf) - 1) _pibuf[_pilen++] = c;
    else if (_pilen >= (int)sizeof(_pibuf) - 1) _pilen = 0;   // overlong line: drop
  }
  if (now - _pi_last_ping >= PI_PING_MS) { _pi_last_ping = now; piSend("{\"t\":\"ping\"}"); }
  if (now - _pi_last_sensors >= PI_SENSORS_MS) { _pi_last_sensors = now; piSendSensors(); }
  if (_pi_up && now - _pi_last_rx > PI_TIMEOUT_MS) {
    _pi_up = false;
    evt("{\"t\":\"pi\",\"state\":\"down\"}");
  }
  for (auto& p : _pending) {
    if (p.id && now - p.at > PI_RUN_TIMEOUT_MS) {
      char msg[80]; snprintf(msg, sizeof(msg), "%s: no reply from Pi", p.script);
      if (!p.from_usb) queueReply(msg);
      char ev[120]; snprintf(ev, sizeof(ev), "{\"t\":\"pireply\",\"script\":\"%s\",\"text\":\"%s\"}", p.script, msg);
      evt(ev);
      p.id = 0;
    }
  }
}

void MeshAddon::piHandleLine(char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  _pi_last_rx = millis();
  const char* t = doc["t"] | "";
  if (!_pi_up) {
    _pi_up = true;
    char h[40], d[80]; jesc(_pi_host, h, sizeof(h)); jesc(_pi_dir, d, sizeof(d));
    char ev[200]; snprintf(ev, sizeof(ev), "{\"t\":\"pi\",\"state\":\"up\",\"host\":\"%s\",\"dir\":\"%s\"}", h, d);
    if (strcmp(t, "hello") != 0) evt(ev);
  }
  if (strcmp(t, "hello") == 0) {
    strncpy(_pi_host, doc["host"] | "", sizeof(_pi_host) - 1);
    strncpy(_pi_dir, doc["dir"] | "", sizeof(_pi_dir) - 1);
    char h[40], d[80]; jesc(_pi_host, h, sizeof(h)); jesc(_pi_dir, d, sizeof(d));
    char ev[200]; snprintf(ev, sizeof(ev), "{\"t\":\"pi\",\"state\":\"up\",\"host\":\"%s\",\"dir\":\"%s\"}", h, d);
    evt(ev);
    piSendSensors();
  } else if (strcmp(t, "scripts") == 0) {
    _nscripts = 0;
    for (JsonObject s : doc["list"].as<JsonArray>()) {
      if (_nscripts >= ADDON_MAX_SCRIPTS) break;
      strncpy(_scripts[_nscripts].name, s["name"] | "", sizeof(_scripts[0].name) - 1);
      _scripts[_nscripts].name[sizeof(_scripts[0].name) - 1] = 0;
      strncpy(_scripts[_nscripts].desc, s["desc"] | "", sizeof(_scripts[0].desc) - 1);
      _scripts[_nscripts].desc[sizeof(_scripts[0].desc) - 1] = 0;
      if (_scripts[_nscripts].name[0]) _nscripts++;
    }
    // forward the list to the browser as-is
    if (_events) { Serial.print("EVT {\"t\":\"scripts\",\"list\":"); serializeJson(doc["list"], Serial); Serial.println("}"); }
  } else if (strcmp(t, "reply") == 0) {
    uint32_t id = doc["id"] | 0;
    const char* text = doc["text"] | "";
    for (auto& p : _pending) {
      if (p.id && p.id == id) {
        if (!p.from_usb) queueReply(text);
        char e[200], sc[40]; jesc(text, e, sizeof(e)); jesc(p.script, sc, sizeof(sc));
        char ev[280]; snprintf(ev, sizeof(ev), "{\"t\":\"pireply\",\"script\":\"%s\",\"text\":\"%s\"}", sc, e);
        evt(ev);
        p.id = 0;
        break;
      }
    }
  }
  // "pong": only refreshes _pi_last_rx
}

// args = " <name> [arg ...]"
bool MeshAddon::piRun(const char* from, const char* args, bool from_usb, char* err, size_t n) {
  if (!_cfg.pi_en) { snprintf(err, n, "no Pi link"); return false; }
  if (!_pi_up) { snprintf(err, n, "Pi link is down"); return false; }
  char buf[160]; strncpy(buf, args, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; trim(buf);
  char* name = strtok(buf, " ");
  if (!name) { snprintf(err, n, "usage: run <script> [args]"); return false; }
  Pending* slot = nullptr;
  for (auto& p : _pending) if (!p.id) { slot = &p; break; }
  if (!slot) { snprintf(err, n, "Pi busy, try again"); return false; }

  JsonDocument doc;
  doc["t"] = "run"; doc["id"] = ++_run_id; doc["ch"] = _cfg.chan_name; doc["from"] = from; doc["script"] = name;
  JsonArray a = doc["args"].to<JsonArray>();
  for (char* tok = strtok(NULL, " "); tok; tok = strtok(NULL, " ")) a.add(tok);
  char j[300]; serializeJson(doc, j, sizeof(j));
  piSend(j);
  slot->id = _run_id; slot->at = millis(); slot->from_usb = from_usb;
  strncpy(slot->script, name, sizeof(slot->script) - 1); slot->script[sizeof(slot->script) - 1] = 0;
  return true;
}

// ---------- USB CLI ----------
void MeshAddon::evt(const char* json) {
  if (_events) { Serial.print("EVT "); Serial.println(json); }
}
void MeshAddon::cliReply(const char* s) { Serial.print("  -> "); Serial.println(s); }

void MeshAddon::cmdDump() {
  JsonDocument doc;
  JsonArray slots = doc["slots"].to<JsonArray>();
  for (int i = 0; i < ADDON_MAX_SLOTS; i++) {
    const AddonSlotCfg& c = _cfg.slots[i];
    if (!c.type) continue;
    JsonObject o = slots.add<JsonObject>();
    o["slot"] = i + 1; o["type"] = typeName(c.type); o["pin"] = c.pin;
    if (c.type == AST_I2C) o["pin2"] = c.pin2;
    o["label"] = c.label; o["ch"] = c.ch;
    JsonObject op = o["opts"].to<JsonObject>();
    switch (c.type) {
      case AST_ANALOG:  op["scale"] = c.scale; op["offset"] = c.offset; break;
      case AST_DIGIN:   op["pull"] = c.pull_up ? "up" : "none"; op["invert"] = c.invert; break;
      case AST_COUNTER: op["per"] = c.per; break;
      case AST_RELAY:   op["invert"] = c.invert; op["pulse_ms"] = c.pulse_ms; break;
      default: o.remove("opts");
    }
  }
  JsonObject ch = doc["chan"].to<JsonObject>();
  ch["name"] = _cfg.chan_name; ch["prefix"] = _cfg.prefix; ch["allow"] = _cfg.allow;
  if (_cfg.chan_key_set) { char hx[33]; for (int i = 0; i < 16; i++) sprintf(hx + i * 2, "%02x", _cfg.chan_key[i]); ch["key"] = hx; }
  else ch["key"] = "";
  JsonObject pi = doc["pi"].to<JsonObject>();
  pi["en"] = _cfg.pi_en; pi["link"] = "uart"; pi["tx"] = _cfg.pi_tx; pi["rx"] = _cfg.pi_rx; pi["baud"] = _cfg.pi_baud;
  pi["state"] = _pi_up ? "up" : "down"; pi["host"] = _pi_host;
  JsonArray rules = doc["rules"].to<JsonArray>();
  for (auto& r : _cfg.rules) if (r.cmd[0]) { JsonObject o = rules.add<JsonObject>(); o["cmd"] = r.cmd; o["act"] = r.act; }
  JsonArray sc = doc["scripts"].to<JsonArray>();
  for (int i = 0; i < _nscripts; i++) { JsonObject o = sc.add<JsonObject>(); o["name"] = _scripts[i].name; o["desc"] = _scripts[i].desc; }
  Serial.print("  -> "); serializeJson(doc, Serial); Serial.println();
}

// "<n> <type>,<pin>[+<pin2>],<ch>,<label>[,k=v;k=v]"  or  "<n> none"
void MeshAddon::cmdSlot(const char* args) {
  char buf[160]; strncpy(buf, args, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; trim(buf);
  int n = atoi(buf);
  if (n < 1 || n > ADDON_MAX_SLOTS) return cliReply("Error: slot must be 1-8");
  char* spec = strchr(buf, ' ');
  if (!spec) return cliReply("Error: usage: addon slot <n> <type>,<pin>,<ch>,<label>[,opts]");
  spec++; trim(spec);
  int i = n - 1;
  if (strcasecmp(spec, "none") == 0) {
    releaseSlot(i); memset(&_cfg.slots[i], 0, sizeof(_cfg.slots[i]));
    return cliReply("OK");
  }
  char* f[5] = {0}; int nf = 0;
  for (char* p = spec; p && nf < 5; ) { f[nf++] = p; char* c = strchr(p, ','); if (c) { *c = 0; p = c + 1; } else p = NULL; }
  if (nf < 4) return cliReply("Error: need type,pin,ch,label");
  uint8_t type = typeFromName(f[0]);
  if (type == 255 || type == AST_NONE) return cliReply("Error: unknown type");
  int pin = atoi(f[1]), pin2 = 0; char* plus = strchr(f[1], '+'); if (plus) pin2 = atoi(plus + 1);
  if (!pinAllowed(pin) || (type == AST_I2C && !pinAllowed(pin2))) return cliReply("Error: pin not allowed on Heltec V4");
  if (type == AST_ANALOG && !isAdcPin(pin)) return cliReply("Error: analog needs an ADC pin (3,4,6,15,16)");
  if (type == AST_I2C && (pin != 3 || pin2 != 4)) return cliReply("Error: I2C is fixed to SDA=3, SCL=4 in this build");
  if (_cfg.pi_en && (pin == _cfg.pi_tx || pin == _cfg.pi_rx)) return cliReply("Error: pin is used by the Pi UART");
  for (int j = 0; j < ADDON_MAX_SLOTS; j++) {
    if (j == i || !_cfg.slots[j].type) continue;
    const AddonSlotCfg& o = _cfg.slots[j];
    if (o.pin == pin || (o.type == AST_I2C && o.pin2 == pin) || (pin2 && (o.pin == pin2 || o.pin2 == pin2)))
      return cliReply("Error: pin already used by another slot");
  }
  releaseSlot(i);
  AddonSlotCfg& c = _cfg.slots[i];
  memset(&c, 0, sizeof(c));
  c.type = type; c.pin = pin; c.pin2 = pin2; c.ch = atoi(f[2]);
  strncpy(c.label, f[3], sizeof(c.label) - 1); trim(c.label);
  c.scale = 1; c.per = 1; c.pull_up = 1;
  if (nf == 5) {
    for (char* kv = strtok(f[4], ";"); kv; kv = strtok(NULL, ";")) {
      char* eq = strchr(kv, '='); if (!eq) continue; *eq = 0; const char* v = eq + 1;
      if (!strcmp(kv, "scale")) c.scale = atof(v);
      else if (!strcmp(kv, "offset")) c.offset = atof(v);
      else if (!strcmp(kv, "per")) c.per = atof(v);
      else if (!strcmp(kv, "invert")) c.invert = atoi(v) ? 1 : 0;
      else if (!strcmp(kv, "pulse_ms")) c.pulse_ms = atoi(v);
      else if (!strcmp(kv, "pull")) c.pull_up = strcmp(v, "none") != 0;
    }
  }
  initSlot(i);
  cliReply("OK");
}

// "<type>,<pin>[+pin2]": one-off reading without changing the config
void MeshAddon::cmdTest(const char* args) {
  char buf[40]; strncpy(buf, args, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; trim(buf);
  char* comma = strchr(buf, ','); if (!comma) return cliReply("Error: usage: addon test <type>,<pin>");
  *comma = 0; uint8_t type = typeFromName(buf); int pin = atoi(comma + 1);
  if (!pinAllowed(pin)) return cliReply("Error: pin not allowed");
  for (int j = 0; j < ADDON_MAX_SLOTS; j++)
    if (_cfg.slots[j].type && (_cfg.slots[j].pin == pin) && _cfg.slots[j].type != type) return cliReply("Error: pin used by another slot");
  char out[64];
  switch (type) {
    case AST_DS18B20: {
      OneWire ow(pin); DallasTemperature dt(&ow); dt.begin();
      if (dt.getDeviceCount() == 0) return cliReply("Error: no DS18B20 found (pull-up resistor?)");
      dt.setWaitForConversion(true); dt.requestTemperatures();
      snprintf(out, sizeof(out), "%.2f °C (%d sensor%s)", dt.getTempCByIndex(0), dt.getDeviceCount(), dt.getDeviceCount() > 1 ? "s" : "");
      break;
    }
    case AST_DHT22: {
      DHTesp d; d.setup(pin, DHTesp::DHT22); delay(2100);
      TempAndHumidity th = d.getTempAndHumidity();
      if (d.getStatus() != DHTesp::ERROR_NONE) return cliReply("Error: DHT22 not responding");
      snprintf(out, sizeof(out), "%.1f °C %.0f %%", th.temperature, th.humidity);
      break;
    }
    case AST_ANALOG:
      if (!isAdcPin(pin)) return cliReply("Error: not an ADC pin");
      pinMode(pin, INPUT);
      snprintf(out, sizeof(out), "%.3f V", analogReadMilliVolts(pin) / 1000.0f);
      break;
    case AST_DIGIN: case AST_PIR: {
      pinMode(pin, type == AST_DIGIN ? INPUT_PULLUP : INPUT);
      delay(5);
      int v = digitalRead(pin);
      snprintf(out, sizeof(out), "pin %s", v ? "HIGH" : "LOW");
      break;
    }
    case AST_COUNTER: {
      pinMode(pin, INPUT_PULLUP);
      int edges = 0, last = digitalRead(pin); uint32_t t0 = millis();
      while (millis() - t0 < 3000) { int v = digitalRead(pin); if (last && !v) edges++; last = v; delay(1); }
      snprintf(out, sizeof(out), "%d pulses in 3 s", edges);
      break;
    }
    case AST_RELAY: {
      pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); delay(500); digitalWrite(pin, LOW);
      snprintf(out, sizeof(out), "clicked 500 ms (HIGH pulse)");
      for (int j = 0; j < ADDON_MAX_SLOTS; j++) if (_cfg.slots[j].pin == pin) initSlot(j);   // restore idle level
      break;
    }
    default: return cliReply("Error: unknown type");
  }
  cliReply(out);
}

void MeshAddon::cmdI2cScan(const char* args) {
  int sda = 0, scl = 0;
  if (sscanf(args, " %d,%d", &sda, &scl) != 2) return cliReply("Error: usage: addon i2c scan <sda>,<scl>");
#if defined(ENV_PIN_SDA) && defined(ENV_PIN_SCL)
  if (sda != ENV_PIN_SDA || scl != ENV_PIN_SCL) return cliReply("Error: this build scans only SDA=3, SCL=4");
  TwoWire& w = Wire1;    // already started by EnvironmentSensorManager
#else
  return cliReply("Error: build has no ENV I2C bus");
  TwoWire& w = Wire1;
#endif
  char out[120]; size_t o = snprintf(out, sizeof(out), "found");
  int found = 0;
  for (uint8_t a = 1; a < 127 && o < sizeof(out) - 6; a++) {
    w.beginTransmission(a);
    if (w.endTransmission() == 0) { o += snprintf(out + o, sizeof(out) - o, " 0x%02x", a); found++; }
  }
  if (!found) snprintf(out, sizeof(out), "found none");
  cliReply(out);
}

bool MeshAddon::handleSerialCommand(const char* line) {
  if (strncmp(line, "addon", 5) != 0 || (line[5] != 0 && line[5] != ' ')) return false;
  const char* a = line + 5; while (*a == ' ') a++;

  if (!*a || !strcmp(a, "info")) {
    char out[80];
    snprintf(out, sizeof(out), "addon %s slots=%d pi=%d chan=%d", MESHADDON_VERSION, ADDON_MAX_SLOTS, _cfg.pi_en, _chan_ok ? 1 : 0);
    cliReply(out);
  }
  else if (!strcmp(a, "dump")) cmdDump();
  else if (!strncmp(a, "slot ", 5)) cmdSlot(a + 5);
  else if (!strcmp(a, "save")) cliReply(save() ? "OK" : "Error: could not write /addon.cfg");
  else if (!strcmp(a, "events on"))  { _events = true;  cliReply("OK"); }
  else if (!strcmp(a, "events off")) { _events = false; cliReply("OK"); }
  else if (!strncmp(a, "chan ", 5)) {
    // <#name>,<hexkey|->,<prefix>,<allow|*>
    char buf[160]; strncpy(buf, a + 5, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char* f[4] = {0}; int nf = 0;
    for (char* p = buf; p && nf < 4; ) { f[nf++] = p; char* c = strchr(p, ','); if (c) { *c = 0; p = c + 1; } else p = NULL; }
    if (nf < 1 || !f[0][0]) return cliReply("Error: usage: addon chan <name>,<key|->,<prefix>,<allow|*>"), true;
    trim(f[0]);
    if (nf >= 2) {
      trim(f[1]);
      if (strcmp(f[1], "-") == 0 || !f[1][0]) _cfg.chan_key_set = 0;
      else {
        if (strlen(f[1]) != 32) return cliReply("Error: key must be 32 hex chars (16 bytes)"), true;
        for (int i = 0; i < 16; i++) {
          int h = hexval(f[1][i * 2]), l = hexval(f[1][i * 2 + 1]);
          if (h < 0 || l < 0) return cliReply("Error: key is not hex"), true;
          _cfg.chan_key[i] = h << 4 | l;
        }
        _cfg.chan_key_set = 1;
      }
    }
    strncpy(_cfg.chan_name, f[0], sizeof(_cfg.chan_name) - 1);
    if (nf >= 3) { trim(f[2]); strncpy(_cfg.prefix, f[2][0] ? f[2] : "!", sizeof(_cfg.prefix) - 1); }
    if (nf >= 4) { trim(f[3]); strncpy(_cfg.allow, strcmp(f[3], "*") ? f[3] : "", sizeof(_cfg.allow) - 1); }
    applyChannel();
    cliReply("OK");
  }
  else if (!strncmp(a, "rule add ", 9)) {
    // <action>,<command text>
    const char* p = a + 9; const char* c = strchr(p, ',');
    if (!c) return cliReply("Error: usage: addon rule add <action>,<command>"), true;
    char act[16] = {0}, cmd[32] = {0};
    strncpy(act, p, min((size_t)(c - p), sizeof(act) - 1)); trim(act);
    strncpy(cmd, c + 1, sizeof(cmd) - 1); trim(cmd); lower(cmd);
    if (!strncmp(cmd, "scripts", 7) || !strncmp(cmd, "run", 3)) return cliReply("Error: 'scripts' and 'run' are reserved"), true;
    int free_i = -1;
    for (int i = 0; i < ADDON_MAX_RULES; i++) {
      if (_cfg.rules[i].cmd[0] && !strcmp(_cfg.rules[i].cmd, cmd)) { free_i = i; break; }
      if (!_cfg.rules[i].cmd[0] && free_i < 0) free_i = i;
    }
    if (free_i < 0) return cliReply("Error: rule table full (16)"), true;
    strcpy(_cfg.rules[free_i].cmd, cmd); strcpy(_cfg.rules[free_i].act, act);
    cliReply("OK");
  }
  else if (!strncmp(a, "rule del ", 9)) {
    char cmd[32] = {0}; strncpy(cmd, a + 9, sizeof(cmd) - 1); trim(cmd); lower(cmd);
    bool hit = false;
    for (auto& r : _cfg.rules) if (r.cmd[0] && !strcmp(r.cmd, cmd)) { memset(&r, 0, sizeof(r)); hit = true; }
    cliReply(hit ? "OK" : "Error: no such rule");
  }
  else if (!strcmp(a, "pi list")) { piSend("{\"t\":\"list\"}"); cliReply(_pi_up ? "OK" : "OK (Pi link is down)"); }
  else if (!strncmp(a, "pi run ", 7)) {
    char err[80];
    if (piRun("usb", a + 6, true, err, sizeof(err))) cliReply("OK"); else cliReply(err);
  }
  else if (!strncmp(a, "pi ", 3)) {
    // <en>,<tx>,<rx>,<baud>   (or <en>,usb,<baud>: not supported yet)
    int en = 0, tx = 0, rx = 0; long baud = 115200;
    if (strstr(a, "usb")) return cliReply("Error: USB link to the Pi is not supported yet, use UART"), true;
    if (sscanf(a + 3, "%d,%d,%d,%ld", &en, &tx, &rx, &baud) < 3) return cliReply("Error: usage: addon pi <en>,<tx>,<rx>,<baud>"), true;
    if (en && (!pinAllowed(tx) || !pinAllowed(rx) || tx == rx)) return cliReply("Error: bad UART pins"), true;
    for (int j = 0; en && j < ADDON_MAX_SLOTS; j++)
      if (_cfg.slots[j].type && (_cfg.slots[j].pin == tx || _cfg.slots[j].pin == rx)) return cliReply("Error: pin used by a sensor slot"), true;
    _cfg.pi_en = en ? 1 : 0; _cfg.pi_tx = tx; _cfg.pi_rx = rx; _cfg.pi_baud = baud;
    piBegin();
    cliReply("OK");
  }
  else if (!strncmp(a, "set ", 4)) {
    int slot = 0; char op = 0;
    if (sscanf(a + 4, "%d %c", &slot, &op) != 2) return cliReply("Error: usage: addon set <slot> <0|1|t>"), true;
    char act[16]; snprintf(act, sizeof(act), "relay:%d=%c", slot, op);
    char out[80]; doAction(act, out, sizeof(out)); cliReply(out);
  }
  else if (!strncmp(a, "test ", 5)) cmdTest(a + 5);
  else if (!strncmp(a, "i2c scan", 8)) cmdI2cScan(a + 8);
  else cliReply("Error: unknown addon command");
  return true;
}

#endif
