#include <Arduino.h>
#include <LowPower.h>
#include <DHT.h>
#include <math.h>

// Hardware & Sensor Configuration
#define HM10_BAUDRATE 9600

#define DHTPIN 2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

#define SOIL_PIN A1
#define BATTERY_PIN A0

// Controls VCC for soil sensor and voltage divider
#define SENSOR_POWER_PIN 3 

// Battery Parameters & Normalization
const float ADC_REF_VOLTAGE = 3.3; 
const float BATTERY_SCALE = 3.0;  
const float BATTERY_EMPTY = 3.2;   
const float BATTERY_FULL = 4.2;    

// HM-10 Wake-up sequence
const char HM10_WAKE_SEQUENCE[] = "1111111111111111111111111111111111111111111111111111111111111111111111111111111111";

int last_temperature = 25;
int last_humidity = 60;

// Utilities: Clamp & Median Filter
int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

// Returns median of 5 samples to eliminate spike noise
int read_analog_median(uint8_t pin) {
    const uint8_t samples = 5;
    int arr[samples];
    
    for (uint8_t i = 0; i < samples; i++) {
        arr[i] = analogRead(pin);
        // Sleep 15ms to allow ADC capacitor to charge
        LowPower.powerDown(SLEEP_15MS, ADC_OFF, BOD_OFF); 
    }
    
    // Bubble sort
    for (uint8_t i = 0; i < samples - 1; i++) {
        for (uint8_t j = 0; j < samples - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
    return arr[samples / 2];
}

// Sensor Reading Routine
void read_sensors(int &temperature, int &humidity, int &soil, int &battery) {
    // Power up sensors
    digitalWrite(SENSOR_POWER_PIN, HIGH);
    
    // Wait 250ms for DHT11 and voltage divider stabilization
    LowPower.powerDown(SLEEP_250MS, ADC_OFF, BOD_OFF);

    // Read DHT11
    float hum_read = dht.readHumidity();
    float temp_read = dht.readTemperature();

    if (!isnan(hum_read) && !isnan(temp_read)) {
        last_temperature = clamp_int((int)round(temp_read), 0, 99);
        last_humidity = clamp_int((int)round(hum_read), 0, 99);
    }
    temperature = last_temperature;
    humidity = last_humidity;

    // Read Soil Moisture
    soil = clamp_int(read_analog_median(SOIL_PIN), 0, 999);

    // Read Battery Voltage
    int bat_raw = read_analog_median(BATTERY_PIN);

    float voltage_adc = (bat_raw / 1023.0) * ADC_REF_VOLTAGE; 
    float battery_voltage = voltage_adc * BATTERY_SCALE;

    float percentage = ((battery_voltage - BATTERY_EMPTY) / (BATTERY_FULL - BATTERY_EMPTY)) * 100.0;
    int final_percent = clamp_int((int)percentage, 0, 100);

    battery = final_percent;
    // Power down sensors to conserve energy
    digitalWrite(SENSOR_POWER_PIN, LOW);
}

// BLE Communication & Power Management
void send_to_hm10_and_sleep(int temp, int hum, int soil, int bat) {
    // Wake up HM-10
    Serial.print(HM10_WAKE_SEQUENCE);
    Serial.flush(); // Wait for TX buffer to empty
    LowPower.powerDown(SLEEP_250MS, ADC_OFF, BOD_OFF); 

    // Calculate checksum and format string
    int checksum = (temp + hum + soil + bat) % 10;
    char name_cmd[25];
    snprintf(name_cmd, sizeof(name_cmd), "AT+NAMEU%02d%02d%03d%02d%d", temp, hum, soil, bat, checksum);

    // Send configure command
    Serial.print(name_cmd);
    Serial.flush(); 
    LowPower.powerDown(SLEEP_250MS, ADC_OFF, BOD_OFF);

    // Reset HM-10 to start broadcasting
    Serial.print("AT+RESET");
    Serial.flush(); 
    
    // Deep sleep MCU
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);

    // Put HM-10 to sleep after broadcasting
    Serial.print("AT+SLEEP");
    Serial.flush(); 
    LowPower.powerDown(SLEEP_250MS, ADC_OFF, BOD_OFF);
}

// Main Routine
void setup() {
    Serial.begin(HM10_BAUDRATE);
    
    pinMode(SENSOR_POWER_PIN, OUTPUT);
    digitalWrite(SENSOR_POWER_PIN, LOW); // Sensors off by default

    dht.begin();
    
    // Allow system stabilization upon power-up
    LowPower.powerDown(SLEEP_2S, ADC_OFF, BOD_OFF);
}

// Wake-up and Sleep thresholds based on Raw ADC
#define WAKE_UP_THRESHOLD 325 // < 60% moisture
#define SLEEP_THRESHOLD 265   // >= 70% moisture

void loop() {
    int temperature = 0, humidity = 0, soil = 0, battery = 0;
    
    // Retain mode state across iterations
    static bool is_continuous_mode = false; 

    // Read data and transmit via BLE
    read_sensors(temperature, humidity, soil, battery);
    send_to_hm10_and_sleep(temperature, humidity, soil, battery);

    // State Machine Logic
    if (!is_continuous_mode && soil >= WAKE_UP_THRESHOLD) {
        // Enable continuous transmission
        is_continuous_mode = true;
    } 
    else if (is_continuous_mode && soil <= SLEEP_THRESHOLD) {
        // Revert to power-saving mode
        is_continuous_mode = false;
    }

    // 3. Power Management 
    if (is_continuous_mode) {
        // Continuous mode
        // Bypass deep sleep, loop restarts immediately.
        // Transmission rate: Maximum (~1.2s / packet).
    } else {
        // Cyclic sleep mode
        // Deep sleep for 24s (8s x 3) to conserve battery.
        for (uint8_t i = 0; i < 3; i++) {
            LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
        }
    }
}
