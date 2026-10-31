CC = clang
CFLAGS = -arch x86_64 -arch arm64 -dynamiclib -O2 -Wall -Wextra -x objective-c
FRAMEWORKS = -framework Foundation -framework GameController -framework CoreFoundation -framework IOKit
TARGET = libds4_shim.dylib
SRC = ds4_shim.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(FRAMEWORKS) -o $(TARGET)
	codesign -f -s - $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
