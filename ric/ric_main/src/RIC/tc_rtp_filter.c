#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>

#define TC_ACT_OK 0
#define TC_ACT_SHOT 2
#define VIDEO_PAYLOAD_TYPE 112 // VP8/VP9: 106, H264: 127

#define NOT_RTP  0
#define IS_RTP   1


struct rtp_header {
    __u8 first_byte;
    __u8 second_byte;
    __u16 sequence_number;
    __u32 timestamp;
    __u32 ssrc;
};

struct packet_info {
    __u32 timestamp;
    __u16 sequence_number;
    __u8  marker;
    __u8  payload_type;   /* 0xFF if not RTP            */
    __u8  is_rtp;         /* IS_RTP / NOT_RTP           */
    __u8  pad;            /* keep 4‑byte alignment      */
    __u32 packet_size;    /* UDP payload or total bytes */
    __u32 src_ip;
    __u16 src_port;
    __u16 _pad16;
    __u32 arrival_time_ms;
    __u32 total_len;
    __u32 dst_ip;         /* Destination IP address     */
    __u16 dst_port;       /* Destination port           */
    __u16 _pad16_2;       /* padding for alignment      */
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 8 * 1024 * 1024); // Doubled the buffer size
} packet_ringbuf SEC(".maps");

SEC("classifier")
int rtp_filter(struct __sk_buff *skb) {
    void *data = (void *)(long)skb->data;
    void *data_end = (void *)(long)skb->data_end;

    /* Basic length check */
    __u64 total_len = (__u64)data_end - (__u64)data;
    //bpf_printk("Received packet, length: %d", total_len);
    
    /* Make sure we have at least room for IP header */
    if (data + sizeof(struct iphdr) > data_end) {
        //bpf_printk("Packet too small for IP header");
        return TC_ACT_OK;
    }
    
    /* Parse IP header directly (no Ethernet) */
    struct iphdr *ip = data;
    
    /* Verify we can read IP protocol field */
    if ((void*)ip + 9 + 1 > data_end) {
        //bpf_printk("Cannot access IP protocol field");
        return TC_ACT_OK;
    }
    
    bpf_printk("IP protocol: %d", ip->protocol);
    
    if (ip->protocol != IPPROTO_UDP) {
        //bpf_printk("Not UDP");
        return TC_ACT_OK;
    }
    
    /* Verify we can access the entire IP header */
    if ((void*)(ip + 1) > data_end) {
        //bpf_printk("Cannot access full IP header");
        return TC_ACT_OK;
    }
    
    /* Continue with UDP header */
    struct udphdr *udp = (void*)(ip + 1);
    if ((void*)(udp + 1) > data_end) {
        //bpf_printk("Cannot access UDP header");
        return TC_ACT_OK;
    }
        
    struct rtp_header *rtp = (void*)(udp + 1);
    if ((void*)(rtp + 1) > data_end)
        return TC_ACT_OK;
        
    __u8 version = (rtp->first_byte & 0xC0) >> 6;
    __u8 payload_type = rtp->second_byte & 0x7F;
    __u8 marker = (rtp->second_byte & 0x80) >> 7;
    __u16 seq = bpf_ntohs(rtp->sequence_number);
    __u32 ts = bpf_ntohl(rtp->timestamp);

    __u32 payload_size = bpf_ntohs(udp->len) - sizeof(*udp) - sizeof(*rtp);

    /* Drop non-RTP UDP traffic. RTP requires version=2 and dynamic PT (96-127). */
    if (version != 2)
        return TC_ACT_OK;
    if (payload_type < 96 || payload_type > 127)
        return TC_ACT_OK;

    // Get current time in nanoseconds
    __u32 arrival_time = (__u32)(bpf_ktime_get_ns() / 1000000);
        
    struct packet_info *info;
    info = bpf_ringbuf_reserve(&packet_ringbuf, sizeof(*info), 0);
    if (!info)
        return TC_ACT_OK;
        
    info->timestamp = ts;
    info->sequence_number = seq;
    info->marker = marker;
    info->payload_type = payload_type;
    info->is_rtp = IS_RTP;  // This is an RTP packet
    info->packet_size = payload_size;
    
    // Store IP addresses and ports (in network byte order)
    info->src_ip = ip->saddr;  // Already in network byte order
    info->src_port = udp->source;  // Already in network byte order
    info->dst_ip = ip->daddr;  // Destination IP, already in network byte order
    info->dst_port = udp->dest;  // Destination port, already in network byte order

    // Store arrival time in milliseconds
    info->arrival_time_ms = arrival_time;
    info->total_len = total_len;
    
    bpf_ringbuf_submit(info, BPF_RB_FORCE_WAKEUP);

    //bpf_printk("RTP packet submitted to ring buffer");
    
    return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";