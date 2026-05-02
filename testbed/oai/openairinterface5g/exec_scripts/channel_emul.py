import numpy as np
import pandas as pd
# Re-initializing the list to store data, ensuring values are between 1 and 10
values = [5]

# Generating the data with values constrained between 1 and 10
for _ in range(1, 10000):
    last_value = values[-1]
    change = np.random.choice([-1, 0, 1])
    
    # If the value is 1 and change is -1, convert the change to +1
    if last_value == 5 and change == -1:
        change = 1
    # If the value is 10 and change is +1, convert the change to -1
    elif last_value == 10 and change == 1:
        change = -1
    
    new_value = last_value + change
    values.append(new_value)

# Creating a DataFrame
df = pd.DataFrame(values, columns=['Value'])

# Save the DataFrame to a CSV file
file_path_corrected = './channel_trace/traces.csv'
df.to_csv(file_path_corrected, index=False)