# STD-T98 Receiver for SDR#

SDR#でARIB STD-T98を復調するプラグインです。コーデックのデコードおよび、Alinco社製デジタル簡易無線の強化秘話を含む秘話コードを全自動で解読します。AMBE+2以外のコーデックには対応していません。
このプラグインは [std-t98-receiver](https://github.com/tallcat4/std-t98-receiver)、[std-t98-cracker](https://github.com/tallcat4/std-t98-cracker)、[std-t98-vocode](https://github.com/tallcat4/std-t98-vocode)、[SPEAR](https://github.com/tallcat4/SPEAR) をベースに、これらの機能をSDR#向けにプラグイン化したものです。

## インストール方法

1. Releasesから `STD-T98-Receiver.zip` をダウンロードして展開し、`Plugins/SDRSharp.T98` フォルダーをSDR#の `Plugins` 内へコピーします。
2. SDR#を起動し、`STD-T98 Receiver` パネルを開きます。

更新時はDLLを上書きし、`secret-cache.json` があれば残してください。

## 使い方

「受信開始」を押すと、選局した1チャネルの復調・秘話解読・音声再生が自動で始まります。


## 秘話解読

線形ソルバーおよび訓練済みのAIモデルを用いて、Alincoの強化秘話を含む秘話コードを自動で解読します。詳細は [std-t98-cracker](https://github.com/tallcat4/std-t98-cracker) を確認してください。

確定した秘話コードを、プラグインDLLと同じディレクトリの `secret-cache.json` に自動保存します。
入感時は、保存したキャッシュを候補として直接照合します。ヒットすればその通話が終わるまで採用し、ヒットしなければ既存のソルバー誘導AIへ自動移行します。

キャッシュを消去したい場合はSDR#を終了して `secret-cache.json` を削除してください。

## 録音

録音を有効にすると、同じファイル名で次を保存します。

- `.wav`: 8 kHz / PCM16 / 秘話推定待ちの区間は無音です。
- `.burst`: FEC付き72-bit AMBE、1バースト41 byte。
- `.llr`: `.burst` と対応する軟判定、1バースト288 byte。
- `.jsonl`: 受信時刻、受信フィールド、秘話の推定状態の変更。

`.burst` と `.llr` は [std-t98-cracker](https://github.com/tallcat4/std-t98-cracker)／[std-t98-vocode](https://github.com/tallcat4/std-t98-vocode) で再処理できます。WAVとバースト形式は受信した音声区間を連結するため、送信間の無信号時間は保持しません。詳細は [FORMAT.md](FORMAT.md) を参照してください。

受信停止時に音声・録音を終了し、変更したSDR#のミュート・モード・帯域を復元します。SDR#自体の受信は継続します。

## ビルド

必要環境: Windows x64、C++20対応MinGW GCC、.NET 8対応SDK、Python 3.11以降、ローカルのSDR# x64。

```powershell
./tools/build.ps1 -SdrSharpExe C:/SDRSharp/SDRSharp.dotnet8.exe
# 以下は開発用の比較試験にだけ必要
python -m pip install numpy torch safetensors
./tools/check.ps1
./tools/package.ps1
```

FFTW開発ファイルがなければ `tools/setup-fftw.ps1` で取得・ハッシュ検証します。SDR#の参照アセンブリは手元の実行ファイルから抽出し、配布しません。ビルド時のモデル埋め込みとSDK抽出はPython標準ライブラリだけを使用します。Python版の参照実装は `tests/reference` にあり、実行用配布には含みません。


## ライセンス・由来

[GPL-3.0](LICENSE)。公開プロジェクトの再利用箇所と第三者ライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
