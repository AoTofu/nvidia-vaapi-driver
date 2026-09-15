# YouTube AV1 2倍速停止の調査と修正（2026-09-15）

## 結論

指定動画 `https://www.youtube.com/watch?v=-ruum8wY_Ik` の冒頭をChromeで
2倍速再生すると、旧版では約8.5秒の再生位置でVAサーフェスの再取り込みに失敗した。
ChromeのMediaログにデコードエラーが記録され、YouTubeがAV1 720p → AV1 480p → VP9へ
切り替わることを再現した。ドライバー側に2つの不具合があり、両方を修正した。

修正版では、同じ動画とChromeでその再取り込みに成功し、AV1のまま再生が進む。
検証用の新規プロファイルでは別途、約59.8秒の再生位置でYouTubeが
`ump.spsrejectfailure` / `HTML5_SPS_UMP_STATUS_REJECTED` を返す。
これは `--disable-accelerated-video-decode` を指定したDav1dVideoDecoderでの
ソフトウェア再生でも再現し、ChromeのMediaデコードエラーはない。
配信拒否のさらに詳しい理由はこのログから断定できず、ドライバー修正の対象とは分けた。
このため、YouTubeサイト上での全編完走を確認したとは主張しない。

## 実行環境

- NVIDIA GeForce RTX 5080、NVIDIA 610.57.04。
- Fedora / Wayland、Google Chrome 153.0.8010.36。
- directバックエンド、packedエクスポート、ANGLE GL。
- 旧版：前回シーク修正の `377a768` 相当のClang release。
- 修正版：`61acaf1`、GCC debugoptimizedとClang releaseの両方で確認。
- 通常試験のキャッシュ上限は既定値16。別途0にして完全破棄を強制した。
- ユーザーのChromeとは別の専用プロファイルでCDPのMediaイベントとドライバーログを記録。

## 原因と修正

### 1. legacy PRIMEで欠落するmodifierの扱い

Chromeはデコーダー再初期化時、フレームプールのDMA-BUFをlegacy PRIMEで取り込む。
このAPIの記述子にはDRM modifierを渡す欄がない。ドライバーはそこをLINEARとして
解釈し、自身が出力したNVIDIA block-linear画像との一致判定に失敗していた。

ログでは同じ1280×720のFD、pitch 1280、offset 0 / 983040に対し、
既存modifier `0x300000000606014` と取り込み側の0の差だけで拒否していた。
`6a07e5f` で、保持中の同一DMA-BUFが見つかる場合はそこからmodifierを復元した。
FDの同一性、寸法、形式、全planeのpitchとoffsetを引き続き検証する。
明示的なPRIME_2 modifierの不一致は拒否する。

### 2. キャッシュ破棄後の有効なDMA-BUFを取り込めない

上記だけを直しても、次のフレームで既存BackingImageが見つからず失敗した。
VAサーフェス破棄後、Chromeは出力済みDMA-BUFのFDを保持しているが、
ドライバーはキャッシュ上限を超えるとCUDAマッピングと管理オブジェクトを破棄する。
FDが有効でも、従来は管理オブジェクトを失うとこの画像を再取り込みできなかった。

`61acaf1` で、NVIDIA DRMの `DRM_IOCTL_NVIDIA_GEM_EXPORT_NVKMS_MEMORY` を使い、
DMA-BUFからCUDAが受け取れるRMメモリーFDを取得し直す処理を追加した。
画像の画素を消去・再確保せず、そのメモリーをCUDA配列として再マップする。
一時GEMハンドルには独立したDRMファイル記述を使い、クライアントのハンドルを壊さない。
取り込んだ配列・外部メモリーはサーフェス破棄時に解放する。

対象はこのバックエンドのpacked配置に厳密に一致する単一オブジェクトで、
カーネル側でもnative NVKMSメモリーとしてエクスポートできるものに限定した。
legacy PRIMEで任意の外部タイル配置を推定できるわけではない。
PRIME_2では明示modifierも配置と一致する必要がある。
キャッシュ上限の増量や画像を無期限に保持する回避策は採用していない。

## 実測

| 試験 | 結果 |
| --- | --- |
| 旧版、指定YouTube動画、2倍速 | 約8.5秒で停止。Mediaエラー4イベント、AV1からVP9へ移行 |
| modifier修正のみ | 後続のキャッシュミスで依然失敗 |
| 修正版、キャッシュ0 | native再取り込み207回、Mediaエラー0、VP9への移行なし |
| Clang release、既定キャッシュ | native再取り込み86回、Mediaエラー0、VP9への移行なし |
| ソフトウェア対照 | Dav1dVideoDecoder、Mediaエラー0。約59.8秒で配信拒否 |
| 旧版6a07e5fに対する破棄後PRIME回帰テスト | resource allocation failedで期待どおり失敗 |
| 修正版のCPU/GPUテスト | GCC / Clangとも12件成功 |
| ASan / UBSan | CPUテスト8件成功 |
| 画素・FD回帰テスト | NV12/P010、64×48〜1920×1080の100画像、legacy/PRIME_2計200再取り込みで全画素一致。FD数増加なし |
| AV1 4K60、矢印キー連打150試行、キャッシュ0 | incorrect=0、splitIds=0、VaapiVideoDecoder使用 |

指定動画の公開1080p AV1ストリーム（1920×1080、約29.97fps、889.9891秒）を
ローカルファイルとしてChromeで最初から2倍速再生した。GCC debugoptimized、
キャッシュ0の条件で最後まで完走し、VaapiVideoDecoder、Mediaエラー0、
Chrome報告のcorruptedVideoFrames=0、totalVideoFrames=26728、droppedVideoFrames=5。
native再取り込みは2077回成功した。これは配信経路を除いた同じAV1映像の試験であり、
YouTubeサイト全編の検証とは区別する。

新しい診断スクリプトでClang release・既定キャッシュのYouTube試験をもう一度行い、
AV1 itag 398の維持、Mediaエラー0、約59.8秒で同じ配信拒否、という結果を確認した。

## 再実行

```sh
CC=clang meson setup build-youtube-release --buildtype=release -Dgpu_tests=true
meson test -C build-youtube-release --print-errorlogs
node tests/probe-playback.mjs build-youtube-release \
  'https://www.youtube.com/watch?v=-ruum8wY_Ik' /tmp/youtube-av1-new-run 80 2
NVD_PROBE_SOFTWARE=1 node tests/probe-playback.mjs build-youtube-release \
  'https://www.youtube.com/watch?v=-ruum8wY_Ik' /tmp/youtube-av1-software-new-run 80 2
NVD_MAX_DETACHED_BACKING_IMAGES=0 NVD_BENCH_MODE=burst NVD_BENCH_REQUIRE_CORRECT=1 \
  node tests/bench-seek.mjs build-youtube-release /path/to/seek-4k60-av1.webm \
  /tmp/seek-av1-result.json 150 packed
```

probeは毎回新しいRUN_DIRを使う診断用スクリプトで、サイトの配信拒否を
ドライバー失敗と自動判定するテストではない。`summary.json`、`events.jsonl`、
`chrome.log`、`driver.log`と最後の画面を保存する。URLの代わりにローカル動画も指定できる。
4K60フィクスチャの生成方法は前回の `SEEK_FIX_2026-09-14.ja.md` を参照。

実測ログは `/home/khyt/nvd-youtube-20260915/` に保存した。
配信URLやプロファイルを含む生ログ・動画ファイルはGitに追加していない。

## 適用

検証済みClang releaseを以下に配置し、ユーザー用Chromeデスクトップファイルの
3つのExecにあるドライバーパスを更新した。

`/home/khyt/.local/lib/nvidia-vaapi-driver/youtube-61acaf1/nvidia_drv_video.so`

SHA-256: `ea84e24401f4d7672ed461a4420a91651a57a506d17aebd3442bc2ab48d543d0`

既に起動しているChromeのドライバーは置き換わらない。Chromeを完全終了して
アプリ一覧から起動し直すと新しいライブラリーが使われる。
変更前のデスクトップファイルは測定ディレクトリーの
`google-chrome.desktop.before-youtube-fix` に保存している。

## 参照した一次資料

- [Chromium 153.0.8010.36 vaapi_wrapper.cc](https://chromium.googlesource.com/chromium/src/+/153.0.8010.36/media/gpu/vaapi/vaapi_wrapper.cc)
- [NVIDIA 610.57.04 DRM NVKMS memory export](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/610.57.04/kernel-open/nvidia-drm/nvidia-drm-gem-nvkms-memory.c)
- [NVIDIA 610.57.04 NVKMS ExportMemory](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/610.57.04/src/nvidia-modeset/kapi/src/nvkms-kapi.c)
