import asyncio
import time
from typing import Optional
from bleak import BleakScanner, BleakClient

# ======================================================
#  PC IOT GATEWAY
# ======================================================
#
# Nhiệm vụ:
# - Kết nối tới BLE module đang cắm trên STM32
# - Quét BLE advertisement từ node cảm biến
# - Node phát tên dạng: U3032663994
# - Gateway giải mã thành: 30|32|663|99\n
# - Gửi chuỗi đó xuống STM32 qua BLE UART characteristic FFE1
#
# Điểm sửa quan trọng so với bản cũ:
# - Không gọi write_gatt_char() kiểu fire-and-forget trong callback nữa.
# - Dùng asyncio.Queue để gửi tuần tự.
# - Có log rõ ràng: đã đưa vào hàng đợi / đã gửi thành công / gửi lỗi.
# - Có chống spam cùng một payload quá dày.
#
# ======================================================


# ======================= CẤU HÌNH BLE =======================

# MAC của BLE module cắm trên STM32
STM32_MAC = "18:45:16:8F:AC:16"

# UUID UART ảo thường gặp trên HM-10 / BT05 / AT09 / CC41A
UART_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"

# Nếu True, ngay sau khi kết nối STM32 sẽ gửi thử một frame an toàn.
# Frame này có S=663, nằm giữa 400 và 700 nên theo logic hiện tại sẽ KHÔNG bật bơm.
SEND_TEST_ON_CONNECT = True
TEST_PAYLOAD = "30|32|663|99\n"

# Chống gửi lặp quá nhanh cùng một payload
MIN_SEND_INTERVAL_SEC = 1.0


# ======================= BIẾN TOÀN CỤC =======================

stm32_client: Optional[BleakClient] = None
send_queue: asyncio.Queue[str] = asyncio.Queue()

last_payload = ""
last_payload_time = 0.0


# ======================================================
#  GIẢI MÃ TÊN NODE
# ======================================================

def decode_node_name(name: str) -> Optional[str]:
    """
    Node phát tên dạng:
        U3032663994

    Bóc tách:
        U  30 32 663 99 4
           T  H  S   P  checksum

    Checksum:
        (T + H + S + P) % 10 == checksum

    Trả về:
        "30|32|663|99\n"
    """

    if not name:
        return None

    if not name.startswith("U"):
        return None

    if len(name) != 11:
        return None

    try:
        t = int(name[1:3])
        h = int(name[3:5])
        s = int(name[5:8])
        p = int(name[8:10])
        c = int(name[10])
    except ValueError:
        return None

    if (t + h + s + p) % 10 != c:
        print(f"[Cảnh báo] Sai checksum, bỏ qua: {name}")
        return None

    return f"{t}|{h}|{s}|{p}\n"


# ======================================================
#  CALLBACK KHI SCAN THẤY BLE ADVERTISEMENT
# ======================================================

def detection_callback(device, advertisement_data):
    global last_payload, last_payload_time

    name = advertisement_data.local_name
    payload = decode_node_name(name)

    if payload is None:
        return

    now = time.monotonic()

    # Chống spam cùng một gói quá nhanh
    if payload == last_payload and (now - last_payload_time) < MIN_SEND_INTERVAL_SEC:
        return

    last_payload = payload
    last_payload_time = now

    print(f"[Đã bắt được sóng] {name} -> Dịch thành: {payload.strip()}")

    try:
        send_queue.put_nowait(payload)
        print(f"[Đã đưa vào hàng đợi gửi STM32] {payload.strip()}")
    except asyncio.QueueFull:
        print("[Lỗi] Hàng đợi gửi STM32 bị đầy")


# ======================================================
#  GỬI DỮ LIỆU XUỐNG STM32
# ======================================================

async def send_to_stm32(payload: str):
    global stm32_client

    if stm32_client is None:
        print("[Lỗi] Chưa có client STM32")
        return

    if not stm32_client.is_connected:
        print("[Lỗi] STM32 BLE chưa kết nối")
        return

    data = payload.encode("utf-8")

    # Một số HM-10 clone thích response=False.
    # Một số BLE stack Windows lại ổn hơn với response=True.
    # Thử response=False trước, lỗi thì thử response=True.
    try:
        await stm32_client.write_gatt_char(UART_UUID, data, response=False)
        print(f"[Đã gửi xuống STM32] {payload.strip()}")
    except Exception as e1:
        print(f"[Cảnh báo] Gửi response=False lỗi: {e1}")

        try:
            await stm32_client.write_gatt_char(UART_UUID, data, response=True)
            print(f"[Đã gửi xuống STM32 - response=True] {payload.strip()}")
        except Exception as e2:
            print(f"[Lỗi] Không gửi được xuống STM32: {e2}")


async def sender_loop():
    while True:
        payload = await send_queue.get()

        try:
            await send_to_stm32(payload)
        finally:
            send_queue.task_done()


# ======================================================
#  MAIN
# ======================================================

async def main():
    global stm32_client

    print("Đang khởi động IoT Gateway...")
    print(f"Đang kết nối trạm STM32 BLE: {STM32_MAC}")

    stm32_client = BleakClient(STM32_MAC)

    try:
        await stm32_client.connect()
    except Exception as e:
        print(f"[Lỗi] Không kết nối được STM32 BLE: {e}")
        return

    if not stm32_client.is_connected:
        print("[Lỗi] Kết nối STM32 BLE thất bại")
        return

    print("[OK] Đã kết nối STM32 BLE")

    # Gửi thử một frame để kiểm tra LCD/USART bên STM32 có nhận không
    if SEND_TEST_ON_CONNECT:
        print(f"[TEST] Gửi thử xuống STM32: {TEST_PAYLOAD.strip()}")
        await send_to_stm32(TEST_PAYLOAD)

    # Tạo task gửi dữ liệu tuần tự xuống STM32
    sender_task = asyncio.create_task(sender_loop())

    print("Đang quét node cảm biến xung quanh...")

    scanner = BleakScanner(detection_callback)

    try:
        await scanner.start()

        while True:
            await asyncio.sleep(1)

    except KeyboardInterrupt:
        print("Đang dừng gateway...")

    finally:
        await scanner.stop()

        sender_task.cancel()

        if stm32_client and stm32_client.is_connected:
            await stm32_client.disconnect()

        print("Đã ngắt kết nối gateway")


if __name__ == "__main__":
    asyncio.run(main())