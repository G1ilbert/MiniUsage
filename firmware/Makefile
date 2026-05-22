TOOLCHAIN = D:/Project/MiniUsage/toolchain/xtensa-lx106-elf/bin
CC      = $(TOOLCHAIN)/xtensa-lx106-elf-gcc
AS      = $(TOOLCHAIN)/xtensa-lx106-elf-as
OBJCOPY = python -m esptool

SDK     = D:/Project/MiniUsage/ESP8266_NONOS_SDK
SDK_LIB = ../lib

CFLAGS  = -I. -I.. -I../json -I$(SDK)/include -mlongcalls -O2

PORT = COM3
BAUD = 460800

all: image.bin

image.bin: main.o app.o
	$(CC) -T ../eagle.app.v6.ld -T ../eagle.rom.addr.v6.ld -L.. -L$(SDK_LIB) -o main.elf main.o app.o -nostdlib -Wl,--start-group -lmain -lnet80211 -lwpa -lcrypto -lpp -lphy -llwip -ljson -lmain -lc -lgcc -Wl,--end-group
	$(OBJCOPY) --chip esp8266 elf2image main.elf

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

app.o: app.s
	$(AS) -o app.o app.s

flash: image.bin
	$(OBJCOPY) --chip esp8266 --port $(PORT) --baud $(BAUD) write_flash 0x00000 main-0x00000.bin 0x10000 main-0x10000.bin

clean:
	cmd /c del /q *.o *.elf *.bin 2>nul
