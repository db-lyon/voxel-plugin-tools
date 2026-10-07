#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MCPHandlerRegistration.h"
#include "MCPHandlerSpec.h"

class AActor;
class UActorComponent;
class UWorld;
struct FVoxelPinValue;
struct FVoxelPinType;
struct FVoxelStackLayer;

// Shared conventions for every VoxelPluginTools handler.
namespace VoxelPluginTools
{
	using FResult = TSharedPtr<FJsonValue>;
	using FParams = TSharedPtr<FJsonObject>;

	// A handler and its parameter contract (MCPHandlerSpec.h), registered together.
	struct FHandlerEntry
	{
		FString Name;
		UEMCP::FExternalHandlerFn Fn;
		TArray<FMCPParamSpec> Params;
		FMCPSpecRules Rules;
	};

	// Replaces NaN/Inf numbers with null: JSON has no spelling for them, and the bridge drops a reply that contains them.
	FResult SanitizeJson(const FResult& Value);

	FResult Error(const FString& Message);
	FResult Ok(const TSharedRef<FJsonObject>& Out);

	FString Str(const FParams& Params, const TCHAR* Field);
	bool Bool(const FParams& Params, const TCHAR* Field, bool Default);
	double Num(const FParams& Params, const TCHAR* Field, double Default);
	bool Has(const FParams& Params, const TCHAR* Field);

	// False with OutError naming the first present key that is not in Allowed, which do not apply to Context.
	bool OnlyKeys(const FParams& Params, TConstArrayView<const TCHAR*> Allowed, const FString& Context, FString& OutError);

	// {x,y,z} of JSON numbers; false when the field is absent or malformed.
	bool Vec(const FParams& Params, const TCHAR* Field, FVector& Out);
	// {pitch,yaw,roll} of JSON numbers; false when the field is absent or malformed.
	bool Rot(const FParams& Params, const TCHAR* Field, FRotator& Out);
	TSharedRef<FJsonObject> VecJson(const FVector& Value);

	UWorld* EditorWorld();

	// Actor by actorPath (stable) or actorLabel. Voxel stamp actors relabel themselves, so prefer the path.
	AActor* FindActor(const FParams& Params, FString& OutError);

	template<typename T>
	T* FindComponent(AActor& Actor, const FParams& Params, FString& OutError)
	{
		const FString Name = Str(Params, TEXT("componentName"));
		if (Name.IsEmpty() && Has(Params, TEXT("componentName")))
		{
			OutError = TEXT("componentName must not be empty; omit it for the actor's first component");
			return nullptr;
		}
		TArray<T*> Components;
		Actor.GetComponents<T>(Components);
		for (T* Component : Components)
		{
			if (Name.IsEmpty() || Component->GetName() == Name)
			{
				return Component;
			}
		}
		OutError = FString::Printf(TEXT("%s has no %s%s"), *Actor.GetPathName(), *T::StaticClass()->GetName(),
			Name.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" named %s"), *Name));
		return nullptr;
	}

	// Loads an object of class T from a content path; sets OutError when it fails or is the wrong type.
	UObject* LoadTyped(const FString& Path, UClass* Class, FString& OutError);
	template<typename T>
	T* Load(const FString& Path, FString& OutError)
	{
		return Cast<T>(LoadTyped(Path, T::StaticClass(), OutError));
	}

	// Voxel's ImportFromString reads non-numeric text as 0, so scalars are checked first. Empty text is None for
	// object types and an error for every other type.
	bool ParseValue(FVoxelPinValue& Value, const FString& Text);
	// Value text from a JSON string, number or boolean; null is the empty text. False for an object or array.
	bool ScalarText(const TSharedPtr<FJsonValue>& Value, FString& Out);
	// Text of a present string, number or boolean field; false when absent, null, an object or an array.
	bool ScalarField(const FParams& Params, const TCHAR* Field, FString& Out);
	bool ParsePinType(const FString& In, FVoxelPinType& Out, FString& OutError);

	// Stack and layer asset paths; each one omitted defaults to Voxel's built-in default (/Voxel/Default). Present but empty is an error.
	bool ParseStackLayer(const FParams& Params, const TCHAR* StackField, const TCHAR* LayerField, bool bHeight, FVoxelStackLayer& Out, FString& OutError);

	// Standard actor identity block for results.
	TSharedRef<FJsonObject> ActorJson(const AActor& Actor);

	// Contract pieces several handlers share.
	namespace Spec
	{
		FMCPParamSpec ActorPath(const TCHAR* Description);
		FMCPParamSpec ActorLabel(const TCHAR* Description);
		// actorPath or actorLabel, never both: FindActor would silently prefer the path.
		FMCPSpecRules OneActor();
		FMCPParamSpec ComponentName(const TCHAR* Description);
		// The save RunHandler reads on every mutating handler.
		FMCPParamSpec SaveDirty();
		FMCPParamSpec Save(const TCHAR* Description);
		FMCPParamSpec Vec3(const TCHAR* Name, const TCHAR* Description);
		// { name: value }, published as the argMap form; the handlers that read it take only strings, numbers,
		// booleans and nulls as values, which no value form expresses, so each description says so.
		FMCPParamSpec ValueMap(const TCHAR* Name, bool bRequired, const TCHAR* Description);
		// The parameter's names, in order, as single-name branches of a choice.
		TArray<TArray<FString>> Branches(const TArray<FMCPParamSpec>& Params);
	}

	void AddGraphHandlers(TArray<FHandlerEntry>& Out);
	void AddWorldHandlers(TArray<FHandlerEntry>& Out);
	void AddStampHandlers(TArray<FHandlerEntry>& Out);
	void AddSculptHandlers(TArray<FHandlerEntry>& Out);
	void AddAssetHandlers(TArray<FHandlerEntry>& Out);

	const TArray<FHandlerEntry>& GetHandlers();
	void ReleaseCatalog();
}
