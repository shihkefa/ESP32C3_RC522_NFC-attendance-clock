#include "attendance_web.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_system.h"

namespace esphome::attendance_web {
static const char *const TAG = "attendance_web";
static constexpr float STATUS_LIGHT_BRIGHTNESS = 0.65f;
static const char *action_label(uint8_t action) {
  switch (action) {
    case ACTION_CLOCK_OUT:
      return "下班";
    case ACTION_CLOCK_IN:
      return "上班";
    case ACTION_OVERTIME_IN:
      return "加班上班";
    case ACTION_OVERTIME_OUT:
      return "加班下班";
    default:
      return "未知";
  }
}
static uint32_t fnv1a_update(uint32_t hash, const void *data, size_t len) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  while (len--) {
    hash ^= *bytes++;
    hash *= 16777619U;
  }
  return hash;
}
static uint32_t fnv1a(const void *data, size_t len) { return fnv1a_update(2166136261U, data, len); }
static uint32_t embedded_employee_store_crc(
    const std::array<Employee, MAX_EMPLOYEES> &employees,
    const std::array<DailyNote, LEGACY_MAX_DAILY_NOTES> &notes) {
  return fnv1a_update(fnv1a(employees.data(), sizeof(employees)), notes.data(), sizeof(notes));
}
static std::string esc(const char *s) { std::string o; for (; *s; s++) { if (*s=='"'||*s=='\\') o+='\\'; if ((uint8_t)*s>=32) o+=*s; } return o; }
static std::string csv_quote(const char *value) {
  std::string output = "\"";
  for (; *value; value++) {
    if (*value == '"')
      output += '"';
    output += *value;
  }
  output += '"';
  return output;
}
static int hex_value(char c) { if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='F')return c-'A'+10; if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static std::string url_decode_value(const std::string &in) { std::string out; out.reserve(in.size()); for(size_t i=0;i<in.size();i++) { if(in[i]=='%'&&i+2<in.size()) { int a=hex_value(in[i+1]),b=hex_value(in[i+2]); if(a>=0&&b>=0) { out.push_back((char)((a<<4)|b)); i+=2; continue; } } out.push_back(in[i]=='+'?' ':in[i]); } return out; }
static std::string param(AsyncWebServerRequest *r, const char *name) { return r->hasParam(name) ? url_decode_value(r->getParam(name)->value().c_str()) : ""; }
static bool parse_uint32_strict(const std::string &value, uint32_t *output) {
  if (output == nullptr || value.empty())
    return false;
  uint64_t parsed = 0;
  for (const char character : value) {
    if (character < '0' || character > '9')
      return false;
    parsed = parsed * 10U + static_cast<uint8_t>(character - '0');
    if (parsed > UINT32_MAX)
      return false;
  }
  *output = static_cast<uint32_t>(parsed);
  return true;
}
static bool is_ascii_space(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}
static std::string trim_ascii_space(const std::string &value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && is_ascii_space(value[begin]))
    begin++;
  while (end > begin && is_ascii_space(value[end - 1]))
    end--;
  return value.substr(begin, end - begin);
}
static char ascii_upper(char value) {
  return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}
static bool employee_code_equal(const std::string &left, const char *right) {
  const size_t right_size = strlen(right);
  if (left.size() != right_size)
    return false;
  for (size_t index = 0; index < left.size(); index++) {
    if (ascii_upper(left[index]) != ascii_upper(right[index]))
      return false;
  }
  return true;
}
static size_t utf8_character_count(const std::string &value) {
  size_t count = 0;
  for (const unsigned char byte : value) {
    if ((byte & 0xC0U) != 0x80U)
      count++;
  }
  return count;
}
static bool secure_equals(const std::string &left, const std::string &right) {
  const size_t compare_length = std::max(left.size(), right.size());
  size_t difference = left.size() ^ right.size();
  for (size_t i = 0; i < compare_length; i++) {
    const uint8_t left_value = i < left.size() ? static_cast<uint8_t>(left[i]) : 0;
    const uint8_t right_value = i < right.size() ? static_cast<uint8_t>(right[i]) : 0;
    difference |= left_value ^ right_value;
  }
  return difference == 0;
}
static bool valid_new_admin_password(const std::string &password) {
  if (password.size() < 8 || password.size() > MAX_ADMIN_PASSWORD_BYTES)
    return false;
  for (const unsigned char byte : password) {
    if (byte < 0x21U || byte > 0x7EU)
      return false;
  }
  return true;
}
static bool parse_date_value(const std::string &value, uint32_t *date) {
  if (date == nullptr || value.size() != 10 || value[4] != '-' || value[7] != '-')
    return false;
  for (size_t index = 0; index < value.size(); index++) {
    if (index != 4 && index != 7 && (value[index] < '0' || value[index] > '9'))
      return false;
  }
  const int year = atoi(value.substr(0, 4).c_str());
  const int month = atoi(value.substr(5, 2).c_str());
  const int day = atoi(value.substr(8, 2).c_str());
  if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31)
    return false;
  struct tm candidate {};
  candidate.tm_year = year - 1900;
  candidate.tm_mon = month - 1;
  candidate.tm_mday = day;
  candidate.tm_hour = 12;
  const time_t timestamp = mktime(&candidate);
  struct tm normalized {};
  localtime_r(&timestamp, &normalized);
  if (normalized.tm_year != year - 1900 || normalized.tm_mon != month - 1 || normalized.tm_mday != day)
    return false;
  *date = static_cast<uint32_t>(year * 10000 + month * 100 + day);
  return true;
}
static bool parse_time_value(const std::string &value, uint16_t *minute_of_day) {
  if (minute_of_day == nullptr || value.size() != 5 || value[2] != ':' ||
      value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9' ||
      value[3] < '0' || value[3] > '9' || value[4] < '0' || value[4] > '9')
    return false;
  const unsigned hour = static_cast<unsigned>((value[0] - '0') * 10 + value[1] - '0');
  const unsigned minute = static_cast<unsigned>((value[3] - '0') * 10 + value[4] - '0');
  if (hour > 23 || minute > 59)
    return false;
  *minute_of_day = static_cast<uint16_t>(hour * 60U + minute);
  return true;
}
static std::string format_time_value(uint16_t minute_of_day) {
  char output[8]{};
  snprintf(output, sizeof(output), "%02u:%02u", static_cast<unsigned>(minute_of_day / 60U),
           static_cast<unsigned>(minute_of_day % 60U));
  return output;
}
static bool valid_mode_schedule_values(uint8_t action_mask, uint16_t clock_in_minute,
                                       uint16_t clock_out_minute, uint16_t overtime_in_minute,
                                       uint16_t overtime_out_minute) {
  if ((action_mask & 0xF0U) != 0)
    return false;
  const uint16_t minutes[4] = {clock_out_minute, clock_in_minute, overtime_in_minute,
                               overtime_out_minute};
  for (uint8_t action = 0; action < 4; action++) {
    if (minutes[action] >= 24U * 60U)
      return false;
    if ((action_mask & (1U << action)) == 0)
      continue;
    for (uint8_t other = action + 1; other < 4; other++) {
      if ((action_mask & (1U << other)) != 0 && minutes[action] == minutes[other])
        return false;
    }
  }
  return true;
}
static void format_date_value(uint32_t date, char *output, size_t output_size) {
  snprintf(output, output_size, "%04u-%02u-%02u", static_cast<unsigned>(date / 10000U),
           static_cast<unsigned>((date / 100U) % 100U), static_cast<unsigned>(date % 100U));
}

void AttendanceWeb::setup() {
  load_admin_password_();
  load_mode_schedule_();
  apply_web_auth_password_();
  partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x40, "attendance");
  if (!partition_) { ESP_LOGE(TAG, "attendance partition missing"); mark_failed(); return; }
  notes_partition_ =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x41, "att_notes");
  const uint32_t employee_storage_size = EMPLOYEE_SLOT_SIZE * EMPLOYEE_SLOT_COUNT;
  if (partition_->size <= employee_storage_size || EMPLOYEE_SLOT_SIZE % partition_->erase_size != 0) {
    ESP_LOGE(TAG, "attendance partition is too small or has incompatible erase geometry");
    mark_failed();
    return;
  }
  employee_storage_offset_ = partition_->size - employee_storage_size;
  log_physical_size_ = employee_storage_offset_;
  const uint32_t target_log_size = static_cast<uint32_t>(
      (static_cast<uint64_t>(log_physical_size_) * LOG_HIGH_WATER_PERCENT + 99U) / 100U);
  log_region_size_ = ((target_log_size + partition_->erase_size - 1U) / partition_->erase_size) *
                     partition_->erase_size;
  if (log_region_size_ > log_physical_size_)
    log_region_size_ = (log_physical_size_ / partition_->erase_size) * partition_->erase_size;
  if (log_region_size_ == 0) {
    ESP_LOGE(TAG, "attendance log region is unavailable");
    mark_failed();
    return;
  }
  load_employees_();
  if (notes_partition_ != nullptr && notes_partition_->size >= NOTE_SLOT_SIZE * NOTE_SLOT_COUNT &&
      NOTE_SLOT_SIZE % notes_partition_->erase_size == 0) {
    load_notes_();
  } else {
    notes_partition_ = nullptr;
    notes_.fill(DailyNote{});
    ESP_LOGE(TAG, "att_notes partition missing or incompatible; daily notes are unavailable");
  }
  scan_flash_();
  if (web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(this);
}

void AttendanceWeb::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Attendance Web: /attendance, employees=%u, log capacity=%u, high-water=%u%%, "
                "log offset=%u, employee generation=%u",
                (unsigned) MAX_EMPLOYEES, (unsigned) (log_region_size_ / sizeof(LogRecord)),
                (unsigned) LOG_HIGH_WATER_PERCENT, (unsigned) write_offset_, (unsigned) employee_generation_);
  ESP_LOGCONFIG(TAG, "DS3231 hardware RTC: %s",
                hardware_rtc_ != nullptr && !hardware_rtc_->is_failed() ? "connected" : "unavailable");
  ESP_LOGCONFIG(TAG, "Daily note storage: %s, capacity=%u, generation=%u",
                notes_partition_ != nullptr ? "available" : "unavailable", (unsigned) MAX_DAILY_NOTES,
                (unsigned) note_generation_);
  ESP_LOGCONFIG(TAG, "Shared management password: %s",
                runtime_admin_password_loaded_ ? "runtime password from Flash" : "YAML factory password");
  ESP_LOGCONFIG(TAG, "Automatic mode schedule: %s, mask=0x%02X, clock-in=%s, clock-out=%s, "
                     "overtime-in=%s, overtime-out=%s",
                mode_schedule_enabled_ ? "enabled" : "disabled", (unsigned) mode_schedule_action_mask_,
                format_time_value(clock_in_minute_).c_str(), format_time_value(clock_out_minute_).c_str(),
                format_time_value(overtime_in_minute_).c_str(), format_time_value(overtime_out_minute_).c_str());
}

void AttendanceWeb::load_admin_password_() {
  admin_password_pref_ = global_preferences->make_preference<AdminPasswordStore>(0xA771E024, true);
  runtime_admin_password_loaded_ = false;

  AdminPasswordStore store{};
  if (!admin_password_pref_.load(&store) || store.magic != ADMIN_PASSWORD_MAGIC ||
      store.version != ADMIN_PASSWORD_STORE_VERSION || store.length < 8 ||
      store.length > MAX_ADMIN_PASSWORD_BYTES || store.password[store.length] != '\0' ||
      store.crc != fnv1a(&store, offsetof(AdminPasswordStore, crc))) {
    admin_password_ = configured_admin_password_;
    ESP_LOGI(TAG, "Using the YAML factory management password");
    return;
  }

  std::string stored_password(store.password, store.length);
  if (!valid_new_admin_password(stored_password)) {
    std::fill(stored_password.begin(), stored_password.end(), '\0');
    admin_password_ = configured_admin_password_;
    ESP_LOGW(TAG, "Stored management password is invalid; using the YAML factory password");
    return;
  }
  admin_password_ = std::move(stored_password);
  runtime_admin_password_loaded_ = true;
  ESP_LOGI(TAG, "Loaded the runtime management password from Flash");
}

bool AttendanceWeb::save_admin_password_(const std::string &password) {
  if (!valid_new_admin_password(password))
    return false;
  AdminPasswordStore store{};
  store.magic = ADMIN_PASSWORD_MAGIC;
  store.version = ADMIN_PASSWORD_STORE_VERSION;
  store.length = static_cast<uint16_t>(password.size());
  memcpy(store.password, password.data(), password.size());
  store.password[password.size()] = '\0';
  store.crc = fnv1a(&store, offsetof(AdminPasswordStore, crc));
  const bool saved = admin_password_pref_.save(&store);
  memset(store.password, 0, sizeof(store.password));
  return saved;
}

void AttendanceWeb::load_mode_schedule_() {
  mode_schedule_pref_ = global_preferences->make_preference<ModeScheduleStore>(0xA771E033, true);
  mode_schedule_enabled_ = true;
  mode_schedule_action_mask_ = (1U << ACTION_CLOCK_IN) | (1U << ACTION_CLOCK_OUT);
  clock_in_minute_ = DEFAULT_CLOCK_IN_MINUTE;
  clock_out_minute_ = DEFAULT_CLOCK_OUT_MINUTE;
  overtime_in_minute_ = DEFAULT_OVERTIME_IN_MINUTE;
  overtime_out_minute_ = DEFAULT_OVERTIME_OUT_MINUTE;

  ModeScheduleStore store{};
  if (mode_schedule_pref_.load(&store) && store.magic == MODE_SCHEDULE_MAGIC &&
      store.version == MODE_SCHEDULE_STORE_VERSION && store.enabled <= 1 &&
      (store.enabled == 0 || store.action_mask != 0) &&
      valid_mode_schedule_values(store.action_mask, store.clock_in_minute, store.clock_out_minute,
                                 store.overtime_in_minute, store.overtime_out_minute) &&
      store.crc == fnv1a(&store, offsetof(ModeScheduleStore, crc))) {
    mode_schedule_enabled_ = store.enabled != 0;
    mode_schedule_action_mask_ = store.action_mask;
    clock_in_minute_ = store.clock_in_minute;
    clock_out_minute_ = store.clock_out_minute;
    overtime_in_minute_ = store.overtime_in_minute;
    overtime_out_minute_ = store.overtime_out_minute;
    ESP_LOGI(TAG, "Loaded the four-mode automatic schedule from Flash");
    return;
  }

  auto legacy_pref = global_preferences->make_preference<LegacyModeScheduleStore>(0xA771E032, true);
  LegacyModeScheduleStore legacy{};
  if (legacy_pref.load(&legacy) && legacy.magic == MODE_SCHEDULE_MAGIC &&
      legacy.version == LEGACY_MODE_SCHEDULE_STORE_VERSION && legacy.enabled <= 1 &&
      legacy.clock_in_minute < 24U * 60U && legacy.clock_out_minute < 24U * 60U &&
      legacy.clock_in_minute != legacy.clock_out_minute &&
      legacy.crc == fnv1a(&legacy, offsetof(LegacyModeScheduleStore, crc))) {
    mode_schedule_enabled_ = legacy.enabled != 0;
    clock_in_minute_ = legacy.clock_in_minute;
    clock_out_minute_ = legacy.clock_out_minute;
    if (save_mode_schedule_())
      ESP_LOGI(TAG, "Migrated the v1.3.32 mode schedule; overtime schedules remain disabled");
    else
      ESP_LOGW(TAG, "Loaded the v1.3.32 mode schedule but could not migrate it to the new format");
    return;
  }

  ESP_LOGI(TAG, "Using the default schedule; overtime schedules are disabled");
}

bool AttendanceWeb::save_mode_schedule_() {
  ModeScheduleStore store{};
  store.magic = MODE_SCHEDULE_MAGIC;
  store.version = MODE_SCHEDULE_STORE_VERSION;
  store.enabled = mode_schedule_enabled_ ? 1 : 0;
  store.action_mask = mode_schedule_action_mask_;
  store.clock_in_minute = clock_in_minute_;
  store.clock_out_minute = clock_out_minute_;
  store.overtime_in_minute = overtime_in_minute_;
  store.overtime_out_minute = overtime_out_minute_;
  store.crc = fnv1a(&store, offsetof(ModeScheduleStore, crc));
  return mode_schedule_pref_.save(&store);
}

void AttendanceWeb::apply_web_auth_password_() {
#ifdef USE_WEBSERVER_AUTH
  if (web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->set_auth_password(admin_password_.c_str());
    return;
  }
#endif
  ESP_LOGE(TAG, "Web server authentication is unavailable; shared password could not be applied");
}

ESPTime AttendanceWeb::effective_now_() const {
  if (manual_time_override_) {
    const uint32_t elapsed_seconds = (millis() - manual_time_millis_) / 1000U;
    return ESPTime::from_epoch_local(manual_time_base_ + elapsed_seconds);
  }
  if (rtc_ == nullptr)
    return ESPTime{};
  return rtc_->now();
}

static std::string format_timestamp_text(time_t timestamp) {
  if (timestamp <= 0)
    return "尚無紀錄";
  struct tm time_info {};
  localtime_r(&timestamp, &time_info);
  char output[24]{};
  if (strftime(output, sizeof(output), "%Y-%m-%d %H:%M:%S", &time_info) == 0)
    return "時間格式錯誤";
  return output;
}

std::string AttendanceWeb::last_sync_time_text() const {
  return last_sync_timestamp_ > 0 ? format_timestamp_text(last_sync_timestamp_) : "本次開機尚未完成校時";
}

std::string AttendanceWeb::time_source_text() const {
  if (manual_time_override_)
    return "管理員手動校時（優先）";
  if (network_time_synced_)
    return last_sync_source_;
  if (effective_now_().is_valid() && hardware_rtc_is_connected())
    return "DS3231 硬體時鐘";
  return "等待有效時間";
}

std::string AttendanceWeb::last_card_info_text() const {
  const std::string time_text = last_card_timestamp_ > 0 ? format_timestamp_text(last_card_timestamp_) : "時間未同步";
  return time_text + "｜姓名：" + last_card_employee_ + "｜卡號：" + last_card_uid_ + "｜結果：" +
         last_card_result_;
}

std::string AttendanceWeb::attendance_mode_text() const {
  if (pending_action_ < ACTION_CLOCK_OUT || pending_action_ > ACTION_OVERTIME_OUT)
    return "未設定";
  return action_label(static_cast<uint8_t>(pending_action_));
}

void AttendanceWeb::mark_network_time_synced(const std::string &source) {
  if (manual_time_override_)
    return;
  network_time_synced_ = true;
  last_sync_source_ = source.empty() ? "網路自動校時" : source;
  const ESPTime now = effective_now_();
  if (now.is_valid())
    last_sync_timestamp_ = now.timestamp;
}

void AttendanceWeb::remember_card_result_(const std::string &uid, const std::string &employee,
                                          const std::string &result) {
  last_card_uid_ = uid.empty() ? "未知" : uid;
  last_card_employee_ = employee.empty() ? "未登記" : employee;
  last_card_result_ = result;
  const ESPTime now = effective_now_();
  last_card_timestamp_ = now.is_valid() ? now.timestamp : 0;
}

void AttendanceWeb::update_scheduled_mode_() {
  if (!mode_schedule_enabled_)
    return;
  const ESPTime now = effective_now_();
  if (!now.is_valid())
    return;
  const uint16_t current_minute = static_cast<uint16_t>(now.hour * 60U + now.minute);
  const uint16_t schedule_minutes[4] = {clock_out_minute_, clock_in_minute_, overtime_in_minute_,
                                        overtime_out_minute_};
  uint16_t minutes_since_latest = 24U * 60U;
  int8_t latest_action = -1;
  for (uint8_t action = ACTION_CLOCK_OUT; action <= ACTION_OVERTIME_OUT; action++) {
    if ((mode_schedule_action_mask_ & (1U << action)) == 0)
      continue;
    const uint16_t elapsed =
        static_cast<uint16_t>((current_minute + 24U * 60U - schedule_minutes[action]) % (24U * 60U));
    if (elapsed < minutes_since_latest) {
      minutes_since_latest = elapsed;
      latest_action = static_cast<int8_t>(action);
    }
  }
  if (latest_action < 0)
    return;
  const uint32_t schedule_bucket = static_cast<uint32_t>(now.timestamp / 60U) - minutes_since_latest;
  if (schedule_bucket == last_schedule_bucket_)
    return;
  last_schedule_bucket_ = schedule_bucket;
  select_action_code_(static_cast<uint8_t>(latest_action));
}

void AttendanceWeb::loop() {
  const uint32_t now_millis = millis();
  if (import_in_progress_ && now_millis - last_import_millis_ > 1800000U) {
    import_in_progress_ = false;
    import_token_ = 0;
    load_employees_();
    load_notes_();
    scan_flash_();
    last_result_ = "資料還原逾時，請重新選擇備份檔匯入";
    ESP_LOGW(TAG, "Backup import session timed out");
  }
  if (enroll_employee_ >= 0 && wifi_ != nullptr && wifi_->is_connected() &&
      now_millis - last_rainbow_millis_ >= 250U) {
    last_rainbow_millis_ = now_millis;
    update_enroll_rainbow_();
  }
  if (now_millis - last_time_check_millis_ < 1000U)
    return;
  last_time_check_millis_ = now_millis;
  update_scheduled_mode_();
  const ESPTime current_time = effective_now_();
  if (current_time.is_valid()) {
    const uint32_t current_date = static_cast<uint32_t>(current_time.year) * 10000U +
                                  static_cast<uint32_t>(current_time.month) * 100U + current_time.day_of_month;
    if (current_date != last_retention_cleanup_date_) {
      last_retention_cleanup_date_ = current_date;
      purge_expired_notes_();
    }
  }
}

void AttendanceWeb::purge_expired_notes_() {
  if (notes_partition_ == nullptr)
    return;
  uint32_t cutoff_date = 0;
  const uint32_t cutoff = cutoff_timestamp_();
  if (cutoff == 0)
    return;
  time_t timestamp = cutoff;
  struct tm time_info {};
  localtime_r(&timestamp, &time_info);
  cutoff_date = static_cast<uint32_t>((time_info.tm_year + 1900) * 10000 +
                                      (time_info.tm_mon + 1) * 100 + time_info.tm_mday);
  size_t removed = 0;
  for (auto &note : notes_) {
    if (note.active && note.date < cutoff_date) {
      note = DailyNote{};
      removed++;
    }
  }
  if (removed == 0)
    return;
  if (save_notes_())
    ESP_LOGI(TAG, "Automatically removed %u daily notes older than two calendar months", (unsigned) removed);
  else
    ESP_LOGE(TAG, "Expired daily notes were removed from RAM but could not be committed to Flash");
}

uint32_t AttendanceWeb::employee_slot_offset_(uint8_t slot) const {
  return employee_storage_offset_ + static_cast<uint32_t>(slot) * EMPLOYEE_SLOT_SIZE;
}

uint32_t AttendanceWeb::note_slot_offset_(uint8_t slot) const {
  return static_cast<uint32_t>(slot) * NOTE_SLOT_SIZE;
}

bool AttendanceWeb::load_employee_slot_(uint8_t slot, std::array<Employee, MAX_EMPLOYEES> *employees,
                                        std::array<DailyNote, LEGACY_MAX_DAILY_NOTES> *embedded_notes,
                                        uint32_t *generation, bool *has_embedded_notes) const {
  if (partition_ == nullptr || slot >= EMPLOYEE_SLOT_COUNT || employees == nullptr || embedded_notes == nullptr ||
      generation == nullptr || has_embedded_notes == nullptr)
    return false;
  return load_employee_slot_at_(employee_slot_offset_(slot), employees, embedded_notes, generation,
                                has_embedded_notes);
}

bool AttendanceWeb::load_employee_slot_at_(uint32_t offset, std::array<Employee, MAX_EMPLOYEES> *employees,
                                           std::array<DailyNote, LEGACY_MAX_DAILY_NOTES> *embedded_notes,
                                           uint32_t *generation, bool *has_embedded_notes) const {
  if (partition_ == nullptr || employees == nullptr || embedded_notes == nullptr || generation == nullptr ||
      has_embedded_notes == nullptr || offset + EMPLOYEE_SLOT_SIZE > partition_->size)
    return false;
  EmployeeStoreHeader header{};
  if (esp_partition_read(partition_, offset, &header, sizeof(header)) != ESP_OK)
    return false;
  if (header.magic != EMPLOYEE_MAGIC)
    return false;
  const uint32_t payload_offset = offset + sizeof(header);
  if ((header.version == LEGACY_EMPLOYEE_STORE_VERSION || header.version == EMPLOYEE_STORE_VERSION) &&
      header.payload_size == sizeof(*employees)) {
    if (esp_partition_read(partition_, payload_offset, employees->data(), sizeof(*employees)) != ESP_OK ||
        header.payload_crc != fnv1a(employees->data(), sizeof(*employees)))
      return false;
    embedded_notes->fill(DailyNote{});
    *has_embedded_notes = false;
  } else if (header.version == EMBEDDED_NOTE_EMPLOYEE_STORE_VERSION &&
             header.payload_size == sizeof(*employees) + sizeof(*embedded_notes)) {
    if (esp_partition_read(partition_, payload_offset, employees->data(), sizeof(*employees)) != ESP_OK ||
        esp_partition_read(partition_, payload_offset + sizeof(*employees), embedded_notes->data(),
                           sizeof(*embedded_notes)) != ESP_OK ||
        header.payload_crc != embedded_employee_store_crc(*employees, *embedded_notes))
      return false;
    *has_embedded_notes = true;
  } else {
    return false;
  }
  *generation = header.generation;
  return true;
}

void AttendanceWeb::load_employees_() {
  uint32_t generation_zero = 0;
  uint32_t generation_one = 0;
  bool embedded_zero = false;
  bool embedded_one = false;
  bool valid_zero = load_employee_slot_(0, &employees_, &embedded_notes_, &generation_zero, &embedded_zero);
  bool valid_one = load_employee_slot_(1, &employee_scratch_, &embedded_note_scratch_, &generation_one,
                                       &embedded_one);
  bool migrate_legacy_partition = false;
  if (!valid_zero && !valid_one && employee_storage_offset_ != LEGACY_EMPLOYEE_STORAGE_OFFSET &&
      LEGACY_EMPLOYEE_STORAGE_OFFSET + EMPLOYEE_SLOT_SIZE * EMPLOYEE_SLOT_COUNT <= partition_->size) {
    valid_zero = load_employee_slot_at_(LEGACY_EMPLOYEE_STORAGE_OFFSET, &employees_, &embedded_notes_,
                                        &generation_zero, &embedded_zero);
    valid_one = load_employee_slot_at_(LEGACY_EMPLOYEE_STORAGE_OFFSET + EMPLOYEE_SLOT_SIZE, &employee_scratch_,
                                       &embedded_note_scratch_, &generation_one, &embedded_one);
    migrate_legacy_partition = valid_zero || valid_one;
  }
  if (valid_zero || valid_one) {
    const bool use_one = valid_one && (!valid_zero || static_cast<int32_t>(generation_one - generation_zero) > 0);
    if (use_one) {
      employees_ = employee_scratch_;
      embedded_notes_ = embedded_note_scratch_;
    }
    employee_generation_ = use_one ? generation_one : generation_zero;
    employee_active_slot_ = migrate_legacy_partition ? -1 : (use_one ? 1 : 0);
    for (size_t i = 0; i < MAX_EMPLOYEES; i++) {
      employees_[i].id = i + 1;
      employees_[i].code[sizeof(Employee::code) - 1] = '\0';
      employees_[i].name[sizeof(Employee::name) - 1] = '\0';
      employees_[i].uid[sizeof(Employee::uid) - 1] = '\0';
    }
    for (auto &note : embedded_notes_) {
      note.text[sizeof(note.text) - 1] = '\0';
      if (note.employee_id < 1 || note.employee_id > MAX_EMPLOYEES || note.date < 20200101U ||
          note.date > 21001231U)
        note.active = 0;
    }
    embedded_notes_available_ = use_one ? embedded_one : embedded_zero;
    if (migrate_legacy_partition) {
      if (save_employees_())
        ESP_LOGI(TAG, "Migrated employee data to the expanded attendance partition");
      else
        ESP_LOGE(TAG, "Unable to migrate employee data to the expanded attendance partition");
    } else {
      ESP_LOGI(TAG, "Loaded employee data from Flash slot %d, generation=%u%s", employee_active_slot_,
               (unsigned) employee_generation_, embedded_notes_available_ ? " (contains legacy notes)" : "");
    }
    return;
  }

  // Migrate the v1.3.7 NVS blob if it was successfully committed before this upgrade.
  employees_.fill(Employee{});
  embedded_notes_.fill(DailyNote{});
  embedded_notes_available_ = false;
  auto legacy_pref = global_preferences->make_preference<std::array<Employee, MAX_EMPLOYEES>>(0xA771E003, true);
  const bool migrated = legacy_pref.load(&employees_);
  for (size_t i=0; i<MAX_EMPLOYEES; i++)
    employees_[i].id=i+1;
  if (!save_employees_())
    ESP_LOGE(TAG, "Unable to initialize persistent employee storage");
  else if (migrated)
    ESP_LOGI(TAG, "Migrated employee data from v1.3.7 NVS storage");
}

bool AttendanceWeb::save_employees_() {
  if (partition_ == nullptr)
    return false;
  const uint8_t next_slot = employee_active_slot_ == 0 ? 1 : 0;
  const uint32_t offset = employee_slot_offset_(next_slot);
  if (esp_partition_erase_range(partition_, offset, EMPLOYEE_SLOT_SIZE) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase employee Flash slot %u", (unsigned) next_slot);
    return false;
  }
  if (esp_partition_write(partition_, offset + sizeof(EmployeeStoreHeader), employees_.data(), sizeof(employees_)) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Failed to write employee payload to Flash slot %u", (unsigned) next_slot);
    return false;
  }
  EmployeeStoreHeader header{};
  header.magic = EMPLOYEE_MAGIC;
  header.version = EMPLOYEE_STORE_VERSION;
  header.generation = employee_generation_ + 1;
  header.payload_size = sizeof(employees_);
  header.payload_crc = fnv1a(employees_.data(), sizeof(employees_));
  // Header is written last. A power loss during payload writing leaves this slot invalid,
  // so the previous generation remains available on the next boot.
  if (esp_partition_write(partition_, offset, &header, sizeof(header)) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit employee Flash slot %u", (unsigned) next_slot);
    return false;
  }
  uint32_t verify_generation = 0;
  bool verify_embedded = false;
  if (!load_employee_slot_(next_slot, &employee_scratch_, &embedded_note_scratch_, &verify_generation,
                           &verify_embedded) ||
      verify_embedded ||
      verify_generation != header.generation ||
      memcmp(employee_scratch_.data(), employees_.data(), sizeof(employees_)) != 0) {
    ESP_LOGE(TAG, "Employee Flash verification failed for slot %u", (unsigned) next_slot);
    return false;
  }
  employee_active_slot_ = next_slot;
  employee_generation_ = header.generation;
  ESP_LOGI(TAG, "Employee data committed to Flash slot %u, generation=%u", (unsigned) next_slot,
           (unsigned) employee_generation_);
  return true;
}

bool AttendanceWeb::load_note_slot_(uint8_t slot, std::array<DailyNote, MAX_DAILY_NOTES> *notes,
                                    uint32_t *generation) const {
  if (notes_partition_ == nullptr || slot >= NOTE_SLOT_COUNT || notes == nullptr || generation == nullptr)
    return false;
  const uint32_t offset = note_slot_offset_(slot);
  EmployeeStoreHeader header{};
  if (esp_partition_read(notes_partition_, offset, &header, sizeof(header)) != ESP_OK ||
      header.magic != NOTE_MAGIC || header.version != NOTE_STORE_VERSION || header.payload_size != sizeof(*notes))
    return false;
  if (esp_partition_read(notes_partition_, offset + sizeof(header), notes->data(), sizeof(*notes)) != ESP_OK ||
      header.payload_crc != fnv1a(notes->data(), sizeof(*notes)))
    return false;
  *generation = header.generation;
  return true;
}

bool AttendanceWeb::load_legacy_note_slot_(uint8_t slot, std::array<DailyNote, MAX_DAILY_NOTES> *notes,
                                           uint32_t *generation) const {
  constexpr size_t LEGACY_PAYLOAD_SIZE = sizeof(DailyNote) * LEGACY_PARTITION_MAX_DAILY_NOTES;
  if (partition_ == nullptr || slot >= NOTE_SLOT_COUNT || notes == nullptr || generation == nullptr)
    return false;
  const uint32_t offset = LEGACY_NOTE_PARTITION_OFFSET + static_cast<uint32_t>(slot) * LEGACY_NOTE_SLOT_SIZE;
  if (offset + LEGACY_NOTE_SLOT_SIZE > partition_->size)
    return false;
  EmployeeStoreHeader header{};
  if (esp_partition_read(partition_, offset, &header, sizeof(header)) != ESP_OK || header.magic != NOTE_MAGIC ||
      header.version != LEGACY_NOTE_STORE_VERSION || header.payload_size != LEGACY_PAYLOAD_SIZE)
    return false;
  notes->fill(DailyNote{});
  if (esp_partition_read(partition_, offset + sizeof(header), notes->data(), LEGACY_PAYLOAD_SIZE) != ESP_OK ||
      header.payload_crc != fnv1a(notes->data(), LEGACY_PAYLOAD_SIZE))
    return false;
  *generation = header.generation;
  return true;
}

void AttendanceWeb::load_notes_() {
  uint32_t generation_zero = 0;
  uint32_t generation_one = 0;
  const bool valid_zero = load_note_slot_(0, &notes_, &generation_zero);
  const bool valid_one = load_note_slot_(1, &note_scratch_, &generation_one);
  if (valid_zero || valid_one) {
    const bool use_one = valid_one && (!valid_zero || static_cast<int32_t>(generation_one - generation_zero) > 0);
    if (use_one)
      notes_ = note_scratch_;
    note_generation_ = use_one ? generation_one : generation_zero;
    note_active_slot_ = use_one ? 1 : 0;
    for (auto &note : notes_) {
      note.text[sizeof(note.text) - 1] = '\0';
      if (note.employee_id < 1 || note.employee_id > MAX_EMPLOYEES || note.date < 20200101U ||
          note.date > 21001231U)
        note.active = 0;
    }
    ESP_LOGI(TAG, "Loaded daily notes from Flash slot %d, generation=%u", note_active_slot_,
             (unsigned) note_generation_);
    return;
  }

  const bool legacy_valid_zero = load_legacy_note_slot_(0, &notes_, &generation_zero);
  const bool legacy_valid_one = load_legacy_note_slot_(1, &note_scratch_, &generation_one);
  if (legacy_valid_zero || legacy_valid_one) {
    const bool use_one = legacy_valid_one &&
                         (!legacy_valid_zero || static_cast<int32_t>(generation_one - generation_zero) > 0);
    if (use_one)
      notes_ = note_scratch_;
    note_generation_ = use_one ? generation_one : generation_zero;
    note_active_slot_ = -1;
    for (auto &note : notes_) {
      note.text[sizeof(note.text) - 1] = '\0';
      if (note.employee_id < 1 || note.employee_id > MAX_EMPLOYEES || note.date < 20200101U ||
          note.date > 21001231U)
        note.active = 0;
    }
    if (save_notes_())
      ESP_LOGI(TAG, "Migrated up to %u daily notes to the expanded note partition",
               (unsigned) LEGACY_PARTITION_MAX_DAILY_NOTES);
    else
      ESP_LOGE(TAG, "Unable to migrate daily notes to the expanded note partition");
    return;
  }

  notes_.fill(DailyNote{});
  size_t migrated_count = 0;
  if (embedded_notes_available_) {
    for (const auto &legacy_note : embedded_notes_) {
      if (!legacy_note.active || legacy_note.text[0] == '\0')
        continue;
      notes_[migrated_count++] = legacy_note;
    }
  }
  if (!save_notes_()) {
    ESP_LOGE(TAG, "Unable to initialize persistent daily note storage");
  } else if (migrated_count > 0) {
    ESP_LOGI(TAG, "Migrated %u daily notes from v1.3.17/v1.3.18 employee storage",
             (unsigned) migrated_count);
  }
}

bool AttendanceWeb::save_notes_() {
  if (notes_partition_ == nullptr)
    return false;
  const uint8_t next_slot = note_active_slot_ == 0 ? 1 : 0;
  const uint32_t offset = note_slot_offset_(next_slot);
  if (esp_partition_erase_range(notes_partition_, offset, NOTE_SLOT_SIZE) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase daily note Flash slot %u", (unsigned) next_slot);
    return false;
  }
  if (esp_partition_write(notes_partition_, offset + sizeof(EmployeeStoreHeader), notes_.data(), sizeof(notes_)) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Failed to write daily note payload to Flash slot %u", (unsigned) next_slot);
    return false;
  }
  EmployeeStoreHeader header{};
  header.magic = NOTE_MAGIC;
  header.version = NOTE_STORE_VERSION;
  header.generation = note_generation_ + 1;
  header.payload_size = sizeof(notes_);
  header.payload_crc = fnv1a(notes_.data(), sizeof(notes_));
  if (esp_partition_write(notes_partition_, offset, &header, sizeof(header)) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit daily note Flash slot %u", (unsigned) next_slot);
    return false;
  }
  uint32_t verify_generation = 0;
  if (!load_note_slot_(next_slot, &note_scratch_, &verify_generation) ||
      verify_generation != header.generation || memcmp(note_scratch_.data(), notes_.data(), sizeof(notes_)) != 0) {
    ESP_LOGE(TAG, "Daily note Flash verification failed for slot %u", (unsigned) next_slot);
    return false;
  }
  note_active_slot_ = next_slot;
  note_generation_ = header.generation;
  ESP_LOGI(TAG, "Daily notes committed to Flash slot %u, generation=%u", (unsigned) next_slot,
           (unsigned) note_generation_);
  return true;
}

void AttendanceWeb::scan_flash_() {
  LogRecord r{};
  uint32_t newest = 0, newest_off = 0;
  uint64_t newest_sequence = 0;
  bool any = false, has_sequence = false;
  // New records carry an insertion sequence in the existing padding bytes.
  // Event timestamps may go backwards for manual entries or clock corrections.
  for (uint32_t off = 0; off + sizeof(r) <= log_region_size_; off += sizeof(r)) {
    if (esp_partition_read(partition_, off, &r, sizeof(r)) != ESP_OK) break;
    if (r.magic != LOG_MAGIC || r.crc != fnv1a(&r, sizeof(r) - 4)) continue;
    uint32_t marker = 0;
    uint64_t sequence = 0;
    memcpy(&marker, r.padding, sizeof(marker));
    memcpy(&sequence, r.padding + sizeof(marker), sizeof(sequence));
    if (marker == 0x53455131U && sequence != 0) {
      if (!has_sequence || sequence > newest_sequence) {
        newest_sequence = sequence; newest_off = off;
        has_sequence = true; any = true;
      }
    } else if (!has_sequence && (!any || r.timestamp >= newest)) {
      newest = r.timestamp; newest_off = off; any = true;
    }
  }
  next_log_sequence_ = newest_sequence + 1;
  write_offset_ = any ? newest_off + sizeof(r) : 0;
  if (write_offset_ + sizeof(r) > log_region_size_) write_offset_ = 0;
}

void AttendanceWeb::show_enroll_color_() {
  rainbow_index_ = 0;
  last_rainbow_millis_ = millis();
  update_enroll_rainbow_();
}

void AttendanceWeb::update_enroll_rainbow_() {
  if (status_light_ == nullptr)
    return;
  // One WS2812 cannot show seven colours simultaneously, so cycle through
  // red, orange, yellow, green, cyan, blue and purple while waiting for a card.
  static constexpr float RAINBOW[7][3] = {
      {1.0f, 0.0f, 0.0f}, {1.0f, 0.45f, 0.0f}, {1.0f, 0.65f, 0.0f},
      {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 1.0f},  {0.0f, 0.0f, 1.0f},
      {0.65f, 0.0f, 1.0f},
  };
  const auto &colour = RAINBOW[rainbow_index_];
  auto call = status_light_->turn_on();
  call.set_rgb(colour[0], colour[1], colour[2]);
  call.set_brightness(STATUS_LIGHT_BRIGHTNESS);
  call.set_transition_length(180);
  call.perform();
  rainbow_index_ = (rainbow_index_ + 1U) % 7U;
}

void AttendanceWeb::restore_mode_color_() {
  if (status_light_ == nullptr)
    return;
  if (pending_action_ < 0) {
    auto call = status_light_->turn_off();
    call.set_transition_length(0);
    call.perform();
    return;
  }
  auto call = status_light_->turn_on();
  switch (pending_action_) {
    case ACTION_CLOCK_IN:
      call.set_rgb(0.0f, 1.0f, 0.0f);  // Green
      break;
    case ACTION_CLOCK_OUT:
      call.set_rgb(1.0f, 0.0f, 0.0f);  // Red
      break;
    case ACTION_OVERTIME_IN:
      // The WS2812 green channel appears brighter than red. Reducing green
      // produces a warmer, more saturated yellow instead of yellow-green.
      call.set_rgb(1.0f, 0.65f, 0.0f);  // Warm yellow
      break;
    case ACTION_OVERTIME_OUT:
      call.set_rgb(0.0f, 0.0f, 1.0f);  // Blue
      break;
    default:
      call.set_rgb(0.0f, 0.0f, 0.0f);
      break;
  }
  call.set_brightness(STATUS_LIGHT_BRIGHTNESS);
  call.set_transition_length(0);
  call.perform();
}

void AttendanceWeb::refresh_status_light() {
  if (enroll_employee_ >= 0)
    show_enroll_color_();
  else
    restore_mode_color_();
}

void AttendanceWeb::restart_network_time_sync() {
  if (!network_is_connected()) {
    ESP_LOGW(TAG, "SNTP immediate sync skipped because Wi-Fi is offline");
    return;
  }
  if (!esp_sntp_enabled()) {
    ESP_LOGW(TAG, "SNTP immediate sync skipped because the SNTP service is not initialized");
    return;
  }
  esp_sntp_restart();
  ESP_LOGI(TAG, "SNTP synchronization restarted immediately");
}

void AttendanceWeb::select_action(bool in) {
  select_action_code_(in ? ACTION_CLOCK_IN : ACTION_CLOCK_OUT);
}

void AttendanceWeb::select_overtime_action(bool in) {
  select_action_code_(in ? ACTION_OVERTIME_IN : ACTION_OVERTIME_OUT);
}

void AttendanceWeb::select_action_code_(uint8_t action) {
  if (action > ACTION_OVERTIME_OUT)
    return;
  pending_action_ = static_cast<int8_t>(action);
  last_result_ = std::string("目前模式：") + action_label(action);
  if (enroll_employee_ < 0)
    restore_mode_color_();
}
int AttendanceWeb::find_uid_(const std::string &uid) const {
  for (size_t i = 0; i < MAX_EMPLOYEES; i++) {
    if (employees_[i].active && uid == employees_[i].uid)
      return i;
  }
  return -1;
}

bool AttendanceWeb::handle_tag(const std::string &uid) {
  if (import_in_progress_) {
    last_result_ = "資料還原中，暫停刷卡";
    remember_card_result_(uid, "", last_result_);
    ESP_LOGW(TAG, "%s", last_result_.c_str());
    return false;
  }
  if (enroll_employee_>=0 && enroll_employee_<(int)MAX_EMPLOYEES) {
    const int target_employee=enroll_employee_;
    int same_employee=-1;
    int conflicting_employee=-1;
    // Scan every active employee rather than stopping at the first match. This
    // also detects duplicate UIDs left behind by firmware versions before 1.3.22.
    for(size_t index=0;index<MAX_EMPLOYEES;index++) {
      if(!employees_[index].active||uid!=employees_[index].uid)
        continue;
      if((int)index==target_employee)
        same_employee=target_employee;
      else {
        conflicting_employee=static_cast<int>(index);
        break;
      }
    }
    if(conflicting_employee>=0) {
      last_result_="此卡已綁定其他員工："+std::string(employees_[conflicting_employee].code)+"／"+
                   std::string(employees_[conflicting_employee].name);
      remember_card_result_(uid, employees_[conflicting_employee].name, "卡片已綁定其他員工");
      ESP_LOGW(TAG,"%s",last_result_.c_str());
      enroll_employee_=-1;
      restore_mode_color_();
      return false;
    }
    if(same_employee>=0) {
      last_result_="此卡已完成綁定："+std::string(employees_[target_employee].code)+"／"+
                   std::string(employees_[target_employee].name);
      remember_card_result_(uid, employees_[target_employee].name, "卡片已完成綁定");
      ESP_LOGI(TAG,"%s",last_result_.c_str());
      enroll_employee_=-1;
      restore_mode_color_();
      return true;
    }
    Employee previous = employees_[enroll_employee_];
    strlcpy(employees_[enroll_employee_].uid, uid.c_str(), sizeof(Employee::uid));
    employees_[enroll_employee_].active=1;
    if (!save_employees_()) {
      employees_[enroll_employee_] = previous;
      last_result_="卡片綁定儲存失敗，請勿斷電並查看日誌";
      remember_card_result_(uid, previous.name, last_result_);
      ESP_LOGE(TAG, "%s", last_result_.c_str());
      enroll_employee_=-1;
      restore_mode_color_();
      return false;
    }
    last_result_="卡片綁定完成："+std::string(employees_[enroll_employee_].name);
    remember_card_result_(uid, employees_[enroll_employee_].name, "卡片綁定完成");
    ESP_LOGI(TAG, "%s", last_result_.c_str());
    enroll_employee_=-1;
    restore_mode_color_();
    return true;
  }
  int i=find_uid_(uid);
  if(i<0) {
    last_result_="未登記卡片："+uid;
    remember_card_result_(uid, "", "未登記卡片");
    ESP_LOGW(TAG, "%s", last_result_.c_str());
    return false;
  }
  const ESPTime current_time = effective_now_();
  if(!current_time.is_valid()) {
    last_result_="時間尚未同步，打卡未寫入";
    remember_card_result_(uid, employees_[i].name, last_result_);
    ESP_LOGW(TAG, "%s", last_result_.c_str());
    return false;
  }
  if (pending_action_<0) {
    select_action(current_time.hour < 14);
  }
  uint32_t uh=fnv1a(uid.data(),uid.size());
  const uint8_t action = static_cast<uint8_t>(pending_action_);
  if(append_log_(employees_[i].id,action,uh,current_time.timestamp)) {
    last_result_=std::string(employees_[i].name)+" "+action_label(action)+"成功";
    remember_card_result_(uid, employees_[i].name, std::string(action_label(action)) + "成功");
    ESP_LOGI(TAG, "%s，Flash offset=%u", last_result_.c_str(), (unsigned) write_offset_);
    return true;
  }
  last_result_="資料保存失敗，本次打卡未完成";
  remember_card_result_(uid, employees_[i].name, last_result_);
  ESP_LOGE(TAG, "%s", last_result_.c_str());
  return false;
}

bool AttendanceWeb::append_log_(uint16_t eid, uint8_t action, uint32_t uid_hash, uint32_t timestamp) {
  const uint32_t sector = partition_->erase_size;
  if (write_offset_ % sector == 0 && esp_partition_erase_range(partition_, write_offset_, sector) != ESP_OK)
    return false;
  LogRecord record{};
  record.magic = LOG_MAGIC;
  record.timestamp = timestamp;
  record.employee_id = eid;
  record.action = action;
  record.uid_hash = uid_hash;
  const uint32_t sequence_marker = 0x53455131U;
  memcpy(record.padding, &sequence_marker, sizeof(sequence_marker));
  memcpy(record.padding + sizeof(sequence_marker), &next_log_sequence_, sizeof(next_log_sequence_));
  record.crc = fnv1a(&record, sizeof(record) - 4);
  if (esp_partition_write(partition_, write_offset_, &record, sizeof(record)) != ESP_OK)
    return false;
  next_log_sequence_++;
  write_offset_ += sizeof(record);
  if (write_offset_ + sizeof(record) > log_region_size_)
    write_offset_ = 0;
  return true;
}

uint32_t AttendanceWeb::cutoff_timestamp_() const {
  const ESPTime current_time = effective_now_();
  if (!current_time.is_valid())
    return 0;

  time_t timestamp = current_time.timestamp;
  struct tm time_info {};
  localtime_r(&timestamp, &time_info);
  time_info.tm_mon -= 2;
  time_info.tm_hour = 0;
  time_info.tm_min = 0;
  time_info.tm_sec = 0;
  return static_cast<uint32_t>(mktime(&time_info));
}

bool AttendanceWeb::canHandle(AsyncWebServerRequest *r) const {
  std::array<char, AsyncWebServerRequest::URL_BUF_SIZE> url_buffer{};
  const std::string url = r->url_to(url_buffer).str();
  return url.rfind("/attendance", 0) == 0;
}
void AttendanceWeb::send_json_(AsyncWebServerRequest *r,int code,const std::string &body) { auto *res=r->beginResponse(code,"application/json; charset=utf-8",body.c_str()); res->addHeader("Cache-Control","no-store"); r->send(res); }

void AttendanceWeb::handleRequest(AsyncWebServerRequest *r) {
  std::array<char, AsyncWebServerRequest::URL_BUF_SIZE> url_buffer{};
  const std::string u = r->url_to(url_buffer).str();
  if(u=="/attendance"||u=="/attendance/") serve_page_(r);
  else if(u=="/attendance/api/status") serve_status_(r);
  else if(u=="/attendance/api/employees") serve_employees_(r);
  else if(u=="/attendance/api/notes") serve_notes_(r);
  else if(u=="/attendance/api/note") mutate_note_(r);
  else if(u=="/attendance/api/employee") mutate_employee_(r);
  else if(u=="/attendance/api/mode") set_attendance_mode_(r);
  else if(u=="/attendance/api/mode-schedule") set_mode_schedule_(r);
  else if(u=="/attendance/api/manual-punch") manual_punch_(r);
  else if(u=="/attendance/api/clear-logs") clear_logs_(r);
  else if(u=="/attendance/api/manual-time") set_manual_time_(r);
  else if(u=="/attendance/api/automatic-time") restore_automatic_time_(r);
  else if(u=="/attendance/api/admin-password") change_admin_password_(r);
  else if(u=="/attendance/api/import-begin") begin_backup_import_(r);
  else if(u=="/attendance/api/import-employee") import_employee_(r);
  else if(u=="/attendance/api/import-note") import_note_(r);
  else if(u=="/attendance/api/import-logs") import_log_batch_(r);
  else if(u=="/attendance/api/import-finish") finish_backup_import_(r);
  else if(u=="/attendance/api/import-abort") abort_backup_import_(r);
  else if(u=="/attendance/api/delete") delete_employee_(r);
  else if(u=="/attendance/api/enroll") { int slot=atoi(param(r,"slot").c_str()); if(slot<1||slot>(int)MAX_EMPLOYEES||!employees_[slot-1].active) { send_json_(r,400,"{\"error\":\"找不到這位員工，請重新整理頁面\"}"); } else { enroll_employee_=slot-1; last_result_="請將要登記的新卡靠近感應區（七彩循環燈）"; show_enroll_color_(); send_json_(r,200,"{\"ok\":true}"); } }
  else if(u=="/attendance/api/export-page") serve_export_data_(r);
  else if(u=="/attendance/api/logs") serve_logs_(r,false);
  else if(u=="/attendance/export.xlsx") serve_export_launcher_(r);
  else if(u=="/attendance/export.csv") serve_logs_(r,true);
  else r->send(404,"text/plain","Not found");
}

void AttendanceWeb::serve_status_(AsyncWebServerRequest *r) {
  const std::string mode = attendance_mode_text();
  const ESPTime effective_time = effective_now_();
  const bool time_valid = effective_time.is_valid();
  const bool hardware_rtc_connected = hardware_rtc_is_connected();
  const std::string time_source = time_source_text();
  char current_time[24] = "尚未同步";
  if (time_valid) {
    time_t timestamp = effective_time.timestamp;
    struct tm time_info {};
    localtime_r(&timestamp, &time_info);
    strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", &time_info);
  }

  const bool network_connected = network_is_connected();
  std::array<char, wifi::SSID_BUFFER_SIZE> ssid_buffer{};
  const char *wifi_ssid = network_connected ? wifi_->wifi_ssid_to(ssid_buffer) : "";
  char ip_address[network::IP_ADDRESS_BUFFER_SIZE]{};
  if (network_connected) {
    const auto addresses = wifi_->get_ip_addresses();
    for (const auto &address : addresses) {
      if (address.is_set() && address.is_ip4()) {
        address.str_to(ip_address);
        break;
      }
    }
    if (ip_address[0] == '\0') {
      for (const auto &address : addresses) {
        if (address.is_set()) {
          address.str_to(ip_address);
          break;
        }
      }
    }
  }

  std::ostringstream o;
  o << "{\"result\":\"" << esc(last_result_.c_str()) << "\",\"mode\":\"" << mode
    << "\",\"enrolling\":" << (enroll_employee_ >= 0 ? "true" : "false")
    << ",\"time_valid\":" << (time_valid ? "true" : "false")
    << ",\"manual_time_override\":" << (manual_time_override_ ? "true" : "false")
    << ",\"time_source\":\"" << time_source << "\""
    << ",\"hardware_rtc_connected\":" << (hardware_rtc_connected ? "true" : "false")
    << ",\"current_time\":\"" << current_time << "\""
    << ",\"network_connected\":" << (network_connected ? "true" : "false")
    << ",\"wifi_ssid\":\"" << esc(wifi_ssid) << "\""
    << ",\"ip_address\":\"" << esc(ip_address) << "\""
    << ",\"mode_schedule_enabled\":" << (mode_schedule_enabled_ ? "true" : "false")
    << ",\"clock_in_schedule_enabled\":"
    << ((mode_schedule_action_mask_ & (1U << ACTION_CLOCK_IN)) != 0 ? "true" : "false")
    << ",\"clock_out_schedule_enabled\":"
    << ((mode_schedule_action_mask_ & (1U << ACTION_CLOCK_OUT)) != 0 ? "true" : "false")
    << ",\"overtime_in_schedule_enabled\":"
    << ((mode_schedule_action_mask_ & (1U << ACTION_OVERTIME_IN)) != 0 ? "true" : "false")
    << ",\"overtime_out_schedule_enabled\":"
    << ((mode_schedule_action_mask_ & (1U << ACTION_OVERTIME_OUT)) != 0 ? "true" : "false")
    << ",\"clock_in_time\":\"" << format_time_value(clock_in_minute_) << "\""
    << ",\"clock_out_time\":\"" << format_time_value(clock_out_minute_) << "\""
    << ",\"overtime_in_time\":\"" << format_time_value(overtime_in_minute_) << "\""
    << ",\"overtime_out_time\":\"" << format_time_value(overtime_out_minute_) << "\""
    << ",\"free_heap\":" << esp_get_free_heap_size()
    << ",\"log_capacity\":" << (log_region_size_ / sizeof(LogRecord))
    << ",\"note_capacity\":" << MAX_DAILY_NOTES
    << ",\"log_high_water_percent\":" << static_cast<unsigned>(LOG_HIGH_WATER_PERCENT) << "}";
  send_json_(r, 200, o.str());
}
void AttendanceWeb::serve_employees_(AsyncWebServerRequest *r) {
  std::ostringstream o;
  o << "[";
  bool first = true;
  for (auto &e : employees_) {
    if (!e.name[0] || !e.active)
      continue;
    if (!first)
      o << ",";
    first = false;
    o << "{\"slot\":" << e.id << ",\"id\":\"" << esc(e.code) << "\",\"name\":\"" << esc(e.name)
      << "\",\"uid\":\"" << esc(e.uid) << "\",\"active\":true}";
  }
  o << "]";
  send_json_(r, 200, o.str());
}

void AttendanceWeb::serve_notes_(AsyncWebServerRequest *request) {
  const bool include_all = param(request, "all") == "1";
  const bool paged = param(request, "paged") == "1";
  const uint32_t requested_offset = paged ? strtoul(param(request, "offset").c_str(), nullptr, 10) : 0;
  uint32_t requested_limit = paged ? strtoul(param(request, "limit").c_str(), nullptr, 10) : MAX_DAILY_NOTES;
  if (requested_limit == 0 || requested_limit > 100)
    requested_limit = 50;
  uint32_t cutoff_date = 0;
  const uint32_t cutoff_timestamp = cutoff_timestamp_();
  if (cutoff_timestamp != 0) {
    time_t timestamp = cutoff_timestamp;
    struct tm time_info {};
    localtime_r(&timestamp, &time_info);
    cutoff_date = static_cast<uint32_t>((time_info.tm_year + 1900) * 10000 +
                                        (time_info.tm_mon + 1) * 100 + time_info.tm_mday);
  }
  std::ostringstream output;
  output << (paged ? "{\"notes\":[" : "[");
  bool first = true;
  uint32_t matched = 0;
  uint32_t added = 0;
  bool has_more = false;
  for (const auto &note : notes_) {
    if (!note.active || note.text[0] == '\0' || (!include_all && cutoff_date != 0 && note.date < cutoff_date))
      continue;
    if (matched++ < requested_offset)
      continue;
    if (paged && added >= requested_limit) {
      has_more = true;
      break;
    }
    const Employee *employee = nullptr;
    for (const auto &candidate : employees_) {
      if (candidate.id == note.employee_id) {
        employee = &candidate;
        break;
      }
    }
    // Keep extra room because printf field width is a minimum, not a maximum.
    // Valid stored dates still render as the usual 10-character YYYY-MM-DD.
    char date[16];
    format_date_value(note.date, date, sizeof(date));
    if (!first)
      output << ",";
    first = false;
    const bool employee_active = employee != nullptr && employee->active && employee->name[0] != '\0';
    std::string deleted_employee_label;
    if (!employee_active)
      deleted_employee_label = "已刪除員工（槽位 " + std::to_string(note.employee_id) + "）";
    output << "{\"date\":\"" << date << "\",\"slot\":" << note.employee_id << ",\"id\":\""
           << esc(employee_active ? employee->code : "") << "\",\"name\":\""
           << esc(employee_active ? employee->name : deleted_employee_label.c_str()) << "\",\"note\":\""
           << esc(note.text) << "\"}";
    added++;
  }
  output << "]";
  if (paged)
    output << ",\"next\":" << (requested_offset + added) << ",\"done\":"
           << (has_more ? "false" : "true") << "}";
  send_json_(request, 200, output.str());
}

void AttendanceWeb::mutate_note_(AsyncWebServerRequest *request) {
  if (import_in_progress_) {
    send_json_(request, 409, "{\"error\":\"資料還原中，暫時無法修改備註\"}");
    return;
  }
  if (notes_partition_ == nullptr) {
    send_json_(request, 503, "{\"error\":\"備註儲存空間尚未準備完成，請聯絡設備維護人員\"}");
    return;
  }
  const int slot = atoi(param(request, "slot").c_str());
  const std::string date_text = param(request, "date");
  const std::string note_text = param(request, "note");
  uint32_t date = 0;
  if (slot < 1 || slot > static_cast<int>(MAX_EMPLOYEES)) {
    send_json_(request, 400, "{\"error\":\"員工槽位不正確\"}");
    return;
  }
  if (!parse_date_value(date_text, &date)) {
    send_json_(request, 400, "{\"error\":\"備註日期格式不正確\"}");
    return;
  }
  if (note_text == "undefined" || note_text.size() >= sizeof(notes_[0].text) ||
      utf8_character_count(note_text) > 15) {
    send_json_(request, 400, "{\"error\":\"備註最多 15 個字\"}");
    return;
  }

  int note_index = -1;
  for (size_t index = 0; index < notes_.size(); index++) {
    if (notes_[index].active && notes_[index].employee_id == slot && notes_[index].date == date) {
      note_index = static_cast<int>(index);
      break;
    }
  }
  if (note_text.empty()) {
    if (note_index < 0) {
      send_json_(request, 200, "{\"ok\":true}");
      return;
    }
    const DailyNote previous = notes_[note_index];
    notes_[note_index] = DailyNote{};
    if (!save_notes_()) {
      notes_[note_index] = previous;
      send_json_(request, 500, "{\"error\":\"備註刪除失敗，請稍後再試\"}");
      return;
    }
    const std::string employee_label = employees_[slot - 1].active && employees_[slot - 1].name[0] != '\0'
                                           ? employees_[slot - 1].name
                                           : "已刪除員工（槽位 " + std::to_string(slot) + "）";
    last_result_ = "每日備註已刪除：" + date_text + " " + employee_label;
    send_json_(request, 200, "{\"ok\":true}");
    return;
  }

  // An orphaned note must remain deletable after its employee is removed, but
  // new notes and edits still require an active employee.
  if (!employees_[slot - 1].active) {
    send_json_(request, 400, "{\"error\":\"員工已刪除，只能刪除原有備註\"}");
    return;
  }

  if (note_index < 0) {
    uint32_t cutoff_date = 0;
    const uint32_t cutoff_timestamp = cutoff_timestamp_();
    if (cutoff_timestamp != 0) {
      time_t timestamp = cutoff_timestamp;
      struct tm time_info {};
      localtime_r(&timestamp, &time_info);
      cutoff_date = static_cast<uint32_t>((time_info.tm_year + 1900) * 10000 +
                                          (time_info.tm_mon + 1) * 100 + time_info.tm_mday);
    }
    for (size_t index = 0; index < notes_.size(); index++) {
      if (!notes_[index].active || (cutoff_date != 0 && notes_[index].date < cutoff_date)) {
        note_index = static_cast<int>(index);
        break;
      }
    }
  }
  if (note_index < 0) {
    send_json_(request, 409, "{\"error\":\"近期備註已達 500 筆，請先刪除不需要的舊備註\"}");
    return;
  }

  const DailyNote previous = notes_[note_index];
  notes_[note_index] = DailyNote{};
  notes_[note_index].date = date;
  notes_[note_index].employee_id = static_cast<uint16_t>(slot);
  notes_[note_index].active = 1;
  strlcpy(notes_[note_index].text, note_text.c_str(), sizeof(notes_[note_index].text));
  if (!save_notes_()) {
    notes_[note_index] = previous;
    send_json_(request, 500, "{\"error\":\"備註保存失敗，請稍後再試\"}");
    return;
  }
  last_result_ = "每日備註已儲存：" + date_text + " " + employees_[slot - 1].name;
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::mutate_employee_(AsyncWebServerRequest *r) {
  if (import_in_progress_) {
    send_json_(r,409,"{\"error\":\"資料還原中，暫時無法修改員工\"}");
    return;
  }
  std::string password=param(r,"password");
  const bool password_valid=!admin_password_.empty()&&secure_equals(password,admin_password_);
  std::fill(password.begin(),password.end(),'\0');
  if(!password_valid) {
    ESP_LOGW(TAG,"Rejected employee add/update request: invalid password");
    send_json_(r,403,"{\"error\":\"管理密碼錯誤，員工資料未變更\"}");
    return;
  }
  int slot=atoi(param(r,"slot").c_str()); std::string code=trim_ascii_space(param(r,"code")), name=param(r,"name"), active=param(r,"active");
  if(code.empty()||code.size()>16||name.empty()||name.size()>40||employee_code_equal(code,"undefined")||name=="undefined") { send_json_(r,400,"{\"error\":\"請輸入有效的員工編號與姓名\"}"); return; }
  if(slot<1||slot>(int)MAX_EMPLOYEES) { slot=0; for(size_t i=0;i<MAX_EMPLOYEES;i++) if(employee_code_equal(code,employees_[i].code)) { slot=i+1; break; } if(slot==0) for(size_t i=0;i<MAX_EMPLOYEES;i++) if(!employees_[i].name[0]) { slot=i+1; break; } }
  if(slot<1) { send_json_(r,409,"{\"error\":\"員工人數已達 100 人上限\"}"); return; }
  for(size_t i=0;i<MAX_EMPLOYEES;i++) if((int)i!=slot-1 && employee_code_equal(code,employees_[i].code)) { send_json_(r,409,"{\"error\":\"員工工號已存在\"}"); return; }
  auto &e=employees_[slot-1];
  Employee previous=e;
  e.id=slot;
  strlcpy(e.code,code.c_str(),sizeof(e.code));
  strlcpy(e.name,name.c_str(),sizeof(e.name));
  e.active=active!="0";
  if(!save_employees_()) {
    e=previous;
    send_json_(r,500,"{\"error\":\"員工資料保存失敗，請稍後再試\"}");
    return;
  }
  last_result_="員工資料已儲存："+name;
  send_json_(r,200,"{\"ok\":true}");
}

void AttendanceWeb::delete_employee_(AsyncWebServerRequest *r) {
  if (import_in_progress_) {
    send_json_(r,409,"{\"error\":\"資料還原中，暫時無法刪除員工\"}");
    return;
  }
  std::string password=param(r,"password");
  const bool password_valid=!admin_password_.empty()&&secure_equals(password,admin_password_);
  std::fill(password.begin(),password.end(),'\0');
  if(!password_valid) {
    ESP_LOGW(TAG,"Rejected employee delete request: invalid password");
    send_json_(r,403,"{\"error\":\"管理密碼錯誤，員工未刪除\"}");
    return;
  }
  const int slot=atoi(param(r,"slot").c_str());
  if(slot<1||slot>(int)MAX_EMPLOYEES||!employees_[slot-1].active) {
    send_json_(r,400,"{\"error\":\"員工不存在或已刪除\"}");
    return;
  }
  const Employee previous=employees_[slot-1];
  employees_[slot-1]=Employee{};
  employees_[slot-1].id=slot;
  if(!save_employees_()) {
    employees_[slot-1]=previous;
    send_json_(r,500,"{\"error\":\"員工刪除失敗，請稍後再試\"}");
    return;
  }
  last_result_="員工已刪除，原卡片已失效";
  send_json_(r,200,"{\"ok\":true}");
}

void AttendanceWeb::clear_logs_(AsyncWebServerRequest *request) {
  if (import_in_progress_) {
    send_json_(request, 409, "{\"error\":\"資料還原中，暫時無法清除刷卡紀錄\"}");
    return;
  }
  std::string password = param(request, "password");
  const bool password_valid = !admin_password_.empty() && secure_equals(password, admin_password_);
  std::fill(password.begin(), password.end(), '\0');
  if (!password_valid) {
    ESP_LOGW(TAG, "Rejected request to erase attendance logs: invalid password");
    send_json_(request, 403, "{\"error\":\"管理密碼錯誤，刷卡紀錄未刪除\"}");
    return;
  }
  if (partition_ == nullptr) {
    send_json_(request, 503, "{\"error\":\"找不到刷卡紀錄分割區\"}");
    return;
  }
  if (log_region_size_ == 0 || log_region_size_ % partition_->erase_size != 0) {
    ESP_LOGE(TAG, "Invalid attendance partition geometry");
    send_json_(request, 500, "{\"error\":\"刷卡紀錄分割區格式錯誤\"}");
    return;
  }
  const esp_err_t result = esp_partition_erase_range(partition_, 0, log_region_size_);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase attendance logs: %d", static_cast<int>(result));
    send_json_(request, 500, "{\"error\":\"刪除失敗，原有紀錄可能仍然存在\"}");
    return;
  }
  write_offset_ = 0;
  last_result_ = "所有刷卡紀錄已刪除";
  ESP_LOGW(TAG, "All attendance logs were erased from flash");
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::set_attendance_mode_(AsyncWebServerRequest *request) {
  const std::string mode = param(request, "mode");
  if (mode == "in") {
    select_action(true);
  } else if (mode == "out") {
    select_action(false);
  } else if (mode == "overtime_in") {
    select_overtime_action(true);
  } else if (mode == "overtime_out") {
    select_overtime_action(false);
  } else {
    send_json_(request, 400, "{\"error\":\"上下班模式參數不正確\"}");
    return;
  }
  ESP_LOGI(TAG, "Attendance mode remotely changed to %s", mode.c_str());
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::manual_punch_(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST) {
    send_json_(request, 405, "{\"error\":\"請使用 POST 提交補打卡\"}");
    return;
  }
  std::string password = param(request, "password");
  const bool authorized = !admin_password_.empty() && secure_equals(password, admin_password_);
  std::fill(password.begin(), password.end(), '\0');
  if (!authorized) {
    send_json_(request, 403, "{\"error\":\"管理員密碼錯誤，補打卡未保存\"}");
    return;
  }
  if (import_in_progress_) {
    send_json_(request, 409, "{\"error\":\"資料還原中，請完成後再補打卡\"}");
    return;
  }
  const ESPTime now = effective_now_();
  if (!now.is_valid() || partition_ == nullptr || log_region_size_ == 0) {
    send_json_(request, 409, "{\"error\":\"請先確認裝置時間與資料儲存正常\"}");
    return;
  }
  uint32_t slot = 0, action = 0, timestamp = 0;
  if (!parse_uint32_strict(param(request, "slot"), &slot) || slot < 1 || slot > MAX_EMPLOYEES ||
      !employees_[slot - 1].active ||
      !parse_uint32_strict(param(request, "action"), &action) || action > ACTION_OVERTIME_OUT ||
      !parse_uint32_strict(param(request, "timestamp"), &timestamp)) {
    send_json_(request, 400, "{\"error\":\"員工、打卡類型或時間格式不正確\"}");
    return;
  }
  if (timestamp < cutoff_timestamp_() || timestamp > static_cast<uint32_t>(now.timestamp)) {
    send_json_(request, 400, "{\"error\":\"只能補登最近兩個日曆月內且不晚於目前裝置時間的紀錄\"}");
    return;
  }
  // Do not fabricate a physical card UID for an administrator entry.
  if (!append_log_(employees_[slot - 1].id, static_cast<uint8_t>(action), 0, timestamp)) {
    send_json_(request, 500, "{\"error\":\"補打卡寫入失敗，請確認裝置狀態\"}");
    return;
  }
  last_result_ = std::string(employees_[slot - 1].name) + " " + action_label(action) + "補打卡已保存";
  ESP_LOGI(TAG, "Administrator added attendance: employee=%u, action=%u, timestamp=%u",
           (unsigned) slot, (unsigned) action, (unsigned) timestamp);
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::set_mode_schedule_(AsyncWebServerRequest *request) {
  std::string password = param(request, "password");
  const bool password_valid = !admin_password_.empty() && secure_equals(password, admin_password_);
  std::fill(password.begin(), password.end(), '\0');
  if (!password_valid) {
    send_json_(request, 403, "{\"error\":\"管理密碼錯誤，自動切換設定未變更\"}");
    return;
  }

  const std::string enabled_value = param(request, "enabled");
  const std::string clock_in_enabled_value = param(request, "clock_in_enabled");
  const std::string clock_out_enabled_value = param(request, "clock_out_enabled");
  const std::string overtime_in_enabled_value = param(request, "overtime_in_enabled");
  const std::string overtime_out_enabled_value = param(request, "overtime_out_enabled");
  const auto valid_boolean_value = [](const std::string &value) { return value == "0" || value == "1"; };
  uint16_t requested_clock_in = 0;
  uint16_t requested_clock_out = 0;
  uint16_t requested_overtime_in = 0;
  uint16_t requested_overtime_out = 0;
  if (!valid_boolean_value(enabled_value) || !valid_boolean_value(clock_in_enabled_value) ||
      !valid_boolean_value(clock_out_enabled_value) || !valid_boolean_value(overtime_in_enabled_value) ||
      !valid_boolean_value(overtime_out_enabled_value) ||
      !parse_time_value(param(request, "clock_in_time"), &requested_clock_in) ||
      !parse_time_value(param(request, "clock_out_time"), &requested_clock_out) ||
      !parse_time_value(param(request, "overtime_in_time"), &requested_overtime_in) ||
      !parse_time_value(param(request, "overtime_out_time"), &requested_overtime_out)) {
    send_json_(request, 400, "{\"error\":\"自動切換時間格式不正確\"}");
    return;
  }

  uint8_t requested_action_mask = 0;
  if (clock_in_enabled_value == "1")
    requested_action_mask |= 1U << ACTION_CLOCK_IN;
  if (clock_out_enabled_value == "1")
    requested_action_mask |= 1U << ACTION_CLOCK_OUT;
  if (overtime_in_enabled_value == "1")
    requested_action_mask |= 1U << ACTION_OVERTIME_IN;
  if (overtime_out_enabled_value == "1")
    requested_action_mask |= 1U << ACTION_OVERTIME_OUT;
  const bool requested_enabled = enabled_value == "1";
  if (requested_enabled && requested_action_mask == 0) {
    send_json_(request, 400, "{\"error\":\"啟用自動切換時，請至少選擇一種模式\"}");
    return;
  }
  if (!valid_mode_schedule_values(requested_action_mask, requested_clock_in, requested_clock_out,
                                  requested_overtime_in, requested_overtime_out)) {
    send_json_(request, 400, "{\"error\":\"已啟用的模式不可設定相同切換時間\"}");
    return;
  }

  const bool previous_enabled = mode_schedule_enabled_;
  const uint8_t previous_action_mask = mode_schedule_action_mask_;
  const uint16_t previous_clock_in = clock_in_minute_;
  const uint16_t previous_clock_out = clock_out_minute_;
  const uint16_t previous_overtime_in = overtime_in_minute_;
  const uint16_t previous_overtime_out = overtime_out_minute_;
  mode_schedule_enabled_ = requested_enabled;
  mode_schedule_action_mask_ = requested_action_mask;
  clock_in_minute_ = requested_clock_in;
  clock_out_minute_ = requested_clock_out;
  overtime_in_minute_ = requested_overtime_in;
  overtime_out_minute_ = requested_overtime_out;
  if (!save_mode_schedule_()) {
    mode_schedule_enabled_ = previous_enabled;
    mode_schedule_action_mask_ = previous_action_mask;
    clock_in_minute_ = previous_clock_in;
    clock_out_minute_ = previous_clock_out;
    overtime_in_minute_ = previous_overtime_in;
    overtime_out_minute_ = previous_overtime_out;
    ESP_LOGE(TAG, "Failed to save the automatic mode schedule");
    send_json_(request, 500, "{\"error\":\"自動切換設定無法保存到 Flash\"}");
    return;
  }

  last_schedule_bucket_ = 0;
  if (mode_schedule_enabled_)
    update_scheduled_mode_();
  last_result_ = mode_schedule_enabled_
                     ? "四種打卡模式的自動定時切換設定已保存"
                     : "自動定時切換已關閉，模式只會由按鍵、網頁或 Home Assistant 變更";
  ESP_LOGI(TAG,
           "Automatic mode schedule saved: %s, mask=0x%02X, clock-in=%s, clock-out=%s, "
           "overtime-in=%s, overtime-out=%s",
           mode_schedule_enabled_ ? "enabled" : "disabled", (unsigned) mode_schedule_action_mask_,
           format_time_value(clock_in_minute_).c_str(), format_time_value(clock_out_minute_).c_str(),
           format_time_value(overtime_in_minute_).c_str(), format_time_value(overtime_out_minute_).c_str());
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::set_manual_time_(AsyncWebServerRequest *request) {
  std::string password = param(request, "password");
  const bool password_valid = !admin_password_.empty() && secure_equals(password, admin_password_);
  std::fill(password.begin(), password.end(), '\0');
  if (!password_valid) {
    send_json_(request, 403, "{\"error\":\"管理密碼錯誤，時間未變更\"}");
    return;
  }
  const std::string timestamp_value = param(request, "timestamp");
  char *end = nullptr;
  const int64_t requested_timestamp = strtoll(timestamp_value.c_str(), &end, 10);
  const ESPTime requested_time = ESPTime::from_epoch_local(static_cast<time_t>(requested_timestamp));
  if (timestamp_value.empty() || end == timestamp_value.c_str() || *end != '\0' || requested_timestamp < 1704067200LL ||
      requested_timestamp > 4102444799LL || !requested_time.is_valid()) {
    send_json_(request, 400, "{\"error\":\"手動日期時間格式不正確\"}");
    return;
  }
  struct timeval system_time {};
  system_time.tv_sec = static_cast<time_t>(requested_timestamp);
  if (settimeofday(&system_time, nullptr) != 0) {
    ESP_LOGE(TAG, "Failed to set the ESP32 system clock from the management page");
    send_json_(request, 500, "{\"error\":\"ESP32 系統時間設定失敗\"}");
    return;
  }
  manual_time_base_ = static_cast<time_t>(requested_timestamp);
  manual_time_millis_ = millis();
  manual_time_override_ = true;
  network_time_synced_ = false;
  last_sync_source_ = "管理員手動校時（優先）";
  last_sync_timestamp_ = manual_time_base_;
  const bool hardware_rtc_connected = hardware_rtc_ != nullptr && !hardware_rtc_->is_failed();
  if (hardware_rtc_connected) {
    hardware_rtc_->write_time();
    ESP_LOGI(TAG, "Manual time written to DS3231");
  } else {
    ESP_LOGW(TAG, "Manual time active, but DS3231 is unavailable and could not be updated");
  }
  last_schedule_bucket_ = 0;
  update_scheduled_mode_();
  last_result_ = hardware_rtc_connected ? "手動時間已啟用並保存到備用時鐘"
                                        : "手動時間已啟用，但備用時鐘未連線";
  ESP_LOGW(TAG, "Manual attendance time override enabled");
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::restore_automatic_time_(AsyncWebServerRequest *request) {
  std::string password = param(request, "password");
  const bool password_valid = !admin_password_.empty() && secure_equals(password, admin_password_);
  std::fill(password.begin(), password.end(), '\0');
  if (!password_valid) {
    send_json_(request, 403, "{\"error\":\"管理密碼錯誤，校時模式未變更\"}");
    return;
  }
  manual_time_override_ = false;
  last_schedule_bucket_ = 0;
  update_scheduled_mode_();
  last_result_ = "已恢復網路自動校時；完成更新前會沿用目前時間";
  ESP_LOGI(TAG, "Automatic network time restored");
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::change_admin_password_(AsyncWebServerRequest *request) {
#ifndef USE_WEBSERVER_AUTH
  send_json_(request, 503, "{\"error\":\"YAML 尚未啟用 web_server.auth，無法同步修改登入密碼\"}");
  return;
#else
  std::string current_password = param(request, "current_password");
  std::string new_password = param(request, "new_password");
  std::string confirm_password = param(request, "confirm_password");
  const bool current_password_valid =
      !admin_password_.empty() && secure_equals(current_password, admin_password_);
  if (!current_password_valid) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    std::fill(confirm_password.begin(), confirm_password.end(), '\0');
    ESP_LOGW(TAG, "Rejected management password change: current password is invalid");
    send_json_(request, 403, "{\"error\":\"目前管理密碼錯誤，密碼未變更\"}");
    return;
  }
  if (!secure_equals(new_password, confirm_password)) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    std::fill(confirm_password.begin(), confirm_password.end(), '\0');
    send_json_(request, 400, "{\"error\":\"兩次輸入的新密碼不一致\"}");
    return;
  }
  if (!valid_new_admin_password(new_password)) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    std::fill(confirm_password.begin(), confirm_password.end(), '\0');
    send_json_(request, 400,
               "{\"error\":\"新密碼必須為 8～64 個半形英文字母、數字或符號，且不可包含空白\"}");
    return;
  }
  if (secure_equals(new_password, admin_password_)) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    std::fill(confirm_password.begin(), confirm_password.end(), '\0');
    send_json_(request, 409, "{\"error\":\"新密碼與目前密碼相同，無需變更\"}");
    return;
  }
  if (!save_admin_password_(new_password)) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    std::fill(confirm_password.begin(), confirm_password.end(), '\0');
    ESP_LOGE(TAG, "Unable to save the new management password to preferences");
    send_json_(request, 500, "{\"error\":\"新密碼未能寫入 Flash，原密碼仍然有效\"}");
    return;
  }

  admin_password_ = new_password;
  runtime_admin_password_loaded_ = true;
  apply_web_auth_password_();
  last_result_ = "管理員密碼已修改；請使用新密碼重新登入";
  std::fill(current_password.begin(), current_password.end(), '\0');
  std::fill(new_password.begin(), new_password.end(), '\0');
  std::fill(confirm_password.begin(), confirm_password.end(), '\0');
  ESP_LOGW(TAG, "Management login and operation password changed together");
  send_json_(request, 200, "{\"ok\":true,\"reauthenticate\":true}");
#endif
}

bool AttendanceWeb::check_import_token_(AsyncWebServerRequest *request) const {
  uint32_t token = 0;
  return import_in_progress_ && parse_uint32_strict(param(request, "token"), &token) && token != 0 &&
         token == import_token_;
}

void AttendanceWeb::begin_backup_import_(AsyncWebServerRequest *request) {
  std::string password = param(request, "password");
  const bool password_valid = !admin_password_.empty() && secure_equals(password, admin_password_);
  std::fill(password.begin(), password.end(), '\0');
  if (!password_valid) {
    ESP_LOGW(TAG, "Rejected backup import: invalid management password");
    send_json_(request, 403, "{\"error\":\"管理密碼錯誤，現有資料未變更\"}");
    return;
  }
  if (partition_ == nullptr || notes_partition_ == nullptr) {
    send_json_(request, 503, "{\"error\":\"資料分割區不完整，無法開始還原\"}");
    return;
  }

  import_in_progress_ = true;
  import_token_ = 0;
  enroll_employee_ = -1;
  employees_.fill(Employee{});
  for (size_t index = 0; index < employees_.size(); index++)
    employees_[index].id = static_cast<uint16_t>(index + 1);
  notes_.fill(DailyNote{});
  if (!save_employees_() || !save_notes_()) {
    import_in_progress_ = false;
    send_json_(request, 500, "{\"error\":\"無法初始化還原區，請查看裝置日誌後重新匯入\"}");
    return;
  }
  if (esp_partition_erase_range(partition_, 0, log_region_size_) != ESP_OK) {
    import_in_progress_ = false;
    send_json_(request, 500, "{\"error\":\"無法清除原刷卡紀錄區，請查看裝置日誌\"}");
    return;
  }

  write_offset_ = 0;
  imported_employee_count_ = 0;
  imported_note_count_ = 0;
  imported_log_count_ = 0;
  last_import_timestamp_ = 0;
  import_token_ = esp_random();
  if (import_token_ == 0)
    import_token_ = 1;
  last_import_millis_ = millis();
  last_result_ = "已清除現有資料，正在從備份還原";
  ESP_LOGW(TAG, "Backup import started; existing attendance data were cleared");
  send_json_(request, 200, "{\"ok\":true,\"token\":" + std::to_string(import_token_) + "}");
}

void AttendanceWeb::import_employee_(AsyncWebServerRequest *request) {
  if (!check_import_token_(request)) {
    send_json_(request, 403, "{\"error\":\"資料還原工作階段無效或已逾時\"}");
    return;
  }
  const int slot = atoi(param(request, "slot").c_str());
  const std::string code = trim_ascii_space(param(request, "code"));
  const std::string name = param(request, "name");
  const std::string uid = param(request, "uid");
  if (slot < 1 || slot > static_cast<int>(MAX_EMPLOYEES) || code.empty() || code.size() > 16 ||
      name.empty() || name.size() > 40 || uid.size() > 24 || employee_code_equal(code, "undefined") ||
      name == "undefined") {
    send_json_(request, 400, "{\"error\":\"備份中的員工資料格式不正確\"}");
    return;
  }
  for (size_t index = 0; index < employees_.size(); index++) {
    if (static_cast<int>(index) == slot - 1 || !employees_[index].active)
      continue;
    if (employee_code_equal(code, employees_[index].code)) {
      send_json_(request, 409, "{\"error\":\"備份內含重複工號\"}");
      return;
    }
    if (!uid.empty() && uid == employees_[index].uid) {
      send_json_(request, 409, "{\"error\":\"備份內含重複卡號\"}");
      return;
    }
  }
  Employee &employee = employees_[slot - 1];
  const bool was_active = employee.active;
  employee = Employee{};
  employee.id = static_cast<uint16_t>(slot);
  employee.active = 1;
  strlcpy(employee.code, code.c_str(), sizeof(employee.code));
  strlcpy(employee.name, name.c_str(), sizeof(employee.name));
  strlcpy(employee.uid, uid.c_str(), sizeof(employee.uid));
  if (!was_active)
    imported_employee_count_++;
  last_import_millis_ = millis();
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::import_note_(AsyncWebServerRequest *request) {
  if (!check_import_token_(request)) {
    send_json_(request, 403, "{\"error\":\"資料還原工作階段無效或已逾時\"}");
    return;
  }
  const int slot = atoi(param(request, "slot").c_str());
  const std::string date_text = param(request, "date");
  const std::string note_text = param(request, "note");
  uint32_t date = 0;
  if (slot < 1 || slot > static_cast<int>(MAX_EMPLOYEES) || !parse_date_value(date_text, &date) ||
      note_text.empty() || note_text.size() >= sizeof(notes_[0].text) || utf8_character_count(note_text) > 15) {
    send_json_(request, 400, "{\"error\":\"備份中的每日備註格式不正確\"}");
    return;
  }
  int target_index = -1;
  for (size_t index = 0; index < notes_.size(); index++) {
    if (notes_[index].active && notes_[index].employee_id == slot && notes_[index].date == date) {
      target_index = static_cast<int>(index);
      break;
    }
    if (target_index < 0 && !notes_[index].active)
      target_index = static_cast<int>(index);
  }
  if (target_index < 0) {
    send_json_(request, 409, "{\"error\":\"備份中的每日備註超過 500 筆\"}");
    return;
  }
  const bool was_active = notes_[target_index].active;
  notes_[target_index] = DailyNote{};
  notes_[target_index].date = date;
  notes_[target_index].employee_id = static_cast<uint16_t>(slot);
  notes_[target_index].active = 1;
  strlcpy(notes_[target_index].text, note_text.c_str(), sizeof(notes_[target_index].text));
  if (!was_active)
    imported_note_count_++;
  last_import_millis_ = millis();
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::import_log_batch_(AsyncWebServerRequest *request) {
  if (!check_import_token_(request)) {
    send_json_(request, 403, "{\"error\":\"資料還原工作階段無效或已逾時\"}");
    return;
  }
  const std::string records = param(request, "records");
  if (records.empty() || records.size() > 8192) {
    send_json_(request, 400, "{\"error\":\"刷卡紀錄批次大小不正確\"}");
    return;
  }
  struct ImportLogValue {
    uint32_t timestamp;
    uint16_t employee_id;
    uint8_t action;
    uint32_t uid_hash;
  };
  std::array<ImportLogValue, 64> values{};
  size_t value_count = 0;
  size_t cursor = 0;
  uint32_t previous_timestamp = last_import_timestamp_;
  while (cursor < records.size()) {
    const size_t line_end = records.find(';', cursor);
    const size_t end = line_end == std::string::npos ? records.size() : line_end;
    const std::string line = records.substr(cursor, end - cursor);
    cursor = line_end == std::string::npos ? records.size() : line_end + 1;
    if (line.empty())
      continue;
    if (value_count >= values.size()) {
      send_json_(request, 400, "{\"error\":\"每批最多匯入 64 筆刷卡紀錄\"}");
      return;
    }
    std::array<std::string, 4> fields{};
    size_t field_start = 0;
    bool field_error = false;
    for (size_t field = 0; field < fields.size(); field++) {
      const size_t separator = line.find(',', field_start);
      if (field < fields.size() - 1 && separator == std::string::npos) {
        field_error = true;
        break;
      }
      if (field == fields.size() - 1 && separator != std::string::npos) {
        field_error = true;
        break;
      }
      const size_t field_end = separator == std::string::npos ? line.size() : separator;
      fields[field] = line.substr(field_start, field_end - field_start);
      field_start = field_end + 1;
    }
    uint32_t timestamp = 0;
    uint32_t employee_id = 0;
    uint32_t action = 0;
    uint32_t uid_hash = 0;
    if (field_error || !parse_uint32_strict(fields[0], &timestamp) ||
        !parse_uint32_strict(fields[1], &employee_id) || !parse_uint32_strict(fields[2], &action) ||
        !parse_uint32_strict(fields[3], &uid_hash) || timestamp < 1577836800U || timestamp > 4102444799U ||
        employee_id < 1 || employee_id > MAX_EMPLOYEES || action > ACTION_OVERTIME_OUT ||
        timestamp < previous_timestamp) {
      send_json_(request, 400, "{\"error\":\"備份中的刷卡紀錄格式或時間順序不正確\"}");
      return;
    }
    values[value_count++] = ImportLogValue{timestamp, static_cast<uint16_t>(employee_id),
                                           static_cast<uint8_t>(action), uid_hash};
    previous_timestamp = timestamp;
  }
  const uint32_t capacity = log_region_size_ / sizeof(LogRecord);
  if (value_count == 0 || imported_log_count_ + value_count > capacity) {
    send_json_(request, 409, "{\"error\":\"備份中的刷卡紀錄超過裝置容量\"}");
    return;
  }
  for (size_t index = 0; index < value_count; index++) {
    const auto &value = values[index];
    if (!append_log_(value.employee_id, value.action, value.uid_hash, value.timestamp)) {
      send_json_(request, 500, "{\"error\":\"刷卡紀錄寫入 Flash 失敗，請重新開始匯入\"}");
      return;
    }
    imported_log_count_++;
    last_import_timestamp_ = value.timestamp;
  }
  last_import_millis_ = millis();
  send_json_(request, 200, "{\"ok\":true,\"imported\":" + std::to_string(imported_log_count_) + "}");
}

void AttendanceWeb::finish_backup_import_(AsyncWebServerRequest *request) {
  if (!check_import_token_(request)) {
    send_json_(request, 403, "{\"error\":\"資料還原工作階段無效或已逾時\"}");
    return;
  }
  if (!save_employees_() || !save_notes_()) {
    send_json_(request, 500, "{\"error\":\"員工或備註未能完成寫入，請再次按匯入重試\"}");
    return;
  }
  import_in_progress_ = false;
  import_token_ = 0;
  last_result_ = "備份還原完成：" + std::to_string(imported_employee_count_) + " 位員工、" +
                 std::to_string(imported_note_count_) + " 筆備註、" + std::to_string(imported_log_count_) +
                 " 筆刷卡紀錄";
  ESP_LOGW(TAG, "%s", last_result_.c_str());
  std::ostringstream output;
  output << "{\"ok\":true,\"employees\":" << imported_employee_count_ << ",\"notes\":"
         << imported_note_count_ << ",\"logs\":" << imported_log_count_ << "}";
  send_json_(request, 200, output.str());
}

void AttendanceWeb::abort_backup_import_(AsyncWebServerRequest *request) {
  if (!check_import_token_(request)) {
    send_json_(request, 403, "{\"error\":\"資料還原工作階段無效或已逾時\"}");
    return;
  }
  import_in_progress_ = false;
  import_token_ = 0;
  load_employees_();
  load_notes_();
  scan_flash_();
  last_result_ = "資料還原已中止；可重新選擇備份檔匯入";
  ESP_LOGW(TAG, "Backup import aborted");
  send_json_(request, 200, "{\"ok\":true}");
}

void AttendanceWeb::serve_export_data_(AsyncWebServerRequest *req) {
  constexpr uint16_t PAGE_SIZE = 100;
  const uint32_t offset = strtoul(param(req, "offset").c_str(), nullptr, 10);
  const uint32_t cutoff = cutoff_timestamp_();
  const uint32_t slots = log_region_size_ / sizeof(LogRecord);
  uint32_t matched = 0;
  uint16_t added = 0;
  bool has_more = false;
  bool first = true;
  LogRecord record{};
  std::ostringstream output;
  output << "{\"logs\":[";

  for (uint32_t n = 0; n < slots; n++) {
    const uint32_t distance = (n + 1) * sizeof(record);
    const uint32_t record_offset =
        (write_offset_ + log_region_size_ - (distance % log_region_size_)) % log_region_size_;
    if (esp_partition_read(partition_, record_offset, &record, sizeof(record)) != ESP_OK)
      break;
    if (record.magic != LOG_MAGIC || record.crc != fnv1a(&record, sizeof(record) - 4) || record.timestamp < cutoff)
      continue;
    if (matched++ < offset)
      continue;
    if (added >= PAGE_SIZE) {
      has_more = true;
      break;
    }

    const Employee *employee = nullptr;
    for (auto &candidate : employees_) {
      if (candidate.id == record.employee_id) {
        employee = &candidate;
        break;
      }
    }
    time_t timestamp = record.timestamp;
    struct tm time_info {};
    localtime_r(&timestamp, &time_info);
    char formatted_time[24];
    strftime(formatted_time, sizeof(formatted_time), "%Y-%m-%d %H:%M:%S", &time_info);
    if (!first)
      output << ",";
    first = false;
    output << "{\"time\":\"" << formatted_time << "\",\"timestamp\":" << record.timestamp
           << ",\"employee_id\":" << record.employee_id
           << ",\"id\":\"" << esc(employee ? employee->code : "") << "\",\"name\":\""
           << esc(employee ? employee->name : "") << "\",\"action\":\"" << action_label(record.action)
           << "\",\"action_code\":" << static_cast<unsigned>(record.action)
           << ",\"uid_hash\":" << record.uid_hash << "}";
    added++;
  }

  output << "],\"next\":" << (offset + added) << ",\"done\":" << (has_more ? "false" : "true") << "}";
  send_json_(req, 200, output.str());
}

void AttendanceWeb::serve_export_launcher_(AsyncWebServerRequest *request) {
  static const char EXPORT_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>產生 Excel 出勤彙總表</title>
<style>body{font:16px system-ui;background:#f3f5f7;color:#17202a;max-width:680px;margin:60px auto;padding:20px}.box{background:#fff;border-radius:14px;padding:24px;box-shadow:0 2px 12px #0001}a{color:#1769e0}</style>
<div class="box"><h2>正在產生 Excel 出勤彙總表</h2><p id="state">正在讀取刷卡資料，請不要關閉此頁面……</p><p><a href="/attendance/">返回管理頁</a></p></div>
<script>
const te=new TextEncoder();
const xmlEscape=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const le=(n,b)=>{const a=new Uint8Array(b);for(let i=0;i<b;i++)a[i]=(n>>>8*i)&255;return a};
const concat=a=>{let n=a.reduce((s,x)=>s+x.length,0),o=new Uint8Array(n),p=0;for(const x of a){o.set(x,p);p+=x.length}return o};
const crcTable=(()=>{let t=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=(c&1)?0xedb88320^(c>>>1):c>>>1;t[n]=c>>>0}return t})();
const crc32=d=>{let c=0xffffffff;for(const b of d)c=crcTable[(c^b)&255]^(c>>>8);return(c^0xffffffff)>>>0};
function makeZip(files){
  const local=[],central=[];let offset=0;
  for(const f of files){const name=te.encode(f.name),data=typeof f.data==='string'?te.encode(f.data):f.data,crc=crc32(data),flag=0x0800;
    const lh=concat([le(0x04034b50,4),le(20,2),le(flag,2),le(0,2),le(0,2),le(0x21,2),le(crc,4),le(data.length,4),le(data.length,4),le(name.length,2),le(0,2),name]);
    local.push(lh,data);
    central.push(concat([le(0x02014b50,4),le(20,2),le(20,2),le(flag,2),le(0,2),le(0,2),le(0x21,2),le(crc,4),le(data.length,4),le(data.length,4),le(name.length,2),le(0,2),le(0,2),le(0,2),le(0,2),le(0,4),le(offset,4),name]));
    offset+=lh.length+data.length;
  }
  const cd=concat(central),end=concat([le(0x06054b50,4),le(0,2),le(0,2),le(files.length,2),le(files.length,2),le(cd.length,4),le(offset,4),le(0,2)]);
  return new Blob([...local,cd,end],{type:'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet'});
}
const columns=['A','B','C','D','E','F','G','H','I','J'];
const headers=['日期','工號','姓名','上班打卡時間','下班打卡時間','加班上班時間','加班下班時間','備註','當日出勤工時','當日加班工時'];
const cell=(ref,value,style='')=>`<c r="${ref}" t="inlineStr"${style}><is><t xml:space="preserve">${xmlEscape(value)}</t></is></c>`;
const durationCell=(ref,value)=>value===null?cell(ref,''):`<c r="${ref}" s="2"><v>${value}</v></c>`;
const durationValue=(start,end)=>{if(!start||!end)return null;const parse=value=>{const parts=String(value).split(':').map(Number);return parts.length>=2&&parts.every(Number.isFinite)?parts[0]*60+parts[1]+(parts[2]||0)/60:null},from=parse(start),to=parse(end);if(from===null||to===null)return null;let minutes=to-from;if(minutes<0)minutes+=24*60;return minutes/(24*60)};
function worksheet(rows){let x=['<?xml version="1.0" encoding="UTF-8" standalone="yes"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><cols><col min="1" max="1" width="13" customWidth="1"/><col min="2" max="3" width="18" customWidth="1"/><col min="4" max="7" width="20" customWidth="1"/><col min="8" max="8" width="24" customWidth="1"/><col min="9" max="10" width="18" customWidth="1"/></cols><sheetData><row r="1">'];
  headers.forEach((v,i)=>x.push(cell(columns[i]+'1',v,' s="1"')));x.push('</row>');let rn=2;
  for(const r of rows){x.push(`<row r="${rn}">`);[r.date,r.id,r.name,r.clockIn,r.clockOut,r.overtimeIn,r.overtimeOut,r.note].forEach((v,i)=>x.push(cell(columns[i]+rn,v)));x.push(durationCell('I'+rn,durationValue(r.clockIn,r.clockOut)),durationCell('J'+rn,durationValue(r.overtimeIn,r.overtimeOut)),'</row>');rn++}
  x.push('</sheetData></worksheet>');return x.join('');
}
function uniqueSheetName(code,employeeName,used){let base=((code||'未設定')+'_'+(employeeName||'未命名')).replace(/[\\\/?*:[\]]/g,'_').replace(/^'+|'+$/g,'').slice(0,31)||'員工',name=base,n=2,key=name.toLowerCase();while(used.has(key)){const s='_'+n++;name=base.slice(0,31-s.length)+s;key=name.toLowerCase()}used.add(key);return name}
async function getJson(url){const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw Error('讀取失敗：HTTP '+r.status);return r.json()}
async function getAllNotes(){const notes=[];let offset=0;while(true){const batch=await getJson('/attendance/api/notes?paged=1&limit=50&offset='+offset);notes.push(...batch.notes);if(batch.done)break;if(batch.next<=offset)throw Error('備註分頁資料異常');offset=batch.next}return notes}
async function run(){try{
  const active=await getJson('/attendance/api/employees'),notes=await getAllNotes(),logs=[];let offset=0,page=0;
  while(true){state.textContent=`正在讀取第 ${++page} 批刷卡資料……`;const p=await getJson('/attendance/api/export-page?offset='+offset);logs.push(...p.logs);if(p.done)break;if(p.next<=offset)throw Error('分頁資料異常');offset=p.next}
  state.textContent='正在彙整每日上下班與加班時間……';
  const employees=new Map();for(const e of active)employees.set(e.id||('未設定-'+e.slot),{id:e.id||('未設定-'+e.slot),name:e.name,days:new Map()});
  for(const l of logs){const id=l.id||('未設定-'+l.employee_id),date=l.time.slice(0,10),time=l.time.slice(11,19);if(!employees.has(id))employees.set(id,{id,name:l.name||'',days:new Map()});const e=employees.get(id);if(!e.name&&l.name)e.name=l.name;let d=e.days.get(date);if(!d){d={date,id:e.id,name:e.name,clockIn:'',clockOut:'',overtimeIn:'',overtimeOut:'',note:''};e.days.set(date,d)}d.name=e.name;if(l.action==='上班'&&(!d.clockIn||time<d.clockIn))d.clockIn=time;if(l.action==='下班'&&(!d.clockOut||time>d.clockOut))d.clockOut=time;if(l.action==='加班上班'&&(!d.overtimeIn||time<d.overtimeIn))d.overtimeIn=time;if(l.action==='加班下班'&&(!d.overtimeOut||time>d.overtimeOut))d.overtimeOut=time}
  for(const n of notes){const id=n.id||('未設定-'+n.slot);if(!employees.has(id))employees.set(id,{id,name:n.name||'',days:new Map()});const e=employees.get(id);if(!e.name&&n.name)e.name=n.name;let d=e.days.get(n.date);if(!d){d={date:n.date,id:e.id,name:e.name,clockIn:'',clockOut:'',overtimeIn:'',overtimeOut:'',note:''};e.days.set(n.date,d)}d.name=e.name;d.note=n.note||''}
  const list=[...employees.values()].sort((a,b)=>a.id.localeCompare(b.id,'zh-Hant',{numeric:true}));const summary=[];for(const e of list)summary.push(...e.days.values());summary.sort((a,b)=>a.date.localeCompare(b.date)||a.id.localeCompare(b.id,'zh-Hant',{numeric:true}));
  const used=new Set(['出勤彙總'.toLowerCase()]),sheets=[{name:'出勤彙總',rows:summary}];for(const e of list)sheets.push({name:uniqueSheetName(e.id,e.name,used),rows:[...e.days.values()].sort((a,b)=>a.date.localeCompare(b.date))});
  state.textContent=`正在建立 ${sheets.length} 個 Excel 分頁……`;
  const files=[],sheetOverrides=[];sheets.forEach((s,i)=>{files.push({name:`xl/worksheets/sheet${i+1}.xml`,data:worksheet(s.rows)});sheetOverrides.push(`<Override PartName="/xl/worksheets/sheet${i+1}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>`)});
  files.push({name:'[Content_Types].xml',data:`<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>${sheetOverrides.join('')}</Types>`});
  files.push({name:'_rels/.rels',data:'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>'});
  files.push({name:'xl/workbook.xml',data:`<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>${sheets.map((s,i)=>`<sheet name="${xmlEscape(s.name)}" sheetId="${i+1}" r:id="rId${i+1}"/>`).join('')}</sheets></workbook>`});
  files.push({name:'xl/_rels/workbook.xml.rels',data:`<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">${sheets.map((s,i)=>`<Relationship Id="rId${i+1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet${i+1}.xml"/>`).join('')}<Relationship Id="rId${sheets.length+1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>`});
  files.push({name:'xl/styles.xml',data:'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><numFmts count="1"><numFmt numFmtId="164" formatCode="[h]:mm"/></numFmts><fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts><fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills><borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs><cellXfs count="3"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/><xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/></cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>'});
  const blob=makeZip(files),url=URL.createObjectURL(blob),now=new Date(),date=`${now.getFullYear()}-${String(now.getMonth()+1).padStart(2,'0')}-${String(now.getDate()).padStart(2,'0')}`,a=document.createElement('a');a.href=url;a.download=`attendance-summary-${date}.xlsx`;a.click();setTimeout(()=>URL.revokeObjectURL(url),30000);state.textContent=`完成：共 ${summary.length} 筆每日彙總、${sheets.length} 個分頁。若未自動下載，請返回後再按一次。`;
}catch(e){state.textContent='產生失敗：'+e.message}}
run();
</script></html>
)HTML";
  auto *response = request->beginResponse(200, "text/html; charset=utf-8",
                                          reinterpret_cast<const uint8_t *>(EXPORT_PAGE), sizeof(EXPORT_PAGE) - 1);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void AttendanceWeb::serve_logs_(AsyncWebServerRequest *req,bool csv) {
  uint32_t cutoff=cutoff_timestamp_(); std::ostringstream o; LogRecord x{};
  const uint32_t slots=log_region_size_/sizeof(x);
  if(!csv) {
    o<<"["; bool first=true; uint16_t found=0;
    for(uint32_t n=0;n<slots && found<100;n++) {
      uint32_t distance=(n+1)*sizeof(x);
      uint32_t off=(write_offset_+log_region_size_-(distance%log_region_size_))%log_region_size_;
      if(esp_partition_read(partition_,off,&x,sizeof(x))!=ESP_OK) break;
      if(x.magic!=LOG_MAGIC||x.crc!=fnv1a(&x,sizeof(x)-4)||x.timestamp<cutoff) continue;
      const Employee *e=nullptr; for(auto &q:employees_) if(q.id==x.employee_id){e=&q;break;}
      time_t tt=x.timestamp; struct tm tmv{}; localtime_r(&tt,&tmv); char ts[24]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&tmv);
      if (!first)
        o << ",";
      first = false;
      found++;
      o<<"{\"time\":\""<<ts<<"\",\"slot\":"<<x.employee_id<<",\"id\":\""<<esc(e?e->code:"")<<"\",\"name\":\""<<esc(e?e->name:"")<<"\",\"action\":\""<<action_label(x.action)<<"\"}";
    }
    o<<"]"; send_json_(req,200,o.str()); return;
  }
  o<<"\xEF\xBB\xBF時間,員工編號,姓名,狀態,備註\r\n";
  // Export in insertion order; manual entries can have earlier event timestamps.
  for(uint32_t n=0;n<slots;n++) { uint32_t off=(write_offset_+n*sizeof(x))%log_region_size_; if(esp_partition_read(partition_,off,&x,sizeof(x))!=ESP_OK)break; if(x.magic!=LOG_MAGIC||x.crc!=fnv1a(&x,sizeof(x)-4)||x.timestamp<cutoff)continue;
    const Employee *e=nullptr; for(auto &q:employees_)if(q.id==x.employee_id){e=&q;break;} time_t tt=x.timestamp; struct tm tmv{}; localtime_r(&tt,&tmv); char ts[24]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&tmv);
    const uint32_t date=static_cast<uint32_t>((tmv.tm_year+1900)*10000+(tmv.tm_mon+1)*100+tmv.tm_mday); const char *note=""; for(const auto &n:notes_)if(n.active&&n.employee_id==x.employee_id&&n.date==date){note=n.text;break;}
    o<<ts<<","<<csv_quote(e?e->code:"")<<","<<csv_quote(e?e->name:"")<<","<<action_label(x.action)<<","<<csv_quote(note)<<"\r\n";
  }
  auto *res=req->beginResponse(200,"text/csv; charset=utf-8",o.str().c_str()); res->addHeader("Content-Disposition","attachment; filename=attendance.csv"); req->send(res);
}

void AttendanceWeb::serve_page_(AsyncWebServerRequest *r) {
  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NFC 打卡鐘管理中心</title>
<style>body{font:15px system-ui;background:#f3f5f7;margin:auto;max-width:1080px;padding:18px;color:#17202a}h1{margin:4px 0 6px}h2{margin-top:0}.subtitle{color:#687386;margin-bottom:14px}.tabs{position:sticky;top:0;z-index:5;display:flex;gap:7px;overflow:auto;padding:9px;background:#f3f5f7;border-bottom:1px solid #dfe4ea}.tab-button{white-space:nowrap;background:white;color:#314154;border:1px solid #dce2e8}.tab-button.active{background:#1769e0;color:white;border-color:#1769e0}.tab-page{display:none}.tab-page.active{display:block}.card{background:white;border-radius:14px;padding:16px;margin:12px 0;box-shadow:0 2px 12px #0001}.device-status{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin-top:14px}.device-status>div{min-width:0;background:#f6f8fb;border:1px solid #e3e8ee;border-radius:10px;padding:11px}.device-status span{display:block;color:#687386;font-size:13px;margin-bottom:5px}.device-status b{display:block;overflow-wrap:anywhere}.device-status .mode-panel{grid-column:span 2}.mode-actions,.employee-form,.password-form,.backup-form,.toolbar{display:flex;gap:7px;flex-wrap:wrap;align-items:center}.mode-actions{margin-top:8px}.mode-actions button{padding:7px 10px}.mode-help{margin-top:8px;color:#687386;font-size:12px}.online{color:#138a45}.offline{color:#c62828}.manual{color:#1769e0}.overtime-in-text{color:#b77900}.overtime-out-text{color:#1769e0}button,a.btn{border:0;border-radius:8px;padding:9px 13px;background:#1769e0;color:white;text-decoration:none;cursor:pointer}.clock-in{background:#138a45}.clock-out,.danger{background:#c62828}.overtime-in{background:#d99200}.overtime-out{background:#1769e0}.secondary{background:#687386}button:disabled{opacity:.55;cursor:wait}input,select{padding:9px;border:1px solid #ccd2da;border-radius:8px;max-width:230px;background:white}.search{min-width:260px}.employee-form,.password-form,.backup-form{margin-bottom:8px}.backup-form input[type=file]{max-width:300px}.note-form{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.note-form input[type=text]{min-width:230px}dialog{border:0;border-radius:14px;padding:22px;max-width:390px;color:#17202a;box-shadow:0 10px 40px #0005}dialog::backdrop{background:#0008}dialog h3{margin-top:0}dialog input{box-sizing:border-box;width:100%;max-width:none;margin:7px 0 14px}table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:9px;border-bottom:1px solid #eee}.muted{color:#687386}.scroll{overflow:auto}.pager{display:flex;justify-content:center;align-items:center;gap:10px;margin-top:13px}.guide{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.guide>div{min-width:0;border:1px solid #e3e8ee;border-radius:10px;padding:12px}.guide h3{margin:0 0 9px}.guide p{margin:7px 0}.compact-form{display:grid;gap:8px;min-width:0}.compact-form input{box-sizing:border-box;width:100%;max-width:none;min-width:0}.compact-actions{display:flex;gap:7px;flex-wrap:wrap}.schedule-grid{display:grid;grid-template-columns:repeat(2,minmax(0,230px));gap:10px}.schedule-item{display:grid;gap:7px;border:1px solid #e3e8ee;border-radius:10px;padding:10px;background:#f8fafc}.schedule-item .checkbox-row{color:#17202a}.checkbox-row{display:flex;gap:9px;align-items:center;font-weight:700}.compact-form .checkbox-row input{width:auto;min-width:auto}.dot{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:7px;vertical-align:-1px}.green{background:#18a558}.red{background:#e53935}.blue{background:#1769e0}.yellow{background:#fbc02d}.orange{background:#ff7a00}.rainbow{background:conic-gradient(#f00,#ff7a00,#ff0,#0b0,#0cc,#06f,#a0f,#f00)}.off{background:#777}code{background:#eef2f6;border-radius:5px;padding:2px 5px;word-break:break-all}details{border:1px solid #e3e8ee;border-radius:10px;padding:12px;margin-top:12px}summary{cursor:pointer;font-weight:700}@media(max-width:780px){.device-status{grid-template-columns:repeat(2,minmax(0,1fr))}.device-status .mode-panel{grid-column:span 2}.guide{grid-template-columns:1fr}}@media(max-width:520px){body{padding:10px}.tabs{margin:0 -10px}.device-status{grid-template-columns:1fr}.device-status .mode-panel{grid-column:span 1}.schedule-grid{grid-template-columns:1fr}.employee-form input,.password-form input,.backup-form input,.search{box-sizing:border-box;width:100%;max-width:none;min-width:0}}</style>
<h1>NFC 打卡鐘管理中心</h1><div class="subtitle">員工、打卡紀錄與系統設定</div>
<nav class="tabs" aria-label="管理功能"><button class="tab-button active" data-tab="dashboard" onclick="showTab('dashboard')">總覽</button><button class="tab-button" data-tab="records" onclick="showTab('records')">刷卡紀錄</button><button class="tab-button" data-tab="employees" onclick="showTab('employees')">員工管理</button><button class="tab-button" data-tab="notes" onclick="showTab('notes')">請假／備註</button><button class="tab-button" data-tab="settings" onclick="showTab('settings')">系統設定</button><button class="tab-button" data-tab="help" onclick="showTab('help')">使用說明</button></nav>
<main>
<section id="tab-dashboard" class="tab-page active"><div class="card"><b id="status">正在讀取打卡鐘狀態…</b><div class="muted">最多可管理 100 位員工，刷卡資料保存最近兩個日曆月。</div><div class="device-status"><div><span>打卡鐘時間</span><b id="current_time">讀取中…</b></div><div><span>時間設定方式</span><b id="time_source">讀取中…</b></div><div><span>備用時鐘</span><b id="hardware_rtc_status">讀取中…</b></div><div><span>網路狀態</span><b id="network_status">讀取中…</b></div><div><span>Wi-Fi 名稱</span><b id="wifi_ssid">讀取中…</b></div><div><span>IP 位址</span><b id="ip_address">讀取中…</b></div><div class="mode-panel"><span>目前打卡模式</span><b id="mode_display">讀取中…</b><div class="mode-actions"><button id="clock_in_mode_button" class="clock-in" onclick="setAttendanceMode('in')">上班</button><button id="clock_out_mode_button" class="clock-out" onclick="setAttendanceMode('out')">下班</button><button id="overtime_in_mode_button" class="overtime-in" onclick="setAttendanceMode('overtime_in')">加班上班</button><button id="overtime_out_mode_button" class="overtime-out" onclick="setAttendanceMode('overtime_out')">加班下班</button></div><div id="mode_help" class="mode-help">正在讀取自動切換設定…</div></div></div></div><div class="card"><h2>近期刷卡</h2><p class="muted">顯示最近 10 筆成功保存的紀錄；完整資料請前往「刷卡紀錄」。</p><div class="scroll"><table><thead><tr><th>時間</th><th>員工編號</th><th>姓名</th><th>打卡類型</th></tr></thead><tbody id="dashboard_logs"><tr><td colspan="4">讀取中…</td></tr></tbody></table></div></div></section>
<section id="tab-records" class="tab-page"><div class="card"><h2>管理員補打卡</h2><div class="note-form"><select id="manual_punch_employee" aria-label="補打卡員工"><option value="">選擇員工</option></select><input id="manual_punch_time" type="datetime-local" step="1" aria-label="補打卡日期時間"><select id="manual_punch_action" aria-label="補打卡類型"><option value="1">上班</option><option value="0">下班</option><option value="2">加班上班</option><option value="3">加班下班</option></select><input id="manual_punch_password" type="password" autocomplete="current-password" placeholder="管理員密碼"><button id="manual_punch_button" onclick="saveManualPunch()">保存補打卡</button></div><p class="muted">使用打卡鐘時區（UTC+8），可補登最近兩個日曆月內的紀錄，不可設定未來時間。補打卡會新增一筆紀錄，並納入 Excel 與備份；不會取代原有紀錄。</p><p id="manual_punch_result" role="status" class="muted"></p></div><div class="card"><h2>刷卡紀錄</h2><div class="toolbar"><input id="records_search" class="search" placeholder="搜尋日期、工號、姓名或打卡類型" oninput="logPage=1;renderLogs()"><a class="btn" href="/attendance/export.xlsx">下載兩個月 Excel 彙總表</a></div><p class="muted">最近 100 筆紀錄以每頁 20 筆顯示；下載的 Excel 仍包含裝置內保存的完整兩個月資料。</p><div class="scroll"><table><thead><tr><th>時間</th><th>員工編號</th><th>姓名</th><th>打卡類型</th><th>當日備註</th></tr></thead><tbody id="logs"><tr><td colspan="5">切換到本頁後自動讀取…</td></tr></tbody></table></div><div class="pager"><button class="secondary" onclick="changeLogPage(-1)">上一頁</button><span id="logs_page_info">第 1 頁</span><button class="secondary" onclick="changeLogPage(1)">下一頁</button></div></div></section>
<section id="tab-employees" class="tab-page"><div class="card"><h2>員工管理</h2><div class="employee-form"><input id="employee_code" maxlength="16" placeholder="員工編號，例如 JFE-001"><input id="employee_name" maxlength="40" placeholder="員工姓名"><input id="employee_password" type="password" autocomplete="current-password" placeholder="管理員密碼"><button onclick="saveEmployee()">新增／更新員工</button></div><p class="muted">新增、修改或刪除員工時都需要管理員密碼。相同工號或卡片不會重複登記。</p><div class="toolbar"><input id="employee_search" class="search" placeholder="搜尋員工編號、姓名或卡號" oninput="employeePage=1;renderEmployees()"><span id="employee_count" class="muted"></span></div><div class="scroll"><table><thead><tr><th>員工編號</th><th>姓名</th><th>卡號</th><th>狀態</th><th></th></tr></thead><tbody id="employees"><tr><td colspan="5">切換到本頁後自動讀取…</td></tr></tbody></table></div><div class="pager"><button class="secondary" onclick="changeEmployeePage(-1)">上一頁</button><span id="employees_page_info">第 1 頁</span><button class="secondary" onclick="changeEmployeePage(1)">下一頁</button></div></div></section>
<section id="tab-notes" class="tab-page"><div class="card"><h2>每日請假／備註</h2><p class="muted">由管理員手動記錄事假、病假或其他說明；每筆最多 15 個字，最多保存 500 筆。</p><div class="note-form"><input id="note_date" type="date"><select id="note_employee"><option value="">選擇員工</option></select><input id="note_text" type="text" maxlength="15" placeholder="例如：事假、病假、特休"><button id="save_note_button" onclick="saveDailyNote()">儲存備註</button></div><div class="toolbar"><input id="notes_search" class="search" placeholder="搜尋日期、工號、姓名或備註" oninput="notePage=1;renderNotes()"><span id="notes_count" class="muted"></span></div><div class="scroll"><table><thead><tr><th>日期</th><th>員工編號</th><th>姓名</th><th>備註</th><th></th></tr></thead><tbody id="notes"><tr><td colspan="5">切換到本頁後自動讀取…</td></tr></tbody></table></div><div class="pager"><button class="secondary" onclick="changeNotePage(-1)">上一頁</button><span id="notes_page_info">第 1 頁</span><button class="secondary" onclick="changeNotePage(1)">下一頁</button></div></div></section>
<section id="tab-settings" class="tab-page"><div class="card"><h2>自動模式切換</h2><div class="compact-form"><label class="checkbox-row"><input id="mode_schedule_enabled" type="checkbox" onchange="toggleScheduleInputs()">啟用自動定時切換</label><div class="schedule-grid"><div class="schedule-item"><label class="checkbox-row"><input id="clock_in_schedule_enabled" type="checkbox" onchange="toggleScheduleInputs()">上班</label><input id="clock_in_time" type="time" step="60" aria-label="上班切換時間"></div><div class="schedule-item"><label class="checkbox-row"><input id="clock_out_schedule_enabled" type="checkbox" onchange="toggleScheduleInputs()">下班</label><input id="clock_out_time" type="time" step="60" aria-label="下班切換時間"></div><div class="schedule-item"><label class="checkbox-row"><input id="overtime_in_schedule_enabled" type="checkbox" onchange="toggleScheduleInputs()">加班上班</label><input id="overtime_in_time" type="time" step="60" aria-label="加班上班切換時間"></div><div class="schedule-item"><label class="checkbox-row"><input id="overtime_out_schedule_enabled" type="checkbox" onchange="toggleScheduleInputs()">加班下班</label><input id="overtime_out_time" type="time" step="60" aria-label="加班下班切換時間"></div></div><input id="mode_schedule_password" type="password" autocomplete="current-password" placeholder="管理員密碼"><div class="compact-actions"><button id="save_mode_schedule_button" onclick="saveModeSchedule()">儲存自動切換設定</button></div></div><p id="mode_schedule_summary" class="muted">正在讀取目前設定…</p><p class="muted">四種模式可分別勾選並設定每天的切換時間；已啟用的模式不可使用相同時間。關閉總開關後，模式只會由實體按鍵、管理網頁或 Home Assistant 變更。設定會保存於裝置，重新開機仍保留。</p></div><div class="card"><h2>時間設定</h2><div class="guide"><div><h3>自動校時方式</h3><p>開機時先使用備用時鐘，網路恢復後會自動校正時間。</p><p>管理員手動設定後，本次開機期間會以手動時間為準；重新開機後恢復網路自動校時。</p></div><div><h3>管理員手動校時</h3><div class="compact-form"><input id="manual_time" type="datetime-local" step="1"><input id="time_password" type="password" autocomplete="current-password" placeholder="管理員密碼"><div class="compact-actions"><button id="set_manual_time_button" onclick="setManualTime()">使用手動時間</button><button id="restore_auto_time_button" class="secondary" onclick="restoreAutomaticTime()">恢復網路校時</button></div></div><p class="muted">手動時間會同步保存到備用時鐘，斷電期間仍能繼續走時。</p></div></div></div><div class="card"><h2>管理員密碼</h2><div class="password-form"><input id="current_admin_password" type="password" autocomplete="current-password" placeholder="目前管理員密碼"><input id="new_admin_password" type="password" autocomplete="new-password" minlength="8" maxlength="64" placeholder="新密碼（8～64 字元）"><input id="confirm_admin_password" type="password" autocomplete="new-password" minlength="8" maxlength="64" placeholder="再次輸入新密碼"><button id="change_admin_password_button" onclick="changeAdminPassword()">修改管理員密碼</button></div><p class="muted">管理頁登入與所有需要核實身分的操作使用同一組密碼；帳號固定為 <code>admin</code>。</p></div><div class="card"><h2>資料備份與還原</h2><p><button id="export_backup_button" onclick="exportBackup()">匯出完整備份</button></p><div class="backup-form"><input id="backup_file" type="file" accept="application/json,.json"><input id="backup_password" type="password" autocomplete="current-password" placeholder="管理員密碼"><button id="import_backup_button" class="danger" onclick="importBackup()">匯入並取代現有資料</button></div><p id="backup_status" class="muted">備份包含員工、卡片、每日備註及刷卡紀錄，不包含管理員密碼、Wi-Fi 密碼或自動切換設定。</p><p class="muted">匯入前請先匯出目前資料。匯入會清除裝置內既有資料，再以備份內容取代。</p></div><div class="card"><h2>清除刷卡紀錄</h2><p>永久刪除裝置內所有刷卡紀錄，不會刪除員工與每日備註。</p><button id="clear_logs_button" class="danger" onclick="clearAllLogs()">刪除所有刷卡紀錄</button><p class="muted">建議先下載 Excel 或匯出完整備份。刪除前必須再次輸入管理員密碼。</p></div></section>
<section id="tab-help" class="tab-page"><div class="card"><h2>燈號與聲音提示</h2><div class="guide"><div><h3>燈號</h3><p><span class="dot green"></span><b>綠燈：</b>上班打卡模式。</p><p><span class="dot red"></span><b>紅燈：</b>下班打卡模式。</p><p><span class="dot yellow"></span><b>黃燈：</b>加班上班模式。</p><p><span class="dot blue"></span><b>藍燈：</b>加班下班模式。</p><p><span class="dot rainbow"></span><b>七彩循環：</b>請將要登記的新卡靠近感應區。</p><p><span class="dot orange"></span><b>橘燈閃爍：</b>Wi-Fi 未連線；若備用時鐘正常，仍可照常刷卡。</p><p><span class="dot off"></span><b>燈光熄滅：</b>裝置正在開機，或尚未選擇打卡模式。</p></div><div><h3>蜂鳴器</h3><p><b>叫一聲：</b>刷卡成功，或新卡登記完成。</p><p><b>連叫三聲：</b>卡片尚未登記、卡片已屬於其他員工、時間尚未準備完成，或資料保存失敗。</p><p>發生問題時，請回到「總覽」查看畫面最上方的提示。</p></div><div><h3>日常使用</h3><p>按下打卡鐘上的模式按鍵，或在「總覽」遠端切換模式，再請員工依序刷卡。</p><p>登記新卡時，先到「員工管理」找到該員工並按「登記卡片」，看到七彩燈後再將卡片靠近感應區。</p></div><div><h3>管理網址</h3><p><a href="http://nfc-attendance-clock.local/attendance/"><code>http://nfc-attendance-clock.local/attendance/</code></a></p><p>電腦或手機需與打卡鐘連接同一個區域網路。若固定名稱無法開啟，請改用「總覽」顯示的 IP 位址。</p></div></div><details><summary>進階設備資訊</summary><p>軟體版本：v1.3.34｜讀卡器：RC522｜備用時鐘：DS3231｜狀態燈：GPIO8 WS2812（亮度 65%）</p><p>時間會依序使用中國、台灣、新加坡的網路校時服務。固定網址使用 mDNS；部分路由器或不同網段可能無法使用固定名稱。</p></details></div></section>
</main>
<dialog id="clear_logs_dialog" onclose="document.getElementById('clear_logs_password').value=''"><h3>確認刪除紀錄</h3><p>請重新輸入管理員密碼。核實成功後，所有刷卡紀錄會立即永久刪除。</p><input id="clear_logs_password" type="password" autocomplete="current-password" placeholder="管理員密碼" onkeydown="if(event.key==='Enter'){event.preventDefault();confirmClearLogs()}"><div><button id="confirm_clear_logs_button" class="danger" onclick="confirmClearLogs()">確認永久刪除</button> <button class="secondary" onclick="closeClearLogsDialog()">取消</button></div></dialog>
<script>
const element=id=>document.getElementById(id);
const html=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const json=(url,options)=>fetch(url,{cache:'no-store',...options}).then(async response=>{const value=await response.json();if(!response.ok)throw Error(value.error||response.status);return value});
const PAGE_SIZE=20;
let currentTab='dashboard',employeesCache=[],notesCache=[],logsCache=[],scheduleFormLoaded=false;
let employeePage=1,notePage=1,logPage=1;
const pageCount=items=>Math.max(1,Math.ceil(items.length/PAGE_SIZE));
const pageSlice=(items,page)=>items.slice((page-1)*PAGE_SIZE,page*PAGE_SIZE);
function consumerTimeSource(value){
  const text=String(value||'');
  if(text.includes('手動'))return '管理員手動設定';
  if(text.includes('SNTP')||text.includes('網路'))return '網路自動校正';
  if(text.includes('DS3231'))return '備用時鐘';
  return text||'等待時間準備完成';
}
function showTab(tab){
  if(!element('tab-'+tab))tab='dashboard';
  currentTab=tab;
  document.querySelectorAll('.tab-page').forEach(page=>page.classList.toggle('active',page.id==='tab-'+tab));
  document.querySelectorAll('.tab-button').forEach(button=>button.classList.toggle('active',button.dataset.tab===tab));
  history.replaceState(null,'','#'+tab);
  load();
}
async function loadStatus(){
  const status=await json('/attendance/api/status');
  element('status').textContent=status.result+'｜目前模式：'+status.mode+(status.enrolling?'｜請將新卡靠近感應區':'')+(status.time_valid?'':'｜時間尚未準備完成');
  const modeDisplay=element('mode_display');
  modeDisplay.textContent=status.mode||'未設定';
  modeDisplay.className=status.mode==='上班'?'online':(status.mode==='下班'?'offline':(status.mode==='加班上班'?'overtime-in-text':(status.mode==='加班下班'?'overtime-out-text':'')));
  element('current_time').textContent=status.current_time||'尚未準備完成';
  element('time_source').textContent=consumerTimeSource(status.time_source);
  element('time_source').className=status.manual_time_override?'manual':(status.time_valid?'online':'offline');
  const hardwareRtcStatus=element('hardware_rtc_status');
  hardwareRtcStatus.textContent=status.hardware_rtc_connected?'運作正常':'未偵測到';
  hardwareRtcStatus.className=status.hardware_rtc_connected?'online':'offline';
  const manualTimeInput=element('manual_time');
  if(status.time_valid&&document.activeElement!==manualTimeInput)manualTimeInput.value=(status.current_time||'').replace(' ','T');
  const noteDateInput=element('note_date');
  if(status.time_valid&&!noteDateInput.value)noteDateInput.value=(status.current_time||'').slice(0,10);
  const networkStatus=element('network_status');
  networkStatus.textContent=status.network_connected?'連線正常':'目前離線';
  networkStatus.className=status.network_connected?'online':'offline';
  element('wifi_ssid').textContent=status.wifi_ssid||'未連線';
  element('ip_address').textContent=status.ip_address||'尚未取得';
  const scheduleEnabled=Boolean(status.mode_schedule_enabled);
  const schedules=[
    {id:'clock_in',label:'上班',enabled:Boolean(status.clock_in_schedule_enabled),time:String(status.clock_in_time||'00:00')},
    {id:'clock_out',label:'下班',enabled:Boolean(status.clock_out_schedule_enabled),time:String(status.clock_out_time||'14:00')},
    {id:'overtime_in',label:'加班上班',enabled:Boolean(status.overtime_in_schedule_enabled),time:String(status.overtime_in_time||'18:00')},
    {id:'overtime_out',label:'加班下班',enabled:Boolean(status.overtime_out_schedule_enabled),time:String(status.overtime_out_time||'22:00')}
  ];
  const activeScheduleText=schedules.filter(schedule=>schedule.enabled).map(schedule=>`${schedule.time} ${schedule.label}`).join('、');
  const savedScheduleText=schedules.map(schedule=>`${schedule.label} ${schedule.time}${schedule.enabled?'':'（未啟用）'}`).join('、');
  element('mode_help').textContent=scheduleEnabled?`按下模式後會維持到下一個排程時間；目前排程：${activeScheduleText}。`:'自動定時切換目前已關閉；模式只會由按鍵、網頁或 Home Assistant 變更。';
  element('mode_schedule_summary').textContent=scheduleEnabled?`目前已啟用：${activeScheduleText}。`:`目前已關閉；已保存設定：${savedScheduleText}。`;
  if(!scheduleFormLoaded){
    element('mode_schedule_enabled').checked=scheduleEnabled;
    schedules.forEach(schedule=>{element(`${schedule.id}_schedule_enabled`).checked=schedule.enabled;element(`${schedule.id}_time`).value=schedule.time});
    scheduleFormLoaded=true;toggleScheduleInputs();
  }
}
function toggleScheduleInputs(){
  const enabled=element('mode_schedule_enabled').checked;
  ['clock_in','clock_out','overtime_in','overtime_out'].forEach(id=>{
    const scheduleEnabled=element(`${id}_schedule_enabled`);scheduleEnabled.disabled=!enabled;element(`${id}_time`).disabled=!enabled||!scheduleEnabled.checked;
  });
}
async function saveManualPunch(){
  const slot=element('manual_punch_employee').value,value=element('manual_punch_time').value,
    action=element('manual_punch_action').value,input=element('manual_punch_password'),password=input.value;
  if(!slot||!value){alert('請選擇員工並輸入補打卡日期時間');return}
  // Explicit device timezone, independent of the browser's timezone.
  const timestamp=Math.floor(Date.parse(value+'+08:00')/1000);
  if(!Number.isFinite(timestamp)){alert('日期時間格式不正確');return}
  if(!password){alert('請輸入管理員密碼');input.focus();return}
  const employee=element('manual_punch_employee').selectedOptions[0].textContent,
    label=element('manual_punch_action').selectedOptions[0].textContent;
  if(!confirm(`確定新增補打卡？\n${employee}\n${value.replace('T',' ')}（UTC+8） ${label}`))return;
  const button=element('manual_punch_button'),result=element('manual_punch_result');button.disabled=true;
  try{
    await postForm('/attendance/api/manual-punch',{slot,action,timestamp:String(timestamp),password});
    input.value='';result.textContent=`已保存：${employee} ${value.replace('T',' ')} ${label}。`;
    try{await loadStatus();await loadLogs()}catch(error){result.textContent+=' 畫面更新失敗，請重新整理；補打卡已保存，請勿重複提交。'}
  }catch(error){input.value='';result.textContent='未確認保存成功：'+error.message+'。若連線中斷，請先查核紀錄再重試。'}finally{button.disabled=false}
}
async function saveModeSchedule(){
  const enabled=element('mode_schedule_enabled').checked,passwordInput=element('mode_schedule_password'),password=passwordInput.value;
  const schedules=['clock_in','clock_out','overtime_in','overtime_out'].map(id=>({id,enabled:element(`${id}_schedule_enabled`).checked,time:element(`${id}_time`).value}));
  if(schedules.some(schedule=>!schedule.time)){alert('請設定四種模式的切換時間');return}
  const activeSchedules=schedules.filter(schedule=>schedule.enabled);
  if(enabled&&activeSchedules.length===0){alert('啟用自動切換時，請至少勾選一種模式');return}
  if(new Set(activeSchedules.map(schedule=>schedule.time)).size!==activeSchedules.length){alert('已啟用的模式不可設定相同切換時間');return}
  if(!password){alert('請輸入管理員密碼');passwordInput.focus();return}
  const button=element('save_mode_schedule_button');button.disabled=true;
  try{
    const values={enabled:enabled?'1':'0',password};
    schedules.forEach(schedule=>{values[`${schedule.id}_enabled`]=schedule.enabled?'1':'0';values[`${schedule.id}_time`]=schedule.time});
    await postForm('/attendance/api/mode-schedule',values);
    passwordInput.value='';scheduleFormLoaded=false;await loadStatus();alert(enabled?'自動定時切換已啟用並保存。':'自動定時切換已關閉並保存。');
  }catch(error){passwordInput.value='';alert(error.message);passwordInput.focus()}finally{button.disabled=false}
}
function updateNoteEmployeeOptions(){
  const noteEmployee=element('note_employee'),selectedEmployee=noteEmployee.value;
  noteEmployee.innerHTML='<option value="">選擇員工</option>'+employeesCache.map(employee=>`<option value="${employee.slot}">${html(employee.id)}｜${html(employee.name)}</option>`).join('');
  if(employeesCache.some(employee=>String(employee.slot)===selectedEmployee))noteEmployee.value=selectedEmployee;
}
async function loadEmployees(){
  employeesCache=await json('/attendance/api/employees');
  employeesCache.sort((a,b)=>String(a.id).localeCompare(String(b.id),'zh-Hant',{numeric:true}));
  updateNoteEmployeeOptions();
  const punchEmployee=element('manual_punch_employee'),punchSelection=punchEmployee.value;
  punchEmployee.innerHTML='<option value="">選擇員工</option>'+employeesCache.map(employee=>`<option value="${employee.slot}">${html(employee.id)}｜${html(employee.name)}</option>`).join('');
  if(employeesCache.some(employee=>String(employee.slot)===punchSelection))punchEmployee.value=punchSelection;
  renderEmployees();
}
async function readAllNotes(includeAll=false,progress=null){
  const notes=[];let offset=0,page=0;
  while(true){
    if(progress)progress.textContent=`正在讀取第 ${++page} 批每日備註……`;
    const batch=await json(`/attendance/api/notes?paged=1&limit=50&offset=${offset}${includeAll?'&all=1':''}`);
    notes.push(...batch.notes);
    if(batch.done)break;
    if(batch.next<=offset)throw Error('備註分頁資料異常');
    offset=batch.next;
  }
  return notes;
}
async function loadNotes(){
  notesCache=await readAllNotes(false);
  notesCache.sort((a,b)=>b.date.localeCompare(a.date)||String(a.id).localeCompare(String(b.id),'zh-Hant',{numeric:true}));
  renderNotes();
}
async function loadLogs(){
  logsCache=await json('/attendance/api/logs');
  logsCache.sort((a,b)=>b.time.localeCompare(a.time));
  renderDashboardLogs();
  renderLogs();
}
function renderDashboardLogs(){
  const rows=logsCache.slice(0,10);
  element('dashboard_logs').innerHTML=rows.length?rows.map(log=>`<tr><td>${html(log.time)}</td><td>${html(log.id||'未設定')}</td><td>${html(log.name||'未知員工')}</td><td>${html(log.action)}</td></tr>`).join(''):'<tr><td colspan="4">尚無成功保存的刷卡紀錄</td></tr>';
}
function renderEmployees(){
  const query=element('employee_search').value.trim().toLocaleLowerCase(),filtered=employeesCache.filter(employee=>[employee.id,employee.name,employee.uid].some(value=>String(value||'').toLocaleLowerCase().includes(query))),pages=pageCount(filtered);
  employeePage=Math.min(employeePage,pages);const rows=pageSlice(filtered,employeePage);
  element('employee_count').textContent=`共 ${filtered.length} 位員工`;
  element('employees_page_info').textContent=`第 ${employeePage}／${pages} 頁`;
  element('employees').innerHTML=rows.length?rows.map(employee=>`<tr><td>${html(employee.id)}</td><td>${html(employee.name)}</td><td>${html(employee.uid||'尚未登記')}</td><td>${employee.active?'使用中':'已停用'}</td><td><button onclick="enroll(${employee.slot})">登記卡片</button> <button class="danger" onclick="removeEmployee(${employee.slot})">刪除</button></td></tr>`).join(''):'<tr><td colspan="5">沒有符合條件的員工</td></tr>';
}
function renderNotes(){
  const query=element('notes_search').value.trim().toLocaleLowerCase(),filtered=notesCache.filter(note=>[note.date,note.id,note.name,note.note].some(value=>String(value||'').toLocaleLowerCase().includes(query))),pages=pageCount(filtered);
  notePage=Math.min(notePage,pages);const rows=pageSlice(filtered,notePage);
  element('notes_count').textContent=`共 ${filtered.length} 筆備註`;
  element('notes_page_info').textContent=`第 ${notePage}／${pages} 頁`;
  element('notes').innerHTML=rows.length?rows.map(note=>`<tr><td>${html(note.date)}</td><td>${html(note.id||'—')}</td><td>${html(note.name)}</td><td>${html(note.note)}</td><td><button class="secondary" onclick="editDailyNote(${note.slot},'${note.date}','${encodeURIComponent(note.note)}')">修改</button> <button class="danger" onclick="deleteDailyNote(${note.slot},'${note.date}')">刪除</button></td></tr>`).join(''):'<tr><td colspan="5">沒有符合條件的備註</td></tr>';
}
function renderLogs(){
  const searchElement=element('records_search');
  if(!searchElement)return;
  const query=searchElement.value.trim().toLocaleLowerCase(),filtered=logsCache.filter(log=>[log.time,log.id,log.name,log.action].some(value=>String(value||'').toLocaleLowerCase().includes(query))),pages=pageCount(filtered),noteMap=new Map(notesCache.map(note=>[note.date+'|'+note.slot,note.note]));
  logPage=Math.min(logPage,pages);const rows=pageSlice(filtered,logPage);
  element('logs_page_info').textContent=`第 ${logPage}／${pages} 頁，共 ${filtered.length} 筆`;
  element('logs').innerHTML=rows.length?rows.map(log=>`<tr><td>${html(log.time)}</td><td>${html(log.id||'未設定')}</td><td>${html(log.name||'未知員工')}</td><td>${html(log.action)}</td><td>${html(noteMap.get(log.time.slice(0,10)+'|'+log.slot)||'')}</td></tr>`).join(''):'<tr><td colspan="5">沒有符合條件的刷卡紀錄</td></tr>';
}
function changeEmployeePage(step){employeePage=Math.max(1,employeePage+step);renderEmployees()}
function changeNotePage(step){notePage=Math.max(1,notePage+step);renderNotes()}
function changeLogPage(step){logPage=Math.max(1,logPage+step);renderLogs()}
async function loadActiveTab(){
  if(currentTab==='dashboard')await loadLogs();
  else if(currentTab==='records'){await Promise.all([loadLogs(),loadNotes(),loadEmployees()]);renderLogs()}
  else if(currentTab==='employees')await loadEmployees();
  else if(currentTab==='notes')await Promise.all([loadEmployees(),loadNotes()]);
}
async function load(){
  try{await loadStatus();await loadActiveTab()}
  catch(error){element('status').textContent='資料讀取失敗，請確認打卡鐘連線後重試：'+error.message}
}
async function setAttendanceMode(mode){
  const modes={in:{label:'上班',button:'clock_in_mode_button'},out:{label:'下班',button:'clock_out_mode_button'},overtime_in:{label:'加班上班',button:'overtime_in_mode_button'},overtime_out:{label:'加班下班',button:'overtime_out_mode_button'}};
  const selected=modes[mode];if(!selected)return;
  if(!confirm(`確定切換為${selected.label}模式？接下來所有員工都會刷${selected.label}卡。`))return;
  const button=element(selected.button);button.disabled=true;
  try{
    const body=new URLSearchParams({mode});
    await json('/attendance/api/mode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    await load();
  }catch(error){alert(error.message)}finally{button.disabled=false}
}
async function saveEmployee(){
  const codeInput=element('employee_code'),nameInput=element('employee_name'),passwordInput=element('employee_password');
  const codeValue=codeInput.value.trim(),nameValue=nameInput.value.trim(),password=passwordInput.value;
  if(!codeValue||!nameValue){alert('請輸入員工編號與姓名');return}
  if(!password){alert('請輸入管理頁密碼');passwordInput.focus();return}
  try{
    const body=new URLSearchParams({slot:'0',code:codeValue,name:nameValue,active:'1',password});
    await json('/attendance/api/employee',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    codeInput.value='';nameInput.value='';passwordInput.value='';await load();
  }catch(error){passwordInput.value='';alert(error.message);passwordInput.focus()}
}
async function enroll(slot){try{await json('/attendance/api/enroll?slot='+slot);await load()}catch(error){alert(error.message)}}
async function removeEmployee(slot){
  const passwordInput=element('employee_password'),password=passwordInput.value;
  if(!password){alert('請先在員工管理區輸入管理頁密碼');passwordInput.focus();return}
  if(!confirm('確定刪除這位員工？原卡片將立即失效。'))return;
  try{
    const body=new URLSearchParams({slot:String(slot),password});
    await json('/attendance/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    passwordInput.value='';await load();
  }catch(error){passwordInput.value='';alert(error.message);passwordInput.focus()}
}
async function saveDailyNote(){
  const date=element('note_date').value,slot=element('note_employee').value,note=element('note_text').value.trim();
  if(!date||!slot){alert('請選擇日期與員工');return}if(!note){alert('請輸入備註內容');return}if([...note].length>15){alert('備註最多 15 個字');return}
  const button=element('save_note_button');button.disabled=true;
  try{const body=new URLSearchParams({date,slot,note});await json('/attendance/api/note',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});element('note_text').value='';await load()}catch(error){alert(error.message)}finally{button.disabled=false}
}
function editDailyNote(slot,date,note){element('note_date').value=date;element('note_employee').value=String(slot);element('note_text').value=decodeURIComponent(note);element('note_text').focus();scrollTo({top:element('note_date').getBoundingClientRect().top+scrollY-100,behavior:'smooth'})}
async function deleteDailyNote(slot,date){if(!confirm('確定刪除這筆每日備註？'))return;try{const body=new URLSearchParams({date,slot,note:''});await json('/attendance/api/note',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});await load()}catch(error){alert(error.message)}}
async function setManualTime(){
  const timeInput=element('manual_time'),passwordInput=element('time_password'),value=timeInput.value,password=passwordInput.value;
  if(!value){alert('請選擇要設定的日期與時間');return}
  if(!password){alert('請輸入管理頁密碼');return}
  const timestamp=Math.floor(new Date(value).getTime()/1000);
  if(!Number.isFinite(timestamp)){alert('日期時間格式不正確');return}
  const button=element('set_manual_time_button');button.disabled=true;
  try{
    const body=new URLSearchParams({timestamp:String(timestamp),password});
    await json('/attendance/api/manual-time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    passwordInput.value='';alert('手動時間已設定並保存到備用時鐘；本次重新開機前會以手動時間為準。');await load();
  }catch(error){passwordInput.value='';alert(error.message)}finally{button.disabled=false}
}
async function restoreAutomaticTime(){
  const passwordInput=element('time_password'),password=passwordInput.value;
  if(!password){alert('請輸入管理頁密碼');return}
  if(!confirm('確定恢復網路自動校時？'))return;
  const button=element('restore_auto_time_button');button.disabled=true;
  try{
    const body=new URLSearchParams({password});
    await json('/attendance/api/automatic-time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    passwordInput.value='';alert('已恢復網路自動校時。');await load();
  }catch(error){passwordInput.value='';alert(error.message)}finally{button.disabled=false}
}
function clearAllLogs(){
  if(!confirm('警告：這會永久刪除所有刷卡紀錄，而且無法復原。確定要繼續嗎？'))return;
  const input=element('clear_logs_password');input.value='';element('clear_logs_dialog').showModal();setTimeout(()=>input.focus(),0);
}
function closeClearLogsDialog(){element('clear_logs_password').value='';element('clear_logs_dialog').close()}
async function confirmClearLogs(){
  const input=element('clear_logs_password'),password=input.value;
  if(!password){alert('未輸入密碼，刷卡紀錄未刪除。');return}
  const button=element('confirm_clear_logs_button');button.disabled=true;
  try{
    const body=new URLSearchParams({password});
    await json('/attendance/api/clear-logs',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    closeClearLogsDialog();alert('所有刷卡紀錄已成功刪除。');await load();
  }catch(error){input.value='';alert(error.message);input.focus()}finally{button.disabled=false}
}
async function readAllBackupLogs(progress){
  const logs=[];let offset=0,page=0;
  while(true){
    progress.textContent=`正在讀取第 ${++page} 批刷卡紀錄……`;
    const batch=await json('/attendance/api/export-page?offset='+offset);
    logs.push(...batch.logs);
    if(batch.done)break;
    if(batch.next<=offset)throw Error('刷卡紀錄分頁資料異常');
    offset=batch.next;
  }
  return logs;
}
async function exportBackup(){
  const button=element('export_backup_button'),progress=element('backup_status');button.disabled=true;
  try{
    progress.textContent='正在讀取員工與每日備註……';
    const employees=await json('/attendance/api/employees'),notes=await readAllNotes(true,progress);
    const logs=await readAllBackupLogs(progress);logs.sort((a,b)=>Number(a.timestamp)-Number(b.timestamp));
    const backup={format:'nfc-attendance-clock-backup',version:1,exported_at:new Date().toISOString(),employees:employees.map(e=>({slot:Number(e.slot),code:String(e.id??''),name:String(e.name??''),uid:String(e.uid??'')})),notes:notes.map(n=>({date:String(n.date??''),slot:Number(n.slot),note:String(n.note??'')})),logs:logs.map(l=>({timestamp:Number(l.timestamp),employee_id:Number(l.employee_id),action:Number(l.action_code),uid_hash:Number(l.uid_hash)}))};
    const blob=new Blob([JSON.stringify(backup)],{type:'application/json;charset=utf-8'}),url=URL.createObjectURL(blob),now=new Date(),stamp=`${now.getFullYear()}-${String(now.getMonth()+1).padStart(2,'0')}-${String(now.getDate()).padStart(2,'0')}`,link=document.createElement('a');
    link.href=url;link.download=`nfc-attendance-backup-${stamp}.json`;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);
    progress.textContent=`備份完成：${backup.employees.length} 位員工、${backup.notes.length} 筆備註、${backup.logs.length} 筆刷卡紀錄。請妥善保存 JSON 檔。`;
  }catch(error){progress.textContent='備份失敗：'+error.message;alert(error.message)}finally{button.disabled=false}
}
const postForm=(url,values)=>json(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:new URLSearchParams(values)});
async function importBackup(){
  const fileInput=element('backup_file'),passwordInput=element('backup_password'),button=element('import_backup_button'),progress=element('backup_status'),file=fileInput.files[0],password=passwordInput.value;
  if(!file){alert('請先選擇先前匯出的 JSON 備份檔');fileInput.focus();return}
  if(file.size>10*1024*1024){alert('備份檔超過 10 MB，請確認是否選錯檔案');return}
  if(!password){alert('請輸入目前管理密碼');passwordInput.focus();return}
  let backup;
  try{backup=JSON.parse(await file.text())}catch(error){alert('備份檔不是有效的 JSON 格式');return}
  if(!backup||backup.format!=='nfc-attendance-clock-backup'||backup.version!==1||!Array.isArray(backup.employees)||!Array.isArray(backup.notes)||!Array.isArray(backup.logs)){alert('備份檔格式或版本不相容');return}
  const encoder=new TextEncoder(),employeeSlots=new Set(),employeeCodes=new Set(),cardUids=new Set(),noteKeys=new Set();let contentValid=true;
  for(const employee of backup.employees){
    const slot=Number(employee?.slot),code=String(employee?.code??'').trim(),name=String(employee?.name??''),uid=String(employee?.uid??''),codeKey=code.toUpperCase();
    if(!Number.isInteger(slot)||slot<1||slot>100||!code||encoder.encode(code).length>16||!name||encoder.encode(name).length>40||encoder.encode(uid).length>24||employeeSlots.has(slot)||employeeCodes.has(codeKey)||(uid&&cardUids.has(uid))){contentValid=false;break}
    employeeSlots.add(slot);employeeCodes.add(codeKey);if(uid)cardUids.add(uid);
  }
  for(const note of backup.notes){
    const slot=Number(note?.slot),date=String(note?.date??''),text=String(note?.note??''),key=slot+'|'+date,parsed=new Date(date+'T12:00:00');
    if(!Number.isInteger(slot)||slot<1||slot>100||!/^[0-9]{4}-[0-9]{2}-[0-9]{2}$/.test(date)||Number.isNaN(parsed.getTime())||parsed.toISOString().slice(0,10)!==date||!text||[...text].length>15||encoder.encode(text).length>48||noteKeys.has(key)){contentValid=false;break}
    noteKeys.add(key);
  }
  for(const log of backup.logs){
    const timestamp=Number(log?.timestamp),employeeId=Number(log?.employee_id),action=Number(log?.action),uidHash=Number(log?.uid_hash);
    if(!Number.isInteger(timestamp)||timestamp<1577836800||timestamp>4102444799||!Number.isInteger(employeeId)||employeeId<1||employeeId>100||!Number.isInteger(action)||action<0||action>3||!Number.isInteger(uidHash)||uidHash<0||uidHash>4294967295){contentValid=false;break}
  }
  if(!contentValid){alert('備份內容損壞或含有重複工號、卡號、槽位或備註，尚未變更裝置資料');return}
  let status;
  try{status=await json('/attendance/api/status')}catch(error){alert('無法讀取裝置容量：'+error.message);return}
  if(backup.employees.length>100||backup.notes.length>Number(status.note_capacity||500)||backup.logs.length>Number(status.log_capacity||0)){alert('備份內容超過這台打卡鐘的儲存容量');return}
  if(!confirm(`確定匯入 ${backup.employees.length} 位員工、${backup.notes.length} 筆備註及 ${backup.logs.length} 筆刷卡紀錄？\n\n裝置內現有資料會先被永久清除。`))return;
  button.disabled=true;element('export_backup_button').disabled=true;let token=0;
  try{
    progress.textContent='正在核實密碼並清除現有資料……';
    const begin=await postForm('/attendance/api/import-begin',{password});token=Number(begin.token);passwordInput.value='';
    for(let index=0;index<backup.employees.length;index++){
      const employee=backup.employees[index];progress.textContent=`正在還原員工 ${index+1}/${backup.employees.length}……`;
      await postForm('/attendance/api/import-employee',{token:String(token),slot:String(employee.slot),code:String(employee.code??''),name:String(employee.name??''),uid:String(employee.uid??'')});
    }
    for(let index=0;index<backup.notes.length;index++){
      const note=backup.notes[index];progress.textContent=`正在還原備註 ${index+1}/${backup.notes.length}……`;
      await postForm('/attendance/api/import-note',{token:String(token),date:String(note.date??''),slot:String(note.slot),note:String(note.note??'')});
    }
    const logs=[...backup.logs].sort((a,b)=>Number(a.timestamp)-Number(b.timestamp)),batchSize=50;
    for(let offset=0;offset<logs.length;offset+=batchSize){
      const batch=logs.slice(offset,offset+batchSize),records=batch.map(log=>`${Number(log.timestamp)},${Number(log.employee_id)},${Number(log.action)},${Number(log.uid_hash)}`).join(';');
      progress.textContent=`正在還原刷卡紀錄 ${Math.min(offset+batch.length,logs.length)}/${logs.length}……`;
      await postForm('/attendance/api/import-logs',{token:String(token),records});
    }
    progress.textContent='正在提交員工與備註資料……';
    const result=await postForm('/attendance/api/import-finish',{token:String(token)});token=0;
    fileInput.value='';passwordInput.value='';progress.textContent=`還原完成：${result.employees} 位員工、${result.notes} 筆備註、${result.logs} 筆刷卡紀錄。`;alert('資料備份已成功還原。');await load();
  }catch(error){
    if(token){try{await postForm('/attendance/api/import-abort',{token:String(token)})}catch(ignore){}}
    passwordInput.value='';progress.textContent='還原失敗：'+error.message+'。請保留備份檔並重新匯入。';alert('還原失敗：'+error.message);
  }finally{button.disabled=false;element('export_backup_button').disabled=false}
}
async function changeAdminPassword(){
  const currentInput=element('current_admin_password'),newInput=element('new_admin_password'),confirmInput=element('confirm_admin_password');
  const current_password=currentInput.value,new_password=newInput.value,confirm_password=confirmInput.value;
  if(!current_password){alert('請輸入目前管理密碼');currentInput.focus();return}
  if(new_password.length<8||new_password.length>64||!/^[!-~]+$/.test(new_password)){alert('新密碼必須為 8～64 個半形英文字母、數字或符號，且不可包含空白');newInput.focus();return}
  if(new_password!==confirm_password){alert('兩次輸入的新密碼不一致');confirmInput.focus();return}
  if(!confirm('確定同步修改管理頁登入密碼及所有敏感操作的核實密碼？'))return;
  const button=element('change_admin_password_button');button.disabled=true;
  try{
    const body=new URLSearchParams({current_password,new_password,confirm_password});
    await json('/attendance/api/admin-password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    currentInput.value='';newInput.value='';confirmInput.value='';
    alert('管理員密碼已同步修改並保存。接下來請使用帳號 admin 與新密碼重新登入。');
    location.replace('/attendance/');
  }catch(error){currentInput.value='';newInput.value='';confirmInput.value='';alert(error.message);currentInput.focus();button.disabled=false}
}
showTab(location.hash.slice(1)||'dashboard');setInterval(load,5000);
</script></html>
)HTML";
  auto *response = r->beginResponse(
      200,
      "text/html; charset=utf-8",
      reinterpret_cast<const uint8_t *>(PAGE),
      sizeof(PAGE) - 1);
  response->addHeader("Cache-Control", "no-store");
  r->send(response);
}
}  // namespace esphome::attendance_web
