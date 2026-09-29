#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FCartographBridge;

DECLARE_LOG_CATEGORY_EXTERN(LogCartographBridge, Log, All);

class FCartographTestBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TUniquePtr<FCartographBridge> Bridge;
};
