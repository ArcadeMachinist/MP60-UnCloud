#include "mp60d.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern const char ui_html[];

#define REQUEST_CAPACITY 20480

static int send_all(int fd, const void *data, size_t length)
{
    const char *p = (const char *)data;
    while (length) {
        ssize_t n = send(fd, p, length, 0);
        if (n > 0) { p += n; length -= (size_t)n; }
        else if (n < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static void respond(int fd, int status, const char *status_text, const char *type,
                    const char *body, int authentication)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "%sConnection: close\r\n\r\n",
        status, status_text, type, (unsigned long)strlen(body),
        authentication ? "WWW-Authenticate: Basic realm=\"MP60\"\r\n" : "");
    if (n > 0 && (size_t)n < sizeof(header)) {
        send_all(fd, header, (size_t)n);
        send_all(fd, body, strlen(body));
    }
}

static void respond_json(int fd, int status, const char *status_text, const char *body)
{
    respond(fd, status, status_text, "application/json; charset=utf-8", body, 0);
}

static int header_value(const char *request, const char *name, char *output, size_t size)
{
    const char *line = strstr(request, "\r\n");
    size_t name_length = strlen(name);
    if (!line) return 0;
    line += 2;
    while (*line && strncmp(line, "\r\n", 2)) {
        const char *end = strstr(line, "\r\n");
        size_t length;
        if (!end) return 0;
        if (!strncasecmp(line, name, name_length) && line[name_length] == ':') {
            const char *value = line + name_length + 1;
            while (value < end && isspace((unsigned char)*value)) value++;
            length = (size_t)(end - value);
            if (length >= size) length = size - 1;
            memcpy(output, value, length);
            output[length] = 0;
            return 1;
        }
        line = end + 2;
    }
    return 0;
}

static int authorized(App *app, const char *request)
{
    char authorization[512], decoded[256];
    int length;
    char *colon;
    if (!app->config.ui_password_set) return 1;
    if (!header_value(request, "Authorization", authorization, sizeof(authorization)) ||
        strncasecmp(authorization, "Basic ", 6)) return 0;
    length = base64_decode(authorization + 6, (unsigned char *)decoded, sizeof(decoded) - 1);
    if (length < 0) return 0;
    decoded[length] = 0;
    colon = strchr(decoded, ':');
    if (!colon) return 0;
    return config_check_ui_password(&app->config, colon + 1);
}

static int read_request(int fd, char *buffer, size_t capacity)
{
    size_t used = 0, header_end = 0, content_length = 0;
    struct timeval timeout = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (used + 1 < capacity) {
        ssize_t n;
        if (!header_end) {
            buffer[used] = 0;
            {
                char *found = strstr(buffer, "\r\n\r\n");
                if (found) {
                    char length_text[32];
                    header_end = (size_t)(found - buffer) + 4;
                    if (header_value(buffer, "Content-Length", length_text, sizeof(length_text))) {
                        char *end;
                        unsigned long value = strtoul(length_text, &end, 10);
                        if (*length_text && !*end && value <= 16384) content_length = value;
                        else return -1;
                    }
                }
            }
        }
        if (header_end && used >= header_end + content_length)
            break;
        n = recv(fd, buffer + used, capacity - used - 1, 0);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else return -1;
    }
    if (!header_end || used < header_end + content_length)
        return -1;
    buffer[used] = 0;
    return (int)(header_end + content_length);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *input, char *output, size_t size)
{
    size_t used = 0;
    while (*input && *input != '&') {
        int c;
        if (*input == '%' && hex_digit(input[1]) >= 0 && hex_digit(input[2]) >= 0) {
            c = hex_digit(input[1]) * 16 + hex_digit(input[2]);
            input += 3;
        } else if (*input == '+') {
            c = ' '; input++;
        } else c = (unsigned char)*input++;
        if (c == 0 || used + 1 >= size) return -1;
        output[used++] = (char)c;
    }
    output[used] = 0;
    return 0;
}

static void tasmota_power_json(App *app, int outlet, char *output, size_t size)
{
    int i;
    output[0] = 0;
    if (outlet) {
        snprintf(output, size, "{\"POWER%d\":\"%s\"}", outlet, app_power_state(app, outlet));
        return;
    }
    strncat(output, "{", size - strlen(output) - 1);
    for (i = 0; i < MP60_OUTLETS; i++) {
        snprintf(output + strlen(output), size - strlen(output), "%s\"POWER%d\":\"%s\"",
                 i ? "," : "", i + 1, app_power_state(app, i + 1));
    }
    strncat(output, "}", size - strlen(output) - 1);
}

static void handle_tasmota(App *app, int fd, const char *target)
{
    const char *query = strstr(target, "?cmnd=");
    char command[160], *argument, response[8192], error[160];
    const char *word;
    int outlet = -1, action;
    if (!query || url_decode(query + 6, command, sizeof(command))) {
        respond_json(fd, 400, "Bad Request", "{\"error\":\"Missing or invalid cmnd\"}");
        return;
    }
    argument = command;
    while (*argument && !isspace((unsigned char)*argument)) argument++;
    if (*argument) *argument++ = 0;
    while (isspace((unsigned char)*argument)) argument++;
    word = command;
    if (str_ieq(word, "State") || str_ieq(word, "Status")) {
        if (app_status_json(app, response, sizeof(response)) == 0) respond_json(fd, 200, "OK", response);
        else respond_json(fd, 500, "Internal Server Error", "{\"error\":\"Status formatting failed\"}");
        return;
    }
    if (!strncasecmp(word, "Power", 5)) {
        const char *suffix = word + 5;
        char *end;
        if (!*suffix) outlet = 0;
        else {
            long number = strtol(suffix, &end, 10);
            if (*end || number < 1 || number > MP60_OUTLETS) outlet = -1;
            else outlet = (int)number;
        }
    }
    if (outlet < 0) {
        respond_json(fd, 400, "Bad Request", "{\"error\":\"Supported commands: State, Power[1-6]\"}");
        return;
    }
    if (!*argument) {
        tasmota_power_json(app, outlet, response, sizeof(response));
        respond_json(fd, 200, "OK", response);
        return;
    }
    action = parse_on_off(argument, outlet != 0);
    if (action < 0 || app_set_outlet(app, outlet, action, error, sizeof(error))) {
        snprintf(response, sizeof(response), "{\"error\":\"%s\"}", action < 0 ? "Invalid power state" : error);
        respond_json(fd, 400, "Bad Request", response);
        return;
    }
    tasmota_power_json(app, outlet, response, sizeof(response));
    respond_json(fd, 200, "OK", response);
}

static void handle_request(App *app, int fd, char *request)
{
    char method[8], target[1024], response[8192], error[256];
    char *body, *header_end;
    int outlet, action;
    if (sscanf(request, "%7s %1023s", method, target) != 2) {
        respond_json(fd, 400, "Bad Request", "{\"error\":\"Malformed request\"}");
        return;
    }
    if (!authorized(app, request)) {
        respond(fd, 401, "Unauthorized", "application/json; charset=utf-8",
                "{\"error\":\"Authentication required\"}", 1);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(target, "/")) {
        respond(fd, 200, "OK", "text/html; charset=utf-8", ui_html, 0);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/api/status")) {
        if (app_status_json(app, response, sizeof(response)) == 0) respond_json(fd, 200, "OK", response);
        else respond_json(fd, 500, "Internal Server Error", "{\"error\":\"Status formatting failed\"}");
    } else if (!strcmp(method, "GET") && !strcmp(target, "/api/config")) {
        if (app_config_json(app, response, sizeof(response)) == 0) respond_json(fd, 200, "OK", response);
        else respond_json(fd, 500, "Internal Server Error", "{\"error\":\"Config formatting failed\"}");
    } else if (!strcmp(method, "GET") && !strcmp(target, "/api/wifi")) {
        if (wifi_status_json(app, response, sizeof(response)) == 0) respond_json(fd, 200, "OK", response);
        else respond_json(fd, 500, "Internal Server Error", "{\"error\":\"Wi-Fi status formatting failed\"}");
    } else if (!strcmp(method, "GET") && !strcmp(target, "/api/wifi/scan")) {
        if (wifi_scan_json(app, response, sizeof(response)) == 0) respond_json(fd, 200, "OK", response);
        else respond_json(fd, 500, "Internal Server Error", "{\"error\":\"Wi-Fi scan failed\"}");
    } else if (!strcmp(method, "GET") && !strncmp(target, "/cm?", 4)) {
        handle_tasmota(app, fd, target);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/api/config")) {
        header_end = strstr(request, "\r\n\r\n");
        body = header_end ? header_end + 4 : NULL;
        if (!body || !*body) respond_json(fd, 400, "Bad Request", "{\"error\":\"JSON body required\"}");
        else if (app_update_config(app, body, error, sizeof(error)) == 0)
            respond_json(fd, 200, "OK", "{\"result\":\"Configuration saved\"}");
        else {
            snprintf(response, sizeof(response), "{\"error\":\"");
            if (!json_append(response, sizeof(response), error) && strlen(response) + 3 < sizeof(response)) {
                strcat(response, "\"}");
                respond_json(fd, 400, "Bad Request", response);
            }
        }
    } else if (!strcmp(method, "POST") && !strcmp(target, "/api/wifi")) {
        header_end = strstr(request, "\r\n\r\n");
        body = header_end ? header_end + 4 : NULL;
        if (!body || !*body) {
            respond_json(fd, 400, "Bad Request", "{\"error\":\"JSON body required\"}");
        } else if (wifi_update_config(app, body, error, sizeof(error)) == 0) {
            respond_json(fd, 200, "OK", "{\"result\":\"Wi-Fi configuration saved\"}");
        } else {
            snprintf(response, sizeof(response), "{\"error\":\"");
            if (!json_append(response, sizeof(response), error) && strlen(response) + 3 < sizeof(response)) {
                strcat(response, "\"}");
                respond_json(fd, 400, "Bad Request", response);
            }
        }
    } else if (!strcmp(method, "POST") && !strncmp(target, "/api/outlets/", 13)) {
        const char *suffix = target + 13;
        char type, state[24];
        header_end = strstr(request, "\r\n\r\n");
        body = header_end ? header_end + 4 : NULL;
        if (!strcmp(suffix, "all")) outlet = 0;
        else {
            char *end;
            long number = strtol(suffix, &end, 10);
            if (*end || number < 1 || number > MP60_OUTLETS) outlet = -1;
            else outlet = (int)number;
        }
        if (outlet < 0 || !body || json_field(body, "state", &type, state, sizeof(state)) != 1 || type != 's') {
            respond_json(fd, 400, "Bad Request", "{\"error\":\"Expected {\\\"state\\\":\\\"on|off|toggle\\\"}\"}");
            return;
        }
        action = parse_on_off(state, outlet != 0);
        if (action < 0) respond_json(fd, 400, "Bad Request", "{\"error\":\"Invalid state\"}");
        else if (app_set_outlet(app, outlet, action, error, sizeof(error))) {
            snprintf(response, sizeof(response), "{\"error\":\"");
            json_append(response, sizeof(response), error);
            strncat(response, "\"}", sizeof(response) - strlen(response) - 1);
            respond_json(fd, 503, "Service Unavailable", response);
        } else if (app_status_json(app, response, sizeof(response)) == 0)
            respond_json(fd, 200, "OK", response);
    } else if (!strcmp(method, "GET")) {
        respond_json(fd, 404, "Not Found", "{\"error\":\"Not found\"}");
    } else {
        respond_json(fd, 405, "Method Not Allowed", "{\"error\":\"Method not allowed\"}");
    }
}

int http_restart(App *app)
{
    int fd, yes = 1;
    struct sockaddr_in address;
    if (app->http_fd >= 0) {
        close(app->http_fd);
        app->http_fd = -1;
    }
    if (!app->config.http_enabled) return 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(app->config.http_port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 4)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    app->http_fd = fd;
    log_msg("info", "HTTP listening on port %u", (unsigned)app->config.http_port);
    return 0;
}

void http_poll(App *app)
{
    fd_set readers;
    struct timeval timeout = {0, 1000};
    int result;
    if (app->http_fd < 0) return;
    FD_ZERO(&readers);
    FD_SET(app->http_fd, &readers);
    result = select(app->http_fd + 1, &readers, NULL, NULL, &timeout);
    if (result > 0 && FD_ISSET(app->http_fd, &readers)) {
        int client = accept(app->http_fd, NULL, NULL);
        if (client >= 0) {
            char *request = malloc(REQUEST_CAPACITY);
            if (request) {
                if (read_request(client, request, REQUEST_CAPACITY) > 0)
                    handle_request(app, client, request);
                free(request);
            }
            shutdown(client, SHUT_RDWR);
            close(client);
        }
    }
}
