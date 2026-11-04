// HLSL for the archipelago ground. Pasted into a Custom material node by
// DragonArchipelagoBuilder.cpp. Everything the web game's terrainmat.js did
// (rock on steep ground, scree, sand and shingle at the water, turf, heath,
// forest floor under the woods, mud by the lakes and on trodden pads, snow
// above the snow line and on the glaciers), redone with 4K scanned PBR
// textures, triplanar cliffs, distance-blended tiling and a far-field layer.
//
// Inputs (see the builder): P (abs world pos, cm), Nv (vertex normal, world),
// Cam (camera pos, cm), WS (world size, m), SnowLine (m), and texture objects
// <Layer>D / <Layer>N / <Layer>A for Cliff Scree Grass Heath Forest Sand
// Shingle Snow Mud, plus FarRockD FarGrassD FarSnowD, MaskA, MaskB, FlowMask.
// Outputs: return = base colour; NormalWS, Rough, AO.
static const TCHAR* GLandscapeHLSL = TEXT(R"HLSL(
float3 pm = P * 0.01;
float3 N = normalize(Nv);
float2 muv = pm.xy / WS + 0.5;
float4 mk = Texture2DSample(MaskA, MaskASampler, muv);
float3 mk2 = Texture2DSample(MaskB, MaskBSampler, muv).rgb;
float flow = Texture2DSample(FlowMask, FlowMaskSampler, muv).r;   // drainage: gully floors, streams
float dist = length(P - Cam) * 0.01;
float h = pm.z;
float slope = saturate((1.0 - N.z) * 2.6);

// Large-scale breakup from the aerial scans, so no boundary is a contour line.
float nz  = Texture2DSample(FarGrassD, FarGrassDSampler, pm.xy / 900.0).g * 2.2 - 0.9;
float nz2 = Texture2DSample(FarRockD,  FarRockDSampler,  pm.xy / 230.0).r * 2.0 - 1.0;

// ---- Weights ---------------------------------------------------------------
float wCliff  = smoothstep(0.50, 0.78, slope + nz2 * 0.08);
float wScree  = saturate(smoothstep(0.30, 0.52, slope + nz2 * 0.06) + smoothstep(300.0, 390.0, h + nz * 40.0)) * (1.0 - wCliff);
float wSnow   = saturate(smoothstep(SnowLine - 40.0, SnowLine + 60.0, h + nz * 70.0) * (1.0 - smoothstep(0.55, 0.82, slope)) + mk.b * 1.2 + mk2.g);
float beach   = (1.0 - smoothstep(2.5, 7.5, h + nz * 2.5)) * (1.0 - smoothstep(0.30, 0.55, slope));
float wSand   = beach * saturate(0.55 + nz2 * 1.2);
float wShingle= beach * (1.0 - saturate(0.55 + nz2 * 1.2));
float land    = saturate(1.0 - wCliff - wScree - beach);
float wForest = land * smoothstep(0.05, 0.55, mk.r);
float wGrass  = land * mk.g * (1.0 - wForest);
float wHeath  = land * saturate(1.0 - mk.g) * (1.0 - wForest);
float wMud    = saturate(mk.a * 0.9 + mk2.b * 0.55) * (1.0 - wCliff) * (1.0 - beach);
float sn = saturate(wSnow);
wCliff *= 1.0 - sn * 0.55; wScree *= 1.0 - sn; wSand *= 1.0 - sn; wShingle *= 1.0 - sn;
wForest *= 1.0 - sn; wGrass *= 1.0 - sn; wHeath *= 1.0 - sn; wMud *= 1.0 - sn;
// Sharpen so layers meet in a band, not a smear.
#define SHARP(w) (w = w * w * (3.0 - 2.0 * w))
SHARP(wCliff); SHARP(wScree); SHARP(wSnow); SHARP(wSand); SHARP(wShingle); SHARP(wForest); SHARP(wGrass); SHARP(wHeath); SHARP(wMud);
float wSum = wCliff + wScree + wSnow + wSand + wShingle + wForest + wGrass + wHeath + wMud + 1e-4;

// ---- Sampling ----------------------------------------------------------------
float fd = smoothstep(25.0, 220.0, dist);   // near tile -> 4.3x tile with distance
float3 C = 0; float3 Nw = 0; float R = 0; float AO = 0;
#define UNPACK(n2) float3(n2, sqrt(saturate(1.0 - dot(n2, n2))))
// Top-projected layer, whiteout-blended onto the surface normal.
#define TOPLAYER(L, S, W, TINT, RMUL) [branch] if (W > 0.002) { \
	float2 u1 = pm.xy / (S); float2 u2 = pm.xy / ((S) * 4.3); \
	float3 c = lerp(Texture2DSample(L##D, L##DSampler, u1).rgb, Texture2DSample(L##D, L##DSampler, u2).rgb, fd); \
	float2 n2 = lerp(Texture2DSample(L##N, L##NSampler, u1).xy, Texture2DSample(L##N, L##NSampler, u2).xy, fd) * 2.0 - 1.0; \
	float3 a = lerp(Texture2DSample(L##A, L##ASampler, u1).rgb, Texture2DSample(L##A, L##ASampler, u2).rgb, fd); \
	float3 tn = UNPACK(n2); \
	float3 wn = normalize(float3(tn.xy + N.xy, abs(tn.z) * N.z)); \
	float k = W / wSum; C += c * (TINT) * k; Nw += wn * k; R += saturate(a.g * (RMUL)) * k; AO += a.r * k; }

TOPLAYER(Scree,   6.0, wScree,   float3(0.72, 0.72, 0.74), 1.0)
TOPLAYER(Grass,   3.0, wGrass,   float3(0.58, 0.74, 0.44) * (0.85 + 0.18 * nz), 1.0)
TOPLAYER(Heath,   4.0, wHeath,   float3(0.70, 0.70, 0.52) * (0.85 + 0.15 * nz2), 1.0)
TOPLAYER(Forest,  4.0, wForest,  float3(0.62, 0.62, 0.56), 1.0)
TOPLAYER(Sand,    5.0, wSand,    float3(0.42, 0.42, 0.44), 1.0)
TOPLAYER(Shingle, 4.0, wShingle, float3(0.62, 0.63, 0.65), 1.0)
TOPLAYER(Snow,    6.0, wSnow,    float3(1.0, 1.0, 1.02), 0.9)
TOPLAYER(Mud,     3.0, wMud,     float3(0.75, 0.72, 0.70), 0.55)

// Cliffs: triplanar, so a vertical face is not a smear of stretched pixels. On top of
// the scanned rock, the structure that makes a sea cliff read from a kilometre out:
// stacked lava-flow strata (bands a few metres thick, each its own tone, a ledge at
// each base), dark runoff streaks down the face, and a grey basalt cast.
[branch] if (wCliff > 0.002)
{
	float3 bw = pow(abs(N), 4.0); bw /= (bw.x + bw.y + bw.z);
	float3 c = 0, a = 0, wn = 0;
	[unroll] for (int t = 0; t < 2; ++t)
	{
		float S = t == 0 ? 14.0 : 63.0;
		float ft = t == 0 ? 1.0 - fd : fd;
		float2 ux = pm.zy / S, uy = pm.xz / S, uz = pm.xy / S;
		c += ft * (Texture2DSample(CliffD, CliffDSampler, ux).rgb * bw.x
		         + Texture2DSample(CliffD, CliffDSampler, uy).rgb * bw.y
		         + Texture2DSample(CliffD, CliffDSampler, uz).rgb * bw.z);
		a += ft * (Texture2DSample(CliffA, CliffASampler, ux).rgb * bw.x
		         + Texture2DSample(CliffA, CliffASampler, uy).rgb * bw.y
		         + Texture2DSample(CliffA, CliffASampler, uz).rgb * bw.z);
		float3 tX = UNPACK(Texture2DSample(CliffN, CliffNSampler, ux).xy * 2.0 - 1.0);
		float3 tY = UNPACK(Texture2DSample(CliffN, CliffNSampler, uy).xy * 2.0 - 1.0);
		float3 tZ = UNPACK(Texture2DSample(CliffN, CliffNSampler, uz).xy * 2.0 - 1.0);
		float3 nX = float3(tX.xy + N.zy, abs(tX.z) * N.x).zyx;
		float3 nY = float3(tY.xy + N.xz, abs(tY.z) * N.y).xzy;
		float3 nZ = float3(tZ.xy + N.xy, abs(tZ.z) * N.z);
		wn += ft * (nX * bw.x + nY * bw.y + nZ * bw.z);
	}
	// Strata: bands wander with the large noise so they never read as contour lines.
	float zw = h + nz2 * 7.0 + nz * 4.0;
	float bandH = 6.5;
	float band = frac(zw / bandH), bid = floor(zw / bandH);
	float hsh = frac(sin(bid * 12.9898 + 4.1) * 43758.5453);
	float tone = lerp(0.62, 1.08, hsh);
	float ledge = 1.0 - smoothstep(0.0, 0.14, band);          // the shelf at each band's base
	float joint = smoothstep(0.86, 1.0, band);                 // the dark seam under the shelf above
	// Runoff streaks: noise stretched down the face along its horizontal tangent.
	float2 tg = normalize(float2(-N.y, N.x) + 1e-4);
	float along = dot(pm.xy, tg);
	float streak = Texture2DSample(FarRockD, FarRockDSampler, float2(along / 38.0, h / 900.0)).r;
	float stain = smoothstep(0.35, 0.75, streak);
	float grey = dot(c, float3(0.3, 0.59, 0.11));
	c = lerp(float3(grey, grey, grey), c, 0.35) * float3(0.80, 0.80, 0.84);
	c *= tone * (1.0 - joint * 0.45) * (1.0 - stain * 0.35);
	// Shelves catch light and hold a little moss.
	wn = normalize(wn) + float3(0, 0, ledge * 0.85);
	c = lerp(c, c * float3(0.75, 0.95, 0.55) + float3(0.01, 0.02, 0.0), ledge * 0.55 * saturate(h / 20.0));
	float k = wCliff / wSum;
	C += c * k; Nw += normalize(wn) * k; R += saturate(a.g + joint * 0.1) * k; AO += a.r * (1.0 - joint * 0.35) * k;
}

// ---- Far field: aerial scans take over the albedo with distance ------------
float ff = smoothstep(900.0, 3500.0, dist) * 0.65;
[branch] if (ff > 0.001)
{
	float3 farG = Texture2DSample(FarGrassD, FarGrassDSampler, pm.xy / 70.0).rgb * float3(0.62, 0.76, 0.52);
	float3 farR = Texture2DSample(FarRockD,  FarRockDSampler,  pm.xy / 60.0).rgb * 0.9;
	float3 farS = Texture2DSample(FarSnowD,  FarSnowDSampler,  pm.xy / 80.0).rgb;
	float gG = (wGrass + wHeath + wForest) / wSum, gR = (wCliff + wScree + wSand + wShingle + wMud) / wSum, gS = wSnow / wSum;
	float3 farC = farG * gG + farR * gR + farS * gS;
	C = lerp(C, farC, ff);
}

// Water-fed ground: gully floors and stream lines darker, mossier and damper.
float wet = smoothstep(0.35, 0.85, flow) * (1.0 - sn) * (1.0 - wCliff / wSum * 0.6) * step(1.0, h);
C = lerp(C, C * float3(0.62, 0.72, 0.55), wet * 0.75);
R *= 1.0 - wet * 0.35;
// Splash zone: the bottom few metres of every shore are wet, dark and glossy.
float splash = (1.0 - smoothstep(0.5, 3.5, h + nz2 * 0.8)) * step(-2.0, h);
C *= 1.0 - splash * 0.45;
R = lerp(R, R * 0.35, splash);
NormalWS = normalize(Nw + N * 0.001);
Rough = saturate(R);
AmbOcc = saturate(AO);
return C;
)HLSL");
