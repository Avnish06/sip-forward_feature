package services

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strings"
	"time"

	"app/db"
	"app/models"
	"app/utils"

	"github.com/gin-gonic/gin"
)

var ctx = context.Background()

// proxyClient is shared across all proxy calls for connection reuse.
// Separate from httpClient in worker.go which has a tighter 5s timeout for health checks.
var proxyClient = &http.Client{
	Timeout: 30 * time.Second,
	Transport: &http.Transport{
		MaxIdleConns:        200,
		MaxIdleConnsPerHost: 50,
		IdleConnTimeout:     90 * time.Second,
	},
}

// ---------------- Handlers ----------------

// RegisterContainerHandler - POST /register-container
// Persists registration metadata to Redis, populates LSML/LSCS/IOSL in memory.
func RegisterContainerHandler(c *gin.Context) {
	var req models.ContainerRegisterRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		SendResponse(c, http.StatusBadRequest, "Invalid request payload", nil, err.Error())
		return
	}

	// Port correction for Docker internal IPs
	finalPort := req.Port
	if IsDockerInternalIP(req.Host) {
		finalPort = 8086
	}

	key := fmt.Sprintf("container:%s", req.Name)

	// Preserve existing inbound/outbound state from Redis if re-registering
	inboundEnabled := false
	outboundEnabled := false

	existingData, err := db.RClient.Get(ctx, key).Result()
	if err == nil && existingData != "" {
		var existing models.ContainerRegistration
		if json.Unmarshal([]byte(existingData), &existing) == nil {
			inboundEnabled = existing.InboundEnabled
			outboundEnabled = existing.OutboundEnabled
		}
	}

	// Build registration (Redis-only schema — no health)
	reg := models.ContainerRegistration{
		Name:            req.Name,
		Host:            req.Host,
		Port:            finalPort,
		Aor:             req.Aor,
		ContactURI:      req.ContactURI,
		InboundEnabled:  inboundEnabled,
		OutboundEnabled: outboundEnabled,
	}

	dataBytes, err := json.Marshal(reg)
	if err != nil {
		SendResponse(c, http.StatusInternalServerError, "Failed to encode registration data", nil, err.Error())
		return
	}

	// Persist to Redis
	if err := db.RClient.Set(ctx, key, dataBytes, 0).Err(); err != nil {
		SendResponse(c, http.StatusServiceUnavailable, "Failed to save to Redis", nil, err.Error())
		return
	}

	// Update in-memory state
	RegisterServer(reg)

	SendResponse(c, http.StatusOK, fmt.Sprintf("Container %s registered successfully", req.Name), nil, "")
}

// UnregisterContainerHandler - DELETE /unregister-container/:container_name
func UnregisterContainerHandler(c *gin.Context) {
	name := c.Param("container_name")
	if name == "" {
		SendResponse(c, http.StatusBadRequest, "Container name is required", nil, "")
		return
	}

	key := fmt.Sprintf("container:%s", name)
	deleted, err := db.RClient.Del(ctx, key).Result()
	if err != nil {
		SendResponse(c, http.StatusServiceUnavailable, "Failed to delete from Redis", nil, err.Error())
		return
	}

	if deleted == 0 {
		SendResponse(c, http.StatusNotFound, "Container not found", nil, "")
		return
	}

	// Remove from all in-memory structures
	UnregisterServer(name)

	SendResponse(c, http.StatusOK, fmt.Sprintf("Container %s unregistered successfully", name), nil, "")
}

// ToggleContainerHandler - PUT /toggle-container/:container_name
func ToggleContainerHandler(c *gin.Context) {
	name := c.Param("container_name")
	if name == "" {
		SendResponse(c, http.StatusBadRequest, "Container name is required", nil, "")
		return
	}

	var req models.ToggleContainerRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		SendResponse(c, http.StatusBadRequest, "Invalid request payload", nil, err.Error())
		return
	}

	key := fmt.Sprintf("container:%s", name)

	// Read current registration from Redis
	val, err := db.RClient.Get(ctx, key).Result()
	if err != nil {
		SendResponse(c, http.StatusNotFound, "Container not found", nil, err.Error())
		return
	}

	var reg models.ContainerRegistration
	if err := json.Unmarshal([]byte(val), &reg); err != nil {
		SendResponse(c, http.StatusInternalServerError, "Failed to decode registration data", nil, err.Error())
		return
	}

	// Apply toggles
	if req.InboundEnabled != nil {
		reg.InboundEnabled = *req.InboundEnabled
	}
	if req.OutboundEnabled != nil {
		reg.OutboundEnabled = *req.OutboundEnabled
	}

	// Persist updated registration to Redis
	dataBytes, _ := json.Marshal(reg)
	db.RClient.Set(ctx, key, dataBytes, 0)

	// Update in-memory state
	ToggleServer(name, req.InboundEnabled, req.OutboundEnabled)

	// Return full metadata (from LSML) for API response
	meta, ok := GetServerMetadata(name)
	if !ok {
		SendResponse(c, http.StatusOK, "Container toggled successfully", reg, "")
		return
	}
	SendResponse(c, http.StatusOK, "Container toggled successfully", meta, "")
}

// ListContainersHandler - GET /list-containers
// Reads entirely from LSML — zero Redis calls.
func ListContainersHandler(c *gin.Context) {
	containers := GetAllServerMetadata()

	if len(containers) == 0 {
		SendResponse(c, http.StatusOK, "No containers found", []models.ServerMetadata{}, "")
		return
	}

	SendResponse(c, http.StatusOK, "Successfully retrieved containers", containers, "")
}

// HealthHandler - GET /health
// Reads entirely from LSML — zero Redis calls.
func HealthHandler(c *gin.Context) {
	containers := GetAllServerMetadata()

	if len(containers) == 0 {
		SendResponse(c, http.StatusOK, "No containers found", gin.H{}, "")
		return
	}

	healthMap := make(map[string]models.ServerMetadata, len(containers))
	for _, container := range containers {
		healthMap[container.Name] = container
	}

	SendResponse(c, http.StatusOK, "Successfully retrieved health status", healthMap, "")
}

// InitiateCallHandler - POST /initiate-call
func InitiateCallHandler(c *gin.Context) {
	// if c.GetHeader("Authorization") != "ch@ch@$h@d!k@rl0bh00kl@g!h@!" {
	// 	log.Printf("Unauthorized access attempt with Authorization header: %s", c.GetHeader("Authorization"))
	// 	SendResponse(c, http.StatusUnauthorized, "Unauthorized", nil, "invalid or missing Authorization header")
	// 	return
	// }

	var req models.InitiateCallRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		SendResponse(c, http.StatusBadRequest, "Invalid request payload", nil, err.Error())
		return
	}

	// Find best container — fully in-memory
	var bestContainer *models.ServerMetadata
	var err error
	if req.TrunkName == "bsnl_bcp" {
		bestContainer, err = FindContainerByType("india_media3")
		log.Printf("Redirecting call for trunk '%s' to container 'india_media3'", req.TrunkName)
	} else if req.Type != "" {
		bestContainer, err = FindContainerByType(req.Type)
		log.Printf("Redirecting call to container by type '%s'", req.Type)
	} else {
		bestContainer, err = FindLowestLoadContainer("outbound")
	}
	if err != nil {
		SendResponse(c, http.StatusServiceUnavailable, "No available containers", nil, err.Error())
		return
	}

	log.Printf("Routing call to container %s with %d calls", bestContainer.Name, bestContainer.Health.CurrentCalls)

	// Proxy request to container
	targetURL := fmt.Sprintf("http://%s:%d/initiate-call", bestContainer.Host, bestContainer.Port)
	proxyBody, _ := json.Marshal(req)

	resp, err := proxyClient.Post(targetURL, "application/json", bytes.NewBuffer(proxyBody))
	if err != nil {
		log.Printf("Failed to connect to container %s: %v", bestContainer.Name, err)
		SendResponse(c, http.StatusServiceUnavailable, "Failed to connect to container", nil, err.Error())
		return
	}
	defer resp.Body.Close()

	body, _ := io.ReadAll(resp.Body)

	if resp.StatusCode >= 400 {
		var errResp map[string]interface{}
		if err := json.Unmarshal(body, &errResp); err == nil {
			SendResponse(c, resp.StatusCode, "Upstream Error", errResp, "Container returned error")
		} else {
			SendResponse(c, resp.StatusCode, "Upstream Error", nil, string(body))
		}
	} else {
		var successResp map[string]interface{}
		if err := json.Unmarshal(body, &successResp); err == nil {
			SendResponse(c, resp.StatusCode, "Call initiated", successResp, "")
		} else {
			c.Data(resp.StatusCode, "application/json", body)
		}
	}
}

// EndCallHandler - POST /end-call
func EndCallHandler(c *gin.Context) {
	var req models.EndCallRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		SendResponse(c, http.StatusBadRequest, "Invalid request payload", nil, err.Error())
		return
	}

	ip, portStr, err := ParseContainerID(req.ContainerID)
	if err != nil {
		SendResponse(c, http.StatusBadRequest, "Invalid container ID", nil, err.Error())
		return
	}

	targetURL := fmt.Sprintf("http://%s:%s/end-call/%s", ip, portStr, req.CallID)

	reqProxy, _ := http.NewRequest("POST", targetURL, nil)
	resp, err := proxyClient.Do(reqProxy)
	if err != nil {
		SendResponse(c, http.StatusServiceUnavailable, "Failed to connect to container", nil, err.Error())
		return
	}
	defer resp.Body.Close()

	body, _ := io.ReadAll(resp.Body)

	if resp.StatusCode == 200 {
		var successResp map[string]interface{}
		if err := json.Unmarshal(body, &successResp); err == nil {
			SendResponse(c, http.StatusOK, "Call end initiated", successResp, "")
		} else {
			SendResponse(c, http.StatusOK, "Call end initiated", gin.H{"status": "success"}, "")
		}
	} else {
		SendResponse(c, resp.StatusCode, "Upstream error", nil, string(body))
	}
}

// GetInboundContactHandler - GET /get-inbound-contact/:asterisk
func GetInboundContactHandler(c *gin.Context) {
	best, err := FindLowestLoadContainer("inbound")
	if err != nil {
		SendResponse(c, http.StatusServiceUnavailable, "No available inbound containers", nil, err.Error())
		return
	}

	if c.Param("asterisk") == "asterisk" {
		c.String(200, "PJSIP/%s/%s", best.Aor, best.ContactURI)
		return
	}

	SendResponse(c, http.StatusOK, "Inbound contact retrieved", best, "")
}

// runAsteriskCLIViaAMI connects to the Asterisk Manager Interface and executes a CLI command.
// Both loadbalancer and asterisk containers run on host network, so we reach AMI at 127.0.0.1:5038.
func runAsteriskCLIViaAMI(command string) (string, error) {
	host := utils.GetEnv("AMI_HOST", "127.0.0.1")
	port := utils.GetEnv("AMI_PORT", "5038")
	user := utils.GetEnv("AMI_USER", "admin")
	secret := utils.GetEnv("AMI_SECRET", "admin")

	conn, err := net.DialTimeout("tcp", net.JoinHostPort(host, port), 5*time.Second)
	if err != nil {
		return "", fmt.Errorf("AMI dial failed: %w", err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(10 * time.Second))

	reader := bufio.NewReader(conn)

	// Banner line e.g. "Asterisk Call Manager/X.Y.Z"
	if _, err := reader.ReadString('\n'); err != nil {
		return "", fmt.Errorf("AMI banner read failed: %w", err)
	}

	// Login
	loginReq := fmt.Sprintf("Action: Login\r\nUsername: %s\r\nSecret: %s\r\n\r\n", user, secret)
	if _, err := conn.Write([]byte(loginReq)); err != nil {
		return "", fmt.Errorf("AMI login write failed: %w", err)
	}

	var loginResp strings.Builder
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return "", fmt.Errorf("AMI login read failed: %w", err)
		}
		if line == "\r\n" || line == "\n" {
			break
		}
		loginResp.WriteString(line)
	}
	if !strings.Contains(loginResp.String(), "Response: Success") {
		return "", fmt.Errorf("AMI login rejected: %s", strings.TrimSpace(loginResp.String()))
	}

	// Command
	cmdReq := fmt.Sprintf("Action: Command\r\nCommand: %s\r\n\r\n", command)
	if _, err := conn.Write([]byte(cmdReq)); err != nil {
		return "", fmt.Errorf("AMI command write failed: %w", err)
	}

	// Response format:
	//   Response: Follows
	//   Privilege: Command
	//   <output lines ...>
	//   --END COMMAND--
	//   <blank>
	var output strings.Builder
	bodyStarted := false
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			break
		}
		if strings.HasPrefix(line, "--END COMMAND--") {
			break
		}
		if !bodyStarted {
			trimmed := strings.TrimRight(line, "\r\n")
			if strings.HasPrefix(trimmed, "Response:") ||
				strings.HasPrefix(trimmed, "Privilege:") ||
				strings.HasPrefix(trimmed, "ActionID:") ||
				trimmed == "" {
				continue
			}
			bodyStarted = true
		}
		output.WriteString(line)
	}

	// Best-effort logoff
	conn.Write([]byte("Action: Logoff\r\n\r\n"))

	return output.String(), nil
}

// ListPjsipEndpointsHandler - GET /pjsip/endpoints
// Equivalent to: asterisk -rx "pjsip list endpoints", executed via AMI.
func ListPjsipEndpointsHandler(c *gin.Context) {
	out, err := runAsteriskCLIViaAMI("pjsip list endpoints")
	if err != nil {
		SendResponse(c, http.StatusInternalServerError, "Failed to execute pjsip list endpoints", nil, err.Error())
		return
	}
	SendResponse(c, http.StatusOK, "pjsip endpoints", gin.H{"output": out}, "")
}

// ShowPjsipEndpointHandler - GET /pjsip/endpoint/:name
// Equivalent to: asterisk -rx "pjsip show endpoints" | grep {name}, executed via AMI.
func ShowPjsipEndpointHandler(c *gin.Context) {
	name := c.Param("name")
	if name == "" {
		SendResponse(c, http.StatusBadRequest, "Endpoint name is required", nil, "")
		return
	}

	out, err := runAsteriskCLIViaAMI("pjsip show endpoints")
	if err != nil {
		SendResponse(c, http.StatusInternalServerError, "Failed to execute pjsip show endpoints", nil, err.Error())
		return
	}

	var matched []string
	for _, line := range strings.Split(out, "\n") {
		if strings.Contains(line, name) {
			matched = append(matched, line)
		}
	}

	if len(matched) == 0 {
		SendResponse(c, http.StatusNotFound, "Endpoint not found", gin.H{"endpoint": name, "output": ""}, "")
		return
	}

	SendResponse(c, http.StatusOK, "pjsip endpoint status", gin.H{
		"endpoint": name,
		"output":   strings.Join(matched, "\n"),
	}, "")
}

// GetAllCallsHandler - GET /get-all-calls
// Reads entirely from LSML — zero Redis calls.
func GetAllCallsHandler(c *gin.Context) {
	containers := GetAllServerMetadata()

	if len(containers) == 0 {
		SendResponse(c, http.StatusOK, "No active calls", []interface{}{}, "")
		return
	}

	allCalls := make([]map[string]interface{}, 0, len(containers))
	for _, container := range containers {
		allCalls = append(allCalls, map[string]interface{}{
			"container":    container.Name,
			"active_count": container.Health.CurrentCalls,
			"status":       container.Health.Status,
		})
	}

	SendResponse(c, http.StatusOK, "All active calls summary", allCalls, "")
}
