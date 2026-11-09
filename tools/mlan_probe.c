/*
 * mlan_probe.c - drive Yamaha's own user-space mLAN manager.
 *
 * Why this exists: the audio driver never scans the FireWire bus. Its
 * sRequestAddDevices takes (count, byteLength, pointer) to _PJ_DEVICE_INFO
 * records - 4808 bytes each - that the *caller* has already built. So a
 * connection manager has to find the n12 and describe it.
 *
 * That manager is not missing. It ships, in x86_64 and unstripped, inside the
 * async kext:
 *
 *   YamahaFWAsync.kext/Contents/PlugIns/YamahaFWAsync.bundle
 *
 * whose Info.plist is a CFPlugIn:
 *
 *   CFPlugInTypes     C21B0B5C-D9D3-410f-842B-47F179185EEC
 *                     -> 25DC25B0-75C4-476c-9D68-416555C3F7B6
 *   CFPlugInFactories 25DC25B0-75C4-476c-9D68-416555C3F7B6 -> MyFactoryFunction
 *
 * MyFactoryFunction(allocator, typeID) checks typeID against that UUID and, if
 * it matches, returns a CmLANAsync object. Its public interface - the vtable, in
 * order, read out of the binary - is:
 *
 *   [0] QueryInterface(CFUUIDBytes, void**)
 *   [1] AddRef()
 *   [2] Release()
 *   [3] GetDevs(tagMADEVDATA*)          enumerate the mLAN devices
 *   [4] OpenDev(uint64 guid, ImLANAsyncDev**)
 *   [5] RegisterObserver(uint32, ImLANAsyncObserver*)
 *
 * and the device it hands back is:
 *
 *   [0] QueryInterface  [1] AddRef  [2] Release
 *   [3] GetPorts(tagMAPORTDATA*)        [4] OpenPort(uint32, ImLANAsyncPort**)
 *
 * The real FireWire work (config ROM reads, address-space registration, the
 * per-port send/receive queues) is behind those, and is done through Apple's
 * IOFireWireLib - see docs/HOW-IT-WORKS.md.
 *
 * Build:
 *     clang -Wall -O2 -o mlan_probe mlan_probe.c -framework CoreFoundation
 *
 * Run:
 *     ./mlan_probe                # default bundle, enumerate then OpenDev the n12
 *     sudo ./mlan_probe           # if IOFireWireLib wants read/write access
 *
 * Known obstacle, reported honestly rather than worked around silently: the
 * bundle links /usr/lib/libstdc++.6.dylib, which on Monterey is a dangling
 * symlink to a file Apple removed. dyld therefore refuses to load it unless a
 * real libstdc++.6.0.9.dylib is reachable, e.g.
 *   DYLD_FALLBACK_LIBRARY_PATH=$PWD/lib ./mlan_probe
 * with the library copied into $PWD/lib. This program prints the exact reason
 * when that happens instead of dying on an opaque dlopen failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <CoreFoundation/CoreFoundation.h>

/* Seconds to keep a connected port open, so the device has a live counterpart. */
static int g_hold = 0;

/* The bundle that carries CmLANAsync. */
#define ASYNC_BUNDLE \
    "/Library/Extensions/YamahaFWAsync.kext/Contents/PlugIns/YamahaFWAsync.bundle"

/* The CFPlugIn factory to call, and the type UUID to call it with. These come
 * straight from the bundle's Info.plist. */
#define FACTORY_NAME "MyFactoryFunction"
#define FACTORY_UUID "25DC25B0-75C4-476c-9D68-416555C3F7B6"
#define TYPE_UUID    "C21B0B5C-D9D3-410f-842B-47F179185EEC"

/* The n12's GUID, from its configuration ROM (see docs/HOW-IT-WORKS.md). */
#define N12_GUID 0x00A0DE0000011013ULL

/*
 * The public interface of CmLANAsync, as a C struct of function pointers.
 *
 * This works because on x86_64 macOS a C++ object with virtual functions is
 * laid out exactly like a struct whose first member is a pointer to an array of
 * function pointers - the Itanium C++ ABI. The object's first 8 bytes are its
 * "vptr", and it points at the *first virtual method*, so QueryInterface is
 * entry 0 as seen from the object even though two bookkeeping words (offset-to-
 * top and typeinfo) sit in front of it in memory.
 *
 * The signatures below are transcribed from the disassembly, including the
 * detail that QueryInterface takes CFUUIDBytes *by value*: a 16-byte struct, so
 * the ABI passes it in two registers (rsi:rdx), ahead of the output pointer.
 */
typedef struct {
    int     (*QueryInterface)(void *self, CFUUIDBytes iid, void **ppv);
    uint32_t (*AddRef)(void *self);
    uint32_t (*Release)(void *self);
    int     (*GetDevs)(void *self, void *madevdata);
    int     (*OpenDev)(void *self, uint64_t guid, void **ppdev);
    int     (*RegisterObserver)(void *self, uint32_t code, void *observer);
} ImLANAsync_vtbl;

/* The public interface of CmLANAsyncDev, same reasoning. */
typedef struct {
    int     (*QueryInterface)(void *self, CFUUIDBytes iid, void **ppv);
    uint32_t (*AddRef)(void *self);
    uint32_t (*Release)(void *self);
    int     (*GetPorts)(void *self, void *maportdata);
    int     (*OpenPort)(void *self, uint32_t index, void **ppport);
} ImLANAsyncDev_vtbl;

/* The public interface of CmLANAsyncPort. Note that Connect() and Disconnect()
 * are *not* here - they are non-virtual and called from inside the bundle; the
 * interface a client gets exposes the receive callback registration, the send
 * path and a reset. */
typedef struct {
    int     (*QueryInterface)(void *self, CFUUIDBytes iid, void **ppv);
    uint32_t (*AddRef)(void *self);
    uint32_t (*Release)(void *self);
    int     (*RegisterObserver)(void *self, uint32_t code, void *observer);
    int     (*Output)(void *self, uint32_t a, uint32_t b, uint8_t *data,
                      uint32_t len);
    int     (*Reset)(void *self, uint32_t flags);
} ImLANAsyncPort_vtbl;

/* Signature of a CFPlugIn factory function. The type UUID is what the plug-in
 * declared in CFPlugInTypes; the allocator argument is unused here. */
typedef void *(*factory_fn)(CFAllocatorRef allocator, CFUUIDRef typeUUID);

/* Recover an object's vptr. The double pointer is deliberate: the object holds
 * a pointer to the function-pointer table, and the table's first entry is the
 * first virtual method. */
static const ImLANAsync_vtbl *async_vtbl(void *obj)
{
    return *(const ImLANAsync_vtbl *const *)obj;
}

static const ImLANAsyncDev_vtbl *dev_vtbl(void *obj)
{
    return *(const ImLANAsyncDev_vtbl *const *)obj;
}

static const ImLANAsyncPort_vtbl *port_vtbl(void *obj)
{
    return *(const ImLANAsyncPort_vtbl *const *)obj;
}

/*
 * Load the bundle and call its factory. Returns the CmLANAsync object, or NULL
 * with a printed reason.
 *
 * CFBundleLoadExecutableAndReturnError rather than plain CFBundleLoadExecutable
 * because the interesting failure on this machine is a dyld error - a missing
 * library - and the CFError carries dyld's own explanation.
 */
static void *load_cmlan_async(void)
{
    CFURLRef url = CFURLCreateWithFileSystemPath(
        NULL, CFSTR(ASYNC_BUNDLE), kCFURLPOSIXPathStyle, true);
    if (url == NULL) {
        printf("could not make a URL for %s\n", ASYNC_BUNDLE);
        return NULL;
    }

    CFBundleRef bundle = CFBundleCreate(NULL, url);
    CFRelease(url);
    if (bundle == NULL) {
        printf("CFBundleCreate failed - is the bundle at the path above?\n");
        return NULL;
    }

    CFErrorRef err = NULL;
    if (!CFBundleLoadExecutableAndReturnError(bundle, &err)) {
        printf("bundle did not load.\n");
        if (err != NULL) {
            // CFError's description includes dyld's message, e.g. which library
            // it could not find.
            CFStringRef d = CFErrorCopyDescription(err);
            char buf[1024] = {0};
            if (d != NULL) {
                CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
                printf("  reason: %s\n", buf);
                CFRelease(d);
            }
            CFRelease(err);
        }
        printf("\n"
               "  darwin 12 note: /usr/lib/libstdc++.6.dylib is a dangling\n"
               "  symlink here, so a bundle built against GNU libstdc++ cannot\n"
               "  resolve it. Provide the real library and point dyld at it:\n"
               "    mkdir -p lib && cp <libstdc++.6.0.9.dylib> lib/\n"
               "    DYLD_FALLBACK_LIBRARY_PATH=$PWD/lib ./mlan_probe\n");
        CFRelease(bundle);
        return NULL;
    }

    factory_fn factory = (factory_fn)CFBundleGetFunctionPointerForName(
        bundle, CFSTR(FACTORY_NAME));
    if (factory == NULL) {
        printf("bundle loaded but has no %s symbol\n", FACTORY_NAME);
        CFRelease(bundle);
        return NULL;
    }

    CFUUIDRef type_id = CFUUIDCreateFromString(NULL, CFSTR(TYPE_UUID));
    if (type_id == NULL) {
        printf("could not parse the plug-in type UUID\n");
        CFRelease(bundle);
        return NULL;
    }

    printf("calling %s(%s)\n", FACTORY_NAME, TYPE_UUID);
    void *obj = factory(NULL, type_id);
    CFRelease(type_id);

    // Deliberately do not release `bundle`: the object's code lives inside it,
    // and dropping the bundle while the object is alive would unload that code.
    if (obj == NULL) {
        printf("  factory refused the type UUID and returned NULL\n");
    }
    return obj;
}

/*
 * Enumerate devices into GetDevs's output structure.
 *
 * The shape was read from CmLANAsync::GetDevs: a uint32 count at +0 and a
 * malloc'd array pointer at +8, each element a 0x118-byte _DEVICE_INFO. The
 * layout confirmed on the wire is
 *
 *   +0x00  uint64   guid          (0x00A0DE0000011013 for the n12)
 *   +0x08  uint32   …
 *   +0x0c  uint32   …
 *   +0x10  uint32   …
 *   +0x14  uint32   …
 *   +0x18  char[]   model name    ("n12")
 *
 * A generous zeroed buffer is used so a larger structure than expected cannot
 * corrupt the stack.
 */
static void enumerate_devices(void *obj, const ImLANAsync_vtbl *vt)
{
    uint8_t mad[128];
    memset(mad, 0, sizeof(mad));

    int rc = vt->GetDevs(obj, mad);
    printf("\nGetDevs -> 0x%08x\n", (uint32_t)rc);

    uint32_t n;
    void *devs;
    memcpy(&n, mad + 0, sizeof(n));       // count at +0
    memcpy(&devs, mad + 8, sizeof(devs)); // array at +8
    printf("  %u device(s)\n", n);

    for (uint32_t i = 0; i < n && devs != NULL; i++) {
        uint8_t *rec = (uint8_t *)devs + (size_t)i * 0x118;
        uint64_t guid;
        memcpy(&guid, rec + 0, sizeof(guid));   // guid at +0x00
        printf("  [%u] guid = 0x%016llx%s\n", i,
               (unsigned long long)guid,
               guid == N12_GUID ? "   <- the n12" : "");
        // The name is a NUL-terminated C string at +0x18.
        printf("       name = \"%.32s\"\n", (const char *)(rec + 0x18));
        // Dump the whole 0x118-byte record as words. The fields the driver
        // needs (isoch channel, sample rate, master channel) are somewhere in
        // here if the mLAN exchange filled them in.
        for (int w = 0; w < 0x118 / 4; w++) {
            uint32_t v;
            memcpy(&v, rec + w * 4, sizeof(v));
            printf("%08x", v);
            printf((w % 8 == 7) ? "\n" : " ");
        }
        printf("\n");
    }
    if (devs != NULL) {
        free(devs);   // GetDevs mallocs this; the caller owns it
    }
}

/*
 * Open the n12 by GUID and look at its ports.
 */
static void open_n12(void *obj, const ImLANAsync_vtbl *vt)
{
    void *dev = NULL;
    int rc = vt->OpenDev(obj, N12_GUID, &dev);
    printf("\nOpenDev(0x%016llx) -> 0x%08x\n",
           (unsigned long long)N12_GUID, (uint32_t)rc);
    if (rc != 0 || dev == NULL) {
        printf("  no device - nothing to open\n");
        return;
    }
    printf("  device object at %p\n", dev);

    const ImLANAsyncDev_vtbl *dvt = dev_vtbl(dev);
    printf("  device vtable at %p\n", (const void *)dvt);

    // GetPorts mirrors GetDevs: a count at +0, a malloc'd array at +8. Each
    // record is 12 bytes - {uint32 index, uint32 code, uint32 number} - as read
    // from CmLANAsyncDev::GetPorts, which mallocs count*12 and fills the three
    // words from its own table. The "code" is a four-character code stored as an
    // integer, so its hex digits spell the name.
    uint8_t map[128];
    memset(map, 0, sizeof(map));
    int prc = dvt->GetPorts(dev, map);
    printf("  GetPorts -> 0x%08x\n", (uint32_t)prc);
    uint32_t np;
    void *ports;
    memcpy(&np, map + 0, sizeof(np));
    memcpy(&ports, map + 8, sizeof(ports));
    printf("  %u port(s)\n", np);

    for (uint32_t i = 0; i < np && ports != NULL; i++) {
        uint8_t *rec = (uint8_t *)ports + (size_t)i * 12;
        uint32_t index, code, number;
        memcpy(&index,  rec + 0, sizeof(index));
        memcpy(&code,   rec + 4, sizeof(code));
        memcpy(&number, rec + 8, sizeof(number));
        // Print the 4CC both as raw bytes and as the integer's hex digits, since
        // which order Yamaha meant is not yet settled.
        printf("    [%u] index=%u  number=%u  code=0x%08x  bytes=\"%c%c%c%c\""
               "  hex-letters=\"%c%c%c%c\"\n",
               i, index, number, code,
               (char)(code & 0xff), (char)((code >> 8) & 0xff),
               (char)((code >> 16) & 0xff), (char)((code >> 24) & 0xff),
               (char)((code >> 24) & 0xff), (char)((code >> 16) & 0xff),
               (char)((code >> 8) & 0xff), (char)(code & 0xff));
    }

    if (ports != NULL) {
        free(ports);
    }

    // Open the first port. This only materialises the port object; nothing is
    // sent until Output() or the internal connect path is used.
    if (np > 0) {
        void *port = NULL;
        int orc = dvt->OpenPort(dev, 0, &port);
        printf("  OpenPort(0) -> 0x%08x", (uint32_t)orc);
        if (port != NULL) {
            const ImLANAsyncPort_vtbl *pvt = port_vtbl(port);
            printf("  port=%p vtable=%p (Output=%p Reset=%p)",
                   port, (const void *)pvt,
                   (const void *)pvt->Output, (const void *)pvt->Reset);
            if (g_hold > 0) {
                // OpenPort calls CmLANAsyncPort::SetMLANAsyncPortInfoAndConnect,
                // which registers the port's 0x200-byte receive address space
                // and publishes its address to the device. Holding the object
                // keeps that registration alive, so this process is a live
                // counterpart for the device rather than a one-shot query.
                printf("\n  holding the connected port for %d s...\n", g_hold);
                for (int t = 0; t < g_hold; t++) {
                    sleep(1);
                }
                printf("  releasing\n");
            }
            pvt->Release(port);
        }
        printf("\n");
    }

    dvt->Release(dev);
}

int main(int argc, char **argv)
{
    printf("mlan_probe - load Yamaha's own mLAN manager and ask it about the n12\n\n");

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--hold=", 7) == 0) {
            g_hold = atoi(argv[i] + 7);
        } else {
            fprintf(stderr, "usage: %s [--hold=N]\n", argv[0]);
            return 2;
        }
    }

    void *obj = load_cmlan_async();
    if (obj == NULL) {
        return 1;
    }
    printf("CmLANAsync object at %p\n", obj);

    const ImLANAsync_vtbl *vt = async_vtbl(obj);
    printf("vtable at %p (QueryInterface=%p AddRef=%p Release=%p)\n",
           (const void *)vt, (const void *)vt->QueryInterface,
           (const void *)vt->AddRef, (const void *)vt->Release);
    if (vt->QueryInterface == NULL || vt->GetDevs == NULL) {
        printf("vtable does not look like ImLANAsync\n");
        return 1;
    }

    // Hold a reference for the life of the probe and drop it on the way out;
    // the factory's object starts at refcount 1.
    vt->AddRef(obj);

    enumerate_devices(obj, vt);
    open_n12(obj, vt);

    vt->Release(obj);
    printf("\ndone\n");
    return 0;
}