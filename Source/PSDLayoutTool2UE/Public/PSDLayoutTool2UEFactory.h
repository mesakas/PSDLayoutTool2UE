#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "PSDWidgetBuilder.h"
#include "PSDLayoutTool2UEFactory.generated.h"

UCLASS()
class PSDLAYOUTTOOL2UE_API UPSDLayoutTool2UEFactory : public UFactory
{
	GENERATED_BODY()

public:
	UPSDLayoutTool2UEFactory(const FObjectInitializer& ObjectInitializer);

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual FText GetDisplayName() const override;
	virtual bool ConfigureProperties() override;
	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;

	static bool ConfigureImportOptions(FPSDWidgetImportOptions& InOutOptions);

	UPROPERTY(EditAnywhere, Category = "PSD Layout")
	bool bClipLayersToCanvas = false;

	UPROPERTY(EditAnywhere, Category = "PSD Layout")
	bool bImportAssetsOnly = false;
};
