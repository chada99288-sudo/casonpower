# คู่มือทดสอบระบบล่ม CASONPOWER แบบปลอดภัย

เอกสารนี้ใช้สำหรับจำลองเหตุการณ์ server ค้าง, LINE ส่งไม่ออก, server ตอบ error หรือ JSON เสีย โดยไม่ทำลายอุปกรณ์จริง

## หลักสำคัญ

- โหมดนี้ปิดอยู่เป็นค่าเริ่มต้น
- ห้ามเปิดบนระบบลูกค้าจริงถ้าไม่ได้ตั้งใจทดสอบ
- ตัว ESP32 ยังต้องตัด DI1 และควบคุม Relay เองได้ ถึง server/LINE มีปัญหา
- หลังทดสอบเสร็จต้องปิด chaos mode ทุกครั้ง

## ตัวแปรที่ใช้

ใส่ใน `.env` เฉพาะตอนทดสอบ หรือใส่ใน Environment ของ Render เฉพาะ service ทดสอบ

```env
CASON_CHAOS_MODE=1
CASON_CHAOS_DELAY_SECONDS=0
CASON_CHAOS_HTTP_500_RATE=0
CASON_CHAOS_BAD_JSON_RATE=0
CASON_CHAOS_DROP_LINE_RATE=0
```

ความหมาย:

- `CASON_CHAOS_MODE=1` เปิดโหมดทดสอบล่ม
- `CASON_CHAOS_DELAY_SECONDS=10` จำลอง server ค้าง/ตอบช้า 10 วินาที ทุก request
- `CASON_CHAOS_HTTP_500_RATE=1` จำลอง server ตอบ error 503 ทุกครั้ง
- `CASON_CHAOS_BAD_JSON_RATE=1` จำลอง server ตอบ JSON เสียทุกครั้ง
- `CASON_CHAOS_DROP_LINE_RATE=1` จำลอง LINE ส่งไม่ออกทุกครั้ง

ค่า rate ใช้ได้ตั้งแต่ `0` ถึง `1` เช่น `0.3` คือสุ่มพังประมาณ 30%

## ชุดทดสอบแนะนำ

### 1. Server ตอบช้า

```env
CASON_CHAOS_MODE=1
CASON_CHAOS_DELAY_SECONDS=10
```

ผลที่ควรเห็น:

- ESP32 ไม่ค้างยาวเกิน timeout
- DI1 ยังต้องตัด Relay ได้
- LINE/command อาจช้า แต่ระบบความปลอดภัยต้องทำงาน

### 2. Server ล่ม ตอบ 503

```env
CASON_CHAOS_MODE=1
CASON_CHAOS_HTTP_500_RATE=1
```

ผลที่ควรเห็น:

- ESP32 แสดง queue pending/retry
- Relay/DI1 ยังทำงานเองได้
- ไฟสถานะควรขึ้นเหลืองถ้ามี queue/server pending

### 3. Server ตอบ JSON เสีย

```env
CASON_CHAOS_MODE=1
CASON_CHAOS_BAD_JSON_RATE=1
```

ผลที่ควรเห็น:

- ESP32 ไม่ reboot
- serial อาจขึ้น JSON error
- command จาก LINE จะไม่ทำงาน แต่ระบบ DI1 ยังทำงาน

### 4. LINE ส่งไม่ออก

```env
CASON_CHAOS_MODE=1
CASON_CHAOS_DROP_LINE_RATE=1
```

ผลที่ควรเห็น:

- Server รับ event ได้ แต่ส่ง LINE ไม่สำเร็จ
- ESP32 ยังควบคุม Relay ตาม DI1 ได้

### 5. สุ่มพังหลายแบบ

```env
CASON_CHAOS_MODE=1
CASON_CHAOS_DELAY_SECONDS=3
CASON_CHAOS_HTTP_500_RATE=0.2
CASON_CHAOS_BAD_JSON_RATE=0.2
CASON_CHAOS_DROP_LINE_RATE=0.3
```

ผลที่ควรเห็น:

- ระบบออนไลน์อาจตอบบ้างไม่ตอบบ้าง
- แต่ DI1/Relay ต้องไม่เสียหลัก

## วิธีปิดหลังทดสอบ

เปลี่ยนเป็น:

```env
CASON_CHAOS_MODE=0
CASON_CHAOS_DELAY_SECONDS=0
CASON_CHAOS_HTTP_500_RATE=0
CASON_CHAOS_BAD_JSON_RATE=0
CASON_CHAOS_DROP_LINE_RATE=0
```

หรือเอาบรรทัด chaos ออกจาก `.env`

จากนั้น restart server หรือ redeploy Render

## เช็คว่าปิดแล้ว

เปิด:

```bash
curl https://casonpower.onrender.com/health
```

ต้องเห็น:

```json
"chaos_mode": false
```
