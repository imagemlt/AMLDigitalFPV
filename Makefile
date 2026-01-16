# 工具链配置
CROSS_COMPILE = armv8a-libreelec-linux-gnueabihf-
CXX = $(CROSS_COMPILE)g++
CC = $(CROSS_COMPILE)gcc

# Sysroot 路径
SYSROOT = /home/docker/CoreELEC/build.CoreELEC-Amlogic-ng.arm-21/toolchain/armv8a-libreelec-linux-gnueabihf/sysroot

# 目录
SRC_DIR := src
BUILD_DIR := build

# 包含路径
INCLUDES = -I$(SYSROOT)/usr/include \
           -I$(SYSROOT)/usr/include/amcodec \
           -I$(SYSROOT)/usr/include/gstreamer-1.0 \
           -I$(SYSROOT)/usr/include/glib-2.0 \
           -I$(SYSROOT)/usr/lib/glib-2.0/include

# 库路径
LIBRARIES = -L$(SYSROOT)/usr/lib

# 编译选项
CXXFLAGS = --sysroot=$(SYSROOT) $(INCLUDES) -DSPDLOG_FMT_EXTERNAL
CFLAGS = --sysroot=$(SYSROOT) $(INCLUDES)
LDFLAGS = --sysroot=$(SYSROOT) $(LIBRARIES) -Wl,--unresolved-symbols=ignore-all

# 库链接
LIBS = -lgstreamer-1.0 -lgstapp-1.0 -lgobject-2.0 -lglib-2.0 -lfmt -lspdlog -lopus -lpulse-simple -lamcodec

# 源文件（header-only 的 scheduling_helper.hpp 无需列在这里）
CXX_SOURCES = $(SRC_DIR)/main.cpp $(SRC_DIR)/gstrtpreceiver.cpp $(SRC_DIR)/spdlog_wrapper.cpp $(SRC_DIR)/audio_receiver.cpp
C_SOURCES = $(SRC_DIR)/util.c $(SRC_DIR)/aml.c

# 目标文件
CXX_OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(CXX_SOURCES))
C_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
OBJECTS = $(CXX_OBJECTS) $(C_OBJECTS)

# 目标可执行文件
TARGET = AMLDigitalFPV

# 默认目标
all: $(TARGET)

# 链接可执行文件
$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

# C++ 源文件编译规则
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# C 源文件编译规则
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# 安装（如果需要）
install: $(TARGET)
	@echo "TODO: add install commands"

.PHONY: all clean install
