#include "string.h"
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/portmacro.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "tcpip_adapter.h"

#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "lwip/err.h"

#include "cJSON.h"

#include "esp32-webserver.h"


static int cntrl_pins[16] = {16, 17, 18, 19, 34, 35, 25, 26, 27, 14, 12, 13, 23, 22, 21, 5};

int cntrl_states[16];

#define delay(ms) (vTaskDelay(ms/portTICK_RATE_MS))

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
//static char* TAG = "app_main";

char *json_unformatted;
wifi_mode_t wifi_mode;

const static char http_html_hdr[] =
	"HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n";
const static char http_json_hdr[] =
	"HTTP/1.1 200 OK\r\nContent-type: application/json\r\n\r\n";

const static char http_index_hml[] = "<!DOCTYPE html>"
	"<html>\n"
	"<head>\n"
	"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
	"  <style type=\"text/css\">\n"
	"	 html, body, iframe { margin: 0; padding: 0; height: 100%; }\n"
	"	 iframe { display: block; width: 100%; border: none; }\n"
	"  </style>\n"
	"<title>HELLO ESP32</title>\n"
	"</head>\n"
	"<body>\n"
	"<h1>Hello World, from ESP32!</h1>\n"
	"</body>\n"
	"</html>\n";


static esp_err_t event_handler(void *ctx, system_event_t *event) {
	switch (event->event_id) {
		case SYSTEM_EVENT_STA_START:
			esp_wifi_connect();
			break;
		case SYSTEM_EVENT_STA_GOT_IP:
			xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
			printf("got ip\n");
			printf("ip: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.ip));
			printf("netmask: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.netmask));
			printf("gw: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.gw));
			printf("\n");
			fflush(stdout);
			break;
		case SYSTEM_EVENT_STA_DISCONNECTED:
			/* This is a workaround as ESP32 WiFi libs don't currently
			   auto-reassociate. */
			esp_wifi_connect();
			xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
			break;
		case SYSTEM_EVENT_AP_START:
			// AP has started up. Now start the DHCP server.
			printf("SYSTEM_EVENT_AP_START.\n");
			// Configure the IP address and DHCP server.
			tcpip_adapter_ip_info_t ipInfo;
			IP4_ADDR(&ipInfo.ip, 192,168,1,1);
			IP4_ADDR(&ipInfo.gw, 192,168,1,1);
			IP4_ADDR(&ipInfo.netmask, 255,255,255,0);
			tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
			if (tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ipInfo) == ESP_OK) {
				printf("starting DHCP server.\n");
				return tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP) == ESP_OK;
			}
			break;
		default:
			break;
	}
	return ESP_OK;
}

static void initialize_wifi(void) {
	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_mode(wifi_mode) );

	if (wifi_mode == WIFI_MODE_STA) {
		wifi_config_t conf = {
			.sta = {
				.ssid = "Nat",
				.password = "123456789",
				.bssid_set = false
			}
		};

		ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &conf) );
		ESP_ERROR_CHECK( esp_wifi_start() );
	} else if (wifi_mode == WIFI_MODE_AP) {
		wifi_config_t conf = {
			.ap = {
				.ssid = "TEST",
				.ssid_len = 4,
				.password = "123456789",
				.authmode = WIFI_AUTH_WPA2_PSK,
				.ssid_hidden = 0,
				.max_connection = 10
			}
		};

		ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &conf) );
		ESP_ERROR_CHECK( esp_wifi_start() );
	}
}

int set_relay_state(int relay, uint32_t level) {
	if ((relay >= 0) && (relay < 16)) {
		gpio_set_level(cntrl_pins[relay], level);
		cntrl_states[relay] = level;
	} else {
		return 0;
	}
	return 1;
}

static void http_server_netconn_serve(struct netconn *conn) {
	struct netbuf *inbuf;
	char *buf;
	u16_t buflen;
	err_t err;

	/* Read the data from the port, blocking if nothing yet there.
	 We assume the request (the part we care about) is in one netbuf */
	err = netconn_recv(conn, &inbuf);

	if (err == ERR_OK) {
		netbuf_data(inbuf, (void**)&buf, &buflen);

	// strncpy(_mBuffer, buf, buflen);

	/* Is this an HTTP GET command? (only check the first 5 chars, since
	 there are other formats for GET, and we're keeping it very simple). */
	printf("buffer = %s \n", buf);
	if (buflen >= 5 &&
		buf[0] == 'G' &&
		buf[1] == 'E' &&
		buf[2] == 'T' &&
		buf[3] == ' ' &&
		buf[4] == '/' ) {
			if ((buflen >= 6) && (buf[5] == 'j')) {
				// JSON status.
				// Send header with 'Content-type: application/json'. 
				netconn_write(conn, http_json_hdr, sizeof(http_json_hdr)-1, NETCONN_NOCOPY);
				// Send the JSON status.
				netconn_write(conn, json_unformatted, strlen(json_unformatted), NETCONN_NOCOPY);
			} else if (buflen >= 7) {
				/* Send the HTML header
				 * subtract 1 from the size, since we dont send the \0 in the string
				 * NETCONN_NOCOPY: our data is const static, so no need to copy it
				*/

				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr)-1, NETCONN_NOCOPY);
				
				uint32_t level = 0;
				int valid = 1;
				// May be a GPIO control request. Check to see.
				// Check for 'h'(igh) or 'l'(ow).
				switch (buf[5]) {
					case 'h':
						level = 1;
						break;
					case 'l':
						level = 0;
						break;
					default: // Neither 'h' nor 'l', so this is not a valid GPIO set request.
						valid = 0;
						break;
				}
				if (valid) {
					//FIXME: Parsing for more than one digit.
					valid = set_relay_state(buf[6] - 48, level);
				}
				if (valid) {
					netconn_write(conn, "OK\n", 3, NETCONN_NOCOPY);
				} else {
					netconn_write(conn, "FAIL\n", 5, NETCONN_NOCOPY);
				}
			} else {
				/* Send the HTML header
				 * subtract 1 from the size, since we dont send the \0 in the string
				 * NETCONN_NOCOPY: our data is const static, so no need to copy it
				*/

				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr)-1, NETCONN_NOCOPY);
				// Default index page.
				netconn_write(conn, http_index_hml, sizeof(http_index_hml)-1, NETCONN_NOCOPY);
			}
		}
	}
	/* Close the connection (server closes in HTTP) */
	netconn_close(conn);

	/* Delete the buffer (netconn_recv gives us ownership,
	 so we have to make sure to deallocate the buffer) */
	netbuf_delete(inbuf);
}

static void http_server(void *pvParameters) {
	struct netconn *conn, *newconn;
	err_t err;
	conn = netconn_new(NETCONN_TCP);
	netconn_bind(conn, NULL, 80);
	netconn_listen(conn);
	do {
		err = netconn_accept(conn, &newconn);
		if (err == ERR_OK) {
			http_server_netconn_serve(newconn);
			netconn_delete(newconn);
		}
	} while(err == ERR_OK);
	netconn_close(conn);
	netconn_delete(conn);
}


static void generate_json() {
	int i;
	char buf[16];
	cJSON *root, *info, *relays;
	root = cJSON_CreateObject();

	cJSON_AddItemToObject(root, "info", info = cJSON_CreateObject());
	cJSON_AddItemToObject(root, "relays", relays = cJSON_CreateObject());

	cJSON_AddStringToObject(info, "ssid", "dummy");
	cJSON_AddNumberToObject(info, "heap", system_get_free_heap_size());
	cJSON_AddStringToObject(info, "sdk", system_get_sdk_version());
	cJSON_AddNumberToObject(info, "time", system_get_time());

	for (i = 0; i < 16; i++) {
		sprintf(buf, "RELAY%d", i); // Relay name.
		cJSON_AddNumberToObject(relays, buf, cntrl_states[i]);
	}

	while (1) {
		cJSON_ReplaceItemInObject(info, "heap",
				cJSON_CreateNumber(system_get_free_heap_size()));
		cJSON_ReplaceItemInObject(info, "time",
				cJSON_CreateNumber(system_get_time()));
		cJSON_ReplaceItemInObject(info, "sdk",
				cJSON_CreateString(system_get_sdk_version()));

		for (i = 0; i < 16; i++) {
			sprintf(buf, "RELAY%d", i); // Relay name.
			cJSON_ReplaceItemInObject(relays, buf, cJSON_CreateNumber(cntrl_states[i]));
		}

		json_unformatted = cJSON_PrintUnformatted(root);
		printf("[len = %d]	", strlen(json_unformatted));

		for (int var = 0; var < strlen(json_unformatted); ++var) {
			putc(json_unformatted[var], stdout);
		}

		printf("\n");
		fflush(stdout);
		delay(2000);
		free(json_unformatted);
	}
}

int app_main(void) {
	int i;
	nvs_flash_init();
	system_init();

	wifi_mode = WIFI_MODE_AP;

	initialize_wifi();

	// Initialize GPIOs.
	for (i = 0; i < 16; i++) {
		gpio_pad_select_gpio(cntrl_pins[i]);
		gpio_set_direction(cntrl_pins[i], GPIO_MODE_OUTPUT);
		gpio_set_level(cntrl_pins[i], 0);
		cntrl_states[i] = 0;
	}

	xTaskCreate(&generate_json, "json", 2048, NULL, 5, NULL);
	xTaskCreate(&http_server, "http_server", 2048, NULL, 5, NULL);
	bt_main(); // Initiate Bluetooth services.

	return 0;
}
