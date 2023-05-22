# emu68kplus emulator (based on Musashi)

68008搭載のSBC(Single Board Computer) [emu68kplus](https://github.com/tendai22/emu68kplus.git)のシミュレータです。Linux/MacOS上で動作します。

680008エミュレータのベースとしたものが[Musashi](https://github.com/kstenerud/Musashi)です。MMUエミュレーションまで入っていてなんかいい感じです。

## 概要

emu68kplus SBCは、68008, 128k SRAM, PIC18F47Q43の3チップ構成のSBCです。PICがUARTのエミュレーションを行います。

本シミュレータでは、ROM領域なし、RAM領域128kB(00000 - 1FFFF)を持ちます。

UARTは、
|address|desripton|
|--|--|
|800A0|Data Register<br>Read: Received Data<br>Write: Transmitting Data|
|800A1|Control Register<br>b0: RxRDY(1:ready, 0:not ready)<br>b1: TxRDY(1:ready, 0:not ready)|
|||

SDカードサポートはありません。気が向いたら導入します。

## 使い方

引数にダンプファイルを指定して起動すると、ダンプ後実行を開始します。

```
% ./sim echo.X
```
この時の`echo.X`は以下の感じです。
```
=0 0000 1000
=4 0000 0080
=80 4eba 0008
=84 4eba 001c
=88 60f6
=8a 3f00
=8c 1039 0008 00a1
=92 0200 0002
=96 67f4
=98 301f
=9a 13c0 0008 00a0
=a0 4e75
=a2 1039 0008 00a1
=a8 0200 0001
=ac 67f4
=ae 1039 0008 00a0
=b4 4e75
!
```
`={address}`で書き込みアドレスを指定して、通常の16進文字列が書き込みデータを表します。最後の`!`はRAMダンプ命令で、これまでに書き込んだアドレス範囲でダンプします。

引数に複数のファイルを与えると順次読み込みます。すべて読み込んだのちに`run...`を出力した後プログラムの実行を開始(リセット解除、00000番地のベクトルを読み込みそこへ飛ぶ)します。

このプログラム(`echo.X`)はUARTから1バイト読み込み、読み込んだデータをUARTに書き出すもので、実行すると何も出力せずユーザ入力を待ちます。入力をエコーバック後、`ESC`を入力すると実行を停止しシェルプロンプトに戻ります。

<img width=500 src="img/exec_example.png">

> `ESC`をたたいたあと、シェルプロンプト出力後キーを叩いても何も表示されない場合、プログラム終了処理に失敗しています。エコーバックなしですが、`stty sane^CR`とたたいてみましょう。一度でだめなら二度三度たたくのです。これでエコーバックが復帰するはずです。

## ビルド方法

ディレクトリ`example`の下で`make`を実行するだけでよいです。

```
% cd example
% make
```

`example`の中にファイル`sim`が生成されます。これに引数を付けて実行するとよいです。

```
% ./sim echo.X
```
## BUGS

[Musashi github のREADME](https://github.com/kstenerud/Musashi)の最後には、「`m68kconf.h`を別の名前で用意して make のオプションでそのファイル名を指定してビルドしろ」と書いてありますが、まるっきり無視してオリジナルのまま使っています。まぁこのままでいいか。

フォルダ`example`の下の`sim.c`と`osd_linux.c`を変更しています。この辺、オリジナルとの差分や、emu68kplusという個別のハードウェア対応部分がわかりにくい。以下に「PORTING NOTE」で変更についてまとめたので、とりあえずこのままで行く。


## PORTING NOTE

emu68kplusというハードウェアをエミュレーションするための変更は、Musashi が呼び出すメモリアクセス関数を変更することになる。この変更では emu68kplus固有のハードウェアである UART を実現するため、UARTのレジスタの Read/Write の際に生じる挙動をメモリアクセス関数の下に組み込む。

ディレクトリ`example`の下の`sim.c`, `osd_linux.c`を書き換えている。

### I/Oポート(シリアルI/O)の実現

もともとは、メモリアクセス関数から`input_device_read/write`を直接呼び出していた。が、本実装では、間に、`uart_creg_read/write`, `uart_dreg_read/write`呼び出しをはさみ、そこから`input_device_read/write`を呼び出している。

メモリアクセスで 800A0, 800A1にアクセスすると UARTのコントロールレジスタ・データレジスタにアクセスする、の部分を `uart_creg_read/write`, `uart_dreg_read/write`で実現している。

`uart_creg_read`は、後述のグローバル変数群に保持されたデバイスの状態をレジスタビット割り当てに基づいてビットを立てて返しているだけである。コントローラレジスタへの書き込みは無視されている。

`uart_dreg_read/write`は、`input_device_read/write`関数を直接呼び出している。

ここまで書いて気付いたが、もともとの実装にはステータスレジスタの実装がなかっただけなので、`uart_creg/dreg_...`関数をつくらずに、`input_output_device_status`関数をつくり、そこに現在の`uart_creg_read`の中身をそのまま持っていくだけでよかったかもしれない。

その場合、下位層への書き込みは`output_device_update`に持ってゆくことになる。

```
int     g_input_device_value = -1;
int     g_input_device_ready = 0;         /* Current status in input device */

unsigned int g_output_device_ready = 0;         /* 1 if output device is ready */
time_t  g_output_device_last_output;       /* Time of last char output */
```

物理層にデータが到着/物理層への書き出しの部分を`input_device_read/write`に担わせるようにした。データ到着はUARTレジスタへのアクセスと非同期で行われるので、データ到着を`update_user_input`関数を定期的に呼び出し、その中で処理させている。

データが到着すると、グローバル変数`g_input_device_value`に書き込み、`g_input_device_ready`を立てる。

データをUARTデータレジスタに書き込むと、一定時間待ったのちに`printf("%c", c);`を使って書き込む。出力待ち処理は`g_output_device_last_output`に最終出力時刻を保存しておき、現在時刻との差分を取って待ち完了を判定している。待ち時間判定も`output_device_update`を定期的に呼び出し、時間が経過すると`g_output_device_ready`を立てている。

現在時刻の取得は`get_msec`関数を作成し使用している。最近のはやりと思われる`clock_gettime`関数を使っている。OS依存なので、定義を`osd_linux.c`に異動させた方がよいかもしれない。

`osd_linux.c`で、`changemode`関数、`kbhit`関数が用意されている。組み込み流のシリアル読み込みを実現するには`kbhit`関数は欲しい。

ttyドライバのモード切替は`changemode`関数で行う。引数に1を渡せばRAWモードにする。具体的にはICANONとECHOビットだけを落としている。引数に0を渡せばCOOKEDに戻す。割り込みのキャッチは行っていない。SIGINT, SIGTERM, SIGQUITにより`sim`コマンドを停止させると`changemode(0);`を実行しそこなうのでttyがRAWモードのままになってしまう。シグナルハンドラを整えてこのあたりもちゃんとさせたいところである。

`kbhit`関数は`select`呼び出しでファイルディスクリプタ0番をチェックして結果を返している。timeout指定に1msを指定して、連続した`kbhit`呼び出しでもビジーループにならないようにしておいた。  

### 割り込み機構

割り込み機構について特に追加の実装を行っていない。もともとのエミュレータそのままである。

本エミュレータではもともとの`sim.c`に実装されていた割り込み処理機能をそのまま残しているが、誰も割り込み要因を発生させないので割り込み機能は存在しないのと同じである。emu68kplusではUARTからの割り込みは今のところ実現していない。PICのファームウェアを書き換えれば割り込みも実現できるので、必要になればその時に行う。エミュレータの割り込み機能もその時に火を噴くだろう。