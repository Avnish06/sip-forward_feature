package db

import (
	"fmt"
	"context"
	"log"
	"time"

	"app/utils"

	"github.com/redis/go-redis/v9"
)

// gloval redis client instance
var RClient *redis.Client

func InitRedis(cfg *utils.AppConfig) error {
	addr := fmt.Sprintf("%s:%s", cfg.RedisHost, cfg.RedisPort)

	RClient = redis.NewClient(&redis.Options{
		Addr:     addr,
		Password: cfg.RedisPassword,
		DB:       cfg.RedisDB,

		// Redis is now pure persistence fore server registrations
		// Only hit at: startup (SCAN+GET), and register/unregister/toggle)
		// Routing and heartbeat are fully in-memory. A tiny pool is more than sufficient.
		PoolSize:     cfg.RedisConnectionPoolSize, // Max concurrent connections
		MinIdleConns: cfg.RedisMinIdleConns,       // Minimum idle connections
		DialTimeout:  5 * time.Second,			   // Connection timeout
		ReadTimeout:  3 * time.Second,			   // Read timeout
		WriteTimeout: 3 * time.Second,			   // Write timeout
	})

	// Test the connection
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	_, err := RClient.Ping(ctx).Result()
	if err != nil {
		return fmt.Errorf("failed to connect to redis: %v", err)
	}

	log.Println("✅ Connected to Redis successfully with connection pooling")
	return nil
}