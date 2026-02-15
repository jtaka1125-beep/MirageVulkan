# MirageVulkan コード品質レビュー

**レビュー日時**: 2026-02-16
**対象**: src/video/, src/vulkan/, src/直下compute系, shaders/*.comp
**レビュアー**: Claude Opus 4.6 (自動コードレビュー)

---

## 総合スコア: **78 / 100**

| カテゴリ | スコア | 重み | 加重スコア |
|---------|--------|------|-----------|
| Vulkan Video実装の正確性 | 75/100 | 25% | 18.8 |
| リソース管理 | 82/100 | 20% | 16.4 |
| エラーハンドリング | 72/100 | 15% | 10.8 |
| スレッドセーフティ | 70/100 | 15% | 10.5 |
| コンピュートシェーダ品質 | 85/100 | 15% | 12.8 |
| UnifiedDecoder設計 | 80/100 | 10% | 8.0 |
| **合計** | | | **77.3 → 78** |

---

## 1. Vulkan Video実装の正確性 (75/100)

### 1.1 VK_KHR_video_decode_h264 API使用法

**良い点:**
- Video Profile構造体（`VkVideoProfileInfoKHR` + `VkVideoDecodeH264ProfileInfoKHR`）のpNextチェーンが正しく構成されている
- `vkGetPhysicalDeviceVideoCapabilitiesKHR` のインスタンスレベルProcAddrロードが適切
- ビデオセッションメモリバインドが仕様通り (`vulkan_video_decoder.cpp:446-508`)
- `stdHeaderVersion` をCapabilities queryから正しく取得し、セッション作成時に渡している (`vulkan_video_decoder.cpp:397-438`)

**問題点:**

**[重大] DPBイメージのVkImageAspectFlagが不正確**
```cpp
// vulkan_video_decoder.cpp:762
view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
```
NV12（G8_B8R8_2PLANE_420_UNORM）フォーマットのイメージに対して `VK_IMAGE_ASPECT_COLOR_BIT` を使用しているが、マルチプレーンフォーマットでは `VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT` を使用するか、またはVulkan Videoのデコード操作では `VK_IMAGE_ASPECT_COLOR_BIT` でも許可される場合がある。ただし、一部のドライバでは問題が発生する可能性がある。UnifiedDecoder側（`unified_decoder.cpp:309,330`）では `VK_IMAGE_ASPECT_PLANE_0_BIT` / `VK_IMAGE_ASPECT_PLANE_1_BIT` を正しく使用しているため、不整合が存在する。

**[重大] decodeSlice()でNALヘッダーをスキップせずにRBSPパース**
```cpp
// vulkan_video_decoder.cpp:1182
std::vector<uint8_t> rbsp = parser.removeEmulationPrevention(nal_data, nal_size);
```
`nal_data` にはスタートコード付きのNALデータが渡されうるが、`removeEmulationPrevention()` をスタートコードごと適用している。`nal.data` はスタートコードを含むポインタ (`h264_parser.cpp:45`) であるため、decodeSlice() に `nal.data` / `nal.size` が渡された場合、RBSPパース結果が不正確になる可能性がある。ただし実際の呼び出し元 (`vulkan_video_decoder.cpp:1063`) を見ると `nal.data` を渡しており、さらに L1178 で `nal_data[0] & 0x1F` としてNAL typeを読んでいるが、これはスタートコード部分を読んでしまう。

**[中] decodeAccessUnit()がdecode()を内部で再呼び出し — ダブルロック**
```cpp
// vulkan_video_decoder.cpp:1080-1087
std::vector<DecodeResult> VulkanVideoDecoder::decodeAccessUnit(...) {
    std::vector<DecodeResult> results;
    auto result = decode(data, size, pts);  // decode() also takes decode_mutex_
```
`decodeAccessUnit()` は `decode()` を呼び出すが、`decode()` は `decode_mutex_` をロックする。しかし `decodeAccessUnit()` 自体はロックしないため、外部から同時に `decode()` と `decodeAccessUnit()` が呼ばれた場合の整合性は保証されない。一方で、`decode()` 内で mutex を取得するため、デッドロックは発生しない。

**[中] flush() と decode() のダブルロック問題**
```cpp
// vulkan_video_decoder.cpp:1089-1090
std::vector<DecodeResult> VulkanVideoDecoder::flush() {
    std::lock_guard<std::mutex> lock(decode_mutex_);
```
flush() は decode_mutex_ をロックするが、decode() も decode_mutex_ をロックする。同一スレッドから呼ぶ場合は問題ないが、flush() 内部で reorder buffer の output 時に `frame_callback_` を呼び出しており、コールバック内で再度 decode() が呼ばれるとデッドロックする可能性がある。

### 1.2 DPB管理

**良い点:**
- MMCO操作 (1-6) が全て実装されている (`vulkan_video_decoder.cpp:1635-1801`)
- スライディングウィンドウ参照ピクチャ管理が正しく実装されている
- IDR処理時のDPBクリアとPOCリセットが適切

**問題点:**

**[重大] applyRefPicMarking() 内の static 変数**
```cpp
// vulkan_video_decoder.cpp:1667
static int32_t max_long_term_frame_idx = -1;
```
`max_long_term_frame_idx` が `static` で宣言されている。これはインスタンスをまたいで共有されるため、複数の `VulkanVideoDecoder` インスタンスが同時に存在する場合に不正な動作を引き起こす。メンバー変数にすべき。

**[中] acquireDpbSlot() の強制再利用ロジック**
```cpp
// vulkan_video_decoder.cpp:818-821
// Force reuse oldest reference
dpb_slots_[0].reset();
dpb_slots_[0].in_use = true;
return 0;
```
全スロットが参照フレームとして使用中の場合、常にスロット0を強制的に再利用する。これは最も古い参照ではなく、単に最初のスロットを選ぶだけであり、デコード品質に影響する可能性がある。POCまたはframe_numベースで最古のものを選ぶべき。

### 1.3 NALパーサ品質

**良い点:**
- Annex-B パーサが3バイトと4バイトのスタートコードを正しく処理
- エミュレーション防止バイト除去が正確 (`h264_parser.cpp:88-104`)
- BitstreamReader がExp-Golomb (UE/SE) を正しく実装
- SPS/PPS パーサが High Profile 拡張に対応
- VUI パラメータ、HRD パラメータ、スケーリングリストの完全なパース
- MMCO コマンドの完全なパース (`h264_parser.cpp:492-539`)

**問題点:**

**[軽微] BitstreamReader にバッファオーバーラン保護が不十分**
```cpp
// h264_parser.hpp:187-195
uint32_t readBit() {
    if (byte_pos_ >= size_) return 0;  // 末端超過時は0を返す
```
末端超過時にエラーフラグを設定せず静かに0を返す。破損したNALデータの場合、静かに不正なパースが進行する可能性がある。readUE() のリーディングゼロカウントが32に制限されている点 (`h264_parser.hpp:161`) は良い防御。

**[軽微] H264SliceHeader::is_idr() の判定が不正確**
```cpp
// h264_parser.hpp:146
bool is_idr() const { return slice_type == 2 || slice_type == 7; }
```
IDRかどうかは `nal_unit_type == 5` で判定すべきであり、slice_type (I/SI) とは無関係。I-sliceであってもIDRでないことは一般的。実際にはこのメソッドはコード中で使用されていないため実害は少ない。

### 1.4 POC計算

**良い点:**
- pic_order_cnt_type 0/1/2 の全3タイプが実装されている (`vulkan_video_decoder.cpp:1500-1633`)
- POC Type 0 のMSBラップアラウンド検出が仕様通り (ITU-T H.264 8.2.1.1)
- POC Type 1 の expected_delta_per_poc_cycle 計算が正しい
- POC Type 2 の temp_poc 計算が正しい

**問題点:**

**[中] POC Type 0 で nal_ref_idc の確認が省略されている**
```cpp
// vulkan_video_decoder.cpp:1534
// Note: Should check nal_ref_idc, but we assume all decoded frames are reference
prev_poc_msb_ = poc_msb;
prev_poc_lsb_ = poc_lsb;
```
コメントにある通り、`nal_ref_idc` のチェックが省略されている。非参照ピクチャ（B-sliceなど）の場合、`prev_poc_msb_/prev_poc_lsb_` を更新してはならない。これにより、B-frameを含むストリームで POC 値が狂う可能性がある。

**[中] POC Type 1 で非参照ピクチャの offset_for_non_ref_pic が適用されていない**
```cpp
// vulkan_video_decoder.cpp:1581
// Apply offset_for_non_ref_pic for non-reference pictures
// (We assume reference picture here)
```

---

## 2. リソース管理 (82/100)

### 2.1 Vulkanオブジェクトの生成/破棄

**良い点:**
- `destroy()` メソッドが全クラスで一貫して実装されている
- VulkanVideoDecoder の `destroyVideoSession()` がセッションパラメータ→DPB→セッションメモリ→セッションの正しい順序で破棄
- YuvConverter の `destroy()` がパイプライン、デスクリプタ、サンプラ、コマンドプールの全リソースを適切に解放 (`yuv_converter.cpp:100-159`)
- VulkanImage の `destroy()` がpersistently mappedメモリを正しくunmapしてから解放 (`vulkan_image.cpp:287-303`)
- FrameResources のビットストリームバッファが `createFrameBitstreamBuffer()` で正しくリサイズ（既存バッファの破棄→新規作成）
- VulkanTexture がデストラクタ内で `destroy()` を呼び出す
- VulkanComputeProcessor の `shutdown()` が正しい順序で解放

**問題点:**

**[中] VulkanTexture::create() でエラー時のリソースリーク**
```cpp
// vulkan_texture.cpp:36
vkBindImageMemory(dev, image_, memory_, 0);
```
`vkBindImageMemory` の戻り値チェックがなく、失敗時にimage_とmemory_が解放されない。後続の処理でも同様のパターンがあり、エラーパスで部分的にリソースがリークする可能性がある。

**[中] VulkanImage::create() でも同様の問題**
```cpp
// vulkan_image.cpp:53
vkBindImageMemory(dev, image_, memory_, 0);
```
`vkBindImageMemory` の戻り値チェックなし。

**[軽微] VulkanContext::shutdown() が単一行に詰め込まれている**
```cpp
// vulkan_context.cpp:188
if (device_) { vkDeviceWaitIdle(device_); vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
```
可読性が低いが、機能的には問題ない。

### 2.2 メモリリーク可能性

**良い点:**
- `vkDeviceWaitIdle()` が `destroy()` の先頭で呼ばれ、GPU操作完了を待機してから解放
- RAII パターンがデストラクタで `destroy()` を呼ぶことで実現されている
- DPB スロットのメモリ管理が `allocateDpbSlots()`/`freeDpbSlots()` で対称的
- ビットストリームバッファのpersistent mapが `destroy()` で正しくunmap/free

**問題点:**

**[中] VulkanVideoDecoder::destroy() でのダブルロック可能性**
```cpp
// vulkan_video_decoder.cpp:320-321
void VulkanVideoDecoder::destroy() {
    std::lock_guard<std::mutex> lock(decode_mutex_);
```
デストラクタ(`~VulkanVideoDecoder()`) から `destroy()` が呼ばれるが、もし別スレッドが `decode()` でmutexを保持中の場合、デストラクタがブロックされる。これ自体は設計として合理的だが、`destroy()` が明示的に呼ばれた後にデストラクタが再度呼ばれた場合、`initialized_` フラグの確認により二重解放は防止されている。

---

## 3. エラーハンドリング (72/100)

### 3.1 VkResult チェック

**良い点:**
- 主要なVulkan API呼び出しの戻り値チェックが実施されている:
  - `vkCreateInstance`, `vkCreateDevice`, `vkCreateCommandPool`, `vkCreateSemaphore` 等
  - ビデオセッション作成、メモリバインド、ビットストリームバッファ作成
- `vkQueueSubmit` の失敗時にリソース解放が行われている (`vulkan_video_decoder.cpp:1407-1413`)

**問題点:**

**[重大] 複数箇所で VkResult チェック漏れ**

```cpp
// vulkan_texture.cpp:97
vkAllocateCommandBuffers(dev, &cai, &cmd);
// 戻り値未チェック
```

```cpp
// vulkan_texture.cpp:134
vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
// 戻り値未チェック
```

```cpp
// vulkan_image.cpp:157
vkAllocateCommandBuffers(dev, &cai, &cmd);
// 戻り値未チェック
```

```cpp
// vulkan_image.cpp:184
vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
// 戻り値未チェック
```

```cpp
// vulkan_compute_processor.cpp:176
vkAllocateCommandBuffers(ctx_->device(), &cai, &cmd);
// 戻り値未チェック
```

```cpp
// vulkan_template_matcher.cpp:129
vkAllocateCommandBuffers(dev, &allocInfo, &cmd_buf_);
// 戻り値未チェック
```

```cpp
// vulkan_template_matcher.cpp:133
vkCreateFence(dev, &fenceCI, nullptr, &fence_);
// 戻り値未チェック
```

**[中] YuvConverter の synchronous convert() がfence未使用**
```cpp
// yuv_converter.cpp:579-584
if (vkQueueSubmit(compute_queue_, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
    ...
}
vkQueueWaitIdle(compute_queue_);
```
`vkQueueWaitIdle()` は全キュー操作の完了を待つため、他のコマンドも待たされる。パフォーマンス上は `VkFence` の使用が望ましい。

### 3.2 例外安全性

**良い点:**
- コードベース全体で例外を使用していない（C-style エラーハンドリング）
- `std::unique_ptr` の使用により、途中でのreturn時にもリソースリークを防止
- FFmpegDecoder ラッパーでの `std::make_unique` 使用

**問題点:**

**[中] H264Parser のメモリアロケーション失敗ハンドリングなし**
```cpp
// h264_parser.cpp:89
rbsp.reserve(size);
```
`reserve()` がメモリ確保に失敗した場合 `std::bad_alloc` が投げられるが、呼び出し元にcatchがない。Vulkanコンテキスト破棄中にこれが発生するとリソースリークになる可能性がある。ただし実際にはメモリ不足時はシステム全体が不安定なため、実用上のリスクは低い。

---

## 4. スレッドセーフティ (70/100)

### 4.1 mutex/atomic 使用の適切さ

**良い点:**
- `decode_mutex_` が `decode()`, `destroy()`, `initialize()`, `flush()` で一貫して使用されている
- `frames_decoded_` / `errors_count_` が `std::atomic<uint64_t>` で宣言されている（VulkanVideoDecoder, UnifiedDecoder）
- YuvConverter が `convert_mutex_` を使用
- Timeline semaphore によるフレームインフライトの同期が適切 (`vulkan_video_decoder.cpp:969-998`)

**問題点:**

**[重大] UnifiedDecoder::onVulkanFrame() / onFFmpegFrame() が mutex 未保護**
```cpp
// unified_decoder.cpp:359-413
void UnifiedDecoder::onVulkanFrame(...) {
    current_width_ = width;    // mutex なし
    current_height_ = height;  // mutex なし
    ...
    frames_decoded_++;         // atomic だが、周囲のコードは非atomic
}
```
`onVulkanFrame()` と `onFFmpegFrame()` はコールバックとして呼び出されるが、`decode_mutex_` を取得していない。一方で `decode()` はmutexを保持した状態でVulkanVideoDecoder::decode()を呼び、そのコールバックチェーンでonVulkanFrame()が呼ばれる。つまり実質的にはdecode_mutex_保護下にあるが、API設計として明示的でない。

しかし `UnifiedDecoder::setFrameCallback()` (`unified_decoder.cpp:280-282`) はmutex保護なしで `frame_callback_` を変更するため、`decode()` 実行中にコールバックが変更された場合にデータ競合が発生する可能性がある。

**[重大] VulkanVideoDecoder の frame_callback_ がスレッドセーフでない**
```cpp
// vulkan_video_decoder.hpp:194
void setFrameCallback(FrameCallback callback) { frame_callback_ = std::move(callback); }
```
`decode_mutex_` なしで `frame_callback_` を変更可能。`decode()` が別スレッドで実行中にコールバックが変更された場合、未定義動作が発生する。

**[中] VulkanComputeProcessor / VulkanTemplateMatcher がスレッドセーフでない**
コメント (`vulkan_compute_processor.hpp:19`) で "Thread-safe" と記載されているが、実際にはmutexやatomicが使用されていない。コマンドバッファやフェンスが単一インスタンスで管理されており、同時呼び出しはデータ競合を引き起こす。

**[中] H264Decoder (FFmpeg) がスレッドセーフでないことの明記**
```cpp
// h264_decoder.hpp:24
// * Thread Safety:
// * - The decoder itself is NOT thread-safe
```
適切にドキュメント化されている点は良い。ただし UnifiedDecoder がこれを使用する際の同期は decode_mutex_ に依存しており、これは適切。

---

## 5. コンピュートシェーダ品質 (85/100)

### 5.1 YUV変換の正確性

**良い点:**
- BT.601 / BT.709 の両方の色空間マトリックスが実装されている
- プッシュ定数による動的色空間選択
- NV12の2プレーン構造（Y + インターリーブUV）の正しい処理
- スタジオスウィング (Y: [16,235], UV: [16,240]) の適切な処理
- UV座標の正規化とサンプラーによるバイリニア補間

**問題点:**

**[中] YUV変換マトリックスの精度**
```glsl
// yuv_to_rgba.comp:28-38
const mat3 YUV_TO_RGB_BT601 = mat3(
    1.164,  1.164,  1.164,
    0.000, -0.392,  2.017,
    1.596, -0.813,  0.000
);
```
マトリックスの配置が列優先（GLSL のデフォルト）であることを考慮すると、正しく見える。ただしBT.601の公式係数は:
- Cr→R: 1.5960（一致）
- Cb→B: 2.0172（近似値、正確には 2.0172）
- Cr→G: -0.8130（近似値、正確には -0.81300）
- Cb→G: -0.3917（近似値、正確には -0.39173）

近似値の使用は実用上問題ないが、BT.709の係数も同様に近似。

**[軽微] yuv_vec の構成順序**
```glsl
// yuv_to_rgba.comp:69
vec3 yuv_vec = vec3(y, uv_adj.x, uv_adj.y);
```
NV12のUV平面はU(Cb)がRチャネル、V(Cr)がGチャネルに格納される。マトリックスの構成 (列2: Cr→R係数, 列1: Cb→G/B係数) と合わせて正しい。

### 5.2 テンプレートマッチングアルゴリズム

**良い点:**
- NCC (正規化相互相関) の正しい数学的実装
- 分散チェック (`denom_s > 1.0 && denom_t > 1.0`) による除算ゼロ防止
- 5点分散ベースの早期終了 (Optimization G)
- テンプレートの共有メモリキャッシュ (Optimization C)
- タイルベースのソース画像読み込み (Optimization B)
- SAT (Summed Area Table) によるO(1)領域和計算 (Optimization E)
- ピラミッドベースのcoarse-to-fineマッチング
- `atomicAdd` によるスレッドセーフな結果書き込み

**問題点:**

**[中] NCC シェーダの共有メモリサイズ制限**
```glsl
// template_match_ncc.comp:51-52
shared float s_src[64][64];  // 64*64*4 = 16KB
shared float s_tpl[48][48];  // 48*48*4 = 9.2KB
```
合計約25KBで、Vulkanの最小保証48KBに収まるが、テンプレートサイズが48x48を超える場合は対応できない。`sat_max_tpl_size = 48` の設定と一致しているが、大きなテンプレートが要求された場合のフォールバックが不明確。

**[軽微] 早期終了の閾値がハードコード**
```glsl
// template_match_ncc.comp:95
if (maxDiff < 0.03) {
    s_skip = 1;
}
```
0.03というマジックナンバー。プッシュ定数で設定可能にすべきだが、実用上この値は合理的。

### 5.3 プレフィックスサム (SAT)

**良い点:**
- 水平→垂直の2パス構成で正しいSATを構築
- ストライプベースの並列プレフィックスサム
- `memoryBarrierImage()` による適切なメモリ同期
- 2つのSAT (sum, sum_of_squares) を1回のコマンドバッファ送信で構築

**問題点:**

**[中] プレフィックスサムの精度問題**
```glsl
// prefix_sum_horizontal.comp:35
float val = imageLoad(inputImage, ivec2(col, row)).r * 255.0;
```
`float` (32-bit) でプレフィックスサムを累積。1920x1080の画像の場合、1行の最大値は `255 * 1920 = 489,600`。全画像のSATでは最大値が `255 * 1920 * 1080 ≈ 530M`。float の有効桁数は約7桁であり、この値の範囲（9桁）では精度損失が発生する。大きな画像ではNCC計算の精度が低下する可能性がある。`R32_SFLOAT` の代わりに `R32_UINT` やダブルバッファリングの検討が必要。

**[中] 垂直パスでの in-place 操作**
```glsl
// prefix_sum_vertical.comp:16
layout(set = 0, binding = 0, r32f) coherent uniform image2D satImage;  // in-place
```
同一イメージに対する読み書きは `coherent` 修飾子で対応しているが、異なるスレッド間での同一ピクセルへのアクセスが保証されるかはGPU実装依存。Phase 1-2-3 の `barrier()` / `memoryBarrierImage()` で対処しているが、大きな画像では `rows_per_thread > 1` となるため、同一ストライプ内のフェーズ間同期は正しいが、異なるストライプ間でのメモリ可視性が問題になる可能性がある。

### 5.4 ワークグループ最適化

**良い点:**
- 全シェーダで16x16 (=256スレッド) のワークグループサイズを使用。Vulkanの最低保証(128)を超えるが、現代のGPUでは標準的
- ディスパッチ計算が正しい: `(width + 15) / 16`
- プレフィックスサムシェーダは行/列あたり1ワークグループ(256スレッド)を使用

**問題点:**

**[軽微] RGBA→Gray のワークグループ最適化余地**
```glsl
// rgba_to_gray.comp:7
layout(local_size_x = 16, local_size_y = 16) in;
```
共有メモリを使用していない単純なピクセル処理であり、32x8 や 8x32 など、GPUのwavefront/warpサイズに合わせた方がメモリアクセスパターンが改善される可能性がある。ただし現状でも十分実用的。

---

## 6. UnifiedDecoder設計 (80/100)

### 6.1 2階層フォールバック設計

**良い点:**
- Tier 1 (Vulkan Video) → Tier 2 (FFmpeg HW) → Tier 3 (FFmpeg SW) の3段階フォールバック
- ランタイム検出による自動バックエンド選択
- `DecoderBackend` enumによる明確なバックエンド識別
- `DecodedFrame` 構造体がVulkan/FFmpeg両パスのデータを統合的に表現
- NV12→RGBA変換パイプライン（VulkanVideoDecoder → YuvConverter）の統合
- plane viewのキャッシュ最適化 (`createPlaneViews()` で同一イメージ/サイズならスキップ)

**問題点:**

**[重大] Tier 2/3 間のフォールバックが不完全**
```cpp
// unified_decoder.cpp:78-88
if (config_.allow_ffmpeg_fallback) {
    if (initializeFFmpeg()) {
        backend_ = config_.enable_hw_accel ? DecoderBackend::FFmpegHW : DecoderBackend::FFmpegSW;
```
`enable_hw_accel` フラグでTier 2 (HW) か Tier 3 (SW) かを選択しているが、H264Decoder::init() は内部でHW→SWのフォールバックを自動で行う (`h264_decoder.cpp:86-107`)。そのため、`enable_hw_accel = true` で初期化しても実際にはSWにフォールバックしている場合があり、`backend_` の値が実際のバックエンドと一致しない可能性がある。

**[中] decode() の戻り値がVulkan/FFmpegで意味が異なる**
```cpp
// unified_decoder.cpp:195-196
auto result = vulkan_decoder_->decode(nal_data, nal_size, pts);
success = result.success;  // Vulkan: デコード成功

// unified_decoder.cpp:206-207
int frames = ffmpeg_decoder_->decode(nal_data, nal_size);
success = (frames > 0);    // FFmpeg: フレーム出力があった
```
Vulkan側は「デコード命令の送信に成功」を返すのに対し、FFmpeg側は「フレームが出力された」を返す。SPS/PPSのみのNALユニットではFFmpeg側は `frames == 0` となりerrors_count_が増加する。

**[中] FFmpegDecoder ラッパーの型変換**
```cpp
// unified_decoder.cpp:28
decoder->set_frame_callback([cb](const uint8_t* data, int w, int h, uint64_t pts) {
    cb(data, w, h, static_cast<int64_t>(pts));
});
```
`uint64_t pts` → `int64_t pts` の変換で、`UINT64_MAX` を超える値（理論上存在しないが）で符号が反転する。

**[軽微] runtime downgrade 時のログ出力が不十分**
Vulkan Video が初期化に成功した後に、ランタイムエラーで FFmpeg にフォールバックする機能が未実装。一度バックエンドが決定されると、デコードエラーが続いても自動切り替えは行われない。

---

## 7. Vulkan基盤レイヤー (追加評価)

### 7.1 VulkanContext

**良い点:**
- Vulkan 1.3 API使用
- デバッグメッセンジャーが `_DEBUG` ビルド時のみ有効
- Vulkan Video キューファミリの自動検出
- 専用コンピュートキュー / 専用転送キュー の優先検索

**問題点:**

**[中] VulkanContext のデストラクタが shutdown() を呼ばない**
```cpp
// vulkan_context.hpp:29
~VulkanContext() = default;
```
明示的に `shutdown()` を呼ばなければリソースがリークする。他のクラス (VulkanTexture, VulkanImage 等) はデストラクタで `destroy()` を呼んでいるため、不整合。

**[中] デバイス拡張のサポート確認なし**
```cpp
// vulkan_context.cpp:153-165
std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};
```
拡張の存在確認なしで要求しているため、非対応デバイスで `vkCreateDevice` が失敗する。Vulkan Video 拡張も同様に確認なしで追加される。

### 7.2 VulkanSwapchain

**良い点:**
- MAILBOX → FIFO のプレゼントモードフォールバック
- currentExtent の特殊値 (UINT32_MAX) の正しい処理
- recreate() でのold swapchain の正しい処理

### 7.3 VulkanTexture

**問題点:**

**[中] update() で毎フレーム VkCommandBuffer を確保/解放**
```cpp
// vulkan_texture.cpp:94-137
VkCommandBuffer cmd;
vkAllocateCommandBuffers(dev, &cai, &cmd);
...
vkFreeCommandBuffers(dev, cmd_pool, 1, &cmd);
```
パフォーマンス上、事前確保したコマンドバッファの再利用が望ましい。`vkQueueWaitIdle()` による同期も、フェンスベースの方が他のキュー操作をブロックしない。

### 7.4 VulkanImage

**良い点:**
- Persistently mapped staging buffer の使用 (Optimization D)
- `upload()` / `download()` の対称的なAPI
- `transitionLayout()` が一般的なレイアウト遷移を網羅
- `uploadFromStaging()` によるゼロコピーアップロードサポート

---

## 8. ファイル別詳細評価

| ファイル | スコア | 主な問題 |
|---------|--------|---------|
| vulkan_video_decoder.hpp | 88 | NALUnitType 定義の重複（h264_parser.hppとの前方宣言の不整合） |
| vulkan_video_decoder.cpp | 72 | static変数, DPB aspectMask, NALデータオフセット問題 |
| h264_parser.hpp | 90 | is_idr()の判定ロジック, BitstreamReaderのエラーフラグ欠如 |
| h264_parser.cpp | 88 | 堅実な実装, parseAnnexBのデータポインタ計算がやや複雑 |
| yuv_converter.cpp | 80 | fopen使用, VkQueueWaitIdle, 同期/非同期パスのコード重複 |
| yuv_converter.hpp | 90 | 明確なAPI設計 |
| unified_decoder.cpp | 78 | Tier2/3フォールバックの不正確さ, コールバック安全性 |
| unified_decoder.hpp | 85 | 良好な抽象化 |
| h264_decoder.cpp | 85 | 堅実なFFmpegラッパー, 適切なHWフォールバック |
| h264_decoder.hpp | 90 | 明確なドキュメントとスレッド安全性の注記 |
| vulkan_context.cpp | 78 | デストラクタ問題, 拡張確認なし |
| vulkan_context.hpp | 85 | 明確なAPI |
| vulkan_swapchain.cpp | 82 | 全体的に良好 |
| vulkan_swapchain.hpp | 88 | 簡潔な設計 |
| vulkan_texture.cpp | 72 | VkResult未チェック, 毎フレームCmdBuf確保 |
| vulkan_texture.hpp | 85 | 簡潔 |
| vulkan_compute.cpp | 85 | 堅実な汎用パイプライン |
| vulkan_compute.hpp | 88 | 明確なAPI |
| vulkan_image.cpp | 82 | vkBindImageMemory未チェック |
| vulkan_image.hpp | 88 | persistently mapped bufferの良い抽象化 |
| vulkan_compute_processor.cpp | 78 | スレッドセーフでない |
| vulkan_compute_processor.hpp | 80 | Thread-safeの虚偽記載 |
| vulkan_template_matcher.cpp | 75 | goto使用, 複雑な制御フロー |
| vulkan_template_matcher.hpp | 82 | SATの良い抽象化 |
| yuv_to_rgba.comp | 88 | 正確なYUV変換 |
| rgba_to_gray.comp | 90 | シンプルで正確 |
| template_match_ncc.comp | 85 | 良い最適化, 共有メモリ活用 |
| template_match_sat.comp | 82 | SAT活用は巧み, 精度問題 |
| pyramid_downsample.comp | 90 | シンプルで正確 |
| prefix_sum_horizontal.comp | 78 | 精度問題, coherentの使い方 |
| prefix_sum_vertical.comp | 78 | in-place操作の安全性 |

---

## 9. 改善提案（優先度順）

### P0 (重大 — 即座に対処)
1. **`applyRefPicMarking()` の static 変数をメンバー変数に変更** — マルチインスタンス時のバグ
2. **`setFrameCallback()` にmutex保護を追加** — データ競合の防止
3. **DPBイメージビューのaspectMaskを検証** — ドライバ互換性

### P1 (高 — 次のイテレーションで対処)
4. **POC Type 0 で nal_ref_idc に基づく prev_poc の更新制御** — B-frame対応
5. **VulkanContext にデストラクタでの shutdown() 呼び出しを追加**
6. **VkResult チェックの追加**（特に vkAllocateCommandBuffers, vkQueueSubmit, vkBindImageMemory）
7. **VulkanComputeProcessor のThread-safeコメントを修正、またはmutex追加**

### P2 (中 — 計画的に対処)
8. **acquireDpbSlot() の強制再利用をPOCベースに変更**
9. **SAT のfloat精度問題の検討** (大画像対応)
10. **decodeSlice() の NAL データオフセット修正**
11. **FFmpegバックエンド判定の修正**（実際のHW使用状態の反映）
12. **VulkanTexture::update() のコマンドバッファ再利用化**

### P3 (低 — 将来の改善)
13. **BitstreamReader にエラーフラグ追加**
14. **convert() / convertAsync() のコード重複削減**
15. **VulkanTemplateMatcher の goto 文をリファクタリング**

---

## 10. 総評

MirageVulkanプロジェクトは、Vulkan Video H.264 デコード、GPUベースYUV変換、テンプレートマッチングを含む意欲的な実装である。VK_KHR_video_decode_h264の使用法は概ね正確であり、DPB管理、POC計算、MMCO操作の実装は標準仕様に忠実である。H264Parserの品質は高く、High Profile拡張やVUIパラメータまでカバーしている。

コンピュートシェーダの最適化（タイルベースNCC、共有メモリキャッシュ、SAT、ピラミッドマッチング）は設計として優れており、GPU性能を効果的に活用している。UnifiedDecoderの3段階フォールバック設計もアーキテクチャとして合理的。

主な改善点は、スレッドセーフティの強化（特にコールバック関連）、static変数のインスタンス化、VkResultチェックの徹底、B-frame対応のPOC計算修正である。これらの修正により、80点台後半のスコアに到達可能と評価する。
