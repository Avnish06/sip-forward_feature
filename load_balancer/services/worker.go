package services

import (
	"encoding/json"
	"fmt"
	"log"
	"math"
	"net/http"
	"sync"
	"time"

	"app/utils"
)

// Dedicated HTTP client for health checks — tight 5s timeout, separate from proxy client.
var httpClient = &http.Client{
	Timeout: 5 * time.Second,
}

// StartHealthCheckLoop starts the background heartbeat worker.
func StartHealthCheckLoop(cfg *utils.AppConfig) {
	go func() {
		log.Println("Health check task started")
		for {
			performHealthChecks()
			time.Sleep(time.Duration(cfg.HealthCheckTimeout) * time.Second)
		}
	}()
}

// performHealthChecks iterates over all servers in LSML, pings them,
// and updates LSML (health) + LSCS (call count or INF if unhealthy).
// Zero Redis calls.
func performHealthChecks() {
	// Take a read lock to snapshot the servers we need to check.
	// We copy out host/port so we can release the lock before doing HTTP calls.
	stateMu.RLock()
	type checkTarget struct {
		name string
		host string
		port int
	}
	targets := make([]checkTarget, 0, len(LSML))
	for name, meta := range LSML {
		targets = append(targets, checkTarget{name: name, host: meta.Host, port: meta.Port})
	}
	stateMu.RUnlock()

	if len(targets) == 0 {
		return
	}

	// Results collected concurrently
	type healthResult struct {
		name   string
		health utils.HealthData
		err    error
	}

	results := make([]healthResult, len(targets))
	var wg sync.WaitGroup

	for i, t := range targets {
		wg.Add(1)
		go func(idx int, target checkTarget) {
			defer wg.Done()

			targetURL := fmt.Sprintf("http://%s:%d/health", target.host, target.port)
			resp, httpErr := httpClient.Get(targetURL)

			if httpErr != nil {
				log.Printf("Health check failed for %s: %v", target.name, httpErr)
				results[idx] = healthResult{
					name: target.name,
					err:  httpErr,
				}
				return
			}
			defer resp.Body.Close()

			if resp.StatusCode != 200 {
				results[idx] = healthResult{
					name: target.name,
					err:  fmt.Errorf("status %d", resp.StatusCode),
				}
				return
			}

			var health utils.HealthData
			if err := json.NewDecoder(resp.Body).Decode(&health); err != nil {
				results[idx] = healthResult{
					name: target.name,
					err:  fmt.Errorf("invalid json: %v", err),
				}
				return
			}

			results[idx] = healthResult{
				name:   target.name,
				health: health,
			}
		}(i, t)
	}

	wg.Wait()

	// Single write lock to apply all results at once — minimal lock hold time.
	stateMu.Lock()
	for _, r := range results {
		meta, ok := LSML[r.name]
		if !ok {
			continue // server was unregistered while we were checking
		}

		if r.err != nil {
			// Unhealthy: update LSML health, set LSCS to INF
			meta.Health = utils.HealthData{
				Status:            "unhealthy",
				AnsweredCalls:     0,
				CurrentCalls:      0,
				MaxCalls:          0,
				AvailableCapacity: 0,
				Timestamp:         time.Now().Unix(),
				Error:             r.err.Error(),
			}
			LSCS[r.name] = math.MaxInt
		} else {
			// Healthy: sync real values from container
			meta.Health = r.health
			LSCS[r.name] = r.health.CurrentCalls
		}
	}
	stateMu.Unlock()
}
