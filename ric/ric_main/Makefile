# Compiler and flags
CXX = g++
CLANG = clang
STRIP = llvm-strip
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I/usr/local/include -DLOGLEVEL=DEBUG -Iinclude
LDFLAGS = -lzmq -lpthread -ljsoncpp -ljson-c -lbpf -lelf -lz -lxdp

# BPF/XDP Compilation
BPF_CFLAGS = -g -O2 -target bpf -I/usr/include/x86_64-linux-gnu -D__TARGET_ARCH_x86
XDP_SRC = src/RIC/xdp_rtp_filter.c
XDP_OBJ = bin/xdp_rtp_filter.o
TC_SRC = src/RIC/tc_rtp_filter.c
TC_OBJ = bin/tc_rtp_filter.o

# Directories
BUILD_DIR = build
BIN_DIR = bin
SRC_DIR = src
INCLUDE_DIR = include
BPF_BUILD_DIR = $(BUILD_DIR)/bpf

# Source directories
RIC_DIR = $(SRC_DIR)/RIC
SCHEDULER_DIR = $(SRC_DIR)/scheduler
STATE_MANAGER_DIR = $(SRC_DIR)/state_manager

# Build directories
BUILD_RIC_DIR = $(BUILD_DIR)/RIC
BUILD_SCHEDULER_DIR = $(BUILD_DIR)/scheduler
BUILD_STATE_MANAGER_DIR = $(BUILD_DIR)/state_manager

# Find all source files
MAIN_SRC = main.cpp
UTIL_SRC = src/async_logger.cpp
RIC_SRCS = $(wildcard $(RIC_DIR)/*.cpp)
SCHEDULER_SRCS = $(wildcard $(SCHEDULER_DIR)/*.cpp)
STATE_MANAGER_SRCS = $(wildcard $(STATE_MANAGER_DIR)/*.cpp)

# RIC subdirectories
RIC_SUBDIRS = controller
RIC_SUBDIR_SRCS = $(foreach dir,$(RIC_SUBDIRS),$(wildcard $(RIC_DIR)/$(dir)/*.cpp))
BUILD_RIC_SUBDIRS = $(foreach dir,$(RIC_SUBDIRS),$(BUILD_RIC_DIR)/$(dir))
RIC_SUBDIR_OBJS = $(patsubst $(RIC_DIR)/%.cpp,$(BUILD_RIC_DIR)/%.o,$(RIC_SUBDIR_SRCS))

# Scheduler subdirectories
SCHEDULER_SUBDIRS = QCON Single DChannel minRTT PD FiveQI
SCHEDULER_SUBDIR_SRCS = $(foreach dir,$(SCHEDULER_SUBDIRS),$(wildcard $(SCHEDULER_DIR)/$(dir)/*.cpp))
BUILD_SCHEDULER_SUBDIRS = $(foreach dir,$(SCHEDULER_SUBDIRS),$(BUILD_SCHEDULER_DIR)/$(dir))
SCHEDULER_SUBDIR_OBJS = $(patsubst $(SCHEDULER_DIR)/%.cpp,$(BUILD_SCHEDULER_DIR)/%.o,$(SCHEDULER_SUBDIR_SRCS))

# Generate object file paths
MAIN_OBJ = $(BUILD_DIR)/main.o
UTIL_OBJ  = $(BUILD_DIR)/async_logger.o
RIC_OBJS = $(patsubst $(RIC_DIR)/%.cpp, $(BUILD_RIC_DIR)/%.o, $(RIC_SRCS))
SCHEDULER_OBJS = $(patsubst $(SCHEDULER_DIR)/%.cpp, $(BUILD_SCHEDULER_DIR)/%.o, $(SCHEDULER_SRCS))
STATE_MANAGER_OBJS = $(patsubst $(STATE_MANAGER_DIR)/%.cpp, $(BUILD_STATE_MANAGER_DIR)/%.o, $(STATE_MANAGER_SRCS))

# All objects combined
ALL_OBJS = $(MAIN_OBJ) $(UTIL_OBJ) $(RIC_OBJS) $(RIC_SUBDIR_OBJS) \
            $(SCHEDULER_OBJS) $(SCHEDULER_SUBDIR_OBJS) $(STATE_MANAGER_OBJS)

# Dependencies
DEPS = $(ALL_OBJS:.o=.d)

# Target executables
TARGET = $(BIN_DIR)/ric_main
TEST_TARGET = $(BIN_DIR)/multi_user_test

# Default target
.PHONY: all clean info directories xdp tc debug release format check test

all: directories xdp tc $(TARGET)

# Test target
test: directories $(TEST_TARGET)

# Create necessary directories
directories:
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_RIC_DIR)
	mkdir -p $(BUILD_SCHEDULER_DIR)
	mkdir -p $(BUILD_STATE_MANAGER_DIR)
	mkdir -p $(BIN_DIR)
	mkdir -p $(BPF_BUILD_DIR)
	mkdir -p $(foreach dir,$(RIC_SUBDIRS),$(BUILD_RIC_DIR)/$(dir))
	mkdir -p $(foreach dir,$(SCHEDULER_SUBDIRS),$(BUILD_SCHEDULER_DIR)/$(dir))

# BPF/XDP targets
xdp: $(XDP_OBJ)
tc: $(TC_OBJ)

# Compile BPF/XDP programs
$(XDP_OBJ): $(XDP_SRC) | $(BIN_DIR)
	@echo "Compiling XDP BPF program for RTP header extraction..."
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	$(STRIP) -g $@
	@echo "XDP BPF program compiled: $@"

$(TC_OBJ): $(TC_SRC) | $(BIN_DIR)
	@echo "Compiling TC BPF program for RTP header extraction..."
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	$(STRIP) -g $@
	@echo "TC BPF program compiled: $@"

# Link the main executable
$(TARGET): $(ALL_OBJS) | $(BIN_DIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Compile main source file
$(MAIN_OBJ): $(MAIN_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(UTIL_OBJ): $(UTIL_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Compile RIC directory files
$(BUILD_RIC_DIR)/%.o: $(RIC_DIR)/%.cpp | directories
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Rules for RIC subdirectory files
define make-ric-subdir-rule
$(BUILD_RIC_DIR)/$(1)/%.o: $(RIC_DIR)/$(1)/%.cpp | directories
	$$(CXX) $$(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef

$(foreach dir,$(RIC_SUBDIRS),$(eval $(call make-ric-subdir-rule,$(dir))))

# Compile scheduler directory files
$(BUILD_SCHEDULER_DIR)/%.o: $(SCHEDULER_DIR)/%.cpp | directories
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Rules for scheduler subdirectory files
define make-scheduler-subdir-rule
$(BUILD_SCHEDULER_DIR)/$(1)/%.o: $(SCHEDULER_DIR)/$(1)/%.cpp | directories
	$$(CXX) $$(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef

$(foreach dir,$(SCHEDULER_SUBDIRS),$(eval $(call make-scheduler-subdir-rule,$(dir))))

# Compile state_manager directory files
$(BUILD_STATE_MANAGER_DIR)/%.o: $(STATE_MANAGER_DIR)/%.cpp | directories
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Include dependencies
-include $(DEPS)

# Build with debug symbols
debug: CXXFLAGS += -g -DDEBUG
debug: all

# Build with optimization
release: CXXFLAGS += -O3 -DNDEBUG
release: all

# Format source files (requires clang-format)
format:
	find $(SRC_DIR) $(INCLUDE_DIR) -name "*.cpp" -o -name "*.hpp" -o -name "*.c" | xargs clang-format -i

# Static analysis (requires cppcheck)
check:
	cppcheck --enable=all --suppress=missingInclude $(SRC_DIR) $(INCLUDE_DIR)

# Install
PREFIX = /usr/local
install: all
	install -d $(PREFIX)/bin
	install $(TARGET) $(PREFIX)/bin
	install -d $(PREFIX)/lib/ric_controller/bpf
	install -d $(PREFIX)/lib/ric_controller/xdp
	install $(TC_OBJ) $(PREFIX)/lib/ric_controller/bpf/
	install $(XDP_OBJ) $(PREFIX)/lib/ric_controller/xdp/

# Uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(notdir $(TARGET))
	rm -rf $(PREFIX)/lib/ric_controller

# Multi-user test source
TEST_SRC = test/multi_user_test.cpp
TEST_OBJ = $(BUILD_DIR)/multi_user_test.o

# Build multi-user test
$(TEST_TARGET): $(TEST_OBJ) $(UTIL_OBJ) | $(BIN_DIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Compile multi-user test source
$(TEST_OBJ): $(TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Clean build files
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TEST_TARGET) $(XDP_OBJ) $(TC_OBJ)
	rm -f /sys/fs/bpf/app_comm/* 2>/dev/null || true
	rm -f /sys/fs/bpf/xdp/* 2>/dev/null || true

# Clean and rebuild
rebuild: clean all

# Print information for debugging
info:
	@echo "Main source: $(MAIN_SRC)"
	@echo "RIC sources: $(RIC_SRCS)"
	@echo "RIC subdir sources: $(RIC_SUBDIR_SRCS)"
	@echo "Scheduler sources: $(SCHEDULER_SRCS)"
	@echo "Scheduler subdir sources: $(SCHEDULER_SUBDIR_SRCS)"
	@echo "State Manager sources: $(STATE_MANAGER_SRCS)"
	@echo "Main object: $(MAIN_OBJ)"
	@echo "RIC objects: $(RIC_OBJS)"
	@echo "RIC subdir objects: $(RIC_SUBDIR_OBJS)"
	@echo "Scheduler objects: $(SCHEDULER_OBJS)"
	@echo "Scheduler subdir objects: $(SCHEDULER_SUBDIR_OBJS)"
	@echo "State Manager objects: $(STATE_MANAGER_OBJS)"