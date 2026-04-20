TOOLCHAIN_PREFIX = /opt/arm-mingw32ce/bin/arm-mingw32ce-
CC = $(TOOLCHAIN_PREFIX)gcc
STRIP = $(TOOLCHAIN_PREFIX)strip
WINDRES = $(TOOLCHAIN_PREFIX)windres

CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -std=c99 -pedantic -D_WIN32_WCE=0x600 -D_UNICODE -DUNICODE
LDFLAGS = -lcommctrl -lcommdlg -lcoredll -static-libgcc

SRC = src/main.c
OBJ = $(SRC:.c=.o)
RES = src/resources.o
TARGET = cepad.exe

all: $(TARGET)

$(TARGET): $(OBJ) $(RES)
	$(CC) $(OBJ) $(RES) -o $@ $(LDFLAGS)
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

src/resources.o: src/resources.rc
	$(WINDRES) -Isrc $< $@

clean:
	rm -f $(OBJ) $(RES) $(TARGET)
