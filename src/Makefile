# 工具链配置
CROSS_COMPILE = armv8a-libreelec-linux-gnueabihf-
CXX = $(CROSS_COMPILE)g++
CC = $(CROSS_COMPILE)gcc

# Sysroot 路径
SYSROOT = /home/docker/CoreELEC/build.CoreELEC-Amlogic-ng.arm-21/toolchain/armv8a-libreelec-linux-gnueabihf/sysroot

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
LIBS = -lgstreamer-1.0 -lgstapp-1.0 -lgobject-2.0 -lglib-2.0 -lfmt -lspdlog -lamcodec

# 源文件
CXX_SOURCES = main.cpp gstrtpreceiver.cpp
C_SOURCES = util.c aml.c

# 目标文件
CXX_OBJECTS = $(CXX_SOURCES:.cpp=.o)
C_OBJECTS = $(C_SOURCES:.c=.o)
OBJECTS = $(CXX_OBJECTS) $(C_OBJECTS)

# 目标可执行文件
TARGET = AMLDigitalFPV

# 默认目标
all: $(TARGET)

# 链接可执行文件
$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

# C++ 源文件编译规则
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# C 源文件编译规则
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	rm -f $(OBJECTS) $(TARGET)

# 安装（如果需要）
install: $(TARGET)
	# 添加安装命令

.PHONY: all clean install
