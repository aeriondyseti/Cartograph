// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using UnrealBuildTool;
using Microsoft.Extensions.Logging;

// Target preset class shared by both game and server configurations. Editor still has it's own configuration not derived from this class.
public class FactorySharedTarget : TargetRules
{
	/** Allows overriding whenever the checks in the shipping builds should be used from the commandline */
	[CommandLine("-UseChecksInShipping")]
	public bool UseChecksInShippingOverride = true; // MODDING EDIT: we want checks in shipping by default

	/** Allows overriding the build type from the command line */
	[CommandLine("-Monolithic", Value = "Monolithic")] 
	[CommandLine("-Modular", Value = "Modular")]
	public TargetLinkType LinkTypeOverride = TargetLinkType.Default;

	[CommandLine("-WithPSA")]
	public bool WithPSA = false;

	public FactorySharedTarget(TargetInfo Target) : base(Target)
	{
		bIsPublicBuild = true;
		//DefaultWarningLevel = WarningLevel.Error;
		//bWarningsAsErrors = true;
		//CppCompileWarningSettings.DeprecationWarningLevel = WarningLevel.Error;
		
		// Marked this as game because the project file generator was trying to initialize this class and failing because it was abstract. What a not very smart tool.
		Type = UnrealBuildTool.TargetType.Game;
		
		// Default to shared build environment and modular build on desktop platforms
		var defaultLinkTypeForPlatform = Target.Platform.IsInGroup(UnrealPlatformGroup.Desktop) ? TargetLinkType.Modular : TargetLinkType.Monolithic;

		BuildEnvironment = TargetBuildEnvironment.Shared;
		LinkType = LinkTypeOverride != TargetLinkType.Default ? LinkTypeOverride : defaultLinkTypeForPlatform;

		// Shared build settings
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		bOverrideAppNameForSharedBuild = true;
		bOverrideBuildEnvironment = true;
		bUseLoggingInShipping = true;
		if (/*Target.Platform == UnrealTargetPlatform.PS5 || Target.Platform == UnrealTargetPlatform.XSX*/ false)
		{
			bUseConsoleInShipping = false;
		}
		else
		{
			bUseConsoleInShipping = true;
		}
		bAllowGeneratedIniWhenCooked = true;
		// Enable diagnostics when disabled plugins are being pulled back into the build as dependencies for other plugins
		CppCompileWarningSettings.DisablePluginsConflictWarningLevel = WarningLevel.Warning;

		// [KapaevV:29/07/2026] Force /Z7 (debug info in the .obj) instead of /Zi (separate PDB per file).
		// UBA cannot remote mspdbsrv - UbaDetours.Build.cs hardcodes supportMspdbSrv = false - so any
		// /Zi compile that the scheduler sends to a helper dies with "C1356: unable to find mspdbcore.dll".
		// UBA's own workaround (patching /FS -> /Z7 in UbaProcess.cpp) cannot fire, because cl.exe args
		// live in a response file and non-PCH compiles never get /FS in the first place.
		// These are assigned here rather than left to default so that an agent-local BuildConfiguration.xml
		// cannot re-enable them: the base TargetRules ctor applies XML config and command-line args before
		// this ctor body runs, so this assignment always wins. The linker still emits the shippable PDB.
		bUsePDBFiles = false;
		bSupportEditAndContinue = false;

		// [ZolotukhinN:10/07/2023] Enabled Network Push Model support in normal game builds, it's disabled in non-editor by default
		bWithPushModel = true;
		// [ZolotukhinN:04/05/2023] Allow cheat manager initialization in Shipping builds to allow cheating even in shipping builds when compiling with WITH_CHEATS=1
		if (true /*Target.Platform != UnrealTargetPlatform.PS5 && Target.Platform != UnrealTargetPlatform.XSX*/)
		{
			GlobalDefinitions.Add("UE_WITH_CHEAT_MANAGER=1");
		}
		// Allow checks in shipping depending on the command line configuration
		bUseChecksInShipping = UseChecksInShippingOverride;
		DefaultBuildSettings = BuildSettingsVersion.Latest;

		// Common module names for the game targets
		ExtraModuleNames.AddRange(new[] {
			"FactoryGame"
		});
		if (UseChecksInShippingOverride || LinkTypeOverride != TargetLinkType.Default)
		{
			Logger.LogInformation($"{Target.Name}: Building with command line overrides: ChecksInShipping: {bUseChecksInShipping} (default: false); LinkType: {LinkType} (default: {defaultLinkTypeForPlatform})");
		}

		// if (bBuildEditor || WithPSA)
		// {
		// 	EnablePlugins.Add("PSA");
		// }
		// else
		// {
		// 	DisablePlugins.Add("PSA");
		// }
	}
}
