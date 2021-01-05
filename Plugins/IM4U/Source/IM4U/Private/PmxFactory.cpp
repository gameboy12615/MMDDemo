﻿// Copyright 2015 BlackMa9. All Rights Reserved.

#include "PmxFactory.h"
#include "IM4UPrivatePCH.h"

#include "CoreMinimal.h"
#include "Factories.h"
#include "BusyCursor.h"
#include "SSkeletonWidget.h"

//#include "FbxImporter.h"

#include "Misc/FbxErrors.h"
#include "AssetRegistryModule.h"
#include "Engine/StaticMesh.h"

/////////////////////////

#include "Engine.h"
#include "Editor.h"
#include "TextureLayout.h"
#include "SkelImport.h"
//#include "FbxImporter.h"
#include "AnimEncoding.h"
#include "SSkeletonWidget.h"

#include "AssetRegistryModule.h"
#include "AssetNotifications.h"

#include "ObjectTools.h"

#include "ApexClothingUtils.h"
#include "Developer/MeshUtilities/Public/MeshUtilities.h"

#include "MessageLogModule.h"
#include "ComponentReregisterContext.h"

#include "PhysicsEngine/PhysicsAsset.h"
#include "Editor/UnrealEd/Public/PhysicsAssetUtils.h"
////////////

#include "PmdImporter.h"
#include "PmxImporter.h"
#include "PmxImportUI.h"

#include "MMDSkeletalMeshImportData.h"
#include "MMDStaticMeshImportData.h"

#include "Components/SkeletalMeshComponent.h"
#include "Rendering/SkeletalMeshModel.h"


#define LOCTEXT_NAMESPACE "PMXImpoter"

DEFINE_LOG_CATEGORY(LogMMD4UE4_PMXFactory)

/////////////////////////////////////////////////////////
// 3 "ProcessImportMesh..." functions outputing Unreal data from a filled FSkeletalMeshBinaryImport
// and a handfull of other minor stuff needed by these 
// Fully taken from SkeletalMeshImport.cpp

extern void ProcessImportMeshInfluences(FSkeletalMeshImportData& ImportData);
extern void ProcessImportMeshMaterials(TArray<FSkeletalMaterial>& Materials, FSkeletalMeshImportData& ImportData);
extern bool ProcessImportMeshSkeleton(const USkeleton* SkeletonAsset, FReferenceSkeleton& RefSkeleton, int32& SkeletalDepth, FSkeletalMeshImportData& ImportData);

/////////////////////////////////////////////////////////

UPmxFactory::UPmxFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{

	SupportedClass = NULL;
	//SupportedClass = UPmxFactory::StaticClass();
	Formats.Empty();

	Formats.Add(TEXT("pmd;PMD meshes and animations"));
	Formats.Add(TEXT("pmx;PMX meshes and animations"));

	bCreateNew = false;
	bText = false;
	bEditorImport = true;
	
	bOperationCanceled = false;
	bDetectImportTypeOnImport = false;

	//ImportUI = NewObject<UPmxImportUI>(this, NAME_None, RF_NoFlags);
}

void UPmxFactory::PostInitProperties()
{
	Super::PostInitProperties();

	ImportUI = NewObject<UPmxImportUI>(this, NAME_None, RF_NoFlags);
}

bool UPmxFactory::DoesSupportClass(UClass* Class)
{
	return (Class == UPmxFactory::StaticClass());
}

UClass* UPmxFactory::ResolveSupportedClass()
{
	return UPmxFactory::StaticClass();
}

////////////////////////////////////////////////
//IM4U Develop Temp Define
//////////////////////////////////////////////
//#define DEBUG_MMD_UE4_ORIGINAL_CODE	(1)
#define DEBUG_MMD_PLUGIN_SKELTON	(1) 
//#define DEBUG_MMD_PLUGIN_STATICMESH	(1)
//#define DEBUG_MMD_PLUGIN_ANIMATION	(1)
//////////////////////////////////////////////


UObject* UPmxFactory::FactoryCreateBinary
	(
	UClass*			Class,
	UObject*			InParent,
	FName				Name,
	EObjectFlags		Flags,
	UObject*			Context,
	const TCHAR*		Type,
	const uint8*&		Buffer,
	const uint8*		BufferEnd,
	FFeedbackContext*	Warn,
	bool&				bOutOperationCanceled
	)
{
	// MMD Default 
	importAssetTypeMMD = E_MMD_TO_UE4_UNKOWN;
#ifdef DEBUG_MMD_PLUGIN_SKELTON
	importAssetTypeMMD = E_MMD_TO_UE4_SKELTON;
#endif
#ifdef DEBUG_MMD_PLUGIN_STATICMESH
	importAssetTypeMMD = E_MMD_TO_UE4_STATICMESH;
#endif

	if (bOperationCanceled)
	{
		bOutOperationCanceled = true;
		FEditorDelegates::OnAssetPostImport.Broadcast(this, NULL);
		return NULL;
	}

	FEditorDelegates::OnAssetPreImport.Broadcast(this, Class, InParent, Name, Type);

	UObject* NewObject = NULL;

#ifdef DEBUG_MMD_UE4_ORIGINAL_CODE
	if (bDetectImportTypeOnImport)
	{
		if (!DetectImportType(UFactory::CurrentFilename))
		{
			// Failed to read the file info, fail the import
			FEditorDelegates::OnAssetPostImport.Broadcast(this, NULL);
			return NULL;
		}
	}

	// logger for all error/warnings
	// this one prints all messages that are stored in FFbxImporter
	UnFbx::FFbxImporter* FbxImporter = UnFbx::FFbxImporter::GetInstance();

	UnFbx::FFbxLoggerSetter Logger(FbxImporter);

	EFBXImportType ForcedImportType = FBXIT_StaticMesh;

	bool bIsObjFormat = false;
	if (FString(Type).Equals(TEXT("obj"), ESearchCase::IgnoreCase))
	{
		bIsObjFormat = true;
	}


	bool bShowImportDialog = bShowOption && !GIsAutomationTesting;
	bool bImportAll = false;
	UnFbx::FBXImportOptions* ImportOptions = GetImportOptions(FbxImporter, ImportUI, bShowImportDialog, InParent->GetPathName(), bOperationCanceled, bImportAll, bIsObjFormat, bIsObjFormat, ForcedImportType);
	bOutOperationCanceled = bOperationCanceled;

	if (bImportAll)
	{
		// If the user chose to import all, we don't show the dialog again and use the same settings for each object until importing another set of files
		bShowOption = false;
	}
#endif
	FPmxImporter* PmxImporter = FPmxImporter::GetInstance();

	EPMXImportType ForcedImportType = PMXIT_StaticMesh;

	// For multiple files, use the same settings
	bDetectImportTypeOnImport = false;

	//judge MMD format(pmx or pmd)
	bool bIsPmxFormat = false;
	if (FString(Type).Equals(TEXT("pmx"), ESearchCase::IgnoreCase))
	{
		//Is PMX format 
		bIsPmxFormat = true;
	}
	//Load MMD Model From binary File
	MMD4UE4::PmxMeshInfo pmxMeshInfoPtr;
	pmxMaterialImportHelper.InitializeBaseValue(InParent);
	bool pmxImportResult = false;
	if (bIsPmxFormat)
	{
		//pmx ver
		pmxImportResult = pmxMeshInfoPtr.PMXLoaderBinary(Buffer, BufferEnd);
	}
	else
	{
		//pmd ver
		MMD4UE4::PmdMeshInfo PmdMeshInfo;
		if (PmdMeshInfo.PMDLoaderBinary(Buffer, BufferEnd))
		{
			//Up convert From PMD to PMX format gfor ue4
			pmxImportResult = PmdMeshInfo.ConvertToPmxFormat(&pmxMeshInfoPtr);
		}
	}
	if (!pmxImportResult)
	{
		// Log the error message and fail the import.
		Warn->Log(ELogVerbosity::Error, "PMX Import ERR...FLT");
		FEditorDelegates::OnAssetPostImport.Broadcast(this, NULL);
		return NULL;
	}
	else
	{
		//モデル読み込み後の警告文表示：コメント欄
		FText TitleStr = FText::Format(LOCTEXT("ImportReadMe_Generic", "{0}"), FText::FromString("tilesss"));
		const FText Message
			= FText::Format(LOCTEXT("ImportReadMe_Generic_Msg",
			"Important!! \nReadMe Lisence \n modele Name:\n'{0}'\n \n Model Comment JP:\n'{1}'"),
			FText::FromString(pmxMeshInfoPtr.modelNameJP), FText::FromString(pmxMeshInfoPtr.modelCommentJP));
		if (EAppReturnType::Ok != FMessageDialog::Open(EAppMsgType::OkCancel, Message))
		{
			FEditorDelegates::OnAssetPostImport.Broadcast(this, NULL);
			return NULL;
		}
		TitleStr = FText::Format(LOCTEXT("ImportReadMe_Generic_Dbg", "{0} 制限事項"), FText::FromString("IM4U Plugin"));
		const FText MessageDbg
			= FText(LOCTEXT("ImportReadMe_Generic_Dbg_Msg",
			"次のImportOption用Slateはまだ実装途中です。\n\
			Import対象はSkeletonのみ生成可能。\n\
			現時点で有効なパラメータは、表示されている項目が有効です。") );
		FMessageDialog::Open(EAppMsgType::Ok, MessageDbg, &TitleStr);
	}

	// show Import Option Slate
	bool bImportAll = false;
	ImportUI->bIsObjImport = true;//obj mode
	ImportUI->OriginalImportType = EPMXImportType::PMXIT_SkeletalMesh;
	PMXImportOptions* ImportOptions
		= GetImportOptions(
		PmxImporter,
		ImportUI,
		true,//bShowImportDialog, 
		InParent->GetPathName(),
		bOperationCanceled,
		bImportAll,
		ImportUI->bIsObjImport,//bIsPmxFormat,
		bIsPmxFormat,
		ForcedImportType
		);
	if (bImportAll)
	{
		// If the user chose to import all, we don't show the dialog again and use the same settings for each object until importing another set of files
		//bShowOption = false;
	}

	if (ImportOptions)
	{
		Warn->BeginSlowTask(NSLOCTEXT("PmxFactory", "BeginImportingPmxMeshTask", "Importing Pmx mesh"), true);
#if 1
		{
#ifdef DEBUG_MMD_UE4_ORIGINAL_CODE
			// Log the import message and import the mesh.
			const TCHAR* errorMessage = FbxImporter->GetErrorMessage();
			if (errorMessage[0] != '\0')
			{
				Warn->Log(errorMessage);
			}

			FbxNode* RootNodeToImport = NULL;
			RootNodeToImport = FbxImporter->Scene->GetRootNode();

			// For animation and static mesh we assume there is at lease one interesting node by default
			int32 InterestingNodeCount = 1;
			TArray< TArray<FbxNode*>* > SkelMeshArray;

			bool bImportStaticMeshLODs = ImportUI->StaticMeshImportData->bImportMeshLODs;
			bool bCombineMeshes = ImportUI->bCombineMeshes;
#endif
			// For animation and static mesh we assume there is at lease one interesting node by default
			int32 InterestingNodeCount = 1;

			if (importAssetTypeMMD == E_MMD_TO_UE4_SKELTON)
			{
#ifdef DEBUG_MMD_PLUGIN_SKELTON
#ifdef DEBUG_MMD_UE4_ORIGINAL_CODE
				FbxImporter->FillFbxSkelMeshArrayInScene(RootNodeToImport, SkelMeshArray, false);
				InterestingNodeCount = SkelMeshArray.Num();
#else
				InterestingNodeCount = 1;//test ? not Anime?
#endif
#endif
			}
			else if (importAssetTypeMMD == E_MMD_TO_UE4_STATICMESH)
			{
#ifdef DEBUG_MMD_PLUGIN_STATICMESH
				FbxImporter->ApplyTransformSettingsToFbxNode(RootNodeToImport, ImportUI->StaticMeshImportData);

				if (bCombineMeshes && !bImportStaticMeshLODs)
				{
					// If Combine meshes and dont import mesh LODs, the interesting node count should be 1 so all the meshes are grouped together into one static mesh
					InterestingNodeCount = 1;
				}
				else
				{
					// count meshes in lod groups if we dont care about importing LODs
					bool bCountLODGroupMeshes = !bImportStaticMeshLODs;
					int32 NumLODGroups = 0;
					InterestingNodeCount = FbxImporter->GetFbxMeshCount(RootNodeToImport, bCountLODGroupMeshes, NumLODGroups);

					// if there were LODs in the file, do not combine meshes even if requested
					if (bImportStaticMeshLODs && bCombineMeshes)
					{
						bCombineMeshes = NumLODGroups == 0;
					}
				}
#endif
			}


			if (InterestingNodeCount > 1)
			{
				// the option only works when there are only one asset
//				ImportOptions->bUsedAsFullName = false;
			}

			const FString Filename(UFactory::CurrentFilename);
			if (/*RootNodeToImport &&*/ InterestingNodeCount > 0)
			{
				int32 NodeIndex = 0;

				int32 ImportedMeshCount = 0;
				UStaticMesh* NewStaticMesh = NULL;
				if (importAssetTypeMMD == E_MMD_TO_UE4_STATICMESH)  // static mesh
				{
#ifdef DEBUG_MMD_PLUGIN_STATICMESH
					if (bCombineMeshes)
					{
						TArray<FbxNode*> FbxMeshArray;
						FbxImporter->FillFbxMeshArray(RootNodeToImport, FbxMeshArray, FbxImporter);
						if (FbxMeshArray.Num() > 0)
						{
							NewStaticMesh = FbxImporter->ImportStaticMeshAsSingle(InParent, FbxMeshArray, Name, Flags, ImportUI->StaticMeshImportData, NULL, 0);
						}

						ImportedMeshCount = NewStaticMesh ? 1 : 0;
					}
					else
					{
						TArray<UObject*> AllNewAssets;
						UObject* Object = RecursiveImportNode(FbxImporter, RootNodeToImport, InParent, Name, Flags, NodeIndex, InterestingNodeCount, AllNewAssets);

						NewStaticMesh = Cast<UStaticMesh>(Object);

						// Make sure to notify the asset registry of all assets created other than the one returned, which will notify the asset registry automatically.
						for (auto AssetIt = AllNewAssets.CreateConstIterator(); AssetIt; ++AssetIt)
						{
							UObject* Asset = *AssetIt;
							if (Asset != NewStaticMesh)
							{
								FAssetRegistryModule::AssetCreated(Asset);
								Asset->MarkPackageDirty();
							}
						}

						ImportedMeshCount = AllNewAssets.Num();
					}

					// Importing static mesh sockets only works if one mesh is being imported
					if (ImportedMeshCount == 1 && NewStaticMesh)
					{
						FbxImporter->ImportStaticMeshSockets(NewStaticMesh);
					}

					NewObject = NewStaticMesh;
#endif
				}
				else if (importAssetTypeMMD == E_MMD_TO_UE4_SKELTON)// skeletal mesh
				{
#ifdef DEBUG_MMD_PLUGIN_SKELTON
					int32 TotalNumNodes = 0;

					//for (int32 i = 0; i < SkelMeshArray.Num(); i++)
					for (int32 i = 0; i < 1/*SkelMeshArray.Num()*/; i++)
					{
						int32 LODIndex=0;
#ifdef DEBUG_MMD_UE4_ORIGINAL_CODE
						TArray<FbxNode*> NodeArray = *SkelMeshArray[i];

						TotalNumNodes += NodeArray.Num();
						// check if there is LODGroup for this skeletal mesh
						int32 MaxLODLevel = 1;
						for (int32 j = 0; j < NodeArray.Num(); j++)
						{
							FbxNode* Node = NodeArray[j];
							if (Node->GetNodeAttribute() && Node->GetNodeAttribute()->GetAttributeType() == FbxNodeAttribute::eLODGroup)
							{
								// get max LODgroup level
								if (MaxLODLevel < Node->GetChildCount())
								{
									MaxLODLevel = Node->GetChildCount();
								}
							}
						}

						bool bImportSkeletalMeshLODs = ImportUI->SkeletalMeshImportData->bImportMeshLODs;
						for (LODIndex = 0; LODIndex < MaxLODLevel; LODIndex++)
						{
							if (!bImportSkeletalMeshLODs && LODIndex > 0) // not import LOD if UI option is OFF
							{
								break;
							}

							TArray<FbxNode*> SkelMeshNodeArray;
							for (int32 j = 0; j < NodeArray.Num(); j++)
							{
								FbxNode* Node = NodeArray[j];
								if (Node->GetNodeAttribute() && Node->GetNodeAttribute()->GetAttributeType() == FbxNodeAttribute::eLODGroup)
								{
									if (Node->GetChildCount() > LODIndex)
									{
										SkelMeshNodeArray.Add(Node->GetChild(LODIndex));
									}
									else // in less some LODGroups have less level, use the last level
									{
										SkelMeshNodeArray.Add(Node->GetChild(Node->GetChildCount() - 1));
									}
								}
								else
								{
									SkelMeshNodeArray.Add(Node);
								}
							}

							if (LODIndex == 0 && SkelMeshNodeArray.Num() != 0)
							{
								FName OutputName = FbxImporter->MakeNameForMesh(Name.ToString(), SkelMeshNodeArray[0]);

								USkeletalMesh* NewMesh = FbxImporter->ImportSkeletalMesh(InParent, SkelMeshNodeArray, OutputName, Flags, ImportUI->SkeletalMeshImportData, FPaths::GetBaseFilename(Filename));
								NewObject = NewMesh;

								if (NewMesh && ImportUI->bImportAnimations)
								{
									// We need to remove all scaling from the root node before we set up animation data.
									// Othewise some of the global transform calculations will be incorrect.
									FbxImporter->RemoveTransformSettingsFromFbxNode(RootNodeToImport, ImportUI->SkeletalMeshImportData);
									FbxImporter->SetupAnimationDataFromMesh(NewMesh, InParent, SkelMeshNodeArray, ImportUI->AnimSequenceImportData, OutputName.ToString());

									// Reapply the transforms for the rest of the import
									FbxImporter->ApplyTransformSettingsToFbxNode(RootNodeToImport, ImportUI->SkeletalMeshImportData);
								}
							}
							else if (NewObject) // the base skeletal mesh is imported successfully
							{
								USkeletalMesh* BaseSkeletalMesh = Cast<USkeletalMesh>(NewObject);
								FName LODObjectName = NAME_None;
								USkeletalMesh *LODObject = FbxImporter->ImportSkeletalMesh(GetTransientPackage(), SkelMeshNodeArray, LODObjectName, RF_NoFlags, ImportUI->SkeletalMeshImportData, FPaths::GetBaseFilename(Filename));
								bool bImportSucceeded = FbxImporter->ImportSkeletalMeshLOD(LODObject, BaseSkeletalMesh, LODIndex, false);

								if (bImportSucceeded)
								{
									BaseSkeletalMesh->LODInfo[LODIndex].ScreenSize = 1.0f / (MaxLODLevel * LODIndex);
								}
								else
								{
									FbxImporter->AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FailedToImport_SkeletalMeshLOD", "Failed to import Skeletal mesh LOD.")), FFbxErrors::SkeletalMesh_LOD_FailedToImport);
								}
							}

							// import morph target
							if (NewObject && ImportUI->SkeletalMeshImportData->bImportMorphTargets)
							{
								// Disable material importing when importing morph targets
								uint32 bImportMaterials = ImportOptions->bImportMaterials;
								ImportOptions->bImportMaterials = 0;

								FbxImporter->ImportFbxMorphTarget(SkelMeshNodeArray, Cast<USkeletalMesh>(NewObject), InParent, Filename, LODIndex);

								ImportOptions->bImportMaterials = !!bImportMaterials;
							}
						}

						if (NewObject)
						{
							NodeIndex++;
							FFormatNamedArguments Args;
							Args.Add(TEXT("NodeIndex"), NodeIndex);
							Args.Add(TEXT("ArrayLength"), SkelMeshArray.Num());
							GWarn->StatusUpdate(NodeIndex, SkelMeshArray.Num(), FText::Format(NSLOCTEXT("UnrealEd", "Importingf", "Importing ({NodeIndex} of {ArrayLength})"), Args));
						}
#else
						// for MMD?
						int32 MaxLODLevel = 1;
						
						if (LODIndex == 0 /*&& SkelMeshNodeArray.Num() != 0*/)
						{
							FName OutputName = FName(*FString::Printf(TEXT("%s"), *Name.ToString() ));// FbxImporter->MakeNameForMesh(Name.ToString(), SkelMeshNodeArray[0]);

							USkeletalMesh* NewMesh = NULL;
							NewMesh = ImportSkeletalMesh(
								InParent,
								&pmxMeshInfoPtr,//SkelMeshNodeArray,
								OutputName,
								Flags,
								//ImportUI->SkeletalMeshImportData,
								FPaths::GetBaseFilename(Filename),
								NULL,// test for MMD,
								true
								);
							NewObject = NewMesh;
						}

						// import morph target
						if (NewObject && ImportUI->SkeletalMeshImportData->bImportMorphTargets)
						{
							// Disable material importing when importing morph targets
							uint32 bImportMaterials = ImportOptions->bImportMaterials;
							ImportOptions->bImportMaterials = 0;

							/*FbxImporter->*/ImportFbxMorphTarget(
								//SkelMeshNodeArray, 
								pmxMeshInfoPtr,
								Cast<USkeletalMesh>(NewObject),
								InParent, 
								Filename, 
								LODIndex
								);

							ImportOptions->bImportMaterials = !!bImportMaterials;
						}

						//add self
						if (NewObject)
						{
							//MMD Extend asset
							CreateMMDExtendFromMMDModel(
								InParent,
								//FName(*NewObject->GetName()),
								Cast<USkeletalMesh>(NewObject),
								&pmxMeshInfoPtr
								);

						}

						//end phese
						if (NewObject)
						{
							TotalNumNodes++;
							NodeIndex++;
							FFormatNamedArguments Args;
							Args.Add(TEXT("NodeIndex"), NodeIndex);
							Args.Add(TEXT("ArrayLength"), 1);// SkelMeshArray.Num());
							GWarn->StatusUpdate(NodeIndex, 1/*SkelMeshArray.Num()*/, FText::Format(NSLOCTEXT("UnrealEd", "Importingf", "Importing ({NodeIndex} of {ArrayLength})"), Args));
						}
#endif
					}
#ifdef DEBUG_MMD_UE4_ORIGINAL_CODE
					for (int32 i = 0; i < SkelMeshArray.Num(); i++)
					{
						delete SkelMeshArray[i];
					}
#endif

					// if total nodes we found is 0, we didn't find anything. 
					if (TotalNumNodes == 0)
					{
						AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FailedToImport_NoMeshFoundOnRoot", "Could not find any valid mesh on the root hierarchy. If you have mesh in the sub hierarchy, please enable option of [Import Meshes In Bone Hierarchy] when import.")),
							FFbxErrors::SkeletalMesh_NoMeshFoundOnRoot);
					}
#endif
				}
			}
			else
			{
#if 1//DEBUG_MMD_UE4_ORIGINAL_CODE
				/*if (RootNodeToImport == NULL)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FailedToImport_InvalidRoot", "Could not find root node.")), FFbxErrors::SkeletalMesh_InvalidRoot);
				}
				else */if (importAssetTypeMMD == E_MMD_TO_UE4_SKELTON)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FailedToImport_InvalidBone", "Failed to find any bone hierarchy. Try disabling the \"Import As Skeletal\" option to import as a rigid mesh. ")), FFbxErrors::SkeletalMesh_InvalidBone);
				}
				else
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FailedToImport_InvalidNode", "Could not find any node.")), FFbxErrors::SkeletalMesh_InvalidNode);
				}
#endif
			}
		}

		if (NewObject == NULL)
		{
			AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FailedToImport_NoObject", "Import failed.")), FFbxErrors::Generic_ImportingNewObjectFailed);
			Warn->Log(ELogVerbosity::Error, "PMX Import ERR [NewObject is NULL]...FLT");
		}

		//FbxImporter->ReleaseScene();
#endif
		Warn->EndSlowTask();
	}
	else
	{
		const FText Message 
			= FText::Format(LOCTEXT("ImportFailed_Generic", 
				"Failed to import '{0}'. Failed to create asset '{1}'\nMMDモデルの読み込みを中止します。\nIM4U Plugin"),
				FText::FromString(*Name.ToString()), FText::FromString(*Name.ToString()));
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		//UE_LOG(LogAssetTools, Warning, TEXT("%s"), *Message.ToString());
	}

	FEditorDelegates::OnAssetPostImport.Broadcast(this, NewObject);

	return NewObject;
}


//////////////////////////////////////////////////////////////////////
USkeletalMesh* UPmxFactory::ImportSkeletalMesh(
	UObject* InParent,
	MMD4UE4::PmxMeshInfo *pmxMeshInfoPtr,
	const FName& Name,
	EObjectFlags Flags,
	//UFbxSkeletalMeshImportData* TemplateImportData,
	FString Filename,
	//TArray<FbxShape*> *FbxShapeArray,
	FSkeletalMeshImportData* OutData,
	bool bCreateRenderData
	)
{
	bool bDiffPose;
	int32 SkelType = 0; // 0 for skeletal mesh, 1 for rigid mesh

#ifdef DEBUG_MMD_UE4_ORIGINAL_CODE
	if (NodeArray.Num() == 0)
	{
		return NULL;
	}

	FbxNode* Node = NodeArray[0];
	// find the mesh by its name
	FbxMesh* FbxMesh = Node->GetMesh();

	if (!FbxMesh)
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_NodeInvalidSkeletalMesh", "Fbx node: '{0}' is not a valid skeletal mesh"), FText::FromString(Node->GetName()))), FFbxErrors::Generic_Mesh_MeshNotFound);
		return NULL;
	}
	if (FbxMesh->GetDeformerCount(FbxDeformer::eSkin) == 0)
	{
		SkelType = 1;
	}

	// warning for missing smoothing group info
	CheckSmoothingInfo(FbxMesh);

	Parent = InParent;


	struct ExistingSkelMeshData* ExistSkelMeshDataPtr = NULL;

	if (!FbxShapeArray)
	{
		UObject* ExistingObject = StaticFindObjectFast(UObject::StaticClass(), InParent, *Name.ToString(), false, false, RF_PendingKill);
		USkeletalMesh* ExistingSkelMesh = Cast<USkeletalMesh>(ExistingObject);

		if (ExistingSkelMesh)
		{
#if WITH_APEX_CLOTHING
			//for supporting re-import 
			ApexClothingUtils::BackupClothingDataFromSkeletalMesh(ExistingSkelMesh);
#endif// #if WITH_APEX_CLOTHING

			ExistingSkelMesh->PreEditChange(NULL);
			ExistSkelMeshDataPtr = SaveExistingSkelMeshData(ExistingSkelMesh);
		}
		// if any other object exists, we can't import with this name
		else if (ExistingObject)
		{
			AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_OverlappingName", "Same name but different class: '{0}' already exists"), FText::FromString(ExistingObject->GetName()))), FFbxErrors::Generic_SameNameAssetExists);
			return NULL;
		}
	}

	// [from USkeletalMeshFactory::FactoryCreateBinary]
	USkeletalMesh* SkeletalMesh = CastChecked<USkeletalMesh>(StaticConstructObject(USkeletalMesh::StaticClass(), InParent, Name, Flags));

	SkeletalMesh->PreEditChange(NULL);

	FSkeletalMeshImportData TempData;
	// Fill with data from buffer - contains the full .FBX file. 	
	FSkeletalMeshImportData* SkelMeshImportDataPtr = &TempData;
	if (OutData)
	{
		SkelMeshImportDataPtr = OutData;
	}

	bool bDiffPose;
	TArray<FbxNode*> SortedLinkArray;
	FbxArray<FbxAMatrix> GlobalsPerLink;

	bool bUseTime0AsRefPose = ImportOptions->bUseT0AsRefPose;
	// Note: importing morph data causes additional passes through this function, so disable the warning dialogs
	// from popping up again on each additional pass.  
	if (!ImportBone(NodeArray, *SkelMeshImportDataPtr, TemplateImportData, SortedLinkArray, bDiffPose, (FbxShapeArray != NULL), bUseTime0AsRefPose))
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FbxSkeletaLMeshimport_MultipleRootFound", "Multiple roots found")), FFbxErrors::SkeletalMesh_MultipleRoots);
		// I can't delete object here since this is middle of import
		// but I can move to transient package, and GC will automatically collect it
		SkeletalMesh->ClearFlags(RF_Standalone);
		SkeletalMesh->Rename(NULL, GetTransientPackage());
		return NULL;
	}

	FbxNode* RootNode = Scene->GetRootNode();
	if (RootNode && TemplateImportData)
	{
		ApplyTransformSettingsToFbxNode(RootNode, TemplateImportData);
	}

	// Create a list of all unique fbx materials.  This needs to be done as a separate pass before reading geometry
	// so that we know about all possible materials before assigning material indices to each triangle
	TArray<FbxSurfaceMaterial*> FbxMaterials;
	for (int32 NodeIndex = 0; NodeIndex < NodeArray.Num(); ++NodeIndex)
	{
		Node = NodeArray[NodeIndex];

		int32 MaterialCount = Node->GetMaterialCount();

		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			FbxSurfaceMaterial* FbxMaterial = Node->GetMaterial(MaterialIndex);
			if (!FbxMaterials.Contains(FbxMaterial))
			{
				FbxMaterials.Add(FbxMaterial);
				// Add an entry for each unique material
				SkelMeshImportDataPtr->Materials.Add(VMaterial());
			}
		}
	}

	for (int32 NodeIndex = 0; NodeIndex < NodeArray.Num(); ++NodeIndex)
	{
		Node = NodeArray[NodeIndex];
		FbxMesh = Node->GetMesh();
		FbxSkin* Skin = (FbxSkin*)FbxMesh->GetDeformer(0, FbxDeformer::eSkin);
		FbxShape* FbxShape = NULL;
		if (FbxShapeArray)
		{
			FbxShape = (*FbxShapeArray)[NodeIndex];
		}

		// NOTE: This function may invalidate FbxMesh and set it to point to a an updated version
		if (!FillSkelMeshImporterFromFbx(*SkelMeshImportDataPtr, FbxMesh, Skin, FbxShape, SortedLinkArray, FbxMaterials))
		{
			// I can't delete object here since this is middle of import
			// but I can move to transient package, and GC will automatically collect it
			SkeletalMesh->ClearFlags(RF_Standalone);
			SkeletalMesh->Rename(NULL, GetTransientPackage());
			return NULL;
		}

		if (bUseTime0AsRefPose && bDiffPose)
		{
			// deform skin vertex to the frame 0 from bind pose
			SkinControlPointsToPose(*SkelMeshImportDataPtr, FbxMesh, FbxShape, true);
		}
	}

	// reorder material according to "SKinXX" in material name
	SetMaterialSkinXXOrder(*SkelMeshImportDataPtr);

	if (ImportOptions->bPreserveSmoothingGroups)
	{
		DoUnSmoothVerts(*SkelMeshImportDataPtr);
	}
	else
	{
		SkelMeshImportDataPtr->PointToRawMap.AddUninitialized(SkelMeshImportDataPtr->Points.Num());
		for (int32 PointIdx = 0; PointIdx<SkelMeshImportDataPtr->Points.Num(); PointIdx++)
		{
			SkelMeshImportDataPtr->PointToRawMap[PointIdx] = PointIdx;
		}
	}

	// process materials from import data
	ProcessImportMeshMaterials(SkeletalMesh->Materials, *SkelMeshImportDataPtr);

	// process reference skeleton from import data
	int32 SkeletalDepth = 0;
	if (!ProcessImportMeshSkeleton(SkeletalMesh->Skeleton, SkeletalMesh->RefSkeleton, SkeletalDepth, *SkelMeshImportDataPtr))
	{
		SkeletalMesh->ClearFlags(RF_Standalone);
		SkeletalMesh->Rename(NULL, GetTransientPackage());
		return NULL;
	}
	UE_LOG(LogFbx, Warning, TEXT("Bones digested - %i  Depth of hierarchy - %i"), SkeletalMesh->RefSkeleton.GetNum(), SkeletalDepth);

	// process bone influences from import data
	ProcessImportMeshInfluences(*SkelMeshImportDataPtr);

	FSkeletalMeshResource* ImportedResource = SkeletalMesh->GetImportedResource();
	check(ImportedResource->LODModels.Num() == 0);
	ImportedResource->LODModels.Empty();
	new(ImportedResource->LODModels)FStaticLODModel();

	SkeletalMesh->LODInfo.Empty();
	SkeletalMesh->LODInfo.AddZeroed();
	SkeletalMesh->LODInfo[0].LODHysteresis = 0.02f;
	FSkeletalMeshOptimizationSettings Settings;
	// set default reduction settings values
	SkeletalMesh->LODInfo[0].ReductionSettings = Settings;

	// Create initial bounding box based on expanded version of reference pose for meshes without physics assets. Can be overridden by artist.
	FBox BoundingBox(SkelMeshImportDataPtr->Points.GetData(), SkelMeshImportDataPtr->Points.Num());
	FBox Temp = BoundingBox;
	FVector MidMesh = 0.5f*(Temp.Min + Temp.Max);
	BoundingBox.Min = Temp.Min + 1.0f*(Temp.Min - MidMesh);
	BoundingBox.Max = Temp.Max + 1.0f*(Temp.Max - MidMesh);
	// Tuck up the bottom as this rarely extends lower than a reference pose's (e.g. having its feet on the floor).
	// Maya has Y in the vertical, other packages have Z.
	//BEN const int32 CoordToTuck = bAssumeMayaCoordinates ? 1 : 2;
	//BEN BoundingBox.Min[CoordToTuck]	= Temp.Min[CoordToTuck] + 0.1f*(Temp.Min[CoordToTuck] - MidMesh[CoordToTuck]);
	BoundingBox.Min[2] = Temp.Min[2] + 0.1f*(Temp.Min[2] - MidMesh[2]);
	SkeletalMesh->Bounds = FBoxSphereBounds(BoundingBox);

	// Store whether or not this mesh has vertex colors
	SkeletalMesh->bHasVertexColors = SkelMeshImportDataPtr->bHasVertexColors;

	FStaticLODModel& LODModel = ImportedResource->LODModels[0];

	// Pass the number of texture coordinate sets to the LODModel.  Ensure there is at least one UV coord
	LODModel.NumTexCoords = FMath::Max<uint32>(1, SkelMeshImportDataPtr->NumTexCoords);

	if (bCreateRenderData)
	{
		TArray<FVector> LODPoints;
		TArray<FMeshWedge> LODWedges;
		TArray<FMeshFace> LODFaces;
		TArray<FVertInfluence> LODInfluences;
		TArray<int32> LODPointToRawMap;
		SkelMeshImportDataPtr->CopyLODImportData(LODPoints, LODWedges, LODFaces, LODInfluences, LODPointToRawMap);

		const bool bShouldComputeNormals = !ImportOptions->ShouldImportNormals() || !SkelMeshImportDataPtr->bHasNormals;
		const bool bShouldComputeTangents = !ImportOptions->ShouldImportTangents() || !SkelMeshImportDataPtr->bHasTangents;

		IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");

		TArray<FText> WarningMessages;
		TArray<FName> WarningNames;
		// Create actual rendering data.
		if (!MeshUtilities.BuildSkeletalMesh(ImportedResource->LODModels[0], SkeletalMesh->RefSkeleton, LODInfluences, LODWedges, LODFaces, LODPoints, LODPointToRawMap, ImportOptions->bKeepOverlappingVertices, bShouldComputeNormals, bShouldComputeTangents, &WarningMessages, &WarningNames))
		{
			if (WarningNames.Num() == WarningMessages.Num())
			{
				// temporary hack of message/names, should be one token or a struct
				for (int32 MessageIdx = 0; MessageIdx<WarningMessages.Num(); ++MessageIdx)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, WarningMessages[MessageIdx]), WarningNames[MessageIdx]);
				}
			}

			SkeletalMesh->MarkPendingKill();
			return NULL;
		}
		else if (WarningMessages.Num() > 0)
		{
			// temporary hack of message/names, should be one token or a struct
			if (WarningNames.Num() == WarningMessages.Num())
			{
				// temporary hack of message/names, should be one token or a struct
				for (int32 MessageIdx = 0; MessageIdx<WarningMessages.Num(); ++MessageIdx)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, WarningMessages[MessageIdx]), WarningNames[MessageIdx]);
				}
			}
		}

		// Presize the per-section shadow casting array with the number of sections in the imported LOD.
		const int32 NumSections = LODModel.Sections.Num();
		for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
		{
			SkeletalMesh->LODInfo[0].TriangleSortSettings.AddZeroed();
		}

		if (ExistSkelMeshDataPtr)
		{
			RestoreExistingSkelMeshData(ExistSkelMeshDataPtr, SkeletalMesh);
		}

		// Store the current file path and timestamp for re-import purposes
		UFbxSkeletalMeshImportData* ImportData = UFbxSkeletalMeshImportData::GetImportDataForSkeletalMesh(SkeletalMesh, TemplateImportData);
		SkeletalMesh->AssetImportData->SourceFilePath = FReimportManager::SanitizeImportFilename(UFactory::CurrentFilename, SkeletalMesh);
		SkeletalMesh->AssetImportData->SourceFileTimestamp = IFileManager::Get().GetTimeStamp(*UFactory::CurrentFilename).ToString();
		SkeletalMesh->AssetImportData->bDirty = false;

		SkeletalMesh->CalculateInvRefMatrices();
		SkeletalMesh->PostEditChange();
		SkeletalMesh->MarkPackageDirty();

		// Now iterate over all skeletal mesh components re-initialising them.
		for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
		{
			USkeletalMeshComponent* SkelComp = *It;
			if (SkelComp->SkeletalMesh == SkeletalMesh)
			{
				FComponentReregisterContext ReregisterContext(SkelComp);
			}
		}
	}

	if (InParent != GetTransientPackage())
	{
		// Create PhysicsAsset if requested and if physics asset is null
		if (ImportOptions->bCreatePhysicsAsset)
		{
			if (SkeletalMesh->PhysicsAsset == NULL)
			{
				FString ObjectName = FString::Printf(TEXT("%s_PhysicsAsset"), *SkeletalMesh->GetName());
				UPhysicsAsset * NewPhysicsAsset = CreateAsset<UPhysicsAsset>(InParent->GetName(), ObjectName, true);
				if (!NewPhysicsAsset)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_CouldNotCreatePhysicsAsset", "Could not create Physics Asset ('{0}') for '{1}'"), FText::FromString(ObjectName), FText::FromString(SkeletalMesh->GetName()))), FFbxErrors::SkeletalMesh_FailedToCreatePhyscisAsset);
				}
				else
				{
					FPhysAssetCreateParams NewBodyData;
					NewBodyData.Initialize();
					FText CreationErrorMessage;
					bool bSuccess = FPhysicsAssetUtils::CreateFromSkeletalMesh(NewPhysicsAsset, SkeletalMesh, NewBodyData, CreationErrorMessage);
					if (!bSuccess)
					{
						AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, CreationErrorMessage), FFbxErrors::SkeletalMesh_FailedToCreatePhyscisAsset);
						// delete the asset since we could not have create physics asset
						TArray<UObject*> ObjectsToDelete;
						ObjectsToDelete.Add(NewPhysicsAsset);
						ObjectTools::DeleteObjects(ObjectsToDelete, false);
					}
				}
			}
		}
		// if physics asset is selected
		else if (ImportOptions->PhysicsAsset)
		{
			SkeletalMesh->PhysicsAsset = ImportOptions->PhysicsAsset;
		}

		// see if we have skeleton set up
		// if creating skeleton, create skeleeton
		USkeleton* Skeleton = ImportOptions->SkeletonForAnimation;
		if (Skeleton == NULL)
		{
			FString ObjectName = FString::Printf(TEXT("%s_Skeleton"), *SkeletalMesh->GetName());
			Skeleton = CreateAsset<USkeleton>(InParent->GetName(), ObjectName, true);
			if (!Skeleton)
			{
				// same object exists, try to see if it's skeleton, if so, load
				Skeleton = LoadObject<USkeleton>(InParent, *ObjectName);

				// if not skeleton, we're done, we can't create skeleton with same name
				// @todo in the future, we'll allow them to rename
				if (!Skeleton)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_SkeletonRecreateError", "'{0}' already exists. It fails to recreate it."), FText::FromString(ObjectName))), FFbxErrors::SkeletalMesh_SkeletonRecreateError);
					return SkeletalMesh;
				}
			}
		}

		// merge bones to the selected skeleton
		if (!Skeleton->MergeAllBonesToBoneTree(SkeletalMesh))
		{
			if (EAppReturnType::Yes == FMessageDialog::Open(EAppMsgType::YesNo,
				LOCTEXT("SkeletonFailed_BoneMerge", "FAILED TO MERGE BONES:\n\n This could happen if significant hierarchical change has been made\n - i.e. inserting bone between nodes\n Would you like to regenerate Skeleton from this mesh? \n\n ***WARNING: THIS WILL REQUIRE RECOMPRESS ALL ANIMATION DATA AND POTENTIALLY INVALIDATE***\n")))
			{
				if (Skeleton->RecreateBoneTree(SkeletalMesh))
				{
					FAssetNotifications::SkeletonNeedsToBeSaved(Skeleton);
				}
			}
		}
		else
		{
			// ask if they'd like to update their position form this mesh
			if (ImportOptions->SkeletonForAnimation && ImportOptions->bUpdateSkeletonReferencePose)
			{
				Skeleton->UpdateReferencePoseFromMesh(SkeletalMesh);
				FAssetNotifications::SkeletonNeedsToBeSaved(Skeleton);
			}
		}
	}
#endif


	//bool bCreateRenderData = true;
	struct ExistingSkelMeshData* ExistSkelMeshDataPtr = NULL;

	if (true /*!FbxShapeArray*/)
	{
		//UObject* ExistingObject = StaticFindObjectFast(UObject::StaticClass(), InParent, *Name.ToString(), false, false, RF_PendingKill);//~UE4.10
		UObject* ExistingObject = StaticFindObjectFast(UObject::StaticClass(), InParent, *Name.ToString(), false, false, EObjectFlags::RF_NoFlags, EInternalObjectFlags::PendingKill);//UE4.11~
		USkeletalMesh* ExistingSkelMesh = Cast<USkeletalMesh>(ExistingObject);

		if (ExistingSkelMesh)
		{

			ExistingSkelMesh->PreEditChange(NULL);
			//ExistSkelMeshDataPtr = SaveExistingSkelMeshData(ExistingSkelMesh);
		}
		// if any other object exists, we can't import with this name
		else if (ExistingObject)
		{
			AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_OverlappingName", "Same name but different class: '{0}' already exists"), FText::FromString(ExistingObject->GetName()))), FFbxErrors::Generic_SameNameAssetExists);
			return NULL;
		}
	}

	// [from USkeletalMeshFactory::FactoryCreateBinary]
	USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(InParent, Name, Flags);

	SkeletalMesh->PreEditChange(NULL);

	FSkeletalMeshImportData TempData;
	// Fill with data from buffer - contains the full .FBX file. 	
	FSkeletalMeshImportData* SkelMeshImportDataPtr = &TempData;
	if (OutData)
	{
		SkelMeshImportDataPtr = OutData;
	}

	/*Import Bone Start*/
	bool bUseTime0AsRefPose = false;// ImportOptions->bUseT0AsRefPose;
	// Note: importing morph data causes additional passes through this function, so disable the warning dialogs
	// from popping up again on each additional pass.  
	if (
		!ImportBone(
			//NodeArray, 
			pmxMeshInfoPtr,
			*SkelMeshImportDataPtr,
			//TemplateImportData, 
			//SortedLinkArray,
			bDiffPose, 
			false,//(FbxShapeArray != NULL),
			bUseTime0AsRefPose)
			
			/*false*/
		)
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(
			EMessageSeverity::Error, 
			LOCTEXT("FbxSkeletaLMeshimport_MultipleRootFound", "Multiple roots found")),
			FFbxErrors::SkeletalMesh_MultipleRoots);
		// I can't delete object here since this is middle of import
		// but I can move to transient package, and GC will automatically collect it
		SkeletalMesh->ClearFlags(RF_Standalone);
		SkeletalMesh->Rename(NULL, GetTransientPackage());
		return NULL;
	}
	/*Import Bone End*/

	/*
	// Inport  Material @@@@@

	// Create a list of all unique fbx materials.  This needs to be done as a separate pass before reading geometry
	// so that we know about all possible materials before assigning material indices to each triangle
	TArray<FbxSurfaceMaterial*> FbxMaterials;
	for (int32 NodeIndex = 0; NodeIndex < NodeArray.Num(); ++NodeIndex)
	{
		Node = NodeArray[NodeIndex];

		int32 MaterialCount = Node->GetMaterialCount();

		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			FbxSurfaceMaterial* FbxMaterial = Node->GetMaterial(MaterialIndex);
			if (!FbxMaterials.Contains(FbxMaterial))
			{
				FbxMaterials.Add(FbxMaterial);
				// Add an entry for each unique material
				SkelMeshImportDataPtr->Materials.Add(VMaterial());
			}
		}
	}
	*/

	for (int32 MaterialIndex = 0; MaterialIndex < pmxMeshInfoPtr->materialList.Num(); ++MaterialIndex)
	{
		// Add an entry for each unique material
		SkeletalMeshImportData::FMaterial NewMaterial;
		SkelMeshImportDataPtr->Materials.Add(NewMaterial);
	}
	/*
	for (int32 NodeIndex = 0; NodeIndex < NodeArray.Num(); ++NodeIndex)
	{
		Node = NodeArray[NodeIndex];
		FbxMesh = Node->GetMesh();
		FbxSkin* Skin = (FbxSkin*)FbxMesh->GetDeformer(0, FbxDeformer::eSkin);
		FbxShape* FbxShape = NULL;
		if (FbxShapeArray)
		{
			FbxShape = (*FbxShapeArray)[NodeIndex];
		}

		// NOTE: This function may invalidate FbxMesh and set it to point to a an updated version
		if (!FillSkelMeshImporterFromFbx(*SkelMeshImportDataPtr, FbxMesh, Skin, FbxShape, SortedLinkArray, FbxMaterials))
		{
			// I can't delete object here since this is middle of import
			// but I can move to transient package, and GC will automatically collect it
			SkeletalMesh->ClearFlags(RF_Standalone);
			SkeletalMesh->Rename(NULL, GetTransientPackage());
			return NULL;
		}

		if (bUseTime0AsRefPose && bDiffPose)
		{
			// deform skin vertex to the frame 0 from bind pose
			SkinControlPointsToPose(*SkelMeshImportDataPtr, FbxMesh, FbxShape, true);
		}
	}
	*/
	// NOTE: This function may invalidate FbxMesh and set it to point to a an updated version
	if (
		!FillSkelMeshImporterFromFbx(
			*SkelMeshImportDataPtr,
			pmxMeshInfoPtr
			/*
			FbxMesh,
			Skin, 
			FbxShape,
			SortedLinkArray, 
			FbxMaterials*/
			)
		)
		/*false*/
	{
		// I can't delete object here since this is middle of import
		// but I can move to transient package, and GC will automatically collect it
		SkeletalMesh->ClearFlags(RF_Standalone);
		SkeletalMesh->Rename(NULL, GetTransientPackage());
		return NULL;
	}

	// reorder material according to "SKinXX" in material name
	/*SetMaterialSkinXXOrder(*SkelMeshImportDataPtr);
*/
	if (/*ImportOptions->bPreserveSmoothingGroups*/
		false//true
		)
	{
		//DoUnSmoothVerts(*SkelMeshImportDataPtr);
	}
	else
	{
		SkelMeshImportDataPtr->PointToRawMap.AddUninitialized(SkelMeshImportDataPtr->Points.Num());
		for (int32 PointIdx = 0; PointIdx<SkelMeshImportDataPtr->Points.Num(); PointIdx++)
		{
			SkelMeshImportDataPtr->PointToRawMap[PointIdx] = PointIdx;
		}
	}

	// process materials from import data
	ProcessImportMeshMaterials(SkeletalMesh->Materials, *SkelMeshImportDataPtr);

	// process reference skeleton from import data
	int32 SkeletalDepth = 0;
	if (!ProcessImportMeshSkeleton(SkeletalMesh->Skeleton, SkeletalMesh->RefSkeleton, SkeletalDepth, *SkelMeshImportDataPtr))
	{
		SkeletalMesh->ClearFlags(RF_Standalone);
		SkeletalMesh->Rename(NULL, GetTransientPackage());
		return NULL;
	}
	UE_LOG(LogMMD4UE4_PMXFactory, Warning, TEXT("Bones digested - %i  Depth of hierarchy - %i"), SkeletalMesh->RefSkeleton.GetNum(), SkeletalDepth);

	// process bone influences from import data
	ProcessImportMeshInfluences(*SkelMeshImportDataPtr);

	FSkeletalMeshModel* ImportedResource = SkeletalMesh->GetImportedModel();
	check(ImportedResource->LODModels.Num() == 0);
	ImportedResource->LODModels.Empty();
	new(ImportedResource->LODModels)FSkeletalMeshLODModel();

	SkeletalMesh->ResetLODInfo();
	FSkeletalMeshLODInfo & NewLODInfo = SkeletalMesh->AddLODInfo();
	NewLODInfo.LODHysteresis = 0.02f;

	// Create initial bounding box based on expanded version of reference pose for meshes without physics assets. Can be overridden by artist.
	FBox BoundingBox(SkelMeshImportDataPtr->Points.GetData(), SkelMeshImportDataPtr->Points.Num());
	FBox Temp = BoundingBox;
	FVector MidMesh = 0.5f*(Temp.Min + Temp.Max);
	BoundingBox.Min = Temp.Min + 1.0f*(Temp.Min - MidMesh);
	BoundingBox.Max = Temp.Max + 1.0f*(Temp.Max - MidMesh);
	// Tuck up the bottom as this rarely extends lower than a reference pose's (e.g. having its feet on the floor).
	// Maya has Y in the vertical, other packages have Z.
	//BEN const int32 CoordToTuck = bAssumeMayaCoordinates ? 1 : 2;
	//BEN BoundingBox.Min[CoordToTuck]	= Temp.Min[CoordToTuck] + 0.1f*(Temp.Min[CoordToTuck] - MidMesh[CoordToTuck]);
	BoundingBox.Min[2] = Temp.Min[2] + 0.1f*(Temp.Min[2] - MidMesh[2]);
#if 0 /* under ~ UE4.11 */
	SkeletalMesh->Bounds = FBoxSphereBounds(BoundingBox);
#else /* over UE4.12 ~*/
	SkeletalMesh->SetImportedBounds( FBoxSphereBounds(BoundingBox) );
#endif

	// Store whether or not this mesh has vertex colors
	SkeletalMesh->bHasVertexColors = SkelMeshImportDataPtr->bHasVertexColors;
	SkeletalMesh->VertexColorGuid = SkeletalMesh->bHasVertexColors ? FGuid::NewGuid() : FGuid();

	FSkeletalMeshLODModel& LODModel = ImportedResource->LODModels[0];

	// Pass the number of texture coordinate sets to the LODModel.  Ensure there is at least one UV coord
	LODModel.NumTexCoords = FMath::Max<uint32>(1, SkelMeshImportDataPtr->NumTexCoords);
#if 1
	if (bCreateRenderData)
	{
		TArray<FVector> LODPoints;
		TArray<SkeletalMeshImportData::FMeshWedge> LODWedges;
		TArray<SkeletalMeshImportData::FMeshFace> LODFaces;
		TArray<SkeletalMeshImportData::FVertInfluence> LODInfluences;
		TArray<int32> LODPointToRawMap;
		SkelMeshImportDataPtr->CopyLODImportData(LODPoints, LODWedges, LODFaces, LODInfluences, LODPointToRawMap);

		const bool bShouldComputeNormals = true/*!ImportOptions->ShouldImportNormals()*/ || !SkelMeshImportDataPtr->bHasNormals;
		const bool bShouldComputeTangents = true/*!ImportOptions->ShouldImportTangents()*/ || !SkelMeshImportDataPtr->bHasTangents;

		IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");

		TArray<FText> WarningMessages;
		TArray<FName> WarningNames;
		// Create actual rendering data.
#if 0 /* under ~ UE4.10 */
		if (!MeshUtilities.BuildSkeletalMesh(
			ImportedResource->LODModels[0], 
			SkeletalMesh->RefSkeleton,
			LODInfluences,
			LODWedges,
			LODFaces,
			LODPoints, 
			LODPointToRawMap,
			false,//ImportOptions->bKeepOverlappingVertices, 
			bShouldComputeNormals, 
			bShouldComputeTangents,
			&WarningMessages, 
			&WarningNames)
			)
#else /* UE4.11~ over */
		if (!MeshUtilities.BuildSkeletalMesh(
			ImportedResource->LODModels[0],
			SkeletalMesh->RefSkeleton,
			LODInfluences,
			LODWedges,
			LODFaces,
			LODPoints,
			LODPointToRawMap)
			)
#endif
		{
			if (WarningNames.Num() == WarningMessages.Num())
			{
				// temporary hack of message/names, should be one token or a struct
				for (int32 MessageIdx = 0; MessageIdx<WarningMessages.Num(); ++MessageIdx)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, WarningMessages[MessageIdx]), WarningNames[MessageIdx]);
				}
			}

			SkeletalMesh->MarkPendingKill();
			return NULL;
		}
		else if (WarningMessages.Num() > 0)
		{
			// temporary hack of message/names, should be one token or a struct
			if (WarningNames.Num() == WarningMessages.Num())
			{
				// temporary hack of message/names, should be one token or a struct
				for (int32 MessageIdx = 0; MessageIdx<WarningMessages.Num(); ++MessageIdx)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, WarningMessages[MessageIdx]), WarningNames[MessageIdx]);
				}
			}
		}

		// Presize the per-section shadow casting array with the number of sections in the imported LOD.
		const int32 NumSections = LODModel.Sections.Num();
		for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
		{
			SkeletalMesh->GetLODInfo(0)->LODMaterialMap.Add(0);
		}

		if (ExistSkelMeshDataPtr)
		{
//			RestoreExistingSkelMeshData(ExistSkelMeshDataPtr, SkeletalMesh);
		}

		// Store the current file path and timestamp for re-import purposes
		/*UFbxSkeletalMeshImportData* ImportData = UFbxSkeletalMeshImportData::GetImportDataForSkeletalMesh(SkeletalMesh, TemplateImportData);
		SkeletalMesh->AssetImportData->SourceFilePath = FReimportManager::SanitizeImportFilename(UFactory::CurrentFilename, SkeletalMesh);
		SkeletalMesh->AssetImportData->SourceFileTimestamp = IFileManager::Get().GetTimeStamp(*UFactory::CurrentFilename).ToString();
		SkeletalMesh->AssetImportData->bDirty = false;
		*/
		SkeletalMesh->AssetImportData->Update(UFactory::CurrentFilename);

		SkeletalMesh->CalculateInvRefMatrices();
		SkeletalMesh->PostEditChange();
		SkeletalMesh->MarkPackageDirty();

		// Now iterate over all skeletal mesh components re-initialising them.
		for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
		{
			USkeletalMeshComponent* SkelComp = *It;
			if (SkelComp->SkeletalMesh == SkeletalMesh)
			{
				FComponentReregisterContext ReregisterContext(SkelComp);
			}
		}
	}
#endif
#if 1 //phy
	if (InParent != GetTransientPackage())
	{
		// Create PhysicsAsset if requested and if physics asset is null
		if (ImportUI->bCreatePhysicsAsset)
		{
			if (SkeletalMesh->PhysicsAsset == NULL)
			{
				FString ObjectName = FString::Printf(TEXT("%s_PhysicsAsset"), *SkeletalMesh->GetName());
				UPhysicsAsset * NewPhysicsAsset = CreateAsset<UPhysicsAsset>(InParent->GetName(), ObjectName, true); 
				if (!NewPhysicsAsset)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_CouldNotCreatePhysicsAsset", "Could not create Physics Asset ('{0}') for '{1}'"), FText::FromString(ObjectName), FText::FromString(SkeletalMesh->GetName()))), FFbxErrors::SkeletalMesh_FailedToCreatePhyscisAsset);
				}
				else
				{
					FPhysAssetCreateParams NewBodyData;
					FText CreationErrorMessage;
					bool bSuccess
						= FPhysicsAssetUtils::CreateFromSkeletalMesh(
								NewPhysicsAsset, 
								SkeletalMesh, 
								NewBodyData, 
								CreationErrorMessage
								);
					if (!bSuccess)
					{
						AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, CreationErrorMessage), FFbxErrors::SkeletalMesh_FailedToCreatePhyscisAsset);
						// delete the asset since we could not have create physics asset
						TArray<UObject*> ObjectsToDelete;
						ObjectsToDelete.Add(NewPhysicsAsset);
						ObjectTools::DeleteObjects(ObjectsToDelete, false);
					}
				}
			}
		}
		/*/ if physics asset is selected
		/else if (ImportOptions->PhysicsAsset)
		{
			SkeletalMesh->PhysicsAsset = ImportOptions->PhysicsAsset;
		}*/

		// see if we have skeleton set up
		// if creating skeleton, create skeleeton
		USkeleton* Skeleton = NULL;
		//Skeleton = ImportOptions->SkeletonForAnimation;
		if (Skeleton == NULL)
		{
			FString ObjectName = FString::Printf(TEXT("%s_Skeleton"), *SkeletalMesh->GetName());
			Skeleton = CreateAsset<USkeleton>(InParent->GetName(), ObjectName, true); 
			if (!Skeleton)
			{
				// same object exists, try to see if it's skeleton, if so, load
				Skeleton = LoadObject<USkeleton>(InParent, *ObjectName);

				// if not skeleton, we're done, we can't create skeleton with same name
				// @todo in the future, we'll allow them to rename
				if (!Skeleton)
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_SkeletonRecreateError", "'{0}' already exists. It fails to recreate it."), FText::FromString(ObjectName))), FFbxErrors::SkeletalMesh_SkeletonRecreateError);
					return SkeletalMesh;
				}
			}
		}
		
		// merge bones to the selected skeleton
		if ( !Skeleton->MergeAllBonesToBoneTree( SkeletalMesh ) )
		{
			if (EAppReturnType::Yes == FMessageDialog::Open(EAppMsgType::YesNo,
				LOCTEXT("SkeletonFailed_BoneMerge", "FAILED TO MERGE BONES:\n\n This could happen if significant hierarchical change has been made\n - i.e. inserting bone between nodes\n Would you like to regenerate Skeleton from this mesh? \n\n ***WARNING: THIS WILL REQUIRE RECOMPRESS ALL ANIMATION DATA AND POTENTIALLY INVALIDATE***\n")))
			{
				if (Skeleton->RecreateBoneTree(SkeletalMesh))
				{
					FAssetNotifications::SkeletonNeedsToBeSaved(Skeleton);
				}
			}
		}
		else
		{
			/*
			// ask if they'd like to update their position form this mesh
			if ( ImportOptions->SkeletonForAnimation && ImportOptions->bUpdateSkeletonReferencePose ) 
			{
				Skeleton->UpdateReferencePoseFromMesh(SkeletalMesh);
				FAssetNotifications::SkeletonNeedsToBeSaved(Skeleton);
			}
			*/
		}
		if (SkeletalMesh->Skeleton != Skeleton)
		{
			SkeletalMesh->Skeleton = Skeleton;
			SkeletalMesh->MarkPackageDirty();
		}
	}
#endif
	return SkeletalMesh;
}


UMMDExtendAsset * UPmxFactory::CreateMMDExtendFromMMDModel(
	UObject* InParent,
	USkeletalMesh* SkeletalMesh, // issue #2: fix param use skeleton mesh
	MMD4UE4::PmxMeshInfo * PmxMeshInfo
	)
{
	UMMDExtendAsset * NewMMDExtendAsset = NULL;

	//Add UE4.9
	if (SkeletalMesh->Skeleton == NULL)
	{
		return NULL;
	}
	check(SkeletalMesh->Skeleton);

	//issue #2 : Fix MMDExtend IK Index
	const FReferenceSkeleton ReferenceSkeleton = SkeletalMesh->Skeleton->GetReferenceSkeleton();
	const FName& Name = FName(*SkeletalMesh->GetName());

	//MMD Extend asset

	// TBD::アセット生成関数で既存アセット時の判断ができていないと思われる。
	// 場合によってはVMDFactoryのアセット生成処理を元に再設計すること
	FString ObjectName = FString::Printf(TEXT("%s_MMDExtendAsset"), *Name.ToString());
	NewMMDExtendAsset = CreateAsset<UMMDExtendAsset>(InParent->GetName(), ObjectName, true);
	if (!NewMMDExtendAsset)
	{

		// same object exists, try to see if it's asset, if so, load
		NewMMDExtendAsset = LoadObject<UMMDExtendAsset>(InParent, *ObjectName);

		if (!NewMMDExtendAsset)
		{
			AddTokenizedErrorMessage(
				FTokenizedMessage::Create(
				EMessageSeverity::Warning,
				FText::Format(LOCTEXT("CouldNotCreateMMDExtendAsset",
				"Could not create MMD Extend Asset ('{0}') for '{1}'"),
				FText::FromString(ObjectName),
				FText::FromString(Name.ToString()))
				),
				FFbxErrors::SkeletalMesh_FailedToCreatePhyscisAsset);
		}
		else
		{
			NewMMDExtendAsset->IkInfoList.Empty();
		}
	}
	
	//create asset info
	if (NewMMDExtendAsset)
	{
		if (NewMMDExtendAsset->IkInfoList.Num() > 0)
		{
			NewMMDExtendAsset->IkInfoList.Empty();
		}
		//create IK
		//mapping
		NewMMDExtendAsset->ModelName = PmxMeshInfo->modelNameJP;
		NewMMDExtendAsset->ModelComment = FText::FromString( PmxMeshInfo->modelCommentJP);
		//
		for (int boneIdx = 0; boneIdx < PmxMeshInfo->boneList.Num(); ++boneIdx)
		{
			//check IK bone 
			if (PmxMeshInfo->boneList[boneIdx].Flag_IK)
			{
				MMD4UE4::PMX_IK * tempPmxIKPtr = &PmxMeshInfo->boneList[boneIdx].IKInfo;
				FMMD_IKInfo addMMDIkInfo;

				addMMDIkInfo.LoopNum = tempPmxIKPtr->LoopNum;
				//set limit rot[rad]
				addMMDIkInfo.RotLimit = tempPmxIKPtr->RotLimit;
				// this bone
				addMMDIkInfo.IKBoneName = FName(*PmxMeshInfo->boneList[boneIdx].Name);
				//issue #2: Fix IK bone index 
				//this bone(ik-bone) index, from skeleton.
				addMMDIkInfo.IKBoneIndex = ReferenceSkeleton.FindBoneIndex(addMMDIkInfo.IKBoneName);
				//ik target 
				addMMDIkInfo.TargetBoneName = FName(*PmxMeshInfo->boneList[tempPmxIKPtr->TargetBoneIndex].Name);
				//issue #2: Fix Target Bone Index 
				//target bone(ik-target bone) index, from skeleton.
				addMMDIkInfo.TargetBoneIndex = ReferenceSkeleton.FindBoneIndex(addMMDIkInfo.TargetBoneName);
				//set sub ik
				addMMDIkInfo.ikLinkList.AddZeroed(tempPmxIKPtr->LinkNum);
				for (int ikInfoID = 0; ikInfoID < tempPmxIKPtr->LinkNum; ++ikInfoID)
				{
					//link bone index
					addMMDIkInfo.ikLinkList[ikInfoID].BoneName
						= FName(*PmxMeshInfo->boneList[tempPmxIKPtr->Link[ikInfoID].BoneIndex].Name);
					//issue #2: Fix link bone index
					//link bone index from skeleton.
					addMMDIkInfo.ikLinkList[ikInfoID].BoneIndex
						= ReferenceSkeleton.FindBoneIndex(addMMDIkInfo.ikLinkList[ikInfoID].BoneName);
					//limit flag
					addMMDIkInfo.ikLinkList[ikInfoID].RotLockFlag = tempPmxIKPtr->Link[ikInfoID].RotLockFlag;
					//min
					addMMDIkInfo.ikLinkList[ikInfoID].RotLockMin.X = tempPmxIKPtr->Link[ikInfoID].RotLockMin[0];
					addMMDIkInfo.ikLinkList[ikInfoID].RotLockMin.Y = tempPmxIKPtr->Link[ikInfoID].RotLockMin[1];
					addMMDIkInfo.ikLinkList[ikInfoID].RotLockMin.Z = tempPmxIKPtr->Link[ikInfoID].RotLockMin[2];
					//max
					addMMDIkInfo.ikLinkList[ikInfoID].RotLockMax.X = tempPmxIKPtr->Link[ikInfoID].RotLockMax[0];
					addMMDIkInfo.ikLinkList[ikInfoID].RotLockMax.Y = tempPmxIKPtr->Link[ikInfoID].RotLockMax[1];
					addMMDIkInfo.ikLinkList[ikInfoID].RotLockMax.Z = tempPmxIKPtr->Link[ikInfoID].RotLockMax[2];
				}
				//add
				NewMMDExtendAsset->IkInfoList.Add(addMMDIkInfo);
			}
		}
		// 
		NewMMDExtendAsset->MarkPackageDirty();
	}

	return NewMMDExtendAsset;
}

#undef LOCTEXT_NAMESPACE
