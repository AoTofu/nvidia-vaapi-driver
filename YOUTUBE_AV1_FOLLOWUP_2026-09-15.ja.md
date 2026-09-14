# AV1→VP9再発報告の追加調査（2026-09-15 05時台）

## 見つかった適用漏れ

前回は `google-chrome.desktop` の内容を書き換えたが、KDEのサービスキャッシュを
明示的に更新していなかった。今回確認した日本語セッションのキャッシュには、
YouTube修正前の `seek-377a768` を参照するChrome起動コマンドが3件残っていた。
修正後の `youtube-61acaf1` は0件だった。Plasmaの実際の言語は `ja_JP.UTF-8`。

別名の `com.google.Chrome.desktop` と `chromium-browser.desktop` も、
旧システムライブラリー `/usr/lib64/dri` を指していた。
ファイルだけ更新された状態を、通常の起動経路でも適用済みと扱ったことが前回の不足。

調査開始時には再発時のChromeプロセスが残っておらず、そのプロセスが使っていた
ライブラリーを直接確認できたわけではない。上記は確認できた適用不備であり、
今回の再発がこれだけで説明できると断定はしない。

KDEの[KSycoca公式資料](https://api.kde.org/ksycoca.html)でも、アプリケーション情報を
キャッシュし、ディレクトリーの更新時刻などから変更を検出する仕組みが説明されている。
ファイル内容の上書きだけでは親ディレクトリーの更新時刻が変わらない。

## 修正と適用確認

`20d1d73` で、インストーラーが起動項目を書き換えた後、利用可能なら
`kbuildsycoca6 --noincremental`（または5）も実行するようにした。
キャッシュ更新が失敗した場合、再起動だけで適用できると案内せず、エラーとして返す。
回帰テストでは呼び出しと失敗の伝播を検証し、実デスクトップへの副作用はスタブで防ぐ。

既存の `--chrome-integration-only` と `NVD_DRIVER_DIR` を使用し、3つの起動項目の
通常・新規・シークレット各アクション、計9件を修正版に統一した。
日本語とen-POSIXの両キャッシュを再生成し、それぞれ修正版の参照9件、
`seek-377a768` と旧システムディレクトリーの参照0件を確認した。
起動ファイル3件は `desktop-file-validate` を通過。

その後、`kstart --application google-chrome --url <指定動画URL>` で通常のKDE経由から
Chromeを起動した。GPUプロセス52524の `/proc/52524/maps` に以下を確認した。

`/home/khyt/.local/lib/nvidia-vaapi-driver/youtube-61acaf1/nvidia_drv_video.so`

ライブラリーのSHA-256は前回検証したものと同じ：
`ea84e24401f4d7672ed461a4420a91651a57a506d17aebd3442bc2ab48d543d0`

この通常プロファイルでの再生は1倍速。05:16:54時点で指定動画の112.7秒まで進み、
AV1処理3389件、native再取り込み92件、再取り込み失敗0件。
MPRISで報告された速度も1であり、この実プロファイル確認を2倍速試験とは扱わない。
今回起動したChromeには診断ログを指定した。既存のユーザーChromeを強制終了していない。

## 追加の2倍速試験

`ce30c44` で診断スクリプトに以下を追加した。

- 指定したChrome実験フラグだけを専用プロファイルへ設定する `NVD_PROBE_LABS`。
- YouTubeの画質を指定する `NVD_PROBE_QUALITY`。
- 試験中の実際のライブラリーマッピングと、指定ライブラリーのSHA-256の記録。

普段のChromeで有効だった4つのフラグを同じ値に設定し、1080p・2倍速で90秒観測した。
Cookieや認証情報などを専用プロファイルへコピーしていない。

```sh
NVD_PROBE_LABS='["force-color-profile@5","pdf-use-skia-renderer@1","skia-graphite@1","trees-in-viz@1"]' \
NVD_PROBE_QUALITY=hd1080 node tests/probe-playback.mjs \
  /home/khyt/.local/lib/nvidia-vaapi-driver/youtube-61acaf1 \
  'https://www.youtube.com/watch?v=-ruum8wY_Ik' /tmp/youtube-flags-new-run 90 2
```

GPUプロセス50889が修正版を読み込むことを確認。AV1 itag 398→399（720p→1080p）へ
画質が変わったが、VP9への移行とChromeのMediaデコードエラーは0件だった。
再生位置59.848秒で、前回同様の配信拒否 `ump.spsrejectfailure` が発生した。
これは新規プロファイルでの制約が残る試験で、YouTube上の全編2倍速完走ではない。

今回の変更にドライバー本体の追加修正は含めていない。
修正版を確実に読み込む起動経路と、実環境に近い条件を記録する検証方法を修正した。

## 保存物

`/home/khyt/nvd-youtube-followup-20260915/` に以下を保存。

- 更新前の起動ファイルとKDEキャッシュ。
- `flags-1080p/summary.json` とChrome・ドライバーのログ。
- `desktop-driver-mapping.json`、`desktop-playback-status.json`。
- 実プロファイルの今回の再生ログ `desktop-chrome-driver.log`。

生のキャッシュ・プロファイル・ログはGitに追加していない。
