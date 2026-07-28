# คู่มือติดตั้ง CASONPOWER สำหรับลูกค้าใหม่

ไฟล์นี้ใช้เป็นลำดับขั้นตอนเวลานำโปรเจค CASONPOWER ไปติดตั้งให้ลูกค้าหรือหน้างานใหม่

## ภาพรวมระบบ

ระบบประกอบด้วย 3 ส่วนหลัก

1. ESP32
   - อ่านสัญญาณ DI1
   - ควบคุม Relay CH1
   - ควบคุมไฟสถานะ CH2, CH3, CH4
   - ส่งแจ้งเตือน LINE
   - รับคำสั่งจาก LINE ผ่าน Render

2. LINE Official Account / Messaging API
   - ใช้รับแจ้งเตือน
   - ใช้ส่งคำสั่ง เช่น สถานะ, เปิด, ปิด, รีเซ็ต

3. Render Server
   - รับ Webhook จาก LINE
   - เก็บคำสั่งจาก LINE
   - ให้ ESP32 มาดึงคำสั่งไปทำงาน
   - ส่งผลการทำงานกลับ LINE

## ขั้นตอนที่ 1: เตรียม LINE ของลูกค้า

1. สร้าง LINE Official Account ใหม่ของลูกค้า
2. เข้า LINE Developers
3. เปิดใช้งาน Messaging API
4. เก็บค่า 2 ตัวนี้ไว้
   - LINE_CHANNEL_ACCESS_TOKEN
   - LINE_CHANNEL_SECRET

ห้ามส่งค่า token หรือ secret ให้คนอื่นที่ไม่เกี่ยวข้อง เพราะเป็นรหัสลับของระบบ LINE

## ขั้นตอนที่ 2: เตรียมโปรเจคใหม่

1. คัดลอกโปรเจค `casonpower`
2. เปลี่ยนชื่อเป็นของลูกค้า เช่น
   - `casonpower-customer01`
   - `casonpower-site-a`
3. เปลี่ยนชื่ออุปกรณ์ในโค้ด ถ้ามีหลายเครื่อง เช่น
   - `CASON-ESP32-01`
   - `CASON-ESP32-02`

## ขั้นตอนที่ 3: ตั้งค่า Render

1. เข้า Render Dashboard
2. สร้าง Web Service ใหม่
3. เลือก GitHub Repository ของโปรเจคลูกค้า
4. ใส่ Environment Variables ดังนี้

```text
LINE_CHANNEL_ACCESS_TOKEN=ใส่ Channel access token ของลูกค้า
LINE_CHANNEL_SECRET=ใส่ Channel secret ของลูกค้า
CASON_DUPLICATE_BLOCK_SECONDS=300
```

5. กด Deploy
6. รอจนขึ้นสถานะ Live หรือสีเขียว
7. ทดสอบด้วย URL นี้

```text
https://ชื่อ-service.onrender.com/health
```

ถ้าถูกต้องควรเห็นประมาณนี้

```json
{
  "ok": true,
  "line_token_configured": true,
  "line_secret_configured": true
}
```

## ขั้นตอนที่ 4: ตั้ง Webhook ใน LINE Developers

นำ URL นี้ไปใส่ในช่อง Webhook URL

```text
https://ชื่อ-service.onrender.com/webhook
```

จากนั้นกด Verify

ถ้าผ่าน แปลว่า LINE ส่งข้อมูลมาถึง Render แล้ว

## ขั้นตอนที่ 5: ตั้งค่า ESP32

เปิดไฟล์

```text
src/main.cpp
```

แก้ Wi-Fi ให้ตรงหน้างาน

```cpp
const char *WIFI_SSID = "ชื่อ WiFi";
const char *WIFI_PASSWORD = "รหัส WiFi";
```

แก้ URL ของ Render ให้ตรงกับของลูกค้า

```cpp
const char *COMMAND_SERVER_BASE_URL = "https://ชื่อ-service.onrender.com";
```

ถ้ามีหลายเครื่อง ควรเปลี่ยนชื่ออุปกรณ์ให้ไม่ซ้ำกัน

```text
CASON-ESP32-01
CASON-ESP32-02
```

## ขั้นตอนที่ 6: Upload ลง ESP32

1. เสียบสาย USB กับ ESP32
2. เปิดโปรเจคใน VS Code / PlatformIO
3. กด Upload
4. เปิด Serial Monitor
5. เช็คว่าขึ้น Wi-Fi connected
6. เช็คว่า DI1 อ่านค่าได้ถูกต้อง

ค่าปกติของ DI1 คือ

```text
ปกติ = HIGH = RAW 1
ทำงาน/ผิดปกติ = LOW = RAW 0
```

## ขั้นตอนที่ 7: เพิ่มผู้รับ LINE

1. ให้คนรับแจ้งเตือนแอด LINE Bot / Official Account
2. ให้ส่งข้อความหา Bot เช่น

```text
สถานะ
```

3. Server จะบันทึกผู้รับไว้ในระบบ
4. ถ้ามีผู้รับหลายคน ให้แต่ละคนส่งข้อความหา Bot อย่างน้อย 1 ครั้ง

## ขั้นตอนที่ 8: ทดสอบคำสั่ง LINE

ลองส่งคำสั่งต่อไปนี้จาก LINE

```text
สถานะ
```

```text
ทดสอบ
```

```text
ปิด
```

```text
เปิด
```

```text
รีเซ็ต
```

ระบบควรตอบ 2 จังหวะ

1. รับคำสั่งแล้ว
2. ผลการทำงานจาก ESP32

## ขั้นตอนที่ 9: ทดสอบ DI1 และ Relay

1. DI1 ปกติ
   - RAW = 1
   - Relay CH1 = ON
   - ไฟเขียว CH2 ติด

2. DI1 ผิดปกติ
   - RAW = 0
   - Relay CH1 = OFF
   - ไฟแดง CH4 ติด
   - LINE แจ้งเตือน

3. DI1 กลับปกติ
   - RAW = 1
   - ระบบรอประมาณ 30 วินาที
   - Relay CH1 ควรกลับ ON เอง
   - LINE แจ้งว่าระบบกลับมาปกติ

## หมายเหตุสำคัญ

- ถ้าสั่ง `ปิด` ผ่าน LINE เอง ระบบจะไม่เปิดกลับเอง เพราะถือว่าเป็นการสั่งปิดด้วยคน
- ถ้า Relay OFF เพราะ DI1 ผิดปกติ เมื่อ DI1 กลับปกติ ระบบควรเปิดกลับเองหลังรอ 30 วินาที
- Render Free อาจช้าครั้งแรกประมาณ 30-60 วินาที ถ้าไม่ได้ใช้งานนาน
- ถ้าใช้ Render แล้ว ไม่ต้องเปิด Mac ทิ้งไว้
- ไม่ต้องเปิด ngrok
- ไม่ต้องรัน `python3 src/server.py` บน Mac ตลอดเวลา

## ไฟล์สำคัญในโปรเจค

```text
src/main.cpp          โค้ด ESP32
src/server.py         Server สำหรับ LINE command
Dockerfile            ไฟล์ให้ Render deploy แบบ Docker
render.yaml           ค่า deploy สำหรับ Render
platformio.ini        ค่า board ESP32 / PlatformIO
.env                  รหัสลับ LINE สำหรับเครื่อง local ห้ามอัปโหลด
src/line_secret.h     รหัส LINE สำหรับ ESP32 ห้ามอัปโหลด
```

## สรุปลำดับสั้นที่สุด

```text
สร้าง LINE Bot
นำ token และ secret ไปใส่ Render
Deploy Render ให้ Live
ใส่ Webhook URL ใน LINE Developers
แก้ Wi-Fi และ Render URL ใน ESP32
Upload ESP32
ให้ผู้รับแอด Bot และส่ง สถานะ
ทดสอบ DI1 / Relay / LINE
```
