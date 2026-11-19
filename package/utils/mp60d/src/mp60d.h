#ifndef MP60D_H
#define MP60D_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MP60_VERSION "1.5.2"
#define MP60_FIRMWARE_NAME "OpenWrt 25.12.6"
#define MP60_OUTLETS 6
#define MP60_CONFIG_PATH "/etc/mp60d.conf"
#define MP60_HTTP_PORT 80
#define MP60_MQTT_PORT 1883

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    int http_enabled;
    uint16_t http_port;
    int mqtt_enabled;
    int ha_discovery;
    char mqtt_host[128];
    uint16_t mqtt_port;
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic[32];
    char mqtt_client_id[48];
    uint16_t mqtt_interval;
    int wifi_enabled;
    char wifi_ssid[33];
    char wifi_password[65];
    uint8_t outlet_mask;
    int ui_password_set;
    char ui_salt[17];
    char ui_hash[65];
} Config;

typedef struct {
    int valid;
    time_t sampled_at;
    uint8_t current_x10[MP60_OUTLETS];
    uint8_t voltage;
    uint8_t frequency;
    uint16_t apparent_x10[MP60_OUTLETS];
    uint16_t active_x10[MP60_OUTLETS];
    uint16_t reactive_x10[MP60_OUTLETS];
    uint8_t surge_depleted;
    uint8_t reported_wire_mask;
} Metrics;

typedef struct {
    int fd;
    int connecting;
    int connected;
    time_t next_connect;
    time_t last_io;
    time_t last_ping;
    time_t next_telemetry;
    time_t next_ha_discovery;
    unsigned long attempts;
    unsigned char rx[8192];
    size_t rx_len;
} Mqtt;

typedef struct {
    Config config;
    Metrics metrics;
    Mqtt mqtt;
    int simulate;
    int http_fd;
    int hardware_stale;
    int last_poll_ok;
    unsigned long hardware_errors;
    char hardware_error[128];
    time_t started_at;
    unsigned long revision;
} App;

/* util.c */
void log_msg(const char *level, const char *fmt, ...);
int str_ieq(const char *a, const char *b);
int parse_on_off(const char *text, int allow_toggle);
int json_field(const char *json, const char *key, char *type, char *value, size_t value_size);
int json_append(char *buf, size_t size, const char *text);
void sha256_hex(const void *data, size_t length, char output[65]);
void random_hex(char *output, size_t bytes);
int base64_decode(const char *input, unsigned char *output, size_t capacity);
int constant_time_equal(const char *a, const char *b);
long long monotonic_ms(void);

/* hardware.c */
uint8_t mp60_xor(const uint8_t *data, size_t length);
uint8_t mask_to_wire(uint8_t mask);
uint8_t wire_to_mask(uint8_t wire);
int hardware_parse_response(const uint8_t response[53], Metrics *metrics);
int hardware_set_mask(App *app, uint8_t mask);
int hardware_read(App *app, Metrics *metrics);
void hardware_simulate_reading(App *app, Metrics *metrics);

/* config.c */
void config_defaults(Config *config);
int config_load(Config *config, const char *path);
int config_save(const Config *config, const char *path);
void config_set_ui_password(Config *config, const char *password);
int config_check_ui_password(const Config *config, const char *password);

/* wifi.c */
int wifi_apply(const Config *config, char *error, size_t error_size);
int wifi_status_json(App *app, char *output, size_t size);
int wifi_scan_parse(const char *text, char *output, size_t size);
int wifi_scan_json(App *app, char *output, size_t size);
int wifi_update_config(App *app, const char *json, char *error, size_t error_size);

/* main.c */
extern const char *config_path;
uint8_t app_actual_mask(const App *app);
const char *app_power_state(const App *app, int outlet);
int app_set_outlet(App *app, int outlet, int action, char *error, size_t error_size);
void app_poll_hardware(App *app);
int app_status_json(App *app, char *output, size_t size);
int app_config_json(App *app, char *output, size_t size);
int app_update_config(App *app, const char *json, char *error, size_t error_size);

/* http.c */
int http_restart(App *app);
void http_poll(App *app);

/* mqtt.c and ha.c */
int publish_topic(App *app, const char *topic, const char *payload, int retain);
int publish_full(App *app, const char *prefix, const char *leaf,
                 const char *payload, int retain);
void mqtt_reset(App *app);
void mqtt_poll(App *app);
void mqtt_state_changed(App *app, int command_result);
void mqtt_telemetry(App *app);
int mqtt_ha_discovery(App *app);

#endif
