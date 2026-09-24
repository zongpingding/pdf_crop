# PDF 区域裁剪

基于 C++17、Qt 6、Poppler Qt6 和 MuPDF 的 Arch Linux / Wayland 桌面应用。打开 PDF，在应用内拖动鼠标框选区域，再选择导出方式。无需 `slurp`、`pdfcrop` 或 `mutool` 命令。

![界面预览](screenshot.png)

[查看暗色主题预览](screenshot-dark.png)

[查看英文界面预览](screenshot-en.png)

[查看 1600% 局部渲染预览](screenshot-1600.png)

## 运行

随附的 `bin/pdf-select-crop` 是在 Arch Linux x86_64 上编译的动态链接程序。需要安装 `qt6-base`、`qt6-wayland`、`poppler-qt6` 和 `libmupdf`：

```sh
sudo pacman -S --needed qt6-base qt6-wayland poppler-qt6 libmupdf
./bin/pdf-select-crop [输入文件.pdf]
```

也可以自行构建：

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base qt6-wayland qt6-tools poppler-qt6 libmupdf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/pdf-select-crop [输入文件.pdf]
```

## 使用

1. 用 Ctrl+O 或“打开 PDF”选择多个文件，每个 PDF 会出现在独立标签页；Ctrl+W 或标签上的关闭按钮可关闭当前页签。重新打开已打开的文件会切换到已有标签页。
2. 在页面上按住鼠标左键拖出裁剪矩形。顶部页码框可直接跳转；预览区获得焦点时，空格/Shift+空格可前后翻页。
3. 预览区单独滚轮上下移动，Shift+滚轮左右移动，Ctrl+滚轮缩放。缩放范围为 25%–1600%，每档 10%；顶部缩放控件也可直接输入百分比。缩放时选区会保留。
4. “旋转”控件提供 0°、90°、180°、270° 四个预览角度。旋转后选区仍对应原 PDF 的正确位置；导出 PDF 保持原有页面方向。
5. 有书签的 PDF 会显示书签侧栏，可点击跳转；没有书签时按钮禁用、侧栏隐藏。
6. 选择“仅导出当前页”或“相同选区比例应用到全部页面”。全部页面模式按宽、高比例套用选区，适合不同尺寸和旋转的页面。
7. 选择导出方式，再点击“导出选区…”。导出文件必须与原文件不同。

“历史”菜单保存最近打开的 10 个文件路径，可再次打开或清空记录；“主题”菜单可切换亮色和暗色；“语言”菜单可在简体中文与英语之间切换。这些设置会在下次启动时保留。界面译文位于 `i18n/`，构建时由 Qt Linguist Tools 编译并嵌入程序。

预览使用 Poppler 的抗锯齿局部渲染，只生成视口附近的像素，而不是缓存整页高倍位图。因此高倍率下的文字和矢量图形仍按当前倍率渲染，内存占用主要随窗口大小变化。源 PDF 内嵌的低分辨率图片本身不会因此变清晰。

“保留文字与矢量（尽量移除）”使用 MuPDF 过滤完全落在选区外的绘制对象，调整输出页面尺寸，保留选区内可搜索的文字和矢量图形。**跨越边界的图片、字形或复杂绘制对象仍可能携带选区外数据**；不要用此模式处理需要彻底清除的敏感信息。部分复杂 PDF 的视觉结果也可能与原件略有差异。

“严格移除（转为图片）”只渲染选区内的页面像素，再写入全新的 PDF。原文件中的文字、矢量、注释、附件和原始图像流不会复制到输出文件。它适合需要移除区域外原始 PDF 数据的场景，但输出页面是位图，文字不能搜索或选取，也不保留矢量可编辑性。可选 150–600 DPI，默认 300 DPI；过大的页面会提示降低 DPI。

暂不支持加密 PDF。导出期间窗口会等待处理完成，大文件可能需要一些时间。

## 许可

本项目源代码采用 [AGPL-3.0-or-later](LICENSE)。项目链接 MuPDF；再分发或闭源使用时，应核对 MuPDF 的 AGPL / 商业授权条款。Qt、Poppler 及其依赖仍遵循各自许可。
