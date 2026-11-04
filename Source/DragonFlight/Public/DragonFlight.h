#pragma once

#include "CoreMinimal.h"

DRAGONFLIGHT_API DECLARE_LOG_CATEGORY_EXTERN(LogDragon, Log, All);

// The web game measured in metres with Y up. Unreal measures centimetres with
// Z up. Every conversion goes through these so there is exactly one mapping:
//   UE (X, Y, Z) = web (x, z, y) * 100
namespace DragonUnits
{
	constexpr double M = 100.0; // centimetres per metre

	inline FVector FromWeb(double X, double Y, double Z) { return FVector(X * M, Z * M, Y * M); }
	inline FVector FromWebXZ(double X, double Z, double Height = 0.0) { return FVector(X * M, Z * M, Height * M); }
}
