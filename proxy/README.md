# claude-proxy

Rust HTTP proxy สำหรับดึง Claude API usage แล้วส่งให้ ESP8266

## Response format

GET http://\<pi-ip\>:8765/usage

```json
{
  "ok": true,
  "used_tokens": 42000,
  "limit_tokens": 200000,
  "used_pct": 21,
  "remaining_tokens": 158000,
  "reset_in_secs": 3540
}
```

## Build บน Pi โดยตรง

```bash
# ติดตั้ง Rust (ครั้งแรกครั้งเดียว)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env

# clone / copy โปรเจกต์มาที่ Pi แล้ว build
cd claude-proxy
cargo build --release

# binary อยู่ที่
./target/release/claude-proxy
```

## Build แบบ cross-compile จาก PC (Pi Zero/1 ใช้ armv6)

```bash
# Pi 1/Zero = armv6
rustup target add arm-unknown-linux-gnueabihf

cargo build --release --target arm-unknown-linux-gnueabihf
# copy binary ไปที่ Pi
scp target/arm-unknown-linux-gnueabihf/release/claude-proxy pi@<ip>:~/
```

## ติดตั้ง systemd service (Pi auto-start)

```bash
# แก้ ANTHROPIC_API_KEY ใน claude-proxy.service ก่อน
sudo cp claude-proxy.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable claude-proxy
sudo systemctl start claude-proxy

# ดู log
journalctl -u claude-proxy -f
```

## ทดสอบ

```bash
curl http://localhost:8765/usage
curl http://localhost:8765/health
```

## ข้อควรระวัง

- proxy cache ผล 60 วินาที — ESP8266 poll บ่อยแค่ไหนก็ไม่ยิง API ซ้ำ
- API key เก็บใน environment variable ใน .service file เท่านั้น อย่า commit ลง git
