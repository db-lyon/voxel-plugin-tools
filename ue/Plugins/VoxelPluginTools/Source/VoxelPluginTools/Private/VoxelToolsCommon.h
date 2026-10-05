#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MCPHandlerRegistration.h"

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

	struct FHandlerEntry
	{
		FString Name;
		UEMCP::FExternalHandlerFn Fn;
	};

	// Replaces NaN/Inf numbers with null: JSON has no spelling for them, and the bridge drops a reply that contains them.
	FResult SanitizeJson(const FResult& Value);

	FResult Error(const FString& Message);
	FResult Ok(const TSharedRef<FJsonObject>& Out);

	FString Str(const FParams& Params, const TCHAR* Field);
	bool Bool(const FParams& Params, const TCHAR* Field, bool Default);
	double Num(const FParams& Params, const TCHAR* Field, double Default);
	bool Has(const FParams& Params, const TCHAR* Field);

	// {x,y,z} vector; false when the field is absent or malformed.
	bool Vec(const FParams& Params, const TCHAR* Field, FVector& Out);
	// {pitch,yaw,roll} rotator.
	bool Rot(const FParams& Params, const TCHAR* Field, FRotator& Out);
	TSharedRef<FJsonObject> VecJson(const FVector& Value);

	UWorld* EditorWorld();

	// Actor by actorPath (stable) or actorLabel. Voxel stamp actors relabel themselves, so prefer the path.
	AActor* FindActor(const FParams& Params, FString& OutError);

	template<typename T>
	T* FindComponent(AActor& Actor, const FParams& Params, FString& OutError)
	{
		const FString Name = Str(Params, TEXT("componentName"));
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

	// Voxel's ImportFromString reads non-numeric text as 0, so scalars are checked first.
	bool ParseValue(FVoxelPinValue& Value, const FString& Text);
	// Text from a JSON value of any scalar kind.
	FString ValueText(const TSharedPtr<FJsonValue>& Value);
	bool ParsePinType(const FString& In, FVoxelPinType& Out, FString& OutError);

	// "Stack:Layer" asset paths, or a bare layer path with the stack defaulted to the project default.
	bool ParseStackLayer(const FParams& Params, const TCHAR* StackField, const TCHAR* LayerField, bool bHeight, FVoxelStackLayer& Out, FString& OutError);

	// Standard actor identity block for results.
	TSharedRef<FJsonObject> ActorJson(const AActor& Actor);

	void AddGraphHandlers(TArray<FHandlerEntry>& Out);
	void AddWorldHandlers(TArray<FHandlerEntry>& Out);
	void AddStampHandlers(TArray<FHandlerEntry>& Out);
	void AddSculptHandlers(TArray<FHandlerEntry>& Out);
	void AddAssetHandlers(TArray<FHandlerEntry>& Out);

	const TArray<FHandlerEntry>& GetHandlers();
	void ReleaseCatalog();
}
