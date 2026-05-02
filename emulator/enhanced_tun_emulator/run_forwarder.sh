#!/usr/bin/env bash
# enhanced_run_forwarders.sh - Comprehensive script to connect all network namespaces with modified tun_forwarder
#
# This script:
# 1. Ensures proper system configuration (IP forwarding, iptables)
# 2. Starts all required TUN forwarders:
#    - Host <-> Receiver
#    - Receiver <-> Link nodes
#    - Link nodes <-> Sender
# 3. Validates connectivity
#
# Usage: sudo ./enhanced_run_forwarders.sh [num_links]

HOST_IF="enp58s0"

# Number of links to configure
NUM_LINKS=${1:-2}

# Directory for log files
LOG_DIR="/tmp/tun_logs"
mkdir -p $LOG_DIR

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

# Check if tun_forwarder is up-to-date:
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
    
    local log_file="$LOG_DIR/${ns1}_${tun1}_to_${ns2}_${tun2}.log"
    local cmd="./tun_forwarder $ns1 $tun1 $ns2 $tun2"
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

# Function to start a forwarder from host to a namespace
start_host_forwarder() {
    local host_tun=$1
    local ns=$2
    
    local log_file="$LOG_DIR/host_${host_tun}_to_${ns}.log"
    # Use the special host mode for added reliability
    local cmd="./tun_forwarder --host-mode $host_tun $ns"
    local desc="host:$host_tun <-> $ns:tun_recv_host"
    
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
    # Standard multi-link command without the flow-hash option
    local cmd="./tun_forwarder --multi-link $num_links $sender_ns $sender_tun"
    
    # Add all link parameters
    for i in $(seq 1 $num_links); do
        cmd="$cmd node_link$i tun_l${i}_s"
    done
    
    local desc="$sender_ns:$sender_tun <-> multiple links"
    
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
    if ! check_interface "host" "tun_host_recv"; then
        echo "ERROR: Host interface tun_host_recv does not exist!"
        echo "Please run setup_network.sh first."
        exit 1
    fi
    
    # Check receiver interfaces
    if ! check_interface "node_receiver" "tun_recv_host"; then
        echo "ERROR: Receiver interface tun_recv_host does not exist!"
        echo "Please run setup_network.sh first."
        exit 1
    fi
    
    for i in $(seq 1 $NUM_LINKS); do
        if ! check_interface "node_receiver" "tun_recv_l$i"; then
            echo "ERROR: Receiver interface tun_recv_l$i does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
    done
    
    # Check link interfaces
    for i in $(seq 1 $NUM_LINKS); do
        if ! check_interface "node_link$i" "tun_l${i}_r"; then
            echo "ERROR: Link$i interface tun_l${i}_r does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
        
        if ! check_interface "node_link$i" "tun_l${i}_s"; then
            echo "ERROR: Link$i interface tun_l${i}_s does not exist!"
            echo "Please run setup_network.sh first."
            exit 1
        fi
    done
    
    echo "All required interfaces exist."
}

# Setup NAT marking and connection tracking for source preservation
setup_nat_preservation() {
    echo "===== CONFIGURING SOURCE NAT PRESERVATION ====="
    
    # 1. Clear existing NAT rules in all link namespaces and receiver
    echo "Clearing existing NAT rules..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "Clearing NAT rules in node_link$i"
        ip netns exec node_link$i iptables -t nat -F
        ip netns exec node_link$i iptables -t mangle -F
    done
    
    echo "Clearing NAT rules in node_receiver"
    ip netns exec node_receiver iptables -t nat -F
    ip netns exec node_receiver iptables -t mangle -F
    
    # 2. Set up packet marking and NAT masquerading for each link
    echo "Setting up packet marking and NAT rules..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "Configuring NAT marking for link $i:"
        
        # In link namespace, mark packets from sender
        echo "  - Marking packets in node_link$i from sender (tun_l${i}_s)"
        ip netns exec node_link$i iptables -t mangle -A PREROUTING -i tun_l${i}_s -j MARK --set-mark $i
        #ip netns exec node_link$i iptables -t nat -A POSTROUTING -m mark --mark $i -j MASQUERADE
        
        # In receiver, preserve the marking
        echo "  - Preserving packet marking in node_receiver for link $i (tun_recv_l$i)"
        ip netns exec node_receiver iptables -t mangle -A PREROUTING -i tun_recv_l$i -j MARK --set-mark $i
        #ip netns exec node_receiver iptables -t nat -A POSTROUTING -m mark --mark $i -j MASQUERADE
    done
    
    # 3. Enable connection tracking in all namespaces
    echo "Enabling connection tracking..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "  - Setting connection tracking limits in node_link$i"
        ip netns exec node_link$i sysctl -w net.netfilter.nf_conntrack_max=131072
    done
    
    echo "  - Setting connection tracking limits in node_receiver"
    ip netns exec node_receiver sysctl -w net.netfilter.nf_conntrack_max=131072
    
    echo "Source NAT preservation configured successfully."
}

# Setup policy routing to ensure correct return path for traffic
setup_policy_routing() {
    echo "===== CONFIGURING POLICY ROUTING FOR RETURN PATH ====="
    
    # Clear any existing policy routing rules
    echo "Clearing existing policy routing rules..."
    for i in $(seq 1 $NUM_LINKS); do
        echo "Clearing policy rules in node_link$i"
        ip netns exec node_link$i ip rule del lookup 100 2>/dev/null || true
        ip netns exec node_link$i ip rule del lookup 100+$i 2>/dev/null || true
    done
    
    echo "Clearing policy rules in node_receiver"
    ip netns exec node_receiver ip rule del lookup 100 2>/dev/null || true
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec node_receiver ip rule del lookup 100+$i 2>/dev/null || true
    done
    
    # Configure policy routing for each link
    for i in $(seq 1 $NUM_LINKS); do
        local TABLE_ID=$((100 + $i))
        echo "Setting up policy routing for link $i (table $TABLE_ID):"
        
        # 1. In link namespace, set up policy routing to maintain return path
        echo "  - Configuring policy routes in node_link$i"
        
        # Add rule to route traffic from sender based on source IP
        ip netns exec node_link$i ip rule add from 10.0.$i.1 table $TABLE_ID
        
        # Set up routes in the custom table
        ip netns exec node_link$i ip route add default via 10.1.$i.2 dev tun_l${i}_r table $TABLE_ID
        ip netns exec node_link$i ip route add 10.0.$i.0/24 dev tun_l${i}_s table $TABLE_ID
        
        # 2. In receiver, ensure return path for this link
        echo "  - Configuring return path in node_receiver for link $i"
        
        # Add rule for returning traffic to sender via this link
        ip netns exec node_receiver ip rule add from 192.168.200.1 fwmark $i table $TABLE_ID
        
        # Set up routes in the custom table
        ip netns exec node_receiver ip route add 10.0.$i.0/24 via 10.1.$i.1 dev tun_recv_l$i table $TABLE_ID
    done
    
    echo "Policy routing configured successfully for all $NUM_LINKS links."
}

# Setup network (most important part - combines both fix scripts)
setup_system() {
    echo "===== CONFIGURING NETWORK (PHASE 1) ====="
    
    # Enable IP forwarding in all namespaces
    echo "Enabling IP forwarding in all namespaces..."
    sysctl -w net.ipv4.ip_forward=1 > /dev/null
    ip netns exec node_receiver sysctl -w net.ipv4.ip_forward=1 > /dev/null
    ip netns exec node_sender sysctl -w net.ipv4.ip_forward=1 > /dev/null
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec node_link$i sysctl -w net.ipv4.ip_forward=1 > /dev/null
        ip netns exec node_link$i sysctl -w net.ipv4.conf.all.forwarding=1 > /dev/null
    done
    
    # Setup sender routing
    echo "Setting up sender routing..."
    # Clear any existing routes
    ip netns exec node_sender ip route flush dev tun_s_multi > /dev/null 2>&1 || true
    # Add default route
    ip netns exec node_sender ip route add default dev tun_s_multi
    # Add explicit route for DNS
    ip netns exec node_sender ip route add 8.8.8.8 dev tun_s_multi
    ip netns exec node_sender ip route add 9.9.9.9 dev tun_s_multi
    # Add routes to receiver
    ip netns exec node_sender ip route add 192.168.200.0/24 dev tun_s_multi
    
    # Make sure DNS is properly configured
    echo "Configuring DNS resolvers..."
    mkdir -p /etc/netns/node_sender
    echo "nameserver 8.8.8.8" > /etc/netns/node_sender/resolv.conf
    echo "nameserver 9.9.9.9" >> /etc/netns/node_sender/resolv.conf
    
    echo "===== CONFIGURING NETWORK (PHASE 2) ====="
    
    # Fix link namespaces routing
    for i in $(seq 1 $NUM_LINKS); do
        echo "Setting up link$i forwarding and routing..."
        
        # Allow forwarding between interfaces in link namespace
        ip netns exec node_link$i iptables -P FORWARD ACCEPT
        ip netns exec node_link$i iptables -F FORWARD
        ip netns exec node_link$i iptables -A FORWARD -i tun_l${i}_s -o tun_l${i}_r -j ACCEPT
        ip netns exec node_link$i iptables -A FORWARD -i tun_l${i}_r -o tun_l${i}_s -j ACCEPT
        
        # Make sure we have a route to the 10.100.0.0/24 network (sender)
        ip netns exec node_link$i ip route del 10.100.0.0/24 > /dev/null 2>&1 || true
        ip netns exec node_link$i ip route add 10.100.0.0/24 dev tun_l${i}_s
        
        # Set up a default route to internet via receiver
        ip netns exec node_link$i ip route del default > /dev/null 2>&1 || true
        ip netns exec node_link$i ip route add default via 10.1.$i.2 dev tun_l${i}_r
        
        # Set up NAT - this is critical for proper function
        ip netns exec node_link$i iptables -t nat -F
        #ip netns exec node_link$i iptables -t nat -A POSTROUTING -o tun_l${i}_r -j MASQUERADE
        #ip netns exec node_link$i iptables -t nat -A POSTROUTING -o tun_l${i}_s -j MASQUERADE
    done
    
    # Fix receiver NAT and forwarding
    echo "Setting up receiver NAT and forwarding..."
    ip netns exec node_receiver iptables -t nat -F
    ip netns exec node_receiver iptables -t nat -A POSTROUTING -o tun_recv_host -j MASQUERADE
    #for i in $(seq 1 $NUM_LINKS); do
    #    ip netns exec node_receiver iptables -t nat -A POSTROUTING -o tun_recv_l$i -j MASQUERADE
    #done
    ip netns exec node_receiver iptables -P FORWARD ACCEPT
    ip netns exec node_receiver iptables -F FORWARD
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec node_receiver iptables -A FORWARD -i tun_recv_l$i -o tun_recv_host -j ACCEPT
        ip netns exec node_receiver iptables -A FORWARD -i tun_recv_host -o tun_recv_l$i -j ACCEPT
    done
    
    # Fix issue with receiver reaching sender
    echo "Fixing receiver-to-sender routing..."
    # First, ensure the receiver has the correct route to reach the sender's multi interface
    ip netns exec node_receiver ip route del 10.100.0.0/24 > /dev/null 2>&1 || true
    # Route through each link namespace
    for i in $(seq 1 $NUM_LINKS); do
        ip netns exec node_receiver ip route add 10.100.0.0/24 via 10.1.$i.1 dev tun_recv_l$i
    done
    
    # Fix sender NAT
    echo "Setting up sender NAT..."
    ip netns exec node_sender iptables -t nat -F
    #ip netns exec node_sender iptables -t nat -A POSTROUTING -o tun_s_multi -j MASQUERADE
    ip netns exec node_sender iptables -P FORWARD ACCEPT
    ip netns exec node_sender iptables -F FORWARD
    
    # Ensure host NAT is properly configured
    echo "Setting up host NAT on interface $HOST_IF..."
    iptables -t nat -F POSTROUTING
    iptables -t nat -A POSTROUTING -s 192.168.200.0/24 -o "$HOST_IF" -j MASQUERADE
    iptables -t nat -A POSTROUTING -s 10.0.0.0/8 -o "$HOST_IF" -j MASQUERADE
    iptables -P FORWARD ACCEPT
    iptables -F FORWARD
    iptables -A FORWARD -i tun_host_recv -o "$HOST_IF" -j ACCEPT
    iptables -A FORWARD -i "$HOST_IF" -o tun_host_recv -j ACCEPT
    
    # Add specific route from host to internal networks
    echo "Adding specific routes in host namespace..."
    ip route add 10.0.0.0/8 via 192.168.200.2 dev tun_host_recv 2>/dev/null || true
    ip route add 10.100.0.0/24 via 192.168.200.2 dev tun_host_recv 2>/dev/null || true
}

verify_connectivity() {
    echo ""
    echo "===== VERIFYING CONNECTIVITY ====="
    echo "Waiting 3 seconds for forwarders to initialize..."
    sleep 3
    
    # Check if receiver can ping host
    echo "Testing receiver -> host connectivity..."
    if ip netns exec node_receiver ping -c 1 -W 2 192.168.200.1 &>/dev/null; then
        echo "✓ Receiver can reach host: SUCCESS"
    else
        echo "✗ Receiver cannot reach host: FAILED"
        echo "  - Check the host-receiver forwarder log: $LOG_DIR/host_tun_host_recv_to_node_receiver.log"
    fi
    
    # Check if host can ping receiver
    echo "Testing host -> receiver connectivity..."
    if ping -c 1 -W 2 192.168.200.2 &>/dev/null; then
        echo "✓ Host can reach receiver: SUCCESS"
    else
        echo "✗ Host cannot reach receiver: FAILED"
        echo "  - Check the host-receiver forwarder log: $LOG_DIR/host_tun_host_recv_to_node_receiver.log"
    fi
    
    # Check if sender can ping receiver
    echo "Testing sender -> receiver connectivity..."
    if ip netns exec node_sender ping -c 1 -W 2 10.1.1.2 &>/dev/null; then
        echo "✓ Sender can reach receiver: SUCCESS"
    else
        echo "✗ Sender cannot reach receiver: FAILED"
        echo "  - Check forwarder logs for sender->link and link->receiver connections"
    fi
    
    # Check if receiver can ping sender
    echo "Testing receiver -> sender connectivity..."
    if ip netns exec node_receiver ping -c 1 -W 2 10.100.0.1 &>/dev/null; then
        echo "✓ Receiver can reach sender: SUCCESS"
    else
        echo "✗ Receiver cannot reach sender: FAILED"
        echo "  - Check forwarder logs for receiver->link and link->sender connections"
    fi
    
    # Test internet connectivity from receiver
    echo "Testing receiver -> internet connectivity..."
    if ip netns exec node_receiver ping -c 1 -W 3 9.9.9.9 &>/dev/null; then
        echo "✓ Receiver can reach internet: SUCCESS"
    else
        echo "✗ Receiver cannot reach internet: FAILED"
        echo "  - This depends on host-receiver and NAT configuration"
    fi
    
    # Test internet connectivity from sender
    echo "Testing sender -> internet connectivity..."
    if ip netns exec node_sender ping -c 1 -W 3 9.9.9.9 &>/dev/null; then
        echo "✓ Sender can reach internet: SUCCESS"
    else
        echo "✗ Sender cannot reach internet: FAILED"
        echo "  - Check all forwarders and routes"
    fi
}

# Clean old logs and PID file
rm -f $LOG_DIR/*.log
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

# 1. Start forwarder between host and receiver
echo "Starting HOST <-> RECEIVER forwarder:"
start_host_forwarder "tun_host_recv" "node_receiver"

# 2. Start forwarders between receiver and each link
echo ""
echo "Starting RECEIVER <-> LINK forwarders:"
for i in $(seq 1 $NUM_LINKS); do
    start_forwarder "node_receiver" "tun_recv_l$i" "node_link$i" "tun_l${i}_r"
done

# 3. Start multi-link forwarder for sender
echo ""
echo "Starting MULTI-LINK forwarder:"
start_multi_link_forwarder "node_sender" "tun_s_multi" "$NUM_LINKS"

# Run connectivity verification
verify_connectivity

echo ""
echo "===== ALL FORWARDERS STARTED ====="
echo ""
echo "To view logs:"
echo "  View all logs: tail -f $LOG_DIR/*.log"
echo "  View host-receiver log: tail -f $LOG_DIR/host_tun_host_recv_to_node_receiver.log"
echo ""
echo "To test internet connectivity, run:"
echo "  sudo ip netns exec node_sender ping 9.9.9.9"
echo "  sudo ip netns exec node_receiver ping 9.9.9.9"
echo ""
echo "To stop all forwarders:"
echo "  sudo pkill -f tun_forwarder"
echo "  or"
echo "  sudo kill \$(cat /tmp/tun_forwarders.pid)"
echo ""
echo "Network setup is complete and all forwarders are running."
echo "Source NAT preservation with packet marking has been configured."
echo "Policy routing for return path preservation has been configured."