#pragma once

#include "esphome.h"

namespace mqtt_climate {

static const char *TAG = "daikin.climate";

class DaikinClimateComponent : public MQTTComponent {
 private:
  //управление режимом
  std::string mode_command_topic_;
  //получение всех данных
  std::string info_topic_;
  //управление температурой
  std::string temperature_command_topic_;
  //режим вентилятора
  std::string fan_mode_command_topic_;
  //режим шторок
  std::string swing_mode_command_topic_;
  //кнопка sleep
  std::string sleep_command_topic_;

  ir_climate::IRDaikin* ir_climate_;
  std::string current_temperature_topic_;
  std::string current_temperature_field_;

  float power_{NAN};
  bool prev_resend_state_{false};
  bool discovery_topic_sended_{false};
  bool init_state_from_retain_message_{true};
  bool initialized_{false};
  bool setup_initialized_{false};
  unsigned long initialize_started_at_{0};
  std::string name_;
  PowerTracker* power_tracker_{nullptr};
  sensor::Sensor* power_sensor_{nullptr};

 public:
  DaikinClimateComponent(uint16_t receiver_pin, uint16_t transmitter_pin, const std::string &name) {
    ir_climate_ = new ir_climate::IRDaikin(receiver_pin, transmitter_pin);
    name_ = name;
    power_tracker_ = new PowerTracker(20, 10, 20);

    //доавляем callback, который будет вызван если значение питания не меняется в течении заданного отрезка времения
    power_tracker_->add_on_power_callback([this](float state) { power_stable_callback_(state); });

    //доавляем callback, вызывается при считывании данных с пульта
    ir_climate_->add_on_state_callback([this]() { this->power_tracker_->reset(); this->publish_state_(); });

    auto sanitized_name = get_sanitized_name_();
    mode_command_topic_ = sanitized_name + "/m/c";
    info_topic_ = sanitized_name + "/i";
    temperature_command_topic_ = sanitized_name + "/t/c";
    fan_mode_command_topic_ = sanitized_name + "/f/c";
    swing_mode_command_topic_ = sanitized_name + "/s/c";
    sleep_command_topic_ = sanitized_name + "/sleep/set";
  }

  void send_discovery(JsonObject &root, SendDiscoveryConfig &config) override {}

  bool send_initial_state() override { delay(500); return this->publish_state_(); }

  bool is_internal() override { return false; }

  std::string component_type() const override { return "climate"; }

  void set_power_sensor(sensor::Sensor *sensor) { this->power_sensor_ = sensor; }

  void set_current_temperature_sensor(std::string topic, std::string field) {
    this->current_temperature_topic_ = topic;
    this->current_temperature_field_ = field;
  }

  void setup() override {
    ir_climate_->setup();

    this->subscribe(this->mode_command_topic_, [this](const std::string &topic, const std::string &payload) {
      ESP_LOGD(TAG, "mode_command_topic: %s", payload.c_str());
      this->power_tracker_->reset();
      ir_climate_->set_hvac_mode(payload);
      ir_climate_->send();
      this->publish_state_();
    });

    this->subscribe(this->temperature_command_topic_, [this](const std::string &topic, const std::string &payload) {
      ESP_LOGD(TAG, "temperature_command_topic: %s", payload.c_str());
      auto val = parse_float(payload);

      if (!val.has_value()) {
        ESP_LOGW(TAG, "Can't convert '%s' to number!", payload.c_str());
        return;
      }

      uint8_t temp = static_cast<uint8_t>(*val);

      if(ir_climate_->set_temp(temp) == true)
        ir_climate_->send();

      this->publish_state_();
    });

    this->subscribe(this->fan_mode_command_topic_, [this](const std::string &topic, const std::string &payload) {
      ESP_LOGD(TAG, "fan_mode_command_topic: %s", payload.c_str());
      if(ir_climate_->set_fan(payload) == true)
        ir_climate_->send();

      this->publish_state_();
    });

    this->subscribe(this->swing_mode_command_topic_, [this](const std::string &topic, const std::string &payload) {
      ESP_LOGD(TAG, "swing_mode_command_topic: %s", payload.c_str());
      ir_climate_->set_swing_mode(payload);
      ir_climate_->send();
      this->publish_state_();
    });

    this->subscribe(this->sleep_command_topic_, [this](const std::string &topic, const std::string &payload) {
      ESP_LOGD(TAG, "sleep_command_topic: %s", payload.c_str());
      if(ir_climate_->set_sleep(payload))
        ir_climate_->send();

      this->publish_state_();
    });

    //инициализация начального состояния из последнего отправленного сообщения
    this->subscribe_json(this->info_topic_, [this](const std::string &topic, JsonObject &root) {
      if(this->init_state_from_retain_message_ == false)
        return;

      if(root.success() == false) {
        this->init_state_from_retain_message_ = false;
        ESP_LOGW(TAG, "Parsing error, skipping initialization from retain state message");
        return;
      }

      const char* hvac_mode_str = root["hvac"];
      const char* fan_mode_str = root["fm"];
      const char* swing_mode_str = root["sm"];
      uint8_t temp = root["t"];
      bool sleep = root["attrs"]["sleep"];

      const char* prev_fan_mode = root["attrs"]["prev_fan_mode"] | "";
      const char* mode_str = root["attrs"]["mode"] | "";

      this->ir_climate_->initialize(hvac_mode_str, mode_str, fan_mode_str, prev_fan_mode, swing_mode_str, temp, sleep);

      if(this->power_tracker_->is_initialized()) {
        power_stable_callback_(power_);
        this->power_tracker_->reset();
      }

      this->init_state_from_retain_message_ = false;

      ESP_LOGD(TAG, "Last state successfully restored: %s", this->ir_climate_->to_string());
    });

    if(this->power_sensor_ != nullptr)
      this->power_sensor_->add_on_raw_state_callback([this](float power) { update_power_(power); });
  }

  void call_setup() override {

    if (this->is_internal())
      return;

    global_mqtt_client->register_mqtt_component(this);

    setup_initialized_ = false;
    this->schedule_resend_state();
  }

  void call_loop() override {

    if (this->is_internal())
      return;

    //При дисконнекте от mqtt он при новом подключении дергает метод this->schedule_resend_state()
    //нужно пониять когда это произошло и переинициализировать плаги, для этого добавил prev_resend_state_
    if(this->is_connected_() == false)
      return;

    if(this->prev_resend_state_ == false && this->resend_state_ == true) {
      //Если была запрошене переинициализация, например отвалился mqtt
      this->initialize_started_at_ = millis();
      this->init_state_from_retain_message_ = true;
      this->discovery_topic_sended_ = false;
      this->power_tracker_->reset();

      if(setup_initialized_ == false) {
        this->setup();
        setup_initialized_ = true;
      }

      ESP_LOGI(TAG, "Сбрасываем состояние");
    }

    this->loop();

    if (this->resend_state_ == false)
      return;

    this->prev_resend_state_ = this->resend_state_;

    if(this->init_state_from_retain_message_) {
      auto state_initialization_time = (millis() - this->initialize_started_at_);
      if(state_initialization_time > 5000) {
        float time = (float)state_initialization_time * 0.001;
        ESP_LOGW(TAG, "Time out, passed: %.2fs, initialize state from the default ac settings", time);
        this->init_state_from_retain_message_ = false;
      }

      return;
    }

    //задержка отправки discovery
    if((millis() - initialize_started_at_) < 5000)
      return;

    this->resend_state_ = false;

    if (this->is_discovery_enabled() && this->discovery_topic_sended_ == false) {
      this->discovery_topic_sended_ = this->send_auto_discovery_();
      if (this->discovery_topic_sended_ == false) {
        this->schedule_resend_state();
        ESP_LOGW(TAG, "sending auto discovery topic failed");
      } else {
        if(this->send_initial_state() == false) {
          ESP_LOGW(TAG,"sending initial state data failed");
          this->schedule_resend_state();
        }
      }
    }
    else {
      if(this->send_initial_state() == false)
        this->schedule_resend_state();
    }

    if(this->resend_state_ == false && (this->discovery_topic_sended_ == true || this->is_discovery_enabled() == false)) {
      this->initialized_ = true;
      this->prev_resend_state_ = this->resend_state_;
      ESP_LOGI(TAG, "Initial state initialized, auto discovery topic sended");
    }
  }

  void loop() override {
    //ждем полной инициализации плагина, отправку автодискавери и начального состояния
    if(this->initialized_ == false)
      return;

    ir_climate_->loop();

    //инициализация отслеживания питания
    if(this->power_tracker_->is_initialized() == false && isnan(this->power_) == false) {
      this->power_tracker_->initialize(this->power_);
      ESP_LOGD(TAG, "[power_tracker] initialize with power: %.2f", this->power_);
    }

    //установка текущего питания
    this->power_tracker_->set_power(this->power_);

    yield();
  }

 protected:
  std::string friendly_name() const override { return this->name_; }
 private:
  std::string get_sanitized_name_() { return sanitize_string_whitelist(this->name_, HOSTNAME_CHARACTER_WHITELIST); }

  bool send_auto_discovery_() {

    auto const &discovery_info = global_mqtt_client->get_discovery_info();

    auto const discovery_topic = discovery_info.prefix + "/" + this->component_type() + "/" + get_sanitized_name_() + "/config";

    if (discovery_info.clean) {
      ESP_LOGV(TAG, "'%s': Cleaning discovery...", this->friendly_name().c_str());
      return global_mqtt_client->publish(discovery_topic, "", 0, 0, true);
    }

    return this->publish_json(discovery_topic, [this](JsonObject &root) {

      SendDiscoveryConfig config;
      config.state_topic = false;
      config.command_topic = false;

      std::string name = this->friendly_name();
      const std::string &node_name = App.get_name();
      std::string unique_id = this->unique_id();

      JsonObject &device_info = root.createNestedObject("device");

      JsonArray &fan_modes = root.createNestedArray("fan_modes");
      for (const char* fan_mode_str : this->ir_climate_->fan_modes_str)
        fan_modes.add(fan_mode_str);

      if(this->current_temperature_topic_.empty() == false) {
        root["curr_temp_t"] = this->current_temperature_topic_;
        root["curr_temp_tpl"]= "{{value_json." + this->current_temperature_field_ + "}}";
      }

      root["mode_cmd_t"] = this->mode_command_topic_;
      root["mode_stat_t"] = this->info_topic_;
      root["mode_stat_tpl"] = "{{value_json.hvac}}";

      JsonArray &modes = root.createNestedArray("modes");

      for (auto mode_str : this->ir_climate_->modes_str)
        modes.add(mode_str);

      JsonArray &swing_modes = root.createNestedArray("swing_modes");
      for (auto swing_mode_str : this->ir_climate_->swing_modes_str)
        swing_modes.add(swing_mode_str);

      root["temp_cmd_t"] = this->temperature_command_topic_;
      root["temp_stat_t"] = this->info_topic_;
      root["temp_stat_tpl"] = "{{value_json.t}}";

      root["min_temp"] = ir_climate_->temp_min;
      root["max_temp"] = ir_climate_->temp_max;
      root["temp_step"] = ir_climate_->temp_step;
      root["fan_mode_cmd_t"] = this->fan_mode_command_topic_;
      root["fan_mode_stat_t"] = this->info_topic_;
      root["fan_mode_stat_tpl"] = "{{value_json.fm}}";
      root["swing_mode_cmd_t"] = this->swing_mode_command_topic_;
      root["swing_mode_stat_t"] = this->info_topic_;
      root["swing_mode_stat_tpl"] = "{{value_json.sm}}";
      root["json_attr_t"] = this->info_topic_;
      root["json_attr_tpl"] = "{{value_json.attrs|tojson}}";
      root["name"] = name;

      device_info["ids"] = get_mac_address();
      device_info["name"] = node_name;
      device_info["sw"] = ESPHOME_VERSION;
      device_info["mf"] = "espressif";

      if (unique_id.empty() == false) {
        root["uniq_id"] = unique_id;
      } else {
        root["uniq_id"] = "ESP_" + this->get_default_object_id_();
      }

      if (this->availability_ == nullptr) {
        if (!global_mqtt_client->get_availability().topic.empty()) {
          root["avty_t"] = global_mqtt_client->get_availability().topic;
          if (global_mqtt_client->get_availability().payload_available != "online")
            root["pl_avail"] = global_mqtt_client->get_availability().payload_available;
          if (global_mqtt_client->get_availability().payload_not_available != "offline")
            root["pl_not_avail"] = global_mqtt_client->get_availability().payload_not_available;
        }
      } else if (!this->availability_->topic.empty()) {
        root["avty_t"] = this->availability_->topic;
        if (this->availability_->payload_available != "online")
          root["pl_avail"] = this->availability_->payload_available;
          if (this->availability_->payload_not_available != "offline")
            root["pl_not_avail"] = this->availability_->payload_not_available;
      }
    });
  }

  void power_stable_callback_(float power) {
    auto hvac_mode = this->ir_climate_->get_hvac_mode();
    //Определеяем по потребеления кондиционера включен ли он
    auto sensor_power_on = this->power_tracker_->power_on();

    //Текущее состояние кондиционера
    auto current_power_on = hvac_mode != ir_climate::AC_MODE::MODE_OFF;

    //Если состояния синхранизиованы, то выходим
    if(sensor_power_on == current_power_on)
      return;

    //если по нагрузке кондиционер выключен, а по состоянию выключен
    if(sensor_power_on == false && current_power_on == true) {
      ESP_LOGW(TAG, "[power_tracker] sending off state; [current power is %.2f]", power);
      this->ir_climate_->set_power_state(false);
      this->publish_state_();
      return;
    }

    //если по нагрузке включен, а по состоянию выключен, то пошлем сигнал на выключение
    if(sensor_power_on == true  && current_power_on == false) {
      ESP_LOGW(TAG, "[power_tracker] current power state is on, restore state; [current power is %.2fW]", power);
      this->ir_climate_->set_power_state(true);
      this->publish_state_();
      return;
    }
  }

  void update_power_(float power) {
    if(isnan(power))
      return;

    this->power_ = power;

    ESP_LOGD("update_power_","power is %.2f", power);

    this->power_tracker_->set_power(this->power_);
  }

  bool publish_state_() {

    auto success = this->publish_json(this->info_topic_, [this](JsonObject &root) {

      const char *hvac_mode_str = ir_climate_->get_hvac_mode_str();
      const char *fan_mode_str = ir_climate_->get_fan_str();
      const char *swing_mode_str = ir_climate_->get_swing_mode_str();
      auto temp = ir_climate_->get_temp();
      auto temp_allowed = ir_climate_->set_temp_allowed();
      auto sleep = ir_climate_->get_sleep();
      auto sleep_allowed = ir_climate_->sleep_allowed();
      auto prev_fan_mode = ir_climate_->get_prev_fan_str();
      root["hvac"] = hvac_mode_str;
      root["fm"] = fan_mode_str;
      root["t"] = temp;
      root["sm"] = swing_mode_str;

      JsonObject &attributes = root.createNestedObject("attrs");
      JsonArray &fan_modes_al = attributes.createNestedArray("fan_modes_al");

      attributes["sleep"] = sleep;
      attributes["sleep_al"] = sleep_allowed;
      //возможность менять температуру
      attributes["set_temp_al"] = temp_allowed;
      attributes["prev_fan_mode"] = prev_fan_mode;
      attributes["mode"] = ir_climate_->get_mode_str();

      for (auto fan_mode : ir_climate_->fan_modes) {
        if(ir_climate_->is_fan_mode_supported(fan_mode))
          fan_modes_al.add(ir_climate::IRDaikin::fan_mode_to_str(fan_mode));
      }

    });

    ESP_LOGD(TAG, "%s publish state: [%s]", success ? "success" : "failed", ir_climate_->to_string());

    return success;
  }
};

}  // namespace mqtt_climate