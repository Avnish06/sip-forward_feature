package main

import (
	"log"

	"app/db"
	"app/services"
	"app/utils"

	"github.com/gin-gonic/gin"
)

/*

	Load Balancer Architecture:

	1.⁠ ⁠Redis as pure registration database registration should only store pure registration metadata from server like aor, port, ip, host, etc. Not the health and current calls. Its pure funtion is for cheap persistent storage for registrations.

	2.⁠ ⁠In Memory: We will have -> Inbound/Outbound Server List (IOSL), LocalServerMetadataList (LSML), and LocalServerCallCountSet (LSCS)


	A. LSML will hold the whole metadata including health and current calls data into a array of set with server name as set key (in short, same as redis, but in memory so no redis pooling and latency):


	LSML = [
		{
			"server1": {
				"host": "...",
				"aor": "...",
				"ip": "..",
				...
			},
			...
		},
		...
	]


	B. LSCS is a set of servername as key and corresponding call count as value, as simple as that. Will use this for quick fetching of corresponding server current call count

	{
		"server1": 10,
		"server2": 9,
		...
	}


	C. IOSL are the arrays of server name that have inbound and outbound enabled. For In/Outbound, we use this list as contenders for routing call and fetch the values from LSCS set for routing logic.

	InboundServerList = ["server1", "server2", ...];
	OutboundServerList = ["server1", "server2", ...];


	Implementation:
	1.⁠ ⁠Heartbeat Worker:
	Iterate over all servers in LSML and update the LSML and LSCS.
	In case of unhealthy, we set unhealty in metadata in LSML and set call count INF in LSCS.

	2.⁠ ⁠Routing:
	A separate funtion for this (so we can change logic anytime, currently its findLowestContainer, keep it).
	For now, depending on call type In.Outbound, we iterate over that IOSL and find minimum call count server using LSCS and construct the endpoint using LSML.
	If minimum call count is INF, hence all servers are unhealthy, return 503, else route to that server.

*/

func main() {
	// Load configuration
	cfg := utils.LoadAppConfig()

	// Initialize Redis connection
	if err := db.InitRedis(cfg); err != nil {
		log.Fatalf("Could not initialize Redis: %v", err)
	}

	// Initialize LSML/LSCS/IOSL from Redis registrations (must happen before health loop)
	services.InitServerArrays()

	// Start background heartbeat worker (Non-blocking) — updates LSML + LSCS
	services.StartHealthCheckLoop(cfg)

	// Initialize the Gin router
	r := gin.Default()

	// Serve static files for dashboard
	r.Static("/public", "./public")
	r.GET("/", func(c *gin.Context) {
		c.File("./public/index.html")
	})

	r.POST("/register-container", services.RegisterContainerHandler)

	r.DELETE("/unregister-container/:container_name", services.UnregisterContainerHandler)

	r.PUT("/toggle-container/:container_name", services.ToggleContainerHandler)

	r.GET("/list-containers", services.ListContainersHandler)

	r.GET("/health", services.HealthHandler)

	r.POST("/initiate-call", services.InitiateCallHandler)

	r.GET("/get-inbound-contact/:asterisk", services.GetInboundContactHandler)

	r.POST("/end-call", services.EndCallHandler)

	r.GET("/get-all-calls", services.GetAllCallsHandler)

	r.GET("/pjsip/endpoints", services.ListPjsipEndpointsHandler)

	r.GET("/pjsip/endpoint/:name", services.ShowPjsipEndpointHandler)

	r.Run(":" + cfg.AppPort)
}
