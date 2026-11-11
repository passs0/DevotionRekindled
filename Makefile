CC = clang
ARCH = -arch x86_64 -arch arm64
SHIM_CFLAGS = $(ARCH) -dynamiclib -O2 -Wall -Wextra -x objective-c
FRAMEWORKS = -framework Foundation -framework GameController -framework CoreFoundation -framework IOKit
SHIM_TARGET = libds4_shim.dylib
SHIM_SRC = ds4_shim.c

APP_BUNDLE = DevotionRekindled.app
LAUNCHER_SRC = launcher.c
LAUNCHER_BIN = $(APP_BUNDLE)/Contents/MacOS/DevotionRekindled

all: $(SHIM_TARGET) app

$(SHIM_TARGET): $(SHIM_SRC)
	$(CC) $(SHIM_CFLAGS) $(SHIM_SRC) $(FRAMEWORKS) -o $(SHIM_TARGET)
	codesign -f -s - $(SHIM_TARGET)

app: $(SHIM_TARGET) $(LAUNCHER_SRC)
	mkdir -p "$(APP_BUNDLE)/Contents/MacOS" "$(APP_BUNDLE)/Contents/Resources"
	$(CC) $(ARCH) -O2 -Wall -Wextra $(LAUNCHER_SRC) -o "$(LAUNCHER_BIN)"
	cp -p $(SHIM_TARGET) "$(APP_BUNDLE)/Contents/MacOS/$(SHIM_TARGET)"
	if [ -d "assets/DevotionRekindled.icon.xcassets" ] && xcrun actool --version >/dev/null 2>&1; then \
		echo "Compiling dynamic Assets.car..."; \
		xcrun actool "assets/DevotionRekindled.icon.xcassets" \
			--compile "$(APP_BUNDLE)/Contents/Resources" \
			--platform macosx \
			--minimum-deployment-target 11.0 \
			--app-icon DevotionRekindled \
			--output-partial-info-plist /tmp/devotion_partial.plist \
			--output-format human-readable-text || true; \
	fi
	codesign -f -s - "$(APP_BUNDLE)/Contents/MacOS/$(SHIM_TARGET)"
	codesign -f -s - "$(APP_BUNDLE)"

install: app
	cp -Rf "$(APP_BUNDLE)/" "/Applications/$(APP_BUNDLE)/"

clean:
	rm -f $(SHIM_TARGET) "$(LAUNCHER_BIN)"

.PHONY: all app install clean
