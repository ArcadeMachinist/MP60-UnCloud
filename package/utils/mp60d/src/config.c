#include "mp60d.h"

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int valid_text(const char *value, size_t maximum, int topic)
{
    size_t i, length = strlen(value);
    if (!length || length > maximum)
        return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 32 || c > 126 || c == '\n' || c == '\r')
            return 0;
        if (topic && !(isalnum(c) || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int valid_wifi_text(const char *value, size_t maximum, int allow_empty)
{
    size_t i, length = strlen(value);
    if ((!allow_empty && !length) || length > maximum)
        return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 32 || c == 127)
            return 0;
    }
    return 1;
}

static void generate_client_id(Config *config)
{
    char suffix[9];
    random_hex(suffix, 4);
    snprintf(config->mqtt_client_id, sizeof(config->mqtt_client_id), "mp60-%s", suffix);
}

static void generate_topic(Config *config)
{
    FILE *file = fopen("/sys/class/net/eth0/address", "r");
    unsigned int mac[6];
    int matched = 0;
    if (file) {
        matched = fscanf(file, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2],
                         &mac[3], &mac[4], &mac[5]);
        fclose(file);
    }
    if (matched == 6)
        snprintf(config->mqtt_topic, sizeof(config->mqtt_topic), "mp60-%02x%02x%02x%02x",
                 mac[2] & 0xff, mac[3] & 0xff, mac[4] & 0xff, mac[5] & 0xff);
    else
        strcpy(config->mqtt_topic, "mp60");
}

void config_defaults(Config *config)
{
    memset(config, 0, sizeof(*config));
    config->http_enabled = 1;
    config->http_port = MP60_HTTP_PORT;
    config->mqtt_port = MP60_MQTT_PORT;
    config->mqtt_interval = 60;
    generate_topic(config);
    config->outlet_mask = 0;
    generate_client_id(config);
}

static void assign(Config *config, const char *key, const char *value)
{
    char *end;
    long number;
    if (!strcmp(key, "http_enabled")) {
        config->http_enabled = atoi(value) != 0;
    } else if (!strcmp(key, "http_port")) {
        number = strtol(value, &end, 10);
        if (*value && !*end && number > 0 && number <= 65535)
            config->http_port = (uint16_t)number;
    } else if (!strcmp(key, "mqtt_enabled")) {
        config->mqtt_enabled = atoi(value) != 0;
    } else if (!strcmp(key, "ha_discovery")) {
        config->ha_discovery = atoi(value) != 0;
    } else if (!strcmp(key, "mqtt_host")) {
        if (valid_text(value, sizeof(config->mqtt_host) - 1, 0))
            snprintf(config->mqtt_host, sizeof(config->mqtt_host), "%s", value);
    } else if (!strcmp(key, "mqtt_port")) {
        number = strtol(value, &end, 10);
        if (*value && !*end && number > 0 && number <= 65535)
            config->mqtt_port = (uint16_t)number;
    } else if (!strcmp(key, "mqtt_username")) {
        if (!*value || valid_text(value, sizeof(config->mqtt_username) - 1, 0))
            snprintf(config->mqtt_username, sizeof(config->mqtt_username), "%s", value);
    } else if (!strcmp(key, "mqtt_password")) {
        if (!*value || valid_text(value, sizeof(config->mqtt_password) - 1, 0))
            snprintf(config->mqtt_password, sizeof(config->mqtt_password), "%s", value);
    } else if (!strcmp(key, "mqtt_topic")) {
        if (valid_text(value, sizeof(config->mqtt_topic) - 1, 1))
            snprintf(config->mqtt_topic, sizeof(config->mqtt_topic), "%s", value);
    } else if (!strcmp(key, "mqtt_client_id")) {
        if (valid_text(value, sizeof(config->mqtt_client_id) - 1, 1))
            snprintf(config->mqtt_client_id, sizeof(config->mqtt_client_id), "%s", value);
    } else if (!strcmp(key, "mqtt_interval")) {
        number = strtol(value, &end, 10);
        if (*value && !*end && number >= 5 && number <= 86400)
            config->mqtt_interval = (uint16_t)number;
    } else if (!strcmp(key, "wifi_enabled")) {
        config->wifi_enabled = atoi(value) != 0;
    } else if (!strcmp(key, "wifi_ssid")) {
        if (valid_wifi_text(value, sizeof(config->wifi_ssid) - 1, 1))
            snprintf(config->wifi_ssid, sizeof(config->wifi_ssid), "%s", value);
    } else if (!strcmp(key, "wifi_password")) {
        if (valid_wifi_text(value, sizeof(config->wifi_password) - 1, 1))
            snprintf(config->wifi_password, sizeof(config->wifi_password), "%s", value);
    } else if (!strcmp(key, "outlet_mask")) {
        number = strtol(value, &end, 10);
        if (*value && !*end && number >= 0 && number < 64)
            config->outlet_mask = (uint8_t)number;
    } else if (!strcmp(key, "ui_salt") && strlen(value) == 16) {
        snprintf(config->ui_salt, sizeof(config->ui_salt), "%s", value);
    } else if (!strcmp(key, "ui_hash") && strlen(value) == 64) {
        snprintf(config->ui_hash, sizeof(config->ui_hash), "%s", value);
        config->ui_password_set = 1;
    }
}

int config_load(Config *config, const char *path)
{
    FILE *file;
    char line[512];
    config_defaults(config);
    file = fopen(path, "r");
    if (!file)
        return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof(line), file)) {
        char *equals;
        line[strcspn(line, "\r\n")] = 0;
        if (!*line || *line == '#')
            continue;
        equals = strchr(line, '=');
        if (!equals)
            continue;
        *equals++ = 0;
        assign(config, line, equals);
    }
    fclose(file);
    if (strlen(config->ui_salt) != 16 || strlen(config->ui_hash) != 64) {
        config->ui_password_set = 0;
        config->ui_salt[0] = 0;
        config->ui_hash[0] = 0;
    }
    return 0;
}

int config_save(const Config *config, const char *path)
{
    char temporary[256];
    FILE *file;
    int fd;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return -1;
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        return -1;
    }
    fprintf(file,
            "http_enabled=%d\nhttp_port=%u\n"
            "mqtt_enabled=%d\nha_discovery=%d\nmqtt_host=%s\nmqtt_port=%u\n"
            "mqtt_username=%s\nmqtt_password=%s\nmqtt_topic=%s\n"
            "mqtt_client_id=%s\nmqtt_interval=%u\n"
            "wifi_enabled=%d\nwifi_ssid=%s\nwifi_password=%s\n"
            "outlet_mask=%u\nui_salt=%s\nui_hash=%s\n",
            config->http_enabled, (unsigned)config->http_port,
            config->mqtt_enabled, config->ha_discovery, config->mqtt_host, (unsigned)config->mqtt_port,
            config->mqtt_username, config->mqtt_password, config->mqtt_topic,
            config->mqtt_client_id, (unsigned)config->mqtt_interval,
            config->wifi_enabled, config->wifi_ssid, config->wifi_password,
            (unsigned)config->outlet_mask, config->ui_salt, config->ui_hash);
    if (fflush(file) || fsync(fd) || fclose(file)) {
        unlink(temporary);
        return -1;
    }
    chmod(temporary, 0600);
    if (rename(temporary, path)) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

void config_set_ui_password(Config *config, const char *password)
{
    char input[192];
    if (!password || !*password) {
        config->ui_password_set = 0;
        config->ui_salt[0] = 0;
        config->ui_hash[0] = 0;
        return;
    }
    random_hex(config->ui_salt, 8);
    snprintf(input, sizeof(input), "%s:%s", config->ui_salt, password);
    sha256_hex(input, strlen(input), config->ui_hash);
    config->ui_password_set = 1;
}

int config_check_ui_password(const Config *config, const char *password)
{
    char input[192], hash[65];
    if (!config->ui_password_set)
        return 1;
    if (!password)
        return 0;
    snprintf(input, sizeof(input), "%s:%s", config->ui_salt, password);
    sha256_hex(input, strlen(input), hash);
    return constant_time_equal(hash, config->ui_hash);
}
