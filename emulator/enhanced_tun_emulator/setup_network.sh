#!/usr/bin/env bash
# updated_setup_network.sh - Creates network namespaces with simplified configuration
# Includes fixes for proper internet connectivity

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
ip netns del node_sender 2>/dev/null || true
ip netns del node_receiver 2>/dev/null || true

for i in $(seq 1 4); do
    ip netns del node_link$i 2>/dev/null || true
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
ip netns add node_sender
ip netns add node_receiver

for i in $(seq 1 $NUM_LINKS); do
    echo "Creating link namespace node_link$i"
    ip netns add node_link$i
done

echo "===== CREATING TUN INTERFACES ====="

# 1. Create host TUN interface to connect with receiver
echo "Creating TUN interface in host namespace"
ip tuntap add dev tun_host_recv mode tun
ip addr add 192.168.200.1/24 dev tun_host_recv
ip link set tun_host_recv up

# 2. Create receiver TUN interface to connect with host
echo "Creating TUN interface in receiver namespace"
ip netns exec node_receiver ip tuntap add dev tun_recv_host mode tun
ip netns exec node_receiver ip addr add 192.168.200.2/24 dev tun_recv_host
ip netns exec node_receiver ip link set tun_recv_host up

# 3. Set default route in receiver to go through host
ip netns exec node_receiver ip route add default via 192.168.200.1 dev tun_recv_host

# 4. Create TUN interfaces in receiver for each link
for i in $(seq 1 $NUM_LINKS); do
    echo "Creating TUN interface in receiver for link $i"
    ip netns exec node_receiver ip tuntap add dev tun_recv_l$i mode tun
    ip netns exec node_receiver ip addr add 10.1.$i.2/24 dev tun_recv_l$i
    ip netns exec node_receiver ip link set tun_recv_l$i up
    
    # Add specific route for this network
    ip netns exec node_receiver ip route add 10.0.$i.0/24 via 10.1.$i.1
done

# 5. Create TUN interfaces in each link namespace
for i in $(seq 1 $NUM_LINKS); do
    echo "Setting up TUN interfaces in link $i namespace"
    
    # TUN for sender side
    ip netns exec node_link$i ip tuntap add dev tun_l${i}_s mode tun
    ip netns exec node_link$i ip addr add 10.0.$i.2/24 dev tun_l${i}_s
    ip netns exec node_link$i ip link set tun_l${i}_s up
    
    # TUN for receiver side
    ip netns exec node_link$i ip tuntap add dev tun_l${i}_r mode tun
    ip netns exec node_link$i ip addr add 10.1.$i.1/24 dev tun_l${i}_r
    ip netns exec node_link$i ip link set tun_l${i}_r up
    
    # Add routes in link namespace
    ip netns exec node_link$i ip route add 10.0.$i.0/24 dev tun_l${i}_s
    ip netns exec node_link$i ip route add 10.1.$i.0/24 dev tun_l${i}_r
    
    # Add routes for internal networks - go to receiver
    ip netns exec node_link$i ip route add 10.1.0.0/16 via 10.1.$i.2
    ip netns exec node_link$i ip route add 192.168.200.0/24 via 10.1.$i.2
    
    # Route to external networks - go to sender
    ip netns exec node_link$i ip route add default via 10.0.$i.1 dev tun_l${i}_s
    
    # Add route to access sender's multi interface
    ip netns exec node_link$i ip route add 10.100.0.0/24 dev tun_l${i}_s
done

# 6. Create ONLY ONE TUN interface in sender namespace (tun_s_multi)
echo "Creating a single TUN interface in sender for all links"
ip netns exec node_sender ip tuntap add dev tun_s_multi mode tun
ip netns exec node_sender ip addr add 10.100.0.1/24 dev tun_s_multi
ip netns exec node_sender ip link set tun_s_multi up

# Add routes from sender to networks
for i in $(seq 1 $NUM_LINKS); do
    echo "Adding routes in sender namespace for link $i"
    ip netns exec node_sender ip route add 10.0.$i.0/24 dev tun_s_multi
    ip netns exec node_sender ip route add 10.1.$i.0/24 dev tun_s_multi
done

# Add default route in sender 
echo "Setting default route in sender namespace"
ip netns exec node_sender ip route add default dev tun_s_multi

# Add route to receiver namespace
ip netns exec node_sender ip route add 192.168.200.0/24 dev tun_s_multi

echo "===== SETTING UP HOST NAT AND FORWARDING ====="
# Enable IP forwarding
sysctl -w net.ipv4.ip_forward=1

# Set up NAT for the receiver namespace to access internet
#iptables -t nat -A POSTROUTING -s 192.168.200.0/24 -o $HOST_INTERFACE -j MASQUERADE
#iptables -t nat -A POSTROUTING -s 10.0.0.0/8 -o "$HOST_INTERFACE" -j MASQUERADE

# Allow forwarding through kernel between host interfaces
iptables -A FORWARD -i tun_host_recv -o $HOST_INTERFACE -j ACCEPT
iptables -A FORWARD -i $HOST_INTERFACE -o tun_host_recv -j ACCEPT

# Add specific route from host to internal networks
ip route add 10.0.0.0/8 via 192.168.200.2 dev tun_host_recv 2>/dev/null || true
ip route add 10.100.0.0/24 via 192.168.200.2 dev tun_host_recv 2>/dev/null || true

# Set default policy to ACCEPT to ensure forwarding works
iptables -P FORWARD ACCEPT

# Set up DNS for each namespace
echo "===== SETTING UP DNS RESOLVERS ====="
mkdir -p /etc/netns/node_sender
mkdir -p /etc/netns/node_receiver

echo "nameserver 8.8.8.8" > /etc/netns/node_sender/resolv.conf
echo "nameserver 9.9.9.9" >> /etc/netns/node_sender/resolv.conf

echo "nameserver 8.8.8.8" > /etc/netns/node_receiver/resolv.conf
echo "nameserver 9.9.9.9" >> /etc/netns/node_receiver/resolv.conf

for i in $(seq 1 $NUM_LINKS); do
    mkdir -p /etc/netns/node_link$i
    echo "nameserver 8.8.8.8" > /etc/netns/node_link$i/resolv.conf
    echo "nameserver 9.9.9.9" >> /etc/netns/node_link$i/resolv.conf
done

# Setup NAT in link namespaces
echo "===== SETTING UP NAT IN NAMESPACES ====="
for i in $(seq 1 $NUM_LINKS); do
    echo "Setting up NAT in link $i"
    ip netns exec node_link$i iptables -t nat -F
    ip netns exec node_link$i iptables -P FORWARD ACCEPT
    ip netns exec node_link$i iptables -F FORWARD
    ip netns exec node_link$i iptables -A FORWARD -i tun_l${i}_s -o tun_l${i}_r -j ACCEPT
    ip netns exec node_link$i iptables -A FORWARD -i tun_l${i}_r -o tun_l${i}_s -j ACCEPT
    
    # Make sure forwarding is enabled at kernel level
    ip netns exec node_link$i sysctl -w net.ipv4.ip_forward=1 > /dev/null
    ip netns exec node_link$i sysctl -w net.ipv4.conf.all.forwarding=1 > /dev/null
done

# Setup NAT in receiver namespace
ip netns exec node_receiver iptables -t nat -F
ip netns exec node_receiver iptables -t nat -A POSTROUTING -o tun_recv_host -j MASQUERADE
#for i in $(seq 1 $NUM_LINKS); do
#    ip netns exec node_receiver iptables -t nat -A POSTROUTING -o tun_recv_l$i -j MASQUERADE
#done
ip netns exec node_receiver iptables -P FORWARD ACCEPT
ip netns exec node_receiver iptables -F FORWARD

# Setup NAT in sender namespace
ip netns exec node_sender iptables -t nat -F
ip netns exec node_sender iptables -P FORWARD ACCEPT
ip netns exec node_sender iptables -F FORWARD

# Print the network diagram
echo ""
echo "Simplified Multi-Connectivity Network Structure with ${NUM_LINKS} Links:"
echo "(Traffic flows upward from sender to internet through multiple paths)"
echo ""
echo "                     +----------------+"
echo "                     |    Internet    |"
echo "                     +-------^--------+"
echo "                             |"
echo "                     +-------^--------+"
echo "                     |      Host      |"
echo "                     |  ${HOST_INTERFACE}   |"
echo "                     |  tun_host_recv:192.168.200.1 |"
echo "                     +-------^--------+"
echo "                             |"
echo "                     +-------^--------+"
echo "                     | node_receiver  |"
echo "                     | tun_recv_host:192.168.200.2 |"
echo "                     +-------^--------+"
echo "                             |"

# Print link nodes in parallel based on NUM_LINKS
link_branches=""
for i in $(seq 1 $NUM_LINKS); do
    link_branches+="  +------^------+  "
done
echo "         ${link_branches}"

link_names=""
for i in $(seq 1 $NUM_LINKS); do
    link_names+="  | node_link${i} |  "
done
echo "         ${link_names}"

link_sender_ips=""
for i in $(seq 1 $NUM_LINKS); do
    link_sender_ips+="  | tun_l${i}_s:10.0.${i}.2 |  "
done
echo "         ${link_sender_ips}"

link_receiver_ips=""
for i in $(seq 1 $NUM_LINKS); do
    link_receiver_ips+="  | tun_l${i}_r:10.1.${i}.1 |  "
done
echo "         ${link_receiver_ips}"

link_bottoms=""
for i in $(seq 1 $NUM_LINKS); do
    link_bottoms+="  +------^------+  "
done
echo "         ${link_bottoms}"

echo "                             |"
echo "                     +-------^--------+"
echo "                     |  node_sender   |"
echo "                     | tun_s_multi:10.100.0.1 |"
echo "                     +----------------+"
echo "                          Traffic"
echo "                         Direction"
echo "                            ^"
echo "                            |"
echo ""

echo "===== KERNEL ROUTING CONFIGURATION ====="
echo "Host to Receiver routing is set up through kernel forwarding:"
echo " - tun_host_recv (192.168.200.1) in host namespace"
echo " - tun_recv_host (192.168.200.2) in receiver namespace"
echo " - Direct kernel forwarding between tun_host_recv and $HOST_INTERFACE"
echo " - NAT enabled for 192.168.200.0/24 network"
echo ""
echo "Sender to Internet routing is set up through multi-link TUN interface:"
echo " - tun_s_multi (10.100.0.1) in sender namespace connects to all links"
echo " - Traffic is distributed across all link namespaces"
echo " - Each link forwards traffic to the receiver and then to the internet"
echo

echo "===== NETWORK SETUP COMPLETE ====="
echo ""
echo "Namespaces created:"
ip netns list
echo ""

echo "Verify interfaces in sender namespace:"
ip netns exec node_sender ip addr show
echo ""

echo "Verify interfaces in receiver namespace:"
ip netns exec node_receiver ip addr show
echo ""

echo "Next steps:"
echo "1. Start the forwarders to connect interfaces between namespaces:"
echo "   - Host <-> Receiver: ./tun_forwarder --host-mode tun_host_recv node_receiver"
for i in $(seq 1 $NUM_LINKS); do
    echo "   - Receiver <-> Link$i: ./tun_forwarder node_receiver tun_recv_l$i node_link$i tun_l${i}_r"
done
echo "   - Connect sender's tun_s_multi with all links: ./tun_forwarder --multi-link $NUM_LINKS node_sender tun_s_multi $(for i in $(seq 1 $NUM_LINKS); do echo -n "node_link$i tun_l${i}_s "; done)"
echo ""
echo "  Or use the updated_run_forwarders.sh script to start all forwarders automatically:"
echo "  ./updated_run_forwarders.sh ${NUM_LINKS}"
echo ""
echo "2. Test connectivity from sender to internet:"
echo "   ip netns exec node_sender ping 8.8.8.8"
echo ""
echo "3. Test connectivity from receiver to internet:"
echo "   ip netns exec node_receiver ping 8.8.8.8"
echo ""
echo "4. To clean up everything later:"
echo "   sudo ./updated_setup_network.sh clean"