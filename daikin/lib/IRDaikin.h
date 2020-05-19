#include <ir_Daikin.h>
#include <IRutils.h>

namespace ir_climate {

static const char *TAG = "ir.daikin";

enum SWING_MODE : uint8_t {
  SWING_OFF = 0,
  SWING_HORIZONTAL = 1,
};

enum FAN_MODE : uint8_t {
  FAN_UNDEFINED = 0,
  FAN_AUTO = 1,
  FAN_HIGH = 2,
  FAN_TURBO = 3, //Режим включается только кнопкой turbo
  FAN_MEDIUM = 4,
  FAN_LOW = 8,
  FAN_QUIET = 9,//Редим включается только кнопкой quiet
};

enum AC_MODE : uint8_t {
  MODE_UNDEFINED = 0,
  MODE_DRY = 1,
  MODE_COOL = 2,
  MODE_FAN = 4,
  MODE_HEAT = 8,
  MODE_AUTO = 10,
  MODE_OFF = 11,
};

class IRDaikin {
 private:
  IRDaikin64* ac_;
  IRrecv* ir_receiver_{nullptr};
  decode_results* decode_results_;
  CallbackManager<void()> state_callback_{};

  bool power_on_{false};//Индикатор питания, включен ли кондиционер
  bool set_temp_enabled_{false};
  bool turbo_enabled_{false};
  bool quiet_enabled_{false};
  bool sleep_enabled_{false};
  FAN_MODE prev_fan_mode_{FAN_MODE::FAN_MEDIUM};

 public:
  const uint8_t temp_min = 16;
  const uint8_t temp_max = 30;
  const uint8_t temp_step = 1;

  const char* modes_str[6] = {
      mode_to_str(AC_MODE::MODE_OFF),
      mode_to_str(AC_MODE::MODE_HEAT),
      mode_to_str(AC_MODE::MODE_AUTO),
      mode_to_str(AC_MODE::MODE_COOL),
      mode_to_str(AC_MODE::MODE_DRY),
      mode_to_str(AC_MODE::MODE_FAN)};

  const FAN_MODE fan_modes[6] = {
      FAN_MODE::FAN_AUTO,
      FAN_MODE::FAN_QUIET,
      FAN_MODE::FAN_LOW,
      FAN_MODE::FAN_MEDIUM,
      FAN_MODE::FAN_HIGH,
      FAN_MODE::FAN_TURBO};

  const char* fan_modes_str[6] = {
      fan_mode_to_str(FAN_MODE::FAN_AUTO),
      fan_mode_to_str(FAN_MODE::FAN_QUIET),
      fan_mode_to_str(FAN_MODE::FAN_LOW),
      fan_mode_to_str(FAN_MODE::FAN_MEDIUM),
      fan_mode_to_str(FAN_MODE::FAN_HIGH),
      fan_mode_to_str(FAN_MODE::FAN_TURBO)};

  const char* swing_modes_str[2] = {
      swing_mode_to_str(SWING_MODE::SWING_OFF),
      swing_mode_to_str(SWING_MODE::SWING_HORIZONTAL)};

  IRDaikin(uint16_t receiver_pin, uint16_t transmitter_pin) {
    ac_ = new IRDaikin64(transmitter_pin);
    ir_receiver_ = new IRrecv(receiver_pin, 140, 80, true);
    ir_receiver_->setTolerance(50);
    ac_->setPowerToggle(false);

    //инициализация констрейнтов
    set_mode(get_mode());
    decode_results_ = new decode_results();
  }

 public:

  void setup() const {
    ir_receiver_->enableIRIn();
    ac_->begin();
  }

  void add_on_state_callback(std::function<void()>&& callback) { this->state_callback_.add(std::move(callback)); }

  void send() const {
    ir_receiver_->disableIRIn();
    ac_->send();
    ac_->setPowerToggle(false);//после отправки сбрасываем бит питания
    ESP_LOGD(TAG, "[send]: %s", this->to_string());
    ir_receiver_->enableIRIn();
  }

  void set_power_state(const bool on) { this->power_on_ = on; }

  bool get_power_state() const { return this->power_on_; }

  void toggle_power() {
    //изменение состояния питания
    this->ac_->setPowerToggle(true);

    auto power = get_power_state();
    auto new_power = !power;

    ESP_LOGD(TAG, "[toggle_power]: setPowerToggle: true, было power_on: %s, стало power_on: %s", bool_to_str_(power), bool_to_str_(new_power));

    set_power_state(new_power);
  }

  bool set_temp(const uint8_t temp) {

    if (temp < this->temp_min || temp > this->temp_max)
      return false;

    this->ac_->setTemp(temp);

    if(this->set_temp_enabled_ == false)
      return false;

    return true;
  }

  uint8_t get_temp() const { return this->ac_->getTemp(); }

  bool set_temp_allowed() const { return this->set_temp_enabled_; }

  void set_hvac_mode(const AC_MODE mode) {
    set_mode(mode);

    if(mode != AC_MODE::MODE_OFF && get_power_state() == false) {
      toggle_power();
    }

    if(mode == AC_MODE::MODE_OFF && get_power_state() == true) {
      toggle_power();
    }
  }

  void set_hvac_mode(const std::string& mode_str) {
    auto mode = parse_mode(mode_str);

    if (mode == AC_MODE::MODE_UNDEFINED)
      ESP_LOGW(TAG, "[set_hvac_mode]: Unrecognized mode %s", mode_str.c_str());
    else
      set_hvac_mode(mode);
  }

  AC_MODE get_hvac_mode() const {
    if(get_power_state() == false)
      return AC_MODE::MODE_OFF;

    return get_mode();
  }

  const char* get_hvac_mode_str() const { return mode_to_str(this->get_hvac_mode()); }

  AC_MODE get_mode() const {
    const auto mode = this->ac_->getMode();

    switch (mode) {
      case 1:
        return AC_MODE::MODE_DRY;
      case 2:
        return AC_MODE::MODE_COOL;
      case 4:
        return AC_MODE::MODE_FAN;
      case 8:
        return AC_MODE::MODE_HEAT;
      case 10:
        return AC_MODE::MODE_AUTO;
      default:
        ESP_LOGW(TAG, "[get_mode]: Unrecognized mode %u", mode);
        return AC_MODE::MODE_UNDEFINED;
    }
  }

  const char* get_mode_str() const { return mode_to_str(this->get_mode()); }

  void set_mode(const AC_MODE mode) {
    switch (mode) {
      case AC_MODE::MODE_HEAT:
        set_mode_(8);
        set_constraints_(true, true, true, true);
        break;
      case AC_MODE::MODE_DRY:
        set_mode_(1);
        set_constraints_(false, false, false, true);
        break;
      case AC_MODE::MODE_COOL:
        set_mode_(2);
        set_constraints_(true, true, true, true);
        break;
      case AC_MODE::MODE_FAN:
        set_mode_(4);
        set_constraints_(false, false, false, false);
        break;
      case AC_MODE::MODE_AUTO:
        set_mode_(10);
        set_constraints_(false, false, true, true);
        break;
      default:
        break;
    }
  }

  SWING_MODE get_swing_mode() const {
    if (this->ac_->getSwingVertical()) return SWING_MODE::SWING_HORIZONTAL;

    return SWING_MODE::SWING_OFF;
  }

  const char* get_swing_mode_str() const { return swing_mode_to_str(this->get_swing_mode()); }

  void set_swing_mode(SWING_MODE swing_mode) const {
    switch (swing_mode) {
      case SWING_MODE::SWING_OFF:
        this->ac_->setSwingVertical(false);
        break;
      case SWING_MODE::SWING_HORIZONTAL:
        this->ac_->setSwingVertical(true);
        break;
      default:
        this->ac_->setSwingVertical(false);
        ESP_LOGW(TAG, "[set_swing_mode]: Unrecognized swing mode %d", swing_mode);
        break;
    }
  }

  void set_swing_mode(const std::string& swing_mode) const {
    if (str_equals_case_insensitive(swing_mode, "OFF")) {
      set_swing_mode(SWING_MODE::SWING_OFF);
    } else if (str_equals_case_insensitive(swing_mode, "HORIZONTAL")) {
      set_swing_mode(SWING_MODE::SWING_HORIZONTAL);
    } else {
      ESP_LOGW(TAG, "[set_swing_mode]: Unrecognized swing mode %s", swing_mode.c_str());
    }
  }

  bool is_fan_mode_supported(const FAN_MODE fan_mode) const {
    const auto mode = this->ac_->getMode();

    if(fan_mode == FAN_QUIET)
      return quiet_enabled_;

    if(fan_mode == FAN_TURBO)
      return turbo_enabled_;

    // для режима FAN auto недоступен
    if (mode == AC_MODE::MODE_FAN && fan_mode == FAN_MODE::FAN_AUTO)
      return false;

    return true;
  }

  bool set_fan(FAN_MODE fan_mode) {
    if (is_fan_mode_supported(fan_mode) == false) {
      ESP_LOGD(TAG, "[set_fan]: fan_mode: %s not supported for mode: %s", fan_mode_to_str(fan_mode), get_hvac_mode_str());
      return false;
    }

    switch (fan_mode) {
      case FAN_MODE::FAN_AUTO:
        this->ac_->setFan(1);
        this->prev_fan_mode_ = FAN_MODE::FAN_AUTO;
        return true;
      case FAN_MODE::FAN_LOW:
        this->ac_->setFan(8);
        this->prev_fan_mode_ = FAN_MODE::FAN_LOW;
        return true;
      case FAN_MODE::FAN_MEDIUM:
        this->ac_->setFan(4);
        this->prev_fan_mode_ = FAN_MODE::FAN_MEDIUM;
        return true;
      case FAN_MODE::FAN_TURBO:
        this->ac_->setFan(3);
        return true;
      case FAN_MODE::FAN_QUIET:
        this->ac_->setFan(9);
        return true;
      case FAN_MODE::FAN_HIGH:
        this->ac_->setFan(2);
        this->prev_fan_mode_ = FAN_MODE::FAN_HIGH;
        return true;
      default:
        this->ac_->setFan(1);
        ESP_LOGW(TAG, "[set_fan]: Unrecognized fan mode %d", fan_mode);
        return true;
    }
  }

  bool set_fan(const std::string& fan_mode_str) {
    auto fan_mode = parse_fan_mode(fan_mode_str);

    if(fan_mode == FAN_MODE::FAN_UNDEFINED) {
      ESP_LOGW(TAG, "[set_fan]: Unrecognized fan mode %s", fan_mode_str.c_str());
      return false;
    }

    return this->set_fan(fan_mode);
  }

  FAN_MODE get_fan() const {
    const auto fan_mode = this->ac_->getFan();
    switch (fan_mode) {
      case 1:
        return FAN_MODE::FAN_AUTO;
      case 2:
        return FAN_MODE::FAN_HIGH;
      case 3:
        return FAN_MODE::FAN_TURBO;
      case 4:
        return FAN_MODE::FAN_MEDIUM;
      case 8:
        return FAN_MODE::FAN_LOW;
      case 9:
        return FAN_MODE::FAN_QUIET;
      default:
        return FAN_MODE::FAN_AUTO;
    }
  }

  const char* get_fan_str() const { return fan_mode_to_str(this->get_fan()); }

  FAN_MODE get_prev_fan() const { return this->prev_fan_mode_; }

  const char* get_prev_fan_str() const { return fan_mode_to_str(this->get_prev_fan()); }

  bool set_sleep(const bool on) const {
    if (this->sleep_enabled_ == false) return false;

    this->ac_->setSleep(on);

    return true;
  }

  bool set_sleep(const std::string& on) const {
    if (str_equals_case_insensitive(on, "OFF") || str_equals_case_insensitive(on, "FALSE"))
      return set_sleep(false);

    if (str_equals_case_insensitive(on, "ON") || str_equals_case_insensitive(on, "TRUE"))
      return set_sleep(true);

    ESP_LOGW(TAG, "[set_sleep]: Unrecognized sleep mode %s", on.c_str());
    return false;
  }

  bool get_sleep() const { return this->ac_->getSleep(); }

  bool sleep_allowed() const { return this->sleep_enabled_; }

  void initialize(
                  const std::string& hvac_mode_str,
                  const std::string& mode_str,
                  const std::string& fan_mode_str,
                  const std::string& prev_fan_mode_str,
                  const std::string& swing_mode_str,
                  const uint8_t temp,
                  const bool sleep) {

    auto hvac_mode = parse_mode(hvac_mode_str);
    auto mode = parse_mode(mode_str);

    auto prev_fan_mode = parse_fan_mode(prev_fan_mode_str);

    if(hvac_mode == AC_MODE::MODE_OFF) {
      set_power_state(false);

      if(mode != AC_MODE::MODE_OFF || mode != AC_MODE::MODE_UNDEFINED)
        set_mode(mode);

    } else {
      set_power_state(true);
      set_hvac_mode(hvac_mode);
    }

    set_fan(fan_mode_str);

    if(prev_fan_mode == FAN_MODE::FAN_UNDEFINED)
      this->prev_fan_mode_ = FAN_MODE::FAN_MEDIUM;
    else
      this->prev_fan_mode_ = prev_fan_mode;

    set_swing_mode(swing_mode_str);

    if(temp >= temp_min && temp <= temp_max)
      this->ac_->setTemp(temp);

    set_sleep(sleep);
  }

  const char* to_string() const {
    String result = "";
    result.reserve(120);

    result += irutils::addBoolToString(get_power_state(), "power", false);
    result += irutils::addIntToString(get_hvac_mode(), "hvac_mode") + " (" + get_hvac_mode_str() +")";
    result += irutils::addIntToString(get_mode(), "mode") + " (" + get_mode_str() +")";
    result += irutils::addTempToString(get_temp());
    result += irutils::addIntToString(get_fan(), "fan") + " (" + get_fan_str() +")";
    result += irutils::addBoolToString(get_sleep(), "sleep");
    result += irutils::addIntToString(get_swing_mode(), "swing") + " (" + get_swing_mode_str() +")";

    std::string data = result.c_str();
    return data.c_str();
  }

  void loop() {

    if (ir_receiver_->decode(decode_results_) == false)
      return;

    ESP_LOGD(TAG, "[decoder]: Получены данные, обновляем состояние");
    ac_->setRaw(decode_results_->value);
    auto const power_toggle = ac_->getPowerToggle();
    ac_->setPowerToggle(false); //сбрасываем бит питания

    if(power_toggle) {  // переключаем текущее питание
      ESP_LOGD(TAG, "[decoder]: меняем притание: с power: %s, на power: %s", bool_to_str_(get_power_state()), bool_to_str_(!get_power_state()));
      set_power_state(!get_power_state());
    }

    //инициализируем режим
    set_mode(get_mode());

    ESP_LOGD(TAG, "[decoder]: %s", this->to_string());

    state_callback_.call();
  }

  static const char* mode_to_str(const AC_MODE mode) {
    switch (mode) {
      case AC_MODE::MODE_HEAT:
        return "heat";
      case AC_MODE::MODE_DRY:
        return "dry";
      case AC_MODE::MODE_COOL:
        return "cool";
      case AC_MODE::MODE_FAN:
        return "fan_only";
      case AC_MODE::MODE_AUTO:
        return "auto";
      case AC_MODE::MODE_UNDEFINED:
        return "undefined";
      case AC_MODE::MODE_OFF:
        return "off";
      default:
        return "undefined";
    }
  }

  static const char* fan_mode_to_str(const FAN_MODE mode) {
    switch (mode) {
      case FAN_MODE::FAN_AUTO:
        return "auto";
      case FAN_MODE::FAN_LOW:
        return "low";
      case FAN_MODE::FAN_MEDIUM:
        return "medium";
      case FAN_MODE::FAN_TURBO:
        return "turbo";
      case FAN_MODE::FAN_QUIET:
        return "quiet";
      case FAN_MODE::FAN_HIGH:
        return "high";
      case FAN_MODE::FAN_UNDEFINED:
        return "undefined";
      default:
        return "auto";
    }
  }

  static const char* swing_mode_to_str(const SWING_MODE mode) {
    switch (mode) {
      case SWING_MODE::SWING_OFF:
        return "off";
      case SWING_MODE::SWING_HORIZONTAL:
        return "horizontal";
      default:
        return "off";
    }
  }

  static const FAN_MODE parse_fan_mode(const std::string& fan_mode) {
    if (str_equals_case_insensitive(fan_mode, "AUTO"))
      return FAN_MODE::FAN_AUTO;

    if (str_equals_case_insensitive(fan_mode, "QUIET"))
      return FAN_MODE::FAN_QUIET;

    if (str_equals_case_insensitive(fan_mode, "LOW"))
      return FAN_MODE::FAN_LOW;

    if (str_equals_case_insensitive(fan_mode, "MEDIUM"))
      return FAN_MODE::FAN_MEDIUM;

    if (str_equals_case_insensitive(fan_mode, "HIGH"))
      return FAN_MODE::FAN_HIGH;

    if (str_equals_case_insensitive(fan_mode, "TURBO"))
      return FAN_MODE::FAN_TURBO;

    return FAN_MODE::FAN_UNDEFINED;
  }

  static const AC_MODE parse_mode(const std::string& mode) {
    if (str_equals_case_insensitive(mode, "OFF"))
      return AC_MODE::MODE_OFF;
    if (str_equals_case_insensitive(mode, "AUTO"))
      return AC_MODE::MODE_AUTO;
    if (str_equals_case_insensitive(mode, "COOL"))
      return AC_MODE::MODE_COOL;
    if (str_equals_case_insensitive(mode, "HEAT"))
      return AC_MODE::MODE_HEAT;
    if (str_equals_case_insensitive(mode, "FAN_ONLY"))
      return AC_MODE::MODE_FAN;
    if (str_equals_case_insensitive(mode, "DRY"))
      return AC_MODE::MODE_DRY;

    return AC_MODE::MODE_UNDEFINED;
  }

 private:
  static const char* bool_to_str_(const bool value) { return value ? "on" : "off"; }

  void re_initialize_fan_mode_(const FAN_MODE default_fan_mode) {
    const auto fan_mode = get_fan();

    if (is_fan_mode_supported(fan_mode))
      return;

    if(is_fan_mode_supported(prev_fan_mode_)) {
      set_fan(prev_fan_mode_);
      return;
    }

    set_fan(default_fan_mode);
  }

  void set_constraints_(const bool turbo_enabled, const bool quiet_enabled,
                       const bool sleep_enabled, const bool set_temp_enabled) {
    this->turbo_enabled_ = turbo_enabled;
    this->quiet_enabled_ = quiet_enabled;
    this->sleep_enabled_ = sleep_enabled;
    this->set_temp_enabled_ = set_temp_enabled;

    ESP_LOGD(TAG,
             "[set_constraints]: turbo: %s, quiet: %s, sleep: %s, set_temp: %s",
             bool_to_str_(this->turbo_enabled_),
             bool_to_str_(this->quiet_enabled_),
             bool_to_str_(this->sleep_enabled_),
             bool_to_str_(this->set_temp_enabled_));

    re_initialize_fan_mode_(FAN_MODE::FAN_MEDIUM);

    const auto turbo = this->ac_->getTurbo();
    const auto quiet = this->ac_->getQuiet();
    const auto sleep = this->ac_->getSleep();

    if (turbo_enabled_ == false && turbo == true) {
      this->ac_->setTurbo(false);
      set_fan(this->prev_fan_mode_);
      ESP_LOGW(TAG, "[set_constraints]: Принудительный сброс состояния turbo");
    }

    if (quiet_enabled_ == false && quiet == true) {
      this->ac_->setQuiet(false);
      set_fan(this->prev_fan_mode_);
      ESP_LOGW(TAG, "[set_constraints]: Принудительный сброс состояния quiet");
    }

    if (sleep_enabled_ == false && sleep == true) {
      this->ac_->setSleep(false);
    }
  }

  void set_mode_(const uint8_t mode) {

    uint64_t raw_data = ac_->getRaw();

    const uint8_t offset = 8;
    const uint8_t nbits = 4;
    if (offset >= 64 || !nbits) return;
    uint64_t mask = UINT64_MAX >> (64 - ((nbits > 64) ? 64 : nbits));
    raw_data &= ~(mask << offset);
    raw_data |= ((mode & mask) << offset);
    ac_->setRaw(raw_data);
  }
};
}