#include "VoxelToolsCommon.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

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
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
			{
				Object->SetField(Pair.Key, SanitizeJson(Pair.Value));
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

	bool Vec(const FParams& Params, const TCHAR* Field, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(Field, Object))
		{
			return false;
		}
		double X = 0, Y = 0, Z = 0;
		const bool bAll = (*Object)->TryGetNumberField(TEXT("x"), X) &
			(*Object)->TryGetNumberField(TEXT("y"), Y) &
			(*Object)->TryGetNumberField(TEXT("z"), Z);
		Out = FVector(X, Y, Z);
		return bAll;
	}

	bool Rot(const FParams& Params, const TCHAR* Field, FRotator& Out)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(Field, Object))
		{
			return false;
		}
		double Pitch = 0, Yaw = 0, Roll = 0;
		(*Object)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*Object)->TryGetNumberField(TEXT("yaw"), Yaw);
		(*Object)->TryGetNumberField(TEXT("roll"), Roll);
		Out = FRotator(Pitch, Yaw, Roll);
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
		const FString Text = In.TrimStartAndEnd();
		const FVoxelPinType& Type = Value.GetType();
		if (Type.Is<float>() || Type.Is<double>() || Type.Is<int32>() || Type.Is<int64>())
		{
			if (!Text.IsNumeric())
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

	FString ValueText(const TSharedPtr<FJsonValue>& Value)
	{
		FString Text;
		if (!Value.IsValid() || Value->TryGetString(Text))
		{
			return Text;
		}
		double Number = 0;
		bool bBool = false;
		if (Value->TryGetNumber(Number))
		{
			return FString::SanitizeFloat(Number);
		}
		if (Value->TryGetBool(bBool))
		{
			return bBool ? TEXT("true") : TEXT("false");
		}
		return Text;
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
