class PowerTracker {
 private:
  enum PowerState : uint8_t {
    UNKNOWN = 0,
    DOWN = 1,
    UP = 2,
    STABLE =3,
  };

  float power_;
  unsigned long power_time_;
  unsigned long power_stable_time_;
  unsigned long stable_power_timeout_;
  bool initialized_{false};
  uint16_t max_power_in_off_state_;
  PowerState power_state_;
  CallbackManager<void(float)> power_callback_{};

  unsigned long increase_count_{0};
  float increase_value_{0.0};

  unsigned long decrease_count_{0};
  float decrease_value_{0.0};
  float stable_power_{0.0};

 public:
  PowerTracker(uint16_t power_stable_time_seconds, uint16_t stable_power_timeout_seconds, uint16_t max_power_in_off_state) {
    power_time_ = millis();
    //время через которое питание считается стабильным
    power_stable_time_ = power_stable_time_seconds * 1000;
    //текущее состояние питания
    power_state_ = PowerState::UNKNOWN;
    max_power_in_off_state_ = max_power_in_off_state;
    stable_power_timeout_ = stable_power_timeout_seconds * 1000;
  }

  void add_on_power_callback(std::function<void(float)> &&callback) {
    this->power_callback_.add(std::move(callback));
  }

  bool is_initialized() { return this->initialized_; }

  void reset() { this->initialized_ = false; }

  void initialize(float power) {
    this->initialized_ = true;
    set_stable_power_(power);
  }

  bool is_power_unknown() {
    return this->power_state_ == PowerState::UNKNOWN;
  }

  bool is_power_stable() {
    return this->power_state_ == PowerState::STABLE;
  }

  bool power_on() {
    if(this->power_ > this->max_power_in_off_state_)
      return true;

    return false;
  }

  void set_power(float power) {

    if(initialized_ == false)
      return;

    if (isnan(power))
      return;

    //если значения не равны, то ставим новое значение
    if(power_ != power) {

      if(power > this->power_) {
        power_state_ = PowerState::UP;
        increase_count_ += 1;
        increase_value_ += power - power_;
      }
      else {
        power_state_ = PowerState::DOWN;
        decrease_count_ += 1;
        decrease_value_ += power_ - power;
      }

      //если суммарно питание увеличилось на 100
      if (power - stable_power_ > 100) {
        set_stable_power_(power);
        ESP_LOGD("power_tracker", "Питание увеличилось на: %.2fW, считаем его стабильным", (power - stable_power_ ));
        power_callback_.call(power);
        return;
      }

      float total_power_change = abs(increase_count_ * increase_value_ - decrease_count_ * decrease_value_);
      auto changes_count = increase_count_ + decrease_count_;

      if(changes_count > 4 && (total_power_change == 0 || total_power_change == abs(stable_power_ - power))) {
        ESP_LOGD("power_tracker", "питание нестабильно, но стабильно изменяется");
        set_stable_power_(power);
        power_callback_.call(power);
        return;
      }

      //если питание все возрастает
      if(increase_count_ > 6) {
        ESP_LOGD("power_tracker", "слишком много изменений");
        set_stable_power_(power);
        power_callback_.call(power);
        return;
      }

      power_ = power;
      power_time_ = millis();

      ESP_LOGD("power_tracker","Отслеживаем питание: %.2f", this->power_);

      return;
    }

    auto change_time = millis() - this->power_time_;

    if(power_state_ == PowerState::STABLE) {

      if(change_time >= this->stable_power_timeout_) {
        ESP_LOGD("power_tracker","Питание %.2fW стабильное, проверка состояния", this->power_);
        power_time_ = millis();
        power_callback_.call(power);
      }
      return;
    }

    //т.е. состояние конечное, не было изменений в течении power_stable_time_ секунд
    if(change_time >= power_stable_time_) {
      ESP_LOGD("power_tracker", "Питание %.2fW стабильное", this->power_);
      set_stable_power_(power);
      power_callback_.call(power);
    }
  }

 private:
  void set_stable_power_(float power) {

    ESP_LOGD("power_tracker", "stable_power: %.2f, increase_count: %ld, increase_value: %.2f, decrease_count: %ld, decrease_value: %.2f, new stable power: %.2f",
             stable_power_,
             increase_count_,
             increase_value_,
             decrease_count_,
             decrease_value_,
             power);

    this->power_state_ = PowerState::STABLE;
    this->power_time_ = millis();
    this->stable_power_ = power;
    this->power_ = power;

    this->increase_count_ = 0;
    this->increase_value_ = 0.0;
    this->decrease_count_ = 0;
    this->decrease_value_ = 0.0;
  }
};