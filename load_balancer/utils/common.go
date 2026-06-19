package utils

import (
	"os"
	"strconv"
	"time"
	"log"
	
	"errors"
)


/*
 ======================= <CONSTANTS and VARS> =======================
*/

var ErrNoHealthyContainers = errors.New("no healthy containers available")


/*
 ======================= <DATA STRUCTS> =======================
*/

// ------------ < Common data structures > ------------

// application configuration struct
type AppConfig struct {
	AppPort       string
	RedisHost     string
	RedisPort     string
	RedisPassword string
	RedisDB       int
	RedisConnectionPoolSize int
	RedisMinIdleConns int
	LogLevel      string
	HealthCheckTimeout int
}

// HealthData represents the health status of a container
type HealthData struct {
	Status string `json:"status"`
	AnsweredCalls int `json:"answered_calls"`
	CurrentCalls int `json:"current_calls"`
	MaxCalls int `json:"max_calls"`
	AvailableCapacity int `json:"available_capacity"`
	Timestamp int64 `json:"timestamp"`
	Error string `json:"error,omitempty"`
}


/*
 ======================= <FUNCTIONS> =======================
*/

// GetEnv retrieves the value of the environment variable named by the key
func GetEnv(key string, defaultValue string) string {
	value := os.Getenv(key)
	if value == "" {
		return defaultValue
	}
	return value
}

// LoadAppConfig creates a new AppConfig instance with the .env configuration values
func LoadAppConfig() *AppConfig {
	// Redis DB str -> int conversion with default value handling
	redisDB, err := strconv.Atoi(GetEnv("REDIS_DB", "0"))
	if err != nil {
		redisDB = 0
	}

	// Redis concurrent connections pool size str -> int conversion with default value handling
	redisConnectionPoolSize, err := strconv.Atoi(GetEnv("REDIS_CONNECTION_POOL_SIZE", "10"))
	if err != nil {
		redisConnectionPoolSize = 10
	}

	// Redis minimum idle connections str -> int conversion with default value handling
	redisMinIdleConns, err := strconv.Atoi(GetEnv("REDIS_MIN_IDLE_CONNS", "5"))
	if err != nil {
		redisMinIdleConns = 5
	}

	// Health check timeout str -> int conversion with default value handling
	healthCheckTimeout, err := strconv.Atoi(GetEnv("HEALTH_CHECK_TIMEOUT", "5"))
	if err != nil {
		healthCheckTimeout = 5
	}

	// Confirm the values
	appPort := GetEnv("APP_PORT", "8000")
	redisHost := GetEnv("REDIS_HOST", "localhost")
	redisPort := GetEnv("REDIS_PORT", "6379")
	redisPassword := GetEnv("REDIS_PASSWORD", "")
	logLevel := GetEnv("LOG_LEVEL", "info")

	// Log the loaded configuration (Optional, can be removed in production)
	log.Printf("Loaded configuration: AppPort=%s, RedisHost=%s, RedisPort=%s, RedisDB=%d, RedisConnectionPoolSize=%d, RedisMinIdleConns=%d, LogLevel=%s, HealthCheckTimeout=%d",
		appPort, redisHost, redisPort, redisDB, redisConnectionPoolSize, redisMinIdleConns, logLevel, healthCheckTimeout)

	// return the populated AppConfig struct
	return &AppConfig{
		AppPort:       appPort,
		RedisHost:     redisHost,
		RedisPort:     redisPort,
		RedisPassword: redisPassword,
		RedisDB:       redisDB,
		RedisConnectionPoolSize: redisConnectionPoolSize,
		RedisMinIdleConns: redisMinIdleConns,
		LogLevel:      logLevel,
		HealthCheckTimeout: healthCheckTimeout,
	}
}


// default health data 
func GetDefaultHealth() HealthData {
	return HealthData{
		Status:            "unknown",
		AnsweredCalls:     0,
		CurrentCalls:      0,
		MaxCalls:          0,
		AvailableCapacity: 0,
		Timestamp:         time.Now().Unix(),
	}
}