#include "mp60d.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/utsname.h>
#include <unistd.h>

const char *config_path = MP60_CONFIG_PATH;
static volatile sig_atomic_t stopping;

static void stop_handler(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

uint8_t app_actual_mask(const App *app)
{
    return wire_to_mask(app->metrics.reported_wire_mask);
}

const char *app_power_state(const App *app, int outlet)
{
    if (!app->metrics.valid)
        return "UNKNOWN";
    return app_actual_mask(app) & (1u << (outlet - 1)) ? "ON" : "OFF";
}

int app_set_outlet(App *app, int outlet, int action, char *error, size_t error_size)
{
    uint8_t old_mask = app->config.outlet_mask;
    uint8_t new_mask = old_mask;
    if (outlet < 0 || outlet > MP60_OUTLETS || action < 0 || action > 2) {
        snprintf(error, error_size, "Invalid outlet or state");
        return -1;
    }
    if (outlet == 0) {
        if (action == 2) {
            snprintf(error, error_size, "Toggle is not valid for all outlets");
            return -1;
        }
        new_mask = action ? 0x3f : 0;
    } else if (action == 2) {
        new_mask = (app->metrics.valid ? app_actual_mask(app) : old_mask) ^
            (uint8_t)(1u << (outlet - 1));
    } else if (action) {
        new_mask |= (uint8_t)(1u << (outlet - 1));
    } else {
        new_mask &= (uint8_t)~(1u << (outlet - 1));
    }
    /* Explicit commands are always sent, even when equal to the saved desired
       mask: this also repairs a relay that drifted from the requested state. */
    {
        if (app->simulate) {
            app->config.outlet_mask = new_mask;
            hardware_simulate_reading(app, &app->metrics);
        } else {
            Metrics confirmed;
            int attempt, write_attempt, read_ok = 0, matches = 0;
            if (hardware_set_mask(app, new_mask)) {
                app->last_poll_ok = 0;
                app->hardware_errors++;
                snprintf(app->hardware_error, sizeof(app->hardware_error), "I2C outlet write failed: %s", strerror(errno));
                snprintf(error, error_size, "Hardware write failed; requested state was not saved");
                return -1;
            }
            /* The controller accepted the write, so retain the complete requested
               mask even if its first response still contains the old relay state.
               Every valid response replaces all six reported outlets at once. */
            app->config.outlet_mask = new_mask;
            for (write_attempt = 0; write_attempt < 2 && !matches; write_attempt++) {
                if (write_attempt && hardware_set_mask(app, new_mask) < 0)
                    break;
                for (attempt = 0; attempt < 3 && !matches; attempt++) {
                    if (hardware_read(app, &confirmed) == 0) {
                        read_ok = 1;
                        app->metrics = confirmed;
                        app->hardware_stale = 0;
                        app->last_poll_ok = 1;
                        matches = app_actual_mask(app) == new_mask;
                    }
                    if (!matches && attempt < 2) {
                        struct timespec delay = {0, 100000000};
                        nanosleep(&delay, NULL);
                    }
                }
            }
            if (matches) {
                app->hardware_error[0] = 0;
            } else if (read_ok) {
                /* Delayed confirmation is a pending state, not a failed API
                   command. The normal poll loop will finish the transition. */
                snprintf(app->hardware_error, sizeof(app->hardware_error),
                    "Waiting for outlet confirmation: requested 0x%02x, reported 0x%02x",
                    (unsigned)new_mask, (unsigned)app_actual_mask(app));
            } else {
                app->hardware_errors++;
                app->last_poll_ok = 0;
                app->hardware_stale = !app->metrics.valid || time(NULL) - app->metrics.sampled_at > 15;
                snprintf(app->hardware_error, sizeof(app->hardware_error),
                    "I2C outlet confirmation read failed: %s", strerror(errno));
            }
        }
        app->revision++;
        if (config_save(&app->config, config_path))
            log_msg("warning", "Outlet changed but state persistence failed: %s", strerror(errno));
        mqtt_state_changed(app, 1);
    }
    return 0;
}

void app_poll_hardware(App *app)
{
    Metrics next;
    uint8_t old_reported = app_actual_mask(app);
    int old_valid = app->metrics.valid;
    if (hardware_read(app, &next) == 0) {
        app->metrics = next;
        app->hardware_stale = 0;
        app->last_poll_ok = 1;
        if (app_actual_mask(app) == app->config.outlet_mask)
            app->hardware_error[0] = 0;
        if (!old_valid || old_reported != app_actual_mask(app)) {
            app->revision++;
            mqtt_state_changed(app, 0);
        }
    } else {
        time_t now = time(NULL);
        app->last_poll_ok = 0;
        app->hardware_stale = !app->metrics.valid || now - app->metrics.sampled_at > 15;
        app->hardware_errors++;
        snprintf(app->hardware_error, sizeof(app->hardware_error), "I2C meter read failed: %s", strerror(errno));
    }
}

int app_status_json(App *app, char *output, size_t size)
{
    int i, used, stale;
    long age;
    time_t now = time(NULL);
    double total_current = 0, total_active = 0, total_apparent = 0, total_reactive = 0;
    struct utsname system_info;
    const char *kernel = "unknown";
    if (uname(&system_info) == 0)
        kernel = system_info.release;
    age = app->metrics.valid ? (long)(now - app->metrics.sampled_at) : -1;
    if (age < 0 && app->metrics.valid)
        age = 0;
    stale = !app->metrics.valid || age > 15;
    used = snprintf(output, size,
        "{\"version\":\"%s\",\"firmware\":\"%s\",\"kernel\":\"%s\","
        "\"backend\":\"%s\",\"uptime\":%ld,\"revision\":%lu,"
        "\"hardware\":{\"stale\":%s,\"last_poll_ok\":%s,\"age_s\":%ld,\"errors\":%lu,\"last_update\":%ld,\"error\":\"",
        MP60_VERSION, MP60_FIRMWARE_NAME, kernel, app->simulate ? "simulator" : "i2c",
        (long)(now - app->started_at), app->revision, stale ? "true" : "false",
        app->last_poll_ok ? "true" : "false", age,
        app->hardware_errors, (long)app->metrics.sampled_at);
    if (used < 0 || (size_t)used >= size) return -1;
    if (json_append(output, size, app->hardware_error)) return -1;
    if (snprintf(output + strlen(output), size - strlen(output),
        "\",\"voltage_v\":%u,\"frequency_hz\":%u,\"surge_depleted\":%s,"
        "\"desired_mask\":%u,\"reported_mask\":%u},\"mqtt\":{\"enabled\":%s,\"connected\":%s},"
        "\"http_enabled\":%s,\"outlets\":[",
        (unsigned)app->metrics.voltage, (unsigned)app->metrics.frequency,
        app->metrics.surge_depleted ? "true" : "false", (unsigned)app->config.outlet_mask,
        (unsigned)wire_to_mask(app->metrics.reported_wire_mask),
        app->config.mqtt_enabled ? "true" : "false", app->mqtt.connected ? "true" : "false",
        app->config.http_enabled ? "true" : "false") < 0) return -1;
    for (i = 0; i < MP60_OUTLETS; i++) {
        double current = app->metrics.current_x10[i] / 10.0;
        double apparent = app->metrics.apparent_x10[i] / 10.0;
        double active = app->metrics.active_x10[i] / 10.0;
        double reactive = app->metrics.reactive_x10[i] / 10.0;
        double factor = apparent > 0 ? active / apparent : 0;
        total_current += current; total_active += active; total_apparent += apparent; total_reactive += reactive;
        if (snprintf(output + strlen(output), size - strlen(output),
            "%s{\"id\":%d,\"state\":\"%s\",\"reported_state\":\"%s\",\"desired_state\":\"%s\","
            "\"confirmed\":%s,\"current_a\":%.1f,\"voltage_v\":%u,\"apparent_va\":%.1f,"
            "\"active_w\":%.1f,\"reactive_var\":%.1f,\"power_factor\":%.3f}",
            i ? "," : "", i + 1, app_power_state(app, i + 1), app_power_state(app, i + 1),
            (app->config.outlet_mask & (1u << i)) ? "ON" : "OFF",
            app->metrics.valid && ((app_actual_mask(app) ^ app->config.outlet_mask) & (1u << i)) == 0 ? "true" : "false",
            current, (unsigned)app->metrics.voltage, apparent, active, reactive, factor) < 0) return -1;
    }
    if (snprintf(output + strlen(output), size - strlen(output),
        "],\"total\":{\"current_a\":%.1f,\"apparent_va\":%.1f,\"active_w\":%.1f,"
        "\"reactive_var\":%.1f,\"power_factor\":%.3f}}",
        total_current, total_apparent, total_active, total_reactive,
        total_apparent > 0 ? total_active / total_apparent : 0) < 0) return -1;
    return strlen(output) < size ? 0 : -1;
}

int app_config_json(App *app, char *output, size_t size)
{
    const Config *c = &app->config;
    int n = snprintf(output, size,
        "{\"http_enabled\":%s,\"http_port\":%u,\"mqtt_enabled\":%s,\"ha_discovery\":%s,\"mqtt_host\":\"",
        c->http_enabled ? "true" : "false", (unsigned)c->http_port,
        c->mqtt_enabled ? "true" : "false", c->ha_discovery ? "true" : "false");
    if (n < 0 || (size_t)n >= size || json_append(output, size, c->mqtt_host)) return -1;
    if (snprintf(output + strlen(output), size - strlen(output),
        "\",\"mqtt_port\":%u,\"mqtt_username\":\"", (unsigned)c->mqtt_port) < 0 ||
        json_append(output, size, c->mqtt_username)) return -1;
    if (snprintf(output + strlen(output), size - strlen(output),
        "\",\"mqtt_password_set\":%s,\"mqtt_topic\":\"%s\",\"mqtt_client_id\":\"%s\","
        "\"mqtt_interval\":%u,\"ui_password_set\":%s}",
        c->mqtt_password[0] ? "true" : "false", c->mqtt_topic, c->mqtt_client_id,
        (unsigned)c->mqtt_interval, c->ui_password_set ? "true" : "false") < 0) return -1;
    return strlen(output) < size ? 0 : -1;
}

static int json_number(const char *json, const char *key, long minimum, long maximum, long *result)
{
    char type, value[64];
    char *end;
    int found = json_field(json, key, &type, value, sizeof(value));
    long number;
    if (found <= 0) return found;
    if (type != 'n') return -1;
    number = strtol(value, &end, 10);
    if (*value && !*end && number >= minimum && number <= maximum) {
        *result = number;
        return 1;
    }
    return -1;
}

static int json_boolean(const char *json, const char *key, int *result)
{
    char type, value[16];
    int found = json_field(json, key, &type, value, sizeof(value));
    if (found <= 0) return found;
    if (type != 'b') return -1;
    *result = !strcmp(value, "true");
    return 1;
}

static int json_string(const char *json, const char *key, char *result, size_t size)
{
    char type;
    int found = json_field(json, key, &type, result, size);
    if (found <= 0) return found;
    return type == 's' ? 1 : -1;
}

int app_update_config(App *app, const char *json, char *error, size_t error_size)
{
    Config next = app->config;
    long number;
    int boolean;
    char value[160];
    int result, mqtt_changed;
    if ((result = json_boolean(json, "http_enabled", &boolean)) < 0) goto invalid;
    if (result > 0) next.http_enabled = boolean;
    if ((result = json_number(json, "http_port", 1, 65535, &number)) < 0) goto invalid;
    if (result > 0) next.http_port = (uint16_t)number;
    if ((result = json_boolean(json, "mqtt_enabled", &boolean)) < 0) goto invalid;
    if (result > 0) next.mqtt_enabled = boolean;
    if ((result = json_boolean(json, "ha_discovery", &boolean)) < 0) goto invalid;
    if (result > 0) next.ha_discovery = boolean;
    if ((result = json_string(json, "mqtt_host", value, sizeof(value))) < 0) goto invalid;
    if (result > 0) {
        size_t i;
        if (!*value || strlen(value) >= sizeof(next.mqtt_host)) goto invalid;
        for (i = 0; value[i]; i++) if ((unsigned char)value[i] < 33 || value[i] == '=') goto invalid;
        strcpy(next.mqtt_host, value);
    }
    if ((result = json_number(json, "mqtt_port", 1, 65535, &number)) < 0) goto invalid;
    if (result > 0) next.mqtt_port = (uint16_t)number;
    if ((result = json_string(json, "mqtt_username", value, sizeof(value))) < 0) goto invalid;
    if (result > 0) {
        if (strlen(value) >= sizeof(next.mqtt_username) || strchr(value, '\n') || strchr(value, '\r')) goto invalid;
        strcpy(next.mqtt_username, value);
    }
    if ((result = json_string(json, "mqtt_password", value, sizeof(value))) < 0) goto invalid;
    if (result > 0) {
        if (strlen(value) >= sizeof(next.mqtt_password) || strchr(value, '\n') || strchr(value, '\r')) goto invalid;
        strcpy(next.mqtt_password, value);
    }
    if ((result = json_string(json, "mqtt_topic", value, sizeof(value))) < 0) goto invalid;
    if (result > 0) {
        size_t i;
        if (!*value || strlen(value) >= sizeof(next.mqtt_topic)) goto invalid;
        for (i = 0; value[i]; i++)
            if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z') ||
                  (value[i] >= '0' && value[i] <= '9') || value[i] == '-' || value[i] == '_')) goto invalid;
        strcpy(next.mqtt_topic, value);
    }
    if ((result = json_number(json, "mqtt_interval", 5, 86400, &number)) < 0) goto invalid;
    if (result > 0) next.mqtt_interval = (uint16_t)number;
    if ((result = json_string(json, "ui_password", value, sizeof(value))) < 0) goto invalid;
    if (result > 0) {
        if (strlen(value) > 128 || strchr(value, '\n') || strchr(value, '\r')) goto invalid;
        config_set_ui_password(&next, value);
    }
    if (next.mqtt_enabled && !next.mqtt_host[0]) {
        snprintf(error, error_size, "MQTT host is required when MQTT is enabled");
        return -1;
    }
    mqtt_changed = next.mqtt_enabled != app->config.mqtt_enabled ||
        next.ha_discovery != app->config.ha_discovery ||
        next.mqtt_port != app->config.mqtt_port || next.mqtt_interval != app->config.mqtt_interval ||
        strcmp(next.mqtt_host, app->config.mqtt_host) || strcmp(next.mqtt_username, app->config.mqtt_username) ||
        strcmp(next.mqtt_password, app->config.mqtt_password) || strcmp(next.mqtt_topic, app->config.mqtt_topic);
    if (config_save(&next, config_path)) {
        snprintf(error, error_size, "Could not save configuration: %s", strerror(errno));
        return -1;
    }
    app->config = next;
    if (mqtt_changed) mqtt_reset(app);
    if (http_restart(app)) {
        snprintf(error, error_size, "Configuration saved, but HTTP listener could not be restarted");
        return -1;
    }
    snprintf(error, error_size, "Configuration saved");
    return 0;
invalid:
    snprintf(error, error_size, "Invalid or out-of-range configuration field");
    return -1;
}

static int self_test(void)
{
    static const uint8_t all_off[6] = {0x01,0x06,0x00,0x00,0x06,0x01};
    static const uint8_t all_on[6] = {0x01,0x06,0xfc,0x00,0x06,0xfd};
    static const char scan_sample[] =
        "BSS 00:11:22:33:44:55(on mp60scan)\n\tfreq: 2412\n"
        "\tsignal: -80.00 dBm\n\tSSID: Cafe\n\tWPA: \n"
        "BSS 00:11:22:33:44:56(on mp60scan)\n\tfreq: 2462\n"
        "\tsignal: -42.50 dBm\n\tSSID: Cafe\n\tRSN: \n"
        "BSS 00:11:22:33:44:57(on mp60scan)\n\tfreq: 2437\n"
        "\tsignal: -70.00 dBm\n\tSSID: A\"B\n"
        "BSS 00:11:22:33:44:58(on mp60scan)\n\tSSID:\n";
    char hash[65], type, value[64], scan_json[2048];
    uint8_t response[53] = {0};
    Metrics metrics;
    response[5] = 1;
    response[6] = 15;
    response[12] = 121;
    response[13] = 60;
    response[14] = 0x04; response[15] = 0xd2;
    response[26] = 0x04; response[27] = 0xce;
    response[38] = 0x00; response[39] = 0x14;
    response[50] = 1;
    response[51] = 0xfc;
    response[52] = mp60_xor(response, 52);
    if (hardware_parse_response(response, &metrics) || metrics.current_x10[0] != 15 ||
        metrics.voltage != 121 || metrics.frequency != 60 || metrics.apparent_x10[0] != 1234 ||
        metrics.active_x10[0] != 1230 || metrics.reactive_x10[0] != 20 ||
        !metrics.surge_depleted || wire_to_mask(metrics.reported_wire_mask) != 0x3f)
        return 6;
    response[52] ^= 1;
    if (hardware_parse_response(response, &metrics) == 0)
        return 7;
    if (mask_to_wire(0x20) != 0x04 || wire_to_mask(0x04) != 0x20 || mask_to_wire(0x3f) != 0xfc)
        return 1;
    if (mp60_xor(all_off, 5) != all_off[5] || mp60_xor(all_on, 5) != all_on[5])
        return 2;
    sha256_hex("abc", 3, hash);
    if (strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
        return 3;
    if (json_field("{\"mqtt_enabled\":true,\"mqtt_port\":1883,\"x\":\"a\\n\"}", "mqtt_port", &type, value, sizeof(value)) != 1 || strcmp(value, "1883"))
        return 4;
    if (json_field("{\"a\":1}", "a", &type, value, sizeof(value)) != 1 || type != 'n')
        return 5;
    if (wifi_scan_parse(scan_sample, scan_json, sizeof(scan_json)) ||
        !strstr(scan_json, "\"count\":2") || !strstr(scan_json, "\"signal_dbm\":-42.50") ||
        !strstr(scan_json, "A\\\"B") || strstr(scan_json, "\"count\":2,\"ssid\""))
        return 8;
    printf("mp60d self-test passed: outlet mapping, XOR, SHA-256, JSON, Wi-Fi scan\n");
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [--simulate] [--config PATH] [--no-http] [--self-test]\n", program);
}

int main(int argc, char **argv)
{
    App app;
    int i, no_http = 0;
    unsigned meter_failures = 0;
    long long next_meter = 0;
    memset(&app, 0, sizeof(app));
    app.http_fd = -1;
    app.mqtt.fd = -1;
    app.started_at = time(NULL);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--simulate")) app.simulate = 1;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--no-http")) no_http = 1;
        else if (!strcmp(argv[i], "--self-test")) return self_test();
        else { usage(argv[0]); return 2; }
    }
    openlog("mp60d", LOG_PID, LOG_DAEMON);
    if (config_load(&app.config, config_path)) {
        log_msg("error", "Cannot load %s: %s", config_path, strerror(errno));
        return 1;
    }
    if (no_http) app.config.http_enabled = 0;
    if (config_save(&app.config, config_path))
        log_msg("warning", "Initial configuration save failed: %s", strerror(errno));
    app.hardware_stale = 1;
    snprintf(app.hardware_error, sizeof(app.hardware_error), "No metering sample yet");
    mqtt_reset(&app);
    if (app.config.wifi_enabled && !app.simulate) {
        char wifi_error[160];
        if (wifi_apply(&app.config, wifi_error, sizeof(wifi_error)))
            log_msg("error", "Initial Wi-Fi setup failed: %s", wifi_error);
    }
    if (hardware_set_mask(&app, app.config.outlet_mask)) {
        app.hardware_errors++;
        snprintf(app.hardware_error, sizeof(app.hardware_error), "Initial I2C outlet write failed: %s", strerror(errno));
        log_msg("error", "%s", app.hardware_error);
    }
    app_poll_hardware(&app);
    if (http_restart(&app))
        log_msg("error", "HTTP listener failed: %s", strerror(errno));
    signal(SIGTERM, stop_handler);
    signal(SIGINT, stop_handler);
    signal(SIGPIPE, SIG_IGN);
    log_msg("info", "Started version %s with %s backend; HTTP %s, MQTT %s",
            MP60_VERSION, app.simulate ? "simulator" : "I2C",
            app.config.http_enabled ? "enabled" : "disabled", app.config.mqtt_enabled ? "enabled" : "disabled");
    while (!stopping) {
        long long now = monotonic_ms();
        if (now >= next_meter) {
            long long delay = 2000;
            app_poll_hardware(&app);
            if (app.last_poll_ok) {
                meter_failures = 0;
            } else {
                if (meter_failures < 5)
                    meter_failures++;
                if (meter_failures > 1)
                    delay = 2000LL << (meter_failures - 1);
                if (delay > 30000)
                    delay = 30000;
            }
            next_meter = monotonic_ms() + delay;
        }
        http_poll(&app);
        mqtt_poll(&app);
        usleep(20000);
    }
    mqtt_reset(&app);
    if (app.http_fd >= 0) close(app.http_fd);
    log_msg("info", "Stopped");
    closelog();
    return 0;
}
