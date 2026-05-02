#!/usr/bin/env python3

import sys
import pandas as pd
import matplotlib.pyplot as plt

def analyze_out_of_order_pdcp(df):
    """
    Returns the number of out-of-order events in the 'pdcp_seq' column.
    We define out-of-order as any row where pdcp_seq < previous pdcp_seq.
    """
    out_of_order_count = 0
    last_seq = None
    
    for seq in df['pdcp_seq']:
        if last_seq is not None and seq < last_seq:
            out_of_order_count += 1
        last_seq = seq
    
    return out_of_order_count

def analyze_out_of_order_tcp(df):
    """
    Returns the number of out-of-order events in 'tcp_seq' 
    skipping 'N/A' or non-integer entries.
    Out-of-order is defined as a row whose tcp_seq is < last_tcp_seq.
    """
    out_of_order_count = 0
    last_tcp_seq = None
    
    for val in df['tcp_seq']:
        # Skip non-integer
        if pd.isna(val):
            continue
        
        if last_tcp_seq is not None and val < last_tcp_seq:
            out_of_order_count += 1
        last_tcp_seq = val

    return out_of_order_count

def analyze_out_of_order_tcp_with_len(df, tcp_seq_col='tcp_seq', data_len_col='data_len'):
    """
    Count out-of-order packets considering both the TCP sequence number 
    and the data length.  For each row:
      - start = tcp_seq
      - end   = tcp_seq + data_len
    A packet is considered out-of-order if start < highest_end_seq 
    from previously seen segments.
    """
    out_of_order_count = 0
    highest_end_seq = 0

    # We assume df is in the arrival order you care about. 
    # If needed, you can sort by arrival time: df.sort_values('pdcp_arrive_time', inplace=True)
    
    for _, row in df.iterrows():
        start = row[tcp_seq_col]
        length = row[data_len_col]
        # Skip if either is NaN
        if pd.isna(start) or pd.isna(length):
            continue
        
        end = start + length

        if start < highest_end_seq:
            out_of_order_count += 1
        
        if end > highest_end_seq:
            highest_end_seq = end

    return out_of_order_count

def main():
    if len(sys.argv) < 2:
        print("Usage: python pdcp_analysis.py <pdcp_receiver.csv>")
        sys.exit(1)
    
    csv_path = sys.argv[1]
    
    # Your CSV snippet has no header, so we must provide column names.
    cols = [
        'pdcp_seq',
        'pdcp_sent_time',
        'pdcp_arrive_time',
        'pdcp_depart_time',
        'tcp_seq',
        'data_len'
    ]
    
    # Read the CSV
    df = pd.read_csv(csv_path, header=None, names=cols)
    
    # Convert columns to numeric where appropriate
    df['pdcp_seq'] = pd.to_numeric(df['pdcp_seq'], errors='coerce')
    df['pdcp_sent_time'] = pd.to_numeric(df['pdcp_sent_time'], errors='coerce')
    df['pdcp_arrive_time'] = pd.to_numeric(df['pdcp_arrive_time'], errors='coerce')
    df['pdcp_depart_time'] = pd.to_numeric(df['pdcp_depart_time'], errors='coerce')
    df['tcp_seq'] = pd.to_numeric(df['tcp_seq'], errors='coerce')
    df['data_len'] = pd.to_numeric(df['data_len'], errors='coerce')
    
    # 1) Count out-of-order for PDCP
    pdcp_ooo = analyze_out_of_order_pdcp(df)
    
    # 2) Count out-of-order for TCP (purely by sequence number)
    tcp_ooo = analyze_out_of_order_tcp(df)
    
    # 3) Count out-of-order for TCP, considering sequence + data_len
    tcp_ooo_with_len = analyze_out_of_order_tcp_with_len(df, tcp_seq_col='tcp_seq', data_len_col='data_len')
    
    print(f"Number of PDCP out-of-order events: {pdcp_ooo}")
    print(f"Number of TCP out-of-order events (seq only): {tcp_ooo}")
    print(f"Number of TCP out-of-order events (seq + data_len): {tcp_ooo_with_len}")
    
    # 4) Compute delays
    df['e2e_delay'] = df['pdcp_depart_time'] - df['pdcp_sent_time']
    df['reorder_delay'] = df['pdcp_depart_time'] - df['pdcp_arrive_time']
    
    # 5) Plot E2E delay vs. pdcp_seq
    plt.figure()
    plt.plot(df['pdcp_seq'], df['e2e_delay'])
    plt.xlabel('PDCP Sequence')
    plt.ylabel('E2E Delay (ms)')
    plt.title('PDCP E2E Delay vs. PDCP Sequence')
    plt.show()
    
    # 6) Plot Reordering delay vs. pdcp_seq
    plt.figure()
    plt.plot(df['pdcp_seq'], df['reorder_delay'])
    plt.xlabel('PDCP Sequence')
    plt.ylabel('Reordering Delay (ms)')
    plt.title('PDCP Reordering Delay vs. PDCP Sequence')
    plt.show()

    # 7) Plot PDCP sequence (x-axis) vs. TCP sequence (y-axis)
    plt.figure()
    plt.plot(df['pdcp_seq'], df['tcp_seq'], marker='o')
    plt.xlabel('PDCP Sequence')
    plt.ylabel('TCP Sequence')
    plt.title('PDCP Sequence vs. TCP Sequence')
    plt.show()

if __name__ == "__main__":
    main()
