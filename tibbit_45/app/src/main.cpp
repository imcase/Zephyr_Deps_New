#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <errno.h>

#include <zephyr/net/socket.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include "socket_mgr.h"


#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/reboot.h>
#include "work_queues.h"


#if defined(CONFIG_ARMV7_M_ARMV8_M_MAINLINE)
#include <cmsis_core.h>   /* SCB->CFSR/HFSR/MMFAR/BFAR in the fatal handler */
#endif

#if defined(CONFIG_WATCHDOG)             
    #include <zephyr/drivers/watchdog.h>
#endif

#include <zephyr/drivers/gpio.h>


#include "udp_fwloader.h"
#include "led_pattern.h"
#ifdef CONFIG_LOG_BACKEND_UDP
#include "log_backend.h"
#endif

#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
#include "gdb_stub.h"
#include "gdb_taiko.h"
#endif

#include <zephyr/input/input.h>

#include "beep.h"

#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>


#if defined(CONFIG_RTC)
#include <zephyr/drivers/rtc.h>
extern "C" time_t timeutil_timegm(const struct tm *tm);
#endif


#include <time.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket_service.h>


#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
#include "etl/string.h"
#include "etl/to_string.h"
#include "utils.h"
#include "system.h"
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


int main_tick_interval = 1000;

bool net_if_ready = false;
typedef etl::string<255> string;


#define NET_L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define NET_IPV4_EVENT_MASK (NET_EVENT_IPV4_ADDR_ADD)
#define NET_IF_EVENT_MASK (NET_EVENT_IF_UP | NET_EVENT_IF_DOWN)
    

#if defined(CONFIG_WATCHDOG)
/* Independent hardware watchdog (IWDG). The main loop must call wdt_feed()
 * at least every WDT_TIMEOUT_MS or the SoC resets. The timeout is set well
 * above main_tick_interval (1 s) so a healthy loop always has margin, while
 * a hung main thread triggers a reboot. */
#define WDT_TIMEOUT_MS 10000
static const struct device *const wdt_dev = DEVICE_DT_GET(DT_NODELABEL(iwdg));
static int wdt_channel_id = -1;

static void watchdog_init(void)
{
    if (!device_is_ready(wdt_dev)) {
        LOG_ERR("Watchdog device not ready; running without watchdog");
        return;
    }

    struct wdt_timeout_cfg wdt_cfg = {
        .window = { .min = 0U, .max = WDT_TIMEOUT_MS },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };

    wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_cfg);
    if (wdt_channel_id < 0) {
        LOG_ERR("wdt_install_timeout failed: %d", wdt_channel_id);
        return;
    }

    /* WDT_OPT_PAUSE_HALTED_BY_DBG so the IWDG stops while halted under a
     * debugger instead of resetting mid-breakpoint. */
    int rc = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (rc < 0) {
        LOG_ERR("wdt_setup failed: %d", rc);
        wdt_channel_id = -1;
        return;
    }

    //LOG_INF("Watchdog enabled (timeout %d ms)", WDT_TIMEOUT_MS);
}

static inline void watchdog_feed(void)
{
    if (wdt_channel_id >= 0) {
        wdt_feed(wdt_dev, wdt_channel_id);
    }
}
#endif    

#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
/* Debugger keepalive: while halted at a breakpoint the main loop and work
 * queues are frozen, so nothing feeds the IWDG or the task-watchdog channels
 * and the board would reset mid-session. Timers keep running (SysTick), so
 * this timer feeds every watchdog - but ONLY while halted, leaving watchdog
 * protection fully effective in normal operation. WDT_OPT_PAUSE_HALTED_BY_DBG
 * cannot help here: monitor-mode debugging never halts the core. */
static void debug_keepalive_tick(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /* gdb_stub_keepalive_tick() returns true only while halted under a live
     * debug session; it also auto-recovers abandoned sessions and, if that
     * fails, returns false so the watchdogs reset the device. */
    if (gdb_stub_keepalive_tick()) {
        #if defined(CONFIG_WATCHDOG)
            watchdog_feed();
        #endif
        work_queues_wdt_feed_all();
    }
}
K_TIMER_DEFINE(debug_keepalive_timer, debug_keepalive_tick, NULL);
#endif    



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


static K_SEM_DEFINE(init_done_sem, 0, 1);
static struct k_work_delayable init_work;


struct interface_set_params {
    int interface;
    bool state;
};

// Work item for interface_set
K_MSGQ_DEFINE(interface_set_msgq, sizeof(struct interface_set_params), 8, 4);
static struct k_work interface_set_work;
static struct k_work input_event_work;

/* Generic deferred-work message. The HTTP handler runs on the 4 KB HTTP-server
 * thread, which is too small for the cmd_* -> fsdb -> LittleFS path (it
 * overflowed and corrupted a return address -> PC=NULL crash). So the handler
 * now just copies the request into one of these and submits it to
 * low_priority_wq; the heavy work runs on that worker thread instead. */
enum app_cmd_kind {
    APP_CMD_INVOKE,    /* run a named cmd_* handler with a value string */
    APP_CMD_VAR_SET,   /* set an application variable */
};

struct var_set_params {
    char varname[64];
    char varvalue[256];
};

struct invoke_params {
    char name[24];         /* "ADD", "LOOKUP_LESS", ... */
    char data[256];        /* value payload (matches param_buf) */
};

struct app_cmd_msg {
    uint8_t kind;                  /* enum app_cmd_kind */
    union {
        struct invoke_params invoke;
        struct var_set_params var_set;
    } u;
};

/* Depth 4 is plenty for HTTP-driven commands; ~324 B/slot. */
K_MSGQ_DEFINE(app_cmd_msgq, sizeof(struct app_cmd_msg), 4, 4);
static struct k_work app_cmd_work;
static void app_cmd_work_handler(struct k_work *work);





int boot_reason = BOOT_REBOOT_NONE;

static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback net_l4_mgmt_cb;
static struct net_mgmt_event_callback net_if_mgmt_cb;


int32_t tz_offset_minutes = 0;

struct input_event_msg {
    uint16_t type;
    uint16_t code;
    int32_t value;
    uint8_t sync;
};

K_MSGQ_DEFINE(input_event_msgq, sizeof(struct input_event_msg), 16, 4);


const char *device_id = "TPP2W(G2)";
const char *app_name = "MyDevice___1.0.0";
const char *app_version = "1.0.0";
const char *app_id = "6a57364dcc438a4ac5fb3133";



static const struct gpio_dt_spec mdbutton =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(mdbutton), gpios, {0});
                    

static struct gpio_callback mdbutton_cb_data;
struct k_work mdbutton_pressed_work;
struct k_work mdbutton_released_work;
            

static bool dhcp_enabled = false;



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

#if defined(CONFIG_RTC)        
static void rtc_time_save(time_t secs);
static bool rtc_time_restore(void);
#endif


static void do_sntp(int family);



static void app_cmd_work_handler(struct k_work *work)
{
    struct app_cmd_msg msg;
    while (k_msgq_get(&app_cmd_msgq, &msg, K_NO_WAIT) == 0) {
        switch (msg.kind) {
        case APP_CMD_INVOKE:
            
            break;
        case APP_CMD_VAR_SET:
            
            break;
        
        default:
            LOG_WRN("app_cmd: bad kind %u", msg.kind);
            break;
        }
    }
}

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
                char buf[NET_IPV4_ADDR_LEN];
                
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

/* Captured PC/LR are runtime addresses in the STM32 flash alias at 0x00000000;
 * the .elf links code at 0x08000000, so add this to turn a captured value into
 * an address addr2line understands. Values already at 0x08xxxxxx (no alias) are
 * left as-is by crash_addr_fixup(). */
#define CRASH_FLASH_BASE 0x08000000u

static inline uint32_t crash_addr_fixup(uint32_t addr)
{
    addr &= ~1u;  /* clear Thumb bit */
    if (addr != 0u && addr < CRASH_FLASH_BASE) {
        addr += CRASH_FLASH_BASE;
    }
    return addr;
}

/* Print the crash context saved by the previous boot's fatal handler. Gated on
 * a valid boot-state cell whose last reason was FATAL, so a cold power cycle
 * (invalid magic) or a non-fatal reboot prints nothing. Call once at startup,
 * before boot_state_mark_healthy() clears the reason. */
static void crash_info_report(void)
{
    if (boot_state_last_reason() != BOOT_REBOOT_FATAL) {
        return;
    }
    uint32_t pc = *crash_pc;
    uint32_t lr = *crash_lr;

    printk("=== Fatal error on previous boot ===\n");
    if (pc == 0u && lr == 0u) {
        printk("  (no exception stack frame was captured)\n");
        return;
    }

    if (pc == 0u) {
        printk("  PC=0x00000000 (branched to NULL - corrupt/NULL function pointer)\n");
    } else {
        printk("  PC -> 0x%08x  (raw 0x%08x)\n", crash_addr_fixup(pc), pc);
    }
    printk("  LR -> 0x%08x  (raw 0x%08x, caller/return address)\n",
           crash_addr_fixup(lr), lr);

    /* Copy-paste-ready: resolve to file:line with the matching .elf. */
    if (pc != 0u) {
        printk("  addr2line -e zephyr.elf -f -p -i 0x%08x 0x%08x\n",
               crash_addr_fixup(pc), crash_addr_fixup(lr));
    } else {
        printk("  addr2line -e zephyr.elf -f -p -i 0x%08x\n", crash_addr_fixup(lr));
    }
}

static void boot_state_mark_healthy(void)
{
    *boot_state_magic  = BOOT_STATE_MAGIC;
    *boot_state_reason = (uint32_t)BOOT_REBOOT_NONE;
}

void k_sys_fatal_error_handler(unsigned int reason,
                               const struct arch_esf *esf)
{
    printk("Fatal error occurred! reason=%u\n", reason);

    /* Optional: print register state if available */
#if defined(CONFIG_ARCH_HAS_EXCEPTION_ESF)
    if (esf) {
        printk("Exception frame available\n");
    }
#endif

    /* small delay so logs flush */
    k_busy_wait(100000);

    /* Routes through the shared boot-state cell: if the previous boot also
     * rebooted for FATAL, this halts instead of looping. The normal reboot
     * path is fault-safe (memory writes + sys_reboot); the loop-halt path
     * calls into the LED subsystem which may or may not respond from fault
     * context - silent halt is acceptable, infinite reboot loop is not. */
    boot_state_reboot(BOOT_REBOOT_FATAL);
}


static int preinit(void)
{
    
    
    work_queues_init();
    k_work_init(&interface_set_work, interface_set_work_handler);
    
    
    #if defined(CONFIG_WATCHDOG)                    
        watchdog_init();
    #endif
    
    
    {                
        /* Initialize UDP firmware loader thread */
        int ret = udp_fwloader_init();
        if (ret) {
            LOG_ERR("Failed to initialize UDP firmware loader: %d", ret);
        } else {
            LOG_DBG("UDP firmware loader initialized");
        }
    }
    
        
    
    // initialize ethernet
    
    #ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
    {
        int ret = gdb_stub_init();
        if (ret) {
            LOG_ERR("Failed to initialize GDB stub: %d", ret);
        }
        gdb_taiko_init();
    
        /* Feed watchdogs while halted under the debugger (1 s period is
            * plenty against the 10 s IWDG and 18 s task-wdt timeouts) */
        k_timer_start(&debug_keepalive_timer, K_MSEC(1000), K_MSEC(1000));
    }
    
    /* Hold startup here (before boot() runs) so a debugger can attach and
        * place breakpoints in boot() and everything main() drives afterwards.
        * The l2 transport is already listening, so the device is reachable over
        * the Taiko tunnel even without an IP.
        *
        * The key subtlety: we must keep calling gdb_stub_poll() and NOT return
        * the moment a packet arrives. GDB's attach ('?') asks for a cooperative
        * halt that only a gdb_stub_poll() caller can service. If we bailed out
        * as soon as the debugger was "seen", that halt would instead be serviced
        * later from the main loop - after boot() had already executed - and any
        * breakpoint set in boot() would be missed. So we stay here, let the
        * attach halt us in preinit context, and only proceed once GDB has halted
        * us and then continued (or the attach window lapses with no debugger).
        * Waits in slices so the watchdog stays fed while blocked. */
    {
        LOG_INF("Waiting up to 8 s for GDB debugger");
        int64_t deadline = k_uptime_get() + 8000;
        bool was_halted = false;
        while (k_uptime_get() < deadline) {
            #if defined(CONFIG_WATCHDOG)
            watchdog_feed();
            #endif
            /* Service an attach-time halt here so 'target remote' during the
                * boot hold stops the target in preinit with real registers. */
            gdb_stub_poll();
    
            if (gdb_stub_is_halted()) {
                /* Debugger is in control (placing breakpoints / inspecting).
                    * While halted this thread is actually blocked in the debug
                    * monitor, not spinning here; the keepalive timer feeds the
                    * watchdogs and auto-resumes an abandoned session. */
                was_halted = true;
            } else if (was_halted) {
                /* GDB halted us and has now continued - stop holding so
                    * boot() runs and the breakpoints placed in it are hit. */
                break;
            }
            k_sleep(K_MSEC(50));
        }
        LOG_INF("GDB debugger %s; continuing boot",
                gdb_taiko_debugger_connected() ? "attached" : "not connected");
    }
    #endif
    
    
    k_work_init(&app_cmd_work, app_cmd_work_handler);
    
    
    #if defined(CONFIG_RTC)
    /* Seed wall-clock time from the RTC as early as possible so boot-time log
        * entries are timestamped correctly (warm resets only; see rtc_time_*). */
    rtc_time_restore();
    #endif
    
    return 0;
}


static struct net_if *get_ethernet_iface(void)
{
    struct net_if *tmp;
    for (int i = 1; (tmp = net_if_get_by_index(i)) != NULL; i++) {
    #if CONFIG_NET_L2_ETHERNET
        if (net_if_l2(tmp) == &NET_L2_GET_NAME(ETHERNET) &&
            !net_if_is_wifi(tmp)) {
            return tmp;
        }
    #endif
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
            // if (dhcp_enabled) {
            //     if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type != NET_ADDR_DHCP)
            //     {
            //         continue;
            //     }
            // }
            LOG_INF("Ethernet Network connectivity up!");
            struct interface_set_params msg = { .interface = 1, .state = true };
            k_msgq_put(&interface_set_msgq, &msg, K_NO_WAIT);
            k_work_submit_to_queue(&mid_priority_wq, &interface_set_work);
            net_if_ready = true;
        }


/* This part belongs to CELLULAR - START: ppp_iface */
        if (iface == get_ppp_iface()) {
            LOG_INF("PPP IPCP Interface is UP!");
            struct interface_set_params msg = { .interface = 3, .state = true };
            (void)k_msgq_put(&interface_set_msgq, &msg, K_NO_WAIT);
            k_work_submit_to_queue(&low_priority_wq, &interface_set_work);
            ppp_if_ready = true;
        }
/* This part belongs to CELLULAR - END: ppp_iface */
    }
}        

static void net_evt_handler(struct net_mgmt_event_callback *cb,
        uint64_t mgmt_event, struct net_if *iface)
{
    LOG_DBG("Network event handler called with event %d", mgmt_event);
    switch (mgmt_event) {
    case NET_EVENT_L4_CONNECTED:
        //k_sem_give(&net_conn_sem);
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

/* This part belongs to CELLULAR - START: ppp_iface */
            } else if (iface == get_ppp_iface()) {
                interface_num = 3;
                ppp_if_ready = false;
            }
/* This part belongs to CELLULAR - END: ppp_iface */

            struct interface_set_params msg = { .interface = interface_num, .state = false };
            k_msgq_put(&interface_set_msgq, &msg, K_NO_WAIT);
            k_work_submit_to_queue(&mid_priority_wq, &interface_set_work);
        }
        break;

    case NET_EVENT_L4_DISCONNECTED:
        LOG_DBG("Network connectivity down!");
        break;
    default:
        break;
    }
}


static void input_event_work_handler(struct k_work *work)
{
    struct input_event_msg msg;
    struct input_event evt;

    while (k_msgq_get(&input_event_msgq, &msg, K_NO_WAIT) == 0) {
        evt.type = msg.type;
        evt.code = msg.code;
        evt.value = msg.value;
        evt.sync = msg.sync;

        
        
        // md button
        if (evt.code == INPUT_KEY_MENU && evt.value == 1) {
            k_work_submit_to_queue(&mid_priority_wq, &mdbutton_pressed_work);
        } else if (evt.code == INPUT_KEY_MENU && evt.value == 0) {
            k_work_submit_to_queue(&mid_priority_wq, &mdbutton_released_work);
        }
    }
}

static void tps_input_cb(struct input_event *evt, void *user_data)
{
    struct input_event_msg msg = {
        .type = evt->type,
        .code = evt->code,
        .value = evt->value,
        .sync = evt->sync,
    };
    k_msgq_put(&input_event_msgq, &msg, K_NO_WAIT);
    k_work_submit_to_queue(&mid_priority_wq, &input_event_work);
}

static void input_cb(struct input_event *evt, void *user_data)
{
    struct input_event_msg msg = {
        .type = evt->type,
        .code = evt->code,
        .value = evt->value,
        .sync = evt->sync,
    };
    k_msgq_put(&input_event_msgq, &msg, K_NO_WAIT);
    k_work_submit_to_queue(&mid_priority_wq, &input_event_work);
}


INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(tps_buttons)), tps_input_cb, NULL);                
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(gpio_buttons)), input_cb, NULL);


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


#if defined(CONFIG_RTC)
static const struct device *const rtc_dev = DEVICE_DT_GET(DT_NODELABEL(rtc));

/* Earliest time we trust the RTC to hold (2024-01-01 00:00:00 UTC). A value
 * below this means the RTC was never set (cold boot) and should be ignored. */
#define RTC_TIME_SANITY_EPOCH   1704067200LL

/* Persist the current UTC wall-clock to the RTC after an SNTP sync. */
static void rtc_time_save(time_t secs)
{
    if (!device_is_ready(rtc_dev)) {
        return;
    }

    struct tm tm_val;
    gmtime_r(&secs, &tm_val);

    struct rtc_time rt = {0};
    rt.tm_sec   = tm_val.tm_sec;
    rt.tm_min   = tm_val.tm_min;
    rt.tm_hour  = tm_val.tm_hour;
    rt.tm_mday  = tm_val.tm_mday;
    rt.tm_mon   = tm_val.tm_mon;
    rt.tm_year  = tm_val.tm_year;
    rt.tm_wday  = tm_val.tm_wday;
    rt.tm_yday  = tm_val.tm_yday;
    rt.tm_isdst = -1;
    rt.tm_nsec  = 0;

    int ret = rtc_set_time(rtc_dev, &rt);
    if (ret < 0) {
        LOG_WRN("Failed to write RTC: %d", ret);
    }
}

/* Seed CLOCK_REALTIME from the RTC at boot. Returns true if a plausible time
 * was restored. */
static bool rtc_time_restore(void)
{
    if (!device_is_ready(rtc_dev)) {
        LOG_WRN("RTC not ready; time starts at epoch until SNTP");
        return false;
    }

    struct rtc_time rt;
    int ret = rtc_get_time(rtc_dev, &rt);
    if (ret < 0) {
        /* -ENODATA: RTC was never set (cold boot / power loss). */
        LOG_INF("RTC has no valid time (%d); waiting for SNTP", ret);
        return false;
    }

    struct tm tm_val = {0};
    tm_val.tm_sec   = rt.tm_sec;
    tm_val.tm_min   = rt.tm_min;
    tm_val.tm_hour  = rt.tm_hour;
    tm_val.tm_mday  = rt.tm_mday;
    tm_val.tm_mon   = rt.tm_mon;
    tm_val.tm_year  = rt.tm_year;
    tm_val.tm_wday  = rt.tm_wday;
    tm_val.tm_yday  = rt.tm_yday;
    tm_val.tm_isdst = -1;

    time_t secs = timeutil_timegm(&tm_val);
    if ((long long)secs < RTC_TIME_SANITY_EPOCH) {
        LOG_WRN("RTC time implausible (%lld); ignoring", (long long)secs);
        return false;
    }

    struct timespec ts = { .tv_sec = secs, .tv_nsec = 0 };
    if (sys_clock_settime(CLOCK_REALTIME, &ts) < 0) {
        LOG_WRN("Failed to seed clock from RTC");
        return false;
    }

    //LOG_INF("Restored time from RTC: %lld", (long long)secs);
    return true;
}
#endif


static void do_sntp(int family) {
  const char *family_str = family == AF_INET ? "IPv4" : "IPv6";
  struct sntp_time s_time;
  struct sntp_ctx ctx;
  struct sockaddr_in addr_in = {0};
  socklen_t addrlen = sizeof(addr_in);
  int rv;

  /* Use static IP address */
  addr_in.sin_family = AF_INET;
  addr_in.sin_port = htons(123);
  rv = zsock_inet_pton(AF_INET, "216.239.35.0",
                 &addr_in.sin_addr);

  rv = sntp_init(&ctx, (struct sockaddr *)&addr_in, addrlen);
  if (rv < 0) {
    LOG_ERR("Failed to init SNTP %s ctx: %d", family_str, rv);
    goto end;
  }

#if SNTP_DEBUG_PRINT
  LOG_INF("Sending SNTP %s request...", family_str);
#endif
  rv = sntp_query(&ctx, 4 * MSEC_PER_SEC, &s_time);
  if (rv < 0) {
    LOG_ERR("SNTP %s request failed: %d", family_str, rv);
    goto end;
  }
#if SNTP_DEBUG_PRINT
  LOG_INF("SNTP Time: %llu", s_time.seconds);
#endif

  sntp_close(&ctx);

  {
    struct timespec ts;
    ts.tv_sec = s_time.seconds;
    ts.tv_nsec = (uint32_t)((uint64_t)s_time.fraction * 1000000000ULL >> 32);
    int ret = sys_clock_settime(CLOCK_REALTIME, &ts);
    if (ret < 0) {
      LOG_ERR("Failed to set time: %d", ret);
    } else {
#if SNTP_DEBUG_PRINT
      LOG_INF("Time set successfully");
#endif
    }
    // get time
    time_t now = time(NULL);
    struct tm *tm = localtime_tz(&now);
#if SNTP_DEBUG_PRINT
    LOG_INF("Time: %d-%02d-%02d %02d:%02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif
  }

#if defined(CONFIG_RTC)
  /* Persist to the RTC so the time survives a reboot/watchdog reset. */
  rtc_time_save((time_t)s_time.seconds);
#endif

end:
  sntp_close(&ctx);
}




SYS_INIT(preinit, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);



static void boot() {
	    
    boot_reason = boot_state_last_reason();            
    LOG_INF("Boot (previous reason: %s)",
            boot_reason_str(boot_state_last_reason()));
    crash_info_report();
    
    
    if (boot_state_last_reason() == BOOT_REBOOT_WATCHDOG) {
        *boot_state_reason = (uint32_t)BOOT_REBOOT_NONE;
        // last boot was not healthy, prevent boot loop and allow device to be accessible via udp fwloader
        while (1) {
            k_sleep(K_SECONDS(1));
        }
    }
    
    
    *boot_state_reason = (uint32_t)BOOT_REBOOT_WATCHDOG;
    
    
    socket_mgr_init();

    net_mgmt_init_event_callback(&net_l4_mgmt_cb, &net_evt_handler, NET_L4_EVENT_MASK);
    net_mgmt_add_event_callback(&net_l4_mgmt_cb);
    
    net_mgmt_init_event_callback(&ipv4_cb, &net_evt_handler, NET_IPV4_EVENT_MASK);
    net_mgmt_add_event_callback(&ipv4_cb);
    
    net_mgmt_init_event_callback(&net_if_mgmt_cb, &net_evt_handler, NET_IF_EVENT_MASK);
    net_mgmt_add_event_callback(&net_if_mgmt_cb);
        
        
    
    tz_offset_minutes = get_timezone_offset((en_td_timezones)atoi("0"));
    #ifdef CONFIG_CRON
    cron_set_utc_offset(tz_offset_minutes);
    #endif
    
    
    
    log_udp_set_work_queue(&low_priority_wq);
    gpio_pin_configure_dt(&green_led_gpio, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure_dt(&red_led_gpio, GPIO_OUTPUT_ACTIVE);
    
    gpio_pin_set_dt(&green_led_gpio, 0);
    gpio_pin_set_dt(&red_led_gpio, 1);
    
    
    
    k_work_init(&input_event_work, input_event_work_handler);                
    k_work_init(&mdbutton_pressed_work, mdbutton_pressed_handler);
    k_work_init(&mdbutton_released_work, mdbutton_released_handler);
    
    dhcp_enabled = 1 == 1;
    
    struct net_if *iface = get_ethernet_iface();
    
    if (iface == NULL) {
        LOG_ERR("No Ethernet interface found");
    } else {
    
        net_if_set_default(iface);
    
        struct in_addr addr4, netmask4;
        net_addr_pton(AF_INET, string("1.0.0.1").c_str(), &addr4);
        net_if_ipv4_set_gw(iface, &addr4);
    
        net_addr_pton(AF_INET, string("1.0.0.1").c_str(), &addr4);
        net_if_ipv4_addr_add(iface, &addr4, NET_ADDR_OVERRIDABLE, 0);
    
        net_addr_pton(AF_INET, string("255.255.255.0").c_str(), &netmask4);
        net_if_ipv4_set_netmask_by_addr(iface, &addr4, &netmask4);
    
        if (dhcp_enabled) {
            net_dhcpv4_start(iface);
        } else {
            net_dhcpv4_stop(iface);
        }
    }
    


	    
    /* Initialize LED pattern handler */
    int led_ret = led_pattern_init(&low_priority_wq);
    if (led_ret) {
        LOG_ERR("Failed to initialize LED pattern handler: %d", led_ret);
    } else {
        LOG_DBG("LED pattern handler initialized");
        /* Start with a simple alternating pattern as an example */
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

}

static void init_work_handler(struct k_work *work) {
	boot();	
	k_sem_give(&init_done_sem);
}

int main(void)
{
/* This part belongs to CELLULAR - START: initialization */
    tb45_cellular_init(&cellular_cfg);

    /* Added for Cellular - SMS */
#if defined(CONFIG_APP_TB45_SMS_ENABLE) && CONFIG_APP_TB45_SMS_ENABLE
    (void)tb45_sms_event_init();
    (void)tb45_sms_event_set_callback(app_sms_event_dispatch, NULL);
#endif
/* This part belongs to CELLULAR - END: initialization */


	k_work_init_delayable(&init_work, init_work_handler);
	k_work_schedule_for_queue(&mid_priority_wq, &init_work, K_MSEC(1));

	k_sem_take(&init_done_sem, K_FOREVER);

	#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
        /* Freeze the main thread too while halted under the debugger */
        gdb_stub_register_app_thread(k_current_get());
    #endif

	    
    boot_state_mark_healthy();

    
    while (1) {
		        
        #if defined(CONFIG_WATCHDOG)                    
            watchdog_feed();
        #endif


		#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
            /* Service debugger interrupt (Ctrl-C / attach) requests: halts
             * this thread here until GDB sends continue. */
            gdb_stub_poll();
        #endif
		k_sleep(K_MSEC(main_tick_interval));
	}
    
	return 0;
}

