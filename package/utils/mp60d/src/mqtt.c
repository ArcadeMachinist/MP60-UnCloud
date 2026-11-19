#include "mp60d.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct { unsigned char data[2048]; size_t length; } Packet;

static int put_byte(Packet *p, unsigned char value)
{
    if (p->length >= sizeof(p->data)) return -1;
    p->data[p->length++] = value;
    return 0;
}

static int put_bytes(Packet *p, const void *data, size_t length)
{
    if (p->length + length > sizeof(p->data)) return -1;
    memcpy(p->data + p->length, data, length);
    p->length += length;
    return 0;
}

static int put_u16(Packet *p, unsigned value)
{
    return put_byte(p, (unsigned char)(value >> 8)) || put_byte(p, (unsigned char)value) ? -1 : 0;
}

static int put_string(Packet *p, const char *text)
{
    size_t length = strlen(text);
    if (length > 65535 || put_u16(p, (unsigned)length)) return -1;
    return put_bytes(p, text, length);
}

static void packet_start(Packet *p, unsigned char type)
{
    p->length = 5;
    p->data[0] = type;
}

static int packet_finish(Packet *p)
{
    size_t body = p->length - 5, count = 0, i;
    unsigned char encoded[4];
    do {
        encoded[count] = (unsigned char)(body % 128);
        body /= 128;
        if (body) encoded[count] |= 128;
        count++;
    } while (body && count < 4);
    memmove(p->data + 1 + count, p->data + 5, p->length - 5);
    for (i = 0; i < count; i++) p->data[1 + i] = encoded[i];
    p->length = p->length - 5 + 1 + count;
    return 0;
}

static int send_packet(App *app, Packet *p)
{
    size_t sent = 0;
    packet_finish(p);
    while (sent < p->length) {
        ssize_t n = send(app->mqtt.fd, p->data + sent, p->length - sent, MSG_DONTWAIT);
        if (n > 0) sent += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else {
            mqtt_reset(app);
            app->mqtt.next_connect = time(NULL) + 5;
            return -1;
        }
    }
    app->mqtt.last_io = time(NULL);
    return 0;
}

int publish_topic(App *app, const char *topic, const char *payload, int retain)
{
    Packet p;
    if (!app->mqtt.connected) return -1;
    packet_start(&p, 0x30 | (retain ? 1 : 0));
    if (put_string(&p, topic) || put_bytes(&p, payload, strlen(payload))) return -1;
    return send_packet(app, &p);
}

int publish_full(App *app, const char *prefix, const char *leaf,
                 const char *payload, int retain)
{
    char topic[256];
    if (snprintf(topic, sizeof(topic), "%s/%s%s%s", prefix, app->config.mqtt_topic,
                 leaf && *leaf ? "/" : "", leaf && *leaf ? leaf : "") >= (int)sizeof(topic)) return -1;
    return publish_topic(app, topic, payload, retain);
}

static int send_connect(App *app)
{
    Packet p;
    char will_topic[160];
    unsigned char flags = 0x06 | 0x20; /* clean session, will, retained will */
    snprintf(will_topic, sizeof(will_topic), "tele/%s/LWT", app->config.mqtt_topic);
    if (app->config.mqtt_password[0] || app->config.mqtt_username[0]) flags |= 0x80;
    if (app->config.mqtt_password[0]) flags |= 0x40;
    packet_start(&p, 0x10);
    if (put_string(&p, "MQTT") || put_byte(&p, 4) || put_byte(&p, flags) || put_u16(&p, 30) ||
        put_string(&p, app->config.mqtt_client_id) || put_string(&p, will_topic) || put_string(&p, "Offline"))
        return -1;
    if ((flags & 0x80) && put_string(&p, app->config.mqtt_username)) return -1;
    if ((flags & 0x40) && put_string(&p, app->config.mqtt_password)) return -1;
    return send_packet(app, &p);
}

static int send_subscribe(App *app)
{
    Packet p;
    char filter[160];
    snprintf(filter, sizeof(filter), "cmnd/%s/#", app->config.mqtt_topic);
    packet_start(&p, 0x82);
    if (put_u16(&p, 1) || put_string(&p, filter) || put_byte(&p, 0)) return -1;
    return send_packet(app, &p);
}

void mqtt_reset(App *app)
{
    if (app->mqtt.fd >= 0) close(app->mqtt.fd);
    app->mqtt.fd = -1;
    app->mqtt.connecting = 0;
    app->mqtt.connected = 0;
    app->mqtt.rx_len = 0;
    app->mqtt.next_connect = time(NULL) + 5;
}

static void start_connect(App *app)
{
    struct addrinfo hints, *addresses = NULL;
    char service[8];
    int fd = -1, flags, result;
    snprintf(service, sizeof(service), "%u", (unsigned)app->config.mqtt_port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    app->mqtt.attempts++;
    result = getaddrinfo(app->config.mqtt_host, service, &hints, &addresses);
    if (result) {
        log_msg("warning", "MQTT DNS lookup failed: %s", gai_strerror(result));
        app->mqtt.next_connect = time(NULL) + 10;
        return;
    }
    if (addresses) {
        fd = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
        if (fd >= 0) {
            flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            result = connect(fd, addresses->ai_addr, addresses->ai_addrlen);
            if (result == 0) {
                app->mqtt.fd = fd;
                app->mqtt.connecting = 0;
                send_connect(app);
            } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
                app->mqtt.fd = fd;
                app->mqtt.connecting = 1;
            } else {
                close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(addresses);
    }
    if (fd < 0) {
        app->mqtt.fd = -1;
        app->mqtt.next_connect = time(NULL) + 5;
    } else {
        app->mqtt.last_io = time(NULL);
        app->mqtt.next_connect = 0;
    }
}

static void command_message(App *app, const char *topic, const unsigned char *payload, size_t length)
{
    const char *leaf;
    char command[80], error[160];
    int outlet = -1, action;
    if (length >= sizeof(command)) return;
    memcpy(command, payload, length);
    command[length] = 0;
    leaf = strrchr(topic, '/');
    if (!leaf) return;
    leaf++;
    if (str_ieq(leaf, "STATE") || str_ieq(leaf, "STATUS")) {
        mqtt_telemetry(app);
        return;
    }
    if (!strncasecmp(leaf, "POWER", 5)) {
        const char *suffix = leaf + 5;
        char *end;
        if (!*suffix) outlet = 0;
        else {
            long number = strtol(suffix, &end, 10);
            if (!*end && number >= 1 && number <= MP60_OUTLETS) outlet = (int)number;
        }
    }
    if (outlet < 0) return;
    action = parse_on_off(command, outlet != 0);
    if (action >= 0 && app_set_outlet(app, outlet, action, error, sizeof(error)))
        log_msg("warning", "MQTT outlet command failed: %s", error);
}

static void connected(App *app)
{
    app->mqtt.connected = 1;
    app->mqtt.connecting = 0;
    app->mqtt.last_io = app->mqtt.last_ping = time(NULL);
    app->mqtt.next_telemetry = 0;
    app->mqtt.next_ha_discovery = 0;
    log_msg("info", "MQTT connected to %s:%u", app->config.mqtt_host, (unsigned)app->config.mqtt_port);
    if (send_subscribe(app) == 0) {
        publish_full(app, "tele", "LWT", "Online", 1);
        mqtt_telemetry(app);
    }
}

static void handle_packet(App *app, unsigned char type, const unsigned char *body, size_t length)
{
    if (type == 0x20 && !app->mqtt.connected) {
        if (length == 2 && body[1] == 0) connected(app);
        else {
            log_msg("warning", "MQTT broker refused connection (code %u)", length == 2 ? body[1] : 255);
            mqtt_reset(app);
        }
    } else if (type == 0x30 && app->mqtt.connected && length >= 2) {
        unsigned topic_length = ((unsigned)body[0] << 8) | body[1];
        if (topic_length + 2 <= length) {
            char topic[192];
            if (topic_length < sizeof(topic)) {
                memcpy(topic, body + 2, topic_length);
                topic[topic_length] = 0;
                command_message(app, topic, body + 2 + topic_length, length - 2 - topic_length);
            }
        }
    }
}

static void receive_packets(App *app)
{
    for (;;) {
        ssize_t n;
        if (app->mqtt.rx_len == sizeof(app->mqtt.rx)) {
            mqtt_reset(app);
            return;
        }
        n = recv(app->mqtt.fd, app->mqtt.rx + app->mqtt.rx_len,
                 sizeof(app->mqtt.rx) - app->mqtt.rx_len, MSG_DONTWAIT);
        if (n > 0) {
            app->mqtt.rx_len += (size_t)n;
            app->mqtt.last_io = time(NULL);
        } else if (n == 0) {
            mqtt_reset(app);
            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            mqtt_reset(app);
            return;
        }
    }
    for (;;) {
        size_t remaining = 0, multiplier = 1, bytes = 1, packet_size;
        unsigned char digit;
        if (app->mqtt.rx_len < 2) return;
        for (;;) {
            if (bytes > 4 || bytes >= app->mqtt.rx_len) return;
            digit = app->mqtt.rx[bytes++];
            remaining += (size_t)(digit & 127) * multiplier;
            multiplier *= 128;
            if (!(digit & 128)) break;
        }
        if (remaining > sizeof(app->mqtt.rx) - bytes) {
            mqtt_reset(app);
            return;
        }
        packet_size = bytes + remaining;
        if (app->mqtt.rx_len < packet_size) return;
        handle_packet(app, app->mqtt.rx[0] & 0xf0, app->mqtt.rx + bytes, remaining);
        memmove(app->mqtt.rx, app->mqtt.rx + packet_size, app->mqtt.rx_len - packet_size);
        app->mqtt.rx_len -= packet_size;
        if (app->mqtt.fd < 0) return;
    }
}

void mqtt_poll(App *app)
{
    time_t now = time(NULL);
    if (!app->config.mqtt_enabled || !app->config.mqtt_host[0]) {
        if (app->mqtt.fd >= 0) mqtt_reset(app);
        return;
    }
    if (app->mqtt.fd < 0) {
        if (now >= app->mqtt.next_connect) start_connect(app);
        return;
    }
    if (app->mqtt.connecting) {
        fd_set writers;
        struct timeval timeout = {0, 0};
        int error = 0;
        socklen_t length = sizeof(error);
        FD_ZERO(&writers);
        FD_SET(app->mqtt.fd, &writers);
        if (select(app->mqtt.fd + 1, NULL, &writers, NULL, &timeout) <= 0) {
            if (now - app->mqtt.last_io > 5) mqtt_reset(app);
            return;
        }
        if (getsockopt(app->mqtt.fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
            mqtt_reset(app);
            return;
        }
        app->mqtt.connecting = 0;
        if (send_connect(app)) return;
    }
    receive_packets(app);
    if (app->mqtt.fd < 0 || !app->mqtt.connected) {
        if (app->mqtt.fd >= 0 && now - app->mqtt.last_io > 5) mqtt_reset(app);
        return;
    }
    if (now - app->mqtt.last_ping >= 20) {
        Packet p;
        packet_start(&p, 0xc0);
        if (send_packet(app, &p) == 0) app->mqtt.last_ping = now;
    }
    if (now - app->mqtt.last_io > 90) {
        mqtt_reset(app);
        return;
    }
    if (now >= app->mqtt.next_telemetry) {
        mqtt_telemetry(app);
        app->mqtt.next_telemetry = now + app->config.mqtt_interval;
    }
    if (app->config.ha_discovery && now >= app->mqtt.next_ha_discovery) {
        app->mqtt.next_ha_discovery = now + (mqtt_ha_discovery(app) == 0 ? 300 : 30);
    }
}

void mqtt_state_changed(App *app, int command_result)
{
    int i;
    char payload[512];
    if (!app->mqtt.connected) return;
    if (command_result) {
        int used = snprintf(payload, sizeof(payload), "{");
        for (i = 0; i < MP60_OUTLETS && used > 0 && (size_t)used < sizeof(payload); i++)
            used += snprintf(payload + used, sizeof(payload) - (size_t)used, "%s\"POWER%d\":\"%s\"",
                i ? "," : "", i + 1, app_power_state(app, i + 1));
        if (used > 0 && (size_t)used + 2 < sizeof(payload)) {
            strcpy(payload + used, "}");
            publish_full(app, "stat", "RESULT", payload, 0);
        }
    }
    for (i = 0; i < MP60_OUTLETS; i++) {
        char leaf[16];
        snprintf(leaf, sizeof(leaf), "POWER%d", i + 1);
        publish_full(app, "stat", leaf, app_power_state(app, i + 1), 0);
    }
    mqtt_telemetry(app);
}

void mqtt_telemetry(App *app)
{
    char payload[2048], state[512];
    int i, used;
    double total_current = 0, total_active = 0, total_apparent = 0, total_reactive = 0;
    if (!app->mqtt.connected) return;
    used = snprintf(state, sizeof(state), "{\"Uptime\":%ld,\"POWER\":\"%s\"",
        (long)(time(NULL) - app->started_at),
        !app->metrics.valid ? "UNKNOWN" : app_actual_mask(app) ? "ON" : "OFF");
    for (i = 0; i < MP60_OUTLETS && used > 0 && (size_t)used < sizeof(state); i++) {
        used += snprintf(state + used, sizeof(state) - (size_t)used, ",\"POWER%d\":\"%s\"",
            i + 1, app_power_state(app, i + 1));
    }
    if (used > 0 && (size_t)used + 2 < sizeof(state)) {
        strcpy(state + used, "}");
        publish_full(app, "tele", "STATE", state, 0);
    }
    for (i = 0; i < MP60_OUTLETS; i++) {
        total_current += app->metrics.current_x10[i] / 10.0;
        total_active += app->metrics.active_x10[i] / 10.0;
        total_apparent += app->metrics.apparent_x10[i] / 10.0;
        total_reactive += app->metrics.reactive_x10[i] / 10.0;
    }
    used = snprintf(payload, sizeof(payload),
        "{\"ENERGY\":{\"Voltage\":%u,\"Frequency\":%u,\"Current\":%.1f,\"Power\":%.1f,\"ApparentPower\":%.1f,"
        "\"ReactivePower\":%.1f,\"Factor\":%.3f},\"MP60\":{\"Stale\":%s,\"Outlets\":[",
        (unsigned)app->metrics.voltage, (unsigned)app->metrics.frequency,
        total_current, total_active, total_apparent, total_reactive,
        total_apparent > 0 ? total_active / total_apparent : 0, app->hardware_stale ? "true" : "false");
    for (i = 0; i < MP60_OUTLETS && used > 0 && (size_t)used < sizeof(payload); i++) {
        used += snprintf(payload + used, sizeof(payload) - (size_t)used,
            "%s{\"Id\":%d,\"Power\":\"%s\",\"Current\":%.1f,\"Active\":%.1f,\"Apparent\":%.1f,\"Reactive\":%.1f}",
            i ? "," : "", i + 1, app_power_state(app, i + 1),
            app->metrics.current_x10[i] / 10.0, app->metrics.active_x10[i] / 10.0,
            app->metrics.apparent_x10[i] / 10.0, app->metrics.reactive_x10[i] / 10.0);
    }
    if (used > 0 && (size_t)used + 5 < sizeof(payload)) {
        strcpy(payload + used, "]}}");
        publish_full(app, "tele", "SENSOR", payload, 0);
    }
}
