Command
1. sudo ./setup_network.sh
2. sudo ./run_forwarder.sh

Used implementation insights
1. Namespace to separtate nodes
2. TUN interface to forward packets to user space and process it for emulation
3. IP forwarding without NAT, which can incur multi-link to loss the states
4. Only NAT on the receiver -> host device to connect to the internet

Network structure
            +----------------+
            |    Internet    |
            +-------^--------+
                    |
            +-------^--------+
            |      Host      |
            |  enp58s0   |
            |  tun_host_recv:192.168.200.1 |
            +-------^--------+
                    |
            +-------^--------+
            | node_receiver  |
            | tun_recv_host:192.168.200.2 |
            +-------^--------+
                    |
+------^------+    +------^------+  
| node_link1 |    | node_link2 |  
| tun_l1_s:10.0.1.2 |    | tun_l2_s:10.0.2.2 |  
| tun_l1_r:10.1.1.1 |    | tun_l2_r:10.1.2.1 |  
+------^------+    +------^------+  
                    |
            +-------^--------+
            |  node_sender   |
            | tun_s_l1:10.0.1.1 |
            | tun_s_l2:10.0.2.1 |
            +----------------+
                Traffic
                Direction
                ^
                |


Tackled Challenges  this emulator
1. Packet fragmentation
p) PDCP encapsulation fragments packet and make several packet not to be decoded with PDCP header
s) Make the sender namespace MTU smaller than others, so that the added encapsulated header does not affect MTU fragmentation
2. Low throughput issue

