#!/bin/bash
# setup_ric.sh - Script to set up the RIC development environment

# Exit on error
set -e

echo "Setting up RIC development environment..."

# Create directory structure if it doesn't exist
mkdir -p include/RIC
mkdir -p src/RIC
mkdir -p build/RIC
mkdir -p bin
mkdir -p logs

# Check if log.hpp exists (if not, create from log.h)
if [ ! -f "include/log.hpp" ] && [ -f "include/log.h" ]; then
    echo "Renaming log.h to log.hpp..."
    cp include/log.h include/log.hpp
elif [ ! -f "include/log.hpp" ] && [ ! -f "include/log.h" ]; then
    echo "Warning: log.hpp not found. Please ensure it exists."
fi

# Copy the Makefile if it doesn't exist
if [ ! -f "Makefile" ]; then
    echo "Creating Makefile..."
    cp Makefile.ric Makefile
fi

# Create basic RIC config if it doesn't exist
if [ ! -f "config.csv" ]; then
    echo "Creating default config.csv..."
    cat > config.csv << 'EOF'
# Format: type,id,ip_address,kpm_port,rc_port,extra_params
# RIC entry must be first
RIC,ric-001,127.0.0.1,0,0,log_level=INFO
# CU and DU entries follow
CU,cu-001,127.0.0.1,5555,5556,max_ues=128
DU,du-001,127.0.0.1,5557,5558,cell_id=1,max_prbs=100
DU,du-002,127.0.0.1,5559,5560,cell_id=2,max_prbs=100
EOF
fi

# Check for required dependencies
echo "Checking for required dependencies..."

# Check for ZeroMQ
if ! pkg-config --exists libzmq || ! ldconfig -p | grep -q libzmq; then
    echo "ZeroMQ not found. Please install libzmq3-dev:"
    echo "sudo apt-get install libzmq3-dev"
    exit 1
fi

echo "ZeroMQ found. Ready to build."
echo ""
echo "To build the RIC component only:"
echo "  make ric-only"
echo ""
echo "To build everything (including XDP):"
echo "  make"
echo ""
echo "To specify log level:"
echo "  make LOG_LEVEL=DEBUG ric-only"
echo ""
echo "To run:"
echo "  ./bin/ric_main config.csv"