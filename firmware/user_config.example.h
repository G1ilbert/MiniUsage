#pragma once

/* ============================================================
 *  Wi-Fi + Proxy configuration
 * ============================================================
 *  วิธีใช้:
 *    1. คัดลอกไฟล์นี้เป็น user_config.h
 *       cp user_config.example.h user_config.h
 *    2. แก้ค่าด้านล่างให้ตรงกับเครือข่าย + IP ของเครื่องที่รัน proxy
 *    3. user_config.h ถูก .gitignore — จะไม่ถูก commit
 */

#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

/* IP ของเครื่อง Windows/Mac/Pi ที่รัน Rust proxy
 * รัน `ipconfig` (Windows) / `ifconfig` (Mac/Linux) เพื่อหา */
#define PROXY_HOST    "192.168.1.100"
#define PROXY_PORT    8765
#define POLL_INTERVAL 60               /* วินาที ระหว่างการดึงข้อมูล /usage */
