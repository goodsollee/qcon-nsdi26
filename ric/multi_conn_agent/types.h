#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <map>
#include <cstring>
#include <algorithm> // Add this for sort, find_if, max_element
#include <numeric>   // Add this for accumulate
#include "../logger/Logger.h"


namespace multiconn {

static const char* MODULE_NAME = "multiconn";

using ue_id_t = uint32_t;
using frame_id_t = uint32_t;
using packet_id_t = uint32_t;
using pdcp_sn_t = uint16_t;
using rlc_sn_t = uint16_t;
using timestamp_t = uint64_t;
using link_id_t = uint32_t;


struct RLCInfo {
    ue_id_t ue_id;
    link_id_t link_id;
    uint32_t pdcp_sn;
    rlc_sn_t rlc_sn;
    uint32_t acked_bytes;
    uint32_t queue_size;
    bool is_ack;
    timestamp_t timestamp;
};

struct PDCPPacketInfo {
    ue_id_t ue_id;
    uint32_t pdcp_sn;
    frame_id_t frame_id;
    std::vector<uint8_t> data;
    timeval timestamp;
    std::map<rlc_sn_t, RLCInfo> rlc_info;  // Simplified to direct mapping of SN to RLCInfo
    bool is_marker;  // Indicates end of frame
};

struct LinkConfig {
    link_id_t link_id;
    std::string link_name;         
    uint32_t priority;           
    double min_throughput_mbps;  
    uint32_t max_latency_ms;     
    uint32_t protocol_delay;
};

struct DelayInfo {
    ue_id_t ue_id;
    link_id_t link_id;
    timestamp_t drx_wake_time;
    uint32_t xn_delay_ms;
    timestamp_t timestamp;
};

enum class RICMessageType {
    LINK_ALLOCATION,   // For link steering decisions
    PDCP_PACKETS,      // For packet forwarding
    LINK_STATUS,       // For link status updates
    ERROR             // For error conditions
};

struct RICLinkControl {
    link_id_t link_id;
    double allocation_ratio;      // Split ratio for this link (0.0 - 1.0)
    bool operator==(const RICLinkControl& other) const {
        return link_id == other.link_id && allocation_ratio == other.allocation_ratio;
    }
};

struct RICPacketData {
    pdcp_sn_t pdcp_sn;
    std::vector<uint8_t> data;
};


class RICControlMessage {
public:
    RICControlMessage() : message_type(RICMessageType::LINK_ALLOCATION), timestamp(0) {}

    // Common fields
    RICMessageType message_type;
    ue_id_t ue_id;
    frame_id_t frame_id;
    uint64_t timestamp;          // Microseconds since epoch

    // Link allocation information
    std::vector<RICLinkControl> link_controls;

    // PDCP packet information
    std::vector<RICPacketData> pdcp_packets;

    // Serialization for network transmission
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        buffer.reserve(4096);  // Larger initial size for packet data

        // Add message type
        buffer.push_back(static_cast<uint8_t>(message_type));

        // Add UE ID
        append_to_buffer(buffer, ue_id);

        // Add frame ID
        append_to_buffer(buffer, frame_id);

        // Add timestamp
        append_to_buffer(buffer, timestamp);

        if (message_type == RICMessageType::LINK_ALLOCATION) {
            // Serialize link controls
            uint32_t num_links = static_cast<uint32_t>(link_controls.size());
            append_to_buffer(buffer, num_links);

            for (const auto& link : link_controls) {
                append_to_buffer(buffer, link.link_id);
                append_to_buffer(buffer, link.allocation_ratio);
            }
        }
        else if (message_type == RICMessageType::PDCP_PACKETS) {
            // Serialize PDCP packets
            uint32_t num_packets = static_cast<uint32_t>(pdcp_packets.size());
            append_to_buffer(buffer, num_packets);

            for (const auto& packet : pdcp_packets) {
                append_to_buffer(buffer, packet.pdcp_sn);
                
                // Add packet data size and data
                uint32_t data_size = static_cast<uint32_t>(packet.data.size());
                append_to_buffer(buffer, data_size);
                buffer.insert(buffer.end(), packet.data.begin(), packet.data.end());
            }
        }

        return buffer;
    }

    bool deserialize(const std::vector<uint8_t>& buffer) {
        if (buffer.size() < sizeof(uint8_t) + sizeof(ue_id_t) + 
            sizeof(frame_id_t) + sizeof(uint64_t)) {
            return false;
        }

        size_t offset = 0;

        // Read message type
        message_type = static_cast<RICMessageType>(buffer[offset++]);

        // Read common fields
        memcpy(&ue_id, buffer.data() + offset, sizeof(ue_id));
        offset += sizeof(ue_id);

        memcpy(&frame_id, buffer.data() + offset, sizeof(frame_id));
        offset += sizeof(frame_id);

        memcpy(&timestamp, buffer.data() + offset, sizeof(timestamp));
        offset += sizeof(timestamp);

        if (message_type == RICMessageType::LINK_ALLOCATION) {
            // Read link controls
            uint32_t num_links;
            memcpy(&num_links, buffer.data() + offset, sizeof(num_links));
            offset += sizeof(num_links);

            link_controls.clear();
            for (uint32_t i = 0; i < num_links; i++) {
                RICLinkControl link;
                memcpy(&link.link_id, buffer.data() + offset, sizeof(link.link_id));
                offset += sizeof(link.link_id);

                memcpy(&link.allocation_ratio, buffer.data() + offset, sizeof(link.allocation_ratio));
                offset += sizeof(link.allocation_ratio);

                link_controls.push_back(link);
            }
        }
        else if (message_type == RICMessageType::PDCP_PACKETS) {
            // Read PDCP packets
            uint32_t num_packets;
            memcpy(&num_packets, buffer.data() + offset, sizeof(num_packets));
            offset += sizeof(num_packets);

            pdcp_packets.clear();
            for (uint32_t i = 0; i < num_packets; i++) {
                RICPacketData packet;
                
                memcpy(&packet.pdcp_sn, buffer.data() + offset, sizeof(packet.pdcp_sn));
                offset += sizeof(packet.pdcp_sn);

                uint32_t data_size;
                memcpy(&data_size, buffer.data() + offset, sizeof(data_size));
                offset += sizeof(data_size);

                packet.data.resize(data_size);
                memcpy(packet.data.data(), buffer.data() + offset, data_size);
                offset += data_size;

                pdcp_packets.push_back(packet);
            }
        }

        return true;
    }

    std::string to_string() const {
        std::stringstream ss;
        ss << "RICControlMessage{"
           << "type=" << static_cast<int>(message_type)
           << ", ue=" << ue_id
           << ", frame=" << frame_id
           << ", timestamp=" << timestamp;

        if (message_type == RICMessageType::LINK_ALLOCATION) {
            ss << ", links=[";
            for (const auto& link : link_controls) {
                ss << "{id=" << link.link_id 
                   << ", ratio=" << link.allocation_ratio << "}, ";
            }
            ss << "]";
        }
        else if (message_type == RICMessageType::PDCP_PACKETS) {
            ss << ", pdcp_packets=" << pdcp_packets.size();
        }
        ss << "}";
        return ss.str();
    }

private:
    template<typename T>
    static void append_to_buffer(std::vector<uint8_t>& buffer, const T& value) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
    }
};

struct LinkDecision {
    std::map<link_id_t, double> link_splits;  // Maps link_id to its split ratio
    bool is_valid;
    
    // Updates link priorities based on split ratios
    void update_link_priorities(std::vector<LinkConfig>& configs) const {
        if (link_splits.empty()) return;

        // Create vector of pairs (link_id, split_ratio) for sorting
        std::vector<std::pair<link_id_t, double>> splits_vec(link_splits.begin(), link_splits.end());
        
        // Sort by split ratio in descending order
        std::sort(splits_vec.begin(), splits_vec.end(),
            [](const auto& a, const auto& b) {
                return a.second > b.second;  // Higher split = lower priority number
            });

        // Assign priorities (0 = highest priority)
        uint32_t priority = 0;
        for (const auto& [link_id, split] : splits_vec) {
            if (split > 0.0) {
                // Find and update the corresponding LinkConfig
                auto it = std::find_if(configs.begin(), configs.end(),
                    [link_id](const LinkConfig& config) {
                        return config.link_id == link_id;
                    });
                if (it != configs.end()) {
                    it->priority = priority++;
                }
            }
        }

        // Assign lower priorities to unused links
        for (auto& config : configs) {
            auto it = link_splits.find(config.link_id);
            if (it == link_splits.end() || it->second == 0.0) {
                config.priority = priority++;
            }
        }

        // Log priority assignments
        LOG_DEBUG(MODULE_NAME, "Updated link priorities based on splits:");
        for (const auto& config : configs) {
            auto it = link_splits.find(config.link_id);
            LOG_DEBUG(MODULE_NAME, "Link ", config.link_id, 
                    " (split: ", it != link_splits.end() ? it->second : 0.0, 
                    ") -> priority: ", config.priority);
        }
    }

    // Helper methods for convenience
    link_id_t get_primary_link() const {
        if (link_splits.empty()) return 0;
        
        return std::max_element(link_splits.begin(), link_splits.end(),
            [](const auto& a, const auto& b) {
                return a.second < b.second;
            })->first;
    }

    std::vector<link_id_t> get_active_links() const {
        std::vector<link_id_t> active_links;
        for (const auto& [link_id, split] : link_splits) {
            if (split > 0.0) {
                active_links.push_back(link_id);
            }
        }
        return active_links;
    }

    static LinkDecision create_multi_link(const std::map<link_id_t, double>& splits) {
        LinkDecision decision;
        decision.link_splits = splits;
        decision.is_valid = !splits.empty();
        return decision;
    }
};

} // namespace multiconn