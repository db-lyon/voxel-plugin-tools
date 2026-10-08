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
#include "Shape/VoxelShapeStamp.h"
#include "Shape/VoxelSphereShape.h"
#include "Shape/VoxelCubeShape.h"
#include "Shape/VoxelPlaneShape.h"
#include "Spline/VoxelHeightSplineGraph.h"
#include "Spline/VoxelVolumeSplineGraph.h"
#include "Spline/VoxelHeightSplineStamp.h"
#include "Spline/VoxelVolumeSplineStamp.h"
#include "Spline/VoxelSplineComponent.h"
#include "Spline/VoxelSplineMetadata.h"
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
		Shape,
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
		{ EKind::Shape, TEXT("shape"), false },
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
		case EKind::Shape: return Fn(TTag<FVoxelShapeStamp>());
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
		if (Stamp.As<FVoxelShapeStamp>()) return TEXT("shape");
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
		for (const auto& Pair : (*Values)->Values)
		{
			FGuid Guid;
			FVoxelParameter Parameter;
			if (!FindGraphParameter(*Graph, FString(*Pair.Key), Guid, Parameter))
			{
				OutError = FString::Printf(TEXT("Graph %s has no parameter '%s'"), *Graph->GetName(), *Pair.Key);
				return false;
			}
			FString Text;
			if (!ScalarText(Pair.Value, Text))
			{
				OutError = FString::Printf(TEXT("parameters.%s must be a string, number, boolean or null"), *Pair.Key);
				return false;
			}
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

	// A JSON number, never a string or boolean standing in for one.
	bool Number(const FParams& Params, const TCHAR* Field, double& Out)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()))
		{
			return false;
		}
		Out = Value->AsNumber();
		return true;
	}

	// The shape a shape stamp adds or removes: its type, and only the fields that type reads. A field left out keeps the
	// current shape's value when the type is unchanged, else Voxel's default.
	bool ApplyShape(FVoxelShapeStamp& Stamp, const FParams& Params, FString& OutError)
	{
		if (!Has(Params, TEXT("shape")))
		{
			if (!Stamp.Shape.IsValid())
			{
				OutError = TEXT("shape is required: { type: Sphere | Cube | Plane, ... }");
				return false;
			}
			return true;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Params->TryGetObjectField(TEXT("shape"), Object))
		{
			OutError = TEXT("shape must be an object: { type: Sphere | Cube | Plane, ... }");
			return false;
		}
		const FParams Shape = *Object;
		const FString Type = Str(Shape, TEXT("type"));
		if (Type.Equals(TEXT("Sphere"), ESearchCase::CaseSensitive))
		{
			if (!OnlyKeys(Shape, { TEXT("type"), TEXT("radius") }, TEXT("a Sphere shape"), OutError)) return false;
			FVoxelSphereShape Sphere = Stamp.Shape.IsA<FVoxelSphereShape>() ? Stamp.Shape.Get<FVoxelSphereShape>() : FVoxelSphereShape();
			if (Has(Shape, TEXT("radius")) && (!Number(Shape, TEXT("radius"), Sphere.Radius) || Sphere.Radius <= 0))
			{
				OutError = TEXT("shape.radius must be a number > 0");
				return false;
			}
			Stamp.Shape = TVoxelInstancedStruct<FVoxelShape>(Sphere);
			return true;
		}
		if (Type.Equals(TEXT("Cube"), ESearchCase::CaseSensitive))
		{
			if (!OnlyKeys(Shape, { TEXT("type"), TEXT("size"), TEXT("roundness") }, TEXT("a Cube shape"), OutError)) return false;
			FVoxelCubeShape Cube = Stamp.Shape.IsA<FVoxelCubeShape>() ? Stamp.Shape.Get<FVoxelCubeShape>() : FVoxelCubeShape();
			if (Has(Shape, TEXT("size")) && (!Vec(Shape, TEXT("size"), Cube.Size) || Cube.Size.GetMin() <= 0))
			{
				OutError = TEXT("shape.size must be {x,y,z}, each > 0");
				return false;
			}
			double Roundness = Cube.Roundness;
			if (Has(Shape, TEXT("roundness")) && (!Number(Shape, TEXT("roundness"), Roundness) || Roundness < 0 || Roundness > 1))
			{
				OutError = TEXT("shape.roundness must be a number in [0, 1]");
				return false;
			}
			Cube.Roundness = static_cast<float>(Roundness);
			Stamp.Shape = TVoxelInstancedStruct<FVoxelShape>(Cube);
			return true;
		}
		if (Type.Equals(TEXT("Plane"), ESearchCase::CaseSensitive))
		{
			if (!OnlyKeys(Shape, { TEXT("type"), TEXT("size"), TEXT("height") }, TEXT("a Plane shape"), OutError)) return false;
			FVoxelPlaneShape Plane = Stamp.Shape.IsA<FVoxelPlaneShape>() ? Stamp.Shape.Get<FVoxelPlaneShape>() : FVoxelPlaneShape();
			if (Has(Shape, TEXT("size")) && (!Vec2(Shape, TEXT("size"), Plane.Size) || Plane.Size.GetMin() <= 0))
			{
				OutError = TEXT("shape.size must be {x,y}, each > 0");
				return false;
			}
			if (Has(Shape, TEXT("height")) && (!Number(Shape, TEXT("height"), Plane.Height) || Plane.Height < 0))
			{
				OutError = TEXT("shape.height must be a number >= 0");
				return false;
			}
			Stamp.Shape = TVoxelInstancedStruct<FVoxelShape>(Plane);
			return true;
		}
		OutError = FString::Printf(TEXT("shape.type '%s' is not one of: Sphere, Cube, Plane"), *Type);
		return false;
	}

	// Layer, blend mode and layer-type padding shared by every height or volume stamp.
	template<typename StampType>
	bool ApplyLayerFields(StampType& Stamp, const FParams& Params, FString& OutError)
	{
		if (Has(Params, TEXT("layer")) && Str(Params, TEXT("layer")).IsEmpty())
		{
			OutError = TEXT("layer must not be empty; omit it to keep the current layer");
			return false;
		}
		// Each layer type reads only its own padding fields; the other type's would be dropped.
		constexpr bool bHeight = std::derived_from<StampType, FVoxelHeightStamp>;
		static const TCHAR* const VolumeOnly[] = { TEXT("boundsExtensionMultiplier"), TEXT("maximumBoundsExtension") };
		static const TCHAR* const HeightOnly[] = { TEXT("heightPaddingMultiplier") };
		for (const TCHAR* Field : bHeight ? TConstArrayView<const TCHAR*>(VolumeOnly) : TConstArrayView<const TCHAR*>(HeightOnly))
		{
			if (Has(Params, Field))
			{
				OutError = FString::Printf(TEXT("%s only applies to %s stamp kinds"), Field, bHeight ? TEXT("volume") : TEXT("height"));
				return false;
			}
		}
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
		bOutAssetChanged = false;

		// A shape stamp is its shape; every other kind is driven by an asset.
		if constexpr (std::is_same_v<StampType, FVoxelShapeStamp>)
		{
			if (Has(Params, TEXT("asset")))
			{
				OutError = TEXT("asset does not apply to the shape kind; pass shape");
				return false;
			}
			if (!ApplyShape(Stamp, Params, OutError)) return false;
		}
		else
		{
			using FTraits = TStampAsset<StampType>;
			if (Has(Params, TEXT("shape")))
			{
				OutError = TEXT("shape only applies to the shape kind");
				return false;
			}
			const FString AssetPath = Str(Params, TEXT("asset"));
			if (AssetPath.IsEmpty() && Has(Params, TEXT("asset")))
			{
				OutError = TEXT("asset must not be empty; omit it to keep the current asset, which a stamp cannot be without");
				return false;
			}
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
		else if constexpr (std::is_same_v<StampType, FVoxelShapeStamp>)
		{
			if (bSurface && !ParseOptionalAsset(Params, TEXT("surfaceType"), Stamp.SurfaceType, OutError)) return false;
		}
		else if (bSurface)
		{
			OutError = TEXT("surfaceType only applies to heightmap, mesh and shape kinds");
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
		else if (const FVoxelShapeStamp* ShapeStamp = Stamp.As<FVoxelShapeStamp>())
		{
			Out->SetStringField(TEXT("surfaceType"), PathOf(ShapeStamp->SurfaceType.Get()));
			TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			if (const FVoxelSphereShape* Sphere = ShapeStamp->Shape.GetPtr<FVoxelSphereShape>())
			{
				Shape->SetStringField(TEXT("type"), TEXT("Sphere"));
				Shape->SetNumberField(TEXT("radius"), Sphere->Radius);
			}
			else if (const FVoxelCubeShape* Cube = ShapeStamp->Shape.GetPtr<FVoxelCubeShape>())
			{
				Shape->SetStringField(TEXT("type"), TEXT("Cube"));
				Shape->SetObjectField(TEXT("size"), VecJson(Cube->Size));
				Shape->SetNumberField(TEXT("roundness"), Cube->Roundness);
			}
			else if (const FVoxelPlaneShape* Plane = ShapeStamp->Shape.GetPtr<FVoxelPlaneShape>())
			{
				Shape->SetStringField(TEXT("type"), TEXT("Plane"));
				TSharedRef<FJsonObject> Size = MakeShared<FJsonObject>();
				Size->SetNumberField(TEXT("x"), Plane->Size.X);
				Size->SetNumberField(TEXT("y"), Plane->Size.Y);
				Shape->SetObjectField(TEXT("size"), Size);
				Shape->SetNumberField(TEXT("height"), Plane->Height);
			}
			else
			{
				Shape->SetStringField(TEXT("type"), ShapeStamp->Shape.IsValid() ? ShapeStamp->Shape.GetScriptStruct()->GetName() : TEXT("None"));
			}
			Out->SetObjectField(TEXT("shape"), Shape);
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
			const FVoxelStampRef Before = Component->GetStamp();
			Component->SetStamp(Stamp);

			const FVoxelStampRef Stored = Component->GetStamp();
			if (!Stored.As<StampType>())
			{
				// Put the previous stamp back so a failure leaves the actor as it was.
				Component->SetStamp(Before);
				return Error(TEXT("The component did not keep the stamp"));
			}

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
				// TryGetNumberField would also read "2" or true as a uniform scale.
				const TSharedPtr<FJsonValue> ScaleValue = Entry->TryGetField(TEXT("scale"));
				if (ScaleValue.IsValid() && ScaleValue->Type == EJson::Number && FMath::IsFinite(ScaleValue->AsNumber()))
				{
					Scale = FVector(ScaleValue->AsNumber());
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

		// Each op reads only its own fields; the rest would be ignored.
		{
			TArray<const TCHAR*> Allowed = { TEXT("actorPath"), TEXT("actorLabel"), TEXT("componentName"), TEXT("op"), TEXT("includeStamps"), TEXT("save") };
			if (Op == TEXT("remove")) Allowed.Add(TEXT("index"));
			if (Op == TEXT("update")) Allowed.Add(TEXT("indices"));
			if (Op == TEXT("add"))
			{
				Allowed.Append({ TEXT("kind"), TEXT("asset"), TEXT("transforms"), TEXT("relativeToComponent"), TEXT("layer"), TEXT("blendMode"),
					TEXT("priority"), TEXT("smoothness"), TEXT("behavior"), TEXT("applyOnVoid"), TEXT("heightPaddingMultiplier"),
					TEXT("boundsExtensionMultiplier"), TEXT("maximumBoundsExtension"), TEXT("surfaceType"), TEXT("useTricubic"), TEXT("parameters"), TEXT("shape") });
			}
			if (!OnlyKeys(Params, Allowed, FString::Printf(TEXT("op %s"), *Op), Err)) return Error(Err);
		}

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
				if (Values->Num() == 0) return Error(TEXT("indices must not be empty; omit it to update every stamp"));
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					const double Number = Value.IsValid() && Value->Type == EJson::Number ? Value->AsNumber() : -1;
					if (Number != FMath::RoundToDouble(Number) || Number < 0 || Number >= Num)
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

	// --------------------------------------------------------------------------------
	// Spline stamps: the curve and per-point metadata of the UVoxelSplineComponent a spline stamp reads.

	const TCHAR* const PointTypeNames[] = { TEXT("Linear"), TEXT("Curve"), TEXT("Constant"), TEXT("CurveClamped"), TEXT("CurveCustomTangent") };

	struct FSplineTarget
	{
		UVoxelSplineComponent* Spline = nullptr;
		// The stamp's spline graph, whose spline parameters are the metadata; null while the stamp has none.
		const UVoxelGraph* Graph = nullptr;
	};

	FString ResolveSpline(const FParams& Params, FSplineTarget& Out)
	{
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Err;
		Out.Spline = FindComponent<UVoxelSplineComponent>(*Actor, Params, Err);
		if (!Out.Spline) return Err;
		if (!Out.Spline->Metadata) return FString::Printf(TEXT("%s has no spline metadata object"), *Out.Spline->GetName());

		// UVoxelStampComponent attaches the spline component it creates for a spline stamp to itself.
		const UVoxelStampComponent* StampComponent = Cast<UVoxelStampComponent>(Out.Spline->GetAttachParent());
		if (!StampComponent)
		{
			return FString::Printf(TEXT("%s is not attached to a UVoxelStampComponent, so no stamp reads it"), *Out.Spline->GetName());
		}
		const FVoxelStampRef Ref = StampComponent->GetStamp();
		if (const FVoxelHeightSplineStamp* Height = Ref.As<FVoxelHeightSplineStamp>())
		{
			Out.Graph = Height->Graph.Get();
		}
		else if (const FVoxelVolumeSplineStamp* Volume = Ref.As<FVoxelVolumeSplineStamp>())
		{
			Out.Graph = Volume->Graph.Get();
		}
		else
		{
			return FString::Printf(TEXT("%s holds a %s stamp, not height_spline or volume_spline"), *StampComponent->GetName(), KindOf(*Ref));
		}
		return FString();
	}

	// Metadata values are float, FVector2D or FVector (FVoxelSplineMetadata::GetRuntime).
	TSharedPtr<FJsonValue> MetadataValueJson(const FVoxelPinValue& Value)
	{
		if (Value.Is<float>()) return MakeShared<FJsonValueNumber>(Value.Get<float>());
		if (Value.Is<FVector2D>())
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("x"), Value.Get<FVector2D>().X);
			J->SetNumberField(TEXT("y"), Value.Get<FVector2D>().Y);
			return MakeShared<FJsonValueObject>(J);
		}
		if (Value.Is<FVector>()) return MakeShared<FJsonValueObject>(VecJson(Value.Get<FVector>()));
		return MakeShared<FJsonValueNull>();
	}

	bool ParseMetadataValue(const FVoxelPinType& Type, const TSharedPtr<FJsonValue>& Json, FVoxelPinValue& Out)
	{
		double Number = 0;
		if (Type.Is<float>())
		{
			if (!Json.IsValid() || Json->Type != EJson::Number || !Json->TryGetNumber(Number) || !FMath::IsFinite(static_cast<float>(Number))) return false;
			Out = FVoxelPinValue::Make(static_cast<float>(Number));
			return true;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Json.IsValid() || !Json->TryGetObject(Object)) return false;
		const bool b2D = Type.Is<FVector2D>();
		if (!b2D && !Type.Is<FVector>()) return false;
		TArray<const TCHAR*> Axes = { TEXT("x"), TEXT("y"), TEXT("z") };
		if (b2D)
		{
			Axes.Pop();
		}
		FString Unused;
		if (!OnlyKeys(*Object, Axes, FString(), Unused)) return false;
		double Numbers[3] = {};
		for (int32 Axis = 0; Axis < Axes.Num(); Axis++)
		{
			const TSharedPtr<FJsonValue> Field = (*Object)->TryGetField(Axes[Axis]);
			if (!Field.IsValid() || Field->Type != EJson::Number || !Field->TryGetNumber(Numbers[Axis]) || !FMath::IsFinite(Numbers[Axis])) return false;
		}
		Out = b2D ? FVoxelPinValue::Make(FVector2D(Numbers[0], Numbers[1])) : FVoxelPinValue::Make(FVector(Numbers[0], Numbers[1], Numbers[2]));
		return true;
	}

	TSharedRef<FJsonObject> SplineJson(const UVoxelSplineComponent& Spline)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("componentName"), Spline.GetName());
		Out->SetBoolField(TEXT("closedLoop"), Spline.IsClosedLoop());
		Out->SetNumberField(TEXT("length"), Spline.GetSplineLength());

		const FSplineCurves Curves = Spline.GetSplineCurves();
		TArray<TSharedPtr<FJsonValue>> Points;
		for (int32 Index = 0; Index < Spline.GetNumberOfSplinePoints(); Index++)
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetObjectField(TEXT("location"), VecJson(Spline.GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::Local)));
			P->SetObjectField(TEXT("worldLocation"), VecJson(Spline.GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World)));
			const int32 Type = Spline.GetSplinePointType(Index);
			P->SetStringField(TEXT("type"), Type >= 0 && Type < UE_ARRAY_COUNT(PointTypeNames) ? PointTypeNames[Type] : TEXT("Unknown"));
			P->SetObjectField(TEXT("arriveTangent"), VecJson(Spline.GetArriveTangentAtSplinePoint(Index, ESplineCoordinateSpace::Local)));
			P->SetObjectField(TEXT("leaveTangent"), VecJson(Spline.GetLeaveTangentAtSplinePoint(Index, ESplineCoordinateSpace::Local)));
			// The stored point rotation, not GetRotationAtSplinePoint's, which also turns along the tangent.
			const FRotator Rotation = Curves.Rotation.Points.IsValidIndex(Index) ? Curves.Rotation.Points[Index].OutVal.Rotator() : FRotator::ZeroRotator;
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetNumberField(TEXT("pitch"), Rotation.Pitch);
			R->SetNumberField(TEXT("yaw"), Rotation.Yaw);
			R->SetNumberField(TEXT("roll"), Rotation.Roll);
			P->SetObjectField(TEXT("rotation"), R);
			P->SetObjectField(TEXT("scale"), VecJson(Spline.GetScaleAtSplinePoint(Index)));
			Points.Add(MakeShared<FJsonValueObject>(P));
		}
		Out->SetArrayField(TEXT("points"), Points);

		TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
		for (const auto& It : Spline.Metadata->GuidToValues)
		{
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("type"), It.Value.Parameter.Type.ToString());
			M->SetField(TEXT("default"), MetadataValueJson(It.Value.DefaultValue));
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TVoxelInstancedStruct<FVoxelPinValue>& Value : It.Value.Values)
			{
				Values.Add(Value.IsValid() ? MetadataValueJson(*Value) : MakeShared<FJsonValueNull>());
			}
			M->SetArrayField(TEXT("values"), Values);
			Metadata->SetObjectField(It.Value.Parameter.Name.ToString(), M);
		}
		Out->SetObjectField(TEXT("metadata"), Metadata);
		return Out;
	}

	FResult SplineRead(const FParams& Params)
	{
		FSplineTarget Target;
		if (const FString Err = ResolveSpline(Params, Target); !Err.IsEmpty()) return Error(Err);
		TSharedRef<FJsonObject> Out = SplineJson(*Target.Spline);
		Out->SetStringField(TEXT("graph"), Target.Graph ? Target.Graph->GetPathName() : FString());
		return Ok(Out);
	}

	FResult SplineSetPoints(const FParams& Params)
	{
		FSplineTarget Target;
		if (const FString Err = ResolveSpline(Params, Target); !Err.IsEmpty()) return Error(Err);
		UVoxelSplineComponent& Spline = *Target.Spline;
		UVoxelSplineMetadata& Metadata = *Spline.Metadata;

		// Validate every point before anything changes.
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Params->TryGetArrayField(TEXT("points"), Items) || Items->Num() < 2)
		{
			return Error(TEXT("points must hold at least 2 points; a spline stamp with fewer generates nothing"));
		}
		TArray<FSplinePoint> Points;
		for (int32 Index = 0; Index < Items->Num(); Index++)
		{
			const FString Where = FString::Printf(TEXT("points[%d]"), Index);
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item)) return Error(Where + TEXT(" must be an object"));
			FString Err;
			if (!OnlyKeys(*Item, { TEXT("location"), TEXT("type"), TEXT("arriveTangent"), TEXT("leaveTangent"), TEXT("rotation"), TEXT("scale") }, Where, Err)) return Error(Err);

			FSplinePoint Point(static_cast<float>(Index), FVector::ZeroVector);
			if (!Vec(*Item, TEXT("location"), Point.Position)) return Error(Where + TEXT(".location needs x, y and z numbers"));

			Point.Type = ESplinePointType::Curve;
			if (Has(*Item, TEXT("type")))
			{
				const FString TypeName = Str(*Item, TEXT("type"));
				int32 Found = INDEX_NONE;
				for (int32 Type = 0; Type < UE_ARRAY_COUNT(PointTypeNames); Type++)
				{
					Found = TypeName == PointTypeNames[Type] ? Type : Found;
				}
				if (Found == INDEX_NONE) return Error(Where + TEXT(".type must be one of Linear, Curve, Constant, CurveClamped, CurveCustomTangent"));
				Point.Type = static_cast<ESplinePointType::Type>(Found);
			}

			const bool bCustom = Point.Type == ESplinePointType::CurveCustomTangent;
			const bool bArrive = Has(*Item, TEXT("arriveTangent"));
			const bool bLeave = Has(*Item, TEXT("leaveTangent"));
			if (!bCustom && (bArrive || bLeave))
			{
				return Error(Where + TEXT(": tangents only apply to type CurveCustomTangent; the other types compute their own"));
			}
			if (bCustom && (!Vec(*Item, TEXT("arriveTangent"), Point.ArriveTangent) || !Vec(*Item, TEXT("leaveTangent"), Point.LeaveTangent)))
			{
				return Error(Where + TEXT(": type CurveCustomTangent needs arriveTangent and leaveTangent, each with x, y and z numbers"));
			}
			if (Has(*Item, TEXT("rotation")) && !Rot(*Item, TEXT("rotation"), Point.Rotation))
			{
				return Error(Where + TEXT(".rotation needs pitch, yaw and roll numbers"));
			}
			if (Has(*Item, TEXT("scale")) && !Vec(*Item, TEXT("scale"), Point.Scale))
			{
				return Error(Where + TEXT(".scale needs x, y and z numbers"));
			}
			Points.Add(Point);
		}

		const TSharedPtr<FJsonObject>* MetadataJson = nullptr;
		if (Has(Params, TEXT("metadata")))
		{
			if (!Params->TryGetObjectField(TEXT("metadata"), MetadataJson)) return Error(TEXT("metadata must be an object: { parameterName: [one value per point] }"));
			if (!Target.Graph) return Error(TEXT("The stamp has no graph, so the spline has no metadata parameters"));
		}

		const FScopedTransaction Transaction(LOCTEXT("SplineSetPoints", "Set Voxel Spline Points"));
		Spline.Modify();
		Metadata.Modify();
		// What the stamp's own fixup does on every update: one metadata entry per spline parameter of the graph.
		if (Target.Graph)
		{
			Metadata.Fixup(*Target.Graph);
		}

		// Requested values per metadata guid, validated against the parameter types before the curve changes.
		TMap<FGuid, TArray<FVoxelPinValue>> Requested;
		if (MetadataJson)
		{
			for (const auto& Pair : (*MetadataJson)->Values)
			{
				const FString Name(*Pair.Key);
				const FGuid* Guid = nullptr;
				TArray<FString> Names;
				for (const auto& It : Metadata.GuidToValues)
				{
					Names.Add(It.Value.Parameter.Name.ToString());
					if (It.Value.Parameter.Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
					{
						Guid = &It.Key;
					}
				}
				if (!Guid)
				{
					return Error(FString::Printf(TEXT("metadata.%s: %s has no spline parameter of that name. Spline parameters: %s"),
						*Name, *Target.Graph->GetName(), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none")));
				}
				const FVoxelPinType& Type = Metadata.GuidToValues[*Guid].Parameter.Type;
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Pair.Value.IsValid() || !Pair.Value->TryGetArray(Values) || Values->Num() != Points.Num())
				{
					return Error(FString::Printf(TEXT("metadata.%s must be an array of %d values, one per point"), *Name, Points.Num()));
				}
				TArray<FVoxelPinValue>& Parsed = Requested.Add(*Guid);
				for (int32 Index = 0; Index < Values->Num(); Index++)
				{
					FVoxelPinValue Value;
					if (!ParseMetadataValue(Type, (*Values)[Index], Value))
					{
						return Error(FString::Printf(TEXT("metadata.%s[%d] must be %s"), *Name, Index,
							Type.Is<float>() ? TEXT("a number") : Type.Is<FVector2D>() ? TEXT("{x,y} numbers") : TEXT("{x,y,z} numbers")));
					}
					Parsed.Add(Value);
				}
			}
		}

		// Clearing the curve resets the metadata; unlisted parameters keep their values when the point count does not change.
		TMap<FGuid, TArray<TVoxelInstancedStruct<FVoxelPinValue>>> Kept;
		if (Spline.GetNumberOfSplinePoints() == Points.Num())
		{
			for (const auto& It : Metadata.GuidToValues)
			{
				Kept.Add(It.Key, It.Value.Values);
			}
		}

		Spline.ClearSplinePoints(false);
		for (const FSplinePoint& Point : Points)
		{
			Spline.AddPoint(Point, false);
		}
		if (Has(Params, TEXT("closedLoop")))
		{
			Spline.SetClosedLoop(Bool(Params, TEXT("closedLoop"), false), false);
		}
		// As the spline visualizer marks it, so a construction script rerun keeps the edit.
		Spline.bSplineHasBeenEdited = true;
		Spline.UpdateSpline();

		for (auto& It : Metadata.GuidToValues)
		{
			FVoxelSplineMetadataValues& Values = It.Value;
			const TArray<FVoxelPinValue>* Given = Requested.Find(It.Key);
			const TArray<TVoxelInstancedStruct<FVoxelPinValue>>* Previous = Kept.Find(It.Key);
			Values.Values.SetNum(Points.Num());
			for (int32 Index = 0; Index < Points.Num(); Index++)
			{
				if (Given)
				{
					Values.Values[Index] = (*Given)[Index];
				}
				else if (Previous && Previous->IsValidIndex(Index) && (*Previous)[Index].IsValid())
				{
					Values.Values[Index] = (*Previous)[Index];
				}
				else
				{
					Values.Values[Index] = Values.DefaultValue;
				}
			}
		}

		// The stamp component rebuilds its stamp when a component attached to it reports an edit (VoxelStampComponent.cpp).
		Metadata.PostEditChange();
		Spline.PostEditChange();

		return Ok(SplineJson(Spline));
	}
}

void AddStampHandlers(TArray<FHandlerEntry>& Out)
{
	// The stamp fields Configure reads, shared by voxel_stamp_set and voxel_instanced_stamps op add.
	const auto StampFields = [](const TCHAR* AssetDescription, const TCHAR* LayerDescription)
	{
		return TArray<FMCPParamSpec>{
			MCPParam::Optional(TEXT("asset"), EMCPParamType::String, AssetDescription),
			MCPParam::Optional(TEXT("layer"), EMCPParamType::String, LayerDescription),
			MCPParam::Optional(TEXT("blendMode"), EMCPParamType::String,
				TEXT("EVoxelHeightBlendMode for height kinds (Max, Min, Override) or EVoxelVolumeBlendMode for volume kinds (Additive, Subtractive, Intersect, Override)."))
				.Enum({ TEXT("Max"), TEXT("Min"), TEXT("Override"), TEXT("Additive"), TEXT("Subtractive"), TEXT("Intersect") }),
			MCPParam::Optional(TEXT("priority"), EMCPParamType::Integer, TEXT("Priority within the layer, an int32; higher applies later.")).Range(MIN_int32, MAX_int32),
			MCPParam::Optional(TEXT("smoothness"), EMCPParamType::Number, TEXT("Blend smoothness in centimetres, >= 0.")).Min(0),
			MCPParam::Optional(TEXT("behavior"), EMCPParamType::String, TEXT("EVoxelStampBehavior: what the stamp writes."))
				.Enum({ TEXT("AffectShape"), TEXT("AffectSurfaceType"), TEXT("AffectMetadata"), TEXT("AffectAll"),
					TEXT("AffectShapeAndSurfaceType"), TEXT("AffectShapeAndMetadata"), TEXT("AffectSurfaceTypeAndMetadata") }),
			MCPParam::Optional(TEXT("applyOnVoid"), EMCPParamType::Boolean, TEXT("False applies only where an earlier stamp already applied; ignored by Override and Intersect blends.")),
			MCPParam::Optional(TEXT("heightPaddingMultiplier"), EMCPParamType::Number, TEXT("Height kinds only: bounds padding relative to the bounds size, >= 0.")).Min(0),
			MCPParam::Optional(TEXT("boundsExtensionMultiplier"), EMCPParamType::Number, TEXT("Volume kinds only: bounds extension relative to the bounds size, >= 0.")).Min(0),
			MCPParam::Optional(TEXT("maximumBoundsExtension"), EMCPParamType::Number, TEXT("Volume kinds only: cap on the bounds extension in centimetres, >= 0.")).Min(0),
			MCPParam::Optional(TEXT("surfaceType"), EMCPParamType::String,
				TEXT("heightmap: the default surface type; mesh and shape: the surface type. A UVoxelSurfaceTypeInterface asset path; \"\" or null clears it.")).Nullable(),
			MCPParam::Optional(TEXT("shape"), EMCPParamType::Object,
				TEXT("shape kind only, required unless the current stamp is a shape: the volume it adds or removes, centred on the component. A field left out keeps the current shape's value when type is unchanged, else Voxel's default."))
				.Tagged(TEXT("type"), {
					MCPParam::Variant(TEXT("Sphere"), TEXT("A sphere."), {
						MCPParam::OptionalField(TEXT("radius"), EMCPParamType::Number, TEXT("Radius in centimetres, > 0; Voxel's default 1000.")).Min(0),
					}),
					MCPParam::Variant(TEXT("Cube"), TEXT("A box."), {
						MCPParam::OptionalField(TEXT("size"), EMCPParamType::Vec3, TEXT("Size in centimetres, each component > 0; Voxel's default 1000 on every axis.")),
						MCPParam::OptionalField(TEXT("roundness"), EMCPParamType::Number, TEXT("Edge rounding in [0, 1]; Voxel's default 0.")).Range(0, 1),
					}),
					MCPParam::Variant(TEXT("Plane"), TEXT("A slab: a flat box of size {x,y}."), {
						MCPParam::OptionalField(TEXT("size"), EMCPParamType::Object, TEXT("{x,y} in centimetres, each > 0; Voxel's default 1000 on both.")),
						MCPParam::OptionalField(TEXT("height"), EMCPParamType::Number, TEXT("Thickness, >= 0; Voxel's default 1.")).Min(0),
					}),
				}),
			MCPParam::Optional(TEXT("useTricubic"), EMCPParamType::Boolean, TEXT("mesh only: tricubic interpolation, slower and smoother.")),
			Spec::ValueMap(TEXT("parameters"), false,
				TEXT("Graph and spline kinds only: { parameterName: value } overrides; each value a string, number, boolean or null, parsed as the parameter's type, and null sets an object parameter to None.")),
		};
	};

	TArray<FMCPParamSpec> SetParams = {
		Spec::ActorPath(TEXT("Stamp actor object path; preferred, since stamp actors relabel themselves.")),
		Spec::ActorLabel(TEXT("Stamp actor label; must match exactly one actor.")),
		Spec::ComponentName(TEXT("UVoxelStampComponent object name; default the actor's first one.")),
		MCPParam::Required(TEXT("kind"), EMCPParamType::String, TEXT("Stamp kind to build; a different kind replaces the current stamp."))
			.Enum({ TEXT("height_graph"), TEXT("volume_graph"), TEXT("heightmap"), TEXT("mesh"), TEXT("height_spline"), TEXT("volume_spline"), TEXT("shape") }),
	};
	SetParams.Append(StampFields(
		TEXT("UVoxelHeightGraph, UVoxelVolumeGraph, UVoxelHeightmap, UVoxelStaticMesh, UVoxelHeightSplineGraph or UVoxelVolumeSplineGraph path matching kind, every kind but shape (which takes shape and refuses asset); required unless the current stamp of this kind has one. Changing a graph clears its overrides."),
		TEXT("UVoxelHeightLayer (height kinds) or UVoxelVolumeLayer (volume kinds) path; default keeps the current layer, else Voxel's built-in default layer.")));
	SetParams.Add(Spec::SaveDirty());
	Out.Add({ TEXT("voxel_stamp_set"), &StampSet, SetParams, Spec::OneActor() });

	Out.Add({ TEXT("voxel_stamp_read"), &StampRead, {
		Spec::ActorPath(TEXT("Stamp actor object path; preferred, since stamp actors relabel themselves.")),
		Spec::ActorLabel(TEXT("Stamp actor label; must match exactly one actor.")),
		Spec::ComponentName(TEXT("UVoxelStampComponent object name; default the actor's first one.")),
	}, Spec::OneActor() });

	TArray<FMCPParamSpec> InstancedParams = {
		Spec::ActorPath(TEXT("Actor object path; preferred, since labels can repeat.")),
		Spec::ActorLabel(TEXT("Actor label; must match exactly one actor.")),
		Spec::ComponentName(TEXT("UVoxelInstancedStampComponent object name; default the actor's first one.")),
		MCPParam::Required(TEXT("op"), EMCPParamType::String,
			TEXT("add appends stamps, remove empties one slot (indices never shift), clear removes all, update re-evaluates stamps, count reports them. Each op takes only its own fields."))
			.Enum({ TEXT("add"), TEXT("remove"), TEXT("clear"), TEXT("update"), TEXT("count") }),
		MCPParam::Optional(TEXT("kind"), EMCPParamType::String, TEXT("add, required: the stamp kind; spline kinds need a stamp actor's spline component and are not offered."))
			.Enum({ TEXT("height_graph"), TEXT("volume_graph"), TEXT("heightmap"), TEXT("mesh"), TEXT("shape") }),
		MCPParam::Optional(TEXT("transforms"), EMCPParamType::Array, TEXT("add, required: one stamp per entry, in world space unless relativeToComponent."))
			.Items(EMCPParamType::Object).WithFields({
				MCPParam::RequiredField(TEXT("location"), EMCPParamType::Vec3, TEXT("Location in centimetres.")),
				MCPParam::OptionalField(TEXT("rotation"), EMCPParamType::Rotator, TEXT("Rotation in degrees; default zero.")),
				MCPParam::OptionalField(TEXT("scale"), EMCPParamType::Any, TEXT("A uniform number or {x,y,z}, no component zero; default 1.")),
			}),
		MCPParam::Optional(TEXT("relativeToComponent"), EMCPParamType::Boolean, TEXT("add: transforms are relative to the component, baked to world space when added; default false.")),
	};
	InstancedParams.Append(StampFields(
		TEXT("add, required: UVoxelHeightGraph, UVoxelVolumeGraph, UVoxelHeightmap or UVoxelStaticMesh path matching kind, every kind but shape (which takes shape and refuses asset)."),
		TEXT("add: UVoxelHeightLayer (height kinds) or UVoxelVolumeLayer (volume kinds) path; default Voxel's built-in default layer.")));
	InstancedParams.Append({
		MCPParam::Optional(TEXT("index"), EMCPParamType::Integer, TEXT("remove, required: the stamp slot to empty, in [0, count).")).Range(0, MAX_int32),
		MCPParam::Optional(TEXT("indices"), EMCPParamType::Array, TEXT("update: non-empty stamp slots to re-evaluate, each in [0, count); default every slot."))
			.Items(EMCPParamType::Integer).Range(0, MAX_int32),
		MCPParam::Optional(TEXT("includeStamps"), EMCPParamType::Boolean, TEXT("Include every non-empty stamp with its index and transform in the result; default false.")),
		Spec::SaveDirty(),
	});
	Out.Add({ TEXT("voxel_instanced_stamps"), &InstancedStamps, InstancedParams, Spec::OneActor() });

	const auto SplineActor = []
	{
		return TArray<FMCPParamSpec>{
			Spec::ActorPath(TEXT("Spline stamp actor object path; preferred, since stamp actors relabel themselves.")),
			Spec::ActorLabel(TEXT("Spline stamp actor label; must match exactly one actor.")),
			Spec::ComponentName(TEXT("UVoxelSplineComponent object name; default the actor's first one.")),
		};
	};

	Out.Add({ TEXT("voxel_spline_read"), &SplineRead, SplineActor(), Spec::OneActor() });

	TArray<FMCPParamSpec> SplineParams = SplineActor();
	SplineParams.Append({
		MCPParam::Required(TEXT("points"), EMCPParamType::Array,
			TEXT("The whole curve, at least 2 points, relative to the spline component; validated before anything changes. Replaces every existing point."))
			.Items(EMCPParamType::Object).WithFields({
				MCPParam::RequiredField(TEXT("location"), EMCPParamType::Vec3, TEXT("Point location in centimetres, relative to the spline component.")),
				MCPParam::OptionalField(TEXT("type"), EMCPParamType::String, TEXT("ESplinePointType; default Curve."))
					.Enum({ TEXT("Linear"), TEXT("Curve"), TEXT("Constant"), TEXT("CurveClamped"), TEXT("CurveCustomTangent") }),
				MCPParam::OptionalField(TEXT("arriveTangent"), EMCPParamType::Vec3, TEXT("CurveCustomTangent only, and required there; relative to the component.")),
				MCPParam::OptionalField(TEXT("leaveTangent"), EMCPParamType::Vec3, TEXT("CurveCustomTangent only, and required there; relative to the component.")),
				MCPParam::OptionalField(TEXT("rotation"), EMCPParamType::Rotator, TEXT("Point rotation in degrees, relative to the component; default zero.")),
				MCPParam::OptionalField(TEXT("scale"), EMCPParamType::Vec3, TEXT("Point scale; default 1.")),
			}),
		MCPParam::Optional(TEXT("closedLoop"), EMCPParamType::Boolean, TEXT("Close the spline; default keeps the current setting.")),
		MCPParam::Optional(TEXT("metadata"), EMCPParamType::Any,
			TEXT("{ splineParameterName: [one value per point] } for the stamp graph's spline parameters (voxel_spline_read lists them): a number for float, {x,y} for vector2d, {x,y,z} for vector. A parameter not listed keeps its values when the point count is unchanged, else takes its default."))
			.OneOfForms({ EMCPValueForm::ArgMap }),
		Spec::SaveDirty(),
	});
	Out.Add({ TEXT("voxel_spline_set_points"), &SplineSetPoints, SplineParams, Spec::OneActor() });
}
}

#undef LOCTEXT_NAMESPACE
