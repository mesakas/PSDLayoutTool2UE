#pragma once

#include "CoreMinimal.h"

namespace PSDLayoutTool2UE
{
enum class EColorMode : int32
{
	Grayscale = 1,
	Indexed = 2,
	RGB = 3,
	CMYK = 4,
	Multichannel = 7,
	Duotone = 8,
	Lab = 9
};

enum class EImageCompression : int32
{
	Raw = 0,
	Rle = 1
};

enum class ETextJustification : uint8
{
	Left,
	Right,
	Center
};

struct FPsdRect
{
	int32 X = 0;
	int32 Y = 0;
	int32 Width = 0;
	int32 Height = 0;

	bool IsValid() const
	{
		return Width > 0 && Height > 0;
	}

	int32 GetRight() const
	{
		return X + Width;
	}

	int32 GetBottom() const
	{
		return Y + Height;
	}
};

struct FPsdChannel
{
	int16 Id = 0;
	int32 Length = 0;
	TArray<uint8> Data;
	TArray<uint8> ImageData;
	EImageCompression Compression = EImageCompression::Raw;
};

struct FPsdMask
{
	FPsdRect Rect;
	uint8 DefaultColor = 255;
	uint8 Flags = 0;
	TArray<uint8> ImageData;

	bool IsPositionRelative() const
	{
		return (Flags & 0x01) != 0;
	}
};

struct FPsdLayer
{
	FString Name;
	FPsdRect Rect;
	TArray<FPsdChannel> Channels;
	TMap<int16, int32> ChannelIndexById;
	FPsdMask Mask;
	uint8 Opacity = 255;
	uint8 Flags = 0;
	bool bHasEffects = false;

	bool bIsTextLayer = false;
	FString Text;
	float FontSize = 16.0f;
	FString FontName;
	ETextJustification Justification = ETextJustification::Left;
	FLinearColor FillColor = FLinearColor::White;

	TArray<TSharedPtr<FPsdLayer>> Children;

	bool IsVisible() const
	{
		return (Flags & 0x02) == 0;
	}

	bool IsPixelDataIrrelevant() const
	{
		return (Flags & 0x10) != 0;
	}

	const FPsdChannel* FindChannel(int16 Id) const;
	FPsdChannel* FindChannel(int16 Id);
};

struct FDecodedPsdLayer
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> BGRA;
};

class FPsdDocument
{
public:
	int32 Width = 0;
	int32 Height = 0;
	int32 Depth = 0;
	EColorMode ColorMode = EColorMode::RGB;
	TArray<uint8> ColorModeData;
	TArray<TSharedPtr<FPsdLayer>> Layers;

	bool Load(const FString& Filename, FString& OutError);
	bool DecodeLayer(const FPsdLayer& Layer, FDecodedPsdLayer& OutImage, FString& OutError) const;

	static TArray<TSharedPtr<FPsdLayer>> BuildLayerTree(const TArray<TSharedPtr<FPsdLayer>>& FlatLayers);

private:
	bool LoadHeader(class FPsdReader& Reader, FString& OutError);
	bool LoadColorModeData(class FPsdReader& Reader, FString& OutError);
	bool LoadImageResources(class FPsdReader& Reader, FString& OutError);
	bool LoadLayerAndMaskInfo(class FPsdReader& Reader, FString& OutError);
	bool LoadLayers(class FPsdReader& Reader, FString& OutError);
	bool LoadLayer(class FPsdReader& Reader, FPsdLayer& OutLayer, FString& OutError);
	bool LoadChannelPixelData(class FPsdReader& Reader, FPsdLayer& Layer, FPsdChannel& Channel, FString& OutError);
	bool LoadMaskPixelData(class FPsdReader& Reader, FPsdLayer& Layer, FString& OutError);
	void ReadTextLayer(const TArray<uint8>& Data, FPsdLayer& Layer) const;

	uint8 SampleChannel(const FPsdLayer& Layer, int16 ChannelId, int32 PixelIndex, uint8 DefaultValue) const;
	uint8 SampleMask(const FPsdLayer& Layer, int32 X, int32 Y) const;
};
}
