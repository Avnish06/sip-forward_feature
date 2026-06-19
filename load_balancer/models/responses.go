package models

import (
	"app/utils"
)

// ContainerRegistration is the Redis-only schema — pure registration metadata, no health/call data.
// Redis key: container:{name}
type ContainerRegistration struct {
	Name            string `json:"name"`
	Host            string `json:"host"`
	Port            int    `json:"port"`
	Aor             string `json:"aor"`
	ContactURI      string `json:"contact_uri"`
	InboundEnabled  bool   `json:"inbound_enabled"`
	OutboundEnabled bool   `json:"outbound_enabled"`
}

// LocalServerMetadataList is the in-memory representation (LSML value).
// Contains registration data + live health + call state. Never persisted to Redis.
type ServerMetadata struct {
	Name            string           `json:"name"`
	Host            string           `json:"host"`
	Port            int              `json:"port"`
	Aor             string           `json:"aor"`
	ContactURI      string           `json:"contact_uri"`
	Health          utils.HealthData `json:"health"`
	InboundEnabled  bool             `json:"inbound_enabled"`
	OutboundEnabled bool             `json:"outbound_enabled"`
}

// ContainerInboundResponse represents the response of inbound call endpoint to Asterisk inbound routing service
type ContainerInboundResponse struct {
	Name       string `json:"name"`
	Host       string `json:"host"`
	Port       int    `json:"port"`
	Aor        string `json:"aor"`
	ContactURI string `json:"contact_uri"`
	Contact    string `json:"contact"`
}

// generic response struct for API responses
type APIResponse struct {
	Status int `json:"status"`
	Message string `json:"message,omitempty"`
	Data interface{} `json:"data,omitempty"`
	Error string `json:"error,omitempty"`
}

// function to create a generic API response
func CreateAPIResponse(status int, message string, data interface{}, err string) APIResponse {
	return APIResponse{
		Status:  status,
		Message: message,
		Data:    data,
		Error:   err,
	}
}