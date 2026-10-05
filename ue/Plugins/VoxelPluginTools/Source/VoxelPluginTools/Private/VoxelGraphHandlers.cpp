#include "VoxelGraphHandlers.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphUtilities.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "VoxelGraph.h"
#include "VoxelTerminalGraph.h"
#include "VoxelParameter.h"
#include "VoxelPinType.h"
#include "VoxelPinValue.h"
#include "VoxelGraphTracker.h"
#include "VoxelParameterOverridesOwner.h"
#include "Buffer/VoxelBaseBuffers.h"
#include "VoxelNode.h"
#include "VoxelFunctionLibrary.h"
#include "Nodes/VoxelOutputNode.h"
#include "Nodes/VoxelNode_UFunction.h"
#include "VoxelStampComponent.h"
#include "Graphs/VoxelHeightGraphStamp.h"
#include "Graphs/VoxelVolumeGraphStamp.h"

#define LOCTEXT_NAMESPACE "VoxelPluginTools"

namespace VoxelPluginTools
{
namespace
{
	using FResult = TSharedPtr<FJsonValue>;
	using FParams = TSharedPtr<FJsonObject>;

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

	// The graph asset plus the terminal graph an edit targets.
	struct FGraphTarget
	{
		UVoxelGraph* Graph = nullptr;
		UVoxelTerminalGraph* Terminal = nullptr;
		UEdGraph* EdGraph = nullptr;
	};

	FString ResolveGraph(const FParams& Params, FGraphTarget& Out)
	{
		const FString AssetPath = Str(Params, TEXT("assetPath"));
		if (AssetPath.IsEmpty())
		{
			return TEXT("assetPath is required");
		}
		Out.Graph = LoadObject<UVoxelGraph>(nullptr, *AssetPath);
		if (!Out.Graph)
		{
			return FString::Printf(TEXT("No UVoxelGraph at '%s'"), *AssetPath);
		}

		const FString TerminalGuid = Str(Params, TEXT("terminalGraph"));
		if (TerminalGuid.IsEmpty())
		{
			if (!Out.Graph->HasMainTerminalGraph())
			{
				return TEXT("Graph has no main terminal graph; pass terminalGraph");
			}
			Out.Terminal = &Out.Graph->GetMainTerminalGraph();
		}
		else
		{
			FGuid Guid;
			if (!FGuid::Parse(TerminalGuid, Guid))
			{
				return FString::Printf(TEXT("terminalGraph '%s' is not a GUID"), *TerminalGuid);
			}
			Out.Terminal = Out.Graph->FindTerminalGraph_NoInheritance(Guid);
			if (!Out.Terminal)
			{
				return FString::Printf(TEXT("No terminal graph %s in this graph"), *TerminalGuid);
			}
		}
		Out.EdGraph = &Out.Terminal->GetEdGraph();
		return FString();
	}

	// Recompile and persist after an edit.
	void Finish(const FGraphTarget& Target, const FParams& Params, const TSharedRef<FJsonObject>& Out)
	{
		Target.EdGraph->NotifyGraphChanged();
		GVoxelGraphTracker->NotifyEdGraphChanged(*Target.EdGraph);
		Target.Graph->MarkPackageDirty();

		bool bSaved = false;
		if (Bool(Params, TEXT("save"), true))
		{
			bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Target.Graph->GetPackage() }, false);
		}
		Out->SetBoolField(TEXT("saved"), bSaved);
	}

	FString PinTypeString(const UEdGraphPin& Pin)
	{
		FString Type = Pin.PinType.PinCategory.ToString();
		if (!Pin.PinType.PinSubCategory.IsNone())
		{
			Type += TEXT(":") + Pin.PinType.PinSubCategory.ToString();
		}
		if (const UObject* Sub = Pin.PinType.PinSubCategoryObject.Get())
		{
			Type += TEXT(":") + Sub->GetName();
		}
		if (Pin.PinType.ContainerType == EPinContainerType::Array)
		{
			Type += TEXT("[]");
		}
		return Type;
	}

	TSharedRef<FJsonObject> NodeJson(const UEdGraphNode& Node, bool bPins)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("id"), Node.NodeGuid.ToString());
		J->SetStringField(TEXT("name"), Node.GetName());
		J->SetStringField(TEXT("title"), Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		J->SetStringField(TEXT("class"), Node.GetClass()->GetName());
		J->SetNumberField(TEXT("x"), Node.NodePosX);
		J->SetNumberField(TEXT("y"), Node.NodePosY);
		if (!bPins)
		{
			return J;
		}

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin || Pin->bHidden)
			{
				continue;
			}
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Pin->PinName.ToString());
			const FString Display = Pin->GetDisplayName().ToString();
			if (!Display.IsEmpty() && Display != Pin->PinName.ToString())
			{
				P->SetStringField(TEXT("displayName"), Display);
			}
			P->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
			P->SetStringField(TEXT("type"), PinTypeString(*Pin));
			if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0)
			{
				P->SetStringField(TEXT("default"), Pin->GetDefaultAsString());
			}
			TArray<TSharedPtr<FJsonValue>> Links;
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNode())
				{
					continue;
				}
				TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
				L->SetStringField(TEXT("node"), Linked->GetOwningNode()->NodeGuid.ToString());
				L->SetStringField(TEXT("pin"), Linked->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(L));
			}
			if (Links.Num() > 0)
			{
				P->SetArrayField(TEXT("links"), Links);
			}
			Pins.Add(MakeShared<FJsonValueObject>(P));
		}
		J->SetArrayField(TEXT("pins"), Pins);
		return J;
	}

	// Node by GUID, object name, or unique full title.
	UEdGraphNode* FindNode(UEdGraph& Graph, const FString& Id, FString& OutError)
	{
		if (Id.IsEmpty())
		{
			OutError = TEXT("node id is required");
			return nullptr;
		}
		FGuid Guid;
		const bool bIsGuid = FGuid::Parse(Id, Guid);
		TArray<UEdGraphNode*> ByTitle;
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if ((bIsGuid && Node->NodeGuid == Guid) || Node->GetName() == Id)
			{
				return Node;
			}
			if (Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Equals(Id, ESearchCase::IgnoreCase) ||
				Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Equals(Id, ESearchCase::IgnoreCase))
			{
				ByTitle.Add(Node);
			}
		}
		if (ByTitle.Num() == 1)
		{
			return ByTitle[0];
		}
		OutError = ByTitle.Num() > 1
			? FString::Printf(TEXT("'%s' matches %d nodes by title; use the node id"), *Id, ByTitle.Num())
			: FString::Printf(TEXT("No node '%s' in graph %s"), *Id, *Graph.GetName());
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode& Node, const FString& Name, EEdGraphPinDirection Direction, FString& OutError)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && !Pin->bHidden &&
				(Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase) ||
				 Pin->GetDisplayName().ToString().Equals(Name, ESearchCase::IgnoreCase)))
			{
				return Pin;
			}
		}
		TArray<FString> Names;
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && !Pin->bHidden)
			{
				Names.Add(Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("No %s pin '%s' on %s. %s pins: %s"),
			Direction == EGPD_Input ? TEXT("input") : TEXT("output"), *Name, *Node.GetName(),
			Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"), *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	// Voxel builds its node menu in private editor code, so the catalog is rebuilt here from the
	// same public sources its node library uses: FVoxelNode structs and UVoxelFunctionLibrary UFUNCTIONs.
	struct FNodeType
	{
		FString Key;
		FString Tooltip;
		TSharedPtr<const FVoxelNode> Node;
		FGuid ParameterGuid;
	};

	const TArray<FNodeType>& NodeCatalog()
	{
		static TArray<FNodeType> Catalog;
		if (Catalog.Num() > 0)
		{
			return Catalog;
		}
		auto Add = [&](const TSharedRef<const FVoxelNode>& Node)
		{
			const FString Category = Node->GetCategory();
			const FString Name = Node->GetDisplayName();
			Catalog.Add({ Category.IsEmpty() ? Name : Category + TEXT("|") + Name, Node->GetTooltip(), Node, FGuid() });
		};
		for (UScriptStruct* Struct : GetDerivedStructs<FVoxelNode>())
		{
			if (Struct->HasMetaData(TEXT("Abstract")) || Struct->HasMetaDataHierarchical(TEXT("Internal")))
			{
				continue;
			}
			const TSharedRef<FVoxelNode> Node = MakeSharedStruct<FVoxelNode>(Struct);
			if (Struct->IsChildOf(FVoxelOutputNode::StaticStruct()) &&
				!Node->GetMetadataContainer().HasMetaDataHierarchical(TEXT("Placeable")))
			{
				continue;
			}
			Add(Node);
		}
		for (const TSubclassOf<UVoxelFunctionLibrary>& Class : GetDerivedClasses<UVoxelFunctionLibrary>())
		{
			for (UFunction* Function : GetClassFunctions(Class))
			{
				if (Function->HasMetaData(TEXT("Internal")))
				{
					continue;
				}
				const TSharedRef<FVoxelNode_UFunction> Node = MakeShared<FVoxelNode_UFunction>();
				Node->SetFunction_EditorOnly(Function);
				Add(Node);
			}
		}
		return Catalog;
	}

	// Nodes this graph type allows, plus one getter per graph parameter.
	TArray<FNodeType> NodeTypes(const UVoxelGraph& Graph, const UVoxelTerminalGraph& Terminal)
	{
		TArray<FNodeType> Out;
		for (const FNodeType& Type : NodeCatalog())
		{
			if (Type.Node->CanPasteHere(Graph, &Terminal))
			{
				Out.Add(Type);
			}
		}
		Graph.ForeachParameter([&](const FGuid& Guid, const FVoxelParameter& Parameter)
		{
			Out.Add({ TEXT("Parameters|") + Parameter.Name.ToString(), Parameter.Description, nullptr, Guid });
		});
		return Out;
	}

	UEdGraphNode* SpawnNode(UEdGraph& Graph, const FNodeType& Type, const FVector2D& Location, FString& OutError)
	{
		const TCHAR* ClassPath = Type.ParameterGuid.IsValid()
			? TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_Parameter")
			: TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_Struct");
		UClass* NodeClass = FindObject<UClass>(nullptr, ClassPath);
		if (!NodeClass)
		{
			OutError = FString::Printf(TEXT("%s is not loaded"), ClassPath);
			return nullptr;
		}

		UEdGraphNode* Node = NewObject<UEdGraphNode>(&Graph, NodeClass, NAME_None, RF_Transactional);
		Graph.AddNode(Node, true, false);
		if (Type.ParameterGuid.IsValid())
		{
			FStructProperty* GuidProperty = CastField<FStructProperty>(NodeClass->FindPropertyByName(TEXT("Guid")));
			check(GuidProperty && GuidProperty->Struct == TBaseStructure<FGuid>::Get());
			*GuidProperty->ContainerPtrToValuePtr<FGuid>(Node) = Type.ParameterGuid;
		}
		else
		{
			FStructProperty* StructProperty = CastField<FStructProperty>(NodeClass->FindPropertyByName(TEXT("Struct")));
			check(StructProperty && StructProperty->Struct == FVoxelInstancedStruct::StaticStruct());
			StructProperty->ContainerPtrToValuePtr<FVoxelInstancedStruct>(Node)->InitializeAs(Type.Node->GetStruct(), &*Type.Node);
		}
		Node->NodePosX = static_cast<int32>(Location.X);
		Node->NodePosY = static_cast<int32>(Location.Y);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		if (Node->Pins.Num() == 0)
		{
			Node->AllocateDefaultPins();
		}
		return Node;
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

	bool FindParameter(const UVoxelGraph& Graph, const FString& Name, FGuid& OutGuid, FVoxelParameter& OutParameter)
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

	TArray<TSharedPtr<FJsonValue>> ParametersJson(const UVoxelGraph& Graph)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Graph.ForeachParameter([&](const FGuid& Guid, const FVoxelParameter& Parameter)
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Parameter.Name.ToString());
			P->SetStringField(TEXT("guid"), Guid.ToString());
			P->SetStringField(TEXT("type"), Parameter.Type.ToString());
			P->SetStringField(TEXT("category"), Parameter.Category);
			FString ValueError;
			const FVoxelPinValue Value = ConstCast(Graph).GetParameter(Parameter.Name, &ValueError);
			if (Value.IsValid())
			{
				P->SetStringField(TEXT("default"), Value.ExportToString());
			}
			Out.Add(MakeShared<FJsonValueObject>(P));
		});
		return Out;
	}

	// --------------------------------------------------------------------------------

	FResult GraphRead(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("graphClass"), Target.Graph->GetClass()->GetName());

		TArray<TSharedPtr<FJsonValue>> Terminals;
		Target.Graph->ForeachTerminalGraph_NoInheritance([&](const UVoxelTerminalGraph& Terminal)
		{
			TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("guid"), Target.Graph->FindTerminalGraphGuid_NoInheritance(&Terminal).ToString());
			T->SetStringField(TEXT("name"), Terminal.GetDisplayName());
			T->SetBoolField(TEXT("isMain"), &Terminal == Target.Terminal && Str(Params, TEXT("terminalGraph")).IsEmpty());
			Terminals.Add(MakeShared<FJsonValueObject>(T));
		});
		Out->SetArrayField(TEXT("terminalGraphs"), Terminals);
		Out->SetArrayField(TEXT("parameters"), ParametersJson(*Target.Graph));

		const bool bPins = Bool(Params, TEXT("includePins"), true);
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const UEdGraphNode* Node : Target.EdGraph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(*Node, bPins)));
			}
		}
		Out->SetArrayField(TEXT("nodes"), Nodes);
		return Ok(Out);
	}

	FResult ListNodeTypes(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		const FString Query = Str(Params, TEXT("query"));
		const int32 Limit = static_cast<int32>(Num(Params, TEXT("limit"), 100));
		TArray<FString> Words;
		Query.ParseIntoArray(Words, TEXT(" "));

		TArray<TSharedPtr<FJsonValue>> Rows;
		int32 Total = 0;
		for (const FNodeType& Type : NodeTypes(*Target.Graph, *Target.Terminal))
		{
			const FString Haystack = Type.Key + TEXT(" ") + Type.Tooltip;
			bool bMatch = true;
			for (const FString& Word : Words)
			{
				bMatch &= Haystack.Contains(Word, ESearchCase::IgnoreCase);
			}
			if (!bMatch)
			{
				continue;
			}
			Total++;
			if (Rows.Num() < Limit)
			{
				TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("action"), Type.Key);
				R->SetStringField(TEXT("tooltip"), Type.Tooltip.Left(200));
				Rows.Add(MakeShared<FJsonValueObject>(R));
			}
		}
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(TEXT("actions"), Rows);
		Out->SetNumberField(TEXT("total"), Total);
		return Ok(Out);
	}

	FResult AddNode(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		const FString Wanted = Str(Params, TEXT("action"));
		if (Wanted.IsEmpty()) return Error(TEXT("action is required (an entry from voxel_graph_list_node_types)"));

		TArray<FNodeType> Matches;
		for (const FNodeType& Type : NodeTypes(*Target.Graph, *Target.Terminal))
		{
			FString Name = Type.Key;
			Type.Key.Split(TEXT("|"), nullptr, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (Type.Key.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				Matches = { Type };
				break;
			}
			if (Name.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				Matches.Add(Type);
			}
		}
		if (Matches.Num() != 1)
		{
			TArray<FString> Keys;
			for (const FNodeType& M : Matches) Keys.Add(M.Key);
			return Error(Matches.Num() == 0
				? FString::Printf(TEXT("No node type '%s'. Search with voxel_graph_list_node_types."), *Wanted)
				: FString::Printf(TEXT("'%s' is ambiguous: %s"), *Wanted, *FString::Join(Keys, TEXT("; "))));
		}

		const FScopedTransaction Transaction(LOCTEXT("AddNode", "Add Voxel Graph Node"));
		Target.EdGraph->Modify();
		FString SpawnError;
		UEdGraphNode* Node = SpawnNode(*Target.EdGraph, Matches[0], FVector2D(Num(Params, TEXT("x"), 0), Num(Params, TEXT("y"), 0)), SpawnError);
		if (!Node) return Error(SpawnError);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("node"), NodeJson(*Node, true));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	FResult Connect(const FParams& Params, bool bConnect)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		FString Err;
		UEdGraphNode* From = FindNode(*Target.EdGraph, Str(Params, TEXT("fromNode")), Err);
		if (!From) return Error(Err);
		UEdGraphNode* To = FindNode(*Target.EdGraph, Str(Params, TEXT("toNode")), Err);
		if (!To) return Error(Err);
		UEdGraphPin* FromPin = FindPin(*From, Str(Params, TEXT("fromPin")), EGPD_Output, Err);
		if (!FromPin) return Error(Err);
		UEdGraphPin* ToPin = FindPin(*To, Str(Params, TEXT("toPin")), EGPD_Input, Err);
		if (!ToPin) return Error(Err);

		const UEdGraphSchema* Schema = Target.EdGraph->GetSchema();
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (bConnect)
		{
			if (FromPin->LinkedTo.Contains(ToPin))
			{
				Out->SetBoolField(TEXT("unchanged"), true);
				return Ok(Out);
			}
			const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				return Error(FString::Printf(TEXT("Cannot connect: %s"), *Response.Message.ToString()));
			}
			const FScopedTransaction Transaction(LOCTEXT("Connect", "Connect Voxel Graph Pins"));
			Target.EdGraph->Modify();
			if (!Schema->TryCreateConnection(FromPin, ToPin))
			{
				return Error(FString::Printf(TEXT("Connection refused: %s"), *Response.Message.ToString()));
			}
		}
		else
		{
			if (!FromPin->LinkedTo.Contains(ToPin))
			{
				Out->SetBoolField(TEXT("unchanged"), true);
				return Ok(Out);
			}
			const FScopedTransaction Transaction(LOCTEXT("Disconnect", "Disconnect Voxel Graph Pins"));
			Target.EdGraph->Modify();
			Schema->BreakSinglePinLink(FromPin, ToPin);
		}
		Out->SetObjectField(TEXT("from"), NodeJson(*From, false));
		Out->SetObjectField(TEXT("to"), NodeJson(*To, true));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	FResult SetPinDefault(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		FString Err;
		UEdGraphNode* Node = FindNode(*Target.EdGraph, Str(Params, TEXT("node")), Err);
		if (!Node) return Error(Err);
		UEdGraphPin* Pin = FindPin(*Node, Str(Params, TEXT("pin")), EGPD_Input, Err);
		if (!Pin) return Error(Err);
		if (!Params->HasField(TEXT("value"))) return Error(TEXT("value is required"));
		const FString Value = Str(Params, TEXT("value"));

		const FString Previous = Pin->GetDefaultAsString();
		const FScopedTransaction Transaction(LOCTEXT("SetDefault", "Set Voxel Pin Default"));
		Node->Modify();
		Target.EdGraph->GetSchema()->TrySetDefaultValue(*Pin, Value);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("previous"), Previous);
		Out->SetStringField(TEXT("value"), Pin->GetDefaultAsString());
		Out->SetObjectField(TEXT("node"), NodeJson(*Node, true));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	FResult DeleteNode(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		FString Err;
		UEdGraphNode* Node = FindNode(*Target.EdGraph, Str(Params, TEXT("node")), Err);
		if (!Node) return Error(Err);
		if (!Node->CanUserDeleteNode()) return Error(FString::Printf(TEXT("%s cannot be deleted"), *Node->GetName()));

		const FScopedTransaction Transaction(LOCTEXT("Delete", "Delete Voxel Graph Node"));
		Target.EdGraph->Modify();
		Node->Modify();
		Node->BreakAllNodeLinks();
		Node->DestroyNode();

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	FResult ExportT3D(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		TSet<UObject*> Nodes;
		const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
		if (Params->TryGetArrayField(TEXT("nodes"), Ids) && Ids->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& Id : *Ids)
			{
				FString Err;
				UEdGraphNode* Node = FindNode(*Target.EdGraph, Id->AsString(), Err);
				if (!Node) return Error(Err);
				Nodes.Add(Node);
			}
		}
		else
		{
			for (UEdGraphNode* Node : Target.EdGraph->Nodes)
			{
				if (Node && Node->CanDuplicateNode())
				{
					Nodes.Add(Node);
				}
			}
		}
		for (UObject* Node : Nodes)
		{
			CastChecked<UEdGraphNode>(Node)->PrepareForCopying();
		}
		FString Text;
		FEdGraphUtilities::ExportNodesToText(Nodes, Text);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("nodeCount"), Nodes.Num());
		Out->SetStringField(TEXT("t3d"), Text);
		return Ok(Out);
	}

	FResult ImportT3D(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		const FString Text = Str(Params, TEXT("t3d"));
		if (Text.IsEmpty()) return Error(TEXT("t3d is required"));
		if (!FEdGraphUtilities::CanImportNodesFromText(Target.EdGraph, Text))
		{
			return Error(TEXT("t3d is not importable into this graph (malformed, or nodes from another graph type)"));
		}

		// Voxel struct nodes refuse a paste unless the graph's editor is open (UVoxelGraphNode_Struct::CanPasteHere).
		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		const bool bWasOpen = AssetEditors->FindEditorForAsset(Target.Graph, false) != nullptr;
		if (!bWasOpen && !AssetEditors->OpenEditorForAsset(Target.Graph))
		{
			return Error(TEXT("Could not open the graph editor, which voxel nodes need in order to be pasted"));
		}
		ON_SCOPE_EXIT
		{
			if (!bWasOpen)
			{
				AssetEditors->CloseAllEditorsForAsset(Target.Graph);
			}
		};

		// Top-level objects only; nested subobjects are indented.
		int32 Expected = 0;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			Expected += Line.StartsWith(TEXT("Begin Object"), ESearchCase::CaseSensitive) ? 1 : 0;
		}

		const FScopedTransaction Transaction(LOCTEXT("Import", "Import Voxel Graph Nodes"));
		Target.EdGraph->Modify();
		TSet<UEdGraphNode*> Pasted;
		FEdGraphUtilities::ImportNodesFromText(Target.EdGraph, Text, Pasted);
		if (Pasted.Num() == 0) return Error(TEXT("Import produced no nodes"));

		const double OffsetX = Num(Params, TEXT("offsetX"), 0);
		const double OffsetY = Num(Params, TEXT("offsetY"), 0);
		// ImportNodesFromText already ran PostPasteNode and ReconstructNode.
		for (UEdGraphNode* Node : Pasted)
		{
			Node->CreateNewGuid();
			Node->NodePosX += static_cast<int32>(OffsetX);
			Node->NodePosY += static_cast<int32>(OffsetY);
		}

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const UEdGraphNode* Node : Pasted)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(*Node, false)));
		}
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(TEXT("nodes"), Nodes);
		Out->SetNumberField(TEXT("expected"), Expected);
		if (Pasted.Num() < Expected)
		{
			Out->SetStringField(TEXT("warning"), FString::Printf(
				TEXT("%d of %d nodes were refused by their CanPasteHere (not allowed in this graph type)"), Expected - Pasted.Num(), Expected));
		}
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	FResult AddParameter(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		const FString Name = Str(Params, TEXT("name"));
		if (Name.IsEmpty()) return Error(TEXT("name is required"));
		FGuid ExistingGuid;
		FVoxelParameter Existing;
		if (FindParameter(*Target.Graph, Name, ExistingGuid, Existing))
		{
			return Error(FString::Printf(TEXT("Parameter '%s' already exists (%s)"), *Name, *ExistingGuid.ToString()));
		}

		FVoxelParameter Parameter;
		FString Err;
		if (!ParsePinType(Str(Params, TEXT("type")), Parameter.Type, Err)) return Error(Err);
		Parameter.Name = FName(*Name);
		Parameter.Category = Str(Params, TEXT("category"));
		Parameter.Description = Str(Params, TEXT("description"));

		const FScopedTransaction Transaction(LOCTEXT("AddParameter", "Add Voxel Graph Parameter"));
		Target.Graph->Modify();
		const FGuid Guid = FGuid::NewGuid();
		Target.Graph->AddParameter(Guid, Parameter);

		const FString Default = Str(Params, TEXT("default"));
		if (!Default.IsEmpty())
		{
			FVoxelPinValue Value(Parameter.Type.GetExposedType());
			if (!Value.ImportFromString(Default))
			{
				return Error(FString::Printf(TEXT("Parameter added, but default '%s' does not parse as %s"), *Default, *Parameter.Type.ToString()));
			}
			FString SetError;
			if (!Target.Graph->SetParameter(Parameter.Name, Value, &SetError))
			{
				return Error(FString::Printf(TEXT("Parameter added, but setting its default failed: %s"), *SetError));
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("guid"), Guid.ToString());
		Out->SetArrayField(TEXT("parameters"), ParametersJson(*Target.Graph));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	FResult SetParameterDefault(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target); !Err.IsEmpty()) return Error(Err);

		const FString Name = Str(Params, TEXT("name"));
		FGuid Guid;
		FVoxelParameter Parameter;
		if (!FindParameter(*Target.Graph, Name, Guid, Parameter)) return Error(FString::Printf(TEXT("No parameter '%s'"), *Name));

		FVoxelPinValue Value(Parameter.Type.GetExposedType());
		if (!Value.ImportFromString(Str(Params, TEXT("value"))))
		{
			return Error(FString::Printf(TEXT("'%s' does not parse as %s"), *Str(Params, TEXT("value")), *Parameter.Type.ToString()));
		}
		const FScopedTransaction Transaction(LOCTEXT("SetParameterDefault", "Set Voxel Parameter Default"));
		Target.Graph->Modify();
		FString SetError;
		if (!Target.Graph->SetParameter(Parameter.Name, Value, &SetError)) return Error(SetError);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(TEXT("parameters"), ParametersJson(*Target.Graph));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	// --------------------------------------------------------------------------------

	UVoxelStampComponent* FindStampComponent(const FParams& Params, FString& OutError)
	{
		// Stamp actors relabel themselves from their stamp, so the path is the stable key.
		const FString Path = Str(Params, TEXT("actorPath"));
		const FString Label = Str(Params, TEXT("actorLabel"));
		if ((Path.IsEmpty() && Label.IsEmpty()) || !GEditor)
		{
			OutError = TEXT("actorPath or actorLabel is required");
			return nullptr;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (Path.IsEmpty() ? It->GetActorLabel() == Label : It->GetPathName() == Path)
			{
				if (UVoxelStampComponent* Component = It->FindComponentByClass<UVoxelStampComponent>())
				{
					return Component;
				}
				OutError = FString::Printf(TEXT("Actor %s has no voxel stamp component"), *It->GetPathName());
				return nullptr;
			}
		}
		OutError = FString::Printf(TEXT("No actor %s"), Path.IsEmpty() ? *Label : *Path);
		return nullptr;
	}

	template<typename StampType>
	FResult SetStampParameters(UVoxelStampComponent& Component, const StampType& Current, const FParams& Params)
	{
		StampType Stamp = Current;
		UVoxelGraph* Graph = Stamp.GetGraph();
		if (!Graph) return Error(TEXT("The stamp has no graph assigned"));

		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (!Params->TryGetObjectField(TEXT("values"), Values) || (*Values)->Values.Num() == 0)
		{
			return Error(TEXT("values is required: { parameterName: valueText, ... }"));
		}

		IVoxelParameterOverridesOwner& Owner = Stamp;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
		{
			FGuid Guid;
			FVoxelParameter Parameter;
			if (!FindParameter(*Graph, Pair.Key, Guid, Parameter))
			{
				return Error(FString::Printf(TEXT("Graph %s has no parameter '%s'"), *Graph->GetName(), *Pair.Key));
			}
			FString Text;
			if (!Pair.Value->TryGetString(Text))
			{
				double Number = 0;
				bool bBool = false;
				if (Pair.Value->TryGetNumber(Number)) Text = FString::SanitizeFloat(Number);
				else if (Pair.Value->TryGetBool(bBool)) Text = bBool ? TEXT("true") : TEXT("false");
			}
			FVoxelPinValue Value(Parameter.Type.GetExposedType());
			if (!Value.ImportFromString(Text))
			{
				return Error(FString::Printf(TEXT("'%s' does not parse as %s for parameter '%s'"), *Text, *Parameter.Type.ToString(), *Pair.Key));
			}
			FVoxelParameterValueOverride& Override = Owner.GetGuidToValueOverride().FindOrAdd(Guid);
			Override.bEnable = true;
			Override.Value = Value;
			Override.CachedName = Parameter.Name;
		}

		const FScopedTransaction Transaction(LOCTEXT("SetStampParameters", "Set Voxel Stamp Parameters"));
		Component.Modify();
		Component.SetStamp(Stamp);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Applied = MakeShared<FJsonObject>();
		for (const TPair<FGuid, FVoxelParameterValueOverride>& Pair : Owner.GetGuidToValueOverride())
		{
			if (Pair.Value.bEnable)
			{
				Applied->SetStringField(Pair.Value.CachedName.ToString(), Pair.Value.Value.ExportToString());
			}
		}
		Out->SetObjectField(TEXT("overrides"), Applied);
		return Ok(Out);
	}

	FResult StampSetParameters(const FParams& Params)
	{
		FString Err;
		UVoxelStampComponent* Component = FindStampComponent(Params, Err);
		if (!Component) return Error(Err);

		const FVoxelStampRef Ref = Component->GetStamp();
		if (const FVoxelHeightGraphStamp* Height = Ref.As<FVoxelHeightGraphStamp>())
		{
			return SetStampParameters(*Component, *Height, Params);
		}
		if (const FVoxelVolumeGraphStamp* Volume = Ref.As<FVoxelVolumeGraphStamp>())
		{
			return SetStampParameters(*Component, *Volume, Params);
		}
		return Error(TEXT("The actor's stamp is not a height or volume graph stamp"));
	}
}

const TArray<FHandlerEntry>& GetHandlers()
{
	static const TArray<FHandlerEntry> Handlers =
	{
		{ TEXT("voxel_graph_read"), &GraphRead },
		{ TEXT("voxel_graph_list_node_types"), &ListNodeTypes },
		{ TEXT("voxel_graph_add_node"), &AddNode },
		{ TEXT("voxel_graph_connect"), [](const FParams& P) { return Connect(P, true); } },
		{ TEXT("voxel_graph_disconnect"), [](const FParams& P) { return Connect(P, false); } },
		{ TEXT("voxel_graph_set_pin_default"), &SetPinDefault },
		{ TEXT("voxel_graph_delete_node"), &DeleteNode },
		{ TEXT("voxel_graph_export_t3d"), &ExportT3D },
		{ TEXT("voxel_graph_import_t3d"), &ImportT3D },
		{ TEXT("voxel_graph_add_parameter"), &AddParameter },
		{ TEXT("voxel_graph_set_parameter_default"), &SetParameterDefault },
		{ TEXT("voxel_stamp_set_parameters"), &StampSetParameters },
	};
	return Handlers;
}
}

#undef LOCTEXT_NAMESPACE
