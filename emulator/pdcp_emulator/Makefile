CXX = g++
CXXFLAGS = -Wall -Wextra -Ofast -std=c++14 -D_GNU_SOURCE -pthread -Iinclude -Iutil -Iexternal -Iexternal/readerwriterqueue
LDFLAGS = -pthread

# Let LOGLEVEL default to INFO if not specified
# Usage: `make LOGLEVEL=DEBUG` or `make LOGLEVEL=TRACE` etc.
LOGLEVEL ?= DEBUG
CXXFLAGS += -DLOGLEVEL=$(LOGLEVEL)
CXXFLAGS += -Iinclude/pdcp
CXXFLAGS += -Iinclude/rlc
CXXFLAGS += -Iinclude/mac
CXXFLAGS += -Iinclude/phy
CXXFLAGS += -Iinclude/sm

# Source and object directory structure
SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj

# Find all source files recursively in all subdirectories
SOURCES := $(shell find $(SRC_DIR) -name "*.cpp")
# Transform source paths to object paths
OBJECTS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SOURCES))
# Create a list of needed directories for objects
OBJDIRS := $(sort $(dir $(OBJECTS)))

# Main targets
all: dirs tun_forwarder

# Create required directories
dirs:
	@mkdir -p $(OBJDIRS)

# Main TUN Forwarder with PDCP support
tun_forwarder: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Generic rule for compiling objects from sources
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Clean up
.PHONY: clean all dirs

clean:
	rm -rf $(OBJ_DIR) tun_forwarder