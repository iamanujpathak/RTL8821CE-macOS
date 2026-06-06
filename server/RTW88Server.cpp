/*
 * RTW88Server — a thin, PERMANENT user-client kext for the RTL8821CE.
 *
 * Purpose: move the whole driver bring-up loop into USERSPACE. This kext does
 * the minimum that only a kext can do on macOS (there is no VFIO/UIO here):
 *   - match the PCI device, enable mem + bus-master, map BAR2 (rtw88 MMIO),
 *   - expose a tiny IOUserClient ABI: REG read/write, PCI config read/write,
 *     DMA alloc/map/free, a SAFE DMA-register-arm, MSI wait, and a MAC-power
 *     guard on interrupt-status reads.
 * All driver LOGIC (power-on sequence, fw download, ring setup, scan) then runs
 * in a userspace C program over this ABI: edit + rerun in seconds, full logs,
 * lldb/ASan, and NO kext rebuild => the macOS approval (cdhash) never changes.
 *
 * Safety model for THIS platform (DisableIoMapper=true, npci=0x2000 => NO IOMMU):
 * the device DMAs to raw physical addresses, untrapped. So the BRIDGE owns DMA
 * addressing — userspace never writes a physical address into a device register;
 * it says "arm register R with DMA handle H + offset" and the kext writes the
 * validated paddr (kBridgeRegWriteDma). And reads of interrupt-status registers
 * are refused while the MAC is powered off (the exact hang this project hit).
 *
 * Self-contained: depends only on IOPCIFamily + IOKit/Libkern KPI.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOBasicOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>
#include <sys/kpi_mbuf.h>
#include <kern/clock.h>
#include <kern/thread.h>

#include "rtw88_abi.h"

#define RTL_BAR_INDEX   2          /* rtw88 MMIO window */

/* the in-kernel control C (kctl.c + the bring-up files).
 * g_kmmio is the BAR2 base the MMIO shim reads/writes through; we publish it once
 * the BAR is mapped. rtw_kctl_poweron() runs the power-on sequence in-kernel. */
extern "C" volatile uint8_t *g_kmmio;
extern "C" uint32_t rtw_kctl_poweron(void);
extern "C" uint32_t rtw_kctl_bringup(void);   /* power -> rings -> fw -> mac_init -> efuse -> phy */
extern "C" uint32_t rtw_kctl_scan(struct rtw_scan_result *out);   /* bring-up + in-kernel scan */
extern "C" void rtw_kctl_connect(const struct rtw_connect_req *req, struct rtw_connect_result *out);  /* in-kernel connect */
extern "C" void rtw_kctl_disconnect(void);   /* tear down enX + rings */

/* The active device, for the C control code's DMA/PCI shim (extern "C" wrappers at
 * end of file). Set in RTW88Server::start(). */
class RTW88Server;
static RTW88Server *g_dev = nullptr;

#pragma mark - Ethernet nub (presents the WiFi link to macOS as enX)

/*
 * RTW88Ethernet — the virtual IOEthernetController the kext publishes as enX.
 * macOS runs its own DHCP/ARP/IP stack on it; the kext does the full data path
 * in-kernel: outputPacket() builds the 802.11+CCMP frame and rings the BE
 * doorbell via MMIO, and a kernel poll thread drains the RX ring and
 * inputPacket()s — no per-packet syscalls. The interface MAC is the card's real
 * MAC so the AP addressing stays consistent.
 */
class RTW88Ethernet : public IOEthernetController {
    OSDeclareDefaultStructors(RTW88Ethernet)
public:
    /* set the MAC BEFORE attach()/start() */
    void prepare(const uint8_t mac[6]);

    virtual bool      start(IOService *provider) override;
    virtual void      free() override;
    virtual IOReturn  enable(IONetworkInterface *netif) override;
    virtual IOReturn  disable(IONetworkInterface *netif) override;
    virtual UInt32    outputPacket(mbuf_t m, void *param) override;
    virtual IOReturn  getHardwareAddress(IOEthernetAddress *addr) override;
    virtual IOReturn  setPromiscuousMode(bool active) override { return kIOReturnSuccess; }
    virtual IOReturn  setMulticastMode(bool active) override { return kIOReturnSuccess; }
    virtual IOReturn  setMulticastList(IOEthernetAddress *a, UInt32 c) override { return kIOReturnSuccess; }
    virtual IOOutputQueue *createOutputQueue() override;

    void setLinkUp(bool up);

    /* ---- in-kext data path (TX/RX done here, no per-packet syscall) ---- */
    bool startData(class RTW88Server *server, const struct rtw_data_cfg *cfg);
    void stopData();

private:
    IOEthernetInterface *fNetif    = nullptr;
    IOEthernetAddress    fMac;
    IOLock              *fTxLock   = nullptr;
    IONetworkMedium     *fMedium   = nullptr;
    bool                 fRunning  = false;

    /* in-kext data path state */
    bool                 fDataMode = false;
    volatile uint8_t    *fMmio     = nullptr;   /* BAR2 base (from the server)   */
    uint8_t              fBssid[6] = {0};
    uint8_t              fRate     = 0;
    uint8_t              fRateId   = 7;     /* RTW_RATEID_* (raid) for the data TX descriptors */
    uint8_t             *fBeBd     = nullptr;    /* BE BD ring (kernel vaddr)     */
    uint32_t             fBeLen=0, fBdSz=0, fDescSz=0, fBeWp=0;
    uint8_t             *fRxData   = nullptr;    /* RX data pool (kernel vaddr)   */
    uint32_t             fRxN=0, fRxBufSz=0, fRxDescSz=0, fRxRp=0;
    IOBufferMemoryDescriptor *fTxPool = nullptr; /* our per-slot TX buffer pool   */
    uint8_t             *fTxPoolV  = nullptr;
    uint64_t             fTxPoolPa = 0;
    uint32_t             fTxSlotSz = 0;
    uint64_t             fCcmpPn   = 1;
    thread_t             fRxThread = THREAD_NULL;
    volatile bool        fRxStop   = false;
    volatile bool        fRxDone   = false;

    static void rxThreadTrampoline(void *arg, wait_result_t);
    void rxLoop();
    void rxFrame(const uint8_t *f, uint32_t pkt_len);
    void txEthFrame(const uint8_t *eth, uint32_t ethlen);
    void txAction(const uint8_t *frame, uint32_t flen);   /* unencrypted fixed-rate mgmt TX */
    void handleAddbaReq(const uint8_t *f, uint32_t len);   /* answer the AP's Block-Ack setup */
    void inputEth(const uint8_t *data, uint32_t len);
    inline uint32_t mmioR32(uint32_t off) { return *(volatile uint32_t *)(fMmio + off); }
    inline void     mmioW16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(fMmio + off) = v; }
};

#define RTK_PCI_TXBD_IDX_BEQ_K    0x3A8
#define RTK_PCI_RXBD_IDX_MPDUQ_K  0x3B4

/* data-path diagnostics (in-memory only — read+reset via kBridgeDataStats; NO
 * disk/log writes). rxMaxDepth = peak RX-ring occupancy: if it pins near the
 * ring size, polling can't keep up and we're dropping RX frames. */
static volatile uint64_t gDtx, gDtxDrop, gDtxBytes, gDrx, gDrxBytes, gDrxRetry, gDrxMcs, gDrxMaxDepth;
static volatile uint64_t gAddba;   /* downlink Block-Ack sessions accepted (ADDBA resp sent) */
static volatile uint64_t gDrxErr;     /* RX frames dropped for CRC32/ICV error (HW flagged)  */
static volatile uint64_t gDrxParse;   /* RX data frames dropped: bad LLC / oversize (format) */
/* diagnostic: 802.11 sequence-number gaps. gGap = a forward jump (>1) = MPDU(s)
 * missing/not-yet-arrived; gBack = a frame at/behind the last seq = retransmit/dup
 * arriving out of order. High gap+back => A-MPDU reordering is hurting TCP -> an RX
 * reorder buffer is the fix. */
static volatile uint64_t gDrxGap, gDrxBack;
static volatile uint16_t gLastSeq = 0xffff;
/* loss-characterization probe (READ-ONLY — no data-path behavior change).
 * gMiss: total MISSING sequence numbers, summed PER-TID (the real loss count, not a
 *        multi-TID artifact). loss% = gMiss / (gMiss + rx).
 * gRxRingFull: polls where the RX ring was >= nslots-2 deep (HW lapping us = overflow).
 * gRateSum/gRateN/gRateMax: RX PHY-rate code stats (avg + peak MCS the AP sends us).
 * gRetryDup: retry-bit frames whose per-TID seq is a dup/old one (AP resend we DID get).
 * gTidMask: bitmask of TIDs seen (popcount rules the multi-TID question in/out). */
static volatile uint64_t gMiss, gRxRingFull, gRateSum, gRateN, gRateMax, gRetryDup;
static volatile uint64_t gBigJump;   /* per-TID seq jumps > a BA window: AP serving others / idle gap, NOT our loss */
static volatile uint32_t gTidMask;
static uint16_t gLastSeqTid[8] = {0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff};

OSDefineMetaClassAndStructors(RTW88Ethernet, IOEthernetController)

void RTW88Ethernet::prepare(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) fMac.bytes[i] = mac[i];
}

bool RTW88Ethernet::start(IOService *provider)
{
    if (!IOEthernetController::start(provider)) return false;
    fTxLock = IOLockAlloc();
    if (!fTxLock) return false;

    /* one auto medium; link reported active once userspace says we're associated */
    OSDictionary *md = OSDictionary::withCapacity(1);
    if (!md) return false;
    fMedium = IONetworkMedium::medium(kIOMediumEthernetAuto | kIOMediumOptionFullDuplex, 300 * 1000000);
    if (fMedium) IONetworkMedium::addMedium(md, fMedium);
    publishMediumDictionary(md);
    if (fMedium) setCurrentMedium(fMedium);
    md->release();

    if (!attachInterface((IONetworkInterface **)&fNetif, true)) return false;
    setLinkStatus(kIONetworkLinkValid, fMedium, 0);   /* valid, not yet active */
    registerService();
    IOLog("RTW-eth: enX published (MAC %02x:%02x:%02x:%02x:%02x:%02x)\n",
          fMac.bytes[0], fMac.bytes[1], fMac.bytes[2], fMac.bytes[3], fMac.bytes[4], fMac.bytes[5]);
    return true;
}

void RTW88Ethernet::free()
{
    if (fTxLock) { IOLockFree(fTxLock); fTxLock = nullptr; }
    IOEthernetController::free();
}

IOOutputQueue *RTW88Ethernet::createOutputQueue()
{
    return IOBasicOutputQueue::withTarget(this, RTW_ETH_NSLOTS);
}

IOReturn RTW88Ethernet::enable(IONetworkInterface *netif)
{
    IOOutputQueue *q = getOutputQueue();
    if (q) { q->setCapacity(RTW_ETH_NSLOTS); q->start(); }
    fRunning = true;
    return kIOReturnSuccess;
}

IOReturn RTW88Ethernet::disable(IONetworkInterface *netif)
{
    fRunning = false;
    IOOutputQueue *q = getOutputQueue();
    if (q) { q->stop(); q->setCapacity(0); q->flush(); }
    return kIOReturnSuccess;
}

IOReturn RTW88Ethernet::getHardwareAddress(IOEthernetAddress *addr)
{
    *addr = fMac;
    return kIOReturnSuccess;
}

/* macOS stack -> WiFi: build + transmit the 802.11 frame in-kernel, no syscall.
 * The output queue serializes outputPacket, so fBeWp needs no lock. */
UInt32 RTW88Ethernet::outputPacket(mbuf_t m, void *param)
{
    if (fDataMode && fRunning) {
        uint8_t flat[1614];
        size_t len = mbuf_pkthdr_len(m);
        if (len >= 14 && len <= sizeof(flat) && mbuf_copydata(m, 0, len, flat) == 0)
            txEthFrame(flat, (uint32_t)len);
    }
    freePacket(m);
    return kIOReturnOutputSuccess;
}

void RTW88Ethernet::setLinkUp(bool up)
{
    if (up) setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, fMedium, 300 * 1000000);
    else    setLinkStatus(kIONetworkLinkValid, fMedium, 0);
    IOLog("RTW-eth: link %s\n", up ? "ACTIVE" : "down");
}

#pragma mark - Bridge service (owns the device + DMA + IRQ state)

struct rtw_dma_slot {
    IOBufferMemoryDescriptor *bmd;
    IOPhysicalAddress         paddr;
    uint64_t                  size;
};

class RTW88Server : public IOService {
    OSDeclareDefaultStructors(RTW88Server)
public:
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                   UInt32 type, IOUserClient **handler) override;

    /* --- operations the user client forwards to (all bounds-checked here) --- */
    IOReturn regRead (uint32_t off, uint32_t width, uint64_t *out);
    IOReturn regWrite(uint32_t off, uint32_t width, uint64_t val);
    IOReturn cfgRead (uint32_t off, uint32_t width, uint64_t *out);
    IOReturn cfgWrite(uint32_t off, uint32_t width, uint64_t val);
    IOReturn dmaAlloc(uint64_t size, uint64_t *handle, uint64_t *paddr);
    IOReturn dmaFree (uint64_t handle);
    IOReturn regWriteDma(uint32_t regOff, uint64_t handle, uint64_t bufOff, uint32_t width);
    IOReturn irqEnable();
    IOReturn irqDisable();
    IOReturn irqWait(uint32_t timeoutMs, uint64_t *firedDelta);
    IOReturn irqStatus(uint32_t off, uint32_t width, uint64_t *out);
    void     setMacPower(bool on);
    IOReturn power(bool memEnable, bool busMaster);

    /* --- in-kext data path (publishes enX, runs TX/RX in-kernel) --- */
    IOReturn dataStart(const struct rtw_data_cfg *cfg);
    IOReturn dataStop();
    IOReturn dataLink(bool up);
    IOReturn dataStats(uint64_t out[21]);  /* read+reset the in-memory counters */

    /* accessors the data-path nub uses (direct MMIO + DMA buffer resolution) */
    volatile uint8_t *mmio() const { return fMmio; }
    bool dmaResolve(uint64_t handle, uint8_t **vaddr, uint64_t *paddr, uint64_t *size);
    void *dmaVaddr(uint64_t handle);   /* kernel virtual address of a DMA handle (for in-kernel control) */

    /* DMA buffer mapping for the user client's clientMemoryForType */
    IOMemoryDescriptor *dmaDescriptorForHandle(uint64_t handle);

    uint16_t vendorID() const { return fVendor; }
    uint16_t deviceID() const { return fDevice; }
    uint64_t barLength() const { return fBarLen; }

private:
    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *src, int count);

    IOPCIDevice              *fPci     = nullptr;
    IOMemoryMap              *fBarMap  = nullptr;
    volatile uint8_t         *fMmio    = nullptr;
    uint64_t                  fBarLen  = 0;
    uint16_t                  fVendor  = 0;
    uint16_t                  fDevice  = 0;

    rtw_dma_slot              fDma[RTW_BRIDGE_MAX_DMA];
    IOLock                   *fDmaLock = nullptr;

    /* IRQ machinery — created LAZILY at irqEnable() (deferred), so MSI is never
     * delivered while the MAC is off. */
    IOWorkLoop               *fWorkLoop = nullptr;
    IOInterruptEventSource   *fIrqSrc   = nullptr;
    IOLock                   *fIrqLock  = nullptr;
    volatile uint64_t         fIrqCount = 0;
    int                       fMsiIndex = -1;

    bool                      fMacOn    = false;  /* userspace asserts this once power-on completes */

    RTW88Ethernet        *fEth      = nullptr; /* the published enX nub, if any */
};

OSDefineMetaClassAndStructors(RTW88Server, IOService)

bool RTW88Server::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) { IOLog("RTW-bridge: provider not IOPCIDevice\n"); return false; }

    fVendor = fPci->configRead16(kIOPCIConfigVendorID);
    fDevice = fPci->configRead16(kIOPCIConfigDeviceID);
    IOLog("RTW-bridge: matched PCI %04x:%04x\n", fVendor, fDevice);

    fPci->setMemoryEnable(true);
    fPci->setBusLeadEnable(true);

    fBarMap = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + RTL_BAR_INDEX * 4);
    if (!fBarMap) { IOLog("RTW-bridge: BAR%d map failed\n", RTL_BAR_INDEX); return false; }
    fMmio  = (volatile uint8_t *)fBarMap->getVirtualAddress();
    fBarLen = fBarMap->getLength();
    g_kmmio = fMmio;   /* publish BAR2 base to the in-kernel control C (kctl.c) */
    g_dev   = this;    /* ... and the device, for the DMA/PCI shim */
    IOLog("RTW-bridge: BAR%d @ %p len 0x%llx\n", RTL_BAR_INDEX, (void *)fMmio, fBarLen);

    fDmaLock = IOLockAlloc();
    fIrqLock = IOLockAlloc();
    if (!fDmaLock || !fIrqLock) return false;
    bzero(fDma, sizeof(fDma));

    /* Discover the MSI (messaged) interrupt index now, but DO NOT arm it. */
    for (int i = 0; ; i++) {
        int type = 0;
        if (fPci->getInterruptType(i, &type) != kIOReturnSuccess) break;
        if (type & kIOInterruptTypePCIMessaged) { fMsiIndex = i; break; }
    }
    IOLog("RTW-bridge: MSI interrupt index = %d (armed lazily at irqEnable)\n", fMsiIndex);

    registerService();   /* so the userspace app can find us by class name */
    IOLog("RTW-bridge: ready. Drive it from userspace (rtw_bridge_cli).\n");
    return true;
}

void RTW88Server::stop(IOService *provider)
{
    dataStop();   /* stops the RX thread + tears down enX */
    irqDisable();
    if (fWorkLoop) { fWorkLoop->release(); fWorkLoop = nullptr; }
    for (int i = 0; i < RTW_BRIDGE_MAX_DMA; i++) {
        if (fDma[i].bmd) { fDma[i].bmd->complete(); fDma[i].bmd->release(); fDma[i].bmd = nullptr; }
    }
    if (fBarMap)  { fBarMap->release(); fBarMap = nullptr; }
    if (fDmaLock) { IOLockFree(fDmaLock); fDmaLock = nullptr; }
    if (fIrqLock) { IOLockFree(fIrqLock); fIrqLock = nullptr; }
    IOLog("RTW-bridge: stop\n");
    IOService::stop(provider);
}

/* ---- register / config access -------------------------------------------- */

IOReturn RTW88Server::regRead(uint32_t off, uint32_t width, uint64_t *out)
{
    if (!fMmio) return kIOReturnNoDevice;
    if (width != 1 && width != 2 && width != 4) return kIOReturnBadArgument;
    if ((uint64_t)off + width > fBarLen || (off & (width - 1))) return kIOReturnBadArgument;
    switch (width) {
        case 1: *out = *(volatile uint8_t  *)(fMmio + off); break;
        case 2: *out = *(volatile uint16_t *)(fMmio + off); break;
        default:*out = *(volatile uint32_t *)(fMmio + off); break;
    }
    return kIOReturnSuccess;
}

IOReturn RTW88Server::regWrite(uint32_t off, uint32_t width, uint64_t val)
{
    if (!fMmio) return kIOReturnNoDevice;
    if (width != 1 && width != 2 && width != 4) return kIOReturnBadArgument;
    if ((uint64_t)off + width > fBarLen || (off & (width - 1))) return kIOReturnBadArgument;
    switch (width) {
        case 1: *(volatile uint8_t  *)(fMmio + off) = (uint8_t)val;  break;
        case 2: *(volatile uint16_t *)(fMmio + off) = (uint16_t)val; break;
        default:*(volatile uint32_t *)(fMmio + off) = (uint32_t)val; break;
    }
    return kIOReturnSuccess;
}

IOReturn RTW88Server::cfgRead(uint32_t off, uint32_t width, uint64_t *out)
{
    if (off + width > 0x1000) return kIOReturnBadArgument;
    switch (width) {
        case 1: *out = fPci->configRead8(off);  break;
        case 2: *out = fPci->configRead16(off); break;
        case 4: *out = fPci->configRead32(off); break;
        default: return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

IOReturn RTW88Server::cfgWrite(uint32_t off, uint32_t width, uint64_t val)
{
    if (off + width > 0x1000) return kIOReturnBadArgument;
    switch (width) {
        case 1: fPci->configWrite8(off,  (uint8_t)val);  break;
        case 2: fPci->configWrite16(off, (uint16_t)val); break;
        case 4: fPci->configWrite32(off, (uint32_t)val); break;
        default: return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

/* ---- DMA: bridge OWNS the physical address ------------------------------- */

IOReturn RTW88Server::dmaAlloc(uint64_t size, uint64_t *handle, uint64_t *paddr)
{
    if (size == 0 || size > (16ull << 20)) return kIOReturnBadArgument;
    IOReturn ret = kIOReturnNoResources;
    IOLockLock(fDmaLock);
    for (int i = 0; i < RTW_BRIDGE_MAX_DMA; i++) {
        if (fDma[i].bmd) continue;
        /* <4GB, page-aligned, physically contiguous, kernel-owned (mapped to the
         * client later via clientMemoryForType). physicalMask forces below 4GB —
         * mandatory because there is no IOMMU to relocate a high address. */
        IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared,
            size, 0x00000000FFFFF000ULL);
        if (!bmd) break;
        if (bmd->prepare() != kIOReturnSuccess) { bmd->release(); break; }
        IOByteCount seglen = 0;
        IOPhysicalAddress pa = bmd->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
        if (!pa) { bmd->complete(); bmd->release(); break; }
        bzero((void *)bmd->getBytesNoCopy(), size);
        fDma[i].bmd = bmd; fDma[i].paddr = pa; fDma[i].size = size;
        *handle = (uint64_t)i; *paddr = pa;
        IOLog("RTW-bridge: DMA[%d] alloc %llu B @ phys 0x%llx\n", i, size, (uint64_t)pa);
        ret = kIOReturnSuccess;
        break;
    }
    IOLockUnlock(fDmaLock);
    return ret;
}

IOReturn RTW88Server::dmaFree(uint64_t handle)
{
    if (handle >= RTW_BRIDGE_MAX_DMA) return kIOReturnBadArgument;
    IOLockLock(fDmaLock);
    rtw_dma_slot *s = &fDma[handle];
    if (s->bmd) { s->bmd->complete(); s->bmd->release(); bzero(s, sizeof(*s)); }
    IOLockUnlock(fDmaLock);
    return kIOReturnSuccess;
}

IOMemoryDescriptor *RTW88Server::dmaDescriptorForHandle(uint64_t handle)
{
    if (handle >= RTW_BRIDGE_MAX_DMA) return nullptr;
    IOMemoryDescriptor *md = nullptr;
    IOLockLock(fDmaLock);
    if (fDma[handle].bmd) { md = fDma[handle].bmd; md->retain(); }
    IOLockUnlock(fDmaLock);
    return md;
}

void *RTW88Server::dmaVaddr(uint64_t handle)
{
    if (handle >= RTW_BRIDGE_MAX_DMA) return nullptr;
    void *v = nullptr;
    IOLockLock(fDmaLock);
    if (fDma[handle].bmd) v = fDma[handle].bmd->getBytesNoCopy();
    IOLockUnlock(fDmaLock);
    return v;
}

/* The ONLY way a device DMA register gets a physical address: the kext writes
 * (validated paddr + bufOff), never a value chosen by userspace. */
IOReturn RTW88Server::regWriteDma(uint32_t regOff, uint64_t handle, uint64_t bufOff, uint32_t width)
{
    if (handle >= RTW_BRIDGE_MAX_DMA) return kIOReturnBadArgument;
    if (width != 4 && width != 8) return kIOReturnBadArgument;
    IOLockLock(fDmaLock);
    rtw_dma_slot s = fDma[handle];
    IOLockUnlock(fDmaLock);
    if (!s.bmd || bufOff >= s.size) return kIOReturnBadArgument;
    uint64_t pa = (uint64_t)s.paddr + bufOff;
    if (width == 4) {
        if (pa > 0xFFFFFFFFull) return kIOReturnNotPermitted;   /* would truncate */
        IOReturn r = regWrite(regOff, 4, (uint32_t)pa);
        if (r == kIOReturnSuccess)
            IOLog("RTW-bridge: armed reg 0x%x = DMA[%llu]+0x%llx (phys 0x%llx)\n", regOff, handle, bufOff, pa);
        return r;
    }
    /* 64-bit: low dword at regOff, high dword at regOff+4 (rtw88 ring base layout) */
    IOReturn r = regWrite(regOff, 4, (uint32_t)(pa & 0xFFFFFFFF));
    if (r == kIOReturnSuccess) r = regWrite(regOff + 4, 4, (uint32_t)(pa >> 32));
    if (r == kIOReturnSuccess)
        IOLog("RTW-bridge: armed reg64 0x%x = DMA[%llu]+0x%llx (phys 0x%llx)\n", regOff, handle, bufOff, pa);
    return r;
}

/* ---- power + the MAC-power guard ----------------------------------------- */

void RTW88Server::setMacPower(bool on)
{
    fMacOn = on;
    IOLog("RTW-bridge: MAC power asserted = %d\n", on);
}

IOReturn RTW88Server::power(bool memEnable, bool busMaster)
{
    fPci->setMemoryEnable(memEnable);
    fPci->setBusLeadEnable(busMaster);
    IOLog("RTW-bridge: power mem=%d busmaster=%d\n", memEnable, busMaster);
    return kIOReturnSuccess;
}

/* IRQ-status read is REFUSED while the MAC is off — reading ISR on a dead MAC is
 * exactly the bus hang this project lost days to. Fail loud instead. */
IOReturn RTW88Server::irqStatus(uint32_t off, uint32_t width, uint64_t *out)
{
    if (!fMacOn) {
        IOLog("RTW-bridge: REFUSED IRQ-status read @0x%x — MAC is OFF (would hang the bus)\n", off);
        return kIOReturnNotReady;
    }
    return regRead(off, width, out);
}

/* ---- interrupts: lazily armed; primary path touches NO device register ---- */

void RTW88Server::interruptOccurred(OSObject *owner, IOInterruptEventSource *src, int count)
{
    RTW88Server *me = OSDynamicCast(RTW88Server, owner);
    if (!me) return;
    IOLockLock(me->fIrqLock);
    me->fIrqCount += (uint64_t)(count > 0 ? count : 1);
    IOLockWakeup(me->fIrqLock, (void *)&me->fIrqCount, false);
    IOLockUnlock(me->fIrqLock);
}

IOReturn RTW88Server::irqEnable()
{
    if (!fMacOn) {
        IOLog("RTW-bridge: REFUSED irqEnable — MAC is OFF (set power first)\n");
        return kIOReturnNotReady;
    }
    if (fIrqSrc) return kIOReturnSuccess;   /* idempotent */
    if (!fWorkLoop) {
        fWorkLoop = IOWorkLoop::workLoop();
        if (!fWorkLoop) return kIOReturnNoResources;
    }
    int idx = (fMsiIndex >= 0) ? fMsiIndex : 0;
    fIrqSrc = IOInterruptEventSource::interruptEventSource(this, &RTW88Server::interruptOccurred, fPci, idx);
    if (!fIrqSrc) return kIOReturnNoResources;
    if (fWorkLoop->addEventSource(fIrqSrc) != kIOReturnSuccess) {
        fIrqSrc->release(); fIrqSrc = nullptr; return kIOReturnError;
    }
    fIrqSrc->enable();
    IOLog("RTW-bridge: IRQ armed (index %d)\n", idx);
    return kIOReturnSuccess;
}

IOReturn RTW88Server::irqDisable()
{
    if (fIrqSrc && fWorkLoop) {
        fIrqSrc->disable();
        fWorkLoop->removeEventSource(fIrqSrc);
        fIrqSrc->release();
        fIrqSrc = nullptr;
        IOLog("RTW-bridge: IRQ disarmed\n");
    }
    return kIOReturnSuccess;
}

IOReturn RTW88Server::irqWait(uint32_t timeoutMs, uint64_t *firedDelta)
{
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeoutMs ? timeoutMs : 1, kMillisecondScale, &deadline);
    IOLockLock(fIrqLock);
    uint64_t start = fIrqCount;
    int wr = THREAD_AWAKENED;
    while (fIrqCount == start && wr != THREAD_TIMED_OUT) {
        wr = IOLockSleepDeadline(fIrqLock, (void *)&fIrqCount, deadline, THREAD_ABORTSAFE);
        if (wr == THREAD_INTERRUPTED) break;
    }
    *firedDelta = fIrqCount - start;
    IOLockUnlock(fIrqLock);
    return kIOReturnSuccess;
}

#pragma mark - in-kext data path (TX/RX run here; zero per-packet syscalls)

bool RTW88Server::dmaResolve(uint64_t handle, uint8_t **vaddr, uint64_t *paddr, uint64_t *size)
{
    if (handle >= RTW_BRIDGE_MAX_DMA) return false;
    IOLockLock(fDmaLock);
    rtw_dma_slot s = fDma[handle];
    IOLockUnlock(fDmaLock);
    if (!s.bmd) return false;
    *vaddr = (uint8_t *)s.bmd->getBytesNoCopy();
    *paddr = (uint64_t)s.paddr;
    *size  = s.size;
    return *vaddr != nullptr;
}

/* macOS stack -> WiFi: build an 802.11 ToDS + CCMP-IV + LLC-SNAP frame from the
 * Ethernet frame, write it into our per-slot TX buffer + the BE BD, ring the BE
 * doorbell. All direct MMIO/DMA — no syscall. Mirrors the userspace
 * trx_tx_data + build_l2 (hardware does the CCMP encrypt + MIC). */
void RTW88Ethernet::txEthFrame(const uint8_t *eth, uint32_t ethlen)
{
    if (ethlen < 14) return;
    const uint8_t *dst = eth;                 /* Ethernet dst -> 802.11 addr3 */
    uint16_t et = (uint16_t)((eth[12] << 8) | eth[13]);
    const uint8_t *payload = eth + 14;
    uint32_t plen = ethlen - 14;
    uint32_t airlen = 24 + 8 + 8 + plen;      /* hdr + CCMP-IV + LLC-SNAP + payload */
    if (fDescSz + airlen > fTxSlotSz) return; /* too big for a slot */

    /* flow control: don't reuse a slot the HW hasn't transmitted (BEQ read idx
     * is bits [27:16]); drop if the ring is full (TCP resends). The BE ring now
     * has two producers (this output thread + the rxLoop's ADDBA responder), so
     * the slot-reserve..doorbell sequence must be serialized with fTxLock. */
    IOLockLock(fTxLock);
    uint32_t slot = fBeWp;
    uint32_t next = (slot + 1) % fBeLen;
    uint32_t hwrd = ((mmioR32(RTK_PCI_TXBD_IDX_BEQ_K) >> 16) & 0xfff) % fBeLen;
    if (next == hwrd) { IOLockUnlock(fTxLock); gDtxDrop++; return; }  /* ring full */
    gDtx++; gDtxBytes += plen;

    uint8_t  *buf  = fTxPoolV + (uint64_t)slot * fTxSlotSz;
    uint64_t  bufpa = fTxPoolPa + (uint64_t)slot * fTxSlotSz;
    uint8_t  *body = buf + fDescSz;

    body[0] = 0x08; body[1] = 0x41; body[2] = 0; body[3] = 0;   /* data, ToDS, Protected */
    memcpy(body + 4, fBssid, 6); memcpy(body + 10, fMac.bytes, 6); memcpy(body + 16, dst, 6);
    body[22] = 0; body[23] = 0;
    uint64_t pn = fCcmpPn++;
    uint8_t *iv = body + 24;
    iv[0] = (uint8_t)pn;        iv[1] = (uint8_t)(pn >> 8);
    iv[2] = 0;                  iv[3] = 0x20;                    /* ExtIV, keyid 0 */
    iv[4] = (uint8_t)(pn >> 16); iv[5] = (uint8_t)(pn >> 24);
    iv[6] = (uint8_t)(pn >> 32); iv[7] = (uint8_t)(pn >> 40);
    uint8_t *llc = body + 32;
    llc[0] = 0xaa; llc[1] = 0xaa; llc[2] = 0x03; llc[3] = 0; llc[4] = 0; llc[5] = 0;
    llc[6] = (uint8_t)(et >> 8); llc[7] = (uint8_t)et;
    memcpy(body + 40, payload, plen);

    /* TX descriptor (48B) — same fields as the userspace path. Bulk data: let the
     * firmware rate-adaptation drive the rate (USE_RATE/DISDATAFB cleared, RATE_ID
     * set to the raid programmed by fw_ra_info; MACID 0). fRate is only the init
     * hint; fRateId is the HT/VHT/legacy raid negotiated at media-connect. */
    uint32_t rate_id = fRateId;
    memset(buf, 0, fDescSz);
    uint32_t *w = (uint32_t *)buf;
    w[0] = (airlen & 0xffff) | ((fDescSz & 0xff) << 16) | (1u << 26);   /* SIZE|OFFSET|LS */
    w[1] = (3u << 22) | (rate_id << 16);                               /* SEC_TYPE=CCMP | RATE_ID (QSEL=BE=0, MACID=0) */
    w[4] = (uint32_t)(fRate & 0x7f);                                    /* DATARATE (init hint) */
    w[8] = (1u << 31);                                                  /* EN_HWSEQ */

    uint32_t total = airlen + fDescSz;
    uint32_t psb = (total - 1) / 128 + 1;
    uint8_t *bd = fBeBd + (uint64_t)slot * fBdSz;
    *(uint16_t *)(bd + 0) = (uint16_t)fDescSz;
    *(uint16_t *)(bd + 2) = (uint16_t)psb;
    *(uint32_t *)(bd + 4) = (uint32_t)bufpa;
    *(uint16_t *)(bd + 8) = (uint16_t)airlen;
    *(uint16_t *)(bd + 10) = 0;
    *(uint32_t *)(bd + 12) = (uint32_t)(bufpa + fDescSz);

    __sync_synchronize();
    fBeWp = next;
    mmioW16(RTK_PCI_TXBD_IDX_BEQ_K, (uint16_t)(fBeWp & 0xfff));
    IOLockUnlock(fTxLock);
}

/* Post a pre-built 802.11 management frame (e.g. an ADDBA response) to the BE ring,
 * unencrypted and at a fixed low rate. Shares fTxLock with txEthFrame since it runs
 * on the rxLoop thread. Rare (once per Block-Ack session), so cost is irrelevant. */
void RTW88Ethernet::txAction(const uint8_t *frame, uint32_t flen)
{
    if (!fRunning || flen < 24 || fDescSz + flen > fTxSlotSz) return;

    IOLockLock(fTxLock);
    uint32_t slot = fBeWp;
    uint32_t next = (slot + 1) % fBeLen;
    uint32_t hwrd = ((mmioR32(RTK_PCI_TXBD_IDX_BEQ_K) >> 16) & 0xfff) % fBeLen;
    if (next == hwrd) { IOLockUnlock(fTxLock); return; }

    uint8_t  *buf   = fTxPoolV + (uint64_t)slot * fTxSlotSz;
    uint64_t  bufpa = fTxPoolPa + (uint64_t)slot * fTxSlotSz;
    memcpy(buf + fDescSz, frame, flen);

    memset(buf, 0, fDescSz);
    uint32_t *w = (uint32_t *)buf;
    w[0] = (flen & 0xffff) | ((fDescSz & 0xff) << 16) | (1u << 26);    /* SIZE|OFFSET|LS */
    w[1] = 0;                                                          /* QSEL=BE, MACID 0, sec_type 0 (unencrypted) */
    w[3] = (1u << 8) | (1u << 10);                                     /* USE_RATE | DISDATAFB */
    w[4] = 0x04;                                                       /* DESC_RATE6M (basic OFDM) */
    w[8] = (1u << 31);                                                 /* EN_HWSEQ */

    uint32_t total = flen + fDescSz;
    uint32_t psb = (total - 1) / 128 + 1;
    uint8_t *bd = fBeBd + (uint64_t)slot * fBdSz;
    *(uint16_t *)(bd + 0) = (uint16_t)fDescSz;
    *(uint16_t *)(bd + 2) = (uint16_t)psb;
    *(uint32_t *)(bd + 4) = (uint32_t)bufpa;
    *(uint16_t *)(bd + 8) = (uint16_t)flen;
    *(uint16_t *)(bd + 10) = 0;
    *(uint32_t *)(bd + 12) = (uint32_t)(bufpa + fDescSz);

    __sync_synchronize();
    fBeWp = next;
    mmioW16(RTK_PCI_TXBD_IDX_BEQ_K, (uint16_t)(fBeWp & 0xfff));
    IOLockUnlock(fTxLock);
}

/* The AP sets up downlink A-MPDU aggregation by sending us an ADDBA Request (a
 * BlockAck-category action frame). We must answer with an ADDBA Response accepting
 * it; the WMAC then receives A-MPDU and auto-generates the Block-Acks in hardware
 * (the same hardware path that already auto-ACKs unicast). Without this answer the
 * AP never aggregates and we stay at one MPDU per PPDU. */
void RTW88Ethernet::handleAddbaReq(const uint8_t *f, uint32_t len)
{
    if (len < 24 + 9) return;
    const uint8_t *rq = f + 24;                 /* action body */
    uint8_t  dialog     = rq[2];
    uint16_t ba_param   = (uint16_t)(rq[3] | (rq[4] << 8));
    uint16_t ba_timeout = (uint16_t)(rq[5] | (rq[6] << 8));
    uint8_t  tid        = (uint8_t)((ba_param >> 2) & 0xf);

    uint8_t r[33];
    r[0] = 0xd0; r[1] = 0x00; r[2] = 0; r[3] = 0;          /* FC=action, duration */
    memcpy(r + 4,  fBssid, 6);                              /* addr1 = RA = AP   */
    memcpy(r + 10, fMac.bytes, 6);                          /* addr2 = TA = us   */
    memcpy(r + 16, fBssid, 6);                              /* addr3 = BSSID     */
    r[22] = 0; r[23] = 0;                                   /* seq (HW fills)    */
    uint8_t *rb = r + 24;
    rb[0] = 0x03;                                           /* category: BlockAck         */
    rb[1] = 0x01;                                           /* action: ADDBA Response     */
    rb[2] = dialog;                                         /* echo dialog token          */
    rb[3] = 0x00; rb[4] = 0x00;                             /* status code = success      */
    rb[5] = (uint8_t)ba_param; rb[6] = (uint8_t)(ba_param >> 8);   /* echo BA params (TID/bufsize/policy) */
    rb[7] = (uint8_t)ba_timeout; rb[8] = (uint8_t)(ba_timeout >> 8);
    txAction(r, 33);

    gAddba++;
    IOLog("RTW88: ADDBA req (tid %u) -> ADDBA resp accepted; HW Block-Ack on\n", tid);
}

/* the IOEthernetController output path: in data mode do the full TX in-kernel */
/* (outputPacket override below dispatches here) */

void RTW88Ethernet::inputEth(const uint8_t *data, uint32_t len)
{
    if (!fRunning || !fNetif || len < 14 || len > 2048) return;
    mbuf_t m = allocatePacket(len);
    if (!m) return;
    if (mbuf_copyback(m, 0, len, data, MBUF_DONTWAIT) != 0) { freePacket(m); return; }
    fNetif->inputPacket(m, len, IONetworkInterface::kInputOptionQueuePacket);   /* rxLoop flushes once per drain */
}

/* one received 802.11 data frame -> Ethernet -> stack */
void RTW88Ethernet::rxFrame(const uint8_t *f, uint32_t pkt_len)
{
    uint16_t fc = (uint16_t)(f[0] | (f[1] << 8));
    /* management Action frame? (type=00, subtype=1101 -> f[0]==0xd0). Answer the AP's
     * ADDBA Request (BlockAck category 3, action 0) to enable downlink aggregation. */
    if ((f[0] & 0x0c) == 0x00) {                            /* type = management */
        if ((f[0] & 0xf0) == 0xd0 && pkt_len >= 26 && f[24] == 0x03 && f[25] == 0x00)
            handleAddbaReq(f, pkt_len);
        return;
    }
    if (((fc >> 2) & 3) != 2) return;                       /* else DATA frames only */
    gDrx++; if (f[1] & 0x08) gDrxRetry++;                    /* retry bit = AP resend */
    bool qos = ((fc >> 4) & 0x8);
    /* diagnostic: 802.11 Sequence Control (f[22..23], seq = bits[15:4]). */
    {
        uint16_t seq = (uint16_t)(((f[22] | (f[23] << 8)) >> 4) & 0xfff);
        if (gLastSeq != 0xffff) {
            int d = ((int)seq - (int)gLastSeq) & 0xfff;
            if (d & 0x800) d -= 0x1000;     /* sign-extend 12-bit delta */
            if (d > 1)       gDrxGap++;      /* forward jump: d-1 MPDUs missing/pending */
            else if (d <= 0) gDrxBack++;     /* at/behind last: retransmit/dup, out of order */
        }
        gLastSeq = seq;

        /* loss probe: same arithmetic but PER-TID, so multi-TID interleaving can't
         * fake gaps. gMiss sums the actually-skipped seq numbers (the true loss).
         * MASK to the 3-bit EDCA TID (0-7): gLastSeqTid is [8], and the QoS-control
         * low nibble on a malformed frame can read 8-15 -> out-of-bounds + bogus
         * tids/miss inflation. */
        uint8_t tid = qos ? (uint8_t)(f[24] & 0x07) : 0;
        gTidMask |= (1u << tid);
        uint16_t prev = gLastSeqTid[tid];
        if (prev != 0xffff) {
            int dt = ((int)seq - (int)prev) & 0xfff;
            if (dt & 0x800) dt -= 0x1000;
            /* a real downlink loss is bounded by the BA window (<=64); a jump beyond that
             * is the AP's per-TID SN counter advancing while it serves OTHER stations (or
             * an idle gap), NOT frames we lost. Count those separately so LOSS% stays honest
             * on a sparse/idle link. */
            if (dt > 1 && dt <= 64)      gMiss += (uint64_t)(dt - 1);  /* plausible in-window loss */
            else if (dt > 64)            gBigJump++;                   /* SN discontinuity, not loss */
            if ((f[1] & 0x08) && dt <= 0) gRetryDup++;        /* AP resend we actually received */
        }
        gLastSeqTid[tid] = seq;
    }
    /* header = 24 + QoS-control(2) + HT-Control(4, present when Order/+HTC, fc bit15)
     * + CCMP-header(8, present when Protected, fc bit14). VHT APs set +HTC on data
     * frames for link-adaptation feedback; missing those 4 bytes shifts the payload
     * and the frame fails to parse (the residual `pdrop`). The HW decrypts in place
     * but leaves the CCMP header, so the MSDU starts after it. */
    uint32_t hdr = 24 + (qos ? 2 : 0) + (((fc >> 15) & 1) ? 4 : 0) + (((fc >> 14) & 1) ? 8 : 0);
    if (pkt_len < hdr + 8) { gDrxParse++; return; }

    /* A-MSDU (QoS control octet0 bit7): one MPDU carries several Ethernet subframes
     * back-to-back — {DA(6),SA(6),len(2),MSDU(len)} each padded to 4 bytes. These are
     * the efficient aggregated frames; not splitting them was dropping ~10% of RX
     * (the `pdrop` counter) and starving TCP. Deliver each subframe up the stack. */
    if (qos && (f[24] & 0x80)) {
        const uint8_t *p = f + hdr;
        uint32_t rem = pkt_len - hdr;
        int delivered = 0;
        while (rem >= 14) {
            uint32_t sublen = (uint32_t)((p[12] << 8) | p[13]);  /* MSDU length */
            if (sublen < 8 || 14 + sublen > rem) break;
            const uint8_t *sllc = p + 14;                        /* MSDU = LLC/SNAP + payload */
            if (sllc[0] == 0xaa && sllc[1] == 0xaa && sllc[2] == 0x03) {
                uint32_t plen = sublen - 8;
                if (plen <= 1600) {
                    uint8_t eth[1614];
                    memcpy(eth, p, 12);                          /* subframe DA + SA */
                    eth[12] = sllc[6]; eth[13] = sllc[7];        /* ethertype */
                    memcpy(eth + 14, sllc + 8, plen);
                    inputEth(eth, 14 + plen);
                    delivered++;
                }
            }
            uint32_t adv = (14 + sublen + 3) & ~3u;              /* next subframe (4-byte aligned) */
            if (adv > rem) break;
            p += adv; rem -= adv;
        }
        if (!delivered) gDrxParse++;
        return;
    }

    const uint8_t *llc = f + hdr;
    if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03) { gDrxParse++; return; }
    uint16_t et = (uint16_t)((llc[6] << 8) | llc[7]);
    uint32_t plen = pkt_len - hdr - 8;
    if (plen > 1600) { gDrxParse++; return; }
    uint8_t eth[1614];
    memcpy(eth, f + 4, 6);        /* addr1 = DA (us / bcast / mcast) */
    memcpy(eth + 6, f + 16, 6);   /* addr3 = SA                       */
    eth[12] = (uint8_t)(et >> 8); eth[13] = (uint8_t)et;
    memcpy(eth + 14, llc + 8, plen);
    inputEth(eth, 14 + plen);
}

/* RX poll loop (its own kernel thread): drain the HW RX ring, deliver frames.
 * Tight when busy (low latency), 1ms idle sleep when quiet (low CPU). Direct
 * MMIO + DMA reads, no syscalls. */
void RTW88Ethernet::rxLoop()
{
    uint32_t idle = 0;
    while (!fRxStop) {
        uint32_t hw = (mmioR32(RTK_PCI_RXBD_IDX_MPDUQ_K) >> 16) & 0xfff;
        uint32_t depth = (hw - fRxRp + fRxN) % fRxN;    /* current ring occupancy */
        if (depth > gDrxMaxDepth) gDrxMaxDepth = depth;
        if (depth >= fRxN - 2) gRxRingFull++;   /* HW has lapped us -> frames overwritten */
        bool got = false, delivered = false;
        while (fRxRp != hw && !fRxStop) {
            uint8_t *buf = fRxData + (uint64_t)fRxRp * fRxBufSz;
            uint32_t w0 = *(volatile uint32_t *)buf;
            uint32_t pkt_len = w0 & 0x3fff, drv = (w0 >> 16) & 0xf, shift = (w0 >> 24) & 0x3;
            if (w0 & ((1u << 14) | (1u << 15))) {      /* W0 CRC32/ICV_ERR: corrupt -> drop */
                gDrxErr++;
            } else if (pkt_len >= 24 && pkt_len <= fRxBufSz) {
                uint32_t rate = (*(volatile uint32_t *)(buf + 12)) & 0x7f;   /* RX desc W3[6:0] */
                if (rate >= 12) gDrxMcs++;             /* >=12 = HT/VHT MCS */
                gRateSum += rate; gRateN++;            /* avg + peak PHY rate the AP gives us */
                if (rate > gRateMax) gRateMax = rate;
                gDrxBytes += pkt_len;
                uint32_t off = fRxDescSz + shift + drv * 8;
                if (off + pkt_len <= fRxBufSz) { rxFrame(buf + off, pkt_len); delivered = true; }
            }
            fRxRp = (fRxRp + 1) % fRxN;
            got = true;
        }
        if (got) mmioW16(RTK_PCI_RXBD_IDX_MPDUQ_K, (uint16_t)(fRxRp & 0xfff));   /* release */
        if (delivered && fNetif) fNetif->flushInputQueue();   /* one stack flush per drain (batched) */
        /* low-latency idle: keep spinning briefly after activity (download inter-burst
         * gaps are sub-ms — a 1ms sleep there inflates the RTT and paces TCP down),
         * fall back to a real sleep only once the link is genuinely quiet. */
        if (got) idle = 0;
        else if (++idle < 64) IODelay(50);
        else IOSleep(1);
    }
    fRxDone = true;
}

void RTW88Ethernet::rxThreadTrampoline(void *arg, wait_result_t)
{
    RTW88Ethernet *me = (RTW88Ethernet *)arg;
    me->rxLoop();
    thread_terminate(current_thread());
}

bool RTW88Ethernet::startData(RTW88Server *server, const struct rtw_data_cfg *cfg)
{
    fMmio = server->mmio();
    if (!fMmio) return false;

    uint8_t *txbdV, *rxV; uint64_t txbdPa, txbdSz, rxPa, rxSz;
    if (!server->dmaResolve(cfg->txbd_handle, &txbdV, &txbdPa, &txbdSz)) return false;
    if ((uint64_t)cfg->txbd_be_off + (uint64_t)cfg->be_len * cfg->bd_sz > txbdSz) return false;
    if (!server->dmaResolve(cfg->rxdata_handle, &rxV, &rxPa, &rxSz)) return false;
    if ((uint64_t)cfg->rx_nslots * cfg->rx_buf_size > rxSz) return false;
    if (!cfg->be_len || !cfg->rx_nslots || cfg->desc_sz > 64) return false;

    fBeBd = txbdV + cfg->txbd_be_off;
    fBeLen = cfg->be_len; fBdSz = cfg->bd_sz; fDescSz = cfg->desc_sz;
    fRxData = rxV; fRxN = cfg->rx_nslots; fRxBufSz = cfg->rx_buf_size; fRxDescSz = cfg->rx_desc_sz;
    memcpy(fMac.bytes, cfg->mac, 6);
    memcpy(fBssid, cfg->bssid, 6);
    fRate   = cfg->rate;
    fRateId = cfg->rate_id ? cfg->rate_id : 7;   /* fall back to legacy OFDM raid */

    /* our own per-slot TX buffer pool (the userspace BE ring shared one buffer) */
    fTxSlotSz = RTW_ETH_SLOT_SIZE;
    fTxPool = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionOut | kIOMemoryPhysicallyContiguous,
        (uint64_t)fBeLen * fTxSlotSz, 0x00000000FFFFF000ULL);
    if (!fTxPool || fTxPool->prepare() != kIOReturnSuccess) return false;
    fTxPoolV = (uint8_t *)fTxPool->getBytesNoCopy();
    IOByteCount seg = 0;
    fTxPoolPa = fTxPool->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!fTxPoolV || !fTxPoolPa) return false;

    /* take over the rings at their current positions */
    fBeWp = mmioR32(RTK_PCI_TXBD_IDX_BEQ_K) & 0xfff;
    fRxRp = (mmioR32(RTK_PCI_RXBD_IDX_MPDUQ_K) >> 16) & 0xfff;
    fCcmpPn = 1;
    fDataMode = true;

    fRxStop = false; fRxDone = false;
    if (kernel_thread_start(&RTW88Ethernet::rxThreadTrampoline, this, &fRxThread) != KERN_SUCCESS) {
        fDataMode = false;
        return false;
    }
    IOLog("RTW-data: in-kext data path running (BE %u slots, RX %u x %u)\n",
          fBeLen, fRxN, fRxBufSz);
    return true;
}

void RTW88Ethernet::stopData()
{
    if (!fDataMode) return;
    fRxStop = true;
    for (int i = 0; i < 400 && !fRxDone; i++) IOSleep(5);   /* wait up to ~2s */
    if (fRxThread != THREAD_NULL) { thread_deallocate(fRxThread); fRxThread = THREAD_NULL; }
    fDataMode = false;
    if (fTxPool) { fTxPool->complete(); fTxPool->release(); fTxPool = nullptr; }
    IOLog("RTW-data: data path stopped\n");
}

IOReturn RTW88Server::dataStart(const struct rtw_data_cfg *cfg)
{
    if (fEth) return kIOReturnBusy;
    RTW88Ethernet *eth = new RTW88Ethernet;
    if (!eth) return kIOReturnNoMemory;
    if (!eth->init(NULL)) { eth->release(); return kIOReturnError; }
    eth->prepare(cfg->mac);
    if (!eth->attach(this)) { eth->release(); return kIOReturnError; }
    if (!eth->start(this)) { eth->detach(this); eth->release(); return kIOReturnError; }
    if (!eth->startData(this, cfg)) {
        eth->terminate(kIOServiceSynchronous); eth->release();
        return kIOReturnError;
    }
    fEth = eth;
    IOLog("RTW-bridge: data path up (enX + in-kext TX/RX)\n");
    return kIOReturnSuccess;
}

IOReturn RTW88Server::dataStop()
{
    if (!fEth) return kIOReturnSuccess;
    RTW88Ethernet *eth = fEth;
    fEth = nullptr;
    eth->setLinkUp(false);
    eth->stopData();
    eth->terminate(kIOServiceSynchronous);
    eth->release();
    IOLog("RTW-bridge: data path down\n");
    return kIOReturnSuccess;
}

IOReturn RTW88Server::dataLink(bool up)
{
    if (!fEth) return kIOReturnNotReady;
    fEth->setLinkUp(up);
    return kIOReturnSuccess;
}

IOReturn RTW88Server::dataStats(uint64_t out[21])
{
    out[0] = gDtx;     out[1] = gDtxDrop; out[2] = gDtxBytes; out[3] = gDrx;
    out[4] = gDrxBytes; out[5] = gDrxRetry; out[6] = gDrxMcs; out[7] = gDrxMaxDepth;
    out[8] = gAddba;   /* cumulative — running total of Block-Ack sessions accepted */
    out[9] = gDrxErr;  out[10] = gDrxParse; out[11] = gDrxGap; out[12] = gDrxBack;
    out[13] = gMiss;   out[14] = gRxRingFull; out[15] = gRateSum; out[16] = gRateN;
    out[17] = gRateMax; out[18] = gRetryDup;  out[19] = gTidMask; out[20] = gBigJump;
    gDtx = gDtxDrop = gDtxBytes = gDrx = gDrxBytes = gDrxRetry = gDrxMcs = gDrxMaxDepth = 0;
    gDrxErr = gDrxParse = gDrxGap = gDrxBack = 0;
    gMiss = gRxRingFull = gRateSum = gRateN = gRateMax = gRetryDup = gBigJump = 0; gTidMask = 0;
    /* NB: gLastSeqTid[] is running delta state — deliberately NOT reset (would fake a
     * gap on the first frame of each interval). */
    return kIOReturnSuccess;
}

#pragma mark - User client (thin RPC shim over the bridge)

class RTW88ServerUserClient : public IOUserClient {
    OSDeclareDefaultStructors(RTW88ServerUserClient)
public:
    virtual bool     initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    virtual bool     start(IOService *provider) override;
    virtual IOReturn clientClose() override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference) override;
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory) override;

    /* read access to fOwner for the static dispatchers (below) */
    RTW88Server *ownerForDispatch() const { return fOwner; }

    /* External-method dispatchers. Public so the file-scope sMethods[] table can
     * take their addresses; they only touch the user client via fOwner. */
    static IOReturn sPing(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sRegRead(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sRegWrite(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sCfgRead(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sCfgWrite(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sDmaAlloc(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sDmaFree(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sRegWriteDma(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sSetMacPower(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sIrqEnable(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sIrqDisable(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sIrqWait(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sIrqStatus(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sPower(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sDataStart(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sDataStop(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sDataLink(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sDataStats(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sKInit(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sKBringup(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sKScan(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sKConnect(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sKDisconnect(OSObject *, void *, IOExternalMethodArguments *);

private:
    RTW88Server *fOwner = nullptr;
};

OSDefineMetaClassAndStructors(RTW88ServerUserClient, IOUserClient)

/* dispatch table — order MUST match the kBridge* enum */
static const IOExternalMethodDispatch sMethods[kBridgeNumMethods] = {
    [kBridgePing]        = { &RTW88ServerUserClient::sPing,        0, 0, 4, 0 },
    [kBridgeRegRead]     = { &RTW88ServerUserClient::sRegRead,     2, 0, 1, 0 },
    [kBridgeRegWrite]    = { &RTW88ServerUserClient::sRegWrite,    3, 0, 0, 0 },
    [kBridgeCfgRead]     = { &RTW88ServerUserClient::sCfgRead,     2, 0, 1, 0 },
    [kBridgeCfgWrite]    = { &RTW88ServerUserClient::sCfgWrite,    3, 0, 0, 0 },
    [kBridgeDmaAlloc]    = { &RTW88ServerUserClient::sDmaAlloc,    1, 0, 3, 0 },
    [kBridgeDmaFree]     = { &RTW88ServerUserClient::sDmaFree,     1, 0, 0, 0 },
    [kBridgeRegWriteDma] = { &RTW88ServerUserClient::sRegWriteDma, 4, 0, 0, 0 },
    [kBridgeSetMacPower] = { &RTW88ServerUserClient::sSetMacPower, 1, 0, 0, 0 },
    [kBridgeIrqEnable]   = { &RTW88ServerUserClient::sIrqEnable,   0, 0, 0, 0 },
    [kBridgeIrqDisable]  = { &RTW88ServerUserClient::sIrqDisable,  0, 0, 0, 0 },
    [kBridgeIrqWait]     = { &RTW88ServerUserClient::sIrqWait,     1, 0, 1, 0 },
    [kBridgeIrqStatus]   = { &RTW88ServerUserClient::sIrqStatus,   2, 0, 1, 0 },
    [kBridgePower]       = { &RTW88ServerUserClient::sPower,       2, 0, 0, 0 },
    [kBridgeDataStart]   = { &RTW88ServerUserClient::sDataStart,   0, sizeof(struct rtw_data_cfg), 0, 0 },
    [kBridgeDataStop]    = { &RTW88ServerUserClient::sDataStop,    0, 0, 0, 0 },
    [kBridgeDataLink]    = { &RTW88ServerUserClient::sDataLink,    1, 0, 0, 0 },
    [kBridgeDataStats]   = { &RTW88ServerUserClient::sDataStats,   0, 0, 0, sizeof(uint64_t) * 21 },
    [kBridgeKInit]       = { &RTW88ServerUserClient::sKInit,       0, 0, 1, 0 },
    [kBridgeKBringup]    = { &RTW88ServerUserClient::sKBringup,    0, 0, 1, 0 },
    [kBridgeKScan]       = { &RTW88ServerUserClient::sKScan,       0, 0, 0, sizeof(struct rtw_scan_result) },
    [kBridgeKConnect]    = { &RTW88ServerUserClient::sKConnect,    0, sizeof(struct rtw_connect_req), 0, sizeof(struct rtw_connect_result) },
    [kBridgeKDisconnect] = { &RTW88ServerUserClient::sKDisconnect, 0, 0, 0, 0 },
};

bool RTW88ServerUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    return IOUserClient::initWithTask(owningTask, securityID, type);
}

bool RTW88ServerUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    fOwner = OSDynamicCast(RTW88Server, provider);
    return fOwner != nullptr;
}

IOReturn RTW88ServerUserClient::clientClose()
{
    terminate();
    return kIOReturnSuccess;
}

IOReturn RTW88ServerUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                                   IOExternalMethodDispatch *dispatch, OSObject *target,
                                                   void *reference)
{
    if (selector < kBridgeNumMethods) {
        dispatch = (IOExternalMethodDispatch *)&sMethods[selector];
        if (!target) target = this;
    }
    return IOUserClient::externalMethod(selector, args, dispatch, target, reference);
}

/* clientMemoryForType: map DMA buffer `type` (== handle) into the client task. */
IOReturn RTW88ServerUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                        IOMemoryDescriptor **memory)
{
    if (!fOwner) return kIOReturnNotAttached;
    IOMemoryDescriptor *md = fOwner->dmaDescriptorForHandle(type);
    if (!md) return kIOReturnBadArgument;
    *options = 0;
    *memory  = md;   /* retained by dmaDescriptorForHandle; framework consumes the ref */
    return kIOReturnSuccess;
}

/* ---- static dispatchers: cast target -> user client -> owner ------------- */
#define UC(target)   OSDynamicCast(RTW88ServerUserClient, (target))

/* fOwner is reachable via the public ownerForDispatch() accessor. */
static inline RTW88Server *RTW88ServerUserClient_owner(RTW88ServerUserClient *uc)
{
    return uc ? uc->ownerForDispatch() : nullptr;
}

IOReturn RTW88ServerUserClient::sPing(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    a->scalarOutput[0] = RTW_BRIDGE_PING_MAGIC;
    a->scalarOutput[1] = RTW_BRIDGE_ABI_VERSION;
    a->scalarOutput[2] = ((uint64_t)o->vendorID() << 16) | o->deviceID();
    a->scalarOutput[3] = o->barLength();
    return kIOReturnSuccess;
}
IOReturn RTW88ServerUserClient::sRegRead(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->regRead((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1], &a->scalarOutput[0]);
}
IOReturn RTW88ServerUserClient::sRegWrite(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->regWrite((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1], a->scalarInput[2]);
}
IOReturn RTW88ServerUserClient::sCfgRead(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->cfgRead((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1], &a->scalarOutput[0]);
}
IOReturn RTW88ServerUserClient::sCfgWrite(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->cfgWrite((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1], a->scalarInput[2]);
}
IOReturn RTW88ServerUserClient::sDmaAlloc(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    uint64_t h = 0, pa = 0;
    IOReturn r = o->dmaAlloc(a->scalarInput[0], &h, &pa);
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = pa & 0xFFFFFFFF;
    a->scalarOutput[2] = pa >> 32;
    return r;
}
IOReturn RTW88ServerUserClient::sDmaFree(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->dmaFree(a->scalarInput[0]);
}
IOReturn RTW88ServerUserClient::sRegWriteDma(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->regWriteDma((uint32_t)a->scalarInput[0], a->scalarInput[1], a->scalarInput[2], (uint32_t)a->scalarInput[3]);
}
IOReturn RTW88ServerUserClient::sSetMacPower(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    o->setMacPower(a->scalarInput[0] != 0);
    return kIOReturnSuccess;
}
IOReturn RTW88ServerUserClient::sIrqEnable(OSObject *t, void *, IOExternalMethodArguments *)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->irqEnable();
}
IOReturn RTW88ServerUserClient::sIrqDisable(OSObject *t, void *, IOExternalMethodArguments *)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->irqDisable();
}
IOReturn RTW88ServerUserClient::sIrqWait(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    uint64_t d = 0;
    IOReturn r = o->irqWait((uint32_t)a->scalarInput[0], &d);
    a->scalarOutput[0] = d;
    return r;
}
IOReturn RTW88ServerUserClient::sIrqStatus(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->irqStatus((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1], &a->scalarOutput[0]);
}
IOReturn RTW88ServerUserClient::sPower(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->power(a->scalarInput[0] != 0, a->scalarInput[1] != 0);
}
IOReturn RTW88ServerUserClient::sDataStart(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    if (!a->structureInput || a->structureInputSize < sizeof(struct rtw_data_cfg))
        return kIOReturnBadArgument;
    return o->dataStart((const struct rtw_data_cfg *)a->structureInput);
}
IOReturn RTW88ServerUserClient::sDataStop(OSObject *t, void *, IOExternalMethodArguments *)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->dataStop();
}
IOReturn RTW88ServerUserClient::sDataLink(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    return o->dataLink(a->scalarInput[0] != 0);
}
IOReturn RTW88ServerUserClient::sDataStats(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    /* struct output (scalar methods cap at 16 values; we return 20). */
    if (!a->structureOutput || a->structureOutputSize < sizeof(uint64_t) * 21) return kIOReturnNoSpace;
    return o->dataStats((uint64_t *)a->structureOutput);
}
/* run the power-on sequence in-kernel (kctl.c). */
IOReturn RTW88ServerUserClient::sKInit(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    a->scalarOutput[0] = rtw_kctl_poweron();
    return kIOReturnSuccess;
}
/* full in-kernel bring-up (power->rings->fw->mac_init->efuse->phy). */
IOReturn RTW88ServerUserClient::sKBringup(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    a->scalarOutput[0] = rtw_kctl_bringup();
    return kIOReturnSuccess;
}
/* bring-up + in-kernel scan, results into the fixed structureOutput. */
IOReturn RTW88ServerUserClient::sKScan(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    if (!a->structureOutput || a->structureOutputSize < sizeof(struct rtw_scan_result))
        return kIOReturnNoSpace;
    rtw_kctl_scan((struct rtw_scan_result *)a->structureOutput);
    return kIOReturnSuccess;
}
/* in-kernel connect (bring-up + scan + auth/assoc/WPA2/keys/media
 * + data-path start); returns status + the card MAC so the CLI can DHCP enX. */
IOReturn RTW88ServerUserClient::sKConnect(OSObject *t, void *, IOExternalMethodArguments *a)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    if (!a->structureInput  || a->structureInputSize  < sizeof(struct rtw_connect_req))    return kIOReturnBadArgument;
    if (!a->structureOutput || a->structureOutputSize < sizeof(struct rtw_connect_result)) return kIOReturnNoSpace;
    rtw_kctl_connect((const struct rtw_connect_req *)a->structureInput,
                     (struct rtw_connect_result *)a->structureOutput);
    return kIOReturnSuccess;
}
/* tear down the in-kernel connection (remove enX + free rings). */
IOReturn RTW88ServerUserClient::sKDisconnect(OSObject *t, void *, IOExternalMethodArguments *)
{
    RTW88Server *o = RTW88ServerUserClient_owner(UC(t)); if (!o) return kIOReturnNotAttached;
    rtw_kctl_disconnect();
    return kIOReturnSuccess;
}

#pragma mark - newUserClient + owner accessor

IOReturn RTW88Server::newUserClient(task_t owningTask, void *securityID,
                                        UInt32 type, IOUserClient **handler)
{
    RTW88ServerUserClient *uc = new RTW88ServerUserClient;
    if (!uc) return kIOReturnNoMemory;
    if (!uc->initWithTask(owningTask, securityID, type) || !uc->attach(this) || !uc->start(this)) {
        if (uc->isInactive() == false) uc->detach(this);
        uc->release();
        return kIOReturnError;
    }
    *handler = uc;
    return kIOReturnSuccess;
}

/* ---- C control shim: the bring-up C's hw_dma + hw_power resolve here ----
 * (the MMIO hw_read / hw_write live in kctl.c via g_kmmio; these need the IOKit
 * object, so they wrap g_dev's existing DMA/PCI methods). */
extern "C" int hw_dma_alloc(uint64_t size, uint64_t *handle, uint64_t *paddr, void **vaddr)
{
    if (!g_dev) return -1;
    uint64_t h = 0, pa = 0;
    if (g_dev->dmaAlloc(size, &h, &pa) != kIOReturnSuccess) return -1;
    *handle = h; *paddr = pa; *vaddr = g_dev->dmaVaddr(h);
    return *vaddr ? 0 : -1;
}
extern "C" void hw_dma_free(uint64_t handle, void *vaddr, uint64_t size)
{
    (void)vaddr; (void)size;
    if (g_dev) g_dev->dmaFree(handle);
}
extern "C" int hw_reg_write_dma(uint32_t reg_off, uint64_t handle, uint64_t buf_off, uint32_t width)
{
    if (!g_dev) return -1;
    return g_dev->regWriteDma(reg_off, handle, buf_off, width) == kIOReturnSuccess ? 0 : -1;
}
extern "C" void hw_power(bool mem_enable, bool busmaster)
{
    if (g_dev) g_dev->power(mem_enable, busmaster);
}
/* the in-kernel bring-up asserts MAC-power so irqEnable() (the scan's IRQ arm) is
 * allowed — userspace does this via kBridgeSetMacPower after power-on. */
extern "C" void hw_set_mac_power(bool on)
{
    if (g_dev) g_dev->setMacPower(on);
}
/* start the in-kernel data path (publish enX, run RX/TX) right after the
 * in-kernel connect, using the kext-allocated trx rings (cfg handles resolve to the
 * same fDma slots). */
extern "C" int hw_data_start(const struct rtw_data_cfg *cfg)
{
    return (g_dev && g_dev->dataStart(cfg) == kIOReturnSuccess) ? 0 : -1;
}
extern "C" void hw_data_link(int up)
{
    if (g_dev) g_dev->dataLink(up != 0);
}
/* tear the in-kernel data path down (remove enX) — macOS/configd then falls
 * back to other interfaces. */
extern "C" void hw_data_stop(void)
{
    if (g_dev) g_dev->dataStop();
}

/* IRQ shim — the in-kernel scan (scan.c) arms MSI + blocks on it per dwell. Wraps
 * g_dev's existing IOInterruptEventSource machinery (same path the userspace scan
 * drives via kBridgeIrq*). */
extern "C" int hw_irq_enable(void)
{
    return (g_dev && g_dev->irqEnable() == kIOReturnSuccess) ? 0 : -1;
}
extern "C" void hw_irq_disable(void)
{
    if (g_dev) g_dev->irqDisable();
}
extern "C" uint64_t hw_irq_wait(uint32_t timeout_ms)
{
    uint64_t fired = 0;
    if (g_dev) g_dev->irqWait(timeout_ms, &fired);
    return fired;
}
