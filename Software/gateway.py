import asyncio
import time
from typing import Optional
from bleak import BleakScanner, BleakClient

# Hardware & BLE Configuration
STM32_MAC = "18:45:16:8F:AC:16"
UART_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"
MIN_SEND_INTERVAL_SEC = 1.0
TEST_PAYLOAD = b"30|32|663|99\n"

class IoTGateway:
    def __init__(self):
        self.client = BleakClient(STM32_MAC)
        self.tx_queue: asyncio.Queue[bytes] = asyncio.Queue()
        self.last_payload: bytes = b""
        self.last_time: float = 0.0

    def parse_adv_name(self, name: str) -> Optional[bytes]:
        """ Decodes node name (e.g., U3032663994) into UART payload. """
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

        # Anti-spam filter
        now = time.monotonic()
        if payload == self.last_payload and (now - self.last_time) < MIN_SEND_INTERVAL_SEC:
            return

        self.last_payload, self.last_time = payload, now
        print(f"[RX] {adv_data.local_name} -> {payload.decode().strip()}")
        
        try: 
            self.tx_queue.put_nowait(payload)
        except asyncio.QueueFull: 
            pass

    async def tx_loop(self):
        """ Asynchronous worker to sequentially write to STM32. """
        while True:
            payload = await self.tx_queue.get()
            try:
                # Fallback mechanism for varying BLE stack implementations
                try: 
                    await self.client.write_gatt_char(UART_UUID, payload, response=False)
                except Exception: 
                    await self.client.write_gatt_char(UART_UUID, payload, response=True)
                print(f"[TX] {payload.decode().strip()}")
            except Exception as e:
                print(f"[TX ERROR] {e}")
            finally:
                self.tx_queue.task_done()

    async def run(self):
        print(f"Connecting to STM32 ({STM32_MAC})...")
        await self.client.connect()
        print("[OK] Connected.")

        # Transmit test frame to verify STM32 UART/LCD
        await self.client.write_gatt_char(UART_UUID, TEST_PAYLOAD, response=False)
        
        tx_task = asyncio.create_task(self.tx_loop())
        
        print("Scanning for sensor nodes...")
        # Context manager handles automated start/stop of BleakScanner
        async with BleakScanner(self.scan_callback):
            await asyncio.Future() # Block indefinitely until manually interrupted

if __name__ == "__main__":
    try:
        asyncio.run(IoTGateway().run())
    except KeyboardInterrupt:
        print("\nGateway stopped.")