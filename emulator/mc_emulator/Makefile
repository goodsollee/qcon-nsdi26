CC = g++

# Default LOG_LEVEL if none is specified
LOG_LEVEL ?= DEBUG

CFLAGS = -Wall -Wextra -std=c++17 -g -O2 \
         -DLOGLEVEL=$(LOG_LEVEL) \
         -I./include \
         -I./include/CU \
         -I./include/DU \
         -I./include/UE_stack \
         -I./include/UE_pdcp \
         -I./include/RIC \
         -I./utils \
         -Iexternal \
         -Iexternal/readerwriterqueue

# Linker flags: include pthread + ZeroMQ, czmq, JSON-C, JSONCpp, and librt
LDFLAGS = -pthread -lzmq -lczmq -ljson-c -ljsoncpp -lrt

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin

# --------------------------------------------------------------------
# 1) Add any RIC .c files to your common sources
#    (If you only have one or two, you can list them explicitly).
#    This example uses wildcard to pick up ALL *.c in src/RIC
# --------------------------------------------------------------------
RIC_C_SRCS = $(wildcard $(SRC_DIR)/RIC/*.c)

RIC_CPP_SRCS = $(SRC_DIR)/RIC/ric_interface_wrapper.cpp

# Common source files (C++ .cpp)
COMMON_SRCS = $(SRC_DIR)/config_parser.cpp \
              $(SRC_DIR)/pdcp_common.cpp \
              $(SRC_DIR)/network/link.cpp \
              $(SRC_DIR)/async_logger.cpp \
              $(SRC_DIR)/rlc_segmentation.cpp \
              $(SRC_DIR)/qos_profiler.cpp

# --------------------------------------------------------------------
# Append the RIC C sources to COMMON_SRCS so every target can link them
# --------------------------------------------------------------------
COMMON_SRCS += $(RIC_C_SRCS)
COMMON_SRCS += $(RIC_CPP_SRCS)

# Source files for emulator_main (PDCP Processor)
EMULATOR_SRCS = $(SRC_DIR)/emulator_main.cpp \
                $(SRC_DIR)/pdcp_processor_implementation.cpp \
                $(COMMON_SRCS)

# Source files for PDCP Sender
SENDER_SRCS = $(SRC_DIR)/CU/pdcp_sender_main.cpp \
              $(SRC_DIR)/CU/pdcp_sender.cpp \
              $(COMMON_SRCS)

# Source files for PDCP Receiver
RECEIVER_SRCS = $(SRC_DIR)/UE_pdcp/pdcp_receiver_main.cpp \
                $(SRC_DIR)/UE_pdcp/pdcp_receiver.cpp \
                $(COMMON_SRCS)

# Source files for DU Link
DU_LINK_SRCS = $(SRC_DIR)/DU/pdcp_du_link_main.cpp \
               $(SRC_DIR)/DU/pdcp_du_link.cpp \
               $(SRC_DIR)/DU/mac_sender.cpp \
               $(SRC_DIR)/DU/phy_sender.cpp \
               $(SRC_DIR)/DU/rlc_sender.cpp \
               $(COMMON_SRCS)

# Source files for UE Link
UE_LINK_SRCS = $(SRC_DIR)/UE_stack/pdcp_ue_link_main.cpp \
               $(SRC_DIR)/UE_stack/pdcp_ue_link.cpp \
               $(SRC_DIR)/UE_stack/mac_receiver.cpp \
               $(SRC_DIR)/UE_stack/phy_receiver.cpp \
               $(SRC_DIR)/UE_stack/rlc_receiver.cpp \
               $(SRC_DIR)/DU/rlc_sender.cpp \
               $(COMMON_SRCS)


# Source files for Manager
MANAGER_SRCS = $(SRC_DIR)/pdcp_emulator_manager.cpp \
               $(COMMON_SRCS)

# Object files
EMULATOR_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(EMULATOR_SRCS))) \
                $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter %.c,$(EMULATOR_SRCS)))

SENDER_OBJS    = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(SENDER_SRCS))) \
                 $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter %.c,$(SENDER_SRCS)))

RECEIVER_OBJS  = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(RECEIVER_SRCS))) \
                 $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter %.c,$(RECEIVER_SRCS)))

DU_LINK_OBJS   = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(DU_LINK_SRCS))) \
                 $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter %.c,$(DU_LINK_SRCS)))

UE_LINK_OBJS   = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(UE_LINK_SRCS))) \
                 $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter %.c,$(UE_LINK_SRCS)))

MANAGER_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(MANAGER_SRCS))) \
               $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter %.c,$(MANAGER_SRCS)))

BUILD_SUBDIRS = \
    $(BUILD_DIR) \
    $(BUILD_DIR)/CU \
    $(BUILD_DIR)/DU \
    $(BUILD_DIR)/UE_stack \
    $(BUILD_DIR)/UE_pdcp \
    $(BUILD_DIR)/RIC \
    $(BUILD_DIR)/network

EMULATOR_TARGET = $(BIN_DIR)/emulator
SENDER_TARGET   = $(BIN_DIR)/pdcp_sender
RECEIVER_TARGET = $(BIN_DIR)/pdcp_receiver
DU_LINK_TARGET  = $(BIN_DIR)/pdcp_du_link
UE_LINK_TARGET  = $(BIN_DIR)/pdcp_ue_link
MANAGER_TARGET = $(BIN_DIR)/pdcp_emulator_manager

all: directories $(EMULATOR_TARGET) $(SENDER_TARGET) $(RECEIVER_TARGET) $(DU_LINK_TARGET) $(UE_LINK_TARGET) $(MANAGER_TARGET)

directories: $(BUILD_SUBDIRS) $(BIN_DIR)

$(BUILD_SUBDIRS) $(BIN_DIR):
	@mkdir -p $@

# --------------------------------------------------------------------
# 2) We need TWO compile rules: one for .cpp (C++), one for .c (C).
#    (Up until now, you only had a rule for .cpp.)
# --------------------------------------------------------------------

# Rule for C++ .cpp -> .o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for C .c -> .o (using $(CC), but you may want a different var)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	# If you have a separate 'gcc' variable, you can use that. Otherwise:
	gcc $(CFLAGS) -c $< -o $@

# Link targets
$(EMULATOR_TARGET): $(EMULATOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@ln -sf $(EMULATOR_TARGET) ./tun_forwarder
	@echo "Created tun_forwarder symlink"

$(SENDER_TARGET): $(SENDER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(RECEIVER_TARGET): $(RECEIVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(DU_LINK_TARGET): $(DU_LINK_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(UE_LINK_TARGET): $(UE_LINK_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(MANAGER_TARGET): $(MANAGER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Individual component targets
pdcp_processor: directories $(EMULATOR_TARGET)
pdcp_sender: directories $(SENDER_TARGET)
pdcp_receiver: directories $(RECEIVER_TARGET)
pdcp_du_link: directories $(DU_LINK_TARGET)
pdcp_ue_link: directories $(UE_LINK_TARGET)
pdcp_emulator_manager: directories $(MANAGER_TARGET)

# Target to build only the basic test components
zmq-test: directories $(EMULATOR_TARGET) $(SENDER_TARGET) $(RECEIVER_TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) tun_forwarder

# Run complete emulator with manager
run: $(MANAGER_TARGET)
	sudo ./$(MANAGER_TARGET) -c config.json

# Help
help:
	@echo "PDCP Emulator Makefile"
	@echo "----------------------"
	@echo "Targets:"
	@echo "  all                  - Build all executables (default)"
	@echo "  pdcp_processor       - Build only the PDCP Processor"
	@echo "  pdcp_sender          - Build only the PDCP Sender"
	@echo "  pdcp_receiver        - Build only the PDCP Receiver"
	@echo "  pdcp_du_link         - Build only the PDCP DU Link"
	@echo "  pdcp_ue_link         - Build only the PDCP UE Link"
	@echo "  pdcp_emulator_manager - Build only the PDCP Emulator Manager"
	@echo "  zmq-test             - Build only the components needed for ZMQ testing"
	@echo "  clean                - Remove build artifacts"
	@echo "  run                  - Run the multi-connectivity emulator via manager"
	@echo ""
	@echo "Build products:"
	@echo "  $(EMULATOR_TARGET)     - The PDCP Processor executable"
	@echo "  $(SENDER_TARGET)       - The PDCP Sender executable"
	@echo "  $(RECEIVER_TARGET)     - The PDCP Receiver executable"
	@echo "  $(DU_LINK_TARGET)      - The PDCP DU Link executable"
	@echo "  $(UE_LINK_TARGET)      - The PDCP UE Link executable"
	@echo "  $(MANAGER_TARGET)      - The PDCP Emulator Manager executable"

.PHONY: all directories clean run help pdcp_processor pdcp_sender pdcp_receiver pdcp_du_link pdcp_ue_link pdcp_emulator_manager zmq-test