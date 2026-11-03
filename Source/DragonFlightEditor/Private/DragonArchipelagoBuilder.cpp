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
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
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
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "WaterBodyOceanActor.h"
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
		if (M) UMaterialEditingLibrary::DeleteAllMaterialExpressions(M);
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
		E->Texture = Default;
		E->SamplerType = Type;
		E->SamplerSource = SSM_Wrap_WorldGroupSettings; // shared samplers: lifts the 16-sampler limit
		return E;
	}

	UMaterialExpressionTextureSampleParameter2D* TexParam(UMaterial* M, const FString& Name, UTexture2D* Default, EMaterialSamplerType Type, int32 X, int32 Y)
	{
		UMaterialExpressionTextureSampleParameter2D* E = Expr<UMaterialExpressionTextureSampleParameter2D>(M, X, Y);
		E->ParameterName = FName(*Name);
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
	if (UTexture2D* Existing = LoadTex(Full + TEXT(".") + AssetName)) return Existing;

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
	AddIn(TEXT("MaskA")); AddIn(TEXT("MaskB"));
	for (const TCHAR* L : GroundLayers) { AddIn(*FString::Printf(TEXT("%sD"), L)); AddIn(*FString::Printf(TEXT("%sN"), L)); AddIn(*FString::Printf(TEXT("%sA"), L)); }
	for (const TCHAR* L : FarLayers) AddIn(*FString::Printf(TEXT("%sD"), L));
	{ FCustomOutput O; O.OutputName = TEXT("NormalWS"); O.OutputType = CMOT_Float3; Cu->AdditionalOutputs.Add(O); }
	{ FCustomOutput O; O.OutputName = TEXT("Rough"); O.OutputType = CMOT_Float1; Cu->AdditionalOutputs.Add(O); }
	{ FCustomOutput O; O.OutputName = TEXT("AmbOcc"); O.OutputType = CMOT_Float1; Cu->AdditionalOutputs.Add(O); }
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
	Finish(M);
	Log(TEXT("M_Archipelago built"));
	return M;
}

void FDragonArchipelagoBuilder::CreateMasterMaterials()
{
	// --- M_Surface: opaque PBR for rocks, bark, deadwood -----------------------
	if (UMaterial* M = NewMaterial(TEXT("M_Surface")))
	{
		M->bUsedWithInstancedStaticMeshes = true;
		M->bUsedWithNanite = true;
		M->bUsedWithSkeletalMesh = true;
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
		M->bUsedWithInstancedStaticMeshes = true;
		M->bUsedWithNanite = true;
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
		M->bUsedWithSkeletalMesh = true;
		M->TwoSided = true;
		M->SetShadingModel(MSM_TwoSidedFoliage);
		UMaterialExpressionTextureSampleParameter2D* D = TexParam(M, TEXT("BaseColor"), nullptr, SAMPLERTYPE_Color, -600, -200);
		UMaterialExpressionTextureSampleParameter2D* N = TexParam(M, TEXT("Normal"), nullptr, SAMPLERTYPE_Normal, -600, 50);
		UMaterialExpressionTextureSampleParameter2D* A = TexParam(M, TEXT("ARM"), nullptr, SAMPLERTYPE_Masks, -600, 300);
		UMaterialExpressionVectorParameter* Through = Expr<UMaterialExpressionVectorParameter>(M, -600, -400);
		Through->ParameterName = TEXT("Translucency"); Through->DefaultValue = FLinearColor(0.35f, 0.12f, 0.06f);
		UMaterialEditingLibrary::ConnectMaterialProperty(D, TEXT("RGB"), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(N, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT("G"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Through, TEXT(""), MP_SubsurfaceColor);
		Finish(M);
	}

	// --- M_Rain: streaks on a camera-following curtain ------------------------
	if (UMaterial* M = NewMaterial(TEXT("M_Rain")))
	{
		M->BlendMode = BLEND_Translucent;
		M->SetShadingModel(MSM_Unlit);
		M->TwoSided = true;
		M->bUsedWithStaticLighting = false;
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
		M->bUsedWithStaticLighting = false;
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
				// Nanite everything; foliage keeps its area as it simplifies.
				// VERIFY(5.5): direct NaniteSettings access may be deprecated in favour of
				// GetNaniteSettings()/SetNaniteSettings(); if it is private, copy, edit and call SetNaniteSettings.
				PRAGMA_DISABLE_DEPRECATION_WARNINGS
				Mesh->NaniteSettings.bEnabled = true;
				Mesh->NaniteSettings.bPreserveArea = Group == TEXT("trees") || Group == TEXT("ground");
				PRAGMA_ENABLE_DEPRECATION_WARNINGS
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
	const FString Fbx = Dir / TEXT("SK_Dragon.fbx");
	if (!FPaths::FileExists(Fbx)) { Log(TEXT("no Data/Dragon/SK_Dragon.fbx — the stand-in dragon will be used")); return false; }
	const FString Dest = TEXT("/Game/Dragon");
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Dragon/SK_Dragon.SK_Dragon"));
	if (!Mesh)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UAutomatedAssetImportData* Data = NewObject<UAutomatedAssetImportData>();
		Data->Filenames.Add(Fbx);
		Data->DestinationPath = Dest;
		Data->bReplaceExisting = true;
		AssetTools.ImportAssetsAutomated(Data);
		Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Dragon/SK_Dragon.SK_Dragon"));
	}
	if (!Mesh) { Log(TEXT("! dragon import produced no SK_Dragon")); return false; }

	UMaterial* Surface = LoadObject<UMaterial>(nullptr, *(MatRoot / TEXT("M_Surface.M_Surface")));
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
		// VERIFY(5.5): ALandscapeProxy::Import(Guid, MinX, MinY, MaxX, MaxY, NumSubsections, SubsectionSizeQuads,
		// HeightData, HeightmapFileName, LayerInfos, AlphamapType[, ImportLayers]) — check the trailing params.
		Landscape->Import(FGuid::NewGuid(), 0, 0, Res - 1, Res - 1, Sections, QuadsPerSection, HeightData, *File, LayerData, ELandscapeImportAlphamapType::Additive);
		if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo()) Info->UpdateLayerInfoMap(Landscape);
		Landscape->RegisterAllComponents();
		FFileHelper::SaveStringToFile(FString::FromInt(Res), *(UDragonWorldSubsystem::DataDir() / TEXT("LandscapeRes.txt")));
		Log(FString::Printf(TEXT("landscape %d x %d at %.2f m/quad"), Res, Res, XY / 100.0));
	}

	// --- Water -----------------------------------------------------------------
	if (Options.bWater)
	{
		AWaterZone* Zone = World->SpawnActor<AWaterZone>(FVector::ZeroVector, FRotator::ZeroRotator);
		Zone->SetActorLabel(TEXT("WaterZone"));
		// VERIFY(5.5): AWaterZone::SetZoneExtent(FVector2D) believed present since 5.1.
		Zone->SetZoneExtent(FVector2D(SizeCm * 1.6, SizeCm * 1.6));

		AWaterBodyOcean* Ocean = World->SpawnActor<AWaterBodyOcean>(FVector::ZeroVector, FRotator::ZeroRotator);
		Ocean->SetActorLabel(TEXT("Ocean"));
		if (UWaterSplineComponent* S = Ocean->GetWaterSpline())
		{
			// A square coast well outside the world: the ocean fills everything inside the zone.
			S->ClearSplinePoints(false);
			const double E = SizeCm * 0.75;
			S->AddSplinePoint(FVector(-E, -E, 0), ESplineCoordinateSpace::World, false);
			S->AddSplinePoint(FVector(E, -E, 0), ESplineCoordinateSpace::World, false);
			S->AddSplinePoint(FVector(E, E, 0), ESplineCoordinateSpace::World, false);
			S->AddSplinePoint(FVector(-E, E, 0), ESplineCoordinateSpace::World, false);
			S->SetClosedLoop(true, true);
		}
		if (UWaterBodyComponent* C = Ocean->GetWaterBodyComponent())
		{
			SetAffectsLandscape(C, false); // the terrain is already the shape it should be
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
