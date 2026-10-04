# Zhengchen firmware notes

Repository này chỉ hỗ trợ board **ZHENGCHEN_1_54TFT_WIFI** trên ESP32-S3.

## Toolchain

- ESP-IDF v6.0.3
- Target cố định: `esp32s3`
- Profile cố định: `devices/sdkconfig.muse-zhengchen-154`
- Chạy lệnh từ thư mục `esp32`

## Build

```powershell
idf.py build
```

`CMakeLists.txt` đã nạp target và profile mặc định. Không tạo profile cho board khác trong fork này. Không commit SDK token, `sdkconfig`, thư mục `build`, `managed_components` hoặc cache.

## Source layout

- `components/muse/boards/board_zhengchen_154.c`: LCD, buttons, microphone và speaker
- `devices/sdkconfig.muse-zhengchen-154`: cấu hình ESP32-S3, flash, PSRAM, LVGL và Muse
- `main/`: logic ứng dụng dùng chung
- `tools/muse/`: helper cho Zhengchen

Sau thay đổi driver hoặc cấu hình, chạy `idf.py build`. Chỉ flash khi build thành công và người dùng yêu cầu flash.
