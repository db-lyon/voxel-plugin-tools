#include "VoxelToolsCommon.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"

#include "VoxelPinType.h"
#include "VoxelPinValue.h"
#include "Buffer/VoxelBaseBuffers.h"
#include "VoxelLayer.h"
#include "VoxelLayerStack.h"
#include "VoxelStackLayer.h"

namespace VoxelPluginTools
{
	FResult SanitizeJson(const FResult& Value)
	{
		if (!Value.IsValid())
		{
			return Value;
		}
		switch (Value->Type)
		{
		case EJson::Number:
			return FMath::IsFinite(Value->AsNumber()) ? Value : MakeShared<FJsonValueNull>();
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Items.Add(SanitizeJson(Item));
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		case EJson::Object:
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			for (const auto& Pair : Value->AsObject()->Values)
			{
				Object->SetField(FString(*Pair.Key), SanitizeJson(Pair.Value));
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		default:
			return Value;
		}
	}

	FResult Error(const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("error"), Message);
		return MakeShared<FJsonValueObject>(Out);
	}

	FResult Ok(const TSharedRef<FJsonObject>& Out)
	{
		Out->SetBoolField(TEXT("success"), true);
		return MakeShared<FJsonValueObject>(Out);
	}

	FString Str(const FParams& Params, const TCHAR* Field)
	{
		FString Value;
		if (Params.IsValid())
		{
			Params->TryGetStringField(Field, Value);
		}
		return Value;
	}

	bool Bool(const FParams& Params, const TCHAR* Field, bool Default)
	{
		bool Value = Default;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(Field, Value);
		}
		return Value;
	}

	double Num(const FParams& Params, const TCHAR* Field, double Default)
	{
		double Value = Default;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(Field, Value);
		}
		return Value;
	}

	bool Has(const FParams& Params, const TCHAR* Field)
	{
		return Params.IsValid() && Params->HasField(Field);
	}

	bool OnlyKeys(const FParams& Params, TConstArrayView<const TCHAR*> Allowed, const FString& Context, FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}
		for (const auto& Pair : Params->Values)
		{
			if (!Allowed.ContainsByPredicate([&](const TCHAR* Name) { return FString(*Pair.Key).Equals(Name, ESearchCase::CaseSensitive); }))
			{
				TArray<FString> Names;
				for (const TCHAR* Name : Allowed) Names.Add(Name);
				OutError = FString::Printf(TEXT("%s does not apply to %s, which takes: %s"), *Pair.Key, *Context, *FString::Join(Names, TEXT(", ")));
				return false;
			}
		}
		return true;
	}

	namespace
	{
		// The named JSON numbers of an object, all required: no coercion from strings or booleans, no default.
		bool Numbers(const FParams& Params, const TCHAR* Field, std::initializer_list<const TCHAR*> Names, TArray<double>& Out)
		{
			const TSharedPtr<FJsonValue> Value = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			for (const TCHAR* Name : Names)
			{
				const TSharedPtr<FJsonValue> Number = Object->TryGetField(Name);
				if (!Number.IsValid() || Number->Type != EJson::Number || !FMath::IsFinite(Number->AsNumber()))
				{
					return false;
				}
				Out.Add(Number->AsNumber());
			}
			return Object->Values.Num() == static_cast<int32>(Names.size());
		}
	}

	bool Vec(const FParams& Params, const TCHAR* Field, FVector& Out)
	{
		TArray<double> V;
		if (!Numbers(Params, Field, { TEXT("x"), TEXT("y"), TEXT("z") }, V))
		{
			return false;
		}
		Out = FVector(V[0], V[1], V[2]);
		return true;
	}

	bool Rot(const FParams& Params, const TCHAR* Field, FRotator& Out)
	{
		TArray<double> V;
		if (!Numbers(Params, Field, { TEXT("pitch"), TEXT("yaw"), TEXT("roll") }, V))
		{
			return false;
		}
		Out = FRotator(V[0], V[1], V[2]);
		return true;
	}

	TSharedRef<FJsonObject> VecJson(const FVector& Value)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("x"), Value.X);
		Out->SetNumberField(TEXT("y"), Value.Y);
		Out->SetNumberField(TEXT("z"), Value.Z);
		return Out;
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	AActor* FindActor(const FParams& Params, FString& OutError)
	{
		const FString Path = Str(Params, TEXT("actorPath"));
		const FString Label = Str(Params, TEXT("actorLabel"));
		if (Path.IsEmpty() && Label.IsEmpty())
		{
			OutError = TEXT("actorPath or actorLabel is required");
			return nullptr;
		}
		UWorld* World = EditorWorld();
		if (!World)
		{
			OutError = TEXT("No editor world");
			return nullptr;
		}
		if (!Path.IsEmpty())
		{
			// FindObject also returns actors kept alive by the undo buffer after deletion, and PIE copies.
			AActor* Actor = FindObject<AActor>(nullptr, *Path);
			if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
			{
				OutError = FString::Printf(TEXT("No live actor at %s"), *Path);
				return nullptr;
			}
			if (Actor->GetWorld() != World)
			{
				OutError = FString::Printf(TEXT("%s is not in the editor world (PIE actors cannot be edited)"), *Path);
				return nullptr;
			}
			return Actor;
		}
		TArray<AActor*> Matches;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == Label)
			{
				Matches.Add(*It);
			}
		}
		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		OutError = Matches.Num() == 0
			? FString::Printf(TEXT("No actor labeled '%s'"), *Label)
			: FString::Printf(TEXT("%d actors are labeled '%s'; use actorPath"), Matches.Num(), *Label);
		return nullptr;
	}

	UObject* LoadTyped(const FString& Path, UClass* Class, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = FString::Printf(TEXT("A %s path is required"), *Class->GetName());
			return nullptr;
		}
		UObject* Object = StaticLoadObject(Class, nullptr, *Path);
		if (!Object)
		{
			// Accept a package path without the object name.
			const FString Full = Path + TEXT(".") + FPackageName::GetShortName(Path);
			Object = StaticLoadObject(Class, nullptr, *Full);
		}
		if (!Object)
		{
			OutError = FString::Printf(TEXT("No %s at '%s'"), *Class->GetName(), *Path);
		}
		return Object;
	}

	bool ParseValue(FVoxelPinValue& Value, const FString& In)
	{
		FString Text = In.TrimStartAndEnd();
		const FVoxelPinType& Type = Value.GetType();
		if (Value.IsObject())
		{
			if (Text.IsEmpty() || Text == TEXT("None"))
			{
				return Value.ImportFromString(TEXT("None"));
			}
			// Accept a bare package path: /Game/Foo/Bar means /Game/Foo/Bar.Bar.
			if (!Text.Contains(TEXT(".")) && Text.StartsWith(TEXT("/")))
			{
				Text += TEXT(".") + FPackageName::GetShortName(Text);
			}
			// An object that does not resolve imports as null and Voxel's fixup then drops the override silently.
			return Value.ImportFromString(Text) && Value.GetObject() != nullptr;
		}
		if (Text.IsEmpty())
		{
			// ImportFromString leaves a struct, name or vector at its default for empty text.
			return false;
		}
		if (Type.Is<float>() || Type.Is<double>() || Type.Is<int32>() || Type.Is<int64>())
		{
			if (!Text.IsNumeric())
			{
				return false;
			}
			// ImportFromString truncates 5.5 into an integer.
			if ((Type.Is<int32>() || Type.Is<int64>()) && Text.Contains(TEXT(".")))
			{
				return false;
			}
		}
		else if (Type.Is<bool>())
		{
			if (!(Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
				  Text == TEXT("1") || Text == TEXT("0")))
			{
				return false;
			}
		}
		return Value.ImportFromString(Text);
	}

	bool ScalarText(const TSharedPtr<FJsonValue>& Value, FString& Out)
	{
		Out.Reset();
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		// A number reads as SanitizeFloat(v, 0) ("5", "5.5") and a boolean as "true" / "false".
		return (Value->Type == EJson::String || Value->Type == EJson::Number || Value->Type == EJson::Boolean) && Value->TryGetString(Out);
	}

	bool ScalarField(const FParams& Params, const TCHAR* Field, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
		return Value.IsValid() && Value->Type != EJson::Null && ScalarText(Value, Out);
	}

	bool ParsePinType(const FString& In, FVoxelPinType& Out, FString& OutError)
	{
		const FString T = In.TrimStartAndEnd();
		const FString L = T.ToLower();
		if (L == TEXT("float")) { Out = FVoxelPinType::Make<float>(); return true; }
		if (L == TEXT("double")) { Out = FVoxelPinType::Make<double>(); return true; }
		if (L == TEXT("int") || L == TEXT("int32")) { Out = FVoxelPinType::Make<int32>(); return true; }
		if (L == TEXT("int64")) { Out = FVoxelPinType::Make<int64>(); return true; }
		if (L == TEXT("bool")) { Out = FVoxelPinType::Make<bool>(); return true; }
		if (L == TEXT("name")) { Out = FVoxelPinType::Make<FName>(); return true; }
		if (L == TEXT("vector2d")) { Out = FVoxelPinType::Make<FVector2D>(); return true; }
		if (L == TEXT("vector")) { Out = FVoxelPinType::Make<FVector>(); return true; }
		if (L == TEXT("color") || L == TEXT("linearcolor")) { Out = FVoxelPinType::Make<FLinearColor>(); return true; }
		if (L == TEXT("seed")) { Out = FVoxelPinType::Make<FVoxelSeed>(); return true; }

		FString Kind, Path;
		if (T.Split(TEXT(":"), &Kind, &Path))
		{
			Kind = Kind.ToLower();
			if (Kind == TEXT("struct"))
			{
				if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Path)) { Out = FVoxelPinType::MakeStruct(Struct); return true; }
			}
			else if (Kind == TEXT("object"))
			{
				if (UClass* Class = LoadObject<UClass>(nullptr, *Path)) { Out = FVoxelPinType::MakeObject(Class); return true; }
			}
			else if (Kind == TEXT("class"))
			{
				if (UClass* Class = LoadObject<UClass>(nullptr, *Path)) { Out = FVoxelPinType::MakeClass(Class); return true; }
			}
			else if (Kind == TEXT("enum"))
			{
				if (UEnum* Enum = LoadObject<UEnum>(nullptr, *Path)) { Out = FVoxelPinType::MakeEnum(Enum); return true; }
			}
			OutError = FString::Printf(TEXT("Could not load %s '%s'"), *Kind, *Path);
			return false;
		}
		OutError = FString::Printf(TEXT("Unknown type '%s'. Use float, double, int32, int64, bool, name, vector2d, vector, color, seed, or struct:/object:/class:/enum:<path>"), *T);
		return false;
	}

	bool ParseStackLayer(const FParams& Params, const TCHAR* StackField, const TCHAR* LayerField, bool bHeight, FVoxelStackLayer& Out, FString& OutError)
	{
		const FString StackPath = Str(Params, StackField);
		const FString LayerPath = Str(Params, LayerField);
		for (const TCHAR* Field : { StackField, LayerField })
		{
			if (Has(Params, Field) && Str(Params, Field).IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s must not be empty; omit it for Voxel's built-in default"), Field);
				return false;
			}
		}

		Out.Stack = StackPath.IsEmpty() ? UVoxelLayerStack::Default() : Load<UVoxelLayerStack>(StackPath, OutError);
		if (!Out.Stack)
		{
			return false;
		}
		if (LayerPath.IsEmpty())
		{
			Out.Layer = bHeight ? static_cast<UVoxelLayer*>(UVoxelHeightLayer::Default()) : static_cast<UVoxelLayer*>(UVoxelVolumeLayer::Default());
		}
		else
		{
			Out.Layer = bHeight ? static_cast<UVoxelLayer*>(Load<UVoxelHeightLayer>(LayerPath, OutError)) : static_cast<UVoxelLayer*>(Load<UVoxelVolumeLayer>(LayerPath, OutError));
		}
		return Out.Layer != nullptr;
	}

	TSharedRef<FJsonObject> ActorJson(const AActor& Actor)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("actorPath"), Actor.GetPathName());
		Out->SetStringField(TEXT("actorLabel"), Actor.GetActorLabel());
		Out->SetStringField(TEXT("class"), Actor.GetClass()->GetName());
		Out->SetObjectField(TEXT("location"), VecJson(Actor.GetActorLocation()));
		return Out;
	}

	namespace Spec
	{
		FMCPParamSpec ActorPath(const TCHAR* Description)
		{
			return MCPParam::Optional(TEXT("actorPath"), EMCPParamType::String, Description);
		}

		FMCPParamSpec ActorLabel(const TCHAR* Description)
		{
			return MCPParam::Optional(TEXT("actorLabel"), EMCPParamType::String, Description);
		}

		FMCPSpecRules OneActor()
		{
			return MCPSpec::ExactlyOne({ { TEXT("actorPath") }, { TEXT("actorLabel") } });
		}

		FMCPParamSpec ComponentName(const TCHAR* Description)
		{
			return MCPParam::Optional(TEXT("componentName"), EMCPParamType::String, Description);
		}

		FMCPParamSpec SaveDirty()
		{
			return Save(TEXT("Save the content packages this call dirties before replying; default true. Levels are never saved."));
		}

		FMCPParamSpec Save(const TCHAR* Description)
		{
			return MCPParam::Optional(TEXT("save"), EMCPParamType::Boolean, Description);
		}

		FMCPParamSpec Vec3(const TCHAR* Name, const TCHAR* Description)
		{
			return MCPParam::Optional(Name, EMCPParamType::Vec3, Description);
		}

		FMCPParamSpec ValueMap(const TCHAR* Name, bool bRequired, const TCHAR* Description)
		{
			FMCPParamSpec Param = MCPParam::Optional(Name, EMCPParamType::Any, Description).OneOfForms({ EMCPValueForm::ScalarMap });
			Param.bRequired = bRequired;
			return Param;
		}

		TArray<TArray<FString>> Branches(const TArray<FMCPParamSpec>& Params)
		{
			TArray<TArray<FString>> Out;
			for (const FMCPParamSpec& Param : Params)
			{
				Out.Add({ Param.Name });
			}
			return Out;
		}
	}

	const TArray<FHandlerEntry>& GetHandlers()
	{
		static const TArray<FHandlerEntry> Handlers = []
		{
			TArray<FHandlerEntry> Out;
			AddGraphHandlers(Out);
			AddWorldHandlers(Out);
			AddStampHandlers(Out);
			AddSculptHandlers(Out);
			AddAssetHandlers(Out);
			return Out;
		}();
		return Handlers;
	}
}
