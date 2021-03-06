#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <lwip/dns.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define EXAMPLE_WIFI_SSID
#define EXAMPLE_WIFI_PASS
/*#define OTA_SERVER_IP   "esp32.chege.eu.org"
#define EXAMPLE_SERVER_PORT "80"
#define EXAMPLE_PATH "examples"
#define EXAMPLE_FILENAME "ota.bin"*/
#define BUFFSIZE 1024
#define TEXT_BUFFSIZE 1024
const int CONNECTED_BIT = BIT0;

std::string ssid;
std::string pass;

typedef struct {
	char ip[16];
	char domain[64];
	char port[16];
	char path[64];
	char file_name[128];
} ota_info_t ;

static EventGroupHandle_t wifi_event_group;

static const char *TAG = "ota";
/*an ota data write buffer ready to write to the flash*/
static char ota_write_data[BUFFSIZE + 1] = { 0 };
/*an packet receive buffer*/
static char text[BUFFSIZE + 1] = { 0 };
/* an image total length*/
static int binary_file_length = 0;
/*socket id*/
static int socket_id = -1;
static char http_request[64] = {0};
static ota_info_t* otaInfo;
static void __attribute__((noreturn)) task_fatal_error()
{
    ESP_LOGE(TAG, "Exiting task due to fatal error...");
    close(socket_id);
    (void)vTaskDelete(NULL);

    while (1) {
        ;
    }
}

static int read_until(char *buffer, char delim, int len)
{
    int i = 0;
    while (buffer[i] != delim && i < len) {
        ++i;
    }
    return i + 1;
}

/* resolve a packet from http socket
 * return true if packet including \r\n\r\n that means http packet header finished,start to receive packet body
 * otherwise return false
 * */
static bool read_past_http_header(char text[], int total_len, esp_ota_handle_t update_handle)
{
    /* i means current position */
    int i = 0, i_read_len = 0;
    while (text[i] != 0 && i < total_len) {
        i_read_len = read_until(&text[i], '\n', total_len);
        // if we resolve \r\n line,we think packet header is finished
        if (i_read_len == 2) {
            int i_write_len = total_len - (i + 2);
            memset(ota_write_data, 0, BUFFSIZE);
            /*copy first http packet body to write buffer*/
            memcpy(ota_write_data, &(text[i + 2]), i_write_len);

            esp_err_t err = esp_ota_write( update_handle, (const void *)ota_write_data, i_write_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
                return false;
            } else {
                ESP_LOGI(TAG, "esp_ota_write header OK");
                binary_file_length += i_write_len;
            }
            return true;
        }
        i += i_read_len;
    }
    return false;
}

ip_addr_t addr;
bool bDNSFound = false;
static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
    ESP_LOGI("ota", "%sfound host ip %s", ipaddr == NULL?"NOT ":"", otaInfo->domain);
    if(ipaddr == NULL)
    	task_fatal_error();
    addr = *ipaddr;
    bDNSFound = true;
    ESP_LOGI("ota", "DNS found IP: %i.%i.%i.%i, host name: %s",
            ip4_addr1(&addr.u_addr.ip4),
            ip4_addr2(&addr.u_addr.ip4),
            ip4_addr3(&addr.u_addr.ip4),
            ip4_addr4(&addr.u_addr.ip4),
			otaInfo->domain);
    vTaskDelay(2000/portTICK_PERIOD_MS);
}

static bool connect_to_http_server()
{

	// todo if nvs ota path empty set default file name
	// todo if nvs ota server empty set default server
	// todo if nvs ota server ip set server ip
	ip_addr_t addr1;
	IP_ADDR4( &addr1, 8,8,4,4 );   // DNS server 0
	dns_setserver(0, &addr1);
	IP_ADDR4( &addr1, 8,8,8,8 );   // DNS server 1
	dns_setserver(1, &addr1);

	err_t err = dns_gethostbyname(otaInfo->domain, &addr, &dns_found_cb, NULL);

    while( !bDNSFound ){
    	vTaskDelay(150/portTICK_PERIOD_MS);
    }

    inet_pton(AF_INET, otaInfo->ip, &addr.u_addr.ip4);
    ESP_LOGD(TAG, "Server IP: %d Server Port:%s", addr.u_addr.ip4.addr, otaInfo->port);
    sprintf(http_request, "GET /%s/%s HTTP/1.1\r\nHost: %s:%s \r\n\r\n", otaInfo->path, otaInfo->file_name, strlen(otaInfo->ip)!=0 ? otaInfo->ip : otaInfo->domain, otaInfo->port);
    ESP_LOGI(TAG, "request: %s", http_request);

    int  http_connect_flag = -1;
    struct sockaddr_in sock_info;

    socket_id = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_id == -1) {
        ESP_LOGE(TAG, "Create socket failed!");
        return false;
    }

    // set connect info
    memset(&sock_info, 0, sizeof(struct sockaddr_in));
    sock_info.sin_family = AF_INET;
    sock_info.sin_addr.s_addr = addr.u_addr.ip4.addr;
    sock_info.sin_port = htons(atoi(otaInfo->port));

    // connect to http server
    http_connect_flag = connect(socket_id, (struct sockaddr *)&sock_info, sizeof(sock_info));
    if (http_connect_flag == -1) {
        ESP_LOGE(TAG, "Connect to server failed! errno=%d", errno);
        close(socket_id);
        return false;
    } else {
        ESP_LOGI(TAG, "Connected to server");
        return true;
    }
    return false;
}

static void ota_example_task(void *pvParameter)
{
    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA example...");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    ESP_LOGI(TAG, "Connect to Wifi ! Start to Connect to Server....");

    /*connect to http server*/
    if (connect_to_http_server()) {
        ESP_LOGI(TAG, "Connected to http server");
    } else {
        ESP_LOGE(TAG, "Connect to http server failed!");
        task_fatal_error();
    }

    int res = -1;
    /*send GET request to http server*/
    res = send(socket_id, http_request, strlen(http_request), 0);
    if (res == -1) {
        ESP_LOGE(TAG, "Send GET request to server failed");
        task_fatal_error();
    } else {
        ESP_LOGI(TAG, "Send GET request to server succeeded");
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
        task_fatal_error();
    }
    ESP_LOGI(TAG, "esp_ota_begin succeeded");

    bool resp_body_start = false, flag = true;
    /*deal with all receive packet*/
    while (flag) {
        memset(text, 0, TEXT_BUFFSIZE);
        memset(ota_write_data, 0, BUFFSIZE);
        int buff_len = recv(socket_id, text, TEXT_BUFFSIZE, 0);
        if (buff_len < 0) { /*receive error*/
            ESP_LOGE(TAG, "Error: receive data error! errno=%d", errno);
            task_fatal_error();
        } else if (buff_len > 0 && !resp_body_start) { /*deal with response header*/
            memcpy(ota_write_data, text, buff_len);
            resp_body_start = read_past_http_header(text, buff_len, update_handle);
        } else if (buff_len > 0 && resp_body_start) { /*deal with response body*/
            memcpy(ota_write_data, text, buff_len);
            err = esp_ota_write( update_handle, (const void *)ota_write_data, buff_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
                task_fatal_error();
            }
            binary_file_length += buff_len;
            ESP_LOGI(TAG, "Have written image length %d", binary_file_length);
        } else if (buff_len == 0) {  /*packet over*/
            flag = false;
            ESP_LOGI(TAG, "Connection closed, all packets received");
            close(socket_id);
        } else {
            ESP_LOGE(TAG, "Unexpected recv result");
        }
    }

    ESP_LOGI(TAG, "Total Write binary data length : %d", binary_file_length);

    if (esp_ota_end(update_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed!");
        task_fatal_error();
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
        task_fatal_error();
    }
    ESP_LOGI(TAG, "Prepare to restart system!");
    esp_restart();
    return ;
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	wifi_config_t sta_config;
	::memset(&sta_config, 0, sizeof(sta_config));
	::memcpy(sta_config.sta.ssid, ssid.data(), ssid.size());
	::memcpy(sta_config.sta.password, pass.data(), pass.size());
	sta_config.sta.bssid_set = 0;

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", sta_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}
#include "CPPNVS.h"
#define WIFIINFO "wifi_info"
#define OTAINFO "ota_info"

//void ota_app_main()
void ota_app_main(void* conn, std::string wifi_ssid, std::string wifi_pass)
{
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    if(wifi_ssid!=""){
    	ssid = wifi_ssid;
    	pass = wifi_pass;
    }//else{
/*    	typedef struct {
    		char ssid[32];
    		char password[64];
    	} wifi_info_t;
    	wifi_info_t wifi;

    	NVS* otaNVS = new NVS("ota_nvs");
		size_t size = sizeof(wifi_info_t);
		otaNVS->get(WIFIINFO, (uint8_t*)&wifi, size);*/

 //   }
    if(conn!=nullptr){
    	otaInfo = (ota_info_t*)conn;
        ESP_LOGI(TAG, "GET /%s/%s HTTP/1.1\r\nHost: %s:%s \r\n\r\n", otaInfo->path, otaInfo->file_name, otaInfo->domain, otaInfo->port);
    }
    initialise_wifi();

    xTaskCreate(&ota_example_task, "ota_example_task", 8192, NULL, 5, NULL);
}
