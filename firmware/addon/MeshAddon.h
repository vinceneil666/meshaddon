#pragma once
/*
 * MeshAddon: GPIO sensors, relays, a command channel and a Raspberry Pi script link
 * for the MeshCore simple_repeater (Heltec WiFi LoRa 32 V4).
 *
 * Everything is compiled only with -D MESHADDON=1. Protocols: ~/meshaddon/docs/PROTOCOL.md
 *   A  USB CLI:  "addon ..." commands, "EVT {json}" event lines
 *   B  Pi link:  newline-delimited JSON on UART2 (default GPIO47 TX / GPIO48 RX)
 *   C  Mesh:     one group channel is decrypted; "!cmd" messages are answered on it
 */
#ifdef MESHADDON

#include <Arduino.h>
#include <Mesh.h>
#include <CayenneLPP.h>
#include <FS.h>

#ifndef MESHADDON_VERSION
  #define MESHADDON_VERSION "0.1"
#endif

#define ADDON_MAX_SLOTS     8
#define ADDON_MAX_RULES     16
#define ADDON_MAX_SCRIPTS   24
#define ADDON_REPLY_MAX     140
#define ADDON_REPLY_GAP_MS  5000UL   // at most one channel reply per 5 s (EU868 duty cycle)

enum AddonSlotType : uint8_t {
  AST_NONE = 0, AST_DS18B20, AST_DHT22, AST_ANALOG, AST_DIGIN, AST_COUNTER, AST_RELAY, AST_PIR, AST_I2C
};

// Persisted config (binary file /addon.cfg). Bump ADDON_CFG_VERSION on layout changes.
struct AddonSlotCfg {
  uint8_t type, pin, pin2, ch;
  char label[21];
  uint8_t pull_up, invert;
  uint16_t pulse_ms;
  float scale, offset, per;
};
struct AddonRuleCfg {
  char cmd[32];
  char act[16];       // status | slot:N | relay:N=1|0|t
};
struct AddonCfg {
  uint32_t magic;
  uint16_t version;
  AddonSlotCfg slots[ADDON_MAX_SLOTS];
  AddonRuleCfg rules[ADDON_MAX_RULES];
  char chan_name[32];
  uint8_t chan_key[16];
  uint8_t chan_key_set;
  char prefix[4];
  char allow[64];
  uint8_t pi_en, pi_tx, pi_rx;
  uint32_t pi_baud;
};

struct AddonSlotState {
  bool valid;
  float v1, v2;            // value (dht22: v1 temp, v2 humidity)
  volatile uint32_t count; // pulse counter
  volatile uint32_t last_us;
  uint8_t out;             // relay state
  uint32_t pulse_until;    // relay pulse end (millis)
  int last_digital;
  void* drv;               // OneWire/DallasTemperature or DHTesp
  void* drv2;
};

struct AddonScript { char name[25]; char desc[41]; };

class MeshAddon {
public:
  void begin(fs::FS* fs);
  void loop();

  // USB CLI: returns true if the line was an "addon" command (reply already printed)
  bool handleSerialCommand(const char* line);

  // mesh hooks (called from MyMesh)
  int  searchChannel(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches);
  void onChannelText(const uint8_t* data, size_t len);
  void appendTelemetry(uint8_t perms, CayenneLPP& lpp);
  void onRx(uint8_t payload_type, float snr, float rssi);

private:
  fs::FS* _fs = nullptr;
  AddonCfg _cfg;
  AddonSlotState _st[ADDON_MAX_SLOTS];
  mesh::GroupChannel _chan;
  bool _chan_ok = false;
  bool _events = false;
  uint32_t _last_poll = 0, _ds_requested = 0;

  // channel replies (rate limited)
  char _rq[4][ADDON_REPLY_MAX + 1];
  int _rq_n = 0;
  uint32_t _last_reply = 0;

  // Pi link
  HardwareSerial* _pi = nullptr;
  char _pibuf[600];
  int _pilen = 0;
  bool _pi_up = false;
  uint32_t _pi_last_rx = 0, _pi_last_ping = 0, _pi_last_sensors = 0;
  char _pi_host[33] = "";
  char _pi_dir[64] = "";
  AddonScript _scripts[ADDON_MAX_SCRIPTS];
  int _nscripts = 0;
  uint32_t _run_id = 0;
  struct Pending { uint32_t id; uint32_t at; bool from_usb; char script[25]; } _pending[4];

  void defaults();
  bool load();
  bool save();
  void applyChannel();
  void applyHardware();
  void releaseSlot(int i);
  void initSlot(int i);
  void pollSensors(bool force);
  void pollDigital();
  bool pinAllowed(int pin) const;
  const char* typeName(uint8_t t) const;
  uint8_t typeFromName(const char* s) const;
  void formatValue(int i, char* out, size_t n) const;
  void doAction(const char* act, char* out, size_t n);

  void queueReply(const char* text);
  void flushReplies();

  void piBegin();
  void piLoop();
  void piSend(const char* json);
  void piHandleLine(char* line);
  void piSendSensors();
  bool piRun(const char* from, const char* args, bool from_usb, char* err, size_t n);

  void evt(const char* json);
  void cliReply(const char* s);
  void cmdDump();
  void cmdSlot(const char* args);
  void cmdTest(const char* args);
  void cmdI2cScan(const char* args);
};

extern MeshAddon addon;

#endif
