﻿// Copyright 2015 BlackMa9. All Rights Reserved.

#include "IM4UPrivatePCH.h"


#include "CoreMinimal.h"
#include "Factories.h"
#include "BusyCursor.h"
#include "SSkeletonWidget.h"

//#include "FbxImporter.h"

#include "Misc/FbxErrors.h"
#include "AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Rendering/SkeletalMeshModel.h"

/////////////////////////

#include "Engine.h"
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

#include "Animation/MorphTarget.h"
#include "ComponentReregisterContext.h"
////////////

#include "PmxFactory.h"
#define LOCTEXT_NAMESPACE "PMXSkeltalMeshImpoter"

////////////////////////////////////////////////////////////////////////////////////////////////
// FMorphMeshRawSource is removed after version 4.16. So added for only this plugin here.
// Converts a mesh to raw vertex data used to generate a morph target mesh

/** compare based on base mesh source vertex indices */
struct FCompareMorphTargetDeltas
{
	FORCEINLINE bool operator()(const FMorphTargetDelta& A, const FMorphTargetDelta& B) const
	{
		return ((int32)A.SourceIdx - (int32)B.SourceIdx) < 0 ? true : false;
	}
};

class FMorphMeshRawSource
{
public:
	struct FMorphMeshVertexRaw
	{
		FVector			Position;

		// Tangent, U-direction
		FVector			TangentX;
		// Binormal, V-direction
		FVector			TangentY;
		// Normal
		FVector4		TangentZ;
	};

	/** vertex data used for comparisons */
	TArray<FMorphMeshVertexRaw> Vertices;

	/** index buffer used for comparison */
	TArray<uint32> Indices;

	/** indices to original imported wedge points */
	TArray<uint32> WedgePointIndices;

	/** Constructor (default) */
	FMorphMeshRawSource() { }
	FMorphMeshRawSource(USkeletalMesh* SrcMesh, int32 LODIndex = 0);
	FMorphMeshRawSource(FSkeletalMeshLODModel& LODModel);

	static void CalculateMorphTargetLODModel(const FMorphMeshRawSource& BaseSource,
		const FMorphMeshRawSource& TargetSource, FMorphTargetLODModel& MorphModel);

private:
	void Initialize(FSkeletalMeshLODModel& LODModel);
};

/**
* Constructor.
* Converts a skeletal mesh to raw vertex data
* needed for creating a morph target mesh
*
* @param	SrcMesh - source skeletal mesh to convert
* @param	LODIndex - level of detail to use for the geometry
*/
FMorphMeshRawSource::FMorphMeshRawSource(USkeletalMesh* SrcMesh, int32 LODIndex)
{
	check(SrcMesh);
	check(SrcMesh->GetImportedModel());
	check(SrcMesh->GetImportedModel()->LODModels.IsValidIndex(LODIndex));

	// get the mesh data for the given lod
	FSkeletalMeshLODModel& LODModel = SrcMesh->GetImportedModel()->LODModels[LODIndex];

	Initialize(LODModel);
}

FMorphMeshRawSource::FMorphMeshRawSource(FSkeletalMeshLODModel& LODModel)
{
	Initialize(LODModel);
}

void FMorphMeshRawSource::Initialize(FSkeletalMeshLODModel& LODModel)
{
	// iterate over the chunks for the skeletal mesh
	for (int32 SectionIdx = 0; SectionIdx < LODModel.Sections.Num(); SectionIdx++)
	{
		const FSkelMeshSection& Section = LODModel.Sections[SectionIdx];
		for (int32 VertexIdx = 0; VertexIdx < Section.SoftVertices.Num(); VertexIdx++)
		{
			const FSoftSkinVertex& SourceVertex = Section.SoftVertices[VertexIdx];
			FMorphMeshVertexRaw RawVertex =
			{
				SourceVertex.Position,
				SourceVertex.TangentX,
				SourceVertex.TangentY,
				SourceVertex.TangentZ
			};
			Vertices.Add(RawVertex);
		}
	}

	// Copy the indices manually, since the LODModel's index buffer may have a different alignment.
	Indices.Empty(LODModel.IndexBuffer.Num());
	for (int32 Index = 0; Index < LODModel.IndexBuffer.Num(); Index++)
	{
		Indices.Add(LODModel.IndexBuffer[Index]);
	}

	// copy the wedge point indices
	if (LODModel.RawPointIndices.GetBulkDataSize())
	{
		WedgePointIndices.Empty(LODModel.RawPointIndices.GetElementCount());
		WedgePointIndices.AddUninitialized(LODModel.RawPointIndices.GetElementCount());
		FMemory::Memcpy(WedgePointIndices.GetData(), LODModel.RawPointIndices.Lock(LOCK_READ_ONLY), LODModel.RawPointIndices.GetBulkDataSize());
		LODModel.RawPointIndices.Unlock();
	}
}

void FMorphMeshRawSource::CalculateMorphTargetLODModel(const FMorphMeshRawSource& BaseSource,
	const FMorphMeshRawSource& TargetSource, FMorphTargetLODModel& MorphModel)
{
	// set the original number of vertices
	MorphModel.NumBaseMeshVerts = BaseSource.Vertices.Num();

	// empty morph mesh vertices first
	MorphModel.Vertices.Empty();

	// array to mark processed base vertices
	TArray<bool> WasProcessed;
	WasProcessed.Empty(BaseSource.Vertices.Num());
	WasProcessed.AddZeroed(BaseSource.Vertices.Num());


	TMap<uint32, uint32> WedgePointToVertexIndexMap;
	// Build a mapping of wedge point indices to vertex indices for fast lookup later.
	for (int32 Idx = 0; Idx < TargetSource.WedgePointIndices.Num(); Idx++)
	{
		WedgePointToVertexIndexMap.Add(TargetSource.WedgePointIndices[Idx], Idx);
	}

	// iterate over all the base mesh indices
	for (int32 Idx = 0; Idx < BaseSource.Indices.Num(); Idx++)
	{
		uint32 BaseVertIdx = BaseSource.Indices[Idx];

		// check for duplicate processing
		if (!WasProcessed[BaseVertIdx])
		{
			// mark this base vertex as already processed
			WasProcessed[BaseVertIdx] = true;

			// get base mesh vertex using its index buffer
			const FMorphMeshVertexRaw& VBase = BaseSource.Vertices[BaseVertIdx];

			// clothing can add extra verts, and we won't have source point, so we ignore those
			if (BaseSource.WedgePointIndices.IsValidIndex(BaseVertIdx))
			{
				// get the base mesh's original wedge point index
				uint32 BasePointIdx = BaseSource.WedgePointIndices[BaseVertIdx];

				// find the matching target vertex by searching for one
				// that has the same wedge point index
				uint32* TargetVertIdx = WedgePointToVertexIndexMap.Find(BasePointIdx);

				// only add the vertex if the source point was found
				if (TargetVertIdx != NULL)
				{
					// get target mesh vertex using its index buffer
					const FMorphMeshVertexRaw& VTarget = TargetSource.Vertices[*TargetVertIdx];

					// change in position from base to target
					FVector PositionDelta(VTarget.Position - VBase.Position);
					FVector NormalDeltaZ(VTarget.TangentZ - VBase.TangentZ);

					// check if position actually changed much
					if (PositionDelta.SizeSquared() > FMath::Square(THRESH_POINTS_ARE_NEAR) ||
						// since we can't get imported morphtarget normal from FBX
						// we can't compare normal unless it's calculated
						// this is special flag to ignore normal diff
						(true && NormalDeltaZ.SizeSquared() > 0.01f))
					{
						// create a new entry
						FMorphTargetDelta NewVertex;
						// position delta
						NewVertex.PositionDelta = PositionDelta;
						// normal delta
						NewVertex.TangentZDelta = NormalDeltaZ;
						// index of base mesh vert this entry is to modify
						NewVertex.SourceIdx = BaseVertIdx;

						// add it to the list of changed verts
						MorphModel.Vertices.Add(NewVertex);
					}
				}
			}
		}
	}

	// sort the array of vertices for this morph target based on the base mesh indices
	// that each vertex is associated with. This allows us to sequentially traverse the list
	// when applying the morph blends to each vertex.
	MorphModel.Vertices.Sort(FCompareMorphTargetDeltas());

	// remove array slack
	MorphModel.Vertices.Shrink();
}

////////////////////////////////////////////////////////////////////////////////////////////////
// UPmxFactory

bool UPmxFactory::ImportBone(
	//TArray<FbxNode*>& NodeArray,
	MMD4UE4::PmxMeshInfo *PmxMeshInfo,
	FSkeletalMeshImportData &ImportData,
	//UFbxSkeletalMeshImportData* TemplateData,
	//TArray<FbxNode*> &SortedLinks,
	bool& bOutDiffPose,
	bool bDisableMissingBindPoseWarning,
	bool & bUseTime0AsRefPose
	)
{
#if 0
	bOutDiffPose = false;
	int32 SkelType = 0; // 0 for skeletal mesh, 1 for rigid mesh
	FbxNode* Link = NULL;
	FbxArray<FbxPose*> PoseArray;
	TArray<FbxCluster*> ClusterArray;

	if (NodeArray[0]->GetMesh()->GetDeformerCount(FbxDeformer::eSkin) == 0)
	{
		SkelType = 1;
		Link = NodeArray[0];
		RecursiveBuildSkeleton(GetRootSkeleton(Link), SortedLinks);
	}
	else
	{
		// get bindpose and clusters from FBX skeleton

		// let's put the elements to their bind pose! (and we restore them after
		// we have built the ClusterInformation.
		int32 Default_NbPoses = SdkManager->GetBindPoseCount(Scene);
		// If there are no BindPoses, the following will generate them.
		//SdkManager->CreateMissingBindPoses(Scene);

		//if we created missing bind poses, update the number of bind poses
		int32 NbPoses = SdkManager->GetBindPoseCount(Scene);

		if (NbPoses != Default_NbPoses)
		{
			AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, LOCTEXT("FbxSkeletaLMeshimport_SceneMissingBinding", "The imported scene has no initial binding position (Bind Pose) for the skin. The plug-in will compute one automatically. However, it may create unexpected results.")), FFbxErrors::SkeletalMesh_NoBindPoseInScene);
		}

		//
		// create the bones / skinning
		//

		for (int32 i = 0; i < NodeArray.Num(); i++)
		{
			FbxMesh* FbxMesh = NodeArray[i]->GetMesh();
			const int32 SkinDeformerCount = FbxMesh->GetDeformerCount(FbxDeformer::eSkin);
			for (int32 DeformerIndex = 0; DeformerIndex < SkinDeformerCount; DeformerIndex++)
			{
				FbxSkin* Skin = (FbxSkin*)FbxMesh->GetDeformer(DeformerIndex, FbxDeformer::eSkin);
				for (int32 ClusterIndex = 0; ClusterIndex < Skin->GetClusterCount(); ClusterIndex++)
				{
					ClusterArray.Add(Skin->GetCluster(ClusterIndex));
				}
			}
		}

		if (ClusterArray.Num() == 0)
		{
			AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, LOCTEXT("FbxSkeletaLMeshimport_NoAssociatedCluster", "No associated clusters")), FFbxErrors::SkeletalMesh_NoAssociatedCluster);
			return false;
		}

		// get bind pose
		if (RetrievePoseFromBindPose(NodeArray, PoseArray) == false)
		{
			UE_LOG(LogFbx, Warning, TEXT("Getting valid bind pose failed. Try to recreate bind pose"));
			// if failed, delete bind pose, and retry.
			const int32 PoseCount = Scene->GetPoseCount();
			for (int32 PoseIndex = PoseCount - 1; PoseIndex >= 0; --PoseIndex)
			{
				FbxPose* CurrentPose = Scene->GetPose(PoseIndex);

				// current pose is bind pose, 
				if (CurrentPose && CurrentPose->IsBindPose())
				{
					Scene->RemovePose(PoseIndex);
					CurrentPose->Destroy();
				}
			}

			SdkManager->CreateMissingBindPoses(Scene);
			if (RetrievePoseFromBindPose(NodeArray, PoseArray) == false)
			{
				UE_LOG(LogFbx, Warning, TEXT("Recreating bind pose failed."));
			}
			else
			{
				UE_LOG(LogFbx, Warning, TEXT("Recreating bind pose succeeded."));
			}
		}

		// recurse through skeleton and build ordered table
		BuildSkeletonSystem(ClusterArray, SortedLinks);
	}

	// error check
	// if no bond is found
	if (SortedLinks.Num() == 0)
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_NoBone", "'{0}' has no bones"), FText::FromString(NodeArray[0]->GetName()))), FFbxErrors::SkeletalMesh_NoBoneFound);
		return false;
	}

	// if no bind pose is found
	if (!bUseTime0AsRefPose && PoseArray.GetCount() == 0)
	{
		// add to tokenized error message
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, LOCTEXT("FbxSkeletaLMeshimport_InvalidBindPose", "Could not find the bind pose.  It will use time 0 as bind pose.")), FFbxErrors::SkeletalMesh_InvalidBindPose);
		bUseTime0AsRefPose = true;
	}

	int32 LinkIndex;

	// Check for duplicate bone names and issue a warning if found
	for (LinkIndex = 0; LinkIndex < SortedLinks.Num(); ++LinkIndex)
	{
		Link = SortedLinks[LinkIndex];

		for (int32 AltLinkIndex = LinkIndex + 1; AltLinkIndex < SortedLinks.Num(); ++AltLinkIndex)
		{
			FbxNode* AltLink = SortedLinks[AltLinkIndex];

			if (FCStringAnsi::Strcmp(Link->GetName(), AltLink->GetName()) == 0)
			{
				FString RawBoneName = ANSI_TO_TCHAR(Link->GetName());
				AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, FText::Format(LOCTEXT("Error_DuplicateBoneName", "Error, Could not import {0}.\nDuplicate bone name found ('{1}'). Each bone must have a unique name."),
					FText::FromString(NodeArray[0]->GetName()), FText::FromString(RawBoneName))), FFbxErrors::SkeletalMesh_DuplicateBones);
				return false;
			}
		}
	}

	FbxArray<FbxAMatrix> GlobalsPerLink;
	GlobalsPerLink.Grow(SortedLinks.Num());
	GlobalsPerLink[0].SetIdentity();

	FbxVector4 LocalLinkT;
	FbxQuaternion LocalLinkQ;
	FbxVector4	LocalLinkS;
#endif
	bool GlobalLinkFoundFlag;

#if 1//test

	bool bAnyLinksNotInBindPose = false;
	FString LinksWithoutBindPoses;
	int32 NumberOfRoot = 0;

	int32 RootIdx = -1;

	for (int LinkIndex = 0; LinkIndex<PmxMeshInfo->boneList.Num(); LinkIndex++)
	{
		// Add a bone for each FBX Link
		ImportData.RefBonesBinary.Add(SkeletalMeshImportData::FBone());

		//Link = SortedLinks[LinkIndex];

		// get the link parent and children
		int32 ParentIndex = INDEX_NONE; // base value for root if no parent found
		/*FbxNode* LinkParent = Link->GetParent();*/
		int32 LinkParent = PmxMeshInfo->boneList[LinkIndex].ParentBoneIndex;
		if (LinkIndex)
		{
			for (int32 ll = 0; ll<LinkIndex; ++ll) // <LinkIndex because parent is guaranteed to be before child in sortedLink
			{
				/*FbxNode* Otherlink = SortedLinks[ll];*/
				int32 Otherlink = ll;
				if (Otherlink == LinkParent)
				{
					ParentIndex = ll;
					break;
				}
			}
		}

		// see how many root this has
		// if more than 
		if (ParentIndex == INDEX_NONE)
		{
			++NumberOfRoot;
			RootIdx = LinkIndex;
			if (NumberOfRoot > 1)
			{
				AddTokenizedErrorMessage(
					FTokenizedMessage::Create(
						EMessageSeverity::Error,
						LOCTEXT("MultipleRootsFound", "Multiple roots are found in the bone hierarchy. We only support single root bone.")), 
					FFbxErrors::SkeletalMesh_MultipleRoots
					);
				return false;
			}
		}

		GlobalLinkFoundFlag = false;
#if 0//Test2
		if (!SkelType) //skeletal mesh
		{
			// there are some links, they have no cluster, but in bindpose
			if (PoseArray.GetCount())
			{
				for (int32 PoseIndex = 0; PoseIndex < PoseArray.GetCount(); PoseIndex++)
				{
					int32 PoseLinkIndex = PoseArray[PoseIndex]->Find(Link);
					if (PoseLinkIndex >= 0)
					{
						FbxMatrix NoneAffineMatrix = PoseArray[PoseIndex]->GetMatrix(PoseLinkIndex);
						FbxAMatrix Matrix = *(FbxAMatrix*)(double*)&NoneAffineMatrix;
						GlobalsPerLink[LinkIndex] = Matrix;
						GlobalLinkFoundFlag = true;
						break;
					}
				}
			}

			if (!GlobalLinkFoundFlag)
			{
				// since now we set use time 0 as ref pose this won't unlikely happen
				// but leaving it just in case it still has case where it's missing partial bind pose
				if (!bUseTime0AsRefPose && !bDisableMissingBindPoseWarning)
				{
					bAnyLinksNotInBindPose = true;
					LinksWithoutBindPoses += ANSI_TO_TCHAR(Link->GetName());
					LinksWithoutBindPoses += TEXT("  \n");
				}

				for (int32 ClusterIndex = 0; ClusterIndex<ClusterArray.Num(); ClusterIndex++)
				{
					FbxCluster* Cluster = ClusterArray[ClusterIndex];
					if (Link == Cluster->GetLink())
					{
						Cluster->GetTransformLinkMatrix(GlobalsPerLink[LinkIndex]);
						GlobalLinkFoundFlag = true;
						break;
					}
				}
			}
		}

		if (!GlobalLinkFoundFlag)
		{
			GlobalsPerLink[LinkIndex] = Link->EvaluateGlobalTransform();
		}

		if (bUseTime0AsRefPose)
		{
			FbxAMatrix& T0Matrix = Scene->GetAnimationEvaluator()->GetNodeGlobalTransform(Link, 0);
			if (GlobalsPerLink[LinkIndex] != T0Matrix)
			{
				bOutDiffPose = true;
			}

			GlobalsPerLink[LinkIndex] = T0Matrix;
		}

		if (LinkIndex)
		{
			FbxAMatrix	Matrix;
			Matrix = GlobalsPerLink[ParentIndex].Inverse() * GlobalsPerLink[LinkIndex];
			LocalLinkT = Matrix.GetT();
			LocalLinkQ = Matrix.GetQ();
			LocalLinkS = Matrix.GetS();
		}
		else	// skeleton root
		{
			// for root, this is global coordinate
			LocalLinkT = GlobalsPerLink[LinkIndex].GetT();
			LocalLinkQ = GlobalsPerLink[LinkIndex].GetQ();
			LocalLinkS = GlobalsPerLink[LinkIndex].GetS();
		}
#else //TsetDebug
		{
			// for root, this is global coordinate
			/*LocalLinkT = GlobalsPerLink[LinkIndex].GetT();
			LocalLinkQ = GlobalsPerLink[LinkIndex].GetQ();
			LocalLinkS = GlobalsPerLink[LinkIndex].GetS();*/
		}
#endif

		//ImportData.RefBonesBinary.AddZeroed();
		// set bone
		SkeletalMeshImportData::FBone& Bone = ImportData.RefBonesBinary[LinkIndex];
		FString BoneName;

		/*const char* LinkName = Link->GetName();
		BoneName = ANSI_TO_TCHAR(MakeName(LinkName));*/
		BoneName = PmxMeshInfo->boneList[LinkIndex].Name;//For MMD
		Bone.Name = BoneName;

		SkeletalMeshImportData::FJointPos& JointMatrix = Bone.BonePos;
#if 0//Test2
		FbxSkeleton* Skeleton = Link->GetSkeleton();
		if (Skeleton)
		{
			JointMatrix.Length = Converter.ConvertDist(Skeleton->LimbLength.Get());
			JointMatrix.XSize = Converter.ConvertDist(Skeleton->Size.Get());
			JointMatrix.YSize = Converter.ConvertDist(Skeleton->Size.Get());
			JointMatrix.ZSize = Converter.ConvertDist(Skeleton->Size.Get());
		}
		else
#endif
		{
			JointMatrix.Length = 1.;
			JointMatrix.XSize = 100.;
			JointMatrix.YSize = 100.;
			JointMatrix.ZSize = 100.;
		}

		// get the link parent and children
		Bone.ParentIndex = ParentIndex;
		Bone.NumChildren = 0;
		/*
		for (int32 ChildIndex = 0; ChildIndex<Link->GetChildCount(); ChildIndex++)
		{
			FbxNode* Child = Link->GetChild(ChildIndex);
			if (IsUnrealBone(Child))
			{
				Bone.NumChildren++;
			}
		}*/
		//For MMD
		for (int32 ChildIndex = 0; ChildIndex < PmxMeshInfo->boneList.Num(); ChildIndex++)
		{
			if (LinkIndex == PmxMeshInfo->boneList[ChildIndex].ParentBoneIndex)
			{
				Bone.NumChildren++;
			}
		}
		/*
		JointMatrix.Transform.SetTranslation(Converter.ConvertPos(LocalLinkT));
		JointMatrix.Transform.SetRotation(Converter.ConvertRotToQuat(LocalLinkQ));
		JointMatrix.Transform.SetScale3D(Converter.ConvertScale(LocalLinkS));
		*/
		//test MMD , not rot asix and LocalAsix
		FVector TransTemp;
		if (ParentIndex != INDEX_NONE)
		{
			TransTemp = PmxMeshInfo->boneList[ParentIndex].Position
				- PmxMeshInfo->boneList[LinkIndex].Position;
			TransTemp *= -1;
		}
		else
		{
			TransTemp = PmxMeshInfo->boneList[LinkIndex].Position;
		}
		//TransTemp *= 10;
		JointMatrix.Transform.SetTranslation(TransTemp);
		JointMatrix.Transform.SetRotation(FQuat(0,0,0,1.0));
		JointMatrix.Transform.SetScale3D(FVector(1));
	}
	/*
	if (TemplateData)
	{
		FbxAMatrix FbxAddedMatrix;
		BuildFbxMatrixForImportTransform(FbxAddedMatrix, TemplateData);
		FMatrix AddedMatrix = Converter.ConvertMatrix(FbxAddedMatrix);

		VBone& RootBone = ImportData.RefBonesBinary[RootIdx];
		FTransform& RootTransform = RootBone.BonePos.Transform;
		RootTransform.SetFromMatrix(RootTransform.ToMatrixWithScale() * AddedMatrix);
	}

	if (bAnyLinksNotInBindPose)
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_BonesAreMissingFromBindPose", "The following bones are missing from the bind pose:\n{0}\nThis can happen for bones that are not vert weighted. If they are not in the correct orientation after importing,\nplease set the \"Use T0 as ref pose\" option or add them to the bind pose and reimport the skeletal mesh."), FText::FromString(LinksWithoutBindPoses))), FFbxErrors::SkeletalMesh_BonesAreMissingFromBindPose);
	}*/
#endif
	return true;
}


bool UPmxFactory::FillSkelMeshImporterFromFbx(
	FSkeletalMeshImportData& ImportData,
	MMD4UE4::PmxMeshInfo *& PmxMeshInfo
	//FbxMesh*& Mesh,
	//FbxSkin* Skin,
	//FbxShape* FbxShape,
	//TArray<FbxNode*> &SortedLinks,
	//const TArray<FbxSurfaceMaterial*>& FbxMaterials
	)
{
#if 0

	FbxNode* Node = Mesh->GetNode();

	//remove the bad polygons before getting any data from mesh
	Mesh->RemoveBadPolygons();

	//Get the base layer of the mesh
	FbxLayer* BaseLayer = Mesh->GetLayer(0);
	if (BaseLayer == NULL)
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("FbxSkeletaLMeshimport_NoGeometry", "There is no geometry information in mesh")), FFbxErrors::Generic_Mesh_NoGeometry);
		return false;
	}

	// Do some checks before proceeding, check to make sure the number of bones does not exceed the maximum supported
	if (SortedLinks.Num() > MAX_BONES)
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_ExceedsMaxBoneCount", "'{0}' mesh has '{1}' bones which exceeds the maximum allowed bone count of {2}."), FText::FromString(Node->GetName()), FText::AsNumber(SortedLinks.Num()), FText::AsNumber(MAX_BONES))), FFbxErrors::SkeletalMesh_ExceedsMaxBoneCount);
		return false;
	}

	//
	//	store the UVs in arrays for fast access in the later looping of triangles 
	//
	// mapping from UVSets to Fbx LayerElementUV
	// Fbx UVSets may be duplicated, remove the duplicated UVSets in the mapping 
	int32 LayerCount = Mesh->GetLayerCount();
	TArray<FString> UVSets;

	UVSets.Empty();
	if (LayerCount > 0)
	{
		int32 UVLayerIndex;
		for (UVLayerIndex = 0; UVLayerIndex<LayerCount; UVLayerIndex++)
		{
			FbxLayer* lLayer = Mesh->GetLayer(UVLayerIndex);
			int32 UVSetCount = lLayer->GetUVSetCount();
			if (UVSetCount)
			{
				FbxArray<FbxLayerElementUV const*> EleUVs = lLayer->GetUVSets();
				for (int32 UVIndex = 0; UVIndex<UVSetCount; UVIndex++)
				{
					FbxLayerElementUV const* ElementUV = EleUVs[UVIndex];
					if (ElementUV)
					{
						const char* UVSetName = ElementUV->GetName();
						FString LocalUVSetName = ANSI_TO_TCHAR(UVSetName);

						UVSets.AddUnique(LocalUVSetName);
					}
				}
			}
		}
	}

	// If the the UV sets are named using the following format (UVChannel_X; where X ranges from 1 to 4)
	// we will re-order them based on these names.  Any UV sets that do not follow this naming convention
	// will be slotted into available spaces.
	if (UVSets.Num() > 0)
	{
		for (int32 ChannelNumIdx = 0; ChannelNumIdx < 4; ChannelNumIdx++)
		{
			FString ChannelName = FString::Printf(TEXT("UVChannel_%d"), ChannelNumIdx + 1);
			int32 SetIdx = UVSets.Find(ChannelName);

			// If the specially formatted UVSet name appears in the list and it is in the wrong spot,
			// we will swap it into the correct spot.
			if (SetIdx != INDEX_NONE && SetIdx != ChannelNumIdx)
			{
				// If we are going to swap to a position that is outside the bounds of the
				// array, then we pad out to that spot with empty data.
				for (int32 ArrSize = UVSets.Num(); ArrSize < ChannelNumIdx + 1; ArrSize++)
				{
					UVSets.Add(FString(TEXT("")));
				}
				//Swap the entry into the appropriate spot.
				UVSets.Swap(SetIdx, ChannelNumIdx);
			}
		}
	}
#endif
	TArray<UMaterialInterface*> Materials;
#if 0//test material
	if (ImportOptions->bImportMaterials)
	{
		CreateNodeMaterials(Node, Materials, UVSets);
	}
	else if (ImportOptions->bImportTextures)
	{
		ImportTexturesFromNode(Node);
	}

	// Maps local mesh material index to global material index
	TArray<int32> MaterialMapping;

	int32 MaterialCount = Node->GetMaterialCount();

	MaterialMapping.AddUninitialized(MaterialCount);

	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
	{
		FbxSurfaceMaterial* FbxMaterial = Node->GetMaterial(MaterialIndex);

		int32 ExistingMatIndex = INDEX_NONE;
		FbxMaterials.Find(FbxMaterial, ExistingMatIndex);
		if (ExistingMatIndex != INDEX_NONE)
		{
			// Reuse existing material
			MaterialMapping[MaterialIndex] = ExistingMatIndex;

			if (ImportOptions->bImportMaterials && Materials.IsValidIndex(MaterialIndex))
			{
				ImportData.Materials[ExistingMatIndex].MaterialImportName = ANSI_TO_TCHAR(MakeName(FbxMaterial->GetName()));
				ImportData.Materials[ExistingMatIndex].Material = Materials[MaterialIndex];
			}
		}
		else
		{
			MaterialMapping[MaterialIndex] = 0;
		}
	}
#else

#if 1 //test Material Textuere
	////////
	TArray<UTexture*> textureAssetList;
	if (ImportUI->bImportTextures)
	{
		for (int k = 0; k < PmxMeshInfo->textureList.Num(); ++k)
		{
			pmxMaterialImportHelper.AssetsCreateTextuer(
				//InParent,
				//Flags,
				//Warn,
				FPaths::GetPath(GetCurrentFilename()),
				PmxMeshInfo->textureList[k].TexturePath,
				textureAssetList
				);

			//if (NewObject)
			/*{
				NodeIndex++;
				FFormatNamedArguments Args;
				Args.Add(TEXT("NodeIndex"), NodeIndex);
				Args.Add(TEXT("ArrayLength"), NodeIndexMax);// SkelMeshArray.Num());
				GWarn->StatusUpdate(NodeIndex, NodeIndexMax, FText::Format(NSLOCTEXT("UnrealEd", "Importingf", "Importing ({NodeIndex} of {ArrayLength})"), Args));
				}*/
		}
	}
	//UE_LOG(LogCategoryPMXFactory, Warning, TEXT("PMX Import Texture Extecd Complete."));
	////////////////////////////////////////////

	TArray<FString> UVSets;
	if (ImportUI->bImportMaterials)
	{
		for (int k = 0; k < PmxMeshInfo->materialList.Num(); ++k)
		{
			pmxMaterialImportHelper.CreateUnrealMaterial(
				PmxMeshInfo->modelNameJP,
				//InParent,
				PmxMeshInfo->materialList[k],
				ImportUI->bCreateMaterialInstMode,
				ImportUI->bUnlitMaterials,
				Materials,
				textureAssetList);
			//if (NewObject)
			/*{
				NodeIndex++;
				FFormatNamedArguments Args;
				Args.Add(TEXT("NodeIndex"), NodeIndex);
				Args.Add(TEXT("ArrayLength"), NodeIndexMax);// SkelMeshArray.Num());
				GWarn->StatusUpdate(NodeIndex, NodeIndexMax, FText::Format(NSLOCTEXT("UnrealEd", "Importingf", "Importing ({NodeIndex} of {ArrayLength})"), Args));
				}*/
			{
				int ExistingMatIndex = k;
				int MaterialIndex = k;

				// material asset set flag for morph target 
				if (ImportUI->bImportMorphTargets)
				{
					if (UMaterial* UnrealMaterialPtr = Cast<UMaterial>(Materials[MaterialIndex]))
					{
						UnrealMaterialPtr->bUsedWithMorphTargets = 1;
					}
				}

				ImportData.Materials[ExistingMatIndex].MaterialImportName
					= "M_" + PmxMeshInfo->materialList[k].Name;
				ImportData.Materials[ExistingMatIndex].Material
					= Materials[MaterialIndex];
			}
		}
	}
	///////////////////////////////////////////
	//UE_LOG(LogCategoryPMXFactory, Warning, TEXT("PMX Import Material Extecd Complete."));
	///////////////////////////////////////////
#endif

#endif
#if 0//test unsuport
	if (LayerCount > 0 && ImportOptions->bPreserveSmoothingGroups)
	{
		// Check and see if the smooothing data is valid.  If not generate it from the normals
		BaseLayer = Mesh->GetLayer(0);
		if (BaseLayer)
		{
			const FbxLayerElementSmoothing* SmoothingLayer = BaseLayer->GetSmoothing();

			if (SmoothingLayer)
			{
				bool bValidSmoothingData = false;
				FbxLayerElementArrayTemplate<int32>& Array = SmoothingLayer->GetDirectArray();
				for (int32 SmoothingIndex = 0; SmoothingIndex < Array.GetCount(); ++SmoothingIndex)
				{
					if (Array[SmoothingIndex] != 0)
					{
						bValidSmoothingData = true;
						break;
					}
				}

				if (!bValidSmoothingData && Mesh->GetPolygonVertexCount() > 0)
				{
					GeometryConverter->ComputeEdgeSmoothingFromNormals(Mesh);
				}
			}
		}
	}

	// Must do this before triangulating the mesh due to an FBX bug in TriangulateMeshAdvance
	int32 LayerSmoothingCount = Mesh->GetLayerCount(FbxLayerElement::eSmoothing);
	for (int32 i = 0; i < LayerSmoothingCount; i++)
	{
		GeometryConverter->ComputePolygonSmoothingFromEdgeSmoothing(Mesh, i);
	}

	//
	// Convert data format to unreal-compatible
	//

	if (!Mesh->IsTriangleMesh())
	{
		UE_LOG(LogFbx, Log, TEXT("Triangulating skeletal mesh %s"), ANSI_TO_TCHAR(Node->GetName()));

		const bool bReplace = true;
		FbxNodeAttribute* ConvertedNode = GeometryConverter->Triangulate(Mesh, bReplace);
		if (ConvertedNode != NULL && ConvertedNode->GetAttributeType() == FbxNodeAttribute::eMesh)
		{
			Mesh = ConvertedNode->GetNode()->GetMesh();
		}
		else
		{
			AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_TriangulatingFailed", "Unable to triangulate mesh '{0}'. Check detail for Ouput Log."), FText::FromString(Node->GetName()))), FFbxErrors::Generic_Mesh_TriangulationFailed);
			return false;
		}
	}

	// renew the base layer
	BaseLayer = Mesh->GetLayer(0);
	Skin = (FbxSkin*)static_cast<FbxGeometry*>(Mesh)->GetDeformer(0, FbxDeformer::eSkin);
#endif
	//
	//	store the UVs in arrays for fast access in the later looping of triangles 
	//
	uint32 UniqueUVCount = UVSets.Num();
#if 0//test 3
	FbxLayerElementUV** LayerElementUV = NULL;
	FbxLayerElement::EReferenceMode* UVReferenceMode = NULL;
	FbxLayerElement::EMappingMode* UVMappingMode = NULL;
	if (UniqueUVCount > 0)
	{
		LayerElementUV = new FbxLayerElementUV*[UniqueUVCount];
		UVReferenceMode = new FbxLayerElement::EReferenceMode[UniqueUVCount];
		UVMappingMode = new FbxLayerElement::EMappingMode[UniqueUVCount];
	}
	else
	{
		AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_NoUVSet", "Mesh '{0}' has no UV set. Creating a default set."), FText::FromString(Node->GetName()))), FFbxErrors::SkeletalMesh_NoUVSet);
	}

	LayerCount = Mesh->GetLayerCount();
	for (uint32 UVIndex = 0; UVIndex < UniqueUVCount; UVIndex++)
	{
		bool bFoundUV = false;
		LayerElementUV[UVIndex] = NULL;
		for (int32 UVLayerIndex = 0; !bFoundUV && UVLayerIndex<LayerCount; UVLayerIndex++)
		{
			FbxLayer* lLayer = Mesh->GetLayer(UVLayerIndex);
			int32 UVSetCount = lLayer->GetUVSetCount();
			if (UVSetCount)
			{
				FbxArray<FbxLayerElementUV const*> EleUVs = lLayer->GetUVSets();
				for (int32 FbxUVIndex = 0; FbxUVIndex<UVSetCount; FbxUVIndex++)
				{
					FbxLayerElementUV const* ElementUV = EleUVs[FbxUVIndex];
					if (ElementUV)
					{
						const char* UVSetName = ElementUV->GetName();
						FString LocalUVSetName = ANSI_TO_TCHAR(UVSetName);
						if (LocalUVSetName == UVSets[UVIndex])
						{
							LayerElementUV[UVIndex] = const_cast<FbxLayerElementUV*>(ElementUV);
							UVReferenceMode[UVIndex] = LayerElementUV[FbxUVIndex]->GetReferenceMode();
							UVMappingMode[UVIndex] = LayerElementUV[FbxUVIndex]->GetMappingMode();
							break;
						}
					}
				}
			}
		}
	}

	//
	// get the smoothing group layer
	//
	bool bSmoothingAvailable = false;

	FbxLayerElementSmoothing const* SmoothingInfo = BaseLayer->GetSmoothing();
	FbxLayerElement::EReferenceMode SmoothingReferenceMode(FbxLayerElement::eDirect);
	FbxLayerElement::EMappingMode SmoothingMappingMode(FbxLayerElement::eByEdge);
	if (SmoothingInfo)
	{
		if (SmoothingInfo->GetMappingMode() == FbxLayerElement::eByEdge)
		{
			if (!GeometryConverter->ComputePolygonSmoothingFromEdgeSmoothing(Mesh))
			{
				AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_ConvertSmoothingGroupFailed", "Unable to fully convert the smoothing groups for mesh '{0}'"), FText::FromString(Mesh->GetName()))), FFbxErrors::Generic_Mesh_ConvertSmoothingGroupFailed);
				bSmoothingAvailable = false;
			}
		}

		if (SmoothingInfo->GetMappingMode() == FbxLayerElement::eByPolygon)
		{
			bSmoothingAvailable = true;
		}


		SmoothingReferenceMode = SmoothingInfo->GetReferenceMode();
		SmoothingMappingMode = SmoothingInfo->GetMappingMode();
	}


	//
	//	get the "material index" layer
	//
	FbxLayerElementMaterial* LayerElementMaterial = BaseLayer->GetMaterials();
	FbxLayerElement::EMappingMode MaterialMappingMode = LayerElementMaterial ?
		LayerElementMaterial->GetMappingMode() : FbxLayerElement::eByPolygon;

	UniqueUVCount = FMath::Min<uint32>(UniqueUVCount, MAX_TEXCOORDS);
#else

	UniqueUVCount = FMath::Min<uint32>(UniqueUVCount, MAX_TEXCOORDS);
#endif
	// One UV set is required but only import up to MAX_TEXCOORDS number of uv layers
	ImportData.NumTexCoords = FMath::Max<uint32>(ImportData.NumTexCoords, UniqueUVCount);

#if 0 //test
	//
	// get the first vertex color layer
	//
	FbxLayerElementVertexColor* LayerElementVertexColor = BaseLayer->GetVertexColors();
	FbxLayerElement::EReferenceMode VertexColorReferenceMode(FbxLayerElement::eDirect);
	FbxLayerElement::EMappingMode VertexColorMappingMode(FbxLayerElement::eByControlPoint);
	if (LayerElementVertexColor)
	{
		VertexColorReferenceMode = LayerElementVertexColor->GetReferenceMode();
		VertexColorMappingMode = LayerElementVertexColor->GetMappingMode();
		ImportData.bHasVertexColors = true;
	}

	//
	// get the first normal layer
	//
	FbxLayerElementNormal* LayerElementNormal = BaseLayer->GetNormals();
	FbxLayerElementTangent* LayerElementTangent = BaseLayer->GetTangents();
	FbxLayerElementBinormal* LayerElementBinormal = BaseLayer->GetBinormals();

	//whether there is normal, tangent and binormal data in this mesh
	bool bHasNormalInformation = LayerElementNormal != NULL;
	bool bHasTangentInformation = LayerElementTangent != NULL && LayerElementBinormal != NULL;

	ImportData.bHasNormals = bHasNormalInformation;
	ImportData.bHasTangents = bHasTangentInformation;
#else //test
	ImportData.bHasNormals = false;
	ImportData.bHasTangents = true;
#endif
#if 0 //test
	FbxLayerElement::EReferenceMode NormalReferenceMode(FbxLayerElement::eDirect);
	FbxLayerElement::EMappingMode NormalMappingMode(FbxLayerElement::eByControlPoint);
	if (LayerElementNormal)
	{
		NormalReferenceMode = LayerElementNormal->GetReferenceMode();
		NormalMappingMode = LayerElementNormal->GetMappingMode();
	}

	FbxLayerElement::EReferenceMode TangentReferenceMode(FbxLayerElement::eDirect);
	FbxLayerElement::EMappingMode TangentMappingMode(FbxLayerElement::eByControlPoint);
	if (LayerElementTangent)
	{
		TangentReferenceMode = LayerElementTangent->GetReferenceMode();
		TangentMappingMode = LayerElementTangent->GetMappingMode();
	}
#endif
	//
	// create the points / wedges / faces
	//
	int32 ControlPointsCount =
		PmxMeshInfo->vertexList.Num();
		//Mesh->GetControlPointsCount();
	int32 ExistPointNum = ImportData.Points.Num();
	ImportData.Points.AddUninitialized(ControlPointsCount);

#if 0//Test points
	// Construct the matrices for the conversion from right handed to left handed system
	FbxAMatrix TotalMatrix;
	FbxAMatrix TotalMatrixForNormal;
	TotalMatrix = ComputeTotalMatrix(Node);
	TotalMatrixForNormal = TotalMatrix.Inverse();
	TotalMatrixForNormal = TotalMatrixForNormal.Transpose();
#endif
	int32 ControlPointsIndex;
	for (ControlPointsIndex = 0; ControlPointsIndex < ControlPointsCount; ControlPointsIndex++)
	{
#if 0
		FbxVector4 Position;
		if (FbxShape)
		{
			Position = FbxShape->GetControlPoints()[ControlPointsIndex];
		}
		else
		{
			Position = Mesh->GetControlPoints()[ControlPointsIndex];
		}
		FbxVector4 FinalPosition;
		FinalPosition = TotalMatrix.MultT(Position);
		ImportData.Points[ControlPointsIndex + ExistPointNum] = Converter.ConvertPos(FinalPosition);
#else
		ImportData.Points[ControlPointsIndex + ExistPointNum]
			= PmxMeshInfo->vertexList[ControlPointsIndex].Position;
#endif
	}
#if 1 //vertex
	bool OddNegativeScale = true;// false;// IsOddNegativeScale(TotalMatrix);

	int32 VertexIndex;
	int32 TriangleCount = PmxMeshInfo->faseList.Num();//Mesh->GetPolygonCount();
	int32 ExistFaceNum = ImportData.Faces.Num();
	ImportData.Faces.AddUninitialized(TriangleCount);
	int32 ExistWedgesNum = ImportData.Wedges.Num();
	SkeletalMeshImportData::FVertex TmpWedges[3];

	int32 facecount =0;
	int32 matIndx = 0;

	for (int32 TriangleIndex = ExistFaceNum, LocalIndex = 0; TriangleIndex < ExistFaceNum + TriangleCount; TriangleIndex++, LocalIndex++)
	{

		SkeletalMeshImportData::FTriangle& Triangle = ImportData.Faces[TriangleIndex];

		//
		// smoothing mask
		//
		// set the face smoothing by default. It could be any number, but not zero
		Triangle.SmoothingGroups = 255;
		/*if (bSmoothingAvailable)
		{
			if (SmoothingInfo)
			{
				if (SmoothingMappingMode == FbxLayerElement::eByPolygon)
				{
					int32 lSmoothingIndex = (SmoothingReferenceMode == FbxLayerElement::eDirect) ? LocalIndex : SmoothingInfo->GetIndexArray().GetAt(LocalIndex);
					Triangle.SmoothingGroups = SmoothingInfo->GetDirectArray().GetAt(lSmoothingIndex);
				}
				else
				{
					AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_Unsupportingsmoothinggroup", "Unsupported Smoothing group mapping mode on mesh '{0}'"), FText::FromString(Mesh->GetName()))), FFbxErrors::Generic_Mesh_UnsupportingSmoothingGroup);
				}
			}
		}*/

		for (VertexIndex = 0; VertexIndex<3; VertexIndex++)
		{
			// If there are odd number negative scale, invert the vertex order for triangles
			int32 UnrealVertexIndex = OddNegativeScale ? 2 - VertexIndex : VertexIndex;

			/*int32 ControlPointIndex = Mesh->GetPolygonVertex(LocalIndex, VertexIndex);
			//
			// normals, tangents and binormals
			//
			if (ImportOptions->ShouldImportNormals() && bHasNormalInformation)
			{
				int32 TmpIndex = LocalIndex * 3 + VertexIndex;
				//normals may have different reference and mapping mode than tangents and binormals
				int32 NormalMapIndex = (NormalMappingMode == FbxLayerElement::eByControlPoint) ?
				ControlPointIndex : TmpIndex;
				int32 NormalValueIndex = (NormalReferenceMode == FbxLayerElement::eDirect) ?
				NormalMapIndex : LayerElementNormal->GetIndexArray().GetAt(NormalMapIndex);

				//tangents and binormals share the same reference, mapping mode and index array
				int32 TangentMapIndex = TmpIndex;

				FbxVector4 TempValue;

				if (ImportOptions->ShouldImportTangents() && bHasTangentInformation)
				{
					TempValue = LayerElementTangent->GetDirectArray().GetAt(TangentMapIndex);
					TempValue = TotalMatrixForNormal.MultT(TempValue);
					Triangle.TangentX[UnrealVertexIndex] = Converter.ConvertDir(TempValue);
					Triangle.TangentX[UnrealVertexIndex].Normalize();

					TempValue = LayerElementBinormal->GetDirectArray().GetAt(TangentMapIndex);
					TempValue = TotalMatrixForNormal.MultT(TempValue);
					Triangle.TangentY[UnrealVertexIndex] = -Converter.ConvertDir(TempValue);
					Triangle.TangentY[UnrealVertexIndex].Normalize();
				}

				TempValue = LayerElementNormal->GetDirectArray().GetAt(NormalValueIndex);
				TempValue = TotalMatrixForNormal.MultT(TempValue);
				Triangle.TangentZ[UnrealVertexIndex] = Converter.ConvertDir(TempValue);
				Triangle.TangentZ[UnrealVertexIndex].Normalize();
				
			}
			else*/
			if(true)
			{
				int32 NormalIndex = UnrealVertexIndex;
				//for (NormalIndex = 0; NormalIndex < 3; ++NormalIndex)
				{
					FVector TangentZ 
						= PmxMeshInfo->vertexList[PmxMeshInfo->faseList[LocalIndex].VertexIndex[NormalIndex]].Normal;

					Triangle.TangentX[NormalIndex] = FVector::ZeroVector;
					Triangle.TangentY[NormalIndex] = FVector::ZeroVector;
					Triangle.TangentZ[NormalIndex] = TangentZ.GetSafeNormal();
				}
			}
			else
			{
				int32 NormalIndex;
				for (NormalIndex = 0; NormalIndex < 3; ++NormalIndex)
				{
					Triangle.TangentX[NormalIndex] = FVector::ZeroVector;
					Triangle.TangentY[NormalIndex] = FVector::ZeroVector;
					Triangle.TangentZ[NormalIndex] = FVector::ZeroVector;
				}
			}
		}

		//
		// material index
		//
		Triangle.MatIndex = 0; // default value
#if 0
		if (MaterialCount>0)
		{
			if (LayerElementMaterial)
			{
				switch (MaterialMappingMode)
				{
					// material index is stored in the IndexArray, not the DirectArray (which is irrelevant with 2009.1)
				case FbxLayerElement::eAllSame:
				{
					Triangle.MatIndex = MaterialMapping[LayerElementMaterial->GetIndexArray().GetAt(0)];
				}
				break;
				case FbxLayerElement::eByPolygon:
				{
					int32 Index = LayerElementMaterial->GetIndexArray().GetAt(LocalIndex);
					if (!MaterialMapping.IsValidIndex(Index))
					{
						AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, LOCTEXT("FbxSkeletaLMeshimport_MaterialIndexInconsistency", "Face material index inconsistency - forcing to 0")), FFbxErrors::Generic_Mesh_MaterialIndexInconsistency);
					}
					else
					{
						Triangle.MatIndex = MaterialMapping[Index];
					}
				}
				break;
				}
			}

			// When import morph, we don't check the material index 
			// because we don't import material for morph, so the ImportData.Materials contains zero material
			if (!FbxShape && (Triangle.MatIndex < 0 || Triangle.MatIndex >= FbxMaterials.Num()))
			{
				AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, LOCTEXT("FbxSkeletaLMeshimport_MaterialIndexInconsistency", "Face material index inconsistency - forcing to 0")), FFbxErrors::Generic_Mesh_MaterialIndexInconsistency);
				Triangle.MatIndex = 0;
			}
		}
#else
		if (true)
		{
			// for mmd

			if (PmxMeshInfo->materialList.Num() > matIndx)
			{
				facecount++;
				facecount++;
				facecount++;
				Triangle.MatIndex = matIndx;
				if (facecount >= PmxMeshInfo->materialList[matIndx].MaterialFaceNum)
				{
					matIndx++;
					facecount = 0;
				}
			}
		}
#endif

		Triangle.AuxMatIndex = 0;
		for (VertexIndex = 0; VertexIndex<3; VertexIndex++)
		{
			// If there are odd number negative scale, invert the vertex order for triangles
			int32 UnrealVertexIndex = OddNegativeScale ? 2 - VertexIndex : VertexIndex;

			TmpWedges[UnrealVertexIndex].MatIndex = Triangle.MatIndex;
			TmpWedges[UnrealVertexIndex].VertexIndex
				= PmxMeshInfo->faseList[LocalIndex].VertexIndex[VertexIndex];
				// = ExistPointNum + Mesh->GetPolygonVertex(LocalIndex, VertexIndex);
			// Initialize all colors to white.
			TmpWedges[UnrealVertexIndex].Color = FColor::White;
		}

		//
		// uvs
		//
		uint32 UVLayerIndex;
		// Some FBX meshes can have no UV sets, so also check the UniqueUVCount
		for (UVLayerIndex = 0; UVLayerIndex< UniqueUVCount; UVLayerIndex++)
		{
#if 0 //test uv vec
			// ensure the layer has data
			if (LayerElementUV[UVLayerIndex] != NULL)
			{
				// Get each UV from the layer
				for (VertexIndex = 0; VertexIndex<3; VertexIndex++)
				{
					// If there are odd number negative scale, invert the vertex order for triangles
					int32 UnrealVertexIndex = OddNegativeScale ? 2 - VertexIndex : VertexIndex;

					int32 lControlPointIndex = Mesh->GetPolygonVertex(LocalIndex, VertexIndex);
					int32 UVMapIndex = (UVMappingMode[UVLayerIndex] == FbxLayerElement::eByControlPoint) ?
					lControlPointIndex : LocalIndex * 3 + VertexIndex;
					int32 UVIndex = (UVReferenceMode[UVLayerIndex] == FbxLayerElement::eDirect) ?
					UVMapIndex : LayerElementUV[UVLayerIndex]->GetIndexArray().GetAt(UVMapIndex);
					FbxVector2	UVVector = LayerElementUV[UVLayerIndex]->GetDirectArray().GetAt(UVIndex);

					TmpWedges[UnrealVertexIndex].UVs[UVLayerIndex].X = static_cast<float>(UVVector[0]);
					TmpWedges[UnrealVertexIndex].UVs[UVLayerIndex].Y = 1.f - static_cast<float>(UVVector[1]);
				}
			}
			else
#endif
			/*if (UVLayerIndex == 0)
			{
				FVector2D tempUV = PmxMeshInfo->vertexList[TmpWedges[VertexIndex].VertexIndex].UV;
				// Set all UV's to zero.  If we are here the mesh had no UV sets so we only need to do this for the
				// first UV set which always exists.
				TmpWedges[VertexIndex].UVs[UVLayerIndex].X = tempUV.X;
				TmpWedges[VertexIndex].UVs[UVLayerIndex].Y = tempUV.Y;
			}else*/
			{
				// Set all UV's to zero.  If we are here the mesh had no UV sets so we only need to do this for the
				// first UV set which always exists.
				TmpWedges[VertexIndex].UVs[UVLayerIndex].X = 0;
				TmpWedges[VertexIndex].UVs[UVLayerIndex].Y = 0;
			}
		}
#if 0 //test vertex color basecolor
		// Read vertex colors if they exist.
		if (LayerElementVertexColor)
		{
			switch (VertexColorMappingMode)
			{
			case FbxLayerElement::eByControlPoint:
			{
				int32 VertexIndex;
				for (VertexIndex = 0; VertexIndex<3; VertexIndex++)
				{
					int32 UnrealVertexIndex = OddNegativeScale ? 2 - VertexIndex : VertexIndex;

					FbxColor VertexColor = (VertexColorReferenceMode == FbxLayerElement::eDirect)
						? LayerElementVertexColor->GetDirectArray().GetAt(Mesh->GetPolygonVertex(LocalIndex, VertexIndex))
						: LayerElementVertexColor->GetDirectArray().GetAt(LayerElementVertexColor->GetIndexArray().GetAt(Mesh->GetPolygonVertex(LocalIndex, VertexIndex)));

					TmpWedges[UnrealVertexIndex].Color = FColor(uint8(255.f*VertexColor.mRed),
						uint8(255.f*VertexColor.mGreen),
						uint8(255.f*VertexColor.mBlue),
						uint8(255.f*VertexColor.mAlpha));
				}
			}
			break;
			case FbxLayerElement::eByPolygonVertex:
			{
				int32 VertexIndex;
				for (VertexIndex = 0; VertexIndex<3; VertexIndex++)
				{
					int32 UnrealVertexIndex = OddNegativeScale ? 2 - VertexIndex : VertexIndex;

					FbxColor VertexColor = (VertexColorReferenceMode == FbxLayerElement::eDirect)
						? LayerElementVertexColor->GetDirectArray().GetAt(LocalIndex * 3 + VertexIndex)
						: LayerElementVertexColor->GetDirectArray().GetAt(LayerElementVertexColor->GetIndexArray().GetAt(LocalIndex * 3 + VertexIndex));

					TmpWedges[UnrealVertexIndex].Color = FColor(uint8(255.f*VertexColor.mRed),
						uint8(255.f*VertexColor.mGreen),
						uint8(255.f*VertexColor.mBlue),
						uint8(255.f*VertexColor.mAlpha));
				}
			}
			break;
			}
		}
#endif
		//
		// basic wedges matching : 3 unique per face. TODO Can we do better ?
		//
		for (VertexIndex = 0; VertexIndex<3; VertexIndex++)
		{
			int32 w;

			w = ImportData.Wedges.AddUninitialized();
			ImportData.Wedges[w].VertexIndex = TmpWedges[VertexIndex].VertexIndex;
			ImportData.Wedges[w].MatIndex = TmpWedges[VertexIndex].MatIndex;
			ImportData.Wedges[w].Color = TmpWedges[VertexIndex].Color;
			ImportData.Wedges[w].Reserved = 0;

			FVector2D tempUV 
				= PmxMeshInfo->vertexList[TmpWedges[VertexIndex].VertexIndex].UV;
			TmpWedges[VertexIndex].UVs[0].X = tempUV.X;
			TmpWedges[VertexIndex].UVs[0].Y = tempUV.Y;
			FMemory::Memcpy(ImportData.Wedges[w].UVs, 
				TmpWedges[VertexIndex].UVs, 
				sizeof(FVector2D)*MAX_TEXCOORDS);

			Triangle.WedgeIndex[VertexIndex] = w;
		}

	}
#endif
#if 1
	// now we can work on a per-cluster basis with good ordering
#if 0
	if (Skin) // skeletal mesh
	{
		// create influences for each cluster
		int32 ClusterIndex;
		for (ClusterIndex = 0; ClusterIndex<Skin->GetClusterCount(); ClusterIndex++)
		{
			FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
			// When Maya plug-in exports rigid binding, it will generate "CompensationCluster" for each ancestor links.
			// FBX writes these "CompensationCluster" out. The CompensationCluster also has weight 1 for vertices.
			// Unreal importer should skip these clusters.
			if (Cluster && FCStringAnsi::Strcmp(Cluster->GetUserDataID(), "Maya_ClusterHint") == 0 && FCStringAnsi::Strcmp(Cluster->GetUserData(), "CompensationCluster") == 0)
			{
				continue;
			}

			FbxNode* Link = Cluster->GetLink();
			// find the bone index
			int32 BoneIndex = -1;
			for (int32 LinkIndex = 0; LinkIndex < SortedLinks.Num(); LinkIndex++)
			{
				if (Link == SortedLinks[LinkIndex])
				{
					BoneIndex = LinkIndex;
					break;
				}
			}

			//	get the vertex indices
			int32 ControlPointIndicesCount = Cluster->GetControlPointIndicesCount();
			int32* ControlPointIndices = Cluster->GetControlPointIndices();
			double* Weights = Cluster->GetControlPointWeights();

			//	for each vertex index in the cluster
			for (int32 ControlPointIndex = 0; ControlPointIndex < ControlPointIndicesCount; ++ControlPointIndex)
			{
				ImportData.Influences.AddUninitialized();
				ImportData.Influences.Last().BoneIndex = BoneIndex;
				ImportData.Influences.Last().Weight = static_cast<float>(Weights[ControlPointIndex]);
				ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndices[ControlPointIndex];
			}
		}
	}
#else
	//For mmd. skining
	if (PmxMeshInfo->boneList.Num() > 0)
	{
		// create influences for each cluster
		//	for each vertex index in the cluster
		for (int32 ControlPointIndex = 0;
			ControlPointIndex < PmxMeshInfo->vertexList.Num();
			++ControlPointIndex)
		{
			int32 multiBone = 0;
			switch (PmxMeshInfo->vertexList[ControlPointIndex].WeightType)
			{
			case 0://0:BDEF1
				{
					ImportData.Influences.AddUninitialized();
					ImportData.Influences.Last().BoneIndex = PmxMeshInfo->vertexList[ControlPointIndex].BoneIndex[0];
					ImportData.Influences.Last().Weight = PmxMeshInfo->vertexList[ControlPointIndex].BoneWeight[0];
					ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndex;
				}
				break;
			case 1:// 1:BDEF2
				{
					for (multiBone = 0; multiBone < 2; ++multiBone)
					{
						ImportData.Influences.AddUninitialized();
						ImportData.Influences.Last().BoneIndex = PmxMeshInfo->vertexList[ControlPointIndex].BoneIndex[multiBone];
						ImportData.Influences.Last().Weight = PmxMeshInfo->vertexList[ControlPointIndex].BoneWeight[multiBone];
						ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndex;
					}
					//UE_LOG(LogMMD4UE4_PMXFactory, Log, TEXT("BDEF2"), "");
				}
				break;
			case 2: //2:BDEF4
				{
					for ( multiBone = 0; multiBone < 4; ++multiBone)
					{
						ImportData.Influences.AddUninitialized();
						ImportData.Influences.Last().BoneIndex = PmxMeshInfo->vertexList[ControlPointIndex].BoneIndex[multiBone];
						ImportData.Influences.Last().Weight = PmxMeshInfo->vertexList[ControlPointIndex].BoneWeight[multiBone];
						ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndex;
					}
				}
				break;
			case 3: //3:SDEF
				{
					//制限事項：SDEF
					//SDEFに関してはBDEF2に変換して扱うとする。
					//これは、SDEF_C、SDEF_R0、SDEF_R1に関するパラメータを設定する方法が不明なため。
					//別PF(ex.MMD4MecanimやDxlib)での実装例を元に解析及び情報収集し、
					//かつ、MMDでのSDEF動作の仕様を満たす方法を見つけられるまで保留、
					//SDEFの仕様（MMD）に関しては以下のページを参考にすること。
					//Ref :： みくだん 各ソフトによるSDEF変形の差異 - FC2
					// http://mikudan.blog120.fc2.com/blog-entry-339.html

					/////////////////////////////////////
					for ( multiBone = 0; multiBone < 2; ++multiBone)
					{
						ImportData.Influences.AddUninitialized();
						ImportData.Influences.Last().BoneIndex = PmxMeshInfo->vertexList[ControlPointIndex].BoneIndex[multiBone];
						ImportData.Influences.Last().Weight = PmxMeshInfo->vertexList[ControlPointIndex].BoneWeight[multiBone];
						ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndex;
					}
				}
				break;
#if 0 //for pmx ver 2.1 formnat
			case 4:
				// 制限事項：QDEF
				// QDEFに関して、MMDでの仕様を調べる事。
				{
					for (multiBone = 0; multiBone < 4; ++multiBone)
					{
						ImportData.Influences.AddUninitialized();
						ImportData.Influences.Last().BoneIndex = PmxMeshInfo->vertexList[ControlPointIndex].BoneIndex[multiBone];
						ImportData.Influences.Last().Weight = PmxMeshInfo->vertexList[ControlPointIndex].BoneWeight[multiBone];
						ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndex;
					}
				}
				break;
#endif
			default:
				{
					//異常系
					//0:BDEF1 形式と同じ手法で暫定対応する
					ImportData.Influences.AddUninitialized();
					ImportData.Influences.Last().BoneIndex = PmxMeshInfo->vertexList[ControlPointIndex].BoneIndex[0];
					ImportData.Influences.Last().Weight = PmxMeshInfo->vertexList[ControlPointIndex].BoneWeight[0];
					ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndex;
					UE_LOG(LogMMD4UE4_PMXFactory, Error, 
						TEXT("Unkown Weight Type :: type = %d , vertex index = %d "), 
						PmxMeshInfo->vertexList[ControlPointIndex].WeightType
						, ControlPointIndex
						);
				}
				break;
			}
		}

	}
#endif
	else // for rigid mesh
	{
		// find the bone index
		int32 BoneIndex = -1;
		/*for (int32 LinkIndex = 0; LinkIndex < SortedLinks.Num(); LinkIndex++)
		{
			// the bone is the node itself
			if (Node == SortedLinks[LinkIndex])
			{
				BoneIndex = LinkIndex;
				break;
			}
		}*/
		BoneIndex = 0;
		//	for each vertex in the mesh
		for (int32 ControlPointIndex = 0; ControlPointIndex < ControlPointsCount; ++ControlPointIndex)
		{
			ImportData.Influences.AddUninitialized();
			ImportData.Influences.Last().BoneIndex = BoneIndex;
			ImportData.Influences.Last().Weight = 1.0;
			ImportData.Influences.Last().VertexIndex = ExistPointNum + ControlPointIndex;
		}
	}
	/*
	//
	// clean up
	//
	if (UniqueUVCount > 0)
	{
		delete[] LayerElementUV;
		delete[] UVReferenceMode;
		delete[] UVMappingMode;
	}
	*/
#endif
	return true;
}


UObject* UPmxFactory::CreateAssetOfClass(
	UClass* AssetClass,
	FString ParentPackageName, 
	FString ObjectName,
	bool bAllowReplace
	)
{
	// See if this sequence already exists.
	UObject* 	ParentPkg = CreatePackage(NULL, *ParentPackageName);
	FString 	ParentPath = FString::Printf(
								TEXT("%s/%s"), 
								*FPackageName::GetLongPackagePath(*ParentPackageName),
								*ObjectName);
	UObject* 	Parent = CreatePackage(NULL, *ParentPath);
	// See if an object with this name exists
	UObject* Object = LoadObject<UObject>(Parent, *ObjectName, NULL, LOAD_None, NULL);

	// if object with same name but different class exists, warn user
	if ((Object != NULL) && (Object->GetClass() != AssetClass))
	{
		//UnFbx::FFbxImporter* FFbxImporter = UnFbx::FFbxImporter::GetInstance();
		//FFbxImporter->AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error, LOCTEXT("Error_AssetExist", "Asset with same name exists. Can't overwrite another asset")), FFbxErrors::Generic_SameNameAssetExists);
		return NULL;
	}

	// if object with same name exists, warn user
	if (Object != NULL && !bAllowReplace)
	{
		// until we have proper error message handling so we don't ask every time, but once, I'm disabling it. 
		// 		if ( EAppReturnType::Yes != FMessageDialog::Open( EAppMsgType::YesNo, LocalizeSecure(NSLOCTEXT("UnrealEd", "Error_AssetExistAsk", "Asset with the name (`~) exists. Would you like to overwrite?").ToString(), *ParentPath) ) ) 
		// 		{
		// 			return NULL;
		// 		}
		//UnFbx::FFbxImporter* FFbxImporter = UnFbx::FFbxImporter::GetInstance();
		//FFbxImporter->AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Warning, FText::Format(LOCTEXT("FbxSkeletaLMeshimport_SameNameExist", "Asset with the name ('{0}') exists. Overwriting..."), FText::FromString(ParentPath))), FFbxErrors::Generic_SameNameAssetOverriding);
	}

	if (Object == NULL)
	{
		// add it to the set
		// do not add to the set, now create independent asset
		Object = NewObject<UObject>(Parent, AssetClass, *ObjectName, RF_Public | RF_Standalone);
		Object->MarkPackageDirty();
		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(Object);
	}

	return Object;
}


/**
* A class encapsulating morph target processing that occurs during import on a separate thread
*/
class FAsyncImportMorphTargetWork : public FNonAbandonableTask
{
public:
	FAsyncImportMorphTargetWork(
		USkeletalMesh* InTempSkelMesh,
		int32 InLODIndex,
		FSkeletalMeshImportData& InImportData, 
		bool bInKeepOverlappingVertices
		)
			: TempSkeletalMesh(InTempSkelMesh)
			, ImportData(InImportData)
			, LODIndex(InLODIndex)
			, bKeepOverlappingVertices(bInKeepOverlappingVertices)
	{
		MeshUtilities = &FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
	}

	void DoWork()
	{
		TArray<FVector> LODPoints;
		TArray<SkeletalMeshImportData::FMeshWedge> LODWedges;
		TArray<SkeletalMeshImportData::FMeshFace> LODFaces;
		TArray<SkeletalMeshImportData::FVertInfluence> LODInfluences;
		TArray<int32> LODPointToRawMap;
		ImportData.CopyLODImportData(LODPoints, LODWedges, LODFaces, LODInfluences, LODPointToRawMap);

		ImportData.Empty();
#if 0	/* under ~ UR4.10 */
		MeshUtilities->BuildSkeletalMesh(
			TempSkeletalMesh->GetImportedResource()->LODModels[0],
			TempSkeletalMesh->RefSkeleton,
			LODInfluences, 
			LODWedges, 
			LODFaces,
			LODPoints,
			LODPointToRawMap, 
			bKeepOverlappingVertices
			);
#else	/* UE4.11 ~ over */
		MeshUtilities->BuildSkeletalMesh(
			TempSkeletalMesh->GetImportedModel()->LODModels[0],
			TempSkeletalMesh->RefSkeleton,
			LODInfluences,
			LODWedges,
			LODFaces,
			LODPoints,
			LODPointToRawMap
			);
#endif
	}

	//UE4.7系まで
	static const TCHAR *Name()
	{
		return TEXT("FAsyncImportMorphTargetWork");
	}

	//UE4.8以降で利用する場合に必要
#if 1
	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FAsyncImportMorphTargetWork, STATGROUP_ThreadPoolAsyncTasks);
	}
#endif

private:
	USkeletalMesh* TempSkeletalMesh;
	FSkeletalMeshImportData ImportData;
	IMeshUtilities* MeshUtilities;
	int32 LODIndex;
	bool bKeepOverlappingVertices;
};

void UPmxFactory::ImportMorphTargetsInternal(
	//TArray<FbxNode*>& SkelMeshNodeArray,
	MMD4UE4::PmxMeshInfo & PmxMeshInfo,
	USkeletalMesh* BaseSkelMesh,
	UObject* InParent,
	const FString& InFilename,
	int32 LODIndex
	)
{
	/*FbxString ShapeNodeName;
	TMap<FString, TArray<FbxShape*>> ShapeNameToShapeArray;*/
	TMap<FString, MMD4UE4::PMX_MORPH>  ShapeNameToShapeArray;

	// Temp arrays to keep track of data being used by threads
	TArray<USkeletalMesh*> TempMeshes;
	TArray<UMorphTarget*> MorphTargets;

	// Array of pending tasks that are not complete
	TIndirectArray<FAsyncTask<FAsyncImportMorphTargetWork> > PendingWork;

	GWarn->BeginSlowTask(NSLOCTEXT("FbxImporter", "BeginGeneratingMorphModelsTask", "Generating Morph Models"), true);

#if 0
	// For each morph in FBX geometries, we create one morph target for the Unreal skeletal mesh
	for (int32 NodeIndex = 0; NodeIndex < SkelMeshNodeArray.Num(); NodeIndex++)
	{
		FbxGeometry* Geometry = (FbxGeometry*)SkelMeshNodeArray[NodeIndex]->GetNodeAttribute();
		if (Geometry)
		{
			const int32 BlendShapeDeformerCount = Geometry->GetDeformerCount(FbxDeformer::eBlendShape);

			/************************************************************************/
			/* collect all the shapes                                               */
			/************************************************************************/
			for (int32 BlendShapeIndex = 0; BlendShapeIndex<BlendShapeDeformerCount; ++BlendShapeIndex)
			{
				FbxBlendShape* BlendShape = (FbxBlendShape*)Geometry->GetDeformer(BlendShapeIndex, FbxDeformer::eBlendShape);
				const int32 BlendShapeChannelCount = BlendShape->GetBlendShapeChannelCount();

				FString BlendShapeName = ANSI_TO_TCHAR(MakeName(BlendShape->GetName()));

				// see below where this is used for explanation...
				const bool bMightBeBadMAXFile = (BlendShapeName == FString("Morpher"));

				for (int32 ChannelIndex = 0; ChannelIndex<BlendShapeChannelCount; ++ChannelIndex)
				{
					FbxBlendShapeChannel* Channel = BlendShape->GetBlendShapeChannel(ChannelIndex);
					if (Channel)
					{
						//Find which shape should we use according to the weight.
						const int32 CurrentChannelShapeCount = Channel->GetTargetShapeCount();

						FString ChannelName = ANSI_TO_TCHAR(MakeName(Channel->GetName()));

						// Maya adds the name of the blendshape and an underscore to the front of the channel name, so remove it
						if (ChannelName.StartsWith(BlendShapeName))
						{
							ChannelName = ChannelName.Right(ChannelName.Len() - (BlendShapeName.Len() + 1));
						}

						for (int32 ShapeIndex = 0; ShapeIndex<CurrentChannelShapeCount; ++ShapeIndex)
						{
							FbxShape* Shape = Channel->GetTargetShape(ShapeIndex);

							FString ShapeName;
							if (CurrentChannelShapeCount > 1)
							{
								ShapeName = ANSI_TO_TCHAR(MakeName(Shape->GetName()));
							}
							else
							{
								if (bMightBeBadMAXFile)
								{
									ShapeName = ANSI_TO_TCHAR(MakeName(Shape->GetName()));
								}
								else
								{
									// Maya concatenates the number of the shape to the end of its name, so instead use the name of the channel
									ShapeName = ChannelName;
								}
							}

							TArray<FbxShape*> & ShapeArray = ShapeNameToShapeArray.FindOrAdd(ShapeName);
							if (ShapeArray.Num() == 0)
							{
								ShapeArray.AddZeroed(SkelMeshNodeArray.Num());
							}

							ShapeArray[NodeIndex] = Shape;
						}
					}
				}
			}
		}
	} // for NodeIndex
#else
	for (int32 NodeIndex = 0; NodeIndex < PmxMeshInfo.morphList.Num(); NodeIndex++)
	{
		MMD4UE4::PMX_MORPH * pmxMorphPtr = &(PmxMeshInfo.morphList[NodeIndex]);
		if (pmxMorphPtr->Type == 1 && pmxMorphPtr->Vertex.Num()>0)
		{//頂点Morph

			FString ShapeName = pmxMorphPtr->Name;
			MMD4UE4::PMX_MORPH & ShapeArray
				= ShapeNameToShapeArray.FindOrAdd(ShapeName);
			ShapeArray = *pmxMorphPtr;
		}
	}
#endif

	int32 ShapeIndex = 0;
	int32 TotalShapeCount = ShapeNameToShapeArray.Num();
	// iterate through shapename, and create morphtarget
	for (auto Iter = ShapeNameToShapeArray.CreateIterator(); Iter; ++Iter)
	{
		FString ShapeName = Iter.Key();
		MMD4UE4::PMX_MORPH & ShapeArray = Iter.Value();

		//FString ShapeName = PmxMeshInfo.morphList[0].Name;
		//copy pmx meta date -> to this morph marge vertex data
		MMD4UE4::PmxMeshInfo ShapePmxMeshInfo = PmxMeshInfo;
		for (int tempVertexIndx = 0; tempVertexIndx< ShapeArray.Vertex.Num(); tempVertexIndx++)
		{
			MMD4UE4::PMX_MORPH_VERTEX tempMorphVertex = ShapeArray.Vertex[tempVertexIndx];
			//
			check(tempMorphVertex.Index <= ShapePmxMeshInfo.vertexList.Num());
			ShapePmxMeshInfo.vertexList[tempMorphVertex.Index].Position
				+= tempMorphVertex.Offset;
		}

		FFormatNamedArguments Args;
		Args.Add(TEXT("ShapeName"), FText::FromString(ShapeName));
		Args.Add(TEXT("CurrentShapeIndex"), ShapeIndex + 1);
		Args.Add(TEXT("TotalShapes"), TotalShapeCount);
		const FText StatusUpate
			= FText::Format(
				NSLOCTEXT(
					"FbxImporter", 
					"GeneratingMorphTargetMeshStatus",
					"Generating morph target mesh {ShapeName} ({CurrentShapeIndex} of {TotalShapes})"
					),
				Args);

		GWarn->StatusUpdate(ShapeIndex + 1, TotalShapeCount, StatusUpate);

		UE_LOG(LogMMD4UE4_PMXFactory, Warning, TEXT("%d_%s"),__LINE__, *(StatusUpate.ToString()) );

		FSkeletalMeshImportData ImportData;

		// See if this morph target already exists.
		UMorphTarget * Result = FindObject<UMorphTarget>(BaseSkelMesh, *ShapeName);
		// we only create new one for LOD0, otherwise don't create new one
		if (!Result)
		{
			if (LODIndex == 0)
			{
				Result = NewObject<UMorphTarget>(BaseSkelMesh, FName(*ShapeName));
			}
			else
			{
				AddTokenizedErrorMessage(
					FTokenizedMessage::Create(
						EMessageSeverity::Error,
						FText::Format(
							FText::FromString(
								"Could not find the {0} morphtarget for LOD {1}. \
								Make sure the name for morphtarget matches with LOD 0"), 
							FText::FromString(ShapeName),
							FText::FromString(FString::FromInt(LODIndex))
						)
					),
					FFbxErrors::SkeletalMesh_LOD_MissingMorphTarget
					);
			}
		}

		if (Result)
		{
			//Test
			//PmxMeshInfo.vertexList[0].Position = FVector::ZeroVector;

			// now we get a shape for whole mesh, import to unreal as a morph target
			// @todo AssetImportData do we need import data for this temp mesh?
			USkeletalMesh* TmpSkeletalMesh 
				= (USkeletalMesh*)ImportSkeletalMesh(
					GetTransientPackage(),
					&ShapePmxMeshInfo,//PmxMeshInfo,//SkelMeshNodeArray, 
					NAME_None,
					(EObjectFlags)0, 
					//TmpMeshImportData,
					FPaths::GetBaseFilename(InFilename), 
					//&ShapeArray, 
					&ImportData,
					false
					);
			TempMeshes.Add(TmpSkeletalMesh);
			MorphTargets.Add(Result);

			
			// Process the skeletal mesh on a separate thread
			FAsyncTask<FAsyncImportMorphTargetWork>* NewWork
				= new (PendingWork)FAsyncTask<FAsyncImportMorphTargetWork>(
					TmpSkeletalMesh,
					LODIndex,
					ImportData,
					true// ImportOptions->bKeepOverlappingVertices
					);
			NewWork->StartBackgroundTask();
			
			++ShapeIndex;
		}
	}
#if 1
	// Wait for all importing tasks to complete
	int32 NumCompleted = 0;
	int32 NumTasks = PendingWork.Num();
	do
	{
		// Check for completed async compression tasks.
		int32 NumNowCompleted = 0;
		for (int32 TaskIndex = 0; TaskIndex < PendingWork.Num(); ++TaskIndex)
		{
			if (PendingWork[TaskIndex].IsDone())
			{
				NumNowCompleted++;
			}
		}
		if (NumNowCompleted > NumCompleted)
		{
			NumCompleted = NumNowCompleted;
			FFormatNamedArguments Args;
			Args.Add(TEXT("NumCompleted"), NumCompleted);
			Args.Add(TEXT("NumTasks"), NumTasks);
			GWarn->StatusUpdate(
				NumCompleted,
				NumTasks, 
				FText::Format(
					LOCTEXT(
						"ImportingMorphTargetStatus",
						"Importing Morph Target: {NumCompleted} of {NumTasks}"),
					Args)
				);
		}
		FPlatformProcess::Sleep(0.1f);
	} while (NumCompleted < NumTasks);
#endif
	// Create morph streams for each morph target we are importing.
	// This has to happen on a single thread since the skeletal meshes' bulk data is locked and cant be accessed by multiple threads simultaneously
	for (int32 Index = 0; Index < TempMeshes.Num(); Index++)
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("NumCompleted"), Index + 1);
		Args.Add(TEXT("NumTasks"), TempMeshes.Num());
		GWarn->StatusUpdate(
			Index + 1,
			TempMeshes.Num(),
			FText::Format(
				LOCTEXT("BuildingMorphTargetRenderDataStatus",
					"Building Morph Target Render Data: {NumCompleted} of {NumTasks}"),
				Args)
			);

		UMorphTarget* MorphTarget = MorphTargets[Index];
		USkeletalMesh* TmpSkeletalMesh = TempMeshes[Index];

		FMorphMeshRawSource TargetMeshRawData(TmpSkeletalMesh);
		FMorphMeshRawSource BaseMeshRawData(BaseSkelMesh, LODIndex);

		FSkeletalMeshLODModel & BaseLODModel = BaseSkelMesh->GetImportedModel()->LODModels[LODIndex];
		FMorphTargetLODModel Result;
		FMorphMeshRawSource::CalculateMorphTargetLODModel(BaseMeshRawData, TargetMeshRawData, Result);

		MorphTarget->PopulateDeltas(Result.Vertices, LODIndex, BaseLODModel.Sections, true);
		// register does mark package as dirty
		if (MorphTarget->HasValidData())
		{
			BaseSkelMesh->RegisterMorphTarget(MorphTarget);
		}
	}

	GWarn->EndSlowTask();
#if 0
#endif
}

// Import Morph target
void UPmxFactory::ImportFbxMorphTarget(
	//TArray<FbxNode*> &SkelMeshNodeArray,
	MMD4UE4::PmxMeshInfo & PmxMeshInfo,
	USkeletalMesh* BaseSkelMesh, 
	UObject* InParent, 
	const FString& Filename, 
	int32 LODIndex
	)
{
	bool bHasMorph = false;
//	int32 NodeIndex;
	// check if there are morph in this geometry
	/*
	for (NodeIndex = 0; NodeIndex < SkelMeshNodeArray.Num(); NodeIndex++)
	{
		FbxGeometry* Geometry = (FbxGeometry*)SkelMeshNodeArray[NodeIndex]->GetNodeAttribute();
		if (Geometry)
		{
			bHasMorph = Geometry->GetDeformerCount(FbxDeformer::eBlendShape) > 0;
			if (bHasMorph)
			{
				break;
			}
		}
	}

	if (bHasMorph)*/
	if (PmxMeshInfo.morphList.Num()>0)
	{
		ImportMorphTargetsInternal(
			//SkelMeshNodeArray, 
			PmxMeshInfo,
			BaseSkelMesh, 
			InParent,
			Filename, 
			LODIndex
			);
	}
}



//////////////////////////////

void UPmxFactory::AddTokenizedErrorMessage(
	TSharedRef<FTokenizedMessage> ErrorMsg, 
	FName FbxErrorName
	)
{
	// check to see if Logger exists, this way, we guarantee only prints to FBX import
	// when we meant to print
	/*if (Logger)
	{
		Logger->TokenizedErrorMessages.Add(Error);

		if (FbxErrorName != NAME_None)
		{
			Error->AddToken(FFbxErrorToken::Create(FbxErrorName));
		}
	}
	else*/
	{
		// if not found, use normal log
		switch (ErrorMsg->GetSeverity())
		{
		case EMessageSeverity::Error:
			UE_LOG(LogMMD4UE4_PMXFactory, Error, TEXT("%d_%s"), __LINE__, *(ErrorMsg->ToText().ToString()));
			break;
		case EMessageSeverity::CriticalError:
			UE_LOG(LogMMD4UE4_PMXFactory, Error, TEXT("%d_%s"), __LINE__, *(ErrorMsg->ToText().ToString()));
			break;
		case EMessageSeverity::Warning:
			UE_LOG(LogMMD4UE4_PMXFactory, Warning, TEXT("%d_%s"), __LINE__, *(ErrorMsg->ToText().ToString()));
			break;
		default:
			UE_LOG(LogMMD4UE4_PMXFactory, Warning, TEXT("%d_%s"), __LINE__, *(ErrorMsg->ToText().ToString()));
			break;
		}
	}
}

#undef LOCTEXT_NAMESPACE