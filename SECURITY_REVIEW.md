# Security Review: nvvaapi-work

- 対象: `/home/khyt/nvvaapi-work`
- 対象コミット: `223c7e9` (`integrate/all-fixes`)
- 実施日: 2026-07-13 (Asia/Tokyo)
- 方式: 手動コードレビュー、入力経路追跡、`clang-tidy`、通常ビルド、ASan/UBSan ビルド、NVIDIA GPU 実機スモークテスト
- 結論: **現状を「敵対的な動画／VA-API 入力に対してメモリ安全」とは判断できない。修正前の一般配布、とくに Firefox の RDD sandbox を無効化した利用は推奨しない。**

## 0. 今回の初回修正（working tree）

以下の局所的な修正を実装済みです。いずれも正常経路の属性内容・対応形式を変更せず、異常入力時の拒否／安全な継続だけを追加しています。

- **NVSEC-05:** `vaQuerySurfaceAttributes` が入力容量を保持し、容量不足では `VA_STATUS_ERROR_MAX_NUM_EXCEEDED` を返すようにした。件数問い合わせ（`attrib_list == NULL`）も明示的に処理する。
- **NVSEC-10（一部）:** NVIDIA バージョン文字列の NULL、形式、数値範囲を検証してから使用するようにした。direct/EGL backend の optional `EGL_KHR_debug` callback は未取得時に呼び出さない。
- **NVSEC-01（一部）:** buffer type を作成時と dispatch 前に検証し、要素サイズと個数を `size_t` の checked multiply で計算するようにした。buffer data allocation failure 時は作成済み object を削除し、出力 ID を `VA_INVALID_ID` のままにする。HEVCはVAの`ReferenceFrames[0..14]`をNVDECのreference slot 0..14へ対応させ、slot 15を未使用として初期化することで、member境界を越える未定義動作と`CurrPic`混入を除去した。

各修正後に通常ビルドと RTX 5080 上の `vainfo` を実行し、最後に H.264/HEVC/VP9/AV1/MJPEG の hardware decode smoke test、Clang ASan/UBSan ビルド、容量不足 API と buffer validation の専用テストを実行した。NVSEC-01 の codec/slice validation と、NVSEC-02〜04、NVSEC-06〜10 の未記載部分は未修正である。

## 1. エグゼクティブサマリー

このドライバは、Web やメディアファイル由来のデータを FFmpeg/Chromium 等が VA-API 構造体へ変換した後、それを C コードと NVIDIA のユーザー／カーネル API に渡す。したがって、上流パーサが正常でも、ドライバ自身が次の境界を検証する必要がある。

1. `vaCreateBuffer` で受け取る buffer type、要素サイズ、要素数
2. codec ごとの picture/slice parameter の値域
3. slice offset/size と実際の slice data buffer の関係
4. DMA-BUF の object/layer/plane、pitch、offset、size、width/height
5. 非同期 resolve thread と surface/context の寿命

現状はこれらの検証が不十分で、境界外読み取り、書き込み先サイズを超える GPU/CPU copy、未定義動作、終了時 use-after-free、無限待ちへ到達できるコード経路がある。特に優先すべきものは以下。

| ID | 重大度 | 要点 | 主な到達条件 |
|---|---|---|---|
| DEPLOY-01 | High | README が Firefox RDD sandbox の無効化を指示 | Web 動画を Firefox で再生 |
| NVSEC-01 | High（一部修正） | VA buffer/codec parameter/slice 範囲を中央で検証していない | 破損動画から不正な VA 値が伝播、または不正 VA client |
| NVSEC-02 | High | 外部 DMA-BUF の plane 境界と算術を検証していない | PRIME/PRIME_2 surface import、VideoProc、decode copy |
| NVSEC-03 | Medium | `vaGetImage` がコピー領域と出力 image buffer の対応を検証しない | 不正または誤った `vaGetImage` 呼び出し |
| NVSEC-04 | High | resolve thread/context/surface の終了順と寿命が安全でない | 同時終了、CUDA stall、未破棄 object を伴う `vaTerminate` |
| NVSEC-05 | Medium（初回修正済み） | `vaQuerySurfaceAttributes` が呼び出し側の配列容量を無視 | 小さい出力配列を渡す正規 API 呼び出し |
| NVSEC-06 | Medium | 16 要素 ring queue の full 判定がなく pending surface を上書き | resolve より decode submission が速い場合 |
| NVSEC-07 | Medium | grow/alloc 失敗、整数 overflow、ゼロ長 append が fail-safe でない | メモリ圧迫、巨大／ゼロ長 buffer |
| NVSEC-08 | Medium | VP9 parser が全 context で共有され同期されない | 複数 VP9 stream の同時 decode |
| NVSEC-09 | High | CI が HTTP + `trusted=yes` の APT source を root で利用 | CI のネットワーク／配布元侵害 |
| NVSEC-10 | Medium（一部修正） | 初期化失敗経路に NULL 参照、関数 pointer 未確認、resource leak | NVIDIA/EGL API の失敗や非対応環境 |
| NVSEC-11 | Low | fuzz/unit test と明示的な binary hardening が不足 | 防御・検出能力の不足 |
| NVSEC-12 | Low | installer の `--clean` が任意パスを無条件 `rm -rf` | 誤った `BUILD_DIR` / `--build-dir` |

重大度は「このドライバをブラウザ／メディア処理経路で使う場合の影響」を基準にしたもので、CVSS ではない。悪意ある同一プロセスの VA client は既にそのプロセス内でコードを実行できるため、それだけを前提とする項目は到達性を低く評価している。一方、codec field の多くは非信頼メディアから間接的に作られるため、上流パーサの検証だけに依存しない方針が必要である。

## 2. Trust boundary と処理経路

他の agent は、まず次の 3 経路を追うと全体像を把握しやすい。

```text
compressed media
  -> FFmpeg / Chromium parser
  -> nvCreateBuffer
  -> nvRenderPicture
  -> codec handler (h264/hevc/av1/...)
  -> nvEndPicture / cuvidDecodePicture
  -> resolveSurfaces thread
  -> direct or EGL backend
  -> CUDA array / DMA-BUF

external DMA-BUF
  -> nvCreateSurfaces2
  -> parseSurfaceImportAttributes
  -> createImportedBackingImage
  -> mmap or CUDA external-memory import
  -> VideoProc / decode copy

shutdown
  -> nvDestroyContext or nvTerminate
  -> signal/join resolve thread
  -> destroy surfaces/backing images
  -> release CUDA/NVIDIA objects
```

主要な実装位置:

- object/buffer/thread の共通処理: `src/vabackend.c`
- codec handler: `src/{h264,hevc,av1,vp8,vp9,mpeg2,vc1,jpeg}.c`
- direct backend: `src/direct/direct-export-buf.c`, `src/direct/nv-driver.c`
- EGL backend: `src/export-buf.c`
- dynamic array: `src/list.c`
- CI/install: `.github/workflows/*`, `install.sh`

## 3. 詳細 findings

### DEPLOY-01 — Firefox RDD sandbox の無効化が exploit impact を大きくする

- 重大度: **High (運用上の impact amplifier)**
- 確度: High
- 根拠: `README.md:130-151`、特に `README.md:148`

README は `MOZ_DISABLE_RDD_SANDBOX=1` を恒常設定候補として案内している。また `src/vabackend.c:264-278` は `/proc/version` を開けない環境を sandbox とみなし、`NVD_FORCE_INIT` がなければ初期化を止める。

この構成では、Web 由来の動画を処理する decoder が本来の RDD sandbox 保護を失う。本レポートで確認した memory-safety 問題、FFmpeg/GStreamer/NVIDIA driver 側の将来の問題、GPU ioctl attack surface のすべてについて、成功時の影響が sandbox 内 crash からユーザー権限での process compromise へ拡大する。

推奨:

1. README の先頭付近に明確な security warning と threat model を置く。
2. `MOZ_DISABLE_RDD_SANDBOX=1` を「通常推奨」ではなく、リスクを理解した一時的 workaround として扱う。
3. sandbox 互換の device/file access 設計または broker 経由の構成を長期目標にする。
4. 少なくとも NVSEC-01〜04 の修正と fuzzing が完了するまでは、ブラウザで非信頼動画を扱う構成を安全と表現しない。

### NVSEC-01 — VA buffer と codec parameter の包括的な validation がない

- 重大度: **High**
- 確度: High（欠落自体）、Medium（任意の破損動画から各値がそのまま到達するか）
- CWE: CWE-20, CWE-125, CWE-787, CWE-190
- 状態: **共通入口の type検証、checked multiply、allocation rollback、およびHEVC picture field境界は修正済み。その他のcodec構造体・slice範囲・semantic validationは未修正。**

共通入口の問題:

- `src/vabackend.c:1966-2013`: `num_elements * size` を 32-bit のまま計算し、overflow check がない。
- `src/vabackend.c:1984-1992`: VP8 のため `data` を最大 15 byte 前へ戻して読み取る。呼び出し側 allocation の前方が readable という VA-API 契約はない。
- `src/vabackend.c:2936-2993`: `buf->bufferType` を `VABufferTypeMax` と照合せず handler 配列の index に使う。
- `NVBuffer` は総 byte 数と要素数しか保持せず、1 要素の宣言サイズを保持しない。そのため handler 側で `elements * sizeof(expected_struct) <= size` を確認できない。
- 多くの handler は `buffer->ptr` を即座に固定構造体へ cast し、最小サイズを確認しない。

slice の問題:

- 例: `src/h264.c:91-103`, `src/hevc.c:262-274`, `src/mpeg2.c:91-100`, `src/vp9.c:152-164`, `src/vc1.c:75-84`
- `slice_data_offset` と `slice_data_size` が `buf->size` 内に入るか確認せず `appendBuffer()` の `memcpy` source にする。
- `ctx->lastSliceParams` は buffer 本体への raw pointer であり、次の API call まで保持される。全 codec 共通で `nvBeginPicture` 時に reset されず、buffer の破棄、順序違反、parameter 欠落で stale/UAF pointer になり得る。JPEG だけは `resetJPEGPictureState()` で reset している。

codec 固有の代表例:

- HEVC `src/hevc.c:173-214`: `num_tile_columns_minus1` / `num_tile_rows_minus1` を VA/CUVID の固定配列上限と照合せず loop bound にする。
- HEVC（修正済み）: 修正前の`src/hevc.c:223-251`は`&buf->CurrPic`を16要素配列のように扱い、NVDECのreference slot 0へ現在画像を混入させていた。現在はVAの`ReferenceFrames[0..14]`をNVDECのslot 0..14へ対応させ、slot 15を未使用として初期化している。`CurrPic.pic_order_cnt`はNVDECの独立した`CurrPicOrderCntVal`へ設定する。
- AV1 `src/av1.c:147-174`: `bit_depth_map[buf->bit_depth_idx]` の index check がない。
- AV1 `src/av1.c:29-34`: `use_superres` 時の denominator 0 を拒否せず除算する。
- AV1 `src/av1.c:239-283`, `src/av1.c:425-447`: `ref_frame_idx[]` / `primary_ref_frame` を 0..7 と検証せず固定配列 index に使う。
- AV1 `src/av1.c:379-391`: `tile_cols` / `tile_rows` を 64 以下と検証せず固定配列へ書く。
- VP8 `src/vp8.c:36-75`: `buf->ptr[0]` および `sliceData[3..5]` を最小長確認なしで読む。

影響:

- user-space crash、hang、境界外読み取り
- `CUVIDPICPARAMS` 内の隣接 field の破壊と、不整合な値の NVIDIA decoder への投入
- 不正 `bufferType` による handler table 範囲外読み取り／間接 call
- 上流 parser の validation bug があった場合、その影響を driver 境界で止められない

推奨修正:

1. `NVBuffer` に `elementSize` を追加し、`size_t` の checked multiply で総サイズを計算する。
2. `type < 0 || type >= VABufferTypeMax` を `nvCreateBuffer` と dispatch 前の両方で拒否する。
3. codec/profile/type ごとの `expected_element_size` と `max_elements` table を作り、handler 呼び出し前に中央で検証する。
4. `slice_offset <= data_size && slice_size <= data_size - slice_offset` の共通 helper を全 codec で使う。
5. slice parameters は context 所有 memory に copy するか、同一 `nvRenderPicture` call 内だけで組み合わせ、raw pointer を call 間で保持しない。
6. AV1/HEVC の spec range と CUDA header の固定配列長の小さい方を上限にする。
7. HEVC reference mapping は修正済み。VAの`ReferenceFrames[0..14]`だけをNVDECのreference slotへ設定し、`CurrPic`は独立したcurrent-picture fieldとして扱う。
8. validation failure を handler の `void` return で失わないよう、handler を `VAStatus` または `bool` return に変える。

### NVSEC-02 — 外部 DMA-BUF の layout と copy bounds が検証されない

- 重大度: **High**
- 確度: High
- CWE: CWE-20, CWE-125, CWE-787, CWE-190

descriptor parse:

- `src/vabackend.c:1229-1260`: `desc->num_layers` を固定 `layers[]` 長と比較せず loop する。
- 各 `layers[l].num_planes`、`object_index[p]`、object size、plane offset/pitch の組を validation していない。
- `src/vabackend.c:1241`: object size の合計が `uint32_t dataSize` で overflow し得る。
- PRIME_2 の複数 object を plane ごとの `object_index` と結び付けず flatten し、その後は主に fd 0 を map/import する。

mapping/import:

- `src/vabackend.c:1382-1457`: 呼び出し引数の width/height と descriptor 内 width/height を照合しない。
- `lseek()` の `off_t` を `uint32_t totalSize` に切り詰める (`src/vabackend.c:1409-1413`)。
- `offset + (height - 1) * pitch + row_bytes <= object_size` を確認しない。
- host mapping への decode copy は `src/direct/direct-export-buf.c:811-879`。`stagingBytes` を 32-bit 乗算し、overflow すると小さい heap buffer へ CUDA copy を要求する。また destination mapping の各 row が mapping 内か確認しない。
- CPU VideoProc も `src/vabackend.c:2665-2673` と `src/vabackend.c:2722-2724` で未検証の offset/stride を使う。
- branch 上の PTX は 32-bit packed store を使うため、外部 RGB surface の offset/pitch が 4-byte alignment を満たすことも必要だが確認していない (`src/kernels.c:105-150`, `src/kernels.c:265-308`)。

影響:

- mapped DMA-BUF 範囲外の CPU read/write、`SIGBUS`/segfault
- overflow で過小確保された host staging buffer への GPU write request
- GPU external-memory fault、process/GPU process crash
- 複数 object descriptor の誤解釈による別 plane/object へのアクセス

推奨修正:

1. `ImportedSurface` に plane ごとの `objectIndex` と object size を保存する。
2. `num_objects`, `num_layers`, `num_planes` を VA の定数と format 固有 plane 数で検証する。
3. width/height/fourcc を API 引数、descriptor、format capability の 3 者で一致確認する。
4. すべての byte 算術を `uint64_t`/`size_t` checked add/multiply で行い、最終 API 型へ入れる前に上限確認する。
5. `row_bytes <= pitch` と、最後の 1 byte まで object size 内であることを plane ごとに検証する。
6. fd 0 固定ではなく、各 plane が指定した object を個別に map/import する。
7. RGB packed store 用に offset/pitch の 4-byte alignment を検証し、満たさなければ byte-store fallback または reject する。

### NVSEC-03 — `vaGetImage` が destination allocation を超える copy を許す

- 重大度: **Medium**（memory corruption impact は High、直接到達は不正／誤用 VA call）
- 確度: High
- CWE: CWE-787, CWE-190

`nvCreateImage` は image の width/height に基づき host buffer を確保する (`src/vabackend.c:3145-3213`)。一方 `nvGetImage` は呼び出し時の `width`/`height` をそのまま `CUDA_MEMCPY2D.dstPitch`, `WidthInBytes`, `Height` と offset 加算に使い、以下を確認しない (`src/vabackend.c:3270-3330`)。

- requested rectangle が source surface 内か
- requested byte 数が destination `NVImage` の dimensions/buffer size 内か
- image format と surface backing format の plane 数が互換か
- `x`/`y`（現在は無視される）が source address に反映されるか
- offset accumulation の overflow

したがって、小さい `VAImage` に対し大きい width/height を指定すると、raw host pointer の確保サイズを超える CUDA copy request が作られる。エラー時 `cuCtxPopCurrent()` 前に return する経路もあり、CUDA context stack を不整合にする。

推奨:

1. rectangle を surface/image の両方に対して検証し、仕様に沿って crop/format conversion するか unsupported を返す。
2. image 作成時の plane offset/size を checked math で保存し、各 copy がその plane interval 内であることを検証する。
3. CUDA push/pop を single-exit cleanup で必ず対にする。

### NVSEC-04 — 非同期 resolve thread と object の寿命管理が unsafe

- 重大度: **High**
- 確度: High
- CWE: CWE-416, CWE-362

複数の独立した問題が同じ lifetime boundary にある。

1. `destroyContext()` は `pthread_timedjoin_np()` が timeout/error でも、そのまま codec buffer を free し、caller は context object を削除する (`src/vabackend.c:517-546`, `src/vabackend.c:1869-1889`)。live thread は `ctx`/`drv` を引き続き参照できる。
2. `exiting` は `volatile bool` で atomic ではなく、predicate 更新と `pthread_cond_signal()` を `resolveMutex` 保持下で行っていない。wait 開始直前の signal lost race があり、5 秒 timeout 後の free へつながる。
3. `nvTerminate()` は context thread を止める前に全 backing image を破棄する (`src/vabackend.c:3853-3863`)。resolve thread が surface/backing image を使っていれば UAF になる。
4. `deleteAllObjects()` は forward iteration 中に現在要素を削除する (`src/vabackend.c:549-558`)。配列が左 shift した後 index を increment するため、1 要素おきに skip する。skip された decode context/thread を残したまま `drv` が free され得る。
5. `getObject()` は mutex を unlock してから raw pointer を返し、多数の API entrypoint が参照中の object を pin/refcount しない (`src/vabackend.c:459-481`)。同時 destroy で UAF になり得る。
6. `nvDestroyConfig()` は ID の type を確認せず generic `deleteObject()` を呼ぶ (`src/vabackend.c:1078-1087`)。誤った type の ID で context 等を teardown なしに free できる。

推奨修正順:

1. shutdown を `stop submissions -> mark all contexts exiting under lock -> broadcast -> join all threads -> destroy surfaces/images -> release backend/CUDA` の phase に分ける。
2. join に失敗した context は絶対に free しない。失敗を返し、安全に retry/abort できる state に置く。
3. `exiting` と queue predicate を同じ mutex で保護する（または正しい atomic protocol を使う）。
4. object table に refcount/state (`LIVE`, `DESTROYING`, `DEAD`) を導入し、lookup と acquire を同じ lock 内で行う。
5. cleanup iteration は reverse、`while (size > 0)`、または一覧を別配列へ detach してから行う。
6. type-specific destructor を object table に持たせ、generic free だけで resource を解放しない。

### NVSEC-05 — `vaQuerySurfaceAttributes` が出力配列 capacity を無視する

- 重大度: **Medium**（write overflow impact は High、API-level 到達）
- 確度: High
- CWE: CWE-787
- 状態: **初回修正済み**（`src/vabackend.c`）。

VA-API 契約では、`*num_attribs` の入力値は呼び出し側が確保した要素数であり、足りなければ必要数へ更新して `VA_STATUS_ERROR_MAX_NUM_EXCEEDED` を返す。

実装は入力 capacity を保存せず、先に必要数で上書きし、その後 full list を無条件に書く。

- VideoProc: `src/vabackend.c:3494-3552`（13 または 14 要素）
- Decode: `src/vabackend.c:3577-3668`

小さいが non-NULL の `attrib_list` は、正規 API で許されるにもかかわらず caller buffer overflow になる。

推奨: entry 時に `capacity = *num_attribs` を保存し、必要数を算出後、`attrib_list == NULL` なら count のみ返す。`capacity < required` なら書かずに `VA_STATUS_ERROR_MAX_NUM_EXCEEDED` を返す。

### NVSEC-06 — resolve queue の full 判定がなく permanent wait を起こす

- 重大度: **Medium**
- 確度: High
- CWE: CWE-400

consumer は `readIdx == writeIdx` を empty と判定する (`src/vabackend.c:611-633`)。producer は full を検出せず書き込み、16 回で write index が wrap する (`src/vabackend.c:3045-3054`)。コードにも `TODO check we're not overflowing the queue` が残っている。

consumer が追い付く前に 16 surface が enqueue されると、pending entry が上書きされ、index が等しくなって queue が空に見える。上書きされた surface の `resolving` が解除されず、`vaSyncSurface()` が永久待ちになる。

推奨: `count` を持つ bounded queue、または 1 slot を空ける標準 ring protocol にする。full 時は producer を condition variable で待たせるか、VA error を返す。enqueue/dequeue と終了 predicate は同じ mutex で扱う。

### NVSEC-07 — allocator/grow 処理が異常時に fail-safe でない

- 重大度: **Medium**
- 確度: High
- CWE: CWE-190, CWE-400, CWE-690

`appendBuffer()` (`src/vabackend.c:415-431`):

- `size * 2`, `ab->size + size`, 1.5 倍 growth の overflow check がない。
- `memalign()` failure を確認せず `memcpy()` する。
- 最初の append が 0 byte で `memalign(16, 0)` が non-NULL を返すと `allocated == 0` のままになる。次の non-zero append は `allocated += allocated >> 1` が永遠に 0 で、growth loop が終了しない。
- function が `void` のため caller に allocation failure を伝えられない。

dynamic array (`src/list.c:7-42`):

- capacity growth の overflow check がない。
- `realloc()` を一時 pointer で受けず、failure 時に元 pointer を失って直後に `memset()`/write する。
- `alloc_and_add_element()` は `calloc()` failure でも NULL element を size に追加する。

object allocation (`src/vabackend.c:442-456`) も `calloc()` failure を確認せず dereference する。

推奨: checked add/multiply/grow helper を 1 箇所に実装し、すべて `bool`/`VAStatus` を返す。ゼロ長 append は no-op にする。`realloc` は一時 pointer へ受け、failure 時に state を変更しない。プロセス全体を落とさず frame 単位の allocation error として返す。

### NVSEC-08 — VP9 parser が process-global mutable state

- 重大度: **Medium**
- 確度: Medium
- CWE: CWE-362

`src/vp9.c:71-140` の `static GstVp9Parser *parser` は全 driver/context/thread で共有され、lazy initialization と parse のいずれにも lock がない。複数 VP9 stream を同時 decode すると、初期化 race、parser internal state の競合、別 stream の color state 混入が起き得る。parser は解放もされない。

推奨: parser を codec-specific context (`ctx->codecData`) に移し、context 作成／破棄で init/free する。同一 context の `nvRenderPicture` も直列化するか、parser が stateless であることを確認した上で frame-local instance を使う。ThreadSanitizer 用の複数 VP9 context test を追加する。

### NVSEC-09 — CI compiler repository が認証されない

- 重大度: **High (supply-chain)**
- 確度: High
- CWE: CWE-494

`.github/workflows/install-clang.sh:25-30` は次を行う。

```text
deb [trusted=yes] http://apt.llvm.org/... main
```

`trusted=yes` は APT signature trust check を無効化し、さらに transport は HTTP である。その repository から `sudo apt-get install` するため、network path または mirror が侵害されると任意 package maintainer script が root で CI runner 上で実行される。

`.github/workflows/ubuntu.yml:17` の `actions/checkout@v7` も commit SHA ではなく mutable tag に依存し、workflow に明示的な最小 `permissions` がない。

推奨:

1. HTTPS と公式 signing key を `signed-by=/usr/share/keyrings/...` で利用し、`trusted=yes` を削除する。
2. 可能なら runner image 内の compiler または Ubuntu signed packages を使用する。
3. GitHub Action は検証済み full commit SHA に pin し、Dependabot で更新する。
4. workflow top-level に `permissions: contents: read` を設定する。
5. 将来 artifact/release を追加する場合は provenance/SBOM と build dependency pinning を追加する。

良い点として、`subprojects/ff-nvcodec-headers.wrap:3-8` は source と patch の両方に SHA-256 を指定している。

### NVSEC-10 — 初期化失敗が安全に処理されない

- 重大度: **Medium**
- 確度: High
- CWE: CWE-476, CWE-404

代表例:

- 修正前の `src/direct/nv-driver.c:378-383` は `nv_get_versions()` の return を十分に扱わず、ioctl failure 時に `ver == NULL` のまま `atoi(ver)` を呼んでいた。`clang-tidy` の `clang-analyzer-core.NonNullParamChecker` でも再現した。**NULL／形式／範囲の検証は初回修正済み（現行コード: `src/direct/nv-driver.c:378-411`）。**
- 修正前の `src/direct/direct-export-buf.c:125-130` は `eglDebugMessageControlKHR` の NULL check がなかった。**direct backend の optional callback は初回修正済み（現行コード: `src/direct/direct-export-buf.c:125-134`）。**
- `src/export-buf.c:203-263`: 多数の EGL extension pointer を取得するが、使用前に全 required function を確認しない。**EGL の debug callback は初回修正済みだが、required extension 全体の検証は未修正。**
- `src/vabackend.c:4001-4013`: global `instances` を CUDA function availability check より先に増やし、以後の失敗経路の多くで減らさない。`NVD_MAX_INSTANCES` 使用時に再初期化不能な process-local DoS になり得る。
- `src/vabackend.c:4059-4078`: exporter/CUDA/profile query failure の cleanup が非対称で、fd/context/mutex、`ctx->pDriverData`、instance count が残り得る。
- `src/export-buf.c:571-573`: workaround 用の 2 回目 allocation failure 後に NULL `img` を dereference し得る。

推奨: init を段階化し、各成功段階を flag で記録した単一 `goto fail` cleanup に統合する。function pointer table の required entries を初期化時に一括確認する。version parse は return、NULL、文字列形式、範囲を検証する。

### NVSEC-11 — test/fuzz と binary hardening の不足

- 重大度: **Low**（直接 vulnerability ではなく assurance gap）
- 確度: High

- `meson test` は `No tests defined.`。
- 正常サンプル生成 script はあるが、境界値、破損 buffer、同時終了を検証しない。
- `meson.build:29-49` は有用な warning を有効化しているが、`-fstack-protector-strong`, `_FORTIFY_SOURCE`, full RELRO (`-z now`) 等を明示しない。
- この環境で作成した通常 binary は NX stack と GNU_RELRO を持つ一方、`BIND_NOW`、stack canary symbol、FORTIFY `_chk` symbol は確認できなかった。packager/toolchain 依存にしない hardening policy が望ましい。

推奨 test architecture:

1. CUDA/NVDEC/backend を stub 化し、VA entrypoint と codec handler を GPU なしで unit test 可能にする。
2. `nvCreateBuffer -> nvRenderPicture` の libFuzzer harness を codec ごとに作る。
3. DMA-BUF descriptor validator を pure function にし、object/layer/plane の組合せを fuzz する。
4. ASan/UBSan job、VP9/lifecycle 用 TSan jobを追加する。
5. boundary regression: zero-size first slice、overflowing element count、invalid buffer type、AV1 refs/tiles、HEVC tiles、small attribute capacity、oversized `vaGetImage`、queue full、join timeout。
6. release build に platform 対応の hardening flags を追加し、CI で ELF property を確認する。

### NVSEC-12 — installer clean target の safety guard がない

- 重大度: **Low**
- 確度: High
- 根拠: `install.sh:149-151`

`BUILD_DIR` または `--build-dir` は任意パスを受け取り、`--clean` 時にそのまま `rm -rf "$BUILD_DIR"` する。通常は非 root 実行だが、誤設定や root 実行で広範な data loss が起き得る。

推奨: `realpath -m` で canonicalize し、空文字、`/`、`$HOME`、repository root、既存 source tree を拒否する。repository 外を削除する場合は別の明示 option を要求する。

## 4. 修正優先順位

### P0 — 配布前

1. NVSEC-01: central VA buffer/type/size validation（**一部修正済み**）と全 codec の slice range check
2. NVSEC-02: DMA-BUF descriptor validator と checked layout math
3. NVSEC-03: `vaGetImage` rectangle/destination validation
4. NVSEC-04: thread join/object lifetime/termination order の再設計
5. DEPLOY-01: sandbox 無効化の警告とサポート方針の明文化

### P1 — 次の安定化サイクル

1. NVSEC-05: attribute capacity contract（**初回修正済み**）
2. NVSEC-06: bounded queue/backpressure
3. NVSEC-07: allocator failure propagation
4. NVSEC-08: per-context VP9 parser
5. NVSEC-10: init/cleanup の単一 unwind path

### P2 — 継続的な防御

1. NVSEC-09: CI trust chain
2. NVSEC-11: fuzz/sanitizer/hardening
3. NVSEC-12: installer safety

## 5. Agent 向け実装分割案

互いに衝突しにくい順序は次の通り。

1. **validation foundation agent**
   - checked add/multiply、`NVBuffer.elementSize`、type/expected-size table、slice range helper
   - `appendBuffer`/`Array` を failure-returning API に変更
2. **codec agent**
   - AV1/HEVC/VP8 を先に semantic validation
   - 全 codec の stale `lastSliceParams` 排除
3. **surface/import agent**
   - PRIME/PRIME_2 descriptor model、plane-to-object mapping、bounds
   - `vaGetImage` と VideoProc checked copy
4. **lifecycle agent**
   - object refcount/state、resolve queue、shutdown phase、join semantics
5. **test/CI agent**
   - GPU-independent harness、fuzz corpus、sanitizer、APT/action pinning

validation API を先に確定しないと codec ごとに異なる局所 check が増えるため、1 を先行させるのがよい。

## 6. 実施した検証と結果

### 静的・構成確認

- tracked source、scripts、CI、Meson、vendored NVIDIA headers の利用箇所を確認
- `system`, `popen`, `exec*`, `mktemp` 等の command execution sink は source 内で確認されなかった
- tracked file の一般的な secret/key pattern scanでは credential file は検出されなかった
- `clang-tidy`（core/security/unix analyzer、selected bugprone/CERT/concurrency checks）を全 17 translation units に実行
  - `nv_get_versions()` failure 後の NULL `atoi`
  - HEVC `CurrPic` field 越境
  - narrowing/unchecked return 等を検出
  - `memcpy` 一般警告のような低 signal warning は finding 根拠に単独では使用していない

### Build

- 通常 Meson build: **成功**
- Clang ASan+UBSan build: `b_lundef=false` で **compile/link 成功**
- sanitizer binary の `vainfo`: **未完了**
  - ASan runtime preload 下で CUDA が `out of memory` / `initialization error` を返した
  - source の sanitizer finding ではなく、CUDA と ASan の address-space/runtime 競合として扱った

### RTX 5080 実機スモーク

環境: NVIDIA GeForce RTX 5080、driver 595.80、libva 1.23.0 API / libva 2.23.0 package 表示。

- 通常 build の `vainfo --display drm --device /dev/dri/renderD128`: **成功**
- ローカル生成した各 320x180 / 10 frame sample を mpv `--hwdec=vaapi-copy` で decode:
  - H.264: **hardware decoding 成功**
  - HEVC: **hardware decoding 成功**
  - VP9: **hardware decoding 成功**
  - AV1: **hardware decoding 成功**
  - MJPEG: **hardware decoding 成功**
- HEVC picture field修正後、参照フレームを含む30 frameのMain/Main10/Main12（12-bitはRext profile表記）を追加decode: **すべてhardware decoding成功**

これは正常系 smoke であり、malformed input、concurrency、OOM、DMA-BUF adversarial layout の安全性を証明しない。

### 未実施／scope 外

- exploit PoC の作成
- coverage-guided fuzzing
- ThreadSanitizer 実行
- NVIDIA kernel driver/ioctl implementation 自体の監査
- system dependencies（NVIDIA driver、libva、EGL、GStreamer、FFmpeg）の現在の CVE 照合
- privileged `install.sh` 実行

## 7. 良い実装・維持すべき点

- `-Werror=format` / `-Werror=format-security` 等を有効化している。
- shared library の symbol visibility を基本 hidden にしている。
- device fd の多くに `O_CLOEXEC` を使っている。
- `subprojects/ff-nvcodec-headers.wrap` は download hash を固定している。
- JPEG reconstruction は slice offset/size、Huffman count、component/table selector、64-bit total size を他 codec より丁寧に検証している。この validation style を共通化の参考にできる。
- detached backing-image cache に byte/count 上限を設けている。
- 直近変更には allocation rollback、thread-start flag、AV1 OOM check 等の改善が含まれており、failure path を意識した修正方向は正しい。

## 8. 最終判定

正常動画の実機 decode は成立しているが、security boundary で必要な validation と lifetime safety が不足している。特にブラウザで sandbox を無効化して非信頼動画を処理する利用形態では、現状の risk は受容しにくい。

最低限、P0 の修正、GPU-independent fuzz harness、ASan/UBSan での malformed VA buffer regression が揃うまでは、security-sensitive deployment を避けるべきである。
