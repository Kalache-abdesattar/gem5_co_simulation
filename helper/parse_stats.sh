#!/bin/bash

# Check if at least one file was provided
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <stats_file1> [stats_file2 ...]"
    exit 1
fi

# Arrays to hold global stats across all files
declare -a SIM_TICKS_ALL
declare -a SIM_INSTS_ALL
declare -a SIM_SECONDS_ALL

# Associative arrays for cache stats
# Key: Cache Name, Value: Space separated list of values
declare -A CACHE_ACCESS_LISTS
declare -A CACHE_HIT_LISTS
declare -a CACHE_NAMES # To maintain display order
declare -A SEEN_CACHE_NAMES # To ensure uniqueness

# Function to format array string "[v1, v2]"
format_output() {
    local items=("$@")
    local len=${#items[@]}
    local out="["
    for ((i=0; i<len; i++)); do
        out+="${items[i]}"
        if [ $i -lt $((len-1)) ]; then
            out+=", "
        fi
    done
    out+="]"
    echo "$out"
}

# Variable to track the number of files processed
file_count=0

# Iterate over each provided stats file
for STATS_FILE in "$@"; do

    # Check if the file exists
    if [ ! -f "$STATS_FILE" ]; then
        echo "Error: File '$STATS_FILE' not found."
        # We add a placeholder "N/A" to maintain array alignment for this file index
        SIM_TICKS_ALL+=("N/A")
        SIM_INSTS_ALL+=("N/A")
        SIM_SECONDS_ALL+=("N/A")
        ((file_count++))
        continue
    fi

    # 1. Parse Global Statistics
    ticks=$(grep "^simTicks" "$STATS_FILE" | head -n 1 | awk '{print $2}')
    insts=$(grep "^simInsts" "$STATS_FILE" | head -n 1 | awk '{print $2}')
    seconds=$(grep "^simSeconds" "$STATS_FILE" | head -n 1 | awk '{print $2}')

    # Default to 0 if missing in file
    SIM_TICKS_ALL+=("${ticks:-0}")
    SIM_INSTS_ALL+=("${insts:-0}")
    SIM_SECONDS_ALL+=("${seconds:-0}")

    # 2. Parse Cache Statistics
    # Use process substitution to avoid subshell so arrays persist
    while read -r line; do
        
        # Extract variable name and value
        full_access_name=$(echo "$line" | awk '{print $1}')
        access_count=$(echo "$line" | awk '{print $2}')

        # Create a readable name 
        # Escape dots (\.) to match literal dots, ensuring we don't accidentally strip 'dcache' or 'icache'
        display_name=$(echo "$full_access_name" | sed 's/board\.cache_hierarchy\.//' | sed 's/\.m_demand_accesses//' | sed 's/\.cache//')

        # Add to ordered list if seen for the first time
        if [ -z "${SEEN_CACHE_NAMES[$display_name]}" ]; then
            CACHE_NAMES+=("$display_name")
            SEEN_CACHE_NAMES[$display_name]=1
        fi

        # Append access count to the specific cache's list
        if [ -z "${CACHE_ACCESS_LISTS[$display_name]}" ]; then
            CACHE_ACCESS_LISTS[$display_name]="$access_count"
        else
            CACHE_ACCESS_LISTS[$display_name]="${CACHE_ACCESS_LISTS[$display_name]} $access_count"
        fi
        
        # Extract corresponding hit count
        full_hit_name="${full_access_name/m_demand_accesses/m_demand_hits}"
        hit_count=$(grep "^$full_hit_name" "$STATS_FILE" | awk '{print $2}')
        if [ -z "$hit_count" ]; then hit_count=0; fi
        
        # Append hit count to the specific cache's list
        if [ -z "${CACHE_HIT_LISTS[$display_name]}" ]; then
            CACHE_HIT_LISTS[$display_name]="$hit_count"
        else
            CACHE_HIT_LISTS[$display_name]="${CACHE_HIT_LISTS[$display_name]} $hit_count"
        fi

    done < <(grep "\.m_demand_accesses" "$STATS_FILE")

    ((file_count++))
done

echo "========================================================"
echo "Gem5 Simulation Summary (Combined)"
echo "Processed ${file_count} files: $@"
echo "========================================================"

# Print Global Stats
out_ticks=$(format_output "${SIM_TICKS_ALL[@]}")
out_insts=$(format_output "${SIM_INSTS_ALL[@]}")
out_seconds=$(format_output "${SIM_SECONDS_ALL[@]}")

printf "%-30s : %s\n" "Simulation Ticks" "$out_ticks"
printf "%-30s : %s\n" "Simulated Instructions" "$out_insts"
printf "%-30s : %s\n" "Simulated Seconds" "$out_seconds"
echo ""

# Helper function to print a specific category of caches
print_cache_category() {
    local category_name=$1
    local filter_keyword=$2  # e.g., "dcache" or "icache"
    local exclude_mode=$3    # "true" to print items NOT matching dcache/icache

    echo "========================================================"
    echo "$category_name Performance (Array-wise)"
    echo "========================================================"
    printf "%-45s | %-25s | %-25s | %-25s\n" "Cache Component" "Demand Accesses" "Demand Hits" "Hit Rate"
    printf "%s\n" "-------------------------------------------------------------------------------------------------------------------------------"

    local found_any=0

    for name in "${CACHE_NAMES[@]}"; do
        local print_row=0

        if [ "$exclude_mode" == "true" ]; then
            # Print if it matches NEITHER dcache nor icache
            if [[ "$name" != *"dcache"* && "$name" != *"icache"* ]]; then
                print_row=1
            fi
        else
            # Print if it matches the filter
            if [[ "$name" == *"$filter_keyword"* ]]; then
                print_row=1
            fi
        fi

        if [ "$print_row" -eq 1 ]; then
            found_any=1
            # Retrieve the space-separated strings
            acc_str="${CACHE_ACCESS_LISTS[$name]}"
            hit_str="${CACHE_HIT_LISTS[$name]}"
            
            # Convert strings back to arrays
            IFS=' ' read -r -a acc_arr <<< "$acc_str"
            IFS=' ' read -r -a hit_arr <<< "$hit_str"
            
            # Calculate Hit Rates
            declare -a rate_arr
            rate_arr=()
            
            for ((i=0; i<${#acc_arr[@]}; i++)); do
                a=${acc_arr[i]}
                h=${hit_arr[i]}
                
                if [ -z "$a" ]; then a=0; fi
                if [ -z "$h" ]; then h=0; fi
                
                if [ "$a" -gt 0 ]; then
                     r=$(awk "BEGIN {printf \"%.2f\", ($h/$a)*100}")
                else
                     r="0.00"
                fi
                rate_arr+=("$r%")
            done
            
            # Format columns
            f_acc=$(format_output "${acc_arr[@]}")
            f_hit=$(format_output "${hit_arr[@]}")
            f_rate=$(format_output "${rate_arr[@]}")
            
            # Print row
            printf "%-45s | %-25s | %-25s | %-25s\n" "$name" "$f_acc" "$f_hit" "$f_rate"
        fi
    done

    if [ "$found_any" -eq 0 ]; then
        echo "No caches found for this category."
    fi
    echo ""
}

# Print separated tables
print_cache_category "DCache" "dcache" "false"
print_cache_category "ICache" "icache" "false"
print_cache_category "Other Cache" "" "true"

echo "Done."