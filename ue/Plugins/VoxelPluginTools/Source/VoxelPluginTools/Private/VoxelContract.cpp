#include "VoxelToolsCommon.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

// Holds a call to its handler's contract the way the ue-mcp server does (src/surface/handler-spec.ts,
// contractViolation). Stricter in one place: unknown keys inside {x,y,z}, {pitch,yaw,roll}, {r,g,b,a?} and entry-list
// objects are refused too.
namespace VoxelPluginTools
{
namespace
{
	// The value under exactly Key, or null when absent. FJsonObject key comparison differs across engine versions;
	// the contract's is case-sensitive.
	TSharedPtr<FJsonValue> FindExact(const FJsonObject& Object, const FString& Key)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
		{
			if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				return Pair.Value.IsValid() ? Pair.Value : MakeShared<FJsonValueNull>();
			}
		}
		return nullptr;
	}

	FString Shown(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("nothing");
		}
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Text.Len() > 80 ? Text.Left(77) + TEXT("...") : Text;
	}

	const TCHAR* TypeText(EMCPParamType Type)
	{
		switch (Type)
		{
		case EMCPParamType::String: return TEXT("a string");
		case EMCPParamType::Number: return TEXT("a number");
		case EMCPParamType::Integer: return TEXT("an integer");
		case EMCPParamType::Boolean: return TEXT("a boolean");
		case EMCPParamType::Object: return TEXT("an object");
		case EMCPParamType::Array: return TEXT("an array");
		case EMCPParamType::Vec3: return TEXT("{x,y,z} numbers");
		case EMCPParamType::Rotator: return TEXT("{pitch,yaw,roll} numbers");
		case EMCPParamType::Color: return TEXT("{r,g,b,a?} numbers");
		default: return TEXT("any value");
		}
	}

	bool IsNumber(const TSharedPtr<FJsonValue>& Value)
	{
		return Value.IsValid() && Value->Type == EJson::Number && FMath::IsFinite(Value->AsNumber());
	}

	// An object holding exactly the named numbers; Optional names may be absent.
	FString NumberObject(const FString& Path, const TSharedPtr<FJsonValue>& Value, TConstArrayView<const TCHAR*> Required, TConstArrayView<const TCHAR*> Optional, EMCPParamType Type)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			return FString::Printf(TEXT("%s must be %s (got %s)"), *Path, TypeText(Type), *Shown(Value));
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const auto Named = [&](const TCHAR* Name) { return Pair.Key.Equals(Name, ESearchCase::CaseSensitive); };
			if (!Required.ContainsByPredicate(Named) && !Optional.ContainsByPredicate(Named))
			{
				return FString::Printf(TEXT("%s.%s is not a field of %s"), *Path, *Pair.Key, TypeText(Type));
			}
			if (!IsNumber(Pair.Value))
			{
				return FString::Printf(TEXT("%s.%s must be a number (got %s)"), *Path, *Pair.Key, *Shown(Pair.Value));
			}
		}
		for (const TCHAR* Name : Required)
		{
			if (!FindExact(*Object, Name).IsValid())
			{
				return FString::Printf(TEXT("%s needs %s"), *Path, Name);
			}
		}
		return FString();
	}

	// The value's JSON kind and fixed shape, without enums, ranges or declared fields.
	FString CheckBase(const FString& Path, EMCPParamType Type, const TSharedPtr<FJsonValue>& Value)
	{
		const EJson Kind = Value.IsValid() ? Value->Type : EJson::None;
		bool bFits = false;
		switch (Type)
		{
		case EMCPParamType::String: bFits = Kind == EJson::String; break;
		case EMCPParamType::Number: bFits = IsNumber(Value); break;
		case EMCPParamType::Integer: bFits = IsNumber(Value) && Value->AsNumber() == FMath::RoundToDouble(Value->AsNumber()); break;
		case EMCPParamType::Boolean: bFits = Kind == EJson::Boolean; break;
		case EMCPParamType::Object: bFits = Kind == EJson::Object; break;
		case EMCPParamType::Array: bFits = Kind == EJson::Array; break;
		case EMCPParamType::Vec3: return NumberObject(Path, Value, { TEXT("x"), TEXT("y"), TEXT("z") }, {}, Type);
		case EMCPParamType::Rotator: return NumberObject(Path, Value, { TEXT("pitch"), TEXT("yaw"), TEXT("roll") }, {}, Type);
		case EMCPParamType::Color: return NumberObject(Path, Value, { TEXT("r"), TEXT("g"), TEXT("b") }, { TEXT("a") }, Type);
		default: bFits = Value.IsValid(); break;
		}
		return bFits ? FString() : FString::Printf(TEXT("%s must be %s (got %s)"), *Path, TypeText(Type), *Shown(Value));
	}

	FString CheckRules(const FString& Path, const TArray<FString>& Enum, const TOptional<double>& Minimum, const TOptional<double>& Maximum, const TSharedPtr<FJsonValue>& Value)
	{
		if (Enum.Num() > 0 && Value->Type == EJson::String)
		{
			const FString Text = Value->AsString();
			if (!Enum.ContainsByPredicate([&](const FString& Allowed) { return Allowed.Equals(Text, ESearchCase::CaseSensitive); }))
			{
				return FString::Printf(TEXT("%s must be one of %s (got \"%s\")"), *Path, *FString::Join(Enum, TEXT(", ")), *Text);
			}
		}
		if (Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			if (Minimum.IsSet() && Number < Minimum.GetValue())
			{
				return FString::Printf(TEXT("%s must be >= %s (got %s)"), *Path, *FString::SanitizeFloat(Minimum.GetValue(), 0), *FString::SanitizeFloat(Number, 0));
			}
			if (Maximum.IsSet() && Number > Maximum.GetValue())
			{
				return FString::Printf(TEXT("%s must be <= %s (got %s)"), *Path, *FString::SanitizeFloat(Maximum.GetValue(), 0), *FString::SanitizeFloat(Number, 0));
			}
		}
		return FString();
	}

	bool IsScalar(const TSharedPtr<FJsonValue>& Value)
	{
		const EJson Kind = Value.IsValid() ? Value->Type : EJson::None;
		return Kind == EJson::String || Kind == EJson::Number || Kind == EJson::Boolean || Kind == EJson::Null;
	}

	// One value of an argMap or argEntryList: a scalar, an object, or an array of those or of scalar arrays.
	bool IsArgValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (IsScalar(Value) || Value->Type == EJson::Object)
		{
			return true;
		}
		if (Value->Type != EJson::Array)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			const bool bScalarArray = Item.IsValid() && Item->Type == EJson::Array &&
				!Item->AsArray().ContainsByPredicate([](const TSharedPtr<FJsonValue>& Inner) { return !IsScalar(Inner); });
			if (!IsScalar(Item) && !(Item.IsValid() && Item->Type == EJson::Object) && !bScalarArray)
			{
				return false;
			}
		}
		return true;
	}

	bool FitsForm(EMCPValueForm Form, const TSharedPtr<FJsonValue>& Value)
	{
		switch (Form)
		{
		case EMCPValueForm::ArgMap:
			if (Value->Type != EJson::Object) return false;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
			{
				if (!IsArgValue(Pair.Value)) return false;
			}
			return true;
		case EMCPValueForm::ArgEntryList:
			if (Value->Type != EJson::Array) return false;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (!Item.IsValid() || Item->Type != EJson::Object) return false;
				const TSharedPtr<FJsonObject> Entry = Item->AsObject();
				const TSharedPtr<FJsonValue> Name = FindExact(*Entry, TEXT("name"));
				const TSharedPtr<FJsonValue> EntryValue = FindExact(*Entry, TEXT("value"));
				if (!Name.IsValid() || Name->Type != EJson::String) return false;
				if (EntryValue.IsValid() && !IsArgValue(EntryValue)) return false;
				if (Entry->Values.Num() > (EntryValue.IsValid() ? 2 : 1)) return false;
			}
			return true;
		case EMCPValueForm::StringList:
			return Value->Type == EJson::Array &&
				!Value->AsArray().ContainsByPredicate([](const TSharedPtr<FJsonValue>& Item) { return !Item.IsValid() || Item->Type != EJson::String; });
		default:
			return Value->Type == EJson::String;
		}
	}

	FString CheckForms(const FString& Path, const TArray<EMCPValueForm>& Forms, const TSharedPtr<FJsonValue>& Value)
	{
		if (Forms.ContainsByPredicate([&](EMCPValueForm Form) { return FitsForm(Form, Value); }))
		{
			return FString();
		}
		TArray<FString> Phrases;
		for (const EMCPValueForm Form : Forms)
		{
			switch (Form)
			{
			case EMCPValueForm::ArgMap: Phrases.Add(TEXT("an object mapping a name to a string, number, boolean or null")); break;
			case EMCPValueForm::ArgEntryList: Phrases.Add(TEXT("an entry list [{name, value?}]")); break;
			case EMCPValueForm::StringList: Phrases.Add(TEXT("an array of strings")); break;
			default: Phrases.Add(TEXT("a string")); break;
			}
		}
		return FString::Printf(TEXT("%s must be %s (got %s)"), *Path, *FString::Join(Phrases, TEXT(" or ")), *Shown(Value));
	}

	FString CheckFields(const FString& Path, const TArray<FMCPParamField>& Fields, const TSharedPtr<FJsonObject>& Object, const TCHAR* TagKey = nullptr);

	FString CheckField(const FString& Path, const FMCPParamField& Field, const TSharedPtr<FJsonValue>& Value)
	{
		if (Field.Forms.Num() > 0)
		{
			return CheckForms(Path, Field.Forms, Value);
		}
		if (FString Problem = CheckBase(Path, Field.Type, Value); !Problem.IsEmpty())
		{
			return Problem;
		}
		if (Field.Type == EMCPParamType::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num(); Index++)
			{
				const FString At = FString::Printf(TEXT("%s[%d]"), *Path, Index);
				FString Problem = CheckBase(At, Field.ItemType, Items[Index]);
				if (Problem.IsEmpty()) Problem = CheckRules(At, Field.EnumValues, Field.Minimum, Field.Maximum, Items[Index]);
				if (Problem.IsEmpty() && Field.Fields.Num() > 0) Problem = CheckFields(At, Field.Fields, Items[Index]->AsObject());
				if (!Problem.IsEmpty()) return Problem;
			}
			return FString();
		}
		if (FString Problem = CheckRules(Path, Field.EnumValues, Field.Minimum, Field.Maximum, Value); !Problem.IsEmpty())
		{
			return Problem;
		}
		return Field.Fields.Num() > 0 ? CheckFields(Path, Field.Fields, Value->AsObject()) : FString();
	}

	FString CheckFields(const FString& Path, const TArray<FMCPParamField>& Fields, const TSharedPtr<FJsonObject>& Object, const TCHAR* TagKey)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const bool bDeclared = (TagKey && Pair.Key.Equals(TagKey, ESearchCase::CaseSensitive)) ||
				Fields.ContainsByPredicate([&](const FMCPParamField& Field) { return Field.Name.Equals(Pair.Key, ESearchCase::CaseSensitive); });
			if (!bDeclared)
			{
				TArray<FString> Names;
				for (const FMCPParamField& Field : Fields) Names.Add(Field.Name);
				return FString::Printf(TEXT("%s.%s is not a field; %s takes %s"), *Path, *Pair.Key, *Path,
					Names.Num() > 0 ? *FString::Join(Names, TEXT(", ")) : TEXT("no other fields"));
			}
		}
		for (const FMCPParamField& Field : Fields)
		{
			const TSharedPtr<FJsonValue> Found = FindExact(*Object, Field.Name);
			const FString At = Path + TEXT(".") + Field.Name;
			if (!Found.IsValid())
			{
				if (Field.bRequired) return FString::Printf(TEXT("%s is required"), *At);
				continue;
			}
			if (FString Problem = CheckField(At, Field, Found); !Problem.IsEmpty())
			{
				return Problem;
			}
		}
		return FString();
	}

	FString CheckVariant(const FString& Path, const FMCPParamSpec& Param, const TSharedPtr<FJsonValue>& Value)
	{
		if (FString Problem = CheckBase(Path, EMCPParamType::Object, Value); !Problem.IsEmpty())
		{
			return Problem;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		const TSharedPtr<FJsonValue> Tag = FindExact(*Object, Param.VariantKey);
		TArray<FString> Tags;
		for (const FMCPParamVariant& Variant : Param.Variants)
		{
			Tags.Add(Variant.Tag);
			if (Tag.IsValid() && Tag->Type == EJson::String && Tag->AsString().Equals(Variant.Tag, ESearchCase::CaseSensitive))
			{
				return CheckFields(Path, Variant.Fields, Object, *Param.VariantKey);
			}
		}
		return FString::Printf(TEXT("%s.%s must be one of %s (got %s)"), *Path, *Param.VariantKey, *FString::Join(Tags, TEXT(", ")), *Shown(Tag));
	}

	// Type, rules, fields and variants of the parameter's own type, ignoring its alternatives.
	FString CheckPrimary(const FMCPParamSpec& Param, const TSharedPtr<FJsonValue>& Value)
	{
		const FString& Path = Param.Name;
		if (Param.Type == EMCPParamType::Array)
		{
			if (FString Problem = CheckBase(Path, EMCPParamType::Array, Value); !Problem.IsEmpty())
			{
				return Problem;
			}
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num(); Index++)
			{
				const FString At = FString::Printf(TEXT("%s[%d]"), *Path, Index);
				FString Problem;
				if (Param.Variants.Num() > 0)
				{
					FMCPParamSpec Element = Param;
					Element.Name = At;
					Problem = CheckVariant(At, Element, Items[Index]);
				}
				else
				{
					Problem = CheckBase(At, Param.ItemType, Items[Index]);
					if (Problem.IsEmpty()) Problem = CheckRules(At, Param.EnumValues, Param.Minimum, Param.Maximum, Items[Index]);
					if (Problem.IsEmpty() && Param.Fields.Num() > 0) Problem = CheckFields(At, Param.Fields, Items[Index]->AsObject());
				}
				if (!Problem.IsEmpty()) return Problem;
			}
			return FString();
		}
		if (Param.Variants.Num() > 0)
		{
			return CheckVariant(Path, Param, Value);
		}
		if (FString Problem = CheckBase(Path, Param.Type, Value); !Problem.IsEmpty())
		{
			return Problem;
		}
		if (FString Problem = CheckRules(Path, Param.EnumValues, Param.Minimum, Param.Maximum, Value); !Problem.IsEmpty())
		{
			return Problem;
		}
		return Param.Fields.Num() > 0 ? CheckFields(Path, Param.Fields, Value->AsObject()) : FString();
	}

	FString CheckParam(const FMCPParamSpec& Param, const TSharedPtr<FJsonValue>& Value)
	{
		if (Value.IsValid() && Value->Type == EJson::Null)
		{
			return Param.bNullable ? FString() : FString::Printf(TEXT("%s must not be null"), *Param.Name);
		}
		if (Param.LiteralValue.IsValid())
		{
			// CompareEqual compares strings case-insensitively; the server's literal does not.
			const bool bEqual = Param.LiteralValue->Type == EJson::String
				? Value->Type == EJson::String && Value->AsString().Equals(Param.LiteralValue->AsString(), ESearchCase::CaseSensitive)
				: FJsonValue::CompareEqual(*Param.LiteralValue, *Value);
			return bEqual
				? FString()
				: FString::Printf(TEXT("%s must be %s (got %s)"), *Param.Name, *Shown(Param.LiteralValue), *Shown(Value));
		}
		if (Param.Forms.Num() > 0)
		{
			return CheckForms(Param.Name, Param.Forms, Value);
		}
		const FString Problem = CheckPrimary(Param, Value);
		if (Problem.IsEmpty() || Param.OrTypes.Num() == 0)
		{
			return Problem;
		}
		// The value is one of the alternatives, or it is the parameter's own type and its problem stands.
		if (Param.OrTypes.ContainsByPredicate([&](EMCPParamType Type) { return CheckBase(Param.Name, Type, Value).IsEmpty(); }))
		{
			return FString();
		}
		if (CheckBase(Param.Name, Param.Type, Value).IsEmpty())
		{
			return Problem;
		}
		TArray<FString> Types = { TypeText(Param.Type) };
		for (const EMCPParamType Type : Param.OrTypes) Types.Add(TypeText(Type));
		return FString::Printf(TEXT("%s must be %s (got %s)"), *Param.Name, *FString::Join(Types, TEXT(" or ")), *Shown(Value));
	}

}

FString ContractViolation(const TArray<FMCPParamSpec>& Specs, const FMCPSpecRules& Rules, const FParams& Params)
{
	const FJsonObject Empty;
	const FJsonObject& Object = Params.IsValid() ? *Params : Empty;

	TArray<FString> Declared;
	for (const FMCPParamSpec& Param : Specs)
	{
		Declared.Add(Param.Name);
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
	{
		if (!Declared.ContainsByPredicate([&](const FString& Name) { return Name.Equals(Pair.Key, ESearchCase::CaseSensitive); }))
		{
			return FString::Printf(TEXT("does not take %s; it takes %s"), *Pair.Key,
				Declared.Num() > 0 ? *FString::Join(Declared, TEXT(", ")) : TEXT("no parameters"));
		}
	}

	for (const FMCPParamSpec& Param : Specs)
	{
		const TSharedPtr<FJsonValue> Value = FindExact(Object, Param.Name);
		if (!Value.IsValid())
		{
			if (Param.bRequired) return FString::Printf(TEXT("needs %s, and it was not given"), *Param.Name);
			continue;
		}
		if (FString Problem = CheckParam(Param, Value); !Problem.IsEmpty())
		{
			return Problem;
		}
	}

	for (const FMCPParamChoice& Choice : Rules.Choices)
	{
		const auto Given = [&](const FString& Name) { return FindExact(Object, Name).IsValid(); };
		TArray<FString> Rendered;
		TArray<const TArray<FString>*> Touched;
		for (const TArray<FString>& Branch : Choice.Branches)
		{
			Rendered.Add(FString::Join(Branch, TEXT(" + ")));
			if (Branch.ContainsByPredicate(Given)) Touched.Add(&Branch);
		}
		const FString Sides = FString::Join(Rendered, Choice.Mode == EMCPChoiceMode::ExactlyOne ? TEXT(" OR ") : TEXT(", "));
		if (Touched.Num() == 0)
		{
			return Choice.Mode == EMCPChoiceMode::ExactlyOne
				? FString::Printf(TEXT("needs %s, and none was given"), *Sides)
				: FString::Printf(TEXT("needs at least one of %s, and none was given"), *Sides);
		}
		if (Choice.Mode == EMCPChoiceMode::ExactlyOne && Touched.Num() > 1)
		{
			return FString::Printf(TEXT("takes one side of %s, not several"), *Sides);
		}
		for (const TArray<FString>* Branch : Touched)
		{
			for (const FString& Name : *Branch)
			{
				if (!Given(Name)) return FString::Printf(TEXT("%s go together, and %s was not given"), *FString::Join(*Branch, TEXT(" + ")), *Name);
			}
		}
	}
	return FString();
}
}
