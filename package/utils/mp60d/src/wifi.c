#include "mp60d.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_capture(char *output, size_t output_size, const char *program, ...)
{
    va_list arguments;
    const char *argv[16];
    int count = 0, pipefd[2], status;
    pid_t child;
    size_t used = 0;
    const char *value;
    if (output && output_size)
        output[0] = 0;
    argv[count++] = program;
    va_start(arguments, program);
    while (count < 15 && (value = va_arg(arguments, const char *)) != NULL)
        argv[count++] = value;
    va_end(arguments);
    argv[count] = NULL;
    if (pipe(pipefd))
        return -1;
    child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(program, (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    for (;;) {
        char buffer[512];
        ssize_t got = read(pipefd[0], buffer, sizeof(buffer));
        if (got > 0) {
            if (output && output_size && used + 1 < output_size) {
                size_t copy = (size_t)got;
                if (copy > output_size - used - 1)
                    copy = output_size - used - 1;
                memcpy(output + used, buffer, copy);
                used += copy;
                output[used] = 0;
            }
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(pipefd[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int apply_command(char *error, size_t error_size, const char *program, ...)
{
    va_list arguments;
    const char *argv[16];
    const char *value;
    int count = 0;
    argv[count++] = program;
    va_start(arguments, program);
    while (count < 15 && (value = va_arg(arguments, const char *)) != NULL)
        argv[count++] = value;
    va_end(arguments);
    argv[count] = NULL;
    {
        pid_t child = fork();
        int status;
        if (child < 0)
            goto failed;
        if (child == 0) {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd >= 0) {
                dup2(nullfd, STDOUT_FILENO);
                dup2(nullfd, STDERR_FILENO);
                if (nullfd > STDERR_FILENO)
                    close(nullfd);
            }
            execvp(program, (char *const *)argv);
            _exit(127);
        }
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            return 0;
    }
failed:
    snprintf(error, error_size, "Network command failed: %s", program);
    return -1;
}

#define WIFI_SCAN_MAX_NETWORKS 16

typedef struct {
    char ssid[33];
    double signal;
    unsigned frequency;
    int security; /* 0=open, 1=legacy WPA, 2=RSN/WPA2+ */
} WifiScanNetwork;

static void scan_commit(WifiScanNetwork *networks, int *count, WifiScanNetwork *current, int present)
{
    int i;
    if (!present || !current->ssid[0])
        return;
    for (i = 0; i < *count; i++) {
        if (!strcmp(networks[i].ssid, current->ssid)) {
            if (current->signal > networks[i].signal)
                networks[i] = *current;
            return;
        }
    }
    if (*count < WIFI_SCAN_MAX_NETWORKS)
        networks[(*count)++] = *current;
}

int wifi_scan_parse(const char *text, char *output, size_t size)
{
    WifiScanNetwork networks[WIFI_SCAN_MAX_NETWORKS];
    WifiScanNetwork current;
    const char *line = text;
    int count = 0, present = 0, i, j;
    memset(networks, 0, sizeof(networks));
    memset(&current, 0, sizeof(current));
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *field = line;
        size_t length = end ? (size_t)(end - line) : strlen(line);
        while (*field == '\t' || *field == ' ')
            field++;
        if (!strncmp(field, "BSS ", 4)) {
            scan_commit(networks, &count, &current, present);
            memset(&current, 0, sizeof(current));
            current.signal = -1000;
            present = 1;
        } else if (present && !strncmp(field, "freq: ", 6)) {
            current.frequency = (unsigned)strtoul(field + 6, NULL, 10);
        } else if (present && !strncmp(field, "signal: ", 8)) {
            current.signal = strtod(field + 8, NULL);
        } else if (present && !strncmp(field, "SSID:", 5)) {
            const char *value = field + 5;
            size_t value_length;
            if (*value == ' ')
                value++;
            value_length = length - (size_t)(value - line);
            if (value_length && value[value_length - 1] == '\r')
                value_length--;
            if (value_length > 32)
                value_length = 32;
            memcpy(current.ssid, value, value_length);
            current.ssid[value_length] = 0;
        } else if (present && !strncmp(field, "RSN:", 4)) {
            current.security = 2;
        } else if (present && !strncmp(field, "WPA:", 4) && current.security == 0) {
            current.security = 1;
        }
        if (!end)
            break;
        line = end + 1;
    }
    scan_commit(networks, &count, &current, present);
    for (i = 0; i < count; i++) {
        int best = i;
        for (j = i + 1; j < count; j++)
            if (networks[j].signal > networks[best].signal)
                best = j;
        if (best != i) {
            WifiScanNetwork swap = networks[i];
            networks[i] = networks[best];
            networks[best] = swap;
        }
    }
    if (snprintf(output, size, "{\"networks\":[") < 0)
        return -1;
    for (i = 0; i < count; i++) {
        const char *encryption = networks[i].security == 2 ? "psk2" :
            networks[i].security == 1 ? "psk" : "none";
        int n = snprintf(output + strlen(output), size - strlen(output),
            "%s{\"ssid\":\"", i ? "," : "");
        if (n < 0 || (size_t)n >= size - strlen(output) ||
            json_append(output, size, networks[i].ssid))
            return -1;
        n = snprintf(output + strlen(output), size - strlen(output),
            "\",\"signal_dbm\":%.2f,\"frequency_mhz\":%u,\"encryption\":\"%s\",\"open_network\":%s}",
            networks[i].signal, networks[i].frequency, encryption,
            networks[i].security ? "false" : "true");
        if (n < 0 || (size_t)n >= size - strlen(output))
            return -1;
    }
    if (strlen(output) + 16 >= size)
        return -1;
    snprintf(output + strlen(output), size - strlen(output), "],\"count\":%d}", count);
    return 0;
}

int wifi_scan_json(App *app, char *output, size_t size)
{
    char *scan;
    int created = 0, result = -1;
    if (app->simulate) {
        static const char simulated[] =
            "{\"networks\":[{\"ssid\":\"MP60-Test\",\"signal_dbm\":-42.00,"
            "\"frequency_mhz\":2462,\"encryption\":\"psk2\",\"open_network\":false},"
            "{\"ssid\":\"Coffee Shop\",\"signal_dbm\":-71.00,\"frequency_mhz\":2437,"
            "\"encryption\":\"none\",\"open_network\":true}],\"count\":2}";
        if (sizeof(simulated) > size)
            return -1;
        memcpy(output, simulated, sizeof(simulated));
        return 0;
    }
    scan = malloc(262144);
    if (!scan)
        return -1;
    /* A second managed interface cannot be brought up while this station is
       associated. Scanning the existing interface works and avoids disruption. */
    if (access("/sys/class/net/phy0-sta0", F_OK) == 0) {
        result = run_capture(scan, 262144, "iw", "dev", "phy0-sta0", "scan", NULL);
        if (result == 0)
            result = wifi_scan_parse(scan, output, size);
        free(scan);
        return result;
    }
    (void)apply_command(NULL, 0, "iw", "dev", "mp60scan", "del", NULL);
    if (apply_command(NULL, 0, "iw", "phy", "phy0", "interface", "add",
                      "mp60scan", "type", "managed", NULL) == 0) {
        created = 1;
        if (apply_command(NULL, 0, "ip", "link", "set", "mp60scan", "up", NULL) == 0 &&
            run_capture(scan, 262144, "iw", "dev", "mp60scan", "scan", NULL) == 0)
            result = wifi_scan_parse(scan, output, size);
    }
    if (created) {
        (void)apply_command(NULL, 0, "ip", "link", "set", "mp60scan", "down", NULL);
        (void)apply_command(NULL, 0, "iw", "dev", "mp60scan", "del", NULL);
    }
    free(scan);
    return result;
}

int wifi_apply(const Config *config, char *error, size_t error_size)
{
    const char *disabled = config->wifi_enabled ? "0" : "1";
    if (error && error_size)
        error[0] = 0;
    (void)apply_command(NULL, 0, "uci", "-q", "delete", "network.wwan", NULL);
    (void)apply_command(NULL, 0, "uci", "-q", "delete", "wireless.mp60_sta", NULL);
    if (apply_command(error, error_size, "uci", "set", "network.wwan=interface", NULL) ||
        apply_command(error, error_size, "uci", "set", "network.wwan.proto=dhcp", NULL) ||
        apply_command(error, error_size, "uci", "set", "network.wwan.metric=20", NULL) ||
        apply_command(error, error_size, "uci", "set", "wireless.mp60_sta=wifi-iface", NULL) ||
        apply_command(error, error_size, "uci", "set", "wireless.mp60_sta.device=radio0", NULL) ||
        apply_command(error, error_size, "uci", "set", "wireless.mp60_sta.mode=sta", NULL) ||
        apply_command(error, error_size, "uci", "set", "wireless.mp60_sta.network=wwan", NULL))
        return -1;
    {
        char setting[96];
        snprintf(setting, sizeof(setting), "wireless.mp60_sta.ssid=%s", config->wifi_ssid);
        if (apply_command(error, error_size, "uci", "set", setting, NULL))
            return -1;
    }
    if (config->wifi_password[0]) {
        char setting[160];
        if (apply_command(error, error_size, "uci", "set", "wireless.mp60_sta.encryption=psk2", NULL))
            return -1;
        snprintf(setting, sizeof(setting), "wireless.mp60_sta.key=%s", config->wifi_password);
        if (apply_command(error, error_size, "uci", "set", setting, NULL))
            return -1;
    } else {
        if (apply_command(error, error_size, "uci", "set", "wireless.mp60_sta.encryption=none", NULL))
            return -1;
        (void)apply_command(NULL, 0, "uci", "-q", "delete", "wireless.mp60_sta.key", NULL);
    }
    {
        char setting[64];
        snprintf(setting, sizeof(setting), "wireless.mp60_sta.disabled=%s", disabled);
        if (apply_command(error, error_size, "uci", "set", setting, NULL))
            return -1;
        snprintf(setting, sizeof(setting), "wireless.radio0.disabled=%s", disabled);
        if (apply_command(error, error_size, "uci", "set", setting, NULL) ||
            apply_command(error, error_size, "uci", "commit", "network", NULL) ||
            apply_command(error, error_size, "uci", "commit", "wireless", NULL) ||
            apply_command(error, error_size, "wifi", "reload", NULL))
            return -1;
    }
    return 0;
}

static int ethernet_linked(void)
{
    char value[8] = {0};
    int fd = open("/sys/class/net/eth0/carrier", O_RDONLY);
    ssize_t got;
    if (fd < 0)
        return -1;
    got = read(fd, value, sizeof(value) - 1);
    close(fd);
    return got > 0 && value[0] == '1';
}

int wifi_status_json(App *app, char *output, size_t size)
{
    char status[4096], device[64] = "", routes[1024] = "", *end;
    char address[512] = "", ip_address[64] = "";
    int up = 0, ethernet = ethernet_linked();
    char type, value[16];
    if (app->simulate) {
        if (snprintf(output, size, "{\"enabled\":%s,\"ssid\":\"",
                     app->config.wifi_enabled ? "true" : "false") < 0 ||
            json_append(output, size, app->config.wifi_ssid))
            return -1;
        if (snprintf(output + strlen(output), size - strlen(output),
            "\",\"password_set\":%s,\"connected\":false,\"interface\":\"\","
            "\"ip_address\":\"\",\"ethernet_linked\":true,\"ethernet_priority\":true,"
            "\"default_route\":\"simulator: Ethernet metric 0, Wi-Fi metric 20\"}",
            app->config.wifi_password[0] ? "true" : "false") < 0)
            return -1;
        return strlen(output) < size ? 0 : -1;
    }
    if (run_capture(status, sizeof(status), "ubus", "call", "network.interface.wwan", "status", NULL) == 0) {
        if (json_field(status, "up", &type, value, sizeof(value)) == 1 && type == 'b')
            up = !strcmp(value, "true");
        if (json_field(status, "l3_device", &type, device, sizeof(device)) != 1 || type != 's')
            device[0] = 0;
    }
    if (device[0] && run_capture(address, sizeof(address), "ip", "-4", "-o", "addr",
                                 "show", "dev", device, NULL) == 0) {
        char *inet = strstr(address, " inet ");
        if (inet && sscanf(inet + 6, "%63[^/ ]", ip_address) != 1)
            ip_address[0] = 0;
    }
    (void)run_capture(routes, sizeof(routes), "ip", "route", "show", "default", NULL);
    end = strchr(routes, '\n');
    if (end)
        *end = 0;
    if (snprintf(output, size,
        "{\"enabled\":%s,\"ssid\":\"", app->config.wifi_enabled ? "true" : "false") < 0 ||
        json_append(output, size, app->config.wifi_ssid))
        return -1;
    if (snprintf(output + strlen(output), size - strlen(output),
        "\",\"password_set\":%s,\"connected\":%s,\"interface\":\"",
        app->config.wifi_password[0] ? "true" : "false", up ? "true" : "false") < 0 ||
        json_append(output, size, device))
        return -1;
    if (snprintf(output + strlen(output), size - strlen(output), "\",\"ip_address\":\"") < 0 ||
        json_append(output, size, ip_address))
        return -1;
    if (snprintf(output + strlen(output), size - strlen(output),
        "\",\"ethernet_linked\":%s,\"ethernet_priority\":%s,\"default_route\":\"",
        ethernet == 1 ? "true" : ethernet == 0 ? "false" : "null",
        ethernet == 1 ? "true" : "false") < 0 ||
        json_append(output, size, routes))
        return -1;
    if (strlen(output) + 3 >= size)
        return -1;
    strcat(output, "\"}");
    return 0;
}

static int text_safe(const char *value)
{
    size_t i;
    for (i = 0; value[i]; i++)
        if ((unsigned char)value[i] < 32 || (unsigned char)value[i] == 127)
            return 0;
    return 1;
}

int wifi_update_config(App *app, const char *json, char *error, size_t error_size)
{
    Config next = app->config;
    char type, value[96];
    int found, open_network = 0;
    found = json_field(json, "enabled", &type, value, sizeof(value));
    if (found < 0 || (found > 0 && type != 'b'))
        goto invalid;
    if (found > 0)
        next.wifi_enabled = !strcmp(value, "true");
    found = json_field(json, "ssid", &type, value, sizeof(value));
    if (found < 0 || (found > 0 && type != 's'))
        goto invalid;
    if (found > 0) {
        if (strlen(value) > 32 || !text_safe(value))
            goto invalid;
        strcpy(next.wifi_ssid, value);
    }
    found = json_field(json, "open_network", &type, value, sizeof(value));
    if (found < 0 || (found > 0 && type != 'b'))
        goto invalid;
    if (found > 0)
        open_network = !strcmp(value, "true");
    found = json_field(json, "password", &type, value, sizeof(value));
    if (found < 0 || (found > 0 && type != 's'))
        goto invalid;
    if (found > 0 && *value) {
        if (strlen(value) < 8 || strlen(value) > 63 || !text_safe(value)) {
            snprintf(error, error_size, "Wi-Fi password must contain 8 to 63 characters");
            return -1;
        }
        strcpy(next.wifi_password, value);
    }
    if (open_network)
        next.wifi_password[0] = 0;
    if (next.wifi_enabled && !next.wifi_ssid[0]) {
        snprintf(error, error_size, "SSID is required when Wi-Fi is enabled");
        return -1;
    }
    if (next.wifi_enabled && !next.wifi_password[0] && !open_network) {
        snprintf(error, error_size, "Enter a Wi-Fi password or select open network");
        return -1;
    }
    if (config_save(&next, config_path)) {
        snprintf(error, error_size, "Could not save Wi-Fi configuration: %s", strerror(errno));
        return -1;
    }
    app->config = next;
    if (!app->simulate && wifi_apply(&next, error, error_size)) {
        char detail[160];
        snprintf(detail, sizeof(detail), "%s", error);
        snprintf(error, error_size, "Wi-Fi settings saved, but apply failed: %s", detail);
        return -1;
    }
    return 0;
invalid:
    snprintf(error, error_size, "Invalid Wi-Fi configuration field");
    return -1;
}
