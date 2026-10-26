#pragma once
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/ds1307/ds1307.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esp_partition.h"
#include <array>
#include <cstddef>
#include <ctime>

namespace esphome::attendance_web {
static constexpr size_t MAX_EMPLOYEES = 100;
static constexpr size_t MAX_DAILY_NOTES = 500;
static constexpr size_t LEGACY_MAX_DAILY_NOTES = 64;
static constexpr size_t LEGACY_PARTITION_MAX_DAILY_NOTES = 256;
static constexpr uint32_t LOG_MAGIC = 0x4154544E;
static constexpr uint32_t EMPLOYEE_MAGIC = 0x454D504C;
static constexpr uint16_t LEGACY_EMPLOYEE_STORE_VERSION = 1;
static constexpr uint16_t EMBEDDED_NOTE_EMPLOYEE_STORE_VERSION = 2;
static constexpr uint16_t EMPLOYEE_STORE_VERSION = 3;
static constexpr uint32_t EMPLOYEE_SLOT_SIZE = 0x3000;
static constexpr uint8_t EMPLOYEE_SLOT_COUNT = 2;
static constexpr uint32_t NOTE_MAGIC = 0x4E4F5445;
static constexpr uint16_t LEGACY_NOTE_STORE_VERSION = 1;
static constexpr uint16_t NOTE_STORE_VERSION = 2;
static constexpr uint32_t NOTE_SLOT_SIZE = 0x7000;
static constexpr uint8_t NOTE_SLOT_COUNT = 2;
static constexpr uint32_t LEGACY_ATTENDANCE_PARTITION_SIZE = 0x80000;
static constexpr uint32_t LEGACY_EMPLOYEE_STORAGE_OFFSET =
    LEGACY_ATTENDANCE_PARTITION_SIZE - EMPLOYEE_SLOT_SIZE * EMPLOYEE_SLOT_COUNT;
static constexpr uint32_t LEGACY_NOTE_PARTITION_OFFSET = 0x90000;
static constexpr uint32_t LEGACY_NOTE_SLOT_SIZE = 0x4000;
static constexpr uint8_t LOG_HIGH_WATER_PERCENT = 99;
static constexpr uint32_t ADMIN_PASSWORD_MAGIC = 0x41505744;
static constexpr uint16_t ADMIN_PASSWORD_STORE_VERSION = 1;
static constexpr size_t MAX_ADMIN_PASSWORD_BYTES = 64;
static constexpr uint32_t MODE_SCHEDULE_MAGIC = 0x4D534348;
static constexpr uint16_t LEGACY_MODE_SCHEDULE_STORE_VERSION = 1;
static constexpr uint16_t MODE_SCHEDULE_STORE_VERSION = 2;
static constexpr uint16_t DEFAULT_CLOCK_IN_MINUTE = 0;
static constexpr uint16_t DEFAULT_CLOCK_OUT_MINUTE = 14 * 60;
static constexpr uint16_t DEFAULT_OVERTIME_IN_MINUTE = 18 * 60;
static constexpr uint16_t DEFAULT_OVERTIME_OUT_MINUTE = 22 * 60;
enum AttendanceAction : uint8_t {
  ACTION_CLOCK_OUT = 0,
  ACTION_CLOCK_IN = 1,
  ACTION_OVERTIME_IN = 2,
  ACTION_OVERTIME_OUT = 3,
};
struct __attribute__((packed)) Employee { uint16_t id{0}; uint8_t active{0}; char code[17]{}; char name[41]{}; char uid[25]{}; };
struct __attribute__((packed)) DailyNote {
  uint32_t date{0};
  uint16_t employee_id{0};
  uint8_t active{0};
  uint8_t reserved{0};
  char text[49]{};
};
struct __attribute__((packed)) EmployeeStoreHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t generation;
  uint32_t payload_size;
  uint32_t payload_crc;
};
struct __attribute__((packed)) AdminPasswordStore {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  char password[MAX_ADMIN_PASSWORD_BYTES + 1];
  uint32_t crc;
};
struct __attribute__((packed)) LegacyModeScheduleStore {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint8_t reserved;
  uint16_t clock_in_minute;
  uint16_t clock_out_minute;
  uint32_t crc;
};
struct __attribute__((packed)) ModeScheduleStore {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint8_t action_mask;
  uint16_t clock_in_minute;
  uint16_t clock_out_minute;
  uint16_t overtime_in_minute;
  uint16_t overtime_out_minute;
  uint32_t crc;
};
// 32 bytes exactly, so each 4 KiB erase sector contains 128 complete records.
// v1.3.34 uses padding for a 4-byte marker and 8-byte insertion sequence;
// legacy records have zero padding. CRC covers these bytes in both layouts.
struct __attribute__((packed)) LogRecord { uint32_t magic; uint32_t timestamp; uint16_t employee_id; uint8_t action; uint8_t reserved; uint32_t uid_hash; uint8_t padding[12]; uint32_t crc; };
static_assert(sizeof(Employee) == 86, "Employee flash layout changed");
static_assert(sizeof(DailyNote) == 57, "Daily note flash layout changed");
static_assert(sizeof(EmployeeStoreHeader) == 20, "Employee store header layout changed");
static_assert(sizeof(LegacyModeScheduleStore) == 16, "Legacy mode schedule store layout changed");
static_assert(sizeof(ModeScheduleStore) == 20, "Mode schedule store layout changed");
static_assert(sizeof(EmployeeStoreHeader) + sizeof(std::array<Employee, MAX_EMPLOYEES>) <= EMPLOYEE_SLOT_SIZE,
              "Employee data exceed one Flash slot");
static_assert(sizeof(EmployeeStoreHeader) + sizeof(std::array<Employee, MAX_EMPLOYEES>) +
                      sizeof(std::array<DailyNote, LEGACY_MAX_DAILY_NOTES>) <= EMPLOYEE_SLOT_SIZE,
              "Legacy embedded note layout exceeds one employee Flash slot");
static_assert(sizeof(EmployeeStoreHeader) + sizeof(std::array<DailyNote, MAX_DAILY_NOTES>) <= NOTE_SLOT_SIZE,
              "Daily note data exceed one note Flash slot");

class AttendanceWeb : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void set_time(time::RealTimeClock *rtc) { rtc_ = rtc; }
  void set_hardware_rtc(ds1307::DS1307Component *rtc) { hardware_rtc_ = rtc; }
  void set_status_light(light::LightState *status_light) { status_light_ = status_light; }
  void set_wifi(wifi::WiFiComponent *wifi) { wifi_ = wifi; }
  void set_admin_password(const std::string &password) {
    configured_admin_password_ = password;
    admin_password_ = password;
  }
  void select_action(bool clock_in);
  void select_overtime_action(bool clock_in);
  void refresh_status_light();
  void restart_network_time_sync();
  bool network_time_write_allowed() const { return !manual_time_override_; }
  void mark_network_time_synced(const std::string &source);
  std::string last_sync_time_text() const;
  std::string time_source_text() const;
  std::string last_card_info_text() const;
  std::string last_card_employee_text() const { return last_card_employee_; }
  std::string last_card_uid_text() const { return last_card_uid_; }
  std::string last_card_result_text() const { return last_card_result_; }
  std::string attendance_mode_text() const;
  bool time_is_valid() const { return effective_now_().is_valid(); }
  bool hardware_rtc_is_connected() const { return hardware_rtc_ != nullptr && !hardware_rtc_->is_failed(); }
  bool network_is_connected() const { return wifi_ != nullptr && wifi_->is_connected(); }
  bool handle_tag(const std::string &tag);
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
 protected:
  void load_employees_(); bool save_employees_(); void load_notes_(); bool save_notes_(); void scan_flash_();
  bool load_employee_slot_(uint8_t slot, std::array<Employee, MAX_EMPLOYEES> *employees,
                           std::array<DailyNote, LEGACY_MAX_DAILY_NOTES> *embedded_notes,
                           uint32_t *generation, bool *has_embedded_notes) const;
  bool load_employee_slot_at_(uint32_t offset, std::array<Employee, MAX_EMPLOYEES> *employees,
                              std::array<DailyNote, LEGACY_MAX_DAILY_NOTES> *embedded_notes,
                              uint32_t *generation, bool *has_embedded_notes) const;
  bool load_note_slot_(uint8_t slot, std::array<DailyNote, MAX_DAILY_NOTES> *notes,
                       uint32_t *generation) const;
  bool load_legacy_note_slot_(uint8_t slot, std::array<DailyNote, MAX_DAILY_NOTES> *notes,
                              uint32_t *generation) const;
  uint32_t employee_slot_offset_(uint8_t slot) const;
  uint32_t note_slot_offset_(uint8_t slot) const;
  bool append_log_(uint16_t employee_id, uint8_t action, uint32_t uid_hash, uint32_t timestamp);
  int find_uid_(const std::string &uid) const; uint32_t cutoff_timestamp_() const;
  ESPTime effective_now_() const; void update_scheduled_mode_(); void purge_expired_notes_();
  void select_action_code_(uint8_t action);
  void remember_card_result_(const std::string &uid, const std::string &employee, const std::string &result);
  void send_json_(AsyncWebServerRequest *, int, const std::string &);
  void serve_page_(AsyncWebServerRequest *); void serve_status_(AsyncWebServerRequest *);
  void serve_employees_(AsyncWebServerRequest *); void serve_logs_(AsyncWebServerRequest *, bool);
  void serve_notes_(AsyncWebServerRequest *); void mutate_note_(AsyncWebServerRequest *);
  void serve_export_launcher_(AsyncWebServerRequest *); void serve_export_data_(AsyncWebServerRequest *);
  void mutate_employee_(AsyncWebServerRequest *); void delete_employee_(AsyncWebServerRequest *);
  void clear_logs_(AsyncWebServerRequest *);
  void set_attendance_mode_(AsyncWebServerRequest *);
  void manual_punch_(AsyncWebServerRequest *);
  void set_mode_schedule_(AsyncWebServerRequest *);
  void set_manual_time_(AsyncWebServerRequest *); void restore_automatic_time_(AsyncWebServerRequest *);
  void load_mode_schedule_(); bool save_mode_schedule_();
  void load_admin_password_(); bool save_admin_password_(const std::string &password);
  void apply_web_auth_password_(); void change_admin_password_(AsyncWebServerRequest *);
  bool check_import_token_(AsyncWebServerRequest *request) const;
  void begin_backup_import_(AsyncWebServerRequest *); void import_employee_(AsyncWebServerRequest *);
  void import_note_(AsyncWebServerRequest *); void import_log_batch_(AsyncWebServerRequest *);
  void finish_backup_import_(AsyncWebServerRequest *); void abort_backup_import_(AsyncWebServerRequest *);
  void show_enroll_color_(); void update_enroll_rainbow_(); void restore_mode_color_();
  time::RealTimeClock *rtc_{nullptr}; ds1307::DS1307Component *hardware_rtc_{nullptr};
  light::LightState *status_light_{nullptr};
  wifi::WiFiComponent *wifi_{nullptr};
  const esp_partition_t *partition_{nullptr};
  const esp_partition_t *notes_partition_{nullptr};
  std::array<Employee, MAX_EMPLOYEES> employees_{};
  // Large Flash read/verify buffers must be component members. Keeping employee
  // and note arrays on the stack would overflow the ESP32-C3 loop task stack.
  std::array<Employee, MAX_EMPLOYEES> employee_scratch_{};
  std::array<DailyNote, MAX_DAILY_NOTES> notes_{};
  std::array<DailyNote, MAX_DAILY_NOTES> note_scratch_{};
  std::array<DailyNote, LEGACY_MAX_DAILY_NOTES> embedded_notes_{};
  std::array<DailyNote, LEGACY_MAX_DAILY_NOTES> embedded_note_scratch_{};
  uint32_t log_region_size_{0};
  uint32_t log_physical_size_{0};
  uint32_t employee_storage_offset_{0};
  uint32_t employee_generation_{0};
  int8_t employee_active_slot_{-1};
  uint32_t note_generation_{0};
  int8_t note_active_slot_{-1};
  bool embedded_notes_available_{false};
  uint64_t next_log_sequence_{1};
  uint32_t write_offset_{0}; int8_t pending_action_{-1};
  int16_t enroll_employee_{-1}; std::string last_result_{"等待操作"};
  ESPPreferenceObject admin_password_pref_;
  ESPPreferenceObject mode_schedule_pref_;
  std::string configured_admin_password_{};
  std::string admin_password_{};
  bool runtime_admin_password_loaded_{false};
  bool import_in_progress_{false};
  uint32_t import_token_{0};
  uint32_t last_import_millis_{0};
  uint16_t imported_employee_count_{0};
  uint16_t imported_note_count_{0};
  uint32_t imported_log_count_{0};
  uint32_t last_import_timestamp_{0};
  bool manual_time_override_{false};
  bool network_time_synced_{false};
  std::string last_sync_source_{"本次開機尚未完成校時"};
  time_t last_sync_timestamp_{0};
  std::string last_card_employee_{"尚無刷卡紀錄"};
  std::string last_card_uid_{"尚無刷卡紀錄"};
  std::string last_card_result_{"尚無刷卡紀錄"};
  time_t last_card_timestamp_{0};
  time_t manual_time_base_{0};
  uint32_t manual_time_millis_{0};
  uint32_t last_time_check_millis_{0};
  uint32_t last_schedule_bucket_{0};
  bool mode_schedule_enabled_{true};
  uint8_t mode_schedule_action_mask_{(1U << ACTION_CLOCK_IN) | (1U << ACTION_CLOCK_OUT)};
  uint16_t clock_in_minute_{DEFAULT_CLOCK_IN_MINUTE};
  uint16_t clock_out_minute_{DEFAULT_CLOCK_OUT_MINUTE};
  uint16_t overtime_in_minute_{DEFAULT_OVERTIME_IN_MINUTE};
  uint16_t overtime_out_minute_{DEFAULT_OVERTIME_OUT_MINUTE};
  uint32_t last_retention_cleanup_date_{0};
  uint32_t last_rainbow_millis_{0};
  uint8_t rainbow_index_{0};
};
}  // namespace esphome::attendance_web
