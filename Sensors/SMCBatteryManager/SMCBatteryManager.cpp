//
//  SMCBatteryManager.cpp
//  SMCBatteryManager
//
//  Copyright © 2018 usrsse2. All rights reserved.
//

#include <VirtualSMCSDK/kern_vsmcapi.hpp>
#include <stdatomic.h>

#include "SMCBatteryManager.hpp"
#include "KeyImplementations.hpp"
#include <IOKit/battery/AppleSmartBatteryCommands.h>
#include <Headers/kern_version.hpp>

OSDefineMetaClassAndStructors(SMCBatteryManager, IOService)

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;
_Atomic(bool) smc_battery_manager_started = false;

IOService *SMCBatteryManager::probe(IOService *provider, SInt32 *score) {
	auto ptr = IOService::probe(provider, score);
	if (!ptr) {
		SYSLOG("sbat", "failed to probe the parent");
		return nullptr;
	}

	if (!BatteryManager::getShared()->probe())
		return nullptr;

	//TODO: implement the keys below as well
	// IB0R: sp4s or sp5s
	// IBAC: sp7s
	// PB0R = IB0R * VP0R

	return this;
}

bool SMCBatteryManager::start(IOService *provider) {
	if (!IOService::start(provider)) {
		SYSLOG("sbat", "failed to start the parent");
		return false;
	}

	setProperty("VersionInfo", kextVersion);

	// AppleSMC presence is a requirement, wait for it.
	auto dict = IOService::nameMatching("AppleSMC");
	if (!dict) {
		SYSLOG("bmgr", "failed to create applesmc matching dictionary");
		return false;
	}
	
#if __MAC_OS_X_VERSION_MIN_REQUIRED < __MAC_10_6
	mach_timespec_t smcTimeout;
	smcTimeout.tv_sec = 0;
	smcTimeout.tv_nsec = 100000000;
	
	auto applesmc = IOService::waitForService(dict, &smcTimeout);
	if (applesmc)
		applesmc->retain();
#else
	auto applesmc = IOService::waitForMatchingService(dict, 100000000);
	dict->release();
#endif

	if (!applesmc) {
		DBGLOG("smcbus", "Timeout in waiting for AppleSMC, will try during next start attempt");
		return false;
	}

	applesmc->release();
	
	DBGLOG("bmgr", "AppleSMC is available now");

	BatteryManager::getShared()->start();

	//WARNING: watch out, key addition is sorted here!

	//FIXME: This needs to be implemented along with AC-W!
	// VirtualSMCAPI::addKey(KeyAC_N, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(0, new AC_N(batteryManager)));

	const auto adaptCount = BatteryManager::getShared()->adapterCount;
	if (adaptCount > 0) {
		VirtualSMCAPI::addKey(KeyACEN, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(0, new ACIN));
		VirtualSMCAPI::addKey(KeyACFP, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(false, new ACIN));
		VirtualSMCAPI::addKey(KeyACID, vsmcPlugin.data, VirtualSMCAPI::valueWithData(nullptr, 8, SmcKeyTypeCh8s, new ACID));
		VirtualSMCAPI::addKey(KeyACIN, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(false, new ACIN));
	}

	const auto batCount = min(BatteryManager::getShared()->batteriesCount, MaxIndexCount);
	for (size_t i = 0; i < batCount; i++) {
		VirtualSMCAPI::addKey(KeyB0AC(i), vsmcPlugin.data, VirtualSMCAPI::valueWithSint16(400, new B0AC(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0AV(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(13000, new B0AV(i)));
		VirtualSMCAPI::addKey(KeyB0BI(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(1, new B0BI(i)));
		VirtualSMCAPI::addKey(KeyB0CT(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(1, new B0CT(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0FC(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(4000, new B0FC(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0PS(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(nullptr, 2, SmcKeyTypeHex, new B0PS(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0RM(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(2000, new B0RM(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0St(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(nullptr, 2, SmcKeyTypeHex, new B0St(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0TF(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0, new B0TF(i)));
		if (BatteryManager::getShared()->state.btInfo[i].state.publishTemperatureKey) {
			VirtualSMCAPI::addKey(KeyTB0T(i+1), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TB0T(i)));
			if (i == 0)
				VirtualSMCAPI::addKey(KeyTB0T(0), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TB0T(0)));
		}
	}

	VirtualSMCAPI::addKey(KeyBATP, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(true, new BATP));
	VirtualSMCAPI::addKey(KeyBBAD, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(false, new BBAD));
	VirtualSMCAPI::addKey(KeyBBIN, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(true, new BBIN));
	VirtualSMCAPI::addKey(KeyBFCL, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(100, new BFCL, SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE));
	VirtualSMCAPI::addKey(KeyBNum, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(1, new BNum));
	VirtualSMCAPI::addKey(KeyBRSC, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(40, new BRSC, SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_PRIVATE_WRITE));
	VirtualSMCAPI::addKey(KeyBSIn, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(0, new BSIn));

	VirtualSMCAPI::addKey(KeyCHBI, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0, new CHBI));
	VirtualSMCAPI::addKey(KeyCHBV, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(8000, new CHBV));
	VirtualSMCAPI::addKey(KeyCHLC, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(1, new CHLC));

	if (getKernelVersion() >= KernelVersion::BigSur) {
		for (size_t i = 0; i < batCount; i++) {
			VirtualSMCAPI::addKey(KeyBC1V(i+1), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(defaultBatteryCellVoltage));
#if 0
			VirtualSMCAPI::addKey(KeyD0IR(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(5000));
			VirtualSMCAPI::addKey(KeyD0VM(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(5000));
			VirtualSMCAPI::addKey(KeyD0VR(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(5000));
			VirtualSMCAPI::addKey(KeyD0VX(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(5000));
#endif
		}

#if 0
		for (size_t i = 0; i < adaptCount; i++) {
			VirtualSMCAPI::addKey(KeyD0PT(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(i));
			VirtualSMCAPI::addKey(KeyD0BD(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint32(static_cast<uint32_t>(i+1)));
			VirtualSMCAPI::addKey(KeyD0ER(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0));
			VirtualSMCAPI::addKey(KeyD0DE(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA*>("trop"), 4, SmcKeyTypeCh8s));
			VirtualSMCAPI::addKey(KeyD0is(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA*>("43210"), 5, SmcKeyTypeCh8s));
			VirtualSMCAPI::addKey(KeyD0if(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA*>("0.1"), 3, SmcKeyTypeCh8s));
			VirtualSMCAPI::addKey(KeyD0ih(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA*>("0.1"), 3, SmcKeyTypeCh8s));
			VirtualSMCAPI::addKey(KeyD0ii(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA*>("0.0.1"), 5, SmcKeyTypeCh8s));
			VirtualSMCAPI::addKey(KeyD0im(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA*>("resu"), 4, SmcKeyTypeCh8s));
			VirtualSMCAPI::addKey(KeyD0in(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA*>("LPAA"), 4, SmcKeyTypeCh8s));
			VirtualSMCAPI::addKey(KeyD0PI(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(0x87));
			VirtualSMCAPI::addKey(KeyD0FC(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0));
		}

		VirtualSMCAPI::addKey(KeyBNCB, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(batCount));
		VirtualSMCAPI::addKey(KeyAC_N, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(adaptCount-1));
		VirtualSMCAPI::addKey(KeyAC_W, vsmcPlugin.data, VirtualSMCAPI::valueWithSint8(1, nullptr, SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));
		VirtualSMCAPI::addKey(KeyCHII, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0xB58));
#endif
	}

	qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);

	vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);
	smc_battery_manager_started = (vsmcNotifier != nullptr);
	return vsmcNotifier != nullptr;
}

bool SMCBatteryManager::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
	if (sensors && vsmc) {
		DBGLOG("sbat", "got vsmc notification");
		auto &plugin = static_cast<SMCBatteryManager *>(sensors)->vsmcPlugin;
		auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &plugin, nullptr, nullptr);
		if (ret == kIOReturnSuccess) {
			DBGLOG("sbat", "submitted plugin");
			return true;
		} else if (ret != kIOReturnUnsupported) {
			SYSLOG("sbat", "plugin submission failure %X", ret);
		} else {
			DBGLOG("sbat", "plugin submission to non vsmc");
		}
	} else {
		SYSLOG("sbat", "got null vsmc notification");
	}
	return false;
}

void SMCBatteryManager::stop(IOService *provider) {
	PANIC("sbat", "called stop!!!");
}

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
	// Report success but actually do not start and let I/O Kit unload us.
	// This works better and increases boot speed in some cases.
	lilu_get_boot_args("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
	ADDPR(debugEnabled) = checkKernelArgument("-vsmcdbg") || checkKernelArgument("-sbatdbg") || checkKernelArgument("-liludbgall");
	BatteryManager::createShared();
	return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
	// It is not safe to unload VirtualSMC plugins!
	return KERN_FAILURE;
}
