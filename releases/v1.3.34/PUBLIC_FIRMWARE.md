# v1.3.34 公開 Factory 韌體

[下載 nfc-attendance-clock-firmware.factory_1334.bin](../../nfc-attendance-clock-firmware.factory_1334.bin?raw=true)

適用本專案 ESP32-C3、4MB Flash、CH340 USB、RC522、DS3231 及四顆打卡模式按鍵。此為 USB 完整安裝映像，含開機程式、分割表與應用程式；不能當成 OTA 更新檔。

## 首次安裝

1. 用可傳資料的 USB 線接上開發板的 CH340 USB 埠。
2. 使用 Chrome／Edge 開啟 [ESPHome Web](https://web.esphome.io/)。
3. 按 `Connect` 選擇對應的序列埠，再按 `Install` 選擇本版 Factory BIN。
4. 等待完成及重新啟動，連上 `NFC-Clock-Fallback` 熱點，密碼 `change-me-1234`。
5. 開啟 `http://192.168.4.1/`，設定自己的 2.4 GHz Wi-Fi。韌體沒有內建任何私人 Wi-Fi 名稱或密碼。
6. 手機或電腦回到相同區域網路，開啟 `http://nfc-attendance-clock.local/attendance/`，或 `http://裝置IP/attendance/`。
7. 帳號 `admin`，初始密碼 `change-me-1334`；首次登入後在管理頁改成自己的管理員密碼。

Factory 完整安裝可能重設配網及初始密碼；採保留 NVS 的更新方式才會沿用已保存的管理員密碼。更換 Wi-Fi 或無法連線時，可透過備用熱點重新配網。ESPHome 的使用者配網機制說明見 [官方 Wi-Fi 文件](https://esphome.io/components/wifi/#user-entered-credentials)。

## 已有資料的裝置

先在管理頁匯出完整 JSON 備份。公開 Factory BIN 用於 USB 完整安裝，可能影響資料、配網及初始密碼；完成後請確認 Wi-Fi、裝置時間與出勤資料，必要時匯入 JSON 備份。

需要無線 OTA 時，先確認裝置分割表與本版一致，並在自己的 ESPHome 環境編譯 OTA 韌體。分割表不同或無法確認時，透過 USB 寫入本版分割表。完整步驟見 [ESPHome 燒錄教學](../../docs/ESPHOME_FLASHING.md)。

## 原始碼及重建

公開版使用 [nfc-attendance-clock-public.yaml](../../firmware/v1.3.34/nfc-attendance-clock-public.yaml)。此設定透過熱點配網、使用公開的初始管理員密碼，並包含 v1.3.34 專案版本資訊；搭配同資料夾的三個 `attendance_web` 元件檔及分割表編譯。

複製公開 YAML、同層的 `attendance_partitions.csv` 與 `components/attendance_web/` 資料夾即可重建，不需 `secrets.yaml`。在安裝好 ESPHome 2026.8.1 的環境、從倉庫根目錄執行：

```text
esphome compile firmware/v1.3.34/nfc-attendance-clock-public.yaml
```

若要預先填入自己的 Wi-Fi 及密碼，使用 `nfc-attendance-clock.yaml` 與自己的 `secrets.yaml`，不要把個人設定上傳至公開倉庫。原始碼 ZIP 包含自行設定用的 YAML、密碼範例、分割表、元件與 v1.3.34 使用說明；公開 YAML 可從倉庫另行取得。

## 建置與驗證範圍

- 編譯環境：ESPHome 2026.8.1、ESP-IDF 5.5.5、ESP32-C3。
- 已檢查公開設定、完整編譯、Factory 內的分割表及應用程式映像，並核對 v1.3.34 元件原始檔。
- 映像大小及 SHA-256 見 [build-info.json](./build-info.json) 與 [SHA256SUMS.txt](./SHA256SUMS.txt)。校驗碼中的路徑以倉庫根目錄為基準。
- 尚未在實體打卡鐘完成配網、刷卡、斷電及燒錄驗證。
