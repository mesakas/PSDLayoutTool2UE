#include "PSDLayoutTool2UEModule.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "IContentBrowserSingleton.h"
#include "IDesktopPlatform.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PSDLayoutTool2UEFactory.h"
#include "PSDWidgetBuilder.h"
#include "ToolMenus.h"
#include "Modules/ModuleManager.h"
#include "WidgetBlueprint.h"

IMPLEMENT_MODULE(FPSDLayoutTool2UEModule, PSDLayoutTool2UE)

#define LOCTEXT_NAMESPACE "FPSDLayoutTool2UEModule"

DEFINE_LOG_CATEGORY_STATIC(LogPSDLayoutTool2UE, Log, All);

namespace
{
constexpr const TCHAR* InterchangePSDImportCVarName = TEXT("Interchange.FeatureFlags.Import.PSD");
}

namespace
{
FString GetCurrentContentBrowserPath()
{
	FString DestinationPath = TEXT("/Game");

	if (FModuleManager::Get().IsModuleLoaded("ContentBrowser"))
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		TArray<FString> SelectedFolders;
		ContentBrowserModule.Get().GetSelectedPathViewFolders(SelectedFolders);
		if (SelectedFolders.Num() > 0)
		{
			DestinationPath = SelectedFolders[0];
		}
	}

	if (DestinationPath.StartsWith(TEXT("/All/")))
	{
		DestinationPath.RightChopInline(4);
	}

	if (!DestinationPath.StartsWith(TEXT("/Game")))
	{
		DestinationPath = TEXT("/Game");
	}

	return DestinationPath;
}

void* GetParentWindowHandle()
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}

	IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
	const TSharedPtr<SWindow> ParentWindow = MainFrameModule.GetParentWindow();
	return ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid()
		? ParentWindow->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;
}
}

void FPSDLayoutTool2UEModule::StartupModule()
{
	DisableInterchangePSDImport();

	if (!IsRunningCommandlet())
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FPSDLayoutTool2UEModule::RegisterMenus));
	}
}

void FPSDLayoutTool2UEModule::ShutdownModule()
{
	RestoreInterchangePSDImport();

	if (UObjectInitialized())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
}

void FPSDLayoutTool2UEModule::DisableInterchangePSDImport()
{
	IConsoleVariable* InterchangePSDImportCVar = IConsoleManager::Get().FindConsoleVariable(InterchangePSDImportCVarName);
	if (!InterchangePSDImportCVar && FModuleManager::Get().ModuleExists(TEXT("InterchangeImport")))
	{
		FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("InterchangeImport"));
		InterchangePSDImportCVar = IConsoleManager::Get().FindConsoleVariable(InterchangePSDImportCVarName);
	}

	if (!InterchangePSDImportCVar)
	{
		UE_LOG(LogPSDLayoutTool2UE, Warning, TEXT("Could not find %s. PSD drag-and-drop may still be handled as a Texture2D by Interchange."), InterchangePSDImportCVarName);
		return;
	}

	if (!bModifiedInterchangePSDImport)
	{
		PreviousInterchangePSDImportValue = InterchangePSDImportCVar->GetInt();
		bModifiedInterchangePSDImport = true;
	}

	InterchangePSDImportCVar->Set(0, ECVF_SetByCode);
	UE_LOG(LogPSDLayoutTool2UE, Log, TEXT("Disabled %s so PSD files can be imported by PSDLayoutTool2UE."), InterchangePSDImportCVarName);
}

void FPSDLayoutTool2UEModule::RestoreInterchangePSDImport()
{
	if (!bModifiedInterchangePSDImport)
	{
		return;
	}

	if (IConsoleVariable* InterchangePSDImportCVar = IConsoleManager::Get().FindConsoleVariable(InterchangePSDImportCVarName))
	{
		InterchangePSDImportCVar->Set(PreviousInterchangePSDImportValue, ECVF_SetByCode);
	}

	bModifiedInterchangePSDImport = false;
}

void FPSDLayoutTool2UEModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	FToolMenuSection& Section = Menu->FindOrAddSection("PSDLayoutTool2UE", LOCTEXT("PSDLayoutTool2UESection", "PSD Layout Tool 2 UE"));
	Section.AddMenuEntry(
		"PSDLayoutTool2UE_ImportPSDAsWidget",
		LOCTEXT("ImportPSDAsWidgetLabel", "Import PSD as Widget"),
		LOCTEXT("ImportPSDAsWidgetTooltip", "Import a layered PSD file into the selected Content Browser folder as a UMG Widget Blueprint."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FPSDLayoutTool2UEModule::ExecuteImportPSDAsWidget)));
}

void FPSDLayoutTool2UEModule::ExecuteImportPSDAsWidget()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoDesktopPlatform", "Desktop platform services are unavailable."));
		return;
	}

	TArray<FString> SelectedFiles;
	const bool bOpened = DesktopPlatform->OpenFileDialog(
		GetParentWindowHandle(),
		LOCTEXT("ChoosePSDDialogTitle", "Choose PSD File").ToString(),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("Photoshop Documents (*.psd)|*.psd"),
		EFileDialogFlags::None,
		SelectedFiles);

	if (!bOpened || SelectedFiles.Num() == 0)
	{
		return;
	}

	const FString DestinationPath = GetCurrentContentBrowserPath();
	const FString BaseAssetName = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(SelectedFiles[0]));

	FPSDWidgetImportOptions ImportOptions;
	if (!UPSDLayoutTool2UEFactory::ConfigureImportOptions(ImportOptions))
	{
		return;
	}

	FString PackageName;
	FString AssetName;
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	AssetToolsModule.Get().CreateUniqueAssetName(DestinationPath / BaseAssetName, TEXT(""), PackageName, AssetName);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("CreatePackageFailed", "Failed to create package: {0}"), FText::FromString(PackageName)));
		return;
	}

	FPSDWidgetImportResult Result;
	FText Error;
	if (!FPSDWidgetBuilder::ImportPSDAsWidget(SelectedFiles[0], Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional, ImportOptions, Result, Error))
	{
		FMessageDialog::Open(EAppMsgType::Ok, Error);
		return;
	}

	if (Result.WidgetBlueprint && GEditor)
	{
		TArray<UObject*> ObjectsToSync;
		ObjectsToSync.Add(Result.WidgetBlueprint);
		GEditor->SyncBrowserToObjects(ObjectsToSync);
	}
}

#undef LOCTEXT_NAMESPACE
