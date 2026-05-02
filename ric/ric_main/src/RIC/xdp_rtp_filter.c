/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>

/* Choose the RTP payload type(s) you're interested in. */
#define VIDEO_PAYLOAD_TYPE 106  // For example, 106 for VP8/VP9

/* For demonstration, we'll copy up to 2000 bytes of the packet */
#define MAX_IP_COPY 1500

/* The standard RTP header is at least 12 bytes; define a small struct. */
struct rtp_header {
    __u8  first_byte;       // version/padding/extension/cc
    __u8  second_byte;      // marker (bit 7) + payload type (bits 0..6)
    __u16 sequence_number;
    __u32 timestamp;
    __u32 ssrc;
};

/* This is the metadata we'll pass to user space. */
struct packet_info {
    __u32 timestamp;         // RTP timestamp (host order)
    __u16 sequence_number;   // RTP sequence
    __u8  marker;            // RTP marker bit
    __u8  payload_type;      // RTP payload type
    __u32 packet_size;       // the UDP payload size (excluding UDP header)
    __u32 src_ip;            // IP source
    __u16 src_port;          // UDP source port
    __u32 arrival_time_ms;   // BPF ktime in ms
    __u32 total_len;         // total bytes copied (L2 + L3 + L4 + partial payload)
};

/* We'll place packet_info + the raw packet bytes into the ring buffer. */
struct packet_full_info {
    struct packet_info info;
    __u8  data[MAX_IP_COPY]; // store the actual packet bytes
};

/* Create the ring buffer map. */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4 * 1024 * 1024);  // 4 MB
} packet_ringbuf SEC(".maps");

/* Safe copy that won't exceed data_end or MAX_IP_COPY. */
static __always_inline void safe_memcpy(void *dst, const void *src, __u32 len, const void *data_end)
{
    __u32 copy_len = (len < MAX_IP_COPY) ? len : MAX_IP_COPY;

#pragma clang loop unroll(disable)
    for (int i = 0; i < MAX_IP_COPY; i++) {
        if (i >= copy_len)
            break;
        if ((const char *)src + i + 1 > (const char *)data_end)
            break;
        ((char *)dst)[i] = ((const char *)src)[i];
    }
}

static __always_inline int process_rtp(void *data, void *data_end)
{
    /* 1) Basic length check */
    __u64 total_len = (__u64)data_end - (__u64)data;
    if (total_len < sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + sizeof(struct rtp_header))
        return XDP_PASS;


    bpf_printk("Received packet, length: %d", total_len);
    //bpf_printk("Protocol check: %d", iph->protocol);

    /* 2) Parse Ethernet */
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    /* 3) Parse IP */
    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;
    if (iph->protocol != IPPROTO_UDP)
        return XDP_PASS;

    /* 4) Parse UDP */
    struct udphdr *udph = (void *)(iph + 1);
    if ((void *)(udph + 1) > data_end)
        return XDP_PASS;
    __u16 udp_len = bpf_ntohs(udph->len);
    if (udp_len < sizeof(*udph) + sizeof(struct rtp_header))
        return XDP_PASS;

    /* 5) Parse RTP */
    struct rtp_header *rtp = (void *)(udph + 1);
    if ((void *)(rtp + 1) > data_end)
        return XDP_PASS;

    __u8 payload_type = rtp->second_byte & 0x7F; 
    __u8 marker       = (rtp->second_byte & 0x80) >> 7;
    __u16 seq         = bpf_ntohs(rtp->sequence_number);
    __u32 ts          = bpf_ntohl(rtp->timestamp);

    //if (payload_type != VIDEO_PAYLOAD_TYPE)
    //    return XDP_PASS;

    // RTP version should be 2 for valid RTP (upper 2 bits of first_byte)
    __u8 rtp_version = rtp->first_byte >> 6;
    if (rtp_version != 2)
        return XDP_PASS;  // not an RTP version=2 packet -> pass
        
    /* 6) Prepare ring buffer item */
    /* We'll copy up to MAX_IP_COPY from the start of the Ethernet header. */
    __u64 copy_len = total_len;
    if (copy_len > MAX_IP_COPY)
        copy_len = MAX_IP_COPY;

    /* Reserve space in ring buffer. */
    __u64 reserve_size = sizeof(struct packet_full_info);
    struct packet_full_info *pfi = bpf_ringbuf_reserve(&packet_ringbuf, reserve_size, 0);
    if (!pfi)
        return XDP_PASS;  // if no space, pass or drop

    /* Fill out the metadata. */
    pfi->info.timestamp       = ts;
    pfi->info.sequence_number = seq;
    pfi->info.marker          = marker;
    pfi->info.payload_type    = payload_type;
    /* The actual UDP payload (RTP payload) size: udph->len minus UDP+RTP headers. */
    pfi->info.packet_size = bpf_ntohs(udph->len) - sizeof(*udph) - sizeof(*rtp);
    pfi->info.src_ip      = iph->saddr;
    pfi->info.src_port    = udph->source;
    pfi->info.arrival_time_ms = (__u32)(bpf_ktime_get_ns() / 1000000ULL);
    pfi->info.total_len   = copy_len;


    bpf_printk("RTP Packet Details:\n");
    bpf_printk("  Payload Type: %u\n", payload_type);
    bpf_printk("  Seq num: %u\n", seq);
    bpf_printk("  Marker Bit: %u\n", marker);


    /* Copy the entire L2+L3+L4 portion. */
    safe_memcpy(pfi->data, data, ( __u32)copy_len, data_end);

    /* Submit to ring buffer. */
    bpf_ringbuf_submit(pfi, 0);

    /* 7) Pass original packet */
    return XDP_PASS;
}

SEC("xdp")
int xdp_rtp_filter(struct xdp_md *ctx)
{
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    return process_rtp(data, data_end);
}

char LICENSE[] SEC("license") = "GPL";