#include "mp60d.h"

#include <stdio.h>
#include <string.h>

static int device_block(App *app, char *output, size_t size)
{
    int n = snprintf(output, size,
        ",\"availability_topic\":\"tele/%s/LWT\",\"payload_available\":\"Online\","
        "\"payload_not_available\":\"Offline\",\"device\":{\"identifiers\":[\"mp60_%s\"],"
        "\"name\":\"MP60 %s\",\"manufacturer\":\"UnDatto\",\"model\":\"MP60 Powerstrip\","
        "\"sw_version\":\"%s\"}}",
        app->config.mqtt_topic, app->config.mqtt_topic, app->config.mqtt_topic, MP60_VERSION);
    return n > 0 && (size_t)n < size ? 0 : -1;
}

static int switch_discovery(App *app, int outlet)
{
    char payload[2048], topic[160];
    int used = snprintf(payload, sizeof(payload),
        "{\"name\":\"Outlet %d\",\"unique_id\":\"mp60_%s_power%d\","
        "\"state_topic\":\"stat/%s/POWER%d\",\"command_topic\":\"cmnd/%s/POWER%d\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"state_on\":\"ON\",\"state_off\":\"OFF\"",
        outlet, app->config.mqtt_topic, outlet, app->config.mqtt_topic, outlet,
        app->config.mqtt_topic, outlet);
    if (used < 0 || (size_t)used >= sizeof(payload) ||
        device_block(app, payload + used, sizeof(payload) - (size_t)used))
        return -1;
    snprintf(topic, sizeof(topic), "homeassistant/switch/%s_power%d/config",
             app->config.mqtt_topic, outlet);
    return publish_topic(app, topic, payload, 1);
}

static int sensor_discovery(App *app, const char *object, const char *name,
                            const char *device_class, const char *unit, const char *value_template)
{
    char payload[2048], topic[160];
    int used = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"mp60_%s_%s\",\"state_topic\":\"tele/%s/SENSOR\","
        "\"value_template\":\"%s\",\"device_class\":\"%s\",\"unit_of_measurement\":\"%s\","
        "\"state_class\":\"measurement\"",
        name, app->config.mqtt_topic, object, app->config.mqtt_topic, value_template, device_class, unit);
    if (used < 0 || (size_t)used >= sizeof(payload) ||
        device_block(app, payload + used, sizeof(payload) - (size_t)used))
        return -1;
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s/config",
             app->config.mqtt_topic, object);
    return publish_topic(app, topic, payload, 1);
}

int mqtt_ha_discovery(App *app)
{
    int i;
    if (sensor_discovery(app, "voltage", "Voltage", "voltage", "V", "{{ value_json.ENERGY.Voltage }}") ||
        sensor_discovery(app, "frequency", "Frequency", "frequency", "Hz", "{{ value_json.ENERGY.Frequency }}") ||
        sensor_discovery(app, "current", "Total current", "current", "A", "{{ value_json.ENERGY.Current }}") ||
        sensor_discovery(app, "power", "Total power", "power", "W", "{{ value_json.ENERGY.Power }}"))
        return -1;
    for (i = 0; i < MP60_OUTLETS; i++) {
        char object[32], name[32], value_template[128];
        if (switch_discovery(app, i + 1))
            return -1;
        snprintf(object, sizeof(object), "power%d_current", i + 1);
        snprintf(name, sizeof(name), "Outlet %d current", i + 1);
        snprintf(value_template, sizeof(value_template), "{{ value_json.MP60.Outlets[%d].Current }}", i);
        if (sensor_discovery(app, object, name, "current", "A", value_template))
            return -1;
        snprintf(object, sizeof(object), "power%d_power", i + 1);
        snprintf(name, sizeof(name), "Outlet %d power", i + 1);
        snprintf(value_template, sizeof(value_template), "{{ value_json.MP60.Outlets[%d].Active }}", i);
        if (sensor_discovery(app, object, name, "power", "W", value_template))
            return -1;
    }
    return 0;
}
