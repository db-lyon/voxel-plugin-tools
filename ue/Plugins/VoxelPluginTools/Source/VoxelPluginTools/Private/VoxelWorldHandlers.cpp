#include "VoxelToolsCommon.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

#include "VoxelWorld.h"
#include "VoxelLODQuality.h"
#include "VoxelStackLayer.h"
#include "VoxelLayer.h"
#include "VoxelLayerStack.h"
#include "MegaMaterial/VoxelMegaMaterial.h"
#include "VoxelStampActor.h"
#include "VoxelStampComponent.h"
#include "VoxelInstancedStampComponent.h"
#include "VoxelNoClippingComponent.h"
#include "VoxelDebugActor.h"
#include "Sculpt/Height/VoxelSculptHeight.h"
#include "Sculpt/Volume/VoxelSculptVolume.h"
#include "Collision/VoxelCollisionBaker.h"
#include "VoxelShaderHook.h"
#include "VoxelShaderHooksManager.h"

#define LOCTEXT_NAMESPACE "VoxelPluginTools"

namespace VoxelPluginTools
{
namespace
{
	// The AVoxelWorld property a JSON field edits, and the field's contract; the field's name is Spec.Name.
	struct FWorldField
	{
		FName Property;
		FMCPParamSpec Spec;
	};

	FMCPParamSpec WorldBool(const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::Optional(Name, EMCPParamType::Boolean, Description);
	}

	// int32 properties with ClampMin = 1 (VoxelWorld.h); ParseInto refuses anything outside [1, MAX_int32].
	FMCPParamSpec WorldCount(const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::Optional(Name, EMCPParamType::Integer, Description).Range(1, MAX_int32);
	}

	// Object properties: ParseInto takes a path that must load as the property class, or "" / null to clear.
	FMCPParamSpec WorldAsset(const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::Optional(Name, EMCPParamType::String, Description).Nullable();
	}

	FMCPParamField IntervalField(const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::OptionalField(Name, EMCPParamType::Object, Description).WithFields({
			MCPParam::OptionalField(TEXT("min"), EMCPParamType::Number, TEXT("Lowest quality, > 0; default keeps the current value.")),
			MCPParam::OptionalField(TEXT("max"), EMCPParamType::Number, TEXT("Highest quality, >= min; default keeps the current value.")),
		});
	}

	const TArray<FWorldField>& WorldFields()
	{
		static const TArray<FWorldField> Fields =
		{
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, LayerStack), WorldAsset(TEXT("layerStack"), TEXT("UVoxelLayerStack asset path; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, MegaMaterial), WorldAsset(TEXT("megaMaterial"), TEXT("UVoxelMegaMaterial asset path; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, VoxelSize), WorldCount(TEXT("voxelSize"), TEXT("Voxel size in centimetres, an integer >= 1; the default world uses 100.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, LODQuality), MCPParam::Optional(TEXT("lodQuality"), EMCPParamType::Object,
				TEXT("FVoxelLODQuality merged onto the current value; each interval needs 0 < min <= max after the merge.")).WithFields({
					IntervalField(TEXT("gameQuality"), TEXT("Quality range at game time.")),
					IntervalField(TEXT("editorQuality"), TEXT("Quality range in the editor.")),
					MCPParam::OptionalField(TEXT("alwaysUseGameQuality"), EMCPParamType::Boolean, TEXT("Use gameQuality in the editor too.")),
				}) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, QualityExponent), MCPParam::Optional(TEXT("qualityExponent"), EMCPParamType::Number,
				TEXT("LOD selection bias; higher gives far chunks more resolution. The details panel offers 0.5 to 1.5; default 1.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableNanite), WorldBool(TEXT("enableNanite"), TEXT("Render the voxel mesh with Nanite; default true.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableTessellation), WorldBool(TEXT("enableTessellation"), TEXT("Nanite tessellation (displacement); default true.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableLumen), WorldBool(TEXT("enableLumen"), TEXT("Include the voxel mesh in Lumen; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableRaytracing), WorldBool(TEXT("enableRaytracing"), TEXT("Visible in ray-traced effects; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bGenerateMeshDistanceFields), WorldBool(TEXT("generateMeshDistanceFields"), TEXT("Generate mesh distance fields; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, BlockinessMetadata), WorldAsset(TEXT("blockinessMetadata"), TEXT("UVoxelFloatMetadata asset path selecting how blocky each voxel renders; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, RenderChunkSize), MCPParam::Optional(TEXT("renderChunkSize"), EMCPParamType::String,
				TEXT("EVoxelRenderChunkSize render chunk size in voxels; default Size32.")).Enum({ TEXT("Size32"), TEXT("Size64"), TEXT("Size128"), TEXT("Size256") }) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bUseCameraAsInvoker), WorldBool(TEXT("useCameraAsInvoker"), TEXT("Use the camera as an LOD invoker; default true.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bCreateRuntimeOnBeginPlay), WorldBool(TEXT("createRuntimeOnBeginPlay"), TEXT("Create the runtime on BeginPlay; false needs a manual CreateRuntime. Default true.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bWaitOnBeginPlay), WorldBool(TEXT("waitOnBeginPlay"), TEXT("Block BeginPlay until the world is generated; default true.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bLimitMaxLOD), WorldBool(TEXT("limitMaxLOD"), TEXT("Enable the maxLOD render cap; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, MaxLOD), MCPParam::Optional(TEXT("maxLOD"), EMCPParamType::Integer,
				TEXT("With limitMaxLOD, chunks with a LOD strictly above this are not rendered; an int32, default 30.")).Range(MIN_int32, MAX_int32) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, MaxBackgroundTasks), WorldCount(TEXT("maxBackgroundTasks"), TEXT("Background task cap, an integer >= 1; default 256.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bDoubleSidedCollision), WorldBool(TEXT("doubleSidedCollision"), TEXT("Double-sided collision; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bGenerateOverlapEvents), WorldBool(TEXT("generateOverlapEvents"), TEXT("Collision generates overlap events; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, CollisionChunkSize), WorldCount(TEXT("collisionChunkSize"), TEXT("Invoker collision chunk size in voxels, an integer >= 1; default 32.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bOverrideCollisionVoxelSize), WorldBool(TEXT("overrideCollisionVoxelSize"), TEXT("Use collisionVoxelSize instead of voxelSize for collision; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, CollisionVoxelSize), WorldCount(TEXT("collisionVoxelSize"), TEXT("Collision voxel size in centimetres, an integer >= 1; default 100.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableNavigation), WorldBool(TEXT("enableNavigation"), TEXT("Generate navigation on voxel chunks; default true.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, NavigationChunkSize), WorldCount(TEXT("navigationChunkSize"), TEXT("Navigation chunk size in voxels, an integer >= 1; default 32.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bGenerateNavigationInsideNavMeshBounds), WorldBool(TEXT("generateNavigationInsideNavMeshBounds"),
				TEXT("Generate navigation on every chunk inside NavMeshBoundsVolumes, expensive for large bounds; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bOnlyGenerateNavigationInEditor), WorldBool(TEXT("onlyGenerateNavigationInEditor"), TEXT("Only generate navigation in the editor, for baking; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bOverrideNavigationVoxelSize), WorldBool(TEXT("overrideNavigationVoxelSize"), TEXT("Use navigationVoxelSize instead of voxelSize for navigation; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, NavigationVoxelSize), WorldCount(TEXT("navigationVoxelSize"), TEXT("Navigation voxel size in centimetres, an integer >= 1; default 100.")) },
			{ GET_MEMBER_NAME_CHECKED(AVoxelWorld, bRenderScatterActors), WorldBool(TEXT("renderScatterActors"), TEXT("Render scatter actors; default true.")) },
		};
		return Fields;
	}

	const FWorldField& WorldField(const TCHAR* Name)
	{
		const FWorldField* Field = WorldFields().FindByPredicate([&](const FWorldField& F) { return F.Spec.Name == Name; });
		check(Field);
		return *Field;
	}

	// A new property value parsed and validated before anything is mutated.
	struct FStagedValue
	{
		FString Field;
		FProperty* Property = nullptr;
		void* Value = nullptr;

		FStagedValue(const FString& InField, FProperty& InProperty, const void* Current)
			: Field(InField)
			, Property(&InProperty)
			, Value(InProperty.AllocateAndInitializeValue())
		{
			Property->CopyCompleteValue(Value, Current);
		}
		~FStagedValue()
		{
			Property->DestroyAndFreeValue(Value);
		}
		FStagedValue(const FStagedValue&) = delete;
		FStagedValue& operator=(const FStagedValue&) = delete;
	};
	using FStaged = TArray<TUniquePtr<FStagedValue>>;

	TSharedPtr<FJsonValue> PathJson(const UObject* Object)
	{
		if (!Object)
		{
			return MakeShared<FJsonValueNull>();
		}
		return MakeShared<FJsonValueString>(Object->GetPathName());
	}

	TArray<FString> EnumNames(const UEnum& Enum)
	{
		TArray<FString> Names;
		const int32 Count = Enum.ContainsExistingMax() ? Enum.NumEnums() - 1 : Enum.NumEnums();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Names.Add(Enum.GetNameStringByIndex(Index));
		}
		return Names;
	}

	TSharedRef<FJsonObject> IntervalJson(const FFloatInterval& Interval)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("min"), Interval.Min);
		J->SetNumberField(TEXT("max"), Interval.Max);
		return J;
	}

	TSharedPtr<FJsonValue> ValueJson(const FProperty& Property, const void* Value)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(Value));
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(&Property))
		{
			const int64 Raw = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
			return MakeShared<FJsonValueString>(EnumProp->GetEnum()->GetNameStringByValue(Raw));
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Numeric->GetSignedIntPropertyValue(Value)));
			}
			return MakeShared<FJsonValueNumber>(Numeric->IsInteger()
				? static_cast<double>(Numeric->GetSignedIntPropertyValue(Value))
				: Numeric->GetFloatingPointPropertyValue(Value));
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			return PathJson(Object->GetObjectPropertyValue(Value));
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property); Struct && Struct->Struct == FVoxelLODQuality::StaticStruct())
		{
			const FVoxelLODQuality& Quality = *static_cast<const FVoxelLODQuality*>(Value);
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetObjectField(TEXT("gameQuality"), IntervalJson(Quality.GameQuality));
			J->SetObjectField(TEXT("editorQuality"), IntervalJson(Quality.EditorQuality));
			J->SetBoolField(TEXT("alwaysUseGameQuality"), Quality.bAlwaysUseGameQuality);
			return MakeShared<FJsonValueObject>(J);
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property); Struct && Struct->Struct == FVoxelStackLayer::StaticStruct())
		{
			const FVoxelStackLayer& Layer = *static_cast<const FVoxelStackLayer*>(Value);
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetField(TEXT("stack"), PathJson(Layer.Stack));
			J->SetField(TEXT("layer"), PathJson(Layer.Layer));
			return MakeShared<FJsonValueObject>(J);
		}
		return MakeShared<FJsonValueNull>();
	}

	bool ParseInterval(const FJsonObject& Object, const TCHAR* Field, FFloatInterval& InOut, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Interval = nullptr;
		if (!Object.HasField(Field))
		{
			return true;
		}
		if (!Object.TryGetObjectField(Field, Interval))
		{
			OutError = FString::Printf(TEXT("lodQuality.%s must be {min,max}"), Field);
			return false;
		}
		double Min = InOut.Min, Max = InOut.Max;
		for (const auto& Pair : (*Interval)->Values)
		{
			const bool bMin = FString(*Pair.Key).Equals(TEXT("min"), ESearchCase::CaseSensitive);
			if (!bMin && !FString(*Pair.Key).Equals(TEXT("max"), ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(TEXT("lodQuality.%s takes only min and max (got %s)"), Field, *Pair.Key);
				return false;
			}
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number || !FMath::IsFinite(Pair.Value->AsNumber()))
			{
				OutError = FString::Printf(TEXT("lodQuality.%s.%s must be a number"), Field, *Pair.Key);
				return false;
			}
			(bMin ? Min : Max) = Pair.Value->AsNumber();
		}
		if (Min <= 0 || Max <= 0 || Min > Max)
		{
			OutError = FString::Printf(TEXT("lodQuality.%s needs 0 < min <= max (got %g, %g)"), Field, Min, Max);
			return false;
		}
		InOut = FFloatInterval(static_cast<float>(Min), static_cast<float>(Max));
		return true;
	}

	// Parses a JSON value into Dest (already holding the current value, so partial structs merge).
	bool ParseInto(const FString& Field, const FProperty& Property, const TSharedPtr<FJsonValue>& Json, void* Dest, FString& OutError)
	{
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(&Property))
		{
			bool bValue = false;
			if (!Json.IsValid() || Json->Type != EJson::Boolean || !Json->TryGetBool(bValue))
			{
				OutError = FString::Printf(TEXT("%s must be a boolean"), *Field);
				return false;
			}
			BoolProp->SetPropertyValue(Dest, bValue);
			return true;
		}

		const UEnum* Enum = nullptr;
		const FNumericProperty* Underlying = nullptr;
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(&Property))
		{
			Enum = EnumProp->GetEnum();
			Underlying = EnumProp->GetUnderlyingProperty();
		}
		else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property); Numeric && Numeric->GetIntPropertyEnum())
		{
			Enum = Numeric->GetIntPropertyEnum();
			Underlying = Numeric;
		}
		if (Enum)
		{
			FString Text;
			const TArray<FString> Names = EnumNames(*Enum);
			if (Json.IsValid() && Json->Type == EJson::String && Json->TryGetString(Text))
			{
				for (int32 Index = 0; Index < Names.Num(); ++Index)
				{
					if (Names[Index].Equals(Text, ESearchCase::IgnoreCase) ||
						Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Text, ESearchCase::IgnoreCase))
					{
						Underlying->SetIntPropertyValue(Dest, Enum->GetValueByIndex(Index));
						return true;
					}
				}
			}
			OutError = FString::Printf(TEXT("%s must be one of: %s"), *Field, *FString::Join(Names, TEXT(", ")));
			return false;
		}

		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			double Number = 0;
			if (!Json.IsValid() || Json->Type != EJson::Number || !Json->TryGetNumber(Number) || !FMath::IsFinite(Number))
			{
				OutError = FString::Printf(TEXT("%s must be a number"), *Field);
				return false;
			}
			if (Property.HasMetaData(TEXT("ClampMin")))
			{
				const double ClampMin = FCString::Atod(*Property.GetMetaData(TEXT("ClampMin")));
				if (Number < ClampMin)
				{
					OutError = FString::Printf(TEXT("%s must be >= %g (got %g)"), *Field, ClampMin, Number);
					return false;
				}
			}
			if (Numeric->IsInteger())
			{
				// The property's own width and signedness bound the value, so an unsigned field cannot wrap.
				double Min = MIN_int32, Max = MAX_int32;
				if (CastField<FByteProperty>(&Property)) { Min = 0; Max = MAX_uint8; }
				else if (CastField<FUInt16Property>(&Property)) { Min = 0; Max = MAX_uint16; }
				else if (CastField<FUInt32Property>(&Property)) { Min = 0; Max = MAX_uint32; }
				else if (CastField<FUInt64Property>(&Property)) { Min = 0; Max = 9007199254740992.0; }
				else if (CastField<FInt8Property>(&Property)) { Min = MIN_int8; Max = MAX_int8; }
				else if (CastField<FInt16Property>(&Property)) { Min = MIN_int16; Max = MAX_int16; }
				else if (CastField<FInt64Property>(&Property)) { Min = -9007199254740992.0; Max = 9007199254740992.0; }
				if (Number != FMath::RoundToDouble(Number) || Number < Min || Number > Max)
				{
					OutError = FString::Printf(TEXT("%s must be an integer in [%g, %g] (got %g)"), *Field, Min, Max, Number);
					return false;
				}
				Numeric->SetIntPropertyValue(Dest, static_cast<int64>(Number));
			}
			else
			{
				Numeric->SetFloatingPointPropertyValue(Dest, Number);
			}
			return true;
		}

		if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(&Property))
		{
			if (Json.IsValid() && Json->Type == EJson::Null)
			{
				ObjectProp->SetObjectPropertyValue(Dest, nullptr);
				return true;
			}
			FString Path;
			if (!Json.IsValid() || Json->Type != EJson::String || !Json->TryGetString(Path))
			{
				OutError = FString::Printf(TEXT("%s must be a %s asset path, or \"\" to clear"), *Field, *ObjectProp->PropertyClass->GetName());
				return false;
			}
			if (Path.IsEmpty())
			{
				ObjectProp->SetObjectPropertyValue(Dest, nullptr);
				return true;
			}
			UObject* Loaded = LoadTyped(Path, ObjectProp->PropertyClass, OutError);
			if (!Loaded)
			{
				OutError = FString::Printf(TEXT("%s: %s"), *Field, *OutError);
				return false;
			}
			ObjectProp->SetObjectPropertyValue(Dest, Loaded);
			return true;
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(&Property); StructProp && StructProp->Struct == FVoxelLODQuality::StaticStruct())
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Json.IsValid() || !Json->TryGetObject(Object))
			{
				OutError = FString::Printf(TEXT("%s must be {gameQuality?:{min,max}, editorQuality?:{min,max}, alwaysUseGameQuality?}"), *Field);
				return false;
			}
			FVoxelLODQuality& Quality = *static_cast<FVoxelLODQuality*>(Dest);
			if (!ParseInterval(**Object, TEXT("gameQuality"), Quality.GameQuality, OutError) ||
				!ParseInterval(**Object, TEXT("editorQuality"), Quality.EditorQuality, OutError))
			{
				return false;
			}
			for (const auto& Pair : (*Object)->Values)
			{
				if (FString(*Pair.Key).Equals(TEXT("alwaysUseGameQuality"), ESearchCase::CaseSensitive))
				{
					// TryGetBoolField would read any string through FString::ToBool.
					if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Boolean)
					{
						OutError = TEXT("lodQuality.alwaysUseGameQuality must be a boolean");
						return false;
					}
					Quality.bAlwaysUseGameQuality = Pair.Value->AsBool();
				}
				else if (!FString(*Pair.Key).Equals(TEXT("gameQuality"), ESearchCase::CaseSensitive) && !FString(*Pair.Key).Equals(TEXT("editorQuality"), ESearchCase::CaseSensitive))
				{
					OutError = FString::Printf(TEXT("lodQuality takes gameQuality, editorQuality and alwaysUseGameQuality (got %s)"), *Pair.Key);
					return false;
				}
			}
			return true;
		}

		OutError = FString::Printf(TEXT("%s has an unsupported property type %s"), *Field, *Property.GetClass()->GetName());
		return false;
	}

	// Stages one JSON field against an object's property, reading the current value from Container.
	bool Stage(const FParams& Params, const FString& Field, const UStruct& Owner, const FName PropertyName, const void* Container, FStaged& Out, FString& OutError)
	{
		FProperty* Property = FindFProperty<FProperty>(&Owner, PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("%s has no property %s"), *Owner.GetName(), *PropertyName.ToString());
			return false;
		}
		TUniquePtr<FStagedValue> Staged = MakeUnique<FStagedValue>(Field, *Property, Property->ContainerPtrToValuePtr<void>(Container));
		if (!ParseInto(Field, *Property, Params->TryGetField(Field), Staged->Value, OutError))
		{
			return false;
		}
		Out.Add(MoveTemp(Staged));
		return true;
	}

	bool StageWorldFields(const FParams& Params, const TArray<FString>& Allowed, const AVoxelWorld& Container, FStaged& Out, FString& OutError)
	{
		for (const FWorldField& Field : WorldFields())
		{
			const TCHAR* Json = *Field.Spec.Name;
			if (Allowed.Contains(Field.Spec.Name) && Has(Params, Json) &&
				!Stage(Params, Json, *AVoxelWorld::StaticClass(), Field.Property, &Container, Out, OutError))
			{
				return false;
			}
		}
		return true;
	}

	// Writes staged values the way a details-panel edit does. Returns true when any value changed.
	bool Apply(UObject& Object, const FStaged& Staged, const TSharedRef<FJsonObject>& OutFields)
	{
		bool bAnyChanged = false;
		bool bModified = false;
		for (const TUniquePtr<FStagedValue>& Value : Staged)
		{
			FProperty& Property = *Value->Property;
			void* Address = Property.ContainerPtrToValuePtr<void>(&Object);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetField(TEXT("previous"), ValueJson(Property, Address));

			const bool bChanged = !Property.Identical(Address, Value->Value);
			if (bChanged)
			{
				if (!bModified)
				{
					Object.Modify();
					bModified = true;
				}
				Object.PreEditChange(&Property);
				Property.CopyCompleteValue(Address, Value->Value);
				FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
				Object.PostEditChangeProperty(Event);
				bAnyChanged = true;
			}
			Entry->SetField(TEXT("value"), ValueJson(Property, Property.ContainerPtrToValuePtr<void>(&Object)));
			Entry->SetBoolField(TEXT("changed"), bChanged);
			OutFields->SetObjectField(Value->Field, Entry);
		}
		return bAnyChanged;
	}

	AVoxelWorld* FindWorld(const FParams& Params, FString& OutError)
	{
		AActor* Actor = FindActor(Params, OutError);
		if (!Actor)
		{
			return nullptr;
		}
		AVoxelWorld* World = Cast<AVoxelWorld>(Actor);
		if (!World)
		{
			OutError = FString::Printf(TEXT("%s is a %s, not a VoxelWorld"), *Actor->GetPathName(), *Actor->GetClass()->GetName());
		}
		return World;
	}

	// Location and rotation for a spawn; malformed values are errors, absent ones default to zero.
	bool SpawnTransform(const FParams& Params, FVector& OutLocation, FRotator& OutRotation, FString& OutError)
	{
		OutLocation = FVector::ZeroVector;
		OutRotation = FRotator::ZeroRotator;
		if (Has(Params, TEXT("location")) && !Vec(Params, TEXT("location"), OutLocation))
		{
			OutError = TEXT("location must be {x,y,z}");
			return false;
		}
		if (Has(Params, TEXT("rotation")) && !Rot(Params, TEXT("rotation"), OutRotation))
		{
			OutError = TEXT("rotation must be {pitch,yaw,roll}");
			return false;
		}
		return true;
	}

	// Places an actor in the editor's current level through the editor actor subsystem (selects it, like a drag-in).
	AActor* SpawnInEditor(UClass* Class, const FVector& Location, const FRotator& Rotation, FString& OutError)
	{
		if (!GEditor || !EditorWorld())
		{
			OutError = TEXT("No editor world");
			return nullptr;
		}
		if (GEditor->PlayWorld)
		{
			OutError = TEXT("Stop PIE before spawning editor actors");
			return nullptr;
		}
		UEditorActorSubsystem* Actors = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
		if (!Actors)
		{
			OutError = TEXT("EditorActorSubsystem is unavailable");
			return nullptr;
		}
		AActor* Actor = Actors->SpawnActorFromClass(Class, Location, Rotation);
		if (!Actor)
		{
			OutError = FString::Printf(TEXT("Failed to spawn %s"), *Class->GetName());
		}
		return Actor;
	}

	const TArray<FString>& SpawnWorldFields()
	{
		static const TArray<FString> Fields = { TEXT("layerStack"), TEXT("megaMaterial"), TEXT("voxelSize") };
		return Fields;
	}

	FResult WorldSpawn(const FParams& Params)
	{
		FString Err;
		FVector Location;
		FRotator Rotation;
		if (!SpawnTransform(Params, Location, Rotation, Err)) return Error(Err);

		FStaged Staged;
		if (!StageWorldFields(Params, SpawnWorldFields(), *GetDefault<AVoxelWorld>(), Staged, Err)) return Error(Err);

		const FString Label = Str(Params, TEXT("label"));
		if (Has(Params, TEXT("label")) && Label.IsEmpty()) return Error(TEXT("label must be a non-empty string"));

		const FScopedTransaction Transaction(LOCTEXT("SpawnVoxelWorld", "Spawn Voxel World"));
		AVoxelWorld* World = Cast<AVoxelWorld>(SpawnInEditor(AVoxelWorld::StaticClass(), Location, Rotation, Err));
		if (!World) return Error(Err);

		if (!Label.IsEmpty())
		{
			World->SetActorLabel(Label);
		}
		TSharedRef<FJsonObject> Applied = MakeShared<FJsonObject>();
		Apply(*World, Staged, Applied);

		TSharedRef<FJsonObject> Out = ActorJson(*World);
		Out->SetObjectField(TEXT("applied"), Applied);
		Out->SetBoolField(TEXT("runtimeCreated"), World->IsRuntimeCreated());
		Out->SetBoolField(TEXT("changed"), true);
		return Ok(Out);
	}

	FResult WorldConfigure(const FParams& Params)
	{
		FString Err;
		AVoxelWorld* World = FindWorld(Params, Err);
		if (!World) return Error(Err);

		// Unknown fields never get here: the contract refuses them (RunHandler).
		TArray<FString> Editable;
		for (const FWorldField& Field : WorldFields())
		{
			Editable.Add(Field.Spec.Name);
		}

		FStaged Staged;
		if (!StageWorldFields(Params, Editable, *World, Staged, Err)) return Error(Err);
		if (Staged.Num() == 0)
		{
			return Error(FString::Printf(TEXT("No fields to set. Editable fields: %s"), *FString::Join(Editable, TEXT(", "))));
		}

		const FScopedTransaction Transaction(LOCTEXT("ConfigureVoxelWorld", "Configure Voxel World"));
		TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		const bool bChanged = Apply(*World, Staged, Fields);

		TSharedRef<FJsonObject> Out = ActorJson(*World);
		Out->SetObjectField(TEXT("fields"), Fields);
		Out->SetBoolField(TEXT("changed"), bChanged);
		return Ok(Out);
	}

	FResult WorldStatus(const FParams& Params)
	{
		FString Err;
		AVoxelWorld* World = FindWorld(Params, Err);
		if (!World) return Error(Err);

		TSharedRef<FJsonObject> Out = ActorJson(*World);
		Out->SetBoolField(TEXT("runtimeCreated"), World->IsRuntimeCreated());
		Out->SetBoolField(TEXT("isReady"), World->IsVoxelWorldReady());
		Out->SetBoolField(TEXT("processingNewState"), World->IsProcessingNewState());
		Out->SetNumberField(TEXT("progress"), World->GetProgress());
		Out->SetNumberField(TEXT("pendingTasks"), World->GetNumPendingTasks());
		Out->SetField(TEXT("layerStack"), PathJson(World->LayerStack));
		Out->SetField(TEXT("megaMaterial"), PathJson(World->MegaMaterial));
		Out->SetNumberField(TEXT("voxelSize"), World->VoxelSize);
		return Ok(Out);
	}

	FResult WorldRuntime(const FParams& Params)
	{
		FString Err;
		AVoxelWorld* World = FindWorld(Params, Err);
		if (!World) return Error(Err);

		const FString Op = Str(Params, TEXT("op"));
		const bool bCreate = Op.Equals(TEXT("create"), ESearchCase::CaseSensitive);
		if (!bCreate && !Op.Equals(TEXT("destroy"), ESearchCase::CaseSensitive)) return Error(TEXT("op must be 'create' or 'destroy'"));

		// The runtime is transient state, so there is nothing to record in a transaction.
		const bool bWasCreated = World->IsRuntimeCreated();
		if (bCreate && !bWasCreated)
		{
			World->CreateRuntime();
		}
		else if (!bCreate && bWasCreated)
		{
			World->DestroyRuntime();
		}

		TSharedRef<FJsonObject> Out = ActorJson(*World);
		Out->SetBoolField(TEXT("runtimeCreated"), World->IsRuntimeCreated());
		Out->SetBoolField(TEXT("changed"), World->IsRuntimeCreated() != bWasCreated);
		return Ok(Out);
	}

	FResult ActorSpawn(const FParams& Params)
	{
		static const TMap<FString, UClass*> Kinds =
		{
			{ TEXT("stamp"), AVoxelStampActor::StaticClass() },
			{ TEXT("height_sculpt"), AVoxelSculptHeight::StaticClass() },
			{ TEXT("volume_sculpt"), AVoxelSculptVolume::StaticClass() },
			{ TEXT("collision_baker"), AVoxelCollisionBaker::StaticClass() },
			{ TEXT("debug"), AVoxelDebugActor::StaticClass() },
		};
		const FString Kind = Str(Params, TEXT("kind"));
		UClass* const* Class = Kinds.Find(Kind);
		if (!Class)
		{
			TArray<FString> Names;
			Kinds.GetKeys(Names);
			return Error(FString::Printf(TEXT("kind must be one of: %s"), *FString::Join(Names, TEXT(", "))));
		}

		FString Err;
		FVector Location;
		FRotator Rotation;
		if (!SpawnTransform(Params, Location, Rotation, Err)) return Error(Err);
		const FString Label = Str(Params, TEXT("label"));
		if (Has(Params, TEXT("label")) && Label.IsEmpty()) return Error(TEXT("label must be a non-empty string"));

		const FScopedTransaction Transaction(LOCTEXT("SpawnVoxelActor", "Spawn Voxel Actor"));
		AActor* Actor = SpawnInEditor(*Class, Location, Rotation, Err);
		if (!Actor) return Error(Err);
		if (!Label.IsEmpty())
		{
			// Stamp actors treat the label as a prefix and append their stamp description.
			Actor->SetActorLabel(Label);
		}

		TSharedRef<FJsonObject> Out = ActorJson(*Actor);
		Out->SetBoolField(TEXT("changed"), true);
		return Ok(Out);
	}

	FResult ComponentAdd(const FParams& Params)
	{
		static const TMap<FString, UClass*> Kinds =
		{
			{ TEXT("stamp"), UVoxelStampComponent::StaticClass() },
			{ TEXT("instanced_stamp"), UVoxelInstancedStampComponent::StaticClass() },
			{ TEXT("no_clipping"), UVoxelNoClippingComponent::StaticClass() },
		};
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Error(Err);

		const FString Kind = Str(Params, TEXT("kind"));
		UClass* const* Class = Kinds.Find(Kind);
		if (!Class)
		{
			TArray<FString> Names;
			Kinds.GetKeys(Names);
			return Error(FString::Printf(TEXT("kind must be one of: %s"), *FString::Join(Names, TEXT(", "))));
		}

		FName Name;
		const FString Requested = Str(Params, TEXT("componentName"));
		if (Requested.IsEmpty())
		{
			if (Has(Params, TEXT("componentName"))) return Error(TEXT("componentName must be a non-empty string"));
			Name = MakeUniqueObjectName(Actor, *Class, (*Class)->GetFName());
		}
		else
		{
			FText Reason;
			if (!FName::IsValidXName(Requested, INVALID_OBJECTNAME_CHARACTERS, &Reason))
			{
				return Error(FString::Printf(TEXT("componentName '%s' is invalid: %s"), *Requested, *Reason.ToString()));
			}
			Name = FName(*Requested);
			if (StaticFindObjectFast(UObject::StaticClass(), Actor, Name))
			{
				return Error(FString::Printf(TEXT("%s already has a subobject named %s"), *Actor->GetPathName(), *Requested));
			}
		}

		const FScopedTransaction Transaction(LOCTEXT("AddVoxelComponent", "Add Voxel Component"));
		Actor->Modify();
		UActorComponent* Component = NewObject<UActorComponent>(Actor, *Class, Name, RF_Transactional);
		bool bIsRoot = false;
		if (USceneComponent* Scene = Cast<USceneComponent>(Component))
		{
			if (USceneComponent* Root = Actor->GetRootComponent())
			{
				Scene->SetupAttachment(Root);
			}
			else
			{
				Actor->SetRootComponent(Scene);
				bIsRoot = true;
			}
		}
		Actor->AddInstanceComponent(Component);
		Component->OnComponentCreated();
		Component->RegisterComponent();
		Actor->PostEditChange();

		TSharedRef<FJsonObject> Out = ActorJson(*Actor);
		Out->SetStringField(TEXT("componentName"), Component->GetName());
		Out->SetStringField(TEXT("componentClass"), Component->GetClass()->GetName());
		Out->SetBoolField(TEXT("isRoot"), bIsRoot);
		Out->SetBoolField(TEXT("changed"), true);
		return Ok(Out);
	}

	FResult NoClippingSetLayer(const FParams& Params)
	{
		FString Err;
		AActor* Actor = FindActor(Params, Err);
		if (!Actor) return Error(Err);
		UVoxelNoClippingComponent* Component = FindComponent<UVoxelNoClippingComponent>(*Actor, Params, Err);
		if (!Component) return Error(Err);

		const bool bLayer = Has(Params, TEXT("stack")) || Has(Params, TEXT("layer"));
		const bool bAuto = Has(Params, TEXT("autoAdjustPlayer"));
		if (!bLayer && !bAuto) return Error(TEXT("Pass stack/layer and/or autoAdjustPlayer"));

		FStaged Staged;
		const UClass& Class = *UVoxelNoClippingComponent::StaticClass();
		if (bLayer)
		{
			// The component samples a volume layer (VoxelNoClippingComponent.cpp, Query.SampleVolumeLayer).
			FVoxelStackLayer Layer;
			if (!ParseStackLayer(Params, TEXT("stack"), TEXT("layer"), false, Layer, Err)) return Error(Err);
			FProperty* Property = FindFProperty<FProperty>(&Class, GET_MEMBER_NAME_CHECKED(UVoxelNoClippingComponent, Layer));
			if (!Property) return Error(TEXT("UVoxelNoClippingComponent has no Layer property"));
			Staged.Add(MakeUnique<FStagedValue>(TEXT("layer"), *Property, &Layer));
		}
		if (bAuto && !Stage(Params, TEXT("autoAdjustPlayer"), Class, GET_MEMBER_NAME_CHECKED(UVoxelNoClippingComponent, bAutoAdjustPlayer), Component, Staged, Err))
		{
			return Error(Err);
		}

		const FScopedTransaction Transaction(LOCTEXT("SetNoClippingLayer", "Set Voxel No Clipping Layer"));
		TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		const bool bChanged = Apply(*Component, Staged, Fields);

		TSharedRef<FJsonObject> Out = ActorJson(*Actor);
		Out->SetStringField(TEXT("componentName"), Component->GetName());
		Out->SetObjectField(TEXT("fields"), Fields);
		Out->SetBoolField(TEXT("changed"), bChanged);
		return Ok(Out);
	}
}

namespace
{
	FString HookStateName(const EVoxelShaderHookState State)
	{
		switch (State)
		{
		case EVoxelShaderHookState::NeverApply: return TEXT("disabled");
		case EVoxelShaderHookState::Active: return TEXT("active");
		case EVoxelShaderHookState::Outdated: return TEXT("outdated");
		case EVoxelShaderHookState::NotApplied: return TEXT("not_applied");
		case EVoxelShaderHookState::Invalid: return TEXT("invalid");
		case EVoxelShaderHookState::Deprecated: return TEXT("deprecated");
		default: return TEXT("unknown");
		}
	}

	// Voxel materials render only when Voxel's patches to the engine shaders are applied; without them every
	// generated surface shader compiles its voxel code out and the terrain shows the grid fallback, with no error.
	FResult ShaderHooksStatus(const FParams& Params)
	{
		if (!GVoxelShaderHooksManager) return Error(TEXT("Voxel shader hook manager is not available"));

		TArray<TSharedPtr<FJsonValue>> Groups;
		bool bAllActive = true;
		for (const FVoxelShaderHookGroup* Group : GVoxelShaderHooksManager->Hooks)
		{
			if (!Group) continue;
			const EVoxelShaderHookState State = Group->GetState();
			const bool bRequired = Group->IsEnabled();
			bAllActive &= !bRequired || State == EVoxelShaderHookState::Active;

			TSharedRef<FJsonObject> G = MakeShared<FJsonObject>();
			G->SetStringField(TEXT("name"), Group->DisplayName);
			G->SetStringField(TEXT("state"), HookStateName(State));
			G->SetBoolField(TEXT("enabled"), bRequired);
			TArray<TSharedPtr<FJsonValue>> Pending;
			for (const FVoxelShaderHook& Hook : Group->Hooks)
			{
				if (Hook.GetState() != EVoxelShaderHookState::Active && Hook.GetState() != EVoxelShaderHookState::Deprecated)
				{
					Pending.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s [%s]"), *Hook.ShaderGuid.ToString(), *HookStateName(Hook.GetState()))));
				}
			}
			if (Pending.Num() > 0) G->SetArrayField(TEXT("pendingHooks"), Pending);
			Groups.Add(MakeShared<FJsonValueObject>(G));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("allActive"), bAllActive);
		Out->SetArrayField(TEXT("groups"), Groups);
		if (!bAllActive)
		{
			Out->SetStringField(TEXT("fix"), TEXT("Close the editor, run `UnrealEditor-Cmd.exe <project>.uproject -run=ApplyVoxelShaderHooks`, then restart so shaders recompile. The patch lives in the engine install: every machine that compiles shaders needs it."));
		}
		return Ok(Out);
	}
}

void AddWorldHandlers(TArray<FHandlerEntry>& Out)
{
	const auto Location = [] { return Spec::Vec3(TEXT("location"), TEXT("World location in centimetres; default the origin.")); };
	const auto Rotation = [] { return MCPParam::Optional(TEXT("rotation"), EMCPParamType::Rotator, TEXT("World rotation in degrees; default zero.")); };
	const auto Label = [](const TCHAR* Description) { return MCPParam::Optional(TEXT("label"), EMCPParamType::String, Description); };
	const auto WorldActor = [](TArray<FMCPParamSpec> Rest)
	{
		Rest.Insert({
			Spec::ActorPath(TEXT("AVoxelWorld actor object path; preferred, since labels can repeat.")),
			Spec::ActorLabel(TEXT("AVoxelWorld actor label; must match exactly one actor.")),
		}, 0);
		return Rest;
	};

	Out.Add({ TEXT("voxel_shader_hooks_status"), &ShaderHooksStatus, {} });

	TArray<FMCPParamSpec> SpawnParams = {
		Label(TEXT("Actor label; default the engine's generated label.")),
		Location(),
		Rotation(),
	};
	for (const FString& Name : SpawnWorldFields())
	{
		SpawnParams.Add(WorldField(*Name).Spec);
	}
	SpawnParams.Add(Spec::SaveDirty());
	Out.Add({ TEXT("voxel_world_spawn"), &WorldSpawn, SpawnParams,
		MCPSpec::ContractExempt(TEXT("Every parameter is optional, so the contract values spawn a voxel world before anything can fail")) });

	TArray<FMCPParamSpec> ConfigureFields;
	for (const FWorldField& Field : WorldFields())
	{
		ConfigureFields.Add(Field.Spec);
	}
	TArray<FMCPParamSpec> ConfigureParams = WorldActor(ConfigureFields);
	ConfigureParams.Add(Spec::SaveDirty());
	Out.Add({ TEXT("voxel_world_configure"), &WorldConfigure, ConfigureParams, Spec::OneActor().AtLeastOne(Spec::Branches(ConfigureFields)) });

	Out.Add({ TEXT("voxel_world_status"), &WorldStatus, WorldActor({}), Spec::OneActor() });

	Out.Add({ TEXT("voxel_world_runtime"), &WorldRuntime, WorldActor({
		MCPParam::Required(TEXT("op"), EMCPParamType::String, TEXT("create builds the world's runtime, destroy releases it; transient and not undoable."))
			.Enum({ TEXT("create"), TEXT("destroy") }),
		Spec::SaveDirty(),
	}), Spec::OneActor() });

	Out.Add({ TEXT("voxel_actor_spawn"), &ActorSpawn, {
		MCPParam::Required(TEXT("kind"), EMCPParamType::String,
			TEXT("stamp (AVoxelStampActor), height_sculpt (AVoxelSculptHeight), volume_sculpt (AVoxelSculptVolume), collision_baker (AVoxelCollisionBaker) or debug (AVoxelDebugActor)."))
			.Enum({ TEXT("stamp"), TEXT("height_sculpt"), TEXT("volume_sculpt"), TEXT("collision_baker"), TEXT("debug") }),
		Label(TEXT("Actor label, non-empty; stamp actors use it as a prefix and append their stamp description.")),
		Location(),
		Rotation(),
		Spec::SaveDirty(),
	}, MCPSpec::ContractExempt(TEXT("The contract values spawn a Voxel actor before anything can fail")) });

	Out.Add({ TEXT("voxel_component_add"), &ComponentAdd, {
		Spec::ActorPath(TEXT("Actor object path; preferred, since labels can repeat.")),
		Spec::ActorLabel(TEXT("Actor label; must match exactly one actor.")),
		MCPParam::Required(TEXT("kind"), EMCPParamType::String,
			TEXT("stamp (UVoxelStampComponent), instanced_stamp (UVoxelInstancedStampComponent) or no_clipping (UVoxelNoClippingComponent)."))
			.Enum({ TEXT("stamp"), TEXT("instanced_stamp"), TEXT("no_clipping") }),
		Spec::ComponentName(TEXT("Object name for the new component, a valid object name not used by another subobject of the actor; default a generated unique name.")),
		Spec::SaveDirty(),
	}, Spec::OneActor() });

	Out.Add({ TEXT("voxel_no_clipping_set_layer"), &NoClippingSetLayer, {
		Spec::ActorPath(TEXT("Actor object path; preferred, since labels can repeat.")),
		Spec::ActorLabel(TEXT("Actor label; must match exactly one actor.")),
		Spec::ComponentName(TEXT("UVoxelNoClippingComponent object name; default the actor's first one.")),
		MCPParam::Optional(TEXT("stack"), EMCPParamType::String, TEXT("UVoxelLayerStack asset path; with only layer given, Voxel's built-in default stack.")),
		MCPParam::Optional(TEXT("layer"), EMCPParamType::String, TEXT("UVoxelVolumeLayer asset path, since the component samples a volume layer; with only stack given, the default volume layer.")),
		MCPParam::Optional(TEXT("autoAdjustPlayer"), EMCPParamType::Boolean, TEXT("Teleport the owner back to its last valid location when it clips into the volume.")),
		Spec::SaveDirty(),
	}, Spec::OneActor().AtLeastOne({ { TEXT("stack") }, { TEXT("layer") }, { TEXT("autoAdjustPlayer") } }) });
}
}

#undef LOCTEXT_NAMESPACE
