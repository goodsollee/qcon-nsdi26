#include "network_emulator.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <atomic>


static const char* NETWORK_EMULATOR_MODULE_NAME = "PHY";

NetworkEmulator::NetworkEmulator() 
    : is_running_(false), current_profile_index_(0) {
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "NetworkEmulator initialized");
}

NetworkEmulator::~NetworkEmulator() {
    Stop();
    DeleteVirtualInterface();
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "NetworkEmulator destroyed");
}

bool NetworkEmulator::Initialize(const std::string& profile_path, 
                                  const std::string& interface_name,
                                  const std::string& peer_interface_name) {
    profile_path_ = profile_path;
    interface_name_ = interface_name;
    peer_interface_name_ = peer_interface_name;

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Initializing NetworkEmulator");
    if (!profile_path_.empty()) {
        LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Profile path provided: ", profile_path_);
    } else {
        LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "No profile path provided. Running without profile.");
    }

    if (!CreateVirtualInterface()) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to create virtual interface");
        return false;
    }

    if (!profile_path_.empty() && !ParseProfileFile()) {
        LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "Profile parsing failed or no valid profiles found");
    }

    // Wait for user input before starting traffic shaping
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Press any key to start traffic shaping...");
    std::cin.get();

    Start();

    LOG_INFO("main", "Network emulator running. Press Ctrl+C to stop...");

    return true;
}

bool NetworkEmulator::CreateVirtualInterface() {
    // Clean up any existing setup
    std::string cleanup_cmd = R"(
        sudo ip netns pids ns1 2>/dev/null | xargs -r kill
        sudo ip netns del ns1 2>/dev/null
        sudo ip link del veth0 2>/dev/null
        sudo ip link del veth1 2>/dev/null
        sudo ip link del veth_host 2>/dev/null
        sudo ip link del veth_ns 2>/dev/null
        sudo rm -rf /etc/netns/ns1 2>/dev/null
    )";
    system(cleanup_cmd.c_str());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Create network namespace
    std::string cmd = "sudo ip netns add ns1";
    if (system(cmd.c_str()) != 0) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to create namespace ns1");
        return false;
    }

    // Create veth pair for connectivity
    cmd = "sudo ip link add veth_host type veth peer name veth_ns";
    if (system(cmd.c_str()) != 0) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to create veth pair");
        return false;
    }

    // Move veth_ns to namespace
    cmd = "sudo ip link set veth_ns netns ns1";
    if (system(cmd.c_str()) != 0) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to move veth_ns to namespace");
        return false;
    }

    // Configure IP addresses
    cmd = "sudo ip addr add 192.168.100.1/24 dev veth_host";
    system(cmd.c_str());
    cmd = "sudo ip netns exec ns1 ip addr add 192.168.100.2/24 dev veth_ns";
    system(cmd.c_str());

    // Bring up interfaces
    cmd = "sudo ip link set veth_host up";
    system(cmd.c_str());
    cmd = "sudo ip netns exec ns1 ip link set veth_ns up";
    system(cmd.c_str());
    cmd = "sudo ip netns exec ns1 ip link set lo up";
    system(cmd.c_str());

    // Set up NAT in host to allow internet access from namespace
    cmd = "sudo sysctl -w net.ipv4.ip_forward=1";
    system(cmd.c_str());
    cmd = "sudo iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -o " + interface_name_ + " -j MASQUERADE";
    system(cmd.c_str());
    cmd = "sudo iptables -A FORWARD -i " + interface_name_ + " -o veth_host -j ACCEPT";
    system(cmd.c_str());
    cmd = "sudo iptables -A FORWARD -o " + interface_name_ + " -i veth_host -j ACCEPT";
    system(cmd.c_str());

    // Set up routing in namespace
    cmd = "sudo ip netns exec ns1 ip route add default via 192.168.100.1";
    system(cmd.c_str());

    // Set up DNS in namespace
    cmd = "sudo mkdir -p /etc/netns/ns1";
    system(cmd.c_str());
    cmd = "sudo bash -c 'echo \"nameserver 8.8.8.8\nnameserver 8.8.4.4\" > /etc/netns/ns1/resolv.conf'";
    system(cmd.c_str());

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Virtual interfaces created and configured successfully");
    return true;
}

void NetworkEmulator::DeleteVirtualInterface() {
    // Clean up NAT rules
    std::string cmd = "sudo iptables -t nat -D POSTROUTING -s 192.168.100.0/24 -o " + interface_name_ + " -j MASQUERADE";
    system(cmd.c_str());
    cmd = "sudo iptables -D FORWARD -i " + interface_name_ + " -o veth_host -j ACCEPT";
    system(cmd.c_str());
    cmd = "sudo iptables -D FORWARD -o " + interface_name_ + " -i veth_host -j ACCEPT";
    system(cmd.c_str());

    // Kill all processes in namespace
    cmd = "sudo ip netns pids ns1 2>/dev/null | xargs -r kill";
    system(cmd.c_str());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Delete virtual interfaces and namespace
    cmd = "sudo ip link del veth_host 2>/dev/null";  // This also removes the peer
    system(cmd.c_str());
    
    cmd = "sudo ip netns del ns1 2>/dev/null";
    system(cmd.c_str());
    cmd = "sudo rm -rf /etc/netns/ns1";
    system(cmd.c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Virtual interfaces deleted and cleaned up");
}

void NetworkEmulator::Start() {
    if (is_running_)
        return;

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Starting emulation loop");
    is_running_ = true;
    emulation_thread_ = std::thread(&NetworkEmulator::EmulationLoop, this);
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Emulation thread created");
}

void NetworkEmulator::Stop() {
    if (!is_running_)
        return;

    is_running_ = false;
    if (emulation_thread_.joinable())
        emulation_thread_.join();
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Stopped network emulation");
}

bool NetworkEmulator::ParseProfileFile() {
    std::ifstream file(profile_path_);
    if (!file.is_open()) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to open network profile file: ", profile_path_);
        return false;
    }

    std::string line;
    // Skip header
    std::getline(file, line);

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string token;
        NetworkProfile profile;

        // Parse CSV format: timestamp,bandwidth,latency
        std::getline(ss, token, ',');
        profile.timestamp_ms = std::stoll(token);
        std::getline(ss, token, ',');
        profile.bandwidth_kbps = std::stod(token);
        std::getline(ss, token, ',');
        profile.latency_ms = std::stod(token);

        network_profiles_.push_back(profile);
    }

    if (network_profiles_.empty()) {
        LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "No valid profiles found in network profile file");
        return false;
    }

    if (!network_profiles_.empty()) {
        profile_duration_ms_ =
            network_profiles_.back().timestamp_ms - network_profiles_.front().timestamp_ms;
        // Normalize timestamps relative to first entry
        int64_t base_timestamp = network_profiles_[0].timestamp_ms;
        for (auto& profile : network_profiles_) {
            profile.timestamp_ms -= base_timestamp;
        }
    }

    // Sort profiles by timestamp to ensure correct order
    std::sort(network_profiles_.begin(), network_profiles_.end(),
              [](const NetworkProfile& a, const NetworkProfile& b) {
                  return a.timestamp_ms < b.timestamp_ms;
              });

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Parsed and sorted ", network_profiles_.size(), " profiles from file");
    return true;
}

void NetworkEmulator::EmulationLoop() {
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Entering emulation loop");

    using Clock = std::chrono::steady_clock;
    auto start_time = Clock::now();
    auto next_update_time = start_time;

    int loops_done = 0;

    while (is_running_) {
        if (network_profiles_.empty()) {
            LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "No profiles loaded, stopping emulation");
            break;
        }

        if (current_profile_index_ >= network_profiles_.size()) {
            // One loop done
            loops_done++;

            // Check repeat condition
            if (loop_ || loops_done < repeat_count_) {
                if (profile_duration_ms_ > 0) {
                    next_update_time += std::chrono::milliseconds(profile_duration_ms_);
                } else {
                    next_update_time = Clock::now();
                }
                current_profile_index_ = 0;
                continue;
            } else {
                LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "End of traffic shaping");
                break;
            }
        }

        auto before_update = Clock::now();

        const auto& current_profile = network_profiles_[current_profile_index_];
        ApplyNetworkConditions(current_profile.bandwidth_kbps, current_profile.latency_ms);

        auto after_update = Clock::now();
        auto overhead = std::chrono::duration_cast<std::chrono::microseconds>(after_update - before_update);

        if (current_profile_index_ + 1 < network_profiles_.size()) {
            const auto& next_profile = network_profiles_[current_profile_index_ + 1];
            int64_t time_diff = next_profile.timestamp_ms - current_profile.timestamp_ms;

            auto sleep_duration = std::chrono::milliseconds(time_diff) - overhead;
            if (sleep_duration.count() > 0) {
                next_update_time += std::chrono::milliseconds(time_diff);
                auto adjusted_sleep_time = next_update_time - overhead;
                std::this_thread::sleep_until(adjusted_sleep_time);
            }
            current_profile_index_++;
        } else {
            current_profile_index_++;
        }
    }

    is_running_ = false;
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Exiting emulation loop");
}


void NetworkEmulator::ApplyNetworkConditions(double bandwidth_kbps, double latency_ms) {
    // Apply tc rules to veth_ns in namespace
    std::string cmd = "sudo ip netns exec ns1 tc qdisc change dev veth_ns root netem rate " + 
                     std::to_string(bandwidth_kbps) + "kbit delay " + 
                     std::to_string(latency_ms) + "ms limit 50000";
    
    if (system(cmd.c_str()) != 0) {
        // If change fails, try to add the qdisc
        cmd = "sudo ip netns exec ns1 tc qdisc add dev veth_ns root netem rate " + 
              std::to_string(bandwidth_kbps) + "kbit delay " + 
              std::to_string(latency_ms) + "ms limit 50000";
        
        if (system(cmd.c_str()) != 0) {
            LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to apply tc rules to veth_ns");
            return;
        }
    }

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Applied to veth_ns - Rate: ", 
             bandwidth_kbps, " kbps, Delay: ", latency_ms, " ms");

    // Log the tc rules for verification
    cmd = "sudo ip netns exec ns1 tc qdisc show dev veth_ns";
    system(cmd.c_str());
}