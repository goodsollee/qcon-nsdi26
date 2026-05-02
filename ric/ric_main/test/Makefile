# Makefile for multi-user test

CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2 -I../include -I../.. -I. -I/usr/include/jsoncpp
LDFLAGS = -ljsoncpp -lzmq -lpthread

# Directory setup
BUILD_DIR = ../build
RIC_DIR = ..

# Target executable
TEST_TARGET = $(BUILD_DIR)/multi_user_test

# Source files
TEST_SRC = multi_user_test.cpp
DUMMY_SRC = $(BUILD_DIR)/dummy_state_manager.cpp

# Object files
TEST_OBJ = $(BUILD_DIR)/multi_user_test.o
DUMMY_OBJ = $(BUILD_DIR)/dummy_state_manager.o
ASYNC_LOGGER_OBJ = $(BUILD_DIR)/async_logger.o

# Default target
all: $(TEST_TARGET)

# Main target
$(TEST_TARGET): $(TEST_OBJ) $(ASYNC_LOGGER_OBJ) $(DUMMY_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Compile test source
$(TEST_OBJ): $(TEST_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile async logger
$(ASYNC_LOGGER_OBJ): $(RIC_DIR)/src/async_logger.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile dummy state manager
$(DUMMY_OBJ): $(DUMMY_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Create dummy state manager source file
$(DUMMY_SRC):
	@mkdir -p $(BUILD_DIR)
	@echo '#include "state_manager/state_manager.hpp"' > $@
	@echo 'namespace ric {' >> $@
	@echo 'CrossLayerStateManager::CrossLayerStateManager() {}' >> $@
	@echo 'CrossLayerStateManager::~CrossLayerStateManager() {}' >> $@
	@echo 'bool CrossLayerStateManager::initialize() { return true; }' >> $@
	@echo 'bool CrossLayerStateManager::processMetrics(const std::string&, const std::string&, const std::string&, const std::string&) { return true; }' >> $@
	@echo 'std::shared_ptr<LinkState> CrossLayerStateManager::getLinkState(int) { return nullptr; }' >> $@
	@echo 'std::vector<std::shared_ptr<LinkState>> CrossLayerStateManager::getActiveLinkStates() { return {}; }' >> $@
	@echo 'void CrossLayerStateManager::registerListener(StateUpdateListener*) {}' >> $@
	@echo 'void CrossLayerStateManager::unregisterListener(StateUpdateListener*) {}' >> $@
	@echo 'Json::Value CrossLayerStateManager::getSystemState() { return Json::Value(); }' >> $@
	@echo '} // namespace ric' >> $@

# Setup directories and symlinks needed for mocks
setup:
	@mkdir -p mocks
	@mkdir -p $(BUILD_DIR)/include/state_manager
	@mkdir -p $(BUILD_DIR)/include/scheduler/QCON
	@mkdir -p $(BUILD_DIR)/include/RIC/controller
	@ln -sf $(RIC_DIR)/include/state_manager/state_manager.hpp $(BUILD_DIR)/include/state_manager/ 2>/dev/null || true
	@ln -sf mocks/link_state.hpp $(BUILD_DIR)/include/state_manager/ 2>/dev/null || true

# Run the test
run: all
	$(TEST_TARGET) --num-users 5 --duration 30

# Run with specific user count and duration
run-with-users: all
	$(TEST_TARGET) --num-users $(USERS) --duration $(DURATION)

# Clean
clean:
	rm -f $(TEST_OBJ) $(ASYNC_LOGGER_OBJ) $(DUMMY_OBJ) $(TEST_TARGET) $(DUMMY_SRC)

.PHONY: all setup run run-with-users clean