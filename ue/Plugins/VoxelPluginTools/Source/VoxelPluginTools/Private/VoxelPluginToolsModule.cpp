#include "Modules/ModuleManager.h"
#include "MCPHandlerRegistration.h"
#include "FileHelpers.h"
#include "VoxelToolsCommon.h"

namespace
{
	// Content packages a call dirtied (directly or through Voxel side effects such as graph
	// migration or sculpting into a linked asset) are saved before replying, unless save:false.
	// Levels are never saved here; that stays the user's call, as in the editor.
	TSharedPtr<FJsonValue> RunHandler(const UEMCP::FExternalHandlerFn& Fn, const TSharedPtr<FJsonObject>& Params)
	{
		TArray<UPackage*> DirtyBefore;
		FEditorFileUtils::GetDirtyContentPackages(DirtyBefore);

		TSharedPtr<FJsonValue> Result = Fn(Params);

		bool bSave = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("save"), bSave);
		}
		TSharedPtr<FJsonObject> Object = Result.IsValid() && Result->Type == EJson::Object ? Result->AsObject() : nullptr;
		bool bSuccess = false;
		if (bSave && Object.IsValid() && Object->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess)
		{
			TArray<UPackage*> DirtyAfter;
			FEditorFileUtils::GetDirtyContentPackages(DirtyAfter);
			TArray<UPackage*> NewlyDirty;
			for (UPackage* Package : DirtyAfter)
			{
				if (!DirtyBefore.Contains(Package))
				{
					NewlyDirty.Add(Package);
				}
			}
			if (NewlyDirty.Num() > 0)
			{
				const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(NewlyDirty, true);
				TArray<TSharedPtr<FJsonValue>> Names;
				for (const UPackage* Package : NewlyDirty)
				{
					Names.Add(MakeShared<FJsonValueString>(Package->GetName()));
				}
				Object->SetArrayField(TEXT("autoSaved"), Names);
				if (!bSaved)
				{
					Object->SetStringField(TEXT("autoSaveWarning"), TEXT("Some packages this call dirtied could not be saved"));
				}
			}
		}
		return VoxelPluginTools::SanitizeJson(Result);
	}
}

class FVoxelPluginToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		for (const VoxelPluginTools::FHandlerEntry& Entry : VoxelPluginTools::GetHandlers())
		{
			UEMCP::RegisterExternalHandler(Entry.Name, [Fn = Entry.Fn](const TSharedPtr<FJsonObject>& Params)
			{
				return RunHandler(Fn, Params);
			});
		}
	}

	virtual void ShutdownModule() override
	{
		for (const VoxelPluginTools::FHandlerEntry& Entry : VoxelPluginTools::GetHandlers())
		{
			UEMCP::UnregisterExternalHandler(Entry.Name);
		}
		VoxelPluginTools::ReleaseCatalog();
	}
};

IMPLEMENT_MODULE(FVoxelPluginToolsModule, VoxelPluginTools)
