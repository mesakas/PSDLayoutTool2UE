#include "PSDLayoutTool2UEFactory.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "PSDLayoutTool2UEFactory"

UPSDLayoutTool2UEFactory::UPSDLayoutTool2UEFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bCreateNew = false;
	bEditorImport = true;
	bEditAfterNew = true;
	SupportedClass = UWidgetBlueprint::StaticClass();
	Formats.Add(TEXT("psd;PSD Layout Widget Blueprint"));

	// Prefer the layout importer over UE's default flattened PSD texture importer while this plugin is enabled.
	ImportPriority = UFactory::GetDefaultImportPriority() + 1;
}

bool UPSDLayoutTool2UEFactory::FactoryCanImport(const FString& Filename)
{
	return FPaths::GetExtension(Filename).Equals(TEXT("psd"), ESearchCase::IgnoreCase);
}

FText UPSDLayoutTool2UEFactory::GetDisplayName() const
{
	return LOCTEXT("FactoryDisplayName", "PSD Layout Widget Blueprint");
}

bool UPSDLayoutTool2UEFactory::ConfigureProperties()
{
	FPSDWidgetImportOptions ImportOptions;
	ImportOptions.bClipLayersToCanvas = bClipLayersToCanvas;
	if (!ConfigureImportOptions(ImportOptions))
	{
		return false;
	}

	bClipLayersToCanvas = ImportOptions.bClipLayersToCanvas;
	return true;
}

bool UPSDLayoutTool2UEFactory::ConfigureImportOptions(FPSDWidgetImportOptions& InOutOptions)
{
	if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return true;
	}

	bool bAccepted = false;
	TSharedPtr<SWindow> OptionsWindow;

	OptionsWindow = SNew(SWindow)
		.Title(LOCTEXT("ImportOptionsTitle", "PSD Layout Import Options"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		[
			SNew(SBorder)
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ImportOptionsDescription", "Choose how PSD layers are converted into the generated Widget Blueprint."))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked(InOutOptions.bClipLayersToCanvas ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged_Lambda([&InOutOptions](ECheckBoxState NewState)
					{
						InOutOptions.bClipLayersToCanvas = NewState == ECheckBoxState::Checked;
					})
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ClipLayersToCanvasLabel", "Clip each layer to the PSD canvas"))
						.ToolTipText(LOCTEXT("ClipLayersToCanvasTooltip", "When enabled, pixels and widget layout outside the PSD document canvas are discarded during import."))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 16.0f, 0.0f, 0.0f)
				[
					SNew(SUniformGridPanel)
					.SlotPadding(FMargin(4.0f, 0.0f))
					+ SUniformGridPanel::Slot(0, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("ImportButton", "Import"))
						.OnClicked_Lambda([&bAccepted, &OptionsWindow]()
						{
							bAccepted = true;
							if (OptionsWindow.IsValid())
							{
								OptionsWindow->RequestDestroyWindow();
							}
							return FReply::Handled();
						})
					]
					+ SUniformGridPanel::Slot(1, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("CancelButton", "Cancel"))
						.OnClicked_Lambda([&OptionsWindow]()
						{
							if (OptionsWindow.IsValid())
							{
								OptionsWindow->RequestDestroyWindow();
							}
							return FReply::Handled();
						})
					]
				]
			]
		];

	FSlateApplication::Get().AddModalWindow(OptionsWindow.ToSharedRef(), FSlateApplication::Get().FindBestParentWindowForDialogs(TSharedPtr<SWidget>()));
	return bAccepted;
}

UObject* UPSDLayoutTool2UEFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;

	FPSDWidgetImportOptions ImportOptions;
	ImportOptions.bClipLayersToCanvas = bClipLayersToCanvas;

	FPSDWidgetImportResult Result;
	FText Error;
	if (!FPSDWidgetBuilder::ImportPSDAsWidget(Filename, InParent, InName, Flags, ImportOptions, Result, Error))
	{
		if (Warn)
		{
			Warn->Log(ELogVerbosity::Error, Error.ToString());
		}

		return nullptr;
	}

	AdditionalImportedObjects.Append(Result.AdditionalAssets);
	return Result.WidgetBlueprint;
}

#undef LOCTEXT_NAMESPACE
