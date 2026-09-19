# 交叉编译说明见 README.md；默认使用 build-env/ 容器里的工具链
TOP      ?= ..
CROSS    ?= aarch64-linux-gnu-
SYSROOT  ?= $(TOP)/reference/device/rootfs
FFMPEG   ?= $(TOP)/thirdparty/ffmpeg-aarch64
VERSION  ?= 0.4.0

CC       := $(CROSS)gcc
CFLAGS   := -O2 -Wall -Wextra -std=gnu11 -mcpu=cortex-a53 -mtune=cortex-a53 \
            -I$(SYSROOT)/usr/include -I$(FFMPEG)/include -Isrc
LDFLAGS  := -L$(SYSROOT)/usr/lib -L$(SYSROOT)/lib -L$(FFMPEG)/lib \
            -Wl,-rpath-link,$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/lib \
            -Wl,-rpath-link,$(FFMPEG)/lib
LDLIBS   := -l:libainice.so -lavformat -lavcodec -lavutil -l:libc.so.6 -lpthread
# 说明：libainice.so 按设备 glibc 2.38 构建（__isoc23_* 符号），故显式链设备的
# libc.so.6（sysroot lib/），工具链自带的 2.35 libc 仅用于头文件。

SRC := src/main.c src/config.c src/compose.c src/push.c src/g2r.c src/capture.c \
       src/auth.c src/panel.c src/catalog.c src/httputil.c \
       src/vendor/cJSON.c src/vendor/xiaomi_api_client.c
BIN := multicam.app

$(BIN): $(SRC) src/multicam.h src/compose.h src/push.h src/g2r.h src/capture.h src/auth.h src/panel.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

package: $(BIN)
	rm -rf pkg && mkdir -p pkg/bin pkg/libs
	cp plugin.json $(BIN) pkg/
	cp -R etc pkg/etc
	cp -R www pkg/www
	cp $(TOP)/reference/mhcamera/bin/go2rtc pkg/bin/go2rtc
	cp -L $(FFMPEG)/lib/libavcodec.so.58 pkg/libs/libavcodec.so.58
	cp -L $(FFMPEG)/lib/libavformat.so.58 pkg/libs/libavformat.so.58
	cp -L $(FFMPEG)/lib/libavutil.so.56 pkg/libs/libavutil.so.56
	chmod 755 pkg/$(BIN) pkg/bin/go2rtc pkg/libs/*.so.*
	cd pkg && tar czf ../multicam-$(VERSION).plugin plugin.json multicam.app etc bin libs www
	rm -rf pkg
	@ls -lah multicam-$(VERSION).plugin

clean:
	rm -f $(BIN) multicam-*.plugin

.PHONY: package clean
