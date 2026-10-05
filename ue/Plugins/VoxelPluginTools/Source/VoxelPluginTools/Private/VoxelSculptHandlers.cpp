#include "VoxelToolsCommon.h"
#include "FileHelpers.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "Misc/Change.h"
#include "Misc/ITransaction.h"
#include "Engine/TextureRenderTarget2D.h"

#include "VoxelGraph.h"
#include "VoxelParameter.h"
#include "VoxelPinValue.h"
#include "VoxelLayer.h"
#include "VoxelLayerStack.h"
#include "VoxelStackLayer.h"
#include "VoxelQuery.h"
#include "VoxelLayers.h"
#include "VoxelMetadata.h"
#include "VoxelMetadataRef.h"
#include "VoxelMetadataOverrides.h"
#include "VoxelQueryBlueprintLibrary.h"
#include "Bulk/VoxelBulkLoader.h"
#include "Surface/VoxelSurfaceType.h"
#include "Surface/VoxelSurfaceTypeInterface.h"
#include "Surface/VoxelSurfaceTypeTable.h"
#include "Surface/VoxelSurfaceTypeBlendBuffer.h"
#include "Surface/VoxelSmartSurfaceTypeResolver.h"
#include "Utilities/VoxelBufferGradientUtilities.h"
#include "Texture/VoxelTexture.h"
#include "Sculpt/VoxelToolBrush.h"
#include "Sculpt/VoxelSculptMode.h"
#include "Sculpt/VoxelLevelToolType.h"
#include "Sculpt/Height/VoxelSculptHeight.h"
#include "Sculpt/Height/VoxelSculptHeightAsset.h"
#include "Sculpt/Height/VoxelSculptHeightData.h"
#include "Sculpt/Height/VoxelHeightSculptGraph.h"
#include "Sculpt/Height/VoxelHeightSculptBlueprintLibrary.h"
#include "Sculpt/Volume/VoxelSculptVolume.h"
#include "Sculpt/Volume/VoxelSculptVolumeAsset.h"
#include "Sculpt/Volume/VoxelSculptVolumeData.h"
#include "Sculpt/Volume/VoxelVolumeSculptGraph.h"
#include "Sculpt/Volume/VoxelVolumeSculptBlueprintLibrary.h"

#define LOCTEXT_NAMESPACE "VoxelPluginTools"

namespace VoxelPluginTools
{
namespace
{
	constexpr int32 MaxQueryPoints = 4096;
	constexpr int32 MaxQueryLOD = 30;

	// --------------------------------------------------------------------------------
	// Parameter parsing

	// Enum by short name, case-insensitive; the error lists every valid name.
	template<typename EnumType>
	bool ParseEnum(const FParams& Params, const TCHAR* Field, EnumType& InOut, FString& OutError)
	{
		if (!Has(Params, Field))
		{
			return true;
		}
		const FString Text = Str(Params, Field).TrimStartAndEnd();
		const UEnum* Enum = StaticEnum<EnumType>();
		TArray<FString> Names;
		for (int32 Index = 0; Index < Enum->NumEnums(); Index++)
		{
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.EndsWith(TEXT("_MAX")))
			{
				continue;
			}
			if (Name.Equals(Text, ESearchCase::IgnoreCase))
			{
				InOut = static_cast<EnumType>(Enum->GetValueByIndex(Index));
				return true;
			}
			Names.Add(Name);
		}
		OutError = FString::Printf(TEXT("%s '%s' is not valid. Use one of: %s"), Field, *Text, *FString::Join(Names, TEXT(", ")));
		return false;
	}

	template<typename EnumType>
	FString EnumName(EnumType Value)
	{
		return StaticEnum<EnumType>()->GetNameStringByValue(static_cast<int64>(Value));
	}

	// Optional finite number within [Min, Max]; absent leaves InOut untouched.
	bool OptNumber(const FParams& Params, const TCHAR* Field, double& InOut, double Min, double Max, FString& OutError)
	{
		if (!Has(Params, Field))
		{
			return true;
		}
		double Value = 0;
		if (!Params->TryGetNumberField(Field, Value) || !FMath::IsFinite(Value))
		{
			OutError = FString::Printf(TEXT("%s must be a finite number"), Field);
			return false;
		}
		if (Value < Min || Value > Max)
		{
			OutError = FString::Printf(TEXT("%s must be in [%g, %g], got %g"), Field, Min, Max, Value);
			return false;
		}
		InOut = Value;
		return true;
	}

	bool OptFloat(const FParams& Params, const TCHAR* Field, float& InOut, double Min, double Max, FString& OutError)
	{
		double Value = InOut;
		if (!OptNumber(Params, Field, Value, Min, Max, OutError))
		{
			return false;
		}
		InOut = static_cast<float>(Value);
		return true;
	}

	bool OptVector(const FParams& Params, const TCHAR* Field, FVector& InOut, FString& OutError)
	{
		if (!Has(Params, Field))
		{
			return true;
		}
		FVector Value;
		if (!Vec(Params, Field, Value) || Value.ContainsNaN())
		{
			OutError = FString::Printf(TEXT("%s must be {x,y,z} numbers"), Field);
			return false;
		}
		InOut = Value;
		return true;
	}

	bool OptDirection(const FParams& Params, const TCHAR* Field, FVector& InOut, FString& OutError)
	{
		FVector Value = InOut;
		if (!OptVector(Params, Field, Value, OutError))
		{
			return false;
		}
		if (!Value.Normalize())
		{
			OutError = FString::Printf(TEXT("%s must be a non-zero vector"), Field);
			return false;
		}
		InOut = Value;
		return true;
	}

	// {x,y} (z accepted and ignored) for height sculpts.
	bool RequireCenter2D(const FParams& Params, FVector2D& Out, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		double X = 0, Y = 0;
		if (!Params->TryGetObjectField(TEXT("center"), Object) ||
			!(*Object)->TryGetNumberField(TEXT("x"), X) ||
			!(*Object)->TryGetNumberField(TEXT("y"), Y) ||
			!FMath::IsFinite(X) || !FMath::IsFinite(Y))
		{
			OutError = TEXT("center {x,y} is required (world centimetres)");
			return false;
		}
		Out = FVector2D(X, Y);
		return true;
	}

	bool RequireCenter3D(const FParams& Params, FVector& Out, FString& OutError)
	{
		if (!Vec(Params, TEXT("center"), Out) || Out.ContainsNaN())
		{
			OutError = TEXT("center {x,y,z} is required (world centimetres)");
			return false;
		}
		return true;
	}

	// brush: { type, falloffType, falloffAmount, texture, textureChannel, autoRotate, fixedRotation,
	// use2DProjection, origin, textureRotation, centerTextureOnOrigin, repeatSize, hitNormal, strokeDirection }.
	// With bTopLevelFalloff, a top-level `falloff` sets the active brush falloff amount when brush.falloffAmount is absent.
	bool ParseBrush(const FParams& Params, FVoxelToolBrush& Brush, FString& OutError, bool bTopLevelFalloff = true)
	{
		const TSharedPtr<FJsonObject>* BrushObject = nullptr;
		FParams B;
		if (Has(Params, TEXT("brush")))
		{
			if (!Params->TryGetObjectField(TEXT("brush"), BrushObject))
			{
				OutError = TEXT("brush must be an object");
				return false;
			}
			B = *BrushObject;
		}

		if (B.IsValid())
		{
			// The type picks which fields the brush has (the contract's variants), so it is never implied.
			if (!Has(B, TEXT("type")))
			{
				OutError = TEXT("brush.type is required: Circular, Alpha or Pattern");
				return false;
			}
			if (!ParseEnum(B, TEXT("type"), Brush.BrushType, OutError)) return false;

			UVoxelTexture* Texture = nullptr;
			if (Has(B, TEXT("texture")))
			{
				Texture = Load<UVoxelTexture>(Str(B, TEXT("texture")), OutError);
				if (!Texture) return false;
			}
			EVoxelTextureChannel Channel = EVoxelTextureChannel::R;
			if (!ParseEnum(B, TEXT("textureChannel"), Channel, OutError)) return false;

			if (Brush.BrushType == EVoxelBrushType::Alpha)
			{
				if (!Texture)
				{
					OutError = TEXT("brush.texture (UVoxelTexture) is required for an Alpha brush");
					return false;
				}
				FVoxelAlphaBrush& Alpha = Brush.AlphaBrushData;
				Alpha.Texture = Texture;
				Alpha.TextureChannel = Channel;
				Alpha.bAutoRotateMask = Bool(B, TEXT("autoRotate"), Alpha.bAutoRotateMask);
				Alpha.bUse2DProjection = Bool(B, TEXT("use2DProjection"), Alpha.bUse2DProjection);
				if (!OptFloat(B, TEXT("fixedRotation"), Alpha.FixedRotation, -360, 360, OutError)) return false;
			}
			else if (Brush.BrushType == EVoxelBrushType::Pattern)
			{
				if (!Texture)
				{
					OutError = TEXT("brush.texture (UVoxelTexture) is required for a Pattern brush");
					return false;
				}
				FVoxelPatternBrush& Pattern = Brush.PatternBrushData;
				Pattern.Texture = Texture;
				Pattern.TextureChannel = Channel;
				Pattern.bCenterTextureOnOrigin = Bool(B, TEXT("centerTextureOnOrigin"), Pattern.bCenterTextureOnOrigin);
				if (!OptFloat(B, TEXT("textureRotation"), Pattern.TextureRotation, -360, 360, OutError)) return false;
				if (!OptFloat(B, TEXT("repeatSize"), Pattern.RepeatSize, UE_KINDA_SMALL_NUMBER, UE_BIG_NUMBER, OutError)) return false;
				if (Has(B, TEXT("origin")))
				{
					const TSharedPtr<FJsonObject>* Origin = nullptr;
					double X = 0, Y = 0;
					if (!B->TryGetObjectField(TEXT("origin"), Origin) ||
						!(*Origin)->TryGetNumberField(TEXT("x"), X) ||
						!(*Origin)->TryGetNumberField(TEXT("y"), Y))
					{
						OutError = TEXT("brush.origin must be {x,y}");
						return false;
					}
					Pattern.Origin = FVector2D(X, Y);
				}
			}

			if (!OptDirection(B, TEXT("hitNormal"), Brush.HitNormal, OutError)) return false;
			if (!OptDirection(B, TEXT("strokeDirection"), Brush.StrokeDirection, OutError)) return false;
		}

		FVoxelFalloff* Falloff = nullptr;
		switch (Brush.BrushType)
		{
		case EVoxelBrushType::Alpha: Falloff = &Brush.AlphaBrushData.Falloff; break;
		case EVoxelBrushType::Pattern: Falloff = &Brush.PatternBrushData.Falloff; break;
		default: Falloff = &Brush.CircularBrushData.Falloff; break;
		}
		if (B.IsValid())
		{
			if (!ParseEnum(B, TEXT("falloffType"), Falloff->Type, OutError)) return false;
		}
		if (B.IsValid() && Has(B, TEXT("falloffAmount")))
		{
			if (bTopLevelFalloff && Has(Params, TEXT("falloff")))
			{
				OutError = TEXT("falloff and brush.falloffAmount both set the brush falloff; pass one");
				return false;
			}
			return OptFloat(B, TEXT("falloffAmount"), Falloff->Amount, 0, 1, OutError);
		}
		return !bTopLevelFalloff || OptFloat(Params, TEXT("falloff"), Falloff->Amount, 0, 1, OutError);
	}

	// metadata: { "<UVoxelMetadata path>": valueText, ... } for paint ops.
	bool ParseMetadataOverrides(const FParams& Params, FVoxelMetadataOverrides& Out, FString& OutError)
	{
		if (!Has(Params, TEXT("metadata")))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (!Params->TryGetObjectField(TEXT("metadata"), Values))
		{
			OutError = TEXT("metadata must be an object { metadataPath: value }");
			return false;
		}
		for (const auto& Pair : (*Values)->Values)
		{
			UVoxelMetadata* Metadata = Load<UVoxelMetadata>(FString(*Pair.Key), OutError);
			if (!Metadata) return false;

			FVoxelMetadataOverride& Override = Out.Overrides.AddDefaulted_GetRef();
			Override.Metadata = Metadata;
			Override.Value = FVoxelPinValue(Metadata->GetInnerType().GetExposedType());
			FString Text;
			if (!ScalarText(Pair.Value, Text))
			{
				OutError = FString::Printf(TEXT("metadata value for %s must be a string, number, boolean or null"), *Pair.Key);
				return false;
			}
			if (!ParseValue(Override.Value, Text))
			{
				OutError = FString::Printf(TEXT("'%s' does not parse as %s for metadata %s"),
					*Text, *Metadata->GetInnerType().GetExposedType().ToString(), *Pair.Key);
				return false;
			}
		}
		return true;
	}

	// parameters: { name: valueText } applied as sculpt-graph parameter overrides.
	bool ParseGraphOverrides(const FParams& Params, const UVoxelGraph& Graph, FVoxelParameterOverrides& Out, FString& OutError)
	{
		if (!Has(Params, TEXT("parameters")))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (!Params->TryGetObjectField(TEXT("parameters"), Values))
		{
			OutError = TEXT("parameters must be an object { parameterName: value }");
			return false;
		}
		for (const auto& Pair : (*Values)->Values)
		{
			bool bFound = false;
			FGuid Guid;
			FVoxelParameter Parameter;
			Graph.ForeachParameter([&](const FGuid& InGuid, const FVoxelParameter& InParameter)
			{
				if (!bFound && InParameter.Name.ToString().Equals(FString(*Pair.Key), ESearchCase::IgnoreCase))
				{
					Guid = InGuid;
					Parameter = InParameter;
					bFound = true;
				}
			});
			if (!bFound)
			{
				OutError = FString::Printf(TEXT("Graph %s has no parameter '%s'"), *Graph.GetName(), *Pair.Key);
				return false;
			}
			FVoxelPinValue Value(Parameter.Type.GetExposedType());
			FString Text;
			if (!ScalarText(Pair.Value, Text))
			{
				OutError = FString::Printf(TEXT("parameters.%s must be a string, number, boolean or null"), *Pair.Key);
				return false;
			}
			if (!ParseValue(Value, Text))
			{
				OutError = FString::Printf(TEXT("'%s' does not parse as %s for parameter '%s'"), *Text, *Parameter.Type.ToString(), *Pair.Key);
				return false;
			}
			FVoxelParameterValueOverride& Override = Out.GuidToValueOverride.FindOrAdd(Guid);
			Override.bEnable = true;
			Override.Value = Value;
			Override.CachedName = Parameter.Name;
		}
		return true;
	}

	// --------------------------------------------------------------------------------
	// Undo: the sculpt-data swap Voxel's own editor tools record (VoxelSculptChanges.h, private to VoxelEditor).

	template<typename ActorType, typename DataType>
	class TSculptDataChange final : public FSwapChange
	{
	public:
		explicit TSculptDataChange(const TVoxelBulkRef<DataType>& InSnapshot)
			: Snapshot(InSnapshot)
		{
		}

		virtual TUniquePtr<FChange> Execute(UObject* Object) override
		{
			ActorType* Actor = Cast<ActorType>(Object);
			if (!Actor)
			{
				return MakeUnique<TSculptDataChange>(Snapshot);
			}
			TUniquePtr<FChange> Inverse = MakeUnique<TSculptDataChange>(Actor->GetSculptData());
			Actor->SetSculptData(Snapshot);
			return Inverse;
		}

		virtual FString ToString() const override
		{
			return TEXT("Voxel sculpt");
		}

	private:
		const TVoxelBulkRef<DataType> Snapshot;
	};

	struct FHeightKind
	{
		using ActorType = AVoxelSculptHeight;
		using ComponentType = UVoxelSculptHeightComponent;
		using AssetType = UVoxelSculptHeightAsset;
		using DataType = FVoxelSculptHeightData;
		static const TCHAR* Name() { return TEXT("height"); }
	};

	struct FVolumeKind
	{
		using ActorType = AVoxelSculptVolume;
		using ComponentType = UVoxelSculptVolumeComponent;
		using AssetType = UVoxelSculptVolumeAsset;
		using DataType = FVoxelSculptVolumeData;
		static const TCHAR* Name() { return TEXT("volume"); }
	};

	// External-asset binding plus the data needed to restore it exactly.
	template<typename Kind>
	struct TAssetState
	{
		TWeakObjectPtr<typename Kind::AssetType> Asset;
		// Actor data while unbound.
		TVoxelBulkPtr<typename Kind::DataType> ActorData;
		// An asset whose contents this state pins (the newly bound asset).
		TWeakObjectPtr<typename Kind::AssetType> TouchedAsset;
		TVoxelBulkPtr<typename Kind::DataType> TouchedData;
	};

	// Detach first so no SetSculptData can write into the previously bound asset, then rebind.
	template<typename Kind>
	void ApplyAssetState(typename Kind::ActorType& Actor, const TAssetState<Kind>& State)
	{
		typename Kind::ComponentType& Component = Actor.GetComponent();

		Component.ExternalAsset = nullptr;
		(void)Component.GetStamp();

		typename Kind::AssetType* Touched = State.TouchedAsset.Get();
		if (Touched && State.TouchedData && !(Touched->GetData().GetHash() == State.TouchedData.GetHash()))
		{
			Touched->SetData(State.TouchedData.ToBulkRef());
		}
		if (State.ActorData)
		{
			Actor.SetSculptData(State.ActorData.ToBulkRef());
		}
		if (typename Kind::AssetType* Asset = State.Asset.Get())
		{
			Component.ExternalAsset = Asset;
			(void)Component.GetStamp();
		}
		(void)Component.MarkPackageDirty();
	}

	template<typename Kind>
	class TAssetChange final : public FSwapChange
	{
	public:
		TAssetChange(const TAssetState<Kind>& InTo, const TAssetState<Kind>& InFrom)
			: To(InTo)
			, From(InFrom)
		{
		}

		virtual TUniquePtr<FChange> Execute(UObject* Object) override
		{
			if (typename Kind::ActorType* Actor = Cast<typename Kind::ActorType>(Object))
			{
				ApplyAssetState<Kind>(*Actor, To);
			}
			return MakeUnique<TAssetChange>(From, To);
		}

		virtual FString ToString() const override
		{
			return TEXT("Set voxel sculpt asset");
		}

	private:
		const TAssetState<Kind> To;
		const TAssetState<Kind> From;
	};

	// --------------------------------------------------------------------------------
	// Sculpt execution

	template<typename ActorType>
	ActorType* FindSculptActor(const FParams& Params, FString& OutError)
	{
		AActor* Actor = FindActor(Params, OutError);
		if (!Actor)
		{
			return nullptr;
		}
		ActorType* Typed = Cast<ActorType>(Actor);
		if (!Typed)
		{
			OutError = FString::Printf(TEXT("%s is a %s, not a %s"), *Actor->GetPathName(), *Actor->GetClass()->GetName(), *ActorType::StaticClass()->GetName());
		}
		return Typed;
	}

	// Records the undo snapshot, runs Apply (which returns the modifier future), and waits through Voxel's
	// synchronous task context unless wait=false.
	template<typename ActorType, typename DataType>
	FResult RunSculpt(ActorType& Actor, const FString& Op, const FParams& Params, const FText& Label, TFunctionRef<FVoxelFuture()> Apply)
	{
		const bool bWait = Bool(Params, TEXT("wait"), true);

		const FScopedTransaction Transaction(Label);
		const bool bUndoable = GUndo != nullptr;
		if (GUndo)
		{
			GUndo->StoreUndo(&Actor, MakeUnique<TSculptDataChange<ActorType, DataType>>(Actor.GetSculptData()));
		}

		bool bCompleted = false;
		if (bWait)
		{
			Voxel::ExecuteSynchronously([&]
			{
				return Apply();
			});
			bCompleted = true;
		}
		else
		{
			bCompleted = Apply().IsComplete();
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("actor"), ActorJson(Actor));
		Out->SetStringField(TEXT("op"), Op);
		Out->SetBoolField(TEXT("changed"), true);
		Out->SetBoolField(TEXT("completed"), bCompleted);
		Out->SetBoolField(TEXT("queued"), !bCompleted);
		// A queued edit lands after this reply: the undo snapshot may predate it and the auto-save cannot see it.
		Out->SetBoolField(TEXT("undoable"), bUndoable && bCompleted);
		if (!bCompleted)
		{
			Out->SetStringField(TEXT("note"), TEXT("Queued: not covered by undo or auto-save. Use wait:true for edits that must persist."));
		}
		return Ok(Out);
	}

	template<typename ActorType>
	FResult ClearCache(ActorType& Actor)
	{
		Actor.ClearSculptCache();
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("actor"), ActorJson(Actor));
		Out->SetStringField(TEXT("op"), TEXT("clear_cache"));
		Out->SetBoolField(TEXT("changed"), false);
		return Ok(Out);
	}

	FResult HeightSculpt(const FParams& Params)
	{
		FString Err;
		AVoxelSculptHeight* Actor = FindSculptActor<AVoxelSculptHeight>(Params, Err);
		if (!Actor) return Error(Err);

		const FString Op = Str(Params, TEXT("op"));

		// The fields each op reads; any other would be ignored, so it is refused.
		static const TMap<FString, TArray<const TCHAR*>> OpFields =
		{
			{ TEXT("clear_cache"), {} },
			{ TEXT("clear_data"), { TEXT("wait") } },
			{ TEXT("sculpt_height"), { TEXT("center"), TEXT("radius"), TEXT("strength"), TEXT("mode"), TEXT("falloff"), TEXT("brush"), TEXT("wait") } },
			{ TEXT("flatten"), { TEXT("center"), TEXT("radius"), TEXT("height"), TEXT("falloff"), TEXT("levelType"), TEXT("brush"), TEXT("wait") } },
			{ TEXT("smooth"), { TEXT("center"), TEXT("radius"), TEXT("strength"), TEXT("falloff"), TEXT("brush"), TEXT("wait") } },
			{ TEXT("paint_surface"), { TEXT("center"), TEXT("radius"), TEXT("strength"), TEXT("mode"), TEXT("falloff"), TEXT("brush"), TEXT("surfaceType"), TEXT("metadata"), TEXT("wait") } },
			{ TEXT("apply_graph"), { TEXT("center"), TEXT("radius"), TEXT("graph"), TEXT("parameters"), TEXT("wait") } },
		};
		const TArray<const TCHAR*>* Fields = OpFields.Find(Op);
		if (!Fields)
		{
			return Error(FString::Printf(TEXT("op '%s' is not valid. Use one of: sculpt_height, flatten, smooth, paint_surface, apply_graph, clear_data, clear_cache"), *Op));
		}
		{
			TArray<const TCHAR*> Allowed = { TEXT("actorPath"), TEXT("actorLabel"), TEXT("op"), TEXT("save") };
			Allowed.Append(*Fields);
			if (!OnlyKeys(Params, Allowed, FString::Printf(TEXT("op %s"), *Op), Err)) return Error(Err);
		}

		if (Op == TEXT("clear_cache"))
		{
			return ClearCache(*Actor);
		}
		if (Op == TEXT("clear_data"))
		{
			return RunSculpt<AVoxelSculptHeight, FVoxelSculptHeightData>(*Actor, Op, Params, LOCTEXT("ClearHeightSculpt", "Clear Voxel Height Sculpt"), [&]
			{
				UVoxelHeightSculptBlueprintLibrary::ClearSculptData(Actor);
				return FVoxelFuture();
			});
		}

		FVector2D Center;
		if (!RequireCenter2D(Params, Center, Err)) return Error(Err);

		// Defaults are UVoxelHeightSculptBlueprintLibrary's.
		float Radius = Op == TEXT("smooth") ? 1000.f : 500.f;
		if (!OptFloat(Params, TEXT("radius"), Radius, UE_KINDA_SMALL_NUMBER, UE_BIG_NUMBER, Err)) return Error(Err);

		if (!Actor->GetSculptContext().IsSet())
		{
			return Error(TEXT("The sculpt actor has no valid layer to sculpt (check its stamp Layer/Stack)"));
		}

		if (Op == TEXT("apply_graph"))
		{
			UVoxelHeightSculptGraph* Graph = Load<UVoxelHeightSculptGraph>(Str(Params, TEXT("graph")), Err);
			if (!Graph) return Error(Err);
			FVoxelHeightSculptGraphWrapper Wrapper;
			Wrapper.Graph = Graph;
			if (!ParseGraphOverrides(Params, *Graph, Wrapper.ParameterOverrides, Err)) return Error(Err);

			return RunSculpt<AVoxelSculptHeight, FVoxelSculptHeightData>(*Actor, Op, Params, LOCTEXT("HeightSculptGraph", "Apply Voxel Height Sculpt Graph"), [&]
			{
				return UVoxelHeightSculptBlueprintLibrary::ApplySculptGraph(Actor, Center, Radius, Wrapper);
			});
		}

		if (Op == TEXT("flatten"))
		{
			float Falloff = 0.1f;
			EVoxelLevelToolType Type = EVoxelLevelToolType::Additive;
			double TargetHeight = 0;
			if (!Has(Params, TEXT("height"))) return Error(TEXT("height (target world Z, centimetres) is required for flatten"));
			if (!OptNumber(Params, TEXT("height"), TargetHeight, -UE_BIG_NUMBER, UE_BIG_NUMBER, Err)) return Error(Err);
			if (!OptFloat(Params, TEXT("falloff"), Falloff, 0, 1, Err)) return Error(Err);
			if (!ParseEnum(Params, TEXT("levelType"), Type, Err)) return Error(Err);
			// Top-level falloff is the flatten falloff here; the brush falloff comes from brush.falloffAmount only.
			FVoxelToolBrush Brush;
			if (!ParseBrush(Params, Brush, Err, false)) return Error(Err);

			return RunSculpt<AVoxelSculptHeight, FVoxelSculptHeightData>(*Actor, Op, Params, LOCTEXT("HeightFlatten", "Voxel Height Flatten"), [&]
			{
				return UVoxelHeightSculptBlueprintLibrary::Flatten(Actor, Center, Radius, Falloff, Type, static_cast<float>(TargetHeight), Brush);
			});
		}

		FVoxelToolBrush Brush;
		if (!ParseBrush(Params, Brush, Err)) return Error(Err);
		EVoxelSculptMode Mode = EVoxelSculptMode::Add;
		if (!ParseEnum(Params, TEXT("mode"), Mode, Err)) return Error(Err);

		if (Op == TEXT("smooth"))
		{
			float Strength = 1.f;
			if (!OptFloat(Params, TEXT("strength"), Strength, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			return RunSculpt<AVoxelSculptHeight, FVoxelSculptHeightData>(*Actor, Op, Params, LOCTEXT("HeightSmooth", "Voxel Height Smooth"), [&]
			{
				return UVoxelHeightSculptBlueprintLibrary::Smooth(Actor, Center, Radius, Strength, Brush);
			});
		}

		if (Op == TEXT("paint_surface"))
		{
			float Strength = 0.05f;
			if (!OptFloat(Params, TEXT("strength"), Strength, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			UVoxelSurfaceTypeInterface* SurfaceType = nullptr;
			if (Has(Params, TEXT("surfaceType")))
			{
				SurfaceType = Load<UVoxelSurfaceTypeInterface>(Str(Params, TEXT("surfaceType")), Err);
				if (!SurfaceType) return Error(Err);
			}
			FVoxelMetadataOverrides Metadatas;
			if (!ParseMetadataOverrides(Params, Metadatas, Err)) return Error(Err);
			if (!SurfaceType && Metadatas.Overrides.Num() == 0)
			{
				return Error(TEXT("paint_surface needs surfaceType and/or metadata"));
			}
			return RunSculpt<AVoxelSculptHeight, FVoxelSculptHeightData>(*Actor, Op, Params, LOCTEXT("HeightPaint", "Voxel Height Paint"), [&]
			{
				return UVoxelHeightSculptBlueprintLibrary::PaintSurface(Actor, Center, Radius, Strength, Mode, SurfaceType, Metadatas, Brush);
			});
		}

		float Strength = 0.5f;
		if (!OptFloat(Params, TEXT("strength"), Strength, 0, UE_BIG_NUMBER, Err)) return Error(Err);
		return RunSculpt<AVoxelSculptHeight, FVoxelSculptHeightData>(*Actor, Op, Params, LOCTEXT("HeightSculpt", "Voxel Height Sculpt"), [&]
		{
			return UVoxelHeightSculptBlueprintLibrary::SculptHeight(Actor, Center, Radius, Strength, Mode, Brush);
		});
	}

	FResult VolumeSculpt(const FParams& Params)
	{
		FString Err;
		AVoxelSculptVolume* Actor = FindSculptActor<AVoxelSculptVolume>(Params, Err);
		if (!Actor) return Error(Err);

		const FString Op = Str(Params, TEXT("op"));

		// The fields each op reads; any other would be ignored, so it is refused.
		static const TMap<FString, TArray<const TCHAR*>> OpFields =
		{
			{ TEXT("clear_cache"), {} },
			{ TEXT("clear_data"), { TEXT("wait") } },
			{ TEXT("sphere"), { TEXT("center"), TEXT("radius"), TEXT("mode"), TEXT("smoothness"), TEXT("wait") } },
			{ TEXT("cube"), { TEXT("center"), TEXT("size"), TEXT("rotation"), TEXT("roundness"), TEXT("mode"), TEXT("smoothness"), TEXT("wait") } },
			{ TEXT("flatten"), { TEXT("center"), TEXT("radius"), TEXT("normal"), TEXT("height"), TEXT("falloff"), TEXT("levelType"), TEXT("wait") } },
			{ TEXT("smooth"), { TEXT("center"), TEXT("radius"), TEXT("strength"), TEXT("falloff"), TEXT("brush"), TEXT("wait") } },
			{ TEXT("surface"), { TEXT("center"), TEXT("radius"), TEXT("strength"), TEXT("mode"), TEXT("falloff"), TEXT("brush"), TEXT("wait") } },
			{ TEXT("angle"), { TEXT("center"), TEXT("radius"), TEXT("strength"), TEXT("planePoint"), TEXT("planeNormal"), TEXT("mergeMode"), TEXT("falloff"), TEXT("brush"), TEXT("wait") } },
			{ TEXT("paint"), { TEXT("center"), TEXT("radius"), TEXT("strength"), TEXT("mode"), TEXT("falloff"), TEXT("brush"), TEXT("surfaceType"), TEXT("metadata"), TEXT("wait") } },
			{ TEXT("apply_graph"), { TEXT("center"), TEXT("radius"), TEXT("graph"), TEXT("parameters"), TEXT("rotation"), TEXT("wait") } },
		};
		const TArray<const TCHAR*>* Fields = OpFields.Find(Op);
		if (!Fields)
		{
			return Error(FString::Printf(TEXT("op '%s' is not valid. Use one of: sphere, cube, flatten, smooth, surface, angle, paint, apply_graph, clear_data, clear_cache"), *Op));
		}
		{
			TArray<const TCHAR*> Allowed = { TEXT("actorPath"), TEXT("actorLabel"), TEXT("op"), TEXT("save") };
			Allowed.Append(*Fields);
			if (!OnlyKeys(Params, Allowed, FString::Printf(TEXT("op %s"), *Op), Err)) return Error(Err);
		}

		if (Op == TEXT("clear_cache"))
		{
			return ClearCache(*Actor);
		}
		if (Op == TEXT("clear_data"))
		{
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("ClearVolumeSculpt", "Clear Voxel Volume Sculpt"), [&]
			{
				UVoxelVolumeSculptBlueprintLibrary::ClearSculptData(Actor);
				return FVoxelFuture();
			});
		}

		FVector Center;
		if (!RequireCenter3D(Params, Center, Err)) return Error(Err);

		// Defaults are UVoxelVolumeSculptBlueprintLibrary's.
		float Radius = (Op == TEXT("sphere") || Op == TEXT("smooth")) ? 1000.f : 500.f;
		if (Op != TEXT("cube") && !OptFloat(Params, TEXT("radius"), Radius, UE_KINDA_SMALL_NUMBER, UE_BIG_NUMBER, Err)) return Error(Err);

		EVoxelSculptMode Mode = EVoxelSculptMode::Add;
		if (!ParseEnum(Params, TEXT("mode"), Mode, Err)) return Error(Err);

		if (!Actor->GetSculptContext().IsSet())
		{
			return Error(TEXT("The sculpt actor has no valid layer to sculpt (check its stamp Layer/Stack)"));
		}

		if (Op == TEXT("sphere"))
		{
			float Smoothness = 0.f;
			if (!OptFloat(Params, TEXT("smoothness"), Smoothness, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumeSphere", "Voxel Volume Sphere"), [&]
			{
				return UVoxelVolumeSculptBlueprintLibrary::SculptSphere(Actor, Center, Radius, Mode, Smoothness);
			});
		}

		if (Op == TEXT("cube"))
		{
			FVector Size(1000.f);
			FRotator Rotation = FRotator::ZeroRotator;
			float Roundness = 0.f;
			float Smoothness = 0.f;
			if (!OptVector(Params, TEXT("size"), Size, Err)) return Error(Err);
			if (Size.X <= 0 || Size.Y <= 0 || Size.Z <= 0) return Error(TEXT("size components must be > 0"));
			if (Has(Params, TEXT("rotation")) && !Rot(Params, TEXT("rotation"), Rotation)) return Error(TEXT("rotation must be {pitch,yaw,roll}"));
			if (!OptFloat(Params, TEXT("roundness"), Roundness, 0, 1, Err)) return Error(Err);
			if (!OptFloat(Params, TEXT("smoothness"), Smoothness, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumeCube", "Voxel Volume Cube"), [&]
			{
				return UVoxelVolumeSculptBlueprintLibrary::SculptCube(Actor, Center, Size, Rotation, Roundness, Mode, Smoothness);
			});
		}

		if (Op == TEXT("flatten"))
		{
			FVector Normal = FVector::UpVector;
			float Height = 1000.f;
			float Falloff = 0.1f;
			EVoxelLevelToolType Type = EVoxelLevelToolType::Additive;
			if (!OptDirection(Params, TEXT("normal"), Normal, Err)) return Error(Err);
			if (!OptFloat(Params, TEXT("height"), Height, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			if (!OptFloat(Params, TEXT("falloff"), Falloff, 0, 1, Err)) return Error(Err);
			if (!ParseEnum(Params, TEXT("levelType"), Type, Err)) return Error(Err);
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumeFlatten", "Voxel Volume Flatten"), [&]
			{
				return UVoxelVolumeSculptBlueprintLibrary::Flatten(Actor, Center, Normal, Radius, Height, Falloff, Type);
			});
		}

		if (Op == TEXT("apply_graph"))
		{
			UVoxelVolumeSculptGraph* Graph = Load<UVoxelVolumeSculptGraph>(Str(Params, TEXT("graph")), Err);
			if (!Graph) return Error(Err);
			FVoxelVolumeSculptGraphWrapper Wrapper;
			Wrapper.Graph = Graph;
			if (!ParseGraphOverrides(Params, *Graph, Wrapper.ParameterOverrides, Err)) return Error(Err);
			FRotator Rotation = FRotator::ZeroRotator;
			if (Has(Params, TEXT("rotation")) && !Rot(Params, TEXT("rotation"), Rotation)) return Error(TEXT("rotation must be {pitch,yaw,roll}"));
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumeSculptGraph", "Apply Voxel Volume Sculpt Graph"), [&]
			{
				return UVoxelVolumeSculptBlueprintLibrary::ApplySculptGraph(Actor, Center, Radius, Wrapper, Rotation);
			});
		}

		FVoxelToolBrush Brush;
		if (!ParseBrush(Params, Brush, Err)) return Error(Err);

		if (Op == TEXT("smooth"))
		{
			float Strength = 1.f;
			if (!OptFloat(Params, TEXT("strength"), Strength, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumeSmooth", "Voxel Volume Smooth"), [&]
			{
				return UVoxelVolumeSculptBlueprintLibrary::Smooth(Actor, Center, Radius, Strength, Brush);
			});
		}

		if (Op == TEXT("angle"))
		{
			float Strength = 1.f;
			FVector PlanePoint = Center;
			FVector PlaneNormal = FVector::UpVector;
			EVoxelSDFMergeMode MergeMode = EVoxelSDFMergeMode::Override;
			if (!OptFloat(Params, TEXT("strength"), Strength, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			if (!OptVector(Params, TEXT("planePoint"), PlanePoint, Err)) return Error(Err);
			if (!OptDirection(Params, TEXT("planeNormal"), PlaneNormal, Err)) return Error(Err);
			if (!ParseEnum(Params, TEXT("mergeMode"), MergeMode, Err)) return Error(Err);
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumeAngle", "Voxel Volume Angle"), [&]
			{
				return UVoxelVolumeSculptBlueprintLibrary::SculptAngle(Actor, Center, Radius, Strength, PlanePoint, PlaneNormal, MergeMode, Brush);
			});
		}

		if (Op == TEXT("paint"))
		{
			float Strength = 0.05f;
			if (!OptFloat(Params, TEXT("strength"), Strength, 0, UE_BIG_NUMBER, Err)) return Error(Err);
			UVoxelSurfaceTypeInterface* SurfaceType = nullptr;
			if (Has(Params, TEXT("surfaceType")))
			{
				SurfaceType = Load<UVoxelSurfaceTypeInterface>(Str(Params, TEXT("surfaceType")), Err);
				if (!SurfaceType) return Error(Err);
			}
			FVoxelMetadataOverrides Metadatas;
			if (!ParseMetadataOverrides(Params, Metadatas, Err)) return Error(Err);
			if (!SurfaceType && Metadatas.Overrides.Num() == 0)
			{
				return Error(TEXT("paint needs surfaceType and/or metadata"));
			}
			return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumePaint", "Voxel Volume Paint"), [&]
			{
				return UVoxelVolumeSculptBlueprintLibrary::PaintSurface(Actor, Center, Radius, Strength, Mode, SurfaceType, Metadatas, Brush);
			});
		}

		// surface
		float Strength = 0.5f;
		if (!OptFloat(Params, TEXT("strength"), Strength, 0, UE_BIG_NUMBER, Err)) return Error(Err);
		return RunSculpt<AVoxelSculptVolume, FVoxelSculptVolumeData>(*Actor, Op, Params, LOCTEXT("VolumeSurface", "Voxel Volume Surface Sculpt"), [&]
		{
			return UVoxelVolumeSculptBlueprintLibrary::SculptSurface(Actor, Center, Radius, Strength, Mode, Brush);
		});
	}

	// --------------------------------------------------------------------------------
	// External save asset

	FResult SculptAssetGet(const FParams& Params)
	{
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Error(Err);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("actor"), ActorJson(*Actor));
		UObject* Asset = nullptr;
		bool bEmpty = true;
		if (const AVoxelSculptHeight* Height = Cast<AVoxelSculptHeight>(Actor))
		{
			Out->SetStringField(TEXT("kind"), FHeightKind::Name());
			Asset = Height->GetComponent().ExternalAsset;
			bEmpty = Height->GetSculptData()->IsEmpty();
		}
		else if (const AVoxelSculptVolume* Volume = Cast<AVoxelSculptVolume>(Actor))
		{
			Out->SetStringField(TEXT("kind"), FVolumeKind::Name());
			Asset = Volume->GetComponent().ExternalAsset;
			bEmpty = Volume->GetSculptData()->IsEmpty();
		}
		else
		{
			return Error(FString::Printf(TEXT("%s is a %s, not a voxel sculpt actor (AVoxelSculptHeight / AVoxelSculptVolume)"), *Actor->GetPathName(), *Actor->GetClass()->GetName()));
		}

		if (Asset)
		{
			Out->SetStringField(TEXT("asset"), Asset->GetPathName());
		}
		else
		{
			Out->SetField(TEXT("asset"), MakeShared<FJsonValueNull>());
		}
		Out->SetBoolField(TEXT("dataEmpty"), bEmpty);
		return Ok(Out);
	}

	template<typename Kind>
	FResult SetAsset(typename Kind::ActorType& Actor, const FString& AssetPath, const FParams& Params)
	{
		using AssetType = typename Kind::AssetType;
		using ComponentType = typename Kind::ComponentType;

		AssetType* NewAsset = nullptr;
		if (!AssetPath.IsEmpty())
		{
			FString Err;
			UObject* Object = LoadTyped(AssetPath, UObject::StaticClass(), Err);
			if (!Object) return Error(Err);
			NewAsset = Cast<AssetType>(Object);
			if (!NewAsset)
			{
				return Error(FString::Printf(TEXT("%s is a %s; a %s sculpt actor needs a %s"),
					*AssetPath, *Object->GetClass()->GetName(), Kind::Name(), *AssetType::StaticClass()->GetName()));
			}
		}
		const bool bLoad = Bool(Params, TEXT("load"), true);

		ComponentType& Component = Actor.GetComponent();
		AssetType* OldAsset = Component.ExternalAsset;

		// Writing the actor's data into the asset needs it fully resident first (as FVoxelSculpt*LocalData::SetAsset does).
		const auto ActorData = Actor.GetSculptData();
		ActorData.FullyLoadSync(*Actor.GetBulkLoader());

		TAssetState<Kind> From;
		From.Asset = OldAsset;
		From.ActorData = ActorData;
		From.TouchedAsset = NewAsset;
		if (NewAsset)
		{
			From.TouchedData = NewAsset->GetData();
		}

		TAssetState<Kind> To;
		To.Asset = NewAsset;
		To.ActorData = ActorData;
		To.TouchedAsset = NewAsset;
		if (NewAsset)
		{
			if (bLoad) To.TouchedData = NewAsset->GetData();
			else To.TouchedData = ActorData;
		}

		FProperty* Property = FindFProperty<FProperty>(ComponentType::StaticClass(), GET_MEMBER_NAME_CHECKED(ComponentType, ExternalAsset));

		const FScopedTransaction Transaction(LOCTEXT("SetSculptAsset", "Set Voxel Sculpt Asset"));
		// The asset is not Modify()'d: its transaction serialize would copy all sculpt data into the undo
		// buffer and reload it without OnDataChanged. TAssetChange restores its contents instead.
		Component.Modify();
		Component.PreEditChange(Property);
		if (GUndo)
		{
			GUndo->StoreUndo(&Actor, MakeUnique<TAssetChange<Kind>>(From, To));
		}
		ApplyAssetState<Kind>(Actor, To);
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Component.PostEditChangeProperty(Event);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("actor"), ActorJson(Actor));
		Out->SetStringField(TEXT("kind"), Kind::Name());
		if (OldAsset) Out->SetStringField(TEXT("previousAsset"), OldAsset->GetPathName());
		else Out->SetField(TEXT("previousAsset"), MakeShared<FJsonValueNull>());
		if (NewAsset) Out->SetStringField(TEXT("asset"), NewAsset->GetPathName());
		else Out->SetField(TEXT("asset"), MakeShared<FJsonValueNull>());
		Out->SetBoolField(TEXT("loaded"), NewAsset && bLoad);
		Out->SetBoolField(TEXT("dataEmpty"), Actor.GetSculptData()->IsEmpty());
		Out->SetBoolField(TEXT("changed"), OldAsset != NewAsset || (NewAsset && !bLoad));
		// Writing the actor's data into the asset dirties it; persist it like every other asset edit.
		bool bSaved = false;
		if (NewAsset && !bLoad && Bool(Params, TEXT("save"), true))
		{
			bSaved = UEditorLoadingAndSavingUtils::SavePackages({ NewAsset->GetPackage() }, false);
		}
		Out->SetBoolField(TEXT("saved"), bSaved);
		return Ok(Out);
	}

	FResult SculptAssetSet(const FParams& Params)
	{
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Error(Err);
		if (!Has(Params, TEXT("asset")))
		{
			return Error(TEXT("asset is required (a sculpt asset path, or \"\" / null to detach)"));
		}
		const FString AssetPath = Str(Params, TEXT("asset"));
		if (AssetPath.IsEmpty() && Has(Params, TEXT("load")))
		{
			return Error(TEXT("load only applies when binding an asset; detaching keeps the actor's data in the level"));
		}

		if (AVoxelSculptHeight* Height = Cast<AVoxelSculptHeight>(Actor))
		{
			return SetAsset<FHeightKind>(*Height, AssetPath, Params);
		}
		if (AVoxelSculptVolume* Volume = Cast<AVoxelSculptVolume>(Actor))
		{
			return SetAsset<FVolumeKind>(*Volume, AssetPath, Params);
		}
		return Error(FString::Printf(TEXT("%s is a %s, not a voxel sculpt actor (AVoxelSculptHeight / AVoxelSculptVolume)"), *Actor->GetPathName(), *Actor->GetClass()->GetName()));
	}

	// --------------------------------------------------------------------------------
	// Layer query

	struct FQueryPoint
	{
		float Value = 0.f;
		FVector3f Normal = FVector3f::ZeroVector;
		TVoxelObjectPtr<UVoxelSurfaceTypeInterface> UnresolvedSurfaceType;
		TVoxelObjectPtr<UVoxelSurfaceTypeInterface> SurfaceType;
		TArray<TPair<TVoxelObjectPtr<UVoxelSurfaceTypeInterface>, float>> SurfaceWeights;
		TArray<FVoxelPinValue> MetadataValues;
	};

	struct FQueryOutput
	{
		TArray<FQueryPoint> Points;
	};

	bool ParseLayerKind(const FParams& Params, bool& bOutHeight, FString& OutError)
	{
		const FString Kind = Str(Params, TEXT("layerKind"));
		if (Kind.IsEmpty() || Kind.Equals(TEXT("height"), ESearchCase::IgnoreCase))
		{
			bOutHeight = true;
			return true;
		}
		if (Kind.Equals(TEXT("volume"), ESearchCase::IgnoreCase))
		{
			bOutHeight = false;
			return true;
		}
		OutError = FString::Printf(TEXT("layerKind '%s' is not valid. Use height or volume"), *Kind);
		return false;
	}

	FString ObjectPathOrEmpty(const TVoxelObjectPtr<UVoxelSurfaceTypeInterface>& Ptr)
	{
		const UObject* Object = Ptr.Resolve();
		return Object ? Object->GetPathName() : FString();
	}

	void SetPathOrNull(const TSharedRef<FJsonObject>& Out, const TCHAR* Field, const FString& Path)
	{
		if (Path.IsEmpty()) Out->SetField(Field, MakeShared<FJsonValueNull>());
		else Out->SetStringField(Field, Path);
	}

	FResult QueryLayer(const FParams& Params)
	{
		FString Err;
		UWorld* World = EditorWorld();
		if (!World) return Error(TEXT("No editor world"));

		bool bHeight = true;
		if (!ParseLayerKind(Params, bHeight, Err)) return Error(Err);
		FVoxelStackLayer StackLayer;
		if (!ParseStackLayer(Params, TEXT("stack"), TEXT("layer"), bHeight, StackLayer, Err)) return Error(Err);
		if (!StackLayer.IsValid()) return Error(TEXT("The stack/layer pair is not valid"));

		double LOD = 0;
		if (!OptNumber(Params, TEXT("lod"), LOD, 0, MaxQueryLOD, Err)) return Error(Err);
		if (LOD != FMath::FloorToDouble(LOD)) return Error(TEXT("lod must be an integer"));
		double GradientStep = 100.0;
		if (!OptNumber(Params, TEXT("gradientStep"), GradientStep, UE_KINDA_SMALL_NUMBER, UE_BIG_NUMBER, Err)) return Error(Err);
		const bool bSurface = Bool(Params, TEXT("querySurface"), false);

		const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("points"), PointValues) || PointValues->Num() == 0)
		{
			return Error(bHeight ? TEXT("points [{x,y}] is required") : TEXT("points [{x,y,z}] is required"));
		}
		if (PointValues->Num() > MaxQueryPoints)
		{
			return Error(FString::Printf(TEXT("At most %d points per query (got %d)"), MaxQueryPoints, PointValues->Num()));
		}
		TArray<FVector> Positions;
		Positions.Reserve(PointValues->Num());
		for (int32 Index = 0; Index < PointValues->Num(); Index++)
		{
			const TSharedPtr<FJsonObject>* Point = nullptr;
			double X = 0, Y = 0, Z = 0;
			const bool bOk = (*PointValues)[Index].IsValid() && (*PointValues)[Index]->TryGetObject(Point) &&
				(*Point)->TryGetNumberField(TEXT("x"), X) && (*Point)->TryGetNumberField(TEXT("y"), Y) &&
				(bHeight || (*Point)->TryGetNumberField(TEXT("z"), Z));
			// A height layer is sampled at (x, y); a z there would be dropped without a word.
			if (!bOk || !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z) || (bHeight && (*Point)->HasField(TEXT("z"))))
			{
				return Error(FString::Printf(TEXT("points[%d] must be %s"), Index, bHeight ? TEXT("{x,y} for a height layer") : TEXT("{x,y,z}")));
			}
			Positions.Add(FVector(X, Y, Z));
		}

		TArray<UVoxelMetadata*> Metadatas;
		if (Has(Params, TEXT("metadata")))
		{
			const TArray<TSharedPtr<FJsonValue>>* MetadataValues = nullptr;
			if (!Params->TryGetArrayField(TEXT("metadata"), MetadataValues))
			{
				return Error(TEXT("metadata must be an array of UVoxelMetadata paths"));
			}
			for (const TSharedPtr<FJsonValue>& Value : *MetadataValues)
			{
				if (!Value.IsValid() || Value->Type != EJson::String) return Error(TEXT("metadata must contain only UVoxelMetadata asset paths"));
				UVoxelMetadata* Metadata = Load<UVoxelMetadata>(Value->AsString(), Err);
				if (!Metadata) return Error(Err);
				if (Metadatas.Contains(Metadata)) return Error(FString::Printf(TEXT("metadata lists %s twice"), *Metadata->GetPathName()));
				Metadatas.Add(Metadata);
			}
		}
		const TVoxelArray<FVoxelMetadataRef> MetadataRefs = FVoxelMetadataRef::GetUniqueValidRefs(Metadatas);
		// GetUniqueValidRefs drops what it cannot query; the result would silently miss those keys.
		if (MetadataRefs.Num() != Metadatas.Num())
		{
			return Error(TEXT("metadata names an asset Voxel cannot query (no valid metadata ref)"));
		}

		// Mirrors UVoxelQueryBlueprintLibrary::MultiQueryVoxelLayer, adding LOD and the full surface blend.
		const TSharedRef<FQueryOutput> Output = MakeShared<FQueryOutput>();
		Voxel::ExecuteSynchronously([&]
		{
			return Voxel::AsyncTask([
				Output,
				Positions,
				MetadataRefs,
				bSurface,
				LODIndex = static_cast<int32>(LOD),
				Step = static_cast<float>(GradientStep),
				WeakLayer = FVoxelWeakStackLayer(StackLayer),
				Layers = FVoxelLayers::Get(World),
				SurfaceTypeTable = FVoxelSurfaceTypeTable::Get()]
			{
				const int32 Num = Positions.Num();

				FVoxelDoubleVectorBuffer PositionsBuffer;
				PositionsBuffer.Allocate(Num);
				for (int32 Index = 0; Index < Num; Index++)
				{
					PositionsBuffer.Set(Index, Positions[Index]);
				}

				FVoxelSurfaceTypeBlendBuffer SurfaceTypes;
				SurfaceTypes.AllocateZeroed(Num);

				TVoxelMap<FVoxelMetadataRef, TSharedRef<FVoxelBuffer>> MetadataToBuffer;
				MetadataToBuffer.Reserve(MetadataRefs.Num());
				for (const FVoxelMetadataRef& Ref : MetadataRefs)
				{
					MetadataToBuffer.Add_EnsureNew(Ref, Ref.MakeDefaultBuffer(Num));
				}

				const FVoxelQuery Query(
					LODIndex,
					*Layers,
					*SurfaceTypeTable,
					FVoxelDependencyCollector::Null);

				FVoxelFloatBuffer Values;
				FVoxelVectorBuffer Normals;
				if (WeakLayer.Type == EVoxelLayerType::Height)
				{
					FVoxelDoubleVector2DBuffer Positions2D;
					Positions2D.X = PositionsBuffer.X;
					Positions2D.Y = PositionsBuffer.Y;

					Values = Query.SampleHeightLayer(WeakLayer, Positions2D, SurfaceTypes.View(), MetadataToBuffer);

					const FVoxelDoubleVector2DBuffer GradientPositions = FVoxelBufferGradientUtilities::SplitPositions2D(Positions2D, Step);
					const FVoxelFloatBuffer GradientHeights = Query.SampleHeightLayer(WeakLayer, GradientPositions);
					Normals = FVoxelBufferGradientUtilities::CollapseGradient2DToNormal(GradientHeights, Num, Step);
				}
				else
				{
					Values = Query.SampleVolumeLayer(WeakLayer, PositionsBuffer, SurfaceTypes.View(), MetadataToBuffer);

					const FVoxelDoubleVectorBuffer GradientPositions = FVoxelBufferGradientUtilities::SplitPositions3D(PositionsBuffer, Step);
					const FVoxelFloatBuffer GradientDistances = Query.SampleVolumeLayer(WeakLayer, GradientPositions);
					Normals = FVoxelBufferGradientUtilities::CollapseGradient3DToNormal(GradientDistances, Num, Step);
				}

				Output->Points.SetNum(Num);
				for (int32 Index = 0; Index < Num; Index++)
				{
					FQueryPoint& Point = Output->Points[Index];
					Point.Value = Values[Index];
					Point.Normal = Normals[Index];
					Point.UnresolvedSurfaceType = SurfaceTypes[Index].GetTopLayer().Type.GetSurfaceTypeInterface();
					for (const FVoxelMetadataRef& Ref : MetadataRefs)
					{
						Point.MetadataValues.Add(Ref.GetValue(*MetadataToBuffer[Ref], Index));
					}
				}

				if (!bSurface)
				{
					return;
				}

				FVoxelSmartSurfaceTypeResolver Resolver(
					LODIndex,
					WeakLayer,
					*Layers,
					*SurfaceTypeTable,
					FVoxelDependencyCollector::Null,
					PositionsBuffer,
					Normals,
					SurfaceTypes.View());
				Resolver.Resolve();

				for (int32 Index = 0; Index < Num; Index++)
				{
					FQueryPoint& Point = Output->Points[Index];
					Point.SurfaceType = SurfaceTypes[Index].GetTopLayer().Type.GetSurfaceTypeInterface();
					for (const FVoxelSurfaceTypeBlendLayer& Layer : SurfaceTypes[Index].GetLayers())
					{
						Point.SurfaceWeights.Emplace(Layer.Type.GetSurfaceTypeInterface(), Layer.Weight.ToFloat());
					}
				}
			});
		});

		TArray<FString> MetadataPaths;
		for (const FVoxelMetadataRef& Ref : MetadataRefs)
		{
			const UVoxelMetadata* Metadata = Ref.GetMetadata().Resolve();
			MetadataPaths.Add(Metadata ? Metadata->GetPathName() : FString());
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(Output->Points.Num());
		for (int32 Index = 0; Index < Output->Points.Num(); Index++)
		{
			const FQueryPoint& Point = Output->Points[Index];
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetNumberField(TEXT("x"), Positions[Index].X);
			P->SetNumberField(TEXT("y"), Positions[Index].Y);
			if (!bHeight) P->SetNumberField(TEXT("z"), Positions[Index].Z);
			P->SetNumberField(bHeight ? TEXT("height") : TEXT("distance"), Point.Value);
			P->SetObjectField(TEXT("normal"), VecJson(FVector(Point.Normal)));
			if (bSurface)
			{
				SetPathOrNull(P, TEXT("unresolvedSurfaceType"), ObjectPathOrEmpty(Point.UnresolvedSurfaceType));
				SetPathOrNull(P, TEXT("surfaceType"), ObjectPathOrEmpty(Point.SurfaceType));
				TArray<TSharedPtr<FJsonValue>> Weights;
				for (const TPair<TVoxelObjectPtr<UVoxelSurfaceTypeInterface>, float>& Weight : Point.SurfaceWeights)
				{
					TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
					SetPathOrNull(W, TEXT("surfaceType"), ObjectPathOrEmpty(Weight.Key));
					W->SetNumberField(TEXT("weight"), Weight.Value);
					Weights.Add(MakeShared<FJsonValueObject>(W));
				}
				P->SetArrayField(TEXT("surfaceWeights"), Weights);
			}
			if (MetadataPaths.Num() > 0)
			{
				TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
				for (int32 MetadataIndex = 0; MetadataIndex < MetadataPaths.Num() && MetadataIndex < Point.MetadataValues.Num(); MetadataIndex++)
				{
					M->SetStringField(MetadataPaths[MetadataIndex], Point.MetadataValues[MetadataIndex].ExportToString());
				}
				P->SetObjectField(TEXT("metadata"), M);
			}
			Results.Add(MakeShared<FJsonValueObject>(P));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("world"), World->GetPathName());
		Out->SetStringField(TEXT("layerKind"), bHeight ? TEXT("height") : TEXT("volume"));
		Out->SetStringField(TEXT("stack"), StackLayer.Stack ? StackLayer.Stack->GetPathName() : FString());
		Out->SetStringField(TEXT("layer"), StackLayer.Layer ? StackLayer.Layer->GetPathName() : FString());
		Out->SetNumberField(TEXT("lod"), LOD);
		Out->SetArrayField(TEXT("points"), Results);
		return Ok(Out);
	}

	// --------------------------------------------------------------------------------
	// Render target export

	// "height" | number (constant) | { metadata: path, component?: R|G|B|A }
	bool ParseChannel(const FParams& Params, const TCHAR* Field, FVoxelFloatQuery& Out, bool& bOutIsHeight, FString& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid())
		{
			return true;
		}
		// By JSON type: TryGetNumber would also read "0.5" or true as a constant.
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->Type == EJson::String && Value->AsString().Equals(TEXT("height"), ESearchCase::IgnoreCase))
		{
			Out = UVoxelQueryBlueprintLibrary::QueryHeight();
			bOutIsHeight = true;
			return true;
		}
		if (Value->Type == EJson::Number && FMath::IsFinite(Value->AsNumber()))
		{
			Out = UVoxelQueryBlueprintLibrary::MakeConstant(static_cast<float>(Value->AsNumber()));
			return true;
		}
		if (Value->Type == EJson::Object && Value->TryGetObject(Object))
		{
			UVoxelMetadata* Metadata = Load<UVoxelMetadata>(Str(*Object, TEXT("metadata")), OutError);
			if (!Metadata) return false;
			EVoxelTextureChannel Component = EVoxelTextureChannel::R;
			if (!ParseEnum(*Object, TEXT("component"), Component, OutError)) return false;
			Out = UVoxelQueryBlueprintLibrary::QueryMetadata(Metadata, Component);
			return true;
		}
		OutError = FString::Printf(TEXT("%s must be \"height\", a number, or {metadata, component?}"), Field);
		return false;
	}

	FResult ExportToRenderTarget(const FParams& Params)
	{
		FString Err;
		UWorld* World = EditorWorld();
		if (!World) return Error(TEXT("No editor world"));

		bool bHeight = true;
		if (!ParseLayerKind(Params, bHeight, Err)) return Error(Err);
		if (!bHeight) return Error(TEXT("Render target export reads height layers only (ExportVoxelDataToRenderTarget takes a FVoxelStackHeightLayer)"));
		FVoxelStackLayer StackLayer;
		if (!ParseStackLayer(Params, TEXT("stack"), TEXT("layer"), true, StackLayer, Err)) return Error(Err);
		const FVoxelStackHeightLayer HeightLayer(StackLayer.Stack, Cast<UVoxelHeightLayer>(StackLayer.Layer));
		if (!HeightLayer.IsValid()) return Error(TEXT("The stack/layer pair is not a valid height layer"));

		const TSharedPtr<FJsonObject>* Bounds = nullptr;
		double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
		if (!Params->TryGetObjectField(TEXT("bounds"), Bounds) ||
			!(*Bounds)->TryGetNumberField(TEXT("minX"), MinX) || !(*Bounds)->TryGetNumberField(TEXT("minY"), MinY) ||
			!(*Bounds)->TryGetNumberField(TEXT("maxX"), MaxX) || !(*Bounds)->TryGetNumberField(TEXT("maxY"), MaxY))
		{
			return Error(TEXT("bounds {minX,minY,maxX,maxY} is required (world centimetres)"));
		}
		const FVoxelBox2D Zone(FVector2D(MinX, MinY), FVector2D(MaxX, MaxY));
		if (!(MaxX > MinX) || !(MaxY > MinY) || !Zone.IsValidAndNotEmpty())
		{
			return Error(TEXT("bounds must have maxX > minX and maxY > minY"));
		}

		UTextureRenderTarget2D* RenderTarget = Load<UTextureRenderTarget2D>(Str(Params, TEXT("renderTarget")), Err);
		if (!RenderTarget) return Error(Err);
		if (RenderTarget->SizeX <= 0 || RenderTarget->SizeY <= 0) return Error(TEXT("The render target has zero size"));
		const ETextureRenderTargetFormat Format = RenderTarget->RenderTargetFormat;
		if (RenderTarget->OverrideFormat != PF_Unknown &&
			RenderTarget->OverrideFormat != GetPixelFormatFromRenderTargetFormat(Format))
		{
			return Error(TEXT("The render target has OverrideFormat set, which Voxel's export does not support"));
		}
		if (Format == RTF_RGB10A2)
		{
			return Error(TEXT("RTF_RGB10A2 is not supported by Voxel's export. Use R16f/RG16f/RGBA16f/R32f/RG32f/RGBA32f"));
		}

		FVoxelColorQuery Query;
		bool bAnyHeight = false;
		const bool bAnyChannel = Has(Params, TEXT("r")) || Has(Params, TEXT("g")) || Has(Params, TEXT("b")) || Has(Params, TEXT("a"));
		if (!bAnyChannel)
		{
			Query.R = UVoxelQueryBlueprintLibrary::QueryHeight();
			bAnyHeight = true;
		}
		if (!ParseChannel(Params, TEXT("r"), Query.R, bAnyHeight, Err)) return Error(Err);
		if (!ParseChannel(Params, TEXT("g"), Query.G, bAnyHeight, Err)) return Error(Err);
		if (!ParseChannel(Params, TEXT("b"), Query.B, bAnyHeight, Err)) return Error(Err);
		if (!ParseChannel(Params, TEXT("a"), Query.A, bAnyHeight, Err)) return Error(Err);

		const bool b8Bit = Format == RTF_R8 || Format == RTF_RG8 || Format == RTF_RGBA8 || Format == RTF_RGBA8_SRGB;
		if (bAnyHeight && b8Bit)
		{
			return Error(FString::Printf(TEXT("Render target format %s is 8-bit [0,1] and would clamp heights. Use a float format (R16f/RG16f/RGBA16f/R32f/RG32f/RGBA32f)"),
				*EnumName(Format)));
		}

		const bool bWait = Bool(Params, TEXT("wait"), true);
		bool bSuccess = false;
		if (bWait)
		{
			Voxel::ExecuteSynchronously([&]
			{
				return UVoxelQueryBlueprintLibrary::K2_ExportVoxelDataToRenderTarget(bSuccess, World, RenderTarget, HeightLayer, Zone, Query);
			});
			if (!bSuccess)
			{
				return Error(TEXT("ExportVoxelDataToRenderTarget failed; see the Voxel message log"));
			}
		}
		else
		{
			(void)UVoxelQueryBlueprintLibrary::ExportVoxelDataToRenderTarget(World, RenderTarget, HeightLayer, Zone, Query);
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("renderTarget"), RenderTarget->GetPathName());
		Out->SetStringField(TEXT("format"), EnumName(Format));
		Out->SetNumberField(TEXT("sizeX"), RenderTarget->SizeX);
		Out->SetNumberField(TEXT("sizeY"), RenderTarget->SizeY);
		Out->SetNumberField(TEXT("texelSizeX"), (MaxX - MinX) / RenderTarget->SizeX);
		Out->SetNumberField(TEXT("texelSizeY"), (MaxY - MinY) / RenderTarget->SizeY);
		Out->SetStringField(TEXT("world"), World->GetPathName());
		Out->SetBoolField(TEXT("completed"), bWait);
		Out->SetBoolField(TEXT("queued"), !bWait);
		Out->SetStringField(TEXT("note"), TEXT("Render target contents are GPU-side and not saved with the asset."));
		return Ok(Out);
	}
}

void AddSculptHandlers(TArray<FHandlerEntry>& Out)
{
	const auto Number = [](const TCHAR* Name, const TCHAR* Description, double Min, double Max)
	{
		return MCPParam::Optional(Name, EMCPParamType::Number, Description).Range(Min, Max);
	};
	const auto Direction = [](const TCHAR* Name, const TCHAR* Description) { return MCPParam::OptionalField(Name, EMCPParamType::Vec3, Description); };
	const auto Falloff = [] { return MCPParam::OptionalField(TEXT("falloffType"), EMCPParamType::String, TEXT("EVoxelFalloffType; default Smooth."))
		.Enum({ TEXT("None"), TEXT("Linear"), TEXT("Smooth"), TEXT("Spherical"), TEXT("Tip") }); };
	const auto FalloffAmount = [] { return MCPParam::OptionalField(TEXT("falloffAmount"), EMCPParamType::Number,
		TEXT("Brush falloff 0..1; default 0.5. Pass this or the top-level falloff, not both.")).Range(0, 1); };
	const auto Texture = [] { return MCPParam::RequiredField(TEXT("texture"), EMCPParamType::String, TEXT("UVoxelTexture asset path sampled as the brush mask.")); };
	const auto Channel = [] { return MCPParam::OptionalField(TEXT("textureChannel"), EMCPParamType::String, TEXT("Texture channel sampled; default R."))
		.Enum({ TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }); };
	const auto Strokes = [&]
	{
		return TArray<FMCPParamField>{
			Falloff(),
			FalloffAmount(),
			Direction(TEXT("hitNormal"), TEXT("Surface normal the brush aligns to, non-zero; default up {0,0,1}.")),
			Direction(TEXT("strokeDirection"), TEXT("Stroke direction, non-zero; default {1,0,0}.")),
		};
	};
	const auto Brush = [&](const TCHAR* Description)
	{
		TArray<FMCPParamField> Alpha = { Texture(), Channel(),
			MCPParam::OptionalField(TEXT("autoRotate"), EMCPParamType::Boolean, TEXT("Rotate the mask along the stroke; default true.")),
			MCPParam::OptionalField(TEXT("use2DProjection"), EMCPParamType::Boolean, TEXT("Project the mask in 2D; default false.")),
			MCPParam::OptionalField(TEXT("fixedRotation"), EMCPParamType::Number, TEXT("Mask rotation in degrees, -360..360; default 0.")).Range(-360, 360),
		};
		Alpha.Append(Strokes());
		TArray<FMCPParamField> Pattern = { Texture(), Channel(),
			MCPParam::OptionalField(TEXT("centerTextureOnOrigin"), EMCPParamType::Boolean, TEXT("Center the pattern on origin; default false.")),
			MCPParam::OptionalField(TEXT("textureRotation"), EMCPParamType::Number, TEXT("Pattern rotation in degrees, -360..360; default 0.")).Range(-360, 360),
			MCPParam::OptionalField(TEXT("repeatSize"), EMCPParamType::Number, TEXT("World size of one pattern repeat in centimetres, > 0; default 1000.")).Range(UE_KINDA_SMALL_NUMBER, UE_BIG_NUMBER),
			MCPParam::OptionalField(TEXT("origin"), EMCPParamType::Object, TEXT("Pattern origin {x,y} in world centimetres; default {0,0}.")).WithFields({
				MCPParam::RequiredField(TEXT("x"), EMCPParamType::Number, TEXT("World X in centimetres.")),
				MCPParam::RequiredField(TEXT("y"), EMCPParamType::Number, TEXT("World Y in centimetres.")),
			}),
		};
		Pattern.Append(Strokes());
		return MCPParam::Optional(TEXT("brush"), EMCPParamType::Object, Description).Tagged(TEXT("type"), {
			MCPParam::Variant(TEXT("Circular"), TEXT("A round brush shaped by its falloff."), Strokes()),
			MCPParam::Variant(TEXT("Alpha"), TEXT("A texture mask stamped along the stroke."), Alpha),
			MCPParam::Variant(TEXT("Pattern"), TEXT("A texture tiled in world space."), Pattern),
		});
	};
	const auto Sculpt = [](const TCHAR* ActorClass, TArray<FMCPParamSpec> Rest)
	{
		Rest.Insert({
			Spec::ActorPath(*FString::Printf(TEXT("%s actor object path; preferred, since voxel actors relabel themselves."), ActorClass)),
			Spec::ActorLabel(*FString::Printf(TEXT("%s actor label; must match exactly one actor."), ActorClass)),
		}, 0);
		Rest.Append({
			MCPParam::Optional(TEXT("wait"), EMCPParamType::Boolean,
				TEXT("Wait for the edit to finish; default true. false returns queued: true, and the queued edit is covered by neither undo nor the auto-save.")),
			Spec::SaveDirty(),
		});
		return Rest;
	};
	const auto Strength = [&](const TCHAR* Description) { return Number(TEXT("strength"), Description, 0, UE_BIG_NUMBER); };
	const auto Radius = [&](const TCHAR* Description) { return Number(TEXT("radius"), Description, UE_KINDA_SMALL_NUMBER, UE_BIG_NUMBER); };
	const auto Mode = [](const TCHAR* Description) { return MCPParam::Optional(TEXT("mode"), EMCPParamType::String, Description).Enum({ TEXT("Add"), TEXT("Remove") }); };
	const auto LevelType = [] { return MCPParam::Optional(TEXT("levelType"), EMCPParamType::String, TEXT("flatten EVoxelLevelToolType; default Additive."))
		.Enum({ TEXT("Additive"), TEXT("Subtractive"), TEXT("Both") }); };
	const auto SurfaceType = [](const TCHAR* Op) { return MCPParam::Optional(TEXT("surfaceType"), EMCPParamType::String,
		*FString::Printf(TEXT("%s: UVoxelSurfaceTypeInterface asset path to paint; %s needs this and/or metadata."), Op, Op)); };
	const auto Metadata = [](const TCHAR* Op) { return Spec::ValueMap(TEXT("metadata"), false,
		*FString::Printf(TEXT("%s: { UVoxelMetadata asset path: value } to paint; each value a string, number or boolean, parsed as the metadata's type."), Op)); };
	const auto GraphPath = [](const TCHAR* Class) { return MCPParam::Optional(TEXT("graph"), EMCPParamType::String,
		*FString::Printf(TEXT("apply_graph, required: %s asset path."), Class)); };
	const auto GraphParameters = [] { return Spec::ValueMap(TEXT("parameters"), false,
		TEXT("apply_graph: { parameterName: value } overrides on the sculpt graph; each value a string, number or boolean, parsed as the parameter's type.")); };
	const auto Falloff01 = [&](const TCHAR* Description) { return Number(TEXT("falloff"), Description, 0, 1); };

	Out.Add({ TEXT("voxel_height_sculpt"), &HeightSculpt, Sculpt(TEXT("AVoxelSculptHeight"), {
		MCPParam::Required(TEXT("op"), EMCPParamType::String, TEXT("The edit; each op takes only its own fields."))
			.Enum({ TEXT("sculpt_height"), TEXT("flatten"), TEXT("smooth"), TEXT("paint_surface"), TEXT("apply_graph"), TEXT("clear_data"), TEXT("clear_cache") }),
		MCPParam::Optional(TEXT("center"), EMCPParamType::Object, TEXT("Required except for clear_data and clear_cache: the brush center in world centimetres.")).WithFields({
			MCPParam::RequiredField(TEXT("x"), EMCPParamType::Number, TEXT("World X in centimetres.")),
			MCPParam::RequiredField(TEXT("y"), EMCPParamType::Number, TEXT("World Y in centimetres.")),
			MCPParam::OptionalField(TEXT("z"), EMCPParamType::Number, TEXT("Accepted so a full location can be passed; a height sculpt ignores it.")),
		}),
		Radius(TEXT("Brush radius in world centimetres, > 0; default 1000 for smooth, 500 otherwise.")),
		Strength(TEXT("sculpt_height speed (default 0.5), smooth strength (default 1) or paint_surface strength (default 0.05), >= 0.")),
		Mode(TEXT("sculpt_height and paint_surface: EVoxelSculptMode; default Add.")),
		Falloff01(TEXT("0..1. flatten: the flatten falloff, default 0.1. sculpt_height, smooth, paint_surface: the brush falloff unless brush.falloffAmount is set.")),
		Number(TEXT("height"), TEXT("flatten, required: target height, world Z in centimetres."), -UE_BIG_NUMBER, UE_BIG_NUMBER),
		LevelType(),
		SurfaceType(TEXT("paint_surface")),
		Metadata(TEXT("paint_surface")),
		GraphPath(TEXT("UVoxelHeightSculptGraph")),
		GraphParameters(),
		Brush(TEXT("sculpt_height, flatten, smooth, paint_surface: the brush; type picks its fields. Default a Circular brush.")),
	}), Spec::OneActor() });

	Out.Add({ TEXT("voxel_volume_sculpt"), &VolumeSculpt, Sculpt(TEXT("AVoxelSculptVolume"), {
		MCPParam::Required(TEXT("op"), EMCPParamType::String, TEXT("The edit; each op takes only its own fields."))
			.Enum({ TEXT("sphere"), TEXT("cube"), TEXT("flatten"), TEXT("smooth"), TEXT("surface"), TEXT("angle"), TEXT("paint"), TEXT("apply_graph"), TEXT("clear_data"), TEXT("clear_cache") }),
		Spec::Vec3(TEXT("center"), TEXT("Required except for clear_data and clear_cache: the edit center in world centimetres.")),
		Radius(TEXT("Every op but cube: radius in world centimetres, > 0; default 1000 for sphere and smooth, 500 otherwise.")),
		Strength(TEXT("smooth (default 1), surface (default 0.5), angle (default 1) or paint (default 0.05) strength, >= 0.")),
		Mode(TEXT("sphere, cube, surface and paint: EVoxelSculptMode; default Add.")),
		Number(TEXT("smoothness"), TEXT("sphere and cube: add/remove smoothness, >= 0; default 0."), 0, UE_BIG_NUMBER),
		Spec::Vec3(TEXT("size"), TEXT("cube: size in centimetres, each component > 0; default 1000 on every axis.")),
		MCPParam::Optional(TEXT("rotation"), EMCPParamType::Rotator, TEXT("cube and apply_graph: rotation in degrees; default zero.")),
		Number(TEXT("roundness"), TEXT("cube: corner roundness 0..1; default 0."), 0, 1),
		Spec::Vec3(TEXT("normal"), TEXT("flatten: plane normal, non-zero; default up {0,0,1}.")),
		Number(TEXT("height"), TEXT("flatten: distance up and down to sculpt in centimetres, >= 0; default 1000."), 0, UE_BIG_NUMBER),
		Falloff01(TEXT("0..1. flatten: the flatten falloff, default 0.1. smooth, surface, angle, paint: the brush falloff unless brush.falloffAmount is set.")),
		LevelType(),
		Spec::Vec3(TEXT("planePoint"), TEXT("angle: a point on the target plane in world centimetres; default center.")),
		Spec::Vec3(TEXT("planeNormal"), TEXT("angle: target plane normal, non-zero; default up {0,0,1}.")),
		MCPParam::Optional(TEXT("mergeMode"), EMCPParamType::String, TEXT("angle: EVoxelSDFMergeMode; default Override."))
			.Enum({ TEXT("Union"), TEXT("Intersection"), TEXT("Override") }),
		SurfaceType(TEXT("paint")),
		Metadata(TEXT("paint")),
		GraphPath(TEXT("UVoxelVolumeSculptGraph")),
		GraphParameters(),
		Brush(TEXT("smooth, surface, angle, paint: the brush; type picks its fields. Default a Circular brush.")),
	}), Spec::OneActor() });

	Out.Add({ TEXT("voxel_sculpt_asset_get"), &SculptAssetGet, {
		Spec::ActorPath(TEXT("AVoxelSculptHeight or AVoxelSculptVolume actor object path; preferred.")),
		Spec::ActorLabel(TEXT("Sculpt actor label; must match exactly one actor.")),
	}, Spec::OneActor() });

	Out.Add({ TEXT("voxel_sculpt_asset_set"), &SculptAssetSet, {
		Spec::ActorPath(TEXT("AVoxelSculptHeight or AVoxelSculptVolume actor object path; preferred.")),
		Spec::ActorLabel(TEXT("Sculpt actor label; must match exactly one actor.")),
		MCPParam::Required(TEXT("asset"), EMCPParamType::String,
			TEXT("UVoxelSculptHeightAsset (height actor) or UVoxelSculptVolumeAsset (volume actor) path; \"\" or null detaches and keeps the data in the level.")).Nullable(),
		MCPParam::Optional(TEXT("load"), EMCPParamType::Boolean,
			TEXT("When binding: true adopts the asset's data (an empty asset receives the actor's), false overwrites the asset with the actor's data; default true.")),
		Spec::Save(TEXT("Save the asset after load: false writes into it, and the content packages the call dirties; default true. Levels are never saved.")),
	}, Spec::OneActor() });

	const auto StackLayer = [](const TCHAR* LayerDescription)
	{
		return TArray<FMCPParamSpec>{
			MCPParam::Optional(TEXT("stack"), EMCPParamType::String, TEXT("UVoxelLayerStack asset path; default the project default stack.")),
			MCPParam::Optional(TEXT("layer"), EMCPParamType::String, LayerDescription),
		};
	};

	TArray<FMCPParamSpec> QueryParams = StackLayer(TEXT("UVoxelHeightLayer or UVoxelVolumeLayer path matching layerKind; default that kind's default layer."));
	QueryParams.Append({
		MCPParam::Optional(TEXT("layerKind"), EMCPParamType::String, TEXT("Which layer type to sample; default height.")).Enum({ TEXT("height"), TEXT("volume") }),
		MCPParam::Required(TEXT("points"), EMCPParamType::Array, TEXT("1 to 4096 positions in world centimetres: {x,y} for a height layer, {x,y,z} for a volume layer."))
			.Items(EMCPParamType::Object).WithFields({
				MCPParam::RequiredField(TEXT("x"), EMCPParamType::Number, TEXT("World X in centimetres.")),
				MCPParam::RequiredField(TEXT("y"), EMCPParamType::Number, TEXT("World Y in centimetres.")),
				MCPParam::OptionalField(TEXT("z"), EMCPParamType::Number, TEXT("World Z in centimetres; volume layers only, where it is required.")),
			}),
		MCPParam::Optional(TEXT("lod"), EMCPParamType::Integer, TEXT("Query LOD, 0..30; default 0.")).Range(0, MaxQueryLOD),
		MCPParam::Optional(TEXT("querySurface"), EMCPParamType::Boolean,
			TEXT("Resolve surface types and return unresolvedSurfaceType, surfaceType and surfaceWeights per point; default false.")),
		Number(TEXT("gradientStep"), TEXT("Step in centimetres for normals and smart surface types, > 0; default 100."), UE_KINDA_SMALL_NUMBER, UE_BIG_NUMBER),
		MCPParam::Optional(TEXT("metadata"), EMCPParamType::Array, TEXT("Distinct UVoxelMetadata asset paths whose values to return per point.")).Items(EMCPParamType::String),
	});
	Out.Add({ TEXT("voxel_query_layer"), &QueryLayer, QueryParams });

	const auto ChannelSpec = [](const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::Optional(Name, EMCPParamType::Object, Description).WithFields({
			MCPParam::RequiredField(TEXT("metadata"), EMCPParamType::String, TEXT("UVoxelMetadata asset path to sample.")),
			MCPParam::OptionalField(TEXT("component"), EMCPParamType::String, TEXT("Metadata component written; default R.")).Enum({ TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }),
		}).Or(EMCPParamType::String).Or(EMCPParamType::Number);
	};
	TArray<FMCPParamSpec> ExportParams = StackLayer(TEXT("UVoxelHeightLayer asset path; default the default height layer."));
	ExportParams.Append({
		MCPParam::Optional(TEXT("layerKind"), EMCPParamType::String, TEXT("The export reads height layers only.")).Literal(TEXT("height")),
		MCPParam::Required(TEXT("bounds"), EMCPParamType::Object, TEXT("Region sampled over the render target, world centimetres; max > min on both axes.")).WithFields({
			MCPParam::RequiredField(TEXT("minX"), EMCPParamType::Number, TEXT("Lowest world X.")),
			MCPParam::RequiredField(TEXT("minY"), EMCPParamType::Number, TEXT("Lowest world Y.")),
			MCPParam::RequiredField(TEXT("maxX"), EMCPParamType::Number, TEXT("Highest world X.")),
			MCPParam::RequiredField(TEXT("maxY"), EMCPParamType::Number, TEXT("Highest world Y.")),
		}),
		MCPParam::Required(TEXT("renderTarget"), EMCPParamType::String,
			TEXT("UTextureRenderTarget2D asset path; heights need R16f, RG16f, RGBA16f, R32f, RG32f or RGBA32f, and OverrideFormat must be unset.")),
		ChannelSpec(TEXT("r"), TEXT("Red channel: \"height\", a constant number, or {metadata, component?}; default height when no channel is given, else 0.")),
		ChannelSpec(TEXT("g"), TEXT("Green channel: \"height\", a constant number, or {metadata, component?}; default 0.")),
		ChannelSpec(TEXT("b"), TEXT("Blue channel: \"height\", a constant number, or {metadata, component?}; default 0.")),
		ChannelSpec(TEXT("a"), TEXT("Alpha channel: \"height\", a constant number, or {metadata, component?}; default 0.")),
		MCPParam::Optional(TEXT("wait"), EMCPParamType::Boolean, TEXT("Wait for the GPU write and report its failure; default true. false returns queued: true.")),
		Spec::SaveDirty(),
	});
	Out.Add({ TEXT("voxel_export_to_render_target"), &ExportToRenderTarget, ExportParams });
}
}

#undef LOCTEXT_NAMESPACE
