#!/bin/bash

# Assuming the script is in a subfolder named '_scripts'
# Get the absolute path of the parent folder
parent_folder="$(dirname "$(pwd)")"

# List of ports for ESP32 devices
ESP32_PORTS=("COM1" "COM2" "COM3" "COM4" "COM5" "COM6" "COM7" "COM8")

# Function to flash and monitor a device on a specified port
function flash_and_monitor {
    local port=$1
    echo "Flashing and monitoring device on port $port"
    
    # Execute the command in a subshell to avoid changing the working directory of the main script
    (
        cd "$parent_folder" || exit 1  # Change to the parent folder
        idf.py -p $port flash monitor
    )
}

# Iterate over each port and call the function
for port in "${ESP32_PORTS[@]}"; do
    flash_and_monitor $port
done