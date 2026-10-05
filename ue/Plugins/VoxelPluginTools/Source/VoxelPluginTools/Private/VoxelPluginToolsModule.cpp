#include "Modules/ModuleManager.h"
#include "MCPHandlerRegistration.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "VoxelGraphTracker.h"
#include "VoxelToolsCommon.h"

namespace
{
	// Read-only handlers skip the save bookkeeping. Must match `effect: read` in ue-mcp.plugin.yml (scripts/check.mjs).
	const TSet<FString> ReadHandlers =
	{
		TEXT("voxel_shader_hooks_status"),
		TEXT("voxel_world_status"),
		TEXT("voxel_stamp_read"),
		TEXT("voxel_sculpt_asset_get"),
		TEXT("voxel_query_layer"),
		TEXT("voxel_graph_read"),
		TEXT("voxel_graph_list_node_types"),
		TEXT("voxel_graph_export_t3d"),
	};

	// Handlers that block in Voxel::ExecuteSynchronously; past the bridge's 30 s default the client would
	// see a timeout while the edit still lands, and a retry would apply it twice.
	const TMap<FString, float> LongHandlers =
	{
		{ TEXT("voxel_height_sculpt"), 600.f },
		{ TEXT("voxel_volume_sculpt"), 600.f },
		{ TEXT("voxel_query_layer"), 300.f },
		{ TEXT("voxel_export_to_render_target"), 300.f },
		{ TEXT("voxel_sculpt_asset_set"), 300.f },
	};

	// Content packages a call marked dirty, directly or through Voxel side effects (graph migration, sculpting
	// into a linked asset), are saved before replying unless save:false. Levels never are: GetDirtyContentPackages
	// excludes map and external-actor packages, and saving the level stays the user's decision.
	TSharedPtr<FJsonValue> RunHandler(const FString& Name, const UEMCP::FExternalHandlerFn& Fn, const TSharedPtr<FJsonObject>& InParams)
	{
		const TSharedPtr<FJsonObject> Params = InParams.IsValid() ? InParams : MakeShared<FJsonObject>();
		if (ReadHandlers.Contains(Name))
		{
			return VoxelPluginTools::SanitizeJson(Fn(Params));
		}

		TSet<UPackage*> Touched;
		const FDelegateHandle Handle = UPackage::PackageMarkedDirtyEvent.AddLambda([&Touched](UPackage* Package, bool)
		{
			Touched.Add(Package);
		});
		TSharedPtr<FJsonValue> Result = Fn(Params);
		UPackage::PackageMarkedDirtyEvent.Remove(Handle);

		bool bSave = true;
		Params->TryGetBoolField(TEXT("save"), bSave);
		const TSharedPtr<FJsonObject> Object = Result.IsValid() && Result->Type == EJson::Object ? Result->AsObject() : nullptr;
		bool bSuccess = false;
		if (Object.IsValid() && Object->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess && Touched.Num() > 0)
		{
			// Voxel rebuilds compiled graphs on its next tick; saving before that stores stale compiled data.
			if (GVoxelGraphTracker)
			{
				GVoxelGraphTracker->Flush();
			}
			TArray<UPackage*> DirtyContent;
			FEditorFileUtils::GetDirtyContentPackages(DirtyContent);
			TArray<UPackage*> ToSave;
			for (UPackage* Package : DirtyContent)
			{
				if (Touched.Contains(Package))
				{
					ToSave.Add(Package);
				}
			}

			const auto Names = [](const TArray<UPackage*>& Packages)
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				for (const UPackage* Package : Packages)
				{
					Out.Add(MakeShared<FJsonValueString>(Package->GetName()));
				}
				return Out;
			};
			if (ToSave.Num() > 0 && bSave)
			{
				UEditorLoadingAndSavingUtils::SavePackages(ToSave, true);
				TArray<UPackage*> Saved;
				TArray<UPackage*> Failed;
				for (UPackage* Package : ToSave)
				{
					(Package->IsDirty() ? Failed : Saved).Add(Package);
				}
				if (Saved.Num() > 0) Object->SetArrayField(TEXT("autoSaved"), Names(Saved));
				if (Failed.Num() > 0) Object->SetArrayField(TEXT("dirtyNotSaved"), Names(Failed));
			}
			else if (ToSave.Num() > 0)
			{
				Object->SetArrayField(TEXT("dirtyNotSaved"), Names(ToSave));
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
			UEMCP::FExternalHandlerFn Wrapped = [Name = Entry.Name, Fn = Entry.Fn](const TSharedPtr<FJsonObject>& Params)
			{
				return RunHandler(Name, Fn, Params);
			};
			if (const float* Timeout = LongHandlers.Find(Entry.Name))
			{
				UEMCP::RegisterExternalHandlerWithTimeout(Entry.Name, MoveTemp(Wrapped), *Timeout);
			}
			else
			{
				UEMCP::RegisterExternalHandler(Entry.Name, MoveTemp(Wrapped));
			}
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
