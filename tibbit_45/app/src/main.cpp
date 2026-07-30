#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <errno.h>

#include <zephyr/net/socket.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include "socket_mgr.h"

#include <zephyr/sys/reboot.h>
#include "work_queues.h"

#include <zephyr/drivers/gpio.h>

#include "udp_fwloader.h"
#include "led_pattern.h"
#ifdef CONFIG_LOG_BACKEND_UDP
#include "log_backend.h"
#endif

#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
#include "gdb_stub.h"
#endif

#include <zephyr/input/input.h>

#include "beep.h"

#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>

#include <time.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket_service.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
#include <string>
#include <cstring>
#include "utils.h"
#include <cmath>

/* This part belongs to CELLULAR - START: includes */
#include <cstdlib>
#include <zephyr/devicetree.h>
#include "tb45_cellular.h"
#include "tb45_ping.h"

#if defined(CONFIG_APP_TB45_SMS_ENABLE) && CONFIG_APP_TB45_SMS_ENABLE
#include "tb45_sms.h"
#include "tb45_sms_event.h"
#endif

/* This part belongs to CELLULAR - END: includes */

LOG_MODULE_REGISTER(app);

using namespace std;

int main_tick_interval = 1000;

bool net_if_ready = false;

#define NET_L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define NET_IPV4_EVENT_MASK (NET_EVENT_IPV4_ADDR_ADD)
#define NET_IF_EVENT_MASK (NET_EVENT_IF_UP | NET_EVENT_IF_DOWN)

#if !DT_NODE_EXISTS(DT_NODELABEL(green_led_gpio))
#error "Overlay for green_led_gpio node not properly defined."
#endif
        
#if !DT_NODE_EXISTS(DT_NODELABEL(red_led_gpio))
#error "Overlay for red_led_gpio node not properly defined."
#endif

static const struct gpio_dt_spec green_led_gpio =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(green_led_gpio), gpios, {0});
static const struct gpio_dt_spec red_led_gpio =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(red_led_gpio), gpios, {0});
static const struct gpio_dt_spec eth_rst =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(eth_rst), gpios, {0});

#if !DT_NODE_EXISTS(DT_NODELABEL(mdbutton))
#error "Overlay for mdbutton node not properly defined."
#endif

#define SNTP_DEBUG_PRINT 0

struct interface_set_params {
    int interface;
    bool state;
};

// Work item for interface_set
K_MSGQ_DEFINE(interface_set_msgq, sizeof(struct interface_set_params), 8, 4);
static struct k_work interface_set_work;

static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback net_l4_mgmt_cb;
static struct net_mgmt_event_callback net_if_mgmt_cb;

const char *device_id = "TPP2W(G2)";
const char *app_name = "nzephyr_cellular";
const char *app_version = "1.0.0";

int32_t tz_offset_minutes = 0;

static const struct gpio_dt_spec mdbutton =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(mdbutton), gpios, {0});
static struct gpio_callback mdbutton_cb_data;
struct k_work mdbutton_pressed_work;
struct k_work mdbutton_released_work;

            
static bool dhcp_enabled = false;

#define VARS_WORKQ_STACK_SIZE 2048
#define VARS_WORKQ_PRIORITY 5
K_THREAD_STACK_DEFINE(vars_workq_stack, VARS_WORKQ_STACK_SIZE);
static struct k_work_q vars_workq;

int T01_S1 = 1;
int T01_S5 = 1;

/* This part belongs to CELLULAR - START: globals, constants, and declarations */

/* Cellular runtime config */
/* NOTE: should you see this warning: <wrn> modem_cellular_custom: AT+COPS failed for carrier_id 46692 (: 30); continuing with modem default operator selection 
 *   then it means that the carrier_id will switch to using AUTO mode.
 * Chunghwa APN: internet
 * onomondo APN: onomondo
 * 1NCE APN: iot.1nce.net
 *
*/
static const struct tb45_cellular_config cellular_cfg = {
    .apn      = "internet",
    .username = NULL,
    .password = NULL,
    .auth_type = TB45_CELL_AUTH_NONE,
    .sim_pin    = "0000",
    .carrier_id = "AUTO",   // Default: AUTO
    .wq         = &low_priority_wq,
};

/* Cellular state and helper declarations */
bool ppp_if_ready = false;

static struct net_if *get_ppp_iface(void);
static void app_queue_ppp_ping_test(void);
#if defined(CONFIG_APP_TB45_SMS_ENABLE) && CONFIG_APP_TB45_SMS_ENABLE
static void app_sms_send_and_ping_test(void);
static void app_sms_recover_stored_unread_messages(void);
static void app_queue_ppp_sms_test_batch(void);
#endif

/* This part belongs to CELLULAR - END: globals, constants, and declarations */

static struct net_if *get_ethernet_iface(void);
void interface_set(int interface, bool state);
void var_T01_S1_update();
void var_T01_S1_set(int value);
void var_T01_S1_update_completed(int value);
void var_T01_S5_update();
void var_T01_S5_set(int value);
void var_T01_S5_update_completed(int value);

static void do_sntp(int family);

void interface_set(int interface, bool state)
{
    if (interface == 1) {
        if (state == true) {
            int i = 0;
            for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++)
            {
                struct net_if *iface = get_ethernet_iface();
                if (dhcp_enabled) {
                    if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type == NET_ADDR_DHCP)
                    {
                        LOG_INF("DHCP assigned");
                    }
                }
            }
        } else {
            LOG_INF("Ethernet Network connectivity down!");
        }
    }
    
    if (state == true) {
        do_sntp(AF_INET);
    }

    // determine default interface
    struct net_if *eth_iface = get_ethernet_iface();
    struct net_if *wifi_iface = net_if_get_first_wifi();
    if (net_if_is_up(eth_iface)) {
        net_if_set_default(eth_iface);
    } else if (net_if_is_up(wifi_iface)) {
        net_if_set_default(wifi_iface);
    } else {
        LOG_DBG("No network interface is up");
    }
}

static void interface_set_work_handler(struct k_work *work)
{
    struct interface_set_params params;
    while (k_msgq_get(&interface_set_msgq, &params, K_NO_WAIT) == 0) {
        interface_set(params.interface, params.state);
        k_sleep(K_MSEC(100));
    }
}

void k_sys_fatal_error_handler(unsigned int reason,
                               const struct arch_esf *esf)
{
    printk("Fatal error occurred! reason=%u\n", reason);
#if defined(CONFIG_ARCH_HAS_EXCEPTION_ESF)
    if (esf) {
        printk("Exception frame available\n");
    }
#endif

    printk("Rebooting system...\n");
    k_sleep(K_MSEC(100));
    sys_reboot(SYS_REBOOT_COLD);
    while (1) {
    }
}

static int preinit(void)
{
    k_work_init(&interface_set_work, interface_set_work_handler);
    // initialize ethernet
    return 0;
}

static struct net_if *get_ethernet_iface(void)
{
    struct net_if *tmp;
    for (int i = 1; (tmp = net_if_get_by_index(i)) != NULL; i++) {
        if (net_if_l2(tmp) == &NET_L2_GET_NAME(ETHERNET) &&
            !net_if_is_wifi(tmp)) {
            return tmp;
        }
    }
    LOG_ERR("No RJ45 Ethernet interface found, falling back to default");
    return net_if_get_default();
}

/* This part belongs to CELLULAR - START: functions */
static struct net_if *get_ppp_iface(void)
{
    struct net_if *tmp;
    for (int i = 1; (tmp = net_if_get_by_index(i)) != NULL; i++) {
        if (net_if_l2(tmp) == &NET_L2_GET_NAME(PPP)) {
            return tmp;
        }
    }
    return NULL;
}

#if defined(CONFIG_APP_TB45_SMS_ENABLE) && CONFIG_APP_TB45_SMS_ENABLE

/* Added for Cellular - SMS and PING TEST ONLY */
/*
 * This manual recovery checks LOCAL MODEM STORAGE (thus NOT from the tower) for unread SMS that may have
 * already arrived but not yet been surfaced to the app. It does not fetch an
 * SMS that is still being held upstream by the network/SMSC (=tower) during reboot or
 * modem downtime.
 */
static void app_sms_recover_stored_unread_messages(void)
{
    int ret = tb45_sms_receive_recover_stored_unread_messages();

    if (ret < 0) {
        LOG_ERR("Manual stored-SMS recovery trigger failed (%d)", ret);
    } else {
        LOG_INF("Manual stored-SMS recovery requested");
        LOG_INF("NOTE: If no SMS appears shortly,...");
        LOG_INF("...the old SMS may not be in local modem storage yet.");
        LOG_INF("REASON: this could be due to a System/Modem Reboot or network/SMSC delay.");
        LOG_INF("In that case it can still arrive later via the normal SMS event path,...");
        LOG_INF("...and this has taken up to about 4 minutes in testing.");
    }
}

static void app_sms_send_and_ping_test(void)
{
    if (!ppp_if_ready) {
        LOG_WRN("TEST ABORTED: PPP IPCP is not ready");
        return;
    } else {
        app_queue_ppp_ping_test();
    }
    app_queue_ppp_sms_test_batch();
}

static void app_queue_ppp_sms_test_batch(void)
{
    const size_t num_sms = 4U;
    struct tb45_sms_request sms_requests[num_sms];
    int ret = 0;
    (void)memset(sms_requests, 0, sizeof(sms_requests));

    uint32_t next_sms_id = (uint32_t)k_uptime_get_32();
    if (next_sms_id == 0U) {
        next_sms_id = 1U;
    }

    for (size_t i = 0U; i < num_sms; i++) {
        (void)snprintf(sms_requests[i].phone_number, sizeof(sms_requests[i].phone_number), "%s",
                       "+886939919942");
        sms_requests[i].message_id = next_sms_id++;
        if (next_sms_id == 0U) {
            next_sms_id = 1U;
        }
        (void)snprintf(sms_requests[i].message, sizeof(sms_requests[i].message),
                       "tb45_sms_send_enqueue_wait: id=%u batch_idx=%u TB45 SMS_SEND Test",
                       (unsigned int)sms_requests[i].message_id, (unsigned int)i);

        LOG_INF("PPP IPCP up detected: sending SMS now (idx=%u id=%u)",
                (unsigned int)i, sms_requests[i].message_id);

        /*
         * Previously this batch used `tb45_sms_send_enqueue_with_result_id()`, which is non-waiting.
         * Instead tb45_sms_send_enqueue_wait() is used here to make sure that each SMS fully finishes before the next one is sent.
        */
        ret = tb45_sms_send_enqueue_wait(&sms_requests[i]);
        if (ret == 0) {
            LOG_INF("PPP IPCP up detected: SMS send completed (idx=%u id=%u)",
                    (unsigned int)i, sms_requests[i].message_id);
        } else {
            LOG_ERR("PPP IPCP up detected: SMS send failed (idx=%u id=%u ret=%d)",
                    (unsigned int)i, sms_requests[i].message_id, ret);
        }
    }
}
#endif

static void app_queue_ppp_ping_test(void)
{
    int ret;
    const char *ping_host = "8.8.8.8";

    ret = tb45_ping_enqueue(ping_host, 0U, 0U);
    if (ret == 0) {
        LOG_INF("PPP IPCP up detected: PING request queued (%s)", ping_host);
    } else {
        LOG_ERR("PPP IPCP up detected: PING queue failed (%d)", ret);
    }
}

#if defined(CONFIG_APP_TB45_SMS_ENABLE) && CONFIG_APP_TB45_SMS_ENABLE
static void app_sms_send_event_handler(const struct tb45_sms_event *event)
{
    if (event == NULL) {
        return;
    }
    if (event->type == TB45_SMS_EVENT_TYPE_SEND_MSG_STATUS) {
        if (event->status == 0) {
            LOG_INF("[SMS_SND] send_ok id=%u phone=%s",
                    event->data.send.message_id, event->data.send.phone);
        } else {
            LOG_ERR("[SMS_SND] send_fail id=%u rc=%d phone=%s",
                    event->data.send.message_id, event->status, event->data.send.phone);
        }
    }
}

static void app_sms_receive_event_handler(const struct tb45_sms_event *event)
{
    if (event == NULL) {
        return;
    }
    if (event->type == TB45_SMS_EVENT_TYPE_RECEIVE_MSG_OUTPUT) {
        if (event->status == 0) {
            struct tb45_sms_rx_message message;
            int ret = tb45_sms_receive_read_index(event->data.receive.storage_index, &message);

            if (ret == 0) {
                LOG_INF("[%s]:<%s>:%s",
                        message.phone, message.timestamp, message.message);

                ret = tb45_sms_receive_delete_index(event->data.receive.storage_index);
                if (ret < 0) {
                    LOG_ERR("[SMS_RCV] delete enqueue failed idx=%u rc=%d",
                            event->data.receive.storage_index, ret);
                }
            } else {
                LOG_ERR("[SMS_RCV] read failed idx=%u rc=%d",
                        event->data.receive.storage_index, ret);
            }
        } else {
            LOG_ERR("[SMS_RCV] failed idx=%u rc=%d", event->data.receive.storage_index, event->status);
        }
    }
}

static void app_sms_event_dispatch(const struct tb45_sms_event *event, void *user_data)
{
    ARG_UNUSED(user_data);
    app_sms_send_event_handler(event);
    app_sms_receive_event_handler(event);
}

#endif

/* This part belongs to CELLULAR - END: functions */

static void handle_ipv4_result(struct net_if *iface)
{
    if ((iface == NULL) || (iface->config.ip.ipv4 == NULL)) {
        return;
    }

    int i = 0;
    for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++)
    {
        char buf[NET_IPV4_ADDR_LEN];

        LOG_INF("IPv4 address: %s\n",
            net_addr_ntop(AF_INET,
                            &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
                            buf, sizeof(buf)));
        LOG_INF("Subnet: %s\n",
            net_addr_ntop(AF_INET,
                            &iface->config.ip.ipv4->unicast[i].netmask,
                            buf, sizeof(buf)));
        LOG_INF("Router: %s\n",
            net_addr_ntop(AF_INET,
                            &iface->config.ip.ipv4->gw,
                            buf, sizeof(buf)));
        
        if (iface == get_ethernet_iface()) {
            LOG_INF("Ethernet Network connectivity up!");
            struct interface_set_params msg = { .interface = 1, .state = true };
            k_msgq_put(&interface_set_msgq, &msg, K_NO_WAIT);
            k_work_submit_to_queue(&low_priority_wq, &interface_set_work);
            net_if_ready = true;
        }

        if (iface == get_ppp_iface()) {
            LOG_INF("PPP IPCP Interface is UP!");
            struct interface_set_params msg = { .interface = 3, .state = true };
            (void)k_msgq_put(&interface_set_msgq, &msg, K_NO_WAIT);
            k_work_submit_to_queue(&low_priority_wq, &interface_set_work);
            ppp_if_ready = true;
        }
    }
}        

static void net_evt_handler(struct net_mgmt_event_callback *cb,
        uint64_t mgmt_event, struct net_if *iface)
{
    LOG_DBG("Network event handler called with event %llu",
            static_cast<unsigned long long>(mgmt_event));
    switch (mgmt_event) {
    case NET_EVENT_L4_CONNECTED:
        handle_ipv4_result(iface);
        break;
    case NET_EVENT_IPV4_ADDR_ADD:
        handle_ipv4_result(iface);
        break;
    case NET_EVENT_IF_UP:
        LOG_DBG("Interface %d is up", net_if_get_by_iface(iface));
        break;
    case NET_EVENT_IF_DOWN:
        {
            LOG_DBG("Interface %d is down", net_if_get_by_iface(iface));
            int interface_num = 0;
            if (iface == get_ethernet_iface()) {
                interface_num = 1;
            } else if (iface == net_if_get_first_wifi()) {
                interface_num = 2;
            } else if (iface == get_ppp_iface()) {
                interface_num = 3;
                ppp_if_ready = false;
            }

            if (interface_num > 0) {
                struct interface_set_params msg = { .interface = interface_num, .state = false };
                k_msgq_put(&interface_set_msgq, &msg, K_NO_WAIT);
                k_work_submit_to_queue(&mid_priority_wq, &interface_set_work);
            }
        }
        break;
    case NET_EVENT_L4_DISCONNECTED:
        LOG_DBG("Network connectivity down!");
        break;
    default:
        break;
    }
}

static void button_input_cb(struct input_event *evt, void *user_data)
{
    if (evt->sync == 0) {
        return;
    }
    if (evt->code == INPUT_KEY_MENU && evt->value == 1) {
        k_work_submit_to_queue(&mid_priority_wq, &mdbutton_pressed_work);
    } else if (evt->code == INPUT_KEY_MENU && evt->value == 0) {
        k_work_submit_to_queue(&mid_priority_wq, &mdbutton_released_work);
    }
}

INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);

void mdbutton_pressed_handler(struct k_work *work) {
    
}

void mdbutton_released_handler(struct k_work *work) {
    ARG_UNUSED(work);
// #if defined(CONFIG_APP_TB45_SMS_ENABLE) && CONFIG_APP_TB45_SMS_ENABLE
//     app_sms_send_and_ping_test();
//     app_sms_recover_stored_unread_messages();
// #else
//     if (ppp_if_ready) {
//         app_queue_ppp_ping_test();
//     }
// #endif
}

void mdbutton_triggered(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    int val = gpio_pin_get_dt(&mdbutton);
    if (val == GPIO_ACTIVE_HIGH) {
        if (k_work_busy_get(&mdbutton_released_work) == 0) {
            k_work_submit_to_queue(&mid_priority_wq, &mdbutton_released_work);
        }
    } else {
        if (k_work_busy_get(&mdbutton_pressed_work) == 0) {
            k_work_submit_to_queue(&mid_priority_wq, &mdbutton_pressed_work);
        }
    }
}

void vars_work_handler(struct k_work *work)
{
    // Periodic vars work logic
}
K_WORK_DEFINE(vars_work, vars_work_handler);

void vars_timer_handler(struct k_timer *dummy)
{
    k_work_submit_to_queue(&vars_workq, &vars_work);
}
K_TIMER_DEFINE(vars_timer, vars_timer_handler, NULL);

void var_T01_S1_update() {
    
}

void var_T01_S1_update_completed(int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    T01_S1 = value;
}

void var_T01_S1_set(int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    T01_S1 = value;
}

void var_T01_S5_update() {
    
}

void var_T01_S5_update_completed(int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    T01_S5 = value;
}

void var_T01_S5_set(int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    T01_S5 = value;
}

static void do_sntp(int family) {
    const char *family_str = family == AF_INET ? "IPv4" : "IPv6";
    struct sntp_time s_time;
    struct sntp_ctx ctx;
    struct sockaddr_in addr_in = {0};
    socklen_t addrlen = sizeof(addr_in);
    int rv;

    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(123);
    rv = zsock_inet_pton(AF_INET, "216.239.35.0", &addr_in.sin_addr);

    rv = sntp_init(&ctx, (struct sockaddr *)&addr_in, addrlen);
    if (rv < 0) {
        LOG_ERR("Failed to init SNTP %s ctx: %d", family_str, rv);
        return;
    }

#if SNTP_DEBUG_PRINT
    LOG_INF("Sending SNTP %s request...", family_str);
#endif
    rv = sntp_query(&ctx, 4 * MSEC_PER_SEC, &s_time);
    if (rv < 0) {
        LOG_ERR("SNTP %s request failed: %d", family_str, rv);
        sntp_close(&ctx);
        return;
    }

    sntp_close(&ctx);

    {
        struct timespec ts;
        ts.tv_sec = s_time.seconds;
        ts.tv_nsec = (uint32_t)((uint64_t)s_time.fraction * 1000000000ULL >> 32);
        int ret = sys_clock_settime(CLOCK_REALTIME, &ts);
        if (ret < 0) {
            LOG_ERR("Failed to set time: %d", ret);
        }
    }
}

SYS_INIT(preinit, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int main(void)
{
    work_queues_init();
    socket_mgr_init();

    /* This part belongs to CELLULAR - START: initialization */
    tb45_cellular_init(&cellular_cfg);

    /* Added for Cellular - SMS */
#if defined(CONFIG_APP_TB45_SMS_ENABLE) && CONFIG_APP_TB45_SMS_ENABLE
    (void)tb45_sms_event_init();
    (void)tb45_sms_event_set_callback(app_sms_event_dispatch, NULL);
#endif

    /* This part belongs to CELLULAR - END: initialization */

    net_mgmt_init_event_callback(&net_l4_mgmt_cb, &net_evt_handler, NET_L4_EVENT_MASK);
    net_mgmt_add_event_callback(&net_l4_mgmt_cb);
    
    net_mgmt_init_event_callback(&ipv4_cb, &net_evt_handler, NET_IPV4_EVENT_MASK);
    net_mgmt_add_event_callback(&ipv4_cb);
    
    net_mgmt_init_event_callback(&net_if_mgmt_cb, &net_evt_handler, NET_IF_EVENT_MASK);
    net_mgmt_add_event_callback(&net_if_mgmt_cb);

    {                
        /* Initialize UDP firmware loader thread */
        int ret = udp_fwloader_init(socket_mgr_register);
        if (ret) {
            LOG_ERR("Failed to initialize UDP firmware loader: %d", ret);
        } else {
            LOG_DBG("UDP firmware loader initialized");
        }
    }
        
    log_udp_set_work_queue(&low_priority_wq);
    gpio_pin_configure_dt(&green_led_gpio, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure_dt(&red_led_gpio, GPIO_OUTPUT_ACTIVE);
    
    gpio_pin_set_dt(&green_led_gpio, 0);
    gpio_pin_set_dt(&red_led_gpio, 1);
    
    tz_offset_minutes = get_timezone_offset((en_td_timezones)atoi("0"));
    
    gpio_pin_configure_dt(&mdbutton, GPIO_INPUT);
    if (gpio_pin_interrupt_configure_dt(&mdbutton, GPIO_INT_EDGE_BOTH) != 0) {
        printk("failed to configure interrupt on %s pin %d\n", mdbutton.port->name, mdbutton.pin);
    }
    gpio_init_callback(&mdbutton_cb_data, mdbutton_triggered, BIT(mdbutton.pin));
    gpio_add_callback(mdbutton.port, &mdbutton_cb_data);
    
#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
    int ret = gdb_stub_init();
    if (ret) {
        LOG_ERR("Failed to initialize GDB stub: %d", ret);
    }
    gdb_tcp_server_start();
#endif
                    
    k_work_init(&mdbutton_pressed_work, mdbutton_pressed_handler);
    k_work_init(&mdbutton_released_work, mdbutton_released_handler);
#if defined(CONFIG_NET_DHCPV4)
    dhcp_enabled = true;
#else
    dhcp_enabled = false;
#endif
    
    struct net_if *iface = get_ethernet_iface();
    if (iface == NULL) {
        LOG_ERR("No Ethernet interface found");
    } else {
        net_if_set_default(iface);
    
        struct in_addr addr4, netmask4;
        net_addr_pton(AF_INET, string("192.168.1.1").c_str(), &addr4);
        net_if_ipv4_set_gw(iface, &addr4);
    
        net_addr_pton(AF_INET, string("192.168.1.101").c_str(), &addr4);
        net_if_ipv4_addr_add(iface, &addr4, NET_ADDR_OVERRIDABLE, 0);
    
        net_addr_pton(AF_INET, string("255.255.255.0").c_str(), &netmask4);
        net_if_ipv4_set_netmask_by_addr(iface, &addr4, &netmask4);
#if defined(CONFIG_NET_DHCPV4)
        if (dhcp_enabled) {
            net_dhcpv4_start(iface);
        } else {
            net_dhcpv4_stop(iface);
        }
#else
        LOG_INF("DHCPv4 disabled via CONFIG_NET_DHCPV4=n");
#endif
    }
    
    k_work_queue_init(&vars_workq);
    k_work_queue_start(&vars_workq, vars_workq_stack,
                        K_THREAD_STACK_SIZEOF(vars_workq_stack),
                        VARS_WORKQ_PRIORITY, NULL);
    k_thread_name_set(&vars_workq.thread, "vars_workq");
    
    k_timer_start(&vars_timer, K_MSEC(1000), K_MSEC(1000));

    /* Initialize LED pattern handler */
    int led_ret = led_pattern_init(&low_priority_wq);
    if (led_ret) {
        LOG_ERR("Failed to initialize LED pattern handler: %d", led_ret);
    } else {
        LOG_DBG("LED pattern handler initialized");
        led_pattern_set("B-B-B-");
    }
    
    /* Initialize blue LED bar */
    int ss_ret = signal_strength_init();
    if (ss_ret) {
        LOG_ERR("Failed to initialize signal strength: %d", ss_ret);
    } else {
        LOG_DBG("Blue LED bar initialized");
    }
    
    /* Initialize buzzer subsystem */
    int beep_ret = beep_init(&low_priority_wq, NULL);
    if (beep_ret) {
        LOG_ERR("Failed to initialize buzzer: %d", beep_ret);
    }

    while (1) {
        k_sleep(K_MSEC(main_tick_interval));
    }
    
    return 0;
}