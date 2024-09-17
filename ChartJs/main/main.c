/*
	Example using WEB Socket.
	This example code is in the Public Domain (or CC0 licensed, at your option.)
	Unless required by applicable law or agreed to in writing, this
	software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
	CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/message_buffer.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "esp32_perfmon.h"
#include "websocket_server.h"
#include "ethernet_init.h"
#include "sdkconfig.h"

#define MAX_NO_OF_ETHERNET_INTERFACES 2
MessageBufferHandle_t xMessageBufferToClient;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

void client_task(void *pvParameters);
void server_task(void *pvParameters);

static const char *TAG = "main";

static int s_retry_num = 0;

static uint8_t eth_port_cnt = 0;
static esp_eth_handle_t *eth_handles;

static void
event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		} else {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG,"connect to the AP fail");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

void wifi_init_sta(void)
{
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
		ESP_EVENT_ANY_ID,
		&event_handler,
		NULL,
		&instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
		IP_EVENT_STA_GOT_IP,
		&event_handler,
		NULL,
		&instance_got_ip));

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_ESP_WIFI_SSID,
			.password = CONFIG_ESP_WIFI_PASSWORD,
			/* Setting a password implies station will connect to all security modes including WEP/WPA.
			 * However these modes are deprecated and not advisable to be used. Incase your Access point
			 * doesn't support WPA2, these mode can be enabled by commenting below line */
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,

			.pmf_cfg = {
				.capable = true,
				.required = false
			},
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
	ESP_ERROR_CHECK(esp_wifi_start() );

	ESP_LOGI(TAG, "wifi_init_sta finished.");

	/* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
	 * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
		WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
		pdFALSE,
		pdFALSE,
		portMAX_DELAY);

	/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
	 * happened. */
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
	} else {
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}

	/* The event will not be processed after unregister */
	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
	vEventGroupDelete(s_wifi_event_group);
}

void initialise_mdns(void)
{
	//initialize mDNS
	ESP_ERROR_CHECK( mdns_init() );
	//set mDNS hostname (required if you want to advertise services)
	ESP_ERROR_CHECK( mdns_hostname_set(CONFIG_MDNS_HOSTNAME) );
	ESP_LOGI(TAG, "mdns hostname set to: [%s]", CONFIG_MDNS_HOSTNAME);

	//initialize service
	ESP_ERROR_CHECK( mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0) );

#if 0
	//set default mDNS instance name
	ESP_ERROR_CHECK( mdns_instance_name_set("ESP32 with mDNS") );
#endif
}

#if 0

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
							  int32_t event_id, void *event_data)
{
	uint8_t mac_addr[6] = {0};
	/* we can get the ethernet driver handle from event data */
	esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

	switch (event_id)
	{
	case ETHERNET_EVENT_CONNECTED:
		esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
		ESP_LOGI(TAG, "Ethernet Link Up");
		ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
				 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
		break;
	case ETHERNET_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "Ethernet Link Down");
		break;
	case ETHERNET_EVENT_START:
		ESP_LOGI(TAG, "Ethernet Started");
		break;
	case ETHERNET_EVENT_STOP:
		ESP_LOGI(TAG, "Ethernet Stopped");
		break;
	default:
		break;
	}
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
								 int32_t event_id, void *event_data)
{
	ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
	const esp_netif_ip_info_t *ip_info = &event->ip_info;

	ESP_LOGI(TAG, "Ethernet Got IP Address");
	ESP_LOGI(TAG, "~~~~~~~~~~~");
	ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
	ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
	ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
	ESP_LOGI(TAG, "~~~~~~~~~~~");
}

void ethernet_init(void)
{
	// Initialize Ethernet driver
	uint8_t eth_port_cnt = 0;
	esp_eth_handle_t *eth_handles;
	ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));

	// Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
	ESP_ERROR_CHECK(esp_netif_init());
	// Create default event loop that running in background
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_netif_t *eth_netifs[eth_port_cnt];
	esp_eth_netif_glue_handle_t eth_netif_glues[eth_port_cnt];

	// Create instance(s) of esp-netif for Ethernet(s)
	if (eth_port_cnt == 1)
	{
		// Use ESP_NETIF_DEFAULT_ETH when just one Ethernet interface is used and you don't need to modify
		// default esp-netif configuration parameters.
		esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
		eth_netifs[0] = esp_netif_new(&cfg);
		eth_netif_glues[0] = esp_eth_new_netif_glue(eth_handles[0]);
		// Attach Ethernet driver to TCP/IP stack
		ESP_ERROR_CHECK(esp_netif_attach(eth_netifs[0], eth_netif_glues[0]));
	}
	else
	{
		// Use ESP_NETIF_INHERENT_DEFAULT_ETH when multiple Ethernet interfaces are used and so you need to modify
		// esp-netif configuration parameters for each interface (name, priority, etc.).
		esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
		esp_netif_config_t cfg_spi = {
			.base = &esp_netif_config,
			.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
		char if_key_str[10];
		char if_desc_str[10];
		char num_str[3];
		for (int i = 0; i < eth_port_cnt; i++)
		{
			itoa(i, num_str, 10);
			strcat(strcpy(if_key_str, "ETH_"), num_str);
			strcat(strcpy(if_desc_str, "eth"), num_str);
			esp_netif_config.if_key = if_key_str;
			esp_netif_config.if_desc = if_desc_str;
			esp_netif_config.route_prio -= i * 5;
			eth_netifs[i] = esp_netif_new(&cfg_spi);
			eth_netif_glues[i] = esp_eth_new_netif_glue(eth_handles[0]);
			// Attach Ethernet driver to TCP/IP stack
			ESP_ERROR_CHECK(esp_netif_attach(eth_netifs[i], eth_netif_glues[i]));
		}
	}

	// Register user defined event handers
	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

	// Start Ethernet driver state machine
	for (int i = 0; i < eth_port_cnt; i++)
	{
		ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
	}

#if CONFIG_EXAMPLE_ETH_DEINIT_AFTER_S >= 0
	// For demonstration purposes, wait and then deinit Ethernet network
	vTaskDelay(pdMS_TO_TICKS(CONFIG_EXAMPLE_ETH_DEINIT_AFTER_S * 1000));
	ESP_LOGI(TAG, "stop and deinitialize Ethernet network...");
	// Stop Ethernet driver state machine and destroy netif
	for (int i = 0; i < eth_port_cnt; i++)
	{
		ESP_ERROR_CHECK(esp_eth_stop(eth_handles[i]));
		ESP_ERROR_CHECK(esp_eth_del_netif_glue(eth_netif_glues[i]));
		esp_netif_destroy(eth_netifs[i]);
	}
	esp_netif_deinit();
	ESP_ERROR_CHECK(example_eth_deinit(eth_handles, eth_port_cnt));
	ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler));
	ESP_ERROR_CHECK(esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler));
	ESP_ERROR_CHECK(esp_event_loop_delete_default());
#endif // EXAMPLE_ETH_DEINIT_AFTER_S > 0
}
#endif



void app_main() {
	// Initialize NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// Initialize WiFi
	wifi_init_sta();

	// Initialize Ethernet driver
	ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));

	// Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
	ESP_ERROR_CHECK(esp_netif_init());
	// Create default event loop that running in background
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_netif_t *eth_netifs[eth_port_cnt];
	esp_eth_netif_glue_handle_t eth_netif_glues[eth_port_cnt];


	// Use ESP_NETIF_DEFAULT_ETH when just one Ethernet interface is used and you don't need to modify
	// default esp-netif configuration parameters.
	esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
	eth_netifs[0] = esp_netif_new(&cfg);
	eth_netif_glues[0] = esp_eth_new_netif_glue(eth_handles[0]);
	// Attach Ethernet driver to TCP/IP stack
	ESP_ERROR_CHECK(esp_netif_attach(eth_netifs[0], eth_netif_glues[0]));

	// Initialize mDNS
	initialise_mdns();

	// Create Message Buffer
	xMessageBufferToClient = xMessageBufferCreate(1024);
	configASSERT( xMessageBufferToClient );

	// Get the local IP address
	esp_netif_ip_info_t ip_info;
	ESP_ERROR_CHECK(esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info));
	char cparam0[64];
	sprintf(cparam0, IPSTR, IP2STR(&ip_info.ip));

	// Start web socket server
	ws_server_start();

	// Start web server
	xTaskCreate(&server_task, "server_task", 1024*4, (void *)cparam0, 5, NULL);

	// Start web client
	xTaskCreate(&client_task, "client_task", 1024*4, NULL, 5, NULL);

	// Start perfmon
	perfmon_start();

	vTaskDelay(100);
}
