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

#define LOCTEXT_NAMESPACE "VoxelPluginTools"

namespace VoxelPluginTools
{
namespace
{
	// JSON field name to the AVoxelWorld property it edits.
	struct FWorldField
	{
		const TCHAR* Json;
		FName Property;
	};

	const TArray<FWorldField>& WorldFields()
	{
		static const TArray<FWorldField> Fields =
		{
			{ TEXT("layerStack"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, LayerStack) },
			{ TEXT("megaMaterial"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, MegaMaterial) },
			{ TEXT("voxelSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, VoxelSize) },
			{ TEXT("lodQuality"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, LODQuality) },
			{ TEXT("qualityExponent"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, QualityExponent) },
			{ TEXT("enableNanite"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableNanite) },
			{ TEXT("enableTessellation"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableTessellation) },
			{ TEXT("enableLumen"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableLumen) },
			{ TEXT("enableRaytracing"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableRaytracing) },
			{ TEXT("generateMeshDistanceFields"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bGenerateMeshDistanceFields) },
			{ TEXT("blockinessMetadata"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, BlockinessMetadata) },
			{ TEXT("renderChunkSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, RenderChunkSize) },
			{ TEXT("useCameraAsInvoker"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bUseCameraAsInvoker) },
			{ TEXT("createRuntimeOnBeginPlay"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bCreateRuntimeOnBeginPlay) },
			{ TEXT("waitOnBeginPlay"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bWaitOnBeginPlay) },
			{ TEXT("limitMaxLOD"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bLimitMaxLOD) },
			{ TEXT("maxLOD"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, MaxLOD) },
			{ TEXT("maxBackgroundTasks"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, MaxBackgroundTasks) },
			{ TEXT("doubleSidedCollision"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bDoubleSidedCollision) },
			{ TEXT("generateOverlapEvents"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bGenerateOverlapEvents) },
			{ TEXT("collisionChunkSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, CollisionChunkSize) },
			{ TEXT("overrideCollisionVoxelSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bOverrideCollisionVoxelSize) },
			{ TEXT("collisionVoxelSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, CollisionVoxelSize) },
			{ TEXT("enableNavigation"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bEnableNavigation) },
			{ TEXT("navigationChunkSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, NavigationChunkSize) },
			{ TEXT("generateNavigationInsideNavMeshBounds"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bGenerateNavigationInsideNavMeshBounds) },
			{ TEXT("onlyGenerateNavigationInEditor"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bOnlyGenerateNavigationInEditor) },
			{ TEXT("overrideNavigationVoxelSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bOverrideNavigationVoxelSize) },
			{ TEXT("navigationVoxelSize"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, NavigationVoxelSize) },
			{ TEXT("renderScatterActors"), GET_MEMBER_NAME_CHECKED(AVoxelWorld, bRenderScatterActors) },
		};
		return Fields;
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
		(*Interval)->TryGetNumberField(TEXT("min"), Min);
		(*Interval)->TryGetNumberField(TEXT("max"), Max);
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
				if (Number != FMath::RoundToDouble(Number) || Number < MIN_int32 || Number > MAX_int32)
				{
					OutError = FString::Printf(TEXT("%s must be a 32-bit integer (got %g)"), *Field, Number);
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
			if ((*Object)->HasField(TEXT("alwaysUseGameQuality")) && !(*Object)->TryGetBoolField(TEXT("alwaysUseGameQuality"), Quality.bAlwaysUseGameQuality))
			{
				OutError = TEXT("lodQuality.alwaysUseGameQuality must be a boolean");
				return false;
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
			if (Allowed.Contains(Field.Json) && Has(Params, Field.Json) &&
				!Stage(Params, Field.Json, *AVoxelWorld::StaticClass(), Field.Property, &Container, Out, OutError))
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

		TArray<FString> Known = { TEXT("actorPath"), TEXT("actorLabel") };
		TArray<FString> Editable;
		for (const FWorldField& Field : WorldFields())
		{
			Known.Add(Field.Json);
			Editable.Add(Field.Json);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
		{
			if (!Known.Contains(Pair.Key))
			{
				return Error(FString::Printf(TEXT("Unknown field '%s'. Editable fields: %s"), *Pair.Key, *FString::Join(Editable, TEXT(", "))));
			}
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
		const bool bCreate = Op == TEXT("create");
		if (!bCreate && Op != TEXT("destroy")) return Error(TEXT("op must be 'create' or 'destroy'"));

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

void AddWorldHandlers(TArray<FHandlerEntry>& Out)
{
	Out.Append(
	{
		{ TEXT("voxel_world_spawn"), &WorldSpawn },
		{ TEXT("voxel_world_configure"), &WorldConfigure },
		{ TEXT("voxel_world_status"), &WorldStatus },
		{ TEXT("voxel_world_runtime"), &WorldRuntime },
		{ TEXT("voxel_actor_spawn"), &ActorSpawn },
		{ TEXT("voxel_component_add"), &ComponentAdd },
		{ TEXT("voxel_no_clipping_set_layer"), &NoClippingSetLayer },
	});
}
}

#undef LOCTEXT_NAMESPACE
