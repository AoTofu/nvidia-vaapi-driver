# 2026-09-05 監査対応の実装・検証記録

対象は `/home/khyt/nvvaapi-work`、開始時のコミットは
`4b4969e4c8fd26505669a9a25263fc96ccf43f04`。変更はローカルの作業ツリーに保存。
システムへのインストール、push、PR作成は実施していない。

## 実装内容

| 指摘 | 変更 |
|---|---|
| F01 | UUID取得のbool判定を修正。UUID取得失敗・不一致時の無条件GPU 0 fallbackを削除。識別できない構成では初期化エラーを返す。 |
| F02 | コピー・event投入の失敗後にもstream完了待ちを実行。初期化失敗後のCPU fallbackも完了確認が条件。同期失敗時はdriver単位の失敗状態を保持し、追加picture処理・export・資源再利用を拒否。workerでunmap前の待機と、decoder破棄前の待機を保証する経路を追加。通知はworker側で確定。 |
| F03 | 配置関数をbool＋出力引数に変更。pitch・アライン済み高さ・サイズ・offsetを64ビットで計算し、32ビットAPIおよび符号付き内部表現の上限で拒否。RGB単独割り当ても同様に検査。 |
| F04 | 自己割り当てにもobject数・plane数・object対応を設定。exportは保存済みobject容量・modifier・plane対応・fourccから生成。RGB全8形式のDRM形式を正しいバイト順で生成。通常RGB割り当てでも要求されたpixel formatを保持。再importでobject順に保存したmodifierをplane順で上書きする処理を削除。 |
| F05 | VP9 parserの非成功・作成失敗をpicture入力失敗に伝播。parserはcontext間で分離し、フレームごとには作り直さない。 |
| F06 | READMEのChrome起動とインストーラー説明をpackedに整合。ログに実際の読み込みパスと選択layout・環境指定・Chrome判定を追加。ライブラリのauto方針は変更していない。 |
| F07 | HTTP＋trusted=yesの追加APTソースを廃止。runnerの標準認証済みリポジトリからClangを取得する。 |
| F08 | VP9のfeature option、GPUテストの明示的有効化とskip理由、GCC/Clang・VP9有無・ASan/UBSanのCI行列を追加。GPU CIは別の手動workflow。 |
| R02 | mipmapped arrayから借用したlevelを独立にdestroyしないように修正。 |

同期完了が分からない状態ではCUDA資源をプロセス終了まで保持する。
これは通常のキャッシュではなくエラー停止であり、回復には利用プロセスの再起動が必要。
UUID取得に対応しないRM環境では従来のfallbackと互換性が変わる。
その環境を確認できていないため、未検証のPCI fallbackは追加していない。

## 検証

- GCC 16.2.1、VP9有効、debug、werror：ビルドとCPUテスト8件が成功。
- GCC、VP9有効、ASan＋UBSan：CPUテスト8件が成功。GPUライブラリを読み込まないテスト構成で、リーク検出も有効。
- Clang 22.1.8、VP9無効、debugoptimized、werror：ビルドとCPUテスト8件が成功。
- 標準テストに登録したGPUテストは、無効時に終了コード77と理由を返す。
- NVIDIA GeForce RTX 5080、driver 610.57.04、libva 1.23：ビルド先の共有ライブラリでGPU状態テストが成功。
- GPU状態テストはpacked、per-plane-shared-modifier、per-plane-naturalで各1回実行。
  NV12・P010・RGB8形式のexport→import→再exportを検査。
  2objectの場合はobject配列を反転させ、plane対応・offset・pitch・容量・modifier・形式が保持されることも検査した。
- `git diff --check`、Clang導入スクリプトの`bash -n`が成功。

`tests/test-direct-audit.c`は実装本体を直接呼ぶ。
UUIDのioctl成功・OSエラー・RMエラー、GPU選択、監査の巨大NV12例、全内部形式の通常・奇数寸法、
object順反転・single object・RGB形式、planeコピー失敗・event失敗・stream同期失敗、
初期化chunk途中失敗、resolve workerのunmap・destroy・完了通知順序、context push失敗のqueue取消し、
VP9不正marker・正常キーフレーム・失敗後の復帰・context分離を検証する。

GStreamer 1.28.6は試した2バイトの途中切断ヘッダーを成功扱いした。
F05の修正はparserが返したエラーの伝播であり、parser自体の完全な入力検証を保証しない。

検証した通常ビルド：`build-audit-20260905/nvidia_drv_video.so`

SHA-256: `e4412b3451c59220661f4ae78697b102fffda7a4903e208e333a780a73844951`

## 再実行

```sh
meson setup build-audit-local -Dwerror=true -Dvp9=enabled
meson compile -C build-audit-local
meson test -C build-audit-local --print-errorlogs

# NVIDIA GPUホストでのみ有効化する。インストールは不要。
meson configure build-audit-local -Dgpu_tests=true
NVD_BACKEND=direct NVD_EXPORT_LAYOUT=packed meson test -C build-audit-local --suite gpu --print-errorlogs
```

GPU workflowは`self-hosted, linux, x64, nvidia`ラベルを持つ、依存関係を準備したrunnerを必要とする。
GitHub上のworkflow実行と、クリーンなUbuntu runnerでのパッケージ取得は未実施。

## 残る実機調査・性能作業

Chrome/YouTubeの連続シーク300/3,000回、codec別の映像一致、複数GPU、複数driver世代、
TSan、R01のpadding初期化保証、R03/R04のAV1 sequence/参照状態は今回検証していない。
GPU状態テストの成功は、Chromeの映像分断が解消した証明ではない。

O1〜O6の性能案は、監査の指定どおり実測で選択する後続項目とした。
今回、16bit fillの新実装、pool拡張、output surface数変更、decoder pool縮小は行っていない。
シーク遅延・VRAM・電力・初期化費用の改善率は未測定。
