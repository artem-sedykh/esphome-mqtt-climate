#include <ir_Tcl.h>
#include <IRutils.h>

namespace ir_climate {

static const char *TAG = "ir.dahatsu";

enum SWING_MODE : uint8_t {
  SWING_OFF = 0,
  SWING_HORIZONTAL = 1,
};

enum FAN_MODE : uint8_t {
  FAN_UNDEFINED = 10,
  FAN_AUTO = 0,
  FAN_LOW = 2,
  FAN_MEDIUM = 3,
  FAN_HIGH = 5,
};

enum AC_MODE : uint8_t {
  MODE_UNDEFINED = 10,
  MODE_OFF = 0,
  MODE_HEAT = 1,
  MODE_DRY = 2,
  MODE_COOL = 3,
  MODE_FAN = 7,
  MODE_AUTO = 8,
};

class State {
 public:
  float temp;
  FAN_MODE fan_mode;
  SWING_MODE swing_mode;

  State(float temp, FAN_MODE fan_mode, SWING_MODE swing_mode) {
    this->temp = temp;
    this->fan_mode = fan_mode;
    this->swing_mode = swing_mode;
  }
};

class IRDahatsu {
 private:
  IRTcl112Ac* ac_;
  IRrecv* ir_receiver_{nullptr};
  decode_results* decode_results_;
  CallbackManager<void()> state_callback_{};
  State* state_{nullptr};

  bool health_enabled_{true};
  bool light_enabled_{true};
  bool turbo_enabled_{true};
  bool eco_enabled_{true};
  bool set_temp_enabled_{true};

 public:
  const float temp_min = 16;
  const float temp_max = 31;
  const float temp_step = 0.5;

  const char* modes_str[6] = {
      mode_to_str(AC_MODE::MODE_OFF),
      mode_to_str(AC_MODE::MODE_HEAT),
      mode_to_str(AC_MODE::MODE_AUTO),
      mode_to_str(AC_MODE::MODE_COOL),
      mode_to_str(AC_MODE::MODE_DRY),
      mode_to_str(AC_MODE::MODE_FAN)};

  const FAN_MODE fan_modes[4] = {
      FAN_MODE::FAN_AUTO,
      FAN_MODE::FAN_LOW,
      FAN_MODE::FAN_MEDIUM,
      FAN_MODE::FAN_HIGH};

  const char* fan_modes_str[4] = {
      fan_mode_to_str(FAN_MODE::FAN_AUTO),
      fan_mode_to_str(FAN_MODE::FAN_LOW),
      fan_mode_to_str(FAN_MODE::FAN_MEDIUM),
      fan_mode_to_str(FAN_MODE::FAN_HIGH)};

  const char* swing_modes_str[2] = {
      swing_mode_to_str(SWING_MODE::SWING_OFF),
      swing_mode_to_str(SWING_MODE::SWING_HORIZONTAL)};

  IRDahatsu(uint16_t receiver_pin, uint16_t transmitter_pin) {
    ac_ = new IRTcl112Ac(transmitter_pin);
    ir_receiver_ = new IRrecv(receiver_pin, 300, 20, true);
    ir_receiver_->setTolerance(40);
    ac_->setPower(false);

    //инициализация констрейнтов
    set_mode(get_mode());
    decode_results_ = new decode_results();
  }

  void setup() const {
    ir_receiver_->enableIRIn();
    ac_->begin();
  }

  void add_on_state_callback(std::function<void()>&& callback) { this->state_callback_.add(std::move(callback)); }

  void send() const {
    ir_receiver_->disableIRIn();
    ac_->send();
    ESP_LOGD(TAG, "[send]: %s", this->to_string());
    ir_receiver_->enableIRIn();
  }

  State* get_prev_state() const { return this->state_; }

  bool set_temp(const float temp) {

    if (temp < this->temp_min || temp > this->temp_max)
      return false;

    change_temp_callback_(get_temp(), temp);

    this->ac_->setTemp(temp);

    if(this->set_temp_enabled_ == false)
      return false;

    return true;
  }

  float get_temp() const { return this->ac_->getTemp(); }

  bool set_temp_allowed() const { return this->set_temp_enabled_; }

  void set_mode(const AC_MODE mode) {

    switch (mode) {
      case AC_MODE::MODE_HEAT:
        this->ac_->setMode(1);
        set_constraints(true, true, true, true, true, FAN_MODE::FAN_MEDIUM);
        break;
      case AC_MODE::MODE_DRY:
        set_turbo(false);
        this->ac_->setMode(2);
        set_constraints(true, true, true, false, false, FAN_MODE::FAN_AUTO);
        break;
      case AC_MODE::MODE_COOL:
        set_turbo(false);
        this->ac_->setMode(3);
        set_constraints(true, true, true, true, true, FAN_MODE::FAN_MEDIUM);
        break;
      case AC_MODE::MODE_FAN:
        this->ac_->setMode(7);
        set_constraints(true, true, true, false, false, FAN_MODE::FAN_MEDIUM);
        break;
      case AC_MODE::MODE_AUTO:
        this->ac_->setMode(8);
        set_constraints(true, true, false, false, false, FAN_MODE::FAN_MEDIUM);
        break;
      default:
        break;
    }
  }

  AC_MODE get_mode() const {
    const auto mode = this->ac_->getMode();

    switch (mode) {
      case 1:
        return AC_MODE::MODE_HEAT;
      case 2:
        return AC_MODE::MODE_DRY;
      case 3:
        return AC_MODE::MODE_COOL;
      case 7:
        return AC_MODE::MODE_FAN;
      case 8:
        return AC_MODE::MODE_AUTO;
      default:
        return AC_MODE::MODE_UNDEFINED;
    }
  }

  const char* get_mode_str() const { return mode_to_str(this->get_mode()); }

  void set_hvac_mode(const AC_MODE mode) {

    if(mode != get_mode())
      set_mode(mode);

    if(mode == AC_MODE::MODE_OFF)
      this->ac_->setPower(false);
    else
      this->ac_->setPower(true);
  }

  void set_hvac_mode(const std::string& mode) {
    if (str_equals_case_insensitive(mode, "OFF")) {
      this->set_hvac_mode(AC_MODE::MODE_OFF);
    } else if (str_equals_case_insensitive(mode, "AUTO")) {
      this->set_hvac_mode(AC_MODE::MODE_AUTO);
    } else if (str_equals_case_insensitive(mode, "COOL")) {
      this->set_hvac_mode(AC_MODE::MODE_COOL);
    } else if (str_equals_case_insensitive(mode, "HEAT")) {
      this->set_hvac_mode(AC_MODE::MODE_HEAT);
    } else if (str_equals_case_insensitive(mode, "FAN_ONLY")) {
      this->set_hvac_mode(AC_MODE::MODE_FAN);
    } else if (str_equals_case_insensitive(mode, "DRY")) {
      this->set_hvac_mode(AC_MODE::MODE_DRY);
    } else {
      ESP_LOGW(TAG, "[set_hvac_mode]: Unrecognized mode %s", mode.c_str());
    }
  }

  AC_MODE get_hvac_mode() const {
    auto power_on = this->ac_->getPower();

    if(power_on == false)
      return AC_MODE::MODE_OFF;

    return get_mode();
  }

  const char* get_hvac_mode_str() const { return mode_to_str(this->get_hvac_mode()); }

  bool is_fan_mode_supported(const FAN_MODE fan_mode) const {
    const auto mode = this->ac_->getMode();

    // для режима FAN доступны все режимы  кроме auto
    if (mode == AC_MODE::MODE_FAN) {
      if (fan_mode == FAN_MODE::FAN_AUTO) return false;

      return true;
    }

    //Для режима осушения доступен только один режим обдува: auto
    if (mode == AC_MODE::MODE_DRY) {
      if (fan_mode == FAN_MODE::FAN_AUTO) return true;

      return false;
    }

    return true;
  }

  bool set_fan(FAN_MODE fan_mode) {

    if (is_fan_mode_supported(fan_mode) == false) {
      ESP_LOGD(TAG, "[set_fan]: fan_mode: %s not supported for mode: %s", fan_mode_to_str(fan_mode), get_hvac_mode_str());
      return false;
    }

    switch (fan_mode) {
      case FAN_MODE::FAN_AUTO:
        change_fan_callback_(get_fan(), FAN_MODE::FAN_AUTO);
        this->ac_->setFan(0);
        return true;
      case FAN_MODE::FAN_LOW:
        change_fan_callback_(get_fan(), FAN_MODE::FAN_LOW);
        this->ac_->setFan(2);
        return true;
      case FAN_MODE::FAN_MEDIUM:
        change_fan_callback_(get_fan(), FAN_MODE::FAN_MEDIUM);
        this->ac_->setFan(3);
        return true;
      case FAN_MODE::FAN_HIGH:
        change_fan_callback_(get_fan(), FAN_MODE::FAN_HIGH);
        this->ac_->setFan(5);
        return true;
      default:
        change_fan_callback_(get_fan(), FAN_MODE::FAN_AUTO);
        this->ac_->setFan(0);
        ESP_LOGW(TAG, "[set_fan]: Unrecognized mode: %s", fan_mode_to_str(fan_mode));
        return false;
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
      case 0:
        return FAN_MODE::FAN_AUTO;
      case 2:
        return FAN_MODE::FAN_LOW;
      case 3:
        return FAN_MODE::FAN_MEDIUM;
      case 5:
        return FAN_MODE::FAN_HIGH;
      default:
        return FAN_MODE::FAN_AUTO;
    }
  }

  const char* get_fan_str() const { return fan_mode_to_str(this->get_fan()); }

  SWING_MODE get_swing_mode() const {
    if (this->ac_->getSwingHorizontal()) return SWING_MODE::SWING_HORIZONTAL;

    return SWING_MODE::SWING_OFF;
  }

  const char* get_swing_mode_str() const { return swing_mode_to_str(this->get_swing_mode()); }

  void set_swing_mode(SWING_MODE swing_mode) const {
    switch (swing_mode) {
      case SWING_MODE::SWING_OFF:
        this->ac_->setSwingHorizontal(false);
        break;
      case SWING_MODE::SWING_HORIZONTAL:
        this->ac_->setSwingHorizontal(true);
        break;
      default:
        this->ac_->setSwingHorizontal(false);
        ESP_LOGW(TAG, "[set_swing_mode]: Unrecognized swing mode %d", swing_mode);
        break;
    }
  }

  void set_swing_mode(const std::string& swing_mode_str) const {

    auto swing_mode = parse_swing_mode(swing_mode_str);

    set_swing_mode(swing_mode);
  }

  bool set_light(const bool on) const {
    if (this->light_enabled_ == false) return false;

    this->ac_->setLight(on);

    return true;
  }

  bool set_light(const std::string& on) const {
    if (str_equals_case_insensitive(on, "OFF") || str_equals_case_insensitive(on, "FALSE"))
      return set_light(false);

    if (str_equals_case_insensitive(on, "ON") || str_equals_case_insensitive(on, "TRUE"))
      return set_light(true);

    ESP_LOGW(TAG, "[set_light]: Unrecognized light mode %s", on.c_str());
    return false;
  }

  bool get_light() const { return this->ac_->getLight(); }

  bool light_allowed() const { return this->light_enabled_; }

  bool set_turbo(const bool on) {
    if (this->turbo_enabled_ == false) return false;

    return set_turbo_(on);
  }

  bool set_turbo(const std::string& on) {
    if (str_equals_case_insensitive(on, "OFF") || str_equals_case_insensitive(on, "FALSE"))
      return set_turbo(false);

    if (str_equals_case_insensitive(on, "ON") || str_equals_case_insensitive(on, "TRUE"))
      return set_turbo(true);

    ESP_LOGW(TAG, "[set_turbo]: Unrecognized turbo mode %s", on.c_str());
    return false;
  }

  bool get_turbo() const { return this->ac_->getTurbo(); }

  bool turbo_allowed() const { return this->turbo_enabled_; }

  bool set_health(const bool on) const {
    if (this->health_enabled_ == false) return false;

    this->ac_->setHealth(on);

    return true;
  }

  bool set_health(const std::string& on) const {
    if (str_equals_case_insensitive(on, "OFF") || str_equals_case_insensitive(on, "FALSE"))
      return set_health(false);

    if (str_equals_case_insensitive(on, "ON") || str_equals_case_insensitive(on, "TRUE"))
      return set_health(true);

    ESP_LOGW(TAG, "[set_health]: Unrecognized health mode %s", on.c_str());

    return false;
  }

  bool get_health() const { return this->ac_->getHealth(); }

  bool health_allowed() const { return this->health_enabled_; }

  bool set_eco(const bool on) {
    if (this->eco_enabled_ == false) return false;

    if (get_eco() == on) return true;

    if (on)
      set_turbo(false);

    this->ac_->setEcono(on);

    return true;
  }

  bool set_eco(const std::string& on) {
    if (str_equals_case_insensitive(on, "OFF") || str_equals_case_insensitive(on, "FALSE"))
      return set_eco(false);

    if (str_equals_case_insensitive(on, "ON") || str_equals_case_insensitive(on, "TRUE"))
      return set_eco(true);

    ESP_LOGW(TAG, "[set_eco]: Unrecognized econo mode %s", on.c_str());

    return false;
  }

  bool get_eco() const { return this->ac_->getEcono(); }

  bool eco_allowed() const { return this->eco_enabled_; }

  //используется при инициализации состояния при запуске
  void initialize(
                  const std::string& hvac_mode_str,
                  const std::string& mode_str,
                  const std::string& fan_mode_str,
                  const std::string& swing_mode_str,
                  State* state,
                  const uint8_t temp,
                  const bool turbo,
                  const bool eco,
                  const bool health,
                  const bool light) {
    auto hvac_mode = parse_mode(hvac_mode_str);
    auto mode = parse_mode(mode_str);

    set_mode(mode);
    set_hvac_mode(hvac_mode);
    set_fan(fan_mode_str);
    set_swing_mode(swing_mode_str);

    if(temp >= temp_min && temp <= temp_max)
      this->ac_->setTemp(temp);

    if(turbo) {
      this->state_ = state;
      this->ac_->setTurbo(true);
    } else{
      this->ac_->setTurbo(false);
    }

    set_eco(eco);
    set_health(health);
    set_light(light);
  }

  const char* to_string() const {
    String result = "";
    result.reserve(140);

    auto power_on = get_hvac_mode() != AC_MODE::MODE_OFF;

    result += irutils::addBoolToString(power_on, "power", false);
    result += irutils::addIntToString(get_hvac_mode(), "hvac_mode") + " (" + get_hvac_mode_str() +")";
    result += irutils::addIntToString(get_mode(), "mode") + " (" + get_mode_str() +")";
    result += irutils::addTempToString(get_temp());
    result += irutils::addIntToString(get_fan(), "fan") + " (" + get_fan_str() +")";
    result += irutils::addIntToString(get_swing_mode(), "swing") + " (" + get_swing_mode_str() +")";

    result += irutils::addBoolToString(get_turbo(), "turbo", true);
    result += irutils::addBoolToString(get_eco(), "eco", true);
    result += irutils::addBoolToString(get_health(), "health", true);
    result += irutils::addBoolToString(get_light(), "light", true);

    std::string data = result.c_str();
    return data.c_str();
  }

  void loop() {

    if (ir_receiver_->decode(decode_results_) == false)
      return;

    ESP_LOGD(TAG, "[decoder]: Получены данные, обновляем состояние");
    ac_->setRaw(decode_results_->state, decode_results_->rawlen);
    auto turbo = get_turbo();

    //инициализируем режим работы
    set_mode(get_mode());

    set_turbo(turbo);

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

  static const SWING_MODE parse_swing_mode(const std::string& swing_mode) {
    if (str_equals_case_insensitive(swing_mode, "OFF"))
      return SWING_MODE::SWING_OFF;

    if (str_equals_case_insensitive(swing_mode, "HORIZONTAL"))
      return SWING_MODE::SWING_HORIZONTAL;

    ESP_LOGW(TAG, "[parse_swing_mode]: Unrecognized swing mode %s", swing_mode.c_str());

    return SWING_MODE::SWING_OFF;
  }

  static const FAN_MODE parse_fan_mode(const std::string& fan_mode) {
    if (str_equals_case_insensitive(fan_mode, "AUTO"))
      return FAN_MODE::FAN_AUTO;

    if (str_equals_case_insensitive(fan_mode, "LOW"))
      return FAN_MODE::FAN_LOW;

    if (str_equals_case_insensitive(fan_mode, "MEDIUM"))
      return FAN_MODE::FAN_MEDIUM;

    if (str_equals_case_insensitive(fan_mode, "HIGH"))
      return FAN_MODE::FAN_HIGH;

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
  void change_fan_callback_(FAN_MODE old_fan_mode, FAN_MODE new_fan_mode) {
    if(old_fan_mode == new_fan_mode)
      return;

    auto turbo = get_turbo();

    if(turbo == false)
      return;

    auto mode = get_mode();

    if(mode == AC_MODE::MODE_COOL || mode == AC_MODE::MODE_HEAT || mode == AC_MODE::MODE_FAN || mode == AC_MODE::MODE_DRY)
      set_turbo(false);
  }

  void change_temp_callback_(float old_temp, float new_temp) {
    if(old_temp == new_temp)
      return;

    auto turbo = get_turbo();

    if(turbo == false)
      return;

    auto mode = get_mode();

    if(mode == AC_MODE::MODE_COOL || mode == AC_MODE::MODE_HEAT)
      set_turbo(false);
  }

  bool set_turbo_(const bool on) {

    if (on == get_turbo()) return true;

    if (on == false) {
      //Если выключаем turbo
      if (get_turbo() == true) {
        this->ac_->setTurbo(false);
        apply_state_();
        return true;
      }

      this->ac_->setTurbo(on);

      return true;
    }

    //отрубаем econo
    set_eco(false);

    const auto mode = get_mode();
    switch (mode) {
      case AC_MODE::MODE_HEAT:
        //запоминаем предыдущее состояние
        save_state_();
        //При включеении переключает Temp: 31C Fan: 5 (High) Swing(H): On
        set_temp(this->temp_max);
        set_fan(FAN_MODE::FAN_HIGH);
        set_swing_mode(SWING_MODE::SWING_HORIZONTAL);
        break;
      case AC_MODE::MODE_DRY:
        //запоминаем предыдущее состояние
        save_state_();
        //При включеении переключает Fan: 2 (Low) Swing(H);
        this->ac_->setFan(2);
        set_swing_mode(SWING_MODE::SWING_HORIZONTAL);
        break;
      case AC_MODE::MODE_COOL:
        //запоминаем предыдущее состояние
        save_state_();
        //При включеении переключает Temp: 16C Fan: 5 (High) Swing(H): On
        set_temp(this->temp_min);
        set_fan(FAN_MODE::FAN_HIGH);
        set_swing_mode(SWING_MODE::SWING_HORIZONTAL);
        break;
      case AC_MODE::MODE_FAN:
        //запоминаем предыдущее состояние
        save_state_();
        //При включеении переключает Fan: 5 (High) Swing(H): On
        set_fan(FAN_MODE::FAN_HIGH);
        set_swing_mode(SWING_MODE::SWING_HORIZONTAL);
        break;
      default:
        break;
    }

    this->ac_->setTurbo(on);
    return true;
  }

  static const char* bool_to_str_(const bool value) { return value ? "on" : "off"; }

  void save_state_() {
    if (this->state_ == nullptr)
      this->state_ = new State(get_temp(), get_fan(), get_swing_mode());
    else {
      this->state_->temp = get_temp();
      this->state_->fan_mode = get_fan();
      this->state_->swing_mode = get_swing_mode();
    }
  }

  void apply_state_() {
    if (this->state_ == nullptr) return;

    const auto temp = this->state_->temp;
    const auto fan_mode = this->state_->fan_mode;
    const auto swing_mode = this->state_->swing_mode;

    set_temp(temp);
    set_fan(fan_mode);
    set_swing_mode(swing_mode);

    ESP_LOGD(TAG,"[apply_state]: Сбрасываем состояние на: temp: %.2f, fan: %s, swing: %s",
                 temp,
                 fan_mode_to_str(fan_mode),
                 swing_mode_to_str(swing_mode));
  }

  void set_constraints(const bool health_enabled,
                       const bool light_enabled,
                       const bool turbo_enabled,
                       const bool eco_enabled,
                       const bool set_temp_enabled,
                       const FAN_MODE default_fan_mode) {
    this->health_enabled_ = health_enabled;
    this->light_enabled_ = light_enabled;
    this->turbo_enabled_ = turbo_enabled;
    this->eco_enabled_ = eco_enabled;
    this->set_temp_enabled_ = set_temp_enabled;

    const auto health = this->ac_->getHealth();
    const auto light = this->ac_->getLight();
    const auto turbo = this->ac_->getTurbo();
    const auto eco = this->ac_->getEcono();

    //Если состояние не поддерживается, но было включено, то выключим его
    if (health_enabled == false && health == true)
      this->ac_->setHealth(false);

    if (light_enabled == false && light == true)
      this->ac_->setLight(false);

    if (turbo == true)
      set_turbo_(false);

    if (eco_enabled == false && eco == true)
      this->ac_->setEcono(false);

    if(is_fan_mode_supported(get_fan()) == false)
      set_fan(default_fan_mode);
  }
};
}