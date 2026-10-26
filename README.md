# 四按鍵 NFC 打卡鐘 v1.3.34｜ESP32-C3／RC522／ESPHome

## v1.3.34 下載與安裝

本版提供「管理員補打卡」，可補登上班、下班、加班上班與加班下班，並納入 Excel 工時與 JSON 備份。

- [下載 v1.3.34 Factory BIN](./nfc-attendance-clock-firmware.factory_1334.bin?raw=true)
- [下載 v1.3.34 原始碼 ZIP](./releases/v1.3.34/nfc-attendance-clock-esphome-v1.3.34.zip?raw=true)
- [瀏覽 v1.3.34 完整原始檔](./firmware/v1.3.34/)
- [ESPHome 燒錄教學：檔案放哪裡、首次 USB、後續 OTA](./docs/ESPHOME_FLASHING.md)
- [v1.3.34 功能摘要](./CHANGELOG.md) · [v1.3.34 OTA 安裝說明](./firmware/v1.3.34/OTA更新說明.md) · [v1.3.34 驗證紀錄](./firmware/v1.3.34/驗證說明.md)

v1.3.34 提供可直接 USB 燒錄的 **公開 Factory BIN**，以及可自行編譯的完整原始碼。

### 直接下載 BIN 燒錄

**[下載 v1.3.34 Factory BIN](./nfc-attendance-clock-firmware.factory_1334.bin?raw=true)** · [公開韌體使用說明與編譯紀錄](./releases/v1.3.34/PUBLIC_FIRMWARE.md)

新裝置可用 Chrome／Edge 開啟 [ESPHome Web](https://web.esphome.io/)，接上 CH340 USB，按 `Connect → Install`，選擇下載的 `nfc-attendance-clock-firmware.factory_1334.bin` 完成燒錄。這是 USB Factory 映像，不能當成 OTA 映像上傳。

| 首次設定 | 公開 BIN 的預設值 |
| --- | --- |
| 配網熱點 | `NFC-Clock-Fallback` |
| 熱點密碼 | `change-me-1234` |
| 配網頁 | `http://192.168.4.1/` |
| 管理頁帳號 | `admin` |
| 初始管理員密碼 | `change-me-1334` |

此 BIN 不內建私人 Wi-Fi 名稱或密碼。開機後連上熱點，在配網頁選擇自己的 2.4 GHz Wi-Fi；再回到同一區域網路開啟 `http://nfc-attendance-clock.local/attendance/`。**首次登入後請在管理頁修改管理員密碼。** Factory 完整安裝可能重設配網及初始密碼；只有採保留 NVS 的更新方式，才會沿用已保存的管理員密碼。

已有出勤資料的裝置，先匯出完整 JSON 備份，再依[升級教學](./docs/ESPHOME_FLASHING.md#7-已有打卡鐘如何升級)處理分割表及資料；Factory 完整安裝後可能需要重新配網。

### 透過 ESPHome 燒錄，要複製哪些檔案？

將 ZIP 解壓後 `nfc-attendance-clock/` 裡的以下 **5 個程式檔**複製到 ESPHome 設定資料夾；若下載整個倉庫，來源是 `firmware/v1.3.34/`。

| 必要程式檔 | 放置位置（相對於 ESPHome 設定資料夾） |
| --- | --- |
| `nfc-attendance-clock.yaml` | `nfc-attendance-clock.yaml` |
| `attendance_partitions.csv` | `attendance_partitions.csv` |
| `components/attendance_web/__init__.py` | `components/attendance_web/__init__.py` |
| `components/attendance_web/attendance_web.h` | `components/attendance_web/attendance_web.h` |
| `components/attendance_web/attendance_web.cpp` | `components/attendance_web/attendance_web.cpp` |

另外，首次使用時將 `secrets.example.yaml` 複製為同層的 **`secrets.yaml`**，填入自己的 `wifi_ssid`、`wifi_password`、`attendance_web_password`。已經有 `secrets.yaml` 的人保留原檔，只補齊缺少的項目。

```text
ESPHome 設定資料夾/（Home Assistant 通常是 /config/esphome/）
├── nfc-attendance-clock.yaml
├── attendance_partitions.csv
├── secrets.yaml
└── components/
    └── attendance_web/
        ├── __init__.py
        ├── attendance_web.h
        └── attendance_web.cpp
```

新裝置：複製檔案 → 填寫密碼 → 在 ESPHome 驗證、編譯 → 下載 Factory 格式韌體 → 用 [ESPHome Web](https://web.esphome.io/) 透過 USB 燒錄。詳細操作與畫面選項請看[完整燒錄教學](./docs/ESPHOME_FLASHING.md)。

已有資料的裝置，安裝前先匯出完整 JSON 備份。確認裝置分割表與本版 `attendance_partitions.csv` 一致，且可使用 ESPHome OTA 時，可透過 `Install → Wirelessly` 安裝；分割表不同或無法確認時，使用 USB 寫入本版分割表與韌體。詳細步驟見[燒錄教學](./docs/ESPHOME_FLASHING.md#7-已有打卡鐘如何升級)。

---

公司的傳統插紙卡的打卡鐘最近壞了，有在考慮買台新的，看了一下市面上現在的打卡鐘管理方式、資料格式與功能通常也不容易自行調整。用Line打卡的想法也有想過，但會有些員工不太願意分享手機定位權限的問題，因此我決定嘗試使用 ESP32 的模塊、RC522讀卡模塊 搭配 ESPHome 編輯軟體，自己製作一台可以獨立運作的 NFC 打卡鐘。

最初的構想只是「刷卡後記錄時間」，後來發現我一開始想的太簡單了，實際需求還會有員工管理、上下班模式、加班打卡、Excel 匯出、請假登記、離線使用、資料備份及 Web 管理介面，最後完成了一套不依賴雲端伺服器或是 Home Assistant主機 也能獨立使用的出勤系統。

## 為什麼選擇 ESP32-C3 與 RC522？

主控制器使用 ESP32-C3，主要原因是價格低、體積小，而且已經內建 Wi-Fi。打卡資料、員工資料與管理網頁都可以直接保存在ESP32-C3中，不需要另外準備伺服器或是USB隨身碟。

讀卡器則選擇 RC522。主要因為它價格便宜、取得容易，使用 SPI 與 ESP32 連接，適合讀取常見的 13.56 MHz MIFARE 卡片或感應扣。

這套設備主要使用以下零件：(因為在大陸上班所以都在淘寶購買取得)

* ESP32-C3 開發板 `淘寶人民幣16.5含運`
* RC522 感應讀卡器 `淘寶人民幣6.95含運`
* DS3231 備用時鐘模組 `淘寶人民幣14.27含運`
* 3.3V 有源蜂鳴器 `淘寶人民幣4.4含運`
* 四顆打卡模式按鈕 `淘寶人民幣8.8含運`
* Type-C充電線一條，拿來供電 `淘寶人民幣4.6含運`
* 防水盒 `淘寶人民幣6.5含運`
* 杜邦線若干 `淘寶人民幣2.5含運`
* 5V USB 豆腐頭 `抽屜撿的`
* 空白的CUID卡*20張 `淘寶人民幣20含運`
  如果家裡有社區門禁或是公司門禁可以先刷刷看，刷RC522能叫三聲的卡也能用，不一定要買很多張新的卡。
  
#以上總費用約人民幣85元，用4.7匯率換算大概400台幣
  
ESP32C3跟蜂鳴器、RC522、DS3231模塊、按鈕的連接方式如下圖
![NFC 打卡鐘管理介面](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/ScreenShot_2026-08-21_153059_940.png?raw=true)

一開始我曾考慮加入 LCD 螢幕與有線網路，但考量成本還有 ESP32 的GPIO接腳數量，最後取消螢幕並改用 Wi-Fi連接。

設備狀態直接透過燈號與蜂鳴器提示，詳細資料則在手機或電腦的管理網頁查看。


依照上圖把硬體接好線後尋找合適的外殼裝入，再依照 [ESPHome 燒錄教學](./docs/ESPHOME_FLASHING.md) 編譯並安裝 v1.3.34。四按鍵版接線請以[原始碼接線說明](./firmware/v1.3.34/README.md#接線)及 YAML 為準：RC522 的 RST 接 3.3V，GPIO10 留給加班下班按鍵。

可直接使用 [v1.3.34 Factory BIN](./nfc-attendance-clock-firmware.factory_1334.bin?raw=true) 透過 USB 安裝。

產品介紹說明:
## 四種打卡模式

一開始打卡鐘通常只有上班與下班，但是只有普通上下班是不夠的，因為還需要記錄加班，因此這台打卡鐘設計了四顆實體按鈕：

* 上班模式按鈕 (綠色)
* 下班模式按鈕 (紅色)
* 加班上班模式按鈕 (黃色)
* 加班下班模式按鈕 (藍色)

按下其中一顆模式按鈕後，設備會維持在該打卡模式，接下來所有員工刷卡都會依照目前模式保存在 Excel 中。

為了避免員工刷錯，透過 ESP32 內建 RGB LED 來使用不同顏色顯示目前狀態，也對應到按鈕顏色，進行顏色管理區分打卡模式：

* 綠燈：上班模式
* 紅燈：下班模式
* 黃燈：加班上班模式
* 藍燈：加班下班模式
* 七彩燈：等待登記新卡
* 橘燈閃爍：Wi-Fi 尚未連線

系統預設每天凌晨 00:00 切換為上班模式、下午 14:00 切換為下班模式。v1.3.34 可在「系統設定 → 自動模式切換」分別啟用四種模式並設定時間，支援跨午夜排程；加班上班與加班下班排程預設關閉。管理員也可以透過 Web 管理介面遠端切換，不一定要走到打卡鐘旁邊按按鈕。

## 刷卡成功與失敗提示

員工將已登記的卡片靠近 RC522 後，如果時間與資料都正常，蜂鳴器會叫一聲，代表本次打卡已成功保存。

如果蜂鳴器連續叫三聲，則表示本次操作沒有成功，可能原因包括：

* 這張卡片尚未登記
* 這張卡片已綁定其他員工
* 系統時間尚未準備完成
* 打卡資料寫入失敗
* 登記新卡時發現重複卡號

這樣即使沒有螢幕，員工也能透過聲音立即判斷打卡簽到結果是否成功。

## 第一次開機使用透過設備的 AP 模式進行WIFI配網連接

使用公開 BIN 的新裝置，開機後會開啟 `NFC-Clock-Fallback` 熱點供首次配網。若自行編譯並在 `secrets.yaml` 填好 Wi-Fi，則會先嘗試連線，失敗約一分鐘後才開啟備用熱點。

使用者只要依照下列步驟設定：

1. 將打卡鐘接上 5V USB 電源。
2. 等待約一分鐘，直到橘燈持續閃爍。
3. 使用手機或是帶有無線網卡的電腦、平板等設備，搜尋WIFI連接 `SSID:NFC-Clock-Fallback  密碼:change-me-1234`（v1.3.34 範例值；如已在 YAML 修改，請使用自己的熱點密碼）。
4. 如果手機提示「這個 Wi-Fi 沒有網際網路」，選擇仍要連線或保持連線。
5. 等待配網頁自動彈出。
6. 如果沒有自動出現，使用瀏覽器開啟 `http://192.168.4.1/`。
7. 配網頁面成功打開後選擇公司的 2.4 GHz Wi-Fi，然後往下拉找到密碼輸入欄位，輸入這個WIFI的連線密碼並儲存。
8. 讓手機、電腦或平板重新連回公司 Wi-Fi，再開啟打卡鐘管理網頁。

同一個區域網路內，可以使用固定名稱的網址，不用特地去找IP，用下方的mDNS網址開啟即可：

`http://nfc-attendance-clock.local/attendance/`

登入帳號為 `admin`。公開 BIN 的初始密碼為 `change-me-1334`，請首次登入後修改；自行編譯版使用 `secrets.yaml` 的 `attendance_web_password`。若採保留 NVS 的 OTA 更新，會繼續使用已保存的新密碼；Factory 完整安裝後可能需要使用初始密碼。
如果部分路由器或電腦不支援 mDNS，也可以進公司的路由器管理介面找尋刷卡機取得的 IP 位址登入。
`http://192.168.X.X/attendance/`

## 不依賴 Home Assistant 的 Web 管理介面

雖然設備使用 ESPHome 開發寫的程式代碼，網路環境中有 Home Assistant 主機是會主動發現刷卡機設備並接入的，後續如果管理人員要做一些擴充自動化，例如異常時間段打卡就發Line通知，或是如果門禁也有接入 Home Assistant 系統，也可以做到直接當作門禁機開門，但就算使用環境沒有 Home Assistant 主機，日常使用打卡與管理功能並不依賴 Home Assistant 主機，打卡機可以獨立正常使用。

ESP32 刷好bin後本身提供完整的 Web 管理介面，管理員可以使用手機、平板或電腦完成以下操作：
![NFC 打卡鐘管理介面](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/pic/web1.png?raw=true)
![NFC 打卡鐘管理介面](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/pic/2.png?raw=true)
![NFC 打卡鐘管理介面](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/pic/3.png?raw=true)
![NFC 打卡鐘管理介面](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/pic/4.png?raw=true)
![NFC 打卡鐘管理介面](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/pic/5.png?raw=true)
![NFC 打卡鐘管理介面](https://github.com/shihkefa/ESP32C3_RC522_NFC-attendance-clock/blob/main/pic/6.png?raw=true)
* 查看設備目前時間
* 查看 Wi-Fi 名稱、網路狀態及 IP 位址
* 遠端切換四種打卡模式
* 新增、修改及刪除員工
* 登記員工感應卡
* 搜尋刷卡紀錄
* 管理員補打卡：選擇員工、日期時間與四種打卡類型，輸入管理員密碼後新增紀錄
* 填寫每日請假或出勤備註
* 下載近兩個月的 Excel 彙總表
* 修改管理員密碼
* 手動校正時間
* 匯出及匯入完整備份
* 清除全部刷卡紀錄

目前最多可管理 100 位員工及 500 筆每日請假／出勤備註，每筆備註最多 15 個字。總覽顯示最近 10 筆刷卡資料，刷卡紀錄、員工與備註採每頁 20 筆。刷卡環形區容量為 25,856 筆，網頁及匯出提供目前日期往前兩個日曆月內的資料；若大量重複刷卡導致容量用滿，最舊紀錄仍會被循環覆蓋。

管理員補打卡只接受上述兩個日曆月範圍內、且不晚於裝置目前時間的紀錄。補登會新增紀錄，不會取代原刷卡紀錄；Excel 沿用上班取最早、下班取最晚的彙總方式。

## Excel 出勤彙總

打卡資料不只可以在網頁查看，也能直接下載成 Excel 檔案。

彙總表會依照以下欄位整理：

* 日期
* 員工編號
* 員工姓名
* 上班時間
* 下班時間
* 加班上班時間
* 加班下班時間
* 每日備註
* 當日出勤工時
* 當日加班工時

除了全部員工的總表，每位員工也會建立獨立分頁，分頁名稱使用「工號_姓名」，方便管理員查詢、列印或交給會計進一步整理核算薪資。



## 完成後的心得

從最初只有 ESP32、RC522 和蜂鳴器的小構思，逐步加入 Web 管理、四種打卡模式、DS3231、Excel 匯出、備註、備份與離線使用後，感覺這台設備已經不只是簡單的讀卡器，而是一套可以實際投入日常使用的小型出勤管理系統，蠻適合小型公司員工數低於100人使用的場合，雖然陽春一點只有刷卡登記，但這樣員工的生物特徵以及定位隱私權線也可以保護的比較好，不會有採集指紋、臉部特徵，或是開放手機定位權限的問題產生。

DIY 最大的優點，不只是節省購買商用設備的費用，而是所有功能都能依照自己的需求調整。未來如果需要增加外接燈號、顯示器、MQTT、Home Assistant 自動化或更多統計功能，都可以繼續在現有架構上擴充。

下班後在宿舍一個人的快樂也許就是像這樣，翻翻身邊的電子小零件來做一些生活中用得上的小東西，然後跟大家分享。  :)
