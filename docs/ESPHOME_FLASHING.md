# 四按鍵 NFC 打卡鐘 v1.3.34：ESPHome 複製檔案與燒錄教學

適用於本專案的 ESP32-C3／4MB Flash／CH340 USB 開發板、RC522、DS3231 及四顆模式按鍵。其他板型請先核對接腳及 USB 方式；本 YAML 將 GPIO18／19 用於 DS3231，不能同時使用這兩腳的原生 USB Serial/JTAG。

本版同時提供 **可直接 USB 燒錄的公開 Factory BIN** 與可自行編譯的原始碼。需要修改接腳、Wi-Fi 或初始密碼時，使用下方第 1～4 節的原始碼流程；使用現成 BIN 則依以下快速步驟。

## 現成 BIN：不用複製原始檔，也不用先編譯

1. 下載 [v1.3.34 Factory BIN](../nfc-attendance-clock-firmware.factory_1334.bin?raw=true)。
2. 新空白裝置用資料 USB 線接上電腦，在 Chrome／Edge 開啟 [ESPHome Web](https://web.esphome.io/)。
3. 按 `Connect` 選擇 CH340／USB Serial 埠，再按 `Install` 選擇下載的 BIN。
4. 安裝後連上 `NFC-Clock-Fallback` 熱點，密碼 `change-me-1234`；開啟 `http://192.168.4.1/` 設定自己的 2.4 GHz Wi-Fi。
5. 電腦或手機回到相同區域網路，開啟 `http://nfc-attendance-clock.local/attendance/`（或裝置 IP 加 `/attendance/`），帳號 `admin`，初始密碼 `change-me-1334`。登入後在管理頁修改密碼。

公開 BIN 不內建私人 Wi-Fi 資料，也不需要使用者提供 `secrets.yaml`。已有員工及刷卡資料的裝置請先匯出 JSON 備份，依第 7 節升級；Factory 完整安裝可能重設配網及初始密碼；採保留 NVS 的更新方式才會沿用已保存的管理員密碼。安裝後請確認 Wi-Fi 連線，必要時透過熱點重新配網。**這個 Factory BIN 只能用於 USB 完整安裝，不能作為 OTA 映像上傳。**

公開 BIN 的來源設定與檢查結果見 [公開韌體說明](../releases/v1.3.34/PUBLIC_FIRMWARE.md)。以下章節保留自行編譯及既有裝置升級流程。

## 1. 下載原始碼，準備 ESPHome

下載 [v1.3.34 原始碼 ZIP](../releases/v1.3.34/nfc-attendance-clock-esphome-v1.3.34.zip?raw=true)，解壓後打開 `nfc-attendance-clock` 資料夾。也可在 GitHub 按 `Code → Download ZIP`，從整個倉庫的 `firmware/v1.3.34/` 取得相同內容。

使用已安裝的 **ESPHome Device Builder**。Home Assistant 使用者可從應用程式／附加元件商店安裝並啟動 ESPHome Device Builder；沒有 Home Assistant 也能使用獨立的 ESPHome 環境。安裝方式請參考 [ESPHome 官方安裝說明](https://esphome.io/install/)。打卡鐘燒錄完成後可獨立運作。

原始碼及本次公開 BIN 均使用 **ESPHome 2026.8.1、ESP-IDF 5.5.5** 編譯。公開 BIN 已完成編譯與映像檢查，尚未實機燒錄；不代表每個後續 ESPHome 版本都已驗證。

## 2. 到底要複製哪些檔案？

先找到 **ESPHome 放裝置 YAML 的設定資料夾**。Home Assistant 通常是 `/config/esphome/`，可以透過 File editor、Studio Code Server 或既有的檔案分享工具進入。不同容器或獨立安裝的路徑可能不同，請以目前裝置 YAML 所在位置為準。

從解壓後的 `nfc-attendance-clock/` 複製以下 **5 個程式檔**，保留資料夾層級：

| 來源檔案 | 放到 ESPHome 的位置 | 用途 |
| --- | --- | --- |
| `nfc-attendance-clock.yaml` | 設定資料夾根目錄 | 裝置、接腳、Wi-Fi 與元件設定 |
| `attendance_partitions.csv` | 與 YAML 同一層 | 4MB Flash 程式及出勤資料分割表 |
| `components/attendance_web/__init__.py` | `components/attendance_web/` | ESPHome 自訂元件設定入口 |
| `components/attendance_web/attendance_web.h` | `components/attendance_web/` | 自訂元件標頭 |
| `components/attendance_web/attendance_web.cpp` | `components/attendance_web/` | 打卡、管理頁、資料保存等功能 |

最方便的做法是一次複製 **YAML、CSV，以及整個 `components/attendance_web` 資料夾**。若原本已有 `components` 資料夾，把 `attendance_web` 放進去即可，保留其他裝置的元件。

再依下一節建立或沿用 `secrets.yaml`，最後應有以下 **6 個必要檔案**：

```text
/config/esphome/                       ← 範例設定資料夾
├── nfc-attendance-clock.yaml
├── attendance_partitions.csv
├── secrets.yaml
└── components/
    └── attendance_web/
        ├── __init__.py
        ├── attendance_web.h
        └── attendance_web.cpp
```

`README.md`、`OTA更新說明.md`、`驗證說明.md` 是閱讀用文件，不需要放入 ESPHome 才能編譯。ZIP 也不直接傳進打卡鐘。

**只貼 YAML 或只匯入 YAML 並不足夠**，CSV 和三個元件檔仍須放好。不要多包一層 `nfc-attendance-clock`，導致 YAML 與 `components` 分在不同層。ESPHome 的本機元件路徑是從 YAML 所在資料夾開始尋找，詳見 [External Components 官方說明](https://esphome.io/components/external_components/#local)。

## 3. 設定 `secrets.yaml`

第一次使用且設定資料夾沒有 `secrets.yaml`：把本包的 `secrets.example.yaml` 複製一份，改名為 `secrets.yaml`，再填入自己的資料。

已經有 `secrets.yaml`：**保留原檔，只補齊缺少的項目**。安裝時沿用自己的 Wi-Fi 與密碼；若這份檔案還供其他裝置使用，不要把它整份覆蓋。

```yaml
wifi_ssid: "YOUR_2_4_GHZ_WIFI_NAME"
wifi_password: "YOUR_WIFI_PASSWORD"
attendance_web_password: "YOUR_OWN_ADMIN_PASSWORD"
```

- `wifi_ssid`、`wifi_password`：打卡鐘要連線的 2.4 GHz Wi-Fi 名稱與密碼。
- `attendance_web_password`：管理頁初始登入密碼，帳號固定為 `admin`。請設為自己的密碼；管理頁修改密碼時要求 8～64 個半形字元且不可有空白。
- 使用半形引號，保留 YAML 縮排，確認檔名不是 `secrets.yaml.txt`。`secrets.yaml` 放在裝置 YAML 同層，參見 [ESPHome secrets 官方說明](https://esphome.io/guides/faq/#tips-for-using-esphome)。
- 網頁曾修改並保存的管理員密碼會保留在裝置內；一般 OTA 後仍使用該密碼，並非改回檔案中的初始值。

本包實際必填的 secret 只有上述三項。YAML 中備用熱點 `NFC-Clock-Fallback` 的範例密碼為 `change-me-1234`，可在 `wifi.ap.password` 改成自己的值。請把個人 `secrets.yaml` 留在自己的 ESPHome 環境，倉庫只提供範例檔。

## 4. 確認設定並編譯

回到 ESPHome Device Builder，重新整理後找到 `nfc-attendance-clock.yaml` 對應的裝置卡片。複製檔案到正確的設定資料夾後，不需要再建立另一份精簡設定；若曾用新增裝置精靈建立同名 YAML，請以本專案完整 YAML 取代其內容。

按 `Edit` 檢查設定。請保留原檔中的下列內容（這只是節錄，不能拿來取代整份 YAML）：

```yaml
esp32:
  board: esp32-c3-devkitm-1
  variant: esp32c3
  partitions: attendance_partitions.csv
  framework:
    type: esp-idf

external_components:
  - source: {type: local, path: components}
    components: [attendance_web]
```

從卡片選單執行 `Validate`／驗證設定。通過後再按 `Install` 安裝並編譯；第一次編譯需要下載工具及套件，時間可能較長。只有看到編譯成功才繼續燒錄。Device Builder 的基本操作可參考 [ESPHome 入門說明](https://esphome.io/install/getting-started/)。

## 5. 新裝置第一次使用 USB 燒錄

以下步驟適用於**沒有需要保留的出勤資料的新裝置**。已有資料的裝置先看第 7 節升級方式。

1. 在裝置卡片按 `Install`，選擇手動下載（常見名稱為 `Manual download`；不同 ESPHome 版本的名稱可能略有差異）。
2. 等待編譯成功後，選擇 **Factory format／Factory image**（舊介面可能叫 `Modern format`），下載合併韌體，檔名通常是 `nfc-attendance-clock.factory.bin` 或 `firmware.factory.bin`。
3. 用可傳資料的 USB 線，把開發板的 **CH340 USB 埠**接到電腦。單純供電線無法燒錄。
4. 在電腦使用支援 Web Serial 的 Chrome 或 Edge 開啟 [ESPHome Web](https://web.esphome.io/)，按 `Connect`，選擇開發板對應的 CH340／USB Serial 埠（Windows 會顯示 COM 編號）。
5. 連線後按 `Install`，選剛才下載的 **`.factory.bin`**，依畫面完成安裝，等待裝置重新啟動。
6. 若畫面提供清除裝置選項，新空白裝置可以採用首次安裝流程；已有出勤資料的裝置不要套用清除整顆 Flash 的步驟。

ESPHome Web 負責把編譯好的 BIN 寫入裝置，**不能直接拿 ZIP、YAML 或 `.cpp` 去燒錄**。若 Device Builder 本身已有 `Plug into this computer` 選項且瀏覽器允許連接，也可從該入口完成 USB 安裝。USB 接線及驅動問題可參考 [ESPHome 官方實體連線說明](https://esphome.io/guides/physical_device_connection/)。

## 6. 開機後確認

1. 等待打卡鐘連上設定的 Wi-Fi，手機或電腦也接到同一個區域網路。
2. 開啟 `http://nfc-attendance-clock.local/attendance/`；若名稱無法解析，改用 `http://裝置IP/attendance/`。IP 可從路由器或 ESPHome 日誌查看。
3. 帳號輸入 `admin`，密碼使用自己的管理員密碼。
4. 確認裝置時間正確、四顆按鍵可切換模式，登記一張卡片測試刷卡及紀錄。
5. 在「刷卡紀錄」確認能看到「管理員補打卡」。需要補登時選擇員工、日期時間與打卡類型，再核對管理員密碼；只接受目前往前兩個日曆月內且不是未來的時間。

若 Wi-Fi 連線失敗，約一分鐘後可連上備用熱點 `NFC-Clock-Fallback`（密碼以自己的 YAML 設定為準），再開啟 `http://192.168.4.1/` 重新配網。

## 7. 已有打卡鐘，如何升級？

安裝 v1.3.34 前，先在管理頁「資料備份與還原」下載**完整 JSON 備份**，並保存自己的 ESPHome 設定及 `secrets.yaml`。Excel 適合查閱，JSON 才是本專案的還原檔案。

| 裝置狀況 | 安裝方式 |
| --- | --- |
| 分割表與本版 `attendance_partitions.csv` 一致，且 ESPHome 可進行 OTA | 自行編譯後使用無線 OTA |
| 分割表不同、無法確認，或沒有可用的 OTA | USB 寫入本版分割表及韌體 |
| 新空白裝置 | USB 首次安裝 Factory 韌體 |

OTA 不會更新 Flash 分割表。無法確認裝置分割配置時，先備份，再依 USB 安裝流程處理。

### 無線 OTA 安裝 v1.3.34

1. 將第 2 節的 5 個程式檔放入 ESPHome 設定資料夾，保留自己的 `secrets.yaml`。若曾調整裝置名稱、接腳或連線設定，也需沿用適合自己裝置的設定。
2. 確認裝置在線，ESPHome 能連到它的 IP；驗證並編譯成功後選 `Install → Wirelessly`。
3. 等待上傳及重新啟動，重新開啟管理頁，核對員工、刷卡紀錄、備註、時間與自動模式排程。

這條路徑會使用 OTA 韌體更新程式。若另行下載檔案，**`.ota.bin` 才是 OTA 映像，`.factory.bin` 不可當成 OTA 映像上傳**；本教學優先使用 Device Builder 的無線安裝入口。

### USB 安裝 v1.3.34 分割表與韌體

完成 JSON 備份後，放好本版程式檔並保留自己的設定。將開發板以 USB 接到執行 ESPHome 的主機，在 `Install` 選擇該主機的 USB／序列埠安裝方式，讓 ESPHome 寫入本版分割表與韌體。若在電腦上使用 ESPHome 命令列，也可在設定資料夾執行：

```text
esphome run nfc-attendance-clock.yaml --device COM5
```

`COM5` 只是 Windows 範例，請換成實際 USB 埠；Linux 可能是 `/dev/ttyUSB0`。這個命令會驗證、編譯並上傳，使用方法見 [ESPHome 官方命令列說明](https://esphome.io/guides/cli/)。

已有資料的裝置，不要任意選擇「清除整顆 Flash」。分割表變動或 Factory 完整安裝仍可能影響資料與配網，請以 JSON 備份作為復原依據。安裝後確認時間、Wi-Fi、管理員密碼與出勤資料；需要還原時，從管理頁匯入 JSON 備份，匯入會取代裝置目前的員工、備註及紀錄。

更多設定與資料保留條件見 [v1.3.34 OTA 安裝說明](../firmware/v1.3.34/OTA更新說明.md)。

## 8. 常見問題

| 問題 | 檢查方式 |
| --- | --- |
| 找不到 `attendance_web` 元件 | 確認三個檔案在 YAML 同層的 `components/attendance_web/`，並保留 `__init__.py` 檔名的雙底線。 |
| 找不到 `attendance_partitions.csv` | CSV 必須與 YAML 同層；確認沒有副檔名變成 `.csv.txt`。 |
| 顯示找不到 secret | 確認 `secrets.yaml` 同層且三個鍵名拼字正確；範例檔名不會自動代替正式檔名。 |
| ESPHome 看不到裝置卡片 | 確認檔案真的放在正在執行的 Device Builder 設定資料夾，並重新整理。 |
| 瀏覽器選不到序列埠 | 更換可傳資料的 USB 線、核對 CH340 驅動，關閉占用序列埠的日誌或其他程式。 |
| 裝置無法正常開機 | 核對 3.3V 接線及板型；GPIO2 是啟動取樣腳位，不要在通電或重啟時按住加班上班鍵。 |
| OTA 後無法連上 Wi-Fi | 確認新韌體沿用自己的 Wi-Fi 設定，並檢查備用熱點；不要使用他人密碼編譯的 BIN。 |
| 更新後登入密碼與檔案不同 | 若曾在網頁改過密碼，裝置會沿用已保存的密碼。 |
| 編譯器升級後出錯 | 先核對檔案是否完整及 YAML 驗證結果，再對照本版使用的 ESPHome 2026.8.1 編譯環境；保留錯誤紀錄以便排查。 |

[返回專案首頁](../README.md) · [v1.3.34 完整使用及接線說明](../firmware/v1.3.34/README.md)
