#pragma once

#include "CoreMinimal.h"

class UWidgetBlueprint;

struct FPSDWidgetImportOptions
{
	bool bClipLayersToCanvas = false;
};

struct FPSDWidgetImportResult
{
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	TArray<UObject*> AdditionalAssets;
};

class FPSDWidgetBuilder
{
public:
	static bool ImportPSDAsWidget(
		const FString& Filename,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		FPSDWidgetImportResult& OutResult,
		FText& OutError);

	static bool ImportPSDAsWidget(
		const FString& Filename,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FPSDWidgetImportOptions& ImportOptions,
		FPSDWidgetImportResult& OutResult,
		FText& OutError);
};
