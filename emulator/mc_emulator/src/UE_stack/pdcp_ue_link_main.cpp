#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <csignal>

#include "config_parser.hpp"
#include "network/link.hpp"
#include "pdcp_ue_link.hpp"
#include "log.hpp"

#define MODULE "PDCP_UE_MAIN"

static std::atomic_bool g_running(true);
static void signalHandler(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    // 1) Setup signals
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 2) Parse command-line arguments
    std::string configFile = "config.json";
    int receivePort = 12000;  // Default port to receive from DU
    int sendPort = 13000;     // Default port to send to Receiver
    int linkId = 0;           // Default link ID

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
    PdcpConfig ueConfig;
    bool configFound = false;
    
    // First try to find config by linkId
    for (const auto& cfg : allPdcpConfigs) {
        if (cfg.du.linkId == linkId + 1) {
            ueConfig = cfg;
            configFound = true;
            break;
        }
    }
    
    // If not found, use default index or first one
    if (!configFound) {
        size_t configIndex = std::min(size_t(2), allPdcpConfigs.size() - 1);
        ueConfig = allPdcpConfigs[configIndex];
        ueConfig.du.linkId = linkId; // Override with our link ID
    }
    
    LOG_MODULE_INFO(MODULE, "Using UE config with linkId " << ueConfig.du.linkId);

    // 4) Create the UE link
    std::unique_ptr<PdcpUELink> ueLink = std::make_unique<PdcpUELink>(ueConfig);

    // 5) Create a Link for receiving from DU Link
    std::string receiveEndpoint = "tcp://*:" + std::to_string(receivePort);
    std::unique_ptr<Link> duReceiver = std::make_unique<Link>(
        "PdcpUELink_Receiver_" + std::to_string(linkId),
        receiveEndpoint,         // PULL (bind) - receives from DU Link
        "tcp://127.0.0.1:9998"   // PUSH (connect) - not used
    );
    LOG_MODULE_INFO(MODULE, "Listening for messages on " << receiveEndpoint);

    // 6) Create a Link for sending to PDCP Receiver
    std::string sendEndpoint = "tcp://127.0.0.1:" + std::to_string(sendPort);
    std::unique_ptr<Link> receiverLink = std::make_unique<Link>(
        "PdcpUELink_Sender_" + std::to_string(linkId),
        "tcp://*:0",             // PULL (bind) - using ephemeral port, not used
        sendEndpoint             // PUSH (connect) - sends to PDCP Receiver
    );
    LOG_MODULE_INFO(MODULE, "Sending messages to Receiver at " << sendEndpoint);

    int controlPort = receivePort - 1000;
    // 6) Create a Link for sending to PDCP Receiver
    std::string controlEndpoint = "tcp://127.0.0.1:" + std::to_string(controlPort);
    std::unique_ptr<Link> controlLink = std::make_unique<Link>(
        "PdcpUELink_Control_" + std::to_string(linkId),
        "tcp://*:0",             // PULL (bind) - using ephemeral port, not used
        controlEndpoint             // PUSH (connect) - sends to PDCP Receiver
    );
    LOG_MODULE_INFO(MODULE, "Sending control messages to Receiver at " << controlEndpoint);

    // Start the sender link
    receiverLink->start();

    // 7) Set the UE link's delivery callback
    ueLink->setDeliveryCallback([&receiverLink, linkId](const unsigned char* data, size_t len) {
        if (len == 0) return;
        
        // Convert data to string for ZMQ message
        std::string payload(reinterpret_cast<const char*>(data), len);
        
        // Send to PDCP Receiver
        receiverLink->sendMessage("PDCP_RECEIVER", "DL_DATA", payload);
        
        LOG_MODULE_INFO(MODULE, "UE link " << linkId << " forwarded " << len << " bytes to PDCP Receiver");
    });

    ueLink->getPhyModule()->setStatusPduCallback([&controlLink, linkId](const RlcStatusPdu& status_pdu) {
        LOG_DEBUG("[PHY] Transmitting RLC Status PDU with ACK SN: " << status_pdu.ack_sn);
        
        // Serialize the Status PDU
        std::vector<unsigned char> pdu_buffer = status_pdu.serialize();
        
        // Convert data to string for ZMQ message
        std::string payload(reinterpret_cast<const char*>(pdu_buffer.data()), pdu_buffer.size());
            
        // Send to PDCP Receiver using the controlLink
        controlLink->sendMessage("PDCP_RECEIVER", "STATUS_REPORT", payload);
        
        LOG_MODULE_INFO(MODULE, "UE link " << linkId << " sent Status PDU with ACK SN: " << status_pdu.ack_sn);
    });

    // 8) Register callback for incoming messages from DU
    duReceiver->registerCallback("DL_DATA", [&ueLink, linkId](const Message& msg) {
        LOG_MODULE_INFO(MODULE, "UE link " << linkId << " received DL_DATA, size=" << msg.payload.size());
        
        if (msg.payload.empty()) return;
        
        // Copy payload to buffer
        std::vector<unsigned char> buffer(msg.payload.begin(), msg.payload.end());
        
        // Process through UE link
        // This will eventually trigger the delivery callback when processing is complete
        ueLink->processPacket(buffer.data(), buffer.size());
    });

    // 9) Start the receiver
    duReceiver->start();

    LOG_MODULE_INFO(MODULE, "PDCP UE link " << linkId << " running. Ctrl-C to stop.");
    
    // 10) Main loop
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 11) Cleanup
    LOG_MODULE_INFO(MODULE, "Stopping links...");
    duReceiver->stop();
    receiverLink->stop();
    ueLink.reset();

    LOG_MODULE_INFO(MODULE, "PDCP UE link " << linkId << " terminated gracefully.");
    return 0;
}