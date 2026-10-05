#include "VoxelToolsCommon.h"

#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#include "VoxelGraph.h"
#include "VoxelLayer.h"
#include "VoxelParameter.h"
#include "VoxelParameterOverridesOwner.h"
#include "VoxelPinType.h"
#include "VoxelPinValue.h"
#include "VoxelStampRef.h"
#include "VoxelStampComponent.h"
#include "VoxelInstancedStampComponent.h"
#include "VoxelHeightStamp.h"
#include "VoxelVolumeStamp.h"
#include "Graphs/VoxelHeightGraph.h"
#include "Graphs/VoxelVolumeGraph.h"
#include "Graphs/VoxelHeightGraphStamp.h"
#include "Graphs/VoxelVolumeGraphStamp.h"
#include "Heightmap/VoxelHeightmap.h"
#include "Heightmap/VoxelHeightmapStamp.h"
#include "StaticMesh/VoxelStaticMesh.h"
#include "StaticMesh/VoxelMeshStamp.h"
#include "Spline/VoxelHeightSplineGraph.h"
#include "Spline/VoxelVolumeSplineGraph.h"
#include "Spline/VoxelHeightSplineStamp.h"
#include "Spline/VoxelVolumeSplineStamp.h"
#include "Surface/VoxelSurfaceTypeInterface.h"

#define LOCTEXT_NAMESPACE "VoxelPluginTools"

namespace VoxelPluginTools
{
namespace
{
	// --------------------------------------------------------------------------------
	// Stamp kinds

	enum class EKind : uint8
	{
		HeightGraph,
		VolumeGraph,
		Heightmap,
		Mesh,
		HeightSpline,
		VolumeSpline,
	};

	struct FKindInfo
	{
		EKind Kind;
		const TCHAR* Name;
		// Spline stamps need a spline component, which only UVoxelStampComponent creates (FixupComponents).
		bool bSpline;
	};

	const FKindInfo GKinds[] =
	{
		{ EKind::HeightGraph, TEXT("height_graph"), false },
		{ EKind::VolumeGraph, TEXT("volume_graph"), false },
		{ EKind::Heightmap, TEXT("heightmap"), false },
		{ EKind::Mesh, TEXT("mesh"), false },
		{ EKind::HeightSpline, TEXT("height_spline"), true },
		{ EKind::VolumeSpline, TEXT("volume_spline"), true },
	};

	FString KindNames()
	{
		TArray<FString> Names;
		for (const FKindInfo& Info : GKinds)
		{
			Names.Add(Info.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}

	const FKindInfo* FindKind(const FParams& Params, FString& OutError)
	{
		const FString Name = Str(Params, TEXT("kind"));
		for (const FKindInfo& Info : GKinds)
		{
			if (Name.Equals(Info.Name, ESearchCase::IgnoreCase))
			{
				return &Info;
			}
		}
		OutError = Name.IsEmpty()
			? FString::Printf(TEXT("kind is required: %s"), *KindNames())
			: FString::Printf(TEXT("Unknown kind '%s'. Use %s"), *Name, *KindNames());
		return nullptr;
	}

	template<typename T>
	struct TTag
	{
		using Type = T;
	};

	template<typename FnType>
	FResult Dispatch(EKind Kind, FnType&& Fn)
	{
		switch (Kind)
		{
		case EKind::HeightGraph: return Fn(TTag<FVoxelHeightGraphStamp>());
		case EKind::VolumeGraph: return Fn(TTag<FVoxelVolumeGraphStamp>());
		case EKind::Heightmap: return Fn(TTag<FVoxelHeightmapStamp>());
		case EKind::Mesh: return Fn(TTag<FVoxelMeshStamp>());
		case EKind::HeightSpline: return Fn(TTag<FVoxelHeightSplineStamp>());
		case EKind::VolumeSpline: return Fn(TTag<FVoxelVolumeSplineStamp>());
		}
		return Error(TEXT("Unhandled stamp kind"));
	}

	const TCHAR* KindOf(const FVoxelStamp& Stamp)
	{
		if (Stamp.As<FVoxelHeightGraphStamp>()) return TEXT("height_graph");
		if (Stamp.As<FVoxelVolumeGraphStamp>()) return TEXT("volume_graph");
		if (Stamp.As<FVoxelHeightmapStamp>()) return TEXT("heightmap");
		if (Stamp.As<FVoxelMeshStamp>()) return TEXT("mesh");
		if (Stamp.As<FVoxelHeightSplineStamp>()) return TEXT("height_spline");
		if (Stamp.As<FVoxelVolumeSplineStamp>()) return TEXT("volume_spline");
		return TEXT("other");
	}

	const IVoxelParameterOverridesOwner* OverridesOwner(const FVoxelStamp& Stamp)
	{
		if (const FVoxelHeightGraphStamp* S = Stamp.As<FVoxelHeightGraphStamp>()) return S;
		if (const FVoxelVolumeGraphStamp* S = Stamp.As<FVoxelVolumeGraphStamp>()) return S;
		if (const FVoxelHeightSplineStamp* S = Stamp.As<FVoxelHeightSplineStamp>()) return S;
		if (const FVoxelVolumeSplineStamp* S = Stamp.As<FVoxelVolumeSplineStamp>()) return S;
		return nullptr;
	}

	// The asset each stamp kind is driven by.
	template<typename StampType>
	struct TStampAsset;

	template<>
	struct TStampAsset<FVoxelHeightGraphStamp>
	{
		using FAsset = UVoxelHeightGraph;
		static UObject* Get(const FVoxelHeightGraphStamp& Stamp) { return Stamp.Graph.Get(); }
		static void Set(FVoxelHeightGraphStamp& Stamp, FAsset* Asset) { Stamp.Graph = Asset; }
	};
	template<>
	struct TStampAsset<FVoxelVolumeGraphStamp>
	{
		using FAsset = UVoxelVolumeGraph;
		static UObject* Get(const FVoxelVolumeGraphStamp& Stamp) { return Stamp.Graph.Get(); }
		static void Set(FVoxelVolumeGraphStamp& Stamp, FAsset* Asset) { Stamp.Graph = Asset; }
	};
	template<>
	struct TStampAsset<FVoxelHeightmapStamp>
	{
		using FAsset = UVoxelHeightmap;
		static UObject* Get(const FVoxelHeightmapStamp& Stamp) { return Stamp.Heightmap.Get(); }
		static void Set(FVoxelHeightmapStamp& Stamp, FAsset* Asset) { Stamp.Heightmap = Asset; }
	};
	template<>
	struct TStampAsset<FVoxelMeshStamp>
	{
		using FAsset = UVoxelStaticMesh;
		static UObject* Get(const FVoxelMeshStamp& Stamp) { return Stamp.NewMesh.Get(); }
		static void Set(FVoxelMeshStamp& Stamp, FAsset* Asset)
		{
			Stamp.NewMesh = Asset;
			// FixupProperties migrates a legacy UStaticMesh into NewMesh and ensures NewMesh is empty when it does.
			Stamp.Mesh = nullptr;
		}
	};
	template<>
	struct TStampAsset<FVoxelHeightSplineStamp>
	{
		using FAsset = UVoxelHeightSplineGraph;
		static UObject* Get(const FVoxelHeightSplineStamp& Stamp) { return Stamp.Graph.Get(); }
		static void Set(FVoxelHeightSplineStamp& Stamp, FAsset* Asset) { Stamp.Graph = Asset; }
	};
	template<>
	struct TStampAsset<FVoxelVolumeSplineStamp>
	{
		using FAsset = UVoxelVolumeSplineGraph;
		static UObject* Get(const FVoxelVolumeSplineStamp& Stamp) { return Stamp.Graph.Get(); }
		static void Set(FVoxelVolumeSplineStamp& Stamp, FAsset* Asset) { Stamp.Graph = Asset; }
	};

	// --------------------------------------------------------------------------------
	// Field parsing (validates only; callers mutate a local copy)

	template<typename EnumType>
	bool ParseEnum(const FString& Text, const TCHAR* Field, EnumType& Out, FString& OutError)
	{
		const UEnum* Enum = StaticEnum<EnumType>();
		TArray<FString> Names;
		for (int32 Index = 0; Index < Enum->NumEnums(); Index++)
		{
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.EndsWith(TEXT("_MAX")) || Name == TEXT("None"))
			{
				continue;
			}
			if (Name.Equals(Text, ESearchCase::IgnoreCase))
			{
				Out = static_cast<EnumType>(Enum->GetValueByIndex(Index));
				return true;
			}
			Names.Add(Name);
		}
		OutError = FString::Printf(TEXT("%s '%s' is not one of: %s"), Field, *Text, *FString::Join(Names, TEXT(", ")));
		return false;
	}

	template<typename EnumType>
	FString EnumName(EnumType Value)
	{
		return StaticEnum<EnumType>()->GetNameStringByValue(static_cast<int64>(Value));
	}

	bool ParseInt(const FParams& Params, const TCHAR* Field, int32& Out, FString& OutError)
	{
		double Value = 0;
		if (!Params->TryGetNumberField(Field, Value) || Value != FMath::RoundToDouble(Value) ||
			Value < double(MIN_int32) || Value > double(MAX_int32))
		{
			OutError = FString::Printf(TEXT("%s must be an integer"), Field);
			return false;
		}
		Out = static_cast<int32>(Value);
		return true;
	}

	bool ParseNonNegative(const FParams& Params, const TCHAR* Field, float& Out, FString& OutError)
	{
		double Value = 0;
		if (!Params->TryGetNumberField(Field, Value) || Value < 0)
		{
			OutError = FString::Printf(TEXT("%s must be a number >= 0"), Field);
			return false;
		}
		Out = static_cast<float>(Value);
		return true;
	}

	bool ParseBool(const FParams& Params, const TCHAR* Field, bool& Out, FString& OutError)
	{
		if (!Params->TryGetBoolField(Field, Out))
		{
			OutError = FString::Printf(TEXT("%s must be a boolean"), Field);
			return false;
		}
		return true;
	}

	// Empty string clears the reference.
	template<typename T>
	bool ParseOptionalAsset(const FParams& Params, const TCHAR* Field, TObjectPtr<T>& Out, FString& OutError)
	{
		const FString Path = Str(Params, Field);
		if (Path.IsEmpty())
		{
			Out = nullptr;
			return true;
		}
		T* Asset = Load<T>(Path, OutError);
		if (!Asset)
		{
			return false;
		}
		Out = Asset;
		return true;
	}

	bool FindGraphParameter(const UVoxelGraph& Graph, const FString& Name, FGuid& OutGuid, FVoxelParameter& OutParameter)
	{
		bool bFound = false;
		Graph.ForeachParameter([&](const FGuid& Guid, const FVoxelParameter& Parameter)
		{
			if (!bFound && Parameter.Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				OutGuid = Guid;
				OutParameter = Parameter;
				bFound = true;
			}
		});
		return bFound;
	}

	bool ApplyOverrides(IVoxelParameterOverridesOwner& Owner, const FParams& Params, FString& OutError)
	{
		if (!Has(Params, TEXT("parameters")))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (!Params->TryGetObjectField(TEXT("parameters"), Values))
		{
			OutError = TEXT("parameters must be an object: { parameterName: value }");
			return false;
		}
		const UVoxelGraph* Graph = Owner.GetGraph();
		if (!Graph)
		{
			OutError = TEXT("parameters needs a graph; pass asset");
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
		{
			FGuid Guid;
			FVoxelParameter Parameter;
			if (!FindGraphParameter(*Graph, Pair.Key, Guid, Parameter))
			{
				OutError = FString::Printf(TEXT("Graph %s has no parameter '%s'"), *Graph->GetName(), *Pair.Key);
				return false;
			}
			const FString Text = ValueText(Pair.Value);
			FVoxelPinValue Value(Parameter.Type.GetExposedType());
			if (!ParseValue(Value, Text))
			{
				OutError = FString::Printf(TEXT("'%s' does not parse as %s for parameter '%s'"), *Text, *Parameter.Type.ToString(), *Pair.Key);
				return false;
			}
			FVoxelParameterValueOverride& Override = Owner.GetGuidToValueOverride().FindOrAdd(Guid);
			Override.bEnable = true;
			Override.Value = Value;
			Override.CachedName = Parameter.Name;
		}
		return true;
	}

	// Layer, blend mode and layer-type padding shared by every height or volume stamp.
	template<typename StampType>
	bool ApplyLayerFields(StampType& Stamp, const FParams& Params, FString& OutError)
	{
		if constexpr (std::derived_from<StampType, FVoxelHeightStamp>)
		{
			const FString LayerPath = Str(Params, TEXT("layer"));
			if (!LayerPath.IsEmpty())
			{
				UVoxelHeightLayer* Layer = Load<UVoxelHeightLayer>(LayerPath, OutError);
				if (!Layer) return false;
				Stamp.Layer = Layer;
			}
			if (!Stamp.Layer)
			{
				Stamp.Layer = UVoxelHeightLayer::Default();
			}
			if (Has(Params, TEXT("blendMode")) && !ParseEnum(Str(Params, TEXT("blendMode")), TEXT("blendMode"), Stamp.BlendMode, OutError)) return false;
			if (Has(Params, TEXT("heightPaddingMultiplier")) && !ParseNonNegative(Params, TEXT("heightPaddingMultiplier"), Stamp.HeightPaddingMultiplier, OutError)) return false;
		}
		else
		{
			static_assert(std::derived_from<StampType, FVoxelVolumeStamp>);
			const FString LayerPath = Str(Params, TEXT("layer"));
			if (!LayerPath.IsEmpty())
			{
				UVoxelVolumeLayer* Layer = Load<UVoxelVolumeLayer>(LayerPath, OutError);
				if (!Layer) return false;
				Stamp.Layer = Layer;
			}
			if (!Stamp.Layer)
			{
				Stamp.Layer = UVoxelVolumeLayer::Default();
			}
			if (Has(Params, TEXT("blendMode")) && !ParseEnum(Str(Params, TEXT("blendMode")), TEXT("blendMode"), Stamp.BlendMode, OutError)) return false;
			if (Has(Params, TEXT("boundsExtensionMultiplier")) && !ParseNonNegative(Params, TEXT("boundsExtensionMultiplier"), Stamp.BoundsExtensionMultiplier, OutError)) return false;
			if (Has(Params, TEXT("maximumBoundsExtension")) && !ParseNonNegative(Params, TEXT("maximumBoundsExtension"), Stamp.MaximumBoundsExtension, OutError)) return false;
		}
		if (!Stamp.Layer)
		{
			// VoxelStampManager skips stamps whose layer is null.
			OutError = TEXT("No layer given and the project has no default layer of this type");
			return false;
		}
		return true;
	}

	// Applies every supplied field to a local stamp copy. Nothing outside the copy is touched.
	template<typename StampType>
	bool Configure(StampType& Stamp, const FParams& Params, FString& OutError, bool& bOutAssetChanged)
	{
		using FTraits = TStampAsset<StampType>;
		bOutAssetChanged = false;

		const FString AssetPath = Str(Params, TEXT("asset"));
		if (!AssetPath.IsEmpty())
		{
			typename FTraits::FAsset* Asset = Load<typename FTraits::FAsset>(AssetPath, OutError);
			if (!Asset) return false;
			bOutAssetChanged = FTraits::Get(Stamp) != Asset;
			FTraits::Set(Stamp, Asset);
		}
		if (!FTraits::Get(Stamp))
		{
			OutError = FString::Printf(TEXT("asset is required (a %s path)"), *FTraits::FAsset::StaticClass()->GetName());
			return false;
		}

		if constexpr (std::derived_from<StampType, IVoxelParameterOverridesOwner>)
		{
			IVoxelParameterOverridesOwner& Owner = Stamp;
			if (bOutAssetChanged)
			{
				// Overrides are keyed by the previous graph's parameter GUIDs.
				Owner.GetGuidToValueOverride().Empty();
			}
			if (!ApplyOverrides(Owner, Params, OutError)) return false;
		}
		else if (Has(Params, TEXT("parameters")))
		{
			OutError = TEXT("parameters only applies to graph and spline kinds");
			return false;
		}

		if (!ApplyLayerFields(Stamp, Params, OutError)) return false;

		if (Has(Params, TEXT("priority")) && !ParseInt(Params, TEXT("priority"), Stamp.Priority, OutError)) return false;
		if (Has(Params, TEXT("smoothness")) && !ParseNonNegative(Params, TEXT("smoothness"), Stamp.Smoothness, OutError)) return false;
		if (Has(Params, TEXT("behavior")) && !ParseEnum(Str(Params, TEXT("behavior")), TEXT("behavior"), Stamp.Behavior, OutError)) return false;
		if (Has(Params, TEXT("applyOnVoid")) && !ParseBool(Params, TEXT("applyOnVoid"), Stamp.bApplyOnVoid, OutError)) return false;

		const bool bSurface = Has(Params, TEXT("surfaceType"));
		if constexpr (std::is_same_v<StampType, FVoxelHeightmapStamp>)
		{
			if (bSurface && !ParseOptionalAsset(Params, TEXT("surfaceType"), Stamp.DefaultSurfaceType, OutError)) return false;
		}
		else if constexpr (std::is_same_v<StampType, FVoxelMeshStamp>)
		{
			if (bSurface && !ParseOptionalAsset(Params, TEXT("surfaceType"), Stamp.SurfaceType, OutError)) return false;
			if (Has(Params, TEXT("useTricubic")) && !ParseBool(Params, TEXT("useTricubic"), Stamp.bUseTricubic, OutError)) return false;
		}
		else if (bSurface)
		{
			OutError = TEXT("surfaceType only applies to heightmap and mesh kinds");
			return false;
		}
		if constexpr (!std::is_same_v<StampType, FVoxelMeshStamp>)
		{
			if (Has(Params, TEXT("useTricubic")))
			{
				OutError = TEXT("useTricubic only applies to the mesh kind");
				return false;
			}
		}
		return true;
	}

	// --------------------------------------------------------------------------------
	// Read-back

	FString PathOf(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	TSharedRef<FJsonObject> StampJson(const FVoxelStamp& Stamp)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("kind"), KindOf(Stamp));
		Out->SetStringField(TEXT("struct"), Stamp.GetStruct()->GetName());
		Out->SetStringField(TEXT("asset"), PathOf(Stamp.GetAsset()));
		Out->SetNumberField(TEXT("priority"), Stamp.Priority);
		Out->SetNumberField(TEXT("smoothness"), Stamp.Smoothness);
		Out->SetStringField(TEXT("behavior"), EnumName(Stamp.Behavior));
		Out->SetBoolField(TEXT("applyOnVoid"), Stamp.bApplyOnVoid);

		if (const FVoxelHeightStamp* Height = Stamp.As<FVoxelHeightStamp>())
		{
			Out->SetStringField(TEXT("layerType"), TEXT("height"));
			Out->SetStringField(TEXT("layer"), PathOf(Height->Layer.Get()));
			Out->SetStringField(TEXT("blendMode"), EnumName(Height->BlendMode));
			Out->SetNumberField(TEXT("heightPaddingMultiplier"), Height->HeightPaddingMultiplier);
			TArray<TSharedPtr<FJsonValue>> Additional;
			for (const TObjectPtr<UVoxelHeightLayer>& Layer : Height->AdditionalLayers)
			{
				Additional.Add(MakeShared<FJsonValueString>(PathOf(Layer.Get())));
			}
			Out->SetArrayField(TEXT("additionalLayers"), Additional);
		}
		else if (const FVoxelVolumeStamp* Volume = Stamp.As<FVoxelVolumeStamp>())
		{
			Out->SetStringField(TEXT("layerType"), TEXT("volume"));
			Out->SetStringField(TEXT("layer"), PathOf(Volume->Layer.Get()));
			Out->SetStringField(TEXT("blendMode"), EnumName(Volume->BlendMode));
			Out->SetNumberField(TEXT("boundsExtensionMultiplier"), Volume->BoundsExtensionMultiplier);
			Out->SetNumberField(TEXT("maximumBoundsExtension"), Volume->MaximumBoundsExtension);
			TArray<TSharedPtr<FJsonValue>> Additional;
			for (const TObjectPtr<UVoxelVolumeLayer>& Layer : Volume->AdditionalLayers)
			{
				Additional.Add(MakeShared<FJsonValueString>(PathOf(Layer.Get())));
			}
			Out->SetArrayField(TEXT("additionalLayers"), Additional);
		}

		if (const FVoxelHeightmapStamp* Heightmap = Stamp.As<FVoxelHeightmapStamp>())
		{
			Out->SetStringField(TEXT("surfaceType"), PathOf(Heightmap->DefaultSurfaceType.Get()));
		}
		else if (const FVoxelMeshStamp* Mesh = Stamp.As<FVoxelMeshStamp>())
		{
			Out->SetStringField(TEXT("surfaceType"), PathOf(Mesh->SurfaceType.Get()));
			Out->SetBoolField(TEXT("useTricubic"), Mesh->bUseTricubic);
		}

		if (const IVoxelParameterOverridesOwner* Owner = OverridesOwner(Stamp))
		{
			TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
			for (const TPair<FGuid, FVoxelParameterValueOverride>& Pair : Owner->GetGuidToValueOverride())
			{
				if (Pair.Value.bEnable)
				{
					Overrides->SetStringField(Pair.Value.CachedName.ToString(), Pair.Value.Value.ExportToString());
				}
			}
			Out->SetObjectField(TEXT("overrides"), Overrides);
		}
		return Out;
	}

	TSharedRef<FJsonObject> ComponentJson(const AActor& Actor, const USceneComponent& Component, const FVoxelStampRef& Ref)
	{
		TSharedRef<FJsonObject> Out = ActorJson(Actor);
		Out->SetStringField(TEXT("componentName"), Component.GetName());
		Out->SetBoolField(TEXT("hasStamp"), Ref.IsValid());
		if (Ref.IsValid())
		{
			Out->SetObjectField(TEXT("stamp"), StampJson(*Ref));
		}
		return Out;
	}

	// --------------------------------------------------------------------------------
	// voxel_stamp_set / voxel_stamp_read

	FResult StampSet(const FParams& Params)
	{
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Error(Err);
		UVoxelStampComponent* Component = FindComponent<UVoxelStampComponent>(*Actor, Params, Err);
		if (!Component) return Error(Err);
		const FKindInfo* Kind = FindKind(Params, Err);
		if (!Kind) return Error(Err);

		return Dispatch(Kind->Kind, [&](auto Tag) -> FResult
		{
			using StampType = typename decltype(Tag)::Type;

			const FVoxelStampRef Current = Component->GetStamp();
			const StampType* Existing = Current.As<StampType>();
			const FString PreviousKind = Current.IsValid() ? FString(KindOf(*Current)) : FString();

			// Start from the current stamp when it is the same kind so unspecified fields keep their values.
			StampType Stamp = Existing ? *Existing : StampType();
			bool bAssetChanged = false;
			FString ConfigureError;
			if (!Configure(Stamp, Params, ConfigureError, bAssetChanged)) return Error(ConfigureError);

			// A stamp component always uses its own transform; a non-identity stamp transform only logs a warning.
			Stamp.Transform = FTransform::Identity;

			const FScopedTransaction Transaction(LOCTEXT("SetStamp", "Set Voxel Stamp"));
			Actor->Modify();
			Component->Modify();
			Component->SetStamp(Stamp);

			const FVoxelStampRef Stored = Component->GetStamp();
			if (!Stored.As<StampType>()) return Error(TEXT("The component did not keep the stamp"));

			TSharedRef<FJsonObject> Out = ComponentJson(*Actor, *Component, Stored);
			Out->SetBoolField(TEXT("changed"), true);
			Out->SetBoolField(TEXT("preserved"), Existing != nullptr);
			if (!Existing && !PreviousKind.IsEmpty())
			{
				Out->SetStringField(TEXT("replacedKind"), PreviousKind);
			}
			if (bAssetChanged && OverridesOwner(*Stored) && Existing)
			{
				Out->SetBoolField(TEXT("overridesReset"), true);
			}
			return Ok(Out);
		});
	}

	FResult StampRead(const FParams& Params)
	{
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Error(Err);
		UVoxelStampComponent* Component = FindComponent<UVoxelStampComponent>(*Actor, Params, Err);
		if (!Component) return Error(Err);
		return Ok(ComponentJson(*Actor, *Component, Component->GetStamp()));
	}

	// --------------------------------------------------------------------------------
	// voxel_instanced_stamps

	bool ParseInstanceTransforms(const FParams& Params, const USceneComponent& Component, TArray<FTransform>& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(TEXT("transforms"), Values) || Values->Num() == 0)
		{
			OutError = TEXT("transforms is required: [{ location: {x,y,z}, rotation?: {pitch,yaw,roll}, scale?: number | {x,y,z} }]");
			return false;
		}
		const bool bRelative = Bool(Params, TEXT("relativeToComponent"), false);
		const FTransform ComponentTransform = Component.GetComponentTransform();
		for (int32 Index = 0; Index < Values->Num(); Index++)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetObject(Object))
			{
				OutError = FString::Printf(TEXT("transforms[%d] is not an object"), Index);
				return false;
			}
			const FParams Entry = *Object;
			FVector Location;
			if (!Vec(Entry, TEXT("location"), Location))
			{
				OutError = FString::Printf(TEXT("transforms[%d].location {x,y,z} is required"), Index);
				return false;
			}
			FRotator Rotation = FRotator::ZeroRotator;
			if (Has(Entry, TEXT("rotation")) && !Rot(Entry, TEXT("rotation"), Rotation))
			{
				OutError = FString::Printf(TEXT("transforms[%d].rotation must be {pitch,yaw,roll}"), Index);
				return false;
			}
			FVector Scale = FVector::OneVector;
			if (Has(Entry, TEXT("scale")))
			{
				double Uniform = 0;
				if (Entry->TryGetNumberField(TEXT("scale"), Uniform))
				{
					Scale = FVector(Uniform);
				}
				else if (!Vec(Entry, TEXT("scale"), Scale))
				{
					OutError = FString::Printf(TEXT("transforms[%d].scale must be a number or {x,y,z}"), Index);
					return false;
				}
			}
			if (FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y) || FMath::IsNearlyZero(Scale.Z))
			{
				OutError = FString::Printf(TEXT("transforms[%d].scale has a zero component"), Index);
				return false;
			}
			const FTransform Local(Rotation, Location, Scale);
			// Instanced stamp transforms are world space (the stamp runtime reads Stamp->Transform directly).
			Out.Add(bRelative ? Local * ComponentTransform : Local);
		}
		return true;
	}

	bool ParseIndex(const FParams& Params, const TCHAR* Field, int32 Num, int32& Out, FString& OutError)
	{
		if (!Has(Params, Field))
		{
			OutError = FString::Printf(TEXT("%s is required"), Field);
			return false;
		}
		if (!ParseInt(Params, Field, Out, OutError)) return false;
		if (Out < 0 || Out >= Num)
		{
			OutError = FString::Printf(TEXT("%s %d is out of range [0, %d)"), Field, Out, Num);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> InstancedJson(const AActor& Actor, UVoxelInstancedStampComponent& Component, bool bIncludeStamps)
	{
		TSharedRef<FJsonObject> Out = ActorJson(Actor);
		Out->SetStringField(TEXT("componentName"), Component.GetName());
		const int32 Num = Component.NumStamps();
		int32 NumValid = 0;
		TArray<TSharedPtr<FJsonValue>> Stamps;
		for (int32 Index = 0; Index < Num; Index++)
		{
			const FVoxelStampRef Ref = Component.GetStamp(Index);
			if (!Ref.IsValid())
			{
				continue;
			}
			NumValid++;
			if (bIncludeStamps)
			{
				TSharedRef<FJsonObject> Entry = StampJson(*Ref);
				Entry->SetNumberField(TEXT("index"), Index);
				Entry->SetObjectField(TEXT("location"), VecJson(Ref->Transform.GetLocation()));
				const FRotator Rotation = Ref->Transform.Rotator();
				TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
				RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
				RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
				RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
				Entry->SetObjectField(TEXT("rotation"), RotationJson);
				Entry->SetObjectField(TEXT("scale"), VecJson(Ref->Transform.GetScale3D()));
				Stamps.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		// Removed slots stay as empty entries; indices never shift.
		Out->SetNumberField(TEXT("count"), Num);
		Out->SetNumberField(TEXT("validCount"), NumValid);
		if (bIncludeStamps)
		{
			Out->SetArrayField(TEXT("stamps"), Stamps);
		}
		return Out;
	}

	FResult InstancedStamps(const FParams& Params)
	{
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Error(Err);
		UVoxelInstancedStampComponent* Component = FindComponent<UVoxelInstancedStampComponent>(*Actor, Params, Err);
		if (!Component) return Error(Err);

		const FString Op = Str(Params, TEXT("op")).ToLower();
		const bool bIncludeStamps = Bool(Params, TEXT("includeStamps"), false);

		if (Op == TEXT("count"))
		{
			return Ok(InstancedJson(*Actor, *Component, bIncludeStamps));
		}

		if (Op == TEXT("clear"))
		{
			const FScopedTransaction Transaction(LOCTEXT("ClearInstancedStamps", "Clear Voxel Instanced Stamps"));
			Component->Modify();
			Component->ClearStamps();
			TSharedRef<FJsonObject> Out = InstancedJson(*Actor, *Component, bIncludeStamps);
			Out->SetBoolField(TEXT("changed"), true);
			return Ok(Out);
		}

		if (Op == TEXT("remove"))
		{
			int32 Index = 0;
			if (!ParseIndex(Params, TEXT("index"), Component->NumStamps(), Index, Err)) return Error(Err);
			if (!Component->GetStamp(Index).IsValid()) return Error(FString::Printf(TEXT("Stamp %d is already removed"), Index));
			const FScopedTransaction Transaction(LOCTEXT("RemoveInstancedStamp", "Remove Voxel Instanced Stamp"));
			Component->Modify();
			Component->RemoveStamp(Index);
			TSharedRef<FJsonObject> Out = InstancedJson(*Actor, *Component, bIncludeStamps);
			Out->SetBoolField(TEXT("changed"), true);
			return Ok(Out);
		}

		if (Op == TEXT("update"))
		{
			const int32 Num = Component->NumStamps();
			TArray<int32> Indices;
			if (Has(Params, TEXT("indices")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Params->TryGetArrayField(TEXT("indices"), Values)) return Error(TEXT("indices must be an array of integers"));
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					double Number = 0;
					if (!Value.IsValid() || !Value->TryGetNumber(Number) || Number != FMath::RoundToDouble(Number) || Number < 0 || Number >= Num)
					{
						return Error(FString::Printf(TEXT("indices entries must be integers in [0, %d)"), Num));
					}
					Indices.AddUnique(static_cast<int32>(Number));
				}
			}
			else
			{
				for (int32 Index = 0; Index < Num; Index++)
				{
					Indices.Add(Index);
				}
			}
			for (const int32 Index : Indices)
			{
				// UpdateStamp(Index) only (un)registers; FVoxelStampRef::Update re-evaluates a registered stamp.
				const FVoxelStampRef Ref = Component->GetStamp(Index);
				if (Ref.IsValid() && Ref.IsRegistered())
				{
					Ref.Update();
				}
				else
				{
					Component->UpdateStamp(Index);
				}
			}
			TSharedRef<FJsonObject> Out = InstancedJson(*Actor, *Component, bIncludeStamps);
			Out->SetNumberField(TEXT("updated"), Indices.Num());
			return Ok(Out);
		}

		if (Op == TEXT("add"))
		{
			const FKindInfo* Kind = FindKind(Params, Err);
			if (!Kind) return Error(Err);
			if (Kind->bSpline)
			{
				return Error(FString::Printf(TEXT("%s stamps need a spline component, which instanced stamps do not get; use voxel_stamp_set on a stamp actor"), Kind->Name));
			}
			TArray<FTransform> Transforms;
			if (!ParseInstanceTransforms(Params, *Component, Transforms, Err)) return Error(Err);

			return Dispatch(Kind->Kind, [&](auto Tag) -> FResult
			{
				using StampType = typename decltype(Tag)::Type;

				StampType Stamp;
				bool bAssetChanged = false;
				FString ConfigureError;
				if (!Configure(Stamp, Params, ConfigureError, bAssetChanged)) return Error(ConfigureError);
				TVoxelArray<FVoxelStampRef> NewStamps;
				NewStamps.Reserve(Transforms.Num());
				for (const FTransform& Transform : Transforms)
				{
					Stamp.Transform = Transform;
					const FVoxelStampRef Ref = FVoxelStampRef::New(Stamp);
					// As UVoxelStampComponent::SetStamp does; AddStamp skips it. Must run on the shared copy (graph stamps bind SharedThis).
					Ref->FixupProperties();
					NewStamps.Add(Ref);
				}

				const int32 FirstIndex = Component->NumStamps();
				const FScopedTransaction Transaction(LOCTEXT("AddInstancedStamps", "Add Voxel Instanced Stamps"));
				Component->Modify();
				Component->Reserve(FirstIndex + NewStamps.Num());
				Component->AddStamps_NoCopy(MoveTemp(NewStamps));

				TSharedRef<FJsonObject> Out = InstancedJson(*Actor, *Component, bIncludeStamps);
				Out->SetBoolField(TEXT("changed"), true);
				Out->SetNumberField(TEXT("firstIndex"), FirstIndex);
				Out->SetNumberField(TEXT("added"), Transforms.Num());
				return Ok(Out);
			});
		}

		return Error(TEXT("op is required: add, clear, update, count or remove"));
	}
}

void AddStampHandlers(TArray<FHandlerEntry>& Out)
{
	Out.Append(
	{
		{ TEXT("voxel_stamp_set"), &StampSet },
		{ TEXT("voxel_stamp_read"), &StampRead },
		{ TEXT("voxel_instanced_stamps"), &InstancedStamps },
	});
}
}

#undef LOCTEXT_NAMESPACE
