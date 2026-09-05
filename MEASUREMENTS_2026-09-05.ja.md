# 監査修正のインストールとChrome実測 — 2026-09-05

監査修正のreleaseビルドをシステムへインストールし、インストール済みライブラリでVA-API状態テストが成功した。
Chromeのフレーム通知遅延には明確な改善を確認できず、ハードウェアデコード経路のシーク直後のフレーム番号不一致は変更前後とも残った。
この結果を「Chrome映像不具合の解消」と扱わない。

## 条件と測定方法

- RTX 5080、NVIDIA 610.57.04、Chrome 152.0.7977.75、Wayland。
- A：監査対象main `4b4969e4c8fd26505669a9a25263fc96ccf43f04` の新規releaseビルド。
- B：PR #4の修正後releaseビルド。A/Bとも同じローカルGCC・依存関係・Meson release設定。
- `NVD_EXPORT_LAYOUT=packed`、direct backend。ドライバー詳細ログ・統計ログは無効。
- 各runで専用の新規Chrome profileを使用。実GPUプロセスのmapsとCDP Media情報から、対象ライブラリと`VaapiVideoDecoder`を確認。
- 自作の1920×1080・30fps・12秒動画。上下に同じ9bitのframe IDを配置。全360フレームをCPUデコードして番号を検証。
- VP9はlibvpx-vp9、AV1はlibaom-av1のlossless。静止領域の多い低ビットレート素材であり、一般的なYouTube映像の代表負荷ではない。
- 3秒の通常再生後に停止し、固定の位置列でシーク。10回のウォームアップ後に150回計測し、A→B→B→Aの順で実行。
- codec・ドライバーごとに合計300回。ソフトウェア対照は各codecで300回。
- 遅延は`currentTime`変更から対象時刻近傍の`requestVideoFrameCallback`通知まで。**正しい画像が表示されるまでの遅延ではない。**
- 通知直後にcanvas readbackで上下の番号と通知されたmediaTimeを比較。上下同一かつ期待ID±1以内を一致とする。
- percentileは各条件の300個の値をまとめたnearest-rank。反復は独立な確率試行として扱わない。

## 結果

| codec / 経路 | 回数 | 通知遅延 median / p95 / p99 (ms) | 番号不一致 | 上下の番号不一致 |
|---|---:|---:|---:|---:|
| VP9 / A 修正前 | 300 | 17.6 / 24.3 / 25.7 | 171 | 0 |
| VP9 / B 修正後 | 300 | 17.8 / 24.9 / 26.1 | 163 | 0 |
| VP9 / VpxVideoDecoder | 300 | 3.3 / 15.1 / 16.6 | 0 | 0 |
| AV1 / A 修正前 | 300 | 15.6 / 19.1 / 20.8 | 144 | 0 |
| AV1 / B 修正後 | 300 | 15.9 / 19.6 / 20.6 | 138 | 0 |
| AV1 / Dav1dVideoDecoder | 300 | 16.4 / 20.2 / 20.8 | 0 | 0 |

A/Bの通常再生区間はいずれも約3秒で90フレーム、ドロップ0。
これは短時間のフレーム数確認であり、長時間再生・4K/8K・電力・総VRAMの性能評価ではない。

番号不一致は今回の修正前にも発生し、検証済み素材のソフトウェア再生では発生しなかった。
調査対象はChromeのハードウェアデコードからcanvas取得までの経路に絞られるが、
NVDEC出力、ドライバーコピー、Chromium import、canvas取得のどこで生じるかは未確定。
物理ディスプレイのtearingを測定したものではない。readback自身による経路・タイミングへの影響もある。
監査のCPU/GPU API回帰テストは合格している一方、Chromeの「正しいフレームで300回無不一致」という安定性条件は未達。

最初に作成したlossy SVT-AV1素材はCPUデコード時点で74/360フレームの番号が変わっていたため除外した。
`av1-A1.json`等の初回AV1データは評価に使わず、`av1-lossless-*.json`だけを使用した。
初回素材での上下不一致をドライバーの映像分断として数えない。

## インストールと復旧

- インストール先：`/usr/lib64/dri/nvidia_drv_video.so`
- 修正後SHA-256：`35be8df895bf828658122e79156383a72017895a60fa9d57660e66c71c750341`
- インストール前のファイルを `/home/khyt/nvd-measurements-20260905/baseline-installed/nvidia_drv_video.so` に保存。
- インストール前SHA-256：`4731199e3353971fed5f8ab5f8b87c24a5e113a6d649807725864b2c440f0378`
- Chrome/Chromiumのユーザー用desktop起動設定をpackedに更新。既存ブラウザは強制終了していない。

```sh
# 必要時の復旧。実行後はChromeを完全終了して再起動する。
sudo install -m 0755 /home/khyt/nvd-measurements-20260905/baseline-installed/nvidia_drv_video.so /usr/lib64/dri/nvidia_drv_video.so
```

## 証拠と再実行

生データは `/home/khyt/nvd-measurements-20260905/` の
`vp9-{A1,B1,B2,A2,software}.json`、`av1-lossless-{A1,B1,B2,A2,software}.json`。
各JSONに全シーク値、CDP decoder情報、読み込まれたライブラリのパスとhash、Chrome引数を保存。
インストール済み旧バイナリは保存したが、比較Aには同一ビルド条件を揃えたmainの再ビルドを使用した。

動画SHA-256：

- VP9：`b67b98b516678aaf8c9734c18efc4cabeb38d067947b260e786e4624ab373f72`
- AV1 lossless：`41968af18ea20e45814a6dfe2f77831a13d0f37a7df23357d3d93a555bb6d0eb`

```sh
python3 tests/generate-seek-video.py /tmp/nvd-seek-vp9.webm
python3 tests/generate-seek-video.py /tmp/nvd-seek-av1.webm --codec av1
node tests/bench-seek.mjs /absolute/driver/directory /tmp/nvd-seek-vp9.webm /tmp/nvd-seek-result.json 150
NVD_BENCH_SOFTWARE=1 node tests/bench-seek.mjs /absolute/driver/directory /tmp/nvd-seek-vp9.webm /tmp/nvd-seek-software.json 300
```

Node 22以上、ffmpeg、Chrome、稼働中のWaylandセッションを使用する。
動画生成は既存ファイルを上書きせず、生成後に全フレームのID検証を行う。
この測定はYouTubeでの右矢印操作そのものではなく、ローカル固定素材へのDOMシークである。
