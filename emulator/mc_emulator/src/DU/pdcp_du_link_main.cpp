#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <csignal>

#include "config_parser.hpp"
#include "network/link.hpp"
#include "pdcp_du_link.hpp"
#include "log.hpp"

#define MODULE "PDCP_DU_MAIN"

// Global atomic for clean shutdown
static std::atomic_bool g_running(true);

// Signal handler
static void signalHandler(int) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    // 1) Setup signals
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 2) Parse command-line arguments
    std::string configFile = "config.json";
    int receivePort = 11000; // Default port to receive from sender
    int sendPort = 12000;    // Default port to send to UE
    int linkId = 1;          // Default link ID

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-r" && i + 1 < argc) {
            receivePort = std::stoi(argv[++i]);
        } else if (arg == "-s" && i + 1 < argc) {
            sendPort = std::stoi(argv[++i]);
        } else if (arg == "-i" && i + 1 < argc) {
            linkId = std::stoi(argv[++i]);
        }
    }

    // 3) Load configuration
    ConfigParser parser;
    if (!parser.loadConfig(configFile)) {
        std::cerr << "Failed to load config: " << configFile << std::endl;
        return 1;
    }
    
    // Get PDCP configs
    auto allPdcpConfigs = parser.getAllPdcpConfigs();
    if (allPdcpConfigs.empty()) {
        std::cerr << "No PDCP configs found. Exiting.\n";
        return 1;
    }
    
    // Use config matching our link ID if available, otherwise use the first one
    PdcpConfig duConfig;
    bool configFound = false;
    
    // First try to find config by linkId
    for (const auto& cfg : allPdcpConfigs) {
        if (cfg.du.linkId == linkId + 1) {
            duConfig = cfg;
            configFound = true;
            break;
        }
    }
    
    // If not found, use default index or first one
    if (!configFound) {
        size_t configIndex = std::min(size_t(1), allPdcpConfigs.size() - 1);
        duConfig = allPdcpConfigs[configIndex];
        duConfig.du.linkId = linkId; // Override with our link ID
    }
    
    LOG_MODULE_INFO(MODULE, "Using DU config with linkId " << duConfig.du.linkId);

    // 4) Create the DU link
    std::unique_ptr<PdcpDULink> duLink = std::make_unique<PdcpDULink>(duConfig);

    // 5) Create a Link for receiving from pdcp_sender
    std::string receiveEndpoint = "tcp://*:" + std::to_string(receivePort);
    std::unique_ptr<Link> mainReceiver = std::make_unique<Link>(
        "PdcpDULink_Receiver_" + std::to_string(linkId),
        receiveEndpoint,         // PULL (bind) - receives from PDCP Sender
        "tcp://127.0.0.1:9999"   // PUSH (connect) - not used
    );
    LOG_MODULE_INFO(MODULE, "Listening for messages on " << receiveEndpoint);

    // 6) Create a Link for sending to UE Link
    std::string sendEndpoint = "tcp://127.0.0.1:" + std::to_string(sendPort);
    std::unique_ptr<Link> ueLink = std::make_unique<Link>(
        "PdcpDULink_Sender_" + std::to_string(linkId),
        "tcp://*:0",            // PULL (bind) - using ephemeral port, not used
        sendEndpoint            // PUSH (connect) - sends to UE Link
    );
    LOG_MODULE_INFO(MODULE, "Sending messages to UE at " << sendEndpoint);

    // Start the sender link
    ueLink->start();

    // 7) Set DU link's delivery callback to forward processed packets to UE Link
    duLink->getPhyModule()->setDeliveryCallback([&ueLink, linkId](const unsigned char* data, size_t len) {
        if (len == 0) return;
        
        // Convert data to string for ZMQ message
        std::string payload(reinterpret_cast<const char*>(data), len);
        
        // Send to UE Link
        ueLink->sendMessage("UE", "DL_DATA", payload);
        
        LOG_MODULE_INFO(MODULE, "DU link " << linkId << " forwarded " << len << " bytes to UE Link");
    });

    // 8) Register callback for incoming messages from PDCP Sender
    mainReceiver->registerCallback("DL_DATA", [&duLink, linkId](const Message& msg) {
        LOG_MODULE_INFO(MODULE, "DU link " << linkId << " received DL_DATA, size=" << msg.payload.size());
        
        if (msg.payload.empty()) return;
        
        // Copy payload to buffer
        std::vector<unsigned char> buffer(msg.payload.begin(), msg.payload.end());
        
        // Process through DU link
        // This will eventually trigger the delivery callback when processing is complete
        duLink->processPacket(buffer.data(), buffer.size());
    });

    mainReceiver->registerCallback("STATUS_REPORT", [&duLink, linkId](const Message& msg) {
        LOG_MODULE_INFO(MODULE, "UE link " << linkId << " received STATUS_REPORT, size=" << msg.payload.size());
        
        if (msg.payload.empty()) return;
        
        // Copy payload to buffer
        std::vector<unsigned char> buffer(msg.payload.begin(), msg.payload.end());
        
        // Pass to RLC Sender's processAck method
        // This assumes your ueLink has a method to access the RLC sender module
        auto rlcSender = duLink->getRlcModule();
        if (rlcSender) {
            bool result = rlcSender->processAck(buffer.data(), buffer.size());
            LOG_MODULE_INFO(MODULE, "UE link " << linkId << " processed STATUS_REPORT with result: " 
                            << (result ? "success" : "failure"));
        } else {
            LOG_MODULE_ERROR(MODULE, "UE link " << linkId << " failed to get RLC sender module");
        }
    });

    // 9) Start the receiver
    mainReceiver->start();

    LOG_MODULE_INFO(MODULE, "PDCP DU link " << linkId << " running. Ctrl-C to stop.");
    
    // 10) Main loop
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 11) Cleanup
    LOG_MODULE_INFO(MODULE, "Stopping links...");
    mainReceiver->stop();
    ueLink->stop();
    duLink.reset();

    LOG_MODULE_INFO(MODULE, "PDCP DU link " << linkId << " terminated gracefully.");
    return 0;
}