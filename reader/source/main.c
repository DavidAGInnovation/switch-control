// Read-only diagnostics through the debugger already owned by Atmosphere.
#define NX_SERVICE_ASSUME_NON_DOMAIN
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct { u64 base, size; } Extents;
typedef struct {
    u64 process_id, title_id;
    Extents main, heap, alias, address_space;
    u8 build_id[32];
} Metadata;
_Static_assert(sizeof(Metadata) == 0x70, "dmnt metadata ABI");
static Service dmnt;
static u64 reader_started;
static Result sleep_set_result, sleep_query_result;
static bool sleep_disabled;
static u8 bytes[0x10000];
static char response[sizeof(bytes) * 2 + 2];
static const u8 expected_build[8] = {0xb1,0xf1,0x2f,0xd9,0x19,0xea,0xe8,0x6a};

#ifdef ZA_READER_USB
// One exact-size bulk transfer per frame avoids short-packet/ZLP ambiguity.
// Four-byte LE payload length, then up to 4092 bytes; unused bytes are zero.
static u8 usb_in[0x1000] __attribute__((aligned(0x1000)));
static u8 usb_out[0x1000] __attribute__((aligned(0x1000)));
static bool usb_ready;
#endif

static bool send_all(int fd, const char *data, size_t size) {
#ifdef ZA_READER_USB
    if (fd >= 0) {
#ifdef ZA_READER_TLS
        return wifi_tls_write_all(fd, data, size);
#else
        while (size) {
            ssize_t n = send(fd, data, size, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
#endif
    }
    while (size && usb_ready) {
        const size_t chunk = size < sizeof(usb_out)-4 ? size : sizeof(usb_out)-4;
        memset(usb_out, 0, sizeof(usb_out));
        const u32 length = (u32)chunk;
        memcpy(usb_out, &length, sizeof(length));
        memcpy(usb_out+4, data, chunk);
        u32 urb = 0, transferred = 0;
        Result rc = usbCommsWriteAsync(usb_out, sizeof(usb_out), &urb, 0);
        if (R_SUCCEEDED(rc)) rc = eventWait(usbCommsGetWriteCompletionEvent(0), 2000000000ULL);
        if (R_SUCCEEDED(rc)) rc = usbCommsGetWriteResult(urb, &transferred, 0);
        if (R_FAILED(rc) || transferred != sizeof(usb_out)) {
            printf("\nUSB write stopped: %08X (%u bytes). Press + to exit.\n", rc, transferred);
            usb_ready = false;
            return false;
        }
        data += chunk;
        size -= chunk;
    }
    return size == 0;
#else
    while (size) {
        ssize_t sent = send(fd, data, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        size -= sent;
    }
    return true;
#endif
}

static bool error_reply(int fd, Result rc) {
    char text[32];
    int n = snprintf(text, sizeof(text), "ERR %08X\n", rc);
    return send_all(fd, text, n);
}

static Result metadata(Metadata *m) {
    return serviceDispatchOut(&dmnt, 65002, *m);
}

static Result read_memory(u64 address, void *buffer, size_t size) {
    const struct { u64 address, size; } in = { address, size };
    return serviceDispatchIn(&dmnt, 65102, in,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { buffer, size } });
}

static bool class_objects(int fd, u64 main_base, u64 descriptor) {
    u64 pool = 0;
    Result rc = read_memory(main_base + 0x3EC21C8, &pool, sizeof(pool));
    if (R_FAILED(rc)) return error_reply(fd, rc);
    size_t used = 0;
    // This build's fixed 2^20-entry handle table. Scan locally to avoid sending
    // 32 MiB of unrelated objects over Wi-Fi. The descriptor was verified on
    // live actor objects through their +0x2038 core/tag relationship.
    for (u64 offset = 0; offset < 0x2000000; offset += sizeof(bytes)) {
        rc = read_memory(pool + 0x10 + offset, bytes, sizeof(bytes));
        if (R_FAILED(rc)) return error_reply(fd, rc);
        for (size_t i = 0; i < sizeof(bytes); i += 32) {
            u64 record[4];
            memcpy(record, bytes + i, sizeof(record));
            if ((record[0] & 0xFFFFF) != (offset + i)/32 ||
                record[1] != main_base + descriptor || !record[2] ||
                !(record[3] & 0xFFFF) || !(record[3] & 0xFFFF0000) ||
                (record[3] >> 32) == UINT32_MAX) continue;
            if (used + 18 >= sizeof(response))
                return send_all(fd, "ERR too-many-actors\n", 20);
            used += snprintf(response + used, sizeof(response)-used,
                             "%016" PRIX64 " ", record[2] - 0x28);
        }
    }
    response[used++] = '\n';
    return send_all(fd, response, used);
}

// Identify the serialized action-12800 root using its actual FlatBuffers
// vtable, version, actor-settings and action-data fields. No process writes.
static u16 get16(const u8 *p) { u16 x; memcpy(&x,p,2); return x; }
static u32 get32(const u8 *p) { u32 x; memcpy(&x,p,4); return x; }
static bool capture_root(const u8 *b,size_t n,size_t at,size_t *root) {
    if(at+4>n || get32(b+at)!=12800)return false;
    for(size_t d=4;d<=128&&d<=at;d+=4){
        size_t t=at-d;
        if(t+4>n)continue;
        s32 back;memcpy(&back,b+t,4);
        s64 v=(s64)t-back;
        if(v<0||(u64)v+28>n)continue;
        u16 vl=get16(b+v),ol=get16(b+v+2);
        if(vl<28||vl>96||(u64)v+vl>n||ol<d+4||ol>512||t+ol>n)continue;
        if(get16(b+v+24)!=d)continue;
        u16 ver=get16(b+v+4),actors=get16(b+v+10),actions=get16(b+v+26);
        if(ver<4||ver+4>ol||get32(b+t+ver)!=6||!actors||!actions)continue;
        *root=t;return true;
    }
    return false;
}
static bool capture_templates(int fd,const Metadata *m) {
    static u8 block[0x10000];
    size_t used=0;u64 previous=0,scanned=0,address=0;unsigned found=0,maps=0;
    unsigned regions=0;Result first_error=0;
    // MapPhysicalMemory allocations may be outside the advertised heap region.
    // Query actual mappings instead of assuming heap extents contain game data.
    while(address<0x8000000000ULL){
        MemoryInfo mi={0};Result rc=serviceDispatchInOut(&dmnt,65104,address,mi);
        if(R_FAILED(rc))return error_reply(fd,rc);
        if(!mi.size||mi.addr>address||mi.addr>UINT64_MAX-mi.size)
            return send_all(fd,"ERR mapping-layout\n",19);
        u64 end=mi.addr+mi.size;
        if(end<=address)return send_all(fd,"ERR mapping-order\n",18);
        if(++maps>16384)return send_all(fd,"ERR mapping-limit\n",18);
        if((mi.perm&3)==3 && !(mi.perm&4)){
            regions++;
            for(u64 at=mi.addr;at<end;){
                size_t len=(size_t)(end-at);if(len>sizeof(block))len=sizeof(block);
                rc=read_memory(at,block,len);
                if(R_FAILED(rc)){if(!first_error)first_error=rc;}
                else{
                    scanned+=len;
                    for(size_t i=0;i+4<=len;i+=4){
                        size_t root=0;if(!capture_root(block,len,i,&root))continue;
                        u64 hit=at+root;if(hit==previous)continue;
                        if(found>=64)return send_all(fd,"ERR too-many-templates\n",23);
                        previous=hit;found++;
                        used+=snprintf(response+used,sizeof(response)-used,"%016" PRIX64 " ",hit);
                    }
                }
                if(end-at<=sizeof(block))break;
                at+=sizeof(block)-0x1000;
            }
        }
        address=end;
    }
    (void)m;
    used+=snprintf(response+used,sizeof(response)-used,"count=%u regions=%u scanned=%" PRIu64 " error=%08X\n",found,regions,scanned,first_error);
    return send_all(fd,response,used);
}

// Locate the two exact name keys used by this build's reticle page factories.
// This inspects writable mappings without modifying UI state or attaching a
// second debugger. Each aligned word is visited once; replies are bounded.
static bool reticle_names(int fd,const Metadata *m) {
    const u64 page_key=0x5456B662AE6F57E7ULL;
    const u64 layout_key=0x81B471D8FC944CEBULL; // gitleaks:allow -- fixed game UI identifier, not a credential
    size_t used=0;u64 scanned=0,address=0;unsigned found=0,maps=0,regions=0;
    Result first_error=0;
    while(address<0x8000000000ULL){
        MemoryInfo mi={0};Result rc=serviceDispatchInOut(&dmnt,65104,address,mi);
        if(R_FAILED(rc))return error_reply(fd,rc);
        if(!mi.size||mi.addr>address||mi.addr>UINT64_MAX-mi.size)
            return send_all(fd,"ERR mapping-layout\n",19);
        u64 end=mi.addr+mi.size;
        if(end<=address)return send_all(fd,"ERR mapping-order\n",18);
        if(++maps>16384)return send_all(fd,"ERR mapping-limit\n",18);
        if((mi.perm&3)==3 && !(mi.perm&4)){
            regions++;
            for(u64 at=mi.addr;at<end;){
                size_t len=(size_t)(end-at);if(len>sizeof(bytes))len=sizeof(bytes);
                rc=read_memory(at,bytes,len);
                if(R_FAILED(rc)){if(!first_error)first_error=rc;}
                else{
                    scanned+=len;
                    for(size_t i=0;i+8<=len;i+=8){
                        u64 key;memcpy(&key,bytes+i,sizeof(key));
                        if(key!=page_key && key!=layout_key &&
                           key!=m->main.base+0x3DBB348 &&
                           key!=m->main.base+0x3DBB620)continue;
                        if(found>=512)return send_all(fd,"ERR too-many-reticle-names\n",27);
                        found++;
                        used+=snprintf(response+used,sizeof(response)-used,
                            "%016" PRIX64 ":%016" PRIX64 " ",at+i,key);
                    }
                }
                at+=len;
            }
        }
        address=end;
    }
    used+=snprintf(response+used,sizeof(response)-used,
        "count=%u regions=%u scanned=%" PRIu64 " error=%08X\n",
        found,regions,scanned,first_error);
    return send_all(fd,response,used);
}

static bool command(int fd, const char *line) {
    if (!strcmp(line, "getTransport")) {
#ifdef ZA_READER_USB
        const char transport[] = "USB ZAU1 frame=4096 payload=4092 read-only\n";
        return send_all(fd, transport, sizeof(transport)-1);
#else
        return send_all(fd, "TCP read-only\n", 14);
#endif
    }
    if (!strcmp(line, "getReaderStatus")) {
        sleep_query_result = appletIsAutoSleepDisabled(&sleep_disabled);
        char text[160];
        int n = snprintf(text, sizeof(text),
            "READER uptime_ms=%" PRIu64 " sleep_set=%08X sleep_query=%08X disabled=%u\n",
            armTicksToNs(svcGetSystemTick()-reader_started)/1000000,
            sleep_set_result, sleep_query_result, sleep_disabled);
        return send_all(fd, text, (size_t)n);
    }
    if (!strcmp(line, "openForeground")) {
        // Never displace an existing cheat process (which may own active
        // cheats). Force-open is explicit, not a side effect of reading.
        bool active = false;
        Result rc = serviceDispatchOut(&dmnt, 65000, active);
        if (R_FAILED(rc)) return error_reply(fd, rc);
        if (active) return send_all(fd, "ALREADY OPEN\n", 13);
        rc = serviceDispatch(&dmnt, 65003);
        if (R_FAILED(rc)) return error_reply(fd, rc);
        return send_all(fd, "OK\n", 3);
    }
    Metadata m = {0};
    Result rc = metadata(&m);
    if (R_FAILED(rc)) return error_reply(fd, rc);
    const bool za_build = m.title_id == 0x0100F43008C44000ULL &&
                          !memcmp(m.build_id, expected_build, sizeof(expected_build));
#ifndef ZA_READER_RESIDENT
    if (!strcmp(line, "getPausedCapture")) {
        // Read AM's existing last-application frame. Do not call either update
        // function: the game must stay paused at the owner's HOME press.
        static u8 capture[0x384000] __attribute__((aligned(0x1000)));
        static u8 half[640*360*4];
        bool valid = false;
        Result image_rc = appletGetLastApplicationCaptureImageEx(capture, sizeof(capture), &valid);
        if (R_FAILED(image_rc)) return error_reply(fd, image_rc);
        for (size_t y=0; y<360; ++y)
            for (size_t x=0; x<640; ++x)
                memcpy(half+(y*640+x)*4, capture+((y*2)*1280+x*2)*4, 4);
        char header[80];
        int len=snprintf(header,sizeof(header),"CAPTURE %u 640 360 %zu\n",valid?1:0,sizeof(half));
        return send_all(fd,header,(size_t)len) && send_all(fd,(const char*)half,sizeof(half));
    }
#endif
    // These object layouts and addresses are specific to this exact Z-A build.
    // Generic metadata and byte reads below operate on dmnt's current process.
    const bool za_command = !strcmp(line, "findCaptureTemplates") ||
                            !strcmp(line, "findReticleNames") ||
                            !strcmp(line, "getActors") ||
                            !strcmp(line, "getTagComponents") ||
                            !strncmp(line, "findClass ", 10);
    if (za_command && !za_build)
        return send_all(fd, "ERR wrong-game-or-build\n", 24);
    if (!strcmp(line, "findCaptureTemplates")) return capture_templates(fd, &m);
    if (!strcmp(line, "findReticleNames")) return reticle_names(fd, &m);
    if (!strcmp(line, "getActors")) return class_objects(fd, m.main.base, 0x3CE2B18);
    if (!strcmp(line, "getTagComponents")) return class_objects(fd, m.main.base, 0x3C957E0);
    char lookup[24], extra_lookup;
    uint64_t descriptor;
    if (sscanf(line, "%23s %" SCNx64 " %c", lookup, &descriptor, &extra_lookup) == 2 &&
        !strcmp(lookup, "findClass") && descriptor >= 0x3BC2000 && descriptor < 0x3F7F000)
        return class_objects(fd, m.main.base, descriptor);
    if (!strcmp(line, "getTitleID"))
        snprintf(response, sizeof(response), "%016" PRIX64 "\n", m.title_id);
    else if (!strcmp(line, "getProcessID"))
        snprintf(response, sizeof(response), "%016" PRIX64 "\n", m.process_id);
    else if (!strcmp(line, "getBuildID")) {
        for (int i = 0; i < 8; ++i) sprintf(response + i * 2, "%02X", m.build_id[i]);
        strcat(response, "\n");
    } else if (!strcmp(line, "getFullBuildID")) {
        for (size_t i = 0; i < sizeof(m.build_id); ++i) sprintf(response + i * 2, "%02X", m.build_id[i]);
        strcat(response, "\n");
    } else if (!strcmp(line, "getMainNsoBase"))
        snprintf(response, sizeof(response), "%016" PRIX64 "\n", m.main.base);
    else if (!strcmp(line, "getMainNsoSize"))
        snprintf(response, sizeof(response), "%016" PRIX64 "\n", m.main.size);
    else if (!strcmp(line, "getHeapBase"))
        snprintf(response, sizeof(response), "%016" PRIX64 "\n", m.heap.base);
    else if (!strcmp(line, "getHeapSize"))
        snprintf(response, sizeof(response), "%016" PRIX64 "\n", m.heap.size);
    else if (!strcmp(line, "getVersion"))
        strcpy(response, "za-dmnt-reader-0.1.0\n");
    else {
        char operation[24], extra;
        uint64_t address, size;
        if (sscanf(line, "%23s %" SCNx64 " %c", operation, &address, &extra) == 2 &&
            !strcmp(operation, "queryMemory")) {
            MemoryInfo mi = {0};
            rc = serviceDispatchInOut(&dmnt, 65104, address, mi);
            if (R_FAILED(rc)) return error_reply(fd, rc);
            if (!mi.size || mi.addr > address || mi.addr > UINT64_MAX - mi.size ||
                address >= mi.addr + mi.size)
                return send_all(fd, "ERR mapping-layout\n", 19);
            int n = snprintf(response, sizeof(response),
                "base=%016" PRIX64 " size=%016" PRIX64 " type=%08X perm=%08X\n",
                mi.addr, mi.size, (unsigned)mi.type, (unsigned)mi.perm);
            return send_all(fd, response, (size_t)n);
        }
        if (sscanf(line, "%23s %" SCNx64 " %" SCNu64 " %c", operation, &address, &size, &extra) != 3 ||
            (strcmp(operation, "peekMain") && strcmp(operation, "peekAbsolute")) ||
            !size || size > sizeof(bytes))
            return send_all(fd, "ERR invalid-command\n", 20);
        if (!strcmp(operation, "peekMain")) {
            if (address >= m.main.size || size > m.main.size - address)
                return send_all(fd, "ERR outside-main\n", 17);
            if (address > UINT64_MAX - m.main.base)
                return send_all(fd, "ERR address-overflow\n", 21);
            address += m.main.base;
        }
        if (address > UINT64_MAX - size)
            return send_all(fd, "ERR address-overflow\n", 21);
        rc = read_memory(address, bytes, size);
        if (R_FAILED(rc)) return error_reply(fd, rc);
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < size; ++i) {
            response[2*i] = hex[bytes[i] >> 4];
            response[2*i+1] = hex[bytes[i] & 15];
        }
        response[2*size] = '\n';
        response[2*size+1] = 0;
    }
    return send_all(fd, response, strlen(response));
}

#ifndef ZA_READER_RESIDENT
int main(void) {
    consoleInit(NULL);
    reader_started = svcGetSystemTick();
    sleep_set_result = appletSetAutoSleepDisabled(true);
    sleep_query_result = appletIsAutoSleepDisabled(&sleep_disabled);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    Result rc = smGetService(&dmnt, "dmnt:cht");
#ifdef ZA_READER_USB
    const UsbCommsInterfaceInfo usb_info = {0xFF, 0x5A, 0x01};
    Result usb_rc = R_SUCCEEDED(rc) ? usbCommsInitializeEx(1, &usb_info, 0x057e, 0x3000) : rc;
    usb_ready = R_SUCCEEDED(usb_rc);
    printf("Z-A USB Reader\n\nRead-only diagnostics; cheats remain active.\n");
    printf("dmnt: %08X   USB: %08X\n", rc, usb_rc);
    printf("Connect directly to the Mac. No network is opened.\nPress + to exit.\n");
    printf("Sleep request: %08X query: %08X disabled: %u\n",
           sleep_set_result, sleep_query_result, sleep_disabled);
    char line[256];
    size_t used = 0;
    bool pending = false, overflow = false, connected = false;
    u32 urb = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        u32 state = 0;
        Result state_rc = usb_ready ? usbDsGetState(&state) : usb_rc;
        if (usb_ready && R_FAILED(state_rc)) {
            printf("\nUSB state error: %08X. Press + to exit.\n", state_rc);
            usb_ready = false;
        }
        if (usb_ready && state == 5) connected = true;
        if (usb_ready && connected && state != 5) {
            // Detachment cancels the queued URB. Its completion belongs to
            // the old endpoints: close them before discarding pending state.
            // Recreate once for this physical transition; never replay a
            // command or accept bytes from the retired transfer/session.
            usbCommsExit();
            pending = false; connected = false; used = 0; overflow = false;
            usb_rc = usbCommsInitializeEx(1, &usb_info, 0x057e, 0x3000);
            usb_ready = R_SUCCEEDED(usb_rc);
            printf("\nUSB detached; endpoints recreated: %08X. No command replay.\n", usb_rc);
            consoleUpdate(NULL);
            continue;
        }
        if (usb_ready && !pending && state == 5) {
            Result read_rc = usbCommsReadAsync(usb_in, sizeof(usb_in), &urb, 0);
            if (R_SUCCEEDED(read_rc)) pending = true;
            else { printf("\nUSB read stopped: %08X. Press + to exit.\n", read_rc); usb_ready = false; }
        }
        if (usb_ready && pending && R_SUCCEEDED(eventWait(usbCommsGetReadCompletionEvent(0), 0))) {
            u32 transferred = 0, length = 0;
            Result read_rc = usbCommsGetReadResult(urb, &transferred, 0);
            pending = false;
            memcpy(&length, usb_in, sizeof(length));
            if (R_FAILED(read_rc) || transferred != sizeof(usb_in) || !length || length > sizeof(usb_in)-4) {
                printf("\nInvalid USB frame: %08X (%u/%u). Press + to exit.\n", read_rc, transferred, length);
                usb_ready = false;
            } else for (u32 i = 0; usb_ready && i < length; ++i) {
                const char ch = (char)usb_in[4+i];
                if (ch == '\r') continue;
                if (ch == '\n') {
                    line[used] = 0;
                    bool ok = overflow ? send_all(-1, "ERR command-too-long\n", 21) : command(-1, line);
                    used = 0; overflow = false;
                    if (!ok) usb_ready = false;
                } else if (!overflow && used < sizeof(line)-1) line[used++] = ch;
                else overflow = true;
            }
        }
        printf("\x1b[22;1HUSB link: %u  reader: %-7s  read: %-7s          ",
               state, usb_ready ? "READY" : "STOPPED", pending ? "pending" : "idle");
        consoleUpdate(NULL);
    }
    // Exit closes endpoints and cancels pending transfers before buffers die.
    if (R_SUCCEEDED(usb_rc)) usbCommsExit();
#else
    Result network_rc = socketInitializeDefault();
    int server = -1, client = -1;
    if (R_SUCCEEDED(rc) && R_SUCCEEDED(network_rc)) {
        server = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(6001),
                                   .sin_addr.s_addr = htonl(INADDR_ANY) };
        int yes = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 1) < 0) {
            close(server); server = -1;
        } else fcntl(server, F_SETFL, O_NONBLOCK);
    }
    printf("Z-A Template Reader\n\nRead-only trainer-capture diagnostics.\n");
    printf("Cheats remain active; no game memory is changed.\n\n");
    printf("dmnt: %08X   network: %08X\n", rc, network_rc);
    printf("Sleep request: %08X  query: %08X  disabled: %u\n",
           sleep_set_result, sleep_query_result, sleep_disabled);
    printf("%s\n\nPress + to exit.\n", server >= 0 ? "Listening on TCP port 6001." : "Could not start the reader.");
    Metadata m = {0};
    if (R_SUCCEEDED(rc)) {
        rc = metadata(&m);
        printf("Game metadata: %08X   title: %016" PRIX64 "\n", rc, m.title_id);
    }
    char line[256];
    size_t used = 0;
    u64 last_status = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        if (server >= 0 && client < 0) {
            client = accept(server, NULL, NULL);
            if (client >= 0) {
                // Accepted sockets can inherit the listener's nonblocking mode.
                // send_all must finish a whole hex line before another command.
                fcntl(client, F_SETFL, 0);
                struct timeval timeout = { .tv_sec = 2 };
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                used = 0;
                printf("Reader connected.\n");
            }
        }
        if (client >= 0) {
            struct pollfd pfd = { .fd = client, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0) {
                char buf[256];
                ssize_t n = recv(client, buf, sizeof(buf), 0);
                bool ok = n > 0;
                for (ssize_t i = 0; ok && i < n; ++i) {
                    if (buf[i] == '\r') continue;
                    if (buf[i] == '\n') {
                        line[used] = 0;
                        ok = command(client, line);
                        used = 0;
                    } else if (used < sizeof(line)-1) line[used++] = buf[i];
                    else ok = false;
                }
                if (!ok) { close(client); client = -1; }
            }
        }
        const u64 now = svcGetSystemTick();
        if (armTicksToNs(now-last_status) >= 10000000000ULL) {
            last_status = now;
            printf("\x1b[23;1HReader uptime: %" PRIu64 " seconds.                 ",
                   armTicksToNs(now-reader_started)/1000000000);
        }
        consoleUpdate(NULL);
    }
    if (client >= 0) close(client);
    if (server >= 0) close(server);
    if (R_SUCCEEDED(network_rc)) socketExit();
#endif
    if (serviceIsActive(&dmnt)) serviceClose(&dmnt);
    if (R_SUCCEEDED(sleep_set_result)) appletSetAutoSleepDisabled(false);
    consoleExit(NULL);
    return 0;
}
#endif
