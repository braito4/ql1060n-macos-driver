SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
CC := xcrun --sdk macosx clang
CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -mmacosx-version-min=14.0 -isysroot $(SDKROOT)
LDLIBS := -lcups
export COPYFILE_DISABLE := 1
BUILD := build
FILTER := $(BUILD)/rastertoql1060n
VERSION := 0.7.0
PPDS := ppd/Brother-QL-1060N-macOS27.ppd ppd/Brother-QL-1050-macOS27.ppd

.PHONY: all test clean package installer

all: $(FILTER)

$(BUILD):
	mkdir -p $(BUILD)

$(FILTER): src/rastertoql1060n.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	codesign --force --sign - $@

$(BUILD)/test_filter: tests/test_filter.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

test: $(FILTER) $(BUILD)/test_filter
	$(BUILD)/test_filter
	$(FILTER) --version
	file $(FILTER) | grep -q 'arm64'
	cupstestppd -q -I filters $(PPDS)
	grep -q 'BrMediaDetect' ppd/Brother-QL-1060N-macOS27.ppd
	! grep -q 'BrMediaDetect' ppd/Brother-QL-1050-macOS27.ppd
	grep -q 'PageSize W103' ppd/Brother-QL-1060N-macOS27.ppd
	grep -q 'PageSize DC103_164' ppd/Brother-QL-1050-macOS27.ppd
	grep -q 'BrHalftone Ordered' ppd/Brother-QL-1060N-macOS27.ppd

package: test
	mkdir -p $(BUILD)/package/Filter $(BUILD)/package/PPDs
	cp $(FILTER) $(BUILD)/package/Filter/
	cp $(PPDS) $(BUILD)/package/PPDs/

installer: test
	mkdir -p $(BUILD)/pkgroot/Library/Printers/QL1060N-macOS27/Filter
	mkdir -p $(BUILD)/pkgroot/Library/Printers/PPDs/Contents/Resources
	cp $(FILTER) $(BUILD)/pkgroot/Library/Printers/QL1060N-macOS27/Filter/
	cp ppd/Brother-QL-1060N-macOS27.ppd '$(BUILD)/pkgroot/Library/Printers/PPDs/Contents/Resources/Brother QL-1060N macOS27.ppd'
	cp ppd/Brother-QL-1050-macOS27.ppd '$(BUILD)/pkgroot/Library/Printers/PPDs/Contents/Resources/Brother QL-1050 macOS27.ppd'
	xattr -cr $(BUILD)/pkgroot
	pkgbuild --root $(BUILD)/pkgroot --scripts pkg-scripts --ownership recommended --identifier io.github.ql1060n.macos27.driver --version $(VERSION) --install-location / '$(BUILD)/QL-1050-QL-1060N-macOS27-arm64-$(VERSION).pkg'

clean:
	rm -rf $(BUILD)
