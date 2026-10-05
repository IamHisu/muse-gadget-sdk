# Muse Gadget SDK — Zhengchen 1.54 TFT Wi-Fi

Đây là bản ESP32 firmware đã được rút gọn cho duy nhất board **ZHENGCHEN_1_54TFT_WIFI**.

## Phần cứng

- ESP32-S3, flash 16 MB, PSRAM 8 MB
- LCD ST7789 SPI 240 × 240
- Nút BOOT GPIO0: nhấn để mở/chọn menu; giữ 1,5 giây để nói; khi đang nghe, nhấn để kết thúc
- Nút VOL- GPIO39: di chuyển xuống trong menu
- Nút VOL+ GPIO10: di chuyển lên trong menu
- Micro I2S: WS GPIO4, SCK GPIO5, SD GPIO6
- Loa MAX98357A: DOUT GPIO7, BCLK GPIO15, LRCK GPIO16

## Môi trường

Project dùng **ESP-IDF v6.0.3** và target `esp32s3`. Mở terminal ESP-IDF 6.0.3, sau đó chuyển vào thư mục này.

## Build

```powershell
idf.py build
```

Target và profile Zhengchen đã được đặt mặc định trong `CMakeLists.txt`, vì vậy không cần truyền thêm `-B`, `IDF_TARGET` hay `SDKCONFIG_DEFAULTS`.

Lần build đầu sẽ tải lại managed components và có thể dùng khoảng 500 MB tạm thời. Sau khi xóa `build` và `managed_components`, lần build tiếp theo sẽ tải và biên dịch lại.

## Gemini

Trợ lý giọng nói dùng Gemini Live và trả lời bằng tiếng Việt. API key được lưu trong NVS của board, không đặt trong source code hoặc `sdkconfig`.

1. Mở menu **Bluetooth setup** trên board và quét QR.
2. Kết nối với board, nhập mã ghép đôi đang hiện trên TFT.
3. Nhập **Gemini API key**, bấm **Save**, rồi **Test connection**.
4. Ở màn hình chính, giữ BOOT 1,5 giây để nói và bấm BOOT để kết thúc.

Trang setup cũ đã publish vẫn dùng được: nhập Gemini API key vào ô **Device token**. Firmware giữ lệnh cũ để tương thích trong lúc trang mới chưa được deploy.

Cũng có thể chạy `idf.py menuconfig`, mở **Muse → Gemini API key**, nhập key rồi lưu và build lại. Đây là key mặc định được biên dịch vào firmware; key đã lưu trong NVS qua Bluetooth sẽ được ưu tiên. `sdkconfig` và thư mục `build` đã được `.gitignore` để tránh đưa key lên Git.

## Flash và monitor

Chỉ chạy sau khi build thành công và đã xác định đúng cổng COM:

```powershell
idf.py -p COMx flash monitor
```

Nhấn `Ctrl+]` để thoát monitor. File cấu hình board nằm tại `devices/sdkconfig.muse-zhengchen-154`; driver phần cứng nằm tại `components/muse/boards/board_zhengchen_154.c`.
