#include "mp60d.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MP60_GPIO_CHIP "/dev/gpiochip0"
#define MP60_GPIO_SDA 14
#define MP60_GPIO_SCL 11
#define MP60_I2C_ADDRESS 0x5c
#define MP60_I2C_DELAY_US 5

#define LINE_SDA (1U << 0)
#define LINE_SCL (1U << 1)
#define LINE_BOTH (LINE_SDA | LINE_SCL)

typedef struct {
    int fd;
} Controller;

uint8_t mp60_xor(const uint8_t *data, size_t length)
{
    uint8_t result = 0;
    while (length--)
        result ^= *data++;
    return result;
}

uint8_t mask_to_wire(uint8_t mask)
{
    static const uint8_t bits[MP60_OUTLETS] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04};
    uint8_t result = 0;
    int i;
    for (i = 0; i < MP60_OUTLETS; i++)
        if (mask & (1u << i))
            result |= bits[i];
    return result;
}

uint8_t wire_to_mask(uint8_t wire)
{
    static const uint8_t bits[MP60_OUTLETS] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04};
    uint8_t result = 0;
    int i;
    for (i = 0; i < MP60_OUTLETS; i++)
        if (wire & bits[i])
            result |= 1u << i;
    return result;
}

static int controller_set(Controller *controller, unsigned bits)
{
    struct gpio_v2_line_values values = { .bits = bits, .mask = LINE_BOTH };
    return ioctl(controller->fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

static int controller_get(Controller *controller, unsigned *bits)
{
    struct gpio_v2_line_values values = { .mask = LINE_BOTH };
    if (ioctl(controller->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
        return -1;
    *bits = values.bits;
    return 0;
}

static void controller_delay(void)
{
    struct timespec start, now;
    uint64_t elapsed;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ULL;
        elapsed += (uint64_t)(now.tv_nsec - start.tv_nsec);
    } while (elapsed < MP60_I2C_DELAY_US * 1000ULL);
}

static int controller_open(Controller *controller)
{
    int chip_fd;
    struct gpio_v2_line_request request;
    memset(&request, 0, sizeof(request));
    controller->fd = -1;
    chip_fd = open(MP60_GPIO_CHIP, O_RDONLY);
    if (chip_fd < 0)
        return -1;
    request.offsets[0] = MP60_GPIO_SDA;
    request.offsets[1] = MP60_GPIO_SCL;
    request.num_lines = 2;
    strcpy(request.consumer, "mp60d");
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request.config.attrs[0].attr.values = LINE_BOTH;
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        int saved = errno;
        close(chip_fd);
        errno = saved;
        return -1;
    }
    close(chip_fd);
    controller->fd = request.fd;
    return 0;
}

static int controller_start(Controller *controller)
{
    if (controller_set(controller, LINE_BOTH) < 0) return -1;
    controller_delay();
    if (controller_set(controller, LINE_SCL) < 0) return -1;
    controller_delay();
    if (controller_set(controller, 0) < 0) return -1;
    controller_delay();
    return 0;
}

static int controller_stop(Controller *controller)
{
    if (controller_set(controller, LINE_SCL) < 0) return -1;
    controller_delay();
    if (controller_set(controller, LINE_BOTH) < 0) return -1;
    controller_delay();
    return 0;
}

static int controller_write_byte(Controller *controller, uint8_t byte)
{
    int bit, ack;
    unsigned bits;
    for (bit = 7; bit >= 0; --bit) {
        unsigned value = byte & (1U << bit) ? LINE_SDA : 0;
        if (controller_set(controller, value) < 0) return -1;
        controller_delay();
        if (controller_set(controller, value | LINE_SCL) < 0) return -1;
        controller_delay();
        if (controller_set(controller, value) < 0) return -1;
        controller_delay();
    }
    if (controller_set(controller, LINE_SDA) < 0) return -1;
    controller_delay();
    if (controller_set(controller, LINE_BOTH) < 0) return -1;
    controller_delay();
    if (controller_get(controller, &bits) < 0) return -1;
    ack = (bits & LINE_SDA) == 0;
    if (controller_set(controller, LINE_SDA) < 0) return -1;
    controller_delay();
    if (!ack) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int controller_read_byte(Controller *controller, uint8_t *byte, int send_ack)
{
    int bit;
    unsigned bits;
    *byte = 0;
    if (controller_set(controller, LINE_SDA) < 0) return -1;
    for (bit = 7; bit >= 0; --bit) {
        controller_delay();
        if (controller_set(controller, LINE_BOTH) < 0) return -1;
        controller_delay();
        if (controller_get(controller, &bits) < 0) return -1;
        if (bits & LINE_SDA)
            *byte |= (uint8_t)(1U << bit);
        if (controller_set(controller, LINE_SDA) < 0) return -1;
    }
    if (controller_set(controller, send_ack ? 0 : LINE_SDA) < 0) return -1;
    controller_delay();
    if (controller_set(controller, (send_ack ? 0 : LINE_SDA) | LINE_SCL) < 0) return -1;
    controller_delay();
    if (controller_set(controller, send_ack ? 0 : LINE_SDA) < 0) return -1;
    controller_delay();
    if (controller_set(controller, LINE_SDA) < 0) return -1;
    return 0;
}

static int controller_write_request(Controller *controller, const uint8_t request[6])
{
    int i, result = -1;
    if (controller_start(controller) < 0 ||
        controller_write_byte(controller, MP60_I2C_ADDRESS << 1) < 0)
        goto out;
    for (i = 0; i < 6; ++i)
        if (controller_write_byte(controller, request[i]) < 0)
            goto out;
    result = 0;
out:
    if (controller_stop(controller) < 0)
        result = -1;
    return result;
}

static Controller persistent_controller = { .fd = -1 };

static Controller *controller_instance(void)
{
    if (persistent_controller.fd < 0 && controller_open(&persistent_controller) < 0)
        return NULL;
    return &persistent_controller;
}

int hardware_set_mask(App *app, uint8_t mask)
{
    static long long last_write_ms;
    uint8_t request[6] = {0x01, 0x06, mask_to_wire(mask), 0x00, 0x06, 0x00};
    int attempt;
    if (app->simulate)
        return 0;
    request[5] = mp60_xor(request, 5);
    for (attempt = 0; attempt < 3; attempt++) {
        Controller *controller;
        long long now = monotonic_ms();
        long long elapsed = last_write_ms ? now - last_write_ms : 1000;
        if (elapsed < 1000) {
            struct timespec delay = {
                (time_t)((1000 - elapsed) / 1000),
                (long)((1000 - elapsed) % 1000) * 1000000L
            };
            nanosleep(&delay, NULL);
        }
        last_write_ms = monotonic_ms();
        controller = controller_instance();
        if (controller && controller_write_request(controller, request) == 0) {
            struct timespec delay = {0, 250000000};
            nanosleep(&delay, NULL);
            return 0;
        }
        {
            struct timespec delay = {0, 50000000};
            nanosleep(&delay, NULL);
        }
    }
    return -1;
}

int hardware_parse_response(const uint8_t response[53], Metrics *metrics)
{
    int i;
    if (response[5] != 0x01 || response[52] != mp60_xor(response, 52)) {
        errno = EPROTO;
        return -1;
    }
    if ((response[12] && response[12] < 80) ||
        (response[13] && (response[13] < 40 || response[13] > 70))) {
        errno = ERANGE;
        return -1;
    }
    memset(metrics, 0, sizeof(*metrics));
    metrics->valid = 1;
    metrics->sampled_at = time(NULL);
    for (i = 0; i < MP60_OUTLETS; i++) {
        metrics->current_x10[i] = response[6 + i];
        metrics->apparent_x10[i] = (uint16_t)((response[14 + i * 2] << 8) | response[15 + i * 2]);
        metrics->active_x10[i] = (uint16_t)((response[26 + i * 2] << 8) | response[27 + i * 2]);
        metrics->reactive_x10[i] = (uint16_t)((response[38 + i * 2] << 8) | response[39 + i * 2]);
    }
    metrics->voltage = response[12];
    metrics->frequency = response[13];
    metrics->surge_depleted = response[50];
    metrics->reported_wire_mask = response[51];
    return 0;
}

static int hardware_read_once(Controller *controller, Metrics *metrics)
{
    static const uint8_t request[6] = {0x01, 0x05, 0x00, 0x00, 0x06, 0x02};
    uint8_t response[53];
    int i;
    if (controller_write_request(controller, request) < 0 ||
        controller_start(controller) < 0 ||
        controller_write_byte(controller, (MP60_I2C_ADDRESS << 1) | 1) < 0)
        return -1;
    for (i = 0; i < 53; ++i)
        if (controller_read_byte(controller, &response[i], i != 52) < 0)
            return -1;
    if (controller_stop(controller) < 0)
        return -1;
    return hardware_parse_response(response, metrics);
}

int hardware_read(App *app, Metrics *metrics)
{
    Controller *controller;
    int attempt;
    if (app->simulate) {
        hardware_simulate_reading(app, metrics);
        return 0;
    }
    /* Keep GPIO lines claimed between polls to avoid request/release glitches.
     * At most three short attempts replace the legacy 50-attempt retry storm;
     * the caller still applies backoff when the controller remains unavailable. */
    controller = controller_instance();
    if (!controller)
        return -1;
    for (attempt = 0; attempt < 3; ++attempt) {
        if (hardware_read_once(controller, metrics) == 0)
            return 0;
        controller_stop(controller);
        if (attempt != 2) {
            struct timespec delay = {0, 25000000};
            nanosleep(&delay, NULL);
        }
    }
    return -1;
}

void hardware_simulate_reading(App *app, Metrics *metrics)
{
    int i;
    memset(metrics, 0, sizeof(*metrics));
    metrics->valid = 1;
    metrics->sampled_at = time(NULL);
    metrics->voltage = 120;
    metrics->frequency = 60;
    metrics->reported_wire_mask = mask_to_wire(app->config.outlet_mask);
    for (i = 0; i < MP60_OUTLETS; i++) {
        if (app->config.outlet_mask & (1u << i)) {
            unsigned active = 120 + (unsigned)i * 37;
            metrics->current_x10[i] = (uint8_t)((active * 10 + 60) / 120);
            metrics->active_x10[i] = (uint16_t)(active * 10);
            metrics->apparent_x10[i] = (uint16_t)(active * 10 + 20);
            metrics->reactive_x10[i] = 20;
        }
    }
}
