# PSDLayoutTool2UE

`PSDLayoutTool2UE` 是一个 Unreal Engine 编辑器插件，用来把分层 `.psd` 文件导入为 UMG `Widget Blueprint`。它的设计思路参考了 Unity 版本的 `UnityPSDLayoutTool2`：读取 PSD 图层、图层组、文本层和命名标签，然后自动生成 Widget 层级与对应贴图资源。

当前版本主要面向 UE 5.6 编辑器环境。

## 功能概览

- 直接读取分层 PSD。
- 根据 PSD Canvas 尺寸设置 Widget Designer 的默认画布尺寸。
- 普通图层生成 `Image`，并自动导出为 `Texture2D`。
- 图片默认使用 `ScaleBox` 包裹，`Stretch` 为 `Scale To Fill`。
- 文本图层生成 `TextBlock`。
- 图层组生成嵌套 `CanvasPanel`。
- `|Button` 图层组生成 `Button`。
- 支持按钮状态图层：`|Normal`、`|Up`、`|Default`、`|Pressed`、`|Highlighted`、`|Disabled`。
- 隐藏图层不会出现在 Widget 中，但仍会导出贴图资源。
- 支持图层透明度和用户蒙版烘焙到导出的贴图。
- 插件加载时会关闭 UE 5.6 自带的 PSD Interchange 贴图导入器，让 PSD 拖拽导入优先走本插件。

## 安装方式

把插件目录放到项目的 `Plugins` 目录下：

```text
YourProject/
  Plugins/
    PSDLayoutTool2UE/
      PSDLayoutTool2UE.uplugin
      Source/
      README.md
```

然后确认项目的 `.uproject` 中启用了插件：

```json
{
  "Name": "PSDLayoutTool2UE",
  "Enabled": true,
  "TargetAllowList": [
    "Editor"
  ]
}
```

首次添加插件后需要重新生成项目文件并编译 Editor 目标。

## 编译

关闭 Unreal Editor，或者先关闭 Live Coding，再编译项目。

命令行示例：

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" YourProjectEditor Win64 Development -Project="C:\Path\To\YourProject.uproject" -WaitMutex -NoHotReload
```

如果看到下面的错误，说明 UE 仍在运行 Live Coding：

```text
Unable to build while Live Coding is active
```

处理方式：

- 关闭 Unreal Editor 后重新编译。
- 或在编辑器中按 `Ctrl+Alt+F11` 停止 Live Coding 后再编译。

## 导入 PSD

推荐流程：

1. 打开 Unreal Editor。
2. 在 Content Browser 中选择目标目录，例如 `/Game/UI`。
3. 把 `.psd` 文件拖进 Content Browser。
4. 等待导入完成。

也可以使用菜单导入：

```text
Tools > PSD Layout Tool 2 UE > Import PSD as Widget
```

导入完成后会生成：

- 一个 `Widget Blueprint`。
- 一个 `<WidgetName>_Layers` 目录，里面包含导出的 `Texture2D` 图层贴图。

如果拖入 PSD 后仍然生成普通 `Texture`，说明 UE 的默认 PSD 导入器仍在接管。可以在 Output Log 控制台输入：

```text
Interchange.FeatureFlags.Import.PSD 0
```

然后删除错误导入的 Texture，重新导入 PSD。

## PSD 命名规则

### 普通图层

普通像素图层会生成：

```text
Image
```

导出的贴图会放在：

```text
<WidgetName>_Layers/
```

### 文本图层

PSD 文本图层会生成：

```text
TextBlock
```

当前会尽量读取文本内容、字号、颜色和对齐方式。复杂 Photoshop 文本样式不保证完全还原。

### 图层组

PSD 图层组会生成：

```text
CanvasPanel
```

子图层会放进对应的 Canvas 中，坐标会按照 PSD 中的相对位置生成。

### 按钮

图层组名称包含 `|Button` 时，会生成：

```text
Button
```

按钮状态图层可以使用以下标签：

```text
|Normal
|Up
|Default
|Pressed
|Highlighted
|Disabled
```

示例：

```text
开始按钮|Button
  背景|Normal
  背景按下|Pressed
  背景悬停|Highlighted
  背景禁用|Disabled
  标题|Text
```

### 图片比例

普通图片图层会被生成成：

```text
ScaleBox
  Image
```

默认设置：

```text
Stretch = Scale To Fill
Stretch Direction = Both
```

这表示图片会保持原始长宽比例，并铺满目标区域。如果目标区域比例和图片比例不同，可能会发生裁切或溢出表现。需要完整显示图片时，可以在 UMG 中把该 `ScaleBox` 的 `Stretch` 改成 `Scale To Fit`。

## 画布尺寸

导入时插件会读取 PSD 的 Canvas 宽高，并设置：

- 根 `SizeBox` 宽高。
- Widget Designer 的 `DesignTimeSize`。
- Widget Designer 的 `DesignSizeMode = Custom`。

因此重新打开生成的 Widget Blueprint 时，Designer 画布应当和 PSD Canvas 尺寸一致。

## 重新导入建议

当前版本没有做完整的自动 Reimport 流程。PSD 改动后建议：

1. 删除旧的 Widget Blueprint。
2. 删除旧的 `<WidgetName>_Layers` 文件夹。
3. 重新导入 PSD。

如果只删除 Widget，不删除 `_Layers`，同名贴图可能会被复用或覆盖，容易让结果和预期不一致。

## 已知限制

- 目前只实现 UMG Widget 生成，不实现 Unity 版中的非 UI `SpriteRenderer` 流程。
- `|Animation` 标签暂未实现。
- Photoshop 图层效果不会完整还原，建议提前栅格化复杂效果。
- 文本层只做基础还原，复杂字体、描边、阴影、段落样式可能需要手动调整。
- PSD 格式差异较多，建议使用 RGB、8-bit、标准分层 PSD。

## 目录说明

```text
PSDLayoutTool2UE.uplugin
Source/
  PSDLayoutTool2UE/
    PSDLayoutTool2UE.Build.cs
    Public/
    Private/
```

主要代码：

- `PSDDocument.*`：PSD 读取与图层解码。
- `PSDWidgetBuilder.*`：Widget Blueprint、Canvas、Image、TextBlock、Button 和 Texture2D 生成。
- `PSDLayoutTool2UEFactory.*`：Content Browser 导入工厂。
- `PSDLayoutTool2UEModule.*`：插件模块、菜单入口和 PSD Interchange 导入开关处理。

