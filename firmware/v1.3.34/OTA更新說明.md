# v1.3.34 OTA 安裝說明

## 安裝前確認

先從管理頁匯出完整 JSON 備份，保存自己的 ESPHome 設定與 `secrets.yaml`。只有裝置目前分割表與本包 `attendance_partitions.csv` 一致，且 ESPHome 能透過網路連線進行 OTA 時，才使用以下無線安裝流程。

OTA 不會更新 Flash 分割表。分割配置不同、無法確認或無法進行 OTA 時，改用 USB 寫入 v1.3.34 的分割表及韌體。Factory 完整安裝可能重設資料、配網與初始密碼，完成後請核對資料，必要時匯入 JSON 備份。

## ESPHome 無線安裝

1. 將 `nfc-attendance-clock.yaml`、`attendance_partitions.csv` 和 `components/attendance_web/` 內的三個檔案放到 ESPHome 設定資料夾，維持相對路徑。
2. 保留自己的 `secrets.yaml`，不要用 `secrets.example.yaml` 覆蓋；核對裝置名稱、接腳及 Wi-Fi 設定。
3. 在 ESPHome 驗證設定並編譯成功後，執行 `Install → Wirelessly`。
4. 等待裝置重新啟動，開啟 `http://nfc-attendance-clock.local/attendance/`，或使用裝置 IP 加 `/attendance/`。
5. 核對員工、卡片、刷卡紀錄、備註、裝置時間與「系統設定 → 自動模式切換」排程。
6. 在「刷卡紀錄 → 管理員補打卡」選擇員工、時間與打卡類型，輸入管理員密碼後即可補登。僅接受目前往前兩個日曆月內且不晚於裝置時間的紀錄。

## Wi-Fi、密碼與資料

採保留 NVS 及出勤資料分割區的 OTA 更新方式，會沿用已保存的配網、管理員密碼與出勤資料。若 Wi-Fi 由 YAML／`secrets.yaml` 提供，編譯時也必須沿用自己的 Wi-Fi 設定。

USB 清除整顆 Flash、Factory 完整安裝或 NVS 被重建，都可能需要重新配網並使用初始密碼登入。請以事先匯出的 JSON 備份作為資料復原依據；匯入會取代裝置目前的員工、備註及刷卡紀錄，過程中請保持供電。

## BIN 類型

- `firmware.ota.bin`：ESPHome 編譯產生的 OTA 映像，適用於符合上述條件的無線安裝。
- `firmware.factory.bin`：USB 完整安裝映像，不能當作 OTA 檔案上傳。
- 專案提供的 `nfc-attendance-clock-firmware.factory_1334.bin` 為 v1.3.34 公開 Factory 映像，透過熱點配網，不內建私人 Wi-Fi 資料；它也不能用於 OTA。

本包提供可自行編譯的原始檔。需要 OTA 時，請在自己的 ESPHome 環境使用自己的設定編譯。詳細檔案清單見[使用說明](./README.md#安裝)，USB 與 OTA 完整操作見 [ESPHome 燒錄教學](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/docs/ESPHOME_FLASHING.md)。
