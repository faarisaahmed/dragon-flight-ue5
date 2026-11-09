// HLSL for the archipelago ground. Pasted into a Custom material node by
// DragonArchipelagoBuilder.cpp. Everything the web game's terrainmat.js did
// (rock on steep ground, scree, sand and shingle at the water, turf, heath,
// forest floor under the woods, mud by the lakes and on trodden pads, snow
// above the snow line and on the glaciers), redone with 4K scanned PBR
// textures, triplanar cliffs, distance-blended tiling and a far-field layer.
//
// Inputs (see the builder): P (abs world pos, cm), Nv (vertex normal, world), Wet (MPC_Weather, 0..1),
// Cam (camera pos, cm), WS (world size, m), SnowLine (m), and texture objects
// <Layer>D / <Layer>N / <Layer>A for Cliff Scree Grass Heath Forest Sand
// Shingle Snow Mud, plus FarRockD FarGrassD FarSnowD, MaskA, MaskB, MaskC (biomes), FlowMask.
// Outputs: return = base colour; NormalWS, Rough, AmbOcc; GrassW, HerbW (grass-map weights).
static const TCHAR* GLandscapeHLSL = TEXT(R"HLSL(
float3 pm = P * 0.01;
float3 N = normalize(Nv);
// Screen-space rates of change of the world position, taken here in uniform control flow.
// Textures sampled inside the [branch] blocks below use them explicitly: derivatives inside a
// branch whose condition varies per pixel are undefined, and on Metal that picked the smallest
// mips - sand, shingle and scree showed as blocky checkerboards of a few big texels.
float3 GX = ddx(pm), GY = ddy(pm);
#define SG(T, uv, gx, gy) Texture2DSampleGrad(T, T##Sampler, uv, gx, gy)
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
// Bedrock breaking through the turf, as all over the Faroes and western Norway: patches of
// bare rock a few to tens of metres across, most on knolls, edges and slopes over ~15 deg.
{
	float rk = Texture2DSample(FarRockD, FarRockDSampler, pm.xy / 67.0 + 0.31).r;
	float rk2 = Texture2DSample(FarGrassD, FarGrassDSampler, pm.xy / 19.0 + 0.7).g;
	float outcrop = smoothstep(0.60, 0.74, rk * 0.75 + rk2 * 0.25 + nz2 * 0.18 + smoothstep(0.04, 0.40, slope) * 0.30 - 0.10);
	outcrop *= smoothstep(4.0, 12.0, h) * (1.0 - wCliff);
	wCliff = max(wCliff, outcrop * 0.92);
	wScree = max(wScree, smoothstep(0.55, 0.66, rk * 0.75 + rk2 * 0.25 + nz2 * 0.18 + slope * 0.3 - 0.12) * (1.0 - outcrop) * smoothstep(4.0, 12.0, h) * 0.6);
}
// Snow lying on in drifts and hollows through the arctic summer, below the snow line too.
wSnow = max(wSnow, cold * cold * smoothstep(0.62, 0.8, nz * 0.6 + nz2 * 0.45 + 0.3) * (1.0 - smoothstep(0.45, 0.7, slope)) * smoothstep(3.0, 15.0, h));
float beach   = (1.0 - smoothstep(2.5, 7.5, h + nz * 2.5)) * (1.0 - smoothstep(0.30, 0.55, slope));
float wSand   = beach * saturate(0.55 + nz2 * 1.2);
float wShingle= beach * (1.0 - saturate(0.55 + nz2 * 1.2));
float land    = saturate(1.0 - wCliff - wScree - beach);
float wForest = land * smoothstep(0.05, 0.55, mk.r);
float wGrass  = land * mk.g * (1.0 - wForest);
float wHeath  = land * saturate(1.0 - mk.g) * (1.0 - wForest);
// (round the villages only the worn ground goes to mud - paths and yards in the noise - the rest is turf)
float wMud    = saturate(mk.a * 0.9 + mk2.b * 0.35 * smoothstep(0.75, 0.95, mk2.b + nz2 * 0.35)) * (1.0 - wCliff) * (1.0 - beach);
// Island characters. Woodland: the conifer woods spread over the sheltered slopes.
wForest = max(wForest, land * bio.r * 0.85 * (1.0 - smoothstep(0.45, 0.75, slope)) * smoothstep(6.0, 30.0, h) * (1.0 - smoothstep(alpine * 0.8, alpine, h)));
// Moorland: heather and blanket bog take over from grass; peat in the flat wet hollows.
wHeath = lerp(wHeath, wHeath + wGrass * 0.8, bio.b); wGrass *= 1.0 - bio.b * 0.8;
wMud = max(wMud, bio.b * land * smoothstep(0.55, 0.9, flow + nz * 0.3) * (1.0 - smoothstep(0.08, 0.2, slope)) * 0.7);
// Volcanic: young lava fields (rough black rock) instead of much of the turf, no woods.
wForest *= 1.0 - bio.g;
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
#define TOPLAYER(L, S, W, TINT, RMUL, NS) [branch] if (W > 0.002) { \
	float2 u1 = pm.xy / (S); float2 u2 = pm.xy / ((S) * 4.3); \
	float2 g1x = GX.xy / (S), g1y = GY.xy / (S), g2x = g1x / 4.3, g2y = g1y / 4.3; \
	float3 c = lerp(SG(L##D, u1, g1x, g1y).rgb, SG(L##D, u2, g2x, g2y).rgb, fd); \
	float2 n2 = (lerp(SG(L##N, u1, g1x, g1y).xy, SG(L##N, u2, g2x, g2y).xy, fd) * 2.0 - 1.0) * (NS); \
	float3 a = lerp(SG(L##A, u1, g1x, g1y).rgb, SG(L##A, u2, g2x, g2y).rgb, fd); \
	float3 tn = UNPACK(n2); \
	float3 wn = normalize(float3(tn.xy + N.xy, abs(tn.z) * N.z)); \
	float k = W / wSum; C += c * (TINT) * k; Nw += wn * k; R += saturate(a.g * (RMUL)) * k; AO += a.r * k; }

// (the scan is a yellow-brown gravel: kept for its detail, coloured as grey stone - brightened
// as it was, it ringed every turf patch and outcrop with a yellow halo)
#define DESAT(k) lerp(float3(1, 1, 1), dot(c, float3(0.3, 0.59, 0.11)) / max(c, 0.02), k)
TOPLAYER(Scree,   6.0, wScree,   DESAT(0.9) * lerp(float3(0.50, 0.51, 0.52), float3(0.22, 0.21, 0.21) * (0.85 + 0.3 * nz2), bio.g), 1.0, 1.0)
// Summer turf on these islands is a deep, wet green, broken by patches of heather and
// bracken (browner) and greener flushes along the gullies; tundra browns in the cold north.
float patchy = saturate(0.5 + nz * 0.9);
// (muted: the old values read as a mown lawn) and broken by drifts of browner, tussocky grass
float3 turf = lerp(float3(0.12, 0.23, 0.07), float3(0.19, 0.28, 0.11), patchy) * lerp(1.0, 1.12, saturate(flow * 1.5));
turf = lerp(turf, float3(0.21, 0.21, 0.11), smoothstep(0.55, 0.9, nz2 * 0.55 + nz * 0.35 + 0.35) * 0.45 * (1.0 - saturate(flow * 2.0)));
// on the steep ledges and cliff tops the turf is wet, short and dark, not sunlit lawn
turf *= lerp(float3(1.0, 1.0, 1.0), float3(0.62, 0.72, 0.62), smoothstep(0.30, 0.62, slope));
// (volcanic isles: the woolly fringe-moss of Iceland's lava fields, yellow-green)
float rush = 0.78 + 0.34 * Texture2DSample(FarGrassD, FarGrassDSampler, pm.xy / 37.0 + 0.53).g;   // rushes and tussocks: darker patches
// The scans are dry and golden (grass) or orange (bracken): DESAT keeps their detail and lets
// the tint set the colour, so the turf is the wet green of a northern island.
TOPLAYER(Grass,   3.0, wGrass,   DESAT(0.85) * rush * lerp(lerp(turf, float3(0.36, 0.42, 0.32), cold), float3(0.22, 0.25, 0.19), bio.g) * (0.88 + 0.16 * nz), 1.0, 1.0)
// (moorland: heather, purple-brown, in drifts)
// (the heath scan is orange bracken: its colour is mostly replaced by the tint, keeping its detail)
// (summer heath on a wet northern island reads green from a distance: grass and bilberry with
// browner drifts of heather, not straw)
// (darker than it was: the old tint over the bright litter scan ringed every patch of turf on
// the cliffs with a yellow-olive halo)
TOPLAYER(Heath,   4.0, wHeath,   DESAT(0.9) * lerp(lerp(lerp(float3(0.12, 0.22, 0.07), float3(0.18, 0.15, 0.10), smoothstep(0.45, 0.75, nz2 + nz * 0.3)), float3(0.24, 0.28, 0.21), cold), lerp(float3(0.14, 0.22, 0.09), float3(0.19, 0.13, 0.14), smoothstep(0.5, 0.85, nz * 0.7 + nz2 * 0.5 + 0.3)), bio.b) * (0.85 + 0.15 * nz2), 1.0, 1.0)
TOPLAYER(Forest,  4.0, wForest,  DESAT(0.6) * float3(0.15, 0.19, 0.11), 1.0, 1.0)   // needle litter and moss in the shade of the pines
TOPLAYER(Sand,    5.0, wSand,    lerp(float3(0.42, 0.42, 0.44), float3(0.10, 0.10, 0.11), bio.g), 1.0, 1.0)   // black sand on the volcanic isles
TOPLAYER(Shingle, 4.0, wShingle, float3(0.62, 0.63, 0.65), 1.0, 1.0)
// (the scan is trampled snow: its footprints repeated every few metres over the ice caps, so
// its contrast and relief are pressed flat - wind-packed snow reads smooth from the air)
TOPLAYER(Snow,    9.0, wSnow,    (lerp(float3(0.78, 0.78, 0.8), c, 0.3) / max(c, 0.02)) * float3(1.22, 1.24, 1.28), 0.75, 0.3)
// (on the moors the "mud" is blanket bog: sphagnum and cotton-grass, a dark wet green, not khaki)
TOPLAYER(Mud,     3.0, wMud,     lerp(DESAT(0.75) * float3(0.24, 0.29, 0.14), DESAT(0.8) * lerp(float3(0.15, 0.25, 0.09), float3(0.22, 0.20, 0.12), smoothstep(0.5, 0.8, nz2 + 0.3)), bio.b), 0.55, 1.0)

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
		float2 xX = GX.zy / S, xY = GY.zy / S, yX = GX.xz / S, yY = GY.xz / S, zX = GX.xy / S, zY = GY.xy / S;
		c += ft * (SG(CliffD, ux, xX, xY).rgb * bw.x
		         + SG(CliffD, uy, yX, yY).rgb * bw.y
		         + SG(CliffD, uz, zX, zY).rgb * bw.z);
		a += ft * (SG(CliffA, ux, xX, xY).rgb * bw.x
		         + SG(CliffA, uy, yX, yY).rgb * bw.y
		         + SG(CliffA, uz, zX, zY).rgb * bw.z);
		float3 tX = UNPACK(SG(CliffN, ux, xX, xY).xy * 2.0 - 1.0);
		float3 tY = UNPACK(SG(CliffN, uy, yX, yY).xy * 2.0 - 1.0);
		float3 tZ = UNPACK(SG(CliffN, uz, zX, zY).xy * 2.0 - 1.0);
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
	// (Kept faint: strong, even bands read as a layer cake, not as rock.)
	float tone = lerp(0.90, 1.04, hsh) * (0.92 + 0.16 * saturate(0.5 + nz2));
	float ledge = (1.0 - smoothstep(0.0, 0.10, band)) * 0.35 * saturate(0.4 + nz);   // a shelf here and there
	float joint = smoothstep(0.90, 1.0, band) * 0.4 * saturate(0.3 + nz2);            // an occasional seam
	// Runoff streaks: noise stretched down the face along its horizontal tangent.
	float2 tg = normalize(float2(-N.y, N.x) + 1e-4);
	float along = dot(pm.xy, tg);
	float streak = SG(FarRockD, float2(along / 38.0, h / 900.0), float2(dot(GX.xy, tg) / 38.0, GX.z / 900.0), float2(dot(GY.xy, tg) / 38.0, GY.z / 900.0)).r;
	float stain = smoothstep(0.35, 0.75, streak);
	float grey = dot(c, float3(0.3, 0.59, 0.11));
	c = lerp(float3(grey, grey, grey), c, 0.35) * float3(0.80, 0.80, 0.84);
	// Crags: the aerial rock scan projected onto the face itself (along it and up it) at two
	// sizes, so a 100 m cliff breaks into buttresses, gullies and ledges in light and shade
	// instead of reading as one smooth grey sheet.
	{
		float2 fu = float2(along, pm.z);
		float2 gx = float2(dot(GX.xy, tg), GX.z), gy = float2(dot(GY.xy, tg), GY.z);
		float m1 = SG(FarRockD, fu / 48.0 + 0.17, gx / 48.0, gy / 48.0).r;
		float m2 = SG(FarRockD, fu / 13.0 + 0.61, gx / 13.0, gy / 13.0).r;
		float crag = m1 * 0.65 + m2 * 0.35;
		c *= lerp(0.55, 1.3, saturate((crag - 0.25) * 1.6));
		// relief to go with it: tilt the normal along the face with the crag pattern's slope
		wn += float3(tg * (crag - 0.5) * 0.6, 0.0);
	}
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
	float3 farG = SG(FarGrassD, pm.xy / 70.0, GX.xy / 70.0, GY.xy / 70.0).rgb * lerp(lerp(float3(0.36, 0.58, 0.28), float3(0.46, 0.52, 0.42), cold), float3(0.36, 0.42, 0.26), bio.b);
	float3 farR = SG(FarRockD, pm.xy / 60.0, GX.xy / 60.0, GY.xy / 60.0).rgb * 0.9;
	float3 farS = SG(FarSnowD, pm.xy / 80.0, GX.xy / 80.0, GY.xy / 80.0).rgb;
	float gG = (wGrass + wHeath + wForest) / wSum, gR = (wCliff + wScree + wSand + wShingle + wMud) / wSum, gS = wSnow / wSum;
	float3 farC = farG * gG + farR * gR + farS * gS;
	C = lerp(C, farC, ff);
}

// Volcanic isles: young lava under a thin, patchy moss - the ground darkens smoothly toward
// bare basalt with the large noise (no hard-edged patches), mossier in the hollows.
{
	float lava = bio.g * (1.0 - sn) * saturate(1.35 + nz * 0.4 + nz2 * 0.25 - flow * 0.7);
	C = lerp(C, float3(0.075, 0.07, 0.068) * (0.8 + 0.4 * nz2), saturate(lava * 1.15));
	R = lerp(R, 0.9, lava * 0.5);
}
// Woods from afar: past the distance the trees are drawn to, the canopy itself is the colour
// you see - a near-black conifer green, a little bluer with distance - not the forest floor.
{
	float canopy = (wForest / wSum) * smoothstep(1700.0, 2900.0, dist) * (1.0 - sn);
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
// Rain: the ground goes dark and shines; water stands in the flat hollows.
{
	float wetR = Wet * (1.0 - sn);
	C *= lerp(1.0, 0.62, wetR);
	R = lerp(R, R * 0.5, wetR);
	float pud = wetR * smoothstep(0.62, 0.8, nz * 0.4 + flow * 0.6 + 0.2) * (1.0 - smoothstep(0.03, 0.10, slope));
	C *= 1.0 - pud * 0.3; R = lerp(R, 0.06, pud); Nw = lerp(Nw, float3(0, 0, 1), pud * 0.8);
}
// The sea floor: kelp, wrack and dark wet rock, not pale sand showing through clear water.
float sub = smoothstep(0.2, -1.8, h + nz2 * 0.5);
C = lerp(C, float3(0.045, 0.052, 0.035) * (0.75 + 0.5 * nz2), sub * 0.85);
NormalWS = normalize(Nw + N * 0.001);
Rough = saturate(R);
AmbOcc = saturate(AO);
// Where the landscape grass system plants turf and herbs (read by the grass-map pass only).
float above = step(1.5, h) * (1.0 - splash);
float soft = saturate(1.0 - (wCliff + wScree + wSand + wShingle + wSnow) / wSum);
// (no lawn on young lava, only moss in its hollows; on the moors the heather, not grass)
float lavaG = bio.g * saturate(1.35 + nz * 0.4 + nz2 * 0.25 - flow * 0.7);
GrassW = saturate(soft * (0.75 + 0.25 * saturate(mk.g * 1.5)) * 1.2 - wForest / wSum * 0.4 + wet * 0.2) * (1.0 - sn) * above * (1.0 - 0.92 * lavaG) * (1.0 - 0.6 * bio.b);
HerbW  = saturate((wForest * 0.9 + wHeath * 0.35 + wGrass * 0.12) / wSum + wet * 0.25) * (1.0 - sn) * above * (1.0 - 0.8 * lavaG);
return C;
)HLSL");
