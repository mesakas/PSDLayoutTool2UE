#pragma once

#include "Modules/ModuleManager.h"

class FPSDLayoutTool2UEModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void ExecuteImportPSDAsWidget();
	void DisableInterchangePSDImport();
	void RestoreInterchangePSDImport();

	bool bModifiedInterchangePSDImport = false;
	int32 PreviousInterchangePSDImportValue = 1;
};
