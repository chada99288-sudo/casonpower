# Cason Solar Safety Controller Backup

สำเนาโปรเจกต์ ESP32 + Python LINE Server

## Firmware

- ESP32-S3 DevKitC-1
- DI1 Active LOW: NORMAL=1, ACTIVE=0
- Relay CH1 OFF เมื่อ DI1 ACTIVE
- Relay CH1 ON อัตโนมัติหลัง DI1 กลับ NORMAL ครบ 30 วินาที
- Relay CH2 = ไฟเขียว ระบบปกติ
- Relay CH3 = ไฟเหลือง ผิดปกติเล็กน้อย/รอกู้คืน/ส่ง server ค้าง/Wi-Fi หลุด
- Relay CH4 = ไฟแดง DI1 active หรือ alarm รุนแรง
- ส่ง event ไป Python server ที่ `http://192.168.1.140:8080/api/alert`

## Commands

```bash
cd ~/Desktop/Cason_Solar_Safety_Controller_Backup
pio run
pio run --target upload
pio device monitor
```

## Python LINE Server

```bash
cd ~/Desktop/Cason_Solar_Safety_Controller_Backup
python3 src/server.py
```

ตั้งค่า LINE token ใน `.env` หรือ export ใน Terminal ก่อนรัน server

```bash
export LINE_CHANNEL_ACCESS_TOKEN="ใส่ token"
export LINE_CHANNEL_SECRET="ใส่ secret ถ้าใช้ webhook"
```
