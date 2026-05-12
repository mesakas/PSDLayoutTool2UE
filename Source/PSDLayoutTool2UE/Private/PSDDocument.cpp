#include "PSDDocument.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

namespace PSDLayoutTool2UE
{
class FPsdReader
{
public:
	FPsdReader() = default;

	explicit FPsdReader(TArray<uint8> InData)
		: Data(MoveTemp(InData))
	{
	}

	bool LoadFile(const FString& Filename)
	{
		Position = 0;
		return FFileHelper::LoadFileToArray(Data, *Filename, FILEREAD_AllowWrite);
	}

	int64 Tell() const
	{
		return Position;
	}

	int64 Size() const
	{
		return Data.Num();
	}

	bool IsAtEnd() const
	{
		return Position >= Data.Num();
	}

	void Seek(int64 NewPosition)
	{
		Position = FMath::Clamp<int64>(NewPosition, 0, Data.Num());
	}

	void Skip(int64 Count)
	{
		Seek(Position + Count);
	}

	bool CanRead(int64 Count) const
	{
		return Count >= 0 && Position + Count <= Data.Num();
	}

	uint8 ReadByte()
	{
		if (!CanRead(1))
		{
			Position = Data.Num();
			return 0;
		}

		return Data[Position++];
	}

	TArray<uint8> ReadBytes(int64 Count)
	{
		TArray<uint8> Result;
		if (Count <= 0)
		{
			return Result;
		}

		const int64 ReadCount = FMath::Min<int64>(Count, Data.Num() - Position);
		Result.Append(Data.GetData() + Position, ReadCount);
		Position += ReadCount;
		return Result;
	}

	FString ReadAscii(int32 Count)
	{
		TArray<uint8> Bytes = ReadBytes(Count);
		FString Result;
		Result.Reserve(Bytes.Num());
		for (uint8 Byte : Bytes)
		{
			Result.AppendChar(static_cast<TCHAR>(Byte));
		}

		return Result;
	}

	int16 ReadInt16()
	{
		return static_cast<int16>(ReadUInt16());
	}

	uint16 ReadUInt16()
	{
		if (!CanRead(2))
		{
			Position = Data.Num();
			return 0;
		}

		const uint16 Result = (static_cast<uint16>(Data[Position]) << 8) |
			static_cast<uint16>(Data[Position + 1]);
		Position += 2;
		return Result;
	}

	int32 ReadInt32()
	{
		return static_cast<int32>(ReadUInt32());
	}

	uint32 ReadUInt32()
	{
		if (!CanRead(4))
		{
			Position = Data.Num();
			return 0;
		}

		const uint32 Result =
			(static_cast<uint32>(Data[Position]) << 24) |
			(static_cast<uint32>(Data[Position + 1]) << 16) |
			(static_cast<uint32>(Data[Position + 2]) << 8) |
			static_cast<uint32>(Data[Position + 3]);
		Position += 4;
		return Result;
	}

	FString ReadPascalString()
	{
		const uint8 Length = ReadByte();
		TArray<uint8> Bytes = ReadBytes(Length);
		if ((Length % 2) == 0)
		{
			Skip(1);
		}

		FString Result;
		Result.Reserve(Bytes.Num());
		for (uint8 Byte : Bytes)
		{
			Result.AppendChar(static_cast<TCHAR>(Byte));
		}

		return Result;
	}

	FString ReadUtf16StringUntilNull()
	{
		FString Result;
		while (CanRead(2))
		{
			const uint16 CodeUnit = ReadUInt16();
			if (CodeUnit == 0)
			{
				break;
			}

			Result.AppendChar(static_cast<TCHAR>(CodeUnit));
		}

		return Result;
	}

	float ReadAsciiFloat()
	{
		while (CanRead(1))
		{
			const uint8 Byte = Data[Position];
			if (Byte != ' ' && Byte != '\t' && Byte != '\r' && Byte != '\n')
			{
				break;
			}

			++Position;
		}

		FString Token;
		while (CanRead(1))
		{
			const uint8 Byte = Data[Position];
			if (Byte == ' ' || Byte == '\t' || Byte == '\r' || Byte == '\n' || Byte == ']')
			{
				break;
			}

			Token.AppendChar(static_cast<TCHAR>(Byte));
			++Position;
		}

		return Token.IsEmpty() ? 0.0f : FCString::Atof(*Token);
	}

	bool SeekAscii(const ANSICHAR* Search)
	{
		const int32 SearchLength = FCStringAnsi::Strlen(Search);
		if (SearchLength <= 0)
		{
			return true;
		}

		while (Position + SearchLength <= Data.Num())
		{
			if (FMemory::Memcmp(Data.GetData() + Position, Search, SearchLength) == 0)
			{
				Position += SearchLength;
				return true;
			}

			++Position;
		}

		Position = Data.Num();
		return false;
	}

private:
	TArray<uint8> Data;
	int64 Position = 0;
};

namespace
{
static bool IsStartGroup(const FPsdLayer& Layer)
{
	return Layer.IsPixelDataIrrelevant();
}

static bool IsEndGroup(const FPsdLayer& Layer)
{
	return Layer.Name.Contains(TEXT("</Layer set>")) ||
		Layer.Name.Contains(TEXT("</Layer group>")) ||
		(Layer.Name == TEXT(" copy") && Layer.Rect.Height == 0);
}

static void DecodeRleRow(FPsdReader& Reader, TArray<uint8>& ImageData, int32 StartIndex, int32 Columns)
{
	int32 Output = 0;
	while (Output < Columns && !Reader.IsAtEnd())
	{
		const uint8 Header = Reader.ReadByte();
		if (Header < 128)
		{
			int32 Count = Header + 1;
			while (Count-- > 0 && Output < Columns && !Reader.IsAtEnd())
			{
				if (ImageData.IsValidIndex(StartIndex + Output))
				{
					ImageData[StartIndex + Output] = Reader.ReadByte();
				}
				else
				{
					Reader.ReadByte();
				}
				++Output;
			}
		}
		else if (Header > 128)
		{
			int32 Count = 257 - Header;
			const uint8 Value = Reader.ReadByte();
			while (Count-- > 0 && Output < Columns)
			{
				if (ImageData.IsValidIndex(StartIndex + Output))
				{
					ImageData[StartIndex + Output] = Value;
				}
				++Output;
			}
		}
	}
}

static uint8 ClampByteFromUnit(double Value)
{
	return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 255.0), 0, 255));
}

static FColor CMYKToRGB(uint8 C, uint8 M, uint8 Y, uint8 K)
{
	const double CUnit = 1.0 - (static_cast<double>(C) / 255.0);
	const double MUnit = 1.0 - (static_cast<double>(M) / 255.0);
	const double YUnit = 1.0 - (static_cast<double>(Y) / 255.0);
	const double KUnit = 1.0 - (static_cast<double>(K) / 255.0);

	return FColor(
		ClampByteFromUnit(1.0 - ((CUnit * (1.0 - KUnit)) + KUnit)),
		ClampByteFromUnit(1.0 - ((MUnit * (1.0 - KUnit)) + KUnit)),
		ClampByteFromUnit(1.0 - ((YUnit * (1.0 - KUnit)) + KUnit)),
		255);
}

static FColor XYZToRGB(double X, double Y, double Z)
{
	X /= 100.0;
	Y /= 100.0;
	Z /= 100.0;

	const double LinearR = (X * 3.2406) + (Y * -1.5372) + (Z * -0.4986);
	const double LinearG = (X * -0.9689) + (Y * 1.8758) + (Z * 0.0415);
	const double LinearB = (X * 0.0557) + (Y * -0.2040) + (Z * 1.0570);

	const auto ToSrgb = [](double Value)
	{
		return Value <= 0.0031308 ? 12.92 * Value : (1.055 * FMath::Pow(Value, 1.0 / 2.4)) - 0.055;
	};

	return FColor(ClampByteFromUnit(ToSrgb(LinearR)), ClampByteFromUnit(ToSrgb(LinearG)), ClampByteFromUnit(ToSrgb(LinearB)), 255);
}

static FColor LabToRGB(uint8 LByte, uint8 AByte, uint8 BByte)
{
	const double L = static_cast<double>(LByte) / 2.56;
	const double A = static_cast<double>(AByte) - 128.0;
	const double B = static_cast<double>(BByte) - 128.0;

	const double Y = (L + 16.0) / 116.0;
	const double X = (A / 500.0) + Y;
	const double Z = Y - (B / 200.0);

	const auto Pivot = [](double Value)
	{
		const double Cubed = FMath::Pow(Value, 3.0);
		return Cubed <= 0.008856 ? Value / 7.787 : Cubed;
	};

	return XYZToRGB(95.047 * Pivot(X), 100.0 * Pivot(Y), 108.883 * Pivot(Z));
}
}

const FPsdChannel* FPsdLayer::FindChannel(int16 Id) const
{
	const int32* Index = ChannelIndexById.Find(Id);
	return Index && Channels.IsValidIndex(*Index) ? &Channels[*Index] : nullptr;
}

FPsdChannel* FPsdLayer::FindChannel(int16 Id)
{
	const int32* Index = ChannelIndexById.Find(Id);
	return Index && Channels.IsValidIndex(*Index) ? &Channels[*Index] : nullptr;
}

bool FPsdDocument::Load(const FString& Filename, FString& OutError)
{
	FPsdReader Reader;
	if (!Reader.LoadFile(Filename))
	{
		OutError = FString::Printf(TEXT("Failed to read PSD file: %s"), *Filename);
		return false;
	}

	if (!LoadHeader(Reader, OutError) ||
		!LoadColorModeData(Reader, OutError) ||
		!LoadImageResources(Reader, OutError) ||
		!LoadLayerAndMaskInfo(Reader, OutError))
	{
		return false;
	}

	return true;
}

bool FPsdDocument::LoadHeader(FPsdReader& Reader, FString& OutError)
{
	if (Reader.ReadAscii(4) != TEXT("8BPS"))
	{
		OutError = TEXT("The selected file is not a PSD document.");
		return false;
	}

	const int16 Version = Reader.ReadInt16();
	if (Version != 1)
	{
		OutError = TEXT("Only PSD version 1 is supported.");
		return false;
	}

	Reader.Skip(6);
	Reader.ReadInt16(); // channel count for the flattened image
	Height = Reader.ReadInt32();
	Width = Reader.ReadInt32();
	Depth = Reader.ReadInt16();
	ColorMode = static_cast<EColorMode>(Reader.ReadInt16());

	return Width > 0 && Height > 0;
}

bool FPsdDocument::LoadColorModeData(FPsdReader& Reader, FString& OutError)
{
	const uint32 Length = Reader.ReadUInt32();
	ColorModeData = Reader.ReadBytes(Length);
	return ColorModeData.Num() == static_cast<int32>(Length);
}

bool FPsdDocument::LoadImageResources(FPsdReader& Reader, FString& OutError)
{
	const uint32 Length = Reader.ReadUInt32();
	if (!Reader.CanRead(Length))
	{
		OutError = TEXT("PSD image resource section is truncated.");
		return false;
	}

	Reader.Skip(Length);
	return true;
}

bool FPsdDocument::LoadLayerAndMaskInfo(FPsdReader& Reader, FString& OutError)
{
	const uint32 Length = Reader.ReadUInt32();
	if (Length == 0)
	{
		return true;
	}

	const int64 End = Reader.Tell() + Length;
	if (!LoadLayers(Reader, OutError))
	{
		return false;
	}

	if (Reader.Tell() + 4 <= End)
	{
		const uint32 GlobalMaskLength = Reader.ReadUInt32();
		Reader.Skip(GlobalMaskLength);
	}

	Reader.Seek(End);
	return true;
}

bool FPsdDocument::LoadLayers(FPsdReader& Reader, FString& OutError)
{
	const uint32 LayerInfoLength = Reader.ReadUInt32();
	if (LayerInfoLength == 0)
	{
		return true;
	}

	const int64 LayerInfoEnd = Reader.Tell() + LayerInfoLength;
	int16 LayerCount = Reader.ReadInt16();
	if (LayerCount < 0)
	{
		LayerCount = FMath::Abs(LayerCount);
	}

	Layers.Reset();
	for (int32 Index = 0; Index < LayerCount; ++Index)
	{
		TSharedPtr<FPsdLayer> Layer = MakeShared<FPsdLayer>();
		if (!LoadLayer(Reader, *Layer, OutError))
		{
			return false;
		}
		Layers.Add(Layer);
	}

	for (const TSharedPtr<FPsdLayer>& Layer : Layers)
	{
		for (FPsdChannel& Channel : Layer->Channels)
		{
			if (Channel.Id != -2 && !LoadChannelPixelData(Reader, *Layer, Channel, OutError))
			{
				return false;
			}
		}

		if (!LoadMaskPixelData(Reader, *Layer, OutError))
		{
			return false;
		}
	}

	if ((Reader.Tell() % 2) == 1)
	{
		Reader.Skip(1);
	}

	Reader.Seek(LayerInfoEnd);
	return true;
}

bool FPsdDocument::LoadLayer(FPsdReader& Reader, FPsdLayer& OutLayer, FString& OutError)
{
	const int32 Top = Reader.ReadInt32();
	const int32 Left = Reader.ReadInt32();
	const int32 Bottom = Reader.ReadInt32();
	const int32 Right = Reader.ReadInt32();
	OutLayer.Rect = { Left, Top, Right - Left, Bottom - Top };

	const uint16 ChannelCount = Reader.ReadUInt16();
	for (uint16 Index = 0; Index < ChannelCount; ++Index)
	{
		FPsdChannel Channel;
		Channel.Id = Reader.ReadInt16();
		Channel.Length = Reader.ReadInt32();
		OutLayer.ChannelIndexById.Add(Channel.Id, OutLayer.Channels.Num());
		OutLayer.Channels.Add(Channel);
	}

	const FString Signature = Reader.ReadAscii(4);
	if (Signature != TEXT("8BIM") && Signature != TEXT("8B64"))
	{
		OutError = TEXT("PSD layer blend mode signature is invalid.");
		return false;
	}

	Reader.Skip(4); // blend mode key
	OutLayer.Opacity = Reader.ReadByte();
	Reader.Skip(1); // clipping
	OutLayer.Flags = Reader.ReadByte();
	Reader.Skip(1); // filler

	const uint32 ExtraLength = Reader.ReadUInt32();
	const int64 ExtraStart = Reader.Tell();
	const int64 ExtraEnd = ExtraStart + ExtraLength;

	const uint32 MaskLength = Reader.ReadUInt32();
	if (MaskLength > 0)
	{
		const int64 MaskEnd = Reader.Tell() + MaskLength;
		const int32 MaskTop = Reader.ReadInt32();
		const int32 MaskLeft = Reader.ReadInt32();
		const int32 MaskBottom = Reader.ReadInt32();
		const int32 MaskRight = Reader.ReadInt32();
		OutLayer.Mask.Rect = { MaskLeft, MaskTop, MaskRight - MaskLeft, MaskBottom - MaskTop };
		OutLayer.Mask.DefaultColor = Reader.ReadByte();
		OutLayer.Mask.Flags = Reader.ReadByte();
		Reader.Seek(MaskEnd);
	}

	const int32 BlendingRangesLength = Reader.ReadInt32();
	Reader.Skip(BlendingRangesLength);

	const int64 NamePaddingStart = Reader.Tell();
	OutLayer.Name = Reader.ReadPascalString();
	const int64 NameRemainder = (Reader.Tell() - NamePaddingStart) % 4;
	if (NameRemainder > 0)
	{
		Reader.Skip(NameRemainder);
	}

	while (Reader.Tell() + 12 <= ExtraEnd)
	{
		const int64 BlockStart = Reader.Tell();
		const FString BlockSignature = Reader.ReadAscii(4);
		if (BlockSignature != TEXT("8BIM") && BlockSignature != TEXT("8B64"))
		{
			Reader.Seek(ExtraEnd);
			break;
		}

		const FString Key = Reader.ReadAscii(4);
		const uint32 DataLength = Reader.ReadUInt32();
		TArray<uint8> Data = Reader.ReadBytes(DataLength);

		if (Key == TEXT("lfx2") || Key == TEXT("lrFX"))
		{
			OutLayer.bHasEffects = true;
		}
		else if (Key == TEXT("TySh"))
		{
			ReadTextLayer(Data, OutLayer);
		}
		else if (Key == TEXT("luni"))
		{
			FPsdReader DataReader(MoveTemp(Data));
			DataReader.Skip(4);
			FString UnicodeName = DataReader.ReadUtf16StringUntilNull();
			UnicodeName.TrimEndInline();
			if (!UnicodeName.IsEmpty())
			{
				OutLayer.Name = UnicodeName;
			}
		}

		if (Reader.Tell() <= BlockStart)
		{
			Reader.Seek(ExtraEnd);
			break;
		}
	}

	Reader.Seek(ExtraEnd);
	return true;
}

bool FPsdDocument::LoadChannelPixelData(FPsdReader& Reader, FPsdLayer& Layer, FPsdChannel& Channel, FString& OutError)
{
	Channel.Data = Reader.ReadBytes(Channel.Length);
	FPsdReader DataReader(Channel.Data);
	Channel.Compression = static_cast<EImageCompression>(DataReader.ReadInt16());

	const int32 BytesPerSample = Depth == 16 ? 2 : 1;
	const int32 Columns = Layer.Rect.Width * BytesPerSample;
	Channel.ImageData.SetNumZeroed(FMath::Max(0, Layer.Rect.Height * Columns));

	if (Channel.Compression == EImageCompression::Raw)
	{
		TArray<uint8> Raw = DataReader.ReadBytes(Channel.ImageData.Num());
		FMemory::Memcpy(Channel.ImageData.GetData(), Raw.GetData(), FMath::Min(Channel.ImageData.Num(), Raw.Num()));
	}
	else if (Channel.Compression == EImageCompression::Rle)
	{
		for (int32 Row = 0; Row < Layer.Rect.Height; ++Row)
		{
			DataReader.ReadUInt16();
		}

		for (int32 Row = 0; Row < Layer.Rect.Height; ++Row)
		{
			DecodeRleRow(DataReader, Channel.ImageData, Row * Columns, Columns);
		}
	}

	return true;
}

bool FPsdDocument::LoadMaskPixelData(FPsdReader& Reader, FPsdLayer& Layer, FString& OutError)
{
	FPsdChannel* Channel = Layer.FindChannel(-2);
	if (!Channel || !Layer.Mask.Rect.IsValid())
	{
		return true;
	}

	Channel->Data = Reader.ReadBytes(Channel->Length);
	FPsdReader DataReader(Channel->Data);
	Channel->Compression = static_cast<EImageCompression>(DataReader.ReadInt16());

	const int32 BytesPerSample = Depth == 16 ? 2 : 1;
	const int32 Columns = Layer.Mask.Rect.Width * BytesPerSample;
	Channel->ImageData.SetNumZeroed(FMath::Max(0, Layer.Mask.Rect.Height * Columns));

	if (Channel->Compression == EImageCompression::Raw)
	{
		TArray<uint8> Raw = DataReader.ReadBytes(Channel->ImageData.Num());
		FMemory::Memcpy(Channel->ImageData.GetData(), Raw.GetData(), FMath::Min(Channel->ImageData.Num(), Raw.Num()));
	}
	else if (Channel->Compression == EImageCompression::Rle)
	{
		for (int32 Row = 0; Row < Layer.Mask.Rect.Height; ++Row)
		{
			DataReader.ReadUInt16();
		}

		for (int32 Row = 0; Row < Layer.Mask.Rect.Height; ++Row)
		{
			DecodeRleRow(DataReader, Channel->ImageData, Row * Columns, Columns);
		}
	}

	Layer.Mask.ImageData = Channel->ImageData;
	return true;
}

void FPsdDocument::ReadTextLayer(const TArray<uint8>& Data, FPsdLayer& Layer) const
{
	FPsdReader DataReader(Data);
	Layer.bIsTextLayer = true;

	if (DataReader.SeekAscii("/Text"))
	{
		DataReader.Skip(4);
		Layer.Text = DataReader.ReadUtf16StringUntilNull();
	}

	DataReader.Seek(0);
	if (DataReader.SeekAscii("/Justification "))
	{
		const uint8 Justification = DataReader.ReadByte();
		if (Justification == '1')
		{
			Layer.Justification = ETextJustification::Right;
		}
		else if (Justification == '2')
		{
			Layer.Justification = ETextJustification::Center;
		}
	}

	DataReader.Seek(0);
	if (DataReader.SeekAscii("/FontSize "))
	{
		Layer.FontSize = FMath::Max(1.0f, DataReader.ReadAsciiFloat());
	}

	DataReader.Seek(0);
	if (DataReader.SeekAscii("/FillColor") && DataReader.SeekAscii("/Values [ "))
	{
		const float Alpha = DataReader.ReadAsciiFloat();
		DataReader.ReadByte();
		const float Red = DataReader.ReadAsciiFloat();
		DataReader.ReadByte();
		const float Green = DataReader.ReadAsciiFloat();
		DataReader.ReadByte();
		const float Blue = DataReader.ReadAsciiFloat();
		Layer.FillColor = FLinearColor(Red, Green, Blue, Alpha);
	}

	DataReader.Seek(0);
	if (DataReader.SeekAscii("/FontSet ") && DataReader.SeekAscii("/Name"))
	{
		DataReader.Skip(4);
		Layer.FontName = DataReader.ReadUtf16StringUntilNull();
	}
}

uint8 FPsdDocument::SampleChannel(const FPsdLayer& Layer, int16 ChannelId, int32 PixelIndex, uint8 DefaultValue) const
{
	const FPsdChannel* Channel = Layer.FindChannel(ChannelId);
	if (!Channel || Channel->ImageData.Num() == 0)
	{
		return DefaultValue;
	}

	const int32 BytesPerSample = Depth == 16 ? 2 : 1;
	const int32 DataIndex = PixelIndex * BytesPerSample;
	return Channel->ImageData.IsValidIndex(DataIndex) ? Channel->ImageData[DataIndex] : DefaultValue;
}

uint8 FPsdDocument::SampleMask(const FPsdLayer& Layer, int32 X, int32 Y) const
{
	if (!Layer.Mask.Rect.IsValid() || Layer.Mask.ImageData.Num() == 0)
	{
		return 255;
	}

	int32 MaskX = X;
	int32 MaskY = Y;
	if (Layer.Mask.IsPositionRelative())
	{
		MaskX -= Layer.Mask.Rect.X;
		MaskY -= Layer.Mask.Rect.Y;
	}
	else
	{
		MaskX = X + Layer.Rect.X - Layer.Mask.Rect.X;
		MaskY = Y + Layer.Rect.Y - Layer.Mask.Rect.Y;
	}

	if (MaskX < 0 || MaskY < 0 || MaskX >= Layer.Mask.Rect.Width || MaskY >= Layer.Mask.Rect.Height)
	{
		return 255;
	}

	const int32 BytesPerSample = Depth == 16 ? 2 : 1;
	const int32 Index = ((MaskY * Layer.Mask.Rect.Width) + MaskX) * BytesPerSample;
	return Layer.Mask.ImageData.IsValidIndex(Index) ? Layer.Mask.ImageData[Index] : 255;
}

bool FPsdDocument::DecodeLayer(const FPsdLayer& Layer, FDecodedPsdLayer& OutImage, FString& OutError) const
{
	if (!Layer.Rect.IsValid())
	{
		OutError = TEXT("PSD layer has an empty rectangle.");
		return false;
	}

	OutImage.Width = Layer.Rect.Width;
	OutImage.Height = Layer.Rect.Height;
	OutImage.BGRA.SetNumZeroed(OutImage.Width * OutImage.Height * 4);

	for (int32 Y = 0; Y < OutImage.Height; ++Y)
	{
		for (int32 X = 0; X < OutImage.Width; ++X)
		{
			const int32 LayerPixel = (Y * OutImage.Width) + X;

			FColor Color = FColor::White;
			switch (ColorMode)
			{
			case EColorMode::Grayscale:
			case EColorMode::Duotone:
			{
				const uint8 Gray = SampleChannel(Layer, 0, LayerPixel, 255);
				Color = FColor(Gray, Gray, Gray, 255);
				break;
			}
			case EColorMode::Indexed:
			{
				const uint8 Index = SampleChannel(Layer, 0, LayerPixel, 0);
				if (ColorModeData.IsValidIndex(Index + 512))
				{
					Color = FColor(ColorModeData[Index], ColorModeData[Index + 256], ColorModeData[Index + 512], 255);
				}
				break;
			}
			case EColorMode::CMYK:
				Color = CMYKToRGB(
					SampleChannel(Layer, 0, LayerPixel, 0),
					SampleChannel(Layer, 1, LayerPixel, 0),
					SampleChannel(Layer, 2, LayerPixel, 0),
					SampleChannel(Layer, 3, LayerPixel, 0));
				break;
			case EColorMode::Multichannel:
				Color = CMYKToRGB(
					SampleChannel(Layer, 0, LayerPixel, 0),
					SampleChannel(Layer, 1, LayerPixel, 0),
					SampleChannel(Layer, 2, LayerPixel, 0),
					0);
				break;
			case EColorMode::Lab:
				Color = LabToRGB(
					SampleChannel(Layer, 0, LayerPixel, 0),
					SampleChannel(Layer, 1, LayerPixel, 128),
					SampleChannel(Layer, 2, LayerPixel, 128));
				break;
			case EColorMode::RGB:
			default:
				Color = FColor(
					SampleChannel(Layer, 0, LayerPixel, 255),
					SampleChannel(Layer, 1, LayerPixel, 255),
					SampleChannel(Layer, 2, LayerPixel, 255),
					255);
				break;
			}

			Color.A = SampleChannel(Layer, -1, LayerPixel, 255);
			Color.A = static_cast<uint8>((static_cast<int32>(Color.A) * SampleMask(Layer, X, Y)) / 255);
			Color.A = static_cast<uint8>((static_cast<int32>(Color.A) * Layer.Opacity) / 255);

			const int32 TexturePixel = (Y * OutImage.Width) + X;
			const int32 Dest = TexturePixel * 4;
			OutImage.BGRA[Dest] = Color.B;
			OutImage.BGRA[Dest + 1] = Color.G;
			OutImage.BGRA[Dest + 2] = Color.R;
			OutImage.BGRA[Dest + 3] = Color.A;
		}
	}

	return true;
}

TArray<TSharedPtr<FPsdLayer>> FPsdDocument::BuildLayerTree(const TArray<TSharedPtr<FPsdLayer>>& FlatLayers)
{
	TArray<TSharedPtr<FPsdLayer>> Tree;
	for (const TSharedPtr<FPsdLayer>& Layer : FlatLayers)
	{
		if (Layer.IsValid())
		{
			Layer->Children.Reset();
		}
	}

	TSharedPtr<FPsdLayer> CurrentGroupLayer;
	TArray<TSharedPtr<FPsdLayer>> PreviousLayers;

	for (int32 Index = FlatLayers.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<FPsdLayer>& Layer = FlatLayers[Index];
		if (!Layer.IsValid())
		{
			continue;
		}

		if (IsEndGroup(*Layer))
		{
			if (PreviousLayers.Num() > 0)
			{
				TSharedPtr<FPsdLayer> PreviousLayer = PreviousLayers.Pop(EAllowShrinking::No);
				if (CurrentGroupLayer.IsValid())
				{
					PreviousLayer->Children.Add(CurrentGroupLayer);
				}
				CurrentGroupLayer = PreviousLayer;
			}
			else if (CurrentGroupLayer.IsValid())
			{
				Tree.Add(CurrentGroupLayer);
				CurrentGroupLayer.Reset();
			}
		}
		else if (IsStartGroup(*Layer))
		{
			if (CurrentGroupLayer.IsValid())
			{
				PreviousLayers.Push(CurrentGroupLayer);
			}

			CurrentGroupLayer = Layer;
		}
		else if (Layer->Rect.Width != 0 && Layer->Rect.Height != 0)
		{
			if (CurrentGroupLayer.IsValid())
			{
				CurrentGroupLayer->Children.Add(Layer);
			}
			else
			{
				Tree.Add(Layer);
			}
		}
	}

	if (Tree.Num() == 0 && CurrentGroupLayer.IsValid() && CurrentGroupLayer->Children.Num() > 0)
	{
		Tree.Add(CurrentGroupLayer);
	}

	return Tree;
}
}
