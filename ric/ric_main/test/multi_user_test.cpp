#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>
#include <csignal>
#include <sys/resource.h>
#include <sys/time.h>
#include <iomanip>
#include <unordered_map>
#include <jsoncpp/json/json.h>
#include <mutex>

using namespace std;

// Global variables for signal handling
volatile sig_atomic_t g_running = 1;

// Performance monitoring vars
struct SystemUsage {
    double cpu_percent;
    long memory_kb;
    chrono::system_clock::time_point timestamp;
};

vector<SystemUsage> system_usage_log;

// Signal handler
void signal_handler(int sig) {
    g_running = 0;
}

// Get current process CPU and memory usage
SystemUsage getCurrentUsage(pid_t pid) {
    SystemUsage usage;
    usage.timestamp = chrono::system_clock::now();
    
    // Get CPU usage
    static clock_t last_cpu_time = 0;
    static chrono::system_clock::time_point last_time_point;
    
    // Read /proc/[pid]/stat for CPU usage
    stringstream stat_file;
    stat_file << "/proc/" << pid << "/stat";
    ifstream stat(stat_file.str());
    
    if (stat.is_open()) {
        string line;
        getline(stat, line);
        
        // Parse the stat file
        istringstream iss(line);
        vector<string> tokens;
        string token;
        
        while (getline(iss, token, ' ')) {
            tokens.push_back(token);
        }
        
        // utime is at index 13, stime is at index 14
        if (tokens.size() > 14) {
            clock_t utime = stol(tokens[13]);
            clock_t stime = stol(tokens[14]);
            clock_t total_time = utime + stime;
            
            auto now = chrono::system_clock::now();
            
            if (last_cpu_time > 0) {
                auto elapsed_ticks = total_time - last_cpu_time;
                auto elapsed_time = chrono::duration_cast<chrono::microseconds>(now - last_time_point).count();
                
                // Convert to percentage (100% per core)
                double cpu_usage = (elapsed_ticks * 1000000.0) / (elapsed_time * sysconf(_SC_CLK_TCK));
                usage.cpu_percent = cpu_usage;
            } else {
                usage.cpu_percent = 0.0;
            }
            
            last_cpu_time = total_time;
            last_time_point = now;
        }
    }
    
    // Get memory usage
    stringstream status_file;
    status_file << "/proc/" << pid << "/status";
    ifstream status(status_file.str());
    
    if (status.is_open()) {
        string line;
        while (getline(status, line)) {
            if (line.find("VmRSS:") != string::npos) {
                istringstream iss(line);
                string label, value, unit;
                iss >> label >> value >> unit;
                usage.memory_kb = stol(value);
                break;
            }
        }
    }
    
    return usage;
}

namespace ric {
    // Simple RTP packet structure
    struct PacketInfo {
        uint32_t timestamp;
        uint16_t sequence_number;
        uint8_t  marker;
        uint8_t  payload_type;
        uint8_t  is_rtp;
        uint32_t packet_size;
        uint32_t src_ip;
        uint16_t src_port;
        uint32_t dst_ip;
        uint16_t dst_port;
        uint32_t arrival_time_ms;
        uint32_t total_len;
    };

    // Frame delivery status
    enum class FrameDeliveryStatus {
        IN_PROGRESS,
        COMPLETE,
        TIMEOUT
    };

    // Frame information
    struct FrameInfo {
        uint32_t timestamp;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point last_packet_time;
        std::chrono::steady_clock::time_point marker_arrival_time;
        std::vector<uint16_t> sequence_numbers;
        std::vector<bool> packets_received;
        uint32_t total_size;
        uint32_t received_size;
        uint32_t required_size;
        FrameDeliveryStatus status;
        double frame_delivery_progress;
        bool has_marker;
        std::chrono::milliseconds delivery_time;
        uint32_t dst_ip;
        std::string user_id;
    };

    // Video payload type for WebRTC
    constexpr uint8_t VIDEO_PAYLOAD_TYPE = 112;

    // Mock QoE processor for testing
    class MockQoEProcessor {
    public:
        MockQoEProcessor() = default;
        
        bool initialize(const std::string& interface, bool test_mode = false) {
            interface_name_ = interface;
            test_mode_ = test_mode;
            return true;
        }
        
        bool start() {
            running_ = true;
            processing_thread_ = std::thread(&MockQoEProcessor::processingThread, this);
            return true;
        }
        
        void stop() {
            running_ = false;
            if (processing_thread_.joinable()) {
                processing_thread_.join();
            }
        }
        
        void addRtpPackets(const std::vector<PacketInfo>& packets) {
            std::lock_guard<std::mutex> lock(packets_mutex_);
            for (const auto& packet : packets) {
                packet_queue_.push_back(packet);
            }
        }
        
        std::unordered_map<uint32_t, FrameInfo> getActiveFramesForUser(const std::string& user_id) const {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            
            if (user_frames_.find(user_id) != user_frames_.end()) {
                return user_frames_.at(user_id);
            }
            
            return {};
        }
        
        std::vector<std::string> getActiveUsers() const {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            std::vector<std::string> users;
            
            for (const auto& [user_id, frames] : user_frames_) {
                users.push_back(user_id);
            }
            
            return users;
        }
        
    private:
        std::atomic<bool> running_{false};
        std::thread processing_thread_;
        std::vector<PacketInfo> packet_queue_;
        std::mutex packets_mutex_;
        
        // User frame tracking
        mutable std::mutex frames_mutex_;
        std::unordered_map<std::string, std::unordered_map<uint32_t, FrameInfo>> user_frames_;
        
        std::string interface_name_;
        bool test_mode_;
        
        void processingThread() {
            while (running_) {
                processPackets();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        
        void processPackets() {
            std::vector<PacketInfo> packets;
            
            {
                std::lock_guard<std::mutex> lock(packets_mutex_);
                packets.swap(packet_queue_);
            }
            
            for (const auto& packet : packets) {
                processRtpPacket(packet);
            }
        }
        
        void processRtpPacket(const PacketInfo& info) {
            if (!info.is_rtp || info.payload_type != VIDEO_PAYLOAD_TYPE) {
                return;
            }
            
            // Convert IP to string format for user ID
            std::stringstream ss;
            ss << ((info.dst_ip >> 24) & 0xFF) << "."
               << ((info.dst_ip >> 16) & 0xFF) << "."
               << ((info.dst_ip >> 8) & 0xFF) << "."
               << (info.dst_ip & 0xFF);
            std::string user_id = ss.str();
            
            // Create or update frame info
            std::lock_guard<std::mutex> lock(frames_mutex_);
            auto& user_frame_map = user_frames_[user_id];
            
            if (user_frame_map.find(info.timestamp) == user_frame_map.end()) {
                FrameInfo frame;
                frame.timestamp = info.timestamp;
                frame.start_time = std::chrono::steady_clock::now();
                frame.last_packet_time = frame.start_time;
                frame.total_size = 0;
                frame.received_size = 0;
                frame.required_size = 0; // Will be updated when marker arrives
                frame.status = FrameDeliveryStatus::IN_PROGRESS;
                frame.frame_delivery_progress = 0.0;
                frame.has_marker = false;
                frame.dst_ip = info.dst_ip;
                frame.user_id = user_id;
                
                user_frame_map[info.timestamp] = frame;
            }
            
            auto& frame = user_frame_map[info.timestamp];
            frame.total_size += info.packet_size;
            frame.received_size += info.packet_size;
            frame.last_packet_time = std::chrono::steady_clock::now();
            
            // Update progress
            if (frame.total_size > 0) {
                frame.frame_delivery_progress = static_cast<double>(frame.received_size) / frame.total_size;
            }
            
            // If marker packet, mark the frame as complete
            if (info.marker) {
                frame.has_marker = true;
                frame.marker_arrival_time = frame.last_packet_time;
                frame.required_size = frame.total_size;
                
                // Update status if fully received
                if (frame.received_size >= frame.required_size) {
                    frame.status = FrameDeliveryStatus::COMPLETE;
                    frame.delivery_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        frame.last_packet_time - frame.start_time
                    );
                }
            }
        }
    };

    // Mock state manager for testing
    class MockStateManager {
    public:
        MockStateManager() = default;
        ~MockStateManager() = default;

        bool initialize() {
            return true;
        }

        void updatePdcpStatus(const std::string& user_id, const std::string& metrics) {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            pdcp_metrics_[user_id] = metrics;
        }

        void updateRlcStatus(const std::string& user_id, const std::string& metrics) {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            rlc_metrics_[user_id] = metrics;
        }

        std::string getPdcpStatus(const std::string& user_id) const {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            if (pdcp_metrics_.find(user_id) != pdcp_metrics_.end()) {
                return pdcp_metrics_.at(user_id);
            }
            return "";
        }

        std::string getRlcStatus(const std::string& user_id) const {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            if (rlc_metrics_.find(user_id) != rlc_metrics_.end()) {
                return rlc_metrics_.at(user_id);
            }
            return "";
        }

    private:
        mutable std::mutex metrics_mutex_;
        std::unordered_map<std::string, std::string> pdcp_metrics_;
        std::unordered_map<std::string, std::string> rlc_metrics_;
    };

    // Mock scheduler interface
    class SchedulerInterface {
    public:
        virtual ~SchedulerInterface() = default;
        virtual bool initialize() { return true; }
        virtual bool start() { return true; }
        virtual void stop() {}
    };
    
    // Mock QCON scheduler
    class QconScheduler : public SchedulerInterface {
    public:
        QconScheduler() = default;
        ~QconScheduler() = default;
        
        void setQoEProcessor(std::shared_ptr<MockQoEProcessor> qoe_processor) {
            qoe_processor_ = qoe_processor;
        }
        
    private:
        std::shared_ptr<MockQoEProcessor> qoe_processor_;
    };
}

// Class to generate dummy RTP packets for a specific user
class RtpGenerator {
public:
    RtpGenerator(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, uint32_t ssrc) 
        : src_ip_(src_ip), dst_ip_(dst_ip), src_port_(src_port), dst_port_(dst_port), 
          ssrc_(ssrc), seq_num_(0), timestamp_(0) {
        
        // Seed random engine with the src_ip to get deterministic but different
        // behavior for each generator
        random_engine_.seed(src_ip);
    }

    // Generate a new RTP packet
    ric::PacketInfo generatePacket(bool is_marker = false) {
        ric::PacketInfo info;
        info.is_rtp = 1;
        info.src_ip = src_ip_;
        info.dst_ip = dst_ip_;
        info.src_port = src_port_;
        info.dst_port = dst_port_;
        info.sequence_number = seq_num_++;
        info.timestamp = timestamp_;
        info.marker = is_marker ? 1 : 0;
        info.payload_type = ric::VIDEO_PAYLOAD_TYPE;
        
        // Random but realistic packet size for video (typically 1000-1400 bytes)
        std::uniform_int_distribution<uint32_t> size_dist(1000, 1400);
        info.packet_size = size_dist(random_engine_);
        info.total_len = info.packet_size;
        
        // Set arrival time to now
        info.arrival_time_ms = chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now().time_since_epoch()
        ).count();
        
        return info;
    }
    
    // Generate a frame with multiple packets
    vector<ric::PacketInfo> generateFrame(int num_packets) {
        vector<ric::PacketInfo> packets;
        
        // Generate packets for this frame
        for (int i = 0; i < num_packets; i++) {
            bool is_marker = (i == num_packets - 1);
            packets.push_back(generatePacket(is_marker));
        }
        
        // Increment timestamp for next frame (90kHz clock for video)
        // For 30fps video: 90000/30 = 3000 ticks per frame
        timestamp_ += 3000;
        
        return packets;
    }
    
    uint32_t getTimestamp() const {
        return timestamp_;
    }

private:
    uint32_t src_ip_;
    uint32_t dst_ip_;
    uint16_t src_port_;
    uint16_t dst_port_;
    uint32_t ssrc_;
    uint16_t seq_num_;
    uint32_t timestamp_;
    mt19937 random_engine_;
};

// Class to generate KPM metrics for a user
class KpmGenerator {
public:
    KpmGenerator(string user_id, int link_id) 
        : user_id_(user_id), link_id_(link_id), pdcp_tx_bytes_(0), rlc_ack_bytes_(0) {
        
        // Seed random for this user
        random_engine_.seed(hash<string>{}(user_id));
    }
    
    // Generate PDCP metrics
    string generatePdcpMetrics() {
        Json::Value metrics;
        metrics["user_id"] = user_id_;
        metrics["link_id"] = link_id_;
        
        // Generate some random increment (100KB to 500KB)
        std::uniform_int_distribution<uint64_t> bytes_dist(100000, 500000);
        uint64_t new_bytes = bytes_dist(random_engine_);
        
        pdcp_tx_bytes_ += new_bytes;
        metrics["pdcp_tx_bytes"] = Json::UInt64(pdcp_tx_bytes_);
        
        // Add some other metrics
        metrics["tx_packets"] = 1000 + (pdcp_tx_bytes_ / 1500); // Approx packet count
        metrics["timestamp"] = Json::UInt64(chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()
        ).count());
        
        Json::FastWriter writer;
        return writer.write(metrics);
    }
    
    // Generate RLC metrics
    string generateRlcMetrics() {
        Json::Value metrics;
        metrics["user_id"] = user_id_;
        metrics["link_id"] = link_id_;
        
        // RLC ack bytes lag behind PDCP tx bytes
        std::uniform_real_distribution<double> ack_ratio(0.7, 0.95);
        rlc_ack_bytes_ = static_cast<uint64_t>(pdcp_tx_bytes_ * ack_ratio(random_engine_));
        
        metrics["rlc_ack_bytes"] = Json::UInt64(rlc_ack_bytes_);
        
        // Add channel quality indicators
        std::uniform_real_distribution<double> sinr_dist(5.0, 25.0);
        std::uniform_real_distribution<double> bler_dist(0.0, 0.1);
        
        metrics["sinr_db"] = sinr_dist(random_engine_);
        metrics["bler"] = bler_dist(random_engine_);
        metrics["timestamp"] = Json::UInt64(chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()
        ).count());
        
        Json::FastWriter writer;
        return writer.write(metrics);
    }

private:
    string user_id_;
    int link_id_;
    uint64_t pdcp_tx_bytes_;
    uint64_t rlc_ack_bytes_;
    mt19937 random_engine_;
};

// Multi-user test class
class MultiUserTest {
public:
    MultiUserTest()
        : qoe_processor_(make_shared<ric::MockQoEProcessor>()),
          state_manager_(make_shared<ric::MockStateManager>()),
          scheduler_(make_shared<ric::QconScheduler>()) {
    }
    
    bool initialize(int num_users, const string& config_file) {
        num_users_ = num_users;
        
        cout << "Initializing multi-user test with " << num_users << " users" << endl;
        
        // Set up signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        
        // Initialize the QoE processor
        if (!qoe_processor_->initialize("tun_host", true)) {
            cerr << "Failed to initialize QoE processor" << endl;
            return false;
        }
        
        if (!qoe_processor_->start()) {
            cerr << "Failed to start QoE processor" << endl;
            return false;
        }
        
        // Initialize state manager
        state_manager_->initialize();
        
        // Initialize scheduler
        if (!scheduler_->initialize()) {
            cerr << "Failed to initialize scheduler" << endl;
            return false;
        }
        
        // Set QoE processor for the QCON scheduler
        scheduler_->setQoEProcessor(qoe_processor_);
        
        // Start the scheduler
        if (!scheduler_->start()) {
            cerr << "Failed to start scheduler" << endl;
            return false;
        }
        
        // Create users
        setupUsers(num_users);
        
        return true;
    }
    
    void run(int duration_seconds) {
        auto start_time = chrono::steady_clock::now();
        auto end_time = start_time + chrono::seconds(duration_seconds);
        
        cout << "Running multi-user test for " << duration_seconds << " seconds" << endl;
        
        // Start performance monitoring thread
        atomic<bool> monitor_running(true);
        thread monitor_thread([&]() {
            pid_t pid = getpid();
            while (monitor_running && g_running) {
                auto usage = getCurrentUsage(pid);
                system_usage_log.push_back(usage);
                
                // Print current usage
                cout << "\rCPU: " << fixed << setprecision(1) << usage.cpu_percent 
                     << "%, Memory: " << usage.memory_kb << " KB" << flush;
                
                this_thread::sleep_for(chrono::milliseconds(500));
            }
            cout << endl;
        });
        
        // Main test loop
        while (g_running && chrono::steady_clock::now() < end_time) {
            // Generate RTP packets for all users
            for (auto& [user_id, generator] : rtp_generators_) {
                // Generate a frame with random number of packets (5-20)
                uniform_int_distribution<int> num_packets_dist(5, 20);
                int num_packets = num_packets_dist(random_engine_);
                
                auto packets = generator.generateFrame(num_packets);
                qoe_processor_->addRtpPackets(packets);
            }
            
            // Generate KPM metrics for all users
            for (auto& [user_id, generators] : kpm_generators_) {
                for (auto& generator : generators) {
                    // PDCP metrics
                    string pdcp_metrics = generator.generatePdcpMetrics();
                    state_manager_->updatePdcpStatus(user_id, pdcp_metrics);
                    
                    // RLC metrics
                    string rlc_metrics = generator.generateRlcMetrics();
                    state_manager_->updateRlcStatus(user_id, rlc_metrics);
                }
            }
            
            // Sleep for a frame interval (33ms for 30fps)
            this_thread::sleep_for(chrono::milliseconds(33));
        }
        
        // Stop monitoring and wait for thread to finish
        monitor_running = false;
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        
        // Write results to file
        writeResultsToFile(num_users_);
    }
    
    void cleanup() {
        cout << "Cleaning up..." << endl;
        
        // Stop the scheduler
        scheduler_->stop();
        
        // Stop the QoE processor
        qoe_processor_->stop();
    }

private:
    int num_users_;
    shared_ptr<ric::MockQoEProcessor> qoe_processor_;
    shared_ptr<ric::MockStateManager> state_manager_;
    shared_ptr<ric::QconScheduler> scheduler_;
    
    // User tracking
    map<string, RtpGenerator> rtp_generators_;
    map<string, vector<KpmGenerator>> kpm_generators_;
    
    // Random engines
    mt19937 random_engine_{static_cast<unsigned>(chrono::system_clock::now().time_since_epoch().count())};
    
    void setupUsers(int num_users) {
        cout << "Setting up " << num_users << " users..." << endl;
        
        for (int i = 0; i < num_users; i++) {
            // Generate a unique user ID (IP-based for simplicity)
            stringstream ss;
            ss << "10.0." << (i / 255) << "." << (i % 255);
            string user_id = ss.str();
            
            // Convert user_id to uint32_t IP format
            vector<string> ip_parts;
            stringstream ip_parts_ss(user_id);
            string part;
            while (getline(ip_parts_ss, part, '.')) {
                ip_parts.push_back(part);
            }
            
            uint32_t dst_ip = (stoi(ip_parts[0]) << 24) | 
                              (stoi(ip_parts[1]) << 16) | 
                              (stoi(ip_parts[2]) << 8) | 
                              stoi(ip_parts[3]);
            
            // Source is always the same for our test
            uint32_t src_ip = (192 << 24) | (168 << 16) | (1 << 8) | 1;
            
            // Random ports
            uniform_int_distribution<uint16_t> port_dist(10000, 60000);
            uint16_t src_port = port_dist(random_engine_);
            uint16_t dst_port = port_dist(random_engine_);
            
            // Random SSRC
            uniform_int_distribution<uint32_t> ssrc_dist(1000, 9999999);
            uint32_t ssrc = ssrc_dist(random_engine_);
            
            // Create RTP generator for this user
            rtp_generators_.emplace(user_id, RtpGenerator(src_ip, dst_ip, src_port, dst_port, ssrc));
            
            // Create KPM generators for multiple links per user
            vector<KpmGenerator> user_kpm_generators;
            
            // For each user, create 2 links (typical dual-connectivity scenario)
            for (int link_id = 0; link_id < 2; link_id++) {
                user_kpm_generators.emplace_back(user_id, link_id);
            }
            
            kpm_generators_[user_id] = move(user_kpm_generators);
            
            cout << "Created user " << user_id << " with 2 links" << endl;
        }
    }
    
    void writeResultsToFile(int num_users) {
        // Create a timestamped file name
        auto now = chrono::system_clock::now();
        auto now_time_t = chrono::system_clock::to_time_t(now);
        stringstream ss;
        ss << "multi_user_test_" << num_users << "users_" << put_time(localtime(&now_time_t), "%Y%m%d_%H%M%S") << ".csv";
        
        string filename = ss.str();
        ofstream file(filename);
        
        if (!file.is_open()) {
            cerr << "Failed to open output file: " << filename << endl;
            return;
        }
        
        // Write CSV header
        file << "timestamp,num_users,cpu_percent,memory_kb" << endl;
        
        // Write data
        for (const auto& usage : system_usage_log) {
            auto time_t = chrono::system_clock::to_time_t(usage.timestamp);
            file << put_time(localtime(&time_t), "%Y-%m-%d %H:%M:%S") << ","
                 << num_users << ","
                 << usage.cpu_percent << ","
                 << usage.memory_kb << endl;
        }
        
        file.close();
        
        cout << "Results written to " << filename << endl;
    }
};

void printUsage(const char* program_name) {
    cout << "Usage: " << program_name << " [options]" << endl;
    cout << "Options:" << endl;
    cout << "  -n, --num-users NUM     Specify the number of users (default: 5)" << endl;
    cout << "  -d, --duration SEC      Test duration in seconds (default: 30)" << endl;
    cout << "  -c, --config PATH       Path to config file (default: config.json)" << endl;
    cout << "  -h, --help              Show this help message" << endl;
}

int main(int argc, char* argv[]) {
    int num_users = 5;       // Default number of users
    int duration = 30;       // Default duration in seconds
    string config_file = "config.json";  // Default config file path
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        
        if (arg == "-n" || arg == "--num-users") {
            if (i + 1 < argc) {
                num_users = stoi(argv[++i]);
            } else {
                cerr << "--num-users requires a number" << endl;
                printUsage(argv[0]);
                return 1;
            }
        } else if (arg == "-d" || arg == "--duration") {
            if (i + 1 < argc) {
                duration = stoi(argv[++i]);
            } else {
                cerr << "--duration requires a number" << endl;
                printUsage(argv[0]);
                return 1;
            }
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                cerr << "--config requires a path" << endl;
                printUsage(argv[0]);
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            cerr << "Unknown option: " << arg << endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    // Run the test
    MultiUserTest test;
    
    if (!test.initialize(num_users, config_file)) {
        cerr << "Failed to initialize test" << endl;
        return 1;
    }
    
    test.run(duration);
    test.cleanup();
    
    return 0;
}