#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt
import sys
import os
from datetime import datetime

def plot_results(csv_file):
    # Check if file exists
    if not os.path.exists(csv_file):
        print(f"Error: File {csv_file} not found")
        return False
    
    # Read the CSV file
    try:
        df = pd.read_csv(csv_file)
    except Exception as e:
        print(f"Error reading CSV file: {e}")
        return False
    
    # Convert timestamp to datetime
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    
    # Create a figure with two subplots
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 10))
    
    # Plot CPU usage
    ax1.plot(df['timestamp'], df['cpu_percent'], 'b-', label='CPU Usage')
    ax1.set_ylabel('CPU Usage (%)')
    ax1.set_title(f'Multi-User Test Results - {df.iloc[0]["num_users"]} Users')
    ax1.grid(True)
    
    # Get the mean and max CPU usage
    mean_cpu = df['cpu_percent'].mean()
    max_cpu = df['cpu_percent'].max()
    ax1.axhline(y=mean_cpu, color='r', linestyle='--', label=f'Mean: {mean_cpu:.2f}%')
    
    # Add text annotation for mean and max
    ax1.text(df['timestamp'].iloc[-1], mean_cpu, f'Mean: {mean_cpu:.2f}%', 
             verticalalignment='bottom', horizontalalignment='right')
    ax1.text(df['timestamp'].iloc[-1], max_cpu, f'Max: {max_cpu:.2f}%', 
             verticalalignment='bottom', horizontalalignment='right')
    
    # Plot memory usage
    ax2.plot(df['timestamp'], df['memory_kb'] / 1024, 'g-', label='Memory Usage')
    ax2.set_xlabel('Time')
    ax2.set_ylabel('Memory Usage (MB)')
    ax2.grid(True)
    
    # Get the mean and max memory usage
    mean_mem = df['memory_kb'].mean() / 1024
    max_mem = df['memory_kb'].max() / 1024
    ax2.axhline(y=mean_mem, color='r', linestyle='--', label=f'Mean: {mean_mem:.2f} MB')
    
    # Add text annotation for mean and max
    ax2.text(df['timestamp'].iloc[-1], mean_mem, f'Mean: {mean_mem:.2f} MB', 
             verticalalignment='bottom', horizontalalignment='right')
    ax2.text(df['timestamp'].iloc[-1], max_mem, f'Max: {max_mem:.2f} MB', 
             verticalalignment='bottom', horizontalalignment='right')
    
    # Adjust layout and save
    plt.tight_layout()
    
    # Generate output filename
    output_file = csv_file.replace('.csv', '.png')
    plt.savefig(output_file)
    print(f"Plot saved to {output_file}")
    
    # Generate summary information
    print("\nSummary:")
    print(f"Number of users: {df.iloc[0]['num_users']}")
    print(f"Test duration: {(df['timestamp'].iloc[-1] - df['timestamp'].iloc[0]).total_seconds():.2f} seconds")
    print(f"CPU usage: mean={mean_cpu:.2f}%, max={max_cpu:.2f}%")
    print(f"Memory usage: mean={mean_mem:.2f} MB, max={max_mem:.2f} MB")
    
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: plot_multi_user_results.py <csv_file>")
        return
    
    csv_file = sys.argv[1]
    plot_results(csv_file)

if __name__ == "__main__":
    main()