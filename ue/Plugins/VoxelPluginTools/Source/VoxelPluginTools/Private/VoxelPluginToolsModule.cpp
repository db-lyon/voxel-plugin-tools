#include "Modules/ModuleManager.h"
#include "MCPHandlerRegistration.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "VoxelGraphTracker.h"
#include "VoxelToolsCommon.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelPluginTools, Log, All);

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
	// The contract is checked before the handler runs: the bridge reaches handlers without the server's own check.
	TSharedPtr<FJsonValue> RunHandler(const VoxelPluginTools::FHandlerEntry& Entry, const TSharedPtr<FJsonObject>& InParams)
	{
		const FString& Name = Entry.Name;
		const UEMCP::FExternalHandlerFn& Fn = Entry.Fn;
		const TSharedPtr<FJsonObject> Params = InParams.IsValid() ? InParams : MakeShared<FJsonObject>();
		if (const FString Violation = VoxelPluginTools::ContractViolation(Entry.Params, Entry.Rules, Params); !Violation.IsEmpty())
		{
			return VoxelPluginTools::Error(FString::Printf(TEXT("Invalid parameters for %s: %s"), *Name, *Violation));
		}
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
		const TArray<VoxelPluginTools::FHandlerEntry>& Handlers = VoxelPluginTools::GetHandlers();
		TSet<FString> Names;
		for (const VoxelPluginTools::FHandlerEntry& Entry : Handlers)
		{
			Names.Add(Entry.Name);
			// GetHandlers() is a function-local static, so the entry outlives every registration.
			UEMCP::FExternalHandlerFn Wrapped = [&Entry](const TSharedPtr<FJsonObject>& Params)
			{
				return RunHandler(Entry, Params);
			};
			const float* Timeout = LongHandlers.Find(Entry.Name);
			if (!UEMCP::RegisterExternalHandler(Entry.Name, MoveTemp(Wrapped), Entry.Params, Entry.Rules, Timeout ? *Timeout : 0.f))
			{
				UE_LOG(LogVoxelPluginTools, Error, TEXT("%s: the bridge refused its parameter contract (LogMCPBridge says why); the handler is registered without one and ue-mcp will not surface it"), *Entry.Name);
			}
		}

		// The contract is what ue-mcp surfaces and validates; a handler without one is unreachable from the server.
		for (const VoxelPluginTools::FHandlerEntry& Entry : Handlers)
		{
			FMCPHandlerSpec Spec;
			if (!UEMCP::LookupExternalHandlerSpec(Entry.Name, Spec))
			{
				UE_LOG(LogVoxelPluginTools, Error, TEXT("%s has no registered parameter contract"), *Entry.Name);
			}
		}
		for (const FString& Name : ReadHandlers)
		{
			if (!Names.Contains(Name)) UE_LOG(LogVoxelPluginTools, Error, TEXT("ReadHandlers names %s, which is not a registered handler"), *Name);
		}
		for (const TPair<FString, float>& Pair : LongHandlers)
		{
			if (!Names.Contains(Pair.Key)) UE_LOG(LogVoxelPluginTools, Error, TEXT("LongHandlers names %s, which is not a registered handler"), *Pair.Key);
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
