package services

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math"
	"strings"
	"sync"

	"app/db"
	"app/models"
	"app/utils"

	"github.com/gin-gonic/gin"
)

/*
  In-memory state — single RWMutex guards all three structures.
  Routing takes RLock (readers never block each other).
  Writes (register, unregister, toggle, heartbeat) take full Lock.

  LSML  — LocalServerMetadataList: map[serverName]*ServerMetadata  (full metadata + health)
  LSCS  — LocalServerCallCountSet: map[serverName]int              (current call count; math.MaxInt = unhealthy)
  IOSL  — InboundServers / OutboundServers: []string               (names of enabled servers)
*/

var (
	LSML = map[string]*models.ServerMetadata{}
	LSCS = map[string]int{}

	InboundServers  []string
	OutboundServers []string

	stateMu sync.RWMutex
)

// InitServerArrays loads registrations from Redis and populates LSML, LSCS, IOSL.
// Called once at startup. Health data starts as "unknown" — first heartbeat will populate it.
func InitServerArrays() {
	ctx := context.Background()
	iter := db.RClient.Scan(ctx, 0, "container:*", 0).Iterator()

	stateMu.Lock()
	defer stateMu.Unlock()

	LSML = map[string]*models.ServerMetadata{}
	LSCS = map[string]int{}
	InboundServers = []string{}
	OutboundServers = []string{}

	for iter.Next(ctx) {
		key := iter.Val()
		val, err := db.RClient.Get(ctx, key).Result()
		if err != nil {
			continue
		}

		var reg models.ContainerRegistration
		if err := json.Unmarshal([]byte(val), &reg); err != nil {
			continue
		}

		name := reg.Name

		// Populate LSML with registration data + default health
		LSML[name] = &models.ServerMetadata{
			Name:            reg.Name,
			Host:            reg.Host,
			Port:            reg.Port,
			Aor:             reg.Aor,
			ContactURI:      reg.ContactURI,
			Health:          utils.GetDefaultHealth(),
			InboundEnabled:  reg.InboundEnabled,
			OutboundEnabled: reg.OutboundEnabled,
		}

		// LSCS: unknown health at startup = INF (unhealthy until first heartbeat proves otherwise)
		LSCS[name] = math.MaxInt

		// IOSL
		if reg.InboundEnabled {
			InboundServers = append(InboundServers, name)
		}
		if reg.OutboundEnabled {
			OutboundServers = append(OutboundServers, name)
		}
	}

	log.Printf("Initialized %d servers from Redis registrations", len(LSML))
}

// RegisterServer adds/updates a server in all in-memory structures.
// Called by RegisterContainerHandler after persisting to Redis.
func RegisterServer(reg models.ContainerRegistration) {
	stateMu.Lock()
	defer stateMu.Unlock()

	name := reg.Name

	if existing, ok := LSML[name]; ok {
		// Re-registration: update metadata but preserve live health data
		existing.Host = reg.Host
		existing.Port = reg.Port
		existing.Aor = reg.Aor
		existing.ContactURI = reg.ContactURI
		existing.InboundEnabled = reg.InboundEnabled
		existing.OutboundEnabled = reg.OutboundEnabled
		// Health and LSCS stay as-is (live data from heartbeat)
	} else {
		// New server: default health, INF call count until first heartbeat
		LSML[name] = &models.ServerMetadata{
			Name:            reg.Name,
			Host:            reg.Host,
			Port:            reg.Port,
			Aor:             reg.Aor,
			ContactURI:      reg.ContactURI,
			Health:          utils.GetDefaultHealth(),
			InboundEnabled:  reg.InboundEnabled,
			OutboundEnabled: reg.OutboundEnabled,
		}
		LSCS[name] = math.MaxInt
	}

	// Update IOSL
	updateIOSL(name, reg.InboundEnabled, reg.OutboundEnabled)
}

// UnregisterServer removes a server from all in-memory structures.
func UnregisterServer(name string) {
	stateMu.Lock()
	defer stateMu.Unlock()

	delete(LSML, name)
	delete(LSCS, name)
	updateIOSL(name, false, false)
}

// ToggleServer updates inbound/outbound flags in LSML and IOSL.
func ToggleServer(name string, inbound *bool, outbound *bool) error {
	stateMu.Lock()
	defer stateMu.Unlock()

	meta, ok := LSML[name]
	if !ok {
		return errors.New("server not found in memory")
	}

	if inbound != nil {
		meta.InboundEnabled = *inbound
	}
	if outbound != nil {
		meta.OutboundEnabled = *outbound
	}

	updateIOSL(name, meta.InboundEnabled, meta.OutboundEnabled)
	return nil
}

// updateIOSL syncs the IOSL arrays for a given server. Must be called with stateMu held.
func updateIOSL(name string, inbound bool, outbound bool) {
	// Inbound
	inIdx := -1
	for i, n := range InboundServers {
		if n == name {
			inIdx = i
			break
		}
	}
	if inbound && inIdx == -1 {		// add if enabled and not present
		InboundServers = append(InboundServers, name)
	} else if !inbound && inIdx != -1 {		// remove if disabled and present
		InboundServers = append(InboundServers[:inIdx], InboundServers[inIdx+1:]...)
	}

	// Outbound
	outIdx := -1
	for i, n := range OutboundServers {
		if n == name {
			outIdx = i
			break
		}
	}
	if outbound && outIdx == -1 {	// add if enabled and not present
		OutboundServers = append(OutboundServers, name)
	} else if !outbound && outIdx != -1 {	// remove if disabled and present
		OutboundServers = append(OutboundServers[:outIdx], OutboundServers[outIdx+1:]...)
	}
}

// GetAllServerMetadata returns a snapshot of all servers' metadata. Used by API handlers.
func GetAllServerMetadata() []models.ServerMetadata {
	stateMu.RLock()
	defer stateMu.RUnlock()

	result := make([]models.ServerMetadata, 0, len(LSML))
	for _, meta := range LSML {
		copy := *meta
		// Overlay LSCS call count into the health snapshot (LSCS is the source of truth for routing)
		if count, ok := LSCS[meta.Name]; ok && count < math.MaxInt {
			copy.Health.CurrentCalls = count
		}
		result = append(result, copy)
	}
	return result
}

// GetServerMetadata returns a single server's metadata snapshot.
func GetServerMetadata(name string) (*models.ServerMetadata, bool) {
	stateMu.RLock()
	defer stateMu.RUnlock()

	meta, ok := LSML[name]
	if !ok {
		return nil, false
	}
	copy := *meta
	if count, exists := LSCS[name]; exists && count < math.MaxInt {
		copy.Health.CurrentCalls = count
	}
	return &copy, true
}

// FindLowestLoadContainer finds the best server for a call — fully in-memory, zero Redis.
// Iterates IOSL, reads LSCS for call counts, constructs response from LSML.
// Returns 503-worthy error if all servers are unhealthy (LSCS = INF).
func FindLowestLoadContainer(direction string) (*models.ServerMetadata, error) {
	stateMu.Lock()
	defer stateMu.Unlock()

	var targetArray []string
	if direction == "inbound" {
		targetArray = InboundServers
	} else {
		targetArray = OutboundServers
	}

	if len(targetArray) == 0 {
		return nil, errors.New("no available containers")
	}

	minName := ""
	minVal := math.MaxInt

	for _, name := range targetArray {
		count, exists := LSCS[name]
		if !exists {
			continue
		}
		if count < minVal {
			minVal = count
			minName = name
		}
	}

	// If minimum is INF, all servers are unhealthy
	if minName == "" || minVal == math.MaxInt {
		return nil, errors.New("no healthy containers available")
	}

	// Optimistic increment — counter goes up before the proxy call
	LSCS[minName]++

	// Build response from LSML
	meta := LSML[minName]
	if meta == nil {
		return nil, fmt.Errorf("server %s missing from LSML", minName)
	}

	result := *meta // copy
	result.Health.CurrentCalls = LSCS[minName]

	return &result, nil
}

// FindContainerByType finds a container whose name contains the given type string.
// Uses LSML instead of Redis scan — fully in-memory.
func FindContainerByType(containerType string) (*models.ServerMetadata, error) {
	stateMu.Lock()
	defer stateMu.Unlock()

	for name, meta := range LSML {
		if strings.Contains(name, containerType) {
			count, exists := LSCS[name]
			if !exists {
				continue
			}
			// Skip unhealthy
			if count == math.MaxInt {
				continue
			}

			LSCS[name]++
			result := *meta
			result.Health.CurrentCalls = LSCS[name]
			return &result, nil
		}
	}
	return nil, errors.New("no container found with type " + containerType)
}

// SendResponse normalizes all API responses
func SendResponse(c *gin.Context, status int, message string, data interface{}, err string) {
	if status >= 500 {
		log.Printf("[Error] %s: %s", message, err)
	}
	c.JSON(status, models.CreateAPIResponse(status, message, data, err))
}

// IsDockerInternalIP checks if the IP belongs to standard Docker subnets
func IsDockerInternalIP(host string) bool {
	return strings.HasPrefix(host, "172.") || strings.HasPrefix(host, "10.") || strings.HasPrefix(host, "192.168.")
}

// ParseContainerID extracts IP and port from container_id (name-ip-port)
func ParseContainerID(containerID string) (string, string, error) {
	parts := strings.Split(containerID, "-")
	length := len(parts)
	if length < 3 {
		return "", "", errors.New("invalid container_id format")
	}
	return parts[length-2], parts[length-1], nil
}
