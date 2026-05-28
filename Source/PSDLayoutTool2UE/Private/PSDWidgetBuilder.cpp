#include "PSDWidgetBuilder.h"

#include "PSDDocument.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "Factories/Factory.h"
#include "Fonts/SlateFontInfo.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Styling/SlateBrush.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"

namespace
{
using namespace PSDLayoutTool2UE;

enum class EButtonChildRole : uint8
{
	None,
	Default,
	Pressed,
	Highlighted,
	Disabled,
	TextImage
};

enum class EAnchorPreset : uint8
{
	None,
	Global,
	TopLeft,
	BottomLeft,
	TopRight,
	BottomRight,
	Center,
	LeftMiddle,
	RightMiddle,
	TopMiddle,
	BottomMiddle
};

struct FLayoutRect
{
	float X = 0.0f;
	float Y = 0.0f;
	float Width = 0.0f;
	float Height = 0.0f;

	float Right() const { return X + Width; }
	float Bottom() const { return Y + Height; }
	FVector2D Size() const { return FVector2D(Width, Height); }
	bool IsValid() const { return Width > 0.0f && Height > 0.0f; }
};

struct FLayerImportInfo
{
	const FPsdLayer* Layer = nullptr;
	const FPsdLayer* ParentLayer = nullptr;
	bool bEffectiveVisible = true;
	bool bFolderLike = false;
	bool bButtonGroup = false;
	bool bAnimationGroup = false;
	EButtonChildRole ButtonRole = EButtonChildRole::None;
	EAnchorPreset ExplicitAnchorPreset = EAnchorPreset::None;
	EAnchorPreset AnchorPreset = EAnchorPreset::None;
	FString UniqueSelfName;
	FString UniqueTextureName;
	FLayoutRect LayoutRect;
	bool bHasLayoutRect = false;
};

struct FUiLayoutContext
{
	FLayoutRect PsdReferenceRect;
	FVector2D LocalRectSize = FVector2D::ZeroVector;
	FLayoutRect LocalDisplayRect;
};

struct FBuildState
{
	FPsdDocument Document;
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	UWidgetTree* WidgetTree = nullptr;
	FPSDWidgetImportOptions ImportOptions;
	FString TextureRootPackagePath;
	TMap<const FPsdLayer*, FLayerImportInfo> LayerInfos;
	TMap<const FPsdLayer*, UTexture2D*> TextureCache;
	TArray<UObject*> AdditionalAssets;
	int32 ZOrder = 0;
};

static FLayoutRect ToLayoutRect(const FPsdRect& Rect)
{
	return FLayoutRect{
		static_cast<float>(Rect.X),
		static_cast<float>(Rect.Y),
		static_cast<float>(Rect.Width),
		static_cast<float>(Rect.Height)
	};
}

static bool ClipLayoutRectToCanvas(FLayoutRect& Rect, const FVector2D& CanvasSize)
{
	const float Left = FMath::Max(Rect.X, 0.0f);
	const float Top = FMath::Max(Rect.Y, 0.0f);
	const float Right = FMath::Min(Rect.Right(), static_cast<float>(CanvasSize.X));
	const float Bottom = FMath::Min(Rect.Bottom(), static_cast<float>(CanvasSize.Y));
	if (Right <= Left || Bottom <= Top)
	{
		return false;
	}

	Rect = FLayoutRect{ Left, Top, Right - Left, Bottom - Top };
	return true;
}

static bool ApplyImportBounds(FLayoutRect& Rect, const FPSDWidgetImportOptions& ImportOptions, const FVector2D& CanvasSize)
{
	return !ImportOptions.bClipLayersToCanvas || ClipLayoutRectToCanvas(Rect, CanvasSize);
}

static FLayoutRect CombineRects(const FLayoutRect& A, const FLayoutRect& B)
{
	const float XMin = FMath::Min(A.X, B.X);
	const float YMin = FMath::Min(A.Y, B.Y);
	const float XMax = FMath::Max(A.Right(), B.Right());
	const float YMax = FMath::Max(A.Bottom(), B.Bottom());
	return FLayoutRect{ XMin, YMin, XMax - XMin, YMax - YMin };
}

static FLayoutRect CenteredRect(const FVector2D& Size)
{
	return FLayoutRect{ 0.0f, 0.0f, static_cast<float>(Size.X), static_cast<float>(Size.Y) };
}

static bool ContainsTag(const FString& Source, const TCHAR* Tag)
{
	return Source.Contains(Tag, ESearchCase::IgnoreCase);
}

static FString RemoveTag(const FString& Source, const TCHAR* Tag)
{
	return Source.Replace(Tag, TEXT(""), ESearchCase::IgnoreCase);
}

static FString SanitizeName(const FString& InName, const FString& Fallback)
{
	FString Name = InName;
	Name.TrimStartAndEndInline();
	if (Name.IsEmpty())
	{
		Name = Fallback;
	}

	const TCHAR InvalidChars[] = TEXT("\\/:*?\"<>|");
	for (TCHAR& Character : Name)
	{
		if (FCString::Strchr(InvalidChars, Character) || Character < 32)
		{
			Character = TEXT('_');
		}
	}

	while (Name.EndsWith(TEXT(".")) || Name.EndsWith(TEXT(" ")))
	{
		Name.LeftChopInline(1);
	}

	Name = ObjectTools::SanitizeObjectName(Name);
	return Name.IsEmpty() ? Fallback : Name;
}

static FString RemoveAnimationTags(const FString& Name)
{
	FString Result = RemoveTag(Name, TEXT("|Animation"));
	int32 FpsIndex = Result.Find(TEXT("|FPS="), ESearchCase::IgnoreCase);
	if (FpsIndex != INDEX_NONE)
	{
		const int32 End = Result.Find(TEXT("|"), ESearchCase::IgnoreCase, ESearchDir::FromStart, FpsIndex + 1);
		Result.RemoveAt(FpsIndex, End == INDEX_NONE ? Result.Len() - FpsIndex : End - FpsIndex, EAllowShrinking::No);
	}
	return Result;
}

static FString GetAnimationLayerBaseName(const FString& Name)
{
	FString Stripped = RemoveAnimationTags(Name);
	FString Left;
	FString Right;
	if (Stripped.Split(TEXT("|"), &Left, &Right))
	{
		Stripped = Left;
	}
	Stripped.TrimStartAndEndInline();
	return Stripped.IsEmpty() ? TEXT("Animation") : Stripped;
}

static EButtonChildRole GetButtonChildRole(const FPsdLayer& Layer)
{
	if (ContainsTag(Layer.Name, TEXT("|Disabled")))
	{
		return EButtonChildRole::Disabled;
	}
	if (ContainsTag(Layer.Name, TEXT("|Highlighted")))
	{
		return EButtonChildRole::Highlighted;
	}
	if (ContainsTag(Layer.Name, TEXT("|Pressed")))
	{
		return EButtonChildRole::Pressed;
	}
	if (ContainsTag(Layer.Name, TEXT("|Default")) ||
		ContainsTag(Layer.Name, TEXT("|Enabled")) ||
		ContainsTag(Layer.Name, TEXT("|Normal")) ||
		ContainsTag(Layer.Name, TEXT("|Up")))
	{
		return EButtonChildRole::Default;
	}
	if (!Layer.bIsTextLayer && ContainsTag(Layer.Name, TEXT("|Text")))
	{
		return EButtonChildRole::TextImage;
	}
	return EButtonChildRole::None;
}

static FString GetButtonChildBaseName(const FPsdLayer& Layer)
{
	FString Name = Layer.Name;
	Name = RemoveTag(Name, TEXT("|Disabled"));
	Name = RemoveTag(Name, TEXT("|Highlighted"));
	Name = RemoveTag(Name, TEXT("|Pressed"));
	Name = RemoveTag(Name, TEXT("|Default"));
	Name = RemoveTag(Name, TEXT("|Enabled"));
	Name = RemoveTag(Name, TEXT("|Normal"));
	Name = RemoveTag(Name, TEXT("|Up"));
	if (!Layer.bIsTextLayer)
	{
		Name = RemoveTag(Name, TEXT("|Text"));
	}
	return Name;
}

static EAnchorPreset ParseAnchorPreset(FString Name)
{
	Name.TrimStartInline();
	if (Name.StartsWith(TEXT("全局"))) return EAnchorPreset::Global;
	if (Name.StartsWith(TEXT("左上"))) return EAnchorPreset::TopLeft;
	if (Name.StartsWith(TEXT("左下"))) return EAnchorPreset::BottomLeft;
	if (Name.StartsWith(TEXT("右上"))) return EAnchorPreset::TopRight;
	if (Name.StartsWith(TEXT("右下"))) return EAnchorPreset::BottomRight;
	if (Name.StartsWith(TEXT("中间"))) return EAnchorPreset::Center;
	if (Name.StartsWith(TEXT("左中"))) return EAnchorPreset::LeftMiddle;
	if (Name.StartsWith(TEXT("右中"))) return EAnchorPreset::RightMiddle;
	if (Name.StartsWith(TEXT("上中"))) return EAnchorPreset::TopMiddle;
	if (Name.StartsWith(TEXT("下中"))) return EAnchorPreset::BottomMiddle;
	if (Name.StartsWith(TEXT("上"))) return EAnchorPreset::TopMiddle;
	if (Name.StartsWith(TEXT("下"))) return EAnchorPreset::BottomMiddle;
	if (Name.StartsWith(TEXT("左"))) return EAnchorPreset::LeftMiddle;
	if (Name.StartsWith(TEXT("右"))) return EAnchorPreset::RightMiddle;
	return EAnchorPreset::None;
}

static FString GetAnchorParsingName(const FLayerImportInfo& Info)
{
	if (!Info.Layer)
	{
		return FString();
	}

	if (Info.bAnimationGroup)
	{
		return GetAnimationLayerBaseName(Info.Layer->Name);
	}
	if (Info.bButtonGroup)
	{
		return RemoveTag(Info.Layer->Name, TEXT("|Button"));
	}
	if (Info.ButtonRole != EButtonChildRole::None)
	{
		return GetButtonChildBaseName(*Info.Layer);
	}

	FString Left;
	FString Right;
	return Info.Layer->Name.Split(TEXT("|"), &Left, &Right) ? Left : Info.Layer->Name;
}

static EAnchorPreset ResolveAnchorPreset(const FLayerImportInfo& Info, const TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap)
{
	if (Info.ExplicitAnchorPreset != EAnchorPreset::None)
	{
		return Info.ExplicitAnchorPreset;
	}

	const FLayerImportInfo* Parent = Info.ParentLayer ? InfoMap.Find(Info.ParentLayer) : nullptr;
	if (Parent && Parent->bFolderLike && Parent->AnchorPreset != EAnchorPreset::None)
	{
		return Parent->AnchorPreset;
	}

	return EAnchorPreset::None;
}

static bool IsGlobalAnchorPreset(EAnchorPreset Preset)
{
	return Preset == EAnchorPreset::Global;
}

static EAnchorPreset NormalizePointAnchorPreset(EAnchorPreset Preset)
{
	return Preset == EAnchorPreset::None || Preset == EAnchorPreset::Global ? EAnchorPreset::Center : Preset;
}

static FVector2D GetAnchorVector(EAnchorPreset Preset)
{
	switch (NormalizePointAnchorPreset(Preset))
	{
	case EAnchorPreset::TopLeft: return FVector2D(0.0f, 0.0f);
	case EAnchorPreset::BottomLeft: return FVector2D(0.0f, 1.0f);
	case EAnchorPreset::TopRight: return FVector2D(1.0f, 0.0f);
	case EAnchorPreset::BottomRight: return FVector2D(1.0f, 1.0f);
	case EAnchorPreset::LeftMiddle: return FVector2D(0.0f, 0.5f);
	case EAnchorPreset::RightMiddle: return FVector2D(1.0f, 0.5f);
	case EAnchorPreset::TopMiddle: return FVector2D(0.5f, 0.0f);
	case EAnchorPreset::BottomMiddle: return FVector2D(0.5f, 1.0f);
	case EAnchorPreset::Center:
	default:
		return FVector2D(0.5f, 0.5f);
	}
}

static FVector2D GetPsdPresetPoint(const FLayoutRect& Rect, EAnchorPreset Preset)
{
	switch (NormalizePointAnchorPreset(Preset))
	{
	case EAnchorPreset::TopLeft: return FVector2D(Rect.X, Rect.Y);
	case EAnchorPreset::BottomLeft: return FVector2D(Rect.X, Rect.Bottom());
	case EAnchorPreset::TopRight: return FVector2D(Rect.Right(), Rect.Y);
	case EAnchorPreset::BottomRight: return FVector2D(Rect.Right(), Rect.Bottom());
	case EAnchorPreset::LeftMiddle: return FVector2D(Rect.X, Rect.Y + Rect.Height * 0.5f);
	case EAnchorPreset::RightMiddle: return FVector2D(Rect.Right(), Rect.Y + Rect.Height * 0.5f);
	case EAnchorPreset::TopMiddle: return FVector2D(Rect.X + Rect.Width * 0.5f, Rect.Y);
	case EAnchorPreset::BottomMiddle: return FVector2D(Rect.X + Rect.Width * 0.5f, Rect.Bottom());
	case EAnchorPreset::Center:
	default:
		return FVector2D(Rect.X + Rect.Width * 0.5f, Rect.Y + Rect.Height * 0.5f);
	}
}

static FVector2D MapPsdPointToLocalSpace(const FVector2D& Point, const FUiLayoutContext& Context)
{
	if (Context.PsdReferenceRect.Width <= 0.0f || Context.PsdReferenceRect.Height <= 0.0f)
	{
		return FVector2D::ZeroVector;
	}

	const float NormalizedX = (Point.X - Context.PsdReferenceRect.X) / Context.PsdReferenceRect.Width;
	const float NormalizedY = (Point.Y - Context.PsdReferenceRect.Y) / Context.PsdReferenceRect.Height;
	return FVector2D(
		Context.LocalDisplayRect.X + NormalizedX * Context.LocalDisplayRect.Width,
		Context.LocalDisplayRect.Y + NormalizedY * Context.LocalDisplayRect.Height);
}

static FUiLayoutContext GetChildLayoutContext(const FUiLayoutContext& ParentContext, const FLayoutRect& Rect, EAnchorPreset Preset)
{
	if (IsGlobalAnchorPreset(Preset))
	{
		return ParentContext;
	}

	const FVector2D ChildSize(Rect.Width, Rect.Height);
	return FUiLayoutContext{ Rect, ChildSize, CenteredRect(ChildSize) };
}

static void ApplyLayerLayout(UCanvasPanelSlot* Slot, const FUiLayoutContext& ParentContext, const FLayoutRect& Rect, EAnchorPreset Preset)
{
	if (!Slot)
	{
		return;
	}

	if (IsGlobalAnchorPreset(Preset))
	{
		Slot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		Slot->SetAlignment(FVector2D(0.5f, 0.5f));
		Slot->SetOffsets(FMargin(0.0f));
		return;
	}

	const FVector2D Anchor = GetAnchorVector(Preset);
	const FVector2D LocalPoint = MapPsdPointToLocalSpace(GetPsdPresetPoint(Rect, Preset), ParentContext);
	const FVector2D AnchorPoint(ParentContext.LocalRectSize.X * Anchor.X, ParentContext.LocalRectSize.Y * Anchor.Y);

	Slot->SetAnchors(FAnchors(Anchor.X, Anchor.Y));
	Slot->SetAlignment(Anchor);
	Slot->SetPosition(LocalPoint - AnchorPoint);
	Slot->SetSize(FVector2D(Rect.Width, Rect.Height));
}

static bool TryResolveLayoutRect(
	FLayerImportInfo& Info,
	const TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap,
	const FPSDWidgetImportOptions& ImportOptions,
	const FVector2D& CanvasSize)
{
	if (!Info.Layer)
	{
		return false;
	}

	if (!Info.bFolderLike)
	{
		if (Info.Layer->Rect.IsValid())
		{
			Info.LayoutRect = ToLayoutRect(Info.Layer->Rect);
			return ApplyImportBounds(Info.LayoutRect, ImportOptions, CanvasSize);
		}
		return false;
	}

	bool bHasBounds = false;
	FLayoutRect Combined;
	for (const TSharedPtr<FPsdLayer>& Child : Info.Layer->Children)
	{
		const FLayerImportInfo* ChildInfo = Child.IsValid() ? InfoMap.Find(Child.Get()) : nullptr;
		if (!ChildInfo || !ChildInfo->bEffectiveVisible || !ChildInfo->bHasLayoutRect)
		{
			continue;
		}

		Combined = bHasBounds ? CombineRects(Combined, ChildInfo->LayoutRect) : ChildInfo->LayoutRect;
		bHasBounds = true;
	}

	if (bHasBounds)
	{
		Info.LayoutRect = Combined;
		return ApplyImportBounds(Info.LayoutRect, ImportOptions, CanvasSize);
	}

	if (Info.Layer->Rect.IsValid())
	{
		Info.LayoutRect = ToLayoutRect(Info.Layer->Rect);
		return ApplyImportBounds(Info.LayoutRect, ImportOptions, CanvasSize);
	}

	return false;
}

static void CreateInfoRecursive(
	const TSharedPtr<FPsdLayer>& Layer,
	const FPsdLayer* Parent,
	bool bParentVisible,
	TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap,
	const FPSDWidgetImportOptions& ImportOptions,
	const FVector2D& CanvasSize)
{
	if (!Layer.IsValid())
	{
		return;
	}

	FLayerImportInfo& Info = InfoMap.Add(Layer.Get());
	Info.Layer = Layer.Get();
	Info.ParentLayer = Parent;
	Info.bEffectiveVisible = bParentVisible && Layer->IsVisible();
	Info.bFolderLike = Layer->Children.Num() > 0 || Layer->Rect.Width == 0;
	Info.bButtonGroup = Info.bFolderLike && ContainsTag(Layer->Name, TEXT("|Button"));
	Info.bAnimationGroup = Info.bFolderLike && ContainsTag(Layer->Name, TEXT("|Animation"));
	const FLayerImportInfo* ParentInfo = Parent ? InfoMap.Find(Parent) : nullptr;
	Info.ButtonRole = ParentInfo && ParentInfo->bButtonGroup ? GetButtonChildRole(*Layer) : EButtonChildRole::None;
	Info.ExplicitAnchorPreset = ParseAnchorPreset(GetAnchorParsingName(Info));
	Info.AnchorPreset = ResolveAnchorPreset(Info, InfoMap);

	for (const TSharedPtr<FPsdLayer>& Child : Layer->Children)
	{
		CreateInfoRecursive(Child, Layer.Get(), Info.bEffectiveVisible, InfoMap, ImportOptions, CanvasSize);
	}

	FLayerImportInfo& StoredInfo = InfoMap.FindChecked(Layer.Get());
	StoredInfo.bHasLayoutRect = TryResolveLayoutRect(StoredInfo, InfoMap, ImportOptions, CanvasSize);
}

static FString GetStableSelfBaseName(const FLayerImportInfo& Info)
{
	if (!Info.Layer)
	{
		return TEXT("Layer");
	}
	if (Info.bAnimationGroup)
	{
		return SanitizeName(GetAnimationLayerBaseName(Info.Layer->Name), TEXT("Animation"));
	}
	if (Info.bButtonGroup)
	{
		return SanitizeName(RemoveTag(Info.Layer->Name, TEXT("|Button")), TEXT("Button"));
	}
	if (Info.ButtonRole != EButtonChildRole::None)
	{
		return SanitizeName(GetButtonChildBaseName(*Info.Layer), Info.Layer->bIsTextLayer ? TEXT("Text") : TEXT("Layer"));
	}
	return SanitizeName(Info.Layer->Name, Info.bFolderLike ? TEXT("Folder") : TEXT("Layer"));
}

static FString GetPreferredTextureBaseName(const FLayerImportInfo& Info, const TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap)
{
	const FLayerImportInfo* Parent = Info.ParentLayer ? InfoMap.Find(Info.ParentLayer) : nullptr;
	if (Parent && Parent->bButtonGroup)
	{
		return SanitizeName(Parent->UniqueSelfName + TEXT("_") + Info.UniqueSelfName, TEXT("Layer"));
	}
	return Info.UniqueSelfName.IsEmpty() ? GetStableSelfBaseName(Info) : Info.UniqueSelfName;
}

template <typename NameGetter, typename NameSetter>
static void AssignUniqueNames(const TArray<const FPsdLayer*>& Layers, TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap, NameGetter GetName, NameSetter SetName)
{
	TMap<FString, int32> Counts;
	for (const FPsdLayer* Layer : Layers)
	{
		FLayerImportInfo& Info = InfoMap.FindChecked(Layer);
		FString BaseName = SanitizeName(GetName(Info), TEXT("Layer"));
		FString Key = BaseName.ToLower();
		int32& Count = Counts.FindOrAdd(Key);
		++Count;
		SetName(Info, Count == 1 ? BaseName : FString::Printf(TEXT("%s_%d"), *BaseName, Count));
	}
}

static void AssignUniqueSelfNamesRecursive(const TArray<TSharedPtr<FPsdLayer>>& Siblings, TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap)
{
	TArray<const FPsdLayer*> Layers;
	for (const TSharedPtr<FPsdLayer>& Layer : Siblings)
	{
		if (Layer.IsValid())
		{
			Layers.Add(Layer.Get());
		}
	}

	AssignUniqueNames(
		Layers,
		InfoMap,
		[](const FLayerImportInfo& Info) { return GetStableSelfBaseName(Info); },
		[](FLayerImportInfo& Info, const FString& Name) { Info.UniqueSelfName = Name; });

	for (const TSharedPtr<FPsdLayer>& Layer : Siblings)
	{
		if (Layer.IsValid())
		{
			AssignUniqueSelfNamesRecursive(Layer->Children, InfoMap);
		}
	}
}

static bool ShouldLayerEmitTexture(const FLayerImportInfo& Info)
{
	if (!Info.Layer || Info.bFolderLike || !Info.Layer->Rect.IsValid())
	{
		return false;
	}

	return !Info.Layer->bIsTextLayer || !Info.bEffectiveVisible;
}

static bool ShouldButtonChildEmitTexture(const FLayerImportInfo& Info)
{
	if (!Info.Layer || Info.bFolderLike)
	{
		return false;
	}

	if (Info.ButtonRole != EButtonChildRole::None)
	{
		return !Info.Layer->bIsTextLayer || !Info.bEffectiveVisible;
	}

	return !Info.bEffectiveVisible && ShouldLayerEmitTexture(Info);
}

static void AssignUniqueTextureNamesForScope(const TArray<TSharedPtr<FPsdLayer>>& Siblings, TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap)
{
	TArray<const FPsdLayer*> Emitters;
	for (const TSharedPtr<FPsdLayer>& Sibling : Siblings)
	{
		if (!Sibling.IsValid())
		{
			continue;
		}

		const FLayerImportInfo& Info = InfoMap.FindChecked(Sibling.Get());
		if (Info.bButtonGroup)
		{
			for (const TSharedPtr<FPsdLayer>& Child : Sibling->Children)
			{
				const FLayerImportInfo& ChildInfo = InfoMap.FindChecked(Child.Get());
				if (ShouldButtonChildEmitTexture(ChildInfo))
				{
					Emitters.Add(Child.Get());
				}
			}
		}
		else if (!Info.bFolderLike && ShouldLayerEmitTexture(Info))
		{
			Emitters.Add(Sibling.Get());
		}
	}

	AssignUniqueNames(
		Emitters,
		InfoMap,
		[&InfoMap](const FLayerImportInfo& Info) { return GetPreferredTextureBaseName(Info, InfoMap); },
		[](FLayerImportInfo& Info, const FString& Name) { Info.UniqueTextureName = Name; });

	for (const TSharedPtr<FPsdLayer>& Sibling : Siblings)
	{
		if (Sibling.IsValid())
		{
			const FLayerImportInfo& Info = InfoMap.FindChecked(Sibling.Get());
			if (Info.bFolderLike && !Info.bButtonGroup)
			{
				AssignUniqueTextureNamesForScope(Sibling->Children, InfoMap);
			}
		}
	}
}

static void BuildLayerInfos(
	const TArray<TSharedPtr<FPsdLayer>>& Tree,
	TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap,
	const FPSDWidgetImportOptions& ImportOptions,
	const FVector2D& CanvasSize)
{
	InfoMap.Reset();
	for (const TSharedPtr<FPsdLayer>& Layer : Tree)
	{
		CreateInfoRecursive(Layer, nullptr, true, InfoMap, ImportOptions, CanvasSize);
	}
	AssignUniqueSelfNamesRecursive(Tree, InfoMap);
	AssignUniqueTextureNamesForScope(Tree, InfoMap);
}

static FName MakeWidgetName(UWidgetTree* WidgetTree, const FString& BaseName)
{
	const FString SafeBase = SanitizeName(BaseName, TEXT("Widget"));
	for (int32 Index = 1;; ++Index)
	{
		const FString Candidate = Index == 1 ? SafeBase : FString::Printf(TEXT("%s_%d"), *SafeBase, Index);
		const FName CandidateName(*Candidate);
		if (!WidgetTree->FindWidget(CandidateName))
		{
			return CandidateName;
		}
	}
}

static FSlateBrush MakeTextureBrush(UTexture2D* Texture, const FVector2D& Size)
{
	FSlateBrush Brush;
	Brush.DrawAs = ESlateBrushDrawType::Image;
	Brush.SetResourceObject(Texture);
	Brush.SetImageSize(Size);
	return Brush;
}

static bool CropDecodedLayerToLayout(const FPsdLayer& Layer, const FLayoutRect& LayoutRect, FDecodedPsdLayer& Image)
{
	if (!LayoutRect.IsValid() || Image.Width <= 0 || Image.Height <= 0)
	{
		return false;
	}

	const int32 SourceX = FMath::RoundToInt(LayoutRect.X - static_cast<float>(Layer.Rect.X));
	const int32 SourceY = FMath::RoundToInt(LayoutRect.Y - static_cast<float>(Layer.Rect.Y));
	const int32 CropWidth = FMath::RoundToInt(LayoutRect.Width);
	const int32 CropHeight = FMath::RoundToInt(LayoutRect.Height);
	if (SourceX < 0 || SourceY < 0 || CropWidth <= 0 || CropHeight <= 0 ||
		SourceX + CropWidth > Image.Width || SourceY + CropHeight > Image.Height)
	{
		return false;
	}

	if (SourceX == 0 && SourceY == 0 && CropWidth == Image.Width && CropHeight == Image.Height)
	{
		return true;
	}

	TArray<uint8> CroppedBGRA;
	CroppedBGRA.SetNumZeroed(CropWidth * CropHeight * 4);
	const int32 SourceStride = Image.Width * 4;
	const int32 DestStride = CropWidth * 4;
	for (int32 Row = 0; Row < CropHeight; ++Row)
	{
		const int32 SourceIndex = ((SourceY + Row) * SourceStride) + (SourceX * 4);
		const int32 DestIndex = Row * DestStride;
		FMemory::Memcpy(CroppedBGRA.GetData() + DestIndex, Image.BGRA.GetData() + SourceIndex, DestStride);
	}

	Image.Width = CropWidth;
	Image.Height = CropHeight;
	Image.BGRA = MoveTemp(CroppedBGRA);
	return true;
}

static UTexture2D* CreateTextureAsset(FBuildState& State, const FString& PackagePath, const FLayerImportInfo& Info)
{
	if (UTexture2D** Cached = State.TextureCache.Find(Info.Layer))
	{
		return *Cached;
	}

	if (!Info.Layer || !Info.Layer->Rect.IsValid())
	{
		return nullptr;
	}

	if (State.ImportOptions.bClipLayersToCanvas && (!Info.bHasLayoutRect || !Info.LayoutRect.IsValid()))
	{
		return nullptr;
	}

	FDecodedPsdLayer Decoded;
	FString DecodeError;
	if (!State.Document.DecodeLayer(*Info.Layer, Decoded, DecodeError))
	{
		return nullptr;
	}

	if (State.ImportOptions.bClipLayersToCanvas && !CropDecodedLayerToLayout(*Info.Layer, Info.LayoutRect, Decoded))
	{
		return nullptr;
	}

	const FString AssetName = SanitizeName(Info.UniqueTextureName.IsEmpty() ? Info.UniqueSelfName : Info.UniqueTextureName, TEXT("Layer"));
	const FString LongPackageName = PackagePath / AssetName;
	UPackage* Package = CreatePackage(*LongPackageName);
	Package->FullyLoad();

	UTexture2D* Texture = FindObject<UTexture2D>(Package, *AssetName);
	if (!Texture)
	{
		Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Texture);
		State.AdditionalAssets.Add(Texture);
	}

	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Decoded.Width, Decoded.Height, 1, 1, TSF_BGRA8, Decoded.BGRA.GetData());
	Texture->CompressionSettings = TC_EditorIcon;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->LODGroup = TEXTUREGROUP_UI;
	Texture->SRGB = true;
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	Package->MarkPackageDirty();

	State.TextureCache.Add(Info.Layer, Texture);
	return Texture;
}

static void ApplyWidgetDesignCanvasSize(UWidgetBlueprint* WidgetBlueprint, const FVector2D& CanvasSize)
{
	if (!WidgetBlueprint)
	{
		return;
	}

	UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return;
	}

	UUserWidget* DefaultWidget = Cast<UUserWidget>(GeneratedClass->GetDefaultObject());
	if (!DefaultWidget)
	{
		return;
	}

	DefaultWidget->Modify();
	DefaultWidget->DesignSizeMode = EDesignPreviewSizeMode::Custom;
	DefaultWidget->DesignTimeSize = CanvasSize;
	DefaultWidget->MarkPackageDirty();
}

static bool HasVisibleRuntimeContent(const TSharedPtr<FPsdLayer>& Layer, const TMap<const FPsdLayer*, FLayerImportInfo>& InfoMap)
{
	if (!Layer.IsValid())
	{
		return false;
	}

	const FLayerImportInfo* Info = InfoMap.Find(Layer.Get());
	if (!Info || !Info->bEffectiveVisible)
	{
		return false;
	}

	if (!Info->bFolderLike)
	{
		return true;
	}

	for (const TSharedPtr<FPsdLayer>& Child : Layer->Children)
	{
		if (HasVisibleRuntimeContent(Child, InfoMap))
		{
			return true;
		}
	}
	return false;
}

static void ExportLayerTexturesOnly(FBuildState& State, const TSharedPtr<FPsdLayer>& Layer, const FString& PackagePath)
{
	if (!Layer.IsValid())
	{
		return;
	}

	const FLayerImportInfo& Info = State.LayerInfos.FindChecked(Layer.Get());
	if (Info.bFolderLike)
	{
		const FString ChildPath = Info.bButtonGroup ? PackagePath : PackagePath / Info.UniqueSelfName;
		for (const TSharedPtr<FPsdLayer>& Child : Layer->Children)
		{
			ExportLayerTexturesOnly(State, Child, ChildPath);
		}
		return;
	}

	if (ShouldLayerEmitTexture(Info) || ShouldButtonChildEmitTexture(Info))
	{
		CreateTextureAsset(State, PackagePath, Info);
	}
}

static void CreateTextWidget(FBuildState& State, UCanvasPanel* Parent, const FUiLayoutContext& ParentContext, const FLayerImportInfo& Info)
{
	UTextBlock* TextBlock = State.WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), MakeWidgetName(State.WidgetTree, Info.UniqueSelfName));
	TextBlock->SetText(FText::FromString(Info.Layer->Text));

	FLinearColor Color = Info.Layer->FillColor;
	Color.A *= static_cast<float>(Info.Layer->Opacity) / 255.0f;
	TextBlock->SetColorAndOpacity(FSlateColor(Color));

	FSlateFontInfo FontInfo = TextBlock->GetFont();
	FontInfo.Size = FMath::RoundToInt(FMath::Max(1.0f, Info.Layer->FontSize));
	TextBlock->SetFont(FontInfo);

	switch (Info.Layer->Justification)
	{
	case ETextJustification::Center:
		TextBlock->SetJustification(ETextJustify::Center);
		break;
	case ETextJustification::Right:
		TextBlock->SetJustification(ETextJustify::Right);
		break;
	case ETextJustification::Left:
	default:
		TextBlock->SetJustification(ETextJustify::Left);
		break;
	}

	UCanvasPanelSlot* Slot = Parent->AddChildToCanvas(TextBlock);
	ApplyLayerLayout(Slot, ParentContext, Info.LayoutRect, Info.AnchorPreset);
	Slot->SetZOrder(State.ZOrder++);
}

static UImage* CreateImageWidget(FBuildState& State, UCanvasPanel* Parent, const FUiLayoutContext& ParentContext, const FLayerImportInfo& Info, const FString& PackagePath)
{
	UTexture2D* Texture = CreateTextureAsset(State, PackagePath, Info);
	if (!Texture)
	{
		return nullptr;
	}

	UScaleBox* ScaleBox = State.WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass(), MakeWidgetName(State.WidgetTree, Info.UniqueSelfName));
	ScaleBox->SetStretch(EStretch::ScaleToFill);
	ScaleBox->SetStretchDirection(EStretchDirection::Both);

	UImage* Image = State.WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), MakeWidgetName(State.WidgetTree, Info.UniqueSelfName + TEXT("_Image")));
	Image->SetBrushFromTexture(Texture, true);
	Image->SetDesiredSizeOverride(Info.LayoutRect.Size());
	ScaleBox->SetContent(Image);

	UCanvasPanelSlot* Slot = Parent->AddChildToCanvas(ScaleBox);
	ApplyLayerLayout(Slot, ParentContext, Info.LayoutRect, Info.AnchorPreset);
	Slot->SetZOrder(State.ZOrder++);
	return Image;
}

static void ExportTree(FBuildState& State, const TArray<TSharedPtr<FPsdLayer>>& Layers, UCanvasPanel* Parent, const FUiLayoutContext& ParentContext, const FString& PackagePath);

static void CreateButtonWidget(FBuildState& State, const TSharedPtr<FPsdLayer>& Layer, UCanvasPanel* Parent, const FUiLayoutContext& ParentContext, const FString& PackagePath)
{
	const FLayerImportInfo& Info = State.LayerInfos.FindChecked(Layer.Get());
	if (!Info.bHasLayoutRect || !Info.bEffectiveVisible)
	{
		ExportLayerTexturesOnly(State, Layer, PackagePath);
		return;
	}

	UButton* Button = State.WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), MakeWidgetName(State.WidgetTree, Info.UniqueSelfName));
	UCanvasPanelSlot* ButtonSlot = Parent->AddChildToCanvas(Button);
	ApplyLayerLayout(ButtonSlot, ParentContext, Info.LayoutRect, Info.AnchorPreset);
	ButtonSlot->SetZOrder(State.ZOrder++);

	FButtonStyle Style = Button->GetStyle();
	const FVector2D ButtonSize(Info.LayoutRect.Width, Info.LayoutRect.Height);
	UCanvasPanel* ContentCanvas = State.WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), MakeWidgetName(State.WidgetTree, Info.UniqueSelfName + TEXT("_Content")));

	for (const TSharedPtr<FPsdLayer>& Child : Layer->Children)
	{
		const FLayerImportInfo& ChildInfo = State.LayerInfos.FindChecked(Child.Get());
		if (!ChildInfo.bEffectiveVisible || !ChildInfo.bHasLayoutRect)
		{
			ExportLayerTexturesOnly(State, Child, PackagePath);
			continue;
		}

		if (!ChildInfo.bFolderLike && !Child->bIsTextLayer && ChildInfo.ButtonRole != EButtonChildRole::None && ChildInfo.ButtonRole != EButtonChildRole::TextImage)
		{
			UTexture2D* Texture = CreateTextureAsset(State, PackagePath, ChildInfo);
			if (!Texture)
			{
				continue;
			}

			FSlateBrush Brush = MakeTextureBrush(Texture, ChildInfo.LayoutRect.Size());
			switch (ChildInfo.ButtonRole)
			{
			case EButtonChildRole::Default: Style.SetNormal(Brush); break;
			case EButtonChildRole::Pressed: Style.SetPressed(Brush); break;
			case EButtonChildRole::Highlighted: Style.SetHovered(Brush); break;
			case EButtonChildRole::Disabled: Style.SetDisabled(Brush); break;
			default: break;
			}
			continue;
		}

		FUiLayoutContext ButtonContext{ Info.LayoutRect, ButtonSize, CenteredRect(ButtonSize) };
		if (Child->bIsTextLayer)
		{
			CreateTextWidget(State, ContentCanvas, ButtonContext, ChildInfo);
		}
		else
		{
			CreateImageWidget(State, ContentCanvas, ButtonContext, ChildInfo, PackagePath);
		}
	}

	Button->SetStyle(Style);
	UButtonSlot* ContentSlot = Cast<UButtonSlot>(Button->SetContent(ContentCanvas));
	if (ContentSlot)
	{
		ContentSlot->SetPadding(FMargin(0.0f));
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Fill);
	}
}

static void ExportLayer(FBuildState& State, const TSharedPtr<FPsdLayer>& Layer, UCanvasPanel* Parent, const FUiLayoutContext& ParentContext, const FString& PackagePath)
{
	if (!Layer.IsValid())
	{
		return;
	}

	const FLayerImportInfo& Info = State.LayerInfos.FindChecked(Layer.Get());
	if (Info.bFolderLike)
	{
		if (Info.bButtonGroup)
		{
			CreateButtonWidget(State, Layer, Parent, ParentContext, PackagePath);
			return;
		}

		const FString ChildPackagePath = PackagePath / Info.UniqueSelfName;
		if (!Info.bEffectiveVisible || !HasVisibleRuntimeContent(Layer, State.LayerInfos) || !Info.bHasLayoutRect)
		{
			ExportLayerTexturesOnly(State, Layer, ChildPackagePath);
			return;
		}

		UCanvasPanel* Group = State.WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), MakeWidgetName(State.WidgetTree, Info.UniqueSelfName));
		UCanvasPanelSlot* GroupSlot = Parent->AddChildToCanvas(Group);
		ApplyLayerLayout(GroupSlot, ParentContext, Info.LayoutRect, Info.AnchorPreset);
		GroupSlot->SetZOrder(State.ZOrder++);

		FUiLayoutContext ChildContext = GetChildLayoutContext(ParentContext, Info.LayoutRect, Info.AnchorPreset);
		ExportTree(State, Layer->Children, Group, ChildContext, ChildPackagePath);
		return;
	}

	if (!Info.bEffectiveVisible)
	{
		if (ShouldLayerEmitTexture(Info))
		{
			CreateTextureAsset(State, PackagePath, Info);
		}
		return;
	}

	if (!Info.bHasLayoutRect)
	{
		if (ShouldLayerEmitTexture(Info))
		{
			CreateTextureAsset(State, PackagePath, Info);
		}
		return;
	}

	if (Layer->bIsTextLayer)
	{
		CreateTextWidget(State, Parent, ParentContext, Info);
	}
	else
	{
		CreateImageWidget(State, Parent, ParentContext, Info, PackagePath);
	}
}

static void ExportTree(FBuildState& State, const TArray<TSharedPtr<FPsdLayer>>& Layers, UCanvasPanel* Parent, const FUiLayoutContext& ParentContext, const FString& PackagePath)
{
	for (int32 Index = Layers.Num() - 1; Index >= 0; --Index)
	{
		ExportLayer(State, Layers[Index], Parent, ParentContext, PackagePath);
	}
}
}

bool FPSDWidgetBuilder::ImportPSDAsWidget(
	const FString& Filename,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	FPSDWidgetImportResult& OutResult,
	FText& OutError)
{
	FPSDWidgetImportOptions ImportOptions;
	return ImportPSDAsWidget(Filename, InParent, InName, Flags, ImportOptions, OutResult, OutError);
}

bool FPSDWidgetBuilder::ImportPSDAsWidget(
	const FString& Filename,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FPSDWidgetImportOptions& ImportOptions,
	FPSDWidgetImportResult& OutResult,
	FText& OutError)
{
	if (!InParent)
	{
		OutError = FText::FromString(TEXT("PSD import requires a valid destination package."));
		return false;
	}

	FBuildState State;
	State.ImportOptions = ImportOptions;
	FString LoadError;
	if (!State.Document.Load(Filename, LoadError))
	{
		OutError = FText::FromString(LoadError);
		return false;
	}

	TArray<TSharedPtr<FPsdLayer>> Tree = FPsdDocument::BuildLayerTree(State.Document.Layers);
	const FVector2D RootSize(static_cast<float>(State.Document.Width), static_cast<float>(State.Document.Height));
	BuildLayerInfos(Tree, State.LayerInfos, State.ImportOptions, RootSize);

	const FString WidgetAssetName = SanitizeName(InName.ToString(), FPaths::GetBaseFilename(Filename));
	const FString DestinationPackageName = InParent->GetOutermost()->GetName();
	const FString DestinationPath = FPackageName::GetLongPackagePath(DestinationPackageName);
	State.TextureRootPackagePath = DestinationPath / (WidgetAssetName + TEXT("_Layers"));

	State.WidgetBlueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UUserWidget::StaticClass(),
		InParent,
		FName(*WidgetAssetName),
		BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass(),
		NAME_None));

	if (!State.WidgetBlueprint || !State.WidgetBlueprint->WidgetTree)
	{
		OutError = FText::FromString(TEXT("Failed to create Widget Blueprint."));
		return false;
	}

	State.WidgetTree = State.WidgetBlueprint->WidgetTree;
	State.WidgetBlueprint->Modify();
	State.WidgetTree->Modify();

	UScaleBox* RootScaleBox = State.WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass(), TEXT("ROOT"));
	RootScaleBox->SetStretch(EStretch::ScaleToFit);
	RootScaleBox->SetStretchDirection(EStretchDirection::Both);

	USizeBox* RootSizeBox = State.WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("SizeBox"));
	RootSizeBox->SetWidthOverride(static_cast<float>(State.Document.Width));
	RootSizeBox->SetHeightOverride(static_cast<float>(State.Document.Height));

	UCanvasPanel* RootCanvas = State.WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas"));
	RootSizeBox->SetContent(RootCanvas);
	RootScaleBox->SetContent(RootSizeBox);
	State.WidgetTree->RootWidget = RootScaleBox;
	State.WidgetBlueprint->OnVariableAdded(RootScaleBox->GetFName());
	State.WidgetBlueprint->OnVariableAdded(RootSizeBox->GetFName());
	State.WidgetBlueprint->OnVariableAdded(RootCanvas->GetFName());

	const FUiLayoutContext RootContext{
		FLayoutRect{ 0.0f, 0.0f, static_cast<float>(RootSize.X), static_cast<float>(RootSize.Y) },
		RootSize,
		CenteredRect(RootSize)
	};

	ExportTree(State, Tree, RootCanvas, RootContext, State.TextureRootPackagePath);

	FKismetEditorUtilities::CompileBlueprint(State.WidgetBlueprint);
	ApplyWidgetDesignCanvasSize(State.WidgetBlueprint, RootSize);
	State.WidgetBlueprint->MarkPackageDirty();
	InParent->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(State.WidgetBlueprint);

	OutResult.WidgetBlueprint = State.WidgetBlueprint;
	OutResult.AdditionalAssets = MoveTemp(State.AdditionalAssets);
	return true;
}
