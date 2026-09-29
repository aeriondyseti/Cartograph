#include "CartographBridge.h"

#include "Async/Async.h"
#include "Buildables/FGBuildable.h"
#include "CartographGameInstanceModule.h"
#include "CartographTestBridge.h"
#include "Engine/Engine.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/World.h"
#include "FGBlueprintFunctionLibrary.h"
#include "FGBuildableSubsystem.h"
#include "FGDismantleInterface.h"
#include "FGGameMode.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGPlayerController.h"
#include "FGRecipe.h"
#include "FGSaveSession.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/EngineVersion.h"
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
	constexpr const TCHAR* BridgeVersion = TEXT("0.1.0");
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
			|| Command == TEXT("load_save") || Command == TEXT("exit_to_menu");
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
		FScopeLock Lock{ &PendingMutex };
		Pending.Add({ Path, MoveTemp(Text), true });
	}
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this] { WritePending(); });
}


void FBridgeFileWriter::Overwrite(const FString& Path, FString&& Text)
{
	{
		FScopeLock Lock{ &PendingMutex };
		Pending.Add({ Path, MoveTemp(Text), false });
	}
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this] { WritePending(); });
}


void FBridgeFileWriter::Flush()
{
	WritePending();
}


void FBridgeFileWriter::WritePending()
{
	// Taken before the pending ones are, that's what keeps the order
	FScopeLock WriteLock{ &WriteMutex };

	TArray<FPendingWrite> ToWrite;
	{
		FScopeLock Lock{ &PendingMutex };
		ToWrite = MoveTemp(Pending);
		Pending.Reset();
	}

	for (const FPendingWrite& Write : ToWrite)
	{
		FFileHelper::SaveStringToFile(Write.Text, *Write.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), Write.bAppend ? FILEWRITE_Append : FILEWRITE_None);
	}
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
		const FBridgeSampleRow Row{
			GFrameCounter, Time - StartTime, DeltaMs,
			FPlatformTime::ToMilliseconds64(GGameThreadTime), FPlatformTime::ToMilliseconds64(GRenderThreadTime)
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
			if (ChangesTheWorld(Command.Name))
			{
				Writer.Flush();
			}

			Status = Start(Command, Error);
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
		return HashRenderTarget(Data, OutError) ? ECommandStatus::Succeeded : ECommandStatus::Failed;
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

	if (Name == TEXT("save"))
	{
		if (CallbackResult.IsSet())
		{
			AddEvent(TEXT("save_end"));
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
		if (World && World->HasBegunPlay() && GameMode && !GameMode->IsMainMenuGameMode()
			&& PlayerController && PlayerController->GetPawn())
		{
			Command.Data->SetStringField(TEXT("world"), World->GetName());
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

	FVector Origin = FVector::ZeroVector;
	if (!ReadVector(Args, TEXT("origin"), Origin))
	{
		const APlayerController* PlayerController = World->GetFirstPlayerController();
		const APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		if (!Pawn)
		{
			OutError = TEXT("There is no player to build at, give an origin");
			return ECommandStatus::Failed;
		}
		Origin = Pawn->GetActorLocation();
	}
	FVector Offset = FVector::ZeroVector;
	ReadVector(Args, TEXT("offset"), Offset);
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
		Command.Data->SetStringField(TEXT("origin"), (Origin + Offset).ToString());
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
		const FVector Location = Origin + Offset + FVector{ (Slot % Columns) * Spacing, (Slot / Columns) * Spacing, 0.0 };
		const FTransform Transform{ FQuat::Identity, Location };

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


bool FCartographBridge::HashRenderTarget(FJsonObject& Data, FString& OutError) const
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

	MutableThis->AddEvent(TEXT("rt_hash_end"));

	if (!bRead)
	{
		OutError = TEXT("Couldn't read the render target");
		return false;
	}
	Data.SetNumberField(TEXT("width"), RenderTarget->SizeX);
	Data.SetNumberField(TEXT("height"), RenderTarget->SizeY);
	Data.SetStringField(TEXT("hash"), FString::Printf(TEXT("%016llx"), Hash));
	Data.SetNumberField(TEXT("nonzero_pixels"), static_cast<double>(NonZero));
	Data.SetNumberField(TEXT("duration"), FPlatformTime::Seconds() - Begin);
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
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, Path = FPaths::Combine(Directory, File), Rows = MoveTemp(Sampler.Rows)]() mutable
		{
			FString Csv = TEXT("frame,t,delta_ms,game_thread_ms,render_thread_ms\n");
			Csv.Reserve(Rows.Num() * 64);
			for (const FBridgeSampleRow& Row : Rows)
			{
				Csv += FString::Printf(TEXT("%llu,%.6f,%.4f,%.4f,%.4f\n"),
					Row.Frame, Row.Time, Row.DeltaMs, Row.GameThreadMs, Row.RenderThreadMs);
			}
			Writer.Overwrite(Path, MoveTemp(Csv));
		});
}
#pragma endregion
