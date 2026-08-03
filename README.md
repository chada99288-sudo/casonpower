# CASON Power Safety Controller

โปรเจค ESP32-S3 สำหรับควบคุมระบบตัดต่อไฟ, แสดงสถานะ, ส่งแจ้งเตือน LINE และรับคำสั่งผ่าน LINE โดยใช้ Render เป็น server กลาง

## โครงสร้างระบบ

- ESP32-S3 อ่านสัญญาณ DI และควบคุม relay board ผ่าน I2C
- ESP32 ส่ง event ไป Render: `https://casonpower.onrender.com/api/alert`
- Render server ส่ง LINE และรับ LINE webhook
- ESP32 ดึงคำสั่งจาก Render ผ่าน `/api/command`
- ถ้า ESP32 หายเกินเวลาที่กำหนด Render จะตรวจ watchdog และแจ้งเตือน offline

## Hardware ปัจจุบัน

- Board: ESP32-S3 DevKitC-1
- Relay controller: TCA9554 I2C address `0x20`
- I2C SDA: GPIO42
- I2C SCL: GPIO41
- DI1: GPIO4, dry contact NC, ปกติ LOW/0, ทำงาน HIGH/1
- DI2: GPIO5, dry contact NC, ปกติ LOW/0, ทำงาน HIGH/1

## Relay Mapping

- CH1: สั่ง main power / contactor
- CH2: ไฟเขียว สถานะปกติ
- CH3: ไฟเหลือง แจ้งเตือนระดับเล็กน้อย เช่น DI2 หรือรอกู้คืน
- CH4: ไฟแดง fault รุนแรง เช่น DI1
- CH5: เสียงเตือน เปิดพร้อมไฟแดงเท่านั้น

## Logic หลัก

- DI1 ACTIVE: ตัด CH1, เปิดไฟแดง CH4, เปิดเสียง CH5, ส่ง LINE
- DI1 กลับ NORMAL: รอ 30 วินาที แล้วเปิด CH1 กลับเอง ถ้า DI1 ยังปกติ
- DI2 ACTIVE: เปิดไฟเหลือง CH3 เท่านั้น ไม่ตัด CH1 และไม่เปิดเสียง
- สถานะปกติ: CH2 เขียว ON, CH3/CH4/CH5 OFF
- ไม่มีการกระพริบ relay เพื่อลดการทำงานถี่ของ relay board

## Wi-Fi Setup

ถ้ายังไม่มี Wi-Fi หรือกดคำสั่ง `WIFI_RESET` ระบบจะเปิด AP:

- SSID: `CASON-SETUP`
- Password: รหัสเฉพาะเครื่องที่สร้างจาก ESP32 MAC address หรือค่าที่ตั้งใน `CASON_SETUP_AP_PASSWORD`

ให้ต่อ Wi-Fi มือถือ/คอมเข้ากับ AP นี้ แล้วเลือก Wi-Fi บ้าน/ไซต์งานจากหน้า setup

## คำสั่ง Serial Monitor

```text
TEST
ON
OFF
ALARM
RESET
STATUS
CHECK
RAW
WIFI_RESET
HELP
```

## คำสั่ง LINE

```text
status
check
on
off
reset
raw
test
wifi_reset
help
```

## Build / Upload

```bash
cd /Users/tor/Desktop/casonpower
pio run
pio run --target upload
pio device monitor
```

ถ้า upload ไม่ติด ให้กดปุ่ม BOOT ค้าง แล้วกด Upload ใหม่ จากนั้นปล่อย BOOT เมื่อเริ่มเขียนโปรแกรม

## Local Server สำหรับทดสอบในเครื่อง

```bash
cd /Users/tor/Desktop/casonpower
python3 src/server.py
curl http://127.0.0.1:8080/health
```

## Production Server บน Render

```bash
curl https://casonpower.onrender.com/health
```

ค่า secret/token อยู่ใน Render Environment Variables:

- `LINE_CHANNEL_ACCESS_TOKEN`
- `LINE_CHANNEL_SECRET`
- `CASON_DUPLICATE_BLOCK_SECONDS`

ห้าม commit ไฟล์ `.env` หรือ token จริงขึ้น GitHub
