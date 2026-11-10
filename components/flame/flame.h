#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <cmath>
#include <cstring>
#include <esp_gattc_api.h>
#include <string>
#include <vector>

namespace esphome {
namespace flame {

static const char *const TAG = "flame";

class FlameSwitch : public esphome::switch_::Switch {
 public:
  void write_state(bool state) override {
    this->dirty_ = true;
    this->publish_state(state);
  }
  bool dirty() const { return this->dirty_; }
  void clear_dirty() { this->dirty_ = false; }

 protected:
  bool dirty_{false};
};

class FlameNumber : public number::Number {
 public:
  void control(float value) override {
    this->dirty_ = true;
    this->publish_state(value);
  }
  bool dirty() const { return this->dirty_; }
  void clear_dirty() { this->dirty_ = false; }

 protected:
  bool dirty_{false};
};

class FlameSelect : public select::Select {
 public:
  void control(const std::string &value) override {
    this->dirty_ = true;
    this->publish_state(value);
  }
  bool dirty() const { return this->dirty_; }
  void clear_dirty() { this->dirty_ = false; }

 protected:
  bool dirty_{false};
};

ble_client::espbt::ESPBTUUID uuid128_from_string(std::string value) {
  esp_bt_uuid_t m_uuid;
  m_uuid.len = ESP_UUID_LEN_128;
  int n = 0;
  for (int i = 0; i < value.length();) {
    if (value.c_str()[i] == '-') {
      i++;
    }
    uint8_t MSB = value.c_str()[i];
    uint8_t LSB = value.c_str()[i + 1];
    if (MSB > '9') {
      MSB -= 7;
    }
    if (LSB > '9') {
      LSB -= 7;
    }
    m_uuid.uuid.uuid128[15 - n++] = ((MSB & 0x0F) << 4) | (LSB & 0x0F);
    i += 2;
  }

  return ble_client::espbt::ESPBTUUID::from_uuid(m_uuid);
}

class Flame : public Component, public ble_client::BLEClientNode {
 public:
  Flame(FlameSwitch *toggle, FlameSwitch *heat, FlameNumber *temperature)
      : toggle_(toggle), heat_(heat), temperature_number_(temperature) {}

  void set_volume(FlameNumber *volume) { this->volume_ = volume; }
  void set_ambient(FlameNumber *ambient) { this->ambient_ = ambient; }
  void set_brightness(FlameNumber *brightness) { this->brightness_ = brightness; }
  void set_timer(FlameNumber *timer) { this->timer_ = timer; }
  void set_sensor(FlameSelect *sensor) { this->sensor_ = sensor; }

  void setup() override {
    ESP_LOGD(TAG, "Flame component setup");
    this->configure_security_once_();
    this->next_connect_attempt_ms_ = millis() + 15000;
  }

  void loop() override {
    using ClientState = esp32_ble_tracker::ClientState;

    if (!this->is_ready_()) {
      const auto state = this->parent()->state();
      if (state == ClientState::IDLE && millis() >= this->next_connect_attempt_ms_) {
        ESP_LOGW(TAG, "BLE not connected, scheduling connection attempt");
        this->configure_security_once_();
        this->next_connect_attempt_ms_ = millis() + 30000;
        this->parent()->connect();
      }
      return;
    }

    if (!this->op_pending_) {
      this->sync_writes_();
    }
    if (!this->op_pending_ && this->ops_.empty() && millis() - this->last_status_poll_ms_ > 15000) {
      this->last_status_poll_ms_ = millis();
      this->queue_status_reads_();
    }
    this->start_next_op_();
  }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
      case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "Connected to device, requesting encryption");
        this->parent()->pair();
        break;

      case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGD(TAG, "Disconnected from device");
        this->reset_connection_();
        this->next_connect_attempt_ms_ = millis() + 30000;
        break;

      case ESP_GATTC_SEARCH_CMPL_EVT:
        this->collect_handles_();
        if (this->h_control_ == 0) {
          this->status_set_warning();
          ESP_LOGW(TAG, "Control characteristic 0x1001 not found");
          break;
        }
        ESP_LOGI(TAG, "Handles control=%u temp=%u vol=%u fx=%u cmd=%u fx5=%u timer_en=%u timer=%u snsr=%u",
                 this->h_control_, this->h_temp_, this->h_volume_, this->h_effects_, this->h_cmd_, this->h_fx5_,
                 this->h_timer_en_, this->h_timer_, this->h_sensor_);
        this->queue_status_reads_();
        this->start_next_op_();
        break;

      case ESP_GATTC_READ_CHAR_EVT:
        this->op_pending_ = false;
        this->on_read_(param->read.handle, param->read.status, param->read.value, param->read.value_len);
        this->start_next_op_();
        break;

      case ESP_GATTC_WRITE_CHAR_EVT:
        this->op_pending_ = false;
        if (param->write.status != ESP_GATT_OK) {
          ESP_LOGW(TAG, "Write failed handle=%u status=%d", param->write.handle, param->write.status);
          if (param->write.status == ESP_GATT_ERROR) {
            this->parent()->disconnect();
            this->reset_connection_();
            this->next_connect_attempt_ms_ = millis() + 10000;
          }
        } else {
          ESP_LOGI(TAG, "Write complete handle=%u", param->write.handle);
        }
        this->start_next_op_();
        break;

      default:
        break;
    }
  }

 protected:
  static uint16_t uuid16_of_(const esp_bt_uuid_t &uuid) {
    if (uuid.len == ESP_UUID_LEN_16) {
      return uuid.uuid.uuid16;
    }
    if (uuid.len == ESP_UUID_LEN_128) {
      return uuid.uuid.uuid128[12] | (uint16_t) (uuid.uuid.uuid128[13] << 8);
    }
    return 0;
  }

  void configure_security_once_() {
    if (this->security_configured_) {
      return;
    }
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IN;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
    this->security_configured_ = true;
  }

  bool is_ready_() { return this->h_control_ != 0 && this->parent()->connected(); }

  void reset_connection_() {
    this->h_control_ = 0;
    this->h_temp_ = 0;
    this->h_volume_ = 0;
    this->h_effects_ = 0;
    this->h_cmd_ = 0;
    this->h_fx5_ = 0;
    this->h_fx4_ = 0;
    this->h_custom_ = 0;
    this->h_timer_en_ = 0;
    this->h_timer_ = 0;
    this->h_sensor_ = 0;
    this->op_pending_ = false;
    this->have_effects_ = false;
    this->ops_.clear();
  }

  void collect_handles_() {
    this->reset_connection_();
    auto *chr = this->parent()->get_characteristic(this->control_service_uuid_, this->control_char_uuid_);
    if (chr != nullptr) {
      this->h_control_ = chr->handle;
    }

    uint16_t count = 0;
    auto *client = this->parent();
    if (esp_ble_gattc_get_attr_count(client->get_gattc_if(), client->get_conn_id(), ESP_GATT_DB_ALL, 0x0001, 0xFFFF, 0,
                                     &count) != ESP_GATT_OK ||
        count == 0) {
      return;
    }
    std::vector<esp_gattc_db_elem_t> db(count);
    if (esp_ble_gattc_get_db(client->get_gattc_if(), client->get_conn_id(), 0x0001, 0xFFFF, db.data(), &count) !=
        ESP_GATT_OK) {
      return;
    }
    for (uint16_t i = 0; i < count; i++) {
      if (db[i].type != ESP_GATT_DB_CHARACTERISTIC) {
        continue;
      }
      if (db[i].uuid.len != ESP_UUID_LEN_16) {
        continue;
      }
      const uint16_t uuid = db[i].uuid.uuid.uuid16;
      const uint16_t handle = db[i].attribute_handle;
      switch (uuid) {
        case 0x1001:
          this->h_control_ = handle;
          break;
        case 0x1003:
          this->h_temp_ = handle;
          break;
        case 0x1030:
          this->h_cmd_ = handle;
          break;
        case 0x4010:
          this->h_volume_ = handle;
          break;
        case 0x4013:
          this->h_effects_ = handle;
          break;
        case 0x4015:
          this->h_fx5_ = handle;
          break;
        case 0x4014:
          this->h_fx4_ = handle;
          break;
        case 0x1033:
          this->h_custom_ = handle;
          break;
        case 0x1017:
          this->h_timer_en_ = handle;
          break;
        case 0x1027:
          this->h_timer_ = handle;
          break;
        case 0x1018:
          this->h_sensor_ = handle;
          break;
        default:
          break;
      }
    }
  }

  void queue_status_reads_() {
    this->enqueue_read_(this->h_control_);
    this->enqueue_read_(this->h_temp_);
    this->enqueue_read_(this->h_volume_);
    this->enqueue_read_(this->h_effects_);
    this->enqueue_read_(this->h_fx5_);
    this->enqueue_read_(this->h_fx4_);
    this->enqueue_read_(this->h_custom_);
    this->enqueue_read_(this->h_timer_);
    this->enqueue_read_(this->h_timer_en_);
    this->enqueue_read_(this->h_sensor_);
  }

  void log_rx_(const char *uuid, const uint8_t *data, uint16_t len) {
    char hex[128];
    size_t pos = 0;
    for (uint16_t i = 0; i < len && pos + 3 < sizeof(hex); i++) {
      pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
    }
    if (pos > 0) {
      hex[pos - 1] = '\0';
    } else {
      hex[0] = '\0';
    }
    ESP_LOGI(TAG, "RX %s len=%u %s", uuid, len, hex);
  }

  void enqueue_read_(uint16_t handle) {
    if (handle == 0) {
      return;
    }
    Op op;
    op.write = false;
    op.handle = handle;
    this->ops_.push_back(op);
  }

  void enqueue_write_(uint16_t handle, const uint8_t *data, uint16_t len) {
    if (handle == 0 || len == 0 || len > sizeof(Op::data)) {
      return;
    }
    Op op;
    op.write = true;
    op.handle = handle;
    op.len = len;
    memcpy(op.data, data, len);
    this->ops_.push_back(op);
  }

  void start_next_op_() {
    if (this->op_pending_ || this->ops_.empty() || !this->parent()->connected()) {
      return;
    }
    Op op = this->ops_.front();
    this->ops_.erase(this->ops_.begin());
    esp_err_t err;
    if (op.write) {
      char hex[40];
      size_t pos = 0;
      for (uint16_t i = 0; i < op.len && pos + 3 < sizeof(hex); i++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", op.data[i]);
      }
      if (pos > 0) {
        hex[pos - 1] = '\0';
      }
      ESP_LOGI(TAG, "TX handle=%u %s", op.handle, hex);
      err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), op.handle, op.len,
                                     op.data, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
    } else {
      err = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), op.handle,
                                    ESP_GATT_AUTH_REQ_MITM);
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "GATT op failed handle=%u write=%s err=%d", op.handle, YESNO(op.write), err);
      return;
    }
    this->op_pending_ = true;
  }

  void on_read_(uint16_t handle, esp_gatt_status_t status, const uint8_t *data, uint16_t len) {
    if (status != ESP_GATT_OK || data == nullptr || len == 0) {
      ESP_LOGW(TAG, "Read failed handle=%u status=%d", handle, status);
      return;
    }
    if (handle == this->h_control_) {
      this->apply_control_(data, len);
    } else if (handle == this->h_volume_) {
      this->volume_state_ = data[0];
      this->have_volume_ = true;
      if (this->volume_ != nullptr && !this->volume_->dirty()) {
        this->volume_->publish_state(this->volume_state_);
      }
    } else if (handle == this->h_effects_ && len >= 3) {
      memcpy(this->effects_, data, std::min<uint16_t>(len, sizeof(this->effects_)));
      this->effects_len_ = std::min<uint16_t>(len, sizeof(this->effects_));
      this->have_effects_ = true;
      this->log_rx_("4013", data, len);
      this->ambient_state_ = this->effects_[1];
      this->brightness_state_ = this->effects_[2];
      if (this->ambient_ != nullptr && !this->ambient_->dirty()) {
        this->ambient_->publish_state(this->ambient_state_);
      }
      if (this->brightness_ != nullptr && !this->brightness_->dirty()) {
        this->brightness_->publish_state(this->brightness_state_);
      }
    } else if (handle == this->h_fx5_) {
      this->log_rx_("4015", data, len);
    } else if (handle == this->h_fx4_) {
      this->log_rx_("4014", data, len);
    } else if (handle == this->h_custom_) {
      this->log_rx_("1033", data, len);
    } else if (handle == this->h_temp_ && len >= 1) {
      this->temperature_ = data[0];
      if (!this->temperature_number_->dirty()) {
        this->temperature_number_->publish_state(this->temperature_);
      }
    } else if (handle == this->h_timer_ && len >= 2) {
      uint16_t remaining = data[0] | (uint16_t(data[1]) << 8);
      this->have_timer_ = true;
      if (this->timer_ != nullptr && this->timer_->dirty()) {
        return;
      }
      if (remaining > 0) {
        this->timer_minutes_ = remaining;
        if (this->timer_ != nullptr) {
          this->timer_->publish_state(remaining / 60.0f);
        }
      }
    } else if (handle == this->h_timer_en_ && len >= 2) {
      this->have_timer_ = true;
      if (this->timer_ != nullptr && this->timer_->dirty()) {
        return;
      }
      if (this->timer_minutes_ == 0 && this->timer_ != nullptr) {
        if (data[0] == 0) {
          this->timer_->publish_state(0);
        } else if (data[1] <= 15) {
          // BLE set index is 0-based half-hours: 0=0.5h, 1=1.0h, ...
          this->timer_minutes_ = static_cast<uint16_t>(data[1] + 1) * 30;
          this->timer_->publish_state((data[1] + 1) / 2.0f);
        } else {
          this->timer_minutes_ = data[1];
          this->timer_->publish_state(data[1] / 60.0f);
        }
      }
    } else if (handle == this->h_sensor_) {
      this->sensor_state_ = data[0];
      this->have_sensor_ = true;
      if (this->sensor_ != nullptr && !this->sensor_->dirty()) {
        this->sensor_->publish_state(this->sensor_name_(this->sensor_state_));
      }
    }
  }

  void apply_control_(const uint8_t *data, uint16_t len) {
    ESP_LOGI(TAG, "Read status value: %02X", data[0]);
    if (data[0] == 0x00) {
      ESP_LOGW(TAG, "Flame device is in error state, reconnecting...");
      this->parent()->disconnect();
      this->reset_connection_();
      this->next_connect_attempt_ms_ = millis() + 10000;
      return;
    }
    if (len >= 5) {
      this->temperature_ = data[4];
    }
    const uint8_t mode = data[0];
    const bool power = mode != 0x06;
    // 0x08 = flame only, 0x02 = heat on (confirmed against the physical fire).
    const bool heat = mode == 0x02;
    this->flame_state_ = power;
    this->heat_state_ = heat;
    if (!this->toggle_->dirty()) {
      this->toggle_->publish_state(power);
    }
    if (!this->heat_->dirty()) {
      this->heat_->publish_state(heat);
    }
    if (!this->temperature_number_->dirty()) {
      this->temperature_number_->publish_state(this->temperature_);
    }
  }

  static const char *sensor_name_(uint8_t value) {
    switch (value) {
      case 1:
        return "Min";
      case 2:
        return "Max";
      default:
        return "Off";
    }
  }

  static uint8_t sensor_value_(const std::string &name) {
    if (name == "Min") {
      return 1;
    }
    if (name == "Max") {
      return 2;
    }
    return 0;
  }

  void sync_writes_() {
    uint8_t wanted_temp = this->temperature_;
    if (!std::isnan(this->temperature_number_->state)) {
      wanted_temp = static_cast<uint8_t>(this->temperature_number_->state);
    }
    const bool power = this->toggle_->state;
    const bool heat = power && this->heat_->state;
    const uint8_t mode = !power ? 0x06 : (heat ? 0x02 : 0x08);
    const bool power_changed =
        this->toggle_->state != this->flame_state_ || this->heat_->state != this->heat_state_;
    const bool temp_changed =
        this->temperature_number_->dirty() || wanted_temp != this->temperature_;

    if (power_changed) {
      uint8_t pkt[6] = {mode, 0, 0, 0, wanted_temp, 0};
      ESP_LOGI(TAG, "Set power=%s heat=%s temp=%u mode=0x%02X", YESNO(power), YESNO(heat), wanted_temp, mode);
      this->enqueue_write_(this->h_control_, pkt, sizeof(pkt));
      this->flame_state_ = power;
      this->heat_state_ = heat;
      this->toggle_->clear_dirty();
      this->heat_->clear_dirty();
      this->heat_->publish_state(heat);
      this->last_status_poll_ms_ = millis();
    }

    if (temp_changed) {
      // 0x1003 is the temperature-set register. Writing it makes the
      // fireplace display flash the setpoint, so only do this when the
      // user actually changes temperature — not on every on/off.
      this->temperature_ = wanted_temp;
      uint8_t tset[2] = {this->temperature_, 0};
      ESP_LOGI(TAG, "Set temp=%u", this->temperature_);
      this->enqueue_write_(this->h_temp_, tset, sizeof(tset));
      if (!power_changed) {
        uint8_t pkt[6] = {mode, 0, 0, 0, this->temperature_, 0};
        this->enqueue_write_(this->h_control_, pkt, sizeof(pkt));
      }
      this->temperature_number_->clear_dirty();
      this->last_status_poll_ms_ = millis();
    }

    if (this->have_volume_ && this->volume_ != nullptr && !std::isnan(this->volume_->state)) {
      uint8_t wanted = static_cast<uint8_t>(this->volume_->state);
      if (this->volume_->dirty() || wanted != this->volume_state_) {
        this->volume_state_ = wanted;
        this->volume_->clear_dirty();
        this->enqueue_write_(this->h_volume_, &wanted, 1);
      }
    }

    if (this->have_effects_) {
      uint8_t amb = this->ambient_state_;
      uint8_t fire = this->brightness_state_;
      bool dirty = false;
      if (this->ambient_ != nullptr && !std::isnan(this->ambient_->state)) {
        amb = static_cast<uint8_t>(this->ambient_->state);
        dirty = dirty || this->ambient_->dirty();
      }
      if (this->brightness_ != nullptr && !std::isnan(this->brightness_->state)) {
        fire = static_cast<uint8_t>(this->brightness_->state);
        dirty = dirty || this->brightness_->dirty();
      }
      if (dirty || amb != this->ambient_state_ || fire != this->brightness_state_) {
        this->effects_[2] = fire;
        this->brightness_state_ = fire;
        if (this->brightness_ != nullptr) {
          this->brightness_->clear_dirty();
        }
        if (amb == 8) {
          // 01 08 updates GATT but not the display. 0x1030 1-byte "+"
          // commands also do not step to AMB8. Leave the fireplace alone.
          ESP_LOGW(TAG, "AMB8 skip: BLE set does not move the display");
          this->ambient_state_ = 8;
          if (this->ambient_ != nullptr) {
            this->ambient_->clear_dirty();
          }
          this->last_status_poll_ms_ = millis();
        } else {
          uint8_t pkt[7] = {0x01, amb, fire, 0, 0, 0, 0};
          this->effects_[0] = 0;
          this->effects_[1] = amb;
          this->ambient_state_ = amb;
          if (this->ambient_ != nullptr) {
            this->ambient_->clear_dirty();
          }
          ESP_LOGI(TAG, "Set ambient=%u flame=%u", amb, fire);
          this->enqueue_write_(this->h_effects_, pkt, sizeof(pkt));
          this->last_status_poll_ms_ = millis();
        }
      }
    }

    if (this->have_timer_ && this->timer_ != nullptr && !std::isnan(this->timer_->state)) {
      uint8_t steps = static_cast<uint8_t>(std::lround(this->timer_->state * 2.0f));
      if (steps > 16) {
        steps = 16;
      }
      uint16_t minutes = static_cast<uint16_t>(steps) * 30;
      if (this->timer_->dirty() || minutes != this->timer_minutes_) {
        this->timer_minutes_ = minutes;
        this->timer_->clear_dirty();
        // Display showed 1.0h for BLE value 1, so the set index is 0-based:
        // 0.5h -> 0, 1.0h -> 1, ... 8.0h -> 15.
        uint8_t encoded = steps > 0 ? static_cast<uint8_t>(steps - 1) : 0;
        uint8_t ten[2] = {uint8_t(steps > 0 ? 1 : 0), encoded};
        ESP_LOGI(TAG, "Set timer hours=%.1f encoded=%u", this->timer_->state, encoded);
        this->enqueue_write_(this->h_timer_en_, ten, 2);
        this->last_status_poll_ms_ = millis();
      }
    }

    if (this->have_sensor_ && this->sensor_ != nullptr) {
      auto idx = this->sensor_->active_index();
      if (idx.has_value()) {
        uint8_t wanted = static_cast<uint8_t>(*idx);
        if (this->sensor_->dirty() || wanted != this->sensor_state_) {
          this->sensor_state_ = wanted;
          this->sensor_->clear_dirty();
          this->enqueue_write_(this->h_sensor_, &wanted, 1);
        }
      }
    }
  }

  struct Op {
    bool write{false};
    uint16_t handle{0};
    uint16_t len{0};
    uint8_t data[8]{};
  };

  FlameSwitch *toggle_;
  FlameSwitch *heat_;
  FlameNumber *temperature_number_;
  FlameNumber *volume_{nullptr};
  FlameNumber *ambient_{nullptr};
  FlameNumber *brightness_{nullptr};
  FlameNumber *timer_{nullptr};
  FlameSelect *sensor_{nullptr};

  ble_client::espbt::ESPBTUUID control_service_uuid_ =
      uuid128_from_string("00000000-0000-1000-8000-00805F9B34FB");
  ble_client::espbt::ESPBTUUID control_char_uuid_ = uuid128_from_string("00001001-0000-1000-8000-00805F9B34FB");

  bool flame_state_{false};
  bool heat_state_{false};
  uint8_t temperature_{0x20};
  uint8_t volume_state_{0};
  uint8_t ambient_state_{1};
  uint8_t brightness_state_{1};
  uint16_t timer_minutes_{0};
  uint8_t sensor_state_{0};
  uint8_t effects_[7]{0, 1, 1, 0, 0, 0, 0};
  uint16_t effects_len_{7};
  bool have_volume_{false};
  bool have_effects_{false};
  bool have_timer_{false};
  bool have_sensor_{false};
  bool security_configured_{false};
  bool op_pending_{false};

  uint16_t h_control_{0};
  uint16_t h_temp_{0};
  uint16_t h_volume_{0};
  uint16_t h_effects_{0};
  uint16_t h_cmd_{0};
  uint16_t h_fx5_{0};
  uint16_t h_fx4_{0};
  uint16_t h_custom_{0};
  uint16_t h_timer_en_{0};
  uint16_t h_timer_{0};
  uint16_t h_sensor_{0};
  uint32_t next_connect_attempt_ms_{0};
  uint32_t last_status_poll_ms_{0};
  std::vector<Op> ops_{};
};

}  // namespace flame
}  // namespace esphome
