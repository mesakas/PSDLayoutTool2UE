#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "PSDLayoutTool2UEFactory.generated.h"

UCLASS()
class PSDLAYOUTTOOL2UE_API UPSDLayoutTool2UEFactory : public UFactory
{
	GENERATED_BODY()

public:
	UPSDLayoutTool2UEFactory(const FObjectInitializer& ObjectInitializer);

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual FText GetDisplayName() const override;
	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;
};
