#include "storage.h"
#include <LittleFS.h>
#include <bearssl/bearssl_hash.h>
#include "config.h"
#include "secrets.h"
#include "log.h"

namespace storage {

static const char CONFIG_PATH[] = "/config.json";
static const char CONFIG_TMP[] = "/config.tmp";
static const uint32_t SAVE_DELAY_MS = 3000;
static const uint8_t CONFIG_VERSION = 1;

static bool mounted = false;
static bool dirty = false;
static uint32_t dirtyAt = 0;

// SHA-256 als hex; leeg = standaardwaarde uit secrets.h.
static char adminHash[65] = "";
static char apiKeyHash[65] = "";

// ---------- hashing ----------

static void sha256Hex(const char* value, char out[65]) {
    // Gezouten met het chip-id: dezelfde waarde geeft per apparaat een andere hash.
    char salt[16];
    snprintf(salt, sizeof(salt), "sqwrtr:%06x:", ESP.getChipId());
    uint8_t digest[32];
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, salt, strlen(salt));
    br_sha256_update(&ctx, value, strlen(value));
    br_sha256_out(&ctx, digest);
    for (uint8_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static bool hexEquals(const char* a, const char* b) {
    uint8_t diff = strlen(a) != strlen(b);
    for (size_t i = 0; a[i] && b[i]; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static bool checkAgainst(const char* value, const char* hash, const char* fallback) {
    if (!value) return false;
    char h[65];
    sha256Hex(value, h);
    if (hash[0]) return hexEquals(h, hash);
    char f[65];
    sha256Hex(fallback, f);  // ook de standaardwaarde via hash vergelijken (constante tijd)
    return hexEquals(h, f);
}

bool checkPassword(const char* password) { return checkAgainst(password, adminHash, ADMIN_PASSWORD); }
bool checkApiKey(const char* key) { return checkAgainst(key, apiKeyHash, API_KEY); }
bool passwordIsDefault() { return !adminHash[0]; }
bool apiKeyIsDefault() { return !apiKeyHash[0]; }

void setPassword(const char* password) {
    sha256Hex(password, adminHash);
    markDirty();
}

String generateApiKey() {
    char key[33];
    for (uint8_t i = 0; i < 32; i += 8) snprintf(key + i, 9, "%08x", (unsigned)ESP.random());
    sha256Hex(key, apiKeyHash);
    markDirty();
    return String(key);
}

// ---------- JSON <-> config ----------

static const char* typeName(zones::Type t) { return t == zones::Type::Border ? "border" : "gazon"; }

void zoneToJson(JsonObject o, const zones::Config& c) {
    o["name"] = c.name;
    o["type"] = typeName(c.type);
    o["enabled"] = c.enabled;
    o["default_duration_s"] = c.defaultDurationS;
    o["max_runtime_s"] = c.maxRuntimeS;
}

bool zoneFromJson(JsonObjectConst in, zones::Config& out, const char** err) {
    zones::Config next = out;
    if (!in["name"].isNull()) {
        const char* n = in["name"] | "";
        if (!n[0] || strlen(n) >= sizeof(next.name)) return *err = "name: 1-23 tekens", false;
        strlcpy(next.name, n, sizeof(next.name));
    }
    if (!in["type"].isNull()) {
        const char* t = in["type"] | "";
        if (!strcmp(t, "gazon")) next.type = zones::Type::Gazon;
        else if (!strcmp(t, "border")) next.type = zones::Type::Border;
        else return *err = "type: gazon of border", false;
    }
    if (!in["enabled"].isNull()) {
        if (!in["enabled"].is<bool>()) return *err = "enabled: true of false", false;
        next.enabled = in["enabled"];
    }
    if (!in["max_runtime_s"].isNull()) {
        uint32_t v = in["max_runtime_s"] | 0u;
        if (v < 1 || v > 4 * 3600) return *err = "max_runtime_s: 1-14400", false;
        next.maxRuntimeS = v;
    }
    if (!in["default_duration_s"].isNull()) {
        uint32_t v = in["default_duration_s"] | 0u;
        if (v < 1) return *err = "default_duration_s: minimaal 1", false;
        next.defaultDurationS = v;
    }
    if (next.defaultDurationS > next.maxRuntimeS) {
        return *err = "Standaardduur mag niet langer zijn dan de max looptijd", false;
    }
    out = next;
    return true;
}

bool settingsFromJson(JsonObjectConst in, zones::Settings& out, const char** err) {
    zones::Settings next = out;
    if (!in["max_concurrent"].isNull()) {
        uint32_t v = in["max_concurrent"] | 0u;
        if (v < 1 || v > ZONE_COUNT) return *err = "max_concurrent: 1-5", false;
        next.maxConcurrent = v;
    }
    if (!in["stagger_ms"].isNull()) {
        uint32_t v = in["stagger_ms"] | 0xFFFFFFFFu;
        if (v > 10000) return *err = "stagger_ms: 0-10000", false;
        next.staggerMs = v;
    }
    out = next;
    return true;
}

void exportBackup(JsonDocument& doc) {
    doc["version"] = CONFIG_VERSION;
    JsonObject s = doc["settings"].to<JsonObject>();
    s["max_concurrent"] = zones::settings().maxConcurrent;
    s["stagger_ms"] = zones::settings().staggerMs;
    JsonArray arr = doc["zones"].to<JsonArray>();
    for (uint8_t i = 0; i < ZONE_COUNT; i++) zoneToJson(arr.add<JsonObject>(), zones::config(i));
}

// Valideert alles eerst; past pas toe als het geheel geldig is.
static bool applyConfig(JsonObjectConst in, const char** err) {
    zones::Settings settings = zones::settings();
    if (in["settings"].is<JsonObjectConst>() && !settingsFromJson(in["settings"], settings, err)) return false;

    zones::Config cfgs[ZONE_COUNT];
    for (uint8_t i = 0; i < ZONE_COUNT; i++) cfgs[i] = zones::config(i);
    JsonArrayConst arr = in["zones"];
    for (uint8_t i = 0; i < ZONE_COUNT && i < arr.size(); i++) {
        if (!arr[i].is<JsonObjectConst>() || !zoneFromJson(arr[i], cfgs[i], err)) return false;
    }

    zones::settings() = settings;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) zones::config(i) = cfgs[i];
    return true;
}

bool importBackup(JsonObjectConst in, const char** err) {
    if (!applyConfig(in, err)) return false;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        if (!zones::config(i).enabled) zones::close(i, zones::Source::Web, "uitgeschakeld");
    }
    markDirty();
    return true;
}

// ---------- bestand ----------

static void save() {
    if (!mounted) return;
    JsonDocument doc;
    exportBackup(doc);
    JsonObject a = doc["auth"].to<JsonObject>();
    a["admin_hash"] = adminHash;
    a["api_key_hash"] = apiKeyHash;

    // Eerst naar tijdelijk bestand, dan hernoemen: nooit een half bestand bij stroomuitval.
    File f = LittleFS.open(CONFIG_TMP, "w");
    if (!f) {
        LOG("Opslag: kan %s niet openen", CONFIG_TMP);
        return;
    }
    size_t n = serializeJson(doc, f);
    f.close();
    LittleFS.remove(CONFIG_PATH);
    LittleFS.rename(CONFIG_TMP, CONFIG_PATH);
    LOG("Opslag: config opgeslagen (%u bytes)", (unsigned)n);
}

static void load() {
    // Stroomuitval tussen remove en rename in save(): dan staat alleen het tijdelijke bestand er.
    const char* path = LittleFS.exists(CONFIG_PATH) ? CONFIG_PATH : CONFIG_TMP;
    File f = LittleFS.open(path, "r");
    if (!f) {
        LOG("Opslag: geen config, standaardwaarden");
        return;
    }
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e || !doc.is<JsonObject>()) {
        LOG("Opslag: config onleesbaar (%s), standaardwaarden", e.c_str());
        return;
    }
    const char* err = nullptr;
    if (!applyConfig(doc.as<JsonObjectConst>(), &err)) {
        LOG("Opslag: config ongeldig (%s), standaardwaarden", err);
        return;
    }
    strlcpy(adminHash, doc["auth"]["admin_hash"] | "", sizeof(adminHash));
    strlcpy(apiKeyHash, doc["auth"]["api_key_hash"] | "", sizeof(apiKeyHash));
    LOG("Opslag: config geladen");
}

void begin() {
    mounted = LittleFS.begin();
    if (!mounted) {
        LOG("Opslag: LittleFS mount mislukt, formatteren...");
        mounted = LittleFS.format() && LittleFS.begin();
    }
    if (!mounted) {
        LOG("Opslag: LittleFS niet beschikbaar – wijzigingen worden niet bewaard");
        return;
    }
    load();
}

void markDirty() {
    dirty = true;
    dirtyAt = millis();
}

void loop() {
    if (dirty && millis() - dirtyAt >= SAVE_DELAY_MS) {
        dirty = false;
        save();
    }
}

}  // namespace storage
