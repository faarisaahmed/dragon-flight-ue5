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
// Shingle Snow Mud, plus FarRockD FarGrassD FarSnowD, MaskA, MaskB, MaskC (biomes), FlowMask.
// Outputs: return = base colour; NormalWS, Rough, AmbOcc; GrassW, HerbW (grass-map weights).
static const TCHAR* GLandscapeHLSL = TEXT(R"HLSL(
float3 pm = P * 0.01;
float3 N = normalize(Nv);
float2 muv = pm.xy / WS + 0.5;
float4 mk = Texture2DSample(MaskA, MaskASampler, muv);
float3 mk2 = Texture2DSample(MaskB, MaskBSampler, muv).rgb;
float3 bio = Texture2DSample(MaskC, MaskCSampler, muv).rgb;     // R woodland  G volcanic  B moorland (biomes.py)
float flow = Texture2DSample(FlowMask, FlowMaskSampler, muv).r;   // drainage: gully floors, streams
float dist = length(P - Cam) * 0.01;
float h = pm.z;
float slope = saturate((1.0 - N.z) * 2.6);

// Large-scale breakup from the aerial scans, so no boundary is a contour line.
float nz  = Texture2DSample(FarGrassD, FarGrassDSampler, pm.xy / 900.0).g * 2.2 - 0.9;
float nz2 = Texture2DSample(FarRockD,  FarRockDSampler,  pm.xy / 230.0).r * 2.0 - 1.0;

// Climate (same as UDragonWorldSubsystem::ColdAt): the northern isles lie in the drift ice of a
// cold current, so the snow line falls from SnowLine to ~40 m toward the northern edge (-y).
float wob = 1800.0 * sin(pm.x / 5200.0 + 0.7) + 900.0 * sin(pm.x / 2100.0 + 2.1);
float cold = saturate((-pm.y - 9000.0 + wob) / 9000.0); cold = cold * cold * (3.0 - 2.0 * cold);
float snowL = lerp(SnowLine, 40.0, cold);
float alpine = min(300.0, snowL * 0.7);

// ---- Weights ---------------------------------------------------------------
// Turf holds on slopes up to ~40 deg (as on the Faroes); bare rock beyond ~48.
float wCliff  = smoothstep(0.62, 0.92, slope + nz2 * 0.08);
float wScree  = saturate(smoothstep(0.42, 0.64, slope + nz2 * 0.06) + smoothstep(alpine, alpine + 90.0, h + nz * 40.0)) * (1.0 - wCliff);
float wSnow   = saturate(smoothstep(snowL - 40.0, snowL + 60.0, h + nz * 70.0) * (1.0 - smoothstep(0.62, 0.9, slope)) + mk.b * 1.2 + mk2.g);
float beach   = (1.0 - smoothstep(2.5, 7.5, h + nz * 2.5)) * (1.0 - smoothstep(0.30, 0.55, slope));
float wSand   = beach * saturate(0.55 + nz2 * 1.2);
float wShingle= beach * (1.0 - saturate(0.55 + nz2 * 1.2));
float land    = saturate(1.0 - wCliff - wScree - beach);
float wForest = land * smoothstep(0.05, 0.55, mk.r);
float wGrass  = land * mk.g * (1.0 - wForest);
float wHeath  = land * saturate(1.0 - mk.g) * (1.0 - wForest);
float wMud    = saturate(mk.a * 0.9 + mk2.b * 0.55) * (1.0 - wCliff) * (1.0 - beach);
// Island characters. Woodland: the conifer woods spread over the sheltered slopes.
wForest = max(wForest, land * bio.r * 0.85 * (1.0 - smoothstep(0.45, 0.75, slope)) * smoothstep(6.0, 30.0, h) * (1.0 - smoothstep(alpine * 0.8, alpine, h)));
// Moorland: heather and blanket bog take over from grass; peat in the flat wet hollows.
wHeath = lerp(wHeath, wHeath + wGrass * 0.8, bio.b); wGrass *= 1.0 - bio.b * 0.8;
wMud = max(wMud, bio.b * land * smoothstep(0.35, 0.8, flow + nz * 0.3) * (1.0 - smoothstep(0.1, 0.3, slope)) * 0.8);
// Volcanic: young lava fields (rough black rock) instead of much of the turf, no woods.
wScree = max(wScree, bio.g * land * saturate(0.45 + nz * 0.5 + nz2 * 0.25 - mk.g * 0.25) * 0.8);
wForest *= 1.0 - bio.g; wGrass *= 1.0 - bio.g * 0.5;
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

TOPLAYER(Scree,   6.0, wScree,   lerp(float3(0.72, 0.72, 0.74), float3(0.22, 0.21, 0.21) * (0.85 + 0.3 * nz2), bio.g), 1.0)
// Summer turf on these islands is a deep, wet green, broken by patches of heather and
// bracken (browner) and greener flushes along the gullies; tundra browns in the cold north.
float patchy = saturate(0.5 + nz * 0.9);
float3 turf = lerp(float3(0.30, 0.62, 0.22), float3(0.42, 0.62, 0.27), patchy) * lerp(1.0, 1.12, saturate(flow * 1.5));
// (volcanic isles: the woolly fringe-moss of Iceland's lava fields, yellow-green)
TOPLAYER(Grass,   3.0, wGrass,   lerp(lerp(turf, float3(0.58, 0.56, 0.42), cold), float3(0.50, 0.58, 0.30), bio.g) * (0.88 + 0.16 * nz), 1.0)
// (moorland: heather, purple-brown, in drifts)
TOPLAYER(Heath,   4.0, wHeath,   lerp(lerp(lerp(float3(0.58, 0.60, 0.40), float3(0.62, 0.50, 0.46), saturate(nz2 + 0.3)), float3(0.62, 0.55, 0.46), cold), lerp(float3(0.50, 0.38, 0.40), float3(0.42, 0.40, 0.30), saturate(nz * 0.8 + 0.4)), bio.b) * (0.85 + 0.15 * nz2), 1.0)
TOPLAYER(Forest,  4.0, wForest,  float3(0.62, 0.62, 0.56), 1.0)
TOPLAYER(Sand,    5.0, wSand,    lerp(float3(0.42, 0.42, 0.44), float3(0.10, 0.10, 0.11), bio.g), 1.0)   // black sand on the volcanic isles
TOPLAYER(Shingle, 4.0, wShingle, float3(0.62, 0.63, 0.65), 1.0)
TOPLAYER(Snow,    6.0, wSnow,    float3(1.22, 1.24, 1.28), 0.75)
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
	c *= lerp(1.0, 0.5, bio.g);                                  // young basalt is near black
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
	float3 farG = Texture2DSample(FarGrassD, FarGrassDSampler, pm.xy / 70.0).rgb * float3(0.50, 0.78, 0.42);
	float3 farR = Texture2DSample(FarRockD,  FarRockDSampler,  pm.xy / 60.0).rgb * 0.9;
	float3 farS = Texture2DSample(FarSnowD,  FarSnowDSampler,  pm.xy / 80.0).rgb;
	float gG = (wGrass + wHeath + wForest) / wSum, gR = (wCliff + wScree + wSand + wShingle + wMud) / wSum, gS = wSnow / wSum;
	float3 farC = farG * gG + farR * gR + farS * gS;
	C = lerp(C, farC, ff);
}

// Woods from afar: past the distance the trees are drawn to, the canopy itself is the colour
// you see - a near-black conifer green, a little bluer with distance - not the forest floor.
{
	float canopy = (wForest / wSum) * smoothstep(1800.0, 4800.0, dist) * (1.0 - sn);
	C = lerp(C, float3(0.045, 0.07, 0.04) * (0.85 + 0.3 * nz), canopy * 0.9);
	R = lerp(R, 0.85, canopy);
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
// Where the landscape grass system plants turf and herbs (read by the grass-map pass only).
float above = step(1.5, h) * (1.0 - splash);
float soft = saturate(1.0 - (wCliff + wScree + wSand + wShingle + wSnow) / wSum);
GrassW = saturate(soft * (0.75 + 0.25 * saturate(mk.g * 1.5)) * 1.2 - wForest / wSum * 0.4 + wet * 0.2) * (1.0 - sn) * above;
HerbW  = saturate((wForest * 0.9 + wHeath * 0.35 + wGrass * 0.12) / wSum + wet * 0.25) * (1.0 - sn) * above;
return C;
)HLSL");
