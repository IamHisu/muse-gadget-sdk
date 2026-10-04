# Zhengchen device profile

Fork này giữ duy nhất một profile thiết bị.

| Board | Chip | Display | Flash / PSRAM | Profile |
|---|---|---|---|---|
| ZHENGCHEN_1_54TFT_WIFI | ESP32-S3 | ST7789 SPI 240 × 240 | 16 MB / 8 MB | `sdkconfig.muse-zhengchen-154` |

## GPIO

| Chức năng | GPIO |
|---|---:|
| LCD MOSI | 41 |
| LCD SCLK | 42 |
| LCD CS | 21 |
| LCD DC | 40 |
| LCD reset | 45 |
| LCD backlight | 20 |
| Menu/select, hold to talk / BOOT | 0 |
| Menu down / VOL- | 39 |
| Menu up / VOL+ | 10 |
| Micro WS / SCK / SD | 4 / 5 / 6 |
| Speaker DOUT / BCLK / LRCK | 7 / 15 / 16 |

## Build

Từ thư mục `esp32`, với ESP-IDF v6.0.3 đã được activate:

```powershell
idf.py build
```

Profile này đã được nạp mặc định bởi top-level `CMakeLists.txt`.
