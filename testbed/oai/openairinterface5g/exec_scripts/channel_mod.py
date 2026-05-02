import telnetlib
import time
import os

# Configuration
host = '0.0.0.0'
port = 9090
interval = 1  # Interval in seconds
read_interval = 0.1  # Time to wait for more data

# Function to read SNR values from file
def read_snr_values(user_num):
    filename = f"channel_trace/snr{user_num + 1}.csv"
    if os.path.exists(filename):
        with open(filename, 'r') as file:
            return [line.strip() for line in file]
    else:
        print('No ' + filename +' we set noise power dB as -5')
        return ['-2.5']

# Function to send command and read response
def send_command(tn, user_num, snr_value):
    command = f'channelmod modify {user_num + 1} noise_power_dB {snr_value}\n'
    tn.write(command.encode('ascii'))

    # Read server response
    time.sleep(read_interval)  # Wait for initial data to arrive
    response = b""
    while True:
        data = tn.read_very_eager()
        if not data:
            break
        response += data
        time.sleep(read_interval)  # Wait for more data

    print(response.decode('ascii'), end='')

# Get the number of users
num_users = int(input("Enter the number of users: "))

# Prepare SNR values for each user
snr_values = [read_snr_values(user_num) for user_num in range(num_users)]
snr_index = [0] * num_users  # Index to track current line in each file

# Establish Telnet Connection
try:
    tn = telnetlib.Telnet(host, port)

    # Send command for each user and read response
    while True:
        for user_num in range(num_users):
            # Get current SNR value
            current_snr = snr_values[user_num][snr_index[user_num]]
            send_command(tn, user_num, current_snr)

            # Move to next SNR value or loop back
            snr_index[user_num] = (snr_index[user_num] + 1) % len(snr_values[user_num])
        time.sleep(interval)

except Exception as e:
    print("An error occurred:", e)
finally:
    tn.close()