.section .text
.align 4
.literal_position
.global asm_fast_add

# int asm_fast_add(int a, int b)
# ทางเข้า: a2 = ตัวแปร a, a3 = ตัวแปร b
# ทางออก: a2 = ผลลัพธ์ที่ส่งกลับ

asm_fast_add:
    # เริ่มกระบวนการประมวลผลระดับฮาร์ดแวร์
    add     a2, a2, a3     # เอา a2 + a3 แล้วเก็บผลลัพธ์ไว้ที่ a2
    
    # ส่งสัญญาณกลับไปที่ฟังก์ชันภาษา C 
    # (ใช้ abi-window หรือ call0-return ตามการคอมไพล์)
    ret
    