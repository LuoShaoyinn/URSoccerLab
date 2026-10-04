#pragma once
#include "Scene/URSSceneConfig.h"
class UTexture2D;
namespace URSoccerLab
{
struct FFieldTextures
{
	UTexture2D* BaseColor = nullptr;
	UTexture2D* Normal = nullptr;
	UTexture2D* MetallicRoughness = nullptr;
	UTexture2D* Ao = nullptr;
	// Game thread only. Decodes external images and builds transient full mip chains.
	static bool Load(const FURSFieldVisualConfig& Visual, const FString& Directory, FFieldTextures& Out, FString& Error);
};
}
