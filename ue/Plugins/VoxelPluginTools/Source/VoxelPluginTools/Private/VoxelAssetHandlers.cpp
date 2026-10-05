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

	// Asset references in import text are loaded up front so a bad path is an error, not a silent None. Soft
	// references too: their import text stores any path, loadable or not.
	bool PreloadObjectRefs(const FProperty& Property, const FString& Text, FString& OutError)
	{
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property);
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
		{
			ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
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

	// The enum value an import text names, by name; ImportText would also take a bare index.
	bool CheckEnumText(const FProperty& Property, const FString& Text, FString& OutError)
	{
		const UEnum* Enum = nullptr;
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property)) Enum = EnumProperty->GetEnum();
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property)) Enum = ByteProperty->Enum;
		if (!Enum || Enum->GetIndexByNameString(Text.TrimStartAndEnd()) != INDEX_NONE)
		{
			return true;
		}
		TArray<FString> Names;
		for (int32 Index = 0; Index < Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0); Index++)
		{
			Names.Add(Enum->GetNameStringByIndex(Index));
		}
		OutError = FString::Printf(TEXT("'%s' is not a %s value; use one of: %s"), *Text, *Enum->GetName(), *FString::Join(Names, TEXT(", ")));
		return false;
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
		if (!PreloadObjectRefs(*Property, Text, OutError) || !CheckEnumText(*Property, Text, OutError))
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

	// JSON string, number, boolean, null (the empty text) or array of those to UE import text.
	bool JsonToText(const FProperty& Property, const TSharedPtr<FJsonValue>& Value, FString& Out, FString& OutError)
	{
		if (Value.IsValid() && Value->Type == EJson::Array)
		{
			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				FString Text;
				if (!Item.IsValid() || Item->Type == EJson::Null || !ScalarText(Item, Text))
				{
					OutError = FString::Printf(TEXT("%s array items must be strings, numbers or booleans"), *Property.GetName());
					return false;
				}
				Items.Add(Item->Type == EJson::String ? TEXT("\"") + Text.ReplaceCharWithEscapedChar() + TEXT("\"") : Text);
			}
			Out = TEXT("(") + FString::Join(Items, TEXT(",")) + TEXT(")");
			return true;
		}
		if (Value.IsValid() && Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
			if (Numeric && Numeric->IsInteger())
			{
				// Rounding 2.6 to 3 would store a value nobody passed.
				if (Number != FMath::RoundToDouble(Number) || FMath::Abs(Number) > 9007199254740992.0)
				{
					OutError = FString::Printf(TEXT("%s is an integer property; %s is not an integer"), *Property.GetName(), *FString::SanitizeFloat(Number, 0));
					return false;
				}
				Out = FString::Printf(TEXT("%lld"), static_cast<long long>(Number));
				return true;
			}
		}
		if (!ScalarText(Value, Out))
		{
			OutError = FString::Printf(TEXT("%s takes a string, number, boolean or array, not an object"), *Property.GetName());
			return false;
		}
		return true;
	}

	bool PrepareJson(UObject& Object, const FName PropertyName, const TSharedPtr<FJsonValue>& Value, FPendingEdit& Out, FString& OutError)
	{
		FProperty* Property = Object.GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("%s has no property %s"), *Object.GetClass()->GetName(), *PropertyName.ToString());
			return false;
		}
		FString Text;
		return JsonToText(*Property, Value, Text, OutError) && PrepareText(Object, Property, Text, Out, OutError);
	}

	// Object array from distinct asset paths, in the given order, after the current entries when appending (an entry
	// already present is kept where it is).
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
			// A repeated entry would be dropped; the list the caller sent is not the list that lands.
			if (Objects.Contains(Loaded))
			{
				OutError = FString::Printf(TEXT("%s is listed twice"), *Loaded->GetPathName());
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

	// The property a JSON field edits, and the field's contract; the field's name is Spec.Name.
	struct FFieldMap
	{
		FName Property;
		FMCPParamSpec Spec;
	};

	bool PrepareFields(UObject& Object, const FParams& Params, TConstArrayView<FFieldMap> Fields, TArray<FPendingEdit>& Edits, FString& OutError)
	{
		for (const FFieldMap& Field : Fields)
		{
			const TCHAR* Param = *Field.Spec.Name;
			if (!Has(Params, Param))
			{
				continue;
			}
			if (!PrepareJson(Object, Field.Property, Params->TryGetField(Param), Edits.AddDefaulted_GetRef(), OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), Param, *OutError);
				return false;
			}
		}
		return true;
	}

	TArray<FMCPParamSpec> SpecsOf(TConstArrayView<FFieldMap> Fields)
	{
		TArray<FMCPParamSpec> Out;
		for (const FFieldMap& Field : Fields)
		{
			Out.Add(Field.Spec);
		}
		return Out;
	}

	FMCPParamSpec AssetRef(const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::Optional(Name, EMCPParamType::String, Description).Nullable();
	}

	FMCPParamSpec Flag(const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::Optional(Name, EMCPParamType::Boolean, Description);
	}

	FMCPParamSpec GenerationType(const TCHAR* Name, const TCHAR* Description)
	{
		return MCPParam::Optional(Name, EMCPParamType::String, Description).Enum({ TEXT("Custom"), TEXT("Generated") });
	}

	const TArray<FFieldMap>& MegaMaterialFields()
	{
		static const TArray<FFieldMap> Fields =
		{
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bAutomaticallyDetectNewSurfaces), Flag(TEXT("automaticallyDetectNewSurfaces"), TEXT("Notify when a rendered surface is missing from the list; default true.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, AttributePostProcess), AssetRef(TEXT("attributePostProcess"),
				TEXT("UMaterialFunction applied everywhere, one MaterialAttributes in and out; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, NonNaniteMaterialType), GenerationType(TEXT("nonNaniteMaterialType"), TEXT("Non-Nanite meshes use customNonNaniteMaterial (Custom) or a generated one; default Custom.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomNonNaniteMaterial), AssetRef(TEXT("customNonNaniteMaterial"), TEXT("UMaterialInterface for non-Nanite meshes when Custom; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, NaniteDisplacementMaterialType), GenerationType(TEXT("naniteDisplacementMaterialType"),
				TEXT("Nanite displacement comes from customNaniteDisplacementMaterial (Custom) or is generated; default Generated.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomNaniteDisplacementMaterial), AssetRef(TEXT("customNaniteDisplacementMaterial"),
				TEXT("UMaterialInterface whose displacement replaces the generated one when Custom; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, LumenMaterialType), GenerationType(TEXT("lumenMaterialType"), TEXT("Lumen uses customLumenMaterial (Custom) or a generated material; default Generated.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomLumenMaterial), AssetRef(TEXT("customLumenMaterial"), TEXT("UMaterialInterface Lumen uses when Custom; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bEnableSmoothBlends), Flag(TEXT("enableSmoothBlends"), TEXT("Dither-based smooth blends; default true.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bGenerateMaskedMaterial), Flag(TEXT("generateMaskedMaterial"), TEXT("Generated materials use the Masked blend mode; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bGenerateTwoSidedMaterial), Flag(TEXT("generateTwoSidedMaterial"), TEXT("Generated materials are two-sided; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bSetHasPixelAnimation), Flag(TEXT("setHasPixelAnimation"), TEXT("Set bHasPixelAnimation to reduce TSR blur; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bEnablePixelDepthOffset), Flag(TEXT("enablePixelDepthOffset"), TEXT("Compile PixelDepthOffset into the non-Nanite material; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, bEnableDitherNoiseTexture), Flag(TEXT("enableDitherNoiseTexture"), TEXT("Use ditherNoiseTexture for smooth-blend dithering; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, DitherNoiseTexture), AssetRef(TEXT("ditherNoiseTexture"),
				TEXT("UTexture2D asset path for dither noise, which must load; \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelMegaMaterial, CustomOutputsMaterial), AssetRef(TEXT("customOutputsMaterial"),
				TEXT("UMaterialInterface whose custom output nodes are copied into the generated material; \"\" or null clears it.")) },
		};
		return Fields;
	}

	const TArray<FFieldMap>& SurfaceTypeFields()
	{
		static const TArray<FFieldMap> Fields =
		{
			{ GET_MEMBER_NAME_CHECKED(UVoxelSurfaceTypeAsset, Material), AssetRef(TEXT("material"), TEXT("UMaterialInterface asset path (material or instance); \"\" or null clears it.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelSurfaceTypeAsset, bInvisible), Flag(TEXT("invisible"), TEXT("Render nothing, to cut holes in the terrain; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(UVoxelSurfaceTypeAsset, BlendSmoothness), MCPParam::Optional(TEXT("blendSmoothness"), EMCPParamType::Number,
				TEXT("Smooth blend amount, >= 0 (ClampMin); the details panel offers 0 to 1. Default 0.5.")).Min(0) },
		};
		return Fields;
	}

	const TArray<FFieldMap>& LayerStackFields()
	{
		static const TArray<FFieldMap> Fields =
		{
			{ GET_MEMBER_NAME_CHECKED(UVoxelLayerStack, MaxDistance), MCPParam::Optional(TEXT("maxDistance"), EMCPParamType::Number,
				TEXT("How far up and down the height distance field extends, in centimetres; default 100000.")) },
		};
		return Fields;
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
		// Assets only: a level actor or map would be saved with its whole level, and a class default is not an asset.
		if (!Asset->IsAsset() || Asset->IsA<UWorld>() || Asset->GetTypedOuter<UWorld>() || Asset->HasAnyFlags(RF_ClassDefaultObject))
		{
			return Error(FString::Printf(TEXT("%s is not an asset (levels, level actors and class defaults are out of scope)"), *Asset->GetPathName()));
		}

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
		FString Text;
		if (!JsonToText(*Property, Params->TryGetField(TEXT("value")), Text, Err) || !PrepareText(*Asset, Property, Text, Edits.AddDefaulted_GetRef(), Err))
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
		if (Has(Params, TEXT("mode")) && !Has(Params, TEXT("surfaceTypes"))) return Error(TEXT("mode only applies with surfaceTypes"));

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

		if (!PrepareFields(*Material, Params, MegaMaterialFields(), Edits, Err)) return Error(Err);
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
		if (!PrepareFields(*Surface, Params, SurfaceTypeFields(), Edits, Err)) return Error(Err);

		if (Has(Params, TEXT("seed")))
		{
			FString Seed;
			if (!ScalarField(Params, TEXT("seed"), Seed)) return Error(TEXT("seed must be a string or a number"));
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
				FString Text;
				if (!ScalarText(Pair.Value, Text)) return Error(FString::Printf(TEXT("parameters.%s must be a string, number, boolean or null"), *Pair.Key));
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
			const bool bAnyApplied = Failures.Num() < Values.Num();
			if (!bAnyApplied && Edits.Num() == 0)
			{
				return Error(FString::Printf(TEXT("No parameter was applied: %s"), *FString::Join(Failures, TEXT("; "))));
			}
			if (bAnyApplied)
			{
				Out->SetBoolField(TEXT("changed"), true);
			}
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
		if (!PrepareFields(*Stack, Params, LayerStackFields(), Edits, Err)) return Error(Err);
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
			// An axis not given keeps the position AddNodeOfType chose.
			int32 X = 0;
			int32 Y = 0;
			Node->GetNodePosition(X, Y);
			Node->Modify();
			Node->SetNodePosition(static_cast<int32>(Num(Params, TEXT("x"), X)), static_cast<int32>(Num(Params, TEXT("y"), Y)));
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

	// A sampler setting: its property on each sampler class (None where that class lacks it) and its contract.
	struct FSamplerField
	{
		FName V1;
		FName V2;
		FMCPParamSpec Spec;
	};

	const TArray<FSamplerField>& SamplerFields()
	{
		static const TArray<FSamplerField> Fields =
		{
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, bUnbounded), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, bUnbounded),
				Flag(TEXT("unbounded"), TEXT("Sample the whole surface instead of the actor or bounding-shape bounds; default false.")) },
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, Looseness), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, Looseness),
				MCPParam::Optional(TEXT("looseness"), EMCPParamType::Number, TEXT("Point jitter, >= 0 (ClampMin); default 1.")).Min(0) },
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, LOD), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, LOD),
				MCPParam::Optional(TEXT("lod"), EMCPParamType::Integer, TEXT("LOD to sample, an integer >= 0 (ClampMin); default 0.")).Range(0, MAX_int32) },
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, bResolveSmartSurfaceTypes), GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, bResolveSmartSurfaceTypes),
				Flag(TEXT("resolveSmartSurfaceTypes"), TEXT("Resolve smart surface types to surface types; default true.")) },
			{ NAME_None, GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, DistanceBetweenPoints),
				MCPParam::Optional(TEXT("distanceBetweenPoints"), EMCPParamType::Number, TEXT("sampler_v2 only: point spacing in centimetres; default 100.")) },
			{ NAME_None, GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, HeightScatterType),
				MCPParam::Optional(TEXT("heightScatterType"), EMCPParamType::String, TEXT("sampler_v2 only: EVoxelHeightScatterType point distribution; default Grid."))
					.Enum({ TEXT("Grid"), TEXT("Sobol"), TEXT("Halton") }) },
			{ NAME_None, GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerV2Settings, MinCellArea),
				MCPParam::Optional(TEXT("minCellArea"), EMCPParamType::Number, TEXT("sampler_v2 only: skip cells with less surface than this, >= 0 (ClampMin); default 0.03.")).Min(0) },
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, PointsPerSquaredMeter), NAME_None,
				MCPParam::Optional(TEXT("pointsPerSquaredMeter"), EMCPParamType::Number, TEXT("sampler only: point density per square metre; default 0.1.")) },
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, CellSize), NAME_None,
				MCPParam::Optional(TEXT("cellSize"), EMCPParamType::Number, TEXT("sampler only: cell size in centimetres; default 100.")) },
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, Tolerance), NAME_None,
				MCPParam::Optional(TEXT("tolerance"), EMCPParamType::Number, TEXT("sampler only: tolerance, >= 0 (ClampMin); default 0.")).Min(0) },
			{ GET_MEMBER_NAME_CHECKED(UPCGVoxelSamplerSettings, bApplyDensityToPoints), NAME_None,
				Flag(TEXT("applyDensityToPoints"), TEXT("sampler only: write density to the points; default true.")) },
		};
		return Fields;
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

		TArray<FPendingEdit> Edits;
		for (const FSamplerField& Field : SamplerFields())
		{
			const TCHAR* Param = *Field.Spec.Name;
			if (!Has(Params, Param)) continue;
			const FName Property = V2 ? Field.V2 : Field.V1;
			if (Property.IsNone())
			{
				return Error(FString::Printf(TEXT("%s does not apply to %s"), Param, *Settings->GetClass()->GetName()));
			}
			if (!PrepareJson(*Settings, Property, Params->TryGetField(Param), Edits.AddDefaulted_GetRef(), Err))
			{
				return Error(FString::Printf(TEXT("%s: %s"), Param, *Err));
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
	const auto AssetPath = [](const TCHAR* Description) { return MCPParam::Required(TEXT("assetPath"), EMCPParamType::String, Description); };
	const auto SavePackage = [] { return Spec::Save(TEXT("Save the asset's package after the edit; default true.")); };
	const auto Paths = [](const TCHAR* Name, const TCHAR* Description) { return MCPParam::Optional(Name, EMCPParamType::Array, Description).Items(EMCPParamType::String); };

	TArray<FString> TypeKeys;
	for (const FAssetType& Type : GAssetTypes)
	{
		TypeKeys.Add(Type.Key);
	}
	Out.Add({ TEXT("voxel_asset_create"), &AssetCreate, {
		MCPParam::Required(TEXT("type"), EMCPParamType::String,
			TEXT("Voxel asset type, made through its own factory; graph types start from their template with an output node.")).Enum(TypeKeys),
		MCPParam::Required(TEXT("name"), EMCPParamType::String, TEXT("Asset name without a path; a valid object and package name.")),
		MCPParam::Optional(TEXT("packagePath"), EMCPParamType::String, TEXT("Content folder; default /Game/Voxel.")),
		MCPParam::Optional(TEXT("onConflict"), EMCPParamType::String,
			TEXT("When the asset exists: skip returns it with existed: true (an error if it is another class), error refuses. An existing asset is never overwritten. Default skip."))
			.Enum({ TEXT("error"), TEXT("skip") }),
		SavePackage(),
	}, MCPSpec::ContractExempt(TEXT("Creates and saves an asset named by the contract values before anything can fail")) });

	Out.Add({ TEXT("voxel_asset_set_property"), &AssetSetProperty, {
		AssetPath(TEXT("Asset path; levels, level actors and class defaults are refused.")),
		MCPParam::Required(TEXT("propertyName"), EMCPParamType::String, TEXT("Top-level C++ property name, e.g. MaxDistance or bInvisible; matched exactly, then case-insensitively.")),
		MCPParam::Required(TEXT("value"), EMCPParamType::Array,
			TEXT("UE import text such as '0.5', 'true', '/Game/M.M' or '(X=1,Y=2,Z=3)'; a number, a boolean, or an array of strings, numbers and booleans ((a,b) import text). An integer property takes only whole numbers, an enum only its value names, and an object reference \"\" to clear."))
			.Or(EMCPParamType::String).Or(EMCPParamType::Number).Or(EMCPParamType::Boolean),
		SavePackage(),
	} });

	TArray<FMCPParamSpec> MegaParams = {
		AssetPath(TEXT("UVoxelMegaMaterial asset path.")),
		Paths(TEXT("surfaceTypes"), TEXT("Distinct UVoxelSurfaceTypeAsset paths; the list the generated material covers.")),
		MCPParam::Optional(TEXT("mode"), EMCPParamType::String,
			TEXT("With surfaceTypes: replace sets the list, append adds the entries not already in it; default replace.")).Enum({ TEXT("replace"), TEXT("append") }),
	};
	const TArray<FMCPParamSpec> MegaSettings = SpecsOf(MegaMaterialFields());
	MegaParams.Append(MegaSettings);
	MegaParams.Add(SavePackage());
	TArray<TArray<FString>> MegaBranches = { { TEXT("surfaceTypes") } };
	MegaBranches.Append(Spec::Branches(MegaSettings));
	Out.Add({ TEXT("voxel_mega_material_set_surfaces"), &MegaMaterialSetSurfaces, MegaParams, MCPSpec::AtLeastOne(MegaBranches) });

	TArray<FMCPParamSpec> SurfaceParams = { AssetPath(TEXT("UVoxelSurfaceTypeAsset asset path.")) };
	SurfaceParams.Append(SpecsOf(SurfaceTypeFields()));
	SurfaceParams.Add(MCPParam::Optional(TEXT("seed"), EMCPParamType::String, TEXT("Text for the surface's exposed seed; a number is written as its text.")).Or(EMCPParamType::Number));
	TArray<TArray<FString>> SurfaceBranches = Spec::Branches(SpecsOf(SurfaceTypeFields()));
	SurfaceBranches.Add({ TEXT("seed") });
	SurfaceParams.Add(SavePackage());
	Out.Add({ TEXT("voxel_surface_type_set"), &SurfaceTypeSet, SurfaceParams, MCPSpec::AtLeastOne(SurfaceBranches) });

	Out.Add({ TEXT("voxel_smart_surface_set"), &SmartSurfaceSet, {
		AssetPath(TEXT("UVoxelSmartSurfaceType asset path.")),
		MCPParam::Optional(TEXT("graph"), EMCPParamType::String, TEXT("UVoxelSmartSurfaceTypeGraph asset path; \"\", None or null clears it.")).Nullable(),
		Spec::ValueMap(TEXT("parameters"), false,
			TEXT("{ parameterName: value } overrides on the target graph's parameters, each parsed as the parameter's type before anything changes.")),
		SavePackage(),
	}, MCPSpec::AtLeastOne({ { TEXT("graph") }, { TEXT("parameters") } }) });

	TArray<FMCPParamSpec> StackParams = {
		AssetPath(TEXT("UVoxelLayerStack asset path.")),
		Paths(TEXT("heightLayers"), TEXT("Distinct UVoxelHeightLayer paths, bottom to top; replaces the list.")),
		Paths(TEXT("volumeLayers"), TEXT("Distinct UVoxelVolumeLayer paths, bottom to top; replaces the list.")),
	};
	StackParams.Append(SpecsOf(LayerStackFields()));
	StackParams.Add(SavePackage());
	TArray<TArray<FString>> StackBranches = { { TEXT("heightLayers") }, { TEXT("volumeLayers") } };
	StackBranches.Append(Spec::Branches(SpecsOf(LayerStackFields())));
	Out.Add({ TEXT("voxel_layer_stack_set"), &LayerStackSet, StackParams, MCPSpec::AtLeastOne(StackBranches) });

	TArray<FString> NodeKeys;
	for (const FPCGNodeType& Type : GPCGNodeTypes)
	{
		NodeKeys.Add(Type.Key);
	}
	Out.Add({ TEXT("voxel_pcg_add_node"), &PCGAddNode, {
		MCPParam::Required(TEXT("graphPath"), EMCPParamType::String, TEXT("UPCGGraph asset path.")),
		MCPParam::Required(TEXT("nodeType"), EMCPParamType::String, TEXT("Voxel PCG node to add; sampler_v2 is Voxel Sampler Experimental.")).Enum(NodeKeys),
		MCPParam::Optional(TEXT("x"), EMCPParamType::Integer, TEXT("Node X position; default where the graph placed it.")).Range(MIN_int32, MAX_int32),
		MCPParam::Optional(TEXT("y"), EMCPParamType::Integer, TEXT("Node Y position; default where the graph placed it.")).Range(MIN_int32, MAX_int32),
		SavePackage(),
	} });

	TArray<FMCPParamSpec> SamplerParams = {
		MCPParam::Required(TEXT("graphPath"), EMCPParamType::String, TEXT("UPCGGraph asset path.")),
		MCPParam::Required(TEXT("node"), EMCPParamType::String, TEXT("Sampler node by object name, or by authored or displayed title when unique (case-insensitive).")),
		MCPParam::Optional(TEXT("stack"), EMCPParamType::String, TEXT("UVoxelLayerStack asset path; the layer is kept unless layer is given.")),
		MCPParam::Optional(TEXT("layer"), EMCPParamType::String, TEXT("UVoxelHeightLayer or UVoxelVolumeLayer asset path; the stack is kept unless stack is given.")),
		Paths(TEXT("metadatasToQuery"), TEXT("Distinct UVoxelMetadata asset paths written as point attributes; replaces the list.")),
	};
	TArray<FMCPParamSpec> SamplerSettings;
	for (const FSamplerField& Field : SamplerFields())
	{
		SamplerSettings.Add(Field.Spec);
	}
	SamplerParams.Append(SamplerSettings);
	SamplerParams.Add(SavePackage());
	TArray<TArray<FString>> SamplerBranches = { { TEXT("stack") }, { TEXT("layer") }, { TEXT("metadatasToQuery") } };
	SamplerBranches.Append(Spec::Branches(SamplerSettings));
	Out.Add({ TEXT("voxel_pcg_configure_sampler"), &PCGConfigureSampler, SamplerParams, MCPSpec::AtLeastOne(SamplerBranches) });
}
}

#undef LOCTEXT_NAMESPACE
