#include "PSDLayoutTool2UEFactory.h"

#include "PSDWidgetBuilder.h"
#include "Misc/Paths.h"
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

	FPSDWidgetImportResult Result;
	FText Error;
	if (!FPSDWidgetBuilder::ImportPSDAsWidget(Filename, InParent, InName, Flags, Result, Error))
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
