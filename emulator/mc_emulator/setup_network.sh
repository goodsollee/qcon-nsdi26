#!/usr/bin/env bash
# setup_namespace_tun.sh - Creates a simple network namespace with TUN interface

# Make sure we're running as root
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root"
    exit 1
fi

echo "===== CLEANING UP EXISTING SETUP ====="
# Clean up existing namespaces
ip netns del app_ns 2>/dev/null || true

# Clean up existing TUN interfaces
ip tuntap del mode tun dev tun_host 2>/dev/null || true

# Drop ANY pre-existing iptables rule referencing 192.168.100.0/24 (our app subnet)
# or tun_host (our TUN device). Without this, every setup run leaves a duplicate
# MASQUERADE/FORWARD rule behind, which after ~100 runs jams conntrack and breaks
# DNS resolution from app_ns.
while iptables -t nat -C POSTROUTING -s 192.168.100.0/24 -o "$(ip -o -4 route show to default | awk '{print $5}' | head -n1)" -j MASQUERADE 2>/dev/null; do
    iptables -t nat -D POSTROUTING -s 192.168.100.0/24 -o "$(ip -o -4 route show to default | awk '{print $5}' | head -n1)" -j MASQUERADE
done
# Catch any other host-iface MASQUERADE rules that may have accumulated for our subnet
while read -r line; do
    [ -z "$line" ] && continue
    iptables -t nat -D POSTROUTING $(echo "$line" | sed -E 's/^-A POSTROUTING //') 2>/dev/null || true
done < <(iptables -t nat -S POSTROUTING 2>/dev/null | grep -F "192.168.100.0/24")
# Drop any FORWARD rules referencing tun_host (forward chain accumulates similarly)
while read -r line; do
    [ -z "$line" ] && continue
    iptables -D FORWARD $(echo "$line" | sed -E 's/^-A FORWARD //') 2>/dev/null || true
done < <(iptables -S FORWARD 2>/dev/null | grep -F "tun_host")

echo "===== CREATING NETWORK NAMESPACE ====="
# Create a network namespace for the application
ip netns add app_ns

echo "===== CREATING TUN INTERFACES ====="
# Create TUN interface in host namespace. If it survived prior runs (BUSY), bring
# it down and try del a couple times — kernel sometimes needs a tick to release.
for attempt in 1 2 3; do
    if ip tuntap add dev tun_host mode tun 2>/dev/null; then break; fi
    ip link set tun_host down 2>/dev/null || true
    ip tuntap del mode tun dev tun_host 2>/dev/null || true
    sleep 1
done
# Disable IPv6 on tun_host BEFORE bringing it up — otherwise IPv6 router
# solicitations leak into the TUN read pipe and the emulator's NsFWD thread
# crashes with "Not an IPv4 packet (version=2)" → SIGABRT cascade that kills
# pdcp_du_link via zmq::error_t. Earlier successful runs happened before the
# host accumulated enough IPv6 sources to cause this; persistent state across
# many runs surfaces it.
sysctl -w net.ipv6.conf.tun_host.disable_ipv6=1 > /dev/null 2>&1 || true
ip addr flush dev tun_host 2>/dev/null || true
ip addr add 192.168.100.1/24 dev tun_host
ip link set tun_host up

# Create TUN interface in app namespace
ip netns exec app_ns ip tuntap add dev tun_app mode tun
ip netns exec app_ns ip addr add 192.168.100.2/24 dev tun_app
ip netns exec app_ns ip link set tun_app up

# Set up default route in app namespace
ip netns exec app_ns ip route add default via 192.168.100.1 dev tun_app

# Enable IP forwarding in host
sysctl -w net.ipv4.ip_forward=1 > /dev/null

# Setup NAT for app namespace to access internet
HOST_IF="$(ip -o -4 route show to default | awk '{print $5}' | head -n1)"
echo "Using host interface: $HOST_IF for NAT"

ip netns exec app_ns iptables -t nat -F
ip netns exec app_ns iptables -t nat -A POSTROUTING -o tun_app -j MASQUERADE
ip netns exec app_ns iptables -P FORWARD ACCEPT
ip netns exec app_ns iptables -F FORWARD

iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -o $HOST_IF -j MASQUERADE
iptables -A FORWARD -i tun_host -o $HOST_IF -j ACCEPT
iptables -A FORWARD -i $HOST_IF -o tun_host -m state --state RELATED,ESTABLISHED -j ACCEPT

echo "===== NETWORK SETUP COMPLETE ====="
echo "Created namespace: app_ns"
echo "Created TUN interfaces:"
echo "  - tun_host (192.168.100.1) in host namespace"
echo "  - tun_app (192.168.100.2) in app_ns namespace"
echo ""
echo "To run a command in the app namespace:"
echo "  ip netns exec app_ns <command>"
echo ""
echo "To test internet connectivity from app namespace:"
echo "  ip netns exec app_ns ping 8.8.8.8"
echo ""
echo "Start the tunnel forwarder with:"
echo "  ./tun_forwarder"
