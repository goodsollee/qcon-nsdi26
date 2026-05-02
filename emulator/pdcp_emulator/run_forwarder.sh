#!/usr/bin/env bash
# updated_run_forwarders.sh - Script to connect all network namespaces with modified tun_forwarder
# Updated to use 5G terminology: CU, DU, UE_stack, UE_PDCP
#
# Usage: sudo ./updated_run_forwarders.sh [num_links]

HOST_IF="enp58s0"

# Number of links to configure
NUM_LINKS=${1:-2}

# Current timestamp: YYYYMMDD_HHMMSS
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Ensure top-level logs directory exists
mkdir -p ./emulator_logs

# Create subdirectory named by the current timestamp
LOG_DIR=./emulator_logs/${TIMESTAMP}
mkdir -p "${LOG_DIR}"

echo "Logs will be stored in: ${LOG_DIR}"

# Validate number of links
if [ "$NUM_LINKS" -lt 1 ] || [ "$NUM_LINKS" -gt 4 ]; then
    echo "Error: Number of links must be between 1 and 4"
    echo "Usage: $0 [num_links]"
    exit 1
fi

# Make sure we're running as root
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root"
    exit 1
fi

# Ensure tun_forwarder is available and up to date
make -q 2>/dev/null
if [ $? -eq 0 ]; then
    echo "tun_forwarder is already up to date."
else
    echo "Compiling tun_forwarder..."
    make || { echo "Failed to compile tun_forwarder"; exit 1; }
fi

# Kill existing forwarder processes
echo "===== STOPPING ANY RUNNING FORWARDERS ====="
pkill -f tun_forwarder || true
sleep 1

# Function to start a forwarder between two namespaces
start_forwarder() {
    local ns1=$1
    local tun1=$2
    local ns2=$3
    local tun2=$4
    local extra_args=${5:-""}
    
    local log_file="$LOG_DIR/${ns1}_${tun1}_to_${ns2}_${tun2}.log"
    local cmd="./tun_forwarder --folder $LOG_DIR --du $ns1 $tun1 $ns2 $tun2"
    local desc="$ns1:$tun1 <-> $ns2:$tun2"
    
    echo "Starting forwarder: $desc"
    echo "Log file: $log_file"
    $cmd > "$log_file" 2>&1 &
    local pid=$!
    echo "$pid" >> /tmp/tun_forwarders.pid
    echo "  [PID: $pid] $cmd"
    
    # Give a short delay to allow the forwarder to start
    sleep 0.2
}

# Function to start a forwarder for the UE stack (PHY/MAC/RLC)
start_ue_stack_forwarder() {
    local ns1=$1
    local tun1=$2
    local ns2=$3
    local tun2=$4
    
    local log_file="$LOG_DIR/ue_stack_${ns1}_${tun1}_to_${ns2}_${tun2}.log"
    local cmd="./tun_forwarder --folder $LOG_DIR --ue $ns1 $tun1 $ns2 $tun2"
    local desc="$ns1:$tun1 <-> $ns2:$tun2 (UE stack)"
    
    echo "Starting UE stack forwarder: $desc"
    echo "Log file: $log_file"
    $cmd > "$log_file" 2>&1 &
    local pid=$!
    echo "$pid" >> /tmp/tun_forwarders.pid
    echo "  [PID: $pid] $cmd"
    
    # Give a short delay to allow the forwarder to start
    sleep 0.3  # Slightly longer for UE stack forwarder
}

# Function to start a forwarder from host to a namespace
start_host_forwarder() {
    local host_tun=$1
    local ns=$2
    
    local log_file="$LOG_DIR/host_${host_tun}_to_${ns}.log"
    # Use the special host mode for added reliability
    local cmd="./tun_forwarder --folder $LOG_DIR --host-mode $host_tun $ns"
    local desc="host:$host_tun <-> $ns:tun_ue_host"
    
    echo "Starting host forwarder: $desc"
    echo "Log file: $log_file"
    $cmd > "$log_file" 2>&1 &
    local pid=$!
    echo "$pid" >> /tmp/tun_forwarders.pid
    echo "  [PID: $pid] $cmd"
    
    # Give a short delay to allow the forwarder to start
    sleep 0.2
}

# Start the multi-link forwarder
start_multi_link_forwarder() {
    local sender_ns=$1
    local sender_tun=$2
    local num_links=$3
    
    local log_file="$LOG_DIR/multi_link_forwarder.log"
    # Standard multi-link command
    local cmd="./tun_forwarder --folder $LOG_DIR --multi-link $num_links $sender_ns $sender_tun"
    
    # Add all DU parameters
    for i in $(seq 1 $num_links); do
        cmd="$cmd DU$i tun_du${i}_sender"
    done
    
    local desc="$sender_ns:$sender_tun <-> multiple DUs"
    
    echo "Starting multi-link forwarder: $desc"
    echo "Log file: $log_file"
    $cmd > "$log_file" 2>&1 &
    local pid=$!
    echo "$pid" >> /tmp/tun_forwarders.pid
    echo "  [PID: $pid] $cmd"
    
    sleep 0.5
}

# Function to check if an interface exists
check_interface() {
    local namespace=$1
    local interface=$2
    
    if [ "$namespace" = "host" ]; then
        ip link show $interface &>/dev/null
    else
        ip netns exec $namespace ip link show $interface &>/dev/null
    fi
    
    return $?
}

# Function to verify all interfaces exist
verify_interfaces() {
    echo "===== VERIFYING INTERFACES ====="
    
    # Check host interface
    if ! check_interface "host" "tun_host_ue"; then
        echo "ERROR: Host interface tun_host_ue does not exist!"
        echo "Please run setup_network.sh first."
        exit 1
    fi
    
    # Check UE_PDCP interfaces
    if ! check_interface "UE_PDCP" "tun_ue_host"; then
        echo "ERROR: UE_PDCP interface tun_ue_host does not exist!"
        echo "Please run setup_network.sh first."
        exit 1
    fi
    
    for i in $(seq 1 $NUM_LINKS); do
        if ! check_interface "UE_PDCP" "tun_pdcp_stack$i"; then
            echo "ERROR: UE_PDCP interface tun_pdcp_stack$i does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
    done
    
    # Check UE stack interfaces
    for i in $(seq 1 $NUM_LINKS); do
        if ! check_interface "UE_stack$i" "tun_stack${i}_pdcp"; then
            echo "ERROR: UE_stack$i interface tun_stack${i}_pdcp does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
        
        if ! check_interface "UE_stack$i" "tun_stack${i}_du"; then
            echo "ERROR: UE_stack$i interface tun_stack${i}_du does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
    done
    
    # Check DU namespace interfaces
    for i in $(seq 1 $NUM_LINKS); do
        if ! check_interface "DU$i" "tun_du${i}_stack"; then
            echo "ERROR: DU$i interface tun_du${i}_stack does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
        
        if ! check_interface "DU$i" "tun_du${i}_sender"; then
            echo "ERROR: DU$i interface tun_du${i}_sender does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
    done
    
    # Check CU interface
    if ! check_interface "CU" "tun_s_multi"; then
        echo "ERROR: CU interface tun_s_multi does not exist!"
        echo "Please run setup_network.sh first."
        exit 1
    fi
    
    echo "All required interfaces exist."
}

# Setup policy routing to ensure correct return path for traffic
setup_policy_routing() {
    echo "===== CONFIGURING POLICY ROUTING FOR RETURN PATH ====="
    
    # Clear any existing policy routing rules
    echo "Clearing existing policy routing rules..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "Clearing policy rules in DU$i"
        ip netns exec DU$i ip rule del lookup 100 2>/dev/null || true
        ip netns exec DU$i ip rule del lookup 100+$i 2>/dev/null || true
        
        echo "Clearing policy rules in UE_stack$i"
        ip netns exec UE_stack$i ip rule del lookup 100 2>/dev/null || true
        ip netns exec UE_stack$i ip rule del lookup 100+$i 2>/dev/null || true
    done
    
    echo "Clearing policy rules in UE_PDCP"
    ip netns exec UE_PDCP ip rule del lookup 100 2>/dev/null || true
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec UE_PDCP ip rule del lookup 100+$i 2>/dev/null || true
    done
    
    # Configure policy routing for each link and stack
    for i in $(seq 1 $NUM_LINKS); do
        local TABLE_ID=$((100 + $i))
        echo "Setting up policy routing for DU $i (table $TABLE_ID):"
        
        # 1. In DU namespace
        echo "  - Configuring policy routes in DU$i"
        ip netns exec DU$i ip rule add from 10.0.$i.1 table $TABLE_ID
        ip netns exec DU$i ip route add default via 10.1.$i.2 dev tun_du${i}_stack table $TABLE_ID
        ip netns exec DU$i ip route add 10.0.$i.0/24 dev tun_du${i}_sender table $TABLE_ID
        
        # 2. In UE stack namespace
        echo "  - Configuring policy routes in UE_stack$i"
        ip netns exec UE_stack$i ip rule add from 10.1.$i.1 table $TABLE_ID
        ip netns exec UE_stack$i ip route add default via 10.2.$i.2 dev tun_stack${i}_pdcp table $TABLE_ID
        ip netns exec UE_stack$i ip route add 10.1.$i.0/24 dev tun_stack${i}_du table $TABLE_ID
        
        # 3. In UE_PDCP namespace
        echo "  - Configuring return path in UE_PDCP for DU $i"
        ip netns exec UE_PDCP ip rule add from 192.168.200.1 fwmark $i table $TABLE_ID
        ip netns exec UE_PDCP ip route add 10.1.$i.0/24 via 10.2.$i.1 dev tun_pdcp_stack$i table $TABLE_ID
        ip netns exec UE_PDCP ip route add 10.0.$i.0/24 via 10.2.$i.1 dev tun_pdcp_stack$i table $TABLE_ID
    done
    
    echo "Policy routing configured successfully for all $NUM_LINKS DUs and UE stacks."
}

# Setup NAT marking for source preservation
setup_nat_preservation() {
    echo "===== CONFIGURING SOURCE NAT PRESERVATION ====="
    
    # Clear existing NAT rules in all namespaces
    echo "Clearing existing NAT rules..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "Clearing NAT rules in DU$i"
        ip netns exec DU$i iptables -t nat -F
        ip netns exec DU$i iptables -t mangle -F
        
        echo "Clearing NAT rules in UE_stack$i"
        ip netns exec UE_stack$i iptables -t nat -F
        ip netns exec UE_stack$i iptables -t mangle -F
    done
    
    echo "Clearing NAT rules in UE_PDCP"
    ip netns exec UE_PDCP iptables -t nat -F
    ip netns exec UE_PDCP iptables -t mangle -F
    
    # Set up packet marking and NAT for each DU
    echo "Setting up packet marking and NAT rules..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "Configuring NAT marking for DU $i:"
        
        # In DU namespace, mark packets from CU
        echo "  - Marking packets in DU$i from CU (tun_du${i}_sender)"
        ip netns exec DU$i iptables -t mangle -A PREROUTING -i tun_du${i}_sender -j MARK --set-mark $i
        
        # In UE stack namespace, preserve the marking
        echo "  - Preserving packet marking in UE_stack$i from DU (tun_stack${i}_du)"
        ip netns exec UE_stack$i iptables -t mangle -A PREROUTING -i tun_stack${i}_du -j MARK --set-mark $i
        
        # In UE_PDCP, preserve the marking
        echo "  - Preserving packet marking in UE_PDCP for stack $i (tun_pdcp_stack$i)"
        ip netns exec UE_PDCP iptables -t mangle -A PREROUTING -i tun_pdcp_stack$i -j MARK --set-mark $i
    done
    
    # Enable connection tracking in all namespaces
    echo "Enabling connection tracking..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "  - Setting connection tracking limits in DU$i"
        ip netns exec DU$i sysctl -w net.netfilter.nf_conntrack_max=131072
        
        echo "  - Setting connection tracking limits in UE_stack$i"
        ip netns exec UE_stack$i sysctl -w net.netfilter.nf_conntrack_max=131072
    done
    
    echo "  - Setting connection tracking limits in UE_PDCP"
    ip netns exec UE_PDCP sysctl -w net.netfilter.nf_conntrack_max=131072
    
    echo "Source NAT preservation configured successfully."
}

# Setup system configuration
setup_system() {
    echo "===== CONFIGURING NETWORK (PHASE 1) ====="
    
    # Enable IP forwarding in all namespaces
    echo "Enabling IP forwarding in all namespaces..."
    sysctl -w net.ipv4.ip_forward=1 > /dev/null
    ip netns exec UE_PDCP sysctl -w net.ipv4.ip_forward=1 > /dev/null
    ip netns exec CU sysctl -w net.ipv4.ip_forward=1 > /dev/null
    
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec DU$i sysctl -w net.ipv4.ip_forward=1 > /dev/null
        ip netns exec DU$i sysctl -w net.ipv4.conf.all.forwarding=1 > /dev/null
        
        ip netns exec UE_stack$i sysctl -w net.ipv4.ip_forward=1 > /dev/null
        ip netns exec UE_stack$i sysctl -w net.ipv4.conf.all.forwarding=1 > /dev/null
    done
    
    # Setup CU routing
    echo "Setting up CU routing..."
    # Clear any existing routes
    ip netns exec CU ip route flush dev tun_s_multi > /dev/null 2>&1 || true
    # Add default route
    ip netns exec CU ip route add default dev tun_s_multi
    # Add explicit route for DNS
    ip netns exec CU ip route add 8.8.8.8 dev tun_s_multi
    ip netns exec CU ip route add 9.9.9.9 dev tun_s_multi
    # Add routes to UE_PDCP
    ip netns exec CU ip route add 192.168.200.0/24 dev tun_s_multi
    
    # Make sure DNS is properly configured
    echo "Configuring DNS resolvers..."
    mkdir -p /etc/netns/CU
    echo "nameserver 8.8.8.8" > /etc/netns/CU/resolv.conf
    echo "nameserver 9.9.9.9" >> /etc/netns/CU/resolv.conf
    
    echo "===== CONFIGURING NETWORK (PHASE 2) ====="
    
    # Fix DU and UE stack namespaces routing
    for i in $(seq 1 $NUM_LINKS); do
        echo "Setting up DU$i forwarding and routing..."
        
        # DU namespace
        ip netns exec DU$i iptables -P FORWARD ACCEPT
        ip netns exec DU$i iptables -F FORWARD
        ip netns exec DU$i iptables -A FORWARD -i tun_du${i}_sender -o tun_du${i}_stack -j ACCEPT
        ip netns exec DU$i iptables -A FORWARD -i tun_du${i}_stack -o tun_du${i}_sender -j ACCEPT
        
        # Make sure we have a route to the 10.100.0.0/24 network (CU)
        ip netns exec DU$i ip route del 10.100.0.0/24 > /dev/null 2>&1 || true
        ip netns exec DU$i ip route add 10.100.0.0/24 dev tun_du${i}_sender
        
        # Set up a default route to internet via UE stack
        ip netns exec DU$i ip route del default > /dev/null 2>&1 || true
        ip netns exec DU$i ip route add default via 10.1.$i.2 dev tun_du${i}_stack
        
        # UE stack namespace
        echo "Setting up UE_stack$i forwarding and routing..."
        ip netns exec UE_stack$i iptables -P FORWARD ACCEPT
        ip netns exec UE_stack$i iptables -F FORWARD
        ip netns exec UE_stack$i iptables -A FORWARD -i tun_stack${i}_du -o tun_stack${i}_pdcp -j ACCEPT
        ip netns exec UE_stack$i iptables -A FORWARD -i tun_stack${i}_pdcp -o tun_stack${i}_du -j ACCEPT
        
        # Make sure we have proper routes
        ip netns exec UE_stack$i ip route del 10.100.0.0/24 > /dev/null 2>&1 || true
        ip netns exec UE_stack$i ip route add 10.100.0.0/24 via 10.1.$i.1 dev tun_stack${i}_du
        
        # Set up a default route to internet via UE_PDCP
        ip netns exec UE_stack$i ip route del default > /dev/null 2>&1 || true
        ip netns exec UE_stack$i ip route add default via 10.2.$i.2 dev tun_stack${i}_pdcp
    done
    
    # Fix UE_PDCP NAT and forwarding
    echo "Setting up UE_PDCP NAT and forwarding..."
    ip netns exec UE_PDCP iptables -t nat -F
    ip netns exec UE_PDCP iptables -t nat -A POSTROUTING -o tun_ue_host -j MASQUERADE
    ip netns exec UE_PDCP iptables -P FORWARD ACCEPT
    ip netns exec UE_PDCP iptables -F FORWARD
    
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec UE_PDCP iptables -A FORWARD -i tun_pdcp_stack$i -o tun_ue_host -j ACCEPT
        ip netns exec UE_PDCP iptables -A FORWARD -i tun_ue_host -o tun_pdcp_stack$i -j ACCEPT
    done
    
    # Fix issue with UE_PDCP reaching CU
    echo "Fixing UE_PDCP-to-CU routing..."
    # First, ensure the UE_PDCP has the correct route to reach the CU's multi interface
    ip netns exec UE_PDCP ip route del 10.100.0.0/24 > /dev/null 2>&1 || true
    # Route through each UE stack namespace
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec UE_PDCP ip route add 10.100.0.0/24 via 10.2.$i.1 dev tun_pdcp_stack$i
    done
    
    # Fix CU NAT
    echo "Setting up CU NAT..."
    ip netns exec CU iptables -t nat -F
    ip netns exec CU iptables -P FORWARD ACCEPT
    ip netns exec CU iptables -F FORWARD
    
    # Ensure host NAT is properly configured
    echo "Setting up host NAT on interface $HOST_IF..."
    iptables -t nat -F POSTROUTING
    iptables -t nat -A POSTROUTING -s 192.168.200.0/24 -o "$HOST_IF" -j MASQUERADE
    iptables -t nat -A POSTROUTING -s 10.0.0.0/8 -o "$HOST_IF" -j MASQUERADE
    iptables -P FORWARD ACCEPT
    iptables -F FORWARD
    iptables -A FORWARD -i tun_host_ue -o "$HOST_IF" -j ACCEPT
    iptables -A FORWARD -i "$HOST_IF" -o tun_host_ue -j ACCEPT
    
    # Add specific route from host to internal networks
    echo "Adding specific routes in host namespace..."
    ip route add 10.0.0.0/8 via 192.168.200.2 dev tun_host_ue 2>/dev/null || true
    ip route add 10.100.0.0/24 via 192.168.200.2 dev tun_host_ue 2>/dev/null || true
}

verify_connectivity() {
    echo ""
    echo "===== VERIFYING CONNECTIVITY ====="
    echo "Waiting 1 second for forwarders to initialize..."
    sleep 1
    
    # Check if UE_PDCP can ping host
    echo "Testing UE_PDCP -> host connectivity..."
    if ip netns exec UE_PDCP ping -c 1 -W 2 192.168.200.1 &>/dev/null; then
        echo "✓ UE_PDCP can reach host: SUCCESS"
    else
        echo "✗ UE_PDCP cannot reach host: FAILED"
        echo "  - Check the host-UE_PDCP forwarder log: $LOG_DIR/host_tun_host_ue_to_UE_PDCP.log"
    fi
    
    # Check connectivity for each UE_stack to UE_PDCP
    for i in $(seq 1 $NUM_LINKS); do
        echo "Testing UE_stack$i -> UE_PDCP connectivity..."
        if ip netns exec UE_stack$i ping -c 1 -W 2 10.2.$i.2 &>/dev/null; then
            echo "✓ UE_stack$i can reach UE_PDCP: SUCCESS"
        else
            echo "✗ UE_stack$i cannot reach UE_PDCP: FAILED"
            echo "  - Check the UE_stack-UE_PDCP forwarder log: $LOG_DIR/UE_stack${i}_tun_stack${i}_pdcp_to_UE_PDCP_tun_pdcp_stack${i}.log"
        fi
    done
    
    # Test internet connectivity from CU
    echo "Testing CU -> internet connectivity..."
    if ip netns exec CU ping -c 1 -W 3 9.9.9.9 &>/dev/null; then
        echo "✓ CU can reach internet: SUCCESS"
    else
        echo "✗ CU cannot reach internet: FAILED"
        echo "  - Check all forwarders and routes"
    fi
}

# Clean old logs and PID file
echo -n "" > /tmp/tun_forwarders.pid

# Verify that all required interfaces exist
verify_interfaces

# Ensure the system is properly configured
setup_system

# Set up NAT preservation with packet marking
setup_nat_preservation

# Set up policy routing for return path preservation
setup_policy_routing

echo "===== STARTING TUN FORWARDERS ====="
echo "Forwarders will run in background. PIDs saved to /tmp/tun_forwarders.pid"
echo "Logs will be saved to $LOG_DIR/"
echo ""

# 1. Start forwarder between host and UE_PDCP
echo "Starting HOST <-> UE_PDCP forwarder:"
start_host_forwarder "tun_host_ue" "UE_PDCP"

# 2. Start forwarders between UE_PDCP and each UE stack
echo ""
echo "Starting UE_PDCP <-> UE_STACK forwarders:"
for i in $(seq 1 $NUM_LINKS); do
    start_ue_stack_forwarder "UE_PDCP" "tun_pdcp_stack$i" "UE_stack$i" "tun_stack${i}_pdcp"
done

# 3. Start forwarders between UE stack and DU (with UE protocol stack)
echo ""
echo "Starting UE_STACK <-> DU forwarders with PHY/MAC/RLC stack:"
for i in $(seq 1 $NUM_LINKS); do
    start_forwarder "UE_stack$i" "tun_stack${i}_du" "DU$i" "tun_du${i}_stack"
done

# 4. Start multi-link forwarder for CU
echo ""
echo "Starting MULTI-LINK forwarder:"
start_multi_link_forwarder "CU" "tun_s_multi" "$NUM_LINKS"

# Run connectivity verification
verify_connectivity

echo ""
echo "===== ALL FORWARDERS STARTED ====="
echo ""
echo "To view logs:"
echo "  View all logs: tail -f $LOG_DIR/*.log"
echo "  View host-UE_PDCP log: tail -f $LOG_DIR/host_tun_host_ue_to_UE_PDCP.log"
echo "  View PHY/MAC/RLC stack logs: tail -f $LOG_DIR/ue_stack_*.log"
echo ""
echo "To test internet connectivity, run:"
echo "  sudo ip netns exec CU ping 9.9.9.9"
echo "  sudo ip netns exec UE_PDCP ping 9.9.9.9"
echo ""
echo "To stop all forwarders:"
echo "  sudo pkill -f tun_forwarder"
echo "  or"
echo "  sudo kill \$(cat /tmp/tun_forwarders.pid)"
echo ""
echo "Network setup is complete and all forwarders are running."
echo "Source NAT preservation with packet marking has been configured."
echo "Policy routing for return path preservation has been configured."
echo "PHY/MAC/RLC protocol stack is active in the UE_stack namespaces."