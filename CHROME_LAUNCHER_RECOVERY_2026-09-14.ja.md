# Chrome 153 起動復旧 — 2026-09-14

## 原因

Chrome 152.0.7977.82 から 153.0.8010.36 への更新後、ユーザーの
desktop ファイルが呼び出す別プロジェクト `chrome-vaapi-hotpatch` の
H.264 エンコード用パッチが次のエラーで停止した。

```text
error: expected one SetUpVeaConfig-like function, found 0 []
```

Chrome 起動前のパッチ処理で終了している。`rpm -V google-chrome-stable`
は差分なし。同じ更新で `libva-nvidia-driver` が 0.0.18 に更新され、
以前 `/usr/lib64/dri/nvidia_drv_video.so` にインストールした自作版も
配布パッケージのファイルに置き換わっていた。

今回使用した自作版は `review/code-audit-hardening` の9月5日の
release ビルド。VA-API のデコードを提供し、EncSlice は広告しない。
この構成のデコードには、別プロジェクトのエンコード用バイナリパッチは不要。

Discover のシステム修復失敗も記録されているが、その詳細原因は未確定。
今回の変更は Chrome の起動経路を復旧するもので、PackageKit の更新処理を
修復したという意味ではない。

## 変更と適用

`install.sh --chrome-integration-only --restore-chrome-launcher NAME` を追加。
指定した desktop ファイルだけをシステムのテンプレートから再作成し、
このドライバーの環境とデコード用フラグを設定する。
既存の管理済みファイルも一意な名前でバックアップする。
通常インストール時のカスタムコマンド保持は継続する。

実機では `google-chrome.desktop` を復旧し、`NVD_DRIVER_DIR` で
`build-audit-20260905-release` を指定した。
本体とシークレットウィンドウの起動は通常の Chrome を直接使用する。
ドライバーの SHA-256 は次のとおり。

```text
35be8df895bf828658122e79156383a72017895a60fa9d57660e66c71c750341
```

システムのライブラリは上書きしていない。
このローカルビルドのディレクトリは Chrome の起動設定から参照されるため、
使用中は削除・移動しないこと。

## 検証

- `bash -n install.sh tests/test-install-script.sh` 成功。
- `bash tests/test-install-script.sh "$PWD"` 成功。既存の設定保持に加え、
  管理済みランチャーの復旧、全アクション、他ランチャーの非変更、
  バックアップ失敗時の保持、連続実行、テンプレート不在、Flatpak の拒否を検査。
- `desktop-file-validate` と `git diff --check` 成功。
- RTX 5080 / NVIDIA 610.57.04 / Fedora 44 / Wayland / Chrome 153.0.8010.36。
- 独立プロファイルで通常 Chrome、および復旧した desktop の Exec 経由で起動。
  両方とも VP9 1920×1080 動画を再生し、CDP の `VaapiVideoDecoder` と
  GPU プロセスの maps から上記自作ライブラリの使用を確認。
- 通常再生3秒で90フレーム、ドロップ0。

短いシーク確認では両経路とも10回中6回のフレーム番号不一致があり、
上下の番号不一致は0回。9月5日の測定でも存在したシーク直後の
フレーム番号不一致は今回の修正対象外であり、映像の完全な正しさや
長時間安定性を証明する結果ではない。

H.264 エンコード用 hotpatch 自体の Chrome 153 対応は行っていない。
