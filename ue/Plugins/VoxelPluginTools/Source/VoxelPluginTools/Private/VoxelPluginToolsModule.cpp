#include "Modules/ModuleManager.h"
#include "MCPHandlerRegistration.h"
#include "VoxelGraphHandlers.h"

class FVoxelPluginToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		for (const VoxelPluginTools::FHandlerEntry& Entry : VoxelPluginTools::GetHandlers())
		{
			UEMCP::RegisterExternalHandler(Entry.Name, Entry.Fn);
		}
	}

	virtual void ShutdownModule() override
	{
		for (const VoxelPluginTools::FHandlerEntry& Entry : VoxelPluginTools::GetHandlers())
		{
			UEMCP::UnregisterExternalHandler(Entry.Name);
		}
	}
};

IMPLEMENT_MODULE(FVoxelPluginToolsModule, VoxelPluginTools)
