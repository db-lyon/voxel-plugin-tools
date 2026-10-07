#include "VoxelToolsCommon.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphUtilities.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectIterator.h"
#include <type_traits>

#include "VoxelGraph.h"
#include "VoxelTerminalGraph.h"
#include "VoxelParameter.h"
#include "VoxelPinType.h"
#include "VoxelPinTypeSet.h"
#include "VoxelPinValue.h"
#include "VoxelGraphTracker.h"
#include "VoxelParameterOverridesOwner.h"
#include "Buffer/VoxelBaseBuffers.h"
#include "VoxelNode.h"
#include "VoxelFunctionLibrary.h"
#include "VoxelFunctionLibraryAsset.h"
#include "AssetRegistry/IAssetRegistry.h"
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
	// The graph asset plus the terminal graph an edit targets.
	struct FGraphTarget
	{
		UVoxelGraph* Graph = nullptr;
		UVoxelTerminalGraph* Terminal = nullptr;
		UEdGraph* EdGraph = nullptr;
	};

	// Edit refuses a terminal graph an instance inherits: the edit would land in, and save, its base graph.
	enum class EGraphAccess { Read, Edit };

	// A graph asset, or the graph a function library asset holds. Accepts a bare package path (/Game/A/B), as every
	// other handler's asset path does.
	UVoxelGraph* LoadGraph(const FParams& Params, FString& OutError)
	{
		const FString Path = Str(Params, TEXT("assetPath"));
		// Name the object: a bare package path loaded as UObject could resolve to the package itself.
		const FString ObjectPath = Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("."))
			? Path + TEXT(".") + FPackageName::GetShortName(Path)
			: Path;
		UObject* Asset = Load<UObject>(ObjectPath, OutError);
		if (UVoxelFunctionLibraryAsset* Library = Cast<UVoxelFunctionLibraryAsset>(Asset))
		{
			return &Library->GetGraph();
		}
		if (UVoxelGraph* Graph = Cast<UVoxelGraph>(Asset))
		{
			return Graph;
		}
		if (Asset)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a UVoxelGraph or UVoxelFunctionLibraryAsset"), *Path, *Asset->GetClass()->GetName());
		}
		return nullptr;
	}

	FString ResolveGraph(const FParams& Params, FGraphTarget& Out, EGraphAccess Access)
	{
		FString Err;
		Out.Graph = LoadGraph(Params, Err);
		if (!Out.Graph)
		{
			return Err;
		}

		const FString TerminalGuid = Str(Params, TEXT("terminalGraph"));
		if (TerminalGuid.IsEmpty() && Has(Params, TEXT("terminalGraph")))
		{
			return TEXT("terminalGraph must not be empty; omit it for the main terminal graph");
		}
		if (TerminalGuid.IsEmpty())
		{
			Out.Terminal = Out.Graph->IsFunctionLibrary() ? nullptr : Out.Graph->GetMainTerminalGraph_CheckBaseGraphs();
			if (!Out.Terminal)
			{
				return TEXT("Graph has no main terminal graph; pass terminalGraph");
			}
		}
		else
		{
			FGuid Guid;
			if (!FGuid::Parse(TerminalGuid, Guid))
			{
				return FString::Printf(TEXT("terminalGraph '%s' is not a GUID"), *TerminalGuid);
			}
			Out.Terminal = Out.Graph->FindTerminalGraph(Guid);
			if (!Out.Terminal)
			{
				return FString::Printf(TEXT("No terminal graph %s in this graph"), *TerminalGuid);
			}
		}
		if (Access == EGraphAccess::Edit && &Out.Terminal->GetGraph() != Out.Graph)
		{
			return FString::Printf(TEXT("%s is an instance of %s and inherits this terminal graph; edit %s, or override parameters with voxel_graph_set_parameter_default"),
				*Out.Graph->GetPathName(), *Out.Terminal->GetGraph().GetPathName(), *Out.Terminal->GetGraph().GetPathName());
		}
		Out.EdGraph = &Out.Terminal->GetEdGraph();
		return FString();
	}

	// Recompile and persist after an edit.
	// EdGraph is null when the edit removed the terminal graph that held it.
	void Finish(const FGraphTarget& Target, const FParams& Params, const TSharedRef<FJsonObject>& Out)
	{
		if (Target.EdGraph)
		{
			Target.EdGraph->NotifyGraphChanged();
			GVoxelGraphTracker->NotifyEdGraphChanged(*Target.EdGraph);
		}
		// Voxel rebuilds the saved compiled graph on its next tick; flush now or the save below stores the pre-edit one.
		GVoxelGraphTracker->Flush();
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
	// same public sources it uses: FVoxelNode structs, UVoxelFunctionLibrary UFUNCTIONs,
	// function-library assets, and the graph's own parameters.
	struct FNodeType
	{
		FString Key;
		FString Tooltip;
		TSharedPtr<const FVoxelNode> Node;
		FGuid Guid;
		UVoxelFunctionLibraryAsset* FunctionLibrary = nullptr;
		// The node class a Guid names when it is not a parameter getter: a function input or output node.
		const TCHAR* GuidNodeClass = nullptr;
	};

	const TCHAR* FunctionInputNodeClass = TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_FunctionInput");
	const TCHAR* FunctionOutputNodeClass = TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_FunctionOutput");

	// Released in ReleaseCatalog() at module shutdown: FVoxelNode instances must not outlive Voxel's leak check.
	TArray<FNodeType> Catalog;

	const TArray<FNodeType>& NodeCatalog()
	{
		if (Catalog.Num() > 0)
		{
			return Catalog;
		}
		auto Add = [&](const TSharedRef<const FVoxelNode>& Node)
		{
			const FString Category = Node->GetCategory();
			const FString Name = Node->GetDisplayName();
			Catalog.Add({ Category.IsEmpty() ? Name : Category + TEXT("|") + Name, Node->GetTooltip(), Node });
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

	// Nodes this graph type allows: library nodes, exposed function-library-asset functions, parameter getters.
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

		TArray<FAssetData> Libraries;
		IAssetRegistry::GetChecked().GetAssetsByClass(UVoxelFunctionLibraryAsset::StaticClass()->GetClassPathName(), Libraries);
		for (const FAssetData& AssetData : Libraries)
		{
			UVoxelFunctionLibraryAsset* Library = Cast<UVoxelFunctionLibraryAsset>(AssetData.GetAsset());
			if (!Library)
			{
				continue;
			}
			for (const FGuid& Guid : Library->GetGraph().GetTerminalGraphs())
			{
				const UVoxelTerminalGraph* Function = Library->GetGraph().FindTerminalGraph(Guid);
				if (!Function ||
					Function->IsMainTerminalGraph() ||
					Function->IsEditorTerminalGraph() ||
					!Function->bExposeToLibrary ||
					!Function->CanBePlaced(Graph))
				{
					continue;
				}
				const FVoxelGraphMetadata Metadata = Function->GetMetadata();
				Out.Add({
					Metadata.Category.IsEmpty() ? Metadata.DisplayName : Metadata.Category + TEXT("|") + Metadata.DisplayName,
					Metadata.Description,
					nullptr,
					Guid,
					Library });
			}
		}

		Graph.ForeachParameter([&](const FGuid& Guid, const FVoxelParameter& Parameter)
		{
			Out.Add({ TEXT("Parameters|") + Parameter.Name.ToString(), Parameter.Description, nullptr, Guid });
		});

		// A function's own inputs and outputs, named as Voxel's context menu names them.
		if (Terminal.IsFunction())
		{
			for (const FGuid& Guid : Terminal.GetFunctionInputs())
			{
				const FVoxelGraphFunctionInput& Input = Terminal.FindInputChecked(Guid);
				Out.Add({ TEXT("Function Inputs|Get ") + Input.Name.ToString(), Input.Description, nullptr, Guid, nullptr, FunctionInputNodeClass });
			}
			for (const FGuid& Guid : Terminal.GetFunctionOutputs())
			{
				const FVoxelGraphFunctionOutput& Output = Terminal.FindOutputChecked(Guid);
				Out.Add({ TEXT("Function Outputs|Set ") + Output.Name.ToString(), Output.Description, nullptr, Guid, nullptr, FunctionOutputNodeClass });
			}
		}
		return Out;
	}

	UEdGraphNode* SpawnNode(UEdGraph& Graph, const FNodeType& Type, const FVector2D& Location, FString& OutError)
	{
		const TCHAR* ClassPath =
			Type.GuidNodeClass ? Type.GuidNodeClass :
			Type.FunctionLibrary ? TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_CallExternalFunction") :
			Type.Guid.IsValid() ? TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_Parameter") :
			TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_Struct");
		UClass* NodeClass = FindObject<UClass>(nullptr, ClassPath);
		if (!NodeClass)
		{
			OutError = FString::Printf(TEXT("%s is not loaded"), ClassPath);
			return nullptr;
		}

		// Validate every reflected property before creating anything; a Voxel update may rename them.
		FStructProperty* GuidProperty = CastField<FStructProperty>(NodeClass->FindPropertyByName(TEXT("Guid")));
		FObjectPropertyBase* LibraryProperty = CastField<FObjectPropertyBase>(NodeClass->FindPropertyByName(TEXT("FunctionLibrary")));
		FStructProperty* StructProperty = CastField<FStructProperty>(NodeClass->FindPropertyByName(TEXT("Struct")));
		const bool bValid = Type.Guid.IsValid()
			? GuidProperty && GuidProperty->Struct == TBaseStructure<FGuid>::Get() && (!Type.FunctionLibrary || LibraryProperty)
			: StructProperty && StructProperty->Struct == FVoxelInstancedStruct::StaticStruct();
		if (!bValid)
		{
			OutError = FString::Printf(TEXT("%s no longer has the properties this tool sets (Guid/FunctionLibrary/Struct); the Voxel version is unsupported"), ClassPath);
			return nullptr;
		}

		UEdGraphNode* Node = NewObject<UEdGraphNode>(&Graph, NodeClass, NAME_None, RF_Transactional);
		Graph.AddNode(Node, true, false);
		if (Type.Guid.IsValid())
		{
			*GuidProperty->ContainerPtrToValuePtr<FGuid>(Node) = Type.Guid;
			if (Type.FunctionLibrary)
			{
				LibraryProperty->SetObjectPropertyValue_InContainer(Node, Type.FunctionLibrary);
			}
		}
		else
		{
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

	// A function's own members: the terminal graph is neither the main graph nor the editor graph.
	bool IsFunctionMember(const UVoxelTerminalGraph& Terminal)
	{
		return !Terminal.IsMainTerminalGraph() && !Terminal.IsEditorTerminalGraph();
	}

	TSharedRef<FJsonObject> TerminalJson(const UVoxelTerminalGraph& Terminal)
	{
		TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("guid"), Terminal.GetGuid().ToString());
		T->SetStringField(TEXT("name"), Terminal.GetDisplayName());
		T->SetBoolField(TEXT("isMain"), Terminal.IsMainTerminalGraph());
		if (!IsFunctionMember(Terminal))
		{
			return T;
		}
		const FVoxelGraphMetadata Metadata = Terminal.GetMetadata();
		T->SetStringField(TEXT("category"), Metadata.Category);
		T->SetStringField(TEXT("description"), Metadata.Description);
		// Inherited: an override of a base graph's function, whose name and category the base owns.
		T->SetBoolField(TEXT("inherited"), !Terminal.IsTopmostTerminalGraph());
		if (Terminal.GetGraph().IsFunctionLibrary())
		{
			T->SetBoolField(TEXT("exposeToLibrary"), Terminal.bExposeToLibrary);
		}

		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (const FGuid& Guid : Terminal.GetFunctionInputs())
		{
			const FVoxelGraphFunctionInput& Input = Terminal.FindInputChecked(Guid);
			TSharedRef<FJsonObject> I = MakeShared<FJsonObject>();
			I->SetStringField(TEXT("guid"), Guid.ToString());
			I->SetStringField(TEXT("name"), Input.Name.ToString());
			I->SetStringField(TEXT("type"), Input.Type.ToString());
			if (!Input.bNoDefault && Input.DefaultPinValue.IsValid())
			{
				I->SetStringField(TEXT("default"), Input.DefaultPinValue.ExportToString());
			}
			Inputs.Add(MakeShared<FJsonValueObject>(I));
		}
		T->SetArrayField(TEXT("inputs"), Inputs);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (const FGuid& Guid : Terminal.GetFunctionOutputs())
		{
			const FVoxelGraphFunctionOutput& Output = Terminal.FindOutputChecked(Guid);
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("guid"), Guid.ToString());
			O->SetStringField(TEXT("name"), Output.Name.ToString());
			O->SetStringField(TEXT("type"), Output.Type.ToString());
			Outputs.Add(MakeShared<FJsonValueObject>(O));
		}
		T->SetArrayField(TEXT("outputs"), Outputs);
		return T;
	}

	// --------------------------------------------------------------------------------

	FResult GraphRead(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Read); !Err.IsEmpty()) return Error(Err);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("graphClass"), Target.Graph->GetClass()->GetName());
		if (const UVoxelGraph* Base = Target.Graph->GetBaseGraph_Unsafe())
		{
			Out->SetStringField(TEXT("baseGraph"), Base->GetPathName());
		}
		// The terminal graph nodes is read from; another graph's when it is inherited.
		Out->SetStringField(TEXT("nodesFrom"), Target.Terminal->GetGraph().GetPathName());

		TArray<TSharedPtr<FJsonValue>> Terminals;
		Target.Graph->ForeachTerminalGraph_NoInheritance([&](const UVoxelTerminalGraph& Terminal)
		{
			TSharedRef<FJsonObject> T = TerminalJson(Terminal);
			T->SetBoolField(TEXT("isTarget"), &Terminal == Target.Terminal);
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
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Read); !Err.IsEmpty()) return Error(Err);

		const FString Query = Str(Params, TEXT("query"));
		const int32 Limit = static_cast<int32>(Num(Params, TEXT("limit"), 100));
		if (Limit < 1) return Error(TEXT("limit must be an integer >= 1"));
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
				R->SetStringField(TEXT("nodeType"), Type.Key);
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
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Edit); !Err.IsEmpty()) return Error(Err);

		const FString Wanted = Str(Params, TEXT("nodeType"));
		if (Wanted.IsEmpty()) return Error(TEXT("nodeType is required (a key from voxel_graph_list_node_types)"));

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
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Edit); !Err.IsEmpty()) return Error(Err);

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
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Edit); !Err.IsEmpty()) return Error(Err);

		FString Err;
		UEdGraphNode* Node = FindNode(*Target.EdGraph, Str(Params, TEXT("node")), Err);
		if (!Node) return Error(Err);
		UEdGraphPin* Pin = FindPin(*Node, Str(Params, TEXT("pin")), EGPD_Input, Err);
		if (!Pin) return Error(Err);
		FString Text;
		if (!ScalarField(Params, TEXT("value"), Text)) return Error(TEXT("value is required: a string, number or boolean"));
		if (Pin->LinkedTo.Num() > 0) return Error(FString::Printf(TEXT("Pin %s is connected; its default is unused"), *Pin->PinName.ToString()));

		// Parse as Voxel does, so bad text is rejected and object pins land in DefaultObject.
		const FVoxelPinType Type = FVoxelPinType(Pin->PinType).GetPinDefaultValueType();
		if (!Type.IsValid() || Type.IsBuffer() || !Type.HasPinDefaultValue())
		{
			return Error(FString::Printf(TEXT("Pin %s takes no default value"), *Pin->PinName.ToString()));
		}
		FVoxelPinValue Value(Type);
		if (!ParseValue(Value, Text))
		{
			return Error(FString::Printf(TEXT("'%s' does not parse as %s"), *Text, *Type.ToString()));
		}

		const FString Previous = Pin->GetDefaultAsString();
		const FScopedTransaction Transaction(LOCTEXT("SetDefault", "Set Voxel Pin Default"));
		Node->Modify();
		Value.ApplyToPinDefaultValue(*Pin);
		Node->PinDefaultValueChanged(Pin);

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
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Edit); !Err.IsEmpty()) return Error(Err);

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
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Read); !Err.IsEmpty()) return Error(Err);

		TSet<UObject*> Nodes;
		const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
		if (Has(Params, TEXT("nodes")))
		{
			if (!Params->TryGetArrayField(TEXT("nodes"), Ids) || Ids->Num() == 0)
			{
				return Error(TEXT("nodes must be a non-empty array of node ids; omit it to export every copyable node"));
			}
			for (const TSharedPtr<FJsonValue>& Id : *Ids)
			{
				if (!Id.IsValid() || Id->Type != EJson::String) return Error(TEXT("nodes must contain only node id strings"));
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
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Edit); !Err.IsEmpty()) return Error(Err);

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
		// Mirror FVoxelGraphCommandManager::PasteNodes: the engine checks CanDuplicateNode only on the
		// class default, so per-instance refusals (output nodes) are dropped here.
		for (UEdGraphNode* Node : TSet<UEdGraphNode*>(Pasted))
		{
			if (!Node->CanDuplicateNode())
			{
				Node->DestroyNode();
				Pasted.Remove(Node);
			}
		}
		if (Pasted.Num() == 0) return Error(TEXT("Import produced no nodes"));

		const double OffsetX = Num(Params, TEXT("offsetX"), 0);
		const double OffsetY = Num(Params, TEXT("offsetY"), 0);
		// ImportNodesFromText already ran PostPasteNode and ReconstructNode.
		for (UEdGraphNode* Node : Pasted)
		{
			Node->CreateNewGuid();
			Node->NodePosX += static_cast<int32>(OffsetX);
			Node->NodePosY += static_cast<int32>(OffsetY);
			if (FBoolProperty* Preview = CastField<FBoolProperty>(Node->GetClass()->FindPropertyByName(TEXT("bEnablePreview"))))
			{
				Preview->SetPropertyValue_InContainer(Node, false);
			}
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
				TEXT("%d of %d nodes were refused (not allowed in this graph type, or not duplicable such as output nodes)"), Expected - Pasted.Num(), Expected));
		}
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	// Writes a graph parameter default the way the Voxel editor does, so listeners see the change.
	bool WriteParameterDefault(UVoxelGraph& Graph, const FName Name, const FVoxelPinValue& Value, FString& OutError)
	{
		Graph.PreEditChange(Graph.GetParameterOverridesProperty());
		const bool bSet = Graph.SetParameter(Name, Value, &OutError);
		FPropertyChangedEvent Event(Graph.GetParameterOverridesProperty());
		Graph.PostEditChangeProperty(Event);
		return bSet;
	}

	FResult AddParameter(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Edit); !Err.IsEmpty()) return Error(Err);

		const FString Name = Str(Params, TEXT("name"));
		if (Name.IsEmpty()) return Error(TEXT("name is required"));
		FGuid ExistingGuid;
		FVoxelParameter Existing;
		if (FindParameter(*Target.Graph, Name, ExistingGuid, Existing))
		{
			return Error(FString::Printf(TEXT("Parameter '%s' already exists (%s)"), *Name, *ExistingGuid.ToString()));
		}

		// Validate type and default before touching the graph.
		FVoxelParameter Parameter;
		FString Err;
		if (!ParsePinType(Str(Params, TEXT("type")), Parameter.Type, Err)) return Error(Err);
		Parameter.Name = FName(*Name);
		Parameter.Category = Str(Params, TEXT("category"));
		Parameter.Description = Str(Params, TEXT("description"));
		Parameter.Fixup();
		if (!FVoxelPinTypeSet::AllParameters().Contains(Parameter.Type))
		{
			return Error(FString::Printf(TEXT("%s is not a valid voxel parameter type"), *Parameter.Type.ToString()));
		}

		FString Default;
		if (Has(Params, TEXT("default")) && (!ScalarField(Params, TEXT("default"), Default) || Default.IsEmpty()))
		{
			return Error(TEXT("default must be a non-empty string, a number or a boolean; omit it for the type's default"));
		}
		FVoxelPinValue Value(Parameter.Type.GetExposedType());
		if (!Default.IsEmpty() && !ParseValue(Value, Default))
		{
			return Error(FString::Printf(TEXT("Default '%s' does not parse as %s"), *Default, *Parameter.Type.ToString()));
		}

		const FScopedTransaction Transaction(LOCTEXT("AddParameter", "Add Voxel Graph Parameter"));
		Target.Graph->Modify();
		const FGuid Guid = FGuid::NewGuid();
		Target.Graph->AddParameter(Guid, Parameter);
		Target.Graph->Fixup();

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		FString SetError;
		if (!Default.IsEmpty() && !WriteParameterDefault(*Target.Graph, Parameter.Name, Value, SetError))
		{
			Out->SetStringField(TEXT("warning"), FString::Printf(TEXT("Parameter added; its default was not applied: %s"), *SetError));
		}
		Out->SetStringField(TEXT("guid"), Guid.ToString());
		Out->SetArrayField(TEXT("parameters"), ParametersJson(*Target.Graph));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	// Mirrors the Voxel members panel: delete usage nodes in every terminal graph, then the parameter.
	FResult RemoveParameter(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Edit); !Err.IsEmpty()) return Error(Err);

		const FString Name = Str(Params, TEXT("name"));
		FGuid Guid;
		FVoxelParameter Parameter;
		if (!FindParameter(*Target.Graph, Name, Guid, Parameter)) return Error(FString::Printf(TEXT("No parameter '%s'"), *Name));
		if (Target.Graph->IsInheritedParameter(Guid)) return Error(FString::Printf(TEXT("'%s' is inherited from a base graph"), *Name));

		UClass* ParameterNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_Parameter"));
		FStructProperty* GuidProperty = ParameterNodeClass ? CastField<FStructProperty>(ParameterNodeClass->FindPropertyByName(TEXT("Guid"))) : nullptr;
		if (!GuidProperty) return Error(TEXT("VoxelGraphNode_Parameter.Guid not found; the Voxel version is unsupported"));

		const FScopedTransaction Transaction(LOCTEXT("RemoveParameter", "Remove Voxel Graph Parameter"));
		Target.Graph->Modify();
		int32 RemovedUsages = 0;
		Target.Graph->ForeachTerminalGraph_NoInheritance([&](UVoxelTerminalGraph& Terminal)
		{
			UEdGraph& EdGraph = Terminal.GetEdGraph();
			for (UEdGraphNode* Node : TArray<UEdGraphNode*>(EdGraph.Nodes))
			{
				if (Node && Node->IsA(ParameterNodeClass) && *GuidProperty->ContainerPtrToValuePtr<FGuid>(Node) == Guid)
				{
					EdGraph.Modify();
					Node->Modify();
					Node->BreakAllNodeLinks();
					EdGraph.RemoveNode(Node);
					RemovedUsages++;
				}
			}
		});
		Target.Graph->RemoveParameter(Guid);
		Target.Graph->GetGuidToValueOverride().Remove(Guid);
		Target.Graph->Fixup();

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("removedUsages"), RemovedUsages);
		Out->SetArrayField(TEXT("parameters"), ParametersJson(*Target.Graph));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	FResult SetParameterDefault(const FParams& Params)
	{
		FGraphTarget Target;
		if (const FString Err = ResolveGraph(Params, Target, EGraphAccess::Read); !Err.IsEmpty()) return Error(Err);

		const FString Name = Str(Params, TEXT("name"));
		FGuid Guid;
		FVoxelParameter Parameter;
		if (!FindParameter(*Target.Graph, Name, Guid, Parameter)) return Error(FString::Printf(TEXT("No parameter '%s'"), *Name));
		FString Text;
		if (!ScalarField(Params, TEXT("value"), Text)) return Error(TEXT("value is required: a string, number or boolean"));

		FVoxelPinValue Value(Parameter.Type.GetExposedType());
		if (!ParseValue(Value, Text))
		{
			return Error(FString::Printf(TEXT("'%s' does not parse as %s"), *Text, *Parameter.Type.ToString()));
		}
		const FScopedTransaction Transaction(LOCTEXT("SetParameterDefault", "Set Voxel Parameter Default"));
		Target.Graph->Modify();
		FString SetError;
		if (!WriteParameterDefault(*Target.Graph, Parameter.Name, Value, SetError)) return Error(SetError);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(TEXT("parameters"), ParametersJson(*Target.Graph));
		Finish(Target, Params, Out);
		return Ok(Out);
	}

	// --------------------------------------------------------------------------------
	// Functions: terminal graphs other than the main and editor graphs, as Voxel's members panel creates them.

	// ParsePinType's spellings, plus Voxel's own ' buffer' and ' array' suffixes (FVoxelPinType::ToString).
	bool ParseMemberType(const FString& In, FVoxelPinType& Out, FString& OutError)
	{
		FString Base = In.TrimStartAndEnd();
		const bool bBuffer = Base.EndsWith(TEXT(" buffer"), ESearchCase::IgnoreCase);
		const bool bArray = Base.EndsWith(TEXT(" array"), ESearchCase::IgnoreCase);
		Base.LeftChopInline(bBuffer ? 7 : bArray ? 6 : 0);
		if (!ParsePinType(Base, Out, OutError))
		{
			return false;
		}
		if (bBuffer || bArray)
		{
			Out = Out.GetBufferType().WithBufferArray(bArray);
		}
		if (!Out.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid voxel pin type"), *In);
			return false;
		}
		return true;
	}

	// The function named Name in Graph (its own or inherited), other than Except; case-insensitive.
	const UVoxelTerminalGraph* FindFunctionByName(const UVoxelGraph& Graph, const FString& Name, const UVoxelTerminalGraph* Except)
	{
		for (const FGuid& Guid : Graph.GetTerminalGraphs())
		{
			const UVoxelTerminalGraph* Terminal = Graph.FindTerminalGraph(Guid);
			if (Terminal && IsFunctionMember(*Terminal) && Terminal->GetGuid() != (Except ? Except->GetGuid() : FGuid()) &&
				Terminal->GetDisplayName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Terminal;
			}
		}
		return nullptr;
	}

	// The function terminalGraph names, defined or overridden in this graph (not only inherited).
	UVoxelTerminalGraph* ResolveFunction(UVoxelGraph& Graph, const FParams& Params, FString& OutError)
	{
		FGuid Guid;
		const FString Text = Str(Params, TEXT("terminalGraph"));
		if (!FGuid::Parse(Text, Guid))
		{
			OutError = FString::Printf(TEXT("terminalGraph '%s' is not a GUID"), *Text);
			return nullptr;
		}
		UVoxelTerminalGraph* Terminal = Graph.FindTerminalGraph_NoInheritance(Guid);
		if (!Terminal)
		{
			OutError = Graph.FindTerminalGraph(Guid)
				? FString::Printf(TEXT("%s inherits function %s from its base graph; edit the base"), *Graph.GetPathName(), *Text)
				: FString::Printf(TEXT("No terminal graph %s in this graph"), *Text);
			return nullptr;
		}
		if (!IsFunctionMember(*Terminal))
		{
			OutError = FString::Printf(TEXT("%s is the %s graph, not a function"), *Text, Terminal->IsMainTerminalGraph() ? TEXT("main") : TEXT("editor"));
			return nullptr;
		}
		return Terminal;
	}

	// Validates the metadata fields add and set share, before anything changes.
	FString CheckFunctionFields(const UVoxelGraph& Graph, const FParams& Params, const UVoxelTerminalGraph* Self)
	{
		if (Has(Params, TEXT("name")))
		{
			const FString Name = Str(Params, TEXT("name")).TrimStartAndEnd();
			if (Name.IsEmpty())
			{
				return TEXT("name must not be empty");
			}
			if (const UVoxelTerminalGraph* Existing = FindFunctionByName(Graph, Name, Self))
			{
				return FString::Printf(TEXT("%s already has a function named '%s' (%s)"), *Graph.GetPathName(), *Existing->GetDisplayName(), *Existing->GetGuid().ToString());
			}
		}
		if (Has(Params, TEXT("exposeToLibrary")) && !Graph.IsFunctionLibrary())
		{
			return TEXT("exposeToLibrary only applies to a function library's functions");
		}
		return FString();
	}

	void ApplyFunctionFields(UVoxelTerminalGraph& Terminal, const FParams& Params)
	{
		Terminal.UpdateMetadata([&](FVoxelGraphMetadata& Metadata)
		{
			if (Has(Params, TEXT("name"))) Metadata.DisplayName = Str(Params, TEXT("name")).TrimStartAndEnd();
			if (Has(Params, TEXT("category"))) Metadata.Category = Str(Params, TEXT("category"));
			if (Has(Params, TEXT("description"))) Metadata.Description = Str(Params, TEXT("description"));
		});
		if (Has(Params, TEXT("exposeToLibrary")))
		{
			Terminal.bExposeToLibrary = Bool(Params, TEXT("exposeToLibrary"), true);
		}
	}

	// inputs or outputs: [{ name, type, category?, description?, default? (inputs only) }], names unique in the list.
	template<typename MemberType>
	FString ParseMembers(const FParams& Params, const TCHAR* Field, TArray<TPair<FGuid, MemberType>>& Out)
	{
		constexpr bool bInput = std::is_same_v<MemberType, FVoxelGraphFunctionInput>;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Has(Params, Field))
		{
			return FString();
		}
		if (!Params->TryGetArrayField(Field, Items))
		{
			return FString::Printf(TEXT("%s must be an array"), Field);
		}
		for (int32 Index = 0; Index < Items->Num(); Index++)
		{
			const FString Where = FString::Printf(TEXT("%s[%d]"), Field, Index);
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item))
			{
				return Where + TEXT(" must be an object");
			}
			FString Err;
			TArray<const TCHAR*> Keys = { TEXT("name"), TEXT("type"), TEXT("category"), TEXT("description") };
			if (bInput)
			{
				Keys.Add(TEXT("default"));
			}
			if (!OnlyKeys(*Item, Keys, Where, Err))
			{
				return Err;
			}

			MemberType Member;
			const FString Name = Str(*Item, TEXT("name")).TrimStartAndEnd();
			if (Name.IsEmpty())
			{
				return Where + TEXT(".name must not be empty");
			}
			for (const TPair<FGuid, MemberType>& Other : Out)
			{
				if (Other.Value.Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					return FString::Printf(TEXT("%s.name '%s' is listed twice"), *Where, *Name);
				}
			}
			Member.Name = FName(*Name);
			if (!ParseMemberType(Str(*Item, TEXT("type")), Member.Type, Err))
			{
				return Where + TEXT(".type: ") + Err;
			}
			Member.Category = Str(*Item, TEXT("category"));
			Member.Description = Str(*Item, TEXT("description"));

			if constexpr (bInput)
			{
				Member.Fixup();
				if (Has(*Item, TEXT("default")))
				{
					FString Text;
					if (!ScalarField(*Item, TEXT("default"), Text) || Text.IsEmpty())
					{
						return Where + TEXT(".default must be a non-empty string, a number or a boolean; omit it for the type's default");
					}
					if (!Member.Type.HasPinDefaultValue())
					{
						return FString::Printf(TEXT("%s.default: %s takes no default value"), *Where, *Member.Type.ToString());
					}
					FVoxelPinValue Value(Member.Type.GetExposedType());
					if (!ParseValue(Value, Text))
					{
						return FString::Printf(TEXT("%s.default '%s' does not parse as %s"), *Where, *Text, *Member.Type.ToString());
					}
					Member.DefaultPinValue = Value;
				}
			}
			Out.Add({ FGuid::NewGuid(), Member });
		}
		return FString();
	}

	FResult AddFunction(const FParams& Params)
	{
		FString Err;
		UVoxelGraph* Graph = LoadGraph(Params, Err);
		if (!Graph) return Error(Err);
		if (const FString FieldError = CheckFunctionFields(*Graph, Params, nullptr); !FieldError.IsEmpty()) return Error(FieldError);

		TArray<TPair<FGuid, FVoxelGraphFunctionInput>> Inputs;
		TArray<TPair<FGuid, FVoxelGraphFunctionOutput>> Outputs;
		if (const FString InputError = ParseMembers(Params, TEXT("inputs"), Inputs); !InputError.IsEmpty()) return Error(InputError);
		if (const FString OutputError = ParseMembers(Params, TEXT("outputs"), Outputs); !OutputError.IsEmpty()) return Error(OutputError);

		const FScopedTransaction Transaction(LOCTEXT("AddFunction", "Add Voxel Graph Function"));
		Graph->Modify();
		UVoxelTerminalGraph& Terminal = Graph->AddTerminalGraph(FGuid::NewGuid());
		ApplyFunctionFields(Terminal, Params);

		// Declare each member, then place its node, as dragging a new input or output off a pin does.
		UEdGraph& EdGraph = Terminal.GetEdGraph();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		const auto Place = [&](const FGuid& Guid, const TCHAR* NodeClass, int32 X, int32 Y) -> bool
		{
			UEdGraphNode* Node = SpawnNode(EdGraph, { FString(), FString(), nullptr, Guid, nullptr, NodeClass }, FVector2D(X, Y), Err);
			if (Node)
			{
				Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(*Node, false)));
			}
			return Node != nullptr;
		};
		for (int32 Index = 0; Index < Inputs.Num(); Index++)
		{
			Terminal.AddFunctionInput(Inputs[Index].Key, Inputs[Index].Value);
			if (!Place(Inputs[Index].Key, FunctionInputNodeClass, -400, Index * 150)) return Error(Err);
		}
		for (int32 Index = 0; Index < Outputs.Num(); Index++)
		{
			Terminal.AddFunctionOutput(Outputs[Index].Key, Outputs[Index].Value);
			if (!Place(Outputs[Index].Key, FunctionOutputNodeClass, 400, Index * 150)) return Error(Err);
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("function"), TerminalJson(Terminal));
		Out->SetArrayField(TEXT("nodes"), Nodes);
		Finish({ Graph, &Terminal, &EdGraph }, Params, Out);
		return Ok(Out);
	}

	FResult SetFunction(const FParams& Params)
	{
		FString Err;
		UVoxelGraph* Graph = LoadGraph(Params, Err);
		if (!Graph) return Error(Err);
		UVoxelTerminalGraph* Terminal = ResolveFunction(*Graph, Params, Err);
		if (!Terminal) return Error(Err);
		if (!Terminal->IsTopmostTerminalGraph())
		{
			return Error(FString::Printf(TEXT("%s overrides a base graph's function, which owns its name, category, description and exposure; edit the base"), *Terminal->GetDisplayName()));
		}
		if (const FString FieldError = CheckFunctionFields(*Graph, Params, Terminal); !FieldError.IsEmpty()) return Error(FieldError);

		const TSharedRef<FJsonObject> Previous = TerminalJson(*Terminal);
		const FScopedTransaction Transaction(LOCTEXT("SetFunction", "Edit Voxel Graph Function"));
		Graph->Modify();
		Terminal->Modify();
		ApplyFunctionFields(*Terminal, Params);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("previous"), Previous);
		Out->SetObjectField(TEXT("function"), TerminalJson(*Terminal));
		Finish({ Graph, Terminal, &Terminal->GetEdGraph() }, Params, Out);
		return Ok(Out);
	}

	// Call nodes, in every loaded graph, that a function's removal would leave pointing at nothing.
	bool FindCallers(const UVoxelTerminalGraph& Function, TArray<UEdGraphNode*>& Out, FString& OutError)
	{
		UClass* MemberCall = FindObject<UClass>(nullptr, TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_CallMemberFunction"));
		UClass* ExternalCall = FindObject<UClass>(nullptr, TEXT("/Script/VoxelGraphEditor.VoxelGraphNode_CallExternalFunction"));
		const auto GuidOf = [](UClass* Class) { return Class ? CastField<FStructProperty>(Class->FindPropertyByName(TEXT("Guid"))) : nullptr; };
		FStructProperty* MemberGuid = GuidOf(MemberCall);
		FStructProperty* ExternalGuid = GuidOf(ExternalCall);
		FObjectPropertyBase* LibraryProperty = ExternalCall ? CastField<FObjectPropertyBase>(ExternalCall->FindPropertyByName(TEXT("FunctionLibrary"))) : nullptr;
		if (!MemberGuid || !ExternalGuid || !LibraryProperty)
		{
			OutError = TEXT("Voxel's call-function node classes no longer have the properties this tool reads (Guid/FunctionLibrary); the Voxel version is unsupported");
			return false;
		}

		// As the members panel does before deleting a function: callers in unloaded graphs are found too.
		UVoxelGraph::LoadAllGraphs();
		const UVoxelGraph& Owner = Function.GetGraph();
		const UObject* Library = Owner.IsFunctionLibrary() ? Owner.GetOuter() : nullptr;
		const FGuid Guid = Function.GetGuid();
		for (TObjectIterator<UEdGraphNode> It; It; ++It)
		{
			UEdGraphNode* Node = *It;
			const UEdGraph* NodeGraph = Node && IsValid(Node) ? Node->GetGraph() : nullptr;
			// Deleted nodes stay alive for undo but leave their graph.
			if (!NodeGraph || !NodeGraph->Nodes.Contains(Node))
			{
				continue;
			}
			if (Node->IsA(MemberCall) && *MemberGuid->ContainerPtrToValuePtr<FGuid>(Node) == Guid)
			{
				const UVoxelGraph* Caller = Node->GetTypedOuter<UVoxelGraph>();
				if (Caller && Caller->GetBaseGraphs().Contains(&Owner))
				{
					Out.Add(Node);
				}
			}
			else if (Library && Node->IsA(ExternalCall) && *ExternalGuid->ContainerPtrToValuePtr<FGuid>(Node) == Guid &&
				LibraryProperty->GetObjectPropertyValue_InContainer(Node) == Library)
			{
				Out.Add(Node);
			}
		}
		return true;
	}

	FResult RemoveFunction(const FParams& Params)
	{
		FString Err;
		UVoxelGraph* Graph = LoadGraph(Params, Err);
		if (!Graph) return Error(Err);
		UVoxelTerminalGraph* Terminal = ResolveFunction(*Graph, Params, Err);
		if (!Terminal) return Error(Err);

		// Removing an override reverts callers to the base graph's function; removing the function itself orphans them.
		const bool bOverride = !Terminal->IsTopmostTerminalGraph();
		if (!bOverride)
		{
			TArray<UEdGraphNode*> Callers;
			if (!FindCallers(*Terminal, Callers, Err)) return Error(Err);
			if (Callers.Num() > 0)
			{
				TArray<FString> Names;
				for (const UEdGraphNode* Caller : Callers)
				{
					const UVoxelGraph* CallerGraph = Caller->GetTypedOuter<UVoxelGraph>();
					Names.Add(FString::Printf(TEXT("%s node %s"), CallerGraph ? *CallerGraph->GetPathName() : TEXT("?"), *Caller->NodeGuid.ToString()));
				}
				return Error(FString::Printf(TEXT("%s is called by %d node(s); delete them first (voxel_graph_delete_node): %s"),
					*Terminal->GetDisplayName(), Callers.Num(), *FString::Join(Names, TEXT("; "))));
			}
		}

		// The graph editor holds a tab on the function's EdGraph; close it as the members panel does.
		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		UObject* Asset = Graph->IsFunctionLibrary() ? Graph->GetOuter() : Graph;
		const bool bClosedEditor = AssetEditors->FindEditorForAsset(Asset, false) != nullptr;
		if (bClosedEditor)
		{
			AssetEditors->CloseAllEditorsForAsset(Asset);
		}

		const TSharedRef<FJsonObject> Removed = TerminalJson(*Terminal);
		const FScopedTransaction Transaction(LOCTEXT("RemoveFunction", "Remove Voxel Graph Function"));
		Graph->Modify();
		Terminal->Modify();
		Graph->RemoveTerminalGraph(Terminal->GetGuid());

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("removed"), Removed);
		Out->SetBoolField(TEXT("override"), bOverride);
		Out->SetBoolField(TEXT("closedEditor"), bClosedEditor);
		Finish({ Graph, nullptr, nullptr }, Params, Out);
		return Ok(Out);
	}

	// --------------------------------------------------------------------------------

	UVoxelStampComponent* FindStampComponent(const FParams& Params, FString& OutError)
	{
		AActor* Actor = FindActor(Params, OutError);
		return Actor ? FindComponent<UVoxelStampComponent>(*Actor, Params, OutError) : nullptr;
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
		for (const auto& Pair : (*Values)->Values)
		{
			FGuid Guid;
			FVoxelParameter Parameter;
			if (!FindParameter(*Graph, FString(*Pair.Key), Guid, Parameter))
			{
				return Error(FString::Printf(TEXT("Graph %s has no parameter '%s'"), *Graph->GetName(), *Pair.Key));
			}
			FString Text;
			if (!ScalarText(Pair.Value, Text))
			{
				return Error(FString::Printf(TEXT("values.%s must be a string, number, boolean or null"), *Pair.Key));
			}
			FVoxelPinValue Value(Parameter.Type.GetExposedType());
			if (!ParseValue(Value, Text))
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

		// Read back from the component: SetStamp runs FixupParameterOverrides on its own copy.
		const StampType* Stored = Component.GetStamp().template As<StampType>();
		if (!Stored) return Error(TEXT("The stamp changed type while being set"));
		const IVoxelParameterOverridesOwner& StoredOwner = *Stored;

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Applied = MakeShared<FJsonObject>();
		for (const TPair<FGuid, FVoxelParameterValueOverride>& Pair : StoredOwner.GetGuidToValueOverride())
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

void AddGraphHandlers(TArray<FHandlerEntry>& Out)
{
	const auto AssetPath = []
	{
		return MCPParam::Required(TEXT("assetPath"), EMCPParamType::String,
			TEXT("UVoxelGraph or UVoxelFunctionLibraryAsset asset path; a bare package path (/Game/A/B) also resolves."));
	};
	// ResolveGraph's inputs, which every graph handler reads, followed by the handler's own.
	const auto Graph = [&](TArray<FMCPParamSpec> Rest, bool bMutates)
	{
		Rest.Insert({
			AssetPath(),
			MCPParam::Optional(TEXT("terminalGraph"), EMCPParamType::String,
				TEXT("Terminal graph GUID from voxel_graph_read; default the main graph. Required for graphs without one, such as function libraries.")),
		}, 0);
		if (bMutates)
		{
			Rest.Add(Spec::Save(TEXT("Save the graph after the edit; default true.")));
		}
		return Rest;
	};
	const auto NodeRef = [](const TCHAR* Name, const TCHAR* Description) { return MCPParam::Required(Name, EMCPParamType::String, Description); };
	const auto Position = [](const TCHAR* Name, const TCHAR* Description) { return MCPParam::Optional(Name, EMCPParamType::Integer, Description).Range(MIN_int32, MAX_int32); };
	const auto ScalarValue = [](const TCHAR* Name, bool bRequired, const TCHAR* Description)
	{
		FMCPParamSpec Param = MCPParam::Optional(Name, EMCPParamType::String, Description).Or(EMCPParamType::Number).Or(EMCPParamType::Boolean);
		Param.bRequired = bRequired;
		return Param;
	};
	const auto Link = [&]
	{
		return Graph({
			NodeRef(TEXT("fromNode"), TEXT("Source node: id (GUID), object name or unique title.")),
			NodeRef(TEXT("fromPin"), TEXT("Visible output pin on fromNode, by name or display name, case-insensitive.")),
			NodeRef(TEXT("toNode"), TEXT("Target node: id (GUID), object name or unique title.")),
			NodeRef(TEXT("toPin"), TEXT("Visible input pin on toNode, by name or display name, case-insensitive.")),
		}, true);
	};
	const auto ParameterName = [] { return MCPParam::Required(TEXT("name"), EMCPParamType::String, TEXT("Graph parameter name, case-insensitive.")); };

	Out.Add({ TEXT("voxel_graph_read"), &GraphRead, Graph({
		MCPParam::Optional(TEXT("includePins"), EMCPParamType::Boolean, TEXT("Include each node's pins, defaults and links; default true.")),
	}, false) });

	Out.Add({ TEXT("voxel_graph_list_node_types"), &ListNodeTypes, Graph({
		MCPParam::Optional(TEXT("query"), EMCPParamType::String, TEXT("Space-separated words that must all match the type key or tooltip, case-insensitive; default every type.")),
		MCPParam::Optional(TEXT("limit"), EMCPParamType::Integer, TEXT("Most rows returned, >= 1; default 100. total still counts every match.")).Range(1, MAX_int32),
	}, false) });

	Out.Add({ TEXT("voxel_graph_add_node"), &AddNode, Graph({
		NodeRef(TEXT("nodeType"), TEXT("Type key from voxel_graph_list_node_types ('Category|Name'), or the bare name when it is unique.")),
		Position(TEXT("x"), TEXT("Node X position in graph units; default 0.")),
		Position(TEXT("y"), TEXT("Node Y position in graph units; default 0.")),
	}, true) });

	Out.Add({ TEXT("voxel_graph_connect"), [](const FParams& P) { return Connect(P, true); }, Link() });
	Out.Add({ TEXT("voxel_graph_disconnect"), [](const FParams& P) { return Connect(P, false); }, Link() });

	Out.Add({ TEXT("voxel_graph_set_pin_default"), &SetPinDefault, Graph({
		NodeRef(TEXT("node"), TEXT("Node id (GUID), object name or unique title.")),
		NodeRef(TEXT("pin"), TEXT("Unconnected visible input pin, by name or display name.")),
		ScalarValue(TEXT("value"), true, TEXT("Pin default text, e.g. 5000, true or (X=1,Y=2), or an asset path for object pins; numbers and booleans are written as text.")),
	}, true) });

	Out.Add({ TEXT("voxel_graph_delete_node"), &DeleteNode, Graph({
		NodeRef(TEXT("node"), TEXT("Node id (GUID), object name or unique title.")),
	}, true) });

	Out.Add({ TEXT("voxel_graph_export_t3d"), &ExportT3D, Graph({
		MCPParam::Optional(TEXT("nodes"), EMCPParamType::Array, TEXT("Node ids, names or unique titles to export, non-empty; default every copyable node."))
			.Items(EMCPParamType::String),
	}, false) });

	Out.Add({ TEXT("voxel_graph_import_t3d"), &ImportT3D, Graph({
		NodeRef(TEXT("t3d"), TEXT("Clipboard T3D node text, e.g. from voxel_graph_export_t3d.")),
		Position(TEXT("offsetX"), TEXT("Added to each pasted node's X position; default 0.")),
		Position(TEXT("offsetY"), TEXT("Added to each pasted node's Y position; default 0.")),
	}, true) });

	Out.Add({ TEXT("voxel_graph_add_parameter"), &AddParameter, Graph({
		MCPParam::Required(TEXT("name"), EMCPParamType::String, TEXT("New parameter name, unique in the graph (case-insensitive).")),
		MCPParam::Required(TEXT("type"), EMCPParamType::String,
			TEXT("float, double, int32 (or int), int64, bool, name, vector2d, vector, color (or linearcolor), seed, or struct:, object:, class: or enum: followed by an asset path; case-insensitive.")),
		ScalarValue(TEXT("default"), false, TEXT("Default value text, validated against the type before anything changes; default the type's own default.")),
		MCPParam::Optional(TEXT("category"), EMCPParamType::String, TEXT("Category shown in the graph's members panel.")),
		MCPParam::Optional(TEXT("description"), EMCPParamType::String, TEXT("Parameter tooltip.")),
	}, true) });

	Out.Add({ TEXT("voxel_graph_remove_parameter"), &RemoveParameter, Graph({ ParameterName() }, true) });

	Out.Add({ TEXT("voxel_graph_set_parameter_default"), &SetParameterDefault, Graph({
		ParameterName(),
		ScalarValue(TEXT("value"), true, TEXT("Value text, e.g. 5000, true or (X=1,Y=2), or an asset path for object parameters; numbers and booleans are written as text.")),
	}, true) });

	const auto FunctionFields = [](bool bNameRequired)
	{
		FMCPParamSpec Name = MCPParam::Optional(TEXT("name"), EMCPParamType::String, TEXT("Function display name, unique among the graph's functions (case-insensitive)."));
		Name.bRequired = bNameRequired;
		return TArray<FMCPParamSpec>{
			Name,
			MCPParam::Optional(TEXT("category"), EMCPParamType::String, TEXT("Category the function is listed under in node menus; \"\" for none.")),
			MCPParam::Optional(TEXT("description"), EMCPParamType::String, TEXT("Function tooltip.")),
			MCPParam::Optional(TEXT("exposeToLibrary"), EMCPParamType::Boolean,
				TEXT("Function libraries only: list the function in other graphs' node menus (bExposeToLibrary); a new function defaults to true.")),
		};
	};
	const auto Member = [](bool bInput)
	{
		TArray<FMCPParamField> Fields = {
			MCPParam::RequiredField(TEXT("name"), EMCPParamType::String, TEXT("Member name, unique in the list (case-insensitive).")),
			MCPParam::RequiredField(TEXT("type"), EMCPParamType::String,
				TEXT("A voxel_graph_add_parameter type, optionally followed by ' buffer' or ' array' (e.g. 'float buffer'), case-insensitive.")),
			MCPParam::OptionalField(TEXT("category"), EMCPParamType::String, TEXT("Category in the members panel.")),
			MCPParam::OptionalField(TEXT("description"), EMCPParamType::String, TEXT("Pin tooltip.")),
		};
		if (bInput)
		{
			Fields.Add(MCPParam::OptionalField(TEXT("default"), EMCPParamType::Any,
				TEXT("Default value text (string, number or boolean) parsed as the type; default the type's own default.")));
		}
		return MCPParam::Optional(bInput ? TEXT("inputs") : TEXT("outputs"), EMCPParamType::Array,
			bInput
				? TEXT("Function inputs, in pin order; each gets a Function Input node at x -400.")
				: TEXT("Function outputs, in pin order; each gets a Function Output node at x 400 to connect the result into."))
			.Items(EMCPParamType::Object).WithFields(Fields);
	};
	const auto FunctionGuid = []
	{
		return MCPParam::Required(TEXT("terminalGraph"), EMCPParamType::String, TEXT("The function's terminal graph GUID, from voxel_graph_read terminalGraphs."));
	};
	const auto GraphSave = [] { return Spec::Save(TEXT("Save the graph after the edit; default true.")); };

	{
		TArray<FMCPParamSpec> Params = { AssetPath() };
		Params.Append(FunctionFields(true));
		Params.Append({ Member(true), Member(false), GraphSave() });
		Out.Add({ TEXT("voxel_graph_add_function"), &AddFunction, Params });
	}
	{
		TArray<FMCPParamSpec> Fields = FunctionFields(false);
		TArray<FMCPParamSpec> Params = { AssetPath(), FunctionGuid() };
		Params.Append(Fields);
		Params.Add(GraphSave());
		Out.Add({ TEXT("voxel_graph_set_function"), &SetFunction, Params, FMCPSpecRules().AtLeastOne(Spec::Branches(Fields)) });
	}
	Out.Add({ TEXT("voxel_graph_remove_function"), &RemoveFunction, { AssetPath(), FunctionGuid(), GraphSave() } });

	Out.Add({ TEXT("voxel_stamp_set_parameters"), &StampSetParameters, {
		Spec::ActorPath(TEXT("Stamp actor object path; preferred, since stamp actors relabel themselves.")),
		Spec::ActorLabel(TEXT("Stamp actor label; must match exactly one actor.")),
		Spec::ComponentName(TEXT("UVoxelStampComponent object name; default the actor's first one.")),
		Spec::ValueMap(TEXT("values"), true,
			TEXT("Non-empty { parameterName: value } overrides on the stamp's height or volume graph; each value a string, number, boolean or null, parsed as the parameter's type, and null sets an object parameter to None.")),
		Spec::SaveDirty(),
	}, Spec::OneActor() });
}

void ReleaseCatalog()
{
	Catalog.Empty();
}
}

#undef LOCTEXT_NAMESPACE
