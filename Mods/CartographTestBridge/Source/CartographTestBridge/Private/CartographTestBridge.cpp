#include "CartographTestBridge.h"

#include "CartographBridge.h"

DEFINE_LOG_CATEGORY(LogCartographBridge);


void FCartographTestBridgeModule::StartupModule()
{
	// Does nothing at all in a normal game
	if (!FParse::Param(FCommandLine::Get(), TEXT("CartographBridge")))
	{
		return;
	}

	FString Directory;
	if (!FParse::Value(FCommandLine::Get(), TEXT("CartographBridgeDir="), Directory) || Directory.IsEmpty())
	{
		Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CartographBridge"));
	}

	Bridge = MakeUnique<FCartographBridge>(FPaths::ConvertRelativePathToFull(Directory));
}


void FCartographTestBridgeModule::ShutdownModule()
{
	Bridge.Reset();
}


IMPLEMENT_MODULE(FCartographTestBridgeModule, CartographTestBridge)
