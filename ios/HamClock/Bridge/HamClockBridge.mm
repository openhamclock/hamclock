//
//  HamClockBridge.mm
//  HamClock
//
//  Objective-C++ Bridge to native C++ HamClock Core Engine.
//

#import "HamClockBridge.h"
#include <string>
#include <vector>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>

#include "HamClock.h"
#include "qrcodegen.h"

extern int hamclock_main(int ac, char *av[]);

struct DaemonArgs {
    std::string dataDir;
    int rwPort;
    int roPort;
    int restPort;
    std::string backendHost;
    bool hasLocation;
    double lat;
    double lng;
    bool forceSetup;
    bool countdownSetup;
};

static pthread_t daemon_thread;
static bool daemon_running = false;
static std::string cached_data_dir;
static bool allow_external_access = false;
static __weak HamClockBridge *g_bridge_instance = nil;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
extern "C" std::string __wrap__ZN4WiFi10macAddressEv() {
#pragma clang diagnostic pop
    char mac_buf[32] = {0};
    std::string mac_file = cached_data_dir.empty() ? "" : (cached_data_dir + "/.mac_address");

    if (!mac_file.empty()) {
        FILE *fp = fopen(mac_file.c_str(), "r");
        if (fp) {
            if (fgets(mac_buf, sizeof(mac_buf), fp)) {
                char *nl = strchr(mac_buf, '\n');
                if (nl) *nl = '\0';
                unsigned int m1, m2, m3, m4, m5, m6;
                if (sscanf(mac_buf, "%x:%x:%x:%x:%x:%x", &m1, &m2, &m3, &m4, &m5, &m6) == 6) {
                    fclose(fp);
                    return std::string(mac_buf);
                }
            }
            fclose(fp);
        }
    }

    // Generate locally administered unicast MAC (02:xx:xx:xx:xx:xx)
    uint8_t rand_bytes[6] = {0};
    arc4random_buf(rand_bytes, 6);
    rand_bytes[0] = (rand_bytes[0] & 0xFE) | 0x02; // locally administered unicast

    snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             rand_bytes[0], rand_bytes[1], rand_bytes[2],
             rand_bytes[3], rand_bytes[4], rand_bytes[5]);

    if (!mac_file.empty()) {
        FILE *fp = fopen(mac_file.c_str(), "w");
        if (fp) {
            fprintf(fp, "%s\n", mac_buf);
            fclose(fp);
        }
    }

    NSLog(@"[HamClockBridge] Generated persistent iOS MAC: %s", mac_buf);
    return std::string(mac_buf);
}

static void *setup_injector_worker(void * /* arg */) {
    NSLog(@"[HamClockBridge] Setup injector started: waiting for displayReady...");
    for (int i = 0; i < 50; i++) {
        if (tft.displayReady()) break;
        usleep(50000); // 50ms
    }
    usleep(150000); // 150ms
    NSLog(@"[HamClockBridge] Setup injector: injecting virtual tap to enter Setup");
    for (int i = 0; i < 20; i++) {
        if (wifi_tt == TT_NONE) {
            wifi_tt_s.x = 200;
            wifi_tt_s.y = 200;
            wifi_tt = TT_TAP;
        }
        usleep(50000); // 50ms
        if (wifi_tt == TT_NONE) {
            NSLog(@"[HamClockBridge] Setup injector: virtual tap consumed by askRun()");
            break;
        }
    }
    return nullptr;
}

static void *daemon_worker(void *arg) {
    DaemonArgs *dargs = static_cast<DaemonArgs *>(arg);
    cached_data_dir = dargs->dataDir;
    our_dir = dargs->dataDir + "/";

    // Preset NTP to Computer (OS) time if unconfigured
    uint8_t ntp_val = 0;
    if (!NVReadUInt8(NV_NTPSET, &ntp_val) || ntp_val == 0) {
        NVWriteUInt8(NV_NTPSET, 2); // NTPSC_OS ("Computer")
        NSLog(@"[HamClockBridge] Preset NTP to Computer (OS) time");
    }

    // If host GPS location is available, update DE location
    if (dargs->hasLocation) {
        LatLong ll;
        ll.lat_d = dargs->lat;
        ll.lng_d = dargs->lng;
        ll.normalize();

        NVWriteFloat(NV_DE_LAT, ll.lat_d);
        NVWriteFloat(NV_DE_LNG, ll.lng_d);
        setNVMaidenhead(NV_DE_GRID, ll);
        setTZAuto(de_tz);
        NVWriteTZ(NV_DE_TZ, de_tz);

        char grid[MAID_CHARLEN] = {0};
        getNVMaidenhead(NV_DE_GRID, grid);
        NSLog(@"[HamClockBridge] Updated DE from host GPS: %.4f, %.4f (Grid: %s, TZ: %d min)",
              ll.lat_d, ll.lng_d, grid, de_tz.tz_secs / 60);
    }

    snprintf(platform, sizeof(platform), "HamClock-iOS");

    std::string progName = "hamclock-ios";
    std::string dirFlag = "-d";
    std::string dirVal = dargs->dataDir;
    std::string rwFlag = "-w";
    std::string rwVal = std::to_string(dargs->rwPort);
    std::string roFlag = "-r";
    std::string roVal = std::to_string(dargs->roPort);
    std::string restFlag = "-e";
    std::string restVal = std::to_string(dargs->restPort);
    std::string throtFlag = "-t";
    std::string throtVal = "80";
    std::string skipFlag = "-k";
    std::string geoFlag = "-g";
    std::string bFlag = "-b";
    std::string bVal = dargs->backendHost;
    bool hasLoc = dargs->hasLocation;
    bool forceSetup = dargs->forceSetup;
    bool countdownSetup = dargs->countdownSetup;

    if (forceSetup) {
        NSLog(@"[HamClockBridge] Direct setup requested: starting setup injector thread");
        pthread_t injector_thread;
        if (pthread_create(&injector_thread, nullptr, setup_injector_worker, nullptr) == 0) {
            pthread_detach(injector_thread);
        }
    } else if (countdownSetup) {
        NSLog(@"[HamClockBridge] Restart with 10s countdown requested");
    }

    delete dargs;

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(progName.c_str()));
    argv.push_back(const_cast<char *>(dirFlag.c_str()));
    argv.push_back(const_cast<char *>(dirVal.c_str()));
    argv.push_back(const_cast<char *>(rwFlag.c_str()));
    argv.push_back(const_cast<char *>(rwVal.c_str()));
    argv.push_back(const_cast<char *>(roFlag.c_str()));
    argv.push_back(const_cast<char *>(roVal.c_str()));
    argv.push_back(const_cast<char *>(restFlag.c_str()));
    argv.push_back(const_cast<char *>(restVal.c_str()));
    argv.push_back(const_cast<char *>(throtFlag.c_str()));
    argv.push_back(const_cast<char *>(throtVal.c_str()));
    if (!forceSetup && !countdownSetup) {
        argv.push_back(const_cast<char *>(skipFlag.c_str()));
        if (!hasLoc) {
            argv.push_back(const_cast<char *>(geoFlag.c_str()));
            NSLog(@"[HamClockBridge] Host GPS location not available: falling back to GeoIP (-g)");
        }
    }
    if (!bVal.empty()) {
        argv.push_back(const_cast<char *>(bFlag.c_str()));
        argv.push_back(const_cast<char *>(bVal.c_str()));
    }

    argv.push_back(nullptr);
    int argc = static_cast<int>(argv.size() - 1);

    NSLog(@"[HamClockBridge] Starting HamClock daemon with argc=%d in dir=%s on rw_port=%s",
          argc, dirVal.c_str(), rwVal.c_str());
    hamclock_main(argc, argv.data());

    NSLog(@"[HamClockBridge] HamClock daemon exited");
    daemon_running = false;
    return nullptr;
}

// C-linkage hooks called by HamClock C++ core
extern "C" {

void ios_request_exit(void) {
    NSLog(@"[HamClockBridge] ios_request_exit invoked from C++");
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_bridge_instance && [g_bridge_instance.delegate respondsToSelector:@selector(hamclockExitRequested)]) {
            [g_bridge_instance.delegate hamclockExitRequested];
        } else {
            exit(0);
        }
    });
}

void ios_request_restart(bool minus_K) {
    NSLog(@"[HamClockBridge] ios_request_restart invoked from C++ (minus_K=%d)", minus_K);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_bridge_instance && [g_bridge_instance.delegate respondsToSelector:@selector(hamclockRestartRequestedWithMinusK:)]) {
            [g_bridge_instance.delegate hamclockRestartRequestedWithMinusK:minus_K];
        }
    });
}

void ios_open_url(const char *url) {
    if (!url) return;
    NSString *urlString = [NSString stringWithUTF8String:url];
    if (!urlString) return;
    NSURL *nsUrl = [NSURL URLWithString:urlString];
    if (!nsUrl) return;

    NSLog(@"[HamClockBridge] ios_open_url invoked with: %@", urlString);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_bridge_instance && [g_bridge_instance.delegate respondsToSelector:@selector(hamclockOpenURL:)]) {
            [g_bridge_instance.delegate hamclockOpenURL:nsUrl];
        } else {
            [[UIApplication sharedApplication] openURL:nsUrl options:@{} completionHandler:nil];
        }
    });
}

bool ios_get_clipboard(char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return false;
    buf[0] = '\0';

    __block NSString *clipText = nil;
    if ([NSThread isMainThread]) {
        if (g_bridge_instance && [g_bridge_instance.delegate respondsToSelector:@selector(hamclockGetClipboardText)]) {
            clipText = [g_bridge_instance.delegate hamclockGetClipboardText];
        } else {
            clipText = [UIPasteboard generalPasteboard].string;
        }
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{
            if (g_bridge_instance && [g_bridge_instance.delegate respondsToSelector:@selector(hamclockGetClipboardText)]) {
                clipText = [g_bridge_instance.delegate hamclockGetClipboardText];
            } else {
                clipText = [UIPasteboard generalPasteboard].string;
            }
        });
    }

    if (clipText && clipText.length > 0) {
        snprintf(buf, buf_len, "%s", [clipText UTF8String]);
        return (buf[0] != '\0');
    }
    return false;
}

bool ios_connect_http(const char *cmd, int *read_fd) {
    if (!cmd || !read_fd) return false;
    std::string cmdStr(cmd);
    std::string url;
    std::string userAgent;
    std::string header;

    // Parse URL (http:// or https://)
    size_t urlPos = cmdStr.find("https://");
    if (urlPos == std::string::npos) {
        urlPos = cmdStr.find("http://");
    }
    if (urlPos != std::string::npos) {
        size_t endPos = cmdStr.find_first_of(" \t\r\n\"", urlPos);
        if (endPos == std::string::npos) {
            url = cmdStr.substr(urlPos);
        } else {
            url = cmdStr.substr(urlPos, endPos - urlPos);
        }
    }

    if (url.empty()) {
        NSLog(@"[HamClockBridge] ios_connect_http: No URL found in command: %s", cmd);
        return false;
    }

    // Parse User-Agent (-A "...")
    size_t uaFlag = cmdStr.find("-A ");
    if (uaFlag != std::string::npos) {
        size_t start = uaFlag + 3;
        while (start < cmdStr.size() && (cmdStr[start] == ' ' || cmdStr[start] == '\t')) start++;
        if (start < cmdStr.size()) {
            char quote = cmdStr[start];
            if (quote == '"' || quote == '\'') {
                size_t end = cmdStr.find(quote, start + 1);
                if (end != std::string::npos) {
                    userAgent = cmdStr.substr(start + 1, end - start - 1);
                }
            } else {
                size_t end = cmdStr.find_first_of(" \t", start);
                userAgent = (end == std::string::npos) ? cmdStr.substr(start) : cmdStr.substr(start, end - start);
            }
        }
    }

    // Parse Header (-H "...")
    size_t hFlag = cmdStr.find("-H ");
    if (hFlag != std::string::npos) {
        size_t start = hFlag + 3;
        while (start < cmdStr.size() && (cmdStr[start] == ' ' || cmdStr[start] == '\t')) start++;
        if (start < cmdStr.size()) {
            char quote = cmdStr[start];
            if (quote == '"' || quote == '\'') {
                size_t end = cmdStr.find(quote, start + 1);
                if (end != std::string::npos) {
                    header = cmdStr.substr(start + 1, end - start - 1);
                }
            } else {
                size_t end = cmdStr.find_first_of(" \t", start);
                header = (end == std::string::npos) ? cmdStr.substr(start) : cmdStr.substr(start, end - start);
            }
        }
    }

    int pipefds[2];
    if (pipe(pipefds) != 0) {
        NSLog(@"[HamClockBridge] ios_connect_http: pipe failed: %s", strerror(errno));
        return false;
    }

    NSString *urlString = [NSString stringWithUTF8String:url.c_str()];
    NSURL *nsUrl = [NSURL URLWithString:urlString];
    if (!nsUrl) {
        close(pipefds[0]);
        close(pipefds[1]);
        return false;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:nsUrl
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:15.0];
    if (!userAgent.empty()) {
        [request setValue:[NSString stringWithUTF8String:userAgent.c_str()] forHTTPHeaderField:@"User-Agent"];
    }
    if (!header.empty()) {
        size_t colon = header.find(':');
        if (colon != std::string::npos) {
            std::string hName = header.substr(0, colon);
            std::string hVal = header.substr(colon + 1);
            [request setValue:[NSString stringWithUTF8String:hVal.c_str()]
           forHTTPHeaderField:[NSString stringWithUTF8String:hName.c_str()]];
        }
    }

    int writeFd = pipefds[1];
    NSURLSession *session = [NSURLSession sharedSession];
    NSURLSessionDataTask *task = [session dataTaskWithRequest:request
                                            completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (data && data.length > 0) {
            const uint8_t *bytes = (const uint8_t *)data.bytes;
            size_t remaining = data.length;
            while (remaining > 0) {
                ssize_t written = write(writeFd, bytes, remaining);
                if (written <= 0) break;
                bytes += written;
                remaining -= written;
            }
        }
        close(writeFd);
    }];
    [task resume];

    *read_fd = pipefds[0];
    return true;
}

} // extern "C"

@implementation HamClockBridge

+ (instancetype)shared {
    static HamClockBridge *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[HamClockBridge alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        g_bridge_instance = self;
    }
    return self;
}

- (BOOL)startDaemonWithDataDir:(NSString *)dataDir
                        rwPort:(int)rwPort
                        roPort:(int)roPort
                      restPort:(int)restPort
                   backendHost:(nullable NSString *)backendHost
                   hasLocation:(BOOL)hasLocation
                           lat:(double)lat
                           lng:(double)lng
                    forceSetup:(BOOL)forceSetup
                countdownSetup:(BOOL)countdownSetup {
    if (daemon_running) {
        NSLog(@"[HamClockBridge] HamClock daemon already running");
        return YES;
    }

    DaemonArgs *args = new DaemonArgs();
    args->dataDir = [dataDir UTF8String];
    args->rwPort = rwPort;
    args->roPort = roPort;
    args->restPort = restPort;
    args->backendHost = backendHost ? [backendHost UTF8String] : "";
    args->hasLocation = hasLocation;
    args->lat = lat;
    args->lng = lng;
    args->forceSetup = forceSetup;
    args->countdownSetup = countdownSetup;

    daemon_running = true;
    if (pthread_create(&daemon_thread, nullptr, daemon_worker, args) != 0) {
        NSLog(@"[HamClockBridge] Failed to create daemon thread");
        delete args;
        daemon_running = false;
        return NO;
    }

    pthread_detach(daemon_thread);
    return YES;
}

- (BOOL)isDaemonRunning {
    return daemon_running;
}

- (void)setAllowExternalAccess:(BOOL)allow {
    allow_external_access = allow;
    NSLog(@"[HamClockBridge] External local network access restriction set to: %@",
          allow ? @"ENABLED (RFC1918 LANs allowed)" : @"DISABLED (Loopback only)");
}

- (nullable UIImage *)generateQRCodeImageForText:(NSString *)text
                                           scale:(int)scale
                                          border:(int)border {
    if (!text || text.length == 0) return nil;

    uint8_t qr0[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText([text UTF8String], tempBuffer, qr0,
                                   qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, 10,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) return nil;

    int qr_size = qrcodegen_getSize(qr0);
    int total_modules = qr_size + 2 * border;
    int img_size = total_modules * scale;

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    size_t bytesPerPixel = 4;
    size_t bytesPerRow = bytesPerPixel * img_size;
    size_t bitsPerComponent = 8;

    std::vector<uint32_t> rawPixels(img_size * img_size, 0xFFFFFFFF); // White RGBA

    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (qrcodegen_getModule(qr0, x, y)) {
                int px0 = (x + border) * scale;
                int py0 = (y + border) * scale;
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        rawPixels[(py0 + dy) * img_size + (px0 + dx)] = 0xFF000000; // Black RGBA
                    }
                }
            }
        }
    }

    CGContextRef context = CGBitmapContextCreate(rawPixels.data(), img_size, img_size,
                                                 bitsPerComponent, bytesPerRow, colorSpace,
                                                 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGImageRef cgImage = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);

    if (!cgImage) return nil;
    UIImage *image = [UIImage imageWithCGImage:cgImage];
    CGImageRelease(cgImage);
    return image;
}

@end
