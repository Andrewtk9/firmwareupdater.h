#include "platform/esp32/EspMqttClient.h"

#if FWUP_TARGET_ESP32

// esp_crt_bundle.h is not on the Arduino include path, but the symbol is
// exported from libmbedtls.a, so the prototype is declared here instead of
// forcing every consuming project to add the directory to build_flags.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
#include <stdio.h>
#include <string.h>

namespace campodata {

EspMqttClient::~EspMqttClient() {
    end();
}

bool EspMqttClient::begin(const MqttSessionConfig& cfg) {
    if (_client != nullptr) end();
    if (cfg.host == nullptr || cfg.host[0] == '\0') return false;

    esp_mqtt_client_config_t mc = {};
    mc.host      = cfg.host;
    mc.port      = cfg.port;
    mc.client_id = cfg.client_id;
    mc.username  = cfg.username;
    mc.password  = cfg.password;
    mc.transport = cfg.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;

    mc.keepalive              = cfg.keepalive_s;
    mc.disable_clean_session  = cfg.clean_session ? 0 : 1;
    mc.buffer_size            = cfg.buffer_bytes;
    mc.disable_auto_reconnect = false;

    if (cfg.will_topic != nullptr && cfg.will_payload != nullptr) {
        mc.lwt_topic   = cfg.will_topic;
        mc.lwt_msg     = cfg.will_payload;
        mc.lwt_msg_len = static_cast<int>(strlen(cfg.will_payload));
        mc.lwt_qos     = cfg.will_qos;
        mc.lwt_retain  = cfg.will_retain ? 1 : 0;
    }

    if (cfg.tls) {
        if (cfg.ca_pem != nullptr) {
            mc.cert_pem = cfg.ca_pem;
        } else if (cfg.verify_ca) {
            // Validates against the CA bundle compiled into the framework, so
            // no certificate has to be embedded.
            mc.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    _client = esp_mqtt_client_init(&mc);
    if (_client == nullptr) return false;

    if (esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, onEvent, this) != ESP_OK) {
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        return false;
    }

    if (esp_mqtt_client_start(_client) != ESP_OK) {
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        return false;
    }

    _started = true;
    return true;
}

void EspMqttClient::end() {
    if (_client == nullptr) return;

    if (_started) esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);

    _client     = nullptr;
    _started    = false;
    _connected  = false;
    _assembling = false;
    _ring.clear();
}

bool EspMqttClient::subscribe(const char* topic, uint8_t qos) {
    if (_client == nullptr || topic == nullptr || !_connected) return false;
    return esp_mqtt_client_subscribe(_client, topic, qos) >= 0;
}

bool EspMqttClient::publish(const char* topic, const uint8_t* payload, size_t len,
                            uint8_t qos, bool retain) {
    if (_client == nullptr || topic == nullptr || !_connected) return false;

    const int id = esp_mqtt_client_publish(_client, topic,
                                           reinterpret_cast<const char*>(payload),
                                           static_cast<int>(len), qos, retain ? 1 : 0);
    return id >= 0;
}

bool EspMqttClient::poll(char* topic, size_t topic_cap,
                         uint8_t* payload, size_t payload_cap, size_t& len) {
    len = 0;

    Ring::Message msg;
    if (!_ring.pop(msg)) return false;
    if (msg.length > payload_cap) return false;

    if (topic != nullptr && topic_cap > 0) {
        snprintf(topic, topic_cap, "%s", msg.topic);
    }
    if (payload != nullptr && msg.length > 0) {
        memcpy(payload, msg.payload, msg.length);
    }
    len = msg.length;
    return true;
}

void EspMqttClient::onEvent(void* arg, esp_event_base_t, int32_t, void* data) {
    auto* self = static_cast<EspMqttClient*>(arg);
    if (self != nullptr) self->handle(static_cast<esp_mqtt_event_handle_t>(data));
}

// Runs on the esp-mqtt task. No parsing and no allocation here: copy and leave.
void EspMqttClient::handle(esp_mqtt_event_handle_t event) {
    if (event == nullptr) return;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            _connected = true;
            break;

        case MQTT_EVENT_DISCONNECTED:
            _connected  = false;
            _assembling = false;
            break;

        case MQTT_EVENT_DATA: {
            const bool fragmented = event->total_data_len > event->data_len;

            if (!fragmented) {
                _ring.push(event->topic != nullptr ? event->topic : _assembly_topic,
                           reinterpret_cast<const uint8_t*>(event->data),
                           static_cast<size_t>(event->data_len));
                break;
            }

            // First fragment carries the topic; later ones do not.
            if (event->current_data_offset == 0) {
                _assembling    = true;
                _assembly_len  = 0;
                snprintf(_assembly_topic, sizeof(_assembly_topic), "%s",
                         event->topic != nullptr ? event->topic : "");
            }
            if (!_assembling) break;

            const size_t chunk = static_cast<size_t>(event->data_len);
            if (_assembly_len + chunk > kAssemblyBytes) {
                _assembling = false;   // too big: drop rather than deliver a partial
                break;
            }
            memcpy(_assembly + _assembly_len, event->data, chunk);
            _assembly_len += chunk;

            if (_assembly_len >= static_cast<size_t>(event->total_data_len)) {
                _ring.push(_assembly_topic, _assembly, _assembly_len);
                _assembling   = false;
                _assembly_len = 0;
            }
            break;
        }

        default:
            break;
    }
}

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
