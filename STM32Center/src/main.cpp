#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <stdio.h>
#include <string.h>

// Hardware Configuration
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

#define RELAY_PIN PA0
#define RELAY_ON  HIGH
#define RELAY_OFF LOW
#define BLE_BAUDRATE 9600

// Hardware Timer States
HardwareTimer *MyTim = new HardwareTimer(TIM2);
volatile bool tick_1s_flag = false; // Must be volatile for ISR access

int cycle_counter = 0;              // Tracks 60s control cycle
int calculated_pump_on_sec = 0; 

// Sensor Calibration
const int SOIL_DRY_RAW = 505; // ADC at 0% moisture
const int SOIL_WET_RAW = 205; // ADC at 100% moisture

// UART & Global States
char rx_buffer[150];
uint8_t rx_index = 0;
unsigned long last_rx_byte_time = 0;
const unsigned long RX_FRAME_TIMEOUT_MS = 1000;

int nhiet_do = 0;
int do_am = 0;
int dien_ap_pin = 0;
int soil_percent = 0;
bool has_valid_data = false;
unsigned long last_valid_packet_time = 0; // [NEW] Lưu thời điểm nhận gói tin cuối
const unsigned long NODE_TIMEOUT_MS = 45000; // [NEW] Ngưỡng timeout 45 giây

// DSP: Median + EMA Pipeline
#define MEDIAN_WINDOW 5
int raw_soil_buffer[MEDIAN_WINDOW];
uint8_t buffer_index = 0;
bool buffer_filled = false;

float filtered_soil_ema = 0;
const float EMA_ALPHA = 0.2; // 20% new signal weight, 80% history

void sort_array(int arr[], int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}

// ADC noise suppression pipeline
int process_dsp_soil(int raw_new) {
    raw_soil_buffer[buffer_index] = raw_new;
    buffer_index++;
    if (buffer_index >= MEDIAN_WINDOW) {
        buffer_index = 0;
        buffer_filled = true;
    }

    if (!buffer_filled) {
        filtered_soil_ema = raw_new; 
        return raw_new;
    }

    // Median filter to eliminate spike noise
    int temp_arr[MEDIAN_WINDOW];
    memcpy(temp_arr, raw_soil_buffer, sizeof(raw_soil_buffer));
    sort_array(temp_arr, MEDIAN_WINDOW);
    int median_val = temp_arr[MEDIAN_WINDOW / 2];

    // EMA filter to smooth high-frequency variance
    filtered_soil_ema = (EMA_ALPHA * median_val) + ((1.0 - EMA_ALPHA) * filtered_soil_ema);
    
    return (int)filtered_soil_ema;
}

// Data Normalization
int calculate_soil_percent(int filtered_raw) {
    // Linear interpolation
    int percent = map(filtered_raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
    // Clamp output to prevent logical overflow
    return constrain(percent, 0, 100);
}

// Adaptive Control Logic
const int HOT_TEMP_THRESHOLD = 32;   
const int DRY_HUM_THRESHOLD = 45;    
const int BASE_TARGET_MOISTURE = 70; 
const float BASE_Kp_SEC = 0.1;       // Proportional gain mapping % error to seconds

unsigned long cycle_start_time = 0;
unsigned long calculated_pump_on_time = 0;
bool pump_on = false;
bool is_manual_mode = false;
const int MOISTURE_START_OFFSET = 10; 
bool watering_request = false;
int current_target_moisture = BASE_TARGET_MOISTURE; 

void pump_control(bool state) {
    if (pump_on != state) {
        digitalWrite(RELAY_PIN, state ? RELAY_ON : RELAY_OFF);
        pump_on = state;
        
        // Mitigate I2C disruption caused by Relay Back-EMF 
        delay(50);
        lcd.init();
    }
}

void timer_driven_pump_control() {
    if (is_manual_mode) return;
    if (!has_valid_data) {
        watering_request = false;
        calculated_pump_on_sec = 0;
        pump_control(false);
        return;
    }

    // Event-Driven Overshoot Protection
    if (soil_percent >= current_target_moisture) {
        if (pump_on) pump_control(false);
        watering_request = false;
        calculated_pump_on_sec = 0;
        // Do not reset cycle_counter to maintain 20s structure
    }

    // 1Hz Timer-Driven Routine
    if (tick_1s_flag) {
        tick_1s_flag = false; 
        cycle_counter++;      

        // 1. Begin new 60s control cycle
        if (cycle_counter >= 60) {
            cycle_counter = 0; 

            current_target_moisture = BASE_TARGET_MOISTURE;
            float dynamic_Kp = BASE_Kp_SEC;

            // Environmental compensation
            if (nhiet_do >= HOT_TEMP_THRESHOLD || do_am <= DRY_HUM_THRESHOLD) {
                current_target_moisture += 5;
                dynamic_Kp = BASE_Kp_SEC * 1.5;
            } 
            else if (nhiet_do <= 20 && do_am >= 80) {
                current_target_moisture -= 5;
                dynamic_Kp = BASE_Kp_SEC * 0.8;
            }

            int pump_start_threshold = current_target_moisture - MOISTURE_START_OFFSET;
            pump_start_threshold = constrain(pump_start_threshold, 0, 100);

            if (soil_percent <= pump_start_threshold) {
                watering_request = true;
                int error = current_target_moisture - soil_percent;
                
                calculated_pump_on_sec = (int)(dynamic_Kp * error);
                
                // Safety upper bound: max 15s pump duration
                if (calculated_pump_on_sec > 15) {
                    calculated_pump_on_sec = 15;
                }
            } else {
                watering_request = false;
                calculated_pump_on_sec = 0;
            }
        }

        // 2. Execute relay state within cycle bounds
        if (watering_request && (cycle_counter < calculated_pump_on_sec)) {
            pump_control(true);
        } else {
            pump_control(false);
        }
    }
}

// UART Packet Parsing
bool parse_pipe_format(const char *buf, int &t, int &h, int &s, int &p) {
    return sscanf(buf, "%d|%d|%d|%d", &t, &h, &s, &p) == 4;
}

void apply_packet(const char *packet) {
    if (strcmp(packet, "A") == 0) {
        is_manual_mode = !is_manual_mode;
        if (is_manual_mode) pump_control(!pump_on); 
        else pump_control(false);
        return;
    }

    int t, h, raw_s, p;
    if (parse_pipe_format(packet, t, h, raw_s, p)) {
        if (t >= -10 && t <= 60 && h >= 0 && h <= 100) {
            nhiet_do = t; do_am = h; dien_ap_pin = p;
            soil_percent = calculate_soil_percent(process_dsp_soil(raw_s));
            has_valid_data = true;
            last_valid_packet_time = millis();
        }
    }
}

void clear_rx_buffer() {
    rx_index = 0;
    memset(rx_buffer, 0, sizeof(rx_buffer));
}

void uart_receive() {
    unsigned long now = millis();

    if (rx_index > 0 && now - last_rx_byte_time > RX_FRAME_TIMEOUT_MS) {
        clear_rx_buffer();
    }

    while (Serial1.available()) {
        char c = (char)Serial1.read();
        last_rx_byte_time = millis();

        if (c == 'A') {
            apply_packet("A");
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (rx_index > 0) {
                rx_buffer[rx_index] = '\0';
                apply_packet(rx_buffer);
                clear_rx_buffer();
            }
            continue;
        }

        if (rx_index < sizeof(rx_buffer) - 1) {
            rx_buffer[rx_index++] = c;
            rx_buffer[rx_index] = '\0';
        } else {
            clear_rx_buffer();
        }
    }
}

// [NEW] Hàm giám sát kết nối
void check_node_timeout() {
    // Chỉ kiểm tra khi đã từng có kết nối (has_valid_data == true)
    if (has_valid_data && (millis() - last_valid_packet_time > NODE_TIMEOUT_MS)) {
        has_valid_data = false; // Đánh dấu mất dữ liệu
        pump_control(false);    // Tính năng an toàn: Ép tắt máy bơm ngay lập tức
        is_manual_mode = false; // Reset luôn cả chế độ thủ công
    }
}

// LCD Interface
void lcd_print_fixed(uint8_t row, const char *text) {
    char line[17];
    uint8_t i = 0;
    for (i = 0; i < 16; i++) {
        if (text[i] == '\0') break;
        line[i] = text[i];
    }
    while (i < 16) line[i++] = ' ';
    line[16] = '\0';
    lcd.setCursor(0, row);
    lcd.print(line);
}

unsigned long last_lcd_update_time = 0;
void update_lcd() {
    unsigned long now = millis();
    if (now - last_lcd_update_time < 500) return;
    last_lcd_update_time = now;

    if (!has_valid_data) {
        // [MODIFIED] Báo lỗi lên LCD
        lcd_print_fixed(0, "ERR: LOST SENSOR");
        lcd_print_fixed(1, "Pump: OFF (SAFE)");
        
        // [NEW] Bắn mã lỗi -1 lên Gateway để App điện thoại biết hệ thống đang lỗi
        Serial1.print("-1|-1|-1|-1|0\n"); 
        return;
    }

    char line1[17];
    char line2[17];
    snprintf(line1, sizeof(line1), "T:%d H:%d B:%d%%", nhiet_do, do_am, dien_ap_pin);
    // Format: S:[Current]/[Adaptive Target] [Pump State]
    snprintf(line2, sizeof(line2), "S:%d/%d %s", soil_percent, current_target_moisture, pump_on ? "ON" : "OFF");

    lcd_print_fixed(0, line1);
    lcd_print_fixed(1, line2);

    Serial1.print(nhiet_do); Serial1.print("|");
    Serial1.print(do_am); Serial1.print("|");
    Serial1.print(soil_percent); Serial1.print("|");
    Serial1.print(dien_ap_pin); Serial1.print("|");
    Serial1.print(pump_on ? "1" : "0");
    Serial1.print("\n");
}

// Timer2 ISR: Minimal execution footprint
void Timer2_ISR() {
    tick_1s_flag = true;
}

// Main Setup & Loop
void setup() {
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, RELAY_OFF);
    
    Wire.setSDA(PB7);
    Wire.setSCL(PB6);
    Wire.begin();

    lcd.init();
    lcd.backlight();
    lcd_print_fixed(0, "System Init...");
    delay(1000);

    Serial1.begin(BLE_BAUDRATE);

    // Hardware Timer 2 Config (1Hz tick)
    MyTim->setOverflow(1, HERTZ_FORMAT); 
    MyTim->attachInterrupt(Timer2_ISR);  
    MyTim->resume();                     
}

void loop() {
    uart_receive();
    check_node_timeout();
    timer_driven_pump_control(); 
    update_lcd();
}