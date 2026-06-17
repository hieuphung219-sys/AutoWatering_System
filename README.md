# AutoWatering System (Hệ thống tưới cây tự động qua BLE)
Dự án IoT điều khiển tưới cây tự động sử dụng kiến trúc phân tán. Hệ thống thu thập dữ liệu môi trường từ các Node cảm biến (Arduino), truyền qua giao thức BLE (HM-10) tới PC Gateway, và cuối cùng điều khiển bơm nước tại bộ trung tâm (STM32) dựa trên thuật toán điều khiển thích nghi.

## Kiến trúc hệ thống (System Architecture)
Hệ thống được chia làm 3 thành phần chính hoạt động song song:
1. Sensor Node (Arduino Pro Mini/Uno + HM-10)
   - Đọc dữ liệu từ cảm biến Nhiệt độ/Độ ẩm (DHT11), Cảm biến độ ẩm đất và điện áp pin.
   - Sử dụng kỹ thuật Power Gating và Deep Sleep (thư viện `LowPower`) để tối ưu năng lượng.
   - Gói dữ liệu và phát (Advertise) qua module BLE HM-10.
2. PC IoT Gateway (Python)
   - Quét sóng BLE liên tục (sử dụng thư viện `bleak`).
   - Bắt gói tin quảng bá từ Node, giải mã (decode) chuỗi dữ liệu.
   - Đẩy dữ liệu tuần tự xuống bộ trung tâm STM32 qua kết nối BLE UART.

3. Center Controller (STM32 Blackpill + LCD I2C + Relay)
   - Nhận dữ liệu từ Gateway qua UART.
   - Ứng dụng xử lý tín hiệu số (DSP): Sử dụng Bộ lọc Trung vị (Median Filter) và Trung bình động lũy thừa (EMA) để khử nhiễu tín hiệu ADC của độ ẩm đất.
   - Điều khiển Relay máy bơm thông qua Hardware Timer (chu kỳ 60s) và logic điều khiển thích nghi (thay đổi thời gian bơm dựa trên nhiệt độ và độ ẩm môi trường).
   - Hiển thị thông số thời gian thực lên LCD 16x2.

## Cấu trúc thư mục
- `/ArduinoNode/`: Mã nguồn C++ (PlatformIO) cho Node cảm biến.
- `/STM32Center/`: Mã nguồn C++ (PlatformIO) cho bộ điều khiển trung tâm.
- `/Software/`: Chứa kịch bản Python (`gateway.py`) chạy trên PC.

## Yêu cầu phần cứng (Hardware)
- Vi điều khiển: Arduino (Pro 8MHz ATmega328), STM32 (Blackpill F401CC).
- Kết nối: 2x Module BLE HM-10 (hoặc tương đương hỗ trợ UART ảo `0000ffe1`).
- Cảm biến: DHT11, Cảm biến độ ẩm đất (Analog).
- Khác: Relay 5V, Màn hình LCD 16x2 (I2C).

## Hướng dẫn cài đặt và sử dụng

### 1. Triển khai Node Cảm biến & Trung tâm
- Cài đặt VS Code và tiện ích mở rộng PlatformIO.
- Mở lần lượt thư mục `ArduinoNode` và `STM32Center`.
- PlatformIO sẽ tự động tải các thư viện cần thiết (như `LowPower`, `DHT`, `LiquidCrystal_I2C`).
- Nhấn Upload để nạp firmware xuống mạch.

### 2. Triển khai PC Gateway
- Yêu cầu môi trường: Python 3.7+
- Cài đặt thư viện `bleak`:
  ```bash
  pip install bleak
- Định cấu hình địa chỉ MAC của STM32 BLE trong file gateway.py (biến STM32_MAC).
- Chạy Gateway:
  ```bash
  python Software/gateway.py
