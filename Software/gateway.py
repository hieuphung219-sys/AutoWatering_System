import asyncio
import time
from typing import Optional
from bleak import BleakScanner, BleakClient
import paho.mqtt.client as mqtt  # [NEW] Thư viện MQTT vừa cài
import json                      # [NEW] Thư viện đóng gói dữ liệu JSON

# Hardware & BLE Configuration
STM32_MAC = "18:45:16:8F:AC:16"
UART_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"
MIN_SEND_INTERVAL_SEC = 1.0
TEST_PAYLOAD = b"30|32|663|99\n"

# [NEW] Thông số Trạm trung chuyển MQTT (HiveMQ)
MQTT_BROKER = "broker.emqx.io"
MQTT_PORT = 1883
TOPIC_DATA = "hieuphung/autowatering/data"
TOPIC_CMD = "hieuphung/autowatering/cmd"

class IoTGateway:
    def __init__(self):
        self.client = BleakClient(STM32_MAC)
        self.tx_queue: asyncio.Queue[bytes] = asyncio.Queue()
        self.last_payload: bytes = b""
        self.last_time: float = 0.0
        self.loop = None # [NEW] Giữ lại Event Loop của hệ điều hành

        # [NEW] Khởi tạo Client MQTT và gán hàm xử lý khi có tin nhắn tới
        self.mqtt = mqtt.Client()
        self.mqtt.on_message = self.on_mqtt_message

    # [NEW] Bản chất: Hàm này tự động chạy khi Laptop nhận được lệnh từ Cloud
    def on_mqtt_message(self, client, userdata, msg):
        command = msg.payload.decode().strip()
        print(f"[MQTT RX] Nhận lệnh từ Cloud: {command}")
        
        if command == "A":
            payload = b"A\n"
            # Đẩy lệnh "A" vào hàng đợi BLE để gửi xuống STM32.
            # call_soon_threadsafe đảm bảo luồng MQTT không làm treo luồng BLE.
            if self.loop:
                self.loop.call_soon_threadsafe(self.tx_queue.put_nowait, payload)

    def parse_adv_name(self, name: str) -> Optional[bytes]:
        if not name or len(name) != 11 or not name.startswith("U"):
            return None
        try:
            t, h, s, p, c = int(name[1:3]), int(name[3:5]), int(name[5:8]), int(name[8:10]), int(name[10])
            if (t + h + s + p) % 10 == c:
                return f"{t}|{h}|{s}|{p}\n".encode("utf-8")
        except ValueError:
            pass
        return None

    def scan_callback(self, device, adv_data):
        payload = self.parse_adv_name(adv_data.local_name)
        if not payload: 
            return

        now = time.monotonic()
        if payload == self.last_payload and (now - self.last_time) < MIN_SEND_INTERVAL_SEC:
            return

        self.last_payload, self.last_time = payload, now
        data_str = payload.decode().strip()
        print(f"[RX BLE] {adv_data.local_name} -> {data_str}")
        
        try: 
            self.tx_queue.put_nowait(payload)
            
            # [NEW] Tách chuỗi dữ liệu (ví dụ: "30|32|663|99") thành các biến
            parts = data_str.split('|')
            if len(parts) == 4:
                # Đóng gói thành chuẩn JSON
                json_data = json.dumps({
                    "nhiet_do": int(parts[0]),
                    "do_am": int(parts[1]),
                    "dat": int(parts[2]),
                    "pin": int(parts[3])
                })
                # Phát (Publish) lên Cloud để App điện thoại lấy về
                self.mqtt.publish(TOPIC_DATA, json_data)
                print(f"[MQTT TX] Đã đẩy lên Cloud: {json_data}")

        except asyncio.QueueFull: 
            pass
        except Exception as e:
            print(f"[Parse Error] {e}")

    async def tx_loop(self):
        while True:
            payload = await self.tx_queue.get()
            try:
                try: 
                    await self.client.write_gatt_char(UART_UUID, payload, response=False)
                except Exception: 
                    await self.client.write_gatt_char(UART_UUID, payload, response=True)
                print(f"[TX BLE] Gửi xuống STM32: {payload.decode().strip()}")
            except Exception as e:
                print(f"[TX ERROR] {e}")
            finally:
                self.tx_queue.task_done()

    async def run(self):
        self.loop = asyncio.get_running_loop() # [NEW] Lấy Loop hiện tại
        
        # [NEW] Kết nối MQTT và Lắng nghe lệnh điều khiển (Subscribe)
        print(f"Đang kết nối MQTT Broker {MQTT_BROKER}...")
        self.mqtt.connect(MQTT_BROKER, MQTT_PORT, 60)
        self.mqtt.subscribe(TOPIC_CMD)
        self.mqtt.loop_start() # Chạy MQTT ở chế độ nền liên tục

        print(f"Connecting to STM32 ({STM32_MAC})...")
        await self.client.connect()
        print("[OK] Connected.")

        await self.client.write_gatt_char(UART_UUID, TEST_PAYLOAD, response=False)
        tx_task = asyncio.create_task(self.tx_loop())
        
        print("Scanning for sensor nodes...")
        async with BleakScanner(self.scan_callback):
            await asyncio.Future()

if __name__ == "__main__":
    try:
        asyncio.run(IoTGateway().run())
    except KeyboardInterrupt:
        print("\nGateway stopped.")