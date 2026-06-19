# One-click manual trigger:
#   places a call -> AI (stand-in) answers -> after 8s it transfers to the
#   human phone (09142436879) via the synchrovox trunk.
# Run from anywhere:  .\human_transfer_trigger\trigger-call.ps1
docker cp "$PSScriptRoot\test_call_body.json" media-server:/tmp/b.json
docker exec media-server curl -s -X POST http://localhost:8086/initiate-call -d "@/tmp/b.json"
Write-Host "`nCall placed. Your phone (9142436879) should ring in ~10 seconds." -ForegroundColor Green
