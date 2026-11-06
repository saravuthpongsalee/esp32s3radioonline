/*
  COOL 93 RADIO | ESP32-CYD S3 BLACK | V1.0.0 | 05 Nov 2026

  เปิดบอร์ดแล้วต่อ Wi-Fi เพื่อเล่นสตรีม COOLFahrenheit 93 ทางลำโพงบนบอร์ด
  Arduino IDE 1.8.19 ใช้บอร์ด ESP32S3 Dev Module และ Arduino-ESP32 Core 2.0.17
  ติดตั้ง ESP8266Audio 2.2.0 และ LovyanGFX 1.2.26 ผ่าน Library Manager
  เลือก Partition Scheme เป็น Huge APP และเลือก Flash Size ให้ตรงกับบอร์ด
  เปิด OPI PSRAM ถ้าบอร์ดมี และเปิด USB CDC On Boot เพื่อดู Serial ที่ 115200
  สเก็ตช์เดียวนี้รวมการต่อ Wi-Fi การรับ MP3 และการส่งเสียง I2S ไปยัง ES8311
*/

#include <Arduino.h>                  // ใช้ millis, delay, GPIO และ Serial ของ Arduino
#include <esp_arduino_version.h>      // ใช้ตรวจรุ่น ESP32 Core ที่เลือกใน Arduino IDE
#include <WiFi.h>                     // ต่อ ESP32-S3 เข้ากับเราเตอร์ในโหมดสถานีลูกข่าย
#include <WiFiClientSecure.h>         // เปิดการเชื่อมต่อ TLS สำหรับสตรีม HTTPS
#include <HTTPClient.h>               // รับข้อมูล HTTP และแกะกรอบ Transfer-Encoding แบบ chunked
#include <Wire.h>                     // คุยกับชิป ES8311 ผ่านบัส I2C
#define LGFX_USE_V1                   // เลือกอินเทอร์เฟซ LovyanGFX V1 ตามที่บอร์ดเดิมใช้
#include <LovyanGFX.hpp>              // วาดเลขสถานีและสถานะบนจอ ILI9341
#include <AudioGeneratorMP3.h>        // ถอดรหัส MP3 ของ ESP8266Audio เป็นตัวอย่างเสียง PCM
#include <driver/i2s.h>               // ส่ง PCM ด้วยไดรเวอร์ I2S ของ Core 2.x
#include <freertos/FreeRTOS.h>        // ใช้ชนิดข้อมูลและเวลา tick ของ FreeRTOS
#include <freertos/task.h>            // แยกงานโหลดข้อมูลออกจากงานถอดรหัสเสียง
#include <freertos/stream_buffer.h>   // ส่งไบต์ MP3 ระหว่างงานเครือข่ายกับ Arduino loop
#include <atomic>                     // ให้สองงานอ่านและเขียนธงสถานะร่วมกันได้

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ESP_ARDUINO_VERSION_MAJOR != 2  // รุ่นนี้ใช้ ESP32-S3 และไดรเวอร์ของ Core 2.x
#error "Select ESP32S3 Dev Module with Arduino-ESP32 Core 2.0.17"         // แสดงค่าที่ต้องตั้งเมื่อเลือกบอร์ดหรือ Core ผิด
#endif                                                                 // จบเงื่อนไขตรวจบอร์ด

constexpr char WIFI_NAME[] = "RINDA 2.4G";    // ชื่อเครือข่ายที่ลองก่อน ต้องตรงกับตัวอักษรและช่องว่างจริง
constexpr char WIFI_FALLBACK[] = "RINDA";     // ลองชื่อนี้ด้วยเมื่อเราเตอร์ใช้ 2.4G เป็นชื่อย่านความถี่
constexpr char WIFI_PASS[] = "0910318833";    // รหัส Wi-Fi ใช้เฉพาะตอนเชื่อมต่อเราเตอร์
constexpr char RADIO_URL[] = "https://coolism-web.cdn.byteark.com/;stream/1";  // URL เสียง MP3 ของสถานี COOL 93
constexpr int VOLUME_PERCENT = 85;            // ระดับเสียงที่ปรับในโค้ดได้ระหว่าง 0 ถึง 100
constexpr char BUILD[] = "V1.0.0  " __DATE__ " " __TIME__;  // แสดงเวอร์ชันและวันเวลาที่คอมไพล์จริง
static_assert(VOLUME_PERCENT >= 0 && VOLUME_PERCENT <= 100, "VOLUME_PERCENT must be 0 to 100");  // กันค่าระดับเสียงนอกช่วง

constexpr int LCD_BL = 45;              // GPIO ที่เปิดไฟส่องหลังของจอ
constexpr int CODEC_SDA = 16;           // สายข้อมูล I2C ของ ES8311
constexpr int CODEC_SCL = 15;           // สายสัญญาณนาฬิกา I2C ของ ES8311
constexpr uint8_t CODEC_ADDR = 0x18;   // ที่อยู่ I2C แบบ 7 บิตของ ES8311 บนบอร์ดสีดำ
constexpr int AMP_PIN = 1;              // ขาควบคุมแอมป์ลำโพง LOW คือเปิด และ HIGH คือปิด
constexpr int I2S_MCLK = 4;             // สัญญาณนาฬิกาหลักจาก ESP32-S3 ไป ES8311
constexpr int I2S_BCLK = 5;             // สัญญาณนาฬิกาบิตเสียง I2S
constexpr int I2S_LRCK = 7;             // สัญญาณแบ่งเฟรมและช่องซ้ายขวาของ I2S
constexpr int I2S_DOUT = 8;             // ขาที่ส่งข้อมูลเสียงจาก ESP32-S3 เข้า DAC

constexpr uint32_t WIFI_WAIT_MS = 15000;     // ให้เราเตอร์ตอบก่อนสลับไปลองอีกชื่อ
constexpr uint32_t RADIO_RETRY_MS = 4000;    // หน่วงก่อนลองเปิดสตรีมรอบใหม่
constexpr uint32_t AUDIO_GAP_MS = 5000;     // รอข้อมูล MP3 ช่วงที่อินเทอร์เน็ตสะดุดได้นานสุดต่อรอบอ่าน
constexpr uint32_t START_TIMEOUT_MS = 18000;  // จำกัดเวลารอเปิดสตรีมและเริ่มถอดรหัส
constexpr size_t PREBUFFER_BYTES = 12 * 1024; // สะสมข้อมูลก่อนเริ่มเล่นเพื่อลดเสียงขาด
constexpr size_t PSRAM_BUFFER_BYTES = 64 * 1024;  // ขนาดบัฟเฟอร์เมื่อเปิด PSRAM
constexpr size_t RAM_BUFFER_BYTES = 24 * 1024;    // ขนาดสำรองเมื่อใช้ RAM ภายใน
constexpr uint16_t BACKGROUND = 0x08C4;      // สีพื้นจอแบบ RGB565
constexpr uint16_t WHITE = 0xFFFF;           // สีขาวสำหรับตัวหนังสือ
constexpr uint16_t CYAN = 0x5EDF;            // สีฟ้าสำหรับสถานะกำลังเชื่อมต่อ
constexpr uint16_t GREEN = 0x6FAD;           // สีเขียวสำหรับสถานะเริ่มส่งเสียง
constexpr uint16_t ORANGE = 0xFD48;          // สีส้มสำหรับสถานะรอลองใหม่

class RadioDisplay : public lgfx::LGFX_Device {  // ตั้งไดรเวอร์จอเฉพาะ ESP32-CYD S3 สีดำ
  lgfx::Bus_SPI bus;                             // เก็บการตั้งค่าสาย SPI ของจอ
  lgfx::Panel_ILI9341 panel;                     // ระบุชิปควบคุมจอเป็น ILI9341
public:                                        // เปิดให้ setup สร้างและใช้ออบเจ็กต์จอได้
  RadioDisplay() {                              // ตั้งผังขาจอก่อนเรียก init
    auto spi = bus.config();                    // อ่านโครงสร้างค่าตั้งต้นของบัส
    spi.spi_host = SPI2_HOST;                   // เลือกตัวควบคุม SPI2
    spi.spi_mode = 0;                           // ใช้รูปแบบสัญญาณ SPI mode 0
    spi.freq_write = 40000000;                  // ตั้งความถี่เขียนจอเป็น 40 MHz
    spi.freq_read = 16000000;                   // ตั้งความถี่อ่านจอให้ตรงกับผังบอร์ดเดิม
    spi.spi_3wire = false;                      // ใช้สายส่งและสายรับของ SPI แยกกัน
    spi.use_lock = true;                        // ให้ไลบรารีล็อกบัสระหว่างเขียนจอ
    spi.pin_sclk = 12;                          // ขานาฬิกา SPI ของจอ
    spi.pin_mosi = 11;                          // ขาส่งข้อมูลจากบอร์ดไปจอ
    spi.pin_miso = 13;                          // ขารับข้อมูลจากจอ
    spi.pin_dc = 46;                            // ขาแยกคำสั่งและข้อมูลพิกเซล
    spi.dma_channel = SPI_DMA_CH_AUTO;           // ให้ LovyanGFX จัดช่อง DMA สำหรับจอ
    bus.config(spi);                            // บันทึกค่าบัสที่แก้แล้ว
    panel.setBus(&bus);                         // ผูกจอเข้ากับบัส SPI นี้
    auto lcd = panel.config();                  // อ่านค่าตั้งต้นของจอ
    lcd.pin_cs = 10;                            // ขาเลือกจอ ILI9341
    lcd.pin_rst = -1;                           // ผังนี้ไม่มีขารีเซ็ตจอแยก
    lcd.pin_busy = -1;                          // จอรุ่นนี้ไม่มีขาสัญญาณ busy แยก
    lcd.panel_width = lcd.memory_width = 240;   // ความกว้างตามแนวตั้งดั้งเดิมของจอ
    lcd.panel_height = lcd.memory_height = 320; // ความสูงตามแนวตั้งดั้งเดิมของจอ
    lcd.offset_x = lcd.offset_y = lcd.offset_rotation = 0;  // ใช้ขอบพิกเซลและมุมพื้นฐานของแผงจอ
    lcd.readable = true;                        // เปิดการอ่านผ่านสาย MISO ตามผังจอเดิม
    lcd.invert = true;                          // ใช้โหมดสีกลับตามผังจอของบอร์ดสีดำ
    lcd.rgb_order = false;                      // ใช้ลำดับสีเดียวกับสเก็ตช์ S3 เดิม
    lcd.bus_shared = false;                     // บัสในโปรแกรมนี้ใช้กับจอเพียงตัวเดียว
    panel.config(lcd);                          // บันทึกค่าของแผงจอ
    setPanel(&panel);                           // ให้คำสั่งวาดของ LovyanGFX ใช้จอนี้
  }                                             // จบการตั้งผังขาจอ
};                                              // จบคลาสจอ

RadioDisplay lcd;                                 // สร้างจอไว้ให้ setup และ loop ใช้ร่วมกัน
StreamBufferHandle_t mp3Buffer = nullptr;          // อ้างถึงบัฟเฟอร์ไบต์ MP3
StaticStreamBuffer_t bufferControl;                // เก็บตัวควบคุมบัฟเฟอร์ที่ใช้พื้นที่ข้อมูลใน PSRAM
TaskHandle_t networkTaskHandle = nullptr;          // ใช้ส่งสัญญาณเริ่มรับสตรีมให้กับงานเครือข่าย
std::atomic<bool> networkBusy{false};               // จริงเมื่อกำลังเปิดหรือรับสตรีม
std::atomic<bool> cancelStream{false};              // สัญญาณขอให้จบรอบรับข้อมูลปัจจุบัน
std::atomic<int> lastHttpCode{0};                   // เก็บผล HTTP ล่าสุดเพื่อใช้ตรวจปัญหาใน Serial
bool hardwareReady = false;                         // จริงเมื่อสร้างงานและตรวจวงจรเสียงแล้ว
bool wifiWasConnected = false;                      // ใช้จับเหตุการณ์ Wi-Fi เพิ่งต่อสำเร็จหรือเพิ่งหลุด
bool useFallbackSsid = false;                       // เท็จใช้ชื่อหลัก จริงใช้ชื่อสำรอง
bool streamRequested = false;                      // จริงเมื่อขอเปิดสตรีมและกำลังรอหรือเล่นอยู่
bool decoderStarted = false;                        // จริงเมื่อเริ่มตัวถอดรหัส MP3 สำเร็จ
bool playingShown = false;                          // กันการวาดสถานะ ON AIR ซ้ำทุกครั้งที่มีเสียง
uint32_t wifiAttemptAt = 0;                         // เวลาที่เริ่มต่อ Wi-Fi รอบปัจจุบัน
uint32_t radioAttemptAt = 0;                        // เวลาที่เริ่มเปิดสตรีมรอบปัจจุบัน
uint32_t radioRetryAt = 0;                          // เวลาที่เริ่มลองสตรีมใหม่ได้
uint32_t audioProgressAt = 0;                       // เวลาที่ส่ง PCM ได้ล่าสุดหรือเริ่มรอเสียงชุดแรก

void drawStatus(const char *state, const char *detail, uint16_t color) {  // เปลี่ยนเฉพาะแถบสถานะบนจอ
  lcd.fillRoundRect(12, 132, 296, 63, 10, 0x1967);                         // ล้างข้อความเดิมในกรอบสถานะ
  lcd.setTextColor(color);                                                 // เลือกสีให้ตรงกับสถานะ
  lcd.drawCentreString(state, 160, 141, &fonts::Font2);                    // วาดสถานะหลักตรงกลาง
  lcd.setTextColor(WHITE);                                                 // ใช้ตัวหนังสือขาวสำหรับรายละเอียด
  lcd.drawCentreString(detail, 160, 172, &fonts::Font0);                   // แสดง IP หรือสาเหตุที่กำลังรอ
  Serial.printf("[STATUS] %s | %s\n", state, detail);                       // ส่งข้อความเดียวกันไป Serial Monitor
}                                                                         // จบการแสดงสถานะ

bool codecWrite(uint8_t reg, uint8_t value) {       // ส่งค่าหนึ่งไบต์ไปยังรีจิสเตอร์ของ ES8311
  Wire.beginTransmission(CODEC_ADDR);             // เริ่มส่งข้อมูลไปที่อยู่ของ ES8311
  Wire.write(reg);                                // เลือกเลขรีจิสเตอร์
  Wire.write(value);                              // ส่งค่าที่ต้องการตั้ง
  return Wire.endTransmission() == 0;              // ค่า 0 แปลว่าชิปตอบรับการเขียน
}                                                 // จบการเขียน I2C

bool configureCodec() {                            // ใช้ชุดค่า DAC เดียวกับบอร์ด S3 สีดำของงานเดิม
  if (!codecWrite(0x00, 0x1F)) return false;        // เริ่มลำดับรีเซ็ตซอฟต์แวร์ของชิปเสียง
  delay(10);                                       // รอให้รีเซ็ตเสร็จ
  if (!codecWrite(0x00, 0x00)) return false;        // ปล่อยรีเซ็ตก่อนเริ่มตั้งค่า
  delay(10);                                       // เว้นช่วงให้ชิปพร้อมรับคำสั่ง
  const uint8_t registers[][2] = {                 // แต่ละแถวเก็บคู่เลขรีจิสเตอร์และค่าที่เขียน
    {0x00, 0x80}, {0x01, 0x3F},                    // เริ่มชิปและเปิดวงจรนาฬิกา
    {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10},      // ตั้งทางเดิน MCLK และอัตราสุ่มภายในชิป
    {0x05, 0x00}, {0x06, 0x03},                    // ตั้งกลุ่มตัวหารนาฬิกา
    {0x07, 0x00}, {0x08, 0xFF},                    // ตั้งค่ารีจิสเตอร์อัตราส่วนเฟรมเสียงของชิป
    {0x09, 0x0C}, {0x0A, 0x0C},                    // เลือกรูปแบบพอร์ตเสียง I2S แบบ 16 บิต
    {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00},      // ตั้งภาคจ่ายไฟและเปิดทางเดินเอาต์พุตเสียง
    {0x13, 0x10}, {0x14, 0x1A},                    // ใช้ค่าภาคแอนะล็อกตามชุดตั้งค่าของบอร์ด
    {0x31, 0x00}, {0x32, 0xBF}, {0x37, 0x08},      // ตั้ง DAC ออกจาก mute และใช้ระดับอ้างอิงของบอร์ด
  };                                               // จบตารางรีจิสเตอร์
  for (const auto &entry : registers) {             // เขียนรีจิสเตอร์ทีละคู่ตามลำดับ
    if (!codecWrite(entry[0], entry[1])) return false;  // แจ้งว่าตั้งค่าไม่สำเร็จทันทีเมื่อชิปไม่ตอบ
  }                                                 // จบการวนตาราง
  return true;                                      // เขียนค่าครบทุกคู่
}                                                   // จบการเตรียม ES8311

class CydSpeaker : public AudioOutput {              // รับ PCM จากตัวถอดรหัสแล้วส่งออกลำโพง
  int16_t pcm[512];                                  // เก็บ 256 เฟรมเสียงแบบสองช่องก่อนส่ง I2S
  size_t used = 0;                                   // จำนวนตัวอย่าง PCM ในบล็อก
  size_t sent = 0;                                   // จำนวนไบต์ที่ DMA รับไปแล้วในบล็อกปัจจุบัน
  bool installed = false;                            // จำว่าติดตั้งไดรเวอร์ I2S แล้วหรือยัง
  bool failed = false;                               // เก็บข้อผิดพลาดของภาคเสียง
  bool active = false;                               // จริงหลัง DMA รับตัวอย่างเสียงชุดแรก

  bool outputError() {                               // รวมการปิดเอาต์พุตเมื่อพบข้อผิดพลาด
    stop();                                          // ปิดแอมป์และคืนไดรเวอร์ I2S
    failed = true;                                   // ให้ loop ตรวจพบและแสดงสาเหตุ
    return false;                                    // บอกตัวถอดรหัสว่าส่งเสียงต่อไม่ได้
  }                                                  // จบการจัดการข้อผิดพลาด

public:                                              // ส่วนที่ ESP8266Audio เรียกใช้งานได้
  bool begin() override {                            // ถูกเรียกเมื่อเริ่มถอดรหัสสตรีมรอบใหม่
    stop();                                          // เคลียร์บล็อกเสียงและทรัพยากรจากรอบก่อน
    failed = false;                                  // เริ่มรอบใหม่ด้วยสถานะปกติ
    return true;                                     // รอให้ตัวถอดรหัสแจ้ง sample rate จริง
  }                                                  // จบการเริ่มเอาต์พุต

  bool SetRate(int rate) override {                   // รับ sample rate จากเฟรม MP3 ที่ถอดรหัสได้
    if (installed && hertz == rate) return true;     // ใช้การตั้งเดิมถ้าอัตราสุ่มเท่าเดิม
    stop();                                          // ปิด I2S เดิมก่อนเปลี่ยนความถี่
    if (rate < 8000 || rate > 48000) return outputError();  // จำกัดช่วงอัตราสุ่มของสเก็ตช์
    i2s_config_t config = {};                        // เริ่มโครงสร้าง I2S ด้วยค่า 0
    config.mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX);  // ให้บอร์ดสร้างนาฬิกาและส่งเสียงออก
    config.sample_rate = rate;                       // ใช้อัตราสุ่มเดียวกับ MP3
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;      // ส่ง PCM ตัวอย่างละ 16 บิต
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;     // ส่งสองช่องเพื่อให้ตรงกับรูปแบบของ ES8311
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;  // ใช้จังหวะบิตตามมาตรฐาน I2S
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;    // ใช้อินเทอร์รัปต์ I2S ระดับ 1
    config.dma_buf_count = 8;                        // ใช้บัฟเฟอร์ DMA แปดชุด
    config.dma_buf_len = 256;                        // แต่ละชุดเก็บ 256 เฟรม
    config.tx_desc_auto_clear = true;                // ส่งความเงียบเมื่อ DMA ขาดข้อมูล
    config.fixed_mclk = rate * 256;                  // ใช้ MCLK อัตราส่วน 256 เท่าตามผังเสียงเดิม
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;      // กำหนดอัตราส่วน MCLK ที่ไดรเวอร์คำนวณ
    if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) return outputError();  // จอง I2S พอร์ต 0
    installed = true;                                // ให้ stop คืนไดรเวอร์ได้ตั้งแต่จุดนี้
    i2s_pin_config_t pins = {};                      // เริ่มโครงสร้างขา I2S ด้วยค่า 0
    pins.mck_io_num = I2S_MCLK;                      // ต่อขานาฬิกาหลักของ ES8311
    pins.bck_io_num = I2S_BCLK;                      // ต่อขานาฬิกาบิต
    pins.ws_io_num = I2S_LRCK;                       // ต่อขาเลือกช่องเสียง
    pins.data_out_num = I2S_DOUT;                    // ส่ง PCM ผ่านสายข้อมูลออก
    pins.data_in_num = I2S_PIN_NO_CHANGE;             // ใช้ภาคส่งเสียงอย่างเดียว
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK || !configureCodec()) return outputError();  // ตั้งขาแล้วเตรียม DAC
    i2s_zero_dma_buffer(I2S_NUM_0);                  // ล้างข้อมูลเก่าที่อาจค้างอยู่ใน DMA
    hertz = rate;                                     // จำอัตราสุ่มเพื่อใช้ตรวจตอนเปลี่ยนเฟรม
    Serial.printf("[I2S] ES8311 rate=%d Hz volume=%d%%\n", rate, VOLUME_PERCENT);  // รายงานว่าเปิดวงจรส่งเสียงแล้ว
    return true;                                     // พร้อมรับ PCM จากตัวถอดรหัส
  }                                                  // จบการตั้ง sample rate

  bool ConsumeSample(int16_t sample[2]) override {    // ถูกเรียกสำหรับ PCM หนึ่งเฟรม
    if (failed) return false;                        // หยุดรับเมื่อฮาร์ดแวร์รายงานข้อผิดพลาด
    if (!installed) return true;                     // ข้ามตัวอย่างเริ่มต้นก่อนทราบ sample rate
    if (used == sizeof(pcm) / sizeof(pcm[0])) {       // ส่งบล็อกเดิมเมื่อบัฟเฟอร์เต็ม
      size_t written = 0;                            // รับจำนวนไบต์ที่ DMA รับได้จริง
      const uint8_t *bytes = reinterpret_cast<const uint8_t *>(pcm);  // มอง PCM เป็นไบต์เพื่อส่งต่อจากตำแหน่งที่ค้าง
      esp_err_t result = i2s_write(I2S_NUM_0, bytes + sent, sizeof(pcm) - sent, &written, 0);  // ส่งโดยไม่รอคิว DMA
      if (result != ESP_OK && result != ESP_ERR_TIMEOUT) return outputError();  // แยก DMA เต็มจากข้อผิดพลาดของไดรเวอร์
      sent += written;                               // จดตำแหน่งที่เขียนสำเร็จเพื่อส่งส่วนที่เหลือครั้งหน้า
      if (written) audioProgressAt = millis();       // ใช้จับกรณีรับไบต์ได้แต่ถอดเสียงไม่สำเร็จนานผิดปกติ
      if (written && !active) {                      // เปิดลำโพงเมื่อมี PCM ส่งออกแล้วเท่านั้น
        digitalWrite(AMP_PIN, LOW);                  // สั่งเปิดแอมป์ลำโพงของบอร์ด
        active = true;                               // ให้หน้าจอขึ้น ON AIR เมื่อกลับไปที่ loop
      }                                              // จบการเปิดแอมป์ครั้งแรก
      if (sent < sizeof(pcm)) return false;          // ให้ตัวถอดรหัสรอเมื่อ DMA ยังรับบล็อกได้ไม่ครบ
      used = sent = 0;                               // บล็อกเดิมส่งครบแล้วจึงเริ่มเติมบล็อกใหม่
    }                                                // จบการส่งบล็อกเต็ม
    int32_t mono = channels == 1 ? sample[0] : (int32_t(sample[0]) + sample[1]) / 2;  // รวมซ้ายและขวาเข้าลำโพงตัวเดียว
    int16_t level = int16_t(mono * VOLUME_PERCENT / 100);  // ปรับแอมพลิจูด PCM ตามเปอร์เซ็นต์ในโค้ด
    pcm[used++] = level;                             // เก็บเสียงช่องแรก
    pcm[used++] = level;                             // ส่งค่าเดียวกันในอีกช่องเพื่อให้ DAC รับโมโนได้
    return true;                                     // รับเฟรมนี้แล้วและพร้อมรับเฟรมถัดไป
  }                                                  // จบการส่งตัวอย่างเสียง

  bool stop() override {                             // ปิดลำโพงเมื่อสตรีมจบหรือมีการเชื่อมต่อใหม่
    digitalWrite(AMP_PIN, HIGH);                     // ปิดแอมป์ก่อนล้างข้อมูลเสียง
    if (installed) {                                 // คืนทรัพยากรเฉพาะเมื่อไดรเวอร์ถูกติดตั้งไว้
      i2s_zero_dma_buffer(I2S_NUM_0);                 // ล้าง PCM ค้างใน DMA
      i2s_driver_uninstall(I2S_NUM_0);                // คืนพอร์ต I2S และหน่วยความจำ DMA
    }                                                // จบการคืนไดรเวอร์
    installed = active = false;                     // เคลียร์สถานะพอร์ตและสถานะกำลังส่งเสียง
    used = sent = 0;                                  // ทิ้งบล็อกที่ยังส่งไม่ครบ
    return true;                                     // เอาต์พุตหยุดแล้ว
  }                                                  // จบการหยุดเอาต์พุต
  bool hasError() const { return failed; }            // ให้ loop อ่านสถานะผิดพลาด
  bool hasPcm() const { return active; }               // ให้ loop ตรวจว่ามี PCM เข้า DMA แล้ว
};                                                    // จบคลาสลำโพง

CydSpeaker speaker;                                   // สร้างปลายทางเสียง ES8311
AudioGeneratorMP3 decoder;                             // สร้างตัวถอดรหัส MP3 ที่นำกลับมาใช้เมื่อสตรีมหลุด

class RadioSource : public AudioFileSource, public Stream {  // เป็นทั้งปลายรับ HTTP และแหล่งข้อมูลของตัวถอดรหัส
  uint32_t position = 0;                               // จำจำนวนไบต์ MP3 ที่ส่งให้ตัวถอดรหัสแล้ว
  uint32_t metaInterval = 0;                           // ระยะไบต์ระหว่างข้อความ ICY ของสถานี
  uint32_t audioLeft = 0;                              // จำนวนไบต์เสียงก่อนถึง ICY รอบถัดไป
  uint32_t metaLeft = 0;                               // จำนวนไบต์ข้อความ ICY ที่ต้องข้าม

public:                                               // ส่วนที่ Arduino loop และงานเครือข่ายเรียกใช้
  void resetPosition() { position = 0; }               // เริ่มนับตำแหน่งใหม่ก่อนถอดรหัสสตรีม
  void setMetadataInterval(uint32_t interval) {        // ตั้งรูปแบบ ICY จากหัวตอบกลับของเซิร์ฟเวอร์
    metaInterval = audioLeft = interval;               // เริ่มจากข้อมูลเสียงจำนวนหนึ่งช่วง
    metaLeft = 0;                                      // เคลียร์ข้อความ ICY ที่อาจค้างจากรอบเดิม
  }                                                    // จบการตั้งค่า ICY
  bool isOpen() override {                             // ตัวถอดรหัสใช้เช็กว่าอ่านสตรีมได้หรือยัง
    return networkBusy.load() || xStreamBufferBytesAvailable(mp3Buffer) > 0;  // เล่นต่อได้ขณะรับข้อมูลหรือยังมีข้อมูลค้าง
  }                                                    // จบการตรวจสตรีม
  uint32_t getPos() override { return position; }      // คืนตำแหน่งอ่านให้ตัวถอดรหัสใช้แจ้งข้อผิดพลาด
  bool close() override { cancelStream.store(true); return true; }  // ส่งสัญญาณหยุดรับข้อมูลเมื่อปิดตัวถอดรหัส

  uint32_t read(void *data, uint32_t length) override {  // อ่าน MP3 ตามจำนวนไบต์ที่ libmad ต้องการ
    size_t total = 0;                                  // นับจำนวนไบต์ที่อ่านได้ในรอบนี้
    uint32_t lastDataAt = millis();                     // จับเวลารอข้อมูลจากบัฟเฟอร์
    auto *out = static_cast<uint8_t *>(data);           // ใช้ตัวชี้ไบต์สำหรับเลื่อนตำแหน่งปลายทาง
    while (total < length && !cancelStream.load()) {    // อ่านต่อจนได้ครบหรือมีคำสั่งหยุด
      if (millis() - audioProgressAt >= START_TIMEOUT_MS) break;  // จบรอบเมื่อไม่มีเฟรมที่ถอดได้ตามเวลา
      size_t got = xStreamBufferReceive(mp3Buffer, out + total, length - total, pdMS_TO_TICKS(40));  // รอข้อมูลช่วงสั้นจากงานเครือข่าย
      total += got;                                    // เพิ่มจำนวนไบต์ที่อ่านได้
      if (got) lastDataAt = millis();                  // เริ่มนับเวลารอใหม่เมื่อมีข้อมูลเข้ามา
      else if (!networkBusy.load() || WiFi.status() != WL_CONNECTED || millis() - lastDataAt >= AUDIO_GAP_MS) break;  // คืนค่าที่มีเมื่อสตรีมขาดช่วงนาน
    }                                                  // จบการอ่านบัฟเฟอร์
    position += total;                                // อัปเดตตำแหน่งอ่านสะสม
    return total;                                      // คืนจำนวนไบต์จริงให้ตัวถอดรหัส
  }                                                    // จบการอ่าน MP3

  int available() override { return 0; }               // ฝั่ง Stream ใช้เป็นปลายทางเขียน HTTP
  int read() override { return -1; }                   // การอ่าน MP3 ใช้เมธอด read ที่รับบัฟเฟอร์ด้านบน
  int peek() override { return -1; }                   // ปลายทาง HTTP ไม่มีไบต์ให้ดูผ่าน Stream
  void flush() override {}                             // งานรับ HTTP ส่งข้อมูลลงบัฟเฟอร์ทันที
  size_t write(uint8_t byte) override { return write(&byte, 1); }  // ใช้เส้นทางเดียวกันเมื่อมีการเขียนหนึ่งไบต์

  size_t write(const uint8_t *data, size_t length) override {  // HTTPClient เรียกเพื่อส่งเนื้อข้อมูลที่แกะ chunk แล้ว
    size_t offset = 0;                                 // นับไบต์ที่จัดการไปแล้วในก้อน HTTP
    while (offset < length && !cancelStream.load()) {    // รับข้อมูลต่อจนจบก้อนหรือถูกขอให้หยุด
      if (metaLeft) {                                  // ช่วงนี้เป็นข้อความ ICY
        size_t skip = min(length - offset, size_t(metaLeft));  // ข้ามเท่าที่มีในก้อนปัจจุบัน
        offset += skip;                                // เลื่อนตำแหน่งข้ามข้อความ
        metaLeft -= skip;                              // ลดจำนวนไบต์ข้อความที่เหลือ
        if (!metaLeft) audioLeft = metaInterval;        // จบข้อความแล้วเริ่มนับไบต์เสียงช่วงใหม่
      } else if (metaInterval && !audioLeft) {          // อ่านไบต์บอกความยาวข้อความ ICY
        metaLeft = uint32_t(data[offset++]) * 16;       // หน่วยความยาว ICY เป็นก้อนละ 16 ไบต์
        if (!metaLeft) audioLeft = metaInterval;        // สถานีส่งค่า 0 เมื่อรอบนี้ไม่มีข้อความ
      } else {                                         // ส่วนนี้เป็นข้อมูล MP3
        size_t count = metaInterval ? min(length - offset, size_t(audioLeft)) : length - offset;  // จำกัดให้ถึงปลายช่วงเสียงเท่านั้น
        size_t wrote = xStreamBufferSend(mp3Buffer, data + offset, count, pdMS_TO_TICKS(50));  // ส่งข้อมูลลงบัฟเฟอร์พร้อมรอที่ว่างได้
        offset += wrote;                               // ขยับตามจำนวนไบต์ที่ส่งได้จริง
        if (metaInterval) audioLeft -= wrote;           // ติดตามจุดที่ข้อมูลเสียงจะสลับไปเป็น ICY
      }                                                // จบการแยกเสียงกับข้อความ ICY
    }                                                  // จบการรับก้อนข้อมูล HTTP
    return offset;                                     // คืนจำนวนไบต์ที่จัดการแล้วให้ HTTPClient
  }                                                    // จบการเขียนข้อมูล MP3
};                                                     // จบคลาสเชื่อม HTTP กับ MP3

RadioSource source;                                     // บัฟเฟอร์นี้มีงานเครือข่ายเขียนและ Arduino loop อ่าน

void receiveStation() {                                 // เปิด HTTPS และส่งข้อมูลเสียงไปที่บัฟเฟอร์
  if (cancelStream.load()) return;                       // ข้ามเมื่อมีคำสั่งหยุดก่อนเปิดสตรีม
  WiFiClientSecure client;                               // สร้างไคลเอนต์สำหรับ URL HTTPS
  client.setInsecure();                                  // สตรีมสาธารณะใช้ TLS โดยข้ามการตรวจใบรับรองเพื่อเริ่มได้ทันที
  client.setHandshakeTimeout(6);                         // จำกัดเวลาเจรจา TLS เป็น 6 วินาที
  HTTPClient http;                                       // สร้างตัวอ่านหัว HTTP และเนื้อสตรีม
  http.setConnectTimeout(4500);                          // จำกัดเวลารอเปิดการเชื่อมต่อ TCP
  http.setTimeout(4500);                                 // จำกัดเวลารอข้อมูล HTTP
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // อนุญาตให้คำขอ GET ตาม URL ที่สถานีเปลี่ยนเส้นทาง
  http.setRedirectLimit(3);                              // จำกัดจำนวนครั้งที่ตามการเปลี่ยนเส้นทาง
  if (!http.begin(client, RADIO_URL)) {                  // เปิดใช้ไคลเอนต์กับ URL ที่กำหนด
    lastHttpCode.store(-1);                              // บอก loop ว่าเริ่มคำขอไม่สำเร็จ
    return;                                             // จบรอบรับข้อมูล
  }                                                     // จบการตรวจเริ่ม HTTP
  const char *headers[] = {"Content-Type", "icy-metaint"};  // เลือกหัวตอบกลับที่ต้องใช้กับเสียง
  http.collectHeaders(headers, 2);                       // ให้ HTTPClient เก็บสองหัวนี้หลัง GET
  http.addHeader("Icy-MetaData", "0");                    // ขอรับเสียงอย่างเดียว
  http.addHeader("Accept-Encoding", "identity");         // ขอข้อมูลเสียงโดยตรงเพื่อให้ถอด MP3 ได้
  http.setUserAgent("COOL93-CYD-S3/1.0");                 // ระบุชื่อไคลเอนต์ให้เซิร์ฟเวอร์
  int code = http.GET();                                 // ขอข้อมูลสตรีมจากสถานี
  lastHttpCode.store(code);                              // เก็บรหัสตอบกลับให้ loop ตรวจได้
  String type = http.header("Content-Type");             // อ่านชนิดข้อมูลที่เซิร์ฟเวอร์ส่งมา
  type.toLowerCase();                                    // ปรับตัวอักษรเพื่อเทียบชื่อชนิดข้อมูล
  Serial.printf("[HTTP] %d type=%s\n", code, type.c_str());  // แสดงผลการขอสตรีมใน Serial
  bool audioType = !type.startsWith("text/") && type.indexOf("html") < 0 && type.indexOf("json") < 0 &&  // ตรวจว่าไม่ได้รับหน้าเว็บหรือข้อความ API
                   type.indexOf("mpegurl") < 0 && type.indexOf("aac") < 0;  // รับสตรีม MP3 และชนิดทั่วไปที่อาจใช้แทน MP3
  if (code == HTTP_CODE_OK && audioType && !cancelStream.load()) {  // เริ่มรับเนื้อสตรีมหลัง HTTP ตอบสำเร็จ
    long interval = http.header("icy-metaint").toInt();   // อ่านช่วง ICY กรณีที่สถานียังส่งข้อความแทรก
    source.setMetadataInterval(interval > 0 ? uint32_t(interval) : 0);  // ตั้งให้แยกข้อมูลเสียงจากข้อความ
    int result = http.writeToStream(&source);            // แกะเนื้อ HTTP แบบ chunked แล้วเขียน MP3 เข้าบัฟเฟอร์
    Serial.printf("[HTTP] stream ended result=%d\n", result);  // แสดงผลเมื่องานรับข้อมูลจบรอบ
  } else if (code == HTTP_CODE_OK && !audioType) {        // แยกกรณี URL ตอบสำเร็จแต่ส่งข้อมูลคนละชนิด
    Serial.println("[HTTP] expected a direct MP3 stream");  // แจ้งให้ตรวจ URL และชนิดสตรีม
  }                                                       // จบการตรวจและรับเนื้อสตรีม
  http.end();                                            // ปิดการเชื่อมต่อเมื่อสตรีมจบ
}                                                        // จบการรับสถานี

void networkTask(void *) {                               // งานนี้ทำเครือข่ายแยกจากการถอดเสียง
  for (;;) {                                             // รอและรับสตรีมซ้ำเมื่อได้รับคำสั่งใหม่
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);               // หลับจนกว่า Arduino loop จะสั่งเริ่ม
    receiveStation();                                    // รับข้อมูลเสียงของสถานีหนึ่งรอบ
    networkBusy.store(false);                            // ให้ loop เริ่มรอบถัดไปได้หลังปิด HTTP แล้ว
  }                                                      // กลับไปรอคำสั่งรอบใหม่
}                                                        // จบงานเครือข่าย

void beginWifi(bool fallback) {                          // เริ่มต่อ Wi-Fi ด้วยชื่อที่เลือก
  useFallbackSsid = fallback;                            // จำชื่อที่กำลังทดลองเชื่อมต่อ
  const char *ssid = fallback ? WIFI_FALLBACK : WIFI_NAME; // เลือกชื่อหลักหรือชื่อสำรอง
  WiFi.disconnect();                                     // ยกเลิกการต่อชื่อเดิมก่อนเปลี่ยนชื่อ
  WiFi.begin(ssid, WIFI_PASS);                           // เริ่มต่อ Wi-Fi ด้วยค่าต้นไฟล์
  wifiAttemptAt = millis();                              // เก็บเวลาเริ่มรอบเพื่อสลับชื่อเมื่อรอนาน
  drawStatus("WIFI CONNECTING", ssid, CYAN);              // แสดงเฉพาะชื่อ Wi-Fi บนจอและ Serial
}                                                        // จบการเริ่ม Wi-Fi

void stopRadio() {                                       // หยุดเสียงและขอปิดสตรีมรอบปัจจุบัน
  cancelStream.store(true);                              // ปลุกทางออกของงานรับข้อมูลเมื่อบัฟเฟอร์เต็ม
  if (decoderStarted) decoder.stop();                    // คืนหน่วยความจำตัวถอดรหัสและปิดลำโพง
  streamRequested = decoderStarted = playingShown = false;  // เคลียร์สถานะก่อนเริ่มสตรีมใหม่
}                                                        // จบการหยุดวิทยุ

void retryRadio(const char *reason) {                    // เตรียมลองเปิดสตรีมใหม่เมื่อเล่นต่อไม่ได้
  stopRadio();                                           // ปิดการเล่นและคืนทรัพยากรรอบเดิม
  radioRetryAt = millis() + RADIO_RETRY_MS;              // เว้นช่วงก่อนขอสตรีมอีกครั้ง
  Serial.printf("[RETRY] HTTP=%d\n", lastHttpCode.load()); // แสดงรหัส HTTP ล่าสุดเพื่อวิเคราะห์
  drawStatus("STREAM RETRY", reason, ORANGE);             // แสดงสาเหตุที่จอ
}                                                        // จบการเตรียมลองใหม่

void startRadio() {                                      // เริ่มรอบรับ MP3 หลังงานก่อนหน้าจบแล้ว
  xStreamBufferReset(mp3Buffer);                         // ทิ้งไบต์เสียงของการเชื่อมต่อก่อนหน้า
  source.resetPosition();                               // เริ่มนับไบต์ MP3 จากศูนย์
  cancelStream.store(false);                             // อนุญาตให้รับข้อมูลในรอบใหม่
  lastHttpCode.store(0);                                 // ล้างรหัส HTTP เก่าก่อนขอสตรีม
  radioAttemptAt = audioProgressAt = millis();          // เริ่มจับเวลารอบรับสตรีมและรอ PCM ชุดแรก
  streamRequested = true;                               // บอก loop ว่ากำลังรอหรือเล่นสถานีนี้
  networkBusy.store(true);                               // จองสิทธิ์รับสตรีมไว้ก่อนส่งสัญญาณให้ task
  xTaskNotifyGive(networkTaskHandle);                    // ปลุกงานเครือข่ายให้เปิด URL
  drawStatus("BUFFERING", WiFi.localIP().toString().c_str(), CYAN);  // แสดง IP และรอข้อมูลเสียงชุดแรก
}                                                        // จบการเริ่มวิทยุ

void setup() {                                            // ทำงานหนึ่งครั้งเมื่อเปิดบอร์ด
  pinMode(AMP_PIN, OUTPUT);                               // ตั้ง GPIO ควบคุมแอมป์เป็นเอาต์พุต
  digitalWrite(AMP_PIN, HIGH);                           // ปิดลำโพงระหว่างเริ่มระบบ
  Serial.begin(115200);                                   // เปิด Serial Monitor สำหรับดูสถานะ
  Serial.printf("\n[BOOT] COOL 93 CYD S3 %s\n", BUILD);    // พิมพ์ชื่อสเก็ตช์ รุ่น และเวลาคอมไพล์
  lcd.init();                                             // เริ่มจอ ILI9341
  lcd.setRotation(3);                                     // ใช้จอแนวนอนตามผังบอร์ดสีดำเดิม
  pinMode(LCD_BL, OUTPUT);                                // ตั้งขาไฟส่องหลังเป็นเอาต์พุต
  digitalWrite(LCD_BL, HIGH);                            // เปิดไฟส่องหลัง
  lcd.fillScreen(BACKGROUND);                            // เติมสีพื้นจอครั้งเดียว
  lcd.setTextColor(WHITE);                                // ใช้สีขาวสำหรับชื่อสถานีและข้อมูลรุ่น
  lcd.drawCentreString("COOLfahrenheit", 160, 22, &fonts::Font2);  // แสดงชื่อสถานีด้านบน
  lcd.setTextColor(CYAN);                                 // เลือกสีฟ้าสำหรับเลขคลื่น
  lcd.drawCentreString("93.0", 160, 51, &fonts::Font7);    // วาดเลขสถานีขนาดใหญ่
  lcd.setTextColor(WHITE);                                // คืนสีตัวอักษรเป็นสีขาว
  lcd.drawCentreString(BUILD, 160, 216, &fonts::Font0);   // แสดงรุ่นและวันเวลาคอมไพล์ด้านล่าง
  if (!Wire.begin(CODEC_SDA, CODEC_SCL, 100000)) {         // เปิด I2C ของชิปเสียงที่ 100 kHz
    drawStatus("I2C ERROR", "SDA 16 / SCL 15", ORANGE);    // แสดงข้อผิดพลาดหากเปิดบัสไม่ได้
    return;                                              // คงลำโพงไว้ในสถานะปิด
  }                                                      // จบการเปิดบัส I2C
  Wire.beginTransmission(CODEC_ADDR);                    // สอบถามว่าชิป ES8311 ตอบหรือไม่
  if (Wire.endTransmission() != 0) {                     // ตรวจการตอบที่ที่อยู่ 0x18
    drawStatus("ES8311 ERROR", "No response at I2C 0x18", ORANGE);  // แจ้งเมื่อผังบอร์ดหรือวงจรเสียงต่างจากรุ่นนี้
    return;                                              // หยุดการเริ่มระบบเสียง
  }                                                      // จบการตรวจชิปเสียง
  uint8_t *storage = static_cast<uint8_t *>(ps_malloc(PSRAM_BUFFER_BYTES + 1));  // จองที่เก็บ MP3 ใน PSRAM
  if (storage) {                                         // ใช้บัฟเฟอร์ใหญ่เมื่อมี PSRAM
    mp3Buffer = xStreamBufferCreateStatic(PSRAM_BUFFER_BYTES, 1, storage, &bufferControl);  // สร้างบัฟเฟอร์จากพื้นที่ที่จองไว้
    if (!mp3Buffer) free(storage);                       // คืนพื้นที่เมื่อสร้างบัฟเฟอร์ไม่สำเร็จ
  }                                                      // จบการเตรียม PSRAM
  if (!mp3Buffer) mp3Buffer = xStreamBufferCreate(RAM_BUFFER_BYTES, 1);  // ใช้ RAM ภายในขนาดเล็กเป็นทางสำรอง
  if (!mp3Buffer || xTaskCreatePinnedToCore(networkTask, "cool93-net", 10240, nullptr, 1, &networkTaskHandle, 0) != pdPASS) {  // สร้างงานรับ HTTPS ที่คอร์ 0
    drawStatus("BUFFER ERROR", "Enable OPI PSRAM", ORANGE); // แจ้งเมื่อสร้างบัฟเฟอร์หรืองานรับข้อมูลไม่ได้
    return;                                                // หยุดการเริ่มระบบ
  }                                                        // จบการเตรียมงานเครือข่าย
  WiFi.persistent(false);                                 // ให้ค่าของวิทยุชุดนี้อยู่ใน RAM ระหว่างใช้งาน
  WiFi.mode(WIFI_STA);                                    // ใช้โหมดต่อกับเราเตอร์
  WiFi.setAutoReconnect(false);                           // ให้สเก็ตช์จัดการสลับชื่อ Wi-Fi เอง
  WiFi.setSleep(false);                                   // ลดช่วงพักวิทยุ Wi-Fi ระหว่างรับเสียง
  hardwareReady = true;                                   // อนุญาตให้ loop เริ่มต่อและเล่นสตรีม
  beginWifi(false);                                       // ลองชื่อ Wi-Fi หลักก่อน
}                                                          // จบ setup

void loop() {                                              // วนดู Wi-Fi และส่งงานให้ตัวถอดรหัส
  if (!hardwareReady) { delay(100); return; }               // รออยู่ที่สถานะผิดพลาดหากเริ่มฮาร์ดแวร์ไม่สำเร็จ
  uint32_t now = millis();                                 // อ่านเวลาปัจจุบันสำหรับตรวจเวลารอ
  if (WiFi.status() != WL_CONNECTED) {                     // จัดการกรณีเพิ่งเปิดบอร์ดหรือ Wi-Fi หลุด
    if (wifiWasConnected) {                               // ทำเฉพาะครั้งแรกที่ตรวจว่าการเชื่อมต่อเดิมหลุด
      wifiWasConnected = false;                           // จำว่าออฟไลน์แล้ว
      stopRadio();                                        // ปิดสตรีมกับลำโพง
      beginWifi(useFallbackSsid);                         // ลองต่อชื่อที่ใช้สำเร็จครั้งล่าสุดก่อน
    } else if (now - wifiAttemptAt >= WIFI_WAIT_MS) {       // ถ้าต่อชื่อปัจจุบันนานเกินกำหนด
      beginWifi(!useFallbackSsid);                        // สลับไปลองอีกชื่อ
    }                                                     // จบการจัดการ Wi-Fi หลุด
    delay(10);                                            // ให้ Wi-Fi มีเวลาทำงานระหว่างรอ
    return;                                               // รอให้ Wi-Fi พร้อมก่อนเปิดสตรีม
  }                                                       // จบการตรวจ Wi-Fi
  if (!wifiWasConnected) {                                // Wi-Fi เพิ่งเชื่อมสำเร็จ
    wifiWasConnected = true;                              // ทำส่วนนี้เพียงครั้งเดียวต่อการเชื่อมต่อ
    radioRetryAt = now;                                   // ให้เริ่มเปิดสถานีได้ทันที
    Serial.printf("[WIFI] IP=%s\n", WiFi.localIP().toString().c_str());  // รายงาน IP ที่ได้รับจากเราเตอร์
  }                                                       // จบเหตุการณ์ Wi-Fi พร้อม
  if (!streamRequested) {                                // สตรีมยังไม่เริ่มหรือกำลังรอลองใหม่
    if (!networkBusy.load() && int32_t(now - radioRetryAt) >= 0) startRadio();  // เปิดสตรีมเมื่อรอบเก่าจบและครบเวลารอ
  } else if (!decoderStarted) {                           // เปิด HTTP แล้วและกำลังสะสม MP3 ชุดแรก
    if (xStreamBufferBytesAvailable(mp3Buffer) >= PREBUFFER_BYTES) {  // รอให้มี MP3 เพียงพอก่อนเริ่มถอดรหัส
      decoderStarted = decoder.begin(&source, &speaker);  // เชื่อมแหล่ง MP3 กับปลายทาง I2S
      if (!decoderStarted) retryRadio("MP3 start failed"); // ลองใหม่ถ้าตัวถอดรหัสเริ่มไม่ได้
    } else if (!networkBusy.load() || now - radioAttemptAt >= START_TIMEOUT_MS) {  // จบรอบหากไม่มีข้อมูลพร้อมเล่น
      retryRadio("Waiting for station data");             // แสดงสถานะและรอลองเปิดอีกครั้ง
    }                                                     // จบการรอ MP3 ชุดแรก
  } else {                                                // มีตัวถอดรหัสพร้อมทำงาน
    bool decoding = decoder.loop();                       // ถอด MP3 แล้วส่ง PCM ตามพื้นที่ว่างใน DMA
    bool stalled = millis() - audioProgressAt >= START_TIMEOUT_MS;  // จับกรณีเอาต์พุตไม่รับ PCM เป็นเวลานาน
    if (!decoding || speaker.hasError() || stalled) {      // ตรวจว่าต้องเปิดสตรีมรอบใหม่หรือยัง
      const char *reason = speaker.hasError() ? "I2S / ES8311 error" : (stalled ? "No PCM data" : "Stream disconnected");  // เลือกสาเหตุให้ตรงกับจุดที่หยุด
      retryRadio(reason);                                   // แจ้งสาเหตุและลองเปิดสถานีเดิมใหม่
    } else if (!playingShown && speaker.hasPcm()) {        // DMA รับเสียงชุดแรกแล้ว
      playingShown = true;                               // เปลี่ยนหน้าจอเพียงครั้งเดียวในรอบนี้
      drawStatus("ON AIR", WiFi.localIP().toString().c_str(), GREEN);  // แสดงว่าส่ง PCM ออก I2S แล้ว
    }                                                     // จบการตรวจสถานะเสียง
  }                                                       // จบการจัดการรอบสตรีม
  delay(1);                                               // ให้ FreeRTOS สลับไปทำ Wi-Fi และงานระบบอื่น
}                                                         // จบรอบ loop
