#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Templates/SubclassOf.h"
#include "UObject/WeakObjectPtr.h"

class AFGBuildable;
class UFGRecipe;
class UWorld;


/// Appends to files off the game thread, in the order the lines were handed over
class FBridgeFileWriter
{
public:
	void Append(const FString& Path, FString&& Text);
	void Overwrite(const FString& Path, FString&& Text);
	/// For text that takes a while to put together, that's done off the game thread as well
	void Overwrite(const FString& Path, TUniqueFunction<FString()>&& MakeText);
	/// @return Whether everything that was handed over is in the files now
	bool Flush();

private:
	struct FPendingWrite
	{
		FString Path;
		FString Text;
		bool bAppend;
	};

	/// The tasks that write hold on to it, they can still be around when the writer isn't anymore
	struct FState
	{
		FCriticalSection PendingMutex;
		TArray<FPendingWrite> Pending;

		FCriticalSection WriteMutex;
	};

	static bool WritePending(FState& State);
	static bool WriteOnce(const FPendingWrite& Write);

	TSharedRef<FState, ESPMode::ThreadSafe> State = MakeShared<FState, ESPMode::ThreadSafe>();
};


struct FBridgeSampleRow
{
	uint64 Frame;
	double Time;
	double DeltaMs;
	double GameThreadMs;
	double RenderThreadMs;

	// What Cartograph was doing that frame
	bool bHasCartographState;
	bool bIsInitializing;
	bool bIsRedrawActive;
	bool bIsRedrawingEntirely;
	bool bIsPendingRedraw;
	bool bIsPendingRedrawEntire;
	int32 PendingAddCount;
	int32 PendingRemoveCount;
};

struct FBridgeSampler
{
	double StartTime = 0;
	uint64 StartFrame = 0;
	TArray<FBridgeSampleRow> Rows;
};


/// A buildable the bridge has constructed. Becomes a lightweight one when the actor has migrated.
struct FBridgeBuildable
{
	TSubclassOf<AFGBuildable> BuildableClass;
	FTransform Transform;
	TWeakObjectPtr<AFGBuildable> Actor;
};


struct FBridgeCommand
{
	FString Id;
	FString Name;
	TSharedPtr<FJsonObject> Args;

	uint64 FrameStart = 0;
	double TimeStart = 0;
	bool bStarted = false;

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

	// Progress of the ones that take more than a frame
	double Deadline = 0;
	int32 Done = 0;
	int32 Total = 0;
	int32 SettledFrames = 0;
	bool bWaitingForCallback = false;

	// Where a build goes
	bool bHasPlacement = false;
	FVector Origin = FVector::ZeroVector;
	FQuat Orientation = FQuat::Identity;
	int32 SettledFramesMissedGround = 0;
	TArray<int32> LightweightIndices;
};


class FCartographBridge
{
public:
	explicit FCartographBridge(const FString& InDirectory);
	~FCartographBridge();

private:
	enum class ECommandStatus : uint8
	{
		Running,
		Succeeded,
		Failed
	};

	bool Tick(float DeltaTime);

	void ReadCompletedIds();
	void FailInterruptedCommands();
	void PollCommands();
	void Finish(FBridgeCommand& Command, bool bSucceeded, const FString& Error);

	ECommandStatus Start(FBridgeCommand& Command, FString& OutError);
	ECommandStatus Continue(FBridgeCommand& Command, FString& OutError);

	ECommandStatus ContinueBuild(FBridgeCommand& Command, FString& OutError);
	ECommandStatus ContinueDismantle(FBridgeCommand& Command, FString& OutError);

	void FillState(FJsonObject& Data) const;
	void FillMemoryStats(FJsonObject& Data) const;
	bool HashRenderTarget(const FJsonObject& Args, FJsonObject& Data, FString& OutError) const;
	bool ProbeCanvas(UWorld& World, FJsonObject& Data, FString& OutError);
	bool SetUpMachines(UWorld& World, const FJsonObject& Args, FJsonObject& Data, FString& OutError);
	bool ListMachines(UWorld& World, const FJsonObject& Args, FJsonObject& Data, FString& OutError);
	bool AdoptBuildables(UWorld& World, const FJsonObject& Args, FJsonObject& Data, FString& OutError);
	void StopSampler(const FString& Name, FJsonObject& Data);

	void AddEvent(const TCHAR* Event, const TSharedPtr<FJsonObject>& Detail = nullptr);

	void OnWorldBeginPlay(UWorld* World);
	void OnWorldTearDown(UWorld* World);

	UWorld* GetGameWorld() const;
	double Now() const;

	static FString ToLine(const TSharedRef<FJsonObject>& Object);

	FString Directory;
	FString CommandsPath;
	FString ResultsPath;
	FString EventsPath;
	FString StatusPath;
	FString StartedPath;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle WorldBeginPlayHandle;
	FDelegateHandle WorldTearDownHandle;

	FBridgeFileWriter Writer;

	double StartTime = 0;
	double LastTickTime = 0;
	double NextPollTime = 0;
	double NextStatusTime = 0;

	int64 CommandsOffset = 0;
	TSet<FString> CompletedIds;
	TArray<FBridgeCommand> Queue;

	TMap<FString, FBridgeSampler> Samplers;
	TMap<FString, TArray<FBridgeBuildable>> Groups;

	uint64 WorldBeginPlayCount = 0;

	/// A save that has been asked to be loaded, and how many worlds had begun play by then.
	/// The one to wait for is one that has begun after.
	FString PendingLoadSave;
	TOptional<uint64> PendingLoadGeneration;

	/// What the game has called back with for the command that's waiting for it
	TOptional<TPair<bool, FString>> CallbackResult;
};
