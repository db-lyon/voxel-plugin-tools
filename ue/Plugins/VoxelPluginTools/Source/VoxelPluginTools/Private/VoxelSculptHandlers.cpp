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
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
		{
			UVoxelMetadata* Metadata = Load<UVoxelMetadata>(Pair.Key, OutError);
			if (!Metadata) return false;

			FVoxelMetadataOverride& Override = Out.Overrides.AddDefaulted_GetRef();
			Override.Metadata = Metadata;
			Override.Value = FVoxelPinValue(Metadata->GetInnerType().GetExposedType());
			const FString Text = ValueText(Pair.Value);
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
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
		{
			bool bFound = false;
			FGuid Guid;
			FVoxelParameter Parameter;
			Graph.ForeachParameter([&](const FGuid& InGuid, const FVoxelParameter& InParameter)
			{
				if (!bFound && InParameter.Name.ToString().Equals(Pair.Key, ESearchCase::IgnoreCase))
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
			const FString Text = ValueText(Pair.Value);
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
		Out->SetBoolField(TEXT("undoable"), bUndoable);
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

		static const TCHAR* Ops = TEXT("sculpt_height, flatten, smooth, paint_surface, apply_graph, clear_data, clear_cache");
		if (Op != TEXT("sculpt_height") && Op != TEXT("flatten") && Op != TEXT("smooth") &&
			Op != TEXT("paint_surface") && Op != TEXT("apply_graph"))
		{
			return Error(FString::Printf(TEXT("op '%s' is not valid. Use one of: %s"), *Op, Ops));
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

		static const TCHAR* Ops = TEXT("sphere, cube, flatten, smooth, surface, angle, paint, apply_graph, clear_data, clear_cache");
		static const TSet<FString> Known = { TEXT("sphere"), TEXT("cube"), TEXT("flatten"), TEXT("smooth"), TEXT("surface"), TEXT("angle"), TEXT("paint"), TEXT("apply_graph") };
		if (!Known.Contains(Op))
		{
			return Error(FString::Printf(TEXT("op '%s' is not valid. Use one of: %s"), *Op, Ops));
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
			if (!bOk || !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
			{
				return Error(FString::Printf(TEXT("points[%d] must be %s"), Index, bHeight ? TEXT("{x,y}") : TEXT("{x,y,z}")));
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
				UVoxelMetadata* Metadata = Load<UVoxelMetadata>(ValueText(Value), Err);
				if (!Metadata) return Error(Err);
				Metadatas.Add(Metadata);
			}
		}
		const TVoxelArray<FVoxelMetadataRef> MetadataRefs = FVoxelMetadataRef::GetUniqueValidRefs(Metadatas);

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
		FString Text;
		double Number = 0;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetString(Text) && Text.Equals(TEXT("height"), ESearchCase::IgnoreCase))
		{
			Out = UVoxelQueryBlueprintLibrary::QueryHeight();
			bOutIsHeight = true;
			return true;
		}
		if (Value->TryGetNumber(Number) && FMath::IsFinite(Number))
		{
			Out = UVoxelQueryBlueprintLibrary::MakeConstant(static_cast<float>(Number));
			return true;
		}
		if (Value->TryGetObject(Object))
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
	Out.Append(
	{
		{ TEXT("voxel_height_sculpt"), &HeightSculpt },
		{ TEXT("voxel_volume_sculpt"), &VolumeSculpt },
		{ TEXT("voxel_sculpt_asset_get"), &SculptAssetGet },
		{ TEXT("voxel_sculpt_asset_set"), &SculptAssetSet },
		{ TEXT("voxel_query_layer"), &QueryLayer },
		{ TEXT("voxel_export_to_render_target"), &ExportToRenderTarget },
	});
}
}

#undef LOCTEXT_NAMESPACE
