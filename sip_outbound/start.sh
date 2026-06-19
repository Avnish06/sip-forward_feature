#!/bin/sh

# Enable unlimited core dump size
ulimit -c unlimited

# Docker nofile ulimit sets the ceiling; this activates it for the process
ulimit -n 65535

# Attempt to set core dump file pattern
if [ -w /proc/sys/kernel/core_pattern ]; then
    echo "/cores/core.%e.%p.%t" > /proc/sys/kernel/core_pattern
    echo "[$(date)] Core pattern set to /cores/core.%e.%p.%t"
else
    echo "[$(date)] WARNING: Cannot set /proc/sys/kernel/core_pattern (read-only filesystem)"
    echo "[$(date)] Core dumps may not be generated unless container has SYS_ADMIN capability"
fi

mkdir -p /cores
chmod 777 /cores
cd /app/sip_outbound

rm -rf /cores/sip_outbound
cp /app/sip_outbound/sip_outbound /cores/

TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
echo "[$(date)] Starting sip_outbound..."

WEBHOOK_URL="https://n8n.subspace.money/webhook/c70ee37a-2922-474f-abb6-666beb5a2acf"
SERVER_ID="${SERVER_ID:-x}"

/app/sip_outbound/sip_outbound
EXIT_CODE=$?

echo "[$(date)] Process exited with code: $EXIT_CODE"

# Handle ANY exit (including 0)
if [ $EXIT_CODE -ne 0 ]; then
    echo "[$(date)] CRASH DETECTED! Exit code: $EXIT_CODE"
    
    # Wait for core dump to write
    sleep 2
    
    # Check for core dump and rename with timestamp
    LATEST_CORE=$(ls -t /cores/core.* 2>/dev/null | head -1)
    if [ -n "$LATEST_CORE" ]; then
        NEW_NAME="/cores/core_${TIMESTAMP}_exit${EXIT_CODE}"
        mv "$LATEST_CORE" "$NEW_NAME"
        echo "[$(date)] Core dump saved: $NEW_NAME"
    else
        echo "[$(date)] WARNING: No core file generated (exit code: $EXIT_CODE)"
        # Save crash info even without core dump
        echo "Exit Code: $EXIT_CODE at $(date)" >> /cores/crash_log_${TIMESTAMP}.txt
    fi
    
    # Categorize crash type
    case $EXIT_CODE in
        139)
            ERR_TYPE="SEGFAULT (SIGSEGV)"
            echo "[$(date)] Type: $ERR_TYPE"
            ;;
        134)
            ERR_TYPE="ABORT (SIGABRT) - likely assertion failure"
            echo "[$(date)] Type: $ERR_TYPE"
            ;;
        136)
            ERR_TYPE="FLOATING POINT EXCEPTION (SIGFPE)"
            echo "[$(date)] Type: $ERR_TYPE"
            ;;
        137)
            ERR_TYPE="KILLED (SIGKILL)"
            echo "[$(date)] Type: $ERR_TYPE"
            ;;
        132)
            ERR_TYPE="ILLEGAL INSTRUCTION (SIGILL)"
            echo "[$(date)] Type: $ERR_TYPE"
            ;;
        *)
            ERR_TYPE="OTHER"
            echo "[$(date)] Type: $ERR_TYPE"
            ;;
    esac
else
    # Exit code 0 - clean exit (but still unexpected in production)
    echo "[$(date)] CLEAN EXIT DETECTED (exit 0) - This shouldn't happen in production!"
    echo "Exit Code: 0 (clean exit) at $(date)" >> /cores/exit_log_${TIMESTAMP}.txt
fi

# --- Send webhook notification ---
echo "[$(date)] Sending crash notification to n8n webhook..."
curl -s -X POST "$WEBHOOK_URL" \
    -H "Content-Type: application/json" \
    -d "{
        \"server\": \"$SERVER_ID\",
        \"error\": \"$EXIT_CODE - $ERR_TYPE\",
        \"timestamp\": \"$TIMESTAMP\"
    }" \
    && echo "[$(date)] Webhook sent successfully" \
    || echo "[$(date)] WARNING: Failed to send webhook"


echo "[$(date)] Restarting container..."