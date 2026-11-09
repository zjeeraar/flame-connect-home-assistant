#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <esp_gattc_api.h>
#include <vector>

namespace esphome {
namespace flame_dump {

static const char *const TAG = "flame_dump";

struct TrackedChar {
  uint16_t handle{0};
  uint16_t uuid16{0};
  std::string uuid_str{};
  bool readable{false};
  bool notifiable{false};
  bool clock{false};
  std::string last_hex{};
};

class FlameDump : public Component, public ble_client::BLEClientNode {
 public:
  void setup() override {
    this->configure_security_once_();
    this->next_connect_attempt_ms_ = millis() + 5000;
  }

  void loop() override {
    using ClientState = esp32_ble_tracker::ClientState;
    const auto state = this->parent()->state();

    if (state == ClientState::IDLE && millis() >= this->next_connect_attempt_ms_) {
      ESP_LOGW(TAG, "Not connected — connecting");
      this->configure_security_once_();
      this->next_connect_attempt_ms_ = millis() + 30000;
      this->parent()->connect();
      return;
    }

    if (this->chars_.empty() || !this->parent()->connected()) {
      return;
    }

    if (this->round_active_) {
      return;
    }

    if (millis() - this->last_round_ms_ < 10000) {
      return;
    }

    this->start_read_round_();
  }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
      case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "Connected, requesting encryption");
        this->parent()->pair();
        break;

      case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "Disconnected");
        this->chars_.clear();
        this->round_active_ = false;
        this->next_connect_attempt_ms_ = millis() + 10000;
        break;

      case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGI(TAG, "Service discovery complete — collecting characteristics");
        this->collect_characteristics_();
        this->subscribe_notifications_();
        this->start_read_round_();
        break;

      case ESP_GATTC_READ_CHAR_EVT:
        this->log_value_("READ", param->read.handle, param->read.status, param->read.value, param->read.value_len);
        this->continue_read_round_();
        break;

      case ESP_GATTC_NOTIFY_EVT:
        this->log_value_("NOTIFY", param->notify.handle, ESP_GATT_OK, param->notify.value, param->notify.value_len);
        break;

      default:
        break;
    }
  }

 protected:
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

  static std::string uuid_to_str_(const esp_bt_uuid_t &uuid) {
    char buf[40];
    if (uuid.len == ESP_UUID_LEN_16) {
      snprintf(buf, sizeof(buf), "0x%04X", uuid.uuid.uuid16);
      return buf;
    }
    if (uuid.len == ESP_UUID_LEN_32) {
      snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long) uuid.uuid.uuid32);
      return buf;
    }
    const uint8_t *p = uuid.uuid.uuid128;
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", p[15], p[14],
             p[13], p[12], p[11], p[10], p[9], p[8], p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    return buf;
  }

  static uint16_t uuid16_of_(const esp_bt_uuid_t &uuid) {
    if (uuid.len == ESP_UUID_LEN_16) {
      return uuid.uuid.uuid16;
    }
    if (uuid.len == ESP_UUID_LEN_128) {
      return uuid.uuid.uuid128[12] | (uint16_t)(uuid.uuid.uuid128[13] << 8);
    }
    return 0;
  }

  static const char *db_type_str_(uint8_t type) {
    switch (type) {
      case ESP_GATT_DB_PRIMARY_SERVICE:
        return "SVC";
      case ESP_GATT_DB_SECONDARY_SERVICE:
        return "SEC";
      case ESP_GATT_DB_CHARACTERISTIC:
        return "CHAR";
      case ESP_GATT_DB_DESCRIPTOR:
        return "DESC";
      case ESP_GATT_DB_INCLUDED_SERVICE:
        return "INC";
      default:
        return "?";
    }
  }

  void add_char_from_db_(const esp_gattc_db_elem_t &elem) {
    if (elem.type != ESP_GATT_DB_CHARACTERISTIC) {
      return;
    }
    for (const auto &existing : this->chars_) {
      if (existing.handle == elem.attribute_handle) {
        return;
      }
    }
    TrackedChar tracked;
    tracked.handle = elem.attribute_handle;
    tracked.uuid16 = uuid16_of_(elem.uuid);
    tracked.uuid_str = uuid_to_str_(elem.uuid);
    tracked.readable = (elem.properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0;
    tracked.notifiable =
        (elem.properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) != 0;
    tracked.clock = (tracked.uuid16 == 0x0006 || tracked.uuid16 == 0x0106);
    ESP_LOGI(TAG, "Found uuid=%s handle=%u props=0x%02X read=%s notify=%s", tracked.uuid_str.c_str(), tracked.handle,
             elem.properties, YESNO(tracked.readable), YESNO(tracked.notifiable));
    this->chars_.push_back(tracked);
  }

  void collect_characteristics_() {
    this->chars_.clear();
    auto *client = this->parent();
    uint16_t count = 0;
    esp_gatt_status_t st = esp_ble_gattc_get_attr_count(client->get_gattc_if(), client->get_conn_id(), ESP_GATT_DB_ALL,
                                                        0x0001, 0xFFFF, 0, &count);
    if (st != ESP_GATT_OK || count == 0) {
      ESP_LOGW(TAG, "GATT db count failed status=%d count=%u — falling back to known UUIDs", st, count);
      this->collect_known_characteristics_();
      return;
    }

    std::vector<esp_gattc_db_elem_t> db(count);
    st = esp_ble_gattc_get_db(client->get_gattc_if(), client->get_conn_id(), 0x0001, 0xFFFF, db.data(), &count);
    if (st != ESP_GATT_OK) {
      ESP_LOGW(TAG, "GATT db read failed status=%d — falling back to known UUIDs", st);
      this->collect_known_characteristics_();
      return;
    }

    ESP_LOGI(TAG, "----- GATT map (%u attrs) -----", count);
    for (uint16_t i = 0; i < count; i++) {
      const auto &elem = db[i];
      ESP_LOGI(TAG, "DB %s handle=%u uuid=%s props=0x%02X", db_type_str_(elem.type), elem.attribute_handle,
               uuid_to_str_(elem.uuid).c_str(), elem.properties);
      this->add_char_from_db_(elem);
    }
    ESP_LOGI(TAG, "----- GATT map done, %u chars -----", (unsigned) this->chars_.size());
    if (this->chars_.empty()) {
      ESP_LOGW(TAG, "No characteristics in GATT db — falling back to known UUIDs");
      this->collect_known_characteristics_();
    }
  }

  void collect_known_characteristics_() {
    const uint16_t services[] = {0x0000, 0x1001, 0x1800, 0x180A};
    const uint16_t uuids[] = {0x0006, 0x0106, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007};
    for (uint16_t service : services) {
      for (uint16_t uuid : uuids) {
        auto *chr = this->parent()->get_characteristic(service, uuid);
        if (chr == nullptr) {
          continue;
        }
        esp_gattc_db_elem_t elem{};
        elem.type = ESP_GATT_DB_CHARACTERISTIC;
        elem.attribute_handle = chr->handle;
        elem.properties = chr->properties;
        elem.uuid.len = ESP_UUID_LEN_16;
        elem.uuid.uuid.uuid16 = uuid;
        this->add_char_from_db_(elem);
      }
    }
  }

  void subscribe_notifications_() {
    for (const auto &chr : this->chars_) {
      if (!chr.notifiable) {
        continue;
      }
      esp_err_t err = this->parent()->register_for_notify(chr.handle);
      ESP_LOGI(TAG, "Notify subscribe uuid=%s handle=%u err=%d", chr.uuid_str.c_str(), chr.handle, err);
    }
  }

  void start_read_round_() {
    this->read_index_ = 0;
    this->round_active_ = true;
    this->last_round_ms_ = millis();
    ESP_LOGI(TAG, "----- dump round -----");
    this->continue_read_round_();
  }

  void continue_read_round_() {
    while (this->read_index_ < this->chars_.size()) {
      const auto &chr = this->chars_[this->read_index_++];
      if (!chr.readable || chr.clock) {
        continue;
      }
      esp_err_t err = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), chr.handle,
                                              ESP_GATT_AUTH_REQ_MITM);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read request failed uuid=%s handle=%u err=%d", chr.uuid_str.c_str(), chr.handle, err);
        continue;
      }
      return;
    }
    this->round_active_ = false;
    ESP_LOGI(TAG, "----- dump round done -----");
  }

  void log_value_(const char *kind, uint16_t handle, esp_gatt_status_t status, const uint8_t *data, uint16_t len) {
    TrackedChar *tracked = nullptr;
    for (auto &chr : this->chars_) {
      if (chr.handle == handle) {
        tracked = &chr;
        break;
      }
    }
    const char *uuid = tracked != nullptr ? tracked->uuid_str.c_str() : "?";
    if (status != ESP_GATT_OK) {
      ESP_LOGW(TAG, "%s uuid=%s handle=%u status=%d", kind, uuid, handle, status);
      return;
    }
    char hex[192];
    size_t pos = 0;
    for (uint16_t i = 0; i < len && pos + 3 < sizeof(hex); i++) {
      pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
    }
    if (pos > 0) {
      hex[pos - 1] = '\0';
    } else {
      hex[0] = '\0';
    }
    if (tracked != nullptr && tracked->last_hex == hex) {
      return;
    }
    const char *prefix = (tracked != nullptr && !tracked->last_hex.empty()) ? "CHANGED" : kind;
    ESP_LOGI(TAG, "%s uuid=%s handle=%u len=%u data=%s", prefix, uuid, handle, len, hex);
    if (tracked != nullptr) {
      tracked->last_hex = hex;
    }
  }

  std::vector<TrackedChar> chars_{};
  size_t read_index_{0};
  bool round_active_{false};
  bool security_configured_{false};
  uint32_t next_connect_attempt_ms_{0};
  uint32_t last_round_ms_{0};
};

}  // namespace flame_dump
}  // namespace esphome
