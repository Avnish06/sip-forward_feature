package models

// ContainerRegister represents the container registration request payload received from the SIP media servers
type ContainerRegisterRequest struct {
	Name string `json:"name"`
	Host string `json:"host"`
	Port int `json:"port"`
	Aor string `json:"aor"`
	ContactURI string `json:"contact_uri"`
}

// InitiateCallRequest represents the request payload for initiating a call from Telephony service
type InitiateCallRequest struct {
	PhoneNumber string `json:"phone_number"`
	WebsocketURL string `json:"websocket_url"`
	WebhookURL string `json:"webhook_url"`
	TrunkName string `json:"trunk_name"`
	SampleRate int `json:"sample_rate"`
	AmbientAudio string `json:"ambient_audio"`
	Type string `json:"type,omitempty"`
}

// EndCallRequest represents the request payload for ending a call from Telephony service
type EndCallRequest struct {
	CallID string `json:"call_id"`
	ContainerID string `json:"container_id"`
}

// ToggleContainerRequest represents the request to enable or disable answering routes
type ToggleContainerRequest struct {
	InboundEnabled *bool `json:"inbound_enabled,omitempty"`
	OutboundEnabled *bool `json:"outbound_enabled,omitempty"`
}