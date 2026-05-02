#!/bin/bash
# setup_qcon.sh - Script to set up the QCON environment

set -e  # Exit on error

echo "Setting up QCON environment..."

# Check if running as root (required for XDP)
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root for XDP functionality."
    echo "Please run with: sudo $0"
    exit 1
fi

# Check for required dependencies
check_dependency() {
    if ! command -v $1 &> /dev/null; then
        echo "Error: $1 is required but not installed."
        echo "Please install $1 with: sudo apt-get install $2"
        exit 1
    fi
}

check_dependency clang clang
check_dependency llvm-strip llvm
check_dependency make make

# Install required BPF development packages
if ! dpkg -l | grep -q libbpf-dev || ! dpkg -l | grep -q libelf-dev || ! dpkg -l | grep -q linux-headers-$(uname -r); then
    echo "Installing BPF development packages..."
    apt-get update
    apt-get install -y libbpf-dev libelf-dev linux-headers-$(uname -r)
fi

# Update config.json to use QCON scheduler
echo "Configuring QCON scheduler..."
sed -i 's/"scheduler_name": "[^"]*"/"scheduler_name": "qcon"/g' config.json

# Clean and build
echo "Cleaning and building RIC with QCON support..."
make clean
make -j$(nproc)

echo "QCON setup complete! You can now run the RIC with:"
echo "  sudo ./bin/ric_main config.json"
echo ""
echo "Note: QCON requires root privileges to use XDP for RTP header extraction."