#include "mqtt.h"
#include <espMqttClientAsync.h>
#include <ESP8266WiFi.h>
#include "config.h"
#include "secrets.h"
#include "zones.h"
#include "programs.h"
#include "history.h"
#include "net.h"
#include "storage.h"
#include "web.h"
#include "log.h"

namespace mqtt {

static espMqttClientAsync client;
static Config cfg;

static char devId[24];       // effective device id
static char base[40];        // sqwrtr/<device_id>
static char statusTopic[48];
static char haStatusTopic[40];  // <prefix>/status: "online" after a Home Assistant restart

static bool reconfigure = true;
static uint32_t nextAttempt = 0;
static uint32_t backoffMs = 0;
static const uint32_t BACKOFF_MIN_MS = 2000;
static const uint32_t BACKOFF_MAX_MS = 60000;
static char lastErr[40] = "";

// Set in the network context, handled in loop().
static volatile bool onlineEvent = false;
static volatile bool offlineEvent = false;
static volatile uint8_t offlineReason = 0;
static bool online = false;

// Incoming messages are queued in the network context and handled in loop().
struct Msg {
    char topic[72];
    char payload[200];
};
static const uint8_t INBOX = 4;
static Msg inbox[INBOX];
static volatile uint8_t inHead = 0, inCount = 0;

// Discovery: published one item per loop pass (heap!), -1 = idle.
static int16_t discoIdx = -1;
static uint32_t lastDisco = 0;
static uint32_t discoSig = 0;

// Published state, to only send changes. 0xFF / UINT32_MAX = unknown → send.
static uint8_t pubZoneState[ZONE_COUNT];
static uint32_t pubRemaining[ZONE_COUNT];
static uint32_t pubDuration[ZONE_COUNT];
static uint32_t lastRemainingPub = 0;
static bool stateDirty = true;
static uint32_t lastStatePub = 0;
static const uint32_t STATE_MIN_MS = 1000;
static const uint32_t STATE_HEARTBEAT_MS = 60000;
static const uint32_t REMAINING_EVERY_MS = 10000;
static const uint32_t MIN_HEAP = 12000;  // don't publish discovery/state below this

static char out[1024];  // shared payload buffer (publish copies it)

// ---------- config ----------

Config& config() { return cfg; }

const char* stateName() {
    if (!cfg.enabled || !cfg.host[0]) return "disabled";
    if (online) return "connected";
    return client.disconnected() ? "disconnected" : "connecting";
}
const char* lastError() { return lastErr; }

static bool validId(const char* s, size_t max) {
    size_t n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum(c) || c == '-' || c == '_')) return false;
    }
    return true;
}

bool configFromJson(JsonObjectConst in, Config& out, const char** err) {
    Config next = out;
    if (!in["enabled"].isNull()) {
        if (!in["enabled"].is<bool>()) return *err = "mqtt.enabled: true or false", false;
        next.enabled = in["enabled"];
    }
    if (!in["host"].isNull()) {
        const char* v = in["host"] | "";
        if (strlen(v) >= sizeof(next.host)) return *err = "mqtt.host: max 63 characters", false;
        strlcpy(next.host, v, sizeof(next.host));
    }
    if (!in["port"].isNull()) {
        uint32_t v = in["port"] | 0u;
        if (v < 1 || v > 65535) return *err = "mqtt.port: 1-65535", false;
        next.port = v;
    }
    if (!in["user"].isNull()) {
        const char* v = in["user"] | "";
        if (strlen(v) >= sizeof(next.user)) return *err = "mqtt.user: max 31 characters", false;
        strlcpy(next.user, v, sizeof(next.user));
    }
    if (!in["password"].isNull()) {
        const char* v = in["password"] | "";
        if (strlen(v) >= sizeof(next.password)) return *err = "mqtt.password: max 63 characters", false;
        strlcpy(next.password, v, sizeof(next.password));
    }
    if (!in["device_id"].isNull()) {
        const char* v = in["device_id"] | "";
        if (!validId(v, sizeof(next.deviceId) - 1)) return *err = "mqtt.device_id: max 23 characters a-z 0-9 - _", false;
        strlcpy(next.deviceId, v, sizeof(next.deviceId));
    }
    if (!in["discovery_prefix"].isNull()) {
        const char* v = in["discovery_prefix"] | "";
        if (!v[0] || !validId(v, sizeof(next.prefix) - 1)) return *err = "mqtt.discovery_prefix: 1-23 characters", false;
        strlcpy(next.prefix, v, sizeof(next.prefix));
    }
    out = next;
    return true;
}

void configToJson(JsonObject o, bool withPassword) {
    o["enabled"] = cfg.enabled;
    o["host"] = cfg.host;
    o["port"] = cfg.port;
    o["user"] = cfg.user;
    if (withPassword) o["password"] = cfg.password;
    else o["password_set"] = cfg.password[0] != '\0';
    o["device_id"] = cfg.deviceId;
    o["discovery_prefix"] = cfg.prefix;
    if (!withPassword) {
        o["effective_device_id"] = devId;
        o["state"] = stateName();
        o["error"] = lastErr;
    }
}

void applyConfig() { reconfigure = true; }

// ---------- publishing helpers ----------

static bool pub(const char* topic, const char* payload, bool retain) {
    if (!online) return false;
    return client.publish(topic, 0, retain, payload) != 0;
}

static void topicOf(char* buf, size_t len, const char* suffix) { snprintf(buf, len, "%s/%s", base, suffix); }

void publishEvent(const char* type, const char* detailJson) {
    stateDirty = true;
    if (!online) return;
    char topic[56], payload[260];
    topicOf(topic, sizeof(topic), "event");
    // detailJson is an object: insert "event" as the first field.
    const char* rest = (detailJson && detailJson[0] == '{') ? detailJson + 1 : "}";
    snprintf(payload, sizeof(payload), "{\"event\":\"%s\"%s%s", type, *rest == '}' ? "" : ",", rest);
    client.publish(topic, 0, false, payload);
}

void markStateDirty() { stateDirty = true; }

// ---------- discovery ----------

static const uint8_t ZONE_ITEMS = 5;
static const uint8_t GLOBAL_ITEMS = 14;
static const uint16_t DISCO_ITEMS = ZONE_COUNT * ZONE_ITEMS + GLOBAL_ITEMS + MAX_PROGRAMS;

// Fills the fields every entity shares. `obj` is unique per device.
static void common(JsonDocument& d, const char* obj, const char* name) {
    char buf[48];
    d["~"] = base;
    d["name"] = name;
    snprintf(buf, sizeof(buf), "%s_%s", devId, obj);
    d["uniq_id"] = buf;
    d["avty_t"] = "~/status";
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"].add(devId);
    dev["name"] = "sqwrtr";
    dev["mf"] = "sqwrtr";
    dev["mdl"] = "ESP8266 irrigation controller";
    dev["sw"] = FW_VERSION;
    snprintf(buf, sizeof(buf), "http://%s/", WiFi.localIP().toString().c_str());
    dev["cu"] = buf;
}

static void stateSensor(JsonDocument& d, const char* tpl) {
    d["stat_t"] = "~/state";
    d["val_tpl"] = tpl;
}

static void cmdButton(JsonDocument& d, const char* action, const char* icon) {
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"action\":\"%s\"}", action);
    d["cmd_t"] = "~/cmd";
    d["pl_prs"] = buf;
    d["ic"] = icon;
}

// Builds discovery item i into `d`. Sets component + object id. Returns false for
// "remove this entity" (empty retained payload).
static bool buildItem(uint16_t i, JsonDocument& d, const char*& component, char* obj, size_t objLen) {
    char name[48], tpl[96], t[40];

    if (i < ZONE_COUNT * ZONE_ITEMS) {
        const uint8_t z = i / ZONE_ITEMS, n = z + 1;
        const zones::Config& zc = zones::config(z);
        switch (i % ZONE_ITEMS) {
            case 0:
                component = "valve";
                snprintf(obj, objLen, "zone%u", n);
                common(d, obj, zc.name);
                snprintf(t, sizeof(t), "~/zone/%u/set", n);
                d["cmd_t"] = t;
                snprintf(t, sizeof(t), "~/zone/%u/state", n);
                d["stat_t"] = t;
                d["dev_cla"] = "water";
                d["ic"] = "mdi:sprinkler-variant";
                break;
            case 1:
                component = "number";
                snprintf(obj, objLen, "zone%u_duration", n);
                snprintf(name, sizeof(name), "%s duration", zc.name);
                common(d, obj, name);
                snprintf(t, sizeof(t), "~/zone/%u/duration/set", n);
                d["cmd_t"] = t;
                snprintf(t, sizeof(t), "~/zone/%u/duration", n);
                d["stat_t"] = t;
                d["min"] = 1;
                d["max"] = max<uint32_t>(1, zc.maxRuntimeS / 60);
                d["step"] = 1;
                d["unit_of_meas"] = "min";
                d["mode"] = "box";
                d["ic"] = "mdi:timer-outline";
                break;
            case 2:
                component = "sensor";
                snprintf(obj, objLen, "zone%u_remaining", n);
                snprintf(name, sizeof(name), "%s remaining", zc.name);
                common(d, obj, name);
                snprintf(t, sizeof(t), "~/zone/%u/remaining", n);
                d["stat_t"] = t;
                d["dev_cla"] = "duration";
                d["unit_of_meas"] = "s";
                d["stat_cla"] = "measurement";
                break;
            default: {
                const bool week = i % ZONE_ITEMS == 3;
                component = "sensor";
                snprintf(obj, objLen, "zone%u_%s", n, week ? "week" : "season");
                snprintf(name, sizeof(name), "%s this %s", zc.name, week ? "week" : "season");
                common(d, obj, name);
                snprintf(tpl, sizeof(tpl), "{{ value_json.zones[%u].%s_s }}", z, week ? "week" : "season");
                stateSensor(d, tpl);
                d["dev_cla"] = "duration";
                d["unit_of_meas"] = "s";
                d["stat_cla"] = "total_increasing";
                d["ic"] = "mdi:water-outline";
            }
        }
        return true;
    }

    i -= ZONE_COUNT * ZONE_ITEMS;
    if (i < GLOBAL_ITEMS) {
        switch (i) {
            case 0: {
                component = "select";
                strlcpy(obj, "mode", objLen);
                common(d, obj, "Mode");
                d["cmd_t"] = "~/mode/set";
                stateSensor(d, "{{ value_json.mode }}");
                JsonArray ops = d["ops"].to<JsonArray>();
                for (const char* m : {"auto", "manual", "winter", "test"}) ops.add(m);
                d["ic"] = "mdi:cog-outline";
                break;
            }
            case 1: case 2: case 3: {
                static const char* const keys[] = {"factor", "factor_lawn", "factor_border"};
                static const char* const names[] = {"Season factor", "Lawn factor", "Border factor"};
                const char* key = keys[i - 1];
                component = "number";
                strlcpy(obj, i == 1 ? "season_factor" : key, objLen);
                common(d, obj, names[i - 1]);
                snprintf(t, sizeof(t), "~/%s/set", key);
                d["cmd_t"] = t;
                snprintf(tpl, sizeof(tpl), "{{ value_json.%s }}", key);
                stateSensor(d, tpl);
                d["min"] = 0;
                d["max"] = 200;
                d["step"] = 5;
                d["unit_of_meas"] = "%";
                d["ic"] = "mdi:percent";
                if (i > 1) d["ent_cat"] = "config";
                break;
            }
            case 4:
                component = "button";
                strlcpy(obj, "stop", objLen);
                common(d, obj, "Emergency stop");
                cmdButton(d, "stop", "mdi:alert-octagon");
                break;
            case 5:
                component = "button";
                strlcpy(obj, "test", objLen);
                common(d, obj, "Test cycle");
                cmdButton(d, "test", "mdi:playlist-play");
                break;
            case 6:
                component = "button";
                strlcpy(obj, "skip_next", objLen);
                common(d, obj, "Skip next run");
                cmdButton(d, "skip_next", "mdi:skip-next");
                break;
            case 7:
                component = "button";
                strlcpy(obj, "clear_skip", objLen);
                common(d, obj, "Clear rain delay");
                cmdButton(d, "clear_skip", "mdi:weather-sunny");
                break;
            case 8:
                component = "binary_sensor";
                strlcpy(obj, "skip", objLen);
                common(d, obj, "Rain delay");
                stateSensor(d, "{{ 'ON' if value_json.skip.active or value_json.skip.next else 'OFF' }}");
                d["json_attr_t"] = "~/state";
                d["json_attr_tpl"] = "{{ value_json.skip | tojson }}";
                d["ic"] = "mdi:weather-pouring";
                break;
            case 9:
                component = "sensor";
                strlcpy(obj, "program", objLen);
                common(d, obj, "Active program");
                stateSensor(d, "{{ value_json.runs[0].name if value_json.runs else 'none' }}");
                d["json_attr_t"] = "~/state";
                d["json_attr_tpl"] = "{{ {'runs': value_json.runs} | tojson }}";
                d["ic"] = "mdi:playlist-check";
                break;
            case 10:
                component = "sensor";
                strlcpy(obj, "queue", objLen);
                common(d, obj, "Queue");
                stateSensor(d, "{{ value_json.queue | length }}");
                d["json_attr_t"] = "~/state";
                d["json_attr_tpl"] = "{{ {'queue': value_json.queue} | tojson }}";
                d["ic"] = "mdi:format-list-numbered";
                break;
            case 11:
                component = "sensor";
                strlcpy(obj, "next_run", objLen);
                common(d, obj, "Next run");
                stateSensor(d, "{{ (value_json.next_run.name ~ ' - ' ~ value_json.next_run.at) if value_json.next_run else 'none' }}");
                d["ic"] = "mdi:calendar-clock";
                break;
            case 12:
                component = "sensor";
                strlcpy(obj, "rssi", objLen);
                common(d, obj, "WiFi signal");
                stateSensor(d, "{{ value_json.rssi }}");
                d["dev_cla"] = "signal_strength";
                d["unit_of_meas"] = "dBm";
                d["stat_cla"] = "measurement";
                d["ent_cat"] = "diagnostic";
                break;
            default:
                component = "sensor";
                strlcpy(obj, "uptime", objLen);
                common(d, obj, "Uptime");
                stateSensor(d, "{{ value_json.uptime_s }}");
                d["dev_cla"] = "duration";
                d["unit_of_meas"] = "s";
                d["ent_cat"] = "diagnostic";
        }
        return true;
    }

    // Program buttons; unused slots are removed from HA.
    const uint8_t id = i - GLOBAL_ITEMS + 1;
    component = "button";
    snprintf(obj, objLen, "program%u", id);
    const char* pname = programs::name(id);
    if (!pname) return false;
    snprintf(name, sizeof(name), "Run %s", pname);
    common(d, obj, name);
    snprintf(tpl, sizeof(tpl), "{\"action\":\"run_program\",\"program\":%u}", id);
    d["cmd_t"] = "~/cmd";
    d["pl_prs"] = tpl;
    d["ic"] = "mdi:play-circle-outline";
    return true;
}

static bool publishDiscovery(uint16_t i) {
    JsonDocument d;
    const char* component = "sensor";
    char obj[24], topic[96];
    bool keep = buildItem(i, d, component, obj, sizeof(obj));
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", cfg.prefix, component, devId, obj);
    if (!keep) return pub(topic, "", true);
    // Stable entity ids (valve.sqwrtr_zone1, select.sqwrtr_mode, ...) so the HA package and
    // dashboard work for everyone. A second controller gets HA's "_2" suffix.
    char entityId[48];
    snprintf(entityId, sizeof(entityId), "%s.sqwrtr_%s", component, obj);
    d["def_ent_id"] = entityId;
    if (measureJson(d) >= sizeof(out)) {
        LOG("MQTT: discovery %s too large", obj);
        return true;  // skip it
    }
    serializeJson(d, out, sizeof(out));
    return pub(topic, out, true);
}

// Changes in names/limits/programs change the discovery payloads → republish.
static uint32_t signature() {
    uint32_t h = 2166136261u;
    auto mix = [&h](const char* s) {
        while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
        h = (h ^ 0xFF) * 16777619u;
    };
    char num[12];
    for (uint8_t z = 0; z < ZONE_COUNT; z++) {
        mix(zones::config(z).name);
        snprintf(num, sizeof(num), "%lu", (unsigned long)zones::config(z).maxRuntimeS / 60);
        mix(num);
    }
    for (uint8_t id = 1; id <= MAX_PROGRAMS; id++) {
        const char* n = programs::name(id);
        mix(n ? n : "");
    }
    return h;
}

// ---------- state ----------

static const char* zoneStateName(zones::State s) {
    switch (s) {
        case zones::State::Open: return "open";
        case zones::State::Pending: return "opening";
        default: return "closed";
    }
}

static void forceRepublish() {
    memset(pubZoneState, 0xFF, sizeof(pubZoneState));
    memset(pubRemaining, 0xFF, sizeof(pubRemaining));
    memset(pubDuration, 0xFF, sizeof(pubDuration));
    stateDirty = true;
}

static void publishZones() {
    char topic[72], val[12];
    const bool remainingDue = millis() - lastRemainingPub >= REMAINING_EVERY_MS;
    if (remainingDue) lastRemainingPub = millis();
    for (uint8_t z = 0; z < ZONE_COUNT; z++) {
        const uint8_t n = z + 1;
        const uint8_t st = (uint8_t)zones::state(z);
        const bool changed = st != pubZoneState[z];
        if (changed) {
            snprintf(topic, sizeof(topic), "%s/zone/%u/state", base, n);
            if (pub(topic, zoneStateName(zones::state(z)), true)) pubZoneState[z] = st;
        }
        const uint32_t rem = zones::remainingS(z);
        // Every 10 s while running, immediately on open/close.
        if (rem != pubRemaining[z] && (changed || remainingDue || rem == 0 || pubRemaining[z] == UINT32_MAX)) {
            snprintf(topic, sizeof(topic), "%s/zone/%u/remaining", base, n);
            snprintf(val, sizeof(val), "%lu", (unsigned long)rem);
            if (pub(topic, val, true)) pubRemaining[z] = rem;
        }
        const uint32_t dur = (zones::config(z).defaultDurationS + 59) / 60;
        if (dur != pubDuration[z]) {
            snprintf(topic, sizeof(topic), "%s/zone/%u/duration", base, n);
            snprintf(val, sizeof(val), "%lu", (unsigned long)dur);
            if (pub(topic, val, true)) pubDuration[z] = dur;
        }
    }
}

static void publishState() {
    JsonDocument d;
    programs::statusJson(d.to<JsonObject>());
    d.remove("time");
    d["active"] = zones::activeCount();
    d["rssi"] = WiFi.RSSI();
    d["uptime_s"] = millis() / 1000;
    d["heap"] = ESP.getFreeHeap();
    JsonArray zs = d["zones"].to<JsonArray>();
    for (uint8_t z = 0; z < ZONE_COUNT; z++) history::runtimeJson(zs.add<JsonObject>(), z);
    if (measureJson(d) >= sizeof(out)) {
        LOG("MQTT: state too large");
        return;
    }
    serializeJson(d, out, sizeof(out));
    char topic[56];
    topicOf(topic, sizeof(topic), "state");
    if (pub(topic, out, true)) {
        stateDirty = false;
        lastStatePub = millis();
    }
}

// ---------- commands ----------

static bool isAny(const char* s, const char* a, const char* b) { return !strcasecmp(s, a) || !strcasecmp(s, b); }

static void setFactor(uint8_t& field, const char* payload) {
    char* end;
    long v = strtol(payload, &end, 10);
    if (end == payload || v < 0 || v > 200) {
        LOG("MQTT: invalid factor '%s' (0-200)", payload);
        return;
    }
    field = v;
    storage::markDirty();
    web::markStatusDirty();
}

static void openZone(uint8_t z, uint32_t secs) {
    if (web::updateInProgress()) {
        LOG("MQTT: zone %u not opened, update in progress", z + 1);
        return;
    }
    zones::open(z, secs, zones::Source::Mqtt);  // rejections are reported as events by zones
}

static void zoneCommand(uint8_t z, const char* sub, const char* payload) {
    if (!strcmp(sub, "set")) {
        if (isAny(payload, "OPEN", "ON")) return openZone(z, zones::config(z).defaultDurationS);
        if (isAny(payload, "CLOSE", "OFF")) {
            zones::close(z, zones::Source::Mqtt);
            return;
        }
        if (payload[0] == '{') {  // {"duration_s": 300}
            JsonDocument d;
            if (!deserializeJson(d, payload) && d["duration_s"].is<uint32_t>()) return openZone(z, d["duration_s"]);
        } else if (isDigit(payload[0])) {
            return openZone(z, strtoul(payload, nullptr, 10));  // seconds; 0 = indefinite (max runtime)
        }
        LOG("MQTT: invalid zone command '%s'", payload);
    } else if (!strcmp(sub, "duration/set")) {
        const float minutes = atof(payload);
        zones::Config& c = zones::config(z);
        const uint32_t secs = (uint32_t)(minutes * 60 + 0.5f);
        if (secs < 1 || secs > c.maxRuntimeS) {
            LOG("MQTT: zone %u duration '%s' out of range", z + 1, payload);
            pubDuration[z] = UINT32_MAX;  // let HA snap back to the real value
            return;
        }
        c.defaultDurationS = secs;
        storage::markDirty();
        web::markStatusDirty();
    }
}

static void setMode(const char* m) {
    if (!strcmp(m, "test")) {
        programs::runNow(0, zones::Source::Mqtt);
        return;
    }
    programs::Mode mode;
    if (programs::parseMode(m, mode)) programs::setMode(mode, zones::Source::Mqtt);
    else LOG("MQTT: invalid mode '%s'", m);
    stateDirty = true;  // HA select snaps back when the value was invalid
}

static void cmdCommand(const char* payload) {
    JsonDocument d;
    if (deserializeJson(d, payload) || !d.is<JsonObject>()) {
        LOG("MQTT: cmd is not a JSON object");
        return;
    }
    const char* action = d["action"] | "";
    const char* reason = d["reason"] | "";
    if (!strcmp(action, "stop")) {
        zones::emergencyStop(zones::Source::Mqtt);
    } else if (!strcmp(action, "test")) {
        programs::runNow(0, zones::Source::Mqtt);
    } else if (!strcmp(action, "run_program")) {
        uint8_t id = d["program"] | 0;
        if (!id) LOG("MQTT: run_program needs \"program\"");
        else programs::runNow(id, zones::Source::Mqtt);
    } else if (!strcmp(action, "skip")) {
        if (d["next"] | false) {
            programs::setSkipNext(reason, zones::Source::Mqtt);
        } else {
            int hours = d["hours"] | -1;
            if (hours < 0 || hours > 24 * 14) LOG("MQTT: skip needs \"hours\" 0-336 or \"next\": true");
            else if (!programs::setSkipHours(hours, reason, zones::Source::Mqtt)) LOG("MQTT: skip refused, clock not set");
        }
    } else if (!strcmp(action, "skip_next")) {
        programs::setSkipNext(reason[0] ? reason : "Home Assistant", zones::Source::Mqtt);
    } else if (!strcmp(action, "clear_skip")) {
        programs::setSkipHours(0, "", zones::Source::Mqtt);
    } else if (!strcmp(action, "mode")) {
        setMode(d["mode"] | "");
    } else if (!strcmp(action, "open")) {
        uint8_t n = d["zone"] | 0;
        if (n < 1 || n > ZONE_COUNT) LOG("MQTT: open needs \"zone\" 1-%u", ZONE_COUNT);
        else openZone(n - 1, d["duration_s"] | zones::config(n - 1).defaultDurationS);
    } else if (!strcmp(action, "close")) {
        uint8_t n = d["zone"] | 0;
        if (n < 1 || n > ZONE_COUNT) LOG("MQTT: close needs \"zone\" 1-%u", ZONE_COUNT);
        else zones::close(n - 1, zones::Source::Mqtt);
    } else {
        LOG("MQTT: unknown action '%s'", action);
    }
}

static void handle(const char* topic, const char* payload) {
    if (!strcmp(topic, haStatusTopic)) {
        if (!strcmp(payload, "online")) {  // HA restarted: send everything again
            discoIdx = 0;
            forceRepublish();
        }
        return;
    }
    const size_t bl = strlen(base);
    if (strncmp(topic, base, bl) || topic[bl] != '/') return;
    const char* sub = topic + bl + 1;

    if (!strncmp(sub, "zone/", 5)) {
        char* end;
        long n = strtol(sub + 5, &end, 10);
        if (n < 1 || n > ZONE_COUNT || *end != '/') return;
        zoneCommand(n - 1, end + 1, payload);
    } else if (!strcmp(sub, "mode/set")) {
        setMode(payload);
    } else if (!strcmp(sub, "factor/set")) {
        setFactor(programs::settings().seasonFactor, payload);
    } else if (!strcmp(sub, "factor_lawn/set")) {
        setFactor(programs::settings().factorLawn, payload);
    } else if (!strcmp(sub, "factor_border/set")) {
        setFactor(programs::settings().factorBorder, payload);
    } else if (!strcmp(sub, "cmd")) {
        cmdCommand(payload);
    }
}

// ---------- connection ----------

static void setup() {
    if (cfg.deviceId[0]) strlcpy(devId, cfg.deviceId, sizeof(devId));
    else snprintf(devId, sizeof(devId), "sqwrtr-%06x", ESP.getChipId());
    snprintf(base, sizeof(base), "sqwrtr/%s", devId);
    snprintf(statusTopic, sizeof(statusTopic), "%s/status", base);
    snprintf(haStatusTopic, sizeof(haStatusTopic), "%s/status", cfg.prefix);

    client.setServer(cfg.host, cfg.port);
    client.setCredentials(cfg.user[0] ? cfg.user : nullptr, cfg.password[0] ? cfg.password : nullptr);
    client.setClientId(devId);
    client.setWill(statusTopic, 1, true, "offline");
    client.setKeepAlive(30);
    client.setCleanSession(true);
}

static void onConnected() {
    online = true;
    backoffMs = BACKOFF_MIN_MS;
    lastErr[0] = '\0';
    LOG("MQTT: connected to %s:%u as %s", cfg.host, cfg.port, devId);
    client.publish(statusTopic, 1, true, "online");

    char t[72];
    static const char* const subs[] = {"zone/+/set", "zone/+/duration/set", "mode/set", "factor/set",
                                       "factor_lawn/set", "factor_border/set", "cmd"};
    for (const char* s : subs) {
        topicOf(t, sizeof(t), s);
        client.subscribe(t, 0);
    }
    client.subscribe(haStatusTopic, 0);

    discoIdx = 0;
    discoSig = signature();
    forceRepublish();
    web::markStatusDirty();
}

static void onDisconnected(uint8_t reason) {
    const bool was = online;
    online = false;
    discoIdx = -1;
    strlcpy(lastErr, espMqttClientTypes::disconnectReasonToString((espMqttClientTypes::DisconnectReason)reason),
            sizeof(lastErr));
    if (was) LOG("MQTT: connection lost (%s)", lastErr);
    else LOG("MQTT: connect to %s:%u failed (%s), retry in %lu s", cfg.host, cfg.port, lastErr,
             (unsigned long)backoffMs / 1000);
    nextAttempt = millis() + backoffMs;
    backoffMs = min(backoffMs * 2, BACKOFF_MAX_MS);
    web::markStatusDirty();
}

void begin() {
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.host, MQTT_HOST, sizeof(cfg.host));
    cfg.port = MQTT_PORT;
    strlcpy(cfg.user, MQTT_USER, sizeof(cfg.user));
    strlcpy(cfg.password, MQTT_PASSWORD, sizeof(cfg.password));
    strlcpy(cfg.prefix, "homeassistant", sizeof(cfg.prefix));
    cfg.enabled = cfg.host[0] != '\0';
    backoffMs = BACKOFF_MIN_MS;

    client.onConnect([](bool) { onlineEvent = true; });
    client.onDisconnect([](espMqttClientTypes::DisconnectReason r) {
        offlineReason = (uint8_t)r;
        offlineEvent = true;
    });
    client.onMessage([](const espMqttClientTypes::MessageProperties& props, const char* topic, const uint8_t* payload,
                        size_t len, size_t index, size_t total) {
        // Network context: only queue. Ignore retained commands (they would replay on every connect),
        // fragments and oversized messages.
        if (props.retain && strcmp(topic, haStatusTopic)) return;
        if (index != 0 || len != total || inCount >= INBOX) return;
        Msg& m = inbox[(inHead + inCount) % INBOX];
        if (strlen(topic) >= sizeof(m.topic) || len >= sizeof(m.payload)) return;
        strcpy(m.topic, topic);
        memcpy(m.payload, payload, len);
        m.payload[len] = '\0';
        inCount++;
    });
}

void loop() {
    if (reconfigure) {
        reconfigure = false;
        if (!client.disconnected()) client.disconnect(true);
        online = false;
        discoIdx = -1;
        setup();
        backoffMs = BACKOFF_MIN_MS;
        nextAttempt = millis();
        lastErr[0] = '\0';
        return;
    }

    if (offlineEvent) {
        offlineEvent = false;
        onlineEvent = false;
        onDisconnected(offlineReason);
    }
    if (onlineEvent) {
        onlineEvent = false;
        onConnected();
    }

    while (inCount) {
        Msg& m = inbox[inHead];
        handle(m.topic, m.payload);
        inHead = (inHead + 1) % INBOX;
        inCount--;
    }

    if (!cfg.enabled || !cfg.host[0]) {
        if (!client.disconnected()) client.disconnect();
        return;
    }
    if (!net::wifiConnected()) return;

    if (client.disconnected()) {
        if ((int32_t)(millis() - nextAttempt) >= 0) {
            nextAttempt = millis() + 15000;  // until the disconnect callback sets the backoff
            client.connect();
        }
        return;
    }
    if (!online) return;

    // Discovery: one item per 50 ms, only with enough free heap.
    static uint32_t lastSigCheck = 0;
    if (discoIdx < 0 && millis() - lastSigCheck >= 2000) {
        lastSigCheck = millis();
        uint32_t s = signature();
        if (s != discoSig) {
            discoSig = s;
            discoIdx = 0;
        }
    }
    if (discoIdx >= 0 && millis() - lastDisco >= 50 && ESP.getFreeHeap() > MIN_HEAP) {
        lastDisco = millis();
        if (publishDiscovery(discoIdx)) discoIdx++;
        if (discoIdx >= (int16_t)DISCO_ITEMS) {
            discoIdx = -1;
            LOG("MQTT: Home Assistant discovery sent (%u entities)", DISCO_ITEMS);
        }
        return;  // states after discovery, so HA has the entities first
    }

    publishZones();
    const uint32_t since = millis() - lastStatePub;
    if (ESP.getFreeHeap() > MIN_HEAP &&
        ((stateDirty && since >= STATE_MIN_MS) || since >= STATE_HEARTBEAT_MS)) {
        publishState();
    }
}

}  // namespace mqtt
