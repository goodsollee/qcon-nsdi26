#!/usr/bin/env bash
# updated_setup_network.sh - Creates network namespaces with 5G naming convention
# Includes support for PHY/MAC/RLC stack between DUs and UE stacks

# Make sure we're running as root
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root"
    exit 1
fi

# Configuration parameters
HOST_INTERFACE=$(ip -o -4 route show to default | awk '{print $5}' | head -n1)  # Interface name of host's internet connection
NUM_LINKS=${1:-2}         # Number of link namespaces to create (default: 2, max: 4)

# Validate number of links
if [ "$NUM_LINKS" -lt 1 ] || [ "$NUM_LINKS" -gt 4 ]; then
    echo "Error: Number of links must be between 1 and 4"
    echo "Usage: $0 [num_links]"
    exit 1
fi

echo "===== STOPPING ANY RUNNING FORWARDERS ====="
pkill -f tun_forwarder 2>/dev/null || true
sleep 1

echo "===== PERFORMING THOROUGH CLEANUP ====="

# Clean up any existing iptables rules
echo "Removing iptables rules..."
iptables -t nat -F
iptables -F FORWARD

# Remove host TUN devices
echo "Removing host TUN devices..."
for tun in $(ip link show | grep -o 'tun[^:]*'); do
    ip link set $tun down 2>/dev/null || true
    ip link delete $tun 2>/dev/null || true
done

# Delete all existing namespaces used by our setup
echo "Removing network namespaces..."
ip netns del CU 2>/dev/null || true
ip netns del UE_PDCP 2>/dev/null || true
ip netns del node_receiver 2>/dev/null || true

for i in $(seq 1 4); do
    ip netns del DU$i 2>/dev/null || true
    ip netns del UE_stack$i 2>/dev/null || true
    ip netns del node_link$i 2>/dev/null || true
    ip netns del node_link_stack$i 2>/dev/null || true
done

# Also clean up any older namespaces that might exist from previous runs
ip netns del tun_sender 2>/dev/null || true 
ip netns del tun_relay 2>/dev/null || true

# Final check for any TUN interfaces and delete them
for tun in $(ip tuntap show | grep -o 'tun[^ ]*'); do
    echo "Removing lingering TUN device: $tun"
    ip tuntap del mode tun dev $tun 2>/dev/null || true
done

# Make sure /run/netns exists
mkdir -p /run/netns

echo "===== CREATING NETWORK NAMESPACES ====="
# Create fresh network namespaces
ip netns add CU
ip netns add UE_PDCP

for i in $(seq 1 $NUM_LINKS); do
    echo "Creating DU namespace DU$i"
    ip netns add DU$i
    
    echo "Creating UE stack namespace UE_stack$i"
    ip netns add UE_stack$i
done

echo "===== CREATING TUN INTERFACES ====="

# 1. Create host TUN interface to connect with UE_PDCP
echo "Creating TUN interface in host namespace"
ip tuntap add dev tun_host_ue mode tun
ip addr add 192.168.200.1/24 dev tun_host_ue
ip link set tun_host_ue up

# 2. Create UE_PDCP TUN interface to connect with host
echo "Creating TUN interface in UE_PDCP namespace"
ip netns exec UE_PDCP ip tuntap add dev tun_ue_host mode tun
ip netns exec UE_PDCP ip addr add 192.168.200.2/24 dev tun_ue_host
ip netns exec UE_PDCP ip link set tun_ue_host up

# 3. Set default route in UE_PDCP to go through host
ip netns exec UE_PDCP ip route add default via 192.168.200.1 dev tun_ue_host

# 4. Create TUN interfaces in UE_PDCP for each UE stack
for i in $(seq 1 $NUM_LINKS); do
    echo "Creating TUN interface in UE_PDCP for UE_stack $i"
    ip netns exec UE_PDCP ip tuntap add dev tun_pdcp_stack$i mode tun
    ip netns exec UE_PDCP ip addr add 10.2.$i.2/24 dev tun_pdcp_stack$i
    ip netns exec UE_PDCP ip link set tun_pdcp_stack$i up
    
    # Add specific route for this UE stack network
    ip netns exec UE_PDCP ip route add 10.1.$i.0/24 via 10.2.$i.1
    ip netns exec UE_PDCP ip route add 10.0.$i.0/24 via 10.2.$i.1
done

# 5. Create TUN interfaces in each UE stack namespace (between UE_PDCP and DU)
for i in $(seq 1 $NUM_LINKS); do
    echo "Setting up TUN interfaces in UE_stack $i namespace"
    
    # TUN for DU side
    ip netns exec UE_stack$i ip tuntap add dev tun_stack${i}_du mode tun
    ip netns exec UE_stack$i ip addr add 10.1.$i.2/24 dev tun_stack${i}_du
    ip netns exec UE_stack$i ip link set tun_stack${i}_du up
    
    # TUN for UE_PDCP side
    ip netns exec UE_stack$i ip tuntap add dev tun_stack${i}_pdcp mode tun
    ip netns exec UE_stack$i ip addr add 10.2.$i.1/24 dev tun_stack${i}_pdcp
    ip netns exec UE_stack$i ip link set tun_stack${i}_pdcp up
    
    # Add routes in UE stack namespace
    ip netns exec UE_stack$i ip route add 10.1.$i.0/24 dev tun_stack${i}_du
    ip netns exec UE_stack$i ip route add 10.2.$i.0/24 dev tun_stack${i}_pdcp
    
    # Add default route to UE_PDCP
    ip netns exec UE_stack$i ip route add default via 10.2.$i.2 dev tun_stack${i}_pdcp
    
    # Add specific routes for networks
    ip netns exec UE_stack$i ip route add 10.0.$i.0/24 via 10.1.$i.1 dev tun_stack${i}_du
    ip netns exec UE_stack$i ip route add 192.168.200.0/24 via 10.2.$i.2 dev tun_stack${i}_pdcp
    ip netns exec UE_stack$i ip route add 10.100.0.0/24 via 10.1.$i.1 dev tun_stack${i}_du
done

# 6. Create TUN interfaces in each DU namespace
for i in $(seq 1 $NUM_LINKS); do
    echo "Setting up TUN interfaces in DU $i namespace"
    
    # TUN for sender side
    ip netns exec DU$i ip tuntap add dev tun_du${i}_sender mode tun
    ip netns exec DU$i ip addr add 10.0.$i.2/24 dev tun_du${i}_sender
    ip netns exec DU$i ip link set tun_du${i}_sender up
    
    # TUN for UE stack side
    ip netns exec DU$i ip tuntap add dev tun_du${i}_stack mode tun
    ip netns exec DU$i ip addr add 10.1.$i.1/24 dev tun_du${i}_stack
    ip netns exec DU$i ip link set tun_du${i}_stack up
    
    # Add routes in DU namespace
    ip netns exec DU$i ip route add 10.0.$i.0/24 dev tun_du${i}_sender
    ip netns exec DU$i ip route add 10.1.$i.0/24 dev tun_du${i}_stack
    
    # Add routes for internal networks - go to UE stack
    ip netns exec DU$i ip route add 10.2.0.0/16 via 10.1.$i.2
    ip netns exec DU$i ip route add 192.168.200.0/24 via 10.1.$i.2
    
    # Route to external networks - go to sender
    ip netns exec DU$i ip route add default via 10.0.$i.1 dev tun_du${i}_sender
    
    # Add route to access sender's multi interface
    ip netns exec DU$i ip route add 10.100.0.0/24 dev tun_du${i}_sender
done

# 7. Create ONLY ONE TUN interface in sender namespace (tun_s_multi)
echo "Creating a single TUN interface in sender for all DUs"
ip netns exec CU ip tuntap add dev tun_s_multi mode tun
ip netns exec CU ip addr add 10.100.0.1/24 dev tun_s_multi
ip netns exec CU ip link set tun_s_multi up

# To prevent packet fragmentation, set MTU to 1400
ip netns exec CU ip link set dev tun_s_multi mtu 1400

# Add routes from sender to networks
for i in $(seq 1 $NUM_LINKS); do
    echo "Adding routes in sender namespace for DU $i"
    ip netns exec CU ip route add 10.0.$i.0/24 dev tun_s_multi
    ip netns exec CU ip route add 10.1.$i.0/24 dev tun_s_multi
    ip netns exec CU ip route add 10.2.$i.0/24 dev tun_s_multi
done

# Add default route in sender 
echo "Setting default route in sender namespace"
ip netns exec CU ip route add default dev tun_s_multi

# Add route to UE_PDCP namespace
ip netns exec CU ip route add 192.168.200.0/24 dev tun_s_multi

echo "===== SETTING UP HOST NAT AND FORWARDING ====="
# Enable IP forwarding
sysctl -w net.ipv4.ip_forward=1

# Set up NAT for the UE_PDCP namespace to access internet
iptables -t nat -A POSTROUTING -s 192.168.200.0/24 -o $HOST_INTERFACE -j MASQUERADE
iptables -t nat -A POSTROUTING -s 10.0.0.0/8 -o "$HOST_INTERFACE" -j MASQUERADE

# Allow forwarding through kernel between host interfaces
iptables -A FORWARD -i tun_host_ue -o $HOST_INTERFACE -j ACCEPT
iptables -A FORWARD -i $HOST_INTERFACE -o tun_host_ue -j ACCEPT

# Add specific route from host to internal networks
ip route add 10.0.0.0/8 via 192.168.200.2 dev tun_host_ue 2>/dev/null || true
ip route add 10.100.0.0/24 via 192.168.200.2 dev tun_host_ue 2>/dev/null || true

# Set default policy to ACCEPT to ensure forwarding works
iptables -P FORWARD ACCEPT

# Set up DNS for each namespace
echo "===== SETTING UP DNS RESOLVERS ====="
mkdir -p /etc/netns/CU
mkdir -p /etc/netns/UE_PDCP

echo "nameserver 8.8.8.8" > /etc/netns/CU/resolv.conf
echo "nameserver 9.9.9.9" >> /etc/netns/CU/resolv.conf

echo "nameserver 8.8.8.8" > /etc/netns/UE_PDCP/resolv.conf
echo "nameserver 9.9.9.9" >> /etc/netns/UE_PDCP/resolv.conf

for i in $(seq 1 $NUM_LINKS); do
    mkdir -p /etc/netns/DU$i
    echo "nameserver 8.8.8.8" > /etc/netns/DU$i/resolv.conf
    echo "nameserver 9.9.9.9" >> /etc/netns/DU$i/resolv.conf
    
    mkdir -p /etc/netns/UE_stack$i
    echo "nameserver 8.8.8.8" > /etc/netns/UE_stack$i/resolv.conf
    echo "nameserver 9.9.9.9" >> /etc/netns/UE_stack$i/resolv.conf
done

# Setup NAT in all namespaces
echo "===== SETTING UP NAT IN NAMESPACES ====="
# For each DU namespace
for i in $(seq 1 $NUM_LINKS); do
    echo "Setting up NAT in DU $i"
    ip netns exec DU$i iptables -t nat -F
    ip netns exec DU$i iptables -P FORWARD ACCEPT
    ip netns exec DU$i iptables -F FORWARD
    ip netns exec DU$i iptables -A FORWARD -i tun_du${i}_sender -o tun_du${i}_stack -j ACCEPT
    ip netns exec DU$i iptables -A FORWARD -i tun_du${i}_stack -o tun_du${i}_sender -j ACCEPT
    
    # Make sure forwarding is enabled at kernel level
    ip netns exec DU$i sysctl -w net.ipv4.ip_forward=1 > /dev/null
    ip netns exec DU$i sysctl -w net.ipv4.conf.all.forwarding=1 > /dev/null
done

# For each UE stack namespace
for i in $(seq 1 $NUM_LINKS); do
    echo "Setting up NAT in UE_stack $i"
    ip netns exec UE_stack$i iptables -t nat -F
    ip netns exec UE_stack$i iptables -P FORWARD ACCEPT
    ip netns exec UE_stack$i iptables -F FORWARD
    ip netns exec UE_stack$i iptables -A FORWARD -i tun_stack${i}_du -o tun_stack${i}_pdcp -j ACCEPT
    ip netns exec UE_stack$i iptables -A FORWARD -i tun_stack${i}_pdcp -o tun_stack${i}_du -j ACCEPT
    
    # Make sure forwarding is enabled at kernel level
    ip netns exec UE_stack$i sysctl -w net.ipv4.ip_forward=1 > /dev/null
    ip netns exec UE_stack$i sysctl -w net.ipv4.conf.all.forwarding=1 > /dev/null
done

# Setup NAT in UE_PDCP namespace
ip netns exec UE_PDCP iptables -t nat -F
ip netns exec UE_PDCP iptables -t nat -A POSTROUTING -o tun_ue_host -j MASQUERADE
ip netns exec UE_PDCP iptables -P FORWARD ACCEPT
ip netns exec UE_PDCP iptables -F FORWARD

# Setup NAT in sender namespace
ip netns exec CU iptables -t nat -F
ip netns exec CU iptables -P FORWARD ACCEPT
ip netns exec CU iptables -F FORWARD

# Print the network diagram
echo ""
echo "5G Network Architecture with ${NUM_LINKS} DUs:"
echo "(Traffic flows upward from CU to internet through multiple paths)"
echo ""
echo "                     +----------------+"
echo "                     |    Internet    |"
echo "                     +-------^--------+"
echo "                             |"
echo "                     +-------^--------+"
echo "                     |      Host      |"
echo "                     |  ${HOST_INTERFACE}   |"
echo "                     |  tun_host_ue:192.168.200.1 |"
echo "                     +-------^--------+"
echo "                             |"
echo "                     +-------^--------+"
echo "                     |    UE_PDCP     |"
echo "                     | tun_ue_host:192.168.200.2 |"
echo "                     +-------^--------+"
echo "                             |"

# Print UE stack nodes
ue_stack_tops=""
for i in $(seq 1 $NUM_LINKS); do
    ue_stack_tops+="  +------^------+  "
done
echo "         ${ue_stack_tops}"

ue_stack_names=""
for i in $(seq 1 $NUM_LINKS); do
    ue_stack_names+="  |  UE_stack${i}  |  "
done
echo "         ${ue_stack_names}"

ue_stack_desc=""
for i in $(seq 1 $NUM_LINKS); do
    ue_stack_desc+="  |(PHY/MAC/RLC)|  "
done
echo "         ${ue_stack_desc}"

ue_stack_bottoms=""
for i in $(seq 1 $NUM_LINKS); do
    ue_stack_bottoms+="  +------^------+  "
done
echo "         ${ue_stack_bottoms}"

# Print DU nodes
du_tops=""
for i in $(seq 1 $NUM_LINKS); do
    du_tops+="  +------^------+  "
done
echo "         ${du_tops}"

du_names=""
for i in $(seq 1 $NUM_LINKS); do
    du_names+="  |     DU${i}     |  "
done
echo "         ${du_names}"

du_sender_ips=""
for i in $(seq 1 $NUM_LINKS); do
    du_sender_ips+="  | tun_du${i}_sender:10.0.${i}.2 |  "
done
echo "         ${du_sender_ips}"

du_stack_ips=""
for i in $(seq 1 $NUM_LINKS); do
    du_stack_ips+="  | tun_du${i}_stack:10.1.${i}.1 |  "
done
echo "         ${du_stack_ips}"

du_bottoms=""
for i in $(seq 1 $NUM_LINKS); do
    du_bottoms+="  +------^------+  "
done
echo "         ${du_bottoms}"

echo "                             |"
echo "                     +-------^--------+"
echo "                     |  CU   |"
echo "                     | tun_s_multi:10.100.0.1 |"
echo "                     +----------------+"
echo "                          Traffic"
echo "                         Direction"
echo "                            ^"
echo "                            |"
echo ""

echo "===== KERNEL ROUTING CONFIGURATION ====="
echo "Host to UE_PDCP routing is set up through kernel forwarding:"
echo " - tun_host_ue (192.168.200.1) in host namespace"
echo " - tun_ue_host (192.168.200.2) in UE_PDCP namespace"
echo " - Direct kernel forwarding between tun_host_ue and $HOST_INTERFACE"
echo " - NAT enabled for 192.168.200.0/24 network"
echo ""
echo "Protocol Stack:"
echo " - Each DU connects to a dedicated UE_stack namespace"
echo " - The UE_stack namespace implements PHY/MAC/RLC protocols"
echo " - Traffic flows through the full protocol stack before reaching the UE_PDCP"
echo ""

echo "===== NETWORK SETUP COMPLETE ====="
echo ""
echo "Namespaces created:"
ip netns list
echo ""

echo "Next steps:"
echo "1. Start the forwarders to connect interfaces between namespaces:"
echo "   - Host <-> UE_PDCP: ./tun_forwarder --host-mode tun_host_ue UE_PDCP"
for i in $(seq 1 $NUM_LINKS); do
    echo "   - UE_PDCP <-> UE_stack $i: ./tun_forwarder UE_PDCP tun_pdcp_stack$i UE_stack$i tun_stack${i}_pdcp"
    echo "   - UE_stack $i <-> DU $i: ./tun_forwarder --receiver UE_stack$i tun_stack${i}_du DU$i tun_du${i}_stack"
done
echo "   - Connect sender's tun_s_multi with all DUs: ./tun_forwarder --multi-link $NUM_LINKS CU tun_s_multi $(for i in $(seq 1 $NUM_LINKS); do echo -n "DU$i tun_du${i}_sender "; done)"
echo ""
echo "  Or use the updated_run_forwarders.sh script to start all forwarders automatically:"
echo "  ./updated_run_forwarders.sh ${NUM_LINKS}"
echo ""
echo "2. Test connectivity from sender to internet:"
echo "   ip netns exec CU ping 8.8.8.8"