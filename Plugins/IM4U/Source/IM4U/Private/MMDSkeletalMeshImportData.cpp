// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "MMDSkeletalMeshImportData.h"
#include "IM4UPrivatePCH.h"
#include "CoreMinimal.h"

UMMDSkeletalMeshImportData::UMMDSkeletalMeshImportData(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bImportMeshesInBoneHierarchy(true)
{
}

UMMDSkeletalMeshImportData* UMMDSkeletalMeshImportData::GetImportDataForSkeletalMesh(USkeletalMesh* SkeletalMesh, UMMDSkeletalMeshImportData* TemplateForCreation)
{
	check(SkeletalMesh);
	
	UMMDSkeletalMeshImportData* ImportData = Cast<UMMDSkeletalMeshImportData>(SkeletalMesh->AssetImportData);
	if ( !ImportData )
	{
		ImportData = NewObject<UMMDSkeletalMeshImportData>(SkeletalMesh, NAME_None, RF_NoFlags, TemplateForCreation);

		// Try to preserve the source file path if possible
		if ( SkeletalMesh->AssetImportData != NULL )
		{
			ImportData->SourceData = SkeletalMesh->AssetImportData->SourceData;
		}

		SkeletalMesh->AssetImportData = ImportData;
	}

	return ImportData;
}

bool UMMDSkeletalMeshImportData::CanEditChange(const UProperty* InProperty) const
{
	bool bMutable = Super::CanEditChange(InProperty);
	UObject* Outer = GetOuter();
	if(Outer && bMutable)
	{
		// Let the MMDImportUi object handle the editability of our properties
		bMutable = Outer->CanEditChange(InProperty);
	}
	return bMutable;
}