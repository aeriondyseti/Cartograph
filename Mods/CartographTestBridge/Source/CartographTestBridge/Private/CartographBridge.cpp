#include "CartographBridge.h"

#include "Async/Async.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "FGInventoryComponent.h"
#include "CartographGameInstanceModule.h"
#include "CartographTestBridge.h"
#include "Engine/Engine.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/World.h"
#include "FGBlueprintFunctionLibrary.h"
#include "FGBuildableSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGDismantleInterface.h"
#include "FGGameMode.h"
#include "FGGameRulesSubsystem.h"
#include "FGGameState.h"
#include "Resources/FGItemDescriptor.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildableWire.h"
#include "FGPowerConnectionComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGPlayerController.h"
#include "FGRecipe.h"
#include "FGSaveManagerInterface.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Engine/Canvas.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/EngineVersion.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "RenderTimer.h"
#include "Resources/FGBuildingDescriptor.h"
#include "RHI.h"
#include "RHIStats.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextureResource.h"


namespace
{
	constexpr const TCHAR* BridgeVersion = TEXT("0.6.0");
	constexpr const TCHAR* TestSavePrefix = TEXT("Cartograph_Test_");
	constexpr const TCHAR* DefaultMap = TEXT("/Game/FactoryGame/Map/GameLevel01/Persistent_Level");

	constexpr double PollInterval = 0.1;
	constexpr double StatusInterval = 1.0;
	constexpr double DefaultTimeout = 600.0;
	constexpr int32 MaxBuildsPerFrame = 200;
	constexpr int32 MaxCommandsPerFrame = 16;

	TSharedRef<FJsonValue> NullValue()
	{
		return MakeShared<FJsonValueNull>();
	}

	/// -1 is what the RHI reports for a value it doesn't know
	void SetKnownNumber(FJsonObject& Object, const FString& Field, int64 Value)
	{
		if (Value < 0)
		{
			Object.SetField(Field, NullValue());
		}
		else
		{
			Object.SetNumberField(Field, static_cast<double>(Value));
		}
	}

	bool ReadVector(const FJsonObject& Args, const FString& Field, FVector& OutVector)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Args.TryGetArrayField(Field, Values) || Values->Num() != 3)
		{
			return false;
		}
		OutVector = FVector{ (*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber() };
		return true;
	}

	FIntVector Quantize(const FVector& Location)
	{
		return FIntVector{ FMath::RoundToInt32(Location.X), FMath::RoundToInt32(Location.Y), FMath::RoundToInt32(Location.Z) };
	}

	bool ChangesTheWorld(const FString& Command)
	{
		return Command == TEXT("build") || Command == TEXT("dismantle") || Command == TEXT("save")
			|| Command == TEXT("delete_save") || Command == TEXT("game_rules") || Command == TEXT("give_items")
			|| Command == TEXT("power_link") || Command == TEXT("fuel")
			|| Command == TEXT("load_save") || Command == TEXT("exit_to_menu")
			|| Command == TEXT("machine_setup") || Command == TEXT("adopt")
			|| Command == TEXT("set_layer") || Command == TEXT("set_z_filter") || Command == TEXT("set_view") || Command == TEXT("teleport");
	}

	// Only saves with the name the tests save under by convention are deleted. The name doesn't prove
	// who made a save, what's to be deleted is for the one asking to list by name.
	// Nothing but a plain name: it goes to the game as the name of a file.
	bool IsNameOfTestSave(const FString& SaveName)
	{
		if (!SaveName.StartsWith(TestSavePrefix, ESearchCase::CaseSensitive) || SaveName.Len() > 120)
		{
			return false;
		}
		for (const TCHAR Character : SaveName)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	double Percentile(const TArray<double>& Sorted, double Fraction)
	{
		if (Sorted.IsEmpty())
		{
			return 0;
		}
		const int32 Index = FMath::Clamp(FMath::CeilToInt32(Fraction * Sorted.Num()) - 1, 0, Sorted.Num() - 1);
		return Sorted[Index];
	}
}


#pragma region Writer
void FBridgeFileWriter::Append(const FString& Path, FString&& Text)
{
	{
		FScopeLock Lock{ &State->PendingMutex };
		State->Pending.Add({ Path, MoveTemp(Text), true });
	}
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [State = State] { WritePending(*State); });
}


void FBridgeFileWriter::Overwrite(const FString& Path, FString&& Text)
{
	{
		FScopeLock Lock{ &State->PendingMutex };
		State->Pending.Add({ Path, MoveTemp(Text), false });
	}
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [State = State] { WritePending(*State); });
}


void FBridgeFileWriter::Overwrite(const FString& Path, TUniqueFunction<FString()>&& MakeText)
{
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [State = State, Path, MakeText = MoveTemp(MakeText)]() mutable
		{
			FString Text = MakeText();
			{
				FScopeLock Lock{ &State->PendingMutex };
				State->Pending.Add({ Path, MoveTemp(Text), false });
			}
			WritePending(*State);
		});
}


bool FBridgeFileWriter::Flush()
{
	return WritePending(*State);
}


bool FBridgeFileWriter::WriteOnce(const FPendingWrite& Write)
{
	// The one that reads these can have them open at any time, which must not be in the way of writing them
	if (Write.bAppend)
	{
		return FFileHelper::SaveStringToFile(Write.Text, *Write.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_Append | FILEWRITE_AllowRead);
	}

	// Replaced as a whole, so that it's never read half written
	const FString TemporaryPath = Write.Path + TEXT(".tmp");
	return FFileHelper::SaveStringToFile(Write.Text, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_AllowRead)
		&& IFileManager::Get().Move(*Write.Path, *TemporaryPath, true, true);
}


bool FBridgeFileWriter::WritePending(FState& State)
{
	constexpr int32 MaxAttempts = 40;
	constexpr float SecondsBetweenAttempts = 0.025f;

	// Taken before the pending ones are, that's what keeps the order
	FScopeLock WriteLock{ &State.WriteMutex };

	TArray<FPendingWrite> ToWrite;
	{
		FScopeLock Lock{ &State.PendingMutex };
		ToWrite = MoveTemp(State.Pending);
		State.Pending.Reset();
	}

	for (int32 i = 0; i < ToWrite.Num(); i++)
	{
		bool bWritten = false;
		for (int32 Attempt = 0; Attempt < MaxAttempts && !bWritten; Attempt++)
		{
			if (Attempt > 0)
			{
				FPlatformProcess::Sleep(SecondsBetweenAttempts);
			}
			bWritten = WriteOnce(ToWrite[i]);
		}

		if (!bWritten)
		{
			UE_LOG(LogCartographBridge, Error, TEXT("Couldn't write to %s, will be tried again"), *ToWrite[i].Path);

			// Nothing is dropped: this one and the ones after it go back to where they came from, ahead of
			// what has been added since, to be written with the next ones
			FScopeLock Lock{ &State.PendingMutex };
			TArray<FPendingWrite> Remaining;
			for (int32 j = i; j < ToWrite.Num(); j++)
			{
				Remaining.Add(MoveTemp(ToWrite[j]));
			}
			Remaining.Append(MoveTemp(State.Pending));
			State.Pending = MoveTemp(Remaining);
			return false;
		}
	}
	return true;
}
#pragma endregion


FCartographBridge::FCartographBridge(const FString& InDirectory)
	: Directory(InDirectory)
	, CommandsPath(FPaths::Combine(InDirectory, TEXT("commands.jsonl")))
	, ResultsPath(FPaths::Combine(InDirectory, TEXT("results.jsonl")))
	, EventsPath(FPaths::Combine(InDirectory, TEXT("events.jsonl")))
	, StatusPath(FPaths::Combine(InDirectory, TEXT("bridge_status.json")))
	, StartedPath(FPaths::Combine(InDirectory, TEXT("started.jsonl")))
{
	StartTime = FPlatformTime::Seconds();
	LastTickTime = StartTime;

	IFileManager::Get().MakeDirectory(*FPaths::Combine(Directory, TEXT("samples")), true);
	ReadCompletedIds();
	FailInterruptedCommands();

	UE_LOG(LogCartographBridge, Log, TEXT("Cartograph test bridge %s active in %s, %d commands already completed"),
		BridgeVersion, *Directory, CompletedIds.Num());

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FCartographBridge::Tick));
	WorldBeginPlayHandle = FWorldDelegates::OnPostWorldInitialization.AddLambda(
		[this](UWorld* World, const UWorld::InitializationValues)
		{
			if (World && World->IsGameWorld())
			{
				World->OnWorldBeginPlay.AddRaw(this, &FCartographBridge::OnWorldBeginPlay, World);
			}
		});
	WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddRaw(this, &FCartographBridge::OnWorldTearDown);

	TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
	Detail->SetStringField(TEXT("version"), BridgeVersion);
	Detail->SetNumberField(TEXT("game_changelist"), FEngineVersion::Current().GetChangelist());
	AddEvent(TEXT("bridge_start"), Detail);
}


FCartographBridge::~FCartographBridge()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	FWorldDelegates::OnPostWorldInitialization.Remove(WorldBeginPlayHandle);
	FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDownHandle);
	Writer.Flush();
}


double FCartographBridge::Now() const
{
	return FPlatformTime::Seconds() - StartTime;
}


UWorld* FCartographBridge::GetGameWorld() const
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}


FString FCartographBridge::ToLine(const TSharedRef<FJsonObject>& Object)
{
	FString Line;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> JsonWriter =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
	FJsonSerializer::Serialize(Object, JsonWriter);
	Line += TEXT("\n");
	return Line;
}


void FCartographBridge::AddEvent(const TCHAR* Event, const TSharedPtr<FJsonObject>& Detail)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("t"), Now());
	Object->SetNumberField(TEXT("frame"), static_cast<double>(GFrameCounter));
	Object->SetStringField(TEXT("event"), Event);
	if (Detail)
	{
		Object->SetObjectField(TEXT("detail"), Detail);
	}
	Writer.Append(EventsPath, ToLine(Object));
}


void FCartographBridge::OnWorldBeginPlay(UWorld* World)
{
	WorldBeginPlayCount++;

	TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
	Detail->SetStringField(TEXT("world"), GetNameSafe(World));
	AddEvent(TEXT("world_begin_play"), Detail);
}


void FCartographBridge::OnWorldTearDown(UWorld* World)
{
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// What the bridge has built is gone with the world
	Groups.Empty();

	TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
	Detail->SetStringField(TEXT("world"), GetNameSafe(World));
	AddEvent(TEXT("world_teardown"), Detail);
}


#pragma region Commands In
void FCartographBridge::ReadCompletedIds()
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *ResultsPath))
	{
		return;
	}

	for (const FString& Line : Lines)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		FString Id;
		if (FJsonSerializer::Deserialize(Reader, Object) && Object && Object->TryGetStringField(TEXT("id"), Id))
		{
			CompletedIds.Add(Id);
		}
	}
}


void FCartographBridge::FailInterruptedCommands()
{
	// Started by a process that was gone before it could report on them. Must not run again on their own,
	// what they have or haven't done to the world isn't known.
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *StartedPath))
	{
		return;
	}

	for (const FString& Line : Lines)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		FString Id;
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object || !Object->TryGetStringField(TEXT("id"), Id)
			|| CompletedIds.Contains(Id))
		{
			continue;
		}

		FString Name;
		Object->TryGetStringField(TEXT("cmd"), Name);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("id"), Id);
		Result->SetStringField(TEXT("cmd"), Name);
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("error"), TEXT("Interrupted: the game exited before the command had finished"));
		Result->SetBoolField(TEXT("interrupted"), true);
		for (const TCHAR* Field : { TEXT("frame_start"), TEXT("frame_end"), TEXT("t_start"), TEXT("t_end") })
		{
			Result->SetField(Field, NullValue());
		}
		Result->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());

		CompletedIds.Add(Id);
		Writer.Append(ResultsPath, ToLine(Result));
	}
	Writer.Flush();
}


void FCartographBridge::PollCommands()
{
	const int64 Size = IFileManager::Get().FileSize(*CommandsPath);
	if (Size < 0)
	{
		return;
	}
	if (Size < CommandsOffset)
	{
		// Replaced by a shorter one. The ids keep what's been run from running again.
		CommandsOffset = 0;
	}
	if (Size == CommandsOffset)
	{
		return;
	}

	TArray<uint8> Bytes;
	{
		const TUniquePtr<FArchive> Reader{ IFileManager::Get().CreateFileReader(*CommandsPath, FILEREAD_AllowWrite) };
		if (!Reader)
		{
			return;
		}
		Reader->Seek(CommandsOffset);
		Bytes.SetNumUninitialized(static_cast<int32>(Size - CommandsOffset));
		Reader->Serialize(Bytes.GetData(), Bytes.Num());
		if (Reader->IsError())
		{
			return;
		}
	}

	// Only complete lines, the rest is still being written
	const int32 LastNewline = Bytes.FindLast(static_cast<uint8>('\n'));
	if (LastNewline == INDEX_NONE)
	{
		return;
	}

	int32 Begin = 0;
	if (CommandsOffset == 0 && LastNewline >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		Begin = 3;
	}
	const FUTF8ToTCHAR Converted{ reinterpret_cast<const ANSICHAR*>(Bytes.GetData()) + Begin, LastNewline + 1 - Begin };
	const FString Text{ Converted.Length(), Converted.Get() };
	CommandsOffset += LastNewline + 1;

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, true);
	for (const FString& Line : Lines)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object)
		{
			UE_LOG(LogCartographBridge, Warning, TEXT("Not a command: %s"), *Line);
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("line"), Line);
			AddEvent(TEXT("bad_command"), Detail);
			continue;
		}

		FBridgeCommand Command;
		Command.Args = Object;
		if (!Object->TryGetStringField(TEXT("id"), Command.Id) || !Object->TryGetStringField(TEXT("cmd"), Command.Name))
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("line"), Line);
			AddEvent(TEXT("bad_command"), Detail);
			continue;
		}
		if (CompletedIds.Contains(Command.Id)
			|| Queue.ContainsByPredicate([&Command](const FBridgeCommand& Queued) { return Queued.Id == Command.Id; }))
		{
			continue;
		}
		Queue.Add(MoveTemp(Command));
	}
}
#pragma endregion


bool FCartographBridge::Tick(float)
{
	const double Time = FPlatformTime::Seconds();
	const double DeltaMs = (Time - LastTickTime) * 1000.0;
	LastTickTime = Time;

	if (!Samplers.IsEmpty())
	{
		const UCartographGameInstanceModule* Cartograph = UCartographGameInstanceModule::Instance;
		const FCartographDebugState State = Cartograph ? Cartograph->GetDebugState() : FCartographDebugState{};
		const FBridgeSampleRow Row{
			GFrameCounter, Time - StartTime, DeltaMs,
			FPlatformTime::ToMilliseconds64(GGameThreadTime), FPlatformTime::ToMilliseconds64(GRenderThreadTime),
			Cartograph != nullptr, State.bIsInitializing, State.bIsRedrawActive, State.bIsRedrawingEntirely,
			State.bIsPendingRedraw, State.bIsPendingRedrawEntire, State.PendingAddCount, State.PendingRemoveCount
		};
		for (auto& [Name, Sampler] : Samplers)
		{
			Sampler.Rows.Add(Row);
		}
	}

	if (Time >= NextPollTime)
	{
		NextPollTime = Time + PollInterval;
		PollCommands();
	}

	for (int32 i = 0; i < MaxCommandsPerFrame && !Queue.IsEmpty(); i++)
	{
		FBridgeCommand& Command = Queue[0];

		FString Error;
		ECommandStatus Status;
		if (!Command.bStarted)
		{
			Command.bStarted = true;
			Command.FrameStart = GFrameCounter;
			Command.TimeStart = Now();

			// On disk before it does anything, that's how the next process knows of it if this one doesn't make it
			TSharedRef<FJsonObject> Started = MakeShared<FJsonObject>();
			Started->SetStringField(TEXT("id"), Command.Id);
			Started->SetStringField(TEXT("cmd"), Command.Name);
			Writer.Append(StartedPath, ToLine(Started));
			if (ChangesTheWorld(Command.Name) && !Writer.Flush())
			{
				// Without it being known to have been started, it could be started once more
				Error = TEXT("Couldn't write that the command is started, it has not been run");
				Status = ECommandStatus::Failed;
			}
			else
			{
				Status = Start(Command, Error);
			}
		}
		else
		{
			Status = Continue(Command, Error);
		}

		if (Status == ECommandStatus::Running)
		{
			break;
		}
		Finish(Command, Status == ECommandStatus::Succeeded, Error);
		Queue.RemoveAt(0);
	}

	if (Time >= NextStatusTime)
	{
		NextStatusTime = Time + StatusInterval;

		const UWorld* World = GetGameWorld();
		TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
		Status->SetStringField(TEXT("version"), BridgeVersion);
		Status->SetNumberField(TEXT("t"), Now());
		Status->SetNumberField(TEXT("frame"), static_cast<double>(GFrameCounter));
		Status->SetStringField(TEXT("wall_clock_utc"), FDateTime::UtcNow().ToIso8601());
		Status->SetNumberField(TEXT("queued"), Queue.Num());
		Status->SetNumberField(TEXT("completed"), CompletedIds.Num());
		if (Queue.IsEmpty())
		{
			Status->SetField(TEXT("current_command"), NullValue());
		}
		else
		{
			Status->SetStringField(TEXT("current_command"), Queue[0].Id);
		}
		Status->SetStringField(TEXT("world"), GetNameSafe(World));
		Writer.Overwrite(StatusPath, ToLine(Status));
	}

	return true;
}


void FCartographBridge::Finish(FBridgeCommand& Command, bool bSucceeded, const FString& Error)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("id"), Command.Id);
	Result->SetStringField(TEXT("cmd"), Command.Name);
	Result->SetBoolField(TEXT("ok"), bSucceeded);
	if (bSucceeded)
	{
		Result->SetField(TEXT("error"), NullValue());
	}
	else
	{
		Result->SetStringField(TEXT("error"), Error);
		UE_LOG(LogCartographBridge, Warning, TEXT("Command %s (%s) failed: %s"), *Command.Id, *Command.Name, *Error);
	}
	Result->SetNumberField(TEXT("frame_start"), static_cast<double>(Command.FrameStart));
	Result->SetNumberField(TEXT("frame_end"), static_cast<double>(GFrameCounter));
	Result->SetNumberField(TEXT("t_start"), Command.TimeStart);
	Result->SetNumberField(TEXT("t_end"), Now());
	Result->SetObjectField(TEXT("data"), Command.Data);

	CompletedIds.Add(Command.Id);
	Writer.Append(ResultsPath, ToLine(Result));
}


#pragma region Commands
FCartographBridge::ECommandStatus FCartographBridge::Start(FBridgeCommand& Command, FString& OutError)
{
	const FString& Name = Command.Name;
	const FJsonObject& Args = *Command.Args;
	FJsonObject& Data = *Command.Data;

	double Timeout = DefaultTimeout;
	Args.TryGetNumberField(TEXT("timeout"), Timeout);
	Command.Deadline = Now() + Timeout;

	UWorld* World = GetGameWorld();
	UCartographGameInstanceModule* Cartograph = UCartographGameInstanceModule::Instance;

	if (Name == TEXT("ping"))
	{
		Data.SetStringField(TEXT("version"), BridgeVersion);
		Data.SetBoolField(TEXT("cartograph_present"), Cartograph != nullptr);
		Data.SetNumberField(TEXT("game_changelist"), FEngineVersion::Current().GetChangelist());
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("state"))
	{
		FillState(Data);
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("mem"))
	{
		FillMemoryStats(Data);
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("wait"))
	{
		double Seconds = 1;
		Args.TryGetNumberField(TEXT("seconds"), Seconds);
		Command.Deadline = Now() + Seconds;
		return ECommandStatus::Running;
	}
	if (Name == TEXT("wait_world") || Name == TEXT("wait_init_done") || Name == TEXT("wait_redraw_idle"))
	{
		return Continue(Command, OutError);
	}
	if (Name == TEXT("sample_start"))
	{
		FString SamplerName;
		if (!Args.TryGetStringField(TEXT("name"), SamplerName) || SamplerName.IsEmpty())
		{
			OutError = TEXT("name is required");
			return ECommandStatus::Failed;
		}
		if (Samplers.Contains(SamplerName))
		{
			OutError = TEXT("A sampler with that name is already running");
			return ECommandStatus::Failed;
		}
		FBridgeSampler& Sampler = Samplers.Add(SamplerName);
		Sampler.StartTime = Now();
		Sampler.StartFrame = GFrameCounter;
		Sampler.Rows.Reserve(60 * 120);

		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("name"), SamplerName);
		AddEvent(TEXT("sample_start"), Detail);
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("sample_stop"))
	{
		FString SamplerName;
		if (!Args.TryGetStringField(TEXT("name"), SamplerName) || !Samplers.Contains(SamplerName))
		{
			OutError = TEXT("No sampler with that name is running");
			return ECommandStatus::Failed;
		}
		StopSampler(SamplerName, Data);
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("quit"))
	{
		// The result has to be on disk before the process is gone
		Finish(Command, true, FString{});
		Writer.Flush();
		CompletedIds.Add(Command.Id);
		FPlatformMisc::RequestExit(false);
		// Already finished above, keep it from being reported again
		Command.Name = TEXT("quit_requested");
		Command.Deadline = Now() + DefaultTimeout;
		return ECommandStatus::Running;
	}

	if (!World)
	{
		OutError = TEXT("There is no game world");
		return ECommandStatus::Failed;
	}

	if (Name == TEXT("load_save"))
	{
		FString SaveName;
		if (!Args.TryGetStringField(TEXT("name"), SaveName) || SaveName.IsEmpty())
		{
			OutError = TEXT("name is required");
			return ECommandStatus::Failed;
		}
		// "manager" is what the load menu does, which is what takes the player to where the game was saved.
		// Travelling to the map with the save as an option starts the player at the hub.
		FString Method = TEXT("manager");
		Args.TryGetStringField(TEXT("method"), Method);
		Data.SetStringField(TEXT("method"), Method);
		Data.SetStringField(TEXT("requested_save"), SaveName);
		Data.SetNumberField(TEXT("world_generation_at_request"), static_cast<double>(WorldBeginPlayCount));

		if (Method != TEXT("manager") && Method != TEXT("travel"))
		{
			OutError = FString::Printf(TEXT("There is no method %s, it's manager or travel"), *Method);
			return ECommandStatus::Failed;
		}

		// The load goes on after this, the world that's there now is not the one to wait for.
		// Only once the load is on its way: when it didn't start, the world that's there is the one that stays.
		const auto ExpectNewWorld = [this, &SaveName, Generation = WorldBeginPlayCount]
			{
				PendingLoadSave = SaveName;
				PendingLoadGeneration = Generation;
			};

		if (Method == TEXT("manager"))
		{
			UFGSaveSystem* SaveSystem = UFGSaveSystem::Get(World);
			APlayerController* PlayerController = World->GetFirstPlayerController();
			if (!SaveSystem || !PlayerController)
			{
				OutError = TEXT("There is no save system or no player controller");
				return ECommandStatus::Failed;
			}
			FSaveHeader Header;
			if (!SaveSystem->LoadSaveGameHeaderSync(SaveName, Header))
			{
				OutError = FString::Printf(TEXT("Can't read the header of the save %s"), *SaveName);
				return ECommandStatus::Failed;
			}
			Data.SetStringField(TEXT("session"), Header.SessionName);
			Data.SetStringField(TEXT("map"), Header.MapName);
			if (!SaveSystem->LoadSaveFile(Header, FLoadSaveFileParameters{}, PlayerController))
			{
				OutError = TEXT("The save system didn't start loading the save");
				return ECommandStatus::Failed;
			}
			ExpectNewWorld();
			return ECommandStatus::Succeeded;
		}
		ExpectNewWorld();

		FString Map = DefaultMap;
		Args.TryGetStringField(TEXT("map"), Map);
		FString Options = FString::Printf(TEXT("skipOnboarding?loadgame=%s"), *SaveName);
		FString ExtraOptions;
		if (Args.TryGetStringField(TEXT("options"), ExtraOptions) && !ExtraOptions.IsEmpty())
		{
			Options += TEXT("?") + ExtraOptions;
		}

		Data.SetStringField(TEXT("map"), Map);
		Data.SetStringField(TEXT("options"), Options);
		UGameplayStatics::OpenLevel(World, FName{ *Map }, true, Options);
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("exit_to_menu"))
	{
		APlayerController* PlayerController = World->GetFirstPlayerController();
		if (!PlayerController)
		{
			OutError = TEXT("There is no player controller");
			return ECommandStatus::Failed;
		}
		UFGBlueprintFunctionLibrary::TravelToMainMenu(PlayerController);
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("save"))
	{
		FString SaveName;
		if (!Args.TryGetStringField(TEXT("name"), SaveName) || SaveName.IsEmpty())
		{
			OutError = TEXT("name is required");
			return ECommandStatus::Failed;
		}
		UFGSaveSession* SaveSession = UFGSaveSession::Get(World);
		if (!SaveSession)
		{
			OutError = TEXT("There is no save session");
			return ECommandStatus::Failed;
		}

		CallbackResult.Reset();
		Command.bWaitingForCallback = true;
		AddEvent(TEXT("save_begin"));
		SaveSession->SaveGame(SaveName, FOnSaveGameComplete::CreateLambda(
			[this](bool bSucceeded, const FText& Message, void*)
			{
				CallbackResult = TPair<bool, FString>{ bSucceeded, Message.ToString() };
			}), nullptr);
		return ECommandStatus::Running;
	}
	if (Name == TEXT("delete_save"))
	{
		FString SaveName;
		if (!Args.TryGetStringField(TEXT("name"), SaveName) || SaveName.IsEmpty())
		{
			OutError = TEXT("name is required");
			return ECommandStatus::Failed;
		}
		if (!IsNameOfTestSave(SaveName))
		{
			OutError = FString::Printf(TEXT("Only saves of tests are deleted, those have a name starting with %s"), TestSavePrefix);
			return ECommandStatus::Failed;
		}
		// The session it's of has to be said as well, a name alone is given wrongly too easily
		FString ExpectedSession;
		if (!Args.TryGetStringField(TEXT("session"), ExpectedSession) || ExpectedSession.IsEmpty())
		{
			OutError = TEXT("session is required, the name of the session the save is of");
			return ECommandStatus::Failed;
		}
		UFGSaveSystem* SaveSystem = UFGSaveSystem::Get(World);
		if (!SaveSystem)
		{
			OutError = TEXT("There is no save system");
			return ECommandStatus::Failed;
		}
		FSaveHeader Header;
		if (!SaveSystem->LoadSaveGameHeaderSync(SaveName, Header))
		{
			OutError = FString::Printf(TEXT("Can't read the header of the save %s"), *SaveName);
			return ECommandStatus::Failed;
		}
		Data.SetStringField(TEXT("session"), Header.SessionName);
		Data.SetStringField(TEXT("map"), Header.MapName);
		if (Header.SessionName != ExpectedSession)
		{
			OutError = FString::Printf(TEXT("The save is of the session %s, not of %s"), *Header.SessionName, *ExpectedSession);
			return ECommandStatus::Failed;
		}

		// Through the save system of the game and not from the disk. Whether that's enough for a save to stay
		// gone where saves are also kept elsewhere isn't known, it has to be looked at after starting the game again.
		CallbackResult.Reset();
		Command.bWaitingForCallback = true;
		AddEvent(TEXT("delete_save_begin"));
		SaveSystem->DeleteSaveFiles({ SaveName }, FOnDeleteSaveGameComplete::CreateLambda(
			[this](bool bSucceeded, void*)
			{
				CallbackResult = TPair<bool, FString>{ bSucceeded, bSucceeded ? FString{} : FString{ TEXT("The game didn't delete the save") } };
			}), nullptr);
		return ECommandStatus::Running;
	}
	if (Name == TEXT("map_open") || Name == TEXT("map_close"))
	{
		AFGPlayerController* PlayerController = Cast<AFGPlayerController>(World->GetFirstPlayerController());
		if (!PlayerController)
		{
			OutError = TEXT("There is no player controller");
			return ECommandStatus::Failed;
		}
		const bool bWantVisible = Name == TEXT("map_open");
		if (Cartograph && Cartograph->GetDebugState().bIsMapVisible == bWantVisible)
		{
			Data.SetBoolField(TEXT("toggled"), false);
			return ECommandStatus::Succeeded;
		}
		// What the key does. Not something that can be called from here otherwise, it's the blueprint's.
		UFunction* ToggleMap = PlayerController->FindFunction(TEXT("ToggleMap"));
		if (!ToggleMap)
		{
			OutError = TEXT("The player controller has no ToggleMap");
			return ECommandStatus::Failed;
		}
		PlayerController->ProcessEvent(ToggleMap, nullptr);
		Data.SetBoolField(TEXT("toggled"), true);
		return ECommandStatus::Succeeded;
	}

	if (Name == TEXT("build"))
	{
		return ContinueBuild(Command, OutError);
	}
	if (Name == TEXT("dismantle"))
	{
		return ContinueDismantle(Command, OutError);
	}
	if (Name == TEXT("game_rules"))
	{
		// What the advanced game settings set, for a world of tests. Each of them only when it's given, and what
		// they are afterwards in any case, so without arguments it only reads. no_power leaves the power network
		// out of a test; where that is what's tested, as in a lab with real generators, don't use it.
		AFGGameState* GameState = World->GetGameState<AFGGameState>();
		AFGGameRulesSubsystem* GameRules = AFGGameRulesSubsystem::Get(World);
		if (!GameState || !GameRules)
		{
			OutError = TEXT("There is no game state or no game rules, is a game loaded?");
			return ECommandStatus::Failed;
		}

		bool bValue = false;
		if (Args.TryGetBoolField(TEXT("no_power"), bValue))
		{
			GameRules->SetNoPower(bValue);
		}
		if (Args.TryGetBoolField(TEXT("no_fuel"), bValue))
		{
			GameRules->SetNoFuelCost(bValue);
		}
		if (Args.TryGetBoolField(TEXT("no_cost"), bValue))
		{
			GameState->SetCheatNoCost(bValue);
		}
		if (Args.TryGetBoolField(TEXT("no_unlock_cost"), bValue))
		{
			GameRules->SetNoUnlockCost(bValue);
		}
		if (Args.TryGetBoolField(TEXT("unlock_all"), bValue) && bValue)
		{
			// Can't be taken back
			GameRules->UnlockAllMileStoneSchematics();
			GameRules->UnlockAllResearchSchematics();
			GameRules->UnlockAllResourceSinkSchematics();
			Data.SetBoolField(TEXT("unlocked_all"), true);
		}

		Data.SetBoolField(TEXT("no_power"), GameState->GetCheatNoPower());
		Data.SetBoolField(TEXT("no_fuel"), GameState->GetCheatNoFuel());
		Data.SetBoolField(TEXT("no_cost"), GameState->GetCheatNoCost());
		Data.SetBoolField(TEXT("no_unlock_cost"), GameRules->GetNoUnlockCost());
		Data.SetBoolField(TEXT("creative_mode"), GameState->IsCreativeModeEnabled());
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("power_link") || Name == TEXT("fuel") || Name == TEXT("power_state"))
	{
		return RunPowerCommand(*World, Name, Args, Data, OutError) ? ECommandStatus::Succeeded : ECommandStatus::Failed;
	}
	if (Name == TEXT("give_items"))
	{
		// Into the player's own inventory, as far as there's room in it. What didn't fit is said, not dropped.
		constexpr int32 MaxItems = 10000;
		FString ItemPath;
		int32 Count = 0;
		if (!Args.TryGetStringField(TEXT("item"), ItemPath) || !Args.TryGetNumberField(TEXT("count"), Count)
			|| Count <= 0 || Count > MaxItems)
		{
			OutError = FString::Printf(TEXT("item (the class path of an item) and count (1 to %d) are required"), MaxItems);
			return ECommandStatus::Failed;
		}
		const TSubclassOf<UFGItemDescriptor> ItemClass = LoadClass<UFGItemDescriptor>(nullptr, *ItemPath);
		if (!ItemClass)
		{
			OutError = FString::Printf(TEXT("Can't load the item %s"), *ItemPath);
			return ECommandStatus::Failed;
		}
		const APlayerController* PlayerController = World->GetFirstPlayerController();
		const AFGCharacterPlayer* Character = PlayerController ? Cast<AFGCharacterPlayer>(PlayerController->GetPawn()) : nullptr;
		UFGInventoryComponent* Inventory = Character ? Character->GetInventory() : nullptr;
		if (!Inventory)
		{
			OutError = TEXT("There is no player with an inventory");
			return ECommandStatus::Failed;
		}
		const int32 Added = Inventory->AddStack(FInventoryStack{ Count, ItemClass }, true);
		Data.SetStringField(TEXT("item"), ItemClass->GetPathName());
		Data.SetNumberField(TEXT("requested"), Count);
		Data.SetNumberField(TEXT("added"), Added);
		if (Added == 0)
		{
			OutError = TEXT("Nothing fitted into the inventory");
			return ECommandStatus::Failed;
		}
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("teleport"))
	{
		const APlayerController* PlayerController = World->GetFirstPlayerController();
		APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		FVector Location;
		if (!Pawn || !ReadVector(Args, TEXT("location"), Location))
		{
			OutError = TEXT("There is no player, or no location: [x, y, z]");
			return ECommandStatus::Failed;
		}
		const bool bTeleported = Pawn->TeleportTo(Location, Pawn->GetActorRotation(), false, true);
		const FVector Now = Pawn->GetActorLocation();
		Data.SetBoolField(TEXT("teleported"), bTeleported);
		Data.SetArrayField(TEXT("player_location"), {
			MakeShared<FJsonValueNumber>(Now.X), MakeShared<FJsonValueNumber>(Now.Y), MakeShared<FJsonValueNumber>(Now.Z) });
		if (!bTeleported)
		{
			OutError = TEXT("The player couldn't be put there");
			return ECommandStatus::Failed;
		}
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("view") || Name == TEXT("set_view"))
	{
		APlayerController* PlayerController = World->GetFirstPlayerController();
		if (!PlayerController)
		{
			OutError = TEXT("There is no player controller");
			return ECommandStatus::Failed;
		}

		if (Name == TEXT("set_view"))
		{
			FRotator Rotation = PlayerController->GetControlRotation();
			double Yaw = Rotation.Yaw;
			double Pitch = Rotation.Pitch;
			Args.TryGetNumberField(TEXT("yaw"), Yaw);
			Args.TryGetNumberField(TEXT("pitch"), Pitch);
			PlayerController->SetControlRotation(FRotator{ Pitch, Yaw, 0.0 });
		}

		const auto ToJson = [](const FVector& Vector)
			{
				return TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(Vector.X), MakeShared<FJsonValueNumber>(Vector.Y), MakeShared<FJsonValueNumber>(Vector.Z) };
			};

		if (const APawn* Pawn = PlayerController->GetPawn())
		{
			Data.SetArrayField(TEXT("player_location"), ToJson(Pawn->GetActorLocation()));
			Data.SetStringField(TEXT("player_class"), Pawn->GetClass()->GetName());

			// To tell the player of the save from one that has been started anew
			Data.SetStringField(TEXT("player_pawn_name"), Pawn->GetName());
			Data.SetStringField(TEXT("player_movement_base"),
				GetNameSafe(APawn::GetMovementBaseActor(Pawn)));
			const AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(Pawn);
			if (const UFGInventoryComponent* Inventory = Character ? Character->GetInventory() : nullptr)
			{
				TArray<FInventoryStack> Stacks;
				Inventory->GetInventoryStacks(Stacks);
				int32 Items = 0;
				for (const FInventoryStack& Stack : Stacks)
				{
					Items += Stack.NumItems;
				}
				Data.SetNumberField(TEXT("inventory_slots"), Inventory->GetSizeLinear());
				Data.SetNumberField(TEXT("inventory_used_slots"), Stacks.Num());
				Data.SetNumberField(TEXT("inventory_items"), Items);
			}
			else
			{
				Data.SetField(TEXT("inventory_slots"), NullValue());
				Data.SetField(TEXT("inventory_used_slots"), NullValue());
				Data.SetField(TEXT("inventory_items"), NullValue());
			}
		}
		else
		{
			Data.SetField(TEXT("player_location"), NullValue());
			Data.SetField(TEXT("player_class"), NullValue());
		}

		const FRotator ControlRotation = PlayerController->GetControlRotation();
		Data.SetNumberField(TEXT("control_yaw"), ControlRotation.Yaw);
		Data.SetNumberField(TEXT("control_pitch"), ControlRotation.Pitch);

		FVector CameraLocation;
		FRotator CameraRotation;
		PlayerController->GetPlayerViewPoint(CameraLocation, CameraRotation);
		Data.SetArrayField(TEXT("camera_location"), ToJson(CameraLocation));
		Data.SetNumberField(TEXT("camera_yaw"), CameraRotation.Yaw);
		Data.SetNumberField(TEXT("camera_pitch"), CameraRotation.Pitch);
		if (const APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
		{
			Data.SetNumberField(TEXT("camera_fov"), CameraManager->GetFOVAngle());
		}

		// What's rendered at, which isn't necessarily what the settings say
		if (GEngine && GEngine->GameViewport)
		{
			FVector2D ViewportSize;
			GEngine->GameViewport->GetViewportSize(ViewportSize);
			Data.SetNumberField(TEXT("viewport_width"), ViewportSize.X);
			Data.SetNumberField(TEXT("viewport_height"), ViewportSize.Y);
		}
		else
		{
			Data.SetField(TEXT("viewport_width"), NullValue());
			Data.SetField(TEXT("viewport_height"), NullValue());
		}
		Data.SetNumberField(TEXT("system_resolution_x"), GSystemResolution.ResX);
		Data.SetNumberField(TEXT("system_resolution_y"), GSystemResolution.ResY);
		Data.SetNumberField(TEXT("window_mode"), static_cast<int32>(GSystemResolution.WindowMode));
		for (const TCHAR* Variable : { TEXT("r.ScreenPercentage"), TEXT("r.VSync"), TEXT("t.MaxFPS") })
		{
			if (const IConsoleVariable* ConsoleVariable = IConsoleManager::Get().FindConsoleVariable(Variable))
			{
				Data.SetNumberField(Variable, ConsoleVariable->GetFloat());
			}
			else
			{
				Data.SetField(Variable, NullValue());
			}
		}
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("machine_setup"))
	{
		return SetUpMachines(*World, Args, Data, OutError) ? ECommandStatus::Succeeded : ECommandStatus::Failed;
	}
	if (Name == TEXT("machines"))
	{
		return ListMachines(*World, Args, Data, OutError) ? ECommandStatus::Succeeded : ECommandStatus::Failed;
	}
	if (Name == TEXT("adopt"))
	{
		return AdoptBuildables(*World, Args, Data, OutError) ? ECommandStatus::Succeeded : ECommandStatus::Failed;
	}

	if (!Cartograph)
	{
		OutError = TEXT("Cartograph isn't loaded");
		return ECommandStatus::Failed;
	}

	if (Name == TEXT("map_open_direct") || Name == TEXT("map_close_direct"))
	{
		Cartograph->SetMapVisible(Name == TEXT("map_open_direct"));
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("full_redraw"))
	{
		Cartograph->RequestEntireRedraw();
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("verify_indices"))
	{
		TArray<FString> Errors;
		const int32 ErrorCount = Cartograph->VerifyBuildingIndices(Errors);
		Data.SetNumberField(TEXT("error_count"), ErrorCount);
		TArray<TSharedPtr<FJsonValue>> ErrorValues;
		for (const FString& Error : Errors)
		{
			ErrorValues.Add(MakeShared<FJsonValueString>(Error));
		}
		Data.SetArrayField(TEXT("errors"), ErrorValues);
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("rt_hash"))
	{
		return HashRenderTarget(Args, Data, OutError) ? ECommandStatus::Succeeded : ECommandStatus::Failed;
	}
	if (Name == TEXT("canvas_probe"))
	{
		return ProbeCanvas(*World, Data, OutError) ? ECommandStatus::Succeeded : ECommandStatus::Failed;
	}
	if (Name == TEXT("layers"))
	{
		TArray<TSharedPtr<FJsonValue>> Categories;
		for (const FLayerCategoryData& Category : Cartograph->LayerCategories)
		{
			TSharedRef<FJsonObject> CategoryObject = MakeShared<FJsonObject>();
			CategoryObject->SetStringField(TEXT("name"), Category.Name.ToString());
			CategoryObject->SetBoolField(TEXT("enabled"), !Cartograph->RuntimeConfig.DisabledLayerMainCategory.Contains(Category.Name));

			const TSet<FName>* DisabledSubCategories = Cartograph->RuntimeConfig.DisabledLayerSubCategory.Find(Category.Name);
			TArray<TSharedPtr<FJsonValue>> SubCategories;
			for (const FLayerSubCategoryData& SubCategory : Category.SubCategories)
			{
				TSharedRef<FJsonObject> SubCategoryObject = MakeShared<FJsonObject>();
				SubCategoryObject->SetStringField(TEXT("name"), SubCategory.Name.ToString());
				SubCategoryObject->SetBoolField(TEXT("enabled"), !DisabledSubCategories || !DisabledSubCategories->Contains(SubCategory.Name));
				SubCategories.Add(MakeShared<FJsonValueObject>(SubCategoryObject));
			}
			CategoryObject->SetArrayField(TEXT("sub_categories"), SubCategories);
			Categories.Add(MakeShared<FJsonValueObject>(CategoryObject));
		}
		Data.SetArrayField(TEXT("categories"), Categories);
		Data.SetNumberField(TEXT("disabled_buildable_count"), Cartograph->RuntimeConfig.DisabledLayerBuildable.Num());
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("set_layer"))
	{
		// What the checkboxes of the layer menu do. Kept by the game with the rest of the layer settings,
		// so a test has to put back what it changes.
		FString Main;
		bool bEnabled = true;
		if (!Args.TryGetStringField(TEXT("main"), Main) || !Args.TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			OutError = TEXT("main and enabled are required");
			return ECommandStatus::Failed;
		}
		const FLayerCategoryData* Category = Cartograph->LayerCategories.FindByPredicate(
			[&Main](const FLayerCategoryData& Candidate) { return Candidate.Name.ToString() == Main; });
		if (!Category)
		{
			OutError = TEXT("There is no such main category");
			return ECommandStatus::Failed;
		}

		FString Sub;
		if (Args.TryGetStringField(TEXT("sub"), Sub) && !Sub.IsEmpty())
		{
			const FLayerSubCategoryData* SubCategory = Category->SubCategories.FindByPredicate(
				[&Sub](const FLayerSubCategoryData& Candidate) { return Candidate.Name.ToString() == Sub; });
			if (!SubCategory)
			{
				OutError = TEXT("There is no such sub category");
				return ECommandStatus::Failed;
			}
			TSet<FName>& Disabled = Cartograph->RuntimeConfig.DisabledLayerSubCategory.FindOrAdd(Category->Name);
			if (bEnabled)
			{
				Disabled.Remove(SubCategory->Name);
			}
			else
			{
				Disabled.Add(SubCategory->Name);
			}
		}
		else if (bEnabled)
		{
			Cartograph->RuntimeConfig.DisabledLayerMainCategory.Remove(Category->Name);
		}
		else
		{
			Cartograph->RuntimeConfig.DisabledLayerMainCategory.Add(Category->Name);
		}
		Cartograph->OnLayerConfigChanged();
		return ECommandStatus::Succeeded;
	}
	if (Name == TEXT("set_z_filter"))
	{
		// Fractions of the height range of what's been built, as the sliders of the map give them
		double Min = 0;
		double Max = 1;
		if (!Args.TryGetNumberField(TEXT("min"), Min) || !Args.TryGetNumberField(TEXT("max"), Max)
			|| Min < 0 || Max > 1 || Min > Max)
		{
			OutError = TEXT("min and max are required, 0 <= min <= max <= 1");
			return ECommandStatus::Failed;
		}
		UFunction* OnZFilterUpdated = Cartograph->FindFunction(TEXT("OnZFilterUpdated"));
		if (!OnZFilterUpdated)
		{
			OutError = TEXT("Cartograph has no OnZFilterUpdated");
			return ECommandStatus::Failed;
		}
		struct
		{
			float Min;
			float Max;
		} Parameters{ static_cast<float>(Min), static_cast<float>(Max) };
		Cartograph->ProcessEvent(OnZFilterUpdated, &Parameters);
		Data.SetNumberField(TEXT("min"), Min);
		Data.SetNumberField(TEXT("max"), Max);
		return ECommandStatus::Succeeded;
	}

	OutError = FString::Printf(TEXT("Unknown command %s"), *Name);
	return ECommandStatus::Failed;
}


FCartographBridge::ECommandStatus FCartographBridge::Continue(FBridgeCommand& Command, FString& OutError)
{
	const FString& Name = Command.Name;
	const UCartographGameInstanceModule* Cartograph = UCartographGameInstanceModule::Instance;

	const bool bTimedOut = Now() >= Command.Deadline;

	if (Name == TEXT("wait"))
	{
		return bTimedOut ? ECommandStatus::Succeeded : ECommandStatus::Running;
	}
	if (Name == TEXT("quit_requested"))
	{
		return ECommandStatus::Running;
	}
	if (Name == TEXT("build"))
	{
		return ContinueBuild(Command, OutError);
	}
	if (Name == TEXT("dismantle"))
	{
		return ContinueDismantle(Command, OutError);
	}

	if (Name == TEXT("save") || Name == TEXT("delete_save"))
	{
		if (CallbackResult.IsSet())
		{
			AddEvent(Name == TEXT("save") ? TEXT("save_end") : TEXT("delete_save_end"));
			Command.Data->SetNumberField(TEXT("duration"), Now() - Command.TimeStart);
			if (!CallbackResult->Key)
			{
				OutError = CallbackResult->Value;
				return ECommandStatus::Failed;
			}
			return ECommandStatus::Succeeded;
		}
	}
	else if (Name == TEXT("wait_world"))
	{
		const UWorld* World = GetGameWorld();
		const AFGGameMode* GameMode = World ? World->GetAuthGameMode<AFGGameMode>() : nullptr;
		const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
		const bool bIsNewerWorld = !PendingLoadGeneration.IsSet() || WorldBeginPlayCount > *PendingLoadGeneration;
		if (bIsNewerWorld && World && World->HasBegunPlay() && GameMode && !GameMode->IsMainMenuGameMode()
			&& PlayerController && PlayerController->GetPawn())
		{
			Command.Data->SetStringField(TEXT("world"), World->GetName());
			Command.Data->SetNumberField(TEXT("world_generation"), static_cast<double>(WorldBeginPlayCount));
			if (PendingLoadGeneration.IsSet())
			{
				Command.Data->SetStringField(TEXT("requested_save"), PendingLoadSave);
			}
			else
			{
				Command.Data->SetField(TEXT("requested_save"), NullValue());
			}
			PendingLoadGeneration.Reset();
			PendingLoadSave.Reset();
			return ECommandStatus::Succeeded;
		}
	}
	else if (Name == TEXT("wait_init_done") || Name == TEXT("wait_redraw_idle"))
	{
		if (!Cartograph)
		{
			OutError = TEXT("Cartograph isn't loaded");
			return ECommandStatus::Failed;
		}

		const FCartographDebugState State = Cartograph->GetDebugState();
		const bool bIsDone = Name == TEXT("wait_init_done")
			? !State.bIsInitializing
			: !State.bIsInitializing && !State.bIsRedrawActive && !State.bIsPendingRedraw;

		int32 SettleFrames = Name == TEXT("wait_init_done") ? 1 : 3;
		Command.Args->TryGetNumberField(TEXT("settle_frames"), SettleFrames);

		Command.SettledFrames = bIsDone ? Command.SettledFrames + 1 : 0;
		if (Command.SettledFrames >= SettleFrames)
		{
			return ECommandStatus::Succeeded;
		}
	}

	if (bTimedOut)
	{
		OutError = TEXT("Timed out");
		return ECommandStatus::Failed;
	}
	return ECommandStatus::Running;
}


FCartographBridge::ECommandStatus FCartographBridge::ContinueBuild(FBridgeCommand& Command, FString& OutError)
{
	const FJsonObject& Args = *Command.Args;
	UWorld* World = GetGameWorld();
	if (!World)
	{
		OutError = TEXT("There is no game world");
		return ECommandStatus::Failed;
	}

	FString RecipePath;
	if (!Args.TryGetStringField(TEXT("recipe"), RecipePath))
	{
		OutError = TEXT("recipe is required");
		return ECommandStatus::Failed;
	}
	const TSubclassOf<UFGRecipe> Recipe = LoadClass<UFGRecipe>(nullptr, *RecipePath);
	if (!Recipe)
	{
		OutError = FString::Printf(TEXT("Can't load the recipe %s"), *RecipePath);
		return ECommandStatus::Failed;
	}
	const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
	const TSubclassOf<UFGBuildingDescriptor> Descriptor = Products.IsEmpty() ? nullptr : TSubclassOf<UFGBuildingDescriptor>{ Products[0].ItemClass };
	const TSubclassOf<AFGBuildable> BuildableClass = Descriptor ? UFGBuildingDescriptor::GetBuildableClass(Descriptor) : nullptr;
	if (!BuildableClass)
	{
		OutError = TEXT("The recipe doesn't make a buildable");
		return ECommandStatus::Failed;
	}

	AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(World);
	if (!BuildableSubsystem)
	{
		OutError = TEXT("There is no buildable subsystem");
		return ECommandStatus::Failed;
	}

	int32 Count = 1;
	Args.TryGetNumberField(TEXT("count"), Count);
	double Rate = 0;
	Args.TryGetNumberField(TEXT("rate"), Rate);
	if (Rate <= 0 && Count > MaxBuildsPerFrame)
	{
		OutError = FString::Printf(TEXT("At most %d at once, give a rate for more"), MaxBuildsPerFrame);
		return ECommandStatus::Failed;
	}
	Command.Total = Count;

	// Where to, decided once: what it's relative to can have moved by the time the last ones are built
	if (!Command.bHasPlacement)
	{
		const APlayerController* PlayerController = World->GetFirstPlayerController();
		const APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;

		if (!ReadVector(Args, TEXT("origin"), Command.Origin))
		{
			if (!Pawn)
			{
				OutError = TEXT("There is no player to build at, give an origin");
				return ECommandStatus::Failed;
			}
			Command.Origin = Pawn->GetActorLocation();
		}

		// "view": X is the direction that's looked in, level with the ground, Y is to the right of it
		FString Orient;
		if (Args.TryGetStringField(TEXT("orient"), Orient) && Orient == TEXT("view"))
		{
			if (!PlayerController)
			{
				OutError = TEXT("There is no player to take the view of");
				return ECommandStatus::Failed;
			}
			Command.Orientation = FRotator{ 0.0, PlayerController->GetControlRotation().Yaw, 0.0 }.Quaternion();
		}
		double Yaw = 0;
		if (Args.TryGetNumberField(TEXT("yaw"), Yaw))
		{
			Command.Orientation = FRotator{ 0.0, Yaw, 0.0 }.Quaternion();
		}
		Command.bHasPlacement = true;
	}
	const FVector Origin = Command.Origin;

	FVector Offset = FVector::ZeroVector;
	ReadVector(Args, TEXT("offset"), Offset);
	bool bSnapToGround = false;
	Args.TryGetBoolField(TEXT("snap_to_ground"), bSnapToGround);
	double Spacing = 800;
	Args.TryGetNumberField(TEXT("spacing"), Spacing);
	int32 Columns = 20;
	Args.TryGetNumberField(TEXT("columns"), Columns);
	Columns = FMath::Max(1, Columns);
	FString GroupName = TEXT("default");
	Args.TryGetStringField(TEXT("group"), GroupName);

	TArray<FBridgeBuildable>& Group = Groups.FindOrAdd(GroupName);
	if (Command.Done == 0)
	{
		// Goes on where the group ends, so that they don't end up in each other
		Command.SettledFrames = Group.Num();
		Command.Data->SetStringField(TEXT("buildable_class"), BuildableClass->GetPathName());
		// What it's relative to, to be given again to have the same once more
		Command.Data->SetArrayField(TEXT("origin"), {
			MakeShared<FJsonValueNumber>(Origin.X), MakeShared<FJsonValueNumber>(Origin.Y), MakeShared<FJsonValueNumber>(Origin.Z) });
	}
	const int32 FirstSlot = Command.SettledFrames;

	int32 Wanted = Count;
	if (Rate > 0)
	{
		const double Elapsed = Now() - Command.TimeStart;
		Wanted = FMath::Min(Count, 1 + FMath::FloorToInt32(Elapsed * Rate));
		Wanted = FMath::Min(Wanted, Command.Done + MaxBuildsPerFrame);
	}

	const int32 DoneBefore = Command.Done;
	while (Command.Done < Wanted)
	{
		const int32 Slot = FirstSlot + Command.Done;
		// As it has been all along: the columns along X, the rows along Y. X is forward when it's given a direction.
		const FVector InGrid = Offset + FVector{ (Slot % Columns) * Spacing, (Slot / Columns) * Spacing, 0.0 };
		FVector Location = Origin + Command.Orientation.RotateVector(InGrid);
		if (bSnapToGround)
		{
			FHitResult Hit;
			FCollisionQueryParams Query{ SCENE_QUERY_STAT(CartographBridgeGround), false };
			if (const APlayerController* PlayerController = World->GetFirstPlayerController())
			{
				Query.AddIgnoredActor(PlayerController->GetPawn());
			}
			if (World->LineTraceSingleByChannel(Hit, Location + FVector{ 0, 0, 5000 }, Location - FVector{ 0, 0, 50000 },
				ECC_WorldStatic, Query))
			{
				Location.Z = Hit.ImpactPoint.Z;
			}
			else
			{
				Command.SettledFramesMissedGround++;
			}
		}
		const FTransform Transform{ Command.Orientation, Location };
		if (Command.Done == 0)
		{
			Command.Data->SetArrayField(TEXT("first_location"), {
				MakeShared<FJsonValueNumber>(Location.X), MakeShared<FJsonValueNumber>(Location.Y), MakeShared<FJsonValueNumber>(Location.Z) });
		}

		AFGBuildable* Buildable = BuildableSubsystem->BeginSpawnBuildable(BuildableClass, Transform);
		if (!Buildable)
		{
			OutError = FString::Printf(TEXT("Couldn't spawn buildable %d"), Command.Done);
			Command.Data->SetNumberField(TEXT("spawned"), Command.Done);
			return ECommandStatus::Failed;
		}
		Buildable->SetBuiltWithRecipe(Recipe);
		Buildable->FinishSpawning(Transform);

		Group.Add({ BuildableClass, Transform, Buildable });
		Command.Done++;
	}

	if (Command.Done > DoneBefore)
	{
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("id"), Command.Id);
		Detail->SetStringField(TEXT("group"), GroupName);
		Detail->SetNumberField(TEXT("count"), Command.Done - DoneBefore);
		Detail->SetNumberField(TEXT("done"), Command.Done);
		AddEvent(TEXT("build"), Detail);
	}

	if (Command.Done < Count)
	{
		return ECommandStatus::Running;
	}

	Command.Data->SetNumberField(TEXT("spawned"), Command.Done);
	Command.Data->SetNumberField(TEXT("yaw"), Command.Orientation.Rotator().Yaw);
	if (bSnapToGround)
	{
		Command.Data->SetNumberField(TEXT("no_ground_found"), Command.SettledFramesMissedGround);
	}
	Command.Data->SetStringField(TEXT("group"), GroupName);
	Command.Data->SetNumberField(TEXT("group_size"), Group.Num());
	return ECommandStatus::Succeeded;
}


FCartographBridge::ECommandStatus FCartographBridge::ContinueDismantle(FBridgeCommand& Command, FString& OutError)
{
	const FJsonObject& Args = *Command.Args;
	UWorld* World = GetGameWorld();
	if (!World)
	{
		OutError = TEXT("There is no game world");
		return ECommandStatus::Failed;
	}

	FString GroupName = TEXT("default");
	Args.TryGetStringField(TEXT("group"), GroupName);
	TArray<FBridgeBuildable>* Group = Groups.Find(GroupName);
	if (!Group)
	{
		OutError = TEXT("The bridge hasn't built anything in that group");
		return ECommandStatus::Failed;
	}

	AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(World);

	if (Command.Total == 0 && Command.Done == 0)
	{
		int32 Count = Group->Num();
		Args.TryGetNumberField(TEXT("count"), Count);
		Command.Total = FMath::Clamp(Count, 0, Group->Num());
		if (Command.Total == 0)
		{
			Command.Data->SetNumberField(TEXT("removed"), 0);
			return ECommandStatus::Succeeded;
		}

		// The ones that aren't an actor anymore have become lightweight, those are found by where they are.
		// Done in one go up front, it would show up in the frame times otherwise.
		AddEvent(TEXT("dismantle_resolve_begin"));
		TMap<UClass*, TMap<FIntVector, int32>> IndicesByClass;
		Command.LightweightIndices.Init(INDEX_NONE, Command.Total);
		int32 LightweightCount = 0;
		for (int32 i = 0; i < Command.Total; i++)
		{
			const FBridgeBuildable& Buildable = (*Group)[Group->Num() - 1 - i];
			if (Buildable.Actor.IsValid() || !LightweightSubsystem)
			{
				continue;
			}

			TMap<FIntVector, int32>* Indices = IndicesByClass.Find(Buildable.BuildableClass);
			if (!Indices)
			{
				Indices = &IndicesByClass.Add(Buildable.BuildableClass);
				if (const TArray<FRuntimeBuildableInstanceData>* Instances =
					LightweightSubsystem->GetAllLightweightBuildableInstances().Find(Buildable.BuildableClass))
				{
					for (int32 Index = 0; Index < Instances->Num(); Index++)
					{
						if ((*Instances)[Index].IsValid())
						{
							Indices->Add(Quantize((*Instances)[Index].Transform.GetLocation()), Index);
						}
					}
				}
			}
			if (const int32* Index = Indices->Find(Quantize(Buildable.Transform.GetLocation())))
			{
				Command.LightweightIndices[i] = *Index;
				LightweightCount++;
			}
		}
		Command.Data->SetNumberField(TEXT("lightweight"), LightweightCount);
		AddEvent(TEXT("dismantle_resolve_end"));
	}

	double Rate = 0;
	Args.TryGetNumberField(TEXT("rate"), Rate);
	int32 Wanted = Command.Total;
	if (Rate > 0)
	{
		const double Elapsed = Now() - Command.TimeStart;
		Wanted = FMath::Min(Command.Total, 1 + FMath::FloorToInt32(Elapsed * Rate));
	}
	Wanted = FMath::Min(Wanted, Command.Done + MaxBuildsPerFrame);

	const int32 DoneBefore = Command.Done;
	while (Command.Done < Wanted)
	{
		const FBridgeBuildable Buildable = Group->Pop(EAllowShrinking::No);
		const int32 LightweightIndex = Command.LightweightIndices[Command.Done];
		Command.Done++;

		if (AFGBuildable* Actor = Buildable.Actor.Get())
		{
			IFGDismantleInterface::Execute_Dismantle(Actor);
			if (IsValid(Actor))
			{
				Actor->Destroy();
			}
		}
		else if (LightweightIndex != INDEX_NONE && LightweightSubsystem)
		{
			LightweightSubsystem->RemoveByInstanceIndex(Buildable.BuildableClass, LightweightIndex);
		}
		else
		{
			Command.SettledFrames++;  // Not found
		}
	}

	if (Command.Done > DoneBefore)
	{
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("id"), Command.Id);
		Detail->SetStringField(TEXT("group"), GroupName);
		Detail->SetNumberField(TEXT("count"), Command.Done - DoneBefore);
		Detail->SetNumberField(TEXT("done"), Command.Done);
		AddEvent(TEXT("dismantle"), Detail);
	}

	if (Command.Done < Command.Total)
	{
		return ECommandStatus::Running;
	}

	Command.Data->SetNumberField(TEXT("removed"), Command.Done - Command.SettledFrames);
	Command.Data->SetNumberField(TEXT("not_found"), Command.SettledFrames);
	Command.Data->SetNumberField(TEXT("group_size"), Group->Num());
	if (Command.SettledFrames > 0)
	{
		OutError = FString::Printf(TEXT("%d of them weren't found"), Command.SettledFrames);
		return ECommandStatus::Failed;
	}
	return ECommandStatus::Succeeded;
}
#pragma endregion


#pragma region Queries
void FCartographBridge::FillState(FJsonObject& Data) const
{
	const UWorld* World = GetGameWorld();
	const AFGGameMode* GameMode = World ? World->GetAuthGameMode<AFGGameMode>() : nullptr;

	Data.SetStringField(TEXT("world"), GetNameSafe(World));
	Data.SetBoolField(TEXT("world_has_begun_play"), World && World->HasBegunPlay());
	if (GameMode)
	{
		Data.SetBoolField(TEXT("is_menu"), GameMode->IsMainMenuGameMode());
	}
	else
	{
		Data.SetField(TEXT("is_menu"), NullValue());
	}
	if (World)
	{
		Data.SetNumberField(TEXT("net_mode"), World->GetNetMode());
	}
	else
	{
		Data.SetField(TEXT("net_mode"), NullValue());
	}
	Data.SetNumberField(TEXT("world_begin_play_count"), static_cast<double>(WorldBeginPlayCount));

	int32 BridgeBuildableCount = 0;
	for (const auto& [Name, Group] : Groups)
	{
		BridgeBuildableCount += Group.Num();
	}
	Data.SetNumberField(TEXT("bridge_buildable_count"), BridgeBuildableCount);

	const UCartographGameInstanceModule* Cartograph = UCartographGameInstanceModule::Instance;
	Data.SetBoolField(TEXT("cartograph_present"), Cartograph != nullptr);
	if (!Cartograph)
	{
		return;
	}

	const FCartographDebugState State = Cartograph->GetDebugState(true);
	Data.SetBoolField(TEXT("is_initializing"), State.bIsInitializing);
	Data.SetNumberField(TEXT("initialize_progress"), State.InitializeProgress);
	Data.SetBoolField(TEXT("is_redraw_active"), State.bIsRedrawActive);
	Data.SetBoolField(TEXT("is_redrawing_entirely"), State.bIsRedrawingEntirely);
	Data.SetBoolField(TEXT("is_pending_redraw"), State.bIsPendingRedraw);
	Data.SetBoolField(TEXT("is_pending_redraw_entire"), State.bIsPendingRedrawEntire);
	Data.SetNumberField(TEXT("pending_add_count"), State.PendingAddCount);
	Data.SetNumberField(TEXT("pending_remove_count"), State.PendingRemoveCount);
	Data.SetNumberField(TEXT("building_count"), State.BuildingCount);
	Data.SetNumberField(TEXT("drawn_building_count"), State.DrawnBuildingCount);
	Data.SetNumberField(TEXT("index_redirector_count"), State.IndexRedirectorCount);
	Data.SetBoolField(TEXT("is_client"), State.bIsClient);
	Data.SetBoolField(TEXT("is_map_visible"), State.bIsMapVisible);
	Data.SetBoolField(TEXT("render_target_needs_full_redraw"), State.bRenderTargetNeedsFullRedraw);
	Data.SetBoolField(TEXT("is_world_torn_down"), State.bIsWorldTornDown);
	Data.SetBoolField(TEXT("free_render_target_when_closed"), State.bFreeRenderTargetWhenClosed);
	Data.SetBoolField(TEXT("generate_mips"), State.bGenerateMips);
	Data.SetNumberField(TEXT("configured_render_texture_size"), State.ConfiguredRenderTextureSize);
	Data.SetBoolField(TEXT("has_render_target"), State.bHasRenderTarget);
	Data.SetBoolField(TEXT("has_render_target_resource"), State.bHasRenderTargetResource);
	Data.SetBoolField(TEXT("render_target_auto_generates_mips"), State.bRenderTargetAutoGeneratesMips);
	Data.SetNumberField(TEXT("render_target_size_x"), State.RenderTargetSizeX);
	Data.SetNumberField(TEXT("render_target_size_y"), State.RenderTargetSizeY);
}


void FCartographBridge::FillMemoryStats(FJsonObject& Data) const
{
	const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
	Data.SetNumberField(TEXT("process_used_physical"), static_cast<double>(Memory.UsedPhysical));
	Data.SetNumberField(TEXT("process_used_virtual"), static_cast<double>(Memory.UsedVirtual));
	Data.SetNumberField(TEXT("process_peak_physical"), static_cast<double>(Memory.PeakUsedPhysical));
	Data.SetNumberField(TEXT("process_peak_virtual"), static_cast<double>(Memory.PeakUsedVirtual));
	Data.SetNumberField(TEXT("system_available_physical"), static_cast<double>(Memory.AvailablePhysical));

	if (GDynamicRHI)
	{
		FTextureMemoryStats Texture;
		RHIGetTextureMemoryStats(Texture);
		SetKnownNumber(Data, TEXT("rhi_dedicated_video_memory"), Texture.DedicatedVideoMemory);
		SetKnownNumber(Data, TEXT("rhi_dedicated_system_memory"), Texture.DedicatedSystemMemory);
		SetKnownNumber(Data, TEXT("rhi_shared_system_memory"), Texture.SharedSystemMemory);
		SetKnownNumber(Data, TEXT("rhi_total_graphics_memory"), Texture.TotalGraphicsMemory);
		Data.SetNumberField(TEXT("rhi_used_graphics_memory"), static_cast<double>(Texture.UsedGraphicsMemory));
		Data.SetNumberField(TEXT("rhi_streaming_memory_size"), static_cast<double>(Texture.StreamingMemorySize));
		Data.SetNumberField(TEXT("rhi_non_streaming_memory_size"), static_cast<double>(Texture.NonStreamingMemorySize));
		Data.SetNumberField(TEXT("rhi_texture_pool_size"), static_cast<double>(Texture.TexturePoolSize));
		Data.SetStringField(TEXT("adapter_name"), GRHIAdapterName);
	}
	else
	{
		for (const TCHAR* Field : { TEXT("rhi_dedicated_video_memory"), TEXT("rhi_dedicated_system_memory"),
			TEXT("rhi_shared_system_memory"), TEXT("rhi_total_graphics_memory"), TEXT("rhi_used_graphics_memory"),
			TEXT("rhi_streaming_memory_size"), TEXT("rhi_non_streaming_memory_size"), TEXT("rhi_texture_pool_size"),
			TEXT("adapter_name") })
		{
			Data.SetField(Field, NullValue());
		}
	}
}


bool FCartographBridge::HashRenderTarget(const FJsonObject& Args, FJsonObject& Data, FString& OutError) const
{
	const UCartographGameInstanceModule* Cartograph = UCartographGameInstanceModule::Instance;
	UCanvasRenderTarget2D* RenderTarget = Cartograph ? Cartograph->GetRenderTarget() : nullptr;
	FTextureRenderTargetResource* Resource = RenderTarget ? RenderTarget->GameThread_GetRenderTargetResource() : nullptr;
	if (!Resource)
	{
		OutError = TEXT("The render target has no resource");
		return false;
	}

	FCartographBridge* MutableThis = const_cast<FCartographBridge*>(this);
	MutableThis->AddEvent(TEXT("rt_hash_begin"));
	const double Begin = FPlatformTime::Seconds();

	TArray<FColor> Pixels;
	const bool bRead = Resource->ReadPixels(Pixels);

	int64 NonZero = 0;
	for (const FColor& Pixel : Pixels)
	{
		NonZero += Pixel.DWColor() != 0 ? 1 : 0;
	}
	const uint64 Hash = bRead && !Pixels.IsEmpty()
		? CityHash64(reinterpret_cast<const char*>(Pixels.GetData()), Pixels.Num() * sizeof(FColor))
		: 0;

	const int32 Width = RenderTarget->SizeX;
	const int32 Height = RenderTarget->SizeY;
	if (!bRead || Pixels.Num() != Width * Height)
	{
		MutableThis->AddEvent(TEXT("rt_hash_end"));
		OutError = TEXT("Couldn't read the render target");
		return false;
	}

	// To find where two of them differ: the same for parts of it, only of the ones that have something in them
	int32 Tiles = 0;
	if (Args.TryGetNumberField(TEXT("tiles"), Tiles) && Tiles > 0)
	{
		Tiles = FMath::Min(Tiles, 256);
		const int32 TileWidth = FMath::DivideAndRoundUp(Width, Tiles);
		const int32 TileHeight = FMath::DivideAndRoundUp(Height, Tiles);

		TArray<TSharedPtr<FJsonValue>> TileValues;
		TArray<FColor> TilePixels;
		for (int32 TileY = 0; TileY < Tiles; TileY++)
		{
			for (int32 TileX = 0; TileX < Tiles; TileX++)
			{
				const int32 BeginX = TileX * TileWidth;
				const int32 BeginY = TileY * TileHeight;
				const int32 EndX = FMath::Min(BeginX + TileWidth, Width);
				const int32 EndY = FMath::Min(BeginY + TileHeight, Height);
				if (BeginX >= EndX || BeginY >= EndY)
				{
					continue;
				}

				TilePixels.Reset();
				int32 TileNonZero = 0;
				for (int32 Y = BeginY; Y < EndY; Y++)
				{
					const FColor* Row = &Pixels[Y * Width + BeginX];
					TilePixels.Append(Row, EndX - BeginX);
					for (int32 X = 0; X < EndX - BeginX; X++)
					{
						TileNonZero += Row[X].DWColor() != 0 ? 1 : 0;
					}
				}
				if (TileNonZero == 0)
				{
					continue;
				}

				TSharedRef<FJsonObject> Tile = MakeShared<FJsonObject>();
				Tile->SetNumberField(TEXT("x"), TileX);
				Tile->SetNumberField(TEXT("y"), TileY);
				Tile->SetStringField(TEXT("hash"), FString::Printf(TEXT("%016llx"),
					CityHash64(reinterpret_cast<const char*>(TilePixels.GetData()), TilePixels.Num() * sizeof(FColor))));
				Tile->SetNumberField(TEXT("nonzero_pixels"), TileNonZero);
				TileValues.Add(MakeShared<FJsonValueObject>(Tile));
			}
		}
		Data.SetNumberField(TEXT("tiles"), Tiles);
		Data.SetNumberField(TEXT("tile_width"), TileWidth);
		Data.SetNumberField(TEXT("tile_height"), TileHeight);
		Data.SetArrayField(TEXT("nonempty_tiles"), TileValues);
	}

	// The pixels themselves, of all of it or of a part: [x0, y0, x1, y1]
	FString DumpName;
	if (Args.TryGetStringField(TEXT("dump"), DumpName) && !DumpName.IsEmpty())
	{
		int32 BeginX = 0, BeginY = 0, EndX = Width, EndY = Height;
		const TArray<TSharedPtr<FJsonValue>>* Region = nullptr;
		if (Args.TryGetArrayField(TEXT("region"), Region) && Region->Num() == 4)
		{
			BeginX = FMath::Clamp(static_cast<int32>((*Region)[0]->AsNumber()), 0, Width);
			BeginY = FMath::Clamp(static_cast<int32>((*Region)[1]->AsNumber()), 0, Height);
			EndX = FMath::Clamp(static_cast<int32>((*Region)[2]->AsNumber()), BeginX, Width);
			EndY = FMath::Clamp(static_cast<int32>((*Region)[3]->AsNumber()), BeginY, Height);
		}

		if (BeginX < EndX && BeginY < EndY)
		{
			TArray<FColor> RegionPixels;
			RegionPixels.Reserve((EndX - BeginX) * (EndY - BeginY));
			for (int32 Y = BeginY; Y < EndY; Y++)
			{
				RegionPixels.Append(&Pixels[Y * Width + BeginX], EndX - BeginX);
			}

			const FString File = FString::Printf(TEXT("dumps/%s.png"), *FPaths::MakeValidFileName(DumpName));
			const FString Path = FPaths::Combine(Directory, File);
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(EndX - BeginX, EndY - BeginY,
				TArrayView64<const FColor>{ RegionPixels.GetData(), RegionPixels.Num() }, Png);
			const bool bSaved = FFileHelper::SaveArrayToFile(Png, *Path);

			Data.SetStringField(TEXT("dump_file"), File);
			Data.SetBoolField(TEXT("dump_saved"), bSaved);
			Data.SetArrayField(TEXT("dump_region"), {
				MakeShared<FJsonValueNumber>(BeginX), MakeShared<FJsonValueNumber>(BeginY),
				MakeShared<FJsonValueNumber>(EndX), MakeShared<FJsonValueNumber>(EndY) });
		}
	}

	MutableThis->AddEvent(TEXT("rt_hash_end"));

	Data.SetNumberField(TEXT("width"), Width);
	Data.SetNumberField(TEXT("height"), Height);
	Data.SetStringField(TEXT("hash"), FString::Printf(TEXT("%016llx"), Hash));
	Data.SetNumberField(TEXT("nonzero_pixels"), static_cast<double>(NonZero));
	Data.SetNumberField(TEXT("duration"), FPlatformTime::Seconds() - Begin);
	return true;
}


namespace
{
	bool ReadBox(const FJsonObject& Args, FBox& OutBox, FString& OutError)
	{
		FVector Min, Max;
		if (!ReadVector(Args, TEXT("box_min"), Min) || !ReadVector(Args, TEXT("box_max"), Max))
		{
			OutError = TEXT("box_min and box_max are required, [x, y, z] each");
			return false;
		}
		OutBox = FBox{ Min.ComponentMin(Max), Min.ComponentMax(Max) };
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> ToJson(const UFGInventoryComponent* Inventory)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (!Inventory)
		{
			return Values;
		}

		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks);
		for (const FInventoryStack& Stack : Stacks)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("item"), GetPathNameSafe(Stack.Item.GetItemClass()));
			Object->SetNumberField(TEXT("count"), Stack.NumItems);
			Values.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Values;
	}
}


bool FCartographBridge::SetUpMachines(UWorld& World, const FJsonObject& Args, FJsonObject& Data, FString& OutError)
{
	FString GroupName = TEXT("default");
	Args.TryGetStringField(TEXT("group"), GroupName);
	const TArray<FBridgeBuildable>* Group = Groups.Find(GroupName);
	if (!Group)
	{
		OutError = TEXT("The bridge hasn't built anything in that group");
		return false;
	}

	FString RecipePath;
	if (!Args.TryGetStringField(TEXT("recipe"), RecipePath))
	{
		OutError = TEXT("recipe is required, the one the machines are to produce with");
		return false;
	}
	const TSubclassOf<UFGRecipe> Recipe = LoadClass<UFGRecipe>(nullptr, *RecipePath);
	if (!Recipe)
	{
		OutError = FString::Printf(TEXT("Can't load the recipe %s"), *RecipePath);
		return false;
	}

	// How many times what the recipe takes is put into the input
	int32 InputBatches = 1;
	Args.TryGetNumberField(TEXT("input_batches"), InputBatches);
	bool bPaused = false;
	const bool bSetPaused = Args.TryGetBoolField(TEXT("paused"), bPaused);
	double Potential = 1;
	const bool bSetPotential = Args.TryGetNumberField(TEXT("potential"), Potential);

	const TArray<FItemAmount> Ingredients = UFGRecipe::GetIngredients(&World, Recipe);

	int32 SetUp = 0;
	int32 NotAMachine = 0;
	int32 ItemsAdded = 0;
	for (const FBridgeBuildable& Buildable : *Group)
	{
		AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(Buildable.Actor.Get());
		if (!Machine)
		{
			NotAMachine++;
			continue;
		}

		Machine->SetRecipe(Recipe);
		if (UFGInventoryComponent* Input = Machine->GetInputInventory())
		{
			for (const FItemAmount& Ingredient : Ingredients)
			{
				if (InputBatches > 0)
				{
					ItemsAdded += Input->AddStack(FInventoryStack{ Ingredient.Amount * InputBatches, Ingredient.ItemClass }, true);
				}
			}
		}
		if (bSetPaused)
		{
			Machine->SetIsProductionPaused(bPaused);
		}
		if (bSetPotential)
		{
			Machine->SetPendingPotential(static_cast<float>(Potential));
		}
		SetUp++;
	}

	Data.SetStringField(TEXT("group"), GroupName);
	Data.SetNumberField(TEXT("set_up"), SetUp);
	Data.SetNumberField(TEXT("not_a_machine"), NotAMachine);
	Data.SetNumberField(TEXT("items_added"), ItemsAdded);
	if (SetUp == 0)
	{
		OutError = TEXT("None of the group is a machine that takes a recipe");
		return false;
	}
	return true;
}


AFGBuildable* FCartographBridge::FindGroupMember(const FJsonObject& Args, const TCHAR* GroupField, const TCHAR* IndexField, FString& OutError)
{
	FString GroupName;
	int32 Index = 0;
	if (!Args.TryGetStringField(GroupField, GroupName))
	{
		OutError = FString::Printf(TEXT("%s is required"), GroupField);
		return nullptr;
	}
	Args.TryGetNumberField(IndexField, Index);
	const TArray<FBridgeBuildable>* Group = Groups.Find(GroupName);
	if (!Group || !Group->IsValidIndex(Index))
	{
		OutError = FString::Printf(TEXT("There is no building %d in the group %s"), Index, *GroupName);
		return nullptr;
	}
	AFGBuildable* Buildable = (*Group)[Index].Actor.Get();
	if (!Buildable)
	{
		OutError = FString::Printf(TEXT("Building %d of the group %s is gone"), Index, *GroupName);
	}
	return Buildable;
}


namespace
{
	// The first power connection of a building with room for one more wire
	UFGPowerConnectionComponent* FreePowerConnection(const AFGBuildable& Buildable)
	{
		TArray<UFGPowerConnectionComponent*> Connections;
		Buildable.GetComponents(Connections);
		for (UFGPowerConnectionComponent* Connection : Connections)
		{
			if (Connection && !Connection->IsHidden() && Connection->GetNumFreeConnections() > 0)
			{
				return Connection;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> DescribePower(const AFGBuildable& Buildable)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("class"), Buildable.GetClass()->GetName());
		TArray<UFGPowerConnectionComponent*> Connections;
		Buildable.GetComponents(Connections);
		TArray<TSharedPtr<FJsonValue>> Circuits;
		for (const UFGPowerConnectionComponent* Connection : Connections)
		{
			if (Connection)
			{
				Circuits.Add(MakeShared<FJsonValueNumber>(Connection->GetCircuitID()));
			}
		}
		Object->SetArrayField(TEXT("circuit_ids"), Circuits);
		if (const AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(&Buildable))
		{
			Object->SetBoolField(TEXT("has_power"), Factory->HasPower());
			Object->SetBoolField(TEXT("is_producing"), Factory->IsProducing());
		}
		if (const AFGBuildableGeneratorFuel* Generator = Cast<AFGBuildableGeneratorFuel>(&Buildable))
		{
			Object->SetBoolField(TEXT("has_fuel"), Generator->HasFuel());
			Object->SetNumberField(TEXT("load_percentage"), Generator->GetLoadPercentage());
		}
		return Object;
	}
}


bool FCartographBridge::RunPowerCommand(UWorld& World, const FString& Name, const FJsonObject& Args, FJsonObject& Data, FString& OutError)
{
	if (Name == TEXT("power_link"))
	{
		// A power line between two buildings the bridge has built, the way a player's is connected:
		// the wire itself connects its ends, nothing is powered in any other way.
		AFGBuildable* First = FindGroupMember(Args, TEXT("from_group"), TEXT("from_index"), OutError);
		AFGBuildable* Second = First ? FindGroupMember(Args, TEXT("to_group"), TEXT("to_index"), OutError) : nullptr;
		if (!First || !Second)
		{
			return false;
		}
		UFGPowerConnectionComponent* FirstConnection = FreePowerConnection(*First);
		UFGPowerConnectionComponent* SecondConnection = FreePowerConnection(*Second);
		if (!FirstConnection || !SecondConnection)
		{
			OutError = FString::Printf(TEXT("No free power connection on the %s"), FirstConnection ? TEXT("second") : TEXT("first"));
			return false;
		}

		FString RecipePath = TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_PowerLine.Recipe_PowerLine_C");
		Args.TryGetStringField(TEXT("wire_recipe"), RecipePath);
		const TSubclassOf<UFGRecipe> Recipe = LoadClass<UFGRecipe>(nullptr, *RecipePath);
		const TArray<FItemAmount> Products = Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>{};
		const TSubclassOf<UFGBuildingDescriptor> Descriptor = Products.IsEmpty() ? nullptr : TSubclassOf<UFGBuildingDescriptor>{ Products[0].ItemClass };
		const TSubclassOf<AFGBuildable> WireClass = Descriptor ? UFGBuildingDescriptor::GetBuildableClass(Descriptor) : nullptr;
		if (!WireClass || !WireClass->IsChildOf<AFGBuildableWire>())
		{
			OutError = FString::Printf(TEXT("%s doesn't make a wire"), *RecipePath);
			return false;
		}

		const FVector Start = FirstConnection->GetComponentLocation();
		const FVector End = SecondConnection->GetComponentLocation();
		const double Length = FVector::Distance(Start, End);
		const float MaxLength = GetDefault<AFGBuildableWire>(WireClass)->mMaxLength;
		Data.SetNumberField(TEXT("length"), Length);
		if (Length > MaxLength)
		{
			OutError = FString::Printf(TEXT("The wire would be %.0f long, at most %.0f"), Length, MaxLength);
			return false;
		}

		AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(&World);
		if (!BuildableSubsystem)
		{
			OutError = TEXT("There is no buildable subsystem");
			return false;
		}
		const FTransform Transform{ FQuat::Identity, (Start + End) / 2 };
		AFGBuildableWire* Wire = Cast<AFGBuildableWire>(BuildableSubsystem->BeginSpawnBuildable(WireClass, Transform));
		if (!Wire)
		{
			OutError = TEXT("Couldn't spawn the wire");
			return false;
		}
		Wire->SetBuiltWithRecipe(Recipe);
		// Before it's finished, like a hologram configures what it builds
		const bool bConnected = Wire->Connect(FirstConnection, SecondConnection);
		Wire->FinishSpawning(Transform);

		FString WireGroup = TEXT("wires");
		Args.TryGetStringField(TEXT("wire_group"), WireGroup);
		Groups.FindOrAdd(WireGroup).Add(FBridgeBuildable{ WireClass, Transform, Wire });

		// What the wire itself says its ends are. No other way of connecting is tried when it didn't:
		// the wire stays, in its group, to be looked at and taken down.
		const bool bEndsMatch = Wire->GetConnection(0) == FirstConnection && Wire->GetConnection(1) == SecondConnection;
		Data.SetBoolField(TEXT("connected"), bConnected);
		Data.SetBoolField(TEXT("ends_match"), bEndsMatch);
		Data.SetStringField(TEXT("wire_group"), WireGroup);
		Data.SetNumberField(TEXT("wire_index"), Groups[WireGroup].Num() - 1);
		// A circuit may only be put together on a later tick, power_state tells then
		Data.SetNumberField(TEXT("from_circuit_id"), FirstConnection->GetCircuitID());
		Data.SetNumberField(TEXT("to_circuit_id"), SecondConnection->GetCircuitID());
		if (!bConnected || !bEndsMatch)
		{
			OutError = TEXT("The wire was built but isn't connected to both buildings");
			return false;
		}
		return true;
	}

	if (Name == TEXT("fuel"))
	{
		// Into the fuel slots of the generators of a group, only what they take as fuel
		FString GroupName, ItemPath;
		int32 Count = 0;
		if (!Args.TryGetStringField(TEXT("group"), GroupName) || !Args.TryGetStringField(TEXT("item"), ItemPath)
			|| !Args.TryGetNumberField(TEXT("count"), Count) || Count <= 0 || Count > 10000)
		{
			OutError = TEXT("group, item and count (1 to 10000 per generator) are required");
			return false;
		}
		const TArray<FBridgeBuildable>* Group = Groups.Find(GroupName);
		const TSubclassOf<UFGItemDescriptor> ItemClass = LoadClass<UFGItemDescriptor>(nullptr, *ItemPath);
		if (!Group || !ItemClass)
		{
			OutError = Group ? FString::Printf(TEXT("Can't load the item %s"), *ItemPath) : TEXT("The bridge hasn't built anything in that group");
			return false;
		}
		int32 Fuelled = 0, NotAGenerator = 0, NotItsFuel = 0, ItemsAdded = 0, ItemsLeftOver = 0;
		for (const FBridgeBuildable& Member : *Group)
		{
			AFGBuildableGeneratorFuel* Generator = Cast<AFGBuildableGeneratorFuel>(Member.Actor.Get());
			if (!Generator)
			{
				NotAGenerator++;
				continue;
			}
			UFGInventoryComponent* Inventory = Generator->GetFuelInventory();
			if (!Inventory || !Generator->IsValidFuel(ItemClass))
			{
				NotItsFuel++;
				continue;
			}
			const int32 Added = Inventory->AddStack(FInventoryStack{ Count, ItemClass }, true);
			ItemsAdded += Added;
			ItemsLeftOver += Count - Added;
			Fuelled += Added > 0 ? 1 : 0;
		}
		Data.SetNumberField(TEXT("fuelled"), Fuelled);
		Data.SetNumberField(TEXT("not_a_generator"), NotAGenerator);
		Data.SetNumberField(TEXT("not_its_fuel"), NotItsFuel);
		Data.SetNumberField(TEXT("items_added"), ItemsAdded);
		Data.SetNumberField(TEXT("items_left_over"), ItemsLeftOver);
		if (Fuelled == 0)
		{
			OutError = TEXT("No generator of the group took the fuel");
			return false;
		}
		return true;
	}

	// power_state: read only. Groups only last as long as the world they were built in, after a load
	// what was built is found again by where it is (machines), not by group.
	FString GroupName;
	if (!Args.TryGetStringField(TEXT("group"), GroupName))
	{
		OutError = TEXT("group is required");
		return false;
	}
	const TArray<FBridgeBuildable>* Group = Groups.Find(GroupName);
	if (!Group)
	{
		OutError = TEXT("The bridge hasn't built anything in that group");
		return false;
	}
	constexpr int32 MaxListed = 500;
	TArray<TSharedPtr<FJsonValue>> Members;
	int32 Gone = 0;
	for (const FBridgeBuildable& Member : *Group)
	{
		const AFGBuildable* Buildable = Member.Actor.Get();
		if (!Buildable)
		{
			Gone++;
			continue;
		}
		if (Members.Num() < MaxListed)
		{
			Members.Add(MakeShared<FJsonValueObject>(DescribePower(*Buildable)));
		}
	}
	Data.SetArrayField(TEXT("buildings"), Members);
	Data.SetNumberField(TEXT("gone"), Gone);
	Data.SetBoolField(TEXT("truncated"), Group->Num() - Gone > MaxListed);
	return true;
}


bool FCartographBridge::ListMachines(UWorld& World, const FJsonObject& Args, FJsonObject& Data, FString& OutError)
{
	FBox Box;
	if (!ReadBox(Args, Box, OutError))
	{
		return false;
	}
	const AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(&World);
	if (!BuildableSubsystem)
	{
		OutError = TEXT("There is no buildable subsystem");
		return false;
	}

	constexpr int32 MaxListed = 500;
	int32 Count = 0;
	TArray<TSharedPtr<FJsonValue>> Machines;
	for (const AFGBuildable* Buildable : BuildableSubsystem->GetAllBuildablesRef())
	{
		const AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(Buildable);
		if (!Machine || !Box.IsInsideOrOn(Machine->GetActorLocation()))
		{
			continue;
		}
		if (++Count > MaxListed)
		{
			continue;
		}

		const FVector Location = Machine->GetActorLocation();
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("class"), Machine->GetClass()->GetPathName());
		Object->SetArrayField(TEXT("location"), {
			MakeShared<FJsonValueNumber>(Location.X), MakeShared<FJsonValueNumber>(Location.Y), MakeShared<FJsonValueNumber>(Location.Z) });
		if (const TSubclassOf<UFGRecipe> Recipe = Machine->GetCurrentRecipe())
		{
			Object->SetStringField(TEXT("recipe"), Recipe->GetPathName());
		}
		else
		{
			Object->SetField(TEXT("recipe"), NullValue());
		}
		Object->SetBoolField(TEXT("paused"), Machine->IsProductionPaused());
		Object->SetNumberField(TEXT("potential"), Machine->GetCurrentPotential());
		Object->SetNumberField(TEXT("pending_potential"), Machine->GetPendingPotential());
		Object->SetNumberField(TEXT("production_progress"), Machine->GetProductionProgress());
		Object->SetArrayField(TEXT("input"), ToJson(Machine->GetInputInventory()));
		Object->SetArrayField(TEXT("output"), ToJson(Machine->GetOutputInventory()));
		Machines.Add(MakeShared<FJsonValueObject>(Object));
	}

	// In the order of where they are, the one they're kept in isn't the same after a load
	Machines.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TArray<TSharedPtr<FJsonValue>>& LocationA = A->AsObject()->GetArrayField(TEXT("location"));
			const TArray<TSharedPtr<FJsonValue>>& LocationB = B->AsObject()->GetArrayField(TEXT("location"));
			for (int32 i = 0; i < 3; i++)
			{
				const double ValueA = LocationA[i]->AsNumber();
				const double ValueB = LocationB[i]->AsNumber();
				if (ValueA != ValueB)
				{
					return ValueA < ValueB;
				}
			}
			return false;
		});

	Data.SetNumberField(TEXT("count"), Count);
	Data.SetBoolField(TEXT("truncated"), Count > MaxListed);
	Data.SetArrayField(TEXT("machines"), Machines);
	return true;
}


bool FCartographBridge::AdoptBuildables(UWorld& World, const FJsonObject& Args, FJsonObject& Data, FString& OutError)
{
	// What the bridge has built is forgotten when the world goes, a save that was made with it still there
	// has it back after a load. This takes what's in the box for the bridge's, to be dismantled by it.
	// Meant for the box that the bridge builds in, it doesn't ask what it's taking.
	FBox Box;
	if (!ReadBox(Args, Box, OutError))
	{
		return false;
	}
	FString GroupName = TEXT("default");
	Args.TryGetStringField(TEXT("group"), GroupName);

	AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(&World);
	const AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(&World);
	if (!BuildableSubsystem)
	{
		OutError = TEXT("There is no buildable subsystem");
		return false;
	}

	TArray<FBridgeBuildable>& Group = Groups.FindOrAdd(GroupName);
	TSet<FIntVector> Known;
	for (const FBridgeBuildable& Buildable : Group)
	{
		Known.Add(Quantize(Buildable.Transform.GetLocation()));
	}

	int32 Actors = 0;
	for (AFGBuildable* Buildable : BuildableSubsystem->GetAllBuildablesRef())
	{
		if (IsValid(Buildable) && Box.IsInsideOrOn(Buildable->GetActorLocation())
			&& !Known.Contains(Quantize(Buildable->GetActorLocation())))
		{
			Known.Add(Quantize(Buildable->GetActorLocation()));
			Group.Add({ Buildable->GetClass(), Buildable->GetActorTransform(), Buildable });
			Actors++;
		}
	}

	int32 Lightweights = 0;
	if (LightweightSubsystem)
	{
		for (const auto& [BuildableClass, Instances] : LightweightSubsystem->GetAllLightweightBuildableInstances())
		{
			for (const FRuntimeBuildableInstanceData& Instance : Instances)
			{
				if (Instance.IsValid() && Box.IsInsideOrOn(Instance.Transform.GetLocation())
					&& !Known.Contains(Quantize(Instance.Transform.GetLocation())))
				{
					Known.Add(Quantize(Instance.Transform.GetLocation()));
					Group.Add({ BuildableClass, Instance.Transform, nullptr });
					Lightweights++;
				}
			}
		}
	}

	Data.SetStringField(TEXT("group"), GroupName);
	Data.SetNumberField(TEXT("adopted_actors"), Actors);
	Data.SetNumberField(TEXT("adopted_lightweights"), Lightweights);
	Data.SetNumberField(TEXT("group_size"), Group.Num());
	return true;
}


bool FCartographBridge::ProbeCanvas(UWorld& World, FJsonObject& Data, FString& OutError)
{
	// Drawing to a render target goes through a canvas that the world has one of, for everything that does so.
	// Cartograph keeps a draw open over many frames, so something else drawing in between takes it from under it.
	// This does such a draw, to see whether that's the case, and puts things back as they were.
	UCanvas* SharedCanvas = World.GetCanvasForRenderingToTarget();
	if (!SharedCanvas)
	{
		OutError = TEXT("The world has no canvas for rendering to a target");
		return false;
	}

	const UCartographGameInstanceModule* Cartograph = UCartographGameInstanceModule::Instance;
	const FCartographDebugState State = Cartograph ? Cartograph->GetDebugState() : FCartographDebugState{};

	FCanvas* const CanvasBefore = SharedCanvas->Canvas;
	const int32 SizeXBefore = SharedCanvas->SizeX;
	const int32 SizeYBefore = SharedCanvas->SizeY;

	UTextureRenderTarget2D* Scratch = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Scratch->RenderTargetFormat = RTF_RGBA8;
	Scratch->InitAutoFormat(64, 64);
	Scratch->UpdateResourceImmediate(true);

	AddEvent(TEXT("canvas_probe_begin"));

	UCanvas* ProbeCanvas = nullptr;
	FVector2D Size;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(&World, Scratch, ProbeCanvas, Size, Context);
	FCanvas* const CanvasDuring = SharedCanvas->Canvas;
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(&World, Context);
	FCanvas* const CanvasAfter = SharedCanvas->Canvas;

	const bool bWasInUse = CanvasBefore != nullptr;
	const bool bWasTakenOver = bWasInUse && CanvasDuring != CanvasBefore;
	bool bRestored = false;
	if (bWasInUse && CanvasAfter != CanvasBefore)
	{
		SharedCanvas->Init(SizeXBefore, SizeYBefore, nullptr, CanvasBefore);
		bRestored = true;
	}

	AddEvent(TEXT("canvas_probe_end"));
	Scratch->ReleaseResource();

	Data.SetBoolField(TEXT("shared_canvas_was_in_use"), bWasInUse);
	Data.SetBoolField(TEXT("same_canvas_object_as_probe"), ProbeCanvas == SharedCanvas);
	Data.SetBoolField(TEXT("was_taken_over"), bWasTakenOver);
	Data.SetBoolField(TEXT("was_left_null"), bWasInUse && CanvasAfter == nullptr);
	Data.SetBoolField(TEXT("restored"), bRestored);
	Data.SetBoolField(TEXT("cartograph_redraw_active"), State.bIsRedrawActive);
	Data.SetBoolField(TEXT("cartograph_redrawing_entirely"), State.bIsRedrawingEntirely);
	Data.SetNumberField(TEXT("shared_canvas_size_x_before"), SizeXBefore);
	Data.SetNumberField(TEXT("shared_canvas_size_y_before"), SizeYBefore);
	return true;
}


void FCartographBridge::StopSampler(const FString& Name, FJsonObject& Data)
{
	FBridgeSampler Sampler;
	Samplers.RemoveAndCopyValue(Name, Sampler);

	TArray<double> Deltas;
	Deltas.Reserve(Sampler.Rows.Num());
	int32 Over33 = 0, Over50 = 0, Over100 = 0, Over250 = 0;
	for (const FBridgeSampleRow& Row : Sampler.Rows)
	{
		Deltas.Add(Row.DeltaMs);
		Over33 += Row.DeltaMs > 33 ? 1 : 0;
		Over50 += Row.DeltaMs > 50 ? 1 : 0;
		Over100 += Row.DeltaMs > 100 ? 1 : 0;
		Over250 += Row.DeltaMs > 250 ? 1 : 0;
	}
	Deltas.Sort();

	Data.SetStringField(TEXT("name"), Name);
	Data.SetNumberField(TEXT("frames"), Sampler.Rows.Num());
	Data.SetNumberField(TEXT("duration"), Now() - Sampler.StartTime);
	Data.SetNumberField(TEXT("p50_ms"), Percentile(Deltas, 0.50));
	Data.SetNumberField(TEXT("p95_ms"), Percentile(Deltas, 0.95));
	Data.SetNumberField(TEXT("p99_ms"), Percentile(Deltas, 0.99));
	Data.SetNumberField(TEXT("max_ms"), Deltas.IsEmpty() ? 0 : Deltas.Last());
	Data.SetNumberField(TEXT("over_33_ms"), Over33);
	Data.SetNumberField(TEXT("over_50_ms"), Over50);
	Data.SetNumberField(TEXT("over_100_ms"), Over100);
	Data.SetNumberField(TEXT("over_250_ms"), Over250);

	const FString File = FString::Printf(TEXT("samples/%s.csv"), *FPaths::MakeValidFileName(Name));
	Data.SetStringField(TEXT("file"), File);

	TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
	Detail->SetStringField(TEXT("name"), Name);
	AddEvent(TEXT("sample_stop"), Detail);

	// Put together off the game thread as well, it's a line per frame
	Writer.Overwrite(FPaths::Combine(Directory, File), [Rows = MoveTemp(Sampler.Rows)]
		{
			// The thread times are only there when the engine keeps them, which shows as them never being anything
			const bool bHasThreadTimes = Rows.ContainsByPredicate(
				[](const FBridgeSampleRow& Row) { return Row.GameThreadMs > 0 || Row.RenderThreadMs > 0; });

			FString Csv = TEXT("frame,t,delta_ms,game_thread_ms,render_thread_ms,is_initializing,is_redraw_active,")
				TEXT("is_redrawing_entirely,is_pending_redraw,is_pending_redraw_entire,pending_add_count,pending_remove_count\n");
			Csv.Reserve(Rows.Num() * 96);
			for (const FBridgeSampleRow& Row : Rows)
			{
				if (bHasThreadTimes)
				{
					Csv += FString::Printf(TEXT("%llu,%.6f,%.4f,%.4f,%.4f"),
						Row.Frame, Row.Time, Row.DeltaMs, Row.GameThreadMs, Row.RenderThreadMs);
				}
				else
				{
					Csv += FString::Printf(TEXT("%llu,%.6f,%.4f,,"), Row.Frame, Row.Time, Row.DeltaMs);
				}

				if (Row.bHasCartographState)
				{
					Csv += FString::Printf(TEXT(",%d,%d,%d,%d,%d,%d,%d\n"),
						Row.bIsInitializing, Row.bIsRedrawActive, Row.bIsRedrawingEntirely,
						Row.bIsPendingRedraw, Row.bIsPendingRedrawEntire, Row.PendingAddCount, Row.PendingRemoveCount);
				}
				else
				{
					Csv += TEXT(",,,,,,,\n");
				}
			}
			return Csv;
		});
}
#pragma endregion
