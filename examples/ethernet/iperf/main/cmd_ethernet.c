/* Console example — Ethernet commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "tcpip_adapter.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "argtable3/argtable3.h"
#include "iperf.h"
#include "sdkconfig.h"

static tcpip_adapter_ip_info_t ip;
static bool started = false;
static EventGroupHandle_t eth_event_group;
static const int GOTIP_BIT = BIT0;
static esp_eth_handle_t eth_handle = NULL;

/* "ethernet" command */
static struct {
    struct arg_str *control;
    struct arg_end *end;
} eth_control_args;

static int eth_cmd_control(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&eth_control_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, eth_control_args.end, argv[0]);
        return 1;
    }

    if (!strncmp(eth_control_args.control->sval[0], "info", 4)) {
        uint8_t mac_addr[6];
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        printf("HW ADDR: " MACSTR "\r\n", MAC2STR(mac_addr));
        tcpip_adapter_get_ip_info(ESP_IF_ETH, &ip);
        printf("ETHIP: " IPSTR "\r\n", IP2STR(&ip.ip));
        printf("ETHMASK: " IPSTR "\r\n", IP2STR(&ip.netmask));
        printf("ETHGW: " IPSTR "\r\n", IP2STR(&ip.gw));
    }
    return 0;
}

/* "iperf" command */

static struct {
    struct arg_str *ip;
    struct arg_lit *server;
    struct arg_lit *udp;
    struct arg_int *port;
    struct arg_int *interval;
    struct arg_int *time;
    struct arg_lit *abort;
    struct arg_end *end;
} iperf_args;

static int eth_cmd_iperf(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&iperf_args);
    iperf_cfg_t cfg;

    if (nerrors != 0) {
        arg_print_errors(stderr, iperf_args.end, argv[0]);
        return 0;
    }

    memset(&cfg, 0, sizeof(cfg));

    /* iperf -a */
    if (iperf_args.abort->count != 0) {
        iperf_stop();
        return 0;
    }

    if (((iperf_args.ip->count == 0) && (iperf_args.server->count == 0)) ||
            ((iperf_args.ip->count != 0) && (iperf_args.server->count != 0))) {
        ESP_LOGE(__func__, "Wrong mode! ESP32 should run in client or server mode");
        return 0;
    }

    /* iperf -s */
    if (iperf_args.ip->count == 0) {
        cfg.flag |= IPERF_FLAG_SERVER;
    }
    /* iperf -c SERVER_ADDRESS */
    else {
        cfg.dip = ipaddr_addr(iperf_args.ip->sval[0]);
        cfg.flag |= IPERF_FLAG_CLIENT;
    }

    /* acquiring for ip, could blocked here */
    xEventGroupWaitBits(eth_event_group, GOTIP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    cfg.sip = ip.ip.addr;
    if (cfg.sip == 0) {
        return 0;
    }

    /* iperf -u */
    if (iperf_args.udp->count == 0) {
        cfg.flag |= IPERF_FLAG_TCP;
    } else {
        cfg.flag |= IPERF_FLAG_UDP;
    }

    /* iperf -p */
    if (iperf_args.port->count == 0) {
        cfg.sport = IPERF_DEFAULT_PORT;
        cfg.dport = IPERF_DEFAULT_PORT;
    } else {
        if (cfg.flag & IPERF_FLAG_SERVER) {
            cfg.sport = iperf_args.port->ival[0];
            cfg.dport = IPERF_DEFAULT_PORT;
        } else {
            cfg.sport = IPERF_DEFAULT_PORT;
            cfg.dport = iperf_args.port->ival[0];
        }
    }

    /* iperf -i */
    if (iperf_args.interval->count == 0) {
        cfg.interval = IPERF_DEFAULT_INTERVAL;
    } else {
        cfg.interval = iperf_args.interval->ival[0];
        if (cfg.interval <= 0) {
            cfg.interval = IPERF_DEFAULT_INTERVAL;
        }
    }

    /* iperf -t */
    if (iperf_args.time->count == 0) {
        cfg.time = IPERF_DEFAULT_TIME;
    } else {
        cfg.time = iperf_args.time->ival[0];
        if (cfg.time <= cfg.interval) {
            cfg.time = cfg.interval;
        }
    }

    printf("mode=%s-%s sip=%d.%d.%d.%d:%d, dip=%d.%d.%d.%d:%d, interval=%d, time=%d\r\n",
           cfg.flag & IPERF_FLAG_TCP ? "tcp" : "udp",
           cfg.flag & IPERF_FLAG_SERVER ? "server" : "client",
           cfg.sip & 0xFF, (cfg.sip >> 8) & 0xFF, (cfg.sip >> 16) & 0xFF, (cfg.sip >> 24) & 0xFF, cfg.sport,
           cfg.dip & 0xFF, (cfg.dip >> 8) & 0xFF, (cfg.dip >> 16) & 0xFF, (cfg.dip >> 24) & 0xFF, cfg.dport,
           cfg.interval, cfg.time);

    iperf_start(&cfg);
    return 0;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_START) {
        started = true;
    } else if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_STOP) {
        xEventGroupClearBits(eth_event_group, GOTIP_BIT);
        started = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        memcpy(&ip, &event->ip_info, sizeof(ip));
        xEventGroupSetBits(eth_event_group, GOTIP_BIT);
    }
}

void register_ethernet()
{
    eth_event_group = xEventGroupCreate();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(tcpip_adapter_set_default_eth_handlers());
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &event_handler, NULL));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
#if CONFIG_EXAMPLE_ETH_PHY_IP101
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#elif CONFIG_EXAMPLE_ETH_PHY_RTL8201
    esp_eth_phy_t *phy = esp_eth_phy_new_rtl8201(&phy_config);
#elif CONFIG_EXAMPLE_ETH_PHY_LAN8720
    esp_eth_phy_t *phy = esp_eth_phy_new_lan8720(&phy_config);
#elif CONFIG_EXAMPLE_ETH_PHY_DP83848
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
#endif
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    eth_control_args.control = arg_str1(NULL, NULL, "<info>", "Get info of Ethernet");
    eth_control_args.end = arg_end(1);
    const esp_console_cmd_t cmd = {
        .command = "ethernet",
        .help = "Ethernet interface IO control",
        .hint = NULL,
        .func = eth_cmd_control,
        .argtable = &eth_control_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

    iperf_args.ip = arg_str0("c", "client", "<ip>",
                             "run in client mode, connecting to <host>");
    iperf_args.server = arg_lit0("s", "server", "run in server mode");
    iperf_args.udp = arg_lit0("u", "udp", "use UDP rather than TCP");
    iperf_args.port = arg_int0("p", "port", "<port>",
                               "server port to listen on/connect to");
    iperf_args.interval = arg_int0("i", "interval", "<interval>",
                                   "seconds between periodic bandwidth reports");
    iperf_args.time = arg_int0("t", "time", "<time>", "time in seconds to transmit for (default 10 secs)");
    iperf_args.abort = arg_lit0("a", "abort", "abort running iperf");
    iperf_args.end = arg_end(1);
    const esp_console_cmd_t iperf_cmd = {
        .command = "iperf",
        .help = "iperf command",
        .hint = NULL,
        .func = &eth_cmd_iperf,
        .argtable = &iperf_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&iperf_cmd));
}
