#pragma once

#include "CoreMinimal.h"
#include "MCPHandlerRegistration.h"

namespace VoxelPluginTools
{
	struct FHandlerEntry
	{
		FString Name;
		UEMCP::FExternalHandlerFn Fn;
	};

	const TArray<FHandlerEntry>& GetHandlers();
}
