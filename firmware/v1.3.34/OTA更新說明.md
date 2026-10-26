# v1.3.34 OTA 更新說明

## 從 v1.3.31～v1.3.33 升級

v1.3.34 沿用 v1.3.31～v1.3.33 的 Flash 分割區配置，因此可直接透過 ESPHome OTA 升級，不必再次以 USB 寫入分割表。

1. 先從管理頁匯出完整 JSON 備份。
2. 將本更新包內的 `components/attendance_web`、`nfc-attendance-clock.yaml`、`attendance_partitions.csv` 更新到原 ESPHome 設定目錄。
3. 保留原本的 `secrets.yaml`，不要使用 `secrets.example.yaml` 覆蓋。
4. 在 ESPHome 執行「Install → Wirelessly」；或編譯後上傳 `firmware.ota.bin`。
5. 更新後開啟管理頁，在「系統設定 → 自動模式切換」設定上班、下班、加班上班與加班下班排程。

原本 v1.3.32 的總開關、上班時間與下班時間會自動移轉。加班上班與加班下班排程預設關閉，建議時間為 `18:00` 與 `22:00`；只有在網頁勾選並保存後才會自動切換。

## Wi-Fi 是否需要重新設定

OTA 本身不會清除 NVS，所以透過配網 AP 保存的 Wi-Fi、網頁修改後的管理員密碼、員工、卡片、備註及刷卡紀錄都會保留。

若 Wi-Fi 只寫在 YAML／`secrets.yaml` 中，則 OTA 韌體也必須使用原本相同的 Wi-Fi 設定編譯，才可保證更新後繼續連線。請勿使用含有其他 SSID 或密碼的通用 BIN 進行 OTA。

以下操作會清除 Wi-Fi，之後需要重新配網：

- USB 刷入時選擇清除整顆 Flash。
- 網頁安裝器執行完整清除後再刷入 factory BIN。
- NVS 因故被刪除或重建。

## BIN 類型

- `firmware.ota.bin`：用於已在運作中的打卡鐘 OTA 升級。
- `firmware.factory.bin`：用於 USB／WebSerial 首次完整刷入，不可當作 OTA 檔案上傳。

為避免把別人的 Wi-Fi 或管理員密碼編入韌體，本更新包提供完整可編譯原始檔，不附帶使用測試密碼建立的通用 OTA BIN。請在原本 ESPHome 環境中編譯，即可沿用原有 `secrets.yaml`。

更新後到「刷卡紀錄 → 管理員補打卡」操作。先確認裝置時間正確，再選員工、時間與打卡類型。保存會新增紀錄並納入 Excel 工時。請勿降回 v1.3.33 或更早版本，以免舊版使用打卡時間判斷寫入位置，覆寫補登後的紀錄。
