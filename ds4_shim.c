#import <stdio.h>
#import <stdlib.h>
#import <stdbool.h>
#import <stdint.h>
#import <string.h>
#import <math.h>
#import <stdatomic.h>
#import <pthread.h>
#import <unistd.h>
#import <dlfcn.h>
#import <mach/mach_time.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <objc/runtime.h>
#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDDevice.h>
#import <IOKit/hid/IOHIDValue.h>
#import <IOKit/hid/IOHIDKeys.h>
#import <IOKit/hid/IOHIDQueue.h>

/* Logging helper: outputs to stderr AND to log file for diagnostics */
static FILE *g_log_fp = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *get_log_path(void) {
    const char *env_path = getenv("DS4_SHIM_LOG");
    return (env_path && env_path[0]) ? env_path : "/tmp/ds4_shim_debug.log";
}

static void log_msg(const char *fmt, ...) {
    pthread_mutex_lock(&g_log_mutex);
    if (!g_log_fp) {
        g_log_fp = fopen(get_log_path(), "a");
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (g_log_fp) {
        va_list args2;
        va_start(args2, fmt);
        vfprintf(g_log_fp, fmt, args2);
        fflush(g_log_fp);
        va_end(args2);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/* ==========================================================
 * Objective-C Method Swizzling for GameController.framework
 * ==========================================================
 */
static NSString *(*orig_GCController_vendorName)(id, SEL) = NULL;
static NSString *(*orig_GCController_legacyVendorName)(id, SEL) = NULL;
static NSString *(*orig_GCController_productCategory)(id, SEL) = NULL;
static NSString *(*orig_GCController_legacyProductCategory)(id, SEL) = NULL;
static NSString *(*orig_GCController_detailedProductCategory)(id, SEL) = NULL;

static NSString *my_GCController_vendorName(id self, SEL _cmd) {
    NSString *orig = orig_GCController_vendorName ? orig_GCController_vendorName(self, _cmd) : nil;
    log_msg("[ds4_shim] GCController vendorName intercepted (orig='%s') -> returning 'Sony Interactive Entertainment Wireless Controller'\n",
            orig ? [orig UTF8String] : "nil");
    return @"Sony Interactive Entertainment Wireless Controller";
}

static NSString *my_GCController_legacyVendorName(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return @"Sony Interactive Entertainment Wireless Controller";
}

static NSString *my_GCController_productCategory(id self, SEL _cmd) {
    NSString *orig = orig_GCController_productCategory ? orig_GCController_productCategory(self, _cmd) : nil;
    log_msg("[ds4_shim] GCController productCategory intercepted (orig='%s') -> returning 'DualShock 4'\n",
            orig ? [orig UTF8String] : "nil");
    return @"DualShock 4";
}

static NSString *my_GCController_legacyProductCategory(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return @"DualShock 4";
}

static NSString *my_GCController_detailedProductCategory(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return @"DualShock 4";
}

static void install_gc_swizzles(void) {
    Class cls = [GCController class];
    if (!cls) {
        log_msg("[ds4_shim] Warning: GCController class not found in runtime\n");
        return;
    }

    Method m_vn = class_getInstanceMethod(cls, @selector(vendorName));
    if (m_vn) {
        orig_GCController_vendorName = (NSString *(*)(id, SEL))method_getImplementation(m_vn);
        method_setImplementation(m_vn, (IMP)my_GCController_vendorName);
    }

    Method m_lvn = class_getInstanceMethod(cls, sel_registerName("_legacy_vendorName"));
    if (m_lvn) {
        orig_GCController_legacyVendorName = (NSString *(*)(id, SEL))method_getImplementation(m_lvn);
        method_setImplementation(m_lvn, (IMP)my_GCController_legacyVendorName);
    }

    Method m_pc = class_getInstanceMethod(cls, @selector(productCategory));
    if (m_pc) {
        orig_GCController_productCategory = (NSString *(*)(id, SEL))method_getImplementation(m_pc);
        method_setImplementation(m_pc, (IMP)my_GCController_productCategory);
    }

    Method m_lpc = class_getInstanceMethod(cls, sel_registerName("_legacy_productCategory"));
    if (m_lpc) {
        orig_GCController_legacyProductCategory = (NSString *(*)(id, SEL))method_getImplementation(m_lpc);
        method_setImplementation(m_lpc, (IMP)my_GCController_legacyProductCategory);
    }

    Method m_dpc = class_getInstanceMethod(cls, sel_registerName("detailedProductCategory"));
    if (m_dpc) {
        orig_GCController_detailedProductCategory = (NSString *(*)(id, SEL))method_getImplementation(m_dpc);
        method_setImplementation(m_dpc, (IMP)my_GCController_detailedProductCategory);
    }
}

/* ==========================================================
 * Spoofed DualShock 4 IOHID Constants (v1 USB: 0x054C:0x05C4)
 * ==========================================================
 * Unity constructs the joystick name by concatenating:
 *   Manufacturer + " " + Product
 * -> "Sony Interactive Entertainment Wireless Controller"
 */
static CFNumberRef g_ds4_pid = NULL;
static CFNumberRef g_ds4_vid = NULL;
static CFNumberRef g_ds4_ver = NULL;
static CFStringRef g_ds4_prod = NULL;
static CFStringRef g_ds4_mfr  = NULL;
static CFStringRef g_ds4_xprt = NULL;

static void init_spoof_constants(void) {
    if (!g_ds4_pid) {
        int pid = 0x05C4; /* DS4 v1 USB PID (1476) */
        g_ds4_pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    }
    if (!g_ds4_vid) {
        int vid = 0x054C; /* Sony VID (1356) */
        g_ds4_vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    }
    if (!g_ds4_ver) {
        int ver = 0x0100; /* Version 1.00 */
        g_ds4_ver = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ver);
    }
    if (!g_ds4_prod) {
        g_ds4_prod = CFSTR("Wireless Controller");
    }
    if (!g_ds4_mfr) {
        g_ds4_mfr = CFSTR("Sony Interactive Entertainment");
    }
    if (!g_ds4_xprt) {
        g_ds4_xprt = CFSTR("USB");
    }
}

/* Device identification (supports multiple IOHIDDeviceRef instances across managers) */
static IOHIDDeviceRef g_known_ds_devs[16] = {0};
static int g_known_ds_count = 0;
static pthread_mutex_t g_ds_dev_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread bool g_in_is_dualsense = false;

static bool is_dualsense(IOHIDDeviceRef dev) {
    if (!dev) return false;

    pthread_mutex_lock(&g_ds_dev_mutex);
    for (int i = 0; i < g_known_ds_count; i++) {
        if (g_known_ds_devs[i] == dev) {
            pthread_mutex_unlock(&g_ds_dev_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_ds_dev_mutex);

    if (g_in_is_dualsense) return false;

    g_in_is_dualsense = true;
    CFTypeRef vid_ref = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDVendorIDKey));
    CFTypeRef pid_ref = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey));
    g_in_is_dualsense = false;

    int vid = 0, pid = 0;
    if (vid_ref && CFGetTypeID(vid_ref) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)vid_ref, kCFNumberIntType, &vid);
    }
    if (pid_ref && CFGetTypeID(pid_ref) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)pid_ref, kCFNumberIntType, &pid);
    }
    bool match = (vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2 || pid == 0x05C4));
    if (match) {
        pthread_mutex_lock(&g_ds_dev_mutex);
        bool exists = false;
        for (int i = 0; i < g_known_ds_count; i++) {
            if (g_known_ds_devs[i] == dev) { exists = true; break; }
        }
        if (!exists && g_known_ds_count < 16) {
            g_known_ds_devs[g_known_ds_count++] = dev;
        }
        pthread_mutex_unlock(&g_ds_dev_mutex);
    }
    return match;
}

/* Lock-free atomic controller state */
static atomic_int g_btn_state[15];               /* 1..14 (Square, Cross, Circle, Triangle, etc.) */
static atomic_int g_hat_state = 8;               /* 0..7 directions, 8 = neutral */
static atomic_int g_axes_state[6] = {128, 128, 128, 128, 0, 0}; /* 0:LX, 1:LY, 2:RX, 3:RY, 4:L2, 5:R2 */
static atomic_int g_axis_dirty[6] = {0};         /* Coalesced dirty flags: at most 1 event per axis per frame */

/* Latched edge-triggered press/release flags so Unity Update never misses a tap */
static atomic_int g_btn_down_latch[15];
static atomic_int g_btn_up_latch[15];
static atomic_int g_hat_down_latch[4]; /* 0:Up, 1:Right, 2:Down, 3:Left */

/* Cached IOHIDValueRef objects for zero-allocation instant return */
static IOHIDValueRef g_cached_btn_vals[15][2] = {0};   /* [Usage 1..14][0 or 1] */
static IOHIDValueRef g_cached_hat_vals[9] = {0};       /* [Hat 0..8] */
static IOHIDValueRef g_cached_axis_vals[6][256] = {0}; /* [Axis 0..5][0..255] */

/* Thread-safe synthetic event ring buffer for Unity's queue */
#define SYNTH_QUEUE_CAP 1024
static IOHIDValueRef g_synth_queue[SYNTH_QUEUE_CAP];
static volatile int g_sq_head = 0;
static volatile int g_sq_tail = 0;
static pthread_mutex_t g_sq_mutex = PTHREAD_MUTEX_INITIALIZER;

static void push_synth_event(IOHIDValueRef val) {
    if (!val) return;
    pthread_mutex_lock(&g_sq_mutex);
    int next = (g_sq_tail + 1) % SYNTH_QUEUE_CAP;
    if (next == g_sq_head) {
        if (g_synth_queue[g_sq_head]) {
            CFRelease(g_synth_queue[g_sq_head]);
            g_synth_queue[g_sq_head] = NULL;
        }
        g_sq_head = (g_sq_head + 1) % SYNTH_QUEUE_CAP;
    }
    CFRetain(val);
    g_synth_queue[g_sq_tail] = val;
    g_sq_tail = next;
    pthread_mutex_unlock(&g_sq_mutex);
}

static IOHIDValueRef pop_synth_event(void) {
    IOHIDValueRef val = NULL;
    pthread_mutex_lock(&g_sq_mutex);
    if (g_sq_head != g_sq_tail) {
        val = g_synth_queue[g_sq_head];
        g_synth_queue[g_sq_head] = NULL;
        g_sq_head = (g_sq_head + 1) % SYNTH_QUEUE_CAP;
    }
    pthread_mutex_unlock(&g_sq_mutex);
    return val;
}

/* Button & Hat display names for live feedback */
static const char *get_btn_name(int u) {
    switch (u) {
        case 1:  return "Square (□)";
        case 2:  return "Cross (✕)";
        case 3:  return "Circle (◯)";
        case 4:  return "Triangle (△)";
        case 5:  return "L1 (Left Bumper)";
        case 6:  return "R1 (Right Bumper)";
        case 7:  return "L2 (Left Trigger)";
        case 8:  return "R2 (Right Trigger)";
        case 9:  return "Create / Share";
        case 10: return "Options";
        case 11: return "L3 (Left Stick Button)";
        case 12: return "R3 (Right Stick Button)";
        case 13: return "PS Button";
        case 14: return "Touchpad Button";
        default: return "Unknown Button";
    }
}

static const char *get_hat_name(int h) {
    switch (h) {
        case 0: return "D-Pad UP";
        case 1: return "D-Pad UP-RIGHT";
        case 2: return "D-Pad RIGHT";
        case 3: return "D-Pad DOWN-RIGHT";
        case 4: return "D-Pad DOWN";
        case 5: return "D-Pad DOWN-LEFT";
        case 6: return "D-Pad LEFT";
        case 7: return "D-Pad UP-LEFT";
        default: return "D-Pad Neutral";
    }
}

/* ==========================================================
 * DualSense Bluetooth Report Parser
 * ==========================================================
 * Parses both:
 *  - Encapsulated vendor element 0xff00:0x3b (77 bytes, where report[1..10] has sticks/buttons)
 *  - Standard Bluetooth Report 0x01 (9+ bytes) or Report 0x31 (78 bytes)
 */
static uint8_t g_prev_btn[15] = {0};
static uint8_t g_prev_hat = 8;
static uint8_t g_prev_axes[6] = {128, 128, 128, 128, 0, 0};
static uint32_t g_last_pkt_seq = 0;
static bool g_has_pkt_seq = false;
static pthread_mutex_t g_parse_mutex = PTHREAD_MUTEX_INITIALIZER;

static void parse_dualsense_payload(const uint8_t *ds_data, CFIndex ds_len) {
    if (!ds_data || ds_len < 10) return;

    pthread_mutex_lock(&g_parse_mutex);

    /* Deduplicate & enforce strict temporal ordering using the DualSense 32-bit monotonic packet counter
     * at ds_data[11..14] (report[12..15] in 0xff00:0x3b) so a lagging queue never overwrites newer state */
    if (ds_len >= 15) {
        uint32_t seq = 0;
        memcpy(&seq, &ds_data[11], sizeof(uint32_t));
        if (g_has_pkt_seq && (int32_t)(seq - g_last_pkt_seq) <= 0) {
            pthread_mutex_unlock(&g_parse_mutex);
            return;
        }
        g_last_pkt_seq = seq;
        g_has_pkt_seq = true;
    }

    uint8_t lx = ds_data[0];
    uint8_t ly = ds_data[1];
    uint8_t rx = ds_data[2];
    uint8_t ry = ds_data[3];
    uint8_t l2 = ds_data[4];
    uint8_t r2 = ds_data[5];
    /* ds_data[6] is sequence counter */
    uint8_t b0 = ds_data[7];
    uint8_t b1 = ds_data[8];
    uint8_t b2 = ds_data[9];

    uint8_t cur_btn[15];
    cur_btn[1]  = (b0 & 0x10) ? 1 : 0; // Square
    cur_btn[2]  = (b0 & 0x20) ? 1 : 0; // Cross
    cur_btn[3]  = (b0 & 0x40) ? 1 : 0; // Circle
    cur_btn[4]  = (b0 & 0x80) ? 1 : 0; // Triangle
    cur_btn[5]  = (b1 & 0x01) ? 1 : 0; // L1
    cur_btn[6]  = (b1 & 0x02) ? 1 : 0; // R1
    cur_btn[7]  = ((b1 & 0x04) || l2 > 40) ? 1 : 0; // L2
    cur_btn[8]  = ((b1 & 0x08) || r2 > 40) ? 1 : 0; // R2
    cur_btn[9]  = (b1 & 0x10) ? 1 : 0; // Share
    cur_btn[10] = (b1 & 0x20) ? 1 : 0; // Options
    cur_btn[11] = (b1 & 0x40) ? 1 : 0; // L3
    cur_btn[12] = (b1 & 0x80) ? 1 : 0; // R3
    cur_btn[13] = (b2 & 0x01) ? 1 : 0; // PS
    cur_btn[14] = (b2 & 0x02) ? 1 : 0; // Touchpad

    uint8_t cur_hat = b0 & 0x0F;
    if (cur_hat > 8) cur_hat = 8;

    /* 1. Buttons */
    for (int u = 1; u <= 14; u++) {
        atomic_store(&g_btn_state[u], cur_btn[u]);
        if (cur_btn[u] != g_prev_btn[u]) {
            g_prev_btn[u] = cur_btn[u];
            if (cur_btn[u]) {
                atomic_store(&g_btn_down_latch[u], 1);
            } else {
                atomic_store(&g_btn_up_latch[u], 1);
            }
            log_msg(">>> [DualSense] %s: %s <<<\n",
                    get_btn_name(u), cur_btn[u] ? "PRESSED" : "RELEASED");
            if (g_cached_btn_vals[u][cur_btn[u]]) {
                push_synth_event(g_cached_btn_vals[u][cur_btn[u]]);
            }
        }
    }

    /* 2. D-Pad / Hat Switch */
    atomic_store(&g_hat_state, cur_hat);
    if (cur_hat != g_prev_hat) {
        bool was_up    = (g_prev_hat == 7 || g_prev_hat == 0 || g_prev_hat == 1);
        bool was_right = (g_prev_hat == 1 || g_prev_hat == 2 || g_prev_hat == 3);
        bool was_down  = (g_prev_hat == 3 || g_prev_hat == 4 || g_prev_hat == 5);
        bool was_left  = (g_prev_hat == 5 || g_prev_hat == 6 || g_prev_hat == 7);

        bool is_up     = (cur_hat == 7 || cur_hat == 0 || cur_hat == 1);
        bool is_right  = (cur_hat == 1 || cur_hat == 2 || cur_hat == 3);
        bool is_down   = (cur_hat == 3 || cur_hat == 4 || cur_hat == 5);
        bool is_left   = (cur_hat == 5 || cur_hat == 6 || cur_hat == 7);

        if (is_up && !was_up)       atomic_store(&g_hat_down_latch[0], 1);
        if (is_right && !was_right) atomic_store(&g_hat_down_latch[1], 1);
        if (is_down && !was_down)   atomic_store(&g_hat_down_latch[2], 1);
        if (is_left && !was_left)   atomic_store(&g_hat_down_latch[3], 1);

        g_prev_hat = cur_hat;
        log_msg(">>> [DualSense] %s <<<\n", get_hat_name(cur_hat));
        if (g_cached_hat_vals[cur_hat]) {
            push_synth_event(g_cached_hat_vals[cur_hat]);
        }
    }

    /* 3. Analog Sticks & Triggers (coalesced dirty flags: ZERO FIFO queue backlog!) */
    atomic_store(&g_axes_state[0], lx);
    atomic_store(&g_axes_state[1], ly);
    atomic_store(&g_axes_state[2], rx);
    atomic_store(&g_axes_state[3], ry);
    atomic_store(&g_axes_state[4], l2);
    atomic_store(&g_axes_state[5], r2);

    uint8_t cur_axes[6] = {lx, ly, rx, ry, l2, r2};
    for (int ax = 0; ax < 6; ax++) {
        if (cur_axes[ax] != g_prev_axes[ax]) {
            g_prev_axes[ax] = cur_axes[ax];
            atomic_store(&g_axis_dirty[ax], 1);
        }
    }

    pthread_mutex_unlock(&g_parse_mutex);
}

static void parse_vendor_0x3b_value(IOHIDValueRef val) {
    if (!val) return;
    const uint8_t *report = (const uint8_t *)IOHIDValueGetBytePtr(val);
    CFIndex len = IOHIDValueGetLength(val);
    if (report && len >= 11) {
        /* In vendor element 0xff00:0x3b, report[0] is the BT header (e.g. 0x01/0x11..0xf1),
         * and report[1..10] is the 10-byte DualSense input payload! */
        parse_dualsense_payload(&report[1], len - 1);
    }
}

/* Background reader thread & private queue */
static volatile int g_running = 0;
static pthread_t g_reader_th;
static IOHIDQueueRef g_priv_queue = NULL;
static IOHIDDeviceRef g_priv_dev = NULL;
static IOHIDDeviceRef g_cached_val_dev = NULL;
static pthread_mutex_t g_bridge_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_bridge_initialized = false;

static void *reader_thread(void *arg) {
    IOHIDQueueRef q = (IOHIDQueueRef)arg;
    log_msg("[ds4_shim] Background Bluetooth reader thread started (500Hz low-latency)\n");

    while (g_running) {
        bool got_any = false;
        IOHIDValueRef val = NULL;
        while ((val = IOHIDQueueCopyNextValueWithTimeout(q, 0.0)) != NULL) {
            got_any = true;
            IOHIDElementRef el = IOHIDValueGetElement(val);
            if (el && IOHIDElementGetUsagePage(el) == 0xff00 && IOHIDElementGetUsage(el) == 0x3b) {
                parse_vendor_0x3b_value(val);
            }
            CFRelease(val);
        }
        if (!got_any) {
            usleep(2000); /* 2ms sleep (500Hz) when queue is empty for minimal latency */
        }
    }
    return NULL;
}

static void cache_elements_for_device_locked(IOHIDDeviceRef dev, IOHIDElementRef *out_vendor_elem) {
    if (!dev) return;
    CFArrayRef elems = IOHIDDeviceCopyMatchingElements(dev, NULL, 0);
    if (!elems) return;

    CFIndex ecnt = CFArrayGetCount(elems);
    for (CFIndex i = 0; i < ecnt; i++) {
        IOHIDElementRef el = (IOHIDElementRef)CFArrayGetValueAtIndex(elems, i);
        uint32_t up = IOHIDElementGetUsagePage(el);
        uint32_t u = IOHIDElementGetUsage(el);

        if (up == 0x09 && u >= 1 && u <= 14) {
            g_cached_btn_vals[u][0] = IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, el, 0, 0);
            g_cached_btn_vals[u][1] = IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, el, 0, 1);
        } else if (up == 0x01 && u == 0x39) {
            for (int h = 0; h <= 8; h++) {
                g_cached_hat_vals[h] = IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, el, 0, h);
            }
        } else if (up == 0x01) {
            int ax = -1;
            switch (u) {
                case 0x30: ax = 0; break; /* LX */
                case 0x31: ax = 1; break; /* LY */
                case 0x32: ax = 2; break; /* RX */
                case 0x35: ax = 3; break; /* RY */
                case 0x33: ax = 4; break; /* L2 */
                case 0x34: ax = 5; break; /* R2 */
                default: break;
            }
            if (ax >= 0) {
                for (int v = 0; v <= 255; v++) {
                    g_cached_axis_vals[ax][v] = IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, el, 0, v);
                }
            }
        } else if (up == 0xff00 && u == 0x3b) {
            if (out_vendor_elem) *out_vendor_elem = el;
        }
    }
    CFRelease(elems);
    g_cached_val_dev = dev;
}

static void init_dualsense_bridge(IOHIDDeviceRef dev) {
    if (!dev) return;

    pthread_mutex_lock(&g_bridge_mutex);
    if (g_bridge_initialized) {
        /* If Unity or InControl passes its own IOHIDDeviceRef (different from discovery_thread's),
         * update cached IOHIDValueRefs so their IOHIDElementRefs match Unity's device! */
        if (dev != g_cached_val_dev && dev != g_priv_dev) {
            cache_elements_for_device_locked(dev, NULL);
        }
        pthread_mutex_unlock(&g_bridge_mutex);
        return;
    }

    g_priv_dev = (IOHIDDeviceRef)CFRetain(dev);
    IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);

    /* Initialize initial atomic state */
    for (int u = 1; u <= 14; u++) {
        atomic_store(&g_btn_state[u], 0);
        atomic_store(&g_btn_down_latch[u], 0);
        atomic_store(&g_btn_up_latch[u], 0);
    }
    for (int d = 0; d < 4; d++) {
        atomic_store(&g_hat_down_latch[d], 0);
    }
    atomic_store(&g_hat_state, 8);
    atomic_store(&g_axes_state[0], 128);
    atomic_store(&g_axes_state[1], 128);
    atomic_store(&g_axes_state[2], 128);
    atomic_store(&g_axes_state[3], 128);
    atomic_store(&g_axes_state[4], 0);
    atomic_store(&g_axes_state[5], 0);

    IOHIDElementRef vendor_elem = NULL;
    cache_elements_for_device_locked(dev, &vendor_elem);

    if (!vendor_elem) {
        log_msg("[ds4_shim] Note: Vendor element 0xff00:0x3b not found or not required\n");
        pthread_mutex_unlock(&g_bridge_mutex);
        return;
    }

    /* Create and start private queue specifically for background thread */
    g_priv_queue = IOHIDQueueCreate(kCFAllocatorDefault, dev, 64, 0);
    if (!g_priv_queue) {
        log_msg("[ds4_shim] Failed to create private IOHIDQueue\n");
        pthread_mutex_unlock(&g_bridge_mutex);
        return;
    }

    IOHIDQueueAddElement(g_priv_queue, vendor_elem);
    IOHIDQueueStart(g_priv_queue);

    g_running = 1;
    pthread_create(&g_reader_th, NULL, reader_thread, g_priv_queue);

    g_bridge_initialized = true;
    log_msg("[ds4_shim] Bridge initialized successfully! DualSense mapped to DS4.\n");
    log_msg("\n==========================================================\n");
    log_msg("  DualSense Wireless Bridge ACTIVE!\n");
    log_msg("  Target Game: Devotion\n");
    log_msg("  Spoofed Device: Sony DualShock 4 (VID 0x054C, PID 0x05C4)\n");
    log_msg("  Profile Name: 'Sony Interactive Entertainment Wireless Controller'\n");
    log_msg("  Status: All controls (Cross, Circle, Triangle, Square,\n");
    log_msg("          D-Pad, L1/R1/L2/R2, L3/R3, Left & Right Sticks) READY!\n");
    log_msg("==========================================================\n\n");

    pthread_mutex_unlock(&g_bridge_mutex);
}

/* Background discovery thread: keeps g_disc_mgr open for process lifetime so g_priv_queue stays live! */
static IOHIDManagerRef g_disc_mgr = NULL;

static void *discovery_thread(void *arg) {
    (void)arg;
    while (!g_bridge_initialized) {
        IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (!mgr) {
            usleep(250000);
            continue;
        }
        IOHIDManagerSetDeviceMatching(mgr, NULL);
        IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);

        CFSetRef devs = IOHIDManagerCopyDevices(mgr);
        if (devs) {
            CFIndex cnt = CFSetGetCount(devs);
            if (cnt > 0) {
                const void **vals = (const void **)calloc(cnt, sizeof(void *));
                if (vals) {
                    CFSetGetValues(devs, vals);
                    for (CFIndex i = 0; i < cnt; i++) {
                        IOHIDDeviceRef d = (IOHIDDeviceRef)vals[i];
                        if (is_dualsense(d)) {
                            init_spoof_constants();
                            init_dualsense_bridge(d);
                            break;
                        }
                    }
                    free(vals);
                }
            }
            CFRelease(devs);
        }
        if (g_bridge_initialized) {
            /* CRITICAL: Keep `mgr` alive for the entire process lifetime!
             * Calling CFRelease(mgr) when g_bridge_initialized is true closes `d`
             * and stops `g_priv_queue` from receiving any Bluetooth packets! */
            g_disc_mgr = mgr;
            break;
        }
        CFRelease(mgr);
        usleep(250000); /* Check every 250ms until DualSense is connected */
    }
    return NULL;
}

/* ==========================================================
 * Mono Internal Call Interception (Direct UnityEngine.Input Hook)
 * ==========================================================
 */
typedef void (*mono_add_internal_call_fn)(const char *name, const void *method);
typedef void *(*mono_domain_get_fn)(void);
typedef void *(*mono_get_string_class_fn)(void);
typedef void *(*mono_array_new_fn)(void *domain, void *eclass, uintptr_t n);
typedef void *(*mono_string_new_fn)(void *domain, const char *text);
typedef char *(*mono_array_addr_with_size_fn)(void *array, int size, uintptr_t idx);
typedef char *(*mono_string_to_utf8_fn)(void *str);
typedef void (*mono_free_fn)(void *ptr);

static mono_add_internal_call_fn    real_mono_add_internal_call = NULL;
static mono_domain_get_fn           real_mono_domain_get = NULL;
static mono_get_string_class_fn     real_mono_get_string_class = NULL;
static mono_array_new_fn            real_mono_array_new = NULL;
static mono_string_new_fn           real_mono_string_new = NULL;
static mono_array_addr_with_size_fn real_mono_array_addr_with_size = NULL;
static mono_string_to_utf8_fn       real_mono_string_to_utf8 = NULL;
static mono_free_fn                 real_mono_free = NULL;

typedef void *(*GetJoystickNames_fn)(void);
typedef uint8_t (*GetKeyInt_fn)(int keycode);
typedef uint8_t (*GetKeyString_fn)(void *name);
typedef uint8_t (*GetButton_fn)(void *name);
typedef float (*GetAxis_fn)(void *name);
typedef uint8_t (*GetAnyKey_fn)(void);
typedef int (*GetFrameCount_fn)(void);
typedef void (*SetInt_fn)(int val);
typedef int (*GetInt_fn)(void);
typedef void (*SetResolution_fn)(int width, int height, int fullscreenMode, int preferredRefreshRate);

static GetJoystickNames_fn orig_GetJoystickNames    = NULL;
static GetKeyInt_fn        orig_GetKeyInt           = NULL;
static GetKeyInt_fn        orig_GetKeyDownInt       = NULL;
static GetKeyInt_fn        orig_GetKeyUpInt         = NULL;
static GetKeyString_fn     orig_GetKeyString        = NULL;
static GetKeyString_fn     orig_GetKeyDownString    = NULL;
static GetKeyString_fn     orig_GetKeyUpString      = NULL;
static GetButton_fn        orig_GetButton           = NULL;
static GetButton_fn        orig_GetButtonDown       = NULL;
static GetButton_fn        orig_GetButtonUp         = NULL;
static GetAxis_fn          orig_GetAxisRaw          = NULL;
static GetAxis_fn          orig_GetAxis             = NULL;
static GetAnyKey_fn        orig_get_anyKey          = NULL;
static GetAnyKey_fn        orig_get_anyKeyDown      = NULL;
static GetFrameCount_fn    orig_get_frameCount      = NULL;
static SetInt_fn           orig_set_targetFrameRate = NULL;
static SetInt_fn           orig_set_vSyncCount      = NULL;
static GetInt_fn           orig_get_width           = NULL;
static GetInt_fn           orig_get_height          = NULL;
static GetInt_fn           orig_get_fullScreenMode  = NULL;
static SetResolution_fn    orig_SetResolution       = NULL;

/* FPS & Retina Resolution Booster state */
static int  g_target_fps = 120;
static int  g_max_render_width = 1920;
static bool g_fps_boost_applied = false;
static bool g_in_set_resolution = false;

static void my_set_targetFrameRate(int val) {
    int target = g_target_fps;
    log_msg("[ds4_shim] FPS Booster: Game requested targetFrameRate=%d -> overriding to %d FPS\n", val, target);
    if (orig_set_targetFrameRate) {
        orig_set_targetFrameRate(target);
    }
}

static void my_set_vSyncCount(int val) {
    log_msg("[ds4_shim] FPS Booster: Game requested vSyncCount=%d -> overriding to 0 (uncapped/ProMotion)\n", val);
    if (orig_set_vSyncCount) {
        orig_set_vSyncCount(0);
    }
    if (orig_set_targetFrameRate) {
        orig_set_targetFrameRate(g_target_fps);
    }
}

static void my_SetResolution(int width, int height, int fullscreenMode, int preferredRefreshRate) {
    if (!orig_SetResolution) return;
    if (g_in_set_resolution) {
        orig_SetResolution(width, height, fullscreenMode, preferredRefreshRate);
        return;
    }
    int new_w = width;
    int new_h = height;
    if (g_max_render_width > 0 && width > g_max_render_width && height > 0) {
        new_w = g_max_render_width;
        new_h = (int)((float)height * ((float)g_max_render_width / (float)width) + 0.5f);
        /* Keep height even for Metal framebuffer alignment */
        if (new_h & 1) new_h++;
    }
    int new_rate = (preferredRefreshRate < g_target_fps && g_target_fps > 0) ? g_target_fps : preferredRefreshRate;
    log_msg("[ds4_shim] FPS Booster: Screen.SetResolution(%dx%d, mode=%d, rate=%d) -> optimized to (%dx%d, mode=%d, rate=%d)\n",
            width, height, fullscreenMode, preferredRefreshRate, new_w, new_h, fullscreenMode, new_rate);
    g_in_set_resolution = true;
    orig_SetResolution(new_w, new_h, fullscreenMode, new_rate);
    g_in_set_resolution = false;
}

static void apply_fps_boost_once(void) {
    if (g_fps_boost_applied) return;
    g_fps_boost_applied = true;

    const char *env_fps = getenv("DS4_TARGET_FPS");
    if (env_fps && env_fps[0]) {
        g_target_fps = atoi(env_fps);
    }
    const char *env_maxw = getenv("DS4_MAX_WIDTH");
    if (env_maxw && env_maxw[0]) {
        g_max_render_width = atoi(env_maxw);
    }

    if (orig_set_vSyncCount) {
        orig_set_vSyncCount(0);
    }
    if (orig_set_targetFrameRate) {
        orig_set_targetFrameRate(g_target_fps);
    }
    if (orig_SetResolution && orig_get_width && orig_get_height && g_max_render_width > 0) {
        int cur_w = orig_get_width();
        int cur_h = orig_get_height();
        int mode  = orig_get_fullScreenMode ? orig_get_fullScreenMode() : 1; /* 1 = FullScreenWindow */
        if (cur_w > g_max_render_width && cur_h > 0) {
            my_SetResolution(cur_w, cur_h, mode, g_target_fps);
        }
    }
    log_msg("[ds4_shim] FPS Booster active: targetFrameRate=%d, vSync=0, maxRenderWidth=%d\n",
            g_target_fps, g_max_render_width);
}

/* Per-Unity-frame button & D-Pad transition state */
static int     g_last_unity_frame = -1;
static uint8_t g_frame_btn_held[15] = {0};
static uint8_t g_frame_btn_down[15] = {0};
static uint8_t g_frame_btn_up[15]   = {0};
static uint8_t g_frame_hat_held[4]  = {0}; /* 0:Up, 1:Right, 2:Down, 3:Left */
static uint8_t g_frame_hat_down[4]  = {0};
static uint8_t g_frame_hat_up[4]    = {0};

static void sync_unity_frame_state(void) {
    int cur_frame = orig_get_frameCount ? orig_get_frameCount() : (g_last_unity_frame + 1);
    if (cur_frame == g_last_unity_frame) {
        return;
    }
    g_last_unity_frame = cur_frame;

    if (!g_fps_boost_applied && cur_frame >= 1) {
        apply_fps_boost_once();
    }

    for (int u = 1; u <= 14; u++) {
        uint8_t prev = g_frame_btn_held[u];
        uint8_t now  = (uint8_t)(atomic_load(&g_btn_state[u]) ? 1 : 0);
        int latched_down = atomic_exchange(&g_btn_down_latch[u], 0);
        int latched_up   = atomic_exchange(&g_btn_up_latch[u], 0);

        g_frame_btn_down[u] = (now && !prev) || latched_down ? 1 : 0;
        g_frame_btn_up[u]   = (!now && prev) || latched_up   ? 1 : 0;
        g_frame_btn_held[u] = now;
    }

    int h = atomic_load(&g_hat_state);
    uint8_t now_hat[4];
    now_hat[0] = (h == 7 || h == 0 || h == 1) ? 1 : 0; /* Up */
    now_hat[1] = (h == 1 || h == 2 || h == 3) ? 1 : 0; /* Right */
    now_hat[2] = (h == 3 || h == 4 || h == 5) ? 1 : 0; /* Down */
    now_hat[3] = (h == 5 || h == 6 || h == 7) ? 1 : 0; /* Left */

    for (int d = 0; d < 4; d++) {
        uint8_t prev = g_frame_hat_held[d];
        uint8_t now  = now_hat[d];
        int latched_down = atomic_exchange(&g_hat_down_latch[d], 0);
        g_frame_hat_down[d] = (now && !prev) || latched_down ? 1 : 0;
        g_frame_hat_up[d]   = (!now && prev) ? 1 : 0;
        g_frame_hat_held[d] = now;
    }
}

static void *my_GetJoystickNames(void) {
    if (real_mono_domain_get && real_mono_get_string_class &&
        real_mono_array_new && real_mono_string_new && real_mono_array_addr_with_size) {
        void *domain = real_mono_domain_get();
        void *str_cls = real_mono_get_string_class();
        if (domain && str_cls) {
            void *arr = real_mono_array_new(domain, str_cls, 1);
            void *str = real_mono_string_new(domain, "Sony Interactive Entertainment Wireless Controller");
            if (arr && str) {
                void **elem_ptr = (void **)real_mono_array_addr_with_size(arr, sizeof(void *), 0);
                if (elem_ptr) {
                    *elem_ptr = str;
                    return arr;
                }
            }
        }
    }
    return orig_GetJoystickNames ? orig_GetJoystickNames() : NULL;
}

static uint8_t my_GetKeyInt(int keycode) {
    sync_unity_frame_state();
    if (keycode >= 330 && keycode <= 429) {
        int btn_idx = (keycode - 330) % 20;
        if (btn_idx >= 0 && btn_idx <= 13) {
            if (g_frame_btn_held[btn_idx + 1]) return 1;
        }
    }
    if (keycode == 273 && g_frame_hat_held[0]) return 1; /* UpArrow */
    if (keycode == 274 && g_frame_hat_held[2]) return 1; /* DownArrow */
    if (keycode == 275 && g_frame_hat_held[1]) return 1; /* RightArrow */
    if (keycode == 276 && g_frame_hat_held[3]) return 1; /* LeftArrow */
    return orig_GetKeyInt ? orig_GetKeyInt(keycode) : 0;
}

static uint8_t my_GetKeyDownInt(int keycode) {
    sync_unity_frame_state();
    if (keycode >= 330 && keycode <= 429) {
        int btn_idx = (keycode - 330) % 20;
        if (btn_idx >= 0 && btn_idx <= 13) {
            if (g_frame_btn_down[btn_idx + 1]) return 1;
        }
    }
    if (keycode == 273 && g_frame_hat_down[0]) return 1; /* UpArrow */
    if (keycode == 274 && g_frame_hat_down[2]) return 1; /* DownArrow */
    if (keycode == 275 && g_frame_hat_down[1]) return 1; /* RightArrow */
    if (keycode == 276 && g_frame_hat_down[3]) return 1; /* LeftArrow */
    return orig_GetKeyDownInt ? orig_GetKeyDownInt(keycode) : 0;
}

static uint8_t my_GetKeyUpInt(int keycode) {
    sync_unity_frame_state();
    if (keycode >= 330 && keycode <= 429) {
        int btn_idx = (keycode - 330) % 20;
        if (btn_idx >= 0 && btn_idx <= 13) {
            if (g_frame_btn_up[btn_idx + 1]) return 1;
        }
    }
    if (keycode == 273 && g_frame_hat_up[0]) return 1; /* UpArrow */
    if (keycode == 274 && g_frame_hat_up[2]) return 1; /* DownArrow */
    if (keycode == 275 && g_frame_hat_up[1]) return 1; /* RightArrow */
    if (keycode == 276 && g_frame_hat_up[3]) return 1; /* LeftArrow */
    return orig_GetKeyUpInt ? orig_GetKeyUpInt(keycode) : 0;
}

static uint8_t my_get_anyKey(void) {
    sync_unity_frame_state();
    for (int u = 1; u <= 14; u++) {
        if (g_frame_btn_held[u]) return 1;
    }
    for (int d = 0; d < 4; d++) {
        if (g_frame_hat_held[d]) return 1;
    }
    return orig_get_anyKey ? orig_get_anyKey() : 0;
}

static uint8_t my_get_anyKeyDown(void) {
    sync_unity_frame_state();
    for (int u = 1; u <= 14; u++) {
        if (g_frame_btn_down[u]) return 1;
    }
    for (int d = 0; d < 4; d++) {
        if (g_frame_hat_down[d]) return 1;
    }
    return orig_get_anyKeyDown ? orig_get_anyKeyDown() : 0;
}

/* Helper to extract ASCII string from MonoString* safely without allocation */
typedef struct {
    void *vtable;
    void *sync;
    int32_t length;
    uint16_t chars[1];
} MonoStringLayout;

static bool mono_str_to_ascii(void *mono_str, char *buf, size_t buf_sz) {
    if (!mono_str || buf_sz == 0) return false;
    MonoStringLayout *ms = (MonoStringLayout *)mono_str;
    int32_t len = ms->length;
    if (len <= 0 || len >= (int32_t)buf_sz) return false;
    for (int32_t i = 0; i < len; i++) {
        buf[i] = (char)(ms->chars[i] & 0x7F);
    }
    buf[len] = '\0';
    return true;
}

static uint8_t my_GetKeyString(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        int joy = 0, btn = -1;
        if (sscanf(buf, "joystick %d button %d", &joy, &btn) == 2 ||
            sscanf(buf, "joystick button %d", &btn) == 1) {
            if (btn >= 0 && btn <= 13) {
                return g_frame_btn_held[btn + 1] ? 1 : 0;
            }
        }
    }
    return orig_GetKeyString ? orig_GetKeyString(name_obj) : 0;
}

static uint8_t my_GetKeyDownString(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        int joy = 0, btn = -1;
        if (sscanf(buf, "joystick %d button %d", &joy, &btn) == 2 ||
            sscanf(buf, "joystick button %d", &btn) == 1) {
            if (btn >= 0 && btn <= 13) {
                return g_frame_btn_down[btn + 1] ? 1 : 0;
            }
        }
    }
    return orig_GetKeyDownString ? orig_GetKeyDownString(name_obj) : 0;
}

static uint8_t my_GetKeyUpString(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        int joy = 0, btn = -1;
        if (sscanf(buf, "joystick %d button %d", &joy, &btn) == 2 ||
            sscanf(buf, "joystick button %d", &btn) == 1) {
            if (btn >= 0 && btn <= 13) {
                return g_frame_btn_up[btn + 1] ? 1 : 0;
            }
        }
    }
    return orig_GetKeyUpString ? orig_GetKeyUpString(name_obj) : 0;
}

static uint8_t my_GetButton(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        if (strcmp(buf, "Submit") == 0) {
            if (g_frame_btn_held[2]) return 1; /* Cross (✕) confirms */
        } else if (strcmp(buf, "Cancel") == 0) {
            if (g_frame_btn_held[3] || g_frame_btn_held[10]) return 1; /* Circle (◯) / Options cancels */
        } else if (strcmp(buf, "Jump") == 0) {
            if (g_frame_btn_held[4]) return 1; /* Triangle (△) */
        } else if (strcmp(buf, "Recall") == 0) {
            if (g_frame_btn_held[3]) return 1; /* Circle (◯) */
        }
    }
    return orig_GetButton ? orig_GetButton(name_obj) : 0;
}

static uint8_t my_GetButtonDown(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        if (strcmp(buf, "Submit") == 0) {
            if (g_frame_btn_down[2]) {
                log_msg("[ds4_shim] Input.GetButtonDown('Submit') triggered by Cross (✕)\n");
                return 1;
            }
        } else if (strcmp(buf, "Cancel") == 0) {
            if (g_frame_btn_down[3] || g_frame_btn_down[10]) {
                log_msg("[ds4_shim] Input.GetButtonDown('Cancel') triggered by Circle (◯) / Options\n");
                return 1;
            }
        } else if (strcmp(buf, "Jump") == 0) {
            if (g_frame_btn_down[4]) return 1;
        } else if (strcmp(buf, "Recall") == 0) {
            if (g_frame_btn_down[3]) return 1;
        }
    }
    return orig_GetButtonDown ? orig_GetButtonDown(name_obj) : 0;
}

static uint8_t my_GetButtonUp(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        if (strcmp(buf, "Submit") == 0) {
            if (g_frame_btn_up[2]) return 1;
        } else if (strcmp(buf, "Cancel") == 0) {
            if (g_frame_btn_up[3] || g_frame_btn_up[10]) return 1;
        } else if (strcmp(buf, "Jump") == 0) {
            if (g_frame_btn_up[4]) return 1;
        } else if (strcmp(buf, "Recall") == 0) {
            if (g_frame_btn_up[3]) return 1;
        }
    }
    return orig_GetButtonUp ? orig_GetButtonUp(name_obj) : 0;
}

static bool compute_dualsense_axis(int analog_idx, float *out_val) {
    switch (analog_idx) {
        case 0: /* Left Stick X (-1.0 left .. +1.0 right) */
        case 1: /* Left Stick Y (-1.0 up   .. +1.0 down)  */
        case 2: /* Right Stick X (-1.0 left .. +1.0 right) */
        case 3: /* Right Stick Y (-1.0 up   .. +1.0 down)  */
        {
            int raw = atomic_load(&g_axes_state[analog_idx]);
            float v = ((float)raw - 127.5f) / 127.5f;
            if (fabsf(v) < 0.08f) v = 0.0f;
            if (v < -1.0f) v = -1.0f;
            if (v >  1.0f) v =  1.0f;
            *out_val = v;
            return true;
        }
        case 4: /* Left Trigger L2 (-1.0 released .. +1.0 pressed) */
        {
            int raw = atomic_load(&g_axes_state[4]);
            *out_val = ((float)raw / 255.0f) * 2.0f - 1.0f;
            return true;
        }
        case 5: /* Right Trigger R2 (-1.0 released .. +1.0 pressed) */
        {
            int raw = atomic_load(&g_axes_state[5]);
            *out_val = ((float)raw / 255.0f) * 2.0f - 1.0f;
            return true;
        }
        case 6: /* D-Pad X (-1.0 left, +1.0 right, 0.0 neutral) */
        {
            int h = atomic_load(&g_hat_state);
            if (h == 1 || h == 2 || h == 3) *out_val = 1.0f;
            else if (h == 5 || h == 6 || h == 7) *out_val = -1.0f;
            else *out_val = 0.0f;
            return true;
        }
        case 7: /* D-Pad Y (-1.0 up, +1.0 down, 0.0 neutral) */
        {
            int h = atomic_load(&g_hat_state);
            if (h == 7 || h == 0 || h == 1) *out_val = -1.0f;
            else if (h == 3 || h == 4 || h == 5) *out_val = 1.0f;
            else *out_val = 0.0f;
            return true;
        }
        default:
            return false;
    }
}

static bool is_kbd_move_held(void) {
    if (!orig_GetKeyInt) return false;
    /* W=119, A=97, S=115, D=100, Up=273, Down=274, Right=275, Left=276 */
    return orig_GetKeyInt(119) || orig_GetKeyInt(97) || orig_GetKeyInt(115) || orig_GetKeyInt(100) ||
           orig_GetKeyInt(273) || orig_GetKeyInt(274) || orig_GetKeyInt(275) || orig_GetKeyInt(276);
}

static bool compute_named_axis(const char *buf, float *out_val) {
    int joy = 0, analog_idx = -1;
    if (sscanf(buf, "joystick %d analog %d", &joy, &analog_idx) == 2 ||
        sscanf(buf, "joystick analog %d", &analog_idx) == 1) {
        return compute_dualsense_axis(analog_idx, out_val);
    }
    if (strcmp(buf, "Horizontal") == 0) {
        float lx = 0.0f, dx = 0.0f;
        compute_dualsense_axis(0, &lx);
        compute_dualsense_axis(6, &dx);
        float v = (fabsf(dx) > fabsf(lx)) ? dx : lx;
        if (fabsf(v) > 0.05f || (g_bridge_initialized && !is_kbd_move_held())) {
            *out_val = v;
            return true;
        }
    } else if (strcmp(buf, "Vertical") == 0) {
        float ly = 0.0f, dy = 0.0f;
        compute_dualsense_axis(1, &ly);
        compute_dualsense_axis(7, &dy);
        /* In Unity InputManager, Vertical is +1.0 for Up, -1.0 for Down (invert=1 on axis 1) */
        float v = (fabsf(dy) > fabsf(ly)) ? -dy : -ly;
        if (fabsf(v) > 0.05f || (g_bridge_initialized && !is_kbd_move_held())) {
            *out_val = v;
            return true;
        }
    } else if (strcmp(buf, "XBOX_XRotate") == 0) {
        if (g_bridge_initialized) {
            return compute_dualsense_axis(2, out_val);
        }
    } else if (strcmp(buf, "XBOX_YRotate") == 0) {
        if (g_bridge_initialized) {
            float ry = 0.0f;
            compute_dualsense_axis(3, &ry);
            *out_val = -ry;
            return true;
        }
    } else if (strcmp(buf, "Gaze") == 0) {
        if (g_bridge_initialized) {
            int l2 = atomic_load(&g_axes_state[4]);
            *out_val = (l2 > 20) ? ((float)l2 / 255.0f) : 0.0f;
            return true;
        }
    }
    return false;
}

static float my_GetAxisRaw(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        float val = 0.0f;
        if (compute_named_axis(buf, &val)) {
            return val;
        }
    }
    return orig_GetAxisRaw ? orig_GetAxisRaw(name_obj) : 0.0f;
}

static float my_GetAxis(void *name_obj) {
    sync_unity_frame_state();
    char buf[64];
    if (mono_str_to_ascii(name_obj, buf, sizeof(buf))) {
        float val = 0.0f;
        if (compute_named_axis(buf, &val)) {
            return val;
        }
    }
    return orig_GetAxis ? orig_GetAxis(name_obj) : 0.0f;
}

static void my_mono_add_internal_call(const char *name, const void *method) {
    if (name && method) {
        if (strcmp(name, "UnityEngine.Input::GetJoystickNames") == 0) {
            orig_GetJoystickNames = (GetJoystickNames_fn)method;
            method = (const void *)my_GetJoystickNames;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetKeyInt") == 0) {
            orig_GetKeyInt = (GetKeyInt_fn)method;
            method = (const void *)my_GetKeyInt;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetKeyDownInt") == 0) {
            orig_GetKeyDownInt = (GetKeyInt_fn)method;
            method = (const void *)my_GetKeyDownInt;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetKeyUpInt") == 0) {
            orig_GetKeyUpInt = (GetKeyInt_fn)method;
            method = (const void *)my_GetKeyUpInt;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetKeyString") == 0) {
            orig_GetKeyString = (GetKeyString_fn)method;
            method = (const void *)my_GetKeyString;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetKeyDownString") == 0) {
            orig_GetKeyDownString = (GetKeyString_fn)method;
            method = (const void *)my_GetKeyDownString;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetKeyUpString") == 0) {
            orig_GetKeyUpString = (GetKeyString_fn)method;
            method = (const void *)my_GetKeyUpString;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetButton") == 0) {
            orig_GetButton = (GetButton_fn)method;
            method = (const void *)my_GetButton;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetButtonDown") == 0) {
            orig_GetButtonDown = (GetButton_fn)method;
            method = (const void *)my_GetButtonDown;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetButtonUp") == 0) {
            orig_GetButtonUp = (GetButton_fn)method;
            method = (const void *)my_GetButtonUp;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetAxisRaw") == 0) {
            orig_GetAxisRaw = (GetAxis_fn)method;
            method = (const void *)my_GetAxisRaw;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::GetAxis") == 0) {
            orig_GetAxis = (GetAxis_fn)method;
            method = (const void *)my_GetAxis;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::get_anyKey") == 0) {
            orig_get_anyKey = (GetAnyKey_fn)method;
            method = (const void *)my_get_anyKey;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Input::get_anyKeyDown") == 0) {
            orig_get_anyKeyDown = (GetAnyKey_fn)method;
            method = (const void *)my_get_anyKeyDown;
            log_msg("[ds4_shim] Hooked Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Time::get_frameCount") == 0) {
            orig_get_frameCount = (GetFrameCount_fn)method;
            log_msg("[ds4_shim] Captured Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Application::set_targetFrameRate") == 0) {
            orig_set_targetFrameRate = (SetInt_fn)method;
            method = (const void *)my_set_targetFrameRate;
            log_msg("[ds4_shim] Hooked Mono icall (FPS Booster): %s\n", name);
        } else if (strcmp(name, "UnityEngine.QualitySettings::set_vSyncCount") == 0) {
            orig_set_vSyncCount = (SetInt_fn)method;
            method = (const void *)my_set_vSyncCount;
            log_msg("[ds4_shim] Hooked Mono icall (FPS Booster): %s\n", name);
        } else if (strcmp(name, "UnityEngine.Screen::SetResolution") == 0) {
            orig_SetResolution = (SetResolution_fn)method;
            method = (const void *)my_SetResolution;
            log_msg("[ds4_shim] Hooked Mono icall (FPS Booster): %s\n", name);
        } else if (strcmp(name, "UnityEngine.Screen::get_width") == 0) {
            orig_get_width = (GetInt_fn)method;
            log_msg("[ds4_shim] Captured Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Screen::get_height") == 0) {
            orig_get_height = (GetInt_fn)method;
            log_msg("[ds4_shim] Captured Mono icall: %s\n", name);
        } else if (strcmp(name, "UnityEngine.Screen::get_fullScreenMode") == 0) {
            orig_get_fullScreenMode = (GetInt_fn)method;
            log_msg("[ds4_shim] Captured Mono icall: %s\n", name);
        }
    }
    if (real_mono_add_internal_call) {
        real_mono_add_internal_call(name, method);
    }
}

void *my_dlsym(void *handle, const char *symbol) {
    void *sym = dlsym(handle, symbol);
    if (!symbol || !sym) return sym;

    if (strcmp(symbol, "mono_add_internal_call") == 0) {
        real_mono_add_internal_call    = (mono_add_internal_call_fn)sym;
        real_mono_domain_get           = (mono_domain_get_fn)dlsym(handle, "mono_domain_get");
        real_mono_get_string_class     = (mono_get_string_class_fn)dlsym(handle, "mono_get_string_class");
        real_mono_array_new            = (mono_array_new_fn)dlsym(handle, "mono_array_new");
        real_mono_string_new           = (mono_string_new_fn)dlsym(handle, "mono_string_new");
        real_mono_array_addr_with_size = (mono_array_addr_with_size_fn)dlsym(handle, "mono_array_addr_with_size");
        real_mono_string_to_utf8       = (mono_string_to_utf8_fn)dlsym(handle, "mono_string_to_utf8");
        real_mono_free                 = (mono_free_fn)dlsym(handle, "mono_free");
        log_msg("[ds4_shim] Intercepted mono_add_internal_call via dlsym\n");
        return (void *)my_mono_add_internal_call;
    }
    return sym;
}

/* ==========================================================
 * Interposed IOKit Functions
 * ==========================================================
 */
CFTypeRef my_IOHIDDeviceGetProperty(IOHIDDeviceRef dev, CFStringRef key) {
    if (!dev || !key) {
        return dev ? IOHIDDeviceGetProperty(dev, key) : NULL;
    }

    if (g_in_is_dualsense) {
        return IOHIDDeviceGetProperty(dev, key);
    }

    if (is_dualsense(dev)) {
        init_spoof_constants();
        init_dualsense_bridge(dev);

        if (CFEqual(key, CFSTR(kIOHIDVendorIDKey))) {
            return g_ds4_vid;
        }
        if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) {
            return g_ds4_pid;
        }
        if (CFEqual(key, CFSTR(kIOHIDVersionNumberKey))) {
            return g_ds4_ver;
        }
        if (CFEqual(key, CFSTR(kIOHIDTransportKey))) {
            return g_ds4_xprt;
        }
        if (CFEqual(key, CFSTR(kIOHIDManufacturerKey))) {
            return g_ds4_mfr;
        }
        if (CFEqual(key, CFSTR(kIOHIDProductKey))) {
            return g_ds4_prod;
        }
    }

    return IOHIDDeviceGetProperty(dev, key);
}

/* Interposed IOHIDDeviceSetReport (absorb DS4 rumble/LED reports safely) */
IOReturn my_IOHIDDeviceSetReport(IOHIDDeviceRef dev, IOHIDReportType type, CFIndex id,
                                 const uint8_t *report, CFIndex len) {
    if (is_dualsense(dev)) {
        return kIOReturnSuccess;
    }
    return IOHIDDeviceSetReport(dev, type, id, report, len);
}

/* Interposed IOHIDDeviceGetValue (Lock-free state lookup for InControlNative) */
IOReturn my_IOHIDDeviceGetValue(IOHIDDeviceRef dev, IOHIDElementRef elem, IOHIDValueRef *pVal) {
    if (!dev || !elem || !pVal) {
        return dev ? IOHIDDeviceGetValue(dev, elem, pVal) : kIOReturnBadArgument;
    }

    if (is_dualsense(dev)) {
        init_dualsense_bridge(dev);

        uint32_t up = IOHIDElementGetUsagePage(elem);
        uint32_t u  = IOHIDElementGetUsage(elem);

        /* 1. Buttons (Usages 1..14) */
        if (up == 0x09 && u >= 1 && u <= 14) {
            int pressed = atomic_load(&g_btn_state[u]) ? 1 : 0;
            if (g_cached_btn_vals[u][pressed]) {
                *pVal = g_cached_btn_vals[u][pressed];
                return kIOReturnSuccess;
            }
        }

        /* 2. Hat Switch / D-Pad (Usage 0x39) */
        if (up == 0x01 && u == 0x39) {
            int hat = atomic_load(&g_hat_state);
            if (hat > 8) hat = 8;
            if (g_cached_hat_vals[hat]) {
                *pVal = g_cached_hat_vals[hat];
                return kIOReturnSuccess;
            }
        }

        /* 3. Analog Sticks & Triggers (Usages 0x30..0x35) */
        if (up == 0x01) {
            int ax = -1;
            switch (u) {
                case 0x30: ax = 0; break; /* LX */
                case 0x31: ax = 1; break; /* LY */
                case 0x32: ax = 2; break; /* RX */
                case 0x35: ax = 3; break; /* RY */
                case 0x33: ax = 4; break; /* L2 */
                case 0x34: ax = 5; break; /* R2 */
                default: break;
            }
            if (ax >= 0) {
                int v = atomic_load(&g_axes_state[ax]);
                if (g_cached_axis_vals[ax][v]) {
                    *pVal = g_cached_axis_vals[ax][v];
                    return kIOReturnSuccess;
                }
            }
        }
    }

    return IOHIDDeviceGetValue(dev, elem, pVal);
}

/* Interposed IOHIDValueGetIntegerValue */
CFIndex my_IOHIDValueGetIntegerValue(IOHIDValueRef value) {
    if (!value) return 0;

    IOHIDElementRef elem = IOHIDValueGetElement(value);
    if (elem && is_dualsense(IOHIDElementGetDevice(elem))) {
        uint32_t up = IOHIDElementGetUsagePage(elem);
        uint32_t u  = IOHIDElementGetUsage(elem);

        /* 1. Buttons (Usages 1..14) */
        if (up == 0x09 && u >= 1 && u <= 14) {
            if (value == g_cached_btn_vals[u][1]) return 1;
            if (value == g_cached_btn_vals[u][0]) return 0;
            return atomic_load(&g_btn_state[u]) ? 1 : 0;
        }

        /* 2. Hat Switch / D-Pad (Usage 0x39) */
        if (up == 0x01 && u == 0x39) {
            for (int h = 0; h <= 8; h++) {
                if (value == g_cached_hat_vals[h]) return h;
            }
            return atomic_load(&g_hat_state);
        }

        /* 3. Analog Sticks & Triggers (Usages 0x30..0x35) */
        if (up == 0x01) {
            int ax = -1;
            switch (u) {
                case 0x30: ax = 0; break; /* LX */
                case 0x31: ax = 1; break; /* LY */
                case 0x32: ax = 2; break; /* RX */
                case 0x35: ax = 3; break; /* RY */
                case 0x33: ax = 4; break; /* L2 */
                case 0x34: ax = 5; break; /* R2 */
                default: break;
            }
            if (ax >= 0) {
                return atomic_load(&g_axes_state[ax]);
            }
        }
    }

    return IOHIDValueGetIntegerValue(value);
}

/*
 * Interposed IOHIDQueueCopyNextValueWithTimeout:
 * ZERO-LATENCY COALESCED DESIGN:
 * 1. Drain ALL pending raw events from Unity's `queue` immediately on every call so the kernel
 *    IOHIDQueue NEVER builds up a backlog of stale 0xff00:0x3b reports.
 * 2. Return discrete button/hat transitions from `g_synth_queue`.
 * 3. Return at most ONE coalesced event per dirty analog axis (0..5) with the exact current
 *    instantaneous position (`g_axes_state[ax]`), eliminating all FIFO stick lag!
 */
IOHIDValueRef my_IOHIDQueueCopyNextValueWithTimeout(IOHIDQueueRef queue, CFTimeInterval timeout) {
    if (!queue) return NULL;

    /* If this is our private queue used by reader_thread, bypass directly to IOKit */
    if (queue == g_priv_queue) {
        return IOHIDQueueCopyNextValueWithTimeout(queue, timeout);
    }

    IOHIDDeviceRef qdev = IOHIDQueueGetDevice(queue);
    bool is_ds_queue = is_dualsense(qdev);

    if (is_ds_queue) {
        init_dualsense_bridge(qdev);

        /* 1. Drain ALL pending raw events in Unity's queue so it never lags behind real-time */
        IOHIDValueRef raw = NULL;
        while ((raw = IOHIDQueueCopyNextValueWithTimeout(queue, 0.0)) != NULL) {
            IOHIDElementRef el = IOHIDValueGetElement(raw);
            if (el && IOHIDElementGetUsagePage(el) == 0xff00 && IOHIDElementGetUsage(el) == 0x3b) {
                parse_vendor_0x3b_value(raw);
            }
            CFRelease(raw);
        }

        /* 2. Pop discrete button or D-Pad transitions first */
        IOHIDValueRef synth = pop_synth_event();
        if (synth) {
            return synth;
        }

        /* 3. Return coalesced instantaneous analog axis updates (at most 1 per axis per poll) */
        for (int ax = 0; ax < 6; ax++) {
            if (atomic_exchange(&g_axis_dirty[ax], 0) != 0) {
                int v = atomic_load(&g_axes_state[ax]);
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                IOHIDValueRef axis_val = g_cached_axis_vals[ax][v];
                if (axis_val) {
                    CFRetain(axis_val);
                    return axis_val;
                }
            }
        }

        return NULL;
    }

    /* Non-DualSense queue (e.g., keyboard/mouse): pass through to IOKit */
    return IOHIDQueueCopyNextValueWithTimeout(queue, timeout);
}

__attribute__((constructor)) static void shim_init(void) {
    FILE *fp = fopen(get_log_path(), "w");
    if (fp) fclose(fp);

    log_msg("\n==========================================================\n");
    log_msg("  [ds4_shim] Initializing DualSense -> DualShock 4 Bridge...\n");
    log_msg("  Process PID: %d\n", getpid());
    install_gc_swizzles();
    init_spoof_constants();

    pthread_t disc_th;
    pthread_create(&disc_th, NULL, discovery_thread, NULL);
    pthread_detach(disc_th);

    log_msg("  [ds4_shim] Initialization complete!\n");
    log_msg("==========================================================\n\n");
}

/* Dyld interpose registration table */
typedef struct {
    const void *replacement;
    const void *replacee;
} interpose_t;

__attribute__((used)) static interpose_t interposers[] __attribute__((section("__DATA, __interpose"))) = {
    { (const void *)my_dlsym,                              (const void *)dlsym },
    { (const void *)my_IOHIDDeviceGetProperty,             (const void *)IOHIDDeviceGetProperty },
    { (const void *)my_IOHIDDeviceSetReport,               (const void *)IOHIDDeviceSetReport },
    { (const void *)my_IOHIDDeviceGetValue,                (const void *)IOHIDDeviceGetValue },
    { (const void *)my_IOHIDValueGetIntegerValue,          (const void *)IOHIDValueGetIntegerValue },
    { (const void *)my_IOHIDQueueCopyNextValueWithTimeout, (const void *)IOHIDQueueCopyNextValueWithTimeout },
};
