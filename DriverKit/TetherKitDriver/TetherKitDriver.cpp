/* TetherKitDriver.cpp
 * TetherKit — DriverKit RNDIS USB tethering driver for macOS.
 * HoRNDIS — RNDIS USB tethering driver for macOS
 *
 *   Copyright (c) 2012 Joshua Wise.
 *   Copyright (c) 2018 Mikhail Iakhiaev
 *
 * RNDIS logic derived from linux/drivers/net/usb/rndis_host.c:
 *   Copyright (c) 2005 David Brownell.
 *
 * Ported to DriverKit by Prostec Labs, 2026.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "TetherKitDriver.h"
#pragma clang diagnostic pop
#include "RNDISProtocol.h"
#include "RNDISProtocolCore.h"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOTypes.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODataQueueDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>

#include <USBDriverKit/AppleUSBDefinitions.h>
#include <USBDriverKit/AppleUSBDescriptorParsing.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include <NetworkingDriverKit/IOUserNetworkEthernet.h>
#include <NetworkingDriverKit/IOUserNetworkPacket.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkPacketQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>

#include <os/log.h>

// DriverKit's <os/log.h> exposes only OS_LOG_DEFAULT (no os_log_create), so
// subsystem/category tagging is done in the format string. Filter live with:
//   log stream --predicate 'process == "com.prostec.tetherkit.driver"'
// Add `&& eventMessage CONTAINS "[error]"` to scope to a single category.
#define LOG_INFO(fmt, ...)  os_log(OS_LOG_DEFAULT, "[TetherKit][main] "  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) os_log(OS_LOG_DEFAULT, "[TetherKit][error] " fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) do {} while (0)

#define super IOUserNetworkEthernet

struct TetherKitDriver_IVars {
    IOUSBHostInterface * commInterface;
    IOUSBHostInterface * dataInterface;
    IOUSBHostPipe * inPipe;
    IOUSBHostPipe * outPipe;
    uint8_t commIfNumber;

    uint32_t rndisXid;
    uint32_t maxOutTransferSize;
    uint16_t outPipeMaxPacketSize;   // wMaxPacketSize of the bulk-OUT endpoint
    uint8_t  pad_[2];                // keep struct alignment explicit
    uint8_t macAddr[6];

    IOBufferMemoryDescriptor * inBufs[N_IN_BUFS];
    OSAction * inActions[N_IN_BUFS];
    uint64_t inBufAddrs[N_IN_BUFS];

    IOBufferMemoryDescriptor * outBufs[N_OUT_BUFS];
    OSAction * outActions[N_OUT_BUFS];
    uint64_t outBufAddrs[N_OUT_BUFS];
    IOUserNetworkPacket * pendingTx[N_OUT_BUFS];
    uint8_t outBufStack[N_OUT_BUFS];
    int numFreeOutBufs;

    IOUserNetworkPacketBufferPool * pktPool;
    IOUserNetworkTxSubmissionQueue * txSubmission;
    IOUserNetworkTxCompletionQueue * txCompletion;
    IOUserNetworkRxSubmissionQueue * rxSubmission;
    IOUserNetworkRxCompletionQueue * rxCompletion;
    IODataQueueDispatchSource * txSource;
    IODispatchQueue * networkDispatch;

    IOBufferMemoryDescriptor * cmdBuf;
    bool running;
    bool stopping;
    uint32_t cancellations;
    uint32_t consecutiveErrors[3];
    uint64_t totalErrors[3];
    bool pipeStopped[3];
    uint32_t submittedLengths[N_OUT_BUFS];
    OSAction * txAction;
    bool rndisInitDone;

    // --- Interrupt-IN notification pipe (comm interface) ---
    IOUSBHostPipe            * intPipe;       // CDC interrupt IN endpoint
    OSAction                 * intAction;     // async completion action
    IOBufferMemoryDescriptor * intBuf;        // backing buffer for notification
    uint64_t                   intBufAddr;    // mapped virtual address
    uint32_t                   intBufLen;     // buffer length (== wMaxPacketSize)
    bool                       responseAvailable; // set by InterruptReadComplete
    bool                       intArmed;     // true while AsyncIO is outstanding
};

#define IVARS ((TetherKitDriver_IVars *) ivars)

static inline bool isRNDISControlInterface(const IOUSBInterfaceDescriptor * d) {
    // Stock Android: Wireless Controller / Radio Frequency / RNDIS
    if (d->bInterfaceClass == 0xE0 && d->bInterfaceSubClass == 0x01 && d->bInterfaceProtocol == 0x03)
        return true;
    // RNDIS over Ethernet: Miscellaneous Device (Nokia 7+, Sony Xperia XZ)
    if (d->bInterfaceClass == 0xEF && d->bInterfaceSubClass == 0x04 && d->bInterfaceProtocol == 0x01)
        return true;
    // Linux USB gadget RNDIS (f_rndis.c): CDC Control / ACM / Vendor-specific
    if (d->bInterfaceClass == 0x02 && d->bInterfaceSubClass == 0x02 && d->bInterfaceProtocol == 0xFF)
        return true;
    return false;
}

static constexpr int kMaxAttempts = 10;
static constexpr uint32_t kRetryDelayMs = 20;
static constexpr uint32_t kTimeoutMs = 5000;
static constexpr uint32_t kMinResponseBytes = 12;
static constexpr uint16_t kEthernetHeaderLength = 14;
static constexpr uint32_t kEthernetFrameLength = ETHERNET_MTU + kEthernetHeaderLength;

static inline bool isUsbGoneStatus(IOReturn rc) {
    return rc == kIOReturnAborted || rc == kIOReturnNotResponding || rc == kIOReturnNoDevice;
}

bool
TetherKitDriver::init()
{
    if (!super::init()) {
        return false;
    }

    ivars = IONewZero(TetherKitDriver_IVars, 1);
    if (!ivars) {
        return false;
    }

    IVARS->rndisXid = 1;
    for (int i = 0; i < N_OUT_BUFS; i++) {
        IVARS->outBufStack[i] = static_cast<uint8_t>(i);
    }
    return true;
}

void
TetherKitDriver::free()
{
    if (ivars) {
        for (int i = 0; i < N_IN_BUFS; i++) {
            OSSafeReleaseNULL(IVARS->inBufs[i]);
            OSSafeReleaseNULL(IVARS->inActions[i]);
        }
        for (int i = 0; i < N_OUT_BUFS; i++) {
            OSSafeReleaseNULL(IVARS->outBufs[i]);
            OSSafeReleaseNULL(IVARS->outActions[i]);
        }
        OSSafeReleaseNULL(IVARS->intBuf);
        OSSafeReleaseNULL(IVARS->intAction);
        OSSafeReleaseNULL(IVARS->intPipe);
        OSSafeReleaseNULL(IVARS->cmdBuf);
        OSSafeReleaseNULL(IVARS->inPipe);
        OSSafeReleaseNULL(IVARS->outPipe);
        OSSafeReleaseNULL(IVARS->dataInterface);
        OSSafeReleaseNULL(IVARS->commInterface);
        for (int i = 0; i < N_OUT_BUFS; i++) {
            if (IVARS->pendingTx[i] && IVARS->pktPool) {
                IVARS->pktPool->DeallocatePacket(IVARS->pendingTx[i]);
                IVARS->pendingTx[i] = nullptr;
            }
        }
        releaseNetworkQueues();
        IOSafeDeleteNULL(ivars, TetherKitDriver_IVars, 1);
    }
    super::free();
}

kern_return_t
TetherKitDriver::Start_Impl(IOService * provider)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Provider is IOUSBHostDevice for the WirelessControllerDevice personality
    // (Samsung S7 Edge, class 224/0/0) or IOUSBHostInterface for the three
    // interface-level personalities that cover all other RNDIS devices.
    IOUSBHostDevice * device = OSDynamicCast(IOUSBHostDevice, provider);
    if (device) {
        ret = openCommInterfaceFromDevice(device);
        if (ret != kIOReturnSuccess) {
            goto bail;
        }
    } else {
        IOUSBHostInterface * iface = OSDynamicCast(IOUSBHostInterface, provider);
        if (!iface) {
            ret = kIOReturnBadArgument;
            goto bail;
        }
        ret = iface->Open(this, 0, nullptr);
        if (ret != kIOReturnSuccess) {
            goto bail;
        }
        iface->retain();
        IVARS->commInterface = iface;

        // CopyConfigurationDescriptor() returns a newly-allocated buffer that
        // the caller must release via IOUSBHostFreeDescriptor().
        const IOUSBConfigurationDescriptor * cfgDesc = IVARS->commInterface->CopyConfigurationDescriptor();
        if (!cfgDesc) {
            ret = kIOReturnError;
            goto bail;
        }
        const IOUSBInterfaceDescriptor * desc = IVARS->commInterface->GetInterfaceDescriptor(cfgDesc);
        if (!desc) {
            IOUSBHostFreeDescriptor(cfgDesc);
            ret = kIOReturnError;
            goto bail;
        }
        IVARS->commIfNumber = desc->bInterfaceNumber;
        IOUSBHostFreeDescriptor(cfgDesc);
    }

    ret = openDataInterface();
    if (ret != kIOReturnSuccess) {
        goto bail;
    }

    ret = openInterruptPipe();
    if (ret != kIOReturnSuccess) {
        // Non-fatal: device may not advertise an interrupt-IN endpoint.
        LOG_ERROR("openInterruptPipe failed (non-fatal): %08x", ret);
    }

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, RNDIS_CMD_BUF_SZ, 0, &IVARS->cmdBuf);
    if (ret != kIOReturnSuccess || !IVARS->cmdBuf) {
        ret = kIOReturnNoMemory;
        goto bail;
    }

    armInterruptRead(); // best-effort; no-op if intPipe is null

    ret = rndisInit();
    if (ret != kIOReturnSuccess) {
        goto bail;
    }

    ret = queryMacAddress();
    if (ret != kIOReturnSuccess) {
        goto bail;
    }

    ret = allocateTransferResources();
    if (ret != kIOReturnSuccess) {
        goto bail;
    }

    ret = setupNetworkInterface();
    if (ret != kIOReturnSuccess) {
        goto bail;
    }

    ret = rndisSetPacketFilter(RNDIS_DEFAULT_FILTER);
    if (ret != kIOReturnSuccess) {
        goto bail;
    }

    IVARS->running = true;
    ret = scheduleInboundReads();
    if (ret != kIOReturnSuccess) {
        goto bail;
    }

    reportLinkStatus(kIOUserNetworkLinkStatusActive, 0);
    pumpTxQueue();
    RegisterService();
    return kIOReturnSuccess;

bail:
    Stop_Impl(provider);
    return ret == kIOReturnSuccess ? kIOReturnError : ret;
}

kern_return_t
TetherKitDriver::Stop_Impl(IOService * provider)
{
    if (IVARS->stopping) return kIOReturnSuccess;
    IVARS->stopping = true;
    IVARS->running = false;
    if (IVARS->txSource) IVARS->txSource->SetDataAvailableHandler(nullptr);
    reportLinkStatus(kIOUserNetworkLinkStatusInvalid, 0);
    // Only control endpoints accept a non-null forClient argument.
    if (IVARS->inPipe) IVARS->inPipe->Abort(0, kIOReturnAborted, nullptr);
    if (IVARS->outPipe) IVARS->outPipe->Abort(0, kIOReturnAborted, nullptr);
    if (IVARS->intPipe) IVARS->intPipe->Abort(0, kIOReturnAborted, nullptr);

    OSAction * actions[N_IN_BUFS + N_OUT_BUFS + 2] = {};
    uint32_t count = 0;
    for (auto * action : IVARS->inActions) if (action) actions[count++] = action;
    for (auto * action : IVARS->outActions) if (action) actions[count++] = action;
    if (IVARS->intAction) actions[count++] = IVARS->intAction;
    if (IVARS->txAction) actions[count++] = IVARS->txAction;
    if (!count) { finishStop(provider); return kIOReturnSuccess; }
    // Cancel's handler runs only after the action's callbacks have drained.
    // A sentinel prevents a synchronous completion from freeing state mid-loop.
    retain();
    provider->retain();
    IVARS->cancellations = count + 1;
    for (uint32_t i = 0; i < count; ++i) {
        kern_return_t result = actions[i]->Cancel(^{ cancelFinished(provider); });
        if (result != kIOReturnSuccess) {
            // Keep resources alive if cancellation cannot prove callback quiescence.
            LOG_ERROR("Stop: action cancellation failed: %08x", result);
        }
    }
    cancelFinished(provider);
    return kIOReturnSuccess;
}

void
TetherKitDriver::cancelFinished(IOService * provider)
{
    if (__atomic_sub_fetch(&IVARS->cancellations, 1, __ATOMIC_ACQ_REL) != 0) return;
    finishStop(provider);
    provider->release();
    release();
}

void
TetherKitDriver::finishStop(IOService * provider)
{
    if (IVARS->dataInterface) {
        IVARS->dataInterface->Close(this, 0);
        OSSafeReleaseNULL(IVARS->dataInterface);
    }
    if (IVARS->commInterface) {
        IVARS->commInterface->Close(this, 0);
        OSSafeReleaseNULL(IVARS->commInterface);
    }

    OSSafeReleaseNULL(IVARS->inPipe);
    OSSafeReleaseNULL(IVARS->outPipe);
    OSSafeReleaseNULL(IVARS->intBuf);
    OSSafeReleaseNULL(IVARS->intAction);
    OSSafeReleaseNULL(IVARS->intPipe);
    IVARS->intArmed = false;
    IVARS->responseAvailable = false;
    IVARS->intBufAddr = 0;
    IVARS->intBufLen = 0;

    for (int i = 0; i < N_IN_BUFS; i++) {
        OSSafeReleaseNULL(IVARS->inBufs[i]);
        OSSafeReleaseNULL(IVARS->inActions[i]);
    }
    for (int i = 0; i < N_OUT_BUFS; i++) {
        OSSafeReleaseNULL(IVARS->outBufs[i]);
        OSSafeReleaseNULL(IVARS->outActions[i]);
    }
    IVARS->numFreeOutBufs = 0;

    OSSafeReleaseNULL(IVARS->cmdBuf);
    for (int i = 0; i < N_OUT_BUFS; i++) {
        if (IVARS->pendingTx[i] && IVARS->pktPool) {
            IVARS->pktPool->DeallocatePacket(IVARS->pendingTx[i]);
            IVARS->pendingTx[i] = nullptr;
        }
    }
    releaseNetworkQueues();

    // Clear RNDIS handshake state so a subsequent Start_Impl re-initialises the
    // device cleanly rather than skipping rndisInit / queryMacAddress.
    IVARS->rndisInitDone = false;
    IVARS->maxOutTransferSize = 0;
    IVARS->commIfNumber = 0;
    __builtin_memset(IVARS->macAddr, 0, sizeof(IVARS->macAddr));

    Stop(provider, SUPERDISPATCH);
}

static constexpr uint8_t kUSBInterfaceClassCDCData = 0x0A;

kern_return_t
TetherKitDriver::openCommInterfaceFromDevice(IOUSBHostDevice * device)
{
    // Mirror HoRNDIS probeDevice: scan all configurations for an RNDIS control
    // interface immediately followed by a CDC-Data interface, then set that config.
    // Samsung S7 Edge exposes both an MTP-only config and a tethering config, so
    // hardcoding index 0 is not sufficient.
    const IOUSBDeviceDescriptor * devDesc = device->CopyDeviceDescriptor();
    if (!devDesc) {
        return kIOReturnError;
    }

    uint8_t configValue = 0;
    uint8_t controlIfNum = 0xFF;
    const IOUSBConfigurationDescriptor * winningCfg = nullptr;
    const uint8_t numConfigurations = devDesc->bNumConfigurations;
    IOUSBHostFreeDescriptor(devDesc);
    devDesc = nullptr;

    for (uint8_t i = 0; i < numConfigurations; i++) {
        const IOUSBConfigurationDescriptor * cfgDesc = device->CopyConfigurationDescriptor(i);
        if (!cfgDesc) {
            continue;
        }
        const IOUSBInterfaceDescriptor * intDesc = nullptr;
        while ((intDesc = IOUSBGetNextInterfaceDescriptor(
                    cfgDesc,
                    reinterpret_cast<const IOUSBDescriptorHeader *>(intDesc))) != nullptr) {
            if (!isRNDISControlInterface(intDesc)) {
                continue;
            }
            // Skip past any alt settings for this interface before checking
            // CDC-Data adjacency — an alt setting has the same bInterfaceNumber
            // but a non-CDC class, which would cause a false miss if unchecked.
            const IOUSBInterfaceDescriptor * next = intDesc;
            while ((next = IOUSBGetNextInterfaceDescriptor(
                        cfgDesc,
                        reinterpret_cast<const IOUSBDescriptorHeader *>(next))) != nullptr) {
                if (next->bInterfaceNumber != intDesc->bInterfaceNumber) {
                    break;
                }
            }
            if (next &&
                next->bInterfaceClass == kUSBInterfaceClassCDCData &&
                next->bInterfaceNumber == intDesc->bInterfaceNumber + 1) {
                configValue = cfgDesc->bConfigurationValue;
                controlIfNum = intDesc->bInterfaceNumber;
                winningCfg = cfgDesc; // transfer ownership; freed below
                break;
            }
        }
        if (winningCfg) {
            break;
        }
        IOUSBHostFreeDescriptor(cfgDesc);
    }

    if (!winningCfg) {
        return kIOReturnNotFound;
    }

    kern_return_t ret = device->SetConfiguration(configValue, false);
    if (ret != kIOReturnSuccess) {
        IOUSBHostFreeDescriptor(winningCfg);
        return ret;
    }

    uintptr_t iterator = 0;
    ret = device->CreateInterfaceIterator(&iterator);
    if (ret != kIOReturnSuccess) {
        IOUSBHostFreeDescriptor(winningCfg);
        return ret;
    }

    while (true) {
        IOUSBHostInterface * candidate = nullptr;
        ret = device->CopyInterface(iterator, &candidate);
        if (ret != kIOReturnSuccess || !candidate) {
            break;
        }
        const IOUSBInterfaceDescriptor * d = candidate->GetInterfaceDescriptor(winningCfg);
        if (d && d->bInterfaceNumber == controlIfNum) {
            IVARS->commInterface = candidate; // CopyInterface already retains
            break;
        }
        OSSafeReleaseNULL(candidate);
    }
    device->DestroyInterfaceIterator(iterator);
    IOUSBHostFreeDescriptor(winningCfg);

    if (!IVARS->commInterface) {
        return kIOReturnNotFound;
    }

    ret = IVARS->commInterface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        OSSafeReleaseNULL(IVARS->commInterface);
        return ret;
    }

    IVARS->commIfNumber = controlIfNum;
    return kIOReturnSuccess;
}

kern_return_t
TetherKitDriver::openDataInterface()
{
    kern_return_t ret;
    IOUSBHostDevice * device = nullptr;

    ret = IVARS->commInterface->CopyDevice(&device);
    if (ret != kIOReturnSuccess || !device) {
        return kIOReturnError;
    }

    // CopyConfigurationDescriptor() returns a newly-allocated buffer that the
    // caller must release via IOUSBHostFreeDescriptor() (see USBDriverKitDefs.h).
    const IOUSBConfigurationDescriptor * configDesc = IVARS->commInterface->CopyConfigurationDescriptor();
    if (!configDesc) {
        OSSafeReleaseNULL(device);
        return kIOReturnError;
    }

    uint8_t targetIfNum = IVARS->commIfNumber + 1;
    const IOUSBInterfaceDescriptor * intDesc = nullptr;
    while ((intDesc = IOUSBGetNextInterfaceDescriptor(
                configDesc,
                reinterpret_cast<const IOUSBDescriptorHeader *>(intDesc))) != nullptr) {
        if (intDesc->bInterfaceNumber == targetIfNum && intDesc->bInterfaceClass == kUSBInterfaceClassCDCData) {
            break;
        }
    }
    if (!intDesc) {
        IOUSBHostFreeDescriptor(configDesc);
        OSSafeReleaseNULL(device);
        return kIOReturnNotFound;
    }

    uintptr_t iterator = 0;
    ret = device->CreateInterfaceIterator(&iterator);
    if (ret != kIOReturnSuccess) {
        IOUSBHostFreeDescriptor(configDesc);
        OSSafeReleaseNULL(device);
        return ret;
    }
    while (true) {
        IOUSBHostInterface * candidate = nullptr;
        ret = device->CopyInterface(iterator, &candidate);
        if (ret != kIOReturnSuccess || !candidate) {
            break;
        }
        const IOUSBInterfaceDescriptor * d = candidate->GetInterfaceDescriptor(configDesc);
        if (d && d->bInterfaceNumber == targetIfNum) {
            IVARS->dataInterface = candidate;
            break;
        }
        OSSafeReleaseNULL(candidate);
    }
    device->DestroyInterfaceIterator(iterator);
    OSSafeReleaseNULL(device);

    if (!IVARS->dataInterface) {
        IOUSBHostFreeDescriptor(configDesc);
        return kIOReturnNotFound;
    }

    ret = IVARS->dataInterface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        IOUSBHostFreeDescriptor(configDesc);
        OSSafeReleaseNULL(IVARS->dataInterface);
        return ret;
    }

    const IOUSBInterfaceDescriptor * dataIfDesc = IVARS->dataInterface->GetInterfaceDescriptor(configDesc);
    if (!dataIfDesc) {
        IOUSBHostFreeDescriptor(configDesc);
        return kIOReturnError;
    }

    // ----- Determine device speed for MPS computation -----
    uint8_t deviceSpeed = 0; // kUSBSpeedFull / kUSBSpeedHigh / kUSBSpeedSuper
    {
        IOUSBHostDevice * speedDev = nullptr;
        kern_return_t srt = IVARS->commInterface->CopyDevice(&speedDev);
        if (srt == kIOReturnSuccess && speedDev) {
            speedDev->GetSpeed(&deviceSpeed);
            OSSafeReleaseNULL(speedDev);
        }
    }

    const IOUSBEndpointDescriptor * epDesc = nullptr;
    while ((epDesc = IOUSBGetNextEndpointDescriptor(
                configDesc,
                dataIfDesc,
                reinterpret_cast<const IOUSBDescriptorHeader *>(epDesc))) != nullptr) {
        const bool isIn = (epDesc->bEndpointAddress & kIOUSBEndpointDescriptorDirection) != 0;
        const bool isBulk =
            (epDesc->bmAttributes & kIOUSBEndpointDescriptorTransferType) ==
            kIOUSBEndpointDescriptorTransferTypeBulk;
        if (!isBulk) {
            continue;
        }
        if (isIn && !IVARS->inPipe) {
            ret = IVARS->dataInterface->CopyPipe(epDesc->bEndpointAddress, &IVARS->inPipe);
            if (ret != kIOReturnSuccess) {
                IOUSBHostFreeDescriptor(configDesc);
                return ret;
            }
        } else if (!isIn && !IVARS->outPipe) {
            ret = IVARS->dataInterface->CopyPipe(epDesc->bEndpointAddress, &IVARS->outPipe);
            if (ret != kIOReturnSuccess) {
                IOUSBHostFreeDescriptor(configDesc);
                return ret;
            }
            // Cache wMaxPacketSize so we can append a trailing zero byte
            // (the DriverKit equivalent of URB_ZERO_PACKET) whenever a TX
            // transfer length lands on an exact MPS multiple.
            IVARS->outPipeMaxPacketSize =
                IOUSBGetEndpointMaxPacketSize(deviceSpeed, epDesc);
            if (IVARS->outPipeMaxPacketSize == 0) {
                LOG_ERROR("openDataInterface: outPipeMaxPacketSize=0; defaulting to 512");
                IVARS->outPipeMaxPacketSize = 512;
            }
        }
    }

    IOUSBHostFreeDescriptor(configDesc);
    return (IVARS->inPipe && IVARS->outPipe) ? kIOReturnSuccess : kIOReturnNotFound;
}

kern_return_t
TetherKitDriver::openInterruptPipe()
{
    if (!IVARS->commInterface) {
        return kIOReturnNotReady;
    }

    const IOUSBConfigurationDescriptor * cfgDesc =
        IVARS->commInterface->CopyConfigurationDescriptor();
    if (!cfgDesc) {
        return kIOReturnError;
    }
    const IOUSBInterfaceDescriptor * commIfDesc =
        IVARS->commInterface->GetInterfaceDescriptor(cfgDesc);
    if (!commIfDesc) {
        IOUSBHostFreeDescriptor(cfgDesc);
        return kIOReturnError;
    }

    uint8_t intEpAddr = 0;
    uint16_t intEpMps = 0;
    {
        uint8_t deviceSpeed = 0;
        IOUSBHostDevice * speedDev = nullptr;
        if (IVARS->commInterface->CopyDevice(&speedDev) == kIOReturnSuccess && speedDev) {
            speedDev->GetSpeed(&deviceSpeed);
            OSSafeReleaseNULL(speedDev);
        }

        const IOUSBEndpointDescriptor * ep = nullptr;
        while ((ep = IOUSBGetNextEndpointDescriptor(
                    cfgDesc,
                    commIfDesc,
                    reinterpret_cast<const IOUSBDescriptorHeader *>(ep))) != nullptr) {
            const bool isIn = (ep->bEndpointAddress & kIOUSBEndpointDescriptorDirection) != 0;
            const bool isInterrupt =
                (ep->bmAttributes & kIOUSBEndpointDescriptorTransferType) ==
                kIOUSBEndpointDescriptorTransferTypeInterrupt;
            if (isIn && isInterrupt) {
                intEpAddr = ep->bEndpointAddress;
                intEpMps  = IOUSBGetEndpointMaxPacketSize(deviceSpeed, ep);
                break;
            }
        }
    }
    IOUSBHostFreeDescriptor(cfgDesc);

    if (intEpAddr == 0) {
        // No interrupt-IN endpoint. Some Linux gadget RNDIS profiles omit it.
        // Fall back to legacy poll-only rndisCommand path.
        LOG_INFO("openInterruptPipe: no interrupt-IN endpoint; will poll EP0");
        return kIOReturnSuccess;
    }
    if (intEpMps == 0) {
        intEpMps = 16;
    }

    kern_return_t ret = IVARS->commInterface->CopyPipe(intEpAddr, &IVARS->intPipe);
    if (ret != kIOReturnSuccess || !IVARS->intPipe) {
        return ret != kIOReturnSuccess ? ret : kIOReturnError;
    }

    IVARS->intBufLen = intEpMps;
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn,
                                           IVARS->intBufLen, 0, &IVARS->intBuf);
    if (ret != kIOReturnSuccess || !IVARS->intBuf) {
        OSSafeReleaseNULL(IVARS->intPipe);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    uint64_t mappedAddress = 0;
    uint64_t mappedLength = 0;
    ret = IVARS->intBuf->Map(0, 0, 0, 0, &mappedAddress, &mappedLength);
    if (ret != kIOReturnSuccess) {
        OSSafeReleaseNULL(IVARS->intBuf);
        OSSafeReleaseNULL(IVARS->intPipe);
        return ret;
    }
    IVARS->intBufAddr = mappedAddress;

    ret = CreateActionInterruptReadComplete(0, &IVARS->intAction);
    if (ret != kIOReturnSuccess) {
        OSSafeReleaseNULL(IVARS->intBuf);
        OSSafeReleaseNULL(IVARS->intPipe);
        return ret;
    }
    return kIOReturnSuccess;
}

kern_return_t
TetherKitDriver::armInterruptRead()
{
    if (IVARS->stopping || IVARS->pipeStopped[2] || !IVARS->intPipe || !IVARS->intBuf || !IVARS->intAction) {
        return kIOReturnSuccess; // No pipe — silently fall back to polling.
    }
    // completionTimeoutMs MUST be 0 for interrupt endpoints.
    kern_return_t ret = kIOReturnError;
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        ret = IVARS->intPipe->AsyncIO(IVARS->intBuf, IVARS->intBufLen, IVARS->intAction, 0);
        if (ret == kIOReturnSuccess) {
            IVARS->intArmed = true;
            return ret;
        }
        if (!recordTransferResult(2, ret)) break;
    }
    return ret;
}

static constexpr uint32_t RNDIS_NOTIF_RESPONSE_AVAILABLE = 0x00000001;

void
TetherKitDriver::handleNotification(const uint8_t * buf, uint32_t len)
{
    if (len < 8 || !buf) {
        return;
    }
    uint32_t code = (uint32_t) buf[0]
                  | ((uint32_t) buf[1] << 8)
                  | ((uint32_t) buf[2] << 16)
                  | ((uint32_t) buf[3] << 24);
    if (code == RNDIS_NOTIF_RESPONSE_AVAILABLE) {
        IVARS->responseAvailable = true;
        LOG_DEBUG("interrupt: RESPONSE_AVAILABLE");
        return;
    }
    if (code == 0x4001000Bu) {
        // A media notification cannot recover a failed USB pipe.
        if (IVARS->stopping || IVARS->pipeStopped[0] ||
            IVARS->pipeStopped[1] || IVARS->pipeStopped[2]) return;
        reportLinkStatus(kIOUserNetworkLinkStatusActive, 0);
    } else if (code == 0x4001000Cu) {
        reportLinkStatus(kIOUserNetworkLinkStatusInactive, 0);
    } else {
        LOG_DEBUG("interrupt: unknown notification code 0x%08x", code);
    }
}

kern_return_t
TetherKitDriver::allocateTransferResources()
{
    for (int i = 0; i < N_IN_BUFS; i++) {
        kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, IN_BUF_SIZE, 0, &IVARS->inBufs[i]);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        uint64_t mappedAddress = 0;
        uint64_t mappedLength = 0;
        ret = IVARS->inBufs[i]->Map(0, 0, 0, 0, &mappedAddress, &mappedLength);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        IVARS->inBufAddrs[i] = mappedAddress;
        ret = CreateActionDataReadComplete(sizeof(uint64_t), &IVARS->inActions[i]);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        *((uint64_t *) IVARS->inActions[i]->GetReference()) = static_cast<uint64_t>(i);
    }

    for (int i = 0; i < N_OUT_BUFS; i++) {
        kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, OUT_BUF_SIZE, 0, &IVARS->outBufs[i]);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        uint64_t mappedAddress = 0;
        uint64_t mappedLength = 0;
        ret = IVARS->outBufs[i]->Map(0, 0, 0, 0, &mappedAddress, &mappedLength);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        IVARS->outBufAddrs[i] = mappedAddress;
        ret = CreateActionDataWriteComplete(sizeof(uint64_t), &IVARS->outActions[i]);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        *((uint64_t *) IVARS->outActions[i]->GetReference()) = static_cast<uint64_t>(i);
    }

    IVARS->numFreeOutBufs = N_OUT_BUFS;
    return kIOReturnSuccess;
}

kern_return_t
TetherKitDriver::setupNetworkInterface()
{
    IOUserNetworkPacketBufferPoolOptions poolOptions = {};
    poolOptions.packetCount = PACKET_POOL_CAPACITY;
    poolOptions.bufferCount = PACKET_POOL_CAPACITY;
    poolOptions.bufferSize = PACKET_BUFFER_SIZE;
    poolOptions.maxBuffersPerPacket = 1;
    poolOptions.poolFlags = PoolFlagMapToDext | PoolFlagIODirectionIn | PoolFlagIODirectionOut;

    kern_return_t ret = IOUserNetworkPacketBufferPool::CreateWithOptions(
        this,
        "Android RNDIS packet pool",
        &poolOptions,
        &IVARS->pktPool);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ret = CopyDispatchQueue("Default", &IVARS->networkDispatch);
    if (ret != kIOReturnSuccess) return ret;
    ret = IOUserNetworkTxSubmissionQueue::Create(IVARS->pktPool, this,
        PACKET_QUEUE_DEPTH, 0, IVARS->networkDispatch, &IVARS->txSubmission);
    if (ret != kIOReturnSuccess) return ret;
    ret = IOUserNetworkTxCompletionQueue::Create(IVARS->pktPool, this,
        PACKET_QUEUE_DEPTH, 0, IVARS->networkDispatch, &IVARS->txCompletion);
    if (ret != kIOReturnSuccess) return ret;
    ret = IOUserNetworkRxSubmissionQueue::Create(IVARS->pktPool, this,
        PACKET_QUEUE_DEPTH, 0, IVARS->networkDispatch, &IVARS->rxSubmission);
    if (ret != kIOReturnSuccess) return ret;
    ret = IOUserNetworkRxCompletionQueue::Create(IVARS->pktPool, this,
        PACKET_QUEUE_DEPTH, 0, IVARS->networkDispatch, &IVARS->rxCompletion);
    if (ret != kIOReturnSuccess) return ret;

    ret = IVARS->txSubmission->CopyDataQueue(&IVARS->txSource);
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionTxPacketAvailable(0, &IVARS->txAction);
    if (ret != kIOReturnSuccess) return ret;
    ret = IVARS->txSource->SetDataAvailableHandler(IVARS->txAction);
    if (ret != kIOReturnSuccess) return ret;

    ether_addr_t mac = {};
    __builtin_memcpy(mac.ether_addr_octet, IVARS->macAddr, sizeof(IVARS->macAddr));
    IOUserNetworkPacketQueue * queues[] = {
        IVARS->txSubmission, IVARS->txCompletion, IVARS->rxSubmission, IVARS->rxCompletion,
    };
    ret = RegisterEthernetInterface(mac, IVARS->pktPool, queues, 4);
    if (ret != kIOReturnSuccess) LOG_ERROR("RegisterEthernetInterface: %08x", ret);
    return ret;
}

void
TetherKitDriver::releaseNetworkQueues()
{
    if (IVARS->txSource) IVARS->txSource->SetDataAvailableHandler(nullptr);
    OSSafeReleaseNULL(IVARS->txSource);
    OSSafeReleaseNULL(IVARS->txAction);
    OSSafeReleaseNULL(IVARS->txSubmission);
    OSSafeReleaseNULL(IVARS->txCompletion);
    OSSafeReleaseNULL(IVARS->rxSubmission);
    OSSafeReleaseNULL(IVARS->rxCompletion);
    OSSafeReleaseNULL(IVARS->pktPool);
    OSSafeReleaseNULL(IVARS->networkDispatch);
}

void
TetherKitDriver::TxPacketAvailable_Impl(OSAction * /* action */)
{
    pumpTxQueue();
}

void
TetherKitDriver::pumpTxQueue()
{
    if (!IVARS->running || IVARS->pipeStopped[1] || !IVARS->txSubmission || !IVARS->txCompletion) return;
    while (IVARS->running && !IVARS->pipeStopped[1] && IVARS->numFreeOutBufs > 0) {
        IOUserNetworkPacket * packet = nullptr;
        if (IVARS->txSubmission->DequeuePackets(&packet, 1) != 1 || !packet) return;

        sendPacket(packet);
    }
}

kern_return_t
TetherKitDriver::scheduleInboundReads()
{
    for (int i = 0; i < N_IN_BUFS; i++) {
        kern_return_t ret = IVARS->inPipe->AsyncIO(IVARS->inBufs[i], IN_BUF_SIZE, IVARS->inActions[i], 0);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
    }
    return kIOReturnSuccess;
}

kern_return_t
TetherKitDriver::SetInterfaceEnable_Impl(bool isEnable)
{
    IOUserNetworkPacketQueue * queues[] = {
        IVARS->txSubmission, IVARS->txCompletion, IVARS->rxSubmission, IVARS->rxCompletion,
    };
    for (auto * queue : queues) {
        if (!queue) return kIOReturnNotReady;
        auto status = queue->setEnable(isEnable);
        if (status != kIOReturnSuccess) return status;
    }
    if (isEnable) pumpTxQueue();
    return kIOReturnSuccess;
}

kern_return_t
TetherKitDriver::SetPromiscuousModeEnable_Impl(bool enable)
{
    if (!IVARS->rndisInitDone) {
        return kIOReturnNotReady;
    }
    const uint32_t filter = enable
        ? RNDIS_DEFAULT_FILTER | RNDIS_PACKET_TYPE_PROMISCUOUS
        : RNDIS_DEFAULT_FILTER;
    return rndisSetPacketFilter(filter);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
kern_return_t
TetherKitDriver::SetMulticastAddresses_Impl(const IOUserNetworkMACAddress * addresses,
                                            uint32_t count)
{
    (void) addresses;
    (void) count;
    return kIOReturnSuccess;
}
#pragma clang diagnostic pop

kern_return_t
TetherKitDriver::SetAllMulticastModeEnable_Impl(bool enable)
{
    (void) enable;
    return kIOReturnSuccess;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
kern_return_t
TetherKitDriver::SelectMediaType_Impl(IOUserNetworkMediaType mediaType)
{
    (void) mediaType;
    return kIOReturnSuccess;
}
#pragma clang diagnostic pop

kern_return_t
TetherKitDriver::SetWakeOnMagicPacketEnable_Impl(bool enable)
{
    (void) enable;
    return kIOReturnSuccess;
}

void
TetherKitDriver::sendPacket(IOUserNetworkPacket * pkt)
{
    const uint32_t ethLen = pkt->getDataLength();
    const uint64_t dataAddress = pkt->getDataVirtualAddress();
    if (!dataAddress || ethLen < kEthernetHeaderLength || ethLen > kEthernetFrameLength ||
        pkt->getDataOffset() > PACKET_BUFFER_SIZE - ethLen) {
        if (IVARS->txCompletion->EnqueuePacket(pkt) != kIOReturnSuccess)
            IVARS->pktPool->DeallocatePacket(pkt);
        return;
    }
    const uint32_t totalLen = ethLen + sizeof(rndis_data_hdr);
    if (totalLen > IVARS->maxOutTransferSize) {
        if (IVARS->txCompletion->EnqueuePacket(pkt) != kIOReturnSuccess)
            IVARS->pktPool->DeallocatePacket(pkt);
        return;
    }

    const int bufferIndex = IVARS->outBufStack[IVARS->numFreeOutBufs - 1];
    if (bufferIndex < 0 || bufferIndex >= N_OUT_BUFS) {
        LOG_ERROR("sendPacket: corrupt outBufStack (idx=%d numFree=%d)",
                  bufferIndex, IVARS->numFreeOutBufs);
        if (IVARS->txCompletion->EnqueuePacket(pkt) != kIOReturnSuccess)
            IVARS->pktPool->DeallocatePacket(pkt);
        return;
    }
    uint8_t * bufPtr = reinterpret_cast<uint8_t *>(IVARS->outBufAddrs[bufferIndex]);
    rndis_data_hdr * hdr = reinterpret_cast<rndis_data_hdr *>(bufPtr);
    __builtin_memset(hdr, 0, sizeof(*hdr));
    hdr->msg_type = RNDIS_MSG_PACKET;
    hdr->msg_len = cpu_to_le32(totalLen);
    hdr->data_offset = RNDIS_DATA_HDR_DATA_OFFSET;
    hdr->data_len = cpu_to_le32(ethLen);

    const uint64_t packetDataAddress = dataAddress + pkt->getDataOffset();
    __builtin_memcpy(bufPtr + sizeof(rndis_data_hdr),
                     reinterpret_cast<const void *>(packetDataAddress),
                     ethLen);

    // ZLP-substitute: if totalLen is an exact multiple of wMaxPacketSize,
    // append a single zero byte so the host controller terminates the
    // transfer with a short packet. The device parses by hdr->msg_len, so
    // the trailing byte is ignored. See URB_ZERO_PACKET in linux rndis_host.
    uint32_t submitLen = totalLen;
    if (IVARS->outPipeMaxPacketSize > 0 &&
        (totalLen % IVARS->outPipeMaxPacketSize) == 0) {
        bufPtr[totalLen] = 0;
        submitLen = totalLen + 1;
    }

    IVARS->pendingTx[bufferIndex] = pkt;
    IVARS->numFreeOutBufs--;
    IVARS->submittedLengths[bufferIndex] = submitLen;
    kern_return_t ret = IVARS->outBufs[bufferIndex]->SetLength(submitLen);
    if (ret == kIOReturnSuccess) ret = IVARS->outPipe->AsyncIO(IVARS->outBufs[bufferIndex],
                                                submitLen,
                                                IVARS->outActions[bufferIndex],
                                                0);
    if (ret != kIOReturnSuccess) {
        IVARS->pendingTx[bufferIndex] = nullptr;
        if (IVARS->txCompletion->EnqueuePacket(pkt) != kIOReturnSuccess)
            IVARS->pktPool->DeallocatePacket(pkt);
        IVARS->outBufStack[IVARS->numFreeOutBufs] = static_cast<uint8_t>(bufferIndex);
        IVARS->numFreeOutBufs++;
        recordTransferResult(1, ret);
        return;
    }

}

void
TetherKitDriver::DataReadComplete_Impl(OSAction * action,
                                       IOReturn status,
                                       uint32_t actualByteCount,
                                       uint64_t /* completionTimestamp */)
{
    if (!IVARS->running || IVARS->pipeStopped[0]) return;
    if (actualByteCount > IN_BUF_SIZE) status = kIOReturnOverrun;

    int bufferIndex = static_cast<int>(*((uint64_t *) action->GetReference()));
    if (bufferIndex < 0 || bufferIndex >= N_IN_BUFS) {
        LOG_ERROR("DataReadComplete: bogus bufferIndex %d", bufferIndex);
        return;
    }
    if (status == kIOReturnSuccess && actualByteCount > 0 && actualByteCount <= IN_BUF_SIZE) {
        status = processInboundBuffer(reinterpret_cast<const uint8_t *>(IVARS->inBufAddrs[bufferIndex]),
                                      actualByteCount);
    }
    // Count a malformed RNDIS transfer as one failure; USB success alone must
    // not clear the streak before parsing, or malformed traffic can spin forever.
    if (!recordTransferResult(0, status)) return;

    for (uint32_t attempt = 0; attempt < 3 && !IVARS->pipeStopped[0]; ++attempt) {
        kern_return_t ret = IVARS->inPipe->AsyncIO(IVARS->inBufs[bufferIndex], IN_BUF_SIZE, action, 0);
        if (ret == kIOReturnSuccess) break;
        if (!recordTransferResult(0, ret)) break;
    }
}

kern_return_t
TetherKitDriver::processInboundBuffer(const uint8_t * buf, uint32_t size)
{
    struct Batch {
        TetherKitDriver_IVars * state;
        IOUserNetworkPacket * packets[PACKET_QUEUE_DEPTH] = {};
        uint32_t count = 0;

        void flush() {
            if (!count) return;
            const uint32_t accepted = state->rxCompletion->EnqueuePackets(packets, count);
            for (uint32_t i = accepted; i < count; ++i)
                state->pktPool->DeallocatePacket(packets[i]);
            count = 0;
        }
    } batch { IVARS };

    auto enqueue = +[](void * context, const uint8_t * eth, uint32_t len) {
        auto * batch = static_cast<Batch *>(context);
        auto * state = batch->state;
        if (len < kEthernetHeaderLength || len > kEthernetFrameLength) return;
        IOUserNetworkPacket * packet = nullptr;
        if (state->rxSubmission->DequeuePackets(&packet, 1) != 1 || !packet) return;
        const uint64_t address = packet->getDataVirtualAddress();
        const uint16_t offset = packet->getDataOffset();
        if (address && offset <= PACKET_BUFFER_SIZE - len &&
            packet->setDataLength(len) == kIOReturnSuccess) {
            __builtin_memcpy(reinterpret_cast<void *>(address + offset), eth, len);
            batch->packets[batch->count++] = packet;
            if (batch->count == PACKET_QUEUE_DEPTH) batch->flush();
        } else {
            state->pktPool->DeallocatePacket(packet);
        }
    };

    rndis::ParseResult result = rndis::parseInboundBuffer(buf, size, enqueue, &batch);
    batch.flush();
    return result == rndis::ParseResult::OK ? kIOReturnSuccess : kIOReturnBadMessageID;
}

void
TetherKitDriver::DataWriteComplete_Impl(OSAction * action,
                                        IOReturn status,
                                        uint32_t actualByteCount,
                                        uint64_t /* completionTimestamp */)
{
    if (!IVARS->running) {
        return;
    }
    int bufferIndex = static_cast<int>(*((uint64_t *) action->GetReference()));
    if (bufferIndex < 0 || bufferIndex >= N_OUT_BUFS) {
        LOG_ERROR("DataWriteComplete: bogus bufferIndex %d", bufferIndex);
        return;
    }
    if (!IVARS->pendingTx[bufferIndex]) return;
    if (status == kIOReturnSuccess && actualByteCount != IVARS->submittedLengths[bufferIndex])
        status = kIOReturnUnderrun;
    recordTransferResult(1, status);
    IOUserNetworkPacket * packet = IVARS->pendingTx[bufferIndex];
    IVARS->pendingTx[bufferIndex] = nullptr;
    if (packet && IVARS->txCompletion->EnqueuePacket(packet) != kIOReturnSuccess)
        IVARS->pktPool->DeallocatePacket(packet);
    if (IVARS->numFreeOutBufs < N_OUT_BUFS) {
        IVARS->outBufStack[IVARS->numFreeOutBufs] = static_cast<uint8_t>(bufferIndex);
        IVARS->numFreeOutBufs++;
    }
    pumpTxQueue();
}

void
TetherKitDriver::InterruptReadComplete_Impl(OSAction * /* action */,
                                            IOReturn status,
                                            uint32_t actualByteCount,
                                            uint64_t /* completionTimestamp */)
{
    IVARS->intArmed = false;

    if (!IVARS->running || IVARS->pipeStopped[2] ||
        !recordTransferResult(2, actualByteCount > IVARS->intBufLen ? kIOReturnOverrun : status)) {
        return;
    }

    if (status == kIOReturnSuccess && actualByteCount > 0) {
        handleNotification(reinterpret_cast<const uint8_t *>(IVARS->intBufAddr),
                           actualByteCount);
    }

    armInterruptRead();
}

bool
TetherKitDriver::recordTransferResult(uint32_t pipe, IOReturn status)
{
    if (IVARS->pipeStopped[pipe]) return false;
    if (status == kIOReturnSuccess) {
        IVARS->consecutiveErrors[pipe] = 0;
        return true;
    }
    ++IVARS->totalErrors[pipe];
    ++IVARS->consecutiveErrors[pipe];
    if (IVARS->totalErrors[pipe] == 1)
        LOG_ERROR("USB pipe %u first error: %08x", pipe, status);
    if (isUsbGoneStatus(status) || IVARS->consecutiveErrors[pipe] >= 3) {
        IVARS->pipeStopped[pipe] = true;
        reportLinkStatus(kIOUserNetworkLinkStatusInvalid, 0);
        LOG_ERROR("USB pipe %u stopped; errors=%llu; reconnect required",
                  pipe, IVARS->totalErrors[pipe]);
        return false;
    }
    return true;
}

kern_return_t
TetherKitDriver::rndisCommand(uint32_t msgLen)
{
    if (!IVARS->commInterface || !IVARS->cmdBuf) {
        return kIOReturnError;
    }

    uint64_t bufAddr = 0;
    uint64_t mappedLen = 0;
    kern_return_t ret = IVARS->cmdBuf->Map(0, 0, 0, 0, &bufAddr, &mappedLen);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    rndis_msg_hdr * hdr = reinterpret_cast<rndis_msg_hdr *>(bufAddr);
    if (le32_to_cpu(hdr->msg_len) != msgLen) {
        LOG_ERROR("rndisCommand: header msgLen %u != expected %u", le32_to_cpu(hdr->msg_len), msgLen);
        return kIOReturnBadArgument;
    }
    if (hdr->msg_type != RNDIS_MSG_HALT && hdr->msg_type != RNDIS_MSG_RESET) {
        uint32_t xid = IVARS->rndisXid++;
        if (xid == 0) {
            xid = IVARS->rndisXid++;
        }
        hdr->request_id = cpu_to_le32(xid);
    }

    const uint32_t requestType = hdr->msg_type;
    const uint32_t requestId = hdr->request_id;
    const uint32_t requestLen = le32_to_cpu(hdr->msg_len);

    // Keep interrupt-IN submitted for devices that require a status-channel
    // read before returning control responses. Do not wait on this serial
    // queue for a completion callback scheduled on the same queue.
    if (IVARS->intPipe && !IVARS->intArmed) {
        armInterruptRead();
    }

    uint16_t bytesSent = 0;
    ret = IVARS->commInterface->DeviceRequest(
        kIOUSBDeviceRequestDirectionOut | kIOUSBDeviceRequestTypeClass | kIOUSBDeviceRequestRecipientInterface,
        USB_CDC_SEND_ENCAPSULATED_COMMAND,
        0,
        static_cast<uint16_t>(IVARS->commIfNumber),
        static_cast<uint16_t>(requestLen),
        IVARS->cmdBuf,
        &bytesSent,
        kTimeoutMs);
    if (ret != kIOReturnSuccess || static_cast<uint32_t>(bytesSent) != requestLen) {
        return ret != kIOReturnSuccess ? ret : kIOReturnUnderrun;
    }

    // Poll the control response directly, as the Linux RNDIS host does.
    // Empty or unrelated responses retain the bounded retry delay.
    for (uint32_t attempt = 0; attempt < static_cast<uint32_t>(kMaxAttempts); attempt++) {
        uint16_t bytesReceived = 0;
        ret = IVARS->commInterface->DeviceRequest(
            kIOUSBDeviceRequestDirectionIn | kIOUSBDeviceRequestTypeClass | kIOUSBDeviceRequestRecipientInterface,
            USB_CDC_GET_ENCAPSULATED_RESPONSE,
            0,
            static_cast<uint16_t>(IVARS->commIfNumber),
            static_cast<uint16_t>(RNDIS_CMD_BUF_SZ),
            IVARS->cmdBuf,
            &bytesReceived,
            kTimeoutMs);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        if (static_cast<uint32_t>(bytesReceived) > RNDIS_CMD_BUF_SZ) {
            LOG_ERROR("rndisCommand: response %u exceeds buffer %u", bytesReceived, RNDIS_CMD_BUF_SZ);
            return kIOReturnIOError;
        }
        if (static_cast<uint32_t>(bytesReceived) < kMinResponseBytes) {
            IOSleep(kRetryDelayMs);
            continue;
        }

        const uint32_t expectedResponseType = requestType | RNDIS_MSG_COMPLETION;
        if (hdr->msg_type != expectedResponseType) {
            IOSleep(kRetryDelayMs);
            continue;
        }
        if (hdr->request_id != requestId) {
            IOSleep(kRetryDelayMs);
            continue;
        }
        if (hdr->status != RNDIS_STATUS_SUCCESS) {
            return kIOReturnIOError;
        }
        if (le32_to_cpu(hdr->msg_len) != static_cast<uint32_t>(bytesReceived)) {
            return kIOReturnIOError;
        }
        return kIOReturnSuccess;
    }

    return kIOReturnTimeout;
}

kern_return_t
TetherKitDriver::rndisInit()
{
    uint64_t bufAddr = 0;
    uint64_t bufLen = 0;
    kern_return_t ret = IVARS->cmdBuf->Map(0, 0, 0, 0, &bufAddr, &bufLen);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    rndis_init * initMsg = reinterpret_cast<rndis_init *>(bufAddr);
    __builtin_memset(initMsg, 0, sizeof(*initMsg));
    initMsg->msg_type = RNDIS_MSG_INIT;
    initMsg->msg_len = cpu_to_le32(static_cast<uint32_t>(sizeof(rndis_init)));
    initMsg->major_version = cpu_to_le32(1);
    initMsg->minor_version = cpu_to_le32(0);
    initMsg->max_transfer_size = cpu_to_le32(IN_BUF_SIZE);

    ret = rndisCommand(static_cast<uint32_t>(sizeof(rndis_init)));
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    const rndis_init_c * initC = reinterpret_cast<const rndis_init_c *>(bufAddr);
    if (le32_to_cpu(initC->msg_len) < sizeof(*initC)) {
        return kIOReturnIOError;
    }
    uint32_t devMax = le32_to_cpu(initC->max_transfer_size);
    uint32_t clamped = rndis::clampMaxTransferSize(devMax, OUT_BUF_PAYLOAD_MAX);
    if (clamped != devMax) {
        LOG_INFO("rndisInit: device reported max_transfer_size=%u, clamped to %u",
                  devMax, clamped);
    }
    IVARS->maxOutTransferSize = clamped;

    IVARS->rndisInitDone = true;
    return kIOReturnSuccess;
}

kern_return_t
TetherKitDriver::rndisQuery(uint32_t oid,
                            uint32_t inLen,
                            const uint8_t ** outData,
                            uint32_t * outLen)
{
    uint64_t bufAddr = 0;
    uint64_t bufLen = 0;
    kern_return_t ret = IVARS->cmdBuf->Map(0, 0, 0, 0, &bufAddr, &bufLen);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    rndis_query * queryMsg = reinterpret_cast<rndis_query *>(bufAddr);
    __builtin_memset(queryMsg, 0, sizeof(*queryMsg) + inLen);
    queryMsg->msg_type = RNDIS_MSG_QUERY;
    queryMsg->msg_len = cpu_to_le32(static_cast<uint32_t>(sizeof(*queryMsg) + inLen));
    queryMsg->oid = oid;
    queryMsg->len = cpu_to_le32(inLen);
    queryMsg->offset = cpu_to_le32(20);

    ret = rndisCommand(static_cast<uint32_t>(sizeof(rndis_query) + inLen));
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    const rndis_query_c * queryResponse = reinterpret_cast<const rndis_query_c *>(bufAddr);
    const uint32_t responseOffset = le32_to_cpu(queryResponse->offset);
    const uint32_t responseLen = le32_to_cpu(queryResponse->len);
    const uint32_t responseMsgLen = le32_to_cpu(queryResponse->msg_len);
    const uint64_t end = 8ull + responseOffset + responseLen;
    if (responseMsgLen < sizeof(*queryResponse) ||
        responseOffset < sizeof(*queryResponse) - 8 ||
        end > responseMsgLen || end > RNDIS_CMD_BUF_SZ) {
        return kIOReturnIOError;
    }

    const uint8_t * responseBase = reinterpret_cast<const uint8_t *>(&queryResponse->request_id);
    *outData = responseBase + responseOffset;
    *outLen = responseLen;
    return kIOReturnSuccess;
}

kern_return_t
TetherKitDriver::rndisSetPacketFilter(uint32_t filter)
{
    if (!IVARS->cmdBuf) {
        return kIOReturnNotReady;
    }

    uint64_t bufAddr = 0;
    uint64_t bufLen = 0;
    kern_return_t ret = IVARS->cmdBuf->Map(0, 0, 0, 0, &bufAddr, &bufLen);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    rndis_set * setMsg = reinterpret_cast<rndis_set *>(bufAddr);
    __builtin_memset(setMsg, 0, sizeof(*setMsg) + sizeof(uint32_t));
    setMsg->msg_type = RNDIS_MSG_SET;
    setMsg->msg_len = cpu_to_le32(static_cast<uint32_t>(sizeof(*setMsg) + 4));
    setMsg->oid = OID_GEN_CURRENT_PACKET_FILTER;
    setMsg->len = cpu_to_le32(4);
    setMsg->offset = cpu_to_le32(static_cast<uint32_t>(sizeof(*setMsg) - 8));
    *reinterpret_cast<uint32_t *>(setMsg + 1) = filter;

    return rndisCommand(static_cast<uint32_t>(sizeof(rndis_set) + 4));
}

kern_return_t
TetherKitDriver::queryMacAddress()
{
    const uint8_t * data = nullptr;
    uint32_t outLen = 0;

    kern_return_t ret = rndisQuery(OID_802_3_CURRENT_ADDRESS, 0, &data, &outLen);
    if (ret != kIOReturnSuccess || outLen < 6 || !data) {
        ret = rndisQuery(OID_802_3_PERMANENT_ADDRESS, 0, &data, &outLen);
        if (ret != kIOReturnSuccess || outLen < 6 || !data) {
            return kIOReturnIOError;
        }
    }

    __builtin_memcpy(IVARS->macAddr, data, 6);

    if (rndis::fixupMacMulticastBit(IVARS->macAddr)) {
        LOG_ERROR("queryMacAddress: device returned multicast MAC; substituted locally-administered unicast (%02x:%02x:%02x:%02x:%02x:%02x)",
                  IVARS->macAddr[0], IVARS->macAddr[1], IVARS->macAddr[2],
                  IVARS->macAddr[3], IVARS->macAddr[4], IVARS->macAddr[5]);
    }

    return kIOReturnSuccess;
}
