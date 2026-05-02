CXX = g++
CXXFLAGS = -std=c++17 -Wall -g -Wno-unused-variable
LDFLAGS = -L/usr/local/lib -lzmq -lpcap -pthread -lboost_system -ljsoncpp

# Directories
RTP_DIR = rtp_profiler
MULTI_CONN_DIR = multi_conn_agent
LOG_DIR = ../logger
UTILS_DIR = utils

# Include directories
INCLUDES = -I. \
           -I$(RTP_DIR) \
           -I$(MULTI_CONN_DIR) \
           -I$(LOG_DIR) \
           -I$(UTILS_DIR) \
           -I/usr/local/include

# Source files by directory
MAIN_SOURCES = main.cpp RIC.cpp

RTP_SOURCES = $(RTP_DIR)/packet_sniffer.cpp \
              $(RTP_DIR)/qoe_profiler.cpp \
              $(RTP_DIR)/rtp_processor.cpp \
              $(RTP_DIR)/utils.cpp

MULTI_CONN_SOURCES = $(MULTI_CONN_DIR)/controller.cpp \
                    $(MULTI_CONN_DIR)/control_interface.cpp \
                    $(MULTI_CONN_DIR)/link_performance.cpp \
                    $(MULTI_CONN_DIR)/rtp_packet_queue.cpp

PERF_SOURCES = $(LOG_DIR)/Logger.cpp

# All sources
SOURCES = $(MAIN_SOURCES) \
          $(RTP_SOURCES) \
          $(MULTI_CONN_SOURCES) \
          $(PERF_SOURCES)

# Generate object files list
OBJECTS = $(SOURCES:.cpp=.o)

# Executable name
EXECUTABLE = RIC

# Dependencies
DEPS = $(SOURCES:.cpp=.d)

all: $(EXECUTABLE)

# Link
$(EXECUTABLE): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

# Compile and generate dependencies
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(DEPS)

# Clean
clean:
	rm -f $(OBJECTS) $(EXECUTABLE) $(DEPS)
	rm -f *.o *.d
	rm -f $(RTP_DIR)/*.o $(RTP_DIR)/*.d
	rm -f $(MULTI_CONN_DIR)/*.o $(MULTI_CONN_DIR)/*.d
	rm -f $(LOG_DIR)/*.o $(LOG_DIR)/*.d

# Show compile commands (for debugging)
show:
	@echo "Sources: $(SOURCES)"
	@echo "Objects: $(OBJECTS)"
	@echo "Dependencies: $(DEPS)"

.PHONY: all clean show