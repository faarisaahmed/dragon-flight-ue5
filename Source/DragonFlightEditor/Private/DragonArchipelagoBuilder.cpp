#include "DragonArchipelagoBuilder.h"
#include "DragonFlight.h"
#include "DragonSky.h"
#include "DragonScatter.h"
#include "DragonWorldSubsystem.h"

#include "Editor.h"
#include "FileHelpers.h"
#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/TextureFactory.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "DragonSea.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionLandscapeGrassOutput.h"
#include "LandscapeGrassType.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "RenderUtils.h"
#include "Animation/Skeleton.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyOceanComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyComponent.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "WaterZoneActor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

#include "LandscapeShader.inl"

namespace
{
	const TCHAR* GroundLayers[] = { TEXT("Cliff"), TEXT("Scree"), TEXT("Grass"), TEXT("Heath"), TEXT("Forest"), TEXT("Sand"), TEXT("Shingle"), TEXT("Snow"), TEXT("Mud") };
	const TCHAR* FarLayers[] = { TEXT("FarRock"), TEXT("FarGrass"), TEXT("FarSnow") };
	const FString TexRoot = TEXT("/Game/World/Textures");
	const FString MatRoot = TEXT("/Game/World/Materials");
	const FString ModelRoot = TEXT("/Game/World/Models");
	const FString MapPath = TEXT("/Game/Maps/Archipelago");

	void Log(const FString& S) { UE_LOG(LogDragon, Display, TEXT("[Build] %s"), *S); }

	UTexture2D* LoadTex(const FString& Path) { return LoadObject<UTexture2D>(nullptr, *Path); }

	/** Find an existing asset or make an empty one with a factory. */
	template<class T, class TFactory>
	T* CreateOrLoad(const FString& PackagePath, const FString& Name, TFunctionRef<void(TFactory*)> Setup)
	{
		const FString Full = PackagePath / Name;
		if (T* Existing = LoadObject<T>(nullptr, *(Full + TEXT(".") + Name))) return Existing;
		TFactory* F = NewObject<TFactory>();
		Setup(F);
		UPackage* Pkg = CreatePackage(*Full);
		T* Obj = Cast<T>(F->FactoryCreateNew(T::StaticClass(), Pkg, FName(*Name), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
		if (Obj) FAssetRegistryModule::AssetCreated(Obj);
		return Obj;
	}

	UMaterial* NewMaterial(const FString& Name)
	{
		UMaterial* M = CreateOrLoad<UMaterial, UMaterialFactoryNew>(MatRoot, Name, [](UMaterialFactoryNew*) {});
		if (M)
		{
			UMaterialEditingLibrary::DeleteAllMaterialExpressions(M);
			// Some node types (custom outputs such as the Single Layer Water output) can
			// survive that; a rebuilt material must start empty or it gains duplicates.
			TArray<UMaterialExpression*> Left(M->GetExpressions());
			for (UMaterialExpression* E : Left) if (E) UMaterialEditingLibrary::DeleteMaterialExpression(M, E);
			if (M->GetExpressions().Num() > 0)
				UE_LOG(LogDragon, Warning, TEXT("[Build] %s still has %d nodes after clearing"), *Name, M->GetExpressions().Num());
		}
		return M;
	}

	template<class T>
	T* Expr(UMaterial* M, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(M, T::StaticClass(), X, Y));
	}

	UMaterialExpressionTextureObjectParameter* TexObj(UMaterial* M, const FString& Name, UTexture2D* Default, EMaterialSamplerType Type, int32 Y)
	{
		UMaterialExpressionTextureObjectParameter* E = Expr<UMaterialExpressionTextureObjectParameter>(M, -1400, Y);
		E->ParameterName = FName(*Name);
		// A texture parameter must have a default, of a type matching its sampler, or the
		// whole material fails to compile (and every instance falls back to the grey grid).
		if (!Default)
		{
			const TCHAR* Which = Type == SAMPLERTYPE_Normal ? TEXT("Normal")
				: Type == SAMPLERTYPE_Masks ? TEXT("ARM")
				: Type == SAMPLERTYPE_LinearGrayscale ? TEXT("Gray") : TEXT("Color");
			const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Defaults"));
			const FString DefName = FString::Printf(TEXT("T_Default_%s"), Which);
			Default = FDragonArchipelagoBuilder::ImportTexture(Dir / (DefName + TEXT(".png")), TEXT("/Game/World/Textures/Defaults"), DefName,
				Type == SAMPLERTYPE_Normal, Type == SAMPLERTYPE_Masks || Type == SAMPLERTYPE_LinearGrayscale);
			if (Default && Type == SAMPLERTYPE_LinearGrayscale && Default->CompressionSettings != TC_Grayscale)
			{
				Default->CompressionSettings = TC_Grayscale;
				Default->PostEditChange();
				UEditorAssetLibrary::SaveLoadedAsset(Default, false);
			}
		}
		E->Texture = Default;
		E->SamplerType = Type;
		E->SamplerSource = SSM_Wrap_WorldGroupSettings; // shared samplers: lifts the 16-sampler limit
		return E;
	}

	UMaterialExpressionTextureSampleParameter2D* TexParam(UMaterial* M, const FString& Name, UTexture2D* Default, EMaterialSamplerType Type, int32 X, int32 Y)
	{
		UMaterialExpressionTextureSampleParameter2D* E = Expr<UMaterialExpressionTextureSampleParameter2D>(M, X, Y);
		E->ParameterName = FName(*Name);
		// A texture parameter must have a default, of a type matching its sampler, or the
		// whole material fails to compile (and every instance falls back to the grey grid).
		if (!Default)
		{
			const TCHAR* Which = Type == SAMPLERTYPE_Normal ? TEXT("Normal")
				: Type == SAMPLERTYPE_Masks ? TEXT("ARM")
				: Type == SAMPLERTYPE_LinearGrayscale ? TEXT("Gray") : TEXT("Color");
			const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Defaults"));
			const FString DefName = FString::Printf(TEXT("T_Default_%s"), Which);
			Default = FDragonArchipelagoBuilder::ImportTexture(Dir / (DefName + TEXT(".png")), TEXT("/Game/World/Textures/Defaults"), DefName,
				Type == SAMPLERTYPE_Normal, Type == SAMPLERTYPE_Masks || Type == SAMPLERTYPE_LinearGrayscale);
			if (Default && Type == SAMPLERTYPE_LinearGrayscale && Default->CompressionSettings != TC_Grayscale)
			{
				Default->CompressionSettings = TC_Grayscale;
				Default->PostEditChange();
				UEditorAssetLibrary::SaveLoadedAsset(Default, false);
			}
		}
		E->Texture = Default;
		E->SamplerType = Type;
		E->SamplerSource = SSM_Wrap_WorldGroupSettings;
		return E;
	}

	void Finish(UMaterial* M)
	{
		UMaterialEditingLibrary::RecompileMaterial(M);
		M->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(M, false);
	}

	TSharedPtr<FJsonObject> LoadWorldJson()
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(UDragonWorldSubsystem::DataDir() / TEXT("World.json")))) return nullptr;
		TSharedPtr<FJsonObject> J;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(R, J);
		return J;
	}

	/** UWaterBodyComponent has no public setter for bAffectsLandscape; set it through reflection so it compiles whatever its access level. */
	void SetAffectsLandscape(UWaterBodyComponent* C, bool bAffects)
	{
		if (!C) return;
		if (FBoolProperty* P = FindFProperty<FBoolProperty>(UWaterBodyComponent::StaticClass(), TEXT("bAffectsLandscape")))
		{
			P->SetPropertyValue_InContainer(C, bAffects);
		}
		else
		{
			Log(TEXT("! UWaterBodyComponent::bAffectsLandscape not found"));
		}
	}
}

FString FDragonArchipelagoBuilder::DownloadsDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Downloads"));
}

// ===========================================================================
// Textures
// ===========================================================================
UTexture2D* FDragonArchipelagoBuilder::ImportTexture(const FString& File, const FString& PackagePath, const FString& AssetName, bool bNormal, bool bLinear)
{
	if (!FPaths::FileExists(File)) return nullptr;
	const FString Full = PackagePath / AssetName;
	if (UTexture2D* Existing = LoadTex(Full + TEXT(".") + AssetName))
	{
		// Our materials sample these as ordinary textures; virtual ones won't bind.
		if (Existing->VirtualTextureStreaming)
		{
			Existing->VirtualTextureStreaming = false;
			Existing->PostEditChange();
			UEditorAssetLibrary::SaveLoadedAsset(Existing, false);
		}
		return Existing;
	}

	UTextureFactory* F = NewObject<UTextureFactory>();
	F->AddToRoot();
	F->SuppressImportOverwriteDialog();
	UPackage* Pkg = CreatePackage(*Full);
	bool bCancel = false;
	UTexture2D* Tex = Cast<UTexture2D>(F->ImportObject(UTexture2D::StaticClass(), Pkg, FName(*AssetName), RF_Public | RF_Standalone, File, nullptr, bCancel));
	F->RemoveFromRoot();
	if (!Tex) { Log(FString::Printf(TEXT("! failed to import %s"), *File)); return nullptr; }

	if (bNormal)
	{
		Tex->CompressionSettings = TC_Normalmap;
		Tex->SRGB = false;
		Tex->LODGroup = TEXTUREGROUP_WorldNormalMap;
	}
	else if (bLinear)
	{
		Tex->CompressionSettings = TC_Masks;
		Tex->SRGB = false;
	}
	Tex->VirtualTextureStreaming = false;
	Tex->PostEditChange();
	FAssetRegistryModule::AssetCreated(Tex);
	Tex->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(Tex, false);
	return Tex;
}

void FDragonArchipelagoBuilder::ImportGroundTextures()
{
	const FString Src = DownloadsDir() / TEXT("Textures");
	auto ImportSet = [&](const FString& Key, bool bFarOnlyDiffuse)
	{
		const FString Dir = Src / Key;
		auto Find = [&](const FString& Map) -> FString
		{
			for (const TCHAR* Ext : { TEXT(".png"), TEXT(".jpg") })
			{
				const FString P = Dir / FString::Printf(TEXT("T_%s_%s%s"), *Key, *Map, Ext);
				if (FPaths::FileExists(P)) return P;
			}
			return FString();
		};
		ImportTexture(Find(TEXT("diff")), TexRoot / Key, FString::Printf(TEXT("T_%s_D"), *Key), false, false);
		if (bFarOnlyDiffuse) return;
		ImportTexture(Find(TEXT("nor_gl")), TexRoot / Key, FString::Printf(TEXT("T_%s_N"), *Key), true, false);
		ImportTexture(Find(TEXT("arm")), TexRoot / Key, FString::Printf(TEXT("T_%s_A"), *Key), false, true);
	};
	for (const TCHAR* L : GroundLayers) ImportSet(L, false);
	for (const TCHAR* L : FarLayers) ImportSet(L, true);

	// Drainage from the erosion bake: wet gully floors in M_Archipelago.
	{
		const FString FFile = UDragonWorldSubsystem::DataDir() / TEXT("Flow_4096.png");
		if (UTexture2D* FT = ImportTexture(FFile, TEXT("/Game/World/Data"), TEXT("T_Flow"), false, true))
		{
			if (FT->CompressionSettings != TC_Grayscale)
			{
				FT->CompressionSettings = TC_Grayscale; FT->SRGB = false;
				FT->AddressX = TA_Clamp; FT->AddressY = TA_Clamp;
				FT->PostEditChange();
				UEditorAssetLibrary::SaveLoadedAsset(FT, false);
			}
		}
	}
	// The heightmap as a texture, for water depth (shore foam, colour) in M_Sea.
	{
		const FString HFile = UDragonWorldSubsystem::DataDir() / TEXT("Heightmap_4033.png");
		if (UTexture2D* HT = ImportTexture(HFile, TEXT("/Game/World/Data"), TEXT("T_Height"), false, true))
		{
			if (HT->CompressionSettings != TC_Grayscale || HT->SRGB || HT->Filter != TF_Bilinear)
			{
				HT->CompressionSettings = TC_Grayscale; // 16-bit source -> G16
				HT->SRGB = false;
				HT->Filter = TF_Bilinear;
				HT->AddressX = TA_Clamp; HT->AddressY = TA_Clamp;
				HT->MipGenSettings = TMGS_NoMipmaps;
				HT->PostEditChange();
				UEditorAssetLibrary::SaveLoadedAsset(HT, false);
			}
		}
	}
	// The baked masks, read in the landscape material by world position.
	TArray<FString> Masks;
	IFileManager::Get().FindFiles(Masks, *(UDragonWorldSubsystem::DataDir() / TEXT("Masks*.png")), true, false);
	for (const FString& F : Masks)
	{
		const bool b2 = F.StartsWith(TEXT("Masks2"));
		UTexture2D* T = ImportTexture(UDragonWorldSubsystem::DataDir() / F, TEXT("/Game/World/Data"), b2 ? TEXT("T_Masks2") : TEXT("T_Masks"), false, true);
		if (T)
		{
			T->CompressionSettings = TC_VectorDisplacementmap; // uncompressed RGBA8: masks must not block-compress
			T->AddressX = TA_Clamp;
			T->AddressY = TA_Clamp;
			T->MipGenSettings = TMGS_NoMipmaps;
			T->PostEditChange();
			UEditorAssetLibrary::SaveLoadedAsset(T, false);
		}
	}
	Log(TEXT("ground textures imported"));
}

// ===========================================================================
// Materials
// ===========================================================================
UMaterial* FDragonArchipelagoBuilder::CreateLandscapeMaterial()
{
	UMaterial* M = NewMaterial(TEXT("M_Archipelago"));
	if (!M) return nullptr;
	M->bTangentSpaceNormal = false;           // the shader writes a world-space normal
	M->SetShadingModel(MSM_DefaultLit);

	UMaterialExpressionCustom* Cu = Expr<UMaterialExpressionCustom>(M, -400, 0);
	Cu->Description = TEXT("Archipelago ground");
	Cu->Code = GLandscapeHLSL;
	Cu->OutputType = CMOT_Float3;
	Cu->Inputs.Reset();
	Cu->AdditionalOutputs.Reset();
	auto AddIn = [&](const TCHAR* Name) { FCustomInput I; I.InputName = Name; Cu->Inputs.Add(I); };
	AddIn(TEXT("P")); AddIn(TEXT("Nv")); AddIn(TEXT("Cam")); AddIn(TEXT("WS")); AddIn(TEXT("SnowLine"));
	AddIn(TEXT("MaskA")); AddIn(TEXT("MaskB")); AddIn(TEXT("FlowMask"));
	for (const TCHAR* L : GroundLayers) { AddIn(*FString::Printf(TEXT("%sD"), L)); AddIn(*FString::Printf(TEXT("%sN"), L)); AddIn(*FString::Printf(TEXT("%sA"), L)); }
	for (const TCHAR* L : FarLayers) AddIn(*FString::Printf(TEXT("%sD"), L));
	{ FCustomOutput O; O.OutputName = TEXT("NormalWS"); O.OutputType = CMOT_Float3; Cu->AdditionalOutputs.Add(O); }
	{ FCustomOutput O; O.OutputName = TEXT("Rough"); O.OutputType = CMOT_Float1; Cu->AdditionalOutputs.Add(O); }
	{ FCustomOutput O; O.OutputName = TEXT("AmbOcc"); O.OutputType = CMOT_Float1; Cu->AdditionalOutputs.Add(O); }
	{ FCustomOutput O; O.OutputName = TEXT("GrassW"); O.OutputType = CMOT_Float1; Cu->AdditionalOutputs.Add(O); }
	{ FCustomOutput O; O.OutputName = TEXT("HerbW"); O.OutputType = CMOT_Float1; Cu->AdditionalOutputs.Add(O); }
	// VERIFY(5.5): RebuildOutputs() must be ENGINE_API-exported (class is MinimalAPI) or this fails to link;
	// alternative: Cu->PostEditChangeProperty(FPropertyChangedEvent(<AdditionalOutputs FProperty>)).
	Cu->RebuildOutputs();
	Cu->PostEditChange();

	int32 Y = -1200;
	UMaterialExpressionWorldPosition* WP = Expr<UMaterialExpressionWorldPosition>(M, -1000, Y);
	WP->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	UMaterialEditingLibrary::ConnectMaterialExpressions(WP, TEXT(""), Cu, TEXT("P"));
	UMaterialExpressionVertexNormalWS* VN = Expr<UMaterialExpressionVertexNormalWS>(M, -1000, Y + 80);
	UMaterialEditingLibrary::ConnectMaterialExpressions(VN, TEXT(""), Cu, TEXT("Nv"));
	UMaterialExpressionCameraPositionWS* CP = Expr<UMaterialExpressionCameraPositionWS>(M, -1000, Y + 160);
	UMaterialEditingLibrary::ConnectMaterialExpressions(CP, TEXT(""), Cu, TEXT("Cam"));

	double SizeM = 45000.0, Snow = 480.0;
	if (TSharedPtr<FJsonObject> J = LoadWorldJson())
	{
		SizeM = J->GetNumberField(TEXT("terrainSize"));
		Snow = J->GetNumberField(TEXT("snowLine"));
	}
	UMaterialExpressionScalarParameter* WS = Expr<UMaterialExpressionScalarParameter>(M, -1000, Y + 240);
	WS->ParameterName = TEXT("WorldSizeM"); WS->DefaultValue = SizeM;
	UMaterialEditingLibrary::ConnectMaterialExpressions(WS, TEXT(""), Cu, TEXT("WS"));
	UMaterialExpressionScalarParameter* SL = Expr<UMaterialExpressionScalarParameter>(M, -1000, Y + 320);
	SL->ParameterName = TEXT("SnowLineM"); SL->DefaultValue = Snow;
	UMaterialEditingLibrary::ConnectMaterialExpressions(SL, TEXT(""), Cu, TEXT("SnowLine"));

	Y = -800;
	auto Connect = [&](const FString& Param, const FString& TexPath, EMaterialSamplerType Type)
	{
		UMaterialExpressionTextureObjectParameter* T = TexObj(M, Param, LoadTex(TexPath), Type, Y);
		Y += 90;
		UMaterialEditingLibrary::ConnectMaterialExpressions(T, TEXT(""), Cu, Param);
	};
	Connect(TEXT("MaskA"), TEXT("/Game/World/Data/T_Masks.T_Masks"), SAMPLERTYPE_LinearColor);
	Connect(TEXT("MaskB"), TEXT("/Game/World/Data/T_Masks2.T_Masks2"), SAMPLERTYPE_LinearColor);
	Connect(TEXT("FlowMask"), TEXT("/Game/World/Data/T_Flow.T_Flow"), SAMPLERTYPE_LinearGrayscale);
	for (const TCHAR* L : GroundLayers)
	{
		const FString K(L);
		Connect(K + TEXT("D"), FString::Printf(TEXT("%s/%s/T_%s_D.T_%s_D"), *TexRoot, L, L, L), SAMPLERTYPE_Color);
		Connect(K + TEXT("N"), FString::Printf(TEXT("%s/%s/T_%s_N.T_%s_N"), *TexRoot, L, L, L), SAMPLERTYPE_Normal);
		Connect(K + TEXT("A"), FString::Printf(TEXT("%s/%s/T_%s_A.T_%s_A"), *TexRoot, L, L, L), SAMPLERTYPE_Masks);
	}
	for (const TCHAR* L : FarLayers)
	{
		Connect(FString(L) + TEXT("D"), FString::Printf(TEXT("%s/%s/T_%s_D.T_%s_D"), *TexRoot, L, L, L), SAMPLERTYPE_Color);
	}

	UMaterialEditingLibrary::ConnectMaterialProperty(Cu, TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Cu, TEXT("NormalWS"), MP_Normal);
	UMaterialEditingLibrary::ConnectMaterialProperty(Cu, TEXT("Rough"), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(Cu, TEXT("AmbOcc"), MP_AmbientOcclusion);
	// Soil, turf and rock are poor reflectors: a low specular level stops low-sun views turning
	// the ground into a pale glare.
	{
		UMaterialExpressionConstant* Sp = Expr<UMaterialExpressionConstant>(M, -200, 300);
		Sp->R = 0.12f; // below ~0.25 UE also damps the grazing Fresnel (scaled by 50*F0)
		UMaterialEditingLibrary::ConnectMaterialProperty(Sp, TEXT(""), MP_Specular);
	}

	// Turf and herbs, planted by the landscape grass system from the shader's own weights:
	// dense, GPU-instanced, generated around the camera — the scatter actor can't afford
	// grass at real density.
	{
		auto Pick = [](const FString& Dir, std::initializer_list<const TCHAR*> Needles)
		{
			TArray<UStaticMesh*> Out;
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TArray<FAssetData> Found;
			Registry.GetAssetsByPath(FName(*Dir), Found, true);
			for (const FAssetData& A : Found)
			{
				if (A.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName()) continue;
				const FString N = A.AssetName.ToString();
				if (N.Contains(TEXT("_LOD")) && !N.EndsWith(TEXT("_LOD0"))) continue;
				for (const TCHAR* K : Needles)
					if (N.Contains(K)) { if (UStaticMesh* SM = Cast<UStaticMesh>(A.GetAsset())) Out.Add(SM); break; }
			}
			return Out;
		};
		auto MakeGrass = [&](const TCHAR* Name, const TArray<UStaticMesh*>& Meshes, float Per100M2 /* UE's "per 10 m2" is per 10x10 m */, int32 StartCull, int32 EndCull, FVector2f Scale)
			-> ULandscapeGrassType*
		{
			if (Meshes.Num() == 0) { Log(FString::Printf(TEXT("! no meshes for %s"), Name)); return nullptr; }
			const FString Pkg = TEXT("/Game/World/Grass/") + FString(Name);
			UPackage* P = CreatePackage(*Pkg);
			ULandscapeGrassType* G = FindObject<ULandscapeGrassType>(P, Name);
			if (!G) G = NewObject<ULandscapeGrassType>(P, Name, RF_Public | RF_Standalone);
			G->GrassVarieties.Reset();
			for (UStaticMesh* SM : Meshes)
			{
				FGrassVariety V;
				V.GrassMesh = SM;
				V.GrassDensity = Per100M2 / Meshes.Num();
				V.StartCullDistance = StartCull; V.EndCullDistance = EndCull;
				V.ScaleX = FFloatInterval(Scale.X, Scale.Y); V.ScaleY = V.ScaleX; V.ScaleZ = V.ScaleX;
				V.Scaling = EGrassScaling::Uniform;
				V.RandomRotation = true; V.AlignToSurface = true;
				V.bUseGrid = true; V.PlacementJitter = 1.f;
				V.bWeightAttenuatesMaxScale = true; V.MaxScaleWeightAttenuation = 0.4f;
				G->GrassVarieties.Add(V);
			}
			G->PostEditChange();
			FAssetRegistryModule::AssetCreated(G);
			UEditorAssetLibrary::SaveLoadedAsset(G, false);
			Log(FString::Printf(TEXT("grass %s: %d meshes, %.1f per m2"), Name, Meshes.Num(), Per100M2 / 100.f));
			return G;
		};
		const FString Gr = ModelRoot / TEXT("ground");
		ULandscapeGrassType* Meadow = MakeGrass(TEXT("LGT_Meadow"),
			Pick(Gr, { TEXT("grass_medium_01_mid"), TEXT("grass_medium_01_large"), TEXT("grass_medium_01_tall"), TEXT("grass_medium_02") }),
			1100.f, 5000, 8000, FVector2f(0.9f, 1.5f));
		ULandscapeGrassType* Herbs = MakeGrass(TEXT("LGT_Herbs"),
			Pick(Gr, { TEXT("fern_02"), TEXT("dandelion_01"), TEXT("shrub_02"), TEXT("shrub_03"), TEXT("grass_medium_01_tiny") }),
			50.f, 6000, 9000, FVector2f(0.6f, 1.2f));
		UMaterialExpressionLandscapeGrassOutput* GO = Expr<UMaterialExpressionLandscapeGrassOutput>(M, 200, 400);
		GO->GrassTypes.Reset();
		int32 Out = 4; // custom node outputs: 0 colour, 1 NormalWS, 2 Rough, 3 AmbOcc, 4 GrassW, 5 HerbW
		for (ULandscapeGrassType* G : { Meadow, Herbs })
		{
			const int32 Idx = Out++;
			if (!G) continue;
			FGrassInput In(G->GetFName());
			In.GrassType = G;
			In.Input.Connect(Idx, Cu);
			GO->GrassTypes.Add(In);
		}
	}
	Finish(M);
	Log(TEXT("M_Archipelago built"));
	return M;
}

void FDragonArchipelagoBuilder::CreateMasterMaterials()
{
	// --- M_Surface: opaque PBR for rocks, bark, deadwood -----------------------
	if (UMaterial* M = NewMaterial(TEXT("M_Surface")))
	{
		M->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
		M->SetUsageByFlag(MATUSAGE_Nanite, true);
		M->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
		M->TwoSided = false;
		UMaterialExpressionTextureSampleParameter2D* D = TexParam(M, TEXT("BaseColor"), nullptr, SAMPLERTYPE_Color, -600, -200);
		UMaterialExpressionTextureSampleParameter2D* N = TexParam(M, TEXT("Normal"), nullptr, SAMPLERTYPE_Normal, -600, 50);
		UMaterialExpressionTextureSampleParameter2D* A = TexParam(M, TEXT("ARM"), nullptr, SAMPLERTYPE_Masks, -600, 300);
		UMaterialExpressionVectorParameter* Tint = Expr<UMaterialExpressionVectorParameter>(M, -600, -400);
		Tint->ParameterName = TEXT("Tint"); Tint->DefaultValue = FLinearColor::White;
		UMaterialExpressionMultiply* Mul = Expr<UMaterialExpressionMultiply>(M, -300, -250);
		UMaterialEditingLibrary::ConnectMaterialExpressions(D, TEXT("RGB"), Mul, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Mul, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(N, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("G"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("B"), MP_Metallic);
		Finish(M);
	}

	// --- M_Foliage: masked, two-sided foliage, wind ---------------------------
	if (UMaterial* M = NewMaterial(TEXT("M_Foliage")))
	{
		M->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
		M->SetUsageByFlag(MATUSAGE_Nanite, true);
		M->TwoSided = true;
		M->BlendMode = BLEND_Masked;
		M->SetShadingModel(MSM_TwoSidedFoliage);
		M->DitherOpacityMask = true;
		UMaterialExpressionTextureSampleParameter2D* D = TexParam(M, TEXT("BaseColor"), nullptr, SAMPLERTYPE_Color, -700, -300);
		UMaterialExpressionTextureSampleParameter2D* N = TexParam(M, TEXT("Normal"), nullptr, SAMPLERTYPE_Normal, -700, -50);
		UMaterialExpressionTextureSampleParameter2D* A = TexParam(M, TEXT("ARM"), nullptr, SAMPLERTYPE_Masks, -700, 200);
		UMaterialExpressionTextureSampleParameter2D* O = TexParam(M, TEXT("Opacity"), nullptr, SAMPLERTYPE_LinearGrayscale, -700, 450);
		UMaterialExpressionVectorParameter* Tint = Expr<UMaterialExpressionVectorParameter>(M, -700, -500);
		Tint->ParameterName = TEXT("Tint"); Tint->DefaultValue = FLinearColor(0.9f, 0.95f, 0.85f);
		UMaterialExpressionMultiply* Mul = Expr<UMaterialExpressionMultiply>(M, -350, -350);
		UMaterialEditingLibrary::ConnectMaterialExpressions(D, TEXT("RGB"), Mul, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Mul, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(N, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("G"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(O, TEXT("R"), MP_OpacityMask);
		// Light through the leaves.
		UMaterialExpressionMultiply* SSS = Expr<UMaterialExpressionMultiply>(M, -350, -150);
		UMaterialExpressionConstant* K = Expr<UMaterialExpressionConstant>(M, -500, -150);
		K->R = 0.6f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Mul, TEXT(""), SSS, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(K, TEXT(""), SSS, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(SSS, TEXT(""), MP_SubsurfaceColor);

		// Wind: sway grows with height above the instance pivot, gusts travel across the island.
		UMaterialExpressionCustom* Wind = Expr<UMaterialExpressionCustom>(M, -350, 650);
		Wind->Description = TEXT("Wind");
		Wind->OutputType = CMOT_Float3;
		Wind->Inputs.Reset();
		for (const TCHAR* In : { TEXT("P"), TEXT("Pivot"), TEXT("T"), TEXT("Strength") }) { FCustomInput I; I.InputName = In; Wind->Inputs.Add(I); }
		Wind->Code = TEXT(
			"float hgt = max(0.0, P.z - Pivot.z) * 0.01;\n"
			"float bend = hgt * hgt * 0.004 + hgt * 0.01;\n"
			"float gust = sin(T * 0.7 + dot(P.xy, float2(0.0011, 0.0007))) * 0.5 + 0.5;\n"
			"float flut = sin(T * 3.1 + dot(P.xyz, float3(0.031, 0.027, 0.04)));\n"
			"float2 dir = normalize(float2(0.55, -0.83));\n"
			"float a = Strength * (bend * (0.4 + gust) + flut * 0.02 * hgt);\n"
			"return float3(dir * a * 100.0, 0.0);\n");
		Wind->PostEditChange();
		UMaterialExpressionWorldPosition* WP = Expr<UMaterialExpressionWorldPosition>(M, -700, 650);
		WP->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
		UMaterialExpressionObjectPositionWS* Pivot = Expr<UMaterialExpressionObjectPositionWS>(M, -700, 730);
		UMaterialExpressionTime* Time = Expr<UMaterialExpressionTime>(M, -700, 810);
		UMaterialExpressionScalarParameter* Str = Expr<UMaterialExpressionScalarParameter>(M, -700, 890);
		Str->ParameterName = TEXT("WindStrength"); Str->DefaultValue = 1.f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(WP, TEXT(""), Wind, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Pivot, TEXT(""), Wind, TEXT("Pivot"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Time, TEXT(""), Wind, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Str, TEXT(""), Wind, TEXT("Strength"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Wind, TEXT(""), MP_WorldPositionOffset);
		Finish(M);
	}

	// --- M_Membrane: wing skin — two-sided, light comes through it -----------
	if (UMaterial* M = NewMaterial(TEXT("M_Membrane")))
	{
		M->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
		M->TwoSided = true;
		M->SetShadingModel(MSM_TwoSidedFoliage);
		UMaterialExpressionTextureSampleParameter2D* D = TexParam(M, TEXT("BaseColor"), nullptr, SAMPLERTYPE_Color, -600, -200);
		UMaterialExpressionTextureSampleParameter2D* N = TexParam(M, TEXT("Normal"), nullptr, SAMPLERTYPE_Normal, -600, 50);
		UMaterialExpressionTextureSampleParameter2D* A = TexParam(M, TEXT("ARM"), nullptr, SAMPLERTYPE_Masks, -600, 300);
		// Thickness mask: white = thin panel that glows against the sun, dark = bone, vessels, thick skin.
		UMaterialExpressionTextureSampleParameter2D* T = TexParam(M, TEXT("Thinness"), nullptr, SAMPLERTYPE_LinearGrayscale, -600, 550);
		UMaterialExpressionVectorParameter* Through = Expr<UMaterialExpressionVectorParameter>(M, -600, -400);
		Through->ParameterName = TEXT("Translucency"); Through->DefaultValue = FLinearColor(0.54f, 0.23f, 0.13f); // ~#8a3a22
		UMaterialExpressionMultiply* Glow = Expr<UMaterialExpressionMultiply>(M, -300, -400);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Through, TEXT(""), Glow, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(T, TEXT("R"), Glow, TEXT("B"));
		// Light through the wing takes the skin's own colour: a dark hide passes a dim, deep red,
		// never a bright orange panel. Gain lifts the (linear, very dark) albedo back into range.
		UMaterialExpressionScalarParameter* Gain = Expr<UMaterialExpressionScalarParameter>(M, -600, -500);
		Gain->ParameterName = TEXT("GlowGain"); Gain->DefaultValue = 3.5f;
		UMaterialExpressionMultiply* Alb = Expr<UMaterialExpressionMultiply>(M, -450, -300);
		UMaterialEditingLibrary::ConnectMaterialExpressions(D, TEXT("RGB"), Alb, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Gain, TEXT(""), Alb, TEXT("B"));
		UMaterialExpressionMultiply* Glow2 = Expr<UMaterialExpressionMultiply>(M, -150, -400);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Glow, TEXT(""), Glow2, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Alb, TEXT(""), Glow2, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(D, TEXT("RGB"), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(N, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("G"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Glow2, TEXT(""), MP_SubsurfaceColor);
		UMaterialExpressionMultiply* Op = Expr<UMaterialExpressionMultiply>(M, -300, 550);
		UMaterialEditingLibrary::ConnectMaterialExpressions(T, TEXT("R"), Op, TEXT("A"));
		Op->ConstB = 0.6f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Op, TEXT(""), MP_Opacity); // how much light scatters through
		Finish(M);
	}

	// --- M_DragonSkin: scaled hide — subsurface, glossy caps, a faint grazing sheen
	if (UMaterial* M = NewMaterial(TEXT("M_DragonSkin")))
	{
		M->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
		M->SetShadingModel(MSM_Subsurface);
		M->TwoSided = true;
		UMaterialExpressionTextureSampleParameter2D* D = TexParam(M, TEXT("BaseColor"), nullptr, SAMPLERTYPE_Color, -700, -250);
		UMaterialExpressionTextureSampleParameter2D* N = TexParam(M, TEXT("Normal"), nullptr, SAMPLERTYPE_Normal, -700, 0);
		UMaterialExpressionTextureSampleParameter2D* A = TexParam(M, TEXT("ARM"), nullptr, SAMPLERTYPE_Masks, -700, 250);
		// Sheen: a faint cool rim at grazing angles, standing in for the iridescence of real scales.
		UMaterialExpressionCustom* Sheen = Expr<UMaterialExpressionCustom>(M, -350, -250);
		Sheen->Description = TEXT("Scale sheen");
		Sheen->OutputType = CMOT_Float3;
		Sheen->Inputs.Reset();
		for (const TCHAR* In : { TEXT("Albedo"), TEXT("Amount") }) { FCustomInput I; I.InputName = In; Sheen->Inputs.Add(I); }
		Sheen->Code = TEXT(
			"float3 V = normalize(Parameters.CameraVector);\n"
			"float f = pow(1.0 - saturate(dot(Parameters.WorldNormal, V)), 4.0) * Amount;\n"
			"return lerp(Albedo, Albedo * 0.7 + float3(0.10, 0.14, 0.20), f);\n");
		Sheen->PostEditChange();
		UMaterialExpressionScalarParameter* Amt = Expr<UMaterialExpressionScalarParameter>(M, -700, -400);
		Amt->ParameterName = TEXT("Sheen"); Amt->DefaultValue = 0.35f;
		// Hide tint: the web textures were painted for a darker renderer; under a real sun
		// the black hide needs pulling down to read black.
		UMaterialExpressionVectorParameter* Tint = Expr<UMaterialExpressionVectorParameter>(M, -700, -550);
		Tint->ParameterName = TEXT("Tint"); Tint->DefaultValue = FLinearColor(1.f, 1.f, 1.f);
		UMaterialExpressionMultiply* Tinted = Expr<UMaterialExpressionMultiply>(M, -500, -300);
		UMaterialEditingLibrary::ConnectMaterialExpressions(D, TEXT("RGB"), Tinted, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Tinted, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tinted, TEXT(""), Sheen, TEXT("Albedo"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Amt, TEXT(""), Sheen, TEXT("Amount"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Sheen, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(N, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("G"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("B"), MP_Metallic);
		// Matte hide: a low specular level, or every grazing view toward the sun turns the
		// near-black skin to a tan sheen.
		UMaterialExpressionScalarParameter* Spec = Expr<UMaterialExpressionScalarParameter>(M, -350, 200);
		Spec->ParameterName = TEXT("Specular"); Spec->DefaultValue = 0.06f; // <0.4 F0 also damps the grazing Fresnel term (UE scales it by 50*F0)
		UMaterialEditingLibrary::ConnectMaterialProperty(Spec, TEXT(""), MP_Specular);
		// Warm blood-red scatter, kept faint: this is thick hide, not a lizard's ear.
		UMaterialExpressionVectorParameter* Sss = Expr<UMaterialExpressionVectorParameter>(M, -350, 300);
		Sss->ParameterName = TEXT("SubsurfaceTint"); Sss->DefaultValue = FLinearColor(0.7f, 0.28f, 0.24f);
		UMaterialExpressionScalarParameter* SssAmt = Expr<UMaterialExpressionScalarParameter>(M, -350, 400);
		SssAmt->ParameterName = TEXT("SubsurfaceAmount"); SssAmt->DefaultValue = 0.1f;
		// Scatter scaled by the albedo, so near-black hide stays black instead of glowing orange.
		UMaterialExpressionMultiply* SssC = Expr<UMaterialExpressionMultiply>(M, -150, 300);
		UMaterialEditingLibrary::ConnectMaterialExpressions(D, TEXT("RGB"), SssC, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sss, TEXT(""), SssC, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(SssC, TEXT(""), MP_SubsurfaceColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(SssAmt, TEXT(""), MP_Opacity);
		Finish(M);
	}

	// --- M_Sea / M_Lake: Single Layer Water ------------------------------------
	// Five Gerstner swells (metres), running with the prevailing wind; shared by the
	// displacement and the normal so the lighting matches the shape exactly.
	static const TCHAR* GWaves = TEXT(
		"const float4 Wv[8] = {\n"
		"  float4( 0.883,  0.469, 97.0, 0.52),\n"
		"  float4( 0.978,  0.208, 61.0, 0.31),\n"
		"  float4( 0.574,  0.819, 43.0, 0.21),\n"
		"  float4( 0.995, -0.100, 29.0, 0.13),\n"
		"  float4( 0.342,  0.940, 19.0, 0.08),\n"
		"  float4( 0.906, -0.423, 12.5, 0.05),\n"
		"  float4( 0.669,  0.743,  8.3, 0.03),\n"
		"  float4(-0.139,  0.990,  5.1, 0.018) };\n");
	auto MakeWater = [&](const TCHAR* Name, bool bSea)
	{
		UMaterial* M = NewMaterial(Name);
		if (!M) return;
		M->SetShadingModel(MSM_SingleLayerWater);
		M->BlendMode = BLEND_Opaque;
		auto Scalar = [&](const TCHAR* N, float V, int32 Y) { UMaterialExpressionScalarParameter* E = Expr<UMaterialExpressionScalarParameter>(M, -1100, Y); E->ParameterName = N; E->DefaultValue = V; return E; };
		auto Vec = [&](const TCHAR* N, FLinearColor V, int32 Y) { UMaterialExpressionVectorParameter* E = Expr<UMaterialExpressionVectorParameter>(M, -1100, Y); E->ParameterName = N; E->DefaultValue = V; return E; };
		auto TexO = [&](const TCHAR* N, const TCHAR* Path, EMaterialSamplerType Ty, int32 Y)
		{
			UMaterialExpressionTextureObjectParameter* E = Expr<UMaterialExpressionTextureObjectParameter>(M, -1100, Y);
			E->ParameterName = N; E->Texture = LoadObject<UTexture2D>(nullptr, Path); E->SamplerType = Ty; return E;
		};
		UMaterialExpressionWorldPosition* WPx = Expr<UMaterialExpressionWorldPosition>(M, -1100, -600);
		WPx->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
		UMaterialExpressionWorldPosition* WP = Expr<UMaterialExpressionWorldPosition>(M, -1100, -520);
		UMaterialExpressionObjectPositionWS* OP = Expr<UMaterialExpressionObjectPositionWS>(M, -1100, -440);
		UMaterialExpressionCameraPositionWS* CP = Expr<UMaterialExpressionCameraPositionWS>(M, -1100, -360);
		UMaterialExpressionTime* Tm = Expr<UMaterialExpressionTime>(M, -1100, -280);
		auto* Amp = Scalar(TEXT("WaveAmplitude"), bSea ? 1.f : 0.f, -200);
		auto* Half = Scalar(TEXT("GridHalfM"), 1500.f, -120);
		auto* DScale = Scalar(TEXT("DetailScale"), bSea ? 1.f : 0.45f, -40);
		auto* Foam = Scalar(TEXT("FoamAmount"), bSea ? 1.f : 0.f, 40);
		auto* WS = Scalar(TEXT("WorldSizeM"), 45000.f, 120);
		auto* WN = TexO(TEXT("WaveNormal"), TEXT("/Water/Textures/Normals/T_Water_TilingNormal_Waves_02.T_Water_TilingNormal_Waves_02"), SAMPLERTYPE_Normal, 200);
		auto* FT = TexO(TEXT("FoamTex"), TEXT("/Water/Textures/Foam/T_WaterFlow_03_Foam_Tiled.T_WaterFlow_03_Foam_Tiled"), SAMPLERTYPE_Color, 290);
		auto* HT = TexO(TEXT("Height"), TEXT("/Game/World/Data/T_Height.T_Height"), SAMPLERTYPE_LinearGrayscale, 380);
		if (!HT->Texture) HT->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/World/Textures/Defaults/T_Default_Gray.T_Default_Gray"));
		auto* ShallowS = Vec(TEXT("ScatterShallow"), bSea ? FLinearColor(0.010f, 0.040f, 0.036f) : FLinearColor(0.012f, 0.014f, 0.008f), 470);
		auto* DeepS = Vec(TEXT("ScatterDeep"), bSea ? FLinearColor(0.003f, 0.010f, 0.014f) : FLinearColor(0.006f, 0.008f, 0.006f), 550);
		auto* Ab = Vec(TEXT("Absorption"), bSea ? FLinearColor(0.36f, 0.070f, 0.042f) : FLinearColor(0.50f, 0.22f, 0.20f), 630);

		auto In = [](UMaterialExpressionCustom* C, std::initializer_list<const TCHAR*> Names) { C->Inputs.Reset(); for (const TCHAR* N : Names) { FCustomInput I; I.InputName = N; C->Inputs.Add(I); } };

		// Displacement.
		UMaterialExpressionCustom* Disp = Expr<UMaterialExpressionCustom>(M, -600, -500);
		Disp->Description = TEXT("Swell");
		Disp->OutputType = CMOT_Float3;
		In(Disp, { TEXT("P"), TEXT("O"), TEXT("T"), TEXT("Amp"), TEXT("Half") });
		Disp->Code = FString(GWaves) + TEXT(
			"float3 p = P * 0.01; float2 o = O.xy * 0.01;\n"
			"float r = max(abs(p.x - o.x), abs(p.y - o.y));\n"
			"float fade = saturate((Half * 0.94 - r) / (Half * 0.5)) * Amp;\n"
			"if (fade <= 0.0) return 0;\n"
			"float3 off = 0;\n"
			"for (int i = 0; i < 8; i++) {\n"
			"  float2 d = Wv[i].xy; float L = Wv[i].z; float A = Wv[i].w * fade;\n"
			"  float k = 6.2832 / L; float c = sqrt(9.81 / k); float f = k * (dot(d, p.xy) - c * T);\n"
			"  float Q = saturate(0.55 / (k * Wv[i].w * 8.0));\n"
			"  off.xy += Q * A * d * cos(f); off.z += A * sin(f); }\n"
			"return off * 100.0;\n");
		Disp->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(WPx, TEXT(""), Disp, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OP, TEXT(""), Disp, TEXT("O"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tm, TEXT(""), Disp, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Amp, TEXT(""), Disp, TEXT("Amp"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Half, TEXT(""), Disp, TEXT("Half"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Disp, TEXT(""), MP_WorldPositionOffset);

		// Surface: normal (swell + ripples), foam, scattering by depth.
		UMaterialExpressionCustom* Surf = Expr<UMaterialExpressionCustom>(M, -600, 100);
		Surf->Description = TEXT("Sea surface");
		Surf->OutputType = CMOT_Float3;
		In(Surf, { TEXT("P"), TEXT("O"), TEXT("Cam"), TEXT("T"), TEXT("Amp"), TEXT("Half"), TEXT("DS"), TEXT("FoamAmt"), TEXT("WS"),
			TEXT("Tex"), TEXT("FoamT"), TEXT("H"), TEXT("SS"), TEXT("SD") });
		Surf->AdditionalOutputs.Reset();
		for (const TCHAR* N : { TEXT("FoamOut"), TEXT("Rough") }) { FCustomOutput O; O.OutputName = N; O.OutputType = CMOT_Float1; Surf->AdditionalOutputs.Add(O); }
		{ FCustomOutput O; O.OutputName = TEXT("Scatter"); O.OutputType = CMOT_Float3; Surf->AdditionalOutputs.Add(O); }
		Surf->Code = FString(GWaves) + TEXT(
			"float3 p = P * 0.01; float2 o = O.xy * 0.01;\n"
			"float dist = length(P - Cam) * 0.01;\n"
			"float r = max(abs(p.x - o.x), abs(p.y - o.y));\n"
			"float fade = saturate((Half * 0.94 - r) / (Half * 0.5)) * Amp;\n"
			// swell normal and height
			"float3 n = float3(0, 0, 1); float hz = 0;\n"
			"for (int i = 0; i < 8; i++) {\n"
			"  float2 d = Wv[i].xy; float L = Wv[i].z; float A = Wv[i].w * fade;\n"
			"  float k = 6.2832 / L; float c = sqrt(9.81 / k); float f = k * (dot(d, p.xy) - c * T);\n"
			"  float Q = saturate(0.55 / (k * Wv[i].w * 8.0));\n"
			"  n.xy -= d * k * A * cos(f); n.z -= Q * k * A * sin(f); hz += A * sin(f); }\n"
			// beyond the grid the swell still reads in the lighting, at half strength
			"float far = (1.0 - fade) * Amp * 0.5;\n"
			"for (int j = 0; j < 3; j++) {\n"
			"  float2 d = Wv[j].xy; float L = Wv[j].z; float A = Wv[j].w * far;\n"
			"  float k = 6.2832 / L; float c = sqrt(9.81 / k); float f = k * (dot(d, p.xy) - c * T);\n"
			"  n.xy -= d * k * A * cos(f); }\n"
			// ripples
			"float near = 1.0 / (1.0 + dist / 700.0);\n"
			"float2 q = 0;\n"
			"q += (Texture2DSample(Tex, TexSampler, p.xy / (31.0 * DS) + T * float2(0.010, 0.006)).xy * 2 - 1) * 0.55;\n"
			"q += (Texture2DSample(Tex, TexSampler, p.xy / (11.0 * DS) + T * float2(-0.019, 0.023)).xy * 2 - 1) * 0.45 * near;\n"
			"q += (Texture2DSample(Tex, TexSampler, p.xy / (3.7 * DS) + T * float2(0.047, -0.033)).xy * 2 - 1) * 0.35 * near * near;\n"
			"float3 N = normalize(float3(n.xy / max(n.z, 0.2) + q * 0.32, 1.0));\n"
			// depth from the baked terrain (sea level 0; v -> metres)
			"float2 muv = p.xy / WS + 0.5;\n"
			"float hv = Texture2DSample(H, HSampler, muv).r;\n"
			"float ground = (hv * 65535.0 - 32768.0) / 32.0;\n"
			"float depth = max(0.0, hz - ground);\n"
			"bool inWorld = all(muv > 0.0) && all(muv < 1.0);\n"
			"if (!inWorld) depth = 400.0;\n"
			// foam: surf along the shore, breaking on the swell's crests
			"float ft = Texture2DSample(FoamT, FoamTSampler, p.xy / 7.0 + T * float2(0.013, 0.009)).r;\n"
			"float ft2 = Texture2DSample(FoamT, FoamTSampler, p.xy / 23.0 - T * float2(0.006, 0.011)).r;\n"
			"float shore = saturate(1.0 - depth / 4.0);\n"
			"float surge = 0.6 + 0.4 * sin(T * 0.9 + dot(p.xy, float2(0.05, 0.03)));\n"
			"float surf = saturate(shore * shore * 1.6 * surge) * smoothstep(0.30, 0.65, ft * 0.7 + ft2 * 0.5 + shore * 0.35);\n"
			// whitecaps come in scattered patches where gusts push the crests over, not on every
			// crest of the long swell (which lines them up into rows); thinned with distance, where
			// they'd shrink to dotted lines.
			"float patch = smoothstep(0.50, 0.80, Texture2DSample(FoamT, FoamTSampler, p.xy / 310.0 + T * float2(0.0021, 0.0013)).r * 0.6\n"
			"            + Texture2DSample(FoamT, FoamTSampler, p.xy / 97.0 - T * float2(0.0030, 0.0042)).r * 0.5);\n"
			"float crest = saturate((hz - 0.60) * 2.0) * smoothstep(0.58, 0.88, ft) * patch * 0.75 * (1.0 - smoothstep(250.0, 900.0, dist));\n"
			"FoamOut = saturate((surf + crest) * FoamAmt);\n"
			"Rough = lerp(0.035, 0.6, FoamOut);\n"
			"Scatter = lerp(SS, SD, saturate(depth / 25.0));\n"
			"return N;\n");
		Surf->RebuildOutputs();
		Surf->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(WP, TEXT(""), Surf, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OP, TEXT(""), Surf, TEXT("O"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(CP, TEXT(""), Surf, TEXT("Cam"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tm, TEXT(""), Surf, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Amp, TEXT(""), Surf, TEXT("Amp"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Half, TEXT(""), Surf, TEXT("Half"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(DScale, TEXT(""), Surf, TEXT("DS"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Foam, TEXT(""), Surf, TEXT("FoamAmt"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WS, TEXT(""), Surf, TEXT("WS"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WN, TEXT(""), Surf, TEXT("Tex"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(FT, TEXT(""), Surf, TEXT("FoamT"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(HT, TEXT(""), Surf, TEXT("H"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(ShallowS, TEXT(""), Surf, TEXT("SS"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(DeepS, TEXT(""), Surf, TEXT("SD"));
		M->bTangentSpaceNormal = false; // the grid is flat and unrotated: we write a world normal
		UMaterialEditingLibrary::ConnectMaterialProperty(Surf, TEXT(""), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(Surf, TEXT("Rough"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Surf, TEXT("FoamOut"), MP_Opacity);
		UMaterialExpressionConstant3Vector* FoamCol = Expr<UMaterialExpressionConstant3Vector>(M, -300, -150);
		FoamCol->Constant = FLinearColor(0.80f, 0.83f, 0.85f);
		UMaterialEditingLibrary::ConnectMaterialProperty(FoamCol, TEXT(""), MP_BaseColor);

		UMaterialExpressionSingleLayerWaterMaterialOutput* Out = Expr<UMaterialExpressionSingleLayerWaterMaterialOutput>(M, -100, 500);
		UMaterialExpressionScalarParameter* G = Scalar(TEXT("PhaseG"), 0.25f, 720);
		UMaterialExpressionConstant* Behind = Expr<UMaterialExpressionConstant>(M, -400, 800);
		Behind->R = 1.f;
		Out->AbsorptionCoefficients.Connect(0, Ab);
		Out->ScatteringCoefficients.Connect(3, Surf); // output index 3 = "Scatter" (0 is the return value)
		Out->PhaseG.Connect(0, G);
		Out->ColorScaleBehindWater.Connect(0, Behind);
		Finish(M);
	};
	MakeWater(TEXT("M_Sea"), true);
	MakeWater(TEXT("M_Lake"), false);

	// --- M_Rain: streaks on a camera-following curtain ------------------------
	if (UMaterial* M = NewMaterial(TEXT("M_Rain")))
	{
		M->BlendMode = BLEND_Translucent;
		M->SetShadingModel(MSM_Unlit);
		M->TwoSided = true;
		M->SetUsageByFlag(MATUSAGE_StaticLighting, false);
		UMaterialExpressionCustom* Rain = Expr<UMaterialExpressionCustom>(M, -350, 0);
		Rain->Description = TEXT("Rain streaks");
		Rain->OutputType = CMOT_Float1;
		Rain->Inputs.Reset();
		for (const TCHAR* In : { TEXT("UV"), TEXT("T"), TEXT("Intensity") }) { FCustomInput I; I.InputName = In; Rain->Inputs.Add(I); }
		Rain->Code = TEXT(
			"float m = 0;\n"
			"for (int k = 0; k < 2; k++) {\n"
			"  float2 uv = UV * (k == 0 ? float2(90, 6) : float2(170, 11));\n"
			"  float col = floor(uv.x);\n"
			"  float h = frac(sin(col * 12.9898 + k * 7.1) * 43758.5453);\n"
			"  float y = frac(uv.y + T * (2.2 + h * 1.4) * (k == 0 ? 1.0 : 1.3) + h * 10.0);\n"
			"  float streak = smoothstep(0.0, 0.04, y) * (1.0 - smoothstep(0.04, 0.32, y));\n"
			"  float w = pow(saturate(1.0 - abs(frac(uv.x) - 0.5) * 2.0), 10.0);\n"
			"  m += streak * w * step(1.0 - Intensity, h) * (k == 0 ? 1.0 : 0.6);\n"
			"}\n"
			"return m;\n");
		Rain->PostEditChange();
		UMaterialExpressionTextureCoordinate* UV = Expr<UMaterialExpressionTextureCoordinate>(M, -600, -50);
		UMaterialExpressionTime* Time = Expr<UMaterialExpressionTime>(M, -600, 30);
		UMaterialExpressionScalarParameter* Int = Expr<UMaterialExpressionScalarParameter>(M, -600, 110);
		Int->ParameterName = TEXT("Intensity"); Int->DefaultValue = 0.6f;
		UMaterialExpressionScalarParameter* Night = Expr<UMaterialExpressionScalarParameter>(M, -600, 190);
		Night->ParameterName = TEXT("Night"); Night->DefaultValue = 0.f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(UV, TEXT(""), Rain, TEXT("UV"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Time, TEXT(""), Rain, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Int, TEXT(""), Rain, TEXT("Intensity"));
		// Opacity = streaks * 0.35; colour = a pale grey that dims at night.
		UMaterialExpressionMultiply* Op = Expr<UMaterialExpressionMultiply>(M, -150, 80);
		UMaterialExpressionConstant* K = Expr<UMaterialExpressionConstant>(M, -350, 160);
		K->R = 0.35f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Rain, TEXT(""), Op, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(K, TEXT(""), Op, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Op, TEXT(""), MP_Opacity);
		UMaterialExpressionCustom* Col = Expr<UMaterialExpressionCustom>(M, -150, -80);
		Col->OutputType = CMOT_Float3;
		Col->Inputs.Reset();
		{ FCustomInput I; I.InputName = TEXT("Night"); Col->Inputs.Add(I); }
		Col->Code = TEXT("return lerp(float3(0.75, 0.78, 0.82), float3(0.05, 0.06, 0.08), Night);");
		Col->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(Night, TEXT(""), Col, TEXT("Night"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Col, TEXT(""), MP_EmissiveColor);
		Finish(M);
	}

	// --- M_Plasma: the blast's core -------------------------------------------
	if (UMaterial* M = NewMaterial(TEXT("M_Plasma")))
	{
		M->SetShadingModel(MSM_Unlit);
		M->SetUsageByFlag(MATUSAGE_StaticLighting, false);
		UMaterialExpressionVectorParameter* C = Expr<UMaterialExpressionVectorParameter>(M, -500, 0);
		C->ParameterName = TEXT("Colour"); C->DefaultValue = FLinearColor(0.45f, 0.55f, 1.f);
		UMaterialExpressionScalarParameter* I = Expr<UMaterialExpressionScalarParameter>(M, -500, 150);
		I->ParameterName = TEXT("Intensity"); I->DefaultValue = 60.f;
		UMaterialExpressionMultiply* Mul = Expr<UMaterialExpressionMultiply>(M, -250, 50);
		UMaterialEditingLibrary::ConnectMaterialExpressions(C, TEXT(""), Mul, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(I, TEXT(""), Mul, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_EmissiveColor);
		Finish(M);
	}
	Log(TEXT("master materials built"));
}

// ===========================================================================
// Models (Poly Haven glTF, re-centred by fetch_assets.py)
// ===========================================================================
TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>> FDragonArchipelagoBuilder::ImportModels()
{
	TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>> ByGroup;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	UMaterial* Surface = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_Surface.M_Surface")));
	UMaterial* Foliage = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_Foliage.M_Foliage")));

	const FString Src = DownloadsDir() / TEXT("Models");
	TArray<FString> Groups;
	IFileManager::Get().FindFiles(Groups, *(Src / TEXT("*")), false, true);
	for (const FString& Group : Groups)
	{
		TArray<FString> Ids;
		IFileManager::Get().FindFiles(Ids, *(Src / Group / TEXT("*")), false, true);
		for (const FString& Id : Ids)
		{
			const FString Dir = Src / Group / Id;
			TArray<FString> Gltfs;
			IFileManager::Get().FindFiles(Gltfs, *(Dir / TEXT("*_ue.gltf")), true, false);
			if (Gltfs.Num() == 0) continue;
			const FString Dest = ModelRoot / Group / Id;

			TArray<FAssetData> Existing;
			Registry.GetAssetsByPath(FName(*Dest), Existing, true);
			if (Existing.Num() == 0)
			{
				UAutomatedAssetImportData* Data = NewObject<UAutomatedAssetImportData>();
				Data->Filenames.Add(Dir / Gltfs[0]);
				Data->DestinationPath = Dest;
				Data->bReplaceExisting = true;
				AssetTools.ImportAssetsAutomated(Data);
				Registry.ScanPathsSynchronous({ Dest }, true);
				Registry.GetAssetsByPath(FName(*Dest), Existing, true);
			}

			// Our own materials, made from Poly Haven's naming: <slot>_diff/_nor_gl/_arm/_alpha.
			auto TexFor = [&](const FString& Slot, const TCHAR* Map, bool bNormal, bool bLinear) -> UTexture2D*
			{
				TArray<FString> Files;
				IFileManager::Get().FindFiles(Files, *(Dir / TEXT("textures") / FString::Printf(TEXT("%s_%s_*"), *Slot, Map)), true, false);
				if (Files.Num() == 0) return nullptr;
				return ImportTexture(Dir / TEXT("textures") / Files[0], Dest / TEXT("Textures"), FString::Printf(TEXT("T_%s_%s"), *Slot, Map), bNormal, bLinear);
			};

			for (const FAssetData& A : Existing)
			{
				UStaticMesh* Mesh = Cast<UStaticMesh>(A.GetAsset());
				if (!Mesh) continue;
				bool bChanged = false;
				TArray<FStaticMaterial>& Mats = Mesh->GetStaticMaterials();
				for (FStaticMaterial& SM : Mats)
				{
					FString Slot = SM.ImportedMaterialSlotName.ToString();
					if (Slot.IsEmpty()) Slot = SM.MaterialSlotName.ToString();
					UTexture2D* D = TexFor(Slot, TEXT("diff"), false, false);
					if (!D) continue;
					UTexture2D* N = TexFor(Slot, TEXT("nor_gl"), true, false);
					UTexture2D* R = TexFor(Slot, TEXT("arm"), false, true);
					UTexture2D* O = TexFor(Slot, TEXT("alpha"), false, true);
					UMaterial* Parent = O ? Foliage : Surface;
					if (!Parent) continue;
					const FString MiName = FString::Printf(TEXT("MI_%s"), *Slot);
					UMaterialInstanceConstant* MI = CreateOrLoad<UMaterialInstanceConstant, UMaterialInstanceConstantFactoryNew>(Dest, MiName,
						[Parent](UMaterialInstanceConstantFactoryNew* F) { F->InitialParent = Parent; });
					if (!MI) continue;
					MI->SetParentEditorOnly(Parent);
					UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("BaseColor"), D);
					if (N) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("Normal"), N);
					if (R) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("ARM"), R);
					if (O) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("Opacity"), O);
					UMaterialEditingLibrary::UpdateMaterialInstance(MI);
					UEditorAssetLibrary::SaveLoadedAsset(MI, false);
					SM.MaterialInterface = MI;
					bChanged = true;
				}
				// Slots we found no textures for keep the importer's own material; it must still
				// be allowed on instanced and Nanite meshes, or it renders as the default grid.
				for (FStaticMaterial& SM : Mats)
				{
					UMaterial* Base = SM.MaterialInterface ? SM.MaterialInterface->GetMaterial() : nullptr;
					if (!Base || Base->GetOutermost() == GetTransientPackage() || Base->GetPathName().StartsWith(TEXT("/Engine/"))) continue;
					if (!Base->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes) || !Base->GetUsageByFlag(MATUSAGE_Nanite))
					{
						Base->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
						Base->SetUsageByFlag(MATUSAGE_Nanite, true);
						Base->PostEditChange();
						UEditorAssetLibrary::SaveLoadedAsset(Base, false);
					}
				}
				// Nanite everything; foliage keeps its area as it simplifies.
				{
					FMeshNaniteSettings NS = Mesh->GetNaniteSettings();
					NS.bEnabled = true;
					NS.ShapePreservation = (Group == TEXT("trees") || Group == TEXT("ground")) ? ENaniteShapePreservation::PreserveArea : ENaniteShapePreservation::None;
					Mesh->SetNaniteSettings(NS);
				}
				if (Group == TEXT("rocks") || Group == TEXT("cliffs")) Mesh->SetCustomizedCollision(false);
				Mesh->Build(false);
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
				UEditorAssetLibrary::SaveLoadedAsset(Mesh, false);
				ByGroup.FindOrAdd(FName(*Group)).Add(TSoftObjectPtr<UStaticMesh>(Mesh));
				(void)bChanged;
			}
			Log(FString::Printf(TEXT("model %s/%s: %d assets"), *Group, *Id, Existing.Num()));
		}
	}
	return ByGroup;
}

// ===========================================================================
// Dragon
// ===========================================================================
bool FDragonArchipelagoBuilder::ImportDragon()
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Dragon"));
	// The web game's own model (Data/Dragon/SK_Dragon.glb) wins; the scripted FBX is the fallback.
	const FString Glb = Dir / TEXT("SK_Dragon.glb");
	const FString Fbx = Dir / TEXT("SK_Dragon.fbx");
	const bool bGlb = FPaths::FileExists(Glb);
	const FString Source = bGlb ? Glb : Fbx;
	if (!FPaths::FileExists(Source)) { Log(TEXT("no Data/Dragon/SK_Dragon.glb or .fbx — the stand-in dragon will be used")); return false; }
	const FString Dest = TEXT("/Game/Dragon");
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Dragon/SK_Dragon.SK_Dragon"));
	if (Mesh && !Mesh->GetSkeleton())
	{
		Log(TEXT("existing SK_Dragon has no skeleton — reimporting"));
		Mesh = nullptr;
	}
	if (!Mesh)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UAutomatedAssetImportData* Data = NewObject<UAutomatedAssetImportData>();
		Data->Filenames.Add(Source);
		Data->DestinationPath = Dest;
		Data->bReplaceExisting = true;
		const TArray<UObject*> Imported = AssetTools.ImportAssetsAutomated(Data);
		// Interchange names assets after the file's own nodes, and creates the skeleton and
		// physics asset as separate packages: save every one, and give the mesh our name.
		for (UObject* O : Imported)
		{
			if (!O) continue;
			Log(FString::Printf(TEXT("  imported %s (%s)"), *O->GetPathName(), *O->GetClass()->GetName()));
			if (USkeletalMesh* SKM = Cast<USkeletalMesh>(O)) Mesh = SKM;
		}
		if (Mesh && Mesh->GetName() != TEXT("SK_Dragon"))
		{
			const FString From = Mesh->GetPathName();
			if (UEditorAssetLibrary::RenameAsset(From, TEXT("/Game/Dragon/SK_Dragon")))
				Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Dragon/SK_Dragon.SK_Dragon"));
		}
		UEditorAssetLibrary::SaveDirectory(Dest, false, true);
	}
	if (!Mesh) { Log(TEXT("! dragon import produced no skeletal mesh")); return false; }
	if (!Mesh->GetSkeleton()) { Log(TEXT("! SK_Dragon has no skeleton after import")); return false; }
	Log(FString::Printf(TEXT("dragon: %d bones, skeleton %s"), Mesh->GetRefSkeleton().GetNum(), *Mesh->GetSkeleton()->GetPathName()));

	if (bGlb)
	{
		// The web model's own colour textures, rendered through our skin and membrane
		// materials (scale sheen, a little subsurface, light through the wing).
		UMaterial* Skin = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_DragonSkin.M_DragonSkin")));
		for (FSkeletalMaterial& SM : Mesh->GetMaterials())
		{
			const FString Slot = SM.MaterialSlotName.ToString();
			UTexture* BaseTex = nullptr;
			if (UMaterialInstance* Src = Cast<UMaterialInstance>(SM.MaterialInterface))
			{
				for (const FTextureParameterValue& TP : Src->TextureParameterValues)
					if (TP.ParameterValue && TP.ParameterInfo.Name.ToString().Contains(TEXT("BaseColor"))) { BaseTex = TP.ParameterValue; break; }
				if (!BaseTex)
					for (const FTextureParameterValue& TP : Src->TextureParameterValues)
						if (TP.ParameterValue) { BaseTex = TP.ParameterValue; break; }
			}
			// Colour maps must be sRGB: Interchange imports a second reference to the same image
			// (renamed Image_01 etc.) as linear, which washes a black hide out to tan.
			if (UTexture2D* T2 = Cast<UTexture2D>(BaseTex))
				if (!T2->SRGB || T2->CompressionSettings != TC_Default)
				{
					T2->SRGB = true; T2->CompressionSettings = TC_Default;
					T2->PostEditChange();
					UEditorAssetLibrary::SaveLoadedAsset(T2, false);
					Log(FString::Printf(TEXT("  %s -> sRGB colour"), *T2->GetName()));
				}
			const bool bWing = Slot.Contains(TEXT("membrane"), ESearchCase::IgnoreCase) || Slot.Contains(TEXT("wing"), ESearchCase::IgnoreCase);
			// The wing uses the skin material too, with more light let through: the foliage
			// shading model washed the dark membrane out to tan wherever the sun hit it.
			UMaterial* Parent = Skin;
			if (!Parent) continue;
			// Fresh every time: values set on a reloaded instance were not reaching the render.
			{
				const FString Existing = Dest / FString::Printf(TEXT("MI_Dragon_%s"), *Slot);
				if (UEditorAssetLibrary::DoesAssetExist(Existing)) UEditorAssetLibrary::DeleteAsset(Existing);
			}
			UMaterialInstanceConstant* MI = CreateOrLoad<UMaterialInstanceConstant, UMaterialInstanceConstantFactoryNew>(Dest, FString::Printf(TEXT("MI_Dragon_%s"), *Slot),
				[Parent](UMaterialInstanceConstantFactoryNew* F) { F->InitialParent = Parent; });
			if (!MI) continue;
			MI->SetParentEditorOnly(Parent);
			if (BaseTex) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("BaseColor"), BaseTex);
			if (bWing) UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("SubsurfaceAmount"), 0.15f);
			if (bWing || Slot == TEXT("acmat_0"))
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI, TEXT("Tint"), FLinearColor(0.45f, 0.45f, 0.5f));
			UMaterialEditingLibrary::UpdateMaterialInstance(MI);
			UEditorAssetLibrary::SaveLoadedAsset(MI, false);
			SM.MaterialInterface = MI;
			Log(FString::Printf(TEXT("  slot %s -> %s (%s)"), *Slot, *MI->GetName(), BaseTex ? *BaseTex->GetName() : TEXT("no texture")));
		}
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
		UEditorAssetLibrary::SaveDirectory(Dest, false, true);
		Log(TEXT("dragon imported (web model)"));
		return true;
	}

	UMaterial* Surface = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_DragonSkin.M_DragonSkin")));
	if (!Surface) Surface = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_Surface.M_Surface")));
	UMaterial* Membrane = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_Membrane.M_Membrane")));
	for (FSkeletalMaterial& SM : Mesh->GetMaterials())
	{
		const FString Slot = SM.MaterialSlotName.ToString();
		const bool bWing = Slot.Contains(TEXT("Wing"));
		const FString Part = bWing ? TEXT("Wing") : TEXT("Body");
		auto Tex = [&](const TCHAR* Map, bool bN, bool bL)
		{
			return ImportTexture(Dir / TEXT("Textures") / FString::Printf(TEXT("T_Dragon_%s_%s.png"), *Part, Map), Dest / TEXT("Textures"), FString::Printf(TEXT("T_Dragon_%s_%s"), *Part, Map), bN, bL);
		};
		UTexture2D* D = Tex(TEXT("D"), false, false);
		UTexture2D* N = Tex(TEXT("N"), true, false);
		UTexture2D* A = Tex(TEXT("ARM"), false, true);
		UMaterial* Parent = bWing && Membrane ? Membrane : Surface;
		if (!Parent || !D) continue;
		UMaterialInstanceConstant* MI = CreateOrLoad<UMaterialInstanceConstant, UMaterialInstanceConstantFactoryNew>(Dest, FString::Printf(TEXT("MI_Dragon_%s"), *Part),
			[Parent](UMaterialInstanceConstantFactoryNew* F) { F->InitialParent = Parent; });
		if (!MI) continue;
		MI->SetParentEditorOnly(Parent);
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("BaseColor"), D);
		if (N) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("Normal"), N);
		if (A) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("ARM"), A);
		if (bWing)
			if (UTexture2D* T = Tex(TEXT("Translucency"), false, true))
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, TEXT("Thinness"), T);
		UMaterialEditingLibrary::UpdateMaterialInstance(MI);
		UEditorAssetLibrary::SaveLoadedAsset(MI, false);
		SM.MaterialInterface = MI;
	}
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(Mesh, false);
	Log(TEXT("dragon imported"));
	return true;
}

// ===========================================================================
// Level
// ===========================================================================
bool FDragonArchipelagoBuilder::BuildLevel(const FDragonBuildOptions& Options, const TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>>& Meshes)
{
	TSharedPtr<FJsonObject> J = LoadWorldJson();
	if (!J.IsValid()) { Log(TEXT("! no World.json — run Tools/WorldBake first")); return false; }

	UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
	if (!World) { Log(TEXT("! could not make a map")); return false; }

	const double SizeM = J->GetNumberField(TEXT("terrainSize"));
	const double SizeCm = SizeM * 100.0;

	// --- Sky ---------------------------------------------------------------
	ADragonSky* Sky = World->SpawnActor<ADragonSky>();
	Sky->SetActorLabel(TEXT("Sky"));
	Sky->CloudMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"));
	Sky->WindYaw = FMath::RadiansToDegrees(J->GetNumberField(TEXT("windBearing")));

	// Water goes in BEFORE the landscape: the Water editor module auto-attaches a
	// landscape brush to any water body spawned over an existing landscape, and that
	// path opens a Slate dialog, which asserts in a commandlet. Our water never edits
	// the terrain anyway (the bake already carved it).
	// --- Water -----------------------------------------------------------------
	// Sea and lakes: our own Single Layer Water surfaces (see ADragonSea).
	if (Options.bWater)
	{
		ADragonSea* Sea = World->SpawnActor<ADragonSea>(FVector::ZeroVector, FRotator::ZeroRotator);
		Sea->SetActorLabel(TEXT("Sea"));
		Sea->SeaMaterial = LoadObject<UMaterialInterface>(nullptr, *(MatRoot / TEXT("M_Sea.M_Sea")));
		Sea->LakeMaterial = LoadObject<UMaterialInterface>(nullptr, *(MatRoot / TEXT("M_Lake.M_Lake")));
		Log(TEXT("sea placed"));
	}
	// The Water plugin's bodies (kept for later: rivers). Off by default — its ocean
	// mesh is only generated by the interactive editor.
	if (Options.bWaterPlugin)
	{
		AWaterZone* Zone = World->SpawnActor<AWaterZone>(FVector::ZeroVector, FRotator::ZeroRotator);
		Zone->SetActorLabel(TEXT("WaterZone"));
		// VERIFY(5.5): AWaterZone::SetZoneExtent(FVector2D) believed present since 5.1.
		Zone->SetZoneExtent(FVector2D(SizeCm * 1.1, SizeCm * 1.1));

		AWaterBodyOcean* Ocean = World->SpawnActor<AWaterBodyOcean>(FVector::ZeroVector, FRotator::ZeroRotator);
		Ocean->SetActorLabel(TEXT("Ocean"));
		if (UWaterSplineComponent* S = Ocean->GetWaterSpline())
		{
			// An ocean's spline is its *coastline*: the water lies outside it, out to the
			// zone's edge. The real coasts come from the terrain (water shows wherever the
			// ground is below the surface), so the spline is just a tiny token "island"
			// parked beyond the playable world, leaving the whole archipelago as sea.
			S->ClearSplinePoints(false);
			const FVector C(SizeCm * 0.7, SizeCm * 0.7, 0.0);
			const double E = 5000.0;
			S->AddSplinePoint(C + FVector(-E, -E, 0), ESplineCoordinateSpace::World, false);
			S->AddSplinePoint(C + FVector(E, -E, 0), ESplineCoordinateSpace::World, false);
			S->AddSplinePoint(C + FVector(E, E, 0), ESplineCoordinateSpace::World, false);
			S->AddSplinePoint(C + FVector(-E, E, 0), ESplineCoordinateSpace::World, false);
			S->SetClosedLoop(true, true);
		}
		if (UWaterBodyComponent* C = Ocean->GetWaterBodyComponent())
		{
			SetAffectsLandscape(C, false); // the terrain is already the shape it should be
			if (UWaterBodyOceanComponent* OC = Cast<UWaterBodyOceanComponent>(C))
			{
				// An ocean's mesh only covers OceanExtents (small by default) around the zone:
				// make it the whole zone, and its collision the whole world.
				// Set the extent directly: FillWaterZoneWithOcean() is a no-op until the body has
				// been assigned to its zone, which hasn't happened yet inside this build.
				OC->SetCollisionExtents(FVector(SizeCm * 0.6, SizeCm * 0.6, 20000.0));
				OC->SetOceanExtent(FVector2D(SizeCm * 1.1, SizeCm * 1.1));
			}
		}

		// Lakes: a body at each lake's level over its bounds; the terrain hides the rest.
		for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("lakes")))
		{
			const TSharedPtr<FJsonObject> L = V->AsObject();
			const double Level = L->GetNumberField(TEXT("level")) * 100.0;
			const double X0 = L->GetNumberField(TEXT("x0")) * 100.0, Y0 = L->GetNumberField(TEXT("z0")) * 100.0;
			const double Span = L->GetNumberField(TEXT("s")) * L->GetNumberField(TEXT("n")) * 100.0;
			AWaterBodyLake* Lake = World->SpawnActor<AWaterBodyLake>(FVector(X0 + Span * 0.5, Y0 + Span * 0.5, Level), FRotator::ZeroRotator);
			if (!Lake) continue;
			if (UWaterSplineComponent* S = Lake->GetWaterSpline())
			{
				S->ClearSplinePoints(false);
				S->AddSplinePoint(FVector(X0, Y0, Level), ESplineCoordinateSpace::World, false);
				S->AddSplinePoint(FVector(X0 + Span, Y0, Level), ESplineCoordinateSpace::World, false);
				S->AddSplinePoint(FVector(X0 + Span, Y0 + Span, Level), ESplineCoordinateSpace::World, false);
				S->AddSplinePoint(FVector(X0, Y0 + Span, Level), ESplineCoordinateSpace::World, false);
				S->SetClosedLoop(true, true);
			}
			if (UWaterBodyComponent* C = Lake->GetWaterBodyComponent()) SetAffectsLandscape(C, false);
		}

		// Rivers: splines down the baked channels, widths from the bake.
		if (Options.bRivers)
		{
			int32 Count = 0;
			for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("rivers")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Pts = V->AsObject()->GetArrayField(TEXT("pts"));
				if (Pts.Num() < 6) continue;
				auto P = [&](int32 i)
				{
					const TArray<TSharedPtr<FJsonValue>>& A = Pts[i]->AsArray();
					return FVector(A[0]->AsNumber() * 100.0, A[1]->AsNumber() * 100.0, A[2]->AsNumber() * 100.0);
				};
				AWaterBodyRiver* River = World->SpawnActor<AWaterBodyRiver>(P(0), FRotator::ZeroRotator);
				if (!River) continue;
				UWaterSplineComponent* S = River->GetWaterSpline();
				// VERIFY(5.5): AWaterBody::GetWaterSplineMetadata() (else River->GetWaterBodyComponent()->GetWaterSplineMetadata()).
				UWaterSplineMetadata* Meta = River->GetWaterSplineMetadata();
				if (S)
				{
					S->ClearSplinePoints(false);
					for (int32 i = 0; i < Pts.Num(); ++i) S->AddSplinePoint(P(i), ESplineCoordinateSpace::World, false);
					S->UpdateSpline();
				}
				if (Meta && S)
				{
					for (int32 i = 0; i < Pts.Num() && i < Meta->RiverWidth.Points.Num(); ++i)
					{
						Meta->RiverWidth.Points[i].OutVal = FMath::Max(300.f, (float)(Pts[i]->AsArray()[3]->AsNumber() * 100.0 * 1.3));
						Meta->Depth.Points[i].OutVal = 150.f;
					}
				}
				if (UWaterBodyComponent* C = River->GetWaterBodyComponent()) SetAffectsLandscape(C, false);
				++Count;
			}
			Log(FString::Printf(TEXT("%d rivers"), Count));
		}
	}

	// --- Landscape -----------------------------------------------------------
	if (Options.bLandscape)
	{
		int32 Res = Options.Res;
		if (Res == 0)
		{
#if PLATFORM_MAC
			Res = 4033;
#else
			Res = 8129;
#endif
		}
		// Valid sizes: N = components * quadsPerComponent + 1.
		int32 QuadsPerSection = 127, Sections = 2;
		if (Res == 4033) { QuadsPerSection = 63; Sections = 2; }
		else if (Res == 16321) { QuadsPerSection = 255; Sections = 1; }
		const FString File = UDragonWorldSubsystem::DataDir() / FString::Printf(TEXT("Heightmap_%d.r16"), Res);
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.Num() != Res * Res * 2)
		{
			Log(FString::Printf(TEXT("! %s missing or wrong size"), *File));
			return false;
		}
		TArray<uint16> Heights;
		Heights.SetNumUninitialized(Res * Res);
		FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());

		const double ZScale = J->GetObjectField(TEXT("heightmap"))->GetNumberField(TEXT("zScalePercent"));
		const double XY = SizeCm / (Res - 1);
		ALandscape* Landscape = World->SpawnActor<ALandscape>();
		Landscape->SetActorLabel(TEXT("Archipelago"));
		Landscape->LandscapeMaterial = LoadObject<UMaterialInterface>(nullptr, *(MatRoot / TEXT("M_Archipelago.M_Archipelago")));
		Landscape->SetActorTransform(FTransform(FQuat::Identity, FVector(-SizeCm * 0.5, -SizeCm * 0.5, 0.0), FVector(XY, XY, ZScale)));
		Landscape->StaticLightingLOD = 0;

		TMap<FGuid, TArray<uint16>> HeightData;
		HeightData.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LayerData;
		LayerData.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
		Landscape->Import(FGuid::NewGuid(), 0, 0, Res - 1, Res - 1, Sections, QuadsPerSection, HeightData, *File, LayerData, ELandscapeImportAlphamapType::Additive,
			TArrayView<const FLandscapeLayer>());
		if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo()) Info->UpdateLayerInfoMap(Landscape);
		Landscape->RegisterAllComponents();
		// Nanite landscape where the platform has it (M2+ Macs, DX12 SM6 PCs): full-resolution
		// geometry at every distance instead of LOD popping, which is what lets 5 m cliffs read.
		// (Opt-in with -LandscapeNanite: the one-shot mesh build needs ~16 GB at 8129^2.)
		if (Options.bLandscapeNanite)
		{
			// bEnableNanite is protected; set it through reflection like the editor's details panel.
			if (FBoolProperty* P = FindFProperty<FBoolProperty>(ALandscapeProxy::StaticClass(), TEXT("bEnableNanite")))
				P->SetPropertyValue_InContainer(Landscape, true);
			Landscape->UpdateNaniteRepresentation(nullptr);
			Log(TEXT("landscape: Nanite on"));
		}
		FFileHelper::SaveStringToFile(FString::FromInt(Res), *(UDragonWorldSubsystem::DataDir() / TEXT("LandscapeRes.txt")));
		Log(FString::Printf(TEXT("landscape %d x %d at %.2f m/quad"), Res, Res, XY / 100.0));
	}

	// Once everything is registered, size the ocean again (now it knows its zone).
	for (TActorIterator<AWaterBodyOcean> It(World); It; ++It)
	{
		if (UWaterBodyOceanComponent* OC = Cast<UWaterBodyOceanComponent>(It->GetWaterBodyComponent()))
		{
			OC->SetOceanExtent(FVector2D(SizeCm * 1.1, SizeCm * 1.1));
			OC->FillWaterZoneWithOcean();
			Log(FString::Printf(TEXT("ocean extent set; zone %s"), OC->GetWaterZone() ? *OC->GetWaterZone()->GetName() : TEXT("none")));
		}
	}

	// --- Scatter -------------------------------------------------------------
	ADragonScatter* Scatter = World->SpawnActor<ADragonScatter>();
	Scatter->SetActorLabel(TEXT("Scatter"));
	Scatter->SetupDefaultLayers(Meshes);

	// --- Player start ----------------------------------------------------------
	const TSharedPtr<FJsonObject> Spawn = J->GetObjectField(TEXT("spawn"));
	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(Spawn->GetNumberField(TEXT("x")) * 100.0, Spawn->GetNumberField(TEXT("z")) * 100.0, 30000.0), FRotator::ZeroRotator);
	Start->SetActorLabel(TEXT("Start"));

	if (Options.bSave)
	{
		UEditorLoadingAndSavingUtils::SaveMap(World, MapPath);
		Log(FString::Printf(TEXT("saved %s"), *MapPath));
	}
	return true;
}

bool FDragonArchipelagoBuilder::Build(const FDragonBuildOptions& Options)
{
	const double T0 = FPlatformTime::Seconds();
	if (Options.bDragonOnly)
	{
		CreateMasterMaterials(); // the skin and membrane parents the instances hang off
		const bool bOk = ImportDragon();
		Log(FString::Printf(TEXT("dragon-only done in %.0f s"), FPlatformTime::Seconds() - T0));
		return bOk;
	}
	if (Options.bTextures)
	{
		ImportGroundTextures();
		CreateLandscapeMaterial();
		CreateMasterMaterials();
	}
	TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>> Meshes;
	if (Options.bModels) { Meshes = ImportModels(); ImportDragon(); }
	else
	{
		// Reuse what's already imported.
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		for (const TCHAR* G : { TEXT("trees"), TEXT("deadwood"), TEXT("rocks"), TEXT("cliffs"), TEXT("ground") })
		{
			TArray<FAssetData> Found;
			Registry.GetAssetsByPath(FName(*(ModelRoot / G)), Found, true);
			for (const FAssetData& A : Found)
			{
				if (A.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
				{
					Meshes.FindOrAdd(G).Add(TSoftObjectPtr<UStaticMesh>(A.GetSoftObjectPath()));
				}
			}
		}
	}
	const bool bOk = BuildLevel(Options, Meshes);
	if (Options.bSave) UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true);
	Log(FString::Printf(TEXT("done in %.0f s"), FPlatformTime::Seconds() - T0));
	return bOk;
}
