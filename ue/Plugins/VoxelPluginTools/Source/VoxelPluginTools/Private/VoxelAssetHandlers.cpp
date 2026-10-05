#include "VoxelToolsCommon.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "VoxelMinimal.h"
#include "VoxelMinimal/VoxelAutoFactoryInterface.h"
#include "VoxelGraph.h"
#include "VoxelParameter.h"
#include "VoxelPinType.h"
#include "VoxelPinValue.h"
#include "VoxelParameterOverridesOwner.h"
#include "Buffer/VoxelBaseBuffers.h"
#include "VoxelLayer.h"
#include "VoxelLayerStack.h"
#include "VoxelStackLayer.h"
#include "VoxelMetadata.h"
#include "Heightmap/VoxelHeightmap.h"
#include "MegaMaterial/VoxelMegaMaterial.h"
#include "Sculpt/Height/VoxelSculptHeightAsset.h"
#include "Sculpt/Volume/VoxelSculptVolumeAsset.h"
#include "StaticMesh/VoxelStaticMesh.h"
#include "Surface/VoxelSurfaceTypeAsset.h"
#include "Surface/VoxelSmartSurfaceType.h"
#include "Surface/VoxelSmartSurfaceTypeGraph.h"
#include "VoxelExposedSeed.h"

#include "PCGCommon.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGApplyOnVoxelGraphSettings.h"
#include "PCGCallVoxelGraph.h"
#include "PCGCreateVoxelSpline.h"
#include "PCGSpawnActorWithVoxelGraph.h"
#include "PCGVoxelElevationIsolines.h"
#include "PCGVoxelProjection.h"
#include "PCGVoxelQuery.h"
#include "PCGVoxelSampler.h"
#include "PCGVoxelSamplerV2.h"
#include "PCGVoxelStampSpawner.h"
#include "PCGWaitForVoxelWorld.h"

#define LOCTEXT_NAMESPACE "VoxelPluginTools"

namespace VoxelPluginTools
{
namespace
{
	///////////////////////////////////////////////////////////////////////////
	// Property editing: every edit is parsed into a detached value first, so a bad input never touches the asset.
	///////////////////////////////////////////////////////////////////////////

	struct FPendingEdit
	{
		FProperty* Property = nullptr;
		FDefaultConstructedPropertyElement Value;
	};

	FString ExportValue(const FProperty& Property, const void* Value, UObject* Owner)
	{
		FString Text;
		Property.ExportTextItem_Direct(Text, Value, nullptr, Owner, PPF_None);
		return Text;
	}

	// Seeds the detached value with the current one, then lets Fill modify it.
	bool PrepareCustom(UObject& Object, FProperty* Property, TFunctionRef<bool(void*, FString&)> Fill, FPendingEdit& Out, FString& OutError)
	{
		if (!Property)
		{
			OutError = TEXT("Property not found");
			return false;
		}
		Out.Property = Property;
		Out.Value = FDefaultConstructedPropertyElement(Property);
		Property->CopyCompleteValue(Out.Value.GetObjAddress(), Property->ContainerPtrToValuePtr<void>(&Object));
		return Fill(Out.Value.GetObjAddress(), OutError);
	}

	// Asset references in import text are loaded up front so a bad path is an error, not a silent None.
	bool PreloadObjectRefs(const FProperty& Property, const FString& Text, FString& OutError)
	{
		const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(&Property);
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
		{
			ObjectProperty = CastField<FObjectProperty>(ArrayProperty->Inner);
		}
		if (!ObjectProperty)
		{
			return true;
		}

		FString Body = Text.TrimStartAndEnd();
		if (Body.StartsWith(TEXT("(")) && Body.EndsWith(TEXT(")")))
		{
			Body = Body.Mid(1, Body.Len() - 2);
		}
		TArray<FString> Tokens;
		Body.ParseIntoArray(Tokens, TEXT(","), true);
		for (FString Token : Tokens)
		{
			Token = Token.TrimStartAndEnd().TrimQuotes();
			int32 First = INDEX_NONE;
			int32 Last = INDEX_NONE;
			if (Token.FindChar(TEXT('\''), First) && Token.FindLastChar(TEXT('\''), Last) && Last > First)
			{
				Token = Token.Mid(First + 1, Last - First - 1);
			}
			if (Token.IsEmpty() || Token.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (!LoadTyped(Token, ObjectProperty->PropertyClass, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool PrepareText(UObject& Object, FProperty* Property, const FString& InText, FPendingEdit& Out, FString& OutError)
	{
		if (!Property)
		{
			OutError = TEXT("Property not found");
			return false;
		}
		// An empty string clears an object reference.
		const FString Text = InText.TrimStartAndEnd().IsEmpty() && Property->IsA<FObjectPropertyBase>() ? FString(TEXT("None")) : InText;
		const bool bTextual = Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FTextProperty>();
		if (Text.TrimStartAndEnd().IsEmpty() && !bTextual)
		{
			OutError = FString::Printf(TEXT("Empty value for %s"), *Property->GetName());
			return false;
		}
		if (!PreloadObjectRefs(*Property, Text, OutError))
		{
			return false;
		}
		return PrepareCustom(Object, Property, [&](void* Value, FString& Err)
		{
			FStringOutputDevice Errors;
			const TCHAR* End = Property->ImportText_Direct(*Text, Value, &Object, PPF_None, &Errors);
			if (!End || !Errors.IsEmpty())
			{
				Err = FString::Printf(TEXT("'%s' is not a valid %s value for %s%s%s"), *Text, *Property->GetCPPType(), *Property->GetName(),
					Errors.IsEmpty() ? TEXT("") : TEXT(": "), *static_cast<const FString&>(Errors).TrimStartAndEnd());
				return false;
			}
			if (!FString(End).TrimStartAndEnd().IsEmpty())
			{
				Err = FString::Printf(TEXT("Unparsed trailing text '%s' in the %s value"), End, *Property->GetName());
				return false;
			}
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property); Numeric && !Numeric->IsEnum())
			{
				const double Number = Numeric->IsFloatingPoint()
					? Numeric->GetFloatingPointPropertyValue(Value)
					: static_cast<double>(Numeric->GetSignedIntPropertyValue(Value));
				if (Property->HasMetaData(TEXT("ClampMin")) && Number < FCString::Atod(*Property->GetMetaData(TEXT("ClampMin"))))
				{
					Err = FString::Printf(TEXT("%s must be >= %s"), *Property->GetName(), *Property->GetMetaData(TEXT("ClampMin")));
					return false;
				}
				if (Property->HasMetaData(TEXT("ClampMax")) && Number > FCString::Atod(*Property->GetMetaData(TEXT("ClampMax"))))
				{
					Err = FString::Printf(TEXT("%s must be <= %s"), *Property->GetName(), *Property->GetMetaData(TEXT("ClampMax")));
					return false;
				}
			}
			if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
			{
				const FString Trimmed = Text.TrimStartAndEnd();
				if (!Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase) && !ObjectProperty->GetObjectPropertyValue(Value))
				{
					Err = FString::Printf(TEXT("'%s' did not resolve to a %s"), *Text, *ObjectProperty->PropertyClass->GetName());
					return false;
				}
			}
			return true;
		}, Out, OutError);
	}

	// JSON scalar or array to UE import text.
	FString JsonToText(const FProperty& Property, const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		if (Value->Type == EJson::Array)
		{
			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				FString Text;
				Items.Add(Item.IsValid() && Item->TryGetString(Text) ? TEXT("\"") + Text.ReplaceCharWithEscapedChar() + TEXT("\"") : ValueText(Item));
			}
			return TEXT("(") + FString::Join(Items, TEXT(",")) + TEXT(")");
		}
		double Number = 0;
		if (Value->Type == EJson::Number && Value->TryGetNumber(Number))
		{
			const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
			if (Numeric && Numeric->IsInteger())
			{
				return FString::Printf(TEXT("%lld"), static_cast<long long>(FMath::RoundToDouble(Number)));
			}
		}
		return ValueText(Value);
	}

	bool PrepareJson(UObject& Object, const FName PropertyName, const TSharedPtr<FJsonValue>& Value, FPendingEdit& Out, FString& OutError)
	{
		FProperty* Property = Object.GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("%s has no property %s"), *Object.GetClass()->GetName(), *PropertyName.ToString());
			return false;
		}
		return PrepareText(Object, Property, JsonToText(*Property, Value), Out, OutError);
	}

	// Object array from asset paths; deduplicated, in the given order, after the current entries when appending.
	bool PrepareObjectArray(UObject& Object, const FName PropertyName, const TArray<FString>& Paths, const bool bAppend, FPendingEdit& Out, FString& OutError)
	{
		FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Object.GetClass()->FindPropertyByName(PropertyName));
		FObjectPropertyBase* Inner = ArrayProperty ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
		if (!Inner)
		{
			OutError = FString::Printf(TEXT("%s.%s is not an object array"), *Object.GetClass()->GetName(), *PropertyName.ToString());
			return false;
		}
		TArray<UObject*> Objects;
		for (const FString& Path : Paths)
		{
			UObject* Loaded = LoadTyped(Path, Inner->PropertyClass, OutError);
			if (!Loaded)
			{
				return false;
			}
			Objects.Add(Loaded);
		}
		return PrepareCustom(Object, ArrayProperty, [&](void* Value, FString&)
		{
			FScriptArrayHelper Helper(ArrayProperty, Value);
			if (!bAppend)
			{
				Helper.EmptyValues();
			}
			for (UObject* Item : Objects)
			{
				bool bPresent = false;
				for (int32 Index = 0; Index < Helper.Num() && !bPresent; Index++)
				{
					bPresent = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)) == Item;
				}
				if (!bPresent)
				{
					Inner->SetObjectPropertyValue(Helper.GetRawPtr(Helper.AddValue()), Item);
				}
			}
			return true;
		}, Out, OutError);
	}

	// Modify, then Pre/PostEditChange per property so the owner reacts exactly as to a details-panel edit.
	bool ApplyEdits(UObject& Object, TArray<FPendingEdit>& Edits, const TSharedRef<FJsonObject>& Out)
	{
		Object.Modify();
		bool bChanged = false;
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		for (FPendingEdit& Edit : Edits)
		{
			void* Dest = Edit.Property->ContainerPtrToValuePtr<void>(&Object);
			const FString Previous = ExportValue(*Edit.Property, Dest, &Object);

			Object.PreEditChange(Edit.Property);
			Edit.Property->CopyCompleteValue(Dest, Edit.Value.GetObjAddress());
			FPropertyChangedEvent Event(Edit.Property, EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(Event);

			const FString Current = ExportValue(*Edit.Property, Dest, &Object);
			bChanged |= Previous != Current;
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("previous"), Previous);
			Entry->SetStringField(TEXT("value"), Current);
			Properties->SetObjectField(Edit.Property->GetName(), Entry);
		}
		Out->SetObjectField(TEXT("properties"), Properties);
		Out->SetBoolField(TEXT("changed"), bChanged);
		return bChanged;
	}

	void SaveAsset(UObject& Asset, const FParams& Params, const TSharedRef<FJsonObject>& Out)
	{
		Asset.MarkPackageDirty();
		bool bSaved = false;
		if (Bool(Params, TEXT("save"), true))
		{
			bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Asset.GetPackage() }, false);
		}
		Out->SetBoolField(TEXT("saved"), bSaved);
	}

	TSharedRef<FJsonObject> AssetJson(const UObject& Asset)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("assetPath"), Asset.GetPathName());
		Out->SetStringField(TEXT("class"), Asset.GetClass()->GetName());
		return Out;
	}

	// Absent field: true with Out empty. Present but not an array of strings: false.
	bool StrArray(const FParams& Params, const TCHAR* Field, TArray<FString>& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(Field, Values))
		{
			OutError = FString::Printf(TEXT("%s must be an array of asset paths"), Field);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value.IsValid() || !Value->TryGetString(Text))
			{
				OutError = FString::Printf(TEXT("%s must contain only strings"), Field);
				return false;
			}
			Out.Add(Text);
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> PathsJson(const TArray<UObject*>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const UObject* Object : Objects)
		{
			Out.Add(MakeShared<FJsonValueString>(Object ? Object->GetPathName() : TEXT("None")));
		}
		return Out;
	}

	template<typename T>
	TArray<UObject*> ToObjects(const TArray<TObjectPtr<T>>& In)
	{
		TArray<UObject*> Out;
		for (const TObjectPtr<T>& Item : In)
		{
			Out.Add(Item.Get());
		}
		return Out;
	}

	struct FFieldMap
	{
		const TCHAR* Param;
		FName Property;
	};

	bool PrepareFields(UObject& Object, const FParams& Params, TConstArrayView<FFieldMap> Fields, TArray<FPendingEdit>& Edits, FString& OutError)
	{
		for (const FFieldMap& Field : Fields)
		{
			if (!Has(Params, Field.Param))
			{
				continue;
			}
			if (!PrepareJson(Object, Field.Property, Params->TryGetField(Field.Param), Edits.AddDefaulted_GetRef(), OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), Field.Param, *OutError);
				return false;
			}
		}
		return true;
	}

	///////////////////////////////////////////////////////////////////////////
	// voxel_asset_create
	///////////////////////////////////////////////////////////////////////////

	struct FAssetType
	{
		const TCHAR* Key;
		// Dedicated factory (private headers, so by class path); null means Voxel's auto factory for Class.
		const TCHAR* FactoryPath;
		UClass* (*Class)();
	};

	const FAssetType GAssetTypes[] =
	{
		{ TEXT("height_graph"), TEXT("/Script/VoxelEditor.VoxelHeightGraphFactory"), nullptr },
		{ TEXT("volume_graph"), TEXT("/Script/VoxelEditor.VoxelVolumeGraphFactory"), nullptr },
		{ TEXT("scatter_graph"), TEXT("/Script/VoxelEditor.VoxelScatterGraphFactory"), nullptr },
		{ TEXT("height_spline_graph"), TEXT("/Script/VoxelEditor.VoxelHeightSplineGraphFactory"), nullptr },
		{ TEXT("volume_spline_graph"), TEXT("/Script/VoxelEditor.VoxelVolumeSplineGraphFactory"), nullptr },
		{ TEXT("height_sculpt_graph"), TEXT("/Script/VoxelEditor.VoxelHeightSculptGraphFactory"), nullptr },
		{ TEXT("volume_sculpt_graph"), TEXT("/Script/VoxelEditor.VoxelVolumeSculptGraphFactory"), nullptr },
		{ TEXT("smart_surface_type_graph"), TEXT("/Script/VoxelEditor.VoxelSmartSurfaceTypeGraphFactory"), nullptr },
		{ TEXT("pcg_graph"), TEXT("/Script/VoxelPCGEditor.VoxelPCGGraphFactory"), nullptr },
		{ TEXT("height_layer"), TEXT("/Script/VoxelEditor.VoxelHeightLayerFactory"), nullptr },
		{ TEXT("volume_layer"), TEXT("/Script/VoxelEditor.VoxelVolumeLayerFactory"), nullptr },
		{ TEXT("layer_stack"), TEXT("/Script/VoxelEditor.VoxelLayerStackFactory"), nullptr },
		{ TEXT("function_library"), TEXT("/Script/VoxelGraphEditor.VoxelFunctionLibraryAssetFactory"), nullptr },
		{ TEXT("surface_type"), nullptr, &UVoxelSurfaceTypeAsset::StaticClass },
		{ TEXT("smart_surface_type"), nullptr, &UVoxelSmartSurfaceType::StaticClass },
		{ TEXT("mega_material"), nullptr, &UVoxelMegaMaterial::StaticClass },
		{ TEXT("heightmap"), nullptr, &UVoxelHeightmap::StaticClass },
		{ TEXT("static_mesh"), nullptr, &UVoxelStaticMesh::StaticClass },
		{ TEXT("height_sculpt_asset"), nullptr, &UVoxelSculptHeightAsset::StaticClass },
		{ TEXT("volume_sculpt_asset"), nullptr, &UVoxelSculptVolumeAsset::StaticClass },
	};

	UFactory* MakeFactory(const FAssetType& Type, UClass*& OutClass, FString& OutError)
	{
		if (Type.FactoryPath)
		{
			UClass* FactoryClass = FindObject<UClass>(nullptr, Type.FactoryPath);
			if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass()))
			{
				OutError = FString::Printf(TEXT("Factory %s is not loaded; its Voxel editor module must be enabled"), Type.FactoryPath);
				return nullptr;
			}
			UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
			OutClass = Factory->GetSupportedClass();
			if (!OutClass)
			{
				OutError = FString::Printf(TEXT("Factory %s has no supported class"), Type.FactoryPath);
				return nullptr;
			}
			return Factory;
		}

		OutClass = Type.Class();
		IVoxelFactory* VoxelFactory = IVoxelAutoFactoryInterface::GetInterface().MakeFactory(OutClass);
		UFactory* Factory = VoxelFactory ? VoxelFactory->GetUFactory() : nullptr;
		if (!Factory)
		{
			OutError = FString::Printf(TEXT("Voxel registered no factory for %s"), *OutClass->GetName());
		}
		return Factory;
	}

	FResult AssetCreate(const FParams& Params)
	{
		const FString TypeKey = Str(Params, TEXT("type")).TrimStartAndEnd().ToLower();
		const FString Name = Str(Params, TEXT("name")).TrimStartAndEnd();
		FString PackagePath = Str(Params, TEXT("packagePath")).TrimStartAndEnd();
		if (PackagePath.IsEmpty())
		{
			PackagePath = TEXT("/Game/Voxel");
		}
		while (PackagePath.Len() > 1 && PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath.LeftChopInline(1);
		}
		FString OnConflict = Str(Params, TEXT("onConflict")).TrimStartAndEnd().ToLower();
		if (OnConflict.IsEmpty())
		{
			OnConflict = TEXT("skip");
		}

		const FAssetType* Type = nullptr;
		TArray<FString> Keys;
		for (const FAssetType& Candidate : GAssetTypes)
		{
			Keys.Add(Candidate.Key);
			if (TypeKey == Candidate.Key)
			{
				Type = &Candidate;
			}
		}
		if (!Type)
		{
			return Error(FString::Printf(TEXT("type '%s' is not one of: %s"), *TypeKey, *FString::Join(Keys, TEXT(", "))));
		}
		if (Name.IsEmpty())
		{
			return Error(TEXT("name is required"));
		}
		FText Reason;
		if (!FName::IsValidXName(Name, FString(INVALID_OBJECTNAME_CHARACTERS INVALID_LONGPACKAGE_CHARACTERS), &Reason))
		{
			return Error(FString::Printf(TEXT("Invalid name '%s': %s"), *Name, *Reason.ToString()));
		}
		if (OnConflict != TEXT("error") && OnConflict != TEXT("skip"))
		{
			return Error(FString::Printf(TEXT("onConflict '%s' must be error or skip (an existing asset is never overwritten: the engine refuses while anything references it, behind a blocking dialog)"), *OnConflict));
		}
		const FString PackageName = PackagePath / Name;
		if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
		{
			return Error(FString::Printf(TEXT("Invalid package '%s': %s"), *PackageName, *Reason.ToString()));
		}

		UClass* Class = nullptr;
		FString Err;
		UFactory* Factory = MakeFactory(*Type, Class, Err);
		if (!Factory)
		{
			return Error(Err);
		}

		const FString ObjectPath = PackageName + TEXT(".") + Name;
		UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
		const bool bPackageExists = FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName);
		if (!Existing && bPackageExists)
		{
			Existing = LoadObject<UObject>(nullptr, ObjectPath, {}, LOAD_NoWarn);
		}
		if (bPackageExists && !Existing)
		{
			return Error(FString::Printf(TEXT("Package %s exists but holds no asset named %s"), *PackageName, *Name));
		}

		if (Existing && OnConflict == TEXT("error"))
		{
			return Error(FString::Printf(TEXT("%s already exists (%s)"), *ObjectPath, *Existing->GetClass()->GetName()));
		}
		if (Existing && OnConflict == TEXT("skip"))
		{
			if (!Existing->IsA(Class))
			{
				return Error(FString::Printf(TEXT("%s exists as %s, not %s"), *ObjectPath, *Existing->GetClass()->GetName(), *Class->GetName()));
			}
			TSharedRef<FJsonObject> Out = AssetJson(*Existing);
			Out->SetBoolField(TEXT("created"), false);
			Out->SetBoolField(TEXT("existed"), true);
			Out->SetBoolField(TEXT("changed"), false);
			return Ok(Out);
		}

		IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
		UObject* Asset = AssetTools.CreateAsset(Name, PackagePath, Class, Factory);
		if (!Asset)
		{
			return Error(FString::Printf(TEXT("CreateAsset failed for %s (%s)"), *ObjectPath, *Class->GetName()));
		}

		TSharedRef<FJsonObject> Out = AssetJson(*Asset);
		Out->SetBoolField(TEXT("created"), true);
		Out->SetBoolField(TEXT("existed"), false);
		Out->SetBoolField(TEXT("changed"), true);
		SaveAsset(*Asset, Params, Out);
		return Ok(Out);
	}

	///////////////////////////////////////////////////////////////////////////
	// voxel_asset_set_property
	///////////////////////////////////////////////////////////////////////////

	FResult AssetSetProperty(const FParams& Params)
	{
		FString Err;
		UObject* Asset = LoadTyped(Str(Params, TEXT("assetPath")), UObject::StaticClass(), Err);
		if (!Asset) return Error(Err);

		const FString PropertyName = Str(Params, TEXT("propertyName")).TrimStartAndEnd();
		if (PropertyName.IsEmpty()) return Error(TEXT("propertyName is required"));
		if (!Has(Params, TEXT("value"))) return Error(TEXT("value is required"));

		FProperty* Property = FindFProperty<FProperty>(Asset->GetClass(), *PropertyName);
		if (!Property)
		{
			for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					Property = *It;
					break;
				}
			}
		}
		if (!Property)
		{
			return Error(FString::Printf(TEXT("%s has no property '%s'"), *Asset->GetClass()->GetName(), *PropertyName));
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient))
		{
			return Error(FString::Printf(TEXT("%s is transient; a value set on it is not saved"), *Property->GetName()));
		}

		TArray<FPendingEdit> Edits;
		if (!PrepareText(*Asset, Property, JsonToText(*Property, Params->TryGetField(TEXT("value"))), Edits.AddDefaulted_GetRef(), Err))
		{
			return Error(Err);
		}

		const FScopedTransaction Transaction(LOCTEXT("SetVoxelAssetProperty", "Set Voxel Asset Property"));
		TSharedRef<FJsonObject> Out = AssetJson(*Asset);
		ApplyEdits(*Asset, Edits, Out);
		const TSharedPtr<FJsonObject> Entry = Out->GetObjectField(TEXT("properties"))->GetObjectField(Property->GetName());
		Out->SetStringField(TEXT("propertyName"), Property->GetName());
		Out->SetStringField(TEXT("previous"), Entry->GetStringField(TEXT("previous")));
		Out->SetStringField(TEXT("value"), Entry->GetStringField(TEXT("value")));
		Out->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst));
		SaveAsset(*Asset, Params, Out);
		return Ok(Out);
	}

	///////////////////////////////////////////////////////////////////////////
	// voxel_mega_material_set_surfaces
	///////////////////////////////////////////////////////////////////////////

	FResult MegaMaterialSetSurfaces(const FParams& Params)
	{
		FString Err;
		UVoxelMegaMaterial* Material = Load<UVoxelMegaMaterial>(Str(Params, TEXT("assetPath")), Err);
		if (!Material) return Error(Err);

		FString Mode = Str(Params, TEXT("mode")).TrimStartAndEnd().ToLower();
		if (Mode.IsEmpty()) Mode = TEXT("replace");
		if (Mode != TEXT("replace") && Mode != TEXT("append")) return Error(FString::Printf(TEXT("mode '%s' must be replace or append"), *Mode));

		TArray<FPendingEdit> Edits;
		if (Has(Params, TEXT("surfaceTypes")))
		{
			TArray<FString> Paths;
			if (!StrArray(Params, TEXT("surfaceTypes"), Paths, Err)) return Error(Err);
			if (!PrepareObjectArray(*Material, GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, SurfaceTypes), Paths, Mode == TEXT("append"), Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(FString::Printf(TEXT("surfaceTypes: %s"), *Err));
			}
		}

		const FFieldMap Fields[] =
		{
			{ TEXT("automaticallyDetectNewSurfaces"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bAutomaticallyDetectNewSurfaces) },
			{ TEXT("attributePostProcess"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, AttributePostProcess) },
			{ TEXT("nonNaniteMaterialType"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, NonNaniteMaterialType) },
			{ TEXT("customNonNaniteMaterial"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomNonNaniteMaterial) },
			{ TEXT("naniteDisplacementMaterialType"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, NaniteDisplacementMaterialType) },
			{ TEXT("customNaniteDisplacementMaterial"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomNaniteDisplacementMaterial) },
			{ TEXT("lumenMaterialType"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, LumenMaterialType) },
			{ TEXT("customLumenMaterial"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomLumenMaterial) },
			{ TEXT("enableSmoothBlends"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bEnableSmoothBlends) },
			{ TEXT("generateMaskedMaterial"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bGenerateMaskedMaterial) },
			{ TEXT("generateTwoSidedMaterial"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bGenerateTwoSidedMaterial) },
			{ TEXT("setHasPixelAnimation"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bSetHasPixelAnimation) },
			{ TEXT("enablePixelDepthOffset"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bEnablePixelDepthOffset) },
			{ TEXT("enableDitherNoiseTexture"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bEnableDitherNoiseTexture) },
			{ TEXT("ditherNoiseTexture"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, DitherNoiseTexture) },
			{ TEXT("customOutputsMaterial"), GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomOutputsMaterial) },
		};
		if (!PrepareFields(*Material, Params, Fields, Edits, Err)) return Error(Err);
		if (Edits.Num() == 0) return Error(TEXT("Nothing to set: pass surfaceTypes or a mega material setting"));

		const FScopedTransaction Transaction(LOCTEXT("SetMegaMaterialSurfaces", "Set Voxel Mega Material Surfaces"));
		TSharedRef<FJsonObject> Out = AssetJson(*Material);
		ApplyEdits(*Material, Edits, Out);
		Out->SetArrayField(TEXT("surfaceTypes"), PathsJson(ToObjects(Material->SurfaceTypes)));
		SaveAsset(*Material, Params, Out);
		return Ok(Out);
	}

	///////////////////////////////////////////////////////////////////////////
	// voxel_surface_type_set
	///////////////////////////////////////////////////////////////////////////

	FResult SurfaceTypeSet(const FParams& Params)
	{
		FString Err;
		UVoxelSurfaceTypeAsset* Surface = Load<UVoxelSurfaceTypeAsset>(Str(Params, TEXT("assetPath")), Err);
		if (!Surface) return Error(Err);

		TArray<FPendingEdit> Edits;
		const FFieldMap Fields[] =
		{
			{ TEXT("material"), GET_MEMBER_NAME_CHECKED(UVoxelSurfaceTypeAsset, Material) },
			{ TEXT("invisible"), GET_MEMBER_NAME_CHECKED(UVoxelSurfaceTypeAsset, bInvisible) },
			{ TEXT("blendSmoothness"), GET_MEMBER_NAME_CHECKED(UVoxelSurfaceTypeAsset, BlendSmoothness) },
		};
		if (!PrepareFields(*Surface, Params, Fields, Edits, Err)) return Error(Err);

		if (Has(Params, TEXT("seed")))
		{
			const FString Seed = ValueText(Params->TryGetField(TEXT("seed")));
			FProperty* SeedProperty = Surface->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UVoxelSurfaceTypeAsset, Seed));
			if (!PrepareCustom(*Surface, SeedProperty, [&](void* Value, FString&)
			{
				static_cast<FVoxelExposedSeed*>(Value)->Seed = Seed;
				return true;
			}, Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(FString::Printf(TEXT("seed: %s"), *Err));
			}
		}
		if (Edits.Num() == 0) return Error(TEXT("Nothing to set: pass material, invisible, blendSmoothness or seed"));

		const FScopedTransaction Transaction(LOCTEXT("SetSurfaceType", "Set Voxel Surface Type"));
		TSharedRef<FJsonObject> Out = AssetJson(*Surface);
		ApplyEdits(*Surface, Edits, Out);
		SaveAsset(*Surface, Params, Out);
		return Ok(Out);
	}

	///////////////////////////////////////////////////////////////////////////
	// voxel_smart_surface_set
	///////////////////////////////////////////////////////////////////////////

	bool FindGraphParameter(const UVoxelGraph& Graph, const FString& Name, FVoxelParameter& Out)
	{
		bool bFound = false;
		Graph.ForeachParameter([&](const FGuid&, const FVoxelParameter& Parameter)
		{
			if (!bFound && Parameter.Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				Out = Parameter;
				bFound = true;
			}
		});
		return bFound;
	}

	FResult SmartSurfaceSet(const FParams& Params)
	{
		FString Err;
		UVoxelSmartSurfaceType* Surface = Load<UVoxelSmartSurfaceType>(Str(Params, TEXT("assetPath")), Err);
		if (!Surface) return Error(Err);

		TArray<FPendingEdit> Edits;
		UVoxelSmartSurfaceTypeGraph* TargetGraph = Surface->Graph;
		if (Has(Params, TEXT("graph")))
		{
			const FString GraphPath = Str(Params, TEXT("graph")).TrimStartAndEnd();
			TargetGraph = nullptr;
			if (!GraphPath.IsEmpty() && !GraphPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				TargetGraph = Load<UVoxelSmartSurfaceTypeGraph>(GraphPath, Err);
				if (!TargetGraph) return Error(Err);
			}
			FProperty* GraphProperty = Surface->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UVoxelSmartSurfaceType, Graph));
			if (!PrepareCustom(*Surface, GraphProperty, [&](void* Value, FString&)
			{
				*static_cast<TObjectPtr<UVoxelSmartSurfaceTypeGraph>*>(Value) = TargetGraph;
				return true;
			}, Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(FString::Printf(TEXT("graph: %s"), *Err));
			}
		}

		TArray<TPair<FName, FVoxelPinValue>> Values;
		if (Has(Params, TEXT("parameters")))
		{
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!Params->TryGetObjectField(TEXT("parameters"), Parameters)) return Error(TEXT("parameters must be an object { name: value }"));
			if (!TargetGraph) return Error(TEXT("parameters need a graph; the smart surface type has none"));
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Parameters)->Values)
			{
				FVoxelParameter Parameter;
				if (!FindGraphParameter(*TargetGraph, Pair.Key, Parameter))
				{
					return Error(FString::Printf(TEXT("Graph %s has no parameter '%s'"), *TargetGraph->GetName(), *Pair.Key));
				}
				const FString Text = ValueText(Pair.Value);
				FVoxelPinValue Value(Parameter.Type.GetExposedType());
				if (!ParseValue(Value, Text))
				{
					return Error(FString::Printf(TEXT("'%s' does not parse as %s for parameter '%s'"), *Text, *Parameter.Type.ToString(), *Pair.Key));
				}
				Values.Emplace(Parameter.Name, Value);
			}
		}
		if (Edits.Num() == 0 && Values.Num() == 0) return Error(TEXT("Nothing to set: pass graph or parameters"));

		const FScopedTransaction Transaction(LOCTEXT("SetSmartSurface", "Set Voxel Smart Surface Type"));
		TSharedRef<FJsonObject> Out = AssetJson(*Surface);
		ApplyEdits(*Surface, Edits, Out);

		if (Values.Num() > 0)
		{
			FProperty* OverridesProperty = Surface->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UVoxelSmartSurfaceType, ParameterOverrides));
			TArray<FString> Failures;
			Surface->PreEditChange(OverridesProperty);
			for (const TPair<FName, FVoxelPinValue>& Pair : Values)
			{
				FString SetError;
				if (!Surface->SetParameter(Pair.Key, Pair.Value, &SetError))
				{
					Failures.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key.ToString(), *SetError));
				}
			}
			FPropertyChangedEvent Event(OverridesProperty, EPropertyChangeType::ValueSet);
			Surface->PostEditChangeProperty(Event);
			Out->SetBoolField(TEXT("changed"), true);
			if (Failures.Num() > 0)
			{
				Out->SetStringField(TEXT("warning"), FString::Printf(TEXT("Not applied: %s"), *FString::Join(Failures, TEXT("; "))));
			}
		}

		TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
		for (const TPair<FGuid, FVoxelParameterValueOverride>& Pair : Surface->GetParameterOverrides().GuidToValueOverride)
		{
			if (Pair.Value.bEnable)
			{
				Overrides->SetStringField(Pair.Value.CachedName.ToString(), Pair.Value.Value.ExportToString());
			}
		}
		Out->SetStringField(TEXT("graph"), Surface->Graph ? Surface->Graph->GetPathName() : TEXT("None"));
		Out->SetObjectField(TEXT("overrides"), Overrides);
		SaveAsset(*Surface, Params, Out);
		return Ok(Out);
	}

	///////////////////////////////////////////////////////////////////////////
	// voxel_layer_stack_set
	///////////////////////////////////////////////////////////////////////////

	FResult LayerStackSet(const FParams& Params)
	{
		FString Err;
		UVoxelLayerStack* Stack = Load<UVoxelLayerStack>(Str(Params, TEXT("assetPath")), Err);
		if (!Stack) return Error(Err);

		TArray<FPendingEdit> Edits;
		const TPair<const TCHAR*, FName> Arrays[] =
		{
			{ TEXT("heightLayers"), GET_MEMBER_NAME_CHECKED(UVoxelLayerStack, HeightLayers) },
			{ TEXT("volumeLayers"), GET_MEMBER_NAME_CHECKED(UVoxelLayerStack, VolumeLayers) },
		};
		for (const TPair<const TCHAR*, FName>& Array : Arrays)
		{
			if (!Has(Params, Array.Key)) continue;
			TArray<FString> Paths;
			if (!StrArray(Params, Array.Key, Paths, Err)) return Error(Err);
			if (!PrepareObjectArray(*Stack, Array.Value, Paths, false, Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(FString::Printf(TEXT("%s: %s"), Array.Key, *Err));
			}
		}
		const FFieldMap Fields[] =
		{
			{ TEXT("maxDistance"), GET_MEMBER_NAME_CHECKED(UVoxelLayerStack, MaxDistance) },
		};
		if (!PrepareFields(*Stack, Params, Fields, Edits, Err)) return Error(Err);
		if (Edits.Num() == 0) return Error(TEXT("Nothing to set: pass heightLayers, volumeLayers or maxDistance"));

		const FScopedTransaction Transaction(LOCTEXT("SetLayerStack", "Set Voxel Layer Stack"));
		TSharedRef<FJsonObject> Out = AssetJson(*Stack);
		ApplyEdits(*Stack, Edits, Out);
		Out->SetArrayField(TEXT("heightLayers"), PathsJson(ToObjects(Stack->HeightLayers)));
		Out->SetArrayField(TEXT("volumeLayers"), PathsJson(ToObjects(Stack->VolumeLayers)));
		Out->SetNumberField(TEXT("maxDistance"), Stack->MaxDistance);
		SaveAsset(*Stack, Params, Out);
		return Ok(Out);
	}

	///////////////////////////////////////////////////////////////////////////
	// PCG
	///////////////////////////////////////////////////////////////////////////

	struct FPCGNodeType
	{
		const TCHAR* Key;
		UClass* (*Class)();
	};

	const FPCGNodeType GPCGNodeTypes[] =
	{
		{ TEXT("call_graph"), &UPCGCallVoxelGraphSettings::StaticClass },
		{ TEXT("sampler"), &UPCGVoxelSamplerSettings::StaticClass },
		{ TEXT("sampler_v2"), &UPCGVoxelSamplerV2Settings::StaticClass },
		{ TEXT("query"), &UPCGVoxelQuerySettings::StaticClass },
		{ TEXT("stamp_spawner"), &UPCGVoxelStampSpawnerSettings::StaticClass },
		{ TEXT("spawn_actor"), &UPCGSpawnActorWithVoxelGraphSettings::StaticClass },
		{ TEXT("apply_on_graph"), &UPCGApplyOnVoxelGraphSettings::StaticClass },
		{ TEXT("create_spline"), &UPCGCreateVoxelSplineSettings::StaticClass },
		{ TEXT("projection"), &UPCGVoxelProjectionSettings::StaticClass },
		{ TEXT("elevation_isolines"), &UPCGVoxelElevationIsolines::StaticClass },
		{ TEXT("wait_for_world"), &UPCGWaitForVoxelWorldSettings::StaticClass },
	};

	TSharedRef<FJsonObject> PCGNodeJson(const UPCGNode& Node)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Node.GetName());
		Out->SetStringField(TEXT("title"), Node.GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
		const UPCGSettings* Settings = Node.GetSettings();
		Out->SetStringField(TEXT("settingsClass"), Settings ? Settings->GetClass()->GetName() : TEXT("None"));
		int32 X = 0;
		int32 Y = 0;
		Node.GetNodePosition(X, Y);
		Out->SetNumberField(TEXT("x"), X);
		Out->SetNumberField(TEXT("y"), Y);
		return Out;
	}

	void SavePCGGraph(UPCGGraph& Graph, const FParams& Params, const TSharedRef<FJsonObject>& Out)
	{
		Graph.MarkPackageDirty();
		bool bSaved = false;
		if (Bool(Params, TEXT("save"), true))
		{
			bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Graph.GetPackage() }, false);
		}
		Out->SetBoolField(TEXT("saved"), bSaved);
	}

	FResult PCGAddNode(const FParams& Params)
	{
		FString Err;
		UPCGGraph* Graph = Load<UPCGGraph>(Str(Params, TEXT("graphPath")), Err);
		if (!Graph) return Error(Err);

		const FString Key = Str(Params, TEXT("nodeType")).TrimStartAndEnd().ToLower();
		UClass* SettingsClass = nullptr;
		TArray<FString> Keys;
		for (const FPCGNodeType& Type : GPCGNodeTypes)
		{
			Keys.Add(Type.Key);
			if (Key == Type.Key)
			{
				SettingsClass = Type.Class();
			}
		}
		if (!SettingsClass)
		{
			return Error(FString::Printf(TEXT("nodeType '%s' is not one of: %s"), *Key, *FString::Join(Keys, TEXT(", "))));
		}

		const FScopedTransaction Transaction(LOCTEXT("AddVoxelPCGNode", "Add Voxel PCG Node"));
		Graph->Modify();
		UPCGSettings* Settings = nullptr;
		UPCGNode* Node = Graph->AddNodeOfType(SettingsClass, Settings);
		if (!Node) return Error(FString::Printf(TEXT("UPCGGraph::AddNodeOfType refused %s"), *SettingsClass->GetName()));

		if (Has(Params, TEXT("x")) || Has(Params, TEXT("y")))
		{
			Node->Modify();
			Node->SetNodePosition(FMath::RoundToInt32(Num(Params, TEXT("x"), 0)), FMath::RoundToInt32(Num(Params, TEXT("y"), 0)));
		}

		TSharedRef<FJsonObject> Out = PCGNodeJson(*Node);
		Out->SetStringField(TEXT("graphPath"), Graph->GetPathName());
		Out->SetBoolField(TEXT("changed"), true);
		SavePCGGraph(*Graph, Params, Out);
		return Ok(Out);
	}

	// Node by object name, authored title or display title (case-insensitive); ambiguity is an error.
	UPCGNode* FindPCGNode(const UPCGGraph& Graph, const FString& Ref, FString& OutError)
	{
		if (Ref.IsEmpty())
		{
			OutError = TEXT("node is required");
			return nullptr;
		}
		TArray<UPCGNode*> Matches;
		for (UPCGNode* Node : Graph.GetNodes())
		{
			if (!Node) continue;
			if (Node->GetName() == Ref)
			{
				return Node;
			}
			if (Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString().Equals(Ref, ESearchCase::IgnoreCase) ||
				Node->GetNodeTitle(EPCGNodeTitleType::FullTitle).ToString().Equals(Ref, ESearchCase::IgnoreCase) ||
				(Node->HasAuthoredTitle() && Node->GetAuthoredTitleName().ToString().Equals(Ref, ESearchCase::IgnoreCase)))
			{
				Matches.Add(Node);
			}
		}
		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		OutError = Matches.Num() == 0
			? FString::Printf(TEXT("No node '%s' in %s"), *Ref, *Graph.GetName())
			: FString::Printf(TEXT("%d nodes match '%s'; use the node name"), Matches.Num(), *Ref);
		return nullptr;
	}

	FResult PCGConfigureSampler(const FParams& Params)
	{
		FString Err;
		UPCGGraph* Graph = Load<UPCGGraph>(Str(Params, TEXT("graphPath")), Err);
		if (!Graph) return Error(Err);
		UPCGNode* Node = FindPCGNode(*Graph, Str(Params, TEXT("node")), Err);
		if (!Node) return Error(Err);

		UPCGSettings* Settings = Node->GetSettings();
		UPCGVoxelSamplerSettings* V1 = Cast<UPCGVoxelSamplerSettings>(Settings);
		UPCGVoxelSamplerV2Settings* V2 = Cast<UPCGVoxelSamplerV2Settings>(Settings);
		if (!V1 && !V2)
		{
			return Error(FString::Printf(TEXT("Node %s holds %s, not a Voxel Sampler"), *Node->GetName(), Settings ? *Settings->GetClass()->GetName() : TEXT("no settings")));
		}

		struct FSamplerField
		{
			const TCHAR* Param;
			FName V1;
			FName V2;
		};
		const FSamplerField Fields[] =
		{
			{ TEXT("unbounded"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, bUnbounded), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, bUnbounded) },
			{ TEXT("looseness"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, Looseness), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, Looseness) },
			{ TEXT("lod"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, LOD), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, LOD) },
			{ TEXT("resolveSmartSurfaceTypes"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, bResolveSmartSurfaceTypes), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, bResolveSmartSurfaceTypes) },
			{ TEXT("distanceBetweenPoints"), NAME_None, GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, DistanceBetweenPoints) },
			{ TEXT("heightScatterType"), NAME_None, GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, HeightScatterType) },
			{ TEXT("minCellArea"), NAME_None, GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, MinCellArea) },
			{ TEXT("pointsPerSquaredMeter"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, PointsPerSquaredMeter), NAME_None },
			{ TEXT("cellSize"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, CellSize), NAME_None },
			{ TEXT("tolerance"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, Tolerance), NAME_None },
			{ TEXT("applyDensityToPoints"), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, bApplyDensityToPoints), NAME_None },
		};

		TArray<FPendingEdit> Edits;
		for (const FSamplerField& Field : Fields)
		{
			if (!Has(Params, Field.Param)) continue;
			const FName Property = V2 ? Field.V2 : Field.V1;
			if (Property.IsNone())
			{
				return Error(FString::Printf(TEXT("%s does not apply to %s"), Field.Param, *Settings->GetClass()->GetName()));
			}
			if (!PrepareJson(*Settings, Property, Params->TryGetField(Field.Param), Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(FString::Printf(TEXT("%s: %s"), Field.Param, *Err));
			}
		}

		if (Has(Params, TEXT("metadatasToQuery")))
		{
			TArray<FString> Paths;
			if (!StrArray(Params, TEXT("metadatasToQuery"), Paths, Err)) return Error(Err);
			const FName Property = V2
				? GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, MetadatasToQuery)
				: GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, NewMetadatasToQuery);
			if (!PrepareObjectArray(*Settings, Property, Paths, false, Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(FString::Printf(TEXT("metadatasToQuery: %s"), *Err));
			}
		}

		// Stack and layer each keep their current value unless given.
		if (Has(Params, TEXT("stack")) || Has(Params, TEXT("layer")))
		{
			UVoxelLayerStack* NewStack = nullptr;
			UVoxelLayer* NewLayer = nullptr;
			if (Has(Params, TEXT("stack")))
			{
				NewStack = Load<UVoxelLayerStack>(Str(Params, TEXT("stack")), Err);
				if (!NewStack) return Error(FString::Printf(TEXT("stack: %s"), *Err));
			}
			if (Has(Params, TEXT("layer")))
			{
				NewLayer = Load<UVoxelLayer>(Str(Params, TEXT("layer")), Err);
				if (!NewLayer) return Error(FString::Printf(TEXT("layer: %s"), *Err));
			}
			FStructProperty* LayerProperty = CastField<FStructProperty>(Settings->GetClass()->FindPropertyByName(V2
				? GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, Layer)
				: GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, Layer)));
			if (!LayerProperty || LayerProperty->Struct != FVoxelStackLayer::StaticStruct())
			{
				return Error(TEXT("The sampler's Layer property is not an FVoxelStackLayer; the Voxel version is unsupported"));
			}
			if (!PrepareCustom(*Settings, LayerProperty, [&](void* Value, FString&)
			{
				FVoxelStackLayer& StackLayer = *static_cast<FVoxelStackLayer*>(Value);
				if (NewStack) StackLayer.Stack = NewStack;
				if (NewLayer) StackLayer.Layer = NewLayer;
				return true;
			}, Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(Err);
			}
		}
		if (Edits.Num() == 0) return Error(TEXT("Nothing to set: pass at least one sampler setting"));

		// UPCGSettings::PostEditChangeProperty broadcasts OnSettingsChangedDelegate, which the node relays to its graph.
		const FScopedTransaction Transaction(LOCTEXT("ConfigureVoxelSampler", "Configure Voxel PCG Sampler"));
		TSharedRef<FJsonObject> Out = PCGNodeJson(*Node);
		Out->SetStringField(TEXT("graphPath"), Graph->GetPathName());
		Out->SetBoolField(TEXT("instance"), Node->IsInstance());
		ApplyEdits(*Settings, Edits, Out);
		SavePCGGraph(*Graph, Params, Out);
		return Ok(Out);
	}
}

void AddAssetHandlers(TArray<FHandlerEntry>& Out)
{
	Out.Append(
	{
		{ TEXT("voxel_asset_create"), &AssetCreate },
		{ TEXT("voxel_asset_set_property"), &AssetSetProperty },
		{ TEXT("voxel_mega_material_set_surfaces"), &MegaMaterialSetSurfaces },
		{ TEXT("voxel_surface_type_set"), &SurfaceTypeSet },
		{ TEXT("voxel_smart_surface_set"), &SmartSurfaceSet },
		{ TEXT("voxel_layer_stack_set"), &LayerStackSet },
		{ TEXT("voxel_pcg_add_node"), &PCGAddNode },
		{ TEXT("voxel_pcg_configure_sampler"), &PCGConfigureSampler },
	});
}
}

#undef LOCTEXT_NAMESPACE
