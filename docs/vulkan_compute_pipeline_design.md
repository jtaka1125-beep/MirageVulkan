# MirageSystem Vulkan Compute Pipeline Design
## 2025-02-10

## 1. 現状アーキテクチャ

```
[Android] MediaProjection → H264 Encoder → RTP Packetizer
    ├── UdpVideoSender (WiFi) ─────── RTP/UDP ─────┐
    └── UsbVideoSender (AOA) ── VID0+RTP ──────────┤
                                                     ↓
[PC] MirrorReceiver ← feed_rtp_packet() ← HybridReceiver
         ↓
    FFmpeg H264 Decode (D3D11VA/CPU) → RGBA buffer
         ↓
    ┌────┴────────────────┐
    │ GUI Display         │ AI Processing
    │ (Vulkan Render)     │ cv::cvtColor(RGBA→Gray)
    │                     │ clCreateImage (OpenCL)
    │                     │ MultiTemplateMatcher (OpenCL)
    │                     │ OCR (Tesseract/CPU)
    └─────────────────────┘
```

### 問題点
- RGBA→GPU転送が毎フレーム発生（CPU→OpenCL、CPU→Vulkan Render）
- OpenCLとVulkanが別々のGPUコンテキスト（メモリ共有不可）
- cv::cvtColor等のCPU前処理がボトルネック
- FFmpegデコード結果がCPUメモリに落ちる（D3D11VA使用時もsw_frame転送あり）

## 2. 理想アーキテクチャ（Vulkan統一）

```
[Android] 同上（H264/RTP、AOA/WiFi両対応、変更なし）
         ↓
[PC] MirrorReceiver → RTP depacketize → NAL units
         ↓
    Vulkan Video Decode → VkImage (GPU memory, 永続)
         ↓
    ┌────┴────────────────┐
    │ Display Path        │ Compute Path
    │ VkImage → Sampler   │ VkImage → Compute Shader
    │ → ImGui Render      │   ├── RGBA→Gray (compute)
    │                     │   ├── Template Match (compute)
    │                     │   ├── OCR Inference (ncnn/compute)
    │                     │   └── Results → CPU readback
    └─────────────────────┘
```

### メリット
- GPU内でデータが完結（zero-copy between decode→render→compute）
- CPU負荷激減（デコード、色変換、マッチング全てGPU）
- レイテンシ改善（GPU pipeline parallelism）

## 3. 段階的実装プラン

### Phase 1: Vulkan Compute基盤（今回）
**目標**: VulkanContextを拡張してCompute Shaderを実行可能にする

- VulkanComputePipeline クラス作成
  - Compute shader ロード（SPV）
  - Descriptor set layout/pool
  - Pipeline layout + Pipeline
  - Command buffer record/submit
- テスト用shader: RGBA→Grayscale変換
  - Input: VkImage (RGBA)
  - Output: VkImage (R8)
  - 検証: CPU readbackで結果確認

### Phase 2: テンプレートマッチング Compute化
**目標**: OpenCL MultiTemplateMatcherをVulkan Computeに移植

- NCC (Normalized Cross Correlation) compute shader
- Multi-scale pyramid（Vulkan Compute生成）
- テンプレート画像のVkImage管理
- 結果バッファのCPU readback

### Phase 3: Vulkan Video Decode統合
**目標**: FFmpegのD3D11VA/CPUデコードをVulkan Videoに置き換え

- VK_KHR_video_decode_h264 使用
- デコード結果が直接VkImageに → zero-copy display/compute
- ※ AMDドライバー更新後に実施

### Phase 4: OCR/推論のGPU化
**目標**: Tesseract CPUからncnn Vulkan推論に移行

- ncnn Vulkan backend
- テキスト検出モデル (CRAFT等)
- テキスト認識モデル (CRNN等)

## 4. Phase 1 詳細設計

### 4.1 VulkanComputePipeline クラス

```
src/vulkan/vulkan_compute.hpp
src/vulkan/vulkan_compute.cpp
```

```cpp
class VulkanComputePipeline {
public:
    bool create(VulkanContext& ctx,
                const std::vector<uint8_t>& spirv_code,
                const std::vector<VkDescriptorSetLayoutBinding>& bindings);
    void destroy();

    // Dispatch compute work
    void dispatch(VkCommandBuffer cmd,
                  uint32_t group_x, uint32_t group_y, uint32_t group_z);

    // Bind descriptor set
    void bind(VkCommandBuffer cmd, VkDescriptorSet ds);

    VkPipelineLayout pipelineLayout() const;
    VkDescriptorSetLayout descriptorSetLayout() const;

private:
    VulkanContext* ctx_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ds_layout_ = VK_NULL_HANDLE;
    VkShaderModule shader_module_ = VK_NULL_HANDLE;
};
```

### 4.2 RGBA→Gray Compute Shader (GLSL)

```glsl
// rgba_to_gray.comp
#version 450
layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0, rgba8) uniform readonly image2D inputImage;
layout(set = 0, binding = 1, r8)    uniform writeonly image2D outputImage;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 rgba = imageLoad(inputImage, pos);
    // ITU-R BT.601 luma coefficients (same as OpenCV)
    float gray = 0.299 * rgba.r + 0.587 * rgba.g + 0.114 * rgba.b;
    imageStore(outputImage, pos, vec4(gray, 0, 0, 0));
}
```

### 4.3 Shader compilation
- glslc (Vulkan SDK同梱) で .comp → .spv にプリコンパイル
- .spv をバイナリとしてビルドに含める
- もしくはランタイムコンパイル（shaderc）

### 4.4 統合ポイント
- AIEngine::processFrame() で、OpenCL path の代わりに Vulkan Compute を使用
- VulkanContext は gui_application 内の既存インスタンスを共有
- Compute Queue (queue family index 1) を使用（Graphics Queue とは分離）

## 5. ファイル構成

```
src/vulkan/
├── vulkan_context.hpp/cpp      (既存: instance, device, queues)
├── vulkan_swapchain.hpp/cpp    (既存: presentation)
├── vulkan_texture.hpp/cpp      (既存: image upload for ImGui)
├── vulkan_compute.hpp/cpp      (NEW: compute pipeline management)
└── vulkan_image.hpp/cpp        (NEW: GPU image management for compute)

shaders/
├── rgba_to_gray.comp           (NEW: GLSL source)
├── rgba_to_gray.spv            (NEW: compiled SPIR-V)
├── template_match_ncc.comp     (Phase 2)
└── template_match_ncc.spv      (Phase 2)
```

## 6. 依存関係

- Vulkan SDK 1.4.335.0 (installed)
- glslc for shader compilation (SDK同梱: VulkanSDK/Bin/glslc.exe)
- VulkanContext の compute queue (queue family index 1)

## 7. 映像ストリーム設計メモ

### AOA/ADB共通H264ストリーム
Android側は送信経路（AOA/WiFi）に関わらず同一のH264エンコード→RTPパケタイズを行う。
PC側はHybridReceiverで両経路を統合し、単一のMirrorReceiverデコーダーに流す。
この設計は正しく、変更不要。

### エンコードパラメータ（確認要）
- Profile: Baseline/Main
- Resolution: デバイス画面解像度（1080x1920等）
- Bitrate: 可変（帯域監視連動）
- FPS: メイン60-30fps / サブ30-10fps
