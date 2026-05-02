#!/bin/bash

# Terminal colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Default log directory from config
LOG_DIR="/tmp/pdcp_logs"
if [ "$1" != "" ]; then
    LOG_DIR="$1"
fi

# Check if log directory exists
if [ ! -d "$LOG_DIR" ]; then
    echo -e "${RED}Error: Log directory $LOG_DIR does not exist${NC}"
    echo -e "${YELLOW}Usage: $0 [log_directory]${NC}"
    exit 1
fi

# Function to display components and let user choose
choose_component() {
    echo -e "${BLUE}Available components:${NC}"
    echo "0. All components (combined log)"
    echo "1. PDCP Processor (emulator)"
    echo "2. PDCP Sender"
    echo "3. PDCP Receiver"
    echo "4. All DU Links"
    echo "5. All UE Links"
    echo "6. Choose specific DU/UE Link"
    echo -e "${YELLOW}Enter component number:${NC} "
    read -r choice
    
    case $choice in
        0)
            monitor_all
            ;;
        1)
            monitor_component "emulator"
            ;;
        2)
            monitor_component "pdcp_sender"
            ;;
        3)
            monitor_component "pdcp_receiver"
            ;;
        4)
            monitor_du_links
            ;;
        5)
            monitor_ue_links
            ;;
        6)
            choose_specific_link
            ;;
        *)
            echo -e "${RED}Invalid choice${NC}"
            choose_component
            ;;
    esac
}

# Monitor all logs combined
monitor_all() {
    echo -e "${GREEN}Monitoring all logs. Press Ctrl+C to exit.${NC}"
    tail -f "$LOG_DIR"/*.log
}

# Monitor specific component
monitor_component() {
    component=$1
    echo -e "${GREEN}Monitoring $component logs. Press Ctrl+C to exit.${NC}"
    
    # Find logs for this component
    logs=$(find "$LOG_DIR" -name "${component}*.log")
    
    if [ -z "$logs" ]; then
        echo -e "${RED}No logs found for $component${NC}"
        choose_component
        return
    fi
    
    tail -f $logs
}

# Monitor all DU links
monitor_du_links() {
    echo -e "${GREEN}Monitoring all DU link logs. Press Ctrl+C to exit.${NC}"
    logs=$(find "$LOG_DIR" -name "pdcp_du_link*.log")
    
    if [ -z "$logs" ]; then
        echo -e "${RED}No DU link logs found${NC}"
        choose_component
        return
    fi
    
    tail -f $logs
}

# Monitor all UE links
monitor_ue_links() {
    echo -e "${GREEN}Monitoring all UE link logs. Press Ctrl+C to exit.${NC}"
    logs=$(find "$LOG_DIR" -name "pdcp_ue_link*.log")
    
    if [ -z "$logs" ]; then
        echo -e "${RED}No UE link logs found${NC}"
        choose_component
        return
    fi
    
    tail -f $logs
}

# Choose specific DU/UE link
choose_specific_link() {
    echo -e "${BLUE}Available links:${NC}"
    
    # Get all DU links
    du_links=$(find "$LOG_DIR" -name "pdcp_du_link*.log" | sort)
    du_count=$(echo "$du_links" | wc -l)
    
    # Get all UE links
    ue_links=$(find "$LOG_DIR" -name "pdcp_ue_link*.log" | sort)
    ue_count=$(echo "$ue_links" | wc -l)
    
    # Display DU links
    i=1
    if [ -n "$du_links" ]; then
        echo -e "${CYAN}DU Links:${NC}"
        for log in $du_links; do
            link_id=$(echo "$log" | grep -o '_[0-9]*' | grep -o '[0-9]*')
            echo "$i. DU Link $link_id"
            ((i++))
        done
    fi
    
    # Display UE links
    if [ -n "$ue_links" ]; then
        echo -e "${CYAN}UE Links:${NC}"
        for log in $ue_links; do
            link_id=$(echo "$log" | grep -o '_[0-9]*' | grep -o '[0-9]*')
            echo "$i. UE Link $link_id"
            ((i++))
        done
    fi
    
    # No links found
    if [ "$i" -eq 1 ]; then
        echo -e "${RED}No link logs found${NC}"
        choose_component
        return
    fi
    
    # Get user choice
    echo -e "${YELLOW}Enter link number:${NC} "
    read -r link_choice
    
    # Validate choice
    if ! [[ "$link_choice" =~ ^[0-9]+$ ]] || [ "$link_choice" -lt 1 ] || [ "$link_choice" -gt "$((du_count + ue_count))" ]; then
        echo -e "${RED}Invalid choice${NC}"
        choose_specific_link
        return
    fi
    
    # Select the appropriate log
    if [ "$link_choice" -le "$du_count" ]; then
        # DU link
        selected_log=$(echo "$du_links" | sed -n "${link_choice}p")
        link_id=$(echo "$selected_log" | grep -o '_[0-9]*' | grep -o '[0-9]*')
        echo -e "${GREEN}Monitoring DU Link $link_id. Press Ctrl+C to exit.${NC}"
    else
        # UE link
        adjusted_choice=$((link_choice - du_count))
        selected_log=$(echo "$ue_links" | sed -n "${adjusted_choice}p")
        link_id=$(echo "$selected_log" | grep -o '_[0-9]*' | grep -o '[0-9]*')
        echo -e "${GREEN}Monitoring UE Link $link_id. Press Ctrl+C to exit.${NC}"
    fi
    
    tail -f "$selected_log"
}

# Main execution
echo -e "${BLUE}==== PDCP Multi-Connectivity Emulator Log Monitor ====${NC}"
echo -e "${YELLOW}Log directory: $LOG_DIR${NC}"

choose_component