#include "DragonArchipelagoBuilder.h"
#include "DragonFlight.h"
#include "DragonSky.h"
#include "DragonScatter.h"
#include "DragonWorldSubsystem.h"

#include "Editor.h"
#include "FileHelpers.h"
#include "EditorAssetLibrary.h"
#include "PhysicsEngine/BodySetup.h"
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
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionCustom.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Materials/MaterialExpressionVertexColor.h"
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
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionEyeAdaptationInverse.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionShadowReplace.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
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

// The full world bake (tools and builder read and write here; the game reads Content/WorldData).
static FString BakeDir() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/World")); }

/** Copy just what the game reads at runtime into Content/WorldData (staged by packaging). */
static void CopyRuntimeData()
{
	const FString Src = BakeDir();
	const FString Dst = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("WorldData"));
	IFileManager& FM = IFileManager::Get();
	FM.DeleteDirectory(*Dst, false, true);
	FM.MakeDirectory(*Dst, true);
	FString Res = TEXT("8129");
	FFileHelper::LoadFileToString(Res, *(Src / TEXT("LandscapeRes.txt")));
	Res.TrimStartAndEndInline();
	for (const FString& F : { FString(TEXT("World.json")), FString(TEXT("LandscapeRes.txt")), FString::Printf(TEXT("Heightmap_%s.r16"), *Res),
		FString(TEXT("Masks_4096.png")), FString(TEXT("Masks2_4096.png")), FString(TEXT("Masks3_4096.png")), FString(TEXT("Flow_4096.png")) })
	{
		if (FM.FileExists(*(Src / F))) FM.Copy(*(Dst / F), *(Src / F));
	}
}


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

	/** MPC_Weather: what the sky tells every material (Wetness 0..1: how rained-on the world is). */
	UMaterialParameterCollection* WeatherMPC()
	{
		UMaterialParameterCollection* C = CreateOrLoad<UMaterialParameterCollection, UMaterialParameterCollectionFactoryNew>(MatRoot, TEXT("MPC_Weather"), [](UMaterialParameterCollectionFactoryNew*) {});
		if (C && !C->ScalarParameters.ContainsByPredicate([](const FCollectionScalarParameter& P) { return P.ParameterName == TEXT("Wetness"); }))
		{
			FCollectionScalarParameter P; P.ParameterName = TEXT("Wetness"); P.DefaultValue = 0.f;
			C->ScalarParameters.Add(P);
			C->PostEditChange();
			UEditorAssetLibrary::SaveLoadedAsset(C, false);
		}
		// Focus: where the dragon is (xyz, cm) and how wide a clear view of it is (w, cm; 0 = off).
		if (C && !C->VectorParameters.ContainsByPredicate([](const FCollectionVectorParameter& P) { return P.ParameterName == TEXT("Focus"); }))
		{
			FCollectionVectorParameter P; P.ParameterName = TEXT("Focus"); P.DefaultValue = FLinearColor(0, 0, 0, 0);
			C->VectorParameters.Add(P);
			C->PostEditChange();
			UEditorAssetLibrary::SaveLoadedAsset(C, false);
		}
		return C;
	}

	/**
	 * Opacity for things the camera may end up inside of (trees): 0 right at the camera and
	 * in a cone from the camera to the dragon, so trunks and boughs never hide it; 1 elsewhere.
	 * Shadows ignore it, so a faded tree still casts its shade.
	 */
	UMaterialExpression* ViewFadeNode(UMaterial* M, int32 X, int32 Y)
	{
		UMaterialExpressionCustom* F = Expr<UMaterialExpressionCustom>(M, X, Y);
		F->Description = TEXT("View fade");
		F->OutputType = CMOT_Float1;
		F->Inputs.Reset();
		for (const TCHAR* In : { TEXT("P"), TEXT("C"), TEXT("Focus") }) { FCustomInput I; I.InputName = In; F->Inputs.Add(I); }
		F->Code = TEXT(
			"float3 v = P - C; float dist = length(v);\n"
			"float nearC = 1.0 - smoothstep(180.0, 520.0, dist);\n"
			"float occl = 0.0;\n"
			"if (Focus.w > 0.0) {\n"
			"  float3 d = Focus.xyz - C; float L = max(length(d), 1.0); float3 dir = d / L;\n"
			"  float t = dot(v, dir);\n"
			"  float perp = length(v - dir * t);\n"
			"  float rad = lerp(160.0, Focus.w, saturate(t / L));\n"
			"  float along = smoothstep(0.0, 60.0, t) * (1.0 - smoothstep(L - 250.0, L + 50.0, t));\n"
			"  occl = along * (1.0 - smoothstep(rad * 0.75, rad * 1.15, perp));\n"
			"}\n"
			"return 1.0 - max(nearC, occl);\n");
		F->PostEditChange();
		UMaterialExpressionWorldPosition* WP = Expr<UMaterialExpressionWorldPosition>(M, X - 300, Y);
		WP->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
		UMaterialExpressionCameraPositionWS* Cam = Expr<UMaterialExpressionCameraPositionWS>(M, X - 300, Y + 80);
		UMaterialExpressionCollectionParameter* Fo = Expr<UMaterialExpressionCollectionParameter>(M, X - 300, Y + 160);
		Fo->Collection = WeatherMPC();
		Fo->ParameterName = TEXT("Focus");
		if (Fo->Collection) Fo->ParameterId = Fo->Collection->GetParameterId(TEXT("Focus"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WP, TEXT(""), F, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Cam, TEXT(""), F, TEXT("C"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Fo, TEXT(""), F, TEXT("Focus"));
		UMaterialExpressionShadowReplace* Sw = Expr<UMaterialExpressionShadowReplace>(M, X + 250, Y);
		UMaterialExpressionConstant* One = Expr<UMaterialExpressionConstant>(M, X, Y + 120);
		One->R = 1.f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(F, TEXT(""), Sw, TEXT("Default"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(One, TEXT(""), Sw, TEXT("Shadow"));
		return Sw;
	}

	/** A node reading MPC_Weather.Wetness. */
	UMaterialExpressionCollectionParameter* WetnessNode(UMaterial* M, int32 X, int32 Y)
	{
		UMaterialExpressionCollectionParameter* E = Expr<UMaterialExpressionCollectionParameter>(M, X, Y);
		E->Collection = WeatherMPC();
		E->ParameterName = TEXT("Wetness");
		if (E->Collection) E->ParameterId = E->Collection->GetParameterId(TEXT("Wetness"));
		return E;
	}

	/**
	 * Emissive that holds its look whatever the exposure. The sun here is ~100 klux and the
	 * camera adapts like an eye, so an emissive of 10 is invisible at noon (it rendered black)
	 * and blinding at night. Through EyeAdaptationInverse, 1 means "as bright as mid-grey on
	 * screen"; Alpha < 1 lets it be a little brighter at night, as a real light would seem.
	 */
	UMaterialExpression* Adapted(UMaterial* M, UMaterialExpression* Src, int32 SrcOut, float Alpha)
	{
		UMaterialExpressionEyeAdaptationInverse* E = Expr<UMaterialExpressionEyeAdaptationInverse>(M, -120, -200);
		UMaterialExpressionConstant* A = Expr<UMaterialExpressionConstant>(M, -260, -120);
		A->R = Alpha;
		E->LightValueInput.Connect(SrcOut, Src);
		E->AlphaInput.Connect(0, A);
		return E;
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
		if (!FFileHelper::LoadFileToString(Text, *(BakeDir() / TEXT("World.json")))) return nullptr;
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
		const FString FFile = BakeDir() / TEXT("Flow_4096.png");
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
		const FString HFile = BakeDir() / TEXT("Heightmap_4033.png");
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
	IFileManager::Get().FindFiles(Masks, *(BakeDir() / TEXT("Masks*.png")), true, false);
	for (const FString& F : Masks)
	{
		if (F.Contains(TEXT("_web"))) continue;   // the web game's original mask, kept for forests.py
		const TCHAR* Name = F.StartsWith(TEXT("Masks2")) ? TEXT("T_Masks2") : F.StartsWith(TEXT("Masks3")) ? TEXT("T_Masks3") : TEXT("T_Masks");
		UTexture2D* T = ImportTexture(BakeDir() / F, TEXT("/Game/World/Data"), Name, false, true);
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
	AddIn(TEXT("P")); AddIn(TEXT("Nv")); AddIn(TEXT("Cam")); AddIn(TEXT("WS")); AddIn(TEXT("SnowLine")); AddIn(TEXT("Wet"));
	AddIn(TEXT("MaskA")); AddIn(TEXT("MaskB")); AddIn(TEXT("MaskC")); AddIn(TEXT("FlowMask"));
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
	UMaterialEditingLibrary::ConnectMaterialExpressions(WetnessNode(M, -1000, Y - 80), TEXT(""), Cu, TEXT("Wet"));

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
	Connect(TEXT("MaskC"), TEXT("/Game/World/Data/T_Masks3.T_Masks3"), SAMPLERTYPE_LinearColor);
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
				V.bCastDynamicShadow = false; V.bCastContactShadow = true; // grass in the shadow maps costs ~5 ms; contact shadows ground it
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
	// The coastline distance field the sea's surf is laid out by (Tools/WorldBake/coast.py).
	{
		const FString CFile = BakeDir() / TEXT("Coast_8192.png");
		if (IFileManager::Get().FileExists(*CFile))
			if (UTexture2D* CT = ImportTexture(CFile, TEXT("/Game/World/Data"), TEXT("T_Coast"), false, true))
			{
				CT->CompressionSettings = TC_Grayscale; CT->SRGB = false; CT->Filter = TF_Bilinear;
				CT->AddressX = TA_Clamp; CT->AddressY = TA_Clamp;
				CT->NeverStream = true;
				CT->PostEditChange();
				UEditorAssetLibrary::SaveLoadedAsset(CT, false);
			}
	}

	// --- M_Surface: opaque PBR for rocks, bark, deadwood -----------------------
	// M_SurfaceFade is the same, masked, for tree trunks: it clears out of the camera's way.
	for (const bool bFade : { false, true })
	if (UMaterial* M = NewMaterial(bFade ? TEXT("M_SurfaceFade") : TEXT("M_Surface")))
	{
		if (bFade)
		{
			M->BlendMode = BLEND_Masked;
			M->DitherOpacityMask = true;
			M->OpacityMaskClipValue = 0.5f;
			UMaterialEditingLibrary::ConnectMaterialProperty(ViewFadeNode(M, -300, 600), TEXT(""), MP_OpacityMask);
		}
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
		// Weathering (rocks, cliffs, deadwood set it to 1; the dragon and buildings keep 0):
		// moss and lichen on the faces that look up, a darker damp foot, and snow on top above
		// the local snow line (same climate as the landscape: lower toward the north).
		UMaterialExpressionScalarParameter* Wx = Expr<UMaterialExpressionScalarParameter>(M, -600, -560);
		Wx->ParameterName = TEXT("Weathering"); Wx->DefaultValue = 0.f;
		UMaterialExpressionScalarParameter* SL = Expr<UMaterialExpressionScalarParameter>(M, -600, -640);
		SL->ParameterName = TEXT("SnowLineM"); SL->DefaultValue = 480.f;
		UMaterialExpressionVertexNormalWS* VN = Expr<UMaterialExpressionVertexNormalWS>(M, -600, -720);
		UMaterialExpressionWorldPosition* WPs = Expr<UMaterialExpressionWorldPosition>(M, -600, -800);
		UMaterialExpressionCustom* Wea = Expr<UMaterialExpressionCustom>(M, -200, -500);
		Wea->Description = TEXT("Weathering");
		Wea->OutputType = CMOT_Float3;
		Wea->Inputs.Reset();
		for (const TCHAR* In : { TEXT("C"), TEXT("Rgh"), TEXT("N"), TEXT("P"), TEXT("W"), TEXT("SnowLine"), TEXT("Wet") }) { FCustomInput I; I.InputName = In; Wea->Inputs.Add(I); }
		Wea->AdditionalOutputs.Reset();
		{ FCustomOutput O; O.OutputName = TEXT("RoughOut"); O.OutputType = CMOT_Float1; Wea->AdditionalOutputs.Add(O); }
		Wea->Code = TEXT(
			"RoughOut = Rgh;\n"
			"if (W <= 0.0) return C;\n"
			"float3 p = P * 0.01; float up = saturate(N.z);\n"
			// smooth value noise (a hash per cell, blended across it): the old per-cell hash with
			// no blend painted moss and lichen on the rocks as hard squares on the world grid
			"#define WH(q) frac(sin(dot(q, float2(12.9898, 78.233))) * 43758.5453)\n"
			// (macro parameters must not be named x or y: they would rewrite the .x/.y swizzles too)
			"#define WVN(Q_, O_) (lerp(lerp(WH(floor(Q_)), WH(floor(Q_) + float2(1, 0)), O_.x), lerp(WH(floor(Q_) + float2(0, 1)), WH(floor(Q_) + float2(1, 1)), O_.x), O_.y))\n"
			"float2 q = p.xy + p.z * float2(0.37, -0.23);\n"
			"float2 x1 = q * 0.7, x2 = q * 0.13 + 17.3, x3 = q * 2.3 + 5.1;\n"
			"float2 f1 = frac(x1), f2 = frac(x2), f3 = frac(x3);\n"
			"f1 = f1 * f1 * (3.0 - 2.0 * f1); f2 = f2 * f2 * (3.0 - 2.0 * f2); f3 = f3 * f3 * (3.0 - 2.0 * f3);\n"
			"float n = WVN(x1, f1) * 0.35 + WVN(x2, f2) * 0.65;\n"
			"float lich = WVN(x3, f3) * 0.6 + WVN(x1, f1) * 0.4;\n"
			"float wob = 1800.0 * sin(p.x / 5200.0 + 0.7) + 900.0 * sin(p.x / 2100.0 + 2.1);\n"
			"float cold = saturate((-p.y - 9000.0 + wob) / 9000.0); cold = cold * cold * (3.0 - 2.0 * cold);\n"
			"float snowL = lerp(SnowLine, 40.0, cold);\n"
			"float moss = W * smoothstep(0.62, 0.95, up * 0.75 + n * 0.4) * (1.0 - smoothstep(snowL * 0.7, snowL, p.z)) * smoothstep(1.0, 6.0, p.z);\n"
			"float3 mossC = lerp(float3(0.06, 0.08, 0.04), float3(0.12, 0.13, 0.08), n);\n"
			// these are basalt islands: the scans (pale sandstones among them) graded to dark grey stone
			"float lumC = dot(C, float3(0.3, 0.59, 0.11));\n"
			"float3 rock = lerp(C * 0.85, lerp(float3(lumC, lumC, lumC * 1.04), C, 0.4) * 0.6, saturate(W));\n"
			"float3 col = lerp(rock, mossC, moss * 0.6);\n"
			// pale crusts of lichen on the bare stone
			"col = lerp(col, float3(0.36, 0.36, 0.32), W * (1.0 - moss) * 0.18 * smoothstep(0.62, 0.85, lich));\n"
			"float snow = W * smoothstep(0.45, 0.8, up + n * 0.2) * smoothstep(snowL - 25.0, snowL + 30.0, p.z + n * 30.0);\n"
			"col = lerp(col, float3(0.86, 0.88, 0.92), snow);\n"
			// wet dark rock in the splash zone
			"col *= lerp(1.0, 0.55, W * (1.0 - smoothstep(0.0, 4.0, p.z)));\n"
			"RoughOut = lerp(lerp(Rgh, 0.95, moss), 0.6, snow) * lerp(1.0, 0.5, W * (1.0 - smoothstep(0.0, 4.0, p.z)));\n"
			// rain: rock and bark go dark and glossy
			"float wet = W * Wet * (1.0 - snow);\n"
			"col *= lerp(1.0, 0.6, wet); RoughOut = lerp(RoughOut, RoughOut * 0.4, wet);\n"
			"return col;\n");
		Wea->RebuildOutputs();
		Wea->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(Mul, TEXT(""), Wea, TEXT("C"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(A, TEXT("G"), Wea, TEXT("Rgh"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(VN, TEXT(""), Wea, TEXT("N"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WPs, TEXT(""), Wea, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Wx, TEXT(""), Wea, TEXT("W"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SL, TEXT(""), Wea, TEXT("SnowLine"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WetnessNode(M, -600, -880), TEXT(""), Wea, TEXT("Wet"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Wea, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(N, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(Wea, TEXT("RoughOut"), MP_Roughness);
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
		// (default = the trees: the scanned conifers are studio-bright; a northern spruce wood is a
		// deep blue-black green from the air. Ground plants set their own tint.)
		Tint->ParameterName = TEXT("Tint"); Tint->DefaultValue = FLinearColor(0.50f, 0.62f, 0.50f);
		// Recolour (ground plants): the scans are dry late-summer gold. Keep their light and dark
		// (the detail) but take the hue from Tint, so the turf is the wet green of a northern isle.
		UMaterialExpressionScalarParameter* Rc = Expr<UMaterialExpressionScalarParameter>(M, -700, -580);
		Rc->ParameterName = TEXT("Recolor"); Rc->DefaultValue = 0.f;
		UMaterialExpressionCustom* Mul = Expr<UMaterialExpressionCustom>(M, -350, -350);
		Mul->Description = TEXT("Tint / recolour");
		Mul->OutputType = CMOT_Float3;
		Mul->Inputs.Reset();
		for (const TCHAR* In : { TEXT("C"), TEXT("T"), TEXT("K") }) { FCustomInput I; I.InputName = In; Mul->Inputs.Add(I); }
		Mul->Code = TEXT(
			"float l = dot(C, float3(0.3, 0.59, 0.11));\n"
			"return lerp(C * T, l * T * 2.2, K);\n");
		Mul->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(D, TEXT("RGB"), Mul, TEXT("C"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Mul, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Rc, TEXT(""), Mul, TEXT("K"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(N, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("G"), MP_Roughness);
		// Leaves must not thin away with distance: a mip averages the cut-out alpha below the clip
		// and far trees go bare. Push the alpha up by how far down the mip chain we are.
		{
			UMaterialExpressionCustom* Cov = Expr<UMaterialExpressionCustom>(M, -350, 450);
			Cov->Description = TEXT("Alpha coverage");
			Cov->OutputType = CMOT_Float1;
			Cov->Inputs.Reset();
			for (const TCHAR* In : { TEXT("A"), TEXT("UV") }) { FCustomInput I; I.InputName = In; Cov->Inputs.Add(I); }
			Cov->Code = TEXT(
				"float2 dx = ddx(UV * 2048.0), dy = ddy(UV * 2048.0);\n"
				"float mip = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1.0));\n"
				"return saturate(A * (1.0 + 0.45 * mip));\n");
			Cov->PostEditChange();
			UMaterialExpressionTextureCoordinate* TC = Expr<UMaterialExpressionTextureCoordinate>(M, -700, 560);
			UMaterialEditingLibrary::ConnectMaterialExpressions(O, TEXT("R"), Cov, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(TC, TEXT(""), Cov, TEXT("UV"));
			UMaterialExpressionMultiply* Fm = Expr<UMaterialExpressionMultiply>(M, -150, 450);
			UMaterialEditingLibrary::ConnectMaterialExpressions(Cov, TEXT(""), Fm, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(ViewFadeNode(M, -450, 1000), TEXT(""), Fm, TEXT("B"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Fm, TEXT(""), MP_OpacityMask);
		}
		// Light through the leaves.
		UMaterialExpressionMultiply* SSS = Expr<UMaterialExpressionMultiply>(M, -350, -150);
		UMaterialExpressionConstant* K = Expr<UMaterialExpressionConstant>(M, -500, -150);
		K->R = 0.3f;   // conifer needles pass little light: more made them glow yellow against the sun
		UMaterialEditingLibrary::ConnectMaterialExpressions(Mul, TEXT(""), SSS, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(K, TEXT(""), SSS, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(SSS, TEXT(""), MP_SubsurfaceColor);

		// Wind: sway grows with height above the instance pivot, gusts travel across the island.
		UMaterialExpressionCustom* Wind = Expr<UMaterialExpressionCustom>(M, -350, 650);
		Wind->Description = TEXT("Wind");
		Wind->OutputType = CMOT_Float3;
		Wind->Inputs.Reset();
		for (const TCHAR* In : { TEXT("P"), TEXT("LP"), TEXT("T"), TEXT("Strength") }) { FCustomInput I; I.InputName = In; Wind->Inputs.Add(I); }
		// Height above the tree's own base, from its local position: the object position of an
		// instanced component is the component's (a whole scatter tile), so on a hillside trees
		// far above the tile's origin bent tens of metres and flung their leaves across the sky.
		Wind->Code = TEXT(
			"float hgt = clamp(LP.z * 0.01, 0.0, 45.0);\n"
			"float bend = hgt * hgt * 0.004 + hgt * 0.01;\n"
			"float gust = sin(T * 0.7 + dot(P.xy, float2(0.0011, 0.0007))) * 0.5 + 0.5;\n"
			"float flut = sin(T * 3.1 + dot(P.xyz, float3(0.031, 0.027, 0.04)));\n"
			"float2 dir = normalize(float2(0.55, -0.83));\n"
			"float a = Strength * (bend * (0.4 + gust) + flut * 0.02 * hgt);\n"
			"return float3(dir * a * 100.0, 0.0);\n");
		Wind->PostEditChange();
		UMaterialExpressionWorldPosition* WP = Expr<UMaterialExpressionWorldPosition>(M, -700, 650);
		WP->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
		UMaterialExpressionLocalPosition* Pivot = Expr<UMaterialExpressionLocalPosition>(M, -700, 730);   // origin: the instance
		UMaterialExpressionTime* Time = Expr<UMaterialExpressionTime>(M, -700, 810);
		UMaterialExpressionScalarParameter* Str = Expr<UMaterialExpressionScalarParameter>(M, -700, 890);
		Str->ParameterName = TEXT("WindStrength"); Str->DefaultValue = 1.f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(WP, TEXT(""), Wind, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Pivot, TEXT(""), Wind, TEXT("LP"));
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
		for (const TCHAR* In : { TEXT("Albedo"), TEXT("Amount"), TEXT("Sat") }) { FCustomInput I; I.InputName = In; Sheen->Inputs.Add(I); }
		Sheen->Code = TEXT(
			// the fan textures are a warm brown; a black hide in full sun reads neutral, a little cool
			"float lum = dot(Albedo, float3(0.3, 0.59, 0.11));\n"
			"Albedo = lerp(float3(lum, lum, lum * 1.06), Albedo, Sat);\n"
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
		UMaterialExpressionScalarParameter* Sat = Expr<UMaterialExpressionScalarParameter>(M, -700, -480);
		Sat->ParameterName = TEXT("Saturation"); Sat->DefaultValue = 1.f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sat, TEXT(""), Sheen, TEXT("Sat"));
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
		Sss->ParameterName = TEXT("SubsurfaceTint"); Sss->DefaultValue = FLinearColor(0.55f, 0.30f, 0.28f);
		UMaterialExpressionScalarParameter* SssAmt = Expr<UMaterialExpressionScalarParameter>(M, -350, 400);
		SssAmt->ParameterName = TEXT("SubsurfaceAmount"); SssAmt->DefaultValue = 0.1f;
		// Scatter scaled by the albedo, so near-black hide stays black instead of glowing orange.
		UMaterialExpressionMultiply* SssC = Expr<UMaterialExpressionMultiply>(M, -150, 300);
		// (from the tinted albedo: the raw warm-brown texture times the red tint browned the sunlit wings)
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tinted, TEXT(""), SssC, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sss, TEXT(""), SssC, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(SssC, TEXT(""), MP_SubsurfaceColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(SssAmt, TEXT(""), MP_Opacity);
		Finish(M);
	}

	// --- M_Sea / M_Lake: Single Layer Water ------------------------------------
	// Twelve Gerstner trains (metres) spread +-80 deg around the prevailing wind, from a
	// 110 m swell down to 2.4 m chop, shared by the displacement and the normal so the
	// lighting matches the shape. Each train's phase is broken up by a slow travelling
	// modulation so no wave runs the whole sea in one straight line (the rows read as a
	// grid from the air otherwise). Amplitude follows the weather (WaveAmplitude) and dies
	// in the shallows so the swell never clips through a beach.
	static const TCHAR* GWaves = TEXT(
		"const float4 Wv[12] = {\n"
		"  float4( 0.883,  0.469, 110.0, 0.550),\n"
		"  float4( 0.695,  0.719,  73.0, 0.360),\n"
		"  float4( 0.999,  0.052,  52.0, 0.260),\n"
		"  float4( 0.375,  0.927,  37.0, 0.180),\n"
		"  float4( 0.927, -0.375,  27.0, 0.120),\n"
		"  float4( 0.788,  0.616,  19.0, 0.085),\n"
		"  float4( 0.974,  0.225,  13.5, 0.060),\n"
		"  float4( 0.035,  0.999,   9.6, 0.040),\n"
		"  float4( 0.743, -0.669,   6.8, 0.028),\n"
		"  float4( 0.530,  0.848,   4.9, 0.020),\n"
		"  float4( 0.993, -0.122,   3.4, 0.014),\n"
		"  float4(-0.309,  0.951,   2.4, 0.010) };\n"
		// group modulation: wave packets, not endless crests
		// (a sum of two oblique sines: their product made every train a checkerboard, and the
		// sea read as a grid from the air)
		"#define GROUP(i, p) (0.62 + 0.2 * sin(dot(p, float2(Wv[i].y, -Wv[i].x)) * (6.2832 / (Wv[i].z * 7.3)) + i * 1.7) + 0.18 * sin(dot(p, float2(0.766 * Wv[i].x - 0.643 * Wv[i].y, 0.643 * Wv[i].x + 0.766 * Wv[i].y)) * (6.2832 / (Wv[i].z * 11.0)) + i * 2.3))\n");
	auto MakeWater = [&](const TCHAR* Name, bool bSea, bool bRiver = false)
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
		// Metres from the shoreline (Tools/WorldBake/coast.py): lays out the surf without the heightmap's texel steps.
		auto* CT = TexO(TEXT("Coast"), TEXT("/Game/World/Data/T_Coast.T_Coast"), SAMPLERTYPE_LinearGrayscale, 420);
		if (!CT->Texture) CT->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		// North Atlantic shallows are a cold bottle green, not a tropical turquoise.
		auto* ShallowS = Vec(TEXT("ScatterShallow"), bSea ? FLinearColor(0.0020f, 0.0052f, 0.0042f) : FLinearColor(0.012f, 0.014f, 0.008f), 470);
		auto* DeepS = Vec(TEXT("ScatterDeep"), bSea ? FLinearColor(0.0013f, 0.0040f, 0.0056f) : FLinearColor(0.006f, 0.008f, 0.006f), 550);
		// (North Atlantic coastal water: plankton absorbs more blue and green than a tropical sea -
		// the old values glowed turquoise in every sunlit cove)
		auto* Ab = Vec(TEXT("Absorption"), bSea ? FLinearColor(0.40f, 0.105f, 0.085f) : FLinearColor(0.50f, 0.22f, 0.20f), 630);

		auto In = [](UMaterialExpressionCustom* C, std::initializer_list<const TCHAR*> Names) { C->Inputs.Reset(); for (const TCHAR* N : Names) { FCustomInput I; I.InputName = N; C->Inputs.Add(I); } };

		// Displacement.
		UMaterialExpressionCustom* Disp = Expr<UMaterialExpressionCustom>(M, -600, -500);
		Disp->Description = TEXT("Swell");
		Disp->OutputType = CMOT_Float3;
		In(Disp, { TEXT("P"), TEXT("O"), TEXT("T"), TEXT("Amp"), TEXT("Half"), TEXT("H"), TEXT("WS"), TEXT("C") });
		Disp->Code = FString(GWaves) + TEXT(
			"float3 p = P * 0.01; float2 o = O.xy * 0.01;\n"
			"float r = max(abs(p.x - o.x), abs(p.y - o.y));\n"
			"float fade = saturate((Half * 0.94 - r) / (Half * 0.5)) * Amp;\n"
			"if (fade <= 0.0) return 0;\n"
			// the swell shoals and dies in the last few metres of depth
			"float2 muv = p.xy / WS + 0.5;\n"
			"float ground = (Texture2DSampleLevel(H, HSampler, muv, 0).r * 65535.0 - 32768.0) / 32.0;\n"
			"if (all(muv > 0.0) && all(muv < 1.0)) fade *= saturate((-ground - 0.5) / 7.0) * saturate((Texture2DSampleLevel(C, CSampler, muv, 0).r * 127.5 - 24.0) / 6.0);\n"
			"float3 off = 0;\n"
			"for (int i = 0; i < 12; i++) {\n"
			"  float2 d = Wv[i].xy; float L = Wv[i].z; float A = Wv[i].w * fade * GROUP(i, p.xy);\n"
			"  float k = 6.2832 / L; float c = sqrt(9.81 / k); float f = k * (dot(d, p.xy) - c * T) + i * 0.9;\n"
			"  float Q = saturate(0.5 / (k * Wv[i].w * 12.0));\n"
			"  off.xy += Q * A * d * cos(f); off.z += A * sin(f); }\n"
			"return off * 100.0;\n");
		Disp->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(WPx, TEXT(""), Disp, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(OP, TEXT(""), Disp, TEXT("O"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tm, TEXT(""), Disp, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Amp, TEXT(""), Disp, TEXT("Amp"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Half, TEXT(""), Disp, TEXT("Half"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(HT, TEXT(""), Disp, TEXT("H"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WS, TEXT(""), Disp, TEXT("WS"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(CT, TEXT(""), Disp, TEXT("C"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Disp, TEXT(""), MP_WorldPositionOffset);

		// Surface: normal (swell + ripples), foam, scattering by depth.
		UMaterialExpressionCustom* Surf = Expr<UMaterialExpressionCustom>(M, -600, 100);
		Surf->Description = TEXT("Sea surface");
		Surf->OutputType = CMOT_Float3;
		In(Surf, { TEXT("P"), TEXT("O"), TEXT("Cam"), TEXT("T"), TEXT("Amp"), TEXT("Half"), TEXT("DS"), TEXT("FoamAmt"), TEXT("WS"),
			TEXT("Tex"), TEXT("FoamT"), TEXT("H"), TEXT("SS"), TEXT("SD"), TEXT("UV"), TEXT("VC"), TEXT("Flow"), TEXT("Cst"), TEXT("WakeT"), TEXT("WakeC"), TEXT("WakeS"), TEXT("RS") });
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
			// depth from the baked terrain first: the swell dies in the shallows here too
			"float2 muv = p.xy / WS + 0.5;\n"
			"float hv = Texture2DSample(H, HSampler, muv).r;\n"
			"float ground = (hv * 65535.0 - 32768.0) / 32.0;\n"
			"bool inWorld = all(muv > 0.0) && all(muv < 1.0);\n"
			"float shoal = inWorld ? saturate((-ground - 0.5) / 7.0) : 1.0;\n"
			"fade *= shoal;\n"
			"for (int i = 0; i < 12; i++) {\n"
			"  float2 d = Wv[i].xy; float L = Wv[i].z; float A = Wv[i].w * fade * GROUP(i, p.xy);\n"
			"  float k = 6.2832 / L; float c = sqrt(9.81 / k); float f = k * (dot(d, p.xy) - c * T) + i * 0.9;\n"
			"  float Q = saturate(0.5 / (k * Wv[i].w * 12.0));\n"
			"  n.xy -= d * k * A * cos(f); n.z -= Q * k * A * sin(f); hz += A * sin(f); }\n"
			// beyond the grid the swell still reads in the lighting (the long trains only, and
			// broken into packets, so the far sea is not ruled into rows)
			"float far = (1.0 - fade) * Amp * 0.45 * shoal;\n"
			"for (int j = 0; j < 5; j++) {\n"
			"  float2 d = Wv[j].xy; float L = Wv[j].z; float A = Wv[j].w * far * GROUP(j, p.xy);\n"
			"  float k = 6.2832 / L; float c = sqrt(9.81 / k); float f = k * (dot(d, p.xy) - c * T) + j * 0.9;\n"
			"  n.xy -= d * k * A * cos(f); }\n"
			// ripples
			"float near = 1.0 / (1.0 + dist / 700.0);\n"
			// rivers: ripples ride the current (UV = metres across, metres along the channel)
			"float2 c = Flow > 0.0 ? float2(UV.x, UV.y - T * Flow) : p.xy;\n"
			"float2 q = 0;\n"
			// Every layer is turned to its own angle and scaled off any common multiple, and two
			// differently turned copies of the main layer cross-fade by a slow, broad noise: no
			// tile's pattern lines up with another's, so the sea shows no grid from any height.
			// (Rivers keep their axes: their ripples run with the current.)
			"float rr = Flow > 0.0 ? 0.0 : 1.0;\n"
			"float2 c1 = lerp(c, float2(0.829 * c.x - 0.559 * c.y, 0.559 * c.x + 0.829 * c.y), rr);\n"
			"float2 c2 = lerp(c, float2(0.391 * c.x + 0.921 * c.y, -0.921 * c.x + 0.391 * c.y), rr);\n"
			"float2 c3 = lerp(c, float2(0.966 * c.x + 0.259 * c.y, -0.259 * c.x + 0.966 * c.y), rr);\n"
			"float2 c4 = lerp(c, float2(-0.454 * c.x - 0.891 * c.y, 0.891 * c.x - 0.454 * c.y), rr);\n"
			"float vA = Texture2DSample(FoamT, FoamTSampler, c / 530.0 + T * float2(0.0011, 0.0007)).r;\n"
			"float vB = Texture2DSample(FoamT, FoamTSampler, c4 / 211.0 - T * float2(0.0017, 0.0009)).r;\n"
			"float mixw = smoothstep(0.3, 0.7, vA);\n"
			// macro ripples (cat's paws) the size of a field, so the far sea has gusty patches
			"q += (Texture2DSample(Tex, TexSampler, c1 / (167.0 * DS) + T * float2(0.0031, 0.0019)).xy * 2 - 1) * 0.35 * Amp * (0.5 + vB);\n"
			"float2 mA = Texture2DSample(Tex, TexSampler, c2 / (29.3 * DS) + T * float2(0.010, 0.006)).xy * 2 - 1;\n"
			"float2 mB = Texture2DSample(Tex, TexSampler, c3 / (37.9 * DS) + T * float2(-0.007, 0.009)).xy * 2 - 1;\n"
			"q += lerp(mA, mB, mixw) * 0.62 * (0.6 + 0.7 * vB);\n"
			"q += (Texture2DSample(Tex, TexSampler, c4 / (10.7 * DS) + T * float2(-0.019, 0.023)).xy * 2 - 1) * 0.45 * near * (0.55 + 0.8 * vA);\n"
			"q += (Texture2DSample(Tex, TexSampler, c1 / (3.3 * DS) + T * float2(0.047, -0.033)).xy * 2 - 1) * 0.35 * near * near;\n"
			"float3 N = normalize(float3(n.xy / max(n.z, 0.2) + q * 0.32 * RS, 1.0));\n"
			// ripples and wakes from ADragonWake's wave-equation field
			"float wakeFoam = 0.0;\n"
			"{ float2 wuv = (P.xy - WakeC.xy) / WakeS + 0.5;\n"
			"  if (Flow <= 0.0 && all(wuv > 0.003) && all(wuv < 0.997)) {\n"
			"    float tx = 1.0 / 768.0;\n"
			"    float3 w0 = Texture2DSample(WakeT, WakeTSampler, wuv).rgb;\n"
			"    if (w0.r > 0.01) {\n"
			"      float hx = Texture2DSample(WakeT, WakeTSampler, wuv + float2(tx, 0)).r - Texture2DSample(WakeT, WakeTSampler, wuv - float2(tx, 0)).r;\n"
			"      float hy = Texture2DSample(WakeT, WakeTSampler, wuv + float2(0, tx)).r - Texture2DSample(WakeT, WakeTSampler, wuv - float2(0, tx)).r;\n"
			"      float2 slope = float2(hx, hy) * 4.0 / (2.0 * WakeS * 0.01 * tx);\n"
			"      N = normalize(float3(N.xy / max(N.z, 0.2) - clamp(slope, -0.6, 0.6) * 0.7, 1.0));\n"
			"      wakeFoam = w0.b; } } }\n"
			"float depth = max(0.0, hz - ground);\n"
			"if (!inWorld) depth = 400.0;\n"
			// ---- The shore, laid out by metres from the coastline (coast.py), not by depth ----
			// Foam is fine lace: small turned samples (the foam texture's cells read as big blotches,
			// like browning butter, when sampled at 7-60 m).
			"float2 pr = float2(0.829 * p.x - 0.559 * p.y, 0.559 * p.x + 0.829 * p.y);\n"
			"float ft = Texture2DSample(FoamT, FoamTSampler, pr / 3.1 + T * float2(0.013, 0.009)).r;\n"
			"float ft2 = Texture2DSample(FoamT, FoamTSampler, p.xy / 8.3 - T * float2(0.006, 0.011)).r;\n"
			"float ft3 = Texture2DSample(FoamT, FoamTSampler, pr / 61.0 + T * float2(0.003, -0.004)).r;\n"
			"float surf = 0.0; float sd = 400.0; float cliff = 0.0; float turbid = 0.0;\n"
			"if (inWorld && Flow <= 0.0) {\n"
			"  float e = 6.0 / WS;\n"
			"  sd = Texture2DSample(Cst, CstSampler, muv).r * 127.5 - 24.0;\n"
			"  float sx = Texture2DSample(Cst, CstSampler, muv + float2(e, 0)).r * 127.5 - 24.0;\n"
			"  float sy = Texture2DSample(Cst, CstSampler, muv + float2(0, e)).r * 127.5 - 24.0;\n"
			"  float2 g = float2(sx - sd, sy - sd); float2 toSea = g / max(length(g), 1e-3);\n"
			"  if (sd < 80.0 && sd > -3.0) {\n"
			// what kind of shore: high ground 15 m behind the line and deep water 20 m off it make a cliff
			"    float2 lp = (p.xy - toSea * (sd + 15.0)) / WS + 0.5; float2 sp = (p.xy - toSea * (sd - 20.0)) / WS + 0.5;\n"
			"    float hl = (Texture2DSample(H, HSampler, lp).r * 65535.0 - 32768.0) / 32.0;\n"
			"    float hs = (Texture2DSample(H, HSampler, sp).r * 65535.0 - 32768.0) / 32.0;\n"
			"    cliff = smoothstep(4.0, 16.0, hl) * smoothstep(2.0, 9.0, -hs);\n"
			"    float sea = saturate((Amp - 0.35) / 1.2);\n"
			// each stretch of coast has its own surf: wider here, calmer there
			"    float vary = Texture2DSample(FoamT, FoamTSampler, p.xy / 410.0 + 0.37).r;\n"
			"    float aa = saturate(dist / 900.0);\n"
			// coordinates along the shore and out from it: foam is drawn out along the waterline
			"    float2 tang = float2(-toSea.y, toSea.x);\n"
			"    float along = dot(p.xy, tang);\n"
			"    float lineN = Texture2DSample(FoamT, FoamTSampler, float2(along / 23.0, sd / 2.2) + T * float2(0.004, 0.0)).r;\n"
			// breakers: lines parallel to the shore rolling in, a bright crest and a fading lace behind
			"    float zone = lerp(lerp(8.0, 20.0, vary), 4.0, cliff) * (0.35 + 0.65 * sea);\n"
			"    float Lb = lerp(13.0, 9.0, cliff);\n"
			"    float ph = sd / Lb + T * (4.2 / Lb) + ft3 * 0.7 + vary * 3.0;\n"
			"    float age = frac(ph);\n"
			"    float lw = fwidth(ph);\n"
			"    float front = smoothstep(0.0, max(lw, 0.03), age) * exp(-age * 5.0);\n"
			"    front = lerp(front, 0.12, saturate(lw * 2.5));\n"
			"    float lace = smoothstep(0.30 + age * 0.45, 0.62 + age * 0.45, ft * 0.55 + lineN * 0.55);\n"
			"    float inZone = smoothstep(zone, zone * 0.5, sd) * smoothstep(-0.5, 1.5, sd);\n"
			"    float brk = front * lerp(lace, 0.3, aa) * inZone * (0.30 + 0.5 * sea);\n"
			// some stretches spill, some don't
			"    brk *= smoothstep(0.25, 0.6, ft3 + 0.25 * sin(ph * 0.37 + vary * 9.0));\n"
			// swash: a soft white edge at the waterline, reaching further as each wave runs up
			"    float run = lerp(1.2, 3.2, pow(saturate(0.5 + 0.5 * sin(T * 0.75 + vary * 12.0 + ft3 * 3.0)), 2.0)) * (0.5 + 0.5 * sea);\n"
			"    float swash = smoothstep(run, run * 0.2, sd) * (0.55 + 0.45 * smoothstep(0.3, 0.7, ft * 0.5 + lineN * 0.6));\n"
			// at the foot of a cliff the swell bursts and falls back: a pulsing collar of white water
			"    float surge = pow(saturate(0.5 + 0.5 * sin(T * 0.62 + vary * 17.0 + ft3 * 2.0)), 3.0);\n"
			"    float collarW = lerp(1.5, 5.0, surge) * (0.5 + 0.6 * sea);\n"
			"    float collar = cliff * smoothstep(collarW, collarW * 0.2, sd) * smoothstep(0.35, 0.65, ft * 0.6 + lineN * 0.5 + surge * 0.2);\n"
			// and the foam it leaves drifts off as thin streaks along the shore, not a sheet
			"    float streak = smoothstep(0.62, 0.8, lineN * 0.8 + ft * 0.3) * exp(-sd / lerp(6.0, 14.0, cliff * sea)) * (0.10 + 0.2 * cliff) * (0.25 + 0.75 * sea);\n"
			"    streak *= 1.0 - aa * 0.6;\n"
			"    surf = max(max(brk, swash * (0.55 + 0.25 * sea)), max(collar * (0.35 + 0.4 * sea), streak));\n"
			// stirred-up water near beaches is milkier and greener; it clears away from the shore
			"    turbid = exp(-sd / 45.0) * (1.0 - 0.6 * cliff) * (0.4 + 0.6 * sea);\n"
			"  }\n"
			"}\n"
			// whitecaps come in scattered patches where gusts push the crests over, not on every
			// crest of the long swell (which lines them up into rows); thinned with distance, where
			// they'd shrink to dotted lines.
			"float patch = smoothstep(0.50, 0.80, Texture2DSample(FoamT, FoamTSampler, p.xy / 310.0 + T * float2(0.0021, 0.0013)).r * 0.6\n"
			"            + Texture2DSample(FoamT, FoamTSampler, p.xy / 97.0 - T * float2(0.0030, 0.0042)).r * 0.5);\n"
			"float crest = saturate((hz - 0.70) * 2.0) * smoothstep(0.68, 0.92, ft) * patch * 0.45 * (1.0 - smoothstep(250.0, 900.0, dist));\n"
			// rapids: white water where the channel steepens (vertex colour R), streaming with the flow
			"float rap = 0.0;\n"
			"if (Flow > 0.0) { float rf = Texture2DSample(FoamT, FoamTSampler, float2(UV.x, UV.y - T * Flow * 1.3) / 5.0).r;\n"
			"  rap = VC * 0.7 * smoothstep(0.45, 0.85, rf + VC * 0.2); }\n"
			"float wf = saturate(wakeFoam);\n"
			"FoamOut = saturate((surf + crest) * FoamAmt + rap + 0.75 * wf * smoothstep(0.5, 0.8, ft * 0.6 + ft2 * 0.45 + wf * 0.25));\n"
			"Rough = lerp(0.035, 0.6, FoamOut);\n"
			"Scatter = lerp(SS, SD, saturate(sd / 35.0)) + float3(0.003, 0.007, 0.005) * turbid;\n"
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
		UMaterialEditingLibrary::ConnectMaterialExpressions(CT, TEXT(""), Surf, TEXT("Cst"));
		{
			// The wave-equation field ADragonWake writes around the camera (black = nothing).
			UMaterialExpressionTextureObjectParameter* WT = Expr<UMaterialExpressionTextureObjectParameter>(M, -1100, 1140);
			WT->ParameterName = TEXT("WakeTex");
			WT->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
			WT->SamplerType = SAMPLERTYPE_LinearColor;
			UMaterialEditingLibrary::ConnectMaterialExpressions(WT, TEXT(""), Surf, TEXT("WakeT"));
			UMaterialExpressionVectorParameter* WC = Expr<UMaterialExpressionVectorParameter>(M, -1100, 1220);
			WC->ParameterName = TEXT("WakeCentre"); WC->DefaultValue = FLinearColor(0, 0, 0, 0);
			UMaterialEditingLibrary::ConnectMaterialExpressions(WC, TEXT(""), Surf, TEXT("WakeC"));
			UMaterialExpressionScalarParameter* WSz = Expr<UMaterialExpressionScalarParameter>(M, -1100, 1300);
			WSz->ParameterName = TEXT("WakeSize"); WSz->DefaultValue = 38400.f;
			UMaterialEditingLibrary::ConnectMaterialExpressions(WSz, TEXT(""), Surf, TEXT("WakeS"));
		}
		{
			UMaterialExpressionTextureCoordinate* TC = Expr<UMaterialExpressionTextureCoordinate>(M, -1100, 900);
			UMaterialExpressionVertexColor* VCx = Expr<UMaterialExpressionVertexColor>(M, -1100, 980);
			auto* FlowP = Scalar(TEXT("FlowSpeed"), bRiver ? 1.4f : 0.f, 1060);
			// A lake in the lee of its hills is calm: faint ripples, long reflections (the sea's
			// ripples on it read as a ruled pattern).
			auto* RSP = Scalar(TEXT("RippleStrength"), bSea || bRiver ? 1.f : 0.4f, 1380);
			UMaterialEditingLibrary::ConnectMaterialExpressions(RSP, TEXT(""), Surf, TEXT("RS"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(TC, TEXT(""), Surf, TEXT("UV"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(VCx, TEXT("R"), Surf, TEXT("VC"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(FlowP, TEXT(""), Surf, TEXT("Flow"));
		}
		M->bTangentSpaceNormal = false; // the grid is flat and unrotated: we write a world normal
		UMaterialEditingLibrary::ConnectMaterialProperty(Surf, TEXT(""), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(Surf, TEXT("Rough"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Surf, TEXT("FoamOut"), MP_Opacity);
		UMaterialExpressionConstant3Vector* FoamCol = Expr<UMaterialExpressionConstant3Vector>(M, -300, -150);
		FoamCol->Constant = FLinearColor(0.80f, 0.83f, 0.85f);
		UMaterialEditingLibrary::ConnectMaterialProperty(FoamCol, TEXT(""), MP_BaseColor);

		UMaterialExpressionSingleLayerWaterMaterialOutput* Out = Expr<UMaterialExpressionSingleLayerWaterMaterialOutput>(M, -100, 500);
		UMaterialExpressionScalarParameter* G = Scalar(TEXT("PhaseG"), bSea ? 0.05f : 0.25f, 720);
		UMaterialExpressionConstant* Behind = Expr<UMaterialExpressionConstant>(M, -400, 800);
		Behind->R = 1.f;
		Out->AbsorptionCoefficients.Connect(0, Ab);
		Out->ScatteringCoefficients.Connect(3, Surf); // output index 3 = "Scatter" (0 is the return value)
		Out->PhaseG.Connect(0, G);
		Out->ColorScaleBehindWater.Connect(0, Behind);
		Finish(M);
	};
	MakeWater(TEXT("M_Sea"), true);
	MakeWater(TEXT("M_River"), false, true);
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
		for (const TCHAR* In : { TEXT("UV"), TEXT("T"), TEXT("Intensity"), TEXT("VC") }) { FCustomInput I; I.InputName = In; Rain->Inputs.Add(I); }
		// Vertex colour: R = how near the layer is (1 at arm's length, ~0.25 at 40 m), G = metres of
		// rain per v-tile / 20 (so every layer falls at ~9 m/s). Drops are hair-thin, short, of
		// uneven brightness, and most are faint: rain is seen mostly as a veil, not as dashes.
		Rain->Code = TEXT(
			"float m = 0;\n"
			"float tileH = max(VC.g * 20.0, 1.0);\n"
			"float spd = 9.0 / tileH;\n"
			"for (int k = 0; k < 3; k++) {\n"
			"  float2 uv = UV * float2(1.0 + k * 0.73, 1.0 + k * 0.41) + float2(k * 0.37, k * 0.11);\n"
			"  float col = floor(uv.x);\n"
			"  float h = frac(sin(col * 12.9898 + k * 7.1) * 43758.5453);\n"
			"  float h2 = frac(h * 7.31 + 0.13);\n"
			"  float y = frac(uv.y + T * spd * (0.8 + 0.4 * h) + h * 10.0);\n"
			"  float len = 0.06 + 0.10 * h2;\n"
			"  float streak = smoothstep(0.0, 0.015, y) * (1.0 - smoothstep(0.015, len, y));\n"
			"  float w = pow(saturate(1.0 - abs(frac(uv.x) - 0.5 - (h2 - 0.5) * 0.6) * 2.0), 28.0);\n"
			"  m += streak * w * step(1.0 - Intensity, h * 0.999) * (0.25 + 0.75 * h2 * h2) / (1.0 + k * 0.6);\n"
			"}\n"
			"return m * VC.r;\n");
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
		UMaterialEditingLibrary::ConnectMaterialExpressions(Expr<UMaterialExpressionVertexColor>(M, -600, 270), TEXT(""), Rain, TEXT("VC"));
		// Opacity = streaks * 0.35; colour = a pale grey that dims at night.
		UMaterialExpressionMultiply* Op = Expr<UMaterialExpressionMultiply>(M, -150, 80);
		UMaterialExpressionConstant* K = Expr<UMaterialExpressionConstant>(M, -350, 160);
		K->R = 0.45f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Rain, TEXT(""), Op, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(K, TEXT(""), Op, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Op, TEXT(""), MP_Opacity);
		UMaterialExpressionCustom* Col = Expr<UMaterialExpressionCustom>(M, -150, -80);
		Col->OutputType = CMOT_Float3;
		Col->Inputs.Reset();
		{ FCustomInput I; I.InputName = TEXT("Night"); Col->Inputs.Add(I); }
		Col->Code = TEXT("return lerp(float3(0.55, 0.58, 0.62), float3(0.04, 0.05, 0.06), Night);");
		Col->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(Night, TEXT(""), Col, TEXT("Night"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Adapted(M, Col, 0, 1.f), TEXT(""), MP_EmissiveColor);
		Finish(M);
	}

	// --- M_WakeSim: one step of the wave equation for ADragonWake -------------------
	// Drawn into a render target each 1/60 s; reads the previous step (Prev). R = h(t) / 4 + 0.5,
	// G = h(t - dt) / 4 + 0.5, B = foam (the write is clamped to 0..1). Sources S0..S7 = (u, v, radius^2, metres this step), F0..F7 foam.
	if (UMaterial* M = NewMaterial(TEXT("M_WakeSim")))
	{
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = BLEND_Opaque;
		M->SetUsageByFlag(MATUSAGE_StaticLighting, false);
		UMaterialExpressionCustom* Sim = Expr<UMaterialExpressionCustom>(M, -350, 0);
		Sim->Description = TEXT("Wave equation step");
		Sim->OutputType = CMOT_Float3;
		Sim->Inputs.Reset();
		TArray<FString> Names = { TEXT("Prev"), TEXT("UV"), TEXT("Offset"), TEXT("Texel"), TEXT("C2"), TEXT("Damp"), TEXT("FoamDecay") };
		for (int32 i = 0; i < 8; ++i) { Names.Add(FString::Printf(TEXT("S%d"), i)); Names.Add(FString::Printf(TEXT("F%d"), i)); }
		for (const FString& N : Names) { FCustomInput I; I.InputName = *N; Sim->Inputs.Add(I); }
		Sim->Code = TEXT(
			"float2 uv = UV + Offset.xy;\n"
			"float t = Texel;\n"
			"float3 c = Texture2DSampleLevel(Prev, PrevSampler, uv, 0).rgb;\n"
			"float inside = (all(uv > 0.0) && all(uv < 1.0)) ? 1.0 : 0.0;\n"
			// stored as h / 4 + 0.5: the render-target write is clamped to 0..1
			"float h = (c.r - 0.5) * 4.0 * inside, hp = (c.g - 0.5) * 4.0 * inside, foam = c.b * inside;\n"
			"float hl = (Texture2DSampleLevel(Prev, PrevSampler, uv - float2(t, 0), 0).r - 0.5) * 4.0;\n"
			"float hr = (Texture2DSampleLevel(Prev, PrevSampler, uv + float2(t, 0), 0).r - 0.5) * 4.0;\n"
			"float hd = (Texture2DSampleLevel(Prev, PrevSampler, uv - float2(0, t), 0).r - 0.5) * 4.0;\n"
			"float hu = (Texture2DSampleLevel(Prev, PrevSampler, uv + float2(0, t), 0).r - 0.5) * 4.0;\n"
			"float lap = (hl + hr + hu + hd - 4.0 * h) * inside;\n"
			// gravity pulls the mean level back: a pressed-down patch doesn't stay down
			"float hn = (2.0 * h - hp + C2 * lap - 0.004 * h) * Damp;\n"
			"foam *= FoamDecay;\n"
			"float4 S[8] = { S0, S1, S2, S3, S4, S5, S6, S7 };\n"
			"float Fq[8] = { F0, F1, F2, F3, F4, F5, F6, F7 };\n"
			"[unroll] for (int i = 0; i < 8; i++) {\n"
			"  float2 d = UV - S[i].xy; float g = exp(-dot(d, d) / max(S[i].z, 1e-9));\n"
			// an object in the water holds the surface near its own depth there (S.w, metres): the
			// field is pulled toward it, then springs back and rings out when it moves on
			"  hn = lerp(hn, S[i].w, saturate(g * 0.65)); foam += Fq[i] * g; }\n"
			// steep, breaking ripples whiten a little
			"foam += saturate(abs(lap) * 4.0 - 0.4) * 0.01;\n"
			// the window's edge absorbs: nothing reflects back in
			"float2 e = min(UV, 1.0 - UV); float edge = saturate(min(e.x, e.y) * 14.0);\n"
			"hn = clamp(hn * edge, -1.9, 1.9); h = clamp(h * edge, -1.9, 1.9);\n"
			"return float3(hn * 0.25 + 0.5, h * 0.25 + 0.5, saturate(foam) * edge);\n");
		Sim->PostEditChange();
		auto Sc = [&](const TCHAR* N, float V, int32 Y) { UMaterialExpressionScalarParameter* E = Expr<UMaterialExpressionScalarParameter>(M, -700, Y); E->ParameterName = N; E->DefaultValue = V; UMaterialEditingLibrary::ConnectMaterialExpressions(E, TEXT(""), Sim, N); };
		auto Ve = [&](const TCHAR* N, int32 Y) { UMaterialExpressionVectorParameter* E = Expr<UMaterialExpressionVectorParameter>(M, -700, Y); E->ParameterName = N; E->DefaultValue = FLinearColor(0, 0, 1e-6f, 0); UMaterialEditingLibrary::ConnectMaterialExpressions(E, TEXT("RGBA"), Sim, N); };
		UMaterialExpressionTextureObjectParameter* Prev = Expr<UMaterialExpressionTextureObjectParameter>(M, -700, -300);
		Prev->ParameterName = TEXT("Prev");
		Prev->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
		Prev->SamplerType = SAMPLERTYPE_LinearColor;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Prev, TEXT(""), Sim, TEXT("Prev"));
		UMaterialExpressionTextureCoordinate* TC = Expr<UMaterialExpressionTextureCoordinate>(M, -700, -200);
		UMaterialEditingLibrary::ConnectMaterialExpressions(TC, TEXT(""), Sim, TEXT("UV"));
		Ve(TEXT("Offset"), -120);
		Sc(TEXT("Texel"), 1.f / 768.f, -40); Sc(TEXT("C2"), 0.007f, 40); Sc(TEXT("Damp"), 0.9965f, 120); Sc(TEXT("FoamDecay"), 0.9955f, 200);
		for (int32 i = 0; i < 8; ++i)
		{
			Ve(*FString::Printf(TEXT("S%d"), i), 280 + i * 160);
			Sc(*FString::Printf(TEXT("F%d"), i), 0.f, 360 + i * 160);
		}
		UMaterialEditingLibrary::ConnectMaterialProperty(Sim, TEXT(""), MP_EmissiveColor);
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
		UMaterialEditingLibrary::ConnectMaterialProperty(Adapted(M, Mul, 0, 0.9f), TEXT(""), MP_EmissiveColor);
		Finish(M);
	}
	// --- Effects (ADragonFX): no textures, all procedural ------------------------
	auto Inputs = [](UMaterialExpressionCustom* C, std::initializer_list<const TCHAR*> Names) { C->Inputs.Reset(); for (const TCHAR* N : Names) { FCustomInput I; I.InputName = N; C->Inputs.Add(I); } };
	auto SParam = [&](UMaterial* M, const TCHAR* N, float V, int32 Y) { UMaterialExpressionScalarParameter* E = Expr<UMaterialExpressionScalarParameter>(M, -800, Y); E->ParameterName = N; E->DefaultValue = V; return E; };
	auto VParam = [&](UMaterial* M, const TCHAR* N, FLinearColor V, int32 Y) { UMaterialExpressionVectorParameter* E = Expr<UMaterialExpressionVectorParameter>(M, -800, Y); E->ParameterName = N; E->DefaultValue = V; return E; };
	// fbm without helper functions (Custom nodes are a function body): four octaves inline
	static const TCHAR* GFbm = TEXT(
		"float fbm = 0.0; { float2 x = q; float amp = 0.5;\n"
		"  [unroll] for (int o = 0; o < 4; o++) { float2 i = floor(x), f = frac(x); f = f * f * (3.0 - 2.0 * f);\n"
		"    float a = frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5453), b = frac(sin(dot(i + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);\n"
		"    float c = frac(sin(dot(i + float2(0, 1), float2(127.1, 311.7))) * 43758.5453), d = frac(sin(dot(i + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);\n"
		"    fbm += amp * lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y); x = x * 2.03 + 17.1; amp *= 0.5; } }\n");

	// M_PlasmaCore: the bolt. White-hot where it faces you, violet-blue at the rim, flickering.
	if (UMaterial* M = NewMaterial(TEXT("M_PlasmaCore")))
	{
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = BLEND_Additive;
		M->SetUsageByFlag(MATUSAGE_StaticLighting, false);
		UMaterialExpressionCustom* C = Expr<UMaterialExpressionCustom>(M, -300, 0);
		C->Description = TEXT("Plasma"); C->OutputType = CMOT_Float3;
		Inputs(C, { TEXT("N"), TEXT("V"), TEXT("T"), TEXT("P"), TEXT("Col"), TEXT("I"), TEXT("Hot"), TEXT("Fade") });
		C->Code = TEXT(
			"float f = saturate(dot(normalize(N), normalize(V)));\n"
			"float core = pow(f, lerp(5.0, 1.6, Hot));\n"
			"float rim = pow(1.0 - f, 1.4);\n"
			"float3 p = P * 0.01;\n"
			"float n = frac(sin(dot(floor(p * 4.0 + T * 23.0), float3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
			"float3 col = lerp(Col, float3(1.0, 1.0, 1.0), saturate(core * Hot * 1.4));\n"
			"float e = core * (0.6 + 2.4 * Hot) + rim * 0.9 * (1.0 - Hot);\n"
			"return col * e * I * Fade * (0.8 + 0.4 * n);\n");
		C->PostEditChange();
		UMaterialExpressionVertexNormalWS* VN = Expr<UMaterialExpressionVertexNormalWS>(M, -800, -300);
		UMaterialExpressionCameraVectorWS* CV = Expr<UMaterialExpressionCameraVectorWS>(M, -800, -220);
		UMaterialExpressionTime* T = Expr<UMaterialExpressionTime>(M, -800, -140);
		UMaterialExpressionWorldPosition* WP = Expr<UMaterialExpressionWorldPosition>(M, -800, -60);
		UMaterialEditingLibrary::ConnectMaterialExpressions(VN, TEXT(""), C, TEXT("N"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(CV, TEXT(""), C, TEXT("V"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(T, TEXT(""), C, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WP, TEXT(""), C, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(VParam(M, TEXT("Colour"), FLinearColor(0.38f, 0.45f, 1.f), 40), TEXT(""), C, TEXT("Col"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Intensity"), 30.f, 120), TEXT(""), C, TEXT("I"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Hot"), 1.f, 200), TEXT(""), C, TEXT("Hot"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Fade"), 1.f, 280), TEXT(""), C, TEXT("Fade"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Adapted(M, C, 0, 0.9f), TEXT(""), MP_EmissiveColor);
		Finish(M);
	}

	// M_FxGlow: additive sprite for plasma, fire, sparks, lightning. A soft core and ragged edge.
	// M_FxSmoke: translucent sprite for smoke, dust, steam, spray: billowing noise, soft against the ground.
	for (int32 Kind = 0; Kind < 2; ++Kind)
	{
		const bool bGlow = Kind == 0;
		UMaterial* M = NewMaterial(bGlow ? TEXT("M_FxGlow") : TEXT("M_FxSmoke"));
		if (!M) continue;
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = bGlow ? BLEND_Additive : BLEND_Translucent;
		M->TwoSided = true;
		M->SetUsageByFlag(MATUSAGE_StaticLighting, false);
		UMaterialExpressionCustom* C = Expr<UMaterialExpressionCustom>(M, -300, 0);
		C->Description = bGlow ? TEXT("Glow sprite") : TEXT("Smoke sprite");
		C->OutputType = CMOT_Float3;
		Inputs(C, { TEXT("UV"), TEXT("T"), TEXT("Seed"), TEXT("Age"), TEXT("Col"), TEXT("Fade"), TEXT("DF") });
		C->AdditionalOutputs.Reset();
		{ FCustomOutput O; O.OutputName = TEXT("Alpha"); O.OutputType = CMOT_Float1; C->AdditionalOutputs.Add(O); }
		C->Code = FString(TEXT("float2 d = UV * 2.0 - 1.0; float r = length(d);\n"
			"float2 q = UV * 2.6 + Seed * 3.7 + float2(Age * 0.7, -Age * 1.1);\n")) + GFbm + (bGlow ? TEXT(
			"float edge = 0.62 + 0.38 * fbm;\n"
			"float m = saturate(1.0 - r / edge); m = m * m * (3.0 - 2.0 * m);\n"
			"float core = exp(-r * r * 7.0);\n"
			"Alpha = 1.0;\n"
			"return Col * (m * (0.45 + 0.55 * fbm) + core * 0.8) * Fade * DF;\n") : TEXT(
			// billows: a second, finer noise eats the edge into wisps, so no puff reads as a ball
			"float2 q2 = UV * 6.3 - Seed * 1.9 + float2(-Age * 1.3, Age * 0.6); float fine = 0.0; { float2 x = q2; float amp = 0.5;\n"
			"  [unroll] for (int o = 0; o < 3; o++) { float2 i = floor(x), f = frac(x); f = f * f * (3.0 - 2.0 * f);\n"
			"    float a = frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5453), b = frac(sin(dot(i + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);\n"
			"    float c = frac(sin(dot(i + float2(0, 1), float2(127.1, 311.7))) * 43758.5453), d = frac(sin(dot(i + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);\n"
			"    fine += amp * lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y); x = x * 2.1 + 5.3; amp *= 0.5; } }\n"
			"float m = saturate((1.0 - r * 1.1) * 2.1 - (1.0 - fbm) * 1.25 - (1.0 - fine) * 0.55);\n"
			"m = m * m * (3.0 - 2.0 * m) * (0.75 + 0.25 * fine);\n"
			"Alpha = saturate(m * Fade * DF);\n"
			// a little self-shading: brighter on the upper side, darker in the folds
			"return Col * (0.7 + 0.45 * fbm - d.y * 0.12);\n"));
		C->RebuildOutputs();
		C->PostEditChange();
		UMaterialExpressionTextureCoordinate* UV = Expr<UMaterialExpressionTextureCoordinate>(M, -800, -300);
		UMaterialExpressionTime* T = Expr<UMaterialExpressionTime>(M, -800, -220);
		UMaterialExpressionDepthFade* DF = Expr<UMaterialExpressionDepthFade>(M, -600, 360);
		DF->FadeDistanceDefault = bGlow ? 120.f : 300.f;
		DF->OpacityDefault = 1.f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(UV, TEXT(""), C, TEXT("UV"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(T, TEXT(""), C, TEXT("T"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Seed"), 0.f, -140), TEXT(""), C, TEXT("Seed"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Age"), 0.f, -60), TEXT(""), C, TEXT("Age"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(VParam(M, TEXT("Colour"), FLinearColor::White, 20), TEXT(""), C, TEXT("Col"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Fade"), 1.f, 100), TEXT(""), C, TEXT("Fade"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(DF, TEXT(""), C, TEXT("DF"));
		// glows a little brighter than "screen white" at night; smoke and spray exactly as lit
		UMaterialEditingLibrary::ConnectMaterialProperty(Adapted(M, C, 0, bGlow ? 0.9f : 1.f), TEXT(""), MP_EmissiveColor);
		if (!bGlow) UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("Alpha"), MP_Opacity);
		Finish(M);
	}

	// M_Scorch: deferred decal - a burnt patch with embers that cool.
	if (UMaterial* M = NewMaterial(TEXT("M_Scorch")))
	{
		M->MaterialDomain = MD_DeferredDecal;
		M->BlendMode = BLEND_Translucent;
		M->SetShadingModel(MSM_DefaultLit);
		UMaterialExpressionCustom* C = Expr<UMaterialExpressionCustom>(M, -300, 0);
		C->Description = TEXT("Scorch"); C->OutputType = CMOT_Float3;
		Inputs(C, { TEXT("UV"), TEXT("Seed"), TEXT("Fade"), TEXT("Glow") });
		C->AdditionalOutputs.Reset();
		for (const TCHAR* N : { TEXT("Alpha"), TEXT("Emis") }) { FCustomOutput O; O.OutputName = N; O.OutputType = N[0] == 'A' ? CMOT_Float1 : CMOT_Float3; C->AdditionalOutputs.Add(O); }
		C->Code = FString(TEXT("float2 d = UV * 2.0 - 1.0; float r = length(d);\n"
			"float2 q = UV * 4.0 + Seed * 5.3;\n")) + GFbm + TEXT(
			"float burn = saturate((1.0 - r) * 1.9 - (1.0 - fbm) * 0.9);\n"
			"Alpha = saturate(burn * 1.3) * 0.92 * Fade;\n"
			"float ember = smoothstep(0.62, 0.8, fbm) * saturate(burn * 2.0 - 1.0);\n"
			"Emis = float3(1.0, 0.32, 0.06) * ember * Glow * 30.0;\n"
			"return lerp(float3(0.05, 0.045, 0.04), float3(0.015, 0.013, 0.012), burn);\n");
		C->RebuildOutputs();
		C->PostEditChange();
		UMaterialExpressionTextureCoordinate* UV = Expr<UMaterialExpressionTextureCoordinate>(M, -800, -300);
		UMaterialEditingLibrary::ConnectMaterialExpressions(UV, TEXT(""), C, TEXT("UV"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Seed"), 0.f, -140), TEXT(""), C, TEXT("Seed"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Fade"), 1.f, -60), TEXT(""), C, TEXT("Fade"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(SParam(M, TEXT("Glow"), 1.f, 20), TEXT(""), C, TEXT("Glow"));
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("Alpha"), MP_Opacity);
		UMaterialEditingLibrary::ConnectMaterialProperty(Adapted(M, C, 2, 0.9f), TEXT(""), MP_EmissiveColor);   // output 2 = "Emis"
		UMaterialExpressionConstant* R = Expr<UMaterialExpressionConstant>(M, -300, 200);
		R->R = 0.95f;
		UMaterialEditingLibrary::ConnectMaterialProperty(R, TEXT(""), MP_Roughness);
		Finish(M);
	}

	// M_Ice: icebergs - glacial ice is white where snow and air bubbles are, blue where it is
	// dense; light goes into it (subsurface) and it is wet and glossy at the waterline.
	if (UMaterial* M = NewMaterial(TEXT("M_Ice")))
	{
		M->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
		M->SetUsageByFlag(MATUSAGE_Nanite, true);
		M->SetShadingModel(MSM_Subsurface);
		UMaterialExpressionCustom* C = Expr<UMaterialExpressionCustom>(M, -300, 0);
		C->Description = TEXT("Ice"); C->OutputType = CMOT_Float3;
		Inputs(C, { TEXT("N"), TEXT("P") });
		C->AdditionalOutputs.Reset();
		{ FCustomOutput O; O.OutputName = TEXT("Rgh"); O.OutputType = CMOT_Float1; C->AdditionalOutputs.Add(O); }
		{ FCustomOutput O; O.OutputName = TEXT("Sss"); O.OutputType = CMOT_Float3; C->AdditionalOutputs.Add(O); }
		C->Code = FString(TEXT("float3 p = P * 0.01; float2 q = p.xy * 0.08 + p.z * 0.11;\n")) + GFbm + TEXT(
			"float up = saturate(N.z);\n"
			"float bands = 0.5 + 0.5 * sin(p.z * 0.9 + fbm * 6.0);\n"
			"float3 dense = lerp(float3(0.30, 0.62, 0.78), float3(0.55, 0.80, 0.90), bands * 0.6 + fbm * 0.4);\n"
			"float snow = smoothstep(0.35, 0.75, up + fbm * 0.3);\n"
			"float3 c = lerp(dense, float3(0.93, 0.95, 0.97), snow);\n"
			"float wet = 1.0 - smoothstep(0.0, 2.5, p.z);\n"
			"c = lerp(c, c * float3(0.55, 0.8, 0.85), wet);\n"
			"Rgh = lerp(lerp(0.22, 0.65, snow), 0.08, wet);\n"
			"Sss = float3(0.12, 0.45, 0.65);\n"
			"return c;\n");
		C->RebuildOutputs();
		C->PostEditChange();
		UMaterialExpressionVertexNormalWS* VN = Expr<UMaterialExpressionVertexNormalWS>(M, -800, -200);
		UMaterialExpressionWorldPosition* WP = Expr<UMaterialExpressionWorldPosition>(M, -800, -120);
		UMaterialEditingLibrary::ConnectMaterialExpressions(VN, TEXT(""), C, TEXT("N"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(WP, TEXT(""), C, TEXT("P"));
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("Rgh"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("Sss"), MP_SubsurfaceColor);
		UMaterialExpressionConstant* Op = Expr<UMaterialExpressionConstant>(M, -300, 250);
		Op->R = 0.55f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Op, TEXT(""), MP_Opacity);
		Finish(M);
	}

	// Rocks, cliffs and deadwood weather (moss, snow, wet foot); the dragon and buildings don't.
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		int32 Count = 0;
		for (const TCHAR* G : { TEXT("rocks"), TEXT("cliffs"), TEXT("deadwood") })
		{
			TArray<FAssetData> Found;
			Registry.GetAssetsByPath(FName(*(ModelRoot / G)), Found, true);
			for (const FAssetData& A : Found)
			{
				if (A.AssetClassPath != UMaterialInstanceConstant::StaticClass()->GetClassPathName()) continue;
				if (UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(A.GetAsset()))
				{
					UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("Weathering"), 1.f);
					UMaterialEditingLibrary::UpdateMaterialInstance(MI);
					UEditorAssetLibrary::SaveLoadedAsset(MI, false);
					++Count;
				}
			}
		}
		Log(FString::Printf(TEXT("weathering on %d rock/cliff/deadwood materials"), Count));
		// Tree bark fades out of the camera's way like the needles do.
		if (UMaterial* Fade = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_SurfaceFade.M_SurfaceFade"))))
		{
			int32 NB = 0;
			TArray<FAssetData> Found;
			Registry.GetAssetsByPath(FName(*(ModelRoot / TEXT("trees"))), Found, true);
			for (const FAssetData& A : Found)
			{
				if (A.AssetClassPath != UMaterialInstanceConstant::StaticClass()->GetClassPathName()) continue;
				UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(A.GetAsset());
				if (!MI || !MI->Parent || !MI->Parent->GetMaterial()) continue;
				const FString PN = MI->Parent->GetMaterial()->GetName();
				if (PN != TEXT("M_Surface") && PN != TEXT("M_SurfaceFade")) continue;
				MI->SetParentEditorOnly(Fade);
				UMaterialEditingLibrary::UpdateMaterialInstance(MI);
				UEditorAssetLibrary::SaveLoadedAsset(MI, false);
				++NB;
			}
			Log(FString::Printf(TEXT("view fade on %d tree bark materials"), NB));
		}
		// The scanned grasses are a dry late-summer gold; these islands' turf is wet and green.
		int32 NG = 0;
		for (const TCHAR* G : { TEXT("ground") })
		{
			TArray<FAssetData> Found;
			Registry.GetAssetsByPath(FName(*(ModelRoot / G)), Found, true);
			for (const FAssetData& A : Found)
			{
				if (A.AssetClassPath != UMaterialInstanceConstant::StaticClass()->GetClassPathName()) continue;
				if (UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(A.GetAsset()))
				{
					const FString N = MI->GetName();
					const bool bGrass = N.Contains(TEXT("grass")) || N.Contains(TEXT("moss")) || N.Contains(TEXT("fern"));
					// grass, moss, fern: recoloured to a fresh green (mostly; a little of the scan's
					// warmth stays in the tips); flowers and shrubs keep their own colours, a touch cooled.
					UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI, TEXT("Tint"), bGrass ? FLinearColor(0.17f, 0.27f, 0.10f) : FLinearColor(0.85f, 0.95f, 0.8f));   // a wet, muted northern green (lawn-bright before)
					UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("Recolor"), bGrass ? 0.85f : 0.f);
					UMaterialEditingLibrary::UpdateMaterialInstance(MI);
					UEditorAssetLibrary::SaveLoadedAsset(MI, false);
					++NG;
				}
			}
		}
		Log(FString::Printf(TEXT("green tint on %d ground-cover materials"), NG));
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
int32 FDragonArchipelagoBuilder::ImportVillage()
{
	const FString Src = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Village"));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Src / TEXT("*.glb")), true, false);
	if (Files.Num() == 0) { Log(TEXT("no Data/Village/*.glb — no village kit")); return 0; }
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	int32 Count = 0;
	for (const FString& F : Files)
	{
		const FString Base = FPaths::GetBaseFilename(F);
		const FString Dest = TEXT("/Game/World/Village/") + Base;
		// Fresh each run, so a regenerated kit replaces the old one completely.
		if (UEditorAssetLibrary::DoesDirectoryExist(Dest)) UEditorAssetLibrary::DeleteDirectory(Dest);
		UAutomatedAssetImportData* Data = NewObject<UAutomatedAssetImportData>();
		Data->Filenames.Add(Src / F);
		Data->DestinationPath = Dest;
		Data->bReplaceExisting = true;
		AssetTools.ImportAssetsAutomated(Data);
		Registry.ScanPathsSynchronous({ Dest }, true);
		TArray<FAssetData> Found;
		Registry.GetAssetsByPath(FName(*Dest), Found, true);
		TArray<UStaticMesh*> Meshes;
		for (const FAssetData& A : Found) if (UStaticMesh* M = Cast<UStaticMesh>(A.GetAsset())) Meshes.Add(M);
		for (UStaticMesh* Mesh : Meshes)
		{
			for (FStaticMaterial& SM : Mesh->GetStaticMaterials())
			{
				UMaterial* M = SM.MaterialInterface ? SM.MaterialInterface->GetMaterial() : nullptr;
				if (!M || M->GetPathName().StartsWith(TEXT("/Engine/"))) continue;
				if (!M->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes) || !M->GetUsageByFlag(MATUSAGE_Nanite))
				{
					M->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
					M->SetUsageByFlag(MATUSAGE_Nanite, true);
					M->PostEditChange();
					UEditorAssetLibrary::SaveLoadedAsset(M, false);
				}
			}
			FMeshNaniteSettings NS = Mesh->GetNaniteSettings();
			NS.bEnabled = true;
			Mesh->SetNaniteSettings(NS);
			// Walk on it, land on its roof: the render mesh is the collision.
			if (UBodySetup* BS = Mesh->GetBodySetup()) BS->CollisionTraceFlag = CTF_UseComplexAsSimple;
			Mesh->Build(false);
			Mesh->PostEditChange();
			UEditorAssetLibrary::SaveLoadedAsset(Mesh, false);
		}
		// One mesh per building, named for its file (the village actor reads kind_variant from it).
		if (Meshes.Num() == 1 && Meshes[0]->GetName() != Base)
			UEditorAssetLibrary::RenameAsset(Meshes[0]->GetPathName(), Dest / Base);
		UEditorAssetLibrary::SaveDirectory(Dest, false, true);
		Log(FString::Printf(TEXT("village %s: %d mesh(es)"), *Base, Meshes.Num()));
		Count += Meshes.Num() > 0;
	}
	return Count;
}

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
			if (bWing)
			{
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("SubsurfaceAmount"), 0.04f);
				// Looking toward the sun, what showed on the wings was not their colour but a broad
				// grazing sheen of warm sunlight (it stayed when the tint went near black): halved.
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("Specular"), 0.03f);
			}
			if (bWing || Slot == TEXT("acmat_0"))
			{
				// (the membrane is most of what the chase camera sees from above, in full sun: darker still)
				const float K = bWing ? 0.22f : 0.30f;   // a black hide reflects ~4-6% (the scans are ~15-30%)
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI, TEXT("Tint"), FLinearColor(K, K, K * 1.08f));
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("Saturation"), 0.2f);
			}
			if (FParse::Param(FCommandLine::Get(), TEXT("DragonSlotColours")))   // debug: one flat colour per slot
			{
				static const FLinearColor Dbg[] = { FLinearColor(1, 0, 0), FLinearColor(0, 1, 0), FLinearColor(0, 0, 1), FLinearColor(1, 1, 0), FLinearColor(1, 0, 1), FLinearColor(0, 1, 1), FLinearColor(1, 1, 1) };
				const int32 Ix = bWing ? 6 : FCString::Atoi(*Slot.RightChop(6)) % 6;
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI, TEXT("Tint"), Dbg[Ix] * 20.f);
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("Saturation"), 1.f);
			}
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
		const FString File = BakeDir() / FString::Printf(TEXT("Heightmap_%d.r16"), Res);
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
		FFileHelper::SaveStringToFile(FString::FromInt(Res), *(BakeDir() / TEXT("LandscapeRes.txt")));
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
	if (Options.bVillageOnly)
	{
		const int32 N = ImportVillage();
		Log(FString::Printf(TEXT("village-only: %d buildings imported in %.0f s"), N, FPlatformTime::Seconds() - T0));
		return N > 0;
	}
	if (Options.bMaterialsOnly)
	{
		CreateLandscapeMaterial();
		CreateMasterMaterials();
		UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true);
		CopyRuntimeData();
		Log(FString::Printf(TEXT("materials-only done in %.0f s"), FPlatformTime::Seconds() - T0));
		return true;
	}
	if (Options.bDragonFulo)
	{
		const bool bOk = ImportDragonFulo();
		Log(FString::Printf(TEXT("dragon-fulo done in %.0f s"), FPlatformTime::Seconds() - T0));
		return bOk;
	}
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
	if (Options.bModels) { Meshes = ImportModels(); ImportDragon(); ImportDragonFulo(); ImportVillage(); }
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
	CopyRuntimeData();
	Log(FString::Printf(TEXT("done in %.0f s"), FPlatformTime::Seconds() - T0));
	return bOk;
}


// ===========================================================================
// The Fulonimation Studios Night Fury (https://ko-fi.com/fulonimationstudios)
// Data/Dragon/Fulo: SK_DragonFulo.glb + Textures/, from Tools/DragonModel/export_fulo.py.
// One master surface (M_FuloSurface) set per material slot as the model's Blender
// materials are; the fin's colour map is the player's choice of fin (UDragonFuloRigComponent).
// ===========================================================================
bool FDragonArchipelagoBuilder::ImportDragonFulo()
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Dragon/Fulo"));
	const FString Glb = Dir / TEXT("SK_DragonFulo.glb");
	if (!FPaths::FileExists(Glb)) { Log(TEXT("! no Data/Dragon/Fulo/SK_DragonFulo.glb (run Tools/DragonModel/export_fulo.py)")); return false; }
	const FString Dest = TEXT("/Game/DragonFulo");
	const FString TexDir = Dir / TEXT("Textures");
	const FString TexPkg = Dest / TEXT("Textures");

	// --- textures
	auto Tex = [&](const TCHAR* Name, bool bNormal, bool bLinear) -> UTexture2D*
	{
		UTexture2D* T = ImportTexture(TexDir / (FString(Name) + TEXT(".png")), TexPkg, Name, bNormal, bLinear);
		if (!T) Log(FString::Printf(TEXT("! missing texture %s"), Name));
		return T;
	};
	UTexture2D* BodyD = Tex(TEXT("T_Fulo_Body_D"), false, false);
	UTexture2D* BodyN = Tex(TEXT("T_Fulo_Body_N"), true, false);
	UTexture2D* BodyC = Tex(TEXT("T_Fulo_Body_Cavity"), false, true);
	UTexture2D* BodyG = Tex(TEXT("T_Fulo_Body_Glow"), false, false);
	UTexture2D* EyeD = Tex(TEXT("T_Fulo_Eye_D"), false, false);
	UTexture2D* MouthD = Tex(TEXT("T_Fulo_Mouth_D"), false, false);
	UTexture2D* MouthN = Tex(TEXT("T_Fulo_Mouth_N"), true, false);
	UTexture2D* MouthC = Tex(TEXT("T_Fulo_Mouth_Cavity"), false, true);
	UTexture2D* TeethD = Tex(TEXT("T_Fulo_Teeth_D"), false, false);
	UTexture2D* TongueN = Tex(TEXT("T_Fulo_Tongue_N"), true, false);
	UTexture2D* TongueC = Tex(TEXT("T_Fulo_Tongue_Cavity"), false, true);
	UTexture2D* LeatherN = Tex(TEXT("T_Fulo_Leather_N"), true, false);
	UTexture2D* LeatherR = Tex(TEXT("T_Fulo_Leather_R"), false, true);
	UTexture2D* MetalN = Tex(TEXT("T_Fulo_Metal_N"), true, false);
	UTexture2D* MetalR = Tex(TEXT("T_Fulo_Metal_R"), false, true);
	for (const TCHAR* Fin : { TEXT("Black"), TEXT("Red"), TEXT("Brown"), TEXT("Blue"), TEXT("Purple"), TEXT("GreenFlames"), TEXT("StrikeClass") })
		Tex(*FString::Printf(TEXT("T_Fulo_Fin_%s"), Fin), false, false);
	UTexture2D* FinRed = LoadObject<UTexture2D>(nullptr, *(TexPkg / TEXT("T_Fulo_Fin_Red.T_Fulo_Fin_Red")));

	// --- master surface
	UMaterial* Surf = NewMaterial(TEXT("M_FuloSurface"));
	if (!Surf) return false;
	{
		UMaterial* M = Surf;
		M->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
		M->SetShadingModel(MSM_Subsurface);
		M->TwoSided = true;   // the wing and fin membranes are single sheets
		auto Sc = [&](const TCHAR* N, float V, int32 Y) { auto* E = Expr<UMaterialExpressionScalarParameter>(M, -900, Y); E->ParameterName = N; E->DefaultValue = V; return E; };
		auto Ve = [&](const TCHAR* N, FLinearColor V, int32 Y) { auto* E = Expr<UMaterialExpressionVectorParameter>(M, -900, Y); E->ParameterName = N; E->DefaultValue = V; return E; };
		UMaterialExpressionTextureCoordinate* UV = Expr<UMaterialExpressionTextureCoordinate>(M, -1300, 0);
		auto* UVS = Sc(TEXT("UVScale"), 1.f, 60);
		UMaterialExpressionMultiply* UVm = Expr<UMaterialExpressionMultiply>(M, -1100, 0);
		UMaterialEditingLibrary::ConnectMaterialExpressions(UV, TEXT(""), UVm, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UVS, TEXT(""), UVm, TEXT("B"));
		auto* D = TexParam(M, TEXT("BaseColorTex"), BodyD, SAMPLERTYPE_Color, -700, -300);
		auto* N = TexParam(M, TEXT("NormalTex"), BodyN, SAMPLERTYPE_Normal, -700, 0);
		auto* R = TexParam(M, TEXT("RoughTex"), BodyC, SAMPLERTYPE_Masks, -700, 300);   // linear maps import as masks
		auto* G = TexParam(M, TEXT("GlowTex"), BodyG, SAMPLERTYPE_Color, -700, 600);
		// detail maps (normal, roughness) tile; the colour, cavity-free parts don't
		UMaterialEditingLibrary::ConnectMaterialExpressions(UVm, TEXT(""), N, TEXT("UVs"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UVm, TEXT(""), R, TEXT("UVs"));
		UMaterialExpressionCustom* C = Expr<UMaterialExpressionCustom>(M, -300, 0);
		C->Description = TEXT("Fulo surface");
		C->OutputType = CMOT_Float3;
		C->Inputs.Reset();
		const TCHAR* Ins[] = { TEXT("Dt"), TEXT("Nt"), TEXT("Rt"), TEXT("Gt"), TEXT("UseTex"), TEXT("Tint"), TEXT("NStr"), TEXT("RA"), TEXT("RB"), TEXT("RLo"), TEXT("RHi"), TEXT("RMin"), TEXT("SssCol"), TEXT("Sheen"), TEXT("GlowAmt"), TEXT("GlowCol"), TEXT("Sat") };
		for (const TCHAR* In : Ins) { FCustomInput I; I.InputName = In; C->Inputs.Add(I); }
		C->AdditionalOutputs.Reset();
		for (const TCHAR* O : { TEXT("NormalOut"), TEXT("SssOut"), TEXT("EmisOut") }) { FCustomOutput Ou; Ou.OutputName = O; Ou.OutputType = CMOT_Float3; C->AdditionalOutputs.Add(Ou); }
		{ FCustomOutput Ou; Ou.OutputName = TEXT("RoughOut"); Ou.OutputType = CMOT_Float1; C->AdditionalOutputs.Add(Ou); }
		C->Code = TEXT(
			"float3 col = lerp(Tint, Dt.rgb * Tint, UseTex);\n"
			"float l = dot(col, float3(0.3, 0.59, 0.11)); col = lerp(float3(l, l, l), col, Sat);\n"
			// Blender ColorRamp: value RLo at RA, RHi at RB, linear between
			"float r = lerp(RLo, RHi, saturate((Rt.r - RA) / max(RB - RA, 1e-4)));\n"
			"RoughOut = clamp(r, RMin, 1.0);\n"
			"float3 n = Nt.xyz; n.xy *= NStr; NormalOut = normalize(n);\n"
			"float3 V = normalize(Parameters.CameraVector);\n"
			"float f = pow(1.0 - saturate(dot(Parameters.WorldNormal, V)), 4.0) * Sheen;\n"
			"col = lerp(col, col * 0.7 + float3(0.06, 0.07, 0.14), f);\n"   // the cool, violet-blue sheen of the scales at grazing angles
			"SssOut = col * SssCol;\n"
			"EmisOut = Gt.rgb * GlowCol * GlowAmt;\n"
			"return col;\n");
		C->RebuildOutputs();
		C->PostEditChange();
		UMaterialEditingLibrary::ConnectMaterialExpressions(D, TEXT(""), C, TEXT("Dt"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(N, TEXT(""), C, TEXT("Nt"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(R, TEXT(""), C, TEXT("Rt"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(G, TEXT(""), C, TEXT("Gt"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("UseBaseTex"), 1.f, 900), TEXT(""), C, TEXT("UseTex"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Ve(TEXT("Tint"), FLinearColor::White, 960), TEXT(""), C, TEXT("Tint"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("NormalStrength"), 1.f, 1020), TEXT(""), C, TEXT("NStr"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("RoughA"), 0.207f, 1080), TEXT(""), C, TEXT("RA"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("RoughB"), 0.44f, 1140), TEXT(""), C, TEXT("RB"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("RoughAtA"), 1.f, 1200), TEXT(""), C, TEXT("RLo"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("RoughAtB"), 0.f, 1260), TEXT(""), C, TEXT("RHi"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("RoughMin"), 0.3f, 1320), TEXT(""), C, TEXT("RMin"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Ve(TEXT("SubsurfaceTint"), FLinearColor(0.6f, 0.25f, 0.2f), 1380), TEXT(""), C, TEXT("SssCol"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("Sheen"), 0.2f, 1440), TEXT(""), C, TEXT("Sheen"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("GlowAmount"), 0.f, 1500), TEXT(""), C, TEXT("GlowAmt"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Ve(TEXT("GlowColor"), FLinearColor(0.2f, 0.6f, 4.f), 1560), TEXT(""), C, TEXT("GlowCol"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sc(TEXT("Saturation"), 1.f, 1620), TEXT(""), C, TEXT("Sat"));
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("NormalOut"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("RoughOut"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("SssOut"), MP_SubsurfaceColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT("EmisOut"), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Sc(TEXT("Metallic"), 0.f, 1700), TEXT(""), MP_Metallic);
		UMaterialEditingLibrary::ConnectMaterialProperty(Sc(TEXT("Specular"), 0.5f, 1760), TEXT(""), MP_Specular);
		UMaterialEditingLibrary::ConnectMaterialProperty(Sc(TEXT("SubsurfaceAmount"), 0.f, 1820), TEXT(""), MP_Opacity);
		Finish(M);
	}

	// --- the mesh
	// (Interchange files it under SK_DragonFulo/SkeletalMeshes/)
	const FString MeshPath = Dest / TEXT("SK_DragonFulo/SkeletalMeshes/SK_DragonFulo.SK_DragonFulo");
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (FParse::Param(FCommandLine::Get(), TEXT("ReimportFulo")) && Mesh)
	{
		UEditorAssetLibrary::DeleteDirectory(Dest / TEXT("SK_DragonFulo"));
		UEditorAssetLibrary::DeleteAsset(Dest / TEXT("SK_DragonFulo"));
		Mesh = nullptr;
	}
	if (!Mesh)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UAutomatedAssetImportData* Data = NewObject<UAutomatedAssetImportData>();
		Data->Filenames.Add(Glb);
		Data->DestinationPath = Dest;
		Data->bReplaceExisting = true;
		for (UObject* O : AssetTools.ImportAssetsAutomated(Data))
		{
			if (!O) continue;
			Log(FString::Printf(TEXT("  imported %s (%s)"), *O->GetPathName(), *O->GetClass()->GetName()));
			if (USkeletalMesh* SKM = Cast<USkeletalMesh>(O)) Mesh = SKM;
		}
		UEditorAssetLibrary::SaveDirectory(Dest, false, true);
	}
	if (!Mesh || !Mesh->GetSkeleton()) { Log(TEXT("! Fulo dragon import produced no skinned mesh")); return false; }
	Log(FString::Printf(TEXT("dragon-fulo: %d bones, %d material slots"), Mesh->GetRefSkeleton().GetNum(), Mesh->GetMaterials().Num()));
	// The rig is evaluated in code; a 700-body physics asset only costs time.
	Mesh->SetPhysicsAsset(nullptr);

	// --- one instance per Blender material, set as the model's own were
	struct FSlotDef { const TCHAR* Slot; UTexture2D* D; UTexture2D* N; UTexture2D* R; float UseTex; FLinearColor Tint; float NStr, RA, RB, RLo, RHi, RMin, UV, Metal, Spec, Sss, Sheen; };
	const FLinearColor W = FLinearColor::White;
	const FSlotDef Defs[] = {
		// slot           colour  normal   rough    useTex tint                                 nstr  rA     rB     r@A  r@B  rMin  uv   metal spec  sss   sheen
		// (the model's Blender look made most of the hide glossy, which out under a real sky turned
		// the black scales lavender: a matte hide, a little sheen left in the scale caps)
		// (A/B test in the game: the lavender cast was specular and metallic sky reflection, not the colour)
		{ TEXT("Body"),     BodyD,  BodyN,   BodyC,   1.f, FLinearColor(0.65f, 0.65f, 0.65f),    1.0f, 0.207f, 0.44f, 0.85f, 0.55f, 0.55f, 1.f, 0.0f, 0.22f, 0.25f, 0.08f },
		{ TEXT("Claws"),    BodyD,  BodyN,   BodyC,   1.f, FLinearColor(1.2f, 1.15f, 1.1f),      1.5f, 0.207f, 0.44f, 0.8f, 0.45f, 0.45f, 1.f, 0.05f, 0.4f, 0.10f, 0.05f },
		{ TEXT("Fin"),      FinRed, LeatherN, LeatherR, 1.f, W,                                   0.5f, 0.436f, 0.931f, 1.f, 0.f, 0.35f, 3.f, 0.0f, 0.33f, 0.20f, 0.05f },
		{ TEXT("Main.001"), nullptr, LeatherN, LeatherR, 0.f, FLinearColor(0.005f, 0.002f, 0.001f), 0.65f, 0.236f, 0.662f, 0.f, 1.f, 0.35f, 4.f, 0.f, 0.5f, 0.f, 0.f },
		{ TEXT("Main.002"), nullptr, LeatherN, LeatherR, 0.f, FLinearColor(0.004f, 0.003f, 0.002f), 1.0f, 0.236f, 0.662f, 0.f, 1.f, 0.35f, 8.f, 0.f, 0.5f, 0.f, 0.f },
		{ TEXT("Main.005"), nullptr, LeatherN, LeatherR, 0.f, FLinearColor(0.015f, 0.01f, 0.008f), 0.65f, 0.236f, 0.662f, 0.f, 1.f, 0.35f, 1.f, 0.f, 0.5f, 0.f, 0.f },
		{ TEXT("Lining.002"), nullptr, LeatherN, LeatherR, 0.f, FLinearColor(0.01f, 0.006f, 0.004f), 0.6f, 0.236f, 0.662f, 0.f, 1.f, 0.35f, 3.f, 0.f, 0.5f, 0.f, 0.f },
		{ TEXT("Metal"),    nullptr, MetalN,  MetalR,  0.f, FLinearColor(0.109f, 0.109f, 0.109f), 0.3f, 0.665f, 0.931f, 1.f, 0.f, 0.25f, 1.f, 0.6f, 0.5f, 0.f, 0.f },
		{ TEXT("Metal2"),   nullptr, MetalN,  MetalR,  0.f, FLinearColor(0.031f, 0.031f, 0.031f), 1.0f, 0.665f, 0.931f, 1.f, 0.f, 0.25f, 2.f, 0.78f, 0.5f, 0.f, 0.f },
		{ TEXT("Material.016"), nullptr, MetalN, MetalR, 0.f, FLinearColor(0.179f, 0.179f, 0.179f), 0.3f, 0.665f, 0.931f, 1.f, 0.f, 0.25f, 1.f, 0.6f, 0.5f, 0.f, 0.f },
		{ TEXT("Mouth"),    MouthD, MouthN,  MouthC,  1.f, W,                                     1.0f, 0.2f, 0.6f, 0.9f, 0.25f, 0.2f, 1.f, 0.f, 0.5f, 0.6f, 0.f },
		{ TEXT("Teeth"),    TeethD, MouthN,  MouthC,  1.f, W,                                     0.2f, 0.2f, 0.6f, 0.5f, 0.5f, 0.35f, 1.f, 0.f, 0.5f, 0.3f, 0.f },
		{ TEXT("Tongue"),   nullptr, TongueN, TongueC, 0.f, FLinearColor(0.435f, 0.195f, 0.212f), 1.0f, 0.2f, 0.6f, 0.8f, 0.3f, 0.25f, 1.f, 0.f, 0.5f, 0.6f, 0.f },
		{ TEXT("Eyes"),     EyeD,   nullptr, nullptr, 1.f, FLinearColor(0.82f, 0.9f, 0.62f),     0.0f, 0.f, 1.f, 0.16f, 0.16f, 0.14f, 1.f, 0.f, 0.45f, 0.f, 0.f },   // moist, softly glossy, lime-green (no red subsurface: it turned them cream; a mirror finish flashed the sun on every bob)
		{ TEXT("EarBumps"), BodyD,  BodyN,   BodyC,   0.f, FLinearColor(0.012f, 0.012f, 0.014f), 0.3f, 0.207f, 0.44f, 1.f, 0.f, 0.6f, 1.f, 0.f, 0.4f, 0.1f, 0.1f },
	};
	int32 Assigned = 0;
	for (FSkeletalMaterial& SM : Mesh->GetMaterials())
	{
		const FString Slot = SM.MaterialSlotName.ToString();
		const FSlotDef* Def = nullptr;
		for (const FSlotDef& Dd : Defs) if (Slot.Equals(FString(Dd.Slot).Replace(TEXT("."), TEXT("_")), ESearchCase::IgnoreCase)) { Def = &Dd; break; }
		if (!Def) { Log(FString::Printf(TEXT("! no material for slot %s"), *Slot)); continue; }
		const FString MiName = FString::Printf(TEXT("MI_Fulo_%s"), *FString(Def->Slot).Replace(TEXT("."), TEXT("_")));
		if (UEditorAssetLibrary::DoesAssetExist(Dest / MiName)) UEditorAssetLibrary::DeleteAsset(Dest / MiName);
		UMaterialInstanceConstant* MI = CreateOrLoad<UMaterialInstanceConstant, UMaterialInstanceConstantFactoryNew>(Dest, MiName,
			[Surf](UMaterialInstanceConstantFactoryNew* F) { F->InitialParent = Surf; });
		if (!MI) continue;
		MI->SetParentEditorOnly(Surf);
		auto SetT = [&](const TCHAR* P, UTexture2D* T) { if (T) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, P, T); };
		auto SetS = [&](const TCHAR* P, float V) { UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, P, V); };
		SetT(TEXT("BaseColorTex"), Def->D); SetT(TEXT("NormalTex"), Def->N); SetT(TEXT("RoughTex"), Def->R);
		if (!Def->N) SetS(TEXT("NormalStrength"), 0.f); else SetS(TEXT("NormalStrength"), Def->NStr);
		SetS(TEXT("UseBaseTex"), Def->UseTex);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI, TEXT("Tint"), Def->Tint);
		SetS(TEXT("RoughA"), Def->RA); SetS(TEXT("RoughB"), Def->RB); SetS(TEXT("RoughAtA"), Def->RLo); SetS(TEXT("RoughAtB"), Def->RHi);
		SetS(TEXT("RoughMin"), Def->RMin); SetS(TEXT("UVScale"), Def->UV); SetS(TEXT("Metallic"), Def->Metal); SetS(TEXT("Specular"), Def->Spec);
		SetS(TEXT("SubsurfaceAmount"), Def->Sss); SetS(TEXT("Sheen"), Def->Sheen);
		// the colour maps are blue-black; under sun and sky that read purple: mostly neutral
		if (FString(Def->Slot) == TEXT("Eyes")) SetS(TEXT("Saturation"), 1.35f);
		if (FString(Def->Slot) == TEXT("Body") || FString(Def->Slot) == TEXT("Claws") || FString(Def->Slot) == TEXT("EarBumps")) SetS(TEXT("Saturation"), 0.4f);
		if (!Def->R) { SetS(TEXT("RoughA"), 0.f); SetS(TEXT("RoughB"), 1.f); }
		UMaterialEditingLibrary::UpdateMaterialInstance(MI);
		UEditorAssetLibrary::SaveLoadedAsset(MI, false);
		SM.MaterialInterface = MI;
		++Assigned;
		Log(FString::Printf(TEXT("  slot %s -> %s"), *Slot, *MiName));
	}
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(Mesh, false);
	Log(FString::Printf(TEXT("dragon-fulo: %d slots dressed"), Assigned));
	return Assigned > 0;
}
