#pragma once

#include "esphome.h"

namespace mqtt_subscribe_json {

static const char *TAG =  "mqtt_json_subscribe.sensor";

class MQTTSubscribeJsonSensor : public sensor::Sensor, public Component {
 private:
  std::string sensor_value_filed_;
  std::string topic_;
 public:
  void set_topic(const std::string &topic, const std::string &filed) { topic_ = topic; sensor_value_filed_ = filed; }

  void setup() override {
    mqtt::global_mqtt_client->subscribe_json(this->topic_, [this](const std::string &topic, JsonObject &payload) { update_sensor_value_(payload); }, this->qos_);
  }

  void set_parent(mqtt::MQTTClientComponent *parent) { parent_ = parent; }

  void set_qos(uint8_t qos) { this->qos_ = qos; }

  void dump_config() {
    LOG_SENSOR("", "MQTT Subscribe", this);
    ESP_LOGCONFIG(TAG, "  Topic: %s, Field: %s", this->topic_.c_str(), this->sensor_value_filed_.c_str());
  }

  float get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

 private:
  void update_sensor_value_(JsonObject &root) {
    //Можно сделать разбивку по точкам, вытягивать разный уроверь вложенности данных, но тут это не требуется
    if(root.containsKey(this->sensor_value_filed_) == false)
      return;

    float value = root[this->sensor_value_filed_] | NAN;

    if(isnan(value) == false)
      this->publish_state(value);
  }

 protected:
  mqtt::MQTTClientComponent *parent_;
  uint8_t qos_{0};
};

}  // namespace mqtt_subscribe_json